#include "common/image_io.hpp"
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>

namespace image_pipeline {
namespace {
std::size_t image_size(int width, int height, int stride) {
    if (width <= 0 || height <= 0 || width > std::numeric_limits<int>::max() / 3 ||
        stride < width * 3)
        throw std::invalid_argument("invalid RGB dimensions or stride");
    const auto rows = static_cast<std::size_t>(height);
    const auto step = static_cast<std::size_t>(stride);
    if (step > std::vector<std::uint8_t>{}.max_size() / rows)
        throw std::invalid_argument("RGB image size exceeds storage range");
    return step * rows;
}
}
void validate_rgb(const RgbImage& image) {
    if (image.pixels.size() < image_size(image.width, image.height, image.stride))
        throw std::invalid_argument("RGB backing storage is too small");
}
void resize_rgb(RgbImage& image, int width, int height) {
    if (width <= 0 || width > std::numeric_limits<int>::max() / 3)
        throw std::invalid_argument("invalid RGB width");
    const auto size = image_size(width, height, width * 3);
    image.pixels.resize(size);
    image.width = width; image.height = height; image.stride = width * 3;
}
std::vector<std::uint8_t> read_jpeg(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open JPEG file: " + path);
    std::vector<std::uint8_t> bytes(std::istreambuf_iterator<char>{input}, {});
    if (input.bad()) throw std::runtime_error("failed reading JPEG file: " + path);
    if (bytes.empty()) throw std::runtime_error("JPEG file is empty");
    return bytes;
}
void save_ppm(const std::string& path, const RgbImage& image) {
    validate_rgb(image);
    std::ofstream output(path, std::ios::binary);
    if (!output) throw std::runtime_error("cannot create PPM file: " + path);
    output << "P6\n" << image.width << ' ' << image.height << "\n255\n";
    for (int y = 0; y < image.height; ++y)
        output.write(reinterpret_cast<const char*>(image.pixels.data() +
                     static_cast<std::size_t>(y) * image.stride), image.width * 3);
    if (!output) throw std::runtime_error("failed writing PPM file: " + path);
}
} // namespace image_pipeline
