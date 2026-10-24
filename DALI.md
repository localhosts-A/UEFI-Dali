# REDMI K80 Ultra (dali / MT6991)

This repository is the UEFI firmware port. Device policy and UniXNU payload
configuration belong in the sibling `UniXNU-Dali` device repository; generic
UniXNU application sources stay in the sibling upstream `UniXNU` checkout.

## Current state

The real-device UEFI stage has passed SEC/MMU/DXE/BDS, memory-map, timer and
WaitForEvent checks, UniXNU menu/configuration, SampleDriver, HelloPayload's
successful return, and Exit back to firmware. Runs7 and8 retained complete logs
through a prompted RGU software reset. Original Android B, root and all 16
backed-up boot/companion/LK partition hashes were verified after recovery.

Inherited GOP text/color bars and GPIO60 UP/ENTER input were also physically
verified. The inherited panel requires command-mode frame submission; CPU
framebuffer readback alone is not display proof.

The validated UEFI milestone above is the hardware baseline. A follow-on source
integration is now under development; its firmware and XNU changes are not yet
hardware qualified. The current integration code adds:

- UniXNU and UniXNUDxe in the same firmware volume, with RAM-backed boot options.
- A read-only 1 MiB FAT test volume embedded in the FV, published as BlockIo
  and connected through the generic DiskIo, FAT and Unicode collation drivers.
- System-table consoles and a diagnostic BDS that starts the registered UniXNU
  option, then retains firmware control after its Exit action.
- A source-built XNU entry, generated Apple DT, DarwinBoot FV driver and early
  ramoops checkpoint. This path still requires build and hardware validation.
- Persistent ramoops logging, with distinct core, filesystem, menu, payload and
  return-to-firmware milestones.

The countdown freeze near 35 seconds was traced to the inherited enabled RGU
watchdog. Run7 recorded MODE `0x17d -> 0x17c` and continued through Hello and
Exit, with heartbeats beyond the previous cutoff. SEC validates the exact RGU
device-tree node before stopping it with the keyed MODE write.

MT6363 TOPSTATUS reads use inherited PMIF SWINF2/SID4. Run7 calibrated three
physical presses each: idle `0x1e`, volume-down `0x16` (active-low bit3), power
`0x1c` (active-low bit1). The new mapping emits DOWN and ENTER respectively,
with 20 ms debounce and no hold repeat. Run8 verified all three physical keys:
volume-down/up changed selection, and power started Hello and selected Exit.
Only PMIC reads are implemented; commands and valid acknowledgements write
the PMIF controller, while PMIC configuration and clocks remain untouched.

After UniXNU's Exit action, firmware provides a ten-second window to hold only
volume-down, then issues the keyed RGU software reset. This retained ramoops
and reached fastboot in Run7. A manual power-key reset left pstore empty in
Run6; use the prompted path when collecting evidence.

Bring-up fixes include waiting for HII dependencies before dispatching BDS,
starting the registered FV application with LoadImage/StartImage, and signaling
EndOfDxe before filesystem EFI loads. The latter preserves SecurityStub's
normal deferred-image policy. The current diagnostic image also checks BDS
application TPL and an actual WaitForEvent timer round trip. A dali CpuLib uses
polling idle (YIELD), retaining the upstream EL1 TLB flush sequence, until WFI
wakeup on this handoff is separately validated. This is a power-efficiency
limitation of the bring-up firmware.

Physical UFS, USB device transport, touch and panel cold initialization remain
unimplemented. The RAM volume validates firmware file access; it does not
provide physical storage. DarwinBoot and XNU integration code is present but
has not yet passed the Dali hardware checkpoint.

## Validated UEFI milestone (2026-10-24)

Run8 passed the strict collector with both `--require-unixnu` and
`--require-pmic-keys`, with no assertions or reported stage failures. The owner
confirmed visible menu interaction for all three keys. This completes the
current UEFI bring-up milestone; no Darwin kernel has been started.

| Check | Result |
| --- | --- |
| Host FDT, evidence and actual C input timing tests | 33 passed |
| Firmware and UniXNU package build | Passed; PE checks and FAT file readback passed |
| SEC/MMU/DXE/BDS, memory map, GIC timer and event wait | Real-device PASS |
| GOP, system consoles, RAM FAT and registered FV application | Real-device PASS |
| SampleDriver, Hello return, menu Exit to firmware | Complete persisted log |
| Volume up / volume down / power | UP / DOWN / ENTER, physical and log confirmation |
| RGU software reset, fastboot and Android recovery | Original B/root/16 partition hashes verified |

The tested boot image is 315392 bytes, SHA256
`71dcbf41691bce26be8228c01d79f5702faf2b27f629b8b038d702220e6721f3`.
Private artifacts retain the raw FD, build manifest, exact staged source patch,
pstore evidence and recovery hashes. The image embeds the test configuration
from UniXNU-Dali `4880518` and generic UniXNU `1a01c2a`.

