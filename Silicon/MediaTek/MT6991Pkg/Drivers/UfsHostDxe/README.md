# Dali UFS host diagnostic

This is the default-disabled MT6991 host adapter. It connects the platform
control core and private DMA allocator to UfsPassThru, ScsiBus, ScsiDisk and
PartitionDxe. The named baseline LUN 2's UEFI reads and diagnostic sync/stop
are qualified; XNU ownership, media writes and persistence remain pending.

DXE entry only publishes `DALI_UFS_DIAGNOSTIC_PROTOCOL` when
`PcdDaliUfsReadOnlyEnabled` is explicitly enabled. BDS calls Initialize after
preparing the live FDT and RAM payload. The register-only probe is a separate,
mutually exclusive mode. The host requires its own 32-hex storage token and
rejects automatic recovery/reset modes.

The platform protocol explicitly selects the repository's v5 identity extension,
including the v4 command policy. Existing providers retain versions 3/4. HCE/link startup are performed
by the MT6991 control core, and the generic driver skips repeating them. The
device reference-clock attribute is preserved. One transfer slot is exposed;
async submissions and zero timeouts are refused before allocation. Positive
SCSI timeouts are capped at three seconds, including ScsiDisk's 30-second
default. Direct PassThru ignores Event when NONBLOCKIO is not advertised, as
the protocol requires, and then executes synchronously.

SCSI permits identification, mode sense, capacity, reads and synchronous cache
commands on common LUNs 0–7. Other opcodes, well-known LUNs and data-out requests
are refused. Query permits reads and the one initialization-only fDeviceInit
set. Provisioning, protection changes, attributes, descriptor writes and RPMB
are refused. Mode sense reports the virtual policy's WP bit without modifying
the raw UFS UnitDescriptor.

The hardware doorbell path independently validates owned TRD/UCD/response/PRDT
spans, direction, UPIU, opcode, actual learned capacity/block size, and one
contiguous device-write mapping. Only slot 0 can be rung. There is no task/UIC/
MCQ submission path through the host protocol. Scratch authorization defaults
off and is restricted to the explicitly selected window at both write gates.

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

ScsiDisk obtains media policy from its actual ExtSCSI parent before publishing
BlockIo. A failed v4 policy prevents publication; MODE SENSE failure cannot
weaken the forced read-only flag. PartitionDxe inherits that media policy.
For this parent only, FlushBlocks and FlushBlocksEx issue one bounded blocking
SYNCHRONIZE CACHE(10), with IMMED clear and no data. Transport, host and target
errors propagate; an event token carries the actual result. The legacy path
is unchanged.

The diagnostic enumerates only this host's BlockIo paths, asserts read-only
whole disks and partitions, and logs observed LUNs, geometry and GPT partition
paths. It hashes blocks 0, 1 and LastBlock on each root using the actual logical
block size. The captured Android LUN-2 geometry additionally selects the boot_b
first block; its content must match the current candidate, since boot_b now
contains that candidate. Media identity is checked around reads and sync.
Each successful Flush must advance the host's validated device-SYNC completion
counter by exactly one; a no-op Flush cannot qualify synchronization.

163 host tests pass, including 26 production controller, 14 DMA/backend, 9 wire-policy,
12 production PassThru, 8 ScsiDisk media/sync, 8 BlockIo diagnostic and 4 host
sync-completion tests and 5 physical-profile packaging tests. An explicitly
enabled AArch64 build has compiled these changes. Software results do not
qualify MT6991 hardware.

The latest completed hardware trial (b67c24a6, firmware 67118fc) stopped before
DMA: early acknowledgement of startup IS=4 was followed by IS=404/HCS=f with
the same isolated PA LINERESET code. Original Android B/root/all 18 hashes were
restored. The current controller defers sampling/ACK of that latched indication
until actual LINK_STARTUP completion, then checks all layer errors and command
result, acknowledges 404 once and requires clean IS. Its ordinary completion
also rejects unrelated status arriving before ACK. The unchanged 500 ms bound,
device-presence check, fatal handling and no-replay policy remain mandatory.
This new control policy still requires a fresh-token hardware trial.

The subsequent e2e5c78/49a0b99b trial showed text/color bars without the menu.
The 120-second observation expired; original-only recovery then restored the
full original boot_b, Android B/root and all 18 hashes. No raw console survived
the forced restart, so control/DMA/media progress remains unknown. Visible
breadcrumbs now identify control commands and the DMA/connect/BlockIo/stop
boundaries without adding MMIO reads, commands or completion-loop output.
They do not grant hardware qualification.

The 89524c9/db963732 photo shows successful controller/DMA/connect return values,
then entry into BlockIo diagnostics without return. Original-only recovery and
Android B/root/all 18 backup-byte/hash checks passed; raw console is unavailable.
The exact linked SHA provider was BaseCryptLibNull, which ASSERTs and deadloops
in this DEBUG build. The current component override selects real BaseCryptLib
and its dependencies only for this host. Before control writes, a SHA-256 abc
known-answer check must pass; packaging checks actual host link inputs and
rejects missing, null, duplicate or mixed providers. This repairs a definite
software defect; actual read/sync qualification still requires new evidence.

