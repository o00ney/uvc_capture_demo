#pragma once

#include "latest_frame_slot.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace camera_demo {

struct CameraConfig {
    std::string device;
    std::uint32_t width = 1280;
    std::uint32_t height = 720;
    std::uint32_t fps = 60;
    std::uint32_t buffer_count = 4;
};

class V4L2Camera {
public:
    explicit V4L2Camera(CameraConfig config);

    // Runs on the calling thread. stop() is checked at least every 500 ms.
    // max_frames == 0 means run until stop() returns true.
    std::uint64_t run(LatestFrameSlot& slot, std::uint64_t max_frames,
                      const std::function<bool()>& stop);

private:
    CameraConfig config_;
};

} // namespace camera_demo
