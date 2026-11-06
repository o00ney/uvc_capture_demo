#include "jpeg_decoder.hpp"
#include "latest_frame_slot.hpp"
#include "v4l2_camera.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <exception>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>

using namespace camera_demo;

namespace {

volatile std::sig_atomic_t interrupted = 0;
void on_signal(int) { interrupted = 1; }

struct Options {
    std::string device;
    std::string jpeg_file;
    std::string save_ppm;
    std::uint64_t frames = 600;
    int width = 1280;
    int height = 720;
    int fps = 60;
};

void usage() {
    std::cout << "Usage: camera_demo (--device /dev/videoX | --jpeg-file frame.jpg) "
                 "[--frames 600] [--width 1280] [--height 720] [--fps 60] "
                 "[--save-ppm first-frame.ppm]\n"
                 "--frames 0 runs until Ctrl+C. The JPEG file mode replays one image at the chosen fps.\n";
}

Options parse_options(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i];
        if (key == "--help") { usage(); std::exit(0); }
        if (i + 1 >= argc) throw std::invalid_argument("missing value for " + key);
        const std::string value = argv[++i];
        if (key == "--device") options.device = value;
        else if (key == "--jpeg-file") options.jpeg_file = value;
        else if (key == "--save-ppm") options.save_ppm = value;
        else if (key == "--frames") options.frames = std::stoull(value);
        else if (key == "--width") options.width = std::stoi(value);
        else if (key == "--height") options.height = std::stoi(value);
        else if (key == "--fps") options.fps = std::stoi(value);
        else throw std::invalid_argument("unknown option " + key);
    }
    if (options.device.empty() == options.jpeg_file.empty())
        throw std::invalid_argument("provide exactly one of --device or --jpeg-file");
    if (options.width <= 0 || options.height <= 0 || options.fps <= 0)
        throw std::invalid_argument("width, height and fps must be positive");
    return options;
}

std::vector<std::uint8_t> read_file(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open JPEG file: " + path);
    std::vector<std::uint8_t> bytes(std::istreambuf_iterator<char>{input}, {});
    if (bytes.empty()) throw std::runtime_error("JPEG file is empty");
    return bytes;
}

void save_ppm(const std::string& path, const RgbImage& rgb) {
    std::ofstream output(path, std::ios::binary);
    if (!output) throw std::runtime_error("cannot create PPM file: " + path);
    output << "P6\n" << rgb.width << ' ' << rgb.height << "\n255\n";
    for (int row = 0; row < rgb.height; ++row)
        output.write(reinterpret_cast<const char*>(rgb.pixels.data() + row * rgb.stride),
                     static_cast<std::streamsize>(rgb.width * 3));
    if (!output) throw std::runtime_error("failed writing PPM file: " + path);
}

std::uint64_t replay_file(const Options& options, LatestFrameSlot& slot,
                          const std::atomic<bool>& worker_failed) {
    const auto bytes = read_file(options.jpeg_file);
    const auto period = std::chrono::nanoseconds(1'000'000'000LL / options.fps);
    auto next = std::chrono::steady_clock::now();
    std::uint64_t count = 0;
    while (!interrupted && !worker_failed && (options.frames == 0 || count < options.frames)) {
        JpegFrame frame{bytes, count, std::chrono::steady_clock::now()};
        slot.publish(std::move(frame));
        ++count;
        next += period;
        std::this_thread::sleep_until(next);
    }
    return count;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parse_options(argc, argv);
        std::signal(SIGINT, on_signal);
        std::signal(SIGTERM, on_signal);
        LatestFrameSlot slot;
        std::atomic<bool> worker_failed{false};
        std::atomic<std::uint64_t> decoded{0};
        std::atomic<std::uint64_t> decode_errors{0};
        std::exception_ptr worker_error;

        // This thread owns the decoder and RGB buffer. Its mutex is held only
        // inside slot.take(), never during JPEG decoding or user processing.
        std::thread worker([&] {
            try {
                JpegDecoder decoder(options.width, options.height);
                RgbImage rgb;
                bool saved = false;
                while (auto frame = slot.take()) {
                    try { decoder.decode(frame->bytes, rgb); }
                    catch (const std::exception& error) {
                        ++decode_errors;
                        if (decode_errors <= 3) std::cerr << "decode: " << error.what() << '\n';
                        continue;
                    }
                    // Replace this block with a synchronous algorithm call.
                    // The algorithm must not keep rgb.pixels.data() after return.
                    if (!saved && !options.save_ppm.empty()) {
                        save_ppm(options.save_ppm, rgb);
                        saved = true;
                    }
                    const auto processed = ++decoded;
                    if (processed % 60 == 0)
                        std::cout << "decoded=" << processed << " latest_sequence="
                                  << frame->sequence << '\n';
                }
            } catch (...) {
                worker_error = std::current_exception();
                worker_failed = true;
                slot.close();
            }
        });

        std::uint64_t received = 0;
        std::exception_ptr producer_error;
        const auto started = std::chrono::steady_clock::now();
        try {
            if (!options.device.empty()) {
                CameraConfig config{options.device, static_cast<std::uint32_t>(options.width),
                                    static_cast<std::uint32_t>(options.height),
                                    static_cast<std::uint32_t>(options.fps), 4};
                V4L2Camera camera(config);
                received = camera.run(slot, options.frames,
                                      [&] { return interrupted || worker_failed.load(); });
            } else {
                received = replay_file(options, slot, worker_failed);
            }
        } catch (...) { producer_error = std::current_exception(); }
        slot.close();
        worker.join();
        const auto seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - started).count();
        std::cout << "received=" << received << " decoded=" << decoded
                  << " overwritten_before_decode=" << slot.overwritten()
                  << " decode_errors=" << decode_errors
                  << " elapsed_seconds=" << seconds << '\n';
        if (producer_error) std::rethrow_exception(producer_error);
        if (worker_error) std::rethrow_exception(worker_error);
        if (decoded == 0) throw std::runtime_error("no frame decoded successfully");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        usage();
        return 1;
    }
}
