#include "preview/preview.hpp"
#include "common/image_io.hpp"
#include "common/latest_slot.hpp"
#include <turbojpeg.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <cstddef>
#include <exception>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace image_pipeline {
namespace {
using Clock = std::chrono::steady_clock;
struct Socket {
    int fd = -1;
    explicit Socket(int value = -1) : fd(value) {}
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& other) noexcept : fd(std::exchange(other.fd,-1)) {}
    Socket& operator=(Socket&& other) noexcept {
        if (this != &other) { if (fd >= 0) ::close(fd); fd = std::exchange(other.fd,-1); }
        return *this;
    }
    ~Socket() { if (fd >= 0) ::close(fd); }
};
void system_error(const char* operation) {
    throw std::runtime_error(std::string(operation) + ": " + std::strerror(errno));
}
struct Compressor {
    tjhandle handle = tjInitCompress();
    Compressor() { if (!handle) throw std::runtime_error("tjInitCompress failed"); }
    ~Compressor() { tjDestroy(handle); }
};
struct JpegMemory {
    unsigned char* data = nullptr;
    ~JpegMemory() { tjFree(data); }
};
struct Encoded {
    std::vector<std::uint8_t> bytes;
    std::uint64_t number = 0;
};
struct Client {
    Socket socket;
    std::string request, pending;
    std::size_t sent = 0;
    bool streaming = false, close_after_send = false;
    std::uint64_t number = 0;
    Clock::time_point accepted = Clock::now(), progress = accepted, pending_since = accepted;
};
std::string response(const std::string& status, const std::string& body) {
    return "HTTP/1.1 " + status + "\r\nContent-Type: text/html\r\nContent-Length: " +
           std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
}
}
struct Preview::Impl {
    Socket listener;
    LatestSlot<FramePtr> frames;
    std::atomic<bool> stopping{false};
    std::atomic<int> viewers{0};
    std::mutex mutex;
    std::shared_ptr<const Encoded> latest;
    std::exception_ptr error;
    std::thread encoder, network;