The next milestone builds the source-only kernel on macOS, verifies DarwinBoot
and its QEMU fixtures, then tests the single-core handoff on boot_b. The source
kernel and local Apple DT are packed into a 24 MiB read-only FAT volume in the
64 MiB firmware volume. Cycle 1 showed that extracting the 48 MiB block device
failed a 12,289-page contiguous DXE allocation, leaving UniXNU without its
configuration and XNU entry. The smaller volume keeps that allocation within
the remaining arena; FAT construction still fails closed if the payload files
do not fit. Every image is checked against the live FDT arenas and the original
boot_b partition size before a test flash.

A later hardware cycle installed DarwinBoot successfully but returned
`Out of Resources` before XNU entry. Its default contiguous window is 256 MiB,
larger than the original 128 MiB diagnostic heap. SEC now selects one additional
2 MiB-aligned gap from the incoming FDT between 2 and 4 GiB. It excludes all
reserve-map entries, nested reserved-memory nodes, pKVM protected regions,
initrd, the incoming FDT and every existing firmware/log/framebuffer allocation.
Size-only pool requests are assigned excluded space within their declared
allocation ranges, respecting size and alignment; unsatisfied requests fail
closed. Their Linux consumers are not initialized. Only the remaining gap is exposed
as additional conventional memory; XNU owns only DarwinBoot's allocated window.
The selection and loader allocation/failure stage are persisted in ramoops.
This extension still requires hardware qualification.

The next hardware attempt retained only the SEC entry and normal register
snapshot. Trace v2 subsequently located the stop inside the static reserved-memory
walk, before MMU/DXE/menu entry. Its underlying cause remains unresolved. Early
`SEC trace` and `RAM trace` records now use the direct ramoops writer, including
the incoming FDT address, SP and EL1 control registers; platform rejection also
uses that path. These records are diagnostics, not evidence that the hang is
fixed. The compiled SEC and real FDT traverse the selector under QEMU, including
strict alignment, then refuse the unimplemented RGU register readback.
Trace v3 keeps parent offsets during reserved-memory traversal instead of
repeatedly searching from the FDT root, with a bounded 64-level ancestor stack.
It retains nested cell-width and translated-bus checks and periodically records
node progress plus physical timer ticks. This removes a measured source-level
rescan hotspot; it is not yet a confirmed hardware hang fix.

## Build

Use this firmware checkout with its pinned dependencies:

```sh
git submodule update --init \
  Common/Mu Common/Mu_OEM_Sample Mu_Basecore Silicium-ACPI \
  Silicon/Silicium/OpensslPkg/Library/OpensslLib/openssl
git -C Mu_Basecore submodule update --init \
  MdePkg/Library/BaseFdtLib/libfdt MdePkg/Library/MipiSysTLib/mipisyst \
  BaseTools/Source/C/BrotliCompress/brotli \
  MdeModulePkg/Library/BrotliCustomDecompressLib/brotli
python3 -m venv /path/to/mu-venv
/path/to/mu-venv/bin/pip install -r pip-requirements.txt
/path/to/mu-venv/bin/stuart_update -c Platforms/Xiaomi/daliPkg/DeviceBuild.py

MU_PYTHON=/path/to/mu-venv/bin/python bash build-dali.sh \
  /absolute/path/to/verified-dali-backup ../UniXNU ../UniXNU-Dali
```

The reviewed UEFI-only build uses the current pinned UniXNU commit. A five-argument
build adds `SOURCE_XNU_MACHO APPLE_ADT`; generate the private Apple DT from the
current runtime FDT. The reviewed UniXNU commit is
`1a01c2afa197d975d04da892325c56b6b14aaecb`. The firmware fork baseline is
`42fe563791e51bf5500c2e470ed294ca0c94b97c`; submodules are pinned by Git.
The build uses the system `clang`, `lld`, `llvm-objcopy`, legacy-capable `lz4`,
`mtools` and the Mu build dependencies. Mu's nasm dependency may require the
system-nasm workaround described by upstream on hosts without Mono.
Host memory-window tests also require `dtc` and libfdt development headers/library.

Outputs are local under `out/`: `UniXNU-dali-uefi-boot.img`, `manifest.json`,
`payload.img`, `payload.json` and build logs. The raw FD is
`Build/daliPkg/DEBUG_CLANGPDB/FV/SILICIUM_UEFI.fd`. A build never reboots or
flashes the phone. `tools/dali/backup.py` creates a verified backup from a
rooted Android device; keep it outside Git.

## Device-specific contracts

- Android boot header v4, legacy LZ4 and `BootShim/DaliShim.S`, preserving LK FDT
  in x0. The generic UniXNU gzip/BootShim.S packer is not used.
- FD `0xc1000000 + 0x4000000`, stack `0xc5000000 + 0x40000`, FDT copy
  `0xc5080000 + 0x100000`, DXE heap `0xc5200000 + 0x8000000`.
- The FDT preflight excludes reserved, protected, memreserve and initrd regions;
  SEC checks again. These addresses require review after firmware changes.
- GICv3 distributor `0x0c400000`, redistributor `0x0c440000`, PSCI SMC and the
  13 MHz architected timer. Only the boot CPU is enabled.
- Inherited 1280×2772 BGRX scanout, pitch 5120, reserved framebuffer
  `0xfc990000 + 0x3660000`. Only validated linear LK buffers are mirrored.
