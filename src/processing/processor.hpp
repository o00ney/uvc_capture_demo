#pragma once

#include "common/frame.hpp"
#include <array>
#include <cstdint>
#include <memory>

namespace image_pipeline {

// Shared mathematical parameters; CUDA API receives *squared* sigmas.
inline constexpr int median_size = 3;
inline constexpr int bilateral_radius = 4;
inline constexpr float sigma_space = 3;
inline constexpr float sigma_color = 25;
inline constexpr int contrast_size = 15;
inline constexpr std::array<float, 5> gaussian_axis{1.f/16, 4.f/16, 6.f/16, 4.f/16, 1.f/16};

struct ProcessTiming {
    double total_ms = 0;
    double compute_ms = 0;
    double upload_ms = 0;
    double download_ms = 0;
};
class Processor {
public:
    virtual ~Processor() = default;
    // Synchronous completion, no retained reference to input/output after return.
    virtual ProcessTiming process(const RgbImage& input, RgbImage& output) = 0;
};
std::unique_ptr<Processor> make_cpu_processor();
std::unique_ptr<Processor> make_cuda_processor();
void configure_cpu(int threads);

struct ImageDifference {
    int maximum = 0;
    std::uint64_t different = 0;
    double mean_absolute = 0;
    double psnr = 0;
};
ImageDifference compare_images(const RgbImage& a, const RgbImage& b);
} // namespace image_pipeline
