#include "processing/processor.hpp"
#include "common/image_io.hpp"

#include <cuda_runtime_api.h>
#include <npp.h>
#include <nppi.h>
#include <array>
#include <chrono>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>

namespace image_pipeline {
namespace {
void check(cudaError_t status, const char* operation) {
    if (status != cudaSuccess)
        throw std::runtime_error(std::string(operation) + ": " + cudaGetErrorString(status));
}
void check(NppStatus status, const char* operation) {
    // Positive statuses are warnings; no warning should silently leave stale output.
    if (status != NPP_SUCCESS)
        throw std::runtime_error(std::string(operation) + ": NPP status " + std::to_string(status));
}
void cleanup(cudaError_t status, const char* operation) noexcept {
    if (status != cudaSuccess)
        std::fprintf(stderr, "%s: %s\n", operation, cudaGetErrorString(status));
}
struct DeviceBuffer {
    void* data = nullptr;
    std::size_t size = 0;
    DeviceBuffer() = default;
    DeviceBuffer(const DeviceBuffer&) = delete;
    DeviceBuffer& operator=(const DeviceBuffer&) = delete;
    ~DeviceBuffer() { if (data) cleanup(cudaFree(data), "cudaFree"); }
    void allocate(std::size_t bytes) {
        if (bytes <= size) return;
        void* fresh = nullptr;
        check(cudaMalloc(&fresh, bytes), "cudaMalloc");
        if (data) cleanup(cudaFree(data), "cudaFree");
        data = fresh; size = bytes;
    }
    template<class T> T* as() { return static_cast<T*>(data); }
};
struct Stream {
    cudaStream_t handle = nullptr;
    Stream() { check(cudaStreamCreateWithFlags(&handle, cudaStreamNonBlocking), "cudaStreamCreate"); }
    ~Stream() {
        cleanup(cudaStreamSynchronize(handle), "cudaStreamSynchronize (cleanup)");
        cleanup(cudaStreamDestroy(handle), "cudaStreamDestroy");
    }
};
struct Event {
    cudaEvent_t handle = nullptr;
    Event() { check(cudaEventCreate(&handle), "cudaEventCreate"); }
    ~Event() { cleanup(cudaEventDestroy(handle), "cudaEventDestroy"); }
};
class CudaProcessor final : public Processor {
    // Destroy stream before buffers, even if process exits via an exception.
    DeviceBuffer packed_, result_, planes_, output_planes_, temp8_, padded8_;
    DeviceBuffer value_, mean_, twice_, padded_float_, scratch_, kernel_;
    std::array<Event, 4> events_;
    Stream stream_;
    NppStreamContext context_{};

