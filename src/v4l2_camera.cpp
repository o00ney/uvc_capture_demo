#include "v4l2_camera.hpp"

#include <stdexcept>
#include <utility>

#ifdef __linux__
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <iostream>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <type_traits>
#include <vector>

namespace {

int xioctl(int fd, unsigned long request, void* argument) {
    int result;
    do { result = ioctl(fd, request, argument); } while (result < 0 && errno == EINTR);
    return result;
}

std::runtime_error device_error(const std::string& device, const char* operation) {
    const int error = errno;
    return std::runtime_error(device + ": " + operation + ": " + std::strerror(error));
}

void check_ioctl(int fd, unsigned long request, void* argument, const char* operation,
                 const std::string& device) {
    if (xioctl(fd, request, argument) < 0)
        throw device_error(device, operation);
}

struct Mapping {
    void* data = MAP_FAILED;
    std::size_t size = 0;
};

void report_cleanup_error(const std::string& device, const char* operation) noexcept {
    const int error = errno;
    std::fprintf(stderr, "%s: %s: errno=%d (%s)\n",
                 device.c_str(), operation, error, std::strerror(error));
}

struct DeviceState {
    explicit DeviceState(const std::string& path) : device(path) {}
    DeviceState(const DeviceState&) = delete;
    DeviceState& operator=(const DeviceState&) = delete;

    const std::string& device; // Borrows config_.device for the duration of run().
    int fd = -1;
    std::vector<Mapping> mappings;
    bool streaming = false;

    ~DeviceState() noexcept {
        const int saved_errno = errno;
        if (fd >= 0 && streaming) {
            v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            if (xioctl(fd, VIDIOC_STREAMOFF, &type) < 0)
                report_cleanup_error(device, "VIDIOC_STREAMOFF");
        }
        for (const auto& mapping : mappings)
            if (mapping.data != MAP_FAILED && munmap(mapping.data, mapping.size) < 0)
                report_cleanup_error(device, "munmap");
        if (fd >= 0 && close(fd) < 0) report_cleanup_error(device, "close");
        errno = saved_errno;
    }
};

static_assert(!std::is_copy_constructible_v<DeviceState>);
static_assert(!std::is_copy_assignable_v<DeviceState>);
static_assert(!std::is_move_constructible_v<DeviceState>);
static_assert(!std::is_move_assignable_v<DeviceState>);

} // namespace
#endif

