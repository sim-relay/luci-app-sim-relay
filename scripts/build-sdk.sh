#!/usr/bin/env bash
set -euo pipefail

repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
release=${SDK_VERSION:-24.10.4}
requested=${ARCH:-x86-64}
jobs=${JOBS:-2}
[[ $jobs =~ ^[1-9][0-9]*$ ]] || { echo 'JOBS must be a positive integer' >&2; exit 2; }
[[ $release =~ ^[0-9A-Za-z._-]+$ ]] || { echo 'SDK_VERSION contains unsupported characters' >&2; exit 2; }

if [[ ${1:-} == --help ]]; then
    cat <<'HELP'
Build OpenWrt IPKs using the official Docker SDK.
Usage: bash scripts/build-sdk.sh
Environment: ARCH, SDK_VERSION, SDK_IMAGE, SDK_LOCAL_IMAGE, SDK_VOLUME,
             JOBS (default 2), EXPECT_ELF_MACHINE,
             WITH_MM_PIN=1, WITH_ZH_CN=0, SIM_RELAY_RELEASE_EPOCH (optional).
ARCH accepts one value or a comma/space separated list (default x86-64):
  x86-64 | amd64                 x86-64 routers and virtual machines
  arm64 | aarch64                aarch64_generic (armvirt, rockchip, ...)
  aarch64_cortex-a53             MediaTek Filogic, ipq807x, sunxi, ...
  aarch64_cortex-a72             Raspberry Pi 4, mvebu cortexa72, ...
  armv7 | arm_cortex-a7_neon-vfpv4  common 32-bit ARM routers
  arm_cortex-a9_vfpv3-d16        Marvell Armada 38x
  arm_cortex-a15_neon-vfpv4      ipq806x
  mips_24kc                      ath79
  mipsel_24kc                    ramips (mt7621 and similar)
SDK_IMAGE, SDK_LOCAL_IMAGE, SDK_VOLUME and EXPECT_ELF_MACHINE are only valid
with a single ARCH. The source mount is read-only. Artifacts and full logs go
to .sdk-artifacts/, one directory per architecture.
HELP
    exit 0
fi
[[ $# == 0 ]] || { echo 'Unexpected argument; use --help' >&2; exit 2; }

resolve_arch() {
    case "$1" in
        x86-64|x86_64|amd64)                  printf '%s %s\n' 'x86-64' 62 ;;
        arm64|aarch64|aarch64_generic)        printf '%s %s\n' 'aarch64_generic' 183 ;;
        aarch64_cortex-a53)                   printf '%s %s\n' 'aarch64_cortex-a53' 183 ;;
        aarch64_cortex-a72)                   printf '%s %s\n' 'aarch64_cortex-a72' 183 ;;
        armv7|arm_cortex-a7_neon-vfpv4)       printf '%s %s\n' 'arm_cortex-a7_neon-vfpv4' 40 ;;
        arm_cortex-a9_vfpv3-d16)              printf '%s %s\n' 'arm_cortex-a9_vfpv3-d16' 40 ;;
        arm_cortex-a15_neon-vfpv4)            printf '%s %s\n' 'arm_cortex-a15_neon-vfpv4' 40 ;;
        mips_24kc)                            printf '%s %s\n' 'mips_24kc' 8 ;;
        mipsel_24kc)                          printf '%s %s\n' 'mipsel_24kc' 8 ;;
        *) return 1 ;;
    esac
}

IFS=', ' read -r -a entries <<< "$requested"
arch_names=()
arch_machines=()
for entry in "${entries[@]}"; do
    [[ -n $entry ]] || continue
    resolved=$(resolve_arch "$entry") || {
        echo "Unsupported ARCH '$entry'; run with --help for the supported list" >&2
        exit 2
    }
    read -r arch machine <<< "$resolved"
    duplicate=0
    for existing in "${arch_names[@]:-}"; do
        [[ $existing == "$arch" ]] && duplicate=1
    done
    if [[ $duplicate == 1 ]]; then
        echo "ARCH '$arch' was requested more than once" >&2
        exit 2
    fi
    arch_names+=("$arch")
    arch_machines+=("$machine")
