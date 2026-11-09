# AGENTS.md

## Core Principles

Less is more. The simplest solution is the best solution. The action hierarchy for every change: Delete > Replace > Add.

- **平台与标准**：代码在 Jetson（AArch64）上编译和运行，使用目标工具链支持的标准 C++20。优先使用标准库，按需采用新特性，集中封装平台相关代码。
- **结构与接口**：职责清晰、接口最小，避免过度抽象和不必要的依赖。头文件自包含，使用明确的类型、命名与 `const`；遵循现有风格，注释解释原因和约束。
- **资源与内存安全**：使用 RAII 管理资源，明确所有权与生命周期；优先值语义和 `std::unique_ptr`，按需共享所有权。检查边界与溢出，避免悬空访问；V4L2 缓冲区归还后不得继续访问，析构函数不得抛出异常。
- **并发与错误处理**：明确共享数据的同步方式和线程退出机制，避免数据竞争、死锁和持锁执行耗时操作。检查系统调用与外部库错误，提供必要上下文，不静默吞错，不以 `assert` 替代运行时检查。
- **性能与修改范围**：先保证正确性，再依据测量优化；减少逐帧路径中不必要的分配、复制与日志，明确队列容量和丢帧策略。保持修改范围最小，优先复用或删除代码，避免无关重构。

## 项目定位与当前约束

- **定位**：Jetson 图像处理与性能对比管线；摄像头采集只是输入模块。收紧采集代码与对外接口，按输入、CPU/CUDA 处理、调试预览划分职责，避免围绕摄像头增加框架。
- **算法**：CPU 与 CUDA 独立消费同一份未处理原图，保持算法、参数、边界规则一致。CUDA 优先使用 NVIDIA 官方算法库，不自写 kernels；CPU 可使用 OpenCV、ARM NEON 等优化。库实现的舍入和精度差异需要报告，不能预先声称逐字节一致或具体加速比。
- **本次阶段**：按用户要求暂不编译、运行测试或基准测试；只做源码和文档静态检查，并明确记录未验证项。后续编译验证以用户的新指示为准。

## Jetson 软件环境

以下版本来自用户于 2026-11-09 提供的 `nvcc --version` 与 `dpkg-query -W` 输出；版本更新后需重新核对。

- **JetPack**：`nvidia-jetpack` / `nvidia-jetpack-dev` / `nvidia-jetpack-runtime` 均为 `6.2.1+b38`。
- **CUDA**：nvcc `12.6.68`（release `12.6`）；`cuda-toolkit-12-6` 为 `12.6.11-1`，`cuda-cudart-12-6` / `cuda-cudart-dev-12-6` 为 `12.6.68-1`。
- **cuDNN**：`libcudnn9-cuda-12` / `libcudnn9-dev-cuda-12` 为 `9.3.0.75-1`。
- **TensorRT**：`libnvinfer10` 及所列开发、插件等带版本包为 `10.3.0.30-1+cuda12.5`；包名中的 `cuda12.5` 不代表当前 nvcc 版本。
- **包状态**：查询输出中没有版本号的匹配项不能视为已安装；不要据此认定同时安装了旧 CUDA、交叉编译工具链或旧 TensorRT。OpenCV/NPP 的开发头文件、组件和 CPU 优化能力需另行确认。

## 脚本

- **同步与提交**：依次执行 `./script/sync_to_jetson.sh`、`git commit` 和 `git push`，仅提交本次任务相关文件，任一步失败即停止并报告。脚本通过 `ssh jetson` 将项目同步至 `~/Desktop/uvc_capture_demo/`，同步新增、修改和删除，遵循 Git 忽略规则，保留远端被忽略的文件及 `.git`；可用 `--dry-run` 预览。
