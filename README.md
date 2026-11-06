# Jetson USB 摄像头采集与 MJPEG → RGB 示例

这是一个 **C++20、Linux V4L2 + mmap、libjpeg-turbo** 的最小示例。目标是从 UVC 摄像头取 1280×720 MJPEG 帧，把尚未处理的旧帧丢弃，并将选中的帧解码为 RGB。当前没有算法；`src/main.cpp` 中标出的代码块是以后接算法的位置。

## 在 Jetson 上构建

先确认使用的是 `Device Caps: Video Capture` 的 USB 摄像头节点，而不是 `Metadata Capture` 或 Tegra CSI 节点。

```bash
sudo apt update
sudo apt install -y build-essential cmake pkg-config libturbojpeg0-dev v4l-utils
pkg-config --modversion libturbojpeg

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

如果系统使用不同的包管理器，安装对应的 C++ 编译器、CMake、pkg-config 和 libjpeg-turbo 开发包即可。

## 运行

把 `/dev/videoX` 换成你验证过、支持 MJPEG 1280×720 的图像节点：

```bash
./build/camera_demo --device /dev/videoX --frames 600 --save-ppm first.ppm
```

数字参数只接受完整的非负十进制整数；宽高必须为正且不超过 `INT_MAX`，fps 范围为 `1～1,000,000,000`，frames 允许为 0。RGB 尺寸还必须满足 stride 和存储大小限制；这些是输入边界，并不表示设备支持如此大的尺寸或帧率。

程序请求 60 fps，并打印摄像头返回的实际帧率。`first.ppm` 是第一张成功解码的 RGB 图像，可用支持 PPM 的图片查看器检查颜色。`--frames 0` 会持续运行，按 Ctrl+C 停止。

没有摄像头时，可以重复读取一张 **1280×720 JPEG** 来验证线程交接和解码：

```bash
./build/camera_demo --jpeg-file sample.jpg --frames 120 --save-ppm first.ppm
```

这个文件模式只重复同一张图片，不模拟真实运动；它用于检查解码和线程结构。测试小尺寸图片时可以同时传 `--width` 和 `--height`。

## 两个线程分别做什么

```text
主线程：V4L2 DQBUF → 复制压缩的 JPEG 字节 → V4L2 QBUF → 发布最新帧
                                                          ↓
工作线程：                    等待最新帧 → JPEG 解码成 RGB → 处理 RGB
```

- **主线程是生产者。** 它从驱动取出一个 mmap 缓冲区，只复制 `bytesused` 字节到 `JpegFrame::bytes`，然后立刻 `QBUF`。此后驱动可以覆盖原 mmap 内存，但 `JpegFrame` 拥有自己的数据副本。
- **工作线程是消费者。** 它从 `LatestFrameSlot` 取走最新帧。解码和后续算法都在锁外执行，因此算法再慢也不会长时间占用互斥锁或 V4L2 缓冲区。
- **槽只有一个位置。** 新帧到来时，如果旧帧还没被取走，就覆盖旧帧并增加 `overwritten_before_decode`。已经取走、正在解码的帧不会被覆盖。这个计数不包含摄像头或驱动内部可能丢失的帧。
- **`std::mutex` 保护共享槽和计数。** 生产者与消费者不能同时修改同一份 `pending_`。`std::condition_variable` 让消费者在没有新帧时睡眠，而不是循环占用 CPU。`take()` 返回时锁已释放。
- **`std::move` 转移所有权。** `JpegFrame` 中的 `std::vector` 从采集线程交给槽，再交给工作线程；转移时通常只移动 vector 的内部指针，不再复制整张 JPEG。
- **解码器和 RGB 缓冲区只归工作线程所有。** 不需要给它们加锁。RGB 缓冲区会复用；未来算法必须在调用返回前用完 RGB 数据，不能保存其裸指针。
- **关闭时**，`close()` 唤醒等待的消费者；主线程再 `join()` 工作线程，确保退出前线程已结束。

`received` 是从 V4L2 取到的帧数，`decoded` 是成功解码的帧数。算法慢时两者可以相差很多，而 `overwritten_before_decode` 应增加。若输出 `decode_errors` 非零，优先检查选中的节点和 MJPEG 数据是否有效。

## 接入算法

在 `src/main.cpp` 工作线程里的 `// Replace this block with a synchronous algorithm call.` 处调用你的算法。输入为：

```cpp
rgb.pixels.data()  // RGBRGB...，每通道 8 bit
rgb.width          // 1280
rgb.height         // 720
rgb.stride         // 每行字节数；本示例为 width * 3
frame->sequence    // 摄像头帧序号；可看出跳过的帧
```

算法可以处理慢一些；下一次 `take()` 会获得当时槽里最新的帧。如果未来算法需要在函数返回后继续持有图像，必须复制数据或重新设计 RGB 缓冲区所有权。

## 当前边界

此示例要求设备接受 **单平面 Video Capture、mmap、MJPEG、指定宽高**。`VIDIOC_S_PARM` 请求 60 fps，但设备可能协商到其他帧率；程序会打印实际值。摄像头断开或流出错时程序报错退出，尚未实现自动重连。本次重构未编译、未运行测试；C++20 工具链兼容性、文件回放与 V4L2 实机取流均待后续在 Jetson 上验证。