done
[[ ${#arch_names[@]} -gt 0 ]] || { echo 'ARCH must name at least one architecture' >&2; exit 2; }

if [[ ${#arch_names[@]} -gt 1 ]]; then
    for variable in SDK_IMAGE SDK_LOCAL_IMAGE SDK_VOLUME EXPECT_ELF_MACHINE; do
        if [[ -n ${!variable:-} ]]; then
            echo "$variable applies to a single ARCH; build each architecture separately or unset it" >&2
            exit 2
        fi
    done
fi

command -v docker >/dev/null || { echo 'Docker CLI is required' >&2; exit 1; }
if ! docker info >/dev/null; then
    echo 'Cannot access the Docker daemon. No SDK build has run.' >&2
    exit 1
fi

mkdir -p "$repo/.sdk-artifacts"
containers=()
outputs=()
cleanup() {
    local status=$?
    local item
    if (( ${#containers[@]} )); then
        for item in "${containers[@]}"; do
            docker rm -f "$item" >/dev/null 2>&1 || true
        done
    fi
    if [[ $(id -u) == 0 && ${SUDO_UID:-} =~ ^[0-9]+$ && ${SUDO_GID:-} =~ ^[0-9]+$ ]]; then
        # mktemp uses 0700. Restore ownership even when sudo builds fail early so
        # the invoking user can read logs and inspect packages without sudo.
        if (( ${#outputs[@]} )); then
            for item in "${outputs[@]}"; do
                if [[ -d $item ]]; then chown -hR -P -- "$SUDO_UID:$SUDO_GID" "$item" || true; fi
            done
        fi
        chown -h -- "$SUDO_UID:$SUDO_GID" "$repo/.sdk-artifacts" || true
    fi
    return "$status"
}
trap cleanup EXIT

build_arch() {
    local arch=$1 machine=$2
    local sdk_image=${SDK_IMAGE:-ghcr.io/openwrt/sdk:${arch}-${release}}
    local local_image=${SDK_LOCAL_IMAGE:-sim-relay-sdk:local-${arch}}
    local expect=${EXPECT_ELF_MACHINE:-$machine}
    local run_dir output container image_id volume attach_status exit_code

    run_dir=$(mktemp -d "$repo/.sdk-artifacts/.run.XXXXXX")
    output="$repo/.sdk-artifacts/$(date -u +%Y%m%dT%H%M%SZ)-${arch}-${run_dir##*.}"
    mv -- "$run_dir" "$output"
    outputs+=("$output")

    printf 'Building %s with %s\n' "$arch" "$sdk_image"
    printf '%s\n' "$sdk_image" > "$output/sdk-image-requested.txt"
    printf '%s\n' "$release" > "$output/sdk-version.txt"
    # BuildKit may pull a tag by digest without leaving the tag in the local
    # image store; keep an explicit tag for the provenance record and inspect.
    if ! docker image inspect "$sdk_image" >/dev/null 2>&1; then
        docker pull "$sdk_image" >/dev/null
    fi
    docker build --build-arg "SDK_IMAGE=$sdk_image" -f "$repo/docker/sdk/Dockerfile" \
        -t "$local_image" "$repo" 2>&1 | tee "$output/image-build.log"
    image_id=$(docker image inspect --format '{{.Id}}' "$local_image")
    printf '%s\n' "$image_id" > "$output/build-image-id.txt"
    docker image inspect "$sdk_image" > "$output/base-image-inspect.json"
    volume=${SDK_VOLUME:-sim-relay-sdk-${image_id#sha256:}}
    # The official image owns /builder as buildbot. Docker initializes the named
    # volume from that image, avoiding writable host source mounts or root builds.
    container=$(docker create --mount "type=bind,src=$repo,dst=/src,readonly" \
        --mount "type=volume,src=$volume,dst=/builder" \
        -e "WITH_ZH_CN=${WITH_ZH_CN:-1}" -e "WITH_MM_PIN=${WITH_MM_PIN:-0}" -e "JOBS=$jobs" \
        -e "SIM_RELAY_RELEASE_EPOCH=${SIM_RELAY_RELEASE_EPOCH:-}" \
        -e "SDK_IMAGE_REF=$sdk_image" \
        -e "EXPECT_ELF_MACHINE=$expect" "$image_id")
    containers+=("$container")
    set +e
    docker start -a "$container" 2>&1 | tee "$output/sdk-build.log"
    attach_status=${PIPESTATUS[0]}
    set -e
    exit_code=$(docker inspect --format '{{.State.ExitCode}}' "$container")
    if [[ $exit_code != 0 || $attach_status != 0 ]]; then
        # The container is stopped but still exists until the EXIT trap runs.
        # Keep configuration/scan diagnostics even when no IPK was generated.
        docker cp "$container:/builder/.config" "$output/sdk.config" 2>/dev/null || true
        docker cp "$container:/builder/tmp/.packageinfo" "$output/packageinfo.txt" 2>/dev/null || true
        docker cp "$container:/builder/logs" "$output/diagnostics" 2>/dev/null || true
        echo "SDK build failed for $arch (exit $exit_code). Log: $output/sdk-build.log" >&2
        exit 1
    fi
    docker cp "$container:/builder/sim-relay-artifacts/." "$output/"
    [[ -s $output/verification.json ]] || { echo "Missing artifact verification report for $arch" >&2; exit 1; }
    docker rm -f "$container" >/dev/null 2>&1 || true
    if [[ $(id -u) == 0 && ${SUDO_UID:-} =~ ^[0-9]+$ && ${SUDO_GID:-} =~ ^[0-9]+$ ]]; then
        chown -hR -P -- "$SUDO_UID:$SUDO_GID" "$output" || true
    fi
    printf 'Verified %s SDK build artifacts: %s\n' "$arch" "$output"
}

for index in "${!arch_names[@]}"; do
    build_arch "${arch_names[$index]}" "${arch_machines[$index]}"
done

printf 'Verified SDK build artifacts:\n'
printf '  %s\n' "${outputs[@]}"
