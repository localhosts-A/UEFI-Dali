#!/usr/bin/env bash
# Build and package only; never reboot or write a connected device.
set -euo pipefail
PORT_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
if [[ $# != 3 ]]; then
  echo "usage: bash build-dali.sh VERIFIED_BACKUP UNIXNU_CHECKOUT DEVICE_REPOSITORY" >&2
  exit 2
fi
PORT_BACKUP=$(realpath "$1")
PORT_MU=$PORT_ROOT
PORT_UNIXNU=$(realpath "$2")
PORT_DEVICE=$(realpath "$3")
PORT_PYTHON=${MU_PYTHON:-python3}
[[ $(git -C "$PORT_MU/Mu_Basecore" rev-parse HEAD) == 3c9c7b2e211e62d2a954ad760a36fe05adc43aa3 ]] || {
  echo "Initialize the pinned Mu_Basecore submodule before building" >&2; exit 1;
}
[[ $(git -C "$PORT_UNIXNU" rev-parse HEAD) == 1a01c2afa197d975d04da892325c56b6b14aaecb ]] || {
  echo "UniXNU revision differs from the reviewed baseline" >&2; exit 1;
}
mkdir -p "$PORT_ROOT/out/logs"
UNIXNU_EDK2="$PORT_MU/Mu_Basecore" UNIXNU_BUILD_DIR="$PORT_ROOT/out/package" \
  bash "$PORT_UNIXNU/package/build.sh" -b DEBUG -t CLANGPDB > "$PORT_ROOT/out/logs/package.log" 2>&1
python3 "$PORT_ROOT/tools/make_payload.py" \
  --config "$PORT_DEVICE/package/unixnu.toml" \
  --efi-dir "$PORT_ROOT/out/package/Build/Out/AARCH64-DEBUG" --output "$PORT_ROOT/out/payload.img"
export DALI_UNIXNU_PACKAGE="$PORT_UNIXNU/package"
export DALI_PAYLOAD_IMAGE="$PORT_ROOT/out/payload.img"
export SOURCE_DATE_EPOCH=$(git -C "$PORT_ROOT" show -s --format=%ct HEAD)
(
  cd "$PORT_MU"
  "$PORT_PYTHON" build_uefi.py -d dali -r DEBUG
) > "$PORT_ROOT/out/logs/firmware.log" 2>&1
python3 "$PORT_ROOT/tools/dali/package.py" --backup "$PORT_BACKUP" --mu "$PORT_MU" --out "$PORT_ROOT/out"
