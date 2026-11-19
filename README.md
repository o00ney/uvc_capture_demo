# Jetson 图像处理管线

项目在 Jetson ARM CPU 和 CUDA 上处理同一份未处理原图，并提供可选的 SSH 视频预览。摄像头采集是输入模块；文件回放可脱离摄像头使用。仓库目录仍为 `uvc_capture_demo`，主程序改为 `image_pipeline`。

**当前状态：源码与静态审阅阶段，按要求未编译、未运行测试、未验证实机视频流或测量性能。** 以下命令供后续验证使用，不表示已执行成功。

## 数据流与结构

```text
V4L2 MJPEG / JPEG文件 → JPEG最新帧槽 → RGB解码 → 不可变原图
                                             ├→ CPU最新帧槽 → OpenCV CPU
                                             └→ CUDA最新帧槽 → NVIDIA NPP
原图 / CPU结果 / CUDA结果 → 预览最新帧槽 → JPEG编码 → HTTP MJPEG
```

`src/input` 只负责采集和解码，`src/processing` 放两种算法后端，`src/preview` 独立提供调试视频流，`src/app` 负责参数和编排，`src/common` 放帧、图像IO及容量1的槽，`src/benchmark` 做固定输入比较。采集接口收敛为函数；V4L2类型不进入算法或预览模块。

所有槽只保留最新一帧，满时覆盖尚未消费的旧帧；已取走的帧不会被覆盖。V4L2缓冲区中的压缩字节复制后立即QBUF，RGB通过不可变共享所有权分发；CPU与CUDA都直接读取原图，不串联。慢算法、编码及客户端不会长期占用采集缓冲区。两路速度不同时会处理不同序号，因此实时两路当前画面不能用来进行逐像素对比。

## 算法与依赖

两端保持相同顺序、参数及复制边缘的边界规则，RGB分别按单通道处理：

1. 3×3中值去噪。
2. 9×9双边滤波：半径4的圆形支持域，空间sigma=3、强度sigma=25，采样间隔1。
3. 15×15盒式均值的局部对比度增强：`saturate(2*input - mean)`。
4. 5×5非锐化掩模：Gaussian核为`[1,4,6,4,1]/16`的外积，输出`saturate(2*input - blur)`。

每级结果为8位RGB；局部均值与锐化的中间运算采用32位浮点，阶段输出饱和量化。公共参数定义在 `src/processing/processor.hpp`。

CPU使用OpenCV `Mat`：medianBlur、bilateralFilter、boxFilter、sepFilter2D、addWeighted。启动时关闭OpenCL、启用CPU优化并配置线程数；ARM NEON和并行能力取决于安装的OpenCV构建，未声称已经验证。CUDA只调用NVIDIA官方NPP和CUDA Runtime，不写kernel，也不需要编译`.cu`文件；内部缓冲区、stream和events复用，阶段间数据留在设备上。

OpenCV和NPP内部的近似、累积和舍入可能产生不同结果；当前不承诺逐字节一致，也没有未经测量确定的误差阈值。先核对算法定义，再依据固定输入基准报告的误差评价结果。

## 后续构建

目标为Linux/AArch64、C++20，CMake最低3.18。已知Jetson OpenCV版本4.8.0，CUDA12.6头文件存在；完整构建兼容性尚未验证。环境版本见 `AGENTS.md`。CPU依赖OpenCV core/imgproc、libjpeg-turbo和Threads；GPU额外依赖CUDA Runtime及NPP的nppc/nppif/nppial/nppidei开发组件。cuDNN和TensorRT不是此管线的依赖。

安装缺少的开发包需自行核对当前设备环境，程序不会自动安装依赖。后续允许验证时可运行：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DENABLE_CUDA=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

没有CUDA时设置`-DENABLE_CUDA=OFF`；显式请求CUDA会报错，不静默回退。CUDA测试必须在有可用GPU的机器运行；`ctest -LE cuda`可排除它。HTTP回归需要Python3标准库。现有解码/槽/CLI回归已调整到新结构，并补充算法和HTTP用例，均未执行。

## 运行与预览

```bash
# 摄像头：CPU、CUDA各自处理原图，预览CUDA结果
./build/image_pipeline --device /dev/video0 --frames 0 --process both --preview cuda

# CPU四线程，预览CPU结果
./build/image_pipeline --device /dev/video0 --frames 0 --process cpu --cpu-threads 4 --preview cpu

# 只预览未处理的原图
./build/image_pipeline --device /dev/video0 --frames 0 --preview raw

# 重复读取一张720p JPEG；不模拟真实运动
./build/image_pipeline --jpeg-file sample.jpg --frames 0 --process both --preview cuda
```

