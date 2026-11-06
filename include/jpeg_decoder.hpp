#pragma once

#include "frame.hpp"

#include <turbojpeg.h>

namespace camera_demo {

// One decoder per worker thread. decode() reuses the caller's RGB storage.
class JpegDecoder {
public:
    JpegDecoder(int expected_width, int expected_height);
    ~JpegDecoder();
    JpegDecoder(const JpegDecoder&) = delete;
    JpegDecoder& operator=(const JpegDecoder&) = delete;

    void decode(const std::vector<std::uint8_t>& jpeg, RgbImage& output);

private:
    tjhandle handle_ = nullptr;
    int expected_width_;
    int expected_height_;
};

} // namespace camera_demo
