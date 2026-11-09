#include "processing/processor.hpp"
#include <stdexcept>
namespace image_pipeline {
std::unique_ptr<Processor> make_cuda_processor() {
    throw std::runtime_error("CUDA processing was not built; configure with -DENABLE_CUDA=ON");
}
}
