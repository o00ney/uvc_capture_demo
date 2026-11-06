#pragma once

#include <chrono>
#include <cstdint>
#include <vector>

namespace camera_demo {

// Owns a copy of one compressed camera frame. It remains valid after V4L2 QBUF.
struct JpegFrame {
    std::vector<std::uint8_t> bytes;
    std::uint64_t sequence = 0;
    std::chrono::steady_clock::time_point received_at{};
};

// RGB pixels are packed R,G,B; stride may be larger than width * 3.
struct RgbImage {
    std::vector<std::uint8_t> pixels;
    int width = 0;
    int height = 0;
    int stride = 0;
};

} // namespace camera_demo
