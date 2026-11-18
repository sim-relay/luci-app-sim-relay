# 当前验证状态

核心包为 0.6.0-r10。以下设备实测记录针对此前版本；r8 的依赖调整、r9 的后台修复和 r10 的远程 API 尚需在路由器上验证。

## 已验证

- OpenWrt 24.10.4 x86/64 Docker SDK 已构建后台、LuCI、中文语言包和可选辅助包；IPK 检查涵盖目标架构、动态依赖、ACL、安装内容与语言包隔离。
- OpenWrt 24.10.4 `mipsel_24kc` SDK 已编译 r10 后台、LuCI、中文语言包和可选 PIN 辅助包；发布整理检查了包架构、32 位 ELF 和小端字节序，尚未在 MIPS 路由器上安装。
- 使用固定提交时间，两次 x86/64 编译以及一次 `mipsel_24kc` 编译生成一致的 LuCI 和语言包版本；带架构前缀的 x86/64 IPK 已用 SDK 的 opkg 离线安装到隔离目录，依赖使用测试替身。
- 2026-11-17 使用同一脚本完成 `aarch64_generic` 24.10.4 SDK 构建：`sim-relay` 为 `aarch64_generic` 架构，ELF machine 183，使用 `/lib/ld-musl-aarch64.so.1`，依赖检查通过；尚未在 arm64 路由器上安装运行。
- 在 x86-64 Linux 上使用目标包的 musl loader 与共享库运行 Mock smoke，覆盖配置、Unicode 接收、发送、进度持久化及缓存恢复。
- 在 ImmortalWrt 24.10.5 x86/64 测试路由器上安装和升级后台、LuCI、中文及 ModemManager PIN 辅助包；检查过 LuCI 页面授权和实际提供的脚本。
- 一台 Huawei ME909s-821 通过 ModemManager 发现，SIM 状态和设备短信读取正常；同一模块的重复添加被拒绝。
- LuCI 英文和中文页面逻辑测试使用真实页面代码及 DOM/RPC 替身，覆盖设备、PIN 和通道配置按钮，包括只读权限以及保存失败后按钮恢复。
- 使用 OpenWrt x86/64 的 uhttpd 和 musl 库，在隔离容器中验证 API 的 HTTP、HTTPS 证书校验、令牌认证、提交和查询；短信设备使用离线替身。

运行自动测试和重现 SDK 构建的命令见[开发与设备适配指南](DEVELOPMENT.zh-CN.md#本地构建和测试)和[编译与发布指南](BUILD.zh-CN.md)。

## 尚需实际环境验证

- 真实浏览器中的完整通道配置流程和所有语言切换组合；自动化测试不验证浏览器布局。
- 真实 SMS、Telegram、企业微信、飞书及 SMTP 端到端转发，以及不同硬件、固件和架构。
- 远程 API 的路由器实机启动、真实 SIM 发送、局域网地址变化、WAN 防火墙和多种证书配置。
- arm64 已完成此前版本的 SDK 构建、`mipsel_24kc` 已完成 r10 SDK 构建，但均尚未实机安装；其余矩阵架构仍需实际构建和验证。
- 实际断电恢复、sysupgrade 备份与恢复，以及闪存写入量和峰值内存。

历史版本的记录保留在 [0.2](VALIDATION.md)、[0.3](VALIDATION-0.3.md)、[0.4](VALIDATION-0.4.md) 和 [0.5](VALIDATION-0.5.md)；其中的“未完成”是记录当时的状态。
