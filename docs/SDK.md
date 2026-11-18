# 使用 Docker 和 OpenWrt SDK 构建

最新的双语构建、架构和自动发布说明见 [编译与发布指南](BUILD.zh-CN.md) / [Build guide](BUILD.md)。

构建脚本使用官方 `ghcr.io/openwrt/sdk` 镜像，默认目标为 OpenWrt 24.10.4 x86/64，也可以通过 `ARCH` 选择 arm64、ARMv7、MIPS 等主流路由器架构。此前已用 x86/64 和 aarch64_generic 配置成功编译 0.6.0-r7；x86/64 已在 ImmortalWrt 24.10.5 上安装，arm64 包尚未实机安装。其他固件、目标架构和软件源需要单独确认兼容性。镜像使用方法参见 [OpenWrt Docker 项目](https://github.com/openwrt/docker)。

## 构建

在项目根目录运行：

```sh
sudo -n env WITH_MM_PIN=1 bash scripts/build-sdk.sh
```

arm64（`aarch64_generic`）：

```sh
sudo -n env ARCH=arm64 WITH_MM_PIN=1 bash scripts/build-sdk.sh
```

一次构建多个架构：

```sh
sudo -n env ARCH=x86-64,aarch64_generic,mipsel_24kc bash scripts/build-sdk.sh
```

宿主机需要 Bash、Docker 和网络连接。若当前用户可直接访问 Docker daemon，可去掉 `sudo -n env`。首次构建会下载 SDK 镜像、feeds 和源码，耗时及空间取决于网络和缓存。构建容器以只读方式挂载源码；SDK 工作区、feeds 和下载缓存在 Docker named volume 中。每个架构使用独立镜像、缓存卷和产物目录。

## 架构

`ARCH` 接受单个值或用逗号/空格分隔的列表，默认 `x86-64`。别名会解析为官方 SDK 的架构标签：

| ARCH 值 | 解析结果 | 常见设备 |
| --- | --- | --- |
| `x86-64`、`amd64` | `x86-64` | x86 软路由、虚拟机 |
| `arm64`、`aarch64` | `aarch64_generic` | armvirt、rockchip 等 |
| `aarch64_cortex-a53` | `aarch64_cortex-a53` | MediaTek Filogic、ipq807x、sunxi |
| `aarch64_cortex-a72` | `aarch64_cortex-a72` | Raspberry Pi 4、mvebu Cortex-A72 |
| `armv7` | `arm_cortex-a7_neon-vfpv4` | 常见 32 位 ARM 路由器 |
| `arm_cortex-a9_vfpv3-d16` | `arm_cortex-a9_vfpv3-d16` | Marvell Armada 38x |
| `arm_cortex-a15_neon-vfpv4` | `arm_cortex-a15_neon-vfpv4` | ipq806x |
| `mips_24kc` | `mips_24kc` | ath79 |
| `mipsel_24kc` | `mipsel_24kc` | ramips（mt7621 等） |

路由器固件的软件源必须提供与目标架构、ABI 匹配的依赖；CPU 相同不代表包架构一定相同，应以固件的 `opkg print-architecture` 输出为准。第三方或厂商 SDK 可通过 `SDK_IMAGE` 覆盖官方镜像，但该选项只适用于单个 `ARCH`。

## 常用变量

| 变量 | 默认值 | 用途 |
| --- | --- | --- |
| `ARCH` | `x86-64` | 一个或多个架构，支持上表别名 |
| `SDK_VERSION` | `24.10.4` | 官方 SDK 版本，例如 `24.10.5`、`23.05.5` |
| `SDK_IMAGE` | 按 ARCH 和版本生成 | 覆盖 SDK 镜像；仅限单个 ARCH |
| `SDK_LOCAL_IMAGE` | `sim-relay-sdk:local-<ARCH>` | 本地构建镜像标签；仅限单个 ARCH |
| `SDK_VOLUME` | 按构建镜像 ID 命名 | 指定专用缓存卷，不要跨 SDK 版本或架构复用 |
| `JOBS` | `2` | 并发数 |
| `WITH_ZH_CN` | `1` | 编译独立简体中文语言包；`0` 仅构建英文 |
| `WITH_MM_PIN` | `0` | 设为 `1` 编译可选 ModemManager PIN 辅助包 |
| `EXPECT_ELF_MACHINE` | 按 ARCH 自动选择 | 覆盖 ELF 架构检查；仅限单个 ARCH |

`SDK_IMAGE`、`SDK_LOCAL_IMAGE`、`SDK_VOLUME` 和 `EXPECT_ELF_MACHINE` 针对单个架构；与多个 `ARCH` 同时使用会直接报错，避免不同架构共用缓存或产物。

`WITH_MM_PIN=1` 只构建依赖 libdbus 的辅助程序，不会安装或强制依赖系统 `modemmanager`；辅助包需在路由器上按需安装。SMTP 所需的 `msmtp` 和 `ca-bundle` 是主包依赖，由 opkg 从软件源安装。

## 产物和验证

成功后命令输出 `Verified SDK build artifacts:`，每个架构在 `.sdk-artifacts/<时间戳>-<架构>-<随机后缀>/` 下生成独立目录。目录还包含 `verification.json`、`SHA256SUMS`、源码哈希、SDK 配置、feeds 版本和完整日志。目录不会提交到 Git。

```sh
# x86-64
python3 scripts/inspect-ipk.py .sdk-artifacts/<x86-64 目录> --machine 62
# arm64
python3 scripts/inspect-ipk.py .sdk-artifacts/<arm64 目录> --machine 183
# 32 位 ARM 为 40，MIPS 为 8
```

检查器核对包内容、权限、ACL、ELF 架构和动态依赖，并检查中文包隔离及主包不依赖 SQLite/ModemManager。构建脚本已按 `ARCH` 自动传入 ELF 架构检查。

`scripts/smoke-sdk.py` 只在 x86-64 Linux 上运行包内 musl loader 和共享库，覆盖 Mock 和持久化恢复；arm64/MIPS 产物可以正常构建和检查，但该 smoke 脚本不模拟对应架构执行。实际网络推送、真实硬件和外部平台仍需在目标设备验证。

从产物中选择与路由器固件和架构兼容的包，安装步骤见[中文 README](../README.zh-CN.md#安装)。实际新增磁盘占用取决于路由器已有依赖，不能只用项目 IPK 大小估算。

旧版本的构建数据保留在 [历史验证记录](VALIDATION.md)，不能代表当前版本。
