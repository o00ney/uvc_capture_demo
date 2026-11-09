#pragma once

#include "common/latest_slot.hpp"
#include "common/frame.hpp"

#include <cstdint>
#include <functional>
#include <string>

namespace image_pipeline {

struct CameraConfig {
    std::string device;
    std::uint32_t width = 1280;
    std::uint32_t height = 720;
    std::uint32_t fps = 60;
};

// Only input code depends on V4L2; owns the device until the call returns.
std::uint64_t run_camera(const CameraConfig& config, LatestSlot<JpegFrame>& slot,
                         std::uint64_t max_frames, const std::function<bool()>& stop);

} // namespace image_pipeline
