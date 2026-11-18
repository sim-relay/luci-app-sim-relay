# 远程短信 API

[English](API.en.md) | 简体中文 · [返回首页](../README.zh-CN.md)

在「短信中继 → 远程 API」中启用服务、生成并复制令牌，然后保存。默认关闭，默认端口为 8848。令牌为 64 位十六进制随机值，配置中只保存摘要；离开页面后不能找回原令牌，可以生成新令牌替换。

访问范围有两种：

- **仅局域网**：监听回环地址和 OpenWrt 逻辑网络 `lan` 当前的地址，并检查客户端来源属于该网络的子网。`lan` 地址变化后服务会重启；其他自定义网络不包括在内。
- **所有网络**：监听所有 IPv4 和 IPv6 地址。路由器防火墙、上级路由器和运营商网络仍会影响访问；本应用不会自动修改防火墙或建立端口映射。

默认使用 HTTPS，复用 `uhttpd.main` 配置的证书和私钥（默认 `/etc/uhttpd.crt`、`/etc/uhttpd.key`），还需要已安装 `libustream` SSL 组件。证书缺失时服务不会启动，可用 `logread -e sim-relay-api` 查看原因。使用自签名证书时，客户端应信任对应证书并使用与证书匹配的主机名。

也可以选择 HTTP，但 HTTP 会明文传输令牌和短信正文，建议仅在可信网络内使用，或在具备认证和 TLS 的反向代理/VPN 后使用。若经过反向代理，局域网模式检查的是代理的实际连接地址；请让代理自身限制允许访问的客户端。

## 请求

请求需要 `Authorization: Bearer <令牌>`。基础地址为 `https://路由器地址:8848/cgi-bin/api`，端口和协议由设置决定。以下示例假设客户端已信任路由器证书，且 `SIM_RELAY_API_TOKEN` 已设置为生成的令牌。

查询设备：

```sh
curl -H "Authorization: Bearer $SIM_RELAY_API_TOKEN" \
  https://router.example:8848/cgi-bin/api/v1/devices
```

提交短信：

```sh
curl -X POST -H "Authorization: Bearer $SIM_RELAY_API_TOKEN" \
  -H 'Content-Type: application/json' \
  -d '{"device_id":1,"request_id":"example-001","number":"13800138000","text":"测试短信"}' \
  https://router.example:8848/cgi-bin/api/v1/messages
```

`request_id` 由调用方生成，只允许 1～128 个英文字母、数字、点、下划线和连字符。同一设备重复提交相同 ID 和内容会返回已有任务；内容不同时会拒绝。同一 ID 可用于不同设备。成功接受返回 HTTP 202 和 `request_id`、`device_id`、`status`，表示已排队，并不表示短信已发送。

查询任务：

```sh
curl -H "Authorization: Bearer $SIM_RELAY_API_TOKEN" \
  'https://router.example:8848/cgi-bin/api/v1/messages?device_id=1&request_id=example-001'
```

状态包括 `queued`、`attempting`、`sent`、`failed` 和 `unconfirmed`。如果提交时中断，`unconfirmed` 表示无法确定设备是否发送成功，调用方不应自动使用新 ID 重发。短信编码及长度限制与 LuCI 手动发送相同。

## 限制与排查

请求 key 去重仅在任务及最近 50 条临时结果仍在时有效，后台服务重启会清除结果历史，路由器重启还会清除 `/tmp` 中的未完成任务。超出范围后查询可能返回 404，不能据此判断短信没有发出。API 不提供跨重启的永久去重保证。

为保护低性能设备，每分钟最多接受 30 次发送提交，临时任务队列最多 64 个文件；达到限制时返回 HTTP 429。无效认证返回 401，来源不在允许的局域网返回 403。请求大小上限为 16 KiB。

修改设置后 API 服务会自动重启，访问范围变更在旧服务完全退出前也会拒绝旧配置请求。API 使用独立的 uhttpd 实例，不改变 LuCI 主服务的监听地址。可通过 `/etc/init.d/sim-relay-api restart` 手动重启 API 服务。
