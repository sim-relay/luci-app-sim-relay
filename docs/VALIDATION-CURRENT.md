# 当前验证状态

截至 2026-11-16，核心包为 0.6.0-r6。

## 已验证

- OpenWrt 24.10.4 x86/64 Docker SDK 已构建后台、LuCI、中文语言包和可选辅助包；IPK 检查涵盖目标架构、动态依赖、ACL、安装内容与语言包隔离。
- 在 x86-64 Linux 上使用目标包的 musl loader 与共享库运行 Mock smoke，覆盖配置、Unicode 接收、发送、进度持久化及缓存恢复。
- 在 ImmortalWrt 24.10.5 x86/64 测试路由器上安装和升级后台、LuCI、中文及 ModemManager PIN 辅助包；检查过 LuCI 页面授权和实际提供的脚本。
- 一台 Huawei ME909s-821 通过 ModemManager 发现，SIM 状态和设备短信读取正常；同一模块的重复添加被拒绝。
- LuCI 英文和中文页面逻辑测试使用真实页面代码及 DOM/RPC 替身，覆盖设备、PIN 和通道配置按钮，包括只读权限以及保存失败后按钮恢复。

运行自动测试和重现 SDK 构建的命令见 [README](../README.md#开发和验证) 和 [SDK 构建说明](SDK.md)。

## 尚需实际环境验证

- 真实浏览器中的完整通道配置流程和所有语言切换组合；自动化测试不验证浏览器布局。
- 真实 SMS、Telegram、企业微信、飞书及 SMTP 端到端转发，以及不同硬件、固件和架构。
- 实际断电恢复、sysupgrade 备份与恢复，以及闪存写入量和峰值内存。

历史版本的记录保留在 [0.2](VALIDATION.md)、[0.3](VALIDATION-0.3.md)、[0.4](VALIDATION-0.4.md) 和 [0.5](VALIDATION-0.5.md)；其中的“未完成”是记录当时的状态。
