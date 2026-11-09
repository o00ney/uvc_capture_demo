#pragma once
#include "common/frame.hpp"
#include <memory>

namespace image_pipeline {
// Optional debug sink, independent of input and processing libraries.
// Owns encoder/network workers; submit never waits for JPEG encoding or clients.
class Preview {
public:
    explicit Preview(int port);
    ~Preview();
    Preview(const Preview&) = delete;
    Preview& operator=(const Preview&) = delete;
    void submit(FramePtr frame);
    void finish();
    void rethrow_if_failed();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace image_pipeline
