# 编译和发布安装包

[English](BUILD.md) | 简体中文 · [返回首页](../README.zh-CN.md)

请使用与路由器固件和软件包架构匹配的 OpenWrt SDK。在路由器运行 `cat /etc/openwrt_release` 和 `opkg print-architecture`，确认软件源可提供 ABI 兼容的依赖。CPU 系列相同不代表安装包一定兼容。以下命令只编译 IPK，不会安装或升级路由器。

## 环境与编译

需要 Bash、能够访问 Docker daemon 的权限、Python 3，以及下载 SDK 镜像、feeds 和源码的网络。如尚未获取项目：

```sh
git clone https://github.com/home-sim-relay/luci-app-sim-relay.git
cd luci-app-sim-relay
```

在项目根目录运行：

```sh
# x86/64，包含中文翻译和可选 PIN 辅助包
WITH_MM_PIN=1 bash scripts/build-sdk.sh

# 编译另一个目标
ARCH=mipsel_24kc WITH_MM_PIN=1 bash scripts/build-sdk.sh

# 依次编译多个架构，分别输出目录
ARCH=x86-64,aarch64_generic,mipsel_24kc WITH_MM_PIN=1 bash scripts/build-sdk.sh
```

如果本机 Docker 需要提升权限，请按实际环境授权，例如 `sudo -n env ARCH=x86-64 WITH_MM_PIN=1 bash scripts/build-sdk.sh`。首次构建会下载 SDK 和依赖。源码以只读方式挂载，SDK 工作目录和缓存放在 Docker named volume 中；成功后脚本输出独立目录 `.sdk-artifacts/<时间戳>-<目标>-<后缀>/`。

