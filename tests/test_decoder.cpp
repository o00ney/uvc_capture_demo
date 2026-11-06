#include "jpeg_decoder.hpp"

#include <turbojpeg.h>

#include <cstdint>
#include <stdexcept>
#include <vector>

using namespace camera_demo;

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

int main() {
    // A real JPEG generated from hand-chosen red pixels checks the RGB contract.
    std::vector<std::uint8_t> red(4 * 4 * 3, 0);
    for (int i = 0; i < 16; ++i) red[i * 3] = 255;
    tjhandle compressor = tjInitCompress();
    require(compressor != nullptr, "compressor init");
    unsigned char* encoded = nullptr;
    unsigned long encoded_size = 0;
    require(tjCompress2(compressor, red.data(), 4, 0, 4, TJPF_RGB,
                        &encoded, &encoded_size, TJSAMP_444, 100, 0) == 0,
            "JPEG fixture encoding");
    std::vector<std::uint8_t> jpeg(encoded, encoded + encoded_size);
    tjFree(encoded);
    tjDestroy(compressor);

    JpegDecoder decoder(4, 4);
    RgbImage rgb;
    decoder.decode(jpeg, rgb);
    require(rgb.width == 4 && rgb.height == 4 && rgb.stride == 12, "RGB dimensions");
    require(rgb.pixels.size() == 48, "RGB byte count");
    for (int i = 0; i < 16; ++i) {
        require(rgb.pixels[i * 3] > 240, "red channel");
        require(rgb.pixels[i * 3 + 1] < 15, "green channel");
        require(rgb.pixels[i * 3 + 2] < 15, "blue channel");
    }

    bool rejected_bad_jpeg = false;
    try { decoder.decode(std::vector<std::uint8_t>{1, 2, 3}, rgb); }
    catch (const std::runtime_error&) { rejected_bad_jpeg = true; }
    require(rejected_bad_jpeg, "corrupt JPEG rejection");

    bool rejected_wrong_size = false;
    try { JpegDecoder wrong(1280, 720); wrong.decode(jpeg, rgb); }
    catch (const std::runtime_error&) { rejected_wrong_size = true; }
    require(rejected_wrong_size, "dimension mismatch rejection");
}
