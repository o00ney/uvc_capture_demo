#include "processing/processor.hpp"
#include "common/image_io.hpp"

#include <opencv2/core.hpp>
#include <opencv2/core/ocl.hpp>
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace image_pipeline {
namespace {
class CpuProcessor final : public Processor {
    std::array<cv::Mat, 3> channels_, output_;
    cv::Mat median_, bilateral_, value_, mean_, enhanced_, blurred_;
public:
    ProcessTiming process(const RgbImage& input, RgbImage& output) override {
        validate_rgb(input);
        if (&input == &output) throw std::invalid_argument("processing output must not alias input");
        resize_rgb(output, input.width, input.height);
        const auto started = std::chrono::steady_clock::now();
        cv::Mat source(input.height, input.width, CV_8UC3,
                       const_cast<std::uint8_t*>(input.pixels.data()), input.stride);
        cv::split(source, channels_.data());
        const cv::Mat axis(1, 5, CV_32F, const_cast<float*>(gaussian_axis.data()));
        for (int c = 0; c < 3; ++c) {
            cv::medianBlur(channels_[c], median_, median_size);
            // Single-channel bilateral gives both libraries the same color distance.
            cv::bilateralFilter(median_, bilateral_, bilateral_radius * 2 + 1,
                                sigma_color, sigma_space, cv::BORDER_REPLICATE);
            bilateral_.convertTo(value_, CV_32F);
            cv::boxFilter(value_, mean_, CV_32F, {contrast_size, contrast_size},
                          {-1, -1}, true, cv::BORDER_REPLICATE);
            cv::addWeighted(value_, 2, mean_, -1, 0, enhanced_, CV_8U);
            enhanced_.convertTo(value_, CV_32F);
            cv::sepFilter2D(value_, blurred_, CV_32F, axis, axis, {-1, -1}, 0,
                            cv::BORDER_REPLICATE);
            cv::addWeighted(value_, 2, blurred_, -1, 0, output_[c], CV_8U);
        }
        cv::Mat destination(output.height, output.width, CV_8UC3,
                            output.pixels.data(), output.stride);
        cv::merge(output_.data(), 3, destination);
        const double elapsed = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count();
        return {elapsed, elapsed, 0, 0};
    }
};
}
void configure_cpu(int threads) {
    if (threads <= 0) throw std::invalid_argument("CPU thread count must be positive");
    cv::ocl::setUseOpenCL(false);
    cv::setUseOptimized(true);
    cv::setNumThreads(threads);
}
std::unique_ptr<Processor> make_cpu_processor() { return std::make_unique<CpuProcessor>(); }
ImageDifference compare_images(const RgbImage& a, const RgbImage& b) {
    validate_rgb(a); validate_rgb(b);
    if (a.width != b.width || a.height != b.height)
        throw std::invalid_argument("cannot compare different image dimensions");
    ImageDifference result;
    long double absolute = 0, squared = 0;
    for (int y = 0; y < a.height; ++y)
        for (int x = 0; x < a.width * 3; ++x) {
            const int delta = std::abs(int(a.pixels[std::size_t(y) * a.stride + x]) -
                                       int(b.pixels[std::size_t(y) * b.stride + x]));
            result.maximum = std::max(result.maximum, delta);
            result.different += delta != 0;
            absolute += delta; squared += delta * delta;
        }
    const long double count = static_cast<long double>(a.width) * a.height * 3;
    result.mean_absolute = static_cast<double>(absolute / count);
    result.psnr = squared == 0 ? std::numeric_limits<double>::infinity() :
                  10 * std::log10(255.L * 255 * count / squared);
    return result;
}
} // namespace image_pipeline
