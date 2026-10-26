#!/usr/bin/env bash
# Build and package only; never reboot or write a connected device.
set -euo pipefail
PORT_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
if [[ $# != 3 && $# != 5 && $# != 7 ]]; then
  echo "usage: bash build-dali.sh VERIFIED_BACKUP UNIXNU_CHECKOUT DEVICE_REPOSITORY [SOURCE_XNU_MACHO APPLE_ADT [OWN_PID1_MACHO OWN_TRUST_CACHE]]" >&2
  exit 2
fi
PORT_BACKUP=$(realpath "$1")
PORT_MU=$PORT_ROOT
PORT_UNIXNU=$(realpath "$2")
PORT_DEVICE=$(realpath "$3")
PORT_PYTHON=${MU_PYTHON:-python3}
PORT_XNU_KERNEL=
PORT_XNU_ADT=
PORT_PROBE_PACKAGE_ARGS=()
PORT_XNU_UFS_READ_ONLY=${DALI_XNU_UFS_READ_ONLY:-false}
PORT_XNU_UFS_FDT=
PORT_UFS_LOG=${DALI_UFS_LOG_ENABLED:-false}
if [[ "$PORT_UFS_LOG" == true ]]; then
  [[ "$PORT_XNU_UFS_READ_ONLY" == true &&
     ( "${DALI_UFS_PROFILE:-off}" == expdb-log-diagnostic || "${DALI_UFS_PROFILE:-off}" == userdata-logs-diagnostic ) &&
     "${DALI_UFS_MEASURE_PAYLOAD:-false}" == true && -n "${DALI_UFS_LOG_WIRE:-}" &&
     -f "$DALI_UFS_LOG_WIRE" ]] || {
    echo "UFS log needs measured source XNU, an explicit target profile/wire and read-only XNU owner" >&2; exit 2;
  }
elif [[ "$PORT_UFS_LOG" != false || -n "${DALI_UFS_LOG_WIRE:-}" ]]; then
  echo "Invalid expdb log opt-in" >&2; exit 2
fi
if [[ "$PORT_XNU_UFS_READ_ONLY" == true ]]; then
  [[ $# == 5 && "${DALI_UFS_READ_ONLY:-false}" == true &&
     ( "${DALI_UFS_PROFILE:-off}" == read-only-blockio || "$PORT_UFS_LOG" == true ) && "${DALI_UFS_PROBE:-false}" == false &&
     "${DALI_AUTO_FASTBOOT:-true}" == false && -z "${DALI_RESET_METHOD:-}" &&
     "${DALI_RUN_TOKEN:-}" =~ ^[0-9a-f]{32}$ && -n "${DALI_XNU_UFS_LIVE_FDT:-}" &&
     -f "${DALI_XNU_UFS_LIVE_FDT}" ]] || {
    echo "Combined firmware/XNU UFS read-only needs five source-XNU inputs, a fresh token, live FDT, manual recovery and no reset" >&2; exit 2;
  }
  PORT_XNU_UFS_FDT=$(realpath "$DALI_XNU_UFS_LIVE_FDT")
  if [[ "$PORT_UFS_LOG" == true ]]; then
    if [[ "${DALI_UFS_PROFILE:-off}" == userdata-logs-diagnostic ]]; then
      PORT_PROBE_PACKAGE_ARGS+=(--ufs-userdata-log-token "$DALI_RUN_TOKEN" --ufs-userdata-log-wire "$(realpath "$DALI_UFS_LOG_WIRE")" --ufs-xnu-live-fdt "$PORT_XNU_UFS_FDT")
    else
      PORT_PROBE_PACKAGE_ARGS+=(--ufs-expdb-log-token "$DALI_RUN_TOKEN" --ufs-expdb-log-wire "$(realpath "$DALI_UFS_LOG_WIRE")" --ufs-xnu-live-fdt "$PORT_XNU_UFS_FDT")
    fi
    export DALI_UFS_SOURCE_DEVICE="$PORT_DEVICE"
  else
    PORT_PROBE_PACKAGE_ARGS+=(--ufs-xnu-read-only-token "$DALI_RUN_TOKEN" --ufs-xnu-live-fdt "$PORT_XNU_UFS_FDT")
  fi
elif [[ "$PORT_XNU_UFS_READ_ONLY" != false ]]; then
  echo "DALI_XNU_UFS_READ_ONLY must be true or false" >&2; exit 2
elif [[ "${DALI_UFS_READ_ONLY:-false}" == true ]]; then
  [[ "${DALI_UFS_PROFILE:-off}" == read-only-blockio && $# == 3 &&
     "${DALI_UFS_PROBE:-false}" == false && "${DALI_AUTO_FASTBOOT:-true}" == false &&
     -z "${DALI_RESET_METHOD:-}" && "${DALI_RUN_TOKEN:-}" =~ ^[0-9a-f]{32}$ ]] || {
    echo "Physical UFS needs explicit DALI_UFS_PROFILE=read-only-blockio, UEFI-only inputs, fresh token, no probe or automatic reset" >&2; exit 2;
  }
  PORT_PROBE_PACKAGE_ARGS+=(--ufs-read-only-token "$DALI_RUN_TOKEN")
elif [[ "${DALI_UFS_READ_ONLY:-false}" != false || "${DALI_UFS_PROFILE:-off}" != off ]]; then
  echo "Invalid UFS mode: host defaults off; read-only-blockio requires DALI_UFS_READ_ONLY=true" >&2; exit 2
elif [[ "${DALI_UFS_PROBE:-false}" == true ]]; then
  [[ $# == 3 && "${DALI_AUTO_FASTBOOT:-true}" == false && -z "${DALI_RESET_METHOD:-}" &&
     "${DALI_RUN_TOKEN:-}" =~ ^[0-9a-f]{32}$ ]] || {
    echo "UFS probe needs UEFI-only build, fresh token, DALI_AUTO_FASTBOOT=false and no reset method" >&2; exit 2;
  }
  PORT_PROBE_PACKAGE_ARGS+=(--ufs-probe-token "$DALI_RUN_TOKEN")
elif [[ "${DALI_UFS_PROBE:-false}" != false ]]; then
  echo "DALI_UFS_PROBE must be true or false" >&2; exit 2
fi
if [[ $# -ge 5 ]]; then
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
  if [[ "$PORT_XNU_UFS_READ_ONLY" == true ]]; then
    PAYLOAD_ARGS+=(--xnu-ufs-readonly --xnu-ufs-live-fdt "$PORT_XNU_UFS_FDT")
  fi
  if [[ "$PORT_UFS_LOG" == true && "${DALI_UFS_PROFILE:-off}" == userdata-logs-diagnostic ]]; then
    PAYLOAD_ARGS+=(--xnu-ufs-userdata-log-wire "$(realpath "$DALI_UFS_LOG_WIRE")")
  fi
  if [[ $# == 7 ]]; then
    [[ "${DALI_AUTO_FASTBOOT:-true}" == false ]] || {
      echo "PID1 requires DALI_AUTO_FASTBOOT=false to disable the short firmware watchdog" >&2; exit 2;
    }
    PAYLOAD_CONFIG="$PORT_DEVICE/package/unixnu-pid1.toml"
    PAYLOAD_ARGS+=(--xnu-ramdisk "$(realpath "$6")" --xnu-trust-cache "$(realpath "$7")")
  fi
fi
FASTBOOT_ARGS=(--watchdog-timeout-seconds "${DALI_WATCHDOG_TIMEOUT_SECONDS:-31}")
if [[ -n "${DALI_RESET_METHOD:-}" ]]; then
  [[ $# == 5 && "${DALI_AUTO_FASTBOOT:-true}" == false && -n "${DALI_RUN_TOKEN:-}" ]] || {
    echo "DALI_RESET_METHOD needs a kernel/ADT, fresh token, no PID1 root and DALI_AUTO_FASTBOOT=false" >&2; exit 2;
  }
  FASTBOOT_ARGS+=(--reset-method "$DALI_RESET_METHOD")
fi
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
PORT_FV_INF="$PORT_MU/Build/daliPkg/DEBUG_CLANGPDB/FV/FVMAIN.inf"
PORT_DARWINBOOT_FFS=$(awk 'index($0, "DarwinBoot/") { sub(/^.* = /, ""); print; exit }' "$PORT_FV_INF")
if [[ -z "$PORT_DARWINBOOT_FFS" || ! -f "$PORT_DARWINBOOT_FFS" ]]; then
  echo "DarwinBoot driver is missing from the built Dali firmware volume: $PORT_FV_INF" >&2
  exit 1
fi
echo "PASS: Dali firmware volume contains DarwinBoot FFS: $PORT_DARWINBOOT_FFS"
python3 "$PORT_ROOT/tools/dali/package.py" --backup "$PORT_BACKUP" --mu "$PORT_MU" \
  --unixnu "$PORT_UNIXNU" --device-repo "$PORT_DEVICE" --out "$PORT_ROOT/out" "${PORT_PROBE_PACKAGE_ARGS[@]}"
