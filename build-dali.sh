#!/usr/bin/env bash
# Build and package only; never reboot or write a connected device.
set -euo pipefail
PORT_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
if [[ $# != 3 && $# != 5 ]]; then
  echo "usage: bash build-dali.sh VERIFIED_BACKUP UNIXNU_CHECKOUT DEVICE_REPOSITORY [SOURCE_XNU_MACHO APPLE_ADT]" >&2
  exit 2
fi
PORT_BACKUP=$(realpath "$1")
PORT_MU=$PORT_ROOT
PORT_UNIXNU=$(realpath "$2")
PORT_DEVICE=$(realpath "$3")
PORT_PYTHON=${MU_PYTHON:-python3}
PORT_XNU_KERNEL=
PORT_XNU_ADT=
if [[ $# == 5 ]]; then
  PORT_XNU_KERNEL=$(realpath "$4")
  PORT_XNU_ADT=$(realpath "$5")
fi
verify_submodule() {
  local parent=$1 path=$2 status expected actual
  status=$(git -C "$parent" submodule status -- "$path")
  expected=$(git -C "$parent" ls-tree HEAD -- "$path" | awk '$1 == "160000" { print $3 }')
  actual=$(git -C "$parent/$path" rev-parse HEAD 2>/dev/null || true)
  if [[ -z "$expected" || "${status:0:1}" != " " || "$actual" != "$expected" ]]; then
    echo "Submodule is missing or differs from its pinned gitlink: $parent/$path" >&2
    exit 1
  fi
  if ! git -C "$parent/$path" diff --quiet --ignore-submodules=all HEAD --; then
    echo "Tracked submodule files are modified: $parent/$path" >&2
    exit 1
  fi
}
[[ $(git -C "$PORT_MU/Mu_Basecore" rev-parse HEAD) == 3c9c7b2e211e62d2a954ad760a36fe05adc43aa3 ]] || {
  echo "Initialize the pinned Mu_Basecore submodule before building" >&2; exit 1;
}
PORT_EXPECTED_UNIXNU=$(python3 - "$PORT_DEVICE/board.toml" <<'PY'
import sys, tomllib
with open(sys.argv[1], "rb") as source:
    print(tomllib.load(source).get("firmware", {}).get("unixnu_commit", ""))
PY
)
PORT_ACTUAL_UNIXNU=$(git -C "$PORT_UNIXNU" rev-parse HEAD)
[[ "$PORT_EXPECTED_UNIXNU" =~ ^[0-9a-f]{40}$ && "$PORT_ACTUAL_UNIXNU" == "$PORT_EXPECTED_UNIXNU" ]] || {
  echo "UniXNU revision differs from the board.toml pin: expected $PORT_EXPECTED_UNIXNU, got $PORT_ACTUAL_UNIXNU" >&2; exit 1;
}
PORT_EXPECTED_UEFI=$(python3 - "$PORT_DEVICE/board.toml" <<'PY'
import sys, tomllib
with open(sys.argv[1], "rb") as source:
    print(tomllib.load(source).get("firmware", {}).get("uefi_commit", ""))
PY
)
PORT_ACTUAL_UEFI=$(git -C "$PORT_ROOT" rev-parse HEAD)
[[ "$PORT_EXPECTED_UEFI" =~ ^[0-9a-f]{40}$ && "$PORT_ACTUAL_UEFI" == "$PORT_EXPECTED_UEFI" ]] || {
  echo "UEFI-Dali revision differs from the board.toml pin: expected $PORT_EXPECTED_UEFI, got $PORT_ACTUAL_UEFI" >&2; exit 1;
}
while IFS= read -r PORT_SUBMODULE_STATUS; do
  [[ -z "$PORT_SUBMODULE_STATUS" || "${PORT_SUBMODULE_STATUS:0:1}" == " " ]] || {
    echo "UEFI-Dali submodule differs from its pinned gitlink: $PORT_SUBMODULE_STATUS" >&2; exit 1;
  }
done < <(git -C "$PORT_ROOT" submodule status)
git -C "$PORT_ROOT" submodule foreach 'if [ "$name" != "Mu_Basecore" ]; then git diff --quiet --ignore-submodules=all HEAD --; fi'
for PORT_SUBMODULE in \
  MdePkg/Library/BaseFdtLib/libfdt \
  MdePkg/Library/MipiSysTLib/mipisyst \
  BaseTools/Source/C/BrotliCompress/brotli \
  MdeModulePkg/Library/BrotliCustomDecompressLib/brotli; do
  verify_submodule "$PORT_MU/Mu_Basecore" "$PORT_SUBMODULE"
done
"$PORT_PYTHON" "$PORT_ROOT/tools/dali/verify_mu_patches.py" \
  --mu "$PORT_MU/Mu_Basecore" --patch-dir "$PORT_ROOT/Resources/MuPatches"
mkdir -p "$PORT_ROOT/out/logs"
git -C "$PORT_UNIXNU" submodule update --init --depth 1 -- drivers/DarwinBoot
UNIXNU_EDK2="$PORT_MU/Mu_Basecore" UNIXNU_BUILD_DIR="$PORT_ROOT/out/package" \
  bash "$PORT_UNIXNU/package/build.sh" -b DEBUG -t CLANGPDB > "$PORT_ROOT/out/logs/package.log" 2>&1
UNIXNU_EDK2="$PORT_MU/Mu_Basecore" TARGET=DEBUG \
  bash "$PORT_UNIXNU/scripts/build-darwinboot.sh" > "$PORT_ROOT/out/logs/darwinboot.log" 2>&1
PAYLOAD_CONFIG="$PORT_DEVICE/package/unixnu-uefi-test.toml"
PAYLOAD_ARGS=()
if [[ -n "$PORT_XNU_KERNEL" ]]; then
  PAYLOAD_CONFIG="$PORT_DEVICE/package/unixnu.toml"
  # GetSectionFromAnyFv needs one contiguous copy of this volume. Cycle 1
  # proved the 48 MiB volume cannot fit after the Dali FVs consume the DXE arena.
  PAYLOAD_ARGS+=(--xnu-kernel "$PORT_XNU_KERNEL" --xnu-adt "$PORT_XNU_ADT" --size-mib 24)
  if [[ -n "${DALI_RUN_TOKEN:-}" ]]; then
    PAYLOAD_ARGS+=(--run-token "$DALI_RUN_TOKEN")
  fi
fi
FASTBOOT_ARGS=(--watchdog-timeout-seconds "${DALI_WATCHDOG_TIMEOUT_SECONDS:-31}")
if [[ "${DALI_AUTO_FASTBOOT:-true}" == "true" ]]; then
  FASTBOOT_ARGS+=(--auto-fastboot)
elif [[ "${DALI_AUTO_FASTBOOT}" == "false" ]]; then
  FASTBOOT_ARGS+=(--no-auto-fastboot)
else
  echo "DALI_AUTO_FASTBOOT must be true or false" >&2
  exit 2
fi
python3 "$PORT_ROOT/tools/make_payload.py" \
  --config "$PAYLOAD_CONFIG" \
  --efi-dir "$PORT_ROOT/out/package/Build/Out/AARCH64-DEBUG" --output "$PORT_ROOT/out/payload.img" \
  "${FASTBOOT_ARGS[@]}" "${PAYLOAD_ARGS[@]}"
export DALI_UNIXNU_PACKAGE="$PORT_UNIXNU/package"
export DALI_DARWINBOOT_PACKAGE="$PORT_UNIXNU/drivers/DarwinBoot"
export DALI_PAYLOAD_IMAGE="$PORT_ROOT/out/payload.img"
export SOURCE_DATE_EPOCH=$(git -C "$PORT_ROOT" show -s --format=%ct HEAD)
(
  cd "$PORT_MU"
  "$PORT_PYTHON" build_uefi.py -d dali -r DEBUG
) > "$PORT_ROOT/out/logs/firmware.log" 2>&1
python3 "$PORT_ROOT/tools/dali/package.py" --backup "$PORT_BACKUP" --mu "$PORT_MU" \
  --unixnu "$PORT_UNIXNU" --device-repo "$PORT_DEVICE" --out "$PORT_ROOT/out"
