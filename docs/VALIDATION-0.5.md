> 历史记录：以下内容和未完成事项反映当时状态，当前结果见 [当前验证状态](VALIDATION-CURRENT.md)。

# 0.5 收尾验证

本轮在已有本机模块实现上补齐安装升级和页面逻辑验证，继续保持无 SQLite、无强制 ModemManager 依赖。

## 完成

- 安装 `/lib/upgrade/keep.d/sim-relay`，只选择 UCI、credentials.key、cursor 与 pending，不带旧数据库与运行缓存。
- 备份恢复可能以默认 umask 重建目录；只对服务用户拥有、非符号链接的 pending/actions 子目录收紧为 0700，根状态目录仍须符合私有目录要求。
- 备份/恢复回归验证凭据仍可解密、进度仍在、失败短信能够续发；旧 relay.db 未包含在备份中。
- LuCI 页面逻辑测试执行真实 overview.js 事件处理器，验证缺少 ModemManager 的扫描/保存提示、添加模块的参数顺序与编辑保存。DOM 和 RPC 使用替身，不代表真实浏览器/设备联调。
- 同一个临时发送请求 key 对应不同号码/正文时明确报错，覆盖队列中和已完成两种状态，不增加持久化流水。
- SDK 检查器要求 0.5 包含 keep.d 规则，继续拒绝 SQLite 和强制 modemmanager 依赖。

保留规则接入的是 OpenWrt 的 [keep.d 备份机制](https://github.com/openwrt/openwrt/blob/openwrt-24.10/package/base-files/files/sbin/sysupgrade)。需要选择保留配置，并确保新固件包含或恢复安装本软件。规则不是在线一致性快照；手动导出完整备份前应先停服务。用户选择不保留配置时不适用。

## 实际结果

- 本地 MinSizeRel 严格警告编译通过。
- Python **49/49** 通过，无跳过。
- CTest **3/3** 通过（协议、转发格式、LuCI 页面逻辑；页面逻辑项需 Node）。
- ASAN/UBSAN 下同样 **49/49 + 3/3** 通过。LeakSanitizer 因环境限制关闭。
- Shell、JS、Python 语法检查通过；历史 0.2 IPK 检查仍通过。

## 未完成

- 当前 0.5 OpenWrt SDK 实编译、目标包运行与实际安装体积。
- 真实 OpenWrt 的 sysupgrade/安装服务流程、LuCI 浏览器交互、ModemManager/实体模块、外部转发平台联调。
- 独立 REST/Token、旧 Python 数据导入尚未实现，列为后续按需功能，不为了补齐清单恢复数据库或复杂流水。

本轮再次执行 docker info 和 sudo -n docker info：前者 Docker socket 权限不足，后者被 no-new-privileges 限制拒绝，没有实际 SDK 编译成功的结果。

宿主机下一步：

```sh
sudo -n env WITH_MM_PIN=1 WITH_SMTP=1 EXPECT_ELF_MACHINE=62 bash scripts/build-sdk.sh
python3 scripts/smoke-sdk.py .sdk-artifacts/<本次构建目录>
```

2026-11-15 RPC 兼容修复（核心包 0.6.0-r2）：rpcd 会把浏览器请求里的 ubus_rpc_session 一并传给可执行插件，之前的严格参数校验会误拒绝该字段。本地已复现同样错误，现允许单个字符串会话字段，并在分发前擦除、删除，避免保存到 UCI；未知业务字段、重复字段和错误类型仍拒绝。已增加带会话字段的读取/保存回归。路由器 SSH 连接受本地网络 socket 限制，未实际登录验证。

2026-11-15 LuCI 菜单权限修复：原来只在 firstchild 父节点声明 ACL，现为 overview/manage/channels 的 view 节点分别声明同一既有 ACL。没有移除 hasViewPermission 检查或扩大 rpcd 授权。英中页面逻辑测试已加入菜单 ACL 和按钮 enabled 断言，安装包检查也要求每个 view 自带 ACL。浏览器扩展能枚举路由器标签，但 DOM/截图读取发生内部接口错误，尚未在真实页面验证修复效果。

2026-11-15 路由器实查：SSH 已可用。确认目标机菜单 ACL 补丁和核心 r2 已生效，但 LuCI DOM helper 会将 false 属性写成字符串。真正的按钮禁用原因是 disabled:false 仍生成 disabled="false"，checkbox 的 checked:false 同样错误。已统一将 disabled/checked/readonly 的假值改为 null，保留全部权限判断；测试 DOM 也改为按 HTML 属性存在性判断。三个实际页面已在路由器备份并修补，通过 HTTP 下载后的 SHA256 与远端一致，且 Node 语法检查通过。英中页面添加/编辑流程通过。真实浏览器最终刷新/点击仍需用户确认，浏览器自动化读取接口不可用。

PIN 保存按钮专项核对：目标机 sim_info 可返回 ICCID，HTTP 认证页面 nodespec 为 satisfied=true/readonly=false，实际分发的 manage.js 保存按钮使用 null 假值，语法正常。增加英中 PIN 页面测试：有 ICCID/写权限时可点击（即使缺少解锁辅助包），默认自动解锁与强制选项不勾选，保存参数正确且清空输入；无 ICCID 或只读时保持禁用。浏览器若仍显示灰色，需在 manage 页面本身强制刷新以替换旧脚本缓存。

PIN 页面继续反馈禁用后，目标端再次确认 ICCID 有效和 nodespec.readonly=false。为可靠替换客户端旧模块，将 view 模块改名 manage-pin.js（页面 URL /manage 不变），并增加只读/缺 ICCID 时的明确提示；模板、译文、测试和安装包检查同步更新。新模块通过无远端 PTY 的 SSH 上传，HTTP 下载内容与源文件一致、Node 语法通过；认证 HTTP 返回的新 action.path 为 sim-relay/manage-pin，readonly=false。没有保存或提交真实 PIN，最终点击/保存结果仍需用户验证。
