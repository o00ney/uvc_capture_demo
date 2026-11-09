#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <utility>

namespace image_pipeline {

// One producer / one consumer. Work happens after take() releases the lock.
// Closing wakes the consumer and preserves the final pending item for draining.
template<class T>
class LatestSlot {
public:
    void publish(T value) {
        {
            std::lock_guard lock(mutex_);
            if (closed_) return;
            if (pending_) ++overwritten_;
            pending_ = std::move(value);
        }
        ready_.notify_one();
    }
    std::optional<T> take() {
        std::unique_lock lock(mutex_);
        ready_.wait(lock, [&] { return closed_ || pending_.has_value(); });
        auto value = std::move(pending_);
        pending_.reset();
        return value;
    }
    void close() {
        {
            std::lock_guard lock(mutex_);
            closed_ = true;
        }
        ready_.notify_all();
    }
    std::uint64_t overwritten() const {
        std::lock_guard lock(mutex_);
        return overwritten_;
    }
private:
    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::optional<T> pending_;
    std::uint64_t overwritten_ = 0;
    bool closed_ = false;
};

} // namespace image_pipeline
