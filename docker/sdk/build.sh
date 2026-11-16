#!/usr/bin/env bash
set -euo pipefail
cd /builder
[[ -d scripts && -d staging_dir ]] || { echo 'OpenWrt SDK is not initialized' >&2; exit 1; }
exec 9>/builder/.sim-relay-sdk.lock
flock -n 9 || { echo 'This SDK volume already has an active build' >&2; exit 1; }
jobs=${JOBS:-2}
[[ $jobs =~ ^[1-9][0-9]*$ ]] || exit 2

suite=/builder/package/sim-relay-suite
if [[ -e $suite ]]; then
    [[ -f $suite/.sim-relay-managed ]] || { echo 'Refusing to replace an unmanaged package directory' >&2; exit 1; }
    rm -rf -- "$suite"
fi
mkdir -p "$suite/package"
touch "$suite/.sim-relay-managed"
cp /src/CMakeLists.txt /src/LICENSE "$suite/"
cp -R /src/src "$suite/src"
cp -R /src/package/sim-relay "$suite/package/sim-relay"
cp -R /src/package/sim-relay-smtp "$suite/package/sim-relay-smtp"
cp -R /src/package/sim-relay-modemmanager "$suite/package/sim-relay-modemmanager"
cp -R /src/luci-app-sim-relay "$suite/luci-app-sim-relay"

# Keep the release SDK's feed URLs/commit pins; never substitute a moving branch.
./scripts/feeds update -a
./scripts/feeds install -a
touch .config
sed -i '/^CONFIG_ALL=/d; /^CONFIG_ALL_NONSHARED=/d; /^CONFIG_ALL_KMODS=/d; /^CONFIG_PACKAGE_sim-relay=/d; /^CONFIG_PACKAGE_luci-app-sim-relay=/d; /^CONFIG_PACKAGE_sim-relay-smtp=/d; /^CONFIG_PACKAGE_sim-relay-modemmanager=/d; /^CONFIG_PACKAGE_luci-i18n-sim-relay-zh-cn=/d; /^CONFIG_LUCI_LANG_zh_Hans=/d' .config
cat >> .config <<'CONFIG'
# CONFIG_ALL is not set
# CONFIG_ALL_NONSHARED is not set
# CONFIG_ALL_KMODS is not set
CONFIG_PACKAGE_sim-relay=m
CONFIG_PACKAGE_luci-app-sim-relay=m
# CONFIG_PACKAGE_sim-relay-smtp is not set
# CONFIG_PACKAGE_sim-relay-modemmanager is not set
# CONFIG_PACKAGE_luci-i18n-sim-relay-zh-cn is not set
# CONFIG_LUCI_LANG_zh_Hans is not set
CONFIG
if [[ ${WITH_SMTP:-0} == 1 ]]; then
    printf '\nCONFIG_PACKAGE_sim-relay-smtp=m\n' >> .config
fi
if [[ ${WITH_MM_PIN:-0} == 1 ]]; then
    printf '\nCONFIG_PACKAGE_sim-relay-modemmanager=m\n' >> .config
fi
if [[ ${WITH_ZH_CN:-1} == 1 ]]; then
    # Some release SDKs define LUCI_LANG_* as bool, newer feeds as tristate.
    # y is valid for both; the translation IPK remains independently selected.
    printf '\nCONFIG_LUCI_LANG_zh_Hans=y\nCONFIG_PACKAGE_luci-i18n-sim-relay-zh-cn=m\n' >> .config
fi
make defconfig
packages=(sim-relay luci-app-sim-relay)
if [[ ${WITH_ZH_CN:-1} == 1 ]]; then packages+=(luci-i18n-sim-relay-zh-cn); fi
for name in "${packages[@]}"; do
    if ! grep -Eq "^CONFIG_PACKAGE_${name}=[my]$" .config; then
        printf 'ERROR: Package %s was not selected after make defconfig\n' "$name" >&2
        grep -E '^CONFIG_(MODULES|PACKAGE_.*sim-relay)' .config >&2 || true
        exit 1
    fi
done
make package/sim-relay-suite/package/sim-relay/clean V=s
make package/sim-relay-suite/luci-app-sim-relay/clean V=s
mkdir -p bin
find bin -type f \( -name 'sim-relay_*.ipk' -o -name 'luci-app-sim-relay_*.ipk' -o -name 'sim-relay-smtp_*.ipk' -o -name 'sim-relay-modemmanager_*.ipk' -o -name 'luci-i18n-sim-relay-*_*.ipk' \) -delete
make -j"$jobs" package/sim-relay-suite/package/sim-relay/compile V=s
make -j"$jobs" package/sim-relay-suite/luci-app-sim-relay/compile V=s
if [[ ${WITH_SMTP:-0} == 1 ]]; then
    make -j"$jobs" package/sim-relay-suite/package/sim-relay-smtp/compile V=s
fi

if [[ ${WITH_MM_PIN:-0} == 1 ]]; then
    make -j"$jobs" package/sim-relay-suite/package/sim-relay-modemmanager/compile V=s
fi

output=/builder/sim-relay-artifacts
mkdir -p "$output"
find "$output" -maxdepth 1 -type f -delete
find bin -type f -name '*.ipk' -exec cp '{}' "$output/" \;
cp .config "$output/sdk.config"
cp feeds.conf.default "$output/feeds.conf.default"
(cd "$suite" && find src package luci-app-sim-relay CMakeLists.txt LICENSE -type f -print0 | sort -z | xargs -0 sha256sum) > "$output/source-sha256sums.txt"
{
    printf 'SDK image: %s\n' "${SDK_IMAGE_REF:-unknown}"
    cat include/version.mk
    for feed in feeds/*; do
        if [[ -d $feed/.git ]]; then printf '%s ' "$feed"; git -C "$feed" rev-parse HEAD; fi
    done
} > "$output/build-provenance.txt"
args=()
if [[ ${WITH_ZH_CN:-1} == 1 ]]; then args+=(--require-zh-cn); fi
if [[ -n ${EXPECT_ELF_MACHINE:-} ]]; then args+=(--machine "$EXPECT_ELF_MACHINE"); fi
python3 /opt/sim-relay/inspect-ipk.py "$output" "${args[@]}" > "$output/verification.json"
(cd "$output" && sha256sum ./*.ipk > SHA256SUMS)
cat "$output/verification.json"