`--width 1280 --height 720 --fps 60`是默认输入配置，JPEG尺寸必须匹配；小图传实际尺寸。数字只接受完整非负十进制整数，宽高和fps必须为正，fps不超过1,000,000,000；`--frames 0`持续至Ctrl+C。`--save-ppm original.ppm`保存首张成功解码的原图。

`--process none|cpu|cuda|both`默认none；`--cpu-threads N`默认为min(4,可用逻辑核数)，范围1–1024。`--preview off|raw|cpu|cuda`默认off；处理结果源必须启用对应后端。`--port`范围1–65535，默认8080。预览仅监听127.0.0.1，浏览器页面`/`，MJPEG路径`/stream.mjpg`。

在电脑另一个终端建立SSH转发：

```bash
ssh -N -L 8080:127.0.0.1:8080 jetson
```

浏览器打开 `http://127.0.0.1:8080/`。本地8080占用时改用`-L 8081:127.0.0.1:8080`，访问本地8081。

预览独立编码，最多15fps、JPEG质量85；没有观看客户端时跳过编码。最多4个连接，每个连接只保留当前待发送数据，不累积历史帧。请求大小和时限有界，卡住的客户端断开；程序退出时停止网络和编码线程。当前选择通过启动参数确定，不提供网页运行时切换。

## 性能统计与固定输入比较

实时输出及退出汇总包含：received、decoded、decode_errors、overwritten_before_decode；每路processed、实际处理fps、overwritten_before_process、平均算法总耗时、设备处理/上传/下载耗时、采集至输出延迟。各路覆盖数仅代表相应槽，不包含摄像头/驱动内部丢帧。fps含等待输入时间，不能直接作为算法极限吞吐。CUDA设备时间用同一stream的events，等待同步完成再读；主机总时间包含传输、提交和同步，初始化及可复用存储的扩容在计时区域外。

单独比较同一张JPEG解码后的RGB，关闭采集、预览和两路竞争：

```bash
./build/image_benchmark --jpeg-file sample.jpg --cpu-threads 4 --iterations 20 --save-prefix result
# 无CUDA时
./build/image_benchmark --jpeg-file sample.jpg --process cpu --iterations 20
```

各预热3次，分别顺序测量。输出平均/中位总耗时、CUDA设备与传输时间、含传输及设备阶段的加速比，以及最大误差、MAE、不同字节数、PSNR；可保存`result-cpu.ppm`和`result-cuda.ppm`。这些是库实现的观察值，不能在未检查误差前将加速比解释为已证明等价的算法比较。目前没有实测结果或帧率承诺。

## 开发脚本

在 Mac 上执行，通过 `ssh jetson` 操作设备；`perf_mode.sh` 和 `bench.sh` 需要远端免密 `sudo`。

```bash
./script/jetson_check.sh [--no-cuda] [--clean]   # 同步后远端 cmake、构建、ctest
./script/perf_mode.sh lock|restore|status       # jetson_clocks 锁频/恢复；nvpmodel 只报告不切换
./script/bench.sh [--jpeg input.jpg] [image_benchmark 参数...]
```

`bench.sh` 同步并构建 `image_benchmark`，不给 JPEG 时用 GStreamer 生成720p测试图（不如真实画面有代表性）。结果连同 `meta.txt`（commit、功耗模式、是否锁频、参数）、`tegrastats.log` 和输出图拉回本地 `build/bench/<时间戳>/`。未锁频时会警告；锁频后记得 `restore`。

## 输入边界

摄像头仍要求单平面Video Capture、mmap、MJPEG和指定尺寸。程序请求帧率并报告设备协商结果，设备必须支持MJPEG图像节点。断开或流错误时报错退出，不做自动重连。采集实现保留RAII、格式与长度检查及错误诊断；没有为缩短代码删除这些约束。

参考：[NPP滤波API](https://docs.nvidia.com/cuda/archive/12.6.3/npp/image_filtering_functions.html)、[OpenCV滤波API](https://docs.opencv.org/4.x/d4/d86/group__imgproc__filter.html)。用户提供的[Multimedia API示例](https://docs.nvidia.com/jetson/archives/r36.4/ApiReference/l4t_mm_12_v4l2_camera_cuda.html)演示YUV采集、缓冲区共享和画框；本项目保留现有MJPEG输入，使用NPP提供滤波。
