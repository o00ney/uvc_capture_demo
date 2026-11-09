#include "common/latest_slot.hpp"
#include "common/frame.hpp"

#include <chrono>
#include <future>
#include <stdexcept>
#include <thread>

using namespace image_pipeline;

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

int main() {
    LatestSlot<JpegFrame> slot;
    JpegFrame first{{1, 2, 3}, 10, std::chrono::steady_clock::now()};
    JpegFrame second{{4, 5}, 11, std::chrono::steady_clock::now()};
    slot.publish(std::move(first));
    slot.publish(std::move(second));
    require(slot.overwritten() == 1, "unread frame overwritten");
    auto selected = slot.take();
    require(selected.has_value(), "frame available");
    require(selected->sequence == 11, "newest sequence selected");
    require((selected->bytes == std::vector<std::uint8_t>{4, 5}), "newest bytes selected");

    auto waiting = std::async(std::launch::async, [&slot] { return slot.take(); });
    require(waiting.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout,
            "consumer waits for a frame");
    slot.close();
    require(waiting.wait_for(std::chrono::seconds(1)) == std::future_status::ready,
            "close wakes consumer");
    require(!waiting.get().has_value(), "closed slot returns no frame");
    slot.close();
    slot.publish(JpegFrame{{9}, 12, {}});
    require(!slot.take(), "publish after close is ignored");
    require(slot.overwritten() == 1, "publish after close does not count overwrites");

    LatestSlot<JpegFrame> draining;
    draining.publish(JpegFrame{{7}, 13, {}});
    draining.close();
    draining.close();
    draining.publish(JpegFrame{{8}, 14, {}});
    auto last = draining.take();
    require(last && last->sequence == 13, "close preserves the last pending frame");
    require(!draining.take(), "closed slot is empty after draining");
    require(draining.overwritten() == 0, "closed publish does not overwrite pending frame");
    // Independent branches share immutable source storage, but never consume each
    // other's pending item or overwrite the item a slow consumer already holds.
    LatestSlot<FramePtr> cpu, cuda;
    auto original = std::make_shared<Frame>();
    original->sequence = 20;
    FramePtr shared = original;
    cpu.publish(shared); cuda.publish(shared);
    auto slow = cpu.take();
    auto newer = std::make_shared<Frame>(); newer->sequence = 21;
    cuda.publish(newer);
    require((*slow)->sequence == 20, "held original survives branch overwrite");
    require(cpu.overwritten() == 0 && cuda.overwritten() == 1, "independent branch counters");
    require((*cuda.take())->sequence == 21, "slow branch selects newest frame");
    cpu.close(); cuda.close();

}