- GPIO60 is physical volume-up: tap emits UP; hold 700 ms emits ENTER, without
  changing the inherited GPIO/KPCOL0 mux. The FDT node is named `volumedown`.
- MT6363 is SID4 on SPMI-M (`spmi-dev-mask` bit4 clear), PMIF `0x1c01a000`.
  Diagnostic reads use SWINF2 ACC `+0x880`, RDATA `+0x894`, VLD_CLR `+0x8a4`,
  STA `+0x8a8`, with bounded polling. The PMIC state register is `0x1e`.
  Bit3 is volume-down (DOWN), bit1 is power (ENTER), both active-low. Live
  FDT keycodes must match 114 and 116. The GPIO60 volume-up path stays separate.
- RGU watchdog `0x1c010000` must advertise both MT6991 and MT6589 compatibility.
  Only inherited MODE.EN is cleared with key `0x22000000`, after reload `0x1971`.
- XNU tests default to a one-shot bootloader reboot marker at RGU `+0x24` and a
  31-second maximum single-shot watchdog; the configuration may select 2–31
  seconds or disable auto-fastboot. The 120-second timeout is only the host
  wait deadline. Linux's MT6991 driver documents the 2–31-second range; whether
  LK consumes this marker after watchdog reset still requires hardware proof.
  LENGTH's timeout field is checked on readback. While UEFI Boot Services remain
  active, a separate 60-second application timer performs a marked warm reset if
  the menu or loader remains running. This timer cannot recover an XNU freeze
  after ExitBootServices; the hardware watchdog must cover that phase.
- Logs use the inspected ramoops console at `0xaec0f000`, 2 MiB. The prompted
  Exit path uses an RGU warm reset with demonstrated retention in Run7.
- The initial 128 MiB DXE heap and firmware arenas remain bounded. The additional
  XNU region is selected and validated from each incoming FDT. The Dali
  ADT generator refuses a changed DRAM/GIC/PSCI/timer/watchdog contract; the
  host FDT preflight rejects collisions before packaging.

## Verification and recovery

```sh
python3 -m unittest discover -s tools/dali -p 'test_*.py' -v
python3 tools/dali/qemu_sec_smoke.py --dtb /private/runtime.dtb --exception-level 1
python3 tools/dali/qemu_sec_smoke.py --dtb /private/runtime.dtb --exception-level 1 \
  --fdt-address 0x87c80000 --strict-alignment
```

The earlier QEMU check verified shim/SEC/MMU entry into DXE. QEMU does not
emulate MT6991 MMIO; the newer strict RGU readback currently rejects its
all-ones register value before MMU entry. This run is unsupported, not a pass.
The image and FAT tools check headers, sizes, hashes and per-file FAT readback.

Hardware tests must first recheck the exact device, running FDT, active B slot
and original boot_b hash. Back up both slots and boot metadata. The owner has
selected direct boot_b tests because fastboot boot is unsupported. Restore the
verified original boot_b through the known hardware-key fastboot path, boot
Android B, then collect immediately:

```sh
python3 tools/dali/collect.py --serial SERIAL --output /private/run \
  --require-unixnu --require-pmic-keys
```

Verify Android boot completion, root and all backed-up boot/companion/LK hashes.
Never restore misc blindly or alter LK/preloader/AVB on an image refusal. Empty
pstore is inconclusive. Keep private images, runtime DTBs and device identifiers
out of commits.

The early-XNU cycle runner in `UniXNU-Dali/scripts/run-bootb-cycles.py` accepts
only `boot_b`, confirms the ADB/fastboot identity, verifies a fresh fastboot
enumeration, and restores the original `boot_b` after every attempt. Each cycle
gets a unique token in both the recovery-arm and XNU checkpoint logs so stale
ramoops data cannot satisfy either check; the collector also checks that the
candidate's 2–31 second watchdog value matches the artifact. After restoration it
verifies Android/root and every backed partition hash on success and failure paths.
The 120-second bound is a host wait deadline, not the device watchdog. Its logs,
kernel artifact, Apple DT and images belong in a private directory.

## Register-contract references

- [Official MT6991 watchdog driver](https://github.com/OnePlusOSS/android_kernel_oneplus_mt6991/blob/oneplus/mt6991_v_15.0.2_ace5_ultra/drivers/watchdog/mtk_wdt.c)
- [Official MT6991 PMIF core](https://github.com/oppo-source/android_kernel_modules_and_devicetree_oppo_mt6991/blob/oppo/mt6991_v_15.0.0_oppo_find_x8_pro/kernel/kernel_device_modules-6.6/drivers/spmi/spmi-mtk-pmif-core.c)
- [MT6363 key driver](https://github.com/oppo-source/android_kernel_modules_and_devicetree_oppo_mt6991/blob/oppo/mt6991_v_15.0.0_oppo_find_x8_pro/kernel/kernel_device_modules-6.6/drivers/input/keyboard/mtk-pmic-keys.c)

The MT6363 Linux driver uses separate press/release IRQs. Its old polling
mask parameters alone are insufficient proof of the raw key-bit layout;
the mapping above was calibrated from physical state transitions on dali.
