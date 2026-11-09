#include "app/pipeline.hpp"
#include "common/frame.hpp"
#include "common/image_io.hpp"
#include "common/latest_slot.hpp"
#include "input/jpeg_decoder.hpp"
#include "input/v4l2.hpp"
#include "processing/processor.hpp"
#include "preview/preview.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <syncstream>
#include <stdexcept>
#include <thread>

namespace image_pipeline {
namespace {
using Clock = std::chrono::steady_clock;
struct Statistics {
    std::uint64_t count = 0;
    ProcessTiming sum;
    double latency_ms = 0;
    void print(const char* name, std::uint64_t overwritten, double seconds) const {
        const double divisor = count ? double(count) : 1;
        std::osyncstream(std::cout) << name << " processed=" << count
            << " fps=" << (seconds > 0 ? count / seconds : 0)
            << " overwritten_before_process=" << overwritten
            << " avg_total_ms=" << sum.total_ms/divisor
            << " avg_compute_ms=" << sum.compute_ms/divisor
            << " avg_upload_ms=" << sum.upload_ms/divisor
            << " avg_download_ms=" << sum.download_ms/divisor
            << " avg_capture_to_output_ms=" << latency_ms/divisor << '\n';
    }
};
std::uint64_t replay(const Options& options, LatestSlot<JpegFrame>& slot,
                     const std::function<bool()>& stop) {
    const auto bytes = read_jpeg(options.jpeg_file);
    const auto period = std::chrono::nanoseconds(1'000'000'000LL/options.fps);
    auto next = Clock::now();
    std::uint64_t count = 0;
    while (!stop() && (options.frames == 0 || count < options.frames)) {
        slot.publish(JpegFrame{bytes, count++, Clock::now()});
        next += period;
        // A 1fps replay must still respond promptly to Ctrl+C / worker errors.
        while (Clock::now() < next && !stop())
            std::this_thread::sleep_until(std::min(next,Clock::now()+std::chrono::milliseconds(20)));
    }
    return count;
}
}
void run_pipeline(const Options& options, const std::function<bool()>& interrupted) {
    // Validate RGB bounds and initialize backend failures before opening a device.
    JpegDecoder decoder(options.width,options.height);
    configure_cpu(options.cpu_threads);
    auto cpu = uses_cpu(options.mode) ? make_cpu_processor() : nullptr;
    auto cuda = uses_cuda(options.mode) ? make_cuda_processor() : nullptr;
    auto preview = options.preview != View::off ? std::make_unique<Preview>(options.port) : nullptr;
    LatestSlot<JpegFrame> jpeg;
    LatestSlot<FramePtr> cpu_input, cuda_input;
    std::atomic<bool> failed{false};
    std::mutex error_mutex;
    std::exception_ptr error;
    auto close_all = [&] { jpeg.close(); cpu_input.close(); cuda_input.close(); };
    auto fail = [&](std::exception_ptr caught) {
        { std::lock_guard lock(error_mutex); if (!error) error = caught; }
        failed = true;
        close_all();
    };
    auto stop = [&] {
        if (preview) preview->rethrow_if_failed();
        return interrupted() || failed.load();
    };
    const auto started = Clock::now();
    Statistics cpu_stats, cuda_stats;
    std::uint64_t decoded = 0, decode_errors = 0, received = 0;
    std::thread decode_thread, cpu_thread, cuda_thread;
    auto process = [&](Processor& processor, LatestSlot<FramePtr>& input,
                       Statistics& stats, View view, const char* name) {
        try {
            RgbImage output;
            while (auto item = input.take()) {
                if (failed) break;
                const auto& frame = **item;
                const auto time = processor.process(frame.image,output);
                ++stats.count;
                stats.sum.total_ms += time.total_ms; stats.sum.compute_ms += time.compute_ms;
                stats.sum.upload_ms += time.upload_ms; stats.sum.download_ms += time.download_ms;
                stats.latency_ms += std::chrono::duration<double,std::milli>(Clock::now()-frame.received_at).count();
                if (preview && options.preview == view)
                    preview->submit(std::make_shared<Frame>(Frame{std::move(output),frame.sequence,frame.received_at}));
                if (stats.count % 60 == 0)
                    stats.print(name,input.overwritten(),std::chrono::duration<double>(Clock::now()-started).count());
            }
        } catch (...) { fail(std::current_exception()); }
    };
    try {
        if (cpu) cpu_thread = std::thread([&] { process(*cpu,cpu_input,cpu_stats,View::cpu,"cpu"); });
        if (cuda) cuda_thread = std::thread([&] { process(*cuda,cuda_input,cuda_stats,View::cuda,"cuda"); });
        decode_thread = std::thread([&] {
            try {
                bool saved = false;
                while (auto frame = jpeg.take()) {
                    if (failed) break;
                    auto rgb = std::make_shared<Frame>();
                    try { decoder.decode(frame->bytes,rgb->image); }
                    catch (const std::exception& caught) {
                        if (++decode_errors <= 3) std::osyncstream(std::cerr) << "decode: " << caught.what() << '\n';
                        continue;
                    }
                    rgb->sequence = frame->sequence; rgb->received_at = frame->received_at;
                    if (!saved && !options.save_ppm.empty()) {
                        save_ppm(options.save_ppm,rgb->image); saved = true;
                    }
                    ++decoded;
                    // Convert to const shared ownership before publishing to any branch.
                    FramePtr original = std::move(rgb);
                    if (cpu) cpu_input.publish(original);
                    if (cuda) cuda_input.publish(original);
                    if (preview && options.preview == View::raw) preview->submit(original);
                }
                cpu_input.close(); cuda_input.close();
            } catch (...) { fail(std::current_exception()); }
        });
        if (options.device.empty()) received = replay(options,jpeg,stop);
        else received = run_camera({options.device,std::uint32_t(options.width),
            std::uint32_t(options.height),std::uint32_t(options.fps)},jpeg,options.frames,stop);
    } catch (...) { fail(std::current_exception()); }
    jpeg.close();
    if (decode_thread.joinable()) decode_thread.join();
    cpu_input.close(); cuda_input.close();
    if (cpu_thread.joinable()) cpu_thread.join();
    if (cuda_thread.joinable()) cuda_thread.join();
    if (preview) {
        try { preview->finish(); }
        catch (...) { fail(std::current_exception()); }
    }
    const double seconds = std::chrono::duration<double>(Clock::now()-started).count();
    std::cout << "received=" << received << " decoded=" << decoded
              << " overwritten_before_decode=" << jpeg.overwritten()
              << " decode_errors=" << decode_errors << " elapsed_seconds=" << seconds << '\n';
    if (cpu) cpu_stats.print("cpu",cpu_input.overwritten(),seconds);
    if (cuda) cuda_stats.print("cuda",cuda_input.overwritten(),seconds);
    if (error) std::rethrow_exception(error);
    if (decoded == 0) throw std::runtime_error("no frame decoded successfully");
}
} // namespace image_pipeline
