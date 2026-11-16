> 历史记录：以下内容和未完成事项反映当时状态，当前结果见 [当前验证状态](VALIDATION-CURRENT.md)。

# 0.4 轻量版本验证

2026-11-14。本轮按“保留主要功能，尽量简化”将 SQLite 方案改为 UCI、临时快照、待转发文件与每设备时间进度。

## 已验证

- MinSizeRel 本地 C 构建，`-Wall -Wextra -Wpedantic -Werror` 通过。
- CTest **2/2**：设备解析/短信编码与转发格式/SMTP 文件描述符传递。
- Python **31/31**：26 项轻量后台测试，5 项打包检查器测试，无跳过项。
- ASAN/UBSAN 下重新编译并执行同样的 CTest 与 31 项 Python 测试，通过。
- JS、Bash、POSIX sh 与 Python 语法检查通过。
- 21 个 RPC 方法、LuCI 声明、ACL 一致。
- ELF NEEDED 只有 libcurl、libcjson、libsodium、libc，无 SQLite。

轻量回归包含首次历史基线、新短信发现、真实 daemon 启动恢复、停机/启动期间多条短信补发、同秒区分、倒序列表、单次/多通道成功删除、一个通道失败时不重复成功通道、失败文件重启续发、原设备短信被删后继续补发、配置变更取消旧任务、凭据加密和丢失密钥保护、PIN/自动解锁次数限制、实际删除协议、临时请求查回、UCI 命令分支、RPC 参数检查与打包依赖。

UCI 命令分支通过独立测试替身验证 get/set/commit 参数传递、引号与 shell 字符原样保存、凭据不明文保存；不是在真实 OpenWrt UCI 上执行。生产安装的程序不包含该测试替身入口。

网络/设备操作使用仅链接到 fixture 二进制的离线替身。实际 daemon 启动测试也不访问网络。本地开发环境缺 libsodium 开发头，沿用 /tmp 下的最小稳定 ABI 声明和宿主机 libsodium 运行库，仓库源码仍 include 官方 sodium.h；SDK 应使用正式开发头完成目标验证。

LeakSanitizer 因环境限制关闭（ASAN_OPTIONS=detect_leaks=0）；不声称完成泄漏检测。UBSAN_OPTIONS=halt_on_error=1。

## 本机体积对比

相同本机 MinSizeRel 构建并 strip：

| 版本 | ELF 字节数 |
| --- | ---: |
| 0.3 数据库版本 | 116,928 |
| 0.4 轻量版本 | 80,064 |

减少 36,864 字节（约 31.5%）。这是 x86-64/glibc 本机后台文件，不是 musl 目标 IPK 大小；移除 libsqlite3 的目标依赖空间节省也尚未测量。

## 尚待验证

0.4 尚未实际 SDK 构建、实体 ZTE/SIM 测试、真实推送平台测试或 LuCI 浏览器交互验证。旧 0.2 的 SDK 结果不代表本次改动。

宿主机可执行：

```sh
sudo -n env WITH_SMTP=1 EXPECT_ELF_MACHINE=62 bash scripts/build-sdk.sh
python3 scripts/smoke-sdk.py .sdk-artifacts/<本次目录>
```

新检查器会拒绝 0.4 IPK 引入 SQLite，smoke 验证 musl 目标程序的文件配置、Mock 收发、进度保存与临时状态丢失恢复。

## 简化后的约定

只有待转发短信正文持久化，成功后删除；时间进度在待转发文件落盘后推进，空闲时不重写。设备时间有序是前提，发送成功但任务文件删除前断电可能重复。首次安装、首次关联与新 SIM 建立基线；已有进度的启动恢复会发现更新短信。长期归档、持久化手动操作流水、本地删除与自动保留条数不再保留；旧数据库不自动导入、不主动删除。
