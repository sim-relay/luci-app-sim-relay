# 开发与设备适配

[English](DEVELOPMENT.md) | 简体中文 · [返回首页](../README.zh-CN.md) · [编译指南](BUILD.zh-CN.md)

## 项目结构

| 位置 | 职责 |
| --- | --- |
| `src/main.c`、`src/lite.c` | RPC 参数、UCI 配置、轮询、操作队列、转发进度和运行状态 |
| `src/relay.h` | C 公共声明和设备适配入口 |
| `src/adapter.c`、`src/zte_send.c`、`src/zte_operations.c` | 内置 ZTE 快照/SIM、发送和 PIN/删除操作，也包含 Mock 分支 |
| `src/modemmanager.c`、`src/mm_pin.c` | ModemManager 发现与操作，以及可选 PIN 辅助程序 |
| `src/protocol.c`、`src/http.c`、`src/vault.c` | 短信编码、HTTP 传输和凭据加密 |
| `src/api.c`、`src/push_config.c`、`src/push_transport.c` | 远程 API 和推送通道校验/发送 |
| `luci-app-sim-relay/` | LuCI 页面、菜单/ACL 和翻译目录 |
| `package/` | OpenWrt IPK 定义、启动脚本和 rpcd 程序 |
| `tests/` | 协议测试、模拟设备、LuCI 测试和 API 回归测试 |

当前没有动态驱动注册机制，允许的适配器名称和界面选项都在代码中显式列出。新增模块前，先确认它能否通过 ModemManager 使用；若可以，优先用现有 `modemmanager` 适配并测试收发短信和 SIM 功能。是否支持取决于模块、驱动及 ModemManager，能够发现设备不代表所有操作都可用。

## 添加一种新设备

设备协议不同时，可使用 `vendor_model` 之类的新适配器 ID。下面描述的是**当前代码需要修改的位置**，并不表示已经存在可直接加载的插件接口：

1. 梳理设备认证、硬件身份、分页、短信编码、SIM/PIN 状态和写入响应；保存经过脱敏的响应样本，确认哪些写入结果可以验证。不能因为设备页面相似，就假设它与 MF833U1 使用相同接口。
2. 新建独立适配文件，例如 `src/vendor_model.c`，加入 `CMakeLists.txt` 的 `RELAY_CORE`，并在 `src/relay.h` 中声明函数。`test-decode` 使用单独的源文件列表，也需要加入新文件；该测试直接包含 `adapter.c` 以测试静态解析函数，不要再把第二份 `adapter.c` 链接进该目标。参考现有适配器，将设备协议细节保留在新文件中。
3. 在 `src/adapter.c` 的 `adapter_snapshot()` 和 `adapter_sim()` 中分派到新实现。返回稳定的 `hardware_id` 和 SIM `iccid`；成功快照的 `messages` 数组包含稳定的 `external_id`、`number`、`text`、`timestamp`、`direction` 和 `state`。消息身份应跨轮询及重启保持稳定，时间使用可解析且带时区的 ISO 8601 格式。快照不完整、过大或无效时应整体失败，不能拿部分数据推进转发进度。
4. 在 `src/zte_send.c` 的短信发送分派和 `src/zte_operations.c` 的 PIN/删除分派中，于 ZTE 专用代码之前接入新实现。若号码、编码或长度限制不同，还需更新 `src/modemmanager.c` 的 `valid_sms()`。不支持的操作应明确失败；不能让未知适配器误走 ZTE 操作。
5. 在不可逆写入前核对设备和 SIM 身份、校验操作目标，并在提交前立即调用传入的 `commit(ctx)`。遵守现有结果约定：`0` 未提交、`1` 已提交但未确认、`2` 明确拒绝、`3` 已确认。不能对结果不明的短信、PIN 或删除操作盲目重试；按需再次核对 PIN 剩余次数和删除目标。入口声明见 [relay.h](../src/relay.h)。
6. 更新 `src/lite.c` 中 `save_device` 的适配器允许列表、配置校验、重复设备/身份处理和输入边界。若增加了界面提交的配置字段，还需更新 `src/main.c` 的方法参数定义，以及 `manage-pin.js` 的 `save_device` RPC 声明；新增 RPC 方法还需配置 LuCI ACL。
7. 在 `luci-app-sim-relay/htdocs/luci-static/resources/view/sim-relay/manage-pin.js` 添加设备选项和按类型显示的配置项。英文消息加入 `po/templates/sim-relay.pot`，中文译文加入 `po/zh_Hans/sim-relay.po`。列表响应和日志不能暴露明文凭据或 PIN；保存秘密信息时复用现有加密机制。
8. 增加离线 fixture 和回归测试，覆盖正常/异常响应、分页限制、编码、设备或 SIM 变化、超时和未确认写入、重复请求以及不支持的操作。可扩展 `tests/test_decode.c`、`tests/test_lite.py`、`tests/fixture_http.c` 或新增协议 fixture，并补充界面配置及翻译测试。
9. 运行本地测试和 SDK 编译，再在目标设备与固件上实测。先测试读取，再测试写入，并明确核对 PIN 和删除保护。记录实际验证过的功能和固件版本。

目前设备分派分布在多个 C 文件中，只修改界面或只修改后端允许列表都不够。[当前验证状态](VALIDATION-CURRENT.md)区分了自动测试和真实设备验证。

## 本地构建和测试

安装 C99 编译器、CMake、libcurl、cJSON 和 libsodium 开发头文件，以及 Python 3 和 Node.js。不同发行版的软件包名称可能不同。在项目根目录运行：

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=MinSizeRel -DSIM_RELAY_TESTS=ON
cmake --build build -j2
ctest --test-dir build --output-on-failure
python3 -m unittest discover -s tests -q
python3 scripts/check-i18n.py
```

Python fixture 测试默认使用 `build/sim-relayd-fixture`，找不到时会跳过。真实 uhttpd 的 HTTP/HTTPS 测试还需要 `SIM_RELAY_TEST_UHTTPD` 和 TLS fixture 环境；部分服务测试需在隔离测试容器中以 root 运行。应检查跳过原因，不能将跳过视为通过。fixture 二进制不会打进生产包。OpenWrt 交叉编译、可选辅助包与 Release 产物见[编译指南](BUILD.zh-CN.md)。

## 状态与兼容性

配置和加密凭据保存在 UCI，主密钥为 `/etc/sim-relay/credentials.key`。cursor 和 pending 文件持久化在 `/etc/sim-relay/`，快照及手动操作结果位于 `/tmp/sim-relay/`；待转发文件包含短信正文。修改适配器时要保持身份校验、异常恢复、资源上限和凭据处理的约定。备份和转发边界见[设备管理](../MANAGEMENT.md)与[转发设计](PUSH.md)。
