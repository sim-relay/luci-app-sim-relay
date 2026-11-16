# SIM Relay for OpenWrt / LuCI

轻量级短信管理与转发服务，为 OpenWrt 提供 C 后台和 LuCI 界面。项目参考 [SIM Relay](https://github.com/coinlet/sim-relay)，以保留主要功能、减少安装体积和闪存写入为目标；不使用 SQLite 或长期短信归档。

> 当前核心包版本：**0.6.0-r6**。已使用 OpenWrt 24.10.4 x86/64 SDK 构建，并在 ImmortalWrt 24.10.5 x86/64 上安装；设备、固件和架构不同的环境仍需自行验证。

## 功能

- 支持 ZTE MF833U1、本机 ModemManager 蜂窝模块及用于测试的 Mock 设备，可管理多个设备。
- 查看、搜索和手动发送短信，批量删除设备短信；支持 SIM 信息和 PIN 管理。
- 支持 SMS、Telegram、企业微信、飞书和可选 SMTP 共享推送通道，可按设备关联。
- 使用 UCI 保存配置，加密保存凭据与 PIN；短信直接从设备读取，无数据库。
- 持久化待转发任务和每设备的最新短信进度，重启后恢复未完成任务并发现停机期间的新短信。
- 英文主界面，简体中文通过独立 LuCI 语言包提供。

主包不强制依赖 ModemManager：选择添加本机模块时才检查，缺少时提示用户安装。PIN 解锁需要额外的 `sim-relay-modemmanager` 辅助包。驱动、APN 和拨号由系统自行管理。

## 安装

先确认路由器固件的版本和架构，使用与其兼容的 OpenWrt SDK 编译安装包；固件的软件源必须提供匹配架构和 ABI 的依赖。软件包与用途：

| 软件包 | 用途 |
| --- | --- |
| `sim-relay` | 后台、服务和 RPC 接口；依赖由 opkg 从已配置软件源解析 |
| `luci-app-sim-relay` | LuCI 英文界面，依赖后台 |
| `luci-i18n-sim-relay-zh-cn` | 可选简体中文翻译 |
| `sim-relay-modemmanager` | 可选的本机模块 PIN 解锁辅助程序；不会自动安装 ModemManager |
| `sim-relay-smtp` | 可选 SMTP 转发支持 |

将相应 IPK 上传到路由器的 `/tmp` 后执行：

```sh
opkg update
opkg install /tmp/sim-relay_<版本>_<架构>.ipk /tmp/luci-app-sim-relay_<版本>_all.ipk
# 按需安装简体中文和本机模块 PIN 支持：
opkg install /tmp/luci-i18n-sim-relay-zh-cn_<版本>_all.ipk
opkg install /tmp/sim-relay-modemmanager_<版本>_<架构>.ipk
```

请将命令中的占位符替换为实际文件名。`opkg` 会从路由器已配置的软件源安装 `cJSON`、`libsodium` 等缺失依赖；若找不到依赖或架构不兼容，应检查软件源及目标架构，不能混用不兼容的包。使用本机模块还需另行安装并运行系统提供的 `modemmanager`。

安装后在 LuCI 的 **服务 → SIM Relay** 中添加设备，再配置并关联推送通道。中文包安装后可在 LuCI 语言设置中选择简体中文。服务控制可使用 `/etc/init.d/sim-relay {start|stop|restart|status}`。

## 使用 Docker 构建

宿主机需要 Bash、Docker 和可访问的 Docker daemon。默认使用 OpenWrt 24.10.4 x86/64 SDK；默认构建中文语言包，不构建 PIN 辅助包和 SMTP 包。以下命令同时构建所有可选包：

```sh
sudo -n env WITH_MM_PIN=1 WITH_SMTP=1 EXPECT_ELF_MACHINE=62 bash scripts/build-sdk.sh
```

如果当前用户可直接使用 Docker，可去掉 `sudo -n env`。首次运行需要下载镜像和依赖。完成后脚本会打印产物目录，例如 `.sdk-artifacts/<时间戳>-<随机后缀>/`，其中包含 IPK、校验报告和构建日志。`WITH_ZH_CN=0` 可关闭中文包；其他架构需要选择相应 SDK 镜像，不能直接使用 x86/64 产物。

详细参数、产物检查与目标程序 smoke 测试见 [SDK 构建说明](docs/SDK.md)。

## 转发和存储

首次为设备或通道建立进度时，以设备现有短信为历史基线，不转发旧短信。此后服务先将新短信写入待转发文件，再推进时间进度；通道确认成功后从任务中移除，全部成功后删除任务文件。重启后继续处理未完成任务，并根据进度检查设备上更晚收到的短信。

进度按短信接收时间比较，同一时间点用消息 ID 区分。发送成功后、任务文件删除前断电可能导致重复转发；无法解析日期、接收时间异常倒退或扫描前已从设备删除的短信不保证补发。失败或结果不明的转发保留任务并重试。更多边界见 [转发设计](docs/PUSH.md)。

| 路径 | 内容 |
| --- | --- |
| `/etc/config/sim-relay` | UCI 配置、设备和通道信息、加密凭据 |
| `/etc/sim-relay/credentials.key` | 凭据加密主密钥 |
| `/etc/sim-relay/cursor-<设备ID>.json` | 每设备转发进度及通道基线 |
| `/etc/sim-relay/pending/` | 尚未完成的转发任务，包含短信正文 |
| `/tmp/sim-relay/` | 短信快照、临时操作和最近结果；重启后清空 |

备份配置时需要同时保留主密钥；恢复转发进度还需要 cursor 和 pending。软件包提供 sysupgrade 保留规则，适用于选择保留配置并恢复安装本软件的情况。删除设备会清除其关联的 SIM PIN、进度和待处理任务，不删除 SIM 上的短信。旧数据库不会自动导入或删除。

## 开发和验证

本机构建需要 C99 编译器、CMake、libcurl、cJSON 和 libsodium 开发文件；页面测试还需要 Node.js。

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=MinSizeRel -DSIM_RELAY_TESTS=ON
cmake --build build -j2
ctest --test-dir build --output-on-failure
python3 -m unittest discover -s tests -q
python3 scripts/check-i18n.py
```

[当前验证状态](docs/VALIDATION-CURRENT.md) 区分了自动测试、SDK 构建和真实设备检查。实际网络推送、各类设备的完整浏览器交互以及断电恢复仍需在具体环境中验证。

更多文档：[设备与 SIM 管理](MANAGEMENT.md)、[ModemManager](docs/MODEMMANAGER.md)、[短信发送](docs/SENDING.md)、[语言包](docs/I18N.md)、[路线](docs/ROADMAP.md)。

## 许可证

[MIT License](LICENSE)。