namespace camera_demo {

V4L2Camera::V4L2Camera(CameraConfig config) : config_(std::move(config)) {
    if (config_.device.empty() || config_.width == 0 || config_.height == 0 ||
        config_.fps == 0 || config_.buffer_count < 2)
        throw std::invalid_argument("invalid camera configuration");
}

std::uint64_t V4L2Camera::run(LatestFrameSlot& slot, std::uint64_t max_frames,
                              const std::function<bool()>& stop) {
#ifndef __linux__
    (void)slot; (void)max_frames; (void)stop;
    throw std::runtime_error("V4L2 camera mode requires Linux");
#else
    DeviceState state(config_.device);
    state.fd = open(config_.device.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (state.fd < 0)
        throw device_error(config_.device, "open");

    v4l2_capability capability{};
    check_ioctl(state.fd, VIDIOC_QUERYCAP, &capability, "VIDIOC_QUERYCAP", config_.device);
    const auto caps = (capability.capabilities & V4L2_CAP_DEVICE_CAPS)
                          ? capability.device_caps : capability.capabilities;
    if (!(caps & V4L2_CAP_VIDEO_CAPTURE) || !(caps & V4L2_CAP_STREAMING))
        throw std::runtime_error(config_.device + ": node lacks Video Capture or Streaming capability");
    std::cout << "driver=" << capability.driver << " bus=" << capability.bus_info << '\n';

    v4l2_format format{};
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    format.fmt.pix.width = config_.width;
    format.fmt.pix.height = config_.height;
    format.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    format.fmt.pix.field = V4L2_FIELD_ANY;
    check_ioctl(state.fd, VIDIOC_S_FMT, &format, "VIDIOC_S_FMT", config_.device);
    if (format.fmt.pix.width != config_.width || format.fmt.pix.height != config_.height ||
        format.fmt.pix.pixelformat != V4L2_PIX_FMT_MJPEG)
        throw std::runtime_error(config_.device + ": requested " +
            std::to_string(config_.width) + "x" + std::to_string(config_.height) +
            " MJPEG (fourcc=" + std::to_string(V4L2_PIX_FMT_MJPEG) + "), got " +
            std::to_string(format.fmt.pix.width) + "x" + std::to_string(format.fmt.pix.height) +
            " (fourcc=" + std::to_string(format.fmt.pix.pixelformat) + ")");

    v4l2_streamparm rate{};
    rate.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    rate.parm.capture.timeperframe.numerator = 1;
    rate.parm.capture.timeperframe.denominator = config_.fps;
    check_ioctl(state.fd, VIDIOC_S_PARM, &rate, "VIDIOC_S_PARM", config_.device);
    const auto interval = rate.parm.capture.timeperframe;
    if (interval.numerator == 0 || interval.denominator == 0)
        throw std::runtime_error(config_.device + ": camera returned an invalid frame interval");
    std::cout << "negotiated " << format.fmt.pix.width << 'x' << format.fmt.pix.height
              << " MJPEG at " << static_cast<double>(interval.denominator) / interval.numerator
              << " fps\n";

    v4l2_requestbuffers request{};
    request.count = config_.buffer_count;
    request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    request.memory = V4L2_MEMORY_MMAP;
    check_ioctl(state.fd, VIDIOC_REQBUFS, &request, "VIDIOC_REQBUFS", config_.device);
    if (request.count < 2) throw std::runtime_error(config_.device + ": camera allocated fewer than two buffers");
    state.mappings.reserve(request.count);
    for (std::uint32_t i = 0; i < request.count; ++i) {
        v4l2_buffer buffer{};
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = i;
        check_ioctl(state.fd, VIDIOC_QUERYBUF, &buffer, "VIDIOC_QUERYBUF", config_.device);
        void* data = mmap(nullptr, buffer.length, PROT_READ | PROT_WRITE,
                          MAP_SHARED, state.fd, buffer.m.offset);
        if (data == MAP_FAILED)
            throw device_error(config_.device, "mmap");
        state.mappings.push_back({data, buffer.length});
        check_ioctl(state.fd, VIDIOC_QBUF, &buffer, "initial VIDIOC_QBUF", config_.device);
    }
    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    check_ioctl(state.fd, VIDIOC_STREAMON, &type, "VIDIOC_STREAMON", config_.device);
    state.streaming = true;

    std::uint64_t received = 0;
    while (!stop() && (max_frames == 0 || received < max_frames)) {
        pollfd descriptor{state.fd, POLLIN, 0};
        int ready;
        do { ready = poll(&descriptor, 1, 500); } while (ready < 0 && errno == EINTR);
        if (ready < 0) throw device_error(config_.device, "poll");
        if (ready == 0) continue;
        if (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL))
            throw std::runtime_error(config_.device + ": camera disconnected or stream failed");
        if (!(descriptor.revents & POLLIN)) continue;

        v4l2_buffer buffer{};
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        if (xioctl(state.fd, VIDIOC_DQBUF, &buffer) < 0) {
            if (errno == EAGAIN || errno == EIO) continue;
            throw device_error(config_.device, "VIDIOC_DQBUF");
        }
        if (buffer.index >= state.mappings.size() ||
            buffer.bytesused > state.mappings[buffer.index].size)
            throw std::runtime_error(config_.device + ": camera returned an invalid buffer");

        JpegFrame frame;
        const auto* start = static_cast<const std::uint8_t*>(state.mappings[buffer.index].data);
        frame.bytes.assign(start, start + buffer.bytesused);
        frame.sequence = buffer.sequence;
        frame.received_at = std::chrono::steady_clock::now();
        check_ioctl(state.fd, VIDIOC_QBUF, &buffer, "VIDIOC_QBUF", config_.device);
        if (!frame.bytes.empty()) slot.publish(std::move(frame));
        ++received;
    }
    return received;
#endif
}

} // namespace camera_demo
