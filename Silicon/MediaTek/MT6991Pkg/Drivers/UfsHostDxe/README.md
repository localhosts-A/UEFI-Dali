# Dali UFS host diagnostic

This is the default-disabled MT6991 host adapter. It connects the platform
control core and private DMA allocator to UfsPassThru, ScsiBus, ScsiDisk and
PartitionDxe. No physical UFS command or DMA qualification has been obtained.

DXE entry only publishes `DALI_UFS_DIAGNOSTIC_PROTOCOL` when
`PcdDaliUfsReadOnlyEnabled` is explicitly enabled. BDS calls Initialize after
preparing the live FDT and RAM payload. The register-only probe is a separate,
mutually exclusive mode. The host requires its own 32-hex storage token and
rejects automatic recovery/reset modes.

The platform protocol explicitly selects the repository's v4 command-policy
extension. Existing providers retain version 3. HCE/link startup are performed
by the MT6991 control core, and the generic driver skips repeating them. The
device reference-clock attribute is preserved. One transfer slot is exposed;
async requests and zero/unbounded timeouts are refused before allocation.

SCSI permits identification, mode sense, capacity, reads and synchronous cache
commands on common LUNs 0–7. Other opcodes, well-known LUNs and data-out requests
are refused. Query permits reads and the one initialization-only fDeviceInit
set. Provisioning, protection changes, attributes, descriptor writes and RPMB
are refused. Mode sense reports the virtual policy's WP bit without modifying
the raw UFS UnitDescriptor.

The hardware doorbell path independently validates owned TRD/UCD/response/PRDT
spans, direction, UPIU, opcode, actual learned capacity/block size, and one
contiguous device-write mapping. Only slot 0 can be rung. There is no task/UIC/
MCQ submission path through the host protocol. No scratch write authorization
exists in this implementation.

DMA pages use EFI_MEMORY_WC because this pinned AArch64 CpuDxe maps it to
Normal Non-cacheable (`ArmPkg/Drivers/CpuDxe/AArch64/Mmu.c`). EFI_MEMORY_UC maps
to Device memory, which cannot safely support the generic driver's packed
descriptor and memcpy/memset accesses. WC capability is mandatory; there is no
Device-memory fallback or assumed coherency. Full original attributes are
restored on release; the direct initial window remains below 4 GiB within the
FDT's declared zero-offset range. No IOMMU or firewall bypass is programmed.

Responses require completed DBR, barriers, OCS, transaction/tag/LUN and bounded
residual/sense fields before data can be copied back. Query device error codes
are returned by PassThru, without a v4 retry. An uncertain request closes and
poisons the host, then confirms HCE=0 before reclaiming any mapping. Stop
failure retains the allocations. Cleanup failures are reported.

For this diagnostic, BDS always quiesces before entering the RAM menu: stop the
host, disconnect the upper drivers, and release residual mappings. Any failed
step refuses the menu/XNU handoff. This is not yet the optional generic
DarwinBoot pre-final-memory-map interface or the XNU ownership handoff.

119 host tests pass, including 14 production DMA/backend tests, 8 production
wire-policy tests and 10 tests executing the production PassThru function
bodies under faults. AArch64 default-disabled and explicitly enabled builds
have compiled. Software results do not qualify MT6991 hardware.

The existing off/register-probe packager verifies the compiled disabled host
inside the actual FD and refuses enabled physical-host builds. Direct
`build_uefi.py` supports explicit host builds for software qualification; a
dedicated sealed candidate profile and runner are still required for hardware.

Remaining work includes physical BlockIo publication/geometry/read evidence,
real ScsiDisk FlushBlocks synchronization (the inherited implementation is a
no-op), scratch child/policy, generic firmware/XNU handoff, physical XNU driver
and repeated write/sync/reboot/readback qualification.
