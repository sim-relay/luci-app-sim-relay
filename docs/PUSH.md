# 0.4 轻量转发

共享通道继续支持 SMS、Telegram、企业微信、飞书、可选 SMTP。配置与凭据通过 LuCI 保存到 UCI，其中凭据由 libsodium secretbox 加密，RPC 不返回凭据。SMTP 需安装 sim-relay-smtp。

## 一条短信的处理

1. 设备读取完整快照，按设备的持久化时间进度发现新入站短信。
2. 在 pending 中原子写入一个文件，包含正文、来源 SIM、设备名称和待处理的通道 ID/版本。
3. 所有新短信的文件写好后，推进设备 cursor；未成功写入时不推进。
4. 逐通道发送，成功后从文件中移除该通道，全部完成后删除文件。
5. 失败/超时/无法确认都保留，每 60 秒重试；Telegram 的 retry_after 可延长到最多 3600 秒。

没有永久成功记录、死信表、数据库队列或复杂未决确认流程。发送成功到删除文件之间断电可能重复发送，这是接受的边界。一个通道失败不会使其他成功通道在正常重启时重发。

cursor 只保存最新时间点的消息 ID（用于同秒区分），以及当时关联的通道/版本。首次建立或新增通道关联不转发历史；已有通道在重启后继续按 cursor 补发更新短信。更换 SIM 建立新基线。

修改/删除/关闭通道或解除关联后，旧版本任务在下一次后台处理时取消；停用设备暂停其任务，删除设备则清除其任务。配置变更在下一轮加载生效。

## 字段

- SMS：config.number，使用来源设备 SIM 发送；同号码回路拒绝发送。
- Telegram：config.chat_id，secrets.token。
- 企业微信：secrets.webhook_url。
- 飞书：secrets.webhook_url，可选 config.signing_enabled 与 secrets.signing_secret。
- SMTP：config.host/port/from/to/username/tls_mode，secrets.password；tls_mode 为 starttls 或 tls。
- 共用：config.prefix（最多 320 字节）。旧 retry_count 不再控制重试上限。

编辑时凭据留空保留原值，清除凭据同时停用通道。测试通道前需先与设备关联，测试消息同样走待转发文件。

HTTP 使用 HTTPS、校验证书、不跟随跳转，并验证官方平台地址；响应限制 256 KiB。SMTP 使用独立客户端，临时配置权限 0600、打开后立即 unlink，通过继承文件描述符传递，凭据不出现在 argv。

ZTE SIM 短信受设备编码和长度约束（Unicode BMP 最多 335 字符，不支持发送 emoji）；带标题/前缀的转发内容也计入限制，超长会保持失败待重试，应调整内容或改用网络通道。网络通道保留既有格式和平台长度限制。

本地 fixture 验证调度与协议边界，不向外发消息；实际平台及具体设备的端到端转发仍需验证。

本机 ModemManager 模块使用同一待转发与时间进度流程，但发送不套用 ZTE 的 BMP 限制；正文通过 UTF-8 文件交给 mmcli。见 [本机模块](MODEMMANAGER.md)。
