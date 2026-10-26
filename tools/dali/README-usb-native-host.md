# Native USB2 host engine checkpoint

The optional DaliUsbNativeHostLib owns the actual pinned Mu xHCI transfer,
register, schedule and memory engine through the private DaliUsbXhciIoLib ABI.
It publishes EFI_USB2_HC_PROTOCOL and a caller-owned device path on a new
handle. It does not publish PCI I/O or call the source PCI DriverBinding,
Start/Stop, reset, cancellation or timer lifecycle.

`DaliUsbNativeHostWorkerDxe` is the opt-in consumer. Its independent
`DALI_USB_NATIVE_HOST` build flag requires both the USB class stack and
ResourceContext. Revision6 exposes the xHCI range and raw FDT interrupt cells
`<0, 600, 4, 0>`; the worker validates both before acquiring provider revision2.
The cells remain untranslated and do not prove a GIC vector or IRQ ownership.
The worker validates every mandatory qualification bit and the exact
`0x16700000/0x1000` window, then opens MMIO/DMA and starts the engine. Its
provider contract covers powered MMIO, clocks, domain, PHY, host role, connector,
VBUS, exclusive controller/IRQ, translated 32-bit DMA, cache policy and stable
source clocks. A missing/not-ready host role remains stopped and can be retried
through the installed worker protocol. Stop retains the lease if host halt, IO
close or provider release fails. The provider must not force a Type-C role or
VBUS transition.

The context must be persistent, zeroed, noncopyable and used on the BSP. An
already-open IO provider must continuously hold the actual MMIO mapping,
clock/domain/PHY/role/connector/IRQ ownership and qualified device DMA/cache
mapping. Common buffers are coherent, translated 32-bit allocations with
64 KiB alignment. The engine selects supported 4 KiB HC pages and checks
capability apertures, extended-capability bounds, inherited legacy/debug
activity and an explicit USB2 port map before writing reset or schedules.

Startup checks partial source allocations before running. Reset and Stop
freshly halt before freeing users. Failed halt retains the source engine,
queues and DMA records; failed unmap/free records remain independently owned
by IO until its separate Close. No reset/rearm clears a fault. Source command
errors latch the native host off, and source command/TRB construction checks
the native guard before modifying queues.

Async completion uses the actual source event decoder and remap function.
The native poller checks remap/rebuild/doorbell and IO errors before callback,
copies callback data, and never touches a URB or list pointer after callback.
Cancellation only frees after a successful endpoint-stop/dequeue proof, or
after a successfully disabled slot. CallbackDepth prevents callback-triggered
Stop from freeing its own executing context.

PrepareHandoff refuses submissions and freshly proves halt without changing
the memory map. Start publishes it through the shared
DaliUsbNativeHostHandoff protocol. DarwinBoot enumerates every USB2 HC after
closing its input files and before the final GetMemoryMap; it calls this stronger
callback for native hosts and uses standard SetState(Halt) for other published
USB2 controllers. Any missing method or failed halt proof refuses the boot.
The call is outside both the map-buffer allocation loop and the EBS retry loop,
and repeated native callbacks are safe. The EBS event is a permanent last
shutdown latch, not permission to proceed after a failed handoff. This host
owns IO's EBS latch; other events may not close IO or release providers before
its halt.

Resources/MuPatches/Xhci-Native-Safety.patch reproduces checked source
allocation failures, partial scheduler cleanup, bounded configuration-cache
validation, value-based configuration lookup and retained slot cleanup.
Xhci-Native-Short-Event.patch adds the one-event MTK short-transfer quirk to the
native event decoder. Xhci-Native-MTK-Scheduler.patch adds native-only USB2
interrupt endpoint admission for high-speed root-port and full/low-speed TT
bandwidth, with transactional Configure Endpoint/SET_INTERFACE reservations and
slot teardown. High-bandwidth transactions and MTK reserved context fields are
checked. Direct full/low-speed root-port periodic endpoints use BPKTS=1 as in the
MTK reference. Isochronous endpoints remain unsupported by the EDK2 engine;
the Slot Context MTT bit follows the active hub interface protocol and is set only
after successful SET_INTERFACE to protocol 2. `UsbHubMultiTt.c` now selects a
validated high-speed hub alternate setting 1 / interface protocol 2 before the
hub queues its interrupt or powers downstream ports. Missing or invalid alternate
settings keep the default single-TT mode; a failed request or controller-context
update aborts hub initialization. The control-transfer fixture compiles this
actual selector against mocked USB I/O, and the AArch64 source build verifies the
object and exported definition in `UsbBusDxe.lib`; it does not model Configure
Endpoint completion or hub hardware timing. If the device accepts alt 1 but the
following native Configure Endpoint command fails, the native host latches a
fault, refuses further submissions, attempts a quiesce, and retains DMA if halt
cannot be proved. It does not send an automatic SET_INTERFACE rollback through
an uncertain slot context.
Native-only guards also exclude PCI binding code from the engine compilation.
Ordinary PCI source builds retain their own lifecycle.

The production tests compile the real engine with DEBUG assertions and
ASan/UBSan, using modeled Boot Services/MMIO/DMA/ownership. They exercise
installation, 4 KiB/32-bit schedules, reset/halt/run, 13 startup pool failures
and four provider failures, event/install/uninstall failures, failed halt
retention, EBS closure, descriptor bounds/noncontiguous interface and
configuration numbers, actual event-ring async completion, cache/remap,
callback cancellation and failed cancellation/remap. The IO regression adds
12 before/applied EBS positions during checked rearm.
The actual source event decoder also rejects a residual length larger than its
TRB or completed bytes larger than the user's buffer; native mode stops and
proves halt before returning DMA ownership after a malformed completion. The
Dali-native event path records completed short TDs and ignores only the next
unmatched successful event for that same slot/endpoint. A new matched TD
completion or successful configuration change clears the one-event marker; the
ordinary PCI source path does not enable this MTK quirk.
The scheduler fixture executes the actual endpoint initializers and verifies
high-speed transaction counts, per-root-port 6144-byte admission, long-interval
64-microframe saturation, 188-byte TT budgets, the USB2 Y6 split restriction,
single/multi-TT scheduler isolation with explicit active MTT context and reservation
rollback/slot teardown across both context sizes under both sanitizer compilers.
It does not run the hardware Configure Endpoint command path or prove controller
timing.

Run from this isolated firmware tree:

```sh
python tools/dali/test_usb_native_host.py
CC=gcc python tools/dali/test_usb_native_host.py
python tools/dali/test_usb_xhci_io.py
python tools/dali/build_usb_libraries.py --output ../.outputs/usb-libraries-native-host
```

This is a callable engine, fail-closed consumer, library and pre-final-map protocol checkpoint. QEMU
tests exercise DarwinBoot with a one-shot mock native provider and with no
native provider; the latter uses QEMU's standard USB2 host-controller protocol.
Neither run emulates the native Dali controller. It is not a ready MT6991
hardware backend: the worker protocol has no production board provider, so the
worker's Depex remains unsatisfied and it is not dispatched in the current FV.
No qualified board policy/DMA provider or native-path QEMU run exists.
The existing PCI/source USB-storage QEMU gate is separate. These files remain
outside the default FV and hardware_qualified=false.
