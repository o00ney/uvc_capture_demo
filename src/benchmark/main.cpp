#include "app/options.hpp"
#include "common/image_io.hpp"
#include "input/jpeg_decoder.hpp"
#include "processing/processor.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace image_pipeline;
int main(int argc, char** argv) {
    try {
        std::string file, save_prefix;
        int iterations = 20;
        int threads = int(std::min(4u,std::max(1u,std::thread::hardware_concurrency())));
        bool with_cuda = true;
        for (int i=1;i<argc;++i) {
            const std::string key = argv[i];
            if (key == "--help") {
                std::cout << "Usage: image_benchmark --jpeg-file input.jpg [--iterations 20] [--cpu-threads N]\n"
                             "  [--process cpu|both] [--save-prefix result]\n";
                return 0;
            }
            if (i+1 >= argc) throw std::invalid_argument("missing value for " + key);
            const std::string value = argv[++i];
            if (key == "--jpeg-file") file = value;
            else if (key == "--save-prefix") save_prefix = value;
            else if (key == "--iterations") iterations = int(parse_unsigned(value,1'000'000,key));
            else if (key == "--cpu-threads") threads = int(parse_unsigned(value,1024,key));
            else if (key == "--process" && (value == "cpu" || value == "both")) with_cuda = value == "both";
            else throw std::invalid_argument("invalid benchmark option: " + key);
        }
        if (file.empty() || iterations <= 0 || threads <= 0)
            throw std::invalid_argument("provide --jpeg-file and positive iteration/thread counts");
        const auto jpeg = read_jpeg(file);
        // Read dimensions once; decoding is outside the measured region.
        tjhandle header = tjInitDecompress();
        if (!header) throw std::runtime_error("JPEG header initialization failed");
        int width=0,height=0,subsampling=0,color=0;
        const int status = tjDecompressHeader3(header,jpeg.data(),jpeg.size(),&width,&height,&subsampling,&color);
        tjDestroy(header);
        if (status < 0) throw std::runtime_error("invalid JPEG header");
        RgbImage input;
        JpegDecoder(width,height).decode(jpeg,input);
        configure_cpu(threads);
        auto cpu = make_cpu_processor();
        auto cuda = with_cuda ? make_cuda_processor() : nullptr;
        std::cout << "size=" << width << 'x' << height << " cpu_threads=" << threads
                  << " warmup=3 iterations=" << iterations << '\n';
        auto measure = [&](Processor& processor, RgbImage& output, const char* name) {
            for (int i=0;i<3;++i) processor.process(input,output);
            ProcessTiming sum;
            std::vector<double> samples;
            samples.reserve(iterations);
            for (int i=0;i<iterations;++i) {
                const auto time = processor.process(input,output);
                sum.total_ms += time.total_ms; sum.compute_ms += time.compute_ms;
                sum.upload_ms += time.upload_ms; sum.download_ms += time.download_ms;
                samples.push_back(time.total_ms);
            }
            std::sort(samples.begin(),samples.end());
            std::cout << name << " avg_total_ms=" << sum.total_ms/iterations
                << " median_total_ms=" << samples[samples.size()/2]
                << " avg_compute_ms=" << sum.compute_ms/iterations
                << " avg_upload_ms=" << sum.upload_ms/iterations
                << " avg_download_ms=" << sum.download_ms/iterations << '\n';
            sum.total_ms /= iterations; sum.compute_ms /= iterations;
            return sum;
        };
        RgbImage cpu_output,cuda_output;
        const auto cpu_time = measure(*cpu,cpu_output,"cpu");
        if (!save_prefix.empty()) save_ppm(save_prefix+"-cpu.ppm",cpu_output);
        if (cuda) {
            const auto gpu_time = measure(*cuda,cuda_output,"cuda");
            const auto diff = compare_images(cpu_output,cuda_output);
            std::cout << "max_error=" << diff.maximum << " mae=" << diff.mean_absolute
                      << " different_bytes=" << diff.different << " psnr=" << diff.psnr << '\n';
            if (gpu_time.total_ms > 0 && gpu_time.compute_ms > 0)
                std::cout << "total_speedup=" << cpu_time.total_ms/gpu_time.total_ms
                          << " device_speedup=" << cpu_time.compute_ms/gpu_time.compute_ms << '\n';
            if (!save_prefix.empty()) save_ppm(save_prefix+"-cuda.ppm",cuda_output);
            std::cout << "Numerical equivalence requires reviewing the reported differences; no acceptance threshold is assumed.\n";
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n'; return 1;
    }
}