Hardware run ea82d126 (firmware 1746ed7/device 9067ecc/generic 2ab7dfd) supplied
that evidence: 3 read-only roots, 96 GPT partition paths, 10 samples and one
validated device SYNC(10) each on LUNs 0/1/2, then HCE=0. LUN 2 geometry/GPT
and known header/current-candidate block hashes match the fresh Android
baseline. The host scorer initially required task tag 0; bounded UINT8 tags
92/96/101 are valid command identifiers, independently of the hardware slot.
The corrected parser passes the same hash-verified raw trace, retaining the
original failure report. Complete original Android B/root/all 18 hashes and
backup bytes were independently verified after restoration. This qualifies
only the explicit UEFI read-only/sync/diagnostic-stop path.

The existing off/register-probe packager verifies the compiled disabled host
inside the actual FD and refuses enabled physical-host builds. The separate
`DALI_UFS_PROFILE=read-only-blockio` profile requires `DALI_UFS_READ_ONLY=true`,
`DALI_UFS_PROBE=false`, a fresh `DALI_RUN_TOKEN`, `DALI_AUTO_FASTBOOT=false`, no
reset method, and the three UEFI-only build arguments. Its packager checks both
BDS/host PCD consumers, the storage token and all six ARM64 modules against the
actual FD. It grants no media-write permission or hardware qualification.

The device runner's explicit `--mode read-only-blockio --baseline-dir ...`
validates that candidate and snapshots its baseline. Before flashing it collects
a fresh Android read-only baseline, compares raw geometry/GPT, and checks the
original partitions. It retains the shared device lease, fresh-token ledger and
original-only recovery entry. Physical qualification still requires a real run.

The isolated scratch publisher now consumes an already authenticated internal
grant and exposes a separate one-MiB BlockIo view with relative LBAs. It matches
one exact root UFS path, keeps ordinary disks/partitions read-only and submits
through that root's synchronous ScsiIo and the existing lower SCSI/doorbell
guards. Flush sends real SYNC(10) and requires one validated completion. Failed
submitted requests are sticky and never retried. It supports serialized BSP
application calls only; it rechecks live parent protocols, grant and media before
access, and rejects reset/rearm. Child admission closes before host shutdown; an
in-flight call prevents closing its completion path or disconnecting its parent.
Failed child uninstall retains its closed protocols and allocations.

The complete production scratch/range code is tested with real Mu protocol
headers, including 512-byte blocks, 64-bit LBAs, boundaries, write timeout/partial
completion, missing/extra sync counts, stale parents, in-flight close and failed
uninstall. The firmware suite currently has 213 passing tests. The pinned Mu
sync UFS implementation leaves zero-initialized HostAdapterStatus unchanged on
success; TargetStatus and the lower command result must explicitly succeed.

The default-off runtime producer now parses only expdb v3 (236-byte) proposals.
Before controller access, `UfsGrant` compares the compiled token and independent
whole-RAM-kernel/source measurements. The proposal is separate from the live
host grant, which remains zero throughout real GPT reads. After reciprocal GPT
inspection, the checked publisher invokes the authorizer; it compares fresh
device identity, unchanged provenance, geometry/WP and every observed layout
field before populating `mScratchGrant`. A missing authorizer is refused; there
is no legacy publication path that accepts an enabled grant without one.

The publisher then follows the actual live grant pointer, so revocation is
visible to every request. Both stop and quiesce clear that grant, including when
an in-flight request delays shutdown. A duplicate prepare or authorization cannot
rearm it. The actual lower SCSI and owned-doorbell checks still enforce ranges.

`PcdDaliUfsScratchEnabled` remains FALSE and its wire defaults to one zero byte.
There is no public write-build profile/runner or physical scratch qualification.
The current RO packager requires exactly one explicit compiled zero for scratch
enable; it refuses missing, duplicate, enabled or malformed definitions.
Archived candidates retain their original packager and sealed evidence.
Remaining work includes host build inputs, recovery-before-restore evidence
export, new source-only kernel/candidate verification, and separate repeated
UEFI/XNU physical write/sync/reboot/readback qualification. No export capability
is implied by an internal PCD or a successful native authorization fixture.

The isolated v5 path now captures the controller's identity during PassThru
startup, before ExtSCSI/BlockIo publication. It validates the actually returned
Device Descriptor prefix (at least 26 bytes, possibly shorter than its declared
length) and reads its named serial String Descriptor exactly once through the
bounded query path. It hashes the two raw big-endian ManufacturerId bytes plus
the complete serial descriptor including its header, matching source-only XNU.
Only the digest is retained/logged; malformed/short responses, an empty serial,
hash failure or duplicate capture cannot produce a ready identity. Connect must
report exact success and a ready observation before diagnostics proceed. Legacy
providers receive no additional query or callback.

The 223-test firmware suite includes 10 native identity tests executing the
complete production capture function and extracted production Mu query helper
and Dali callback with real headers and an independent Python hash backend.
Full DriverBindingStart/Initialize execution remains outside that harness.
Both explicit-readonly and default-off ARM64 builds pass; actual final EFI
identity/hash calls and the off entry have been inspected. This observation is
not a runtime grant issuer, candidate/source authentication or physical identity
qualification. The static scratch grant remains zero and all writes stay closed.
