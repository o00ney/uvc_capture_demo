#pragma once
#include <cstdint>
#include <string>

namespace image_pipeline {
enum class Mode { none, cpu, cuda, both };
enum class View { off, raw, cpu, cuda };
struct Options {
    std::string device, jpeg_file, save_ppm;
    std::uint64_t frames = 600;
    int width = 1280, height = 720, fps = 60;
    int cpu_threads = 1;
    Mode mode = Mode::none;
    View preview = View::off;
    int port = 8080;
};
bool uses_cpu(Mode mode);
bool uses_cuda(Mode mode);
Options parse_options(int argc, char** argv);
std::uint64_t parse_unsigned(const std::string& value, std::uint64_t maximum,
                             const std::string& option);
void usage();
} // namespace image_pipeline