默认镜像是官方 `ghcr.io/openwrt/sdk:<目标>-24.10.4`；镜像标签规则见 [OpenWrt Docker 项目](https://github.com/openwrt/docker#sdk)。可通过 `SDK_VERSION` 选择其他 SDK 版本，安装前应确认与固件及软件源的 ABI 兼容性。

| `ARCH` 参数 | SDK 目标 | 常见 CPU 系列 |
| --- | --- | --- |
| `x86-64`、`x86_64`、`amd64` | `x86-64` | x86/64 |
| `arm64`、`aarch64`、`aarch64_generic` | `aarch64_generic` | 通用 64 位 ARM |
| `aarch64_cortex-a53` | `aarch64_cortex-a53` | Cortex-A53 及兼容目标 |
| `aarch64_cortex-a72` | `aarch64_cortex-a72` | Cortex-A72 及兼容目标 |
| `armv7`、`arm_cortex-a7_neon-vfpv4` | `arm_cortex-a7_neon-vfpv4` | 32 位 ARM Cortex-A7 |
| `arm_cortex-a9_vfpv3-d16` | `arm_cortex-a9_vfpv3-d16` | 32 位 ARM Cortex-A9 |
| `arm_cortex-a15_neon-vfpv4` | `arm_cortex-a15_neon-vfpv4` | 32 位 ARM Cortex-A15 |
| `mips_24kc` | `mips_24kc` | 大端 MIPS 24Kc |
| `mipsel_24kc` | `mipsel_24kc` | 小端 MIPS 24Kc |

表中是 CPU 系列示例，不保证覆盖具体路由器。准确的软件包架构应以路由器 opkg 输出和 SDK 元数据为准。

| 变量 | 默认值 | 作用 |
| --- | --- | --- |
| `ARCH` | `x86-64` | 单个目标或逗号/空格分隔的列表 |
| `SDK_VERSION` | `24.10.4` | 官方 SDK 版本 |
| `SDK_IMAGE` | 按目标和版本生成 | 覆盖镜像，仅限单目标 |
| `SDK_LOCAL_IMAGE` | `sim-relay-sdk:local-<目标>` | 本地构建镜像标签，仅限单目标 |
| `SDK_VOLUME` | 按构建镜像 ID 生成 | 覆盖缓存卷，仅限单目标；不同架构或 SDK 不得共用 |
| `JOBS` | `2` | make 并发数 |
| `WITH_ZH_CN` | `1` | 设置为 `0` 时不构建中文包 |
| `WITH_MM_PIN` | `0` | 设置为 `1` 时构建可选 ModemManager PIN 辅助包 |
| `EXPECT_ELF_MACHINE` | 按目标自动选择 | 覆盖 ELF machine 检查，仅限单目标 |
| `SIM_RELAY_RELEASE_EPOCH` | 未设置 | 将复制后的 LuCI 源文件时间固定为指定 Unix 时间戳，使 LuCI 包版本可重复 |

运行 `bash scripts/build-sdk.sh --help` 查看别名。失败时日志和可获取的诊断文件仍保留在产物目录中。不要让并发构建共用同一个 SDK volume。

## 产物与验证

SDK 产物包含项目 IPK、依赖 IPK、`verification.json`、`SHA256SUMS`、`sdk.config`、构建来源、源码校验和及日志。项目包包括 `sim-relay`、`luci-app-sim-relay`，以及可选的 `luci-i18n-sim-relay-zh-cn` 和 `sim-relay-modemmanager`。`msmtp`、`ca-bundle`、`uhttpd` 等依赖由路由器兼容的软件源提供；PIN 辅助包不会自动安装 ModemManager。

```sh
python3 scripts/inspect-ipk.py .sdk-artifacts/<产物目录> --machine 62 --require-zh-cn
# 仅在 x86-64 Linux 上：使用包内 musl 库运行 Mock 冒烟测试
python3 scripts/smoke-sdk.py .sdk-artifacts/<产物目录>
```

AArch64 的 machine 为 `183`、ARM 为 `40`、MIPS 为 `8`。SDK 脚本会自动检查架构和包内容。Mock 测试不验证真实设备、网络服务或非 x86 程序执行；详见[当前验证状态](VALIDATION-CURRENT.md)和[本地开发测试](DEVELOPMENT.zh-CN.md#本地构建和测试)。

## 标签触发 GitHub Release

推送**新建的 `v` 开头标签**会触发[发布工作流](../.github/workflows/release.yml)。九个矩阵任务分别编译上表目标，启用中文包和 PIN 辅助包，并校验四个项目包的架构以及 ELF 位数和字节序。**全部成功后**，独立任务才会将 IPK 和 `SHA256SUMS.txt` 发布到对应 GitHub Release；某个架构失败会阻止发布，请查看失败任务并重新运行。

Release 文件名为 `<SDK目标>--<原IPK文件名>`，包括内部架构为 `all` 的界面和语言包。前缀仅区分各目标的构建产物，不会改变包内架构。请下载前缀相同的四个包。完整下载后，在所在目录运行 `sha256sum -c SHA256SUMS.txt`。如果只下载了 MIPS 小端目标，可以这样筛选校验：

```sh
grep '  mipsel_24kc--' SHA256SUMS.txt | sha256sum -c -
```

打标签前应更新并提交 `package/sim-relay/Makefile` 的版本，以及需要时更新 `package/sim-relay-modemmanager/Makefile`；**标签不会自动修改 IPK 版本**。LuCI 包版本由 `luci.mk` 生成；工作流将源码时间固定为标签提交时间，使各架构及重跑的文件名保持稳定。例如：

```sh
git tag v0.6.0
git push origin v0.6.0
```

仓库需要允许 GitHub Actions 以 `contents: write` 创建 Release。工作流只处理已推送的标签；创建和推送标签仍由维护者执行。建议逐个推送标签：[一次推送超过三个标签时，GitHub 不会创建标签 push 事件](https://docs.github.com/en/webhooks/webhook-events-and-payloads#push)。其他 OpenWrt 版本或架构需使用对应 SDK 单独编译。

新增发布架构时，需同步更新 `scripts/build-sdk.sh` 的 `resolve_arch()`、`scripts/prepare-release.py` 的 `TARGETS`，以及工作流矩阵和预期安装包数量（每个目标四个包）。应确认官方 SDK 标签、软件包架构、ELF machine、位数和字节序，并在发布前实际构建和测试该目标。
