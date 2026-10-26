# Source UEFI USB QEMU gate

Run only against newly created image files in the isolated USB bundle:

```sh
../.tools/mu-venv/bin/python tools/dali/build_usb_qemu.py --output ../.outputs/usb-qemu-build
python3 tools/dali/run_usb_qemu.py --build ../.outputs/usb-qemu-build/usb-qemu-build.json --output ../.outputs/usb-qemu-runs
```

The builder compiles the pinned Mu XhciDxe, UsbBusDxe, UsbKbDxe, UsbMouseDxe,
UsbMassStorageDxe, DiskIoDxe, PartitionDxe and Fat plus a QEMU-only application.
The platform firmware is an explicitly hashed external AArch64 UEFI image; its
PCI root and basic services remain in use. The application disconnects its USB
controller/classes and explicitly binds the freshly compiled drivers, checking
actual BY_DRIVER agents on controller and USB descendant protocols. Recursive
UEFI connection alone does not guarantee the preferred child drivers were used.
The test requires one controller, one direct hub and one keyboard/mouse/BOT disk
under that hub. The tagged filesystem must be a GPT partition under that same
disk. Ambiguous matching volumes and pre-existing reserved test filenames refuse
the file case before any application write. Disconnect must remove that disk's
filesystem protocol before reconnect. Keyboard release buffers start nonzero so
a short control report cannot prove release.

The boot ESP uses virtio. The separate test GPT/FAT32 disk uses only USB BOT,
behind QEMU's full-speed hub on xHCI USB2 ports. Keyboard/mouse also traverse
the hub. QMP injects a key press/release and relative pointer movement/button
press/release. Source SimpleTextIn/SimplePointer and USB GET_REPORT are checked.
The application also invokes the complete public UniXNURun boot manager, with
an explicit test-only system ConIn binding to the source keyboard. It reads a
fresh boot-ESP config, selects by mouse, cancels the confirmation, selects again
by keyboard, then confirms with a new mouse click. Ordered production logs and
screenshots are required. The source graphics menu and confirmation are tested
in both layouts (`--menu-layout portrait` selects the other run); this does not
qualify native Dali console aggregation or a physical portrait display.
FAT cases create, read, modify, flush, rename, delete a nonempty file, close,
disconnect/rebind the mass-storage/filesystem stack and read back the contents.
After shutdown, mtools reads the resulting FAT32 partition and SHA256 verifies
the complete modified data; the old filename and deleted file must be absent.
Each run preserves images, serial/QEMU logs and a separate result.json. Failure
never writes a passing result. The helper processes only its own QEMU child.
The build manifest records PE architecture/subsystem checks, source commits,
tracked diffs, untracked file hashes and initialized nested submodule identities.
The generic UniXNU source tree is independently identified as a build input.
The run records the external firmware and initial variable-template hashes and
requires exactly one copy of each serial milestone in order.

For Dali FV assembly, `DALI_USB_HOST_STACK=true` enables source xHCI, bus,
keyboard/mouse/BOT, partition and console aggregation drivers. It defaults to
false in both DeviceBuild.py and dali.dsc. DiskIo/Fat/English already belong to
the base FV. This opt-in assembles the class stack; native MT6991 port resources,
MMIO-to-HC registration, qualified DMA, roles, CDC and EBS handoff remain separate
implementation gates. The QEMU test application is never placed in the Dali FV.
An independent `DALI_USB_CDC_LOG=true` build option adds a shared DXE CDC log
protocol and maps DXE drivers/UEFI applications to a best-effort SerialPortLib
tee. It remains off by default and keeps every byte in ramoops first. A future
device-mode MTU3 provider must bind an initialized gadget through that protocol;
no such UEFI provider is installed today. SEC, DXE Core, and runtime-driver
serial writes remain ramoops-only, and early-log replay is not implemented.
DALI_USB_HOST_STACK also explicitly enables the default-false UniXNU pointer-input PCD;
the production menu reacquires physical input protocols, clears button history
on reinstall/stream changes and requires a fresh same-item press/release.
The inherited DSC still compiles several USB modules when the FV switch is off.
After a full opt-in build, verify the actual final compressed FD against compiled
PE images with:

```sh
python3 tools/dali/verify_usb_fv.py --fd Build/daliPkg/DEBUG_CLANGPDB/FV/SILICIUM_UEFI.fd --build Build/daliPkg/DEBUG_CLANGPDB --output ../.outputs/usb-core/firmware-usb-stack-evidence.json
```

For a CDC-only FV, use `--cdc-log-only`; for the combined host/class/CDC profile,
add `--resource-context --native-host-worker --cdc-log` as appropriate.

These results do not qualify MT6991, a physical high-speed hub, the native MMIO
bridge, UEFI CDC output or XNU. Keep `hardware_qualified=false`. No phone command, flashing,
physical role switch, physical medium or internal UFS write belongs to this gate.
