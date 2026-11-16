#!/usr/bin/env bash
set -euo pipefail

repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
sdk_image=${SDK_IMAGE:-ghcr.io/openwrt/sdk:x86-64-24.10.4}
local_image=${SDK_LOCAL_IMAGE:-sim-relay-sdk:local}
jobs=${JOBS:-2}
[[ $jobs =~ ^[1-9][0-9]*$ ]] || { echo 'JOBS must be a positive integer' >&2; exit 2; }
if [[ ${1:-} == --help ]]; then
    cat <<'HELP'
Build OpenWrt IPKs using the official Docker SDK.
Usage: bash scripts/build-sdk.sh
Environment: SDK_IMAGE, SDK_LOCAL_IMAGE, SDK_VOLUME, JOBS (default 2),
             EXPECT_ELF_MACHINE (optional, e.g. 62 for x86-64), WITH_SMTP=1, WITH_MM_PIN=1 (optional helpers).
WITH_ZH_CN=1 (default) builds a separate Chinese language package; use 0 for English only.
The source mount is read-only. Artifacts and full logs go to .sdk-artifacts/.
HELP
    exit 0
fi
[[ $# == 0 ]] || { echo 'Unexpected argument; use --help' >&2; exit 2; }
command -v docker >/dev/null || { echo 'Docker CLI is required' >&2; exit 1; }
if ! docker info >/dev/null; then
    echo 'Cannot access the Docker daemon. No SDK build has run.' >&2
    exit 1
fi
run_dir=$(mktemp -d "$repo/.sdk-artifacts-run.XXXXXX")
# Move the temporary directory under the ignored artifacts tree before writing logs.
mkdir -p "$repo/.sdk-artifacts"
output="$repo/.sdk-artifacts/$(date -u +%Y%m%dT%H%M%SZ)-${run_dir##*.}"
mv -- "$run_dir" "$output"
container=""
cleanup() {
    local status=$?
    if [[ -n $container ]]; then docker rm -f "$container" >/dev/null 2>&1 || true; fi
    # mktemp uses 0700. Restore ownership even when sudo builds fail early so
    # the invoking user can read logs and inspect packages without sudo.
    if [[ $(id -u) == 0 && ${SUDO_UID:-} =~ ^[0-9]+$ && ${SUDO_GID:-} =~ ^[0-9]+$ ]]; then
        chown -hR -P -- "$SUDO_UID:$SUDO_GID" "$output" || true
        chown -h -- "$SUDO_UID:$SUDO_GID" "$repo/.sdk-artifacts" || true
    fi
    return "$status"
}
trap cleanup EXIT
printf '%s\n' "$sdk_image" > "$output/sdk-image-requested.txt"
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
    -e "WITH_ZH_CN=${WITH_ZH_CN:-1}" -e "WITH_MM_PIN=${WITH_MM_PIN:-0}" -e "JOBS=$jobs" -e "SDK_IMAGE_REF=$sdk_image" -e "WITH_SMTP=${WITH_SMTP:-0}" \
    -e "EXPECT_ELF_MACHINE=${EXPECT_ELF_MACHINE:-}" "$image_id")
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
    echo "SDK build failed (exit $exit_code). Log: $output/sdk-build.log" >&2
    exit 1
fi
docker cp "$container:/builder/sim-relay-artifacts/." "$output/"
[[ -s $output/verification.json ]] || { echo 'Missing artifact verification report' >&2; exit 1; }
printf 'Verified SDK build artifacts: %s\n' "$output"
