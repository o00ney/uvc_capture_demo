#include "processing/processor.hpp"
#include "common/image_io.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>

using namespace image_pipeline;
static void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
int main(int argc, char**) {
    configure_cpu(1);
    auto cpu = make_cpu_processor();
    auto gpu = argc > 1 ? make_cuda_processor() : nullptr;
    for (const auto size : {1, 4, 23}) {
        RgbImage input;
        resize_rgb(input, size, size);
        std::fill(input.pixels.begin(), input.pixels.end(), 93);
        RgbImage out;
        cpu->process(input, out);
        require(out.pixels == input.pixels, "constant field preserved including borders");
        // Distinct channels, edges and nonuniform detail catch channel/sampling mistakes.
        for (std::size_t i = 0; i < input.pixels.size(); ++i)
            input.pixels[i] = static_cast<std::uint8_t>((i * 71 + i / 7) % 256);
        const auto original = input.pixels;
        cpu->process(input, out);
        require(input.pixels == original, "original RGB is immutable");
        require(out.width == size && out.height == size, "dimensions preserved");
        RgbImage padded{std::vector<std::uint8_t>((size * 3 + 5) * size, 0),
                        size, size, size * 3 + 5};
        for (int y = 0; y < size; ++y)
            std::copy_n(input.pixels.data() + y * input.stride, size * 3,
                        padded.pixels.data() + y * padded.stride);
        RgbImage padded_out;
        cpu->process(padded, padded_out);
        require(padded_out.pixels == out.pixels, "stride padding is ignored");
        if (gpu) {
            RgbImage cuda_out;
            gpu->process(padded, cuda_out);
            const auto difference = compare_images(out, cuda_out);
            std::cout << "size=" << size << " max_error=" << difference.maximum
                      << " mae=" << difference.mean_absolute << '\n';
            // Numerical equivalence has no unmeasured acceptance threshold yet.
            require(cuda_out.width == size && cuda_out.height == size, "CUDA dimensions");
            std::fill(input.pixels.begin(), input.pixels.end(), 93);
            gpu->process(input, cuda_out);
            require(cuda_out.pixels == input.pixels, "CUDA constant field and borders");
        }
    }

    RgbImage salt;
    resize_rgb(salt,23,23);
    for (int c=0;c<3;++c) salt.pixels[(11*23+11)*3+c] = 255;
    RgbImage clean;
    cpu->process(salt,clean);
    require(std::all_of(clean.pixels.begin(),clean.pixels.end(),[](auto v) { return v == 0; }),
            "isolated impulse removed before sharpening");
    if (gpu) {
        gpu->process(salt,clean);
        require(std::all_of(clean.pixels.begin(),clean.pixels.end(),[](auto v) { return v == 0; }),
                "CUDA isolated impulse removed before sharpening");
    }
    bool alias_rejected = false;
    try { cpu->process(salt,salt); }
    catch (const std::invalid_argument&) { alias_rejected = true; }
    require(alias_rejected, "in-place mutation of original rejected");
    RgbImage invalid{{1,2,3}, 2, 1, 6}, out;
    bool rejected = false;
    try { cpu->process(invalid, out); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "truncated backing storage rejected");
}
