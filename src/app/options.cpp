#include "app/options.hpp"
#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <thread>

namespace image_pipeline {
bool uses_cpu(Mode mode) { return mode == Mode::cpu || mode == Mode::both; }
bool uses_cuda(Mode mode) { return mode == Mode::cuda || mode == Mode::both; }
void usage() {
    std::cout << "Usage: image_pipeline (--device /dev/videoX | --jpeg-file frame.jpg)\n"
                 "  [--frames 600] [--width 1280] [--height 720] [--fps 60]\n"
                 "  [--process none|cpu|cuda|both] [--cpu-threads N]\n"
                 "  [--preview off|raw|cpu|cuda] [--port 8080] [--save-ppm original.ppm]\n"
                 "--frames 0 runs until Ctrl+C. Preview listens on 127.0.0.1 only.\n";
}
std::uint64_t parse_unsigned(const std::string& value, std::uint64_t maximum,
                             const std::string& option) {
    std::uint64_t number = 0;
    const auto* end = value.data() + value.size();
    const auto result = std::from_chars(value.data(), end, number);
    if (result.ec != std::errc{} || result.ptr != end || number > maximum)
        throw std::invalid_argument("invalid value for " + option + ": " + value);
    return number;
}
Options parse_options(int argc, char** argv) {
    Options options;
    options.cpu_threads = int(std::min(4u, std::max(1u, std::thread::hardware_concurrency())));
    for (int i=1; i<argc; ++i) {
        const std::string key = argv[i];
        if (key == "--help") { usage(); std::exit(0); }
        if (i+1 >= argc) throw std::invalid_argument("missing value for " + key);
        const std::string value = argv[++i];
        if (key == "--device") options.device = value;
        else if (key == "--jpeg-file") options.jpeg_file = value;
        else if (key == "--save-ppm") options.save_ppm = value;
        else if (key == "--frames") options.frames = parse_unsigned(value, UINT64_MAX, key);
        else if (key == "--width") options.width = int(parse_unsigned(value, std::numeric_limits<int>::max(), key));
        else if (key == "--height") options.height = int(parse_unsigned(value, std::numeric_limits<int>::max(), key));
        else if (key == "--fps") options.fps = int(parse_unsigned(value, 1'000'000'000, key));
        else if (key == "--cpu-threads") options.cpu_threads = int(parse_unsigned(value, 1024, key));
        else if (key == "--port") options.port = int(parse_unsigned(value, 65535, key));
        else if (key == "--process") {
            if (value == "none") options.mode = Mode::none;
            else if (value == "cpu") options.mode = Mode::cpu;
            else if (value == "cuda") options.mode = Mode::cuda;
            else if (value == "both") options.mode = Mode::both;
            else throw std::invalid_argument("invalid value for --process: " + value);
        } else if (key == "--preview") {
            if (value == "off") options.preview = View::off;
            else if (value == "raw") options.preview = View::raw;
            else if (value == "cpu") options.preview = View::cpu;
            else if (value == "cuda") options.preview = View::cuda;
            else throw std::invalid_argument("invalid value for --preview: " + value);
        } else throw std::invalid_argument("unknown option " + key);
    }
    if (options.device.empty() == options.jpeg_file.empty())
        throw std::invalid_argument("provide exactly one of --device or --jpeg-file");
    if (!options.width) throw std::invalid_argument("--width must be positive");
    if (!options.height) throw std::invalid_argument("--height must be positive");
    if (!options.fps) throw std::invalid_argument("--fps must be positive");
    if (!options.cpu_threads) throw std::invalid_argument("--cpu-threads must be positive");
    if (!options.port) throw std::invalid_argument("--port must be positive");
    if (options.preview == View::cpu && !uses_cpu(options.mode))
        throw std::invalid_argument("--preview cpu requires --process cpu or both");
    if (options.preview == View::cuda && !uses_cuda(options.mode))
        throw std::invalid_argument("--preview cuda requires --process cuda or both");
    return options;
}
} // namespace image_pipeline
