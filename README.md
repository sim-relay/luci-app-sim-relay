# SIM Relay for OpenWrt / LuCI

English | [简体中文](README.zh-CN.md)

SIM Relay is a lightweight SMS management and forwarding service for OpenWrt. It combines a C backend with a LuCI interface and does not require SQLite or retain a permanent SMS archive. The project is inspired by [SIM Relay](https://github.com/coinlet/sim-relay).

## Features

- Manage up to 16 devices. A built-in adapter supports ZTE MF833U1; local cellular modems can be discovered through ModemManager. A Mock device is available for testing.
- Browse, search, send and delete SMS messages; manage SIM PINs and optionally keep only the newest messages on a device.
- Forward incoming messages through SMS, Telegram, WeCom, Feishu, SMTP, or configurable GET/POST/PUT webhooks. Devices can share and bind multiple channels.
- Optionally expose a token-authenticated SMS API, with LAN-only or all-interface access and HTTP or HTTPS. See the [API guide](docs/API.en.md).
- Keep configuration in UCI, encrypt saved credentials and PINs, and persist forwarding progress and pending jobs. The LuCI interface is in English, with a separately installed Simplified Chinese language pack.

ModemManager and modem drivers are installed and managed by the system; the main package does not install ModemManager. PIN unlock for a ModemManager modem additionally requires the optional `sim-relay-modemmanager` package. Device and firmware compatibility must be verified on the target hardware.

## Install

Download packages for your OpenWrt release and package architecture from [GitHub Releases](https://github.com/home-sim-relay/luci-app-sim-relay/releases). Check the router using `cat /etc/openwrt_release` and `opkg print-architecture`; matching a CPU family alone is not enough to establish ABI compatibility. Release packages are built with OpenWrt **24.10.4**, not every OpenWrt or vendor firmware.

Each release filename starts with its SDK target, for example `x86-64--sim-relay_..._x86_64.ipk`. The prefix also distinguishes the `_all.ipk` LuCI packages produced for each target; it does not change the package's internal architecture. For one target, upload its packages to `/tmp` and install:

```sh
opkg update
opkg install /tmp/<target>--sim-relay_<version>_<architecture>.ipk \
  /tmp/<target>--luci-app-sim-relay_<version>_all.ipk
# Optional: install the matching Chinese language and ModemManager PIN packages.
opkg install /tmp/<target>--luci-i18n-sim-relay-zh-cn_<version>_all.ipk
opkg install /tmp/<target>--sim-relay-modemmanager_<version>_<architecture>.ipk
```

Replace placeholders with the actual filenames. Install dependencies from a compatible configured OpenWrt feed. Then add devices under **Services → SIM Relay**. The daemon can be managed with `/etc/init.d/sim-relay restart`.

## Documentation

- [Build and release guide](docs/BUILD.md): supported targets, Docker SDK commands, package contents and release workflow.
- [Development and device adapters](docs/DEVELOPMENT.md): project structure, adding and testing a device adapter.
- [Device management, SMS and forwarding](docs/USAGE.md) · [Remote API](docs/API.en.md).
- [Current validation and limitations](docs/VALIDATION-CURRENT.md) (Chinese).

Forwarding uses a persistent per-device cursor and pending files. A power loss after a channel has accepted a message but before the pending file is updated can cause a duplicate. SMS snapshots, manual actions and recent results are temporary; see the linked guides for their limits and backup requirements.

## License

[MIT](LICENSE)
