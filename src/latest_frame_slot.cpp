#include "latest_frame_slot.hpp"

#include <utility>

namespace camera_demo {

void LatestFrameSlot::publish(JpegFrame frame) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) return;
        if (pending_) ++overwritten_;
        pending_ = std::move(frame);
    }
    ready_.notify_one();
}

std::optional<JpegFrame> LatestFrameSlot::take() {
    std::unique_lock<std::mutex> lock(mutex_);
    ready_.wait(lock, [this] { return closed_ || pending_.has_value(); });
    if (!pending_) return std::nullopt;
    auto frame = std::move(pending_);
    pending_.reset();
    return frame;
}

void LatestFrameSlot::close() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
    }
    ready_.notify_all();
}

std::uint64_t LatestFrameSlot::overwritten() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return overwritten_;
}

} // namespace camera_demo
