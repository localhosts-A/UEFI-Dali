# Native USB2 host engine checkpoint

The optional DaliUsbNativeHostLib owns the actual pinned Mu xHCI transfer,
register, schedule and memory engine through the private DaliUsbXhciIoLib ABI.
It publishes EFI_USB2_HC_PROTOCOL and a caller-owned device path on a new
handle. It does not publish PCI I/O or call the source PCI DriverBinding,
Start/Stop, reset, cancellation or timer lifecycle.

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
the memory map. The platform loader must call it before the final
GetMemoryMap and refuse a failed proof. Its repeated call is safe. The EBS
event is a permanent last shutdown latch, not permission to proceed after a
failed handoff. This host owns IO's EBS latch; other events may not close IO
or release providers before its halt.

Resources/MuPatches/Xhci-Native-Safety.patch reproduces checked source
allocation failures, partial scheduler cleanup, bounded configuration-cache
validation, value-based configuration lookup and retained slot cleanup.
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
proves halt before returning DMA ownership after a malformed completion.

Run from this isolated firmware tree:

```sh
python tools/dali/test_usb_native_host.py
CC=gcc python tools/dali/test_usb_native_host.py
python tools/dali/test_usb_xhci_io.py
python tools/dali/build_usb_libraries.py --output ../.outputs/usb-libraries-native-host
```

This is a callable engine and library checkpoint. No Dali production worker
currently supplies its qualified board policy/DMA provider or calls Start.
The MTK USB2 interrupt/TT bandwidth scheduler and short-event quirk still
need a native binding; this generic engine must not be treated as a ready
MT6991 hardware backend. Native-path QEMU and DarwinBoot pre-final-map wiring
remain required. The existing PCI/source QEMU gate is separate. These files
remain outside the default FV and hardware_qualified=false.