    void record(int index) { check(cudaEventRecord(events_[index].handle, stream_.handle), "cudaEventRecord"); }
    double elapsed(int first, int last) {
        float ms = 0;
        check(cudaEventElapsedTime(&ms, events_[first].handle, events_[last].handle), "cudaEventElapsedTime");
        return ms;
    }
public:
    CudaProcessor() {
        check(cudaGetDevice(&context_.nCudaDeviceId), "cudaGetDevice");
        cudaDeviceProp properties{};
        check(cudaGetDeviceProperties(&properties, context_.nCudaDeviceId), "cudaGetDeviceProperties");
        context_.hStream = stream_.handle;
        context_.nMultiProcessorCount = properties.multiProcessorCount;
        context_.nMaxThreadsPerMultiProcessor = properties.maxThreadsPerMultiProcessor;
        context_.nMaxThreadsPerBlock = properties.maxThreadsPerBlock;
        context_.nSharedMemPerBlock = properties.sharedMemPerBlock;
        context_.nCudaDevAttrComputeCapabilityMajor = properties.major;
        context_.nCudaDevAttrComputeCapabilityMinor = properties.minor;
        check(cudaStreamGetFlags(stream_.handle, &context_.nStreamFlags), "cudaStreamGetFlags");
        std::array<float, 25> weights{};
        for (int y = 0; y < 5; ++y)
            for (int x = 0; x < 5; ++x) weights[y*5+x] = gaussian_axis[y] * gaussian_axis[x];
        kernel_.allocate(sizeof(weights));
        check(cudaMemcpy(kernel_.data, weights.data(), sizeof(weights), cudaMemcpyHostToDevice), "upload Gaussian kernel");
    }
    ProcessTiming process(const RgbImage& input, RgbImage& output) override {
        validate_rgb(input);
        if (&input == &output) throw std::invalid_argument("processing output must not alias input");
        const int w = input.width, h = input.height;
        constexpr int halo = contrast_size / 2;
        if (w > std::numeric_limits<int>::max() / int(sizeof(float)) - 2*halo ||
            h > std::numeric_limits<int>::max() - 2*halo)
            throw std::invalid_argument("image dimensions exceed NPP pitch/halo range");
        const int pitch = w + 2*halo, rows = h + 2*halo, fpitch = w * int(sizeof(float));
        const auto area = static_cast<std::size_t>(w) * h;
        const auto padded_area = static_cast<std::size_t>(pitch) * rows;
        if (padded_area > std::numeric_limits<std::size_t>::max() / sizeof(float))
            throw std::invalid_argument("NPP image allocation overflows");
        packed_.allocate(area*3); result_.allocate(area*3);
        planes_.allocate(area*3); output_planes_.allocate(area*3);
        temp8_.allocate(area); padded8_.allocate(padded_area);
        value_.allocate(area*sizeof(float)); mean_.allocate(area*sizeof(float));
        twice_.allocate(area*sizeof(float)); padded_float_.allocate(padded_area*sizeof(float));
        const NppiSize roi{w,h}, full{pitch,rows};
        const NppiPoint offset{halo,halo};
        Npp32u scratch_size = 0;
        check(nppiFilterMedianGetBufferSize_8u_C1R(roi, {median_size,median_size}, &scratch_size), "median scratch size");
        scratch_.allocate(scratch_size ? scratch_size : 1);
        resize_rgb(output, w, h);
        auto* source = packed_.as<Npp8u>();
        auto* destination = result_.as<Npp8u>();
        Npp8u* channels[3]; Npp8u* outputs[3];
        for (int c=0;c<3;++c) {
            channels[c] = planes_.as<Npp8u>() + c*area;
            outputs[c] = output_planes_.as<Npp8u>() + c*area;
        }
        auto* temp = temp8_.as<Npp8u>();
        auto* pad8 = padded8_.as<Npp8u>();
        auto* center8 = pad8 + halo*pitch + halo;
        auto* value = value_.as<Npp32f>();
        auto* mean = mean_.as<Npp32f>();
        auto* twice = twice_.as<Npp32f>();
        auto* padf = padded_float_.as<Npp32f>();
        auto* centerf = padf + halo*pitch + halo;
        const int padded_fpitch = pitch * int(sizeof(float));
        const auto started = std::chrono::steady_clock::now();
        record(0);
        check(cudaMemcpy2DAsync(source, w*3, input.pixels.data(), input.stride, w*3, h,
                               cudaMemcpyHostToDevice, stream_.handle), "upload RGB");
        record(1);
        check(nppiCopy_8u_C3P3R_Ctx(source, w*3, channels, w, roi, context_), "RGB split");
        for (int c=0;c<3;++c) {
            check(nppiCopyReplicateBorder_8u_C1R_Ctx(channels[c],w,roi,pad8,pitch,full,halo,halo,context_), "median halo");
            check(nppiFilterMedian_8u_C1R_Ctx(center8,pitch,temp,w,roi,{median_size,median_size},{1,1},scratch_.as<Npp8u>(),context_), "median");
            check(nppiCopyReplicateBorder_8u_C1R_Ctx(temp,w,roi,pad8,pitch,full,halo,halo,context_), "bilateral halo");
            check(nppiFilterBilateralGaussBorder_8u_C1R_Ctx(center8,pitch,full,offset,outputs[c],w,roi,
                bilateral_radius,1,sigma_color*sigma_color,sigma_space*sigma_space,NPP_BORDER_REPLICATE,context_), "bilateral");
            check(nppiConvert_8u32f_C1R_Ctx(outputs[c],w,value,fpitch,roi,context_), "contrast to float");
            check(nppiCopyReplicateBorder_32f_C1R_Ctx(value,fpitch,roi,padf,padded_fpitch,full,halo,halo,context_), "box halo");
            check(nppiFilterBoxBorder_32f_C1R_Ctx(centerf,padded_fpitch,full,offset,mean,fpitch,roi,
                {contrast_size,contrast_size},{halo,halo},NPP_BORDER_REPLICATE,context_), "box mean");
            check(nppiMulC_32f_C1R_Ctx(value,fpitch,2,twice,fpitch,roi,context_), "contrast gain");
            // NPP subtract computes pSrc2 - pSrc1, unlike cv::subtract.
            check(nppiSub_32f_C1R_Ctx(mean,fpitch,twice,fpitch,value,fpitch,roi,context_), "contrast subtract");
            check(nppiConvert_32f8u_C1R_Ctx(value,fpitch,temp,w,roi,NPP_RND_NEAR,context_), "contrast saturate");
            check(nppiConvert_8u32f_C1R_Ctx(temp,w,value,fpitch,roi,context_), "sharp to float");
            check(nppiCopyReplicateBorder_32f_C1R_Ctx(value,fpitch,roi,padf,padded_fpitch,full,halo,halo,context_), "Gaussian halo");
            check(nppiFilterBorder_32f_C1R_Ctx(centerf,padded_fpitch,full,offset,mean,fpitch,roi,
                kernel_.as<Npp32f>(),{5,5},{2,2},NPP_BORDER_REPLICATE,context_), "Gaussian");
            check(nppiMulC_32f_C1R_Ctx(value,fpitch,2,twice,fpitch,roi,context_), "sharp gain");
            check(nppiSub_32f_C1R_Ctx(mean,fpitch,twice,fpitch,value,fpitch,roi,context_), "sharp subtract");
            check(nppiConvert_32f8u_C1R_Ctx(value,fpitch,outputs[c],w,roi,NPP_RND_NEAR,context_), "sharp saturate");
        }
        const Npp8u* const_outputs[3]{outputs[0],outputs[1],outputs[2]};
        check(nppiCopy_8u_P3C3R_Ctx(const_outputs,w,destination,w*3,roi,context_), "RGB merge");
        record(2);
        check(cudaMemcpy2DAsync(output.pixels.data(),output.stride,destination,w*3,w*3,h,
                               cudaMemcpyDeviceToHost,stream_.handle), "download RGB");
        record(3);
        check(cudaStreamSynchronize(stream_.handle), "processing completion");
        const double total = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
        return {total,elapsed(1,2),elapsed(0,1),elapsed(2,3)};
    }
};
}
std::unique_ptr<Processor> make_cuda_processor() { return std::make_unique<CudaProcessor>(); }
} // namespace image_pipeline
