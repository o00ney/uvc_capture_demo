#pragma once

#include "frame.hpp"

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>

namespace camera_demo {

// One-slot mailbox. The producer replaces an unread frame; the consumer
// takes ownership and processes it without holding the mutex.
class LatestFrameSlot {
public:
    void publish(JpegFrame frame);
    std::optional<JpegFrame> take();
    void close();
    std::uint64_t overwritten() const;

private:
    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::optional<JpegFrame> pending_;
    std::uint64_t overwritten_ = 0;
    bool closed_ = false;
};

} // namespace camera_demo
