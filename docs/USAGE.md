# Devices, SMS and forwarding

English · [简体中文设备管理](../MANAGEMENT.md) · [README](../README.md)

## Devices and SIM

Device configuration is stored in UCI. The daemon reads SMS and SIM state from each device and caches it in `/tmp`; manual send, PIN and delete operations use temporary jobs. Forwarding jobs are separate and persistent.

Add a device in LuCI and choose an adapter. The ZTE adapter de-duplicates management URLs, treating default ports and trailing slashes as equivalent. For a local modem, install and start the router's ModemManager package, then select a discovered modem. The application resolves its ModemManager path by hardware identity after a restart. The `sim-relay-modemmanager` helper is required only for ModemManager PIN submission; it passes the PIN through standard input to a D-Bus helper. Drivers, APN and connectivity are managed by the system, not this application.

Deleting a device removes its configuration, owned PIN, forwarding cursor and pending jobs. It does not delete SMS stored on the SIM. Older PIN records without an owner are matched using available SIM or cursor data; unidentified records are retained.

PINs are encrypted with the master key and bound to ICCID. Automatic unlock is attempted once per locked cycle, only for a normal PIN lock with three attempts remaining. The attempt is recorded before submission and survives service and router restarts; it is cleared when the SIM is observed unlocked. Manual force permits one attempt with one or two retries remaining, but rejects unknown/zero retries and PUK. If a submission is interrupted, inspect the SIM state before choosing a manual retry.

The runtime state file is limited to 4 MiB. Corrupt or oversized state is preserved and reported; a snapshot that cannot fit does not advance that device's forwarding cursor.

## SMS and retention

The SMS list reflects the device; there is no long-term local SMS archive. Up to 100 messages can be selected for deletion. The daemon re-reads and verifies message identity before submitting deletion. Manual sends are not automatically retried; `unconfirmed` means the actual result needs inspection. A request key can retrieve a result only while the temporary task or recent runtime history is available.

ZTE sends support GSM and Unicode BMP, with up to 335 Unicode characters and no emoji; the device timestamp uses UTC+8. ModemManager sends use a private temporary UTF-8 file, up to 8192 bytes, and delegate encoding and segmentation to ModemManager. Actual device behavior must be validated on the intended hardware.

Optionally retain the latest N device messages (`0` disables automatic cleanup). After a successful read, the daemon deletes the oldest excess messages, at most 20 per polling round. It does not automatically delete messages with unparseable timestamps. Cleanup runs after pending forwarding jobs have been saved.

## Forwarding

Channels support SMS, Telegram, WeCom, Feishu, SMTP and custom webhooks. Saved secrets are encrypted and are not returned by RPC. SMTP uses `msmtp` and `ca-bundle` dependencies.

For each new inbound message, the daemon atomically saves a pending file with its content, source SIM and channel IDs/versions. It advances the per-device cursor only after all new pending files have been saved. A successful channel is removed from the pending file; failures, timeouts and unconfirmed results are retried after at least 60 seconds (Telegram can request up to 3600 seconds). There is no permanent success history or dead-letter table. Power loss after a remote service accepts a message but before the pending file is updated can produce duplicates.

The cursor stores the latest timestamp, message IDs at that time and bound channel versions. Initial device sync, a new channel binding or a SIM change establishes a baseline instead of forwarding old messages. Missing dates, clocks moving backwards and messages removed before scanning cannot be guaranteed recovery. Changing, disabling, deleting or unbinding a channel cancels its old-version pending targets; disabling a device pauses its jobs, while deleting it removes them. Each send rechecks configuration, but an already-started external request cannot be recalled. Each round attempts at most four channels and scans the queue in rotation.

Channel fields are: SMS `config.number`; Telegram `config.chat_id` and `secrets.token`; WeCom `secrets.webhook_url`; Feishu `secrets.webhook_url` and optional signing fields; SMTP `config.host/port/from/to/username/tls_mode` and `secrets.password`; Webhook `config.method/body` and `secrets.url/headers`. All types support `config.prefix` (up to 320 bytes). Blank credential fields retain existing values during edits; clearing credentials also disables the channel. Bind a channel to a device before sending a test.

For Webhook, the URL supports `{{message}}` as URL-encoded message text. Headers use one `Name: value` per line. POST/PUT bodies can use `{{message}}` for raw text or `{{message_json}}` for a JSON-escaped string including quotes, for example `{"text":{{message_json}}}`. GET has no body. The URL and headers are encrypted in configuration; the body is ordinary configuration, so place credentials in the URL or headers instead. HTTPS validates certificates and redirects are not followed. A 2xx response is success; 4xx other than 408 and 429 is rejected; other outcomes are unconfirmed and retried.

## Backup and language

Back up both `/etc/config/sim-relay` and `/etc/sim-relay/credentials.key`; preserving forwarding progress also requires `/etc/sim-relay/cursor-*.json` and `/etc/sim-relay/pending/`. Pending files contain SMS content. The package supplies sysupgrade keep rules, but configuration preservation and reinstallation must be selected as appropriate. Stop the service before a manual full backup. Data from older database-based releases is not imported or deleted automatically.

The application uses English source strings. Simplified Chinese is provided by the separate `luci-i18n-sim-relay-zh-cn` package generated from `po/zh_Hans/sim-relay.po`; install it and choose Simplified Chinese in LuCI. `python3 scripts/check-i18n.py` checks the template and translation catalogs. The base LuCI Chinese translation is a separate system package.

The remote SMS API, including authentication, network scope and retry limits, is documented in the [API guide](API.en.md). Automated fixtures do not replace real-device, real-provider or power-loss testing; see [current validation](VALIDATION-CURRENT.md) for the current evidence.
