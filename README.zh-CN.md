# SIM Relay · 短信中继

[English](README.md) | 简体中文

短信中继是面向 OpenWrt 的轻量级短信管理与转发服务，提供 C 后台和 LuCI 界面，无需 SQLite，也不长期归档短信。项目参考 [SIM Relay](https://github.com/coinlet/sim-relay)。

## 功能

- 最多管理 16 台设备：内置 ZTE MF833U1 适配，通过 ModemManager 发现本机蜂窝模块，并提供用于测试的模拟设备。
- 查看、搜索、发送和删除短信；管理 SIM PIN，可按设备设置保留最近短信的数量。
- 将新短信转发到 SMS、Telegram、企业微信、飞书、SMTP 或自定义 GET/POST/PUT Webhook；多个设备可共享和关联通道。
- 可选令牌认证的远程短信 API，支持仅局域网或所有网络监听，以及 HTTP 或 HTTPS。详见 [API 文档](docs/API.md)。
- 使用 UCI 保存配置，加密保存凭据和 PIN，持久化转发进度与待处理任务；简体中文界面通过独立语言包安装。

ModemManager 和设备驱动由系统安装与管理，主包不强制安装 ModemManager。使用本机模块解锁 PIN 还需安装可选包 `sim-relay-modemmanager`。具体设备和固件仍需实机验证。

## 安装

从 [GitHub Releases](https://github.com/home-sim-relay/luci-app-sim-relay/releases) 下载与固件版本及软件包架构兼容的文件。可在路由器上运行 `cat /etc/openwrt_release` 和 `opkg print-architecture` 确认；CPU 系列相同不代表 ABI 一定兼容。发布包使用 OpenWrt **24.10.4** SDK 构建，不保证适用于所有 OpenWrt 或厂商固件。

发布文件名前缀标明 SDK 目标，例如 `x86-64--sim-relay_..._x86_64.ipk`。界面和语言包的内部架构仍是 `all`，前缀用于区分不同目标的构建产物。将同一目标的安装包上传到 `/tmp` 后安装：

```sh
opkg update
opkg install /tmp/<目标>--sim-relay_<版本>_<架构>.ipk \
  /tmp/<目标>--luci-app-sim-relay_<版本>_all.ipk
# 按需安装相同目标的中文语言包和 ModemManager PIN 辅助包：
opkg install /tmp/<目标>--luci-i18n-sim-relay-zh-cn_<版本>_all.ipk
opkg install /tmp/<目标>--sim-relay-modemmanager_<版本>_<架构>.ipk
```

请将占位符替换为实际文件名，并使用兼容的软件源安装依赖。安装后在 LuCI 的 **服务 → 短信中继** 中添加设备；后台可用 `/etc/init.d/sim-relay restart` 重启。

## 文档

- [编译与发布指南](docs/BUILD.zh-CN.md)：支持的架构、Docker SDK 命令、安装包和自动发布流程。
- [开发与设备适配指南](docs/DEVELOPMENT.zh-CN.md)：项目结构、如何添加和测试新设备。
- [远程 API](docs/API.md) · [当前验证状态与限制](docs/VALIDATION-CURRENT.md)。
- [设备与 SIM 管理](MANAGEMENT.md) · [ModemManager](docs/MODEMMANAGER.md) · [转发](docs/PUSH.md) · [短信发送](docs/SENDING.md) · [翻译](docs/I18N.md)。

转发使用每设备持久化进度和待处理文件。通道已接收但待处理文件尚未更新时断电，可能导致重复推送。短信快照、手动操作和最近结果为临时数据；备份与其他边界见上述文档。

## 许可证

[MIT](LICENSE)。
