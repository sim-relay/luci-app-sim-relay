# 使用 Docker 和 OpenWrt SDK 构建

构建脚本默认使用官方 `ghcr.io/openwrt/sdk:x86-64-24.10.4` 镜像。已用此配置成功编译 0.6.0-r6，并在 ImmortalWrt 24.10.5 x86/64 上安装；其他固件、目标架构和软件源需要单独确认兼容性。镜像使用方法参见 [OpenWrt Docker 项目](https://github.com/openwrt/docker)。

## 构建

在项目根目录运行：

```sh
sudo -n env WITH_MM_PIN=1 WITH_SMTP=1 EXPECT_ELF_MACHINE=62 bash scripts/build-sdk.sh
```

宿主机需要 Bash、Docker 和网络连接。若当前用户可直接访问 Docker daemon，可去掉 `sudo -n env`。首次构建会下载 SDK 镜像、feeds 和源码，耗时及空间取决于网络和缓存。构建容器以只读方式挂载源码；SDK 工作区、feeds 和下载缓存在 Docker named volume 中。

常用变量：

| 变量 | 默认值 | 用途 |
| --- | --- | --- |
| `SDK_IMAGE` | `ghcr.io/openwrt/sdk:x86-64-24.10.4` | SDK 镜像；更换架构或版本时使用对应目标镜像 |
| `JOBS` | `2` | 并发数 |
| `WITH_ZH_CN` | `1` | 编译独立简体中文语言包；`0` 仅构建英文 |
| `WITH_MM_PIN` | `0` | 设为 `1` 编译可选 ModemManager PIN 辅助包 |
| `WITH_SMTP` | `0` | 设为 `1` 编译可选 SMTP 包 |
| `EXPECT_ELF_MACHINE` | 不检查指定编号 | 如 `62` 为 x86-64、`183` 为 AArch64，检查目标 ELF 架构 |
| `SDK_VOLUME` | 按构建镜像 ID 命名 | 指定专用缓存卷，不要跨 SDK 版本或架构复用 |

`WITH_MM_PIN=1` 只构建依赖 libdbus 的辅助程序，不会安装或强制依赖系统 `modemmanager`。`WITH_SMTP=1` 构建独立邮件客户端包。即使构建了可选包，也需在路由器上按需安装。

## 产物和验证

成功后命令输出 `Verified SDK build artifacts: ...`，IPK 位于项目下的 `.sdk-artifacts/<时间戳>-<随机后缀>/`。目录还包含 `verification.json`、`SHA256SUMS`、源码哈希、SDK 配置、feeds 版本和完整日志。目录不会提交到 Git。

```sh
python3 scripts/inspect-ipk.py .sdk-artifacts/<本次构建目录> --machine 62
python3 scripts/smoke-sdk.py .sdk-artifacts/<本次构建目录>
```

检查器核对包内容、权限、ACL、ELF 架构和动态依赖，并检查中文包隔离及主包不依赖 SQLite/ModemManager。smoke 在 x86-64 Linux 上使用包内 musl loader 和共享库运行目标程序，覆盖 Mock 和持久化恢复；不等同于在路由器上执行完整服务、真实硬件或外部平台测试。生成的 `runtime-smoke.json` 记录具体范围。

从产物中选择与路由器固件和架构兼容的包，安装步骤见 [README](../README.md#安装)。实际新增磁盘占用取决于路由器已有依赖，不能只用项目 IPK 大小估算。

旧版本的构建数据保留在 [历史验证记录](VALIDATION.md)，不能代表当前版本。
