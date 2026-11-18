# Build and release packages

English | [简体中文](BUILD.zh-CN.md) · [README](../README.md)

Use the OpenWrt SDK matching the router's firmware and package architecture. On the router, inspect `cat /etc/openwrt_release` and `opkg print-architecture`, and ensure its configured package feeds provide compatible dependencies. A CPU family name does not guarantee package or ABI compatibility. The commands below build IPKs; they do not install or upgrade a router.

## Prerequisites and build

You need Bash, Docker with access to its daemon, Python 3 and network access for the SDK image, feeds and sources. Obtain the project if needed:

```sh
git clone https://github.com/home-sim-relay/luci-app-sim-relay.git
cd luci-app-sim-relay
```

From the repository root:

```sh
# x86/64, including Chinese translations and the optional PIN helper
WITH_MM_PIN=1 bash scripts/build-sdk.sh

# One alternative target
ARCH=mipsel_24kc WITH_MM_PIN=1 bash scripts/build-sdk.sh

# Several targets, built sequentially into separate directories
ARCH=x86-64,aarch64_generic,mipsel_24kc WITH_MM_PIN=1 bash scripts/build-sdk.sh
```

If the Docker daemon requires elevated access on your machine, run the script with appropriate privileges, for example `sudo -n env ARCH=x86-64 WITH_MM_PIN=1 bash scripts/build-sdk.sh`. The first build downloads the SDK and dependencies. Source files are mounted read-only; Docker named volumes hold SDK working directories and caches. Each build gets a separate `.sdk-artifacts/<timestamp>-<target>-<suffix>/` directory, printed on success.

The default is the official `ghcr.io/openwrt/sdk:<target>-24.10.4` image. The [OpenWrt Docker project](https://github.com/openwrt/docker#sdk) documents its SDK image tags. `SDK_VERSION` selects a different image version; verify that its ABI and feeds match the router before installing any resulting package.

| `ARCH` value | SDK target | Typical family |
| --- | --- | --- |
| `x86-64`, `x86_64`, `amd64` | `x86-64` | x86/64 |
| `arm64`, `aarch64`, `aarch64_generic` | `aarch64_generic` | generic 64-bit ARM |
| `aarch64_cortex-a53` | `aarch64_cortex-a53` | Cortex-A53 and compatible targets |
| `aarch64_cortex-a72` | `aarch64_cortex-a72` | Cortex-A72 and compatible targets |
| `armv7`, `arm_cortex-a7_neon-vfpv4` | `arm_cortex-a7_neon-vfpv4` | 32-bit ARM Cortex-A7 |
| `arm_cortex-a9_vfpv3-d16` | `arm_cortex-a9_vfpv3-d16` | 32-bit ARM Cortex-A9 |
| `arm_cortex-a15_neon-vfpv4` | `arm_cortex-a15_neon-vfpv4` | 32-bit ARM Cortex-A15 |
| `mips_24kc` | `mips_24kc` | big-endian MIPS 24Kc |
| `mipsel_24kc` | `mipsel_24kc` | little-endian MIPS 24Kc |

These examples describe CPU families, not a guarantee for any particular router. For exact package architecture use the router's opkg output and the SDK target metadata.

| Variable | Default | Purpose |
| --- | --- | --- |
| `ARCH` | `x86-64` | One target or a comma/space-separated list |
| `SDK_VERSION` | `24.10.4` | Official SDK version |
| `SDK_IMAGE` | Constructed from target and version | Override the image; one target only |
| `SDK_LOCAL_IMAGE` | `sim-relay-sdk:local-<target>` | Local build image tag; one target only |
| `SDK_VOLUME` | Derived from the build image ID | Override the SDK cache volume; one target only, never share across architectures or SDKs |
| `JOBS` | `2` | Parallel make jobs |
| `WITH_ZH_CN` | `1` | Set to `0` to omit the Chinese translation package |
| `WITH_MM_PIN` | `0` | Set to `1` to build the optional ModemManager PIN helper |
| `EXPECT_ELF_MACHINE` | Derived from target | Override ELF machine verification; one target only |
| `SIM_RELAY_RELEASE_EPOCH` | Unset | Set copied LuCI source mtimes to this Unix timestamp for repeatable LuCI package versions |

Run `bash scripts/build-sdk.sh --help` for aliases. A failed build leaves logs and available diagnostics in its artifact directory. Do not run concurrent builds against the same SDK volume.

## Outputs and verification

The SDK output contains the project IPKs and dependency IPKs, `verification.json`, `SHA256SUMS`, `sdk.config`, build provenance, source checksums and logs. The project packages are `sim-relay`, `luci-app-sim-relay`, optional `luci-i18n-sim-relay-zh-cn`, and optional `sim-relay-modemmanager`. `msmtp`, `ca-bundle`, `uhttpd` and other dependencies are resolved by opkg from the router's compatible feeds; the PIN helper does not install ModemManager itself.

```sh
python3 scripts/inspect-ipk.py .sdk-artifacts/<output-directory> --machine 62 --require-zh-cn
# On x86-64 Linux only, execute the packaged binary with its musl libraries and Mock adapter:
python3 scripts/smoke-sdk.py .sdk-artifacts/<output-directory>
```

Replace `62` with `183` for AArch64, `40` for ARM or `8` for MIPS. The SDK script runs the architecture/contents check automatically. The Mock smoke test does not validate real modems, network services or non-x86 execution. See [current validation](VALIDATION-CURRENT.md) and [development tests](DEVELOPMENT.md#build-and-test-locally).

## Tag-triggered GitHub Release

Pushing a **new tag beginning with `v`** triggers [the release workflow](../.github/workflows/release.yml). Nine matrix jobs independently build the targets in the table above with Chinese translations and the PIN helper enabled. Each job validates its four project IPKs, including package architecture and ELF bitness/endianness. After **all** jobs pass, a separate job publishes the IPKs and `SHA256SUMS.txt` to that tag's GitHub Release. A failed architecture prevents the release job from running; inspect the failed Actions job and rerun it.

Release filenames are `<SDK-target>--<original-IPK-name>`, including `_all.ipk` packages. The prefix makes files from different builds distinguishable; package metadata remains unchanged. Download the four files with the same target prefix. To verify a complete download, run `sha256sum -c SHA256SUMS.txt` in the download directory. To verify only a downloaded MIPS little-endian target:

```sh
grep '  mipsel_24kc--' SHA256SUMS.txt | sha256sum -c -
```

Before tagging, update and commit package versions in `package/sim-relay/Makefile` and, if appropriate, `package/sim-relay-modemmanager/Makefile`; the tag does **not** rewrite IPK versions. LuCI package versions are generated by `luci.mk`; the workflow fixes their source timestamps to the tag commit time so that matrix jobs and reruns use stable filenames. For example:

```sh
git tag v0.6.0
git push origin v0.6.0
```

The repository must allow GitHub Actions to create releases with `contents: write`. The workflow operates on the pushed tag; creating or pushing tags remains a maintainer action. Push tags individually: [GitHub does not create tag push events when more than three tags are pushed at once](https://docs.github.com/en/webhooks/webhook-events-and-payloads#push). Other OpenWrt versions and architectures require a separate matching SDK build.

To add a release architecture, update `resolve_arch()` in `scripts/build-sdk.sh`, `TARGETS` in `scripts/prepare-release.py`, and the workflow matrix and expected package count (four per target). Check the official SDK tag, package architecture, ELF machine, bitness and endianness, then build and test the target before publishing.
