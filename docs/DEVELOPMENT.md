# Development and device adapters

English | [简体中文](DEVELOPMENT.zh-CN.md) · [README](../README.md) · [Build guide](BUILD.md)

## Project layout

| Location | Responsibility |
| --- | --- |
| `src/main.c`, `src/lite.c` | RPC schemas, UCI configuration, polling, action queues, forwarding progress and runtime state |
| `src/relay.h` | Shared C declarations and the adapter entry points |
| `src/adapter.c`, `src/zte_send.c`, `src/zte_operations.c` | Built-in ZTE snapshot/SIM, send and PIN/delete operations; includes Mock branches |
| `src/modemmanager.c`, `src/mm_pin.c` | ModemManager discovery and operations; optional PIN helper |
| `src/protocol.c`, `src/http.c`, `src/vault.c` | SMS encoding, HTTP transport and credential encryption |
| `src/api.c`, `src/push_config.c`, `src/push_transport.c` | Remote API and push channel validation/delivery |
| `luci-app-sim-relay/` | LuCI views, menu/ACL and translation catalogs |
| `package/` | OpenWrt IPK definitions, init scripts and rpcd executable |
| `tests/` | Protocol tests, mocked devices, LuCI tests and API regressions |

There is no dynamic driver registry today. The accepted adapter names and UI choices are explicit. First check whether a new modem already works through ModemManager; if so, select the existing `modemmanager` adapter and test its SMS and SIM capabilities before writing another driver. Support depends on the modem, driver and ModemManager; discovery alone does not prove all operations work.

## Add a new device adapter

For a device with a different protocol, use a new adapter ID such as `vendor_model`. The following steps describe the **current implementation**, not a plugin interface that already exists:

1. Document the device's authentication, identity, pagination, message encoding, SIM/PIN states and write responses. Capture sanitized example responses and determine whether writes can be confirmed. Do not assume an endpoint or firmware behaves like MF833U1 just because it looks similar.
2. Implement a separate adapter source file, for example `src/vendor_model.c`, and add it to `RELAY_CORE` in `CMakeLists.txt`. The `test-decode` target has its own explicit source list, so add the new source there too. It includes `adapter.c` directly for tests of static parsers; do not link a second copy of `adapter.c` into that target. Declare the new functions in `src/relay.h`. Reuse existing adapters as examples; keep protocol-specific details in the new file.
3. Route `adapter_snapshot()` and `adapter_sim()` in `src/adapter.c` to the new functions. Return a stable `hardware_id` and SIM `iccid`; a successful snapshot contains a `messages` array with stable `external_id`, `number`, `text`, `timestamp`, `direction` and `state` fields. Preserve message identity across polls and restarts and use parseable ISO 8601 timestamps with a timezone. Incomplete, oversized or invalid snapshots should fail as a whole rather than advance the forwarding cursor with partial data.
4. Route SMS sending in `src/zte_send.c` and PIN/delete operations in `src/zte_operations.c` to the new implementation **before** their ZTE-specific fall-through. Update `valid_sms()` in `src/modemmanager.c` if the new device has different number, encoding or length constraints. Unsupported operations should explicitly return failure; do not let an unknown adapter accidentally use ZTE operations.
5. Before an irreversible write, verify the device and SIM identity, validate the target and call the supplied `commit(ctx)` callback immediately before submission. Respect the existing outcome contract: `0` not submitted, `1` submitted but unconfirmed, `2` explicitly rejected, `3` confirmed. Never blindly retry an uncertain send, PIN attempt or delete. Recheck PIN retries and deletion targets as appropriate; see `adapter_send()` and `adapter_operation()` in [relay.h](../src/relay.h).
6. Add the ID and configuration validation to `save_device` in `src/lite.c`, including duplicate/identity handling and input bounds. If the UI must send new configuration keys, update the method schema in `src/main.c` and the `save_device` RPC declaration in `manage-pin.js`. New RPC methods also need the appropriate LuCI ACL.
7. Add the selection and its conditional configuration fields to `luci-app-sim-relay/htdocs/luci-static/resources/view/sim-relay/manage-pin.js`. Add English message IDs to `po/templates/sim-relay.pot` and translations to `po/zh_Hans/sim-relay.po`. Never expose plaintext credentials or PINs in list responses or logs; use the existing vault for saved secrets.
8. Add offline fixtures and regression coverage for normal and malformed responses, pagination limits, encodings, device/SIM changes, timeout and uncertain writes, duplicate requests, and unsupported operations. Extend `tests/test_decode.c`, `tests/test_lite.py`, `tests/fixture_http.c` or add a protocol-specific fixture; update the LuCI test for configuration and translations.
9. Run local tests and an SDK build, then verify on the intended hardware and firmware. Test SMS reads before writes, and explicitly verify PIN and deletion safeguards. Document which functions and firmware have actually been tested.

Current adapter dispatch is distributed across several C files, so updating only the UI or only the adapter allowlist is insufficient. `docs/VALIDATION-CURRENT.md` distinguishes automated tests from real-device results.

## Build and test locally

Install a C99 compiler, CMake, libcurl, cJSON and libsodium development headers, plus Python 3 and Node.js. Library package names differ by distribution. From the repository root:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=MinSizeRel -DSIM_RELAY_TESTS=ON
cmake --build build -j2
ctest --test-dir build --output-on-failure
python3 -m unittest discover -s tests -q
python3 scripts/check-i18n.py
```

The Python fixture tests use `build/sim-relayd-fixture` when available; they are skipped if it is missing. Real-uhttpd HTTP/HTTPS tests additionally require their `SIM_RELAY_TEST_UHTTPD` and TLS fixture environment, and some service tests require root inside an isolated test container. Read skip reasons instead of treating skipped tests as passes. The fixture binary is not packaged for production. For OpenWrt cross-compilation, optional helpers and Release artifacts use the [build guide](BUILD.md).

## State and compatibility

Configuration and encrypted credentials are stored in UCI; the key is `/etc/sim-relay/credentials.key`. Cursor and pending files are persistent under `/etc/sim-relay/`; snapshots and manual action results are under `/tmp/sim-relay/`. Pending forwarding files contain SMS content. Keep the identity, crash recovery, bounded resource use and credential-handling assumptions intact when changing adapters. Backup and forwarding behavior are described in the [usage guide](USAGE.md).