    explicit Impl(int port) {
        if (port <= 0 || port > 65535) throw std::invalid_argument("invalid preview port");
        listener.fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (listener.fd < 0) system_error("preview socket");
        int reuse = 1;
        if (setsockopt(listener.fd,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse)) < 0)
            system_error("preview SO_REUSEADDR");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<std::uint16_t>(port));
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(listener.fd,reinterpret_cast<sockaddr*>(&address),sizeof(address)) < 0)
            system_error("preview bind");
        if (::listen(listener.fd,4) < 0) system_error("preview listen");
        std::cout << "preview=http://127.0.0.1:" << port << "/\n";
    }
    template<class F> void guard(F work) noexcept {
        try { work(); }
        catch (...) {
            { std::lock_guard lock(mutex); if (!error) error = std::current_exception(); }
            stopping = true;
            frames.close();
        }
    }
    void start() {
        try {
            encoder = std::thread([this] { guard([&] { encode(); }); });
            network = std::thread([this] { guard([&] { serve(); }); });
        } catch (...) { stop(); throw; }
    }
    void stop() noexcept {
        stopping = true;
        frames.close();
        if (encoder.joinable()) encoder.join();
        if (network.joinable()) network.join();
    }
    void encode() {
        Compressor compressor;
        auto next = Clock::now();
        std::uint64_t count = 0;
        while (auto item = frames.take()) {
            if (stopping) break;
            if (!viewers.load() || Clock::now() < next) continue;
            const auto& image = (*item)->image;
            validate_rgb(image);
            JpegMemory memory;
            unsigned long size = 0;
            if (tjCompress2(compressor.handle,image.pixels.data(),image.width,image.stride,
                image.height,TJPF_RGB,&memory.data,&size,TJSAMP_420,85,0) < 0)
                throw std::runtime_error(std::string("preview JPEG: ") + tjGetErrorStr2(compressor.handle));
            auto encoded = std::make_shared<Encoded>();
            encoded->bytes.assign(memory.data,memory.data+size);
            encoded->number = ++count;
            { std::lock_guard lock(mutex); latest = std::move(encoded); }
            next = Clock::now() + std::chrono::microseconds(1'000'000/15);
        }
    }
    void serve() {
        std::vector<Client> clients;
        clients.reserve(4);
        while (!stopping) {
            std::shared_ptr<const Encoded> image;
            { std::lock_guard lock(mutex); image = latest; }
            for (auto& client : clients) {
                if (client.streaming && client.pending.empty() && image && image->number != client.number) {
                    client.pending = "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: " +
                        std::to_string(image->bytes.size()) + "\r\n\r\n";
                    client.pending.append(reinterpret_cast<const char*>(image->bytes.data()),image->bytes.size());
                    client.pending += "\r\n";
                    client.number = image->number;
                    client.sent = 0;
                    client.progress = client.pending_since = Clock::now();
                }
            }
            std::vector<pollfd> descriptors{{listener.fd,POLLIN,0}};
            for (const auto& client : clients)
                descriptors.push_back({client.socket.fd,short(POLLIN | (client.pending.empty()?0:POLLOUT)),0});
            int ready = ::poll(descriptors.data(),descriptors.size(),100);
            if (ready < 0) { if (errno == EINTR) continue; system_error("preview poll"); }
            for (std::size_t i=clients.size();i>0;--i) {
                auto& client = clients[i-1];
                const auto events = descriptors[i].revents;
                bool drop = events & (POLLERR|POLLHUP|POLLNVAL);
                const auto now = Clock::now();
                if ((!client.streaming && now-client.accepted > std::chrono::seconds(5)) ||
                    (!client.pending.empty() && (now-client.progress > std::chrono::seconds(5) ||
                     now-client.pending_since > std::chrono::seconds(5)))) drop = true;
                if (!drop && (events & POLLIN)) {
                    char buffer[1024];
                    const auto length = ::recv(client.socket.fd,buffer,sizeof(buffer),0);
                    if (length == 0 || (length < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) drop = true;
                    if (length > 0 && !client.streaming && !client.close_after_send) {
                        client.request.append(buffer,static_cast<std::size_t>(length));
                        if (client.request.size() > 4096) drop = true;
                        else if (client.request.find("\r\n\r\n") != std::string::npos) {
                            if (client.request.starts_with("GET /stream.mjpg HTTP/1.")) {
                                client.streaming = true; ++viewers;
                                client.pending = "HTTP/1.1 200 OK\r\nContent-Type: multipart/x-mixed-replace; boundary=frame\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
                            } else {
                                client.close_after_send = true;
                                client.pending = client.request.starts_with("GET / HTTP/1.") ?
                                    response("200 OK","<!doctype html><title>Image pipeline preview</title><img src=\"/stream.mjpg\" alt=\"Preview\" style=\"max-width:100%\">") :
                                    response("404 Not Found","Not found");
                            }
                            client.progress = client.pending_since = now;
                            client.request.clear();
                        }
                    }
                }
                if (!drop && !client.pending.empty() && (events & POLLOUT)) {
                    const auto length = ::send(client.socket.fd,client.pending.data()+client.sent,
                                               client.pending.size()-client.sent,MSG_NOSIGNAL);
                    if (length < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) drop = true;
                    if (length > 0) { client.sent += std::size_t(length); client.progress = now; }
                    if (client.sent == client.pending.size()) {
                        client.pending.clear(); client.sent = 0;
                        if (client.close_after_send) drop = true;
                    }
                }
                if (drop) {
                    if (client.streaming) --viewers;
                    clients.erase(clients.begin()+static_cast<std::ptrdiff_t>(i-1));
                }
            }
            if (descriptors[0].revents & (POLLERR|POLLHUP|POLLNVAL))
                throw std::runtime_error("preview listening socket failed");
            if (descriptors[0].revents & POLLIN) {
                // Bound accept work too, so connection floods cannot delay shutdown.
                for (int n=0;n<8;++n) {
                    Socket incoming(::accept4(listener.fd,nullptr,nullptr,SOCK_NONBLOCK|SOCK_CLOEXEC));
                    if (incoming.fd < 0) {
                        if (errno == EINTR) continue;
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        system_error("preview accept");
                    }
                    if (clients.size() < 4) { Client client; client.socket = std::move(incoming); clients.push_back(std::move(client)); }
                }
            }
        }
        viewers = 0;
    }
};
Preview::Preview(int port) : impl_(std::make_unique<Impl>(port)) { impl_->start(); }
Preview::~Preview() { impl_->stop(); }
void Preview::submit(FramePtr frame) { if (impl_->viewers.load()) impl_->frames.publish(std::move(frame)); }
void Preview::finish() { impl_->stop(); rethrow_if_failed(); }
void Preview::rethrow_if_failed() {
    std::exception_ptr error;
    { std::lock_guard lock(impl_->mutex); error = impl_->error; }
    if (error) std::rethrow_exception(error);
}
} // namespace image_pipeline
