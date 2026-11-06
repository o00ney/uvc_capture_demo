#include "jpeg_decoder.hpp"

#include <stdexcept>
#include <string>

namespace camera_demo {

JpegDecoder::JpegDecoder(int expected_width, int expected_height)
    : expected_width_(expected_width), expected_height_(expected_height) {
    if (expected_width <= 0 || expected_height <= 0)
        throw std::invalid_argument("expected JPEG dimensions must be positive");
    handle_ = tjInitDecompress();
    if (!handle_) throw std::runtime_error("tjInitDecompress failed");
}

JpegDecoder::~JpegDecoder() { tjDestroy(handle_); }

void JpegDecoder::decode(const std::vector<std::uint8_t>& jpeg, RgbImage& output) {
    int width = 0;
    int height = 0;
    int subsampling = 0;
    int colorspace = 0;
    if (jpeg.empty() || tjDecompressHeader3(handle_, jpeg.data(), jpeg.size(),
                                            &width, &height, &subsampling, &colorspace) < 0)
        throw std::runtime_error(std::string("invalid JPEG: ") + tjGetErrorStr2(handle_));
    if (width != expected_width_ || height != expected_height_)
        throw std::runtime_error("JPEG dimensions differ from configured capture size");

    output.width = width;
    output.height = height;
    output.stride = width * 3;
    output.pixels.resize(static_cast<std::size_t>(output.stride) * height);
    if (tjDecompress2(handle_, jpeg.data(), jpeg.size(), output.pixels.data(),
                      width, output.stride, height, TJPF_RGB, 0) < 0)
        throw std::runtime_error(std::string("JPEG decode failed: ") + tjGetErrorStr2(handle_));
}

} // namespace camera_demo
