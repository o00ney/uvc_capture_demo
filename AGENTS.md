# AGENTS.md

## Core Principles

Less is more. The simplest solution is the best solution. The action hierarchy for every change: Delete > Replace > Add.

- **平台与标准**：代码在 Jetson（AArch64）上编译和运行，使用目标工具链支持的标准 C++20。优先使用标准库，按需采用新特性，集中封装平台相关代码。
- **结构与接口**：职责清晰、接口最小，避免过度抽象和不必要的依赖。头文件自包含，使用明确的类型、命名与 `const`；遵循现有风格，注释解释原因和约束。
- **资源与内存安全**：使用 RAII 管理资源，明确所有权与生命周期；优先值语义和 `std::unique_ptr`，按需共享所有权。检查边界与溢出，避免悬空访问；V4L2 缓冲区归还后不得继续访问，析构函数不得抛出异常。
- **并发与错误处理**：明确共享数据的同步方式和线程退出机制，避免数据竞争、死锁和持锁执行耗时操作。检查系统调用与外部库错误，提供必要上下文，不静默吞错，不以 `assert` 替代运行时检查。
- **性能与修改范围**：先保证正确性，再依据测量优化；减少逐帧路径中不必要的分配、复制与日志，明确队列容量和丢帧策略。保持修改范围最小，优先复用或删除代码，避免无关重构。

## 脚本

- **同步与提交**：依次执行 `./script/sync_to_jetson.sh`、`git commit` 和 `git push`，仅提交本次任务相关文件，任一步失败即停止并报告。脚本通过 `ssh jetson` 将项目同步至 `~/Desktop/uvc_capture_demo/`，同步新增、修改和删除，遵循 Git 忽略规则，保留远端被忽略的文件及 `.git`；可用 `--dry-run` 预览。
