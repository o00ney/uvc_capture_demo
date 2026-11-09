#pragma once
#include "common/frame.hpp"
#include <string>

namespace image_pipeline {
std::vector<std::uint8_t> read_jpeg(const std::string& path);
void save_ppm(const std::string& path, const RgbImage& image);
// Validates dimensions, stride and backing storage before any image-library call.
void validate_rgb(const RgbImage& image);
void resize_rgb(RgbImage& image, int width, int height);
} // namespace image_pipeline
