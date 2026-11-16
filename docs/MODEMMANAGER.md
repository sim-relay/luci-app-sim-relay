# 本机蜂窝模块（ModemManager）

SIM Relay 通过系统 ModemManager 使用本机蜂窝模块，不负责驱动、APN 或拨号配置。后台通过 `mmcli` 发现并管理模块，不链接 libmm-glib；主包和 LuCI 包均不强制依赖 `modemmanager`。

## 安装与添加

从路由器的软件源安装并启动 `modemmanager`。在 LuCI 添加设备时选择本机蜂窝模块并从发现列表选择设备。若缺少 `mmcli`，页面会提示安装 ModemManager，后端会拒绝保存；程序不会自动安装软件包。

设备通过硬件身份绑定，重启后重新解析 ModemManager 的模块路径，不依赖可能变化的模块编号。同一物理模块不允许重复添加。模块启用且 SIM 解锁后可读取短信、收发和删除，并可使用现有转发流程。

## PIN

普通收发不需要项目的 PIN 辅助包。PIN 提交需要额外安装 `sim-relay-modemmanager`，该包依赖 `sim-relay` 和 libdbus，不会自动安装 `modemmanager`。缺少时页面提示安装，不提交 PIN，也不影响普通短信功能。

PIN 通过标准输入传给辅助程序，再由 D-Bus 调用 SendPin，不放进命令行参数。SIM 身份检查、剩余次数和自动解锁规则见 [设备与 SIM 管理](../MANAGEMENT.md)。

## 构建与验证

使用 SDK 脚本并指定 `WITH_MM_PIN=1` 可生成 PIN 辅助包，见 [SDK 构建说明](SDK.md)。项目已在一台 ImmortalWrt 24.10.5 x86/64 路由器上使用 Huawei ME909s-821 验证模块发现、SIM 状态及短信读取。不同模块、发送、自动解锁和异常情况仍需结合实际设备验证；离线测试使用替身，不代表全部硬件均已验证。

参考：[mmcli 手册](https://www.freedesktop.org/software/ModemManager/man/latest/mmcli.1.html)。
