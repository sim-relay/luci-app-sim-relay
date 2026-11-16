> 历史记录：以下内容和未完成事项反映当时状态，当前结果见 [当前验证状态](VALIDATION-CURRENT.md)。

> 此文为旧数据库版本的历史验证。当前结果见 [0.4 验证](VALIDATION-0.4.md)。

# 0.2 开发版验证记录

0.3 的新增功能验证见 [0.3 验证记录](VALIDATION-0.3.md)，下面的 SDK 包大小仅属于 0.2。

日期：2026-11-14。环境：本机 x86-64 Linux、GCC 15、动态链接系统运行库。
本机缺少开发包，构建使用本机已有 SDK 中的开发头文件和系统共享库；这些临时开发文件不进入项目或安装包。
另已完成下面记录的 OpenWrt SDK 产物验证；实际 ZTE 设备尚未验证。

## 结果

- MinSizeRel 构建通过，开启 `-Wall -Wextra -Wpedantic -Werror`。
- Python 集成测试共 30 项：26 通过、4 项真实本地 HTTP 测试因环境禁止套接字而跳过。
- CTest 编码/协议测试通过：UTF-16、无效编码、GSM-7/Unicode 边界、RFC MD5 向量和 URL 校验。
- AddressSanitizer + UndefinedBehaviorSanitizer 构建运行相同测试通过。
  环境不支持 LeakSanitizer，因此设置 `ASAN_OPTIONS=detect_leaks=0`；没有完成泄漏检测。
- LuCI JavaScript 语法检查通过；RPC 声明、ACL 与菜单 JSON 一致性检查通过。
- strip 后桌面服务 ELF：55,488 字节，约 54.2 KiB，不含共享库。
  不能将其当作 OpenWrt 安装包大小或依赖闭包新增空间。

## 关键故障回归

- 并发使用同一个键只产生一个发送任务，后续重查不会再次发送。
- POST 阶段退出后保留 attempting，恢复为 unconfirmed，不再次 POST。
- 网络超时、重复确认记录均暂停后续发送；人工确认不重发原任务。
- 固件配置异常、AD 参数缺失、换卡/换硬件均在提交前停止。
- 到期租约不能绕过仍在持有的设备 flock。
- 慢设备仍在等待时，另一个 worker 可以完成 Mock 设备同步。
- 多个进程同时首次建库通过短时 schema 锁串行化。
- 0.1 数据保留升级，旧的重复硬件绑定停用，历史保留。

## OpenWrt SDK 实际验证

用户在宿主机 Docker 中完成 OpenWrt 24.10.4 x86/64 SDK 构建，本会话检查了
`.sdk-artifacts/20261114T121113Z-cmnZ7x/` 的日志和实际 IPK。

- 后端包 20,263 B，LuCI 包 4,587 B，后端目标 ELF 49,153 B。
- 实际 ELF machine=62，解释器 `/lib/ld-musl-x86_64.so.1`，动态依赖检查通过。
- 48 个产物的 SHA256 全部匹配；安装文件、脚本权限与 ACL 检查通过。
- 使用 IPK 提供的 musl loader 和运行库实际执行目标程序：建库、Mock Unicode 收件、Mock 发送、
  持久化幂等和本地删除后同步均通过。未安装软件包或操作实际设备。
- base feed 为 openwrt-24.10 分支构建时版本，具体提交已保存；不是原始 24.10.4 全依赖冻结构建。

详见 [SDK 实测记录](SDK.md)。

## 尚未验证

实际 HTTP/TLS、ZTE 真机发送、LuCI 浏览器交互、完整 OpenWrt 环境中的安装升级、
目标设备峰值内存、闪存写入量和断电恢复。
