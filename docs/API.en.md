# Remote SMS API

English | [简体中文](API.md) · [README](../README.md)

Enable the API in **SIM Relay → Remote API**, generate and copy a token, and save. It is disabled by default and defaults to port 8848. Only a hash of the 64-character random token is stored; the original cannot be retrieved after leaving the page. Generate a replacement to revoke the old token.

**LAN only** binds to loopback and the current addresses of OpenWrt's logical `lan` network and checks that clients belong to its subnets. Other custom networks are not included. **All networks** binds to all IPv4/IPv6 interfaces; firewall rules, upstream routers and carriers still determine external reachability. The application does not modify firewall rules or create port mappings.

HTTPS is the default and uses the certificate and key configured in `uhttpd.main` (normally `/etc/uhttpd.crt` and `/etc/uhttpd.key`), plus an installed `libustream` SSL provider. If these are missing the API service will not start; inspect `logread -e sim-relay-api`. Clients must trust the certificate and use a matching hostname. HTTP is available, but sends the token and SMS body unencrypted. Behind a reverse proxy, LAN filtering sees the proxy's connection address; the proxy must restrict its own clients.

All endpoints require `Authorization: Bearer <token>`. Examples assume a trusted HTTPS certificate and a token in `SIM_RELAY_API_TOKEN`:

```sh
curl -H "Authorization: Bearer $SIM_RELAY_API_TOKEN" \
  https://router.example:8848/cgi-bin/api/v1/devices

curl -X POST -H "Authorization: Bearer $SIM_RELAY_API_TOKEN" \
  -H 'Content-Type: application/json' \
  -d '{"device_id":1,"request_id":"example-001","number":"13800138000","text":"Test message"}' \
  https://router.example:8848/cgi-bin/api/v1/messages

curl -H "Authorization: Bearer $SIM_RELAY_API_TOKEN" \
  'https://router.example:8848/cgi-bin/api/v1/messages?device_id=1&request_id=example-001'
```

`request_id` is supplied by the caller and must contain 1–128 ASCII letters, digits, dots, underscores or hyphens. A repeated ID and identical content for the same device returns the existing task; different content is rejected. HTTP 202 means queued, not delivered. Query results can be `queued`, `attempting`, `sent`, `failed` or `unconfirmed`; do not blindly retry an unconfirmed operation with a new ID. Device-specific SMS encoding and length limits still apply.

Deduplication lasts only while the temporary task or the most recent 50 results remain available. Restarting the service clears result history; rebooting also clears unfinished tasks in `/tmp`. A later 404 therefore does not prove a message was never sent. The API accepts at most 30 send submissions per minute and 64 temporary action files; a limit returns 429. Invalid authentication returns 401, and a non-LAN source in LAN mode returns 403. The request size limit is 16 KiB. Saving settings restarts the separate API service; it can also be restarted with `/etc/init.d/sim-relay-api restart`.
