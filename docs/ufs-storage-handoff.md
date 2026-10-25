# Dali storage quiesce provider

The opt-in physical read-only host publishes the versioned
`UNIXNU_STORAGE_QUIESCE_PROTOCOL` from UniXNUPkg, alongside its diagnostic
interface. DarwinBoot calls it after closing boot files and before obtaining
the final memory map. BDS uses the same operation before the existing RAM menu.
Only exact `EFI_SUCCESS` permits handoff. The interface remains present after
an initialization failure so that an unsafe partial takeover cannot be hidden
by a missing provider. It is absent when physical UFS is disabled.

The provider closes public submission and DMA allocation/mapping admission,
then drains slot zero with a three-second deadline and validates its response
before completing or retiring a data mapping. It stops idle legacy queues,
retains their common buffers and creates one private owned common page for
ordered, no-data SYNCHRONIZE CACHE(10) commands. Each LUN with learned valid
geometry must complete a real synchronous command; the provider never assumes
LUN zero or a 4096-byte logical block. Public PassThru is never reopened for
these commands. Failed or uncertain submissions are never replayed.

After drain/sync, including failures, cleanup attempts controller stop and
checks HCE=0 before allowing DMA release. It disconnects PassThru and confirms
the protocol is absent before releasing residual common pages. A failed stop,
uncertain disconnect, still-present protocol or failed release retains the
relevant ownership records. The first failure remains sticky even if later
cleanup succeeds. Repeated calls cannot repeat the sync attempt; repeated
successful calls do no work. XNU receives no firmware queue or buffer pointer.

This boot-service interface runs on the firmware BSP at TPL_APPLICATION.
Closure, drain/stop and public submission/DMA callbacks share TPL_NOTIFY
serialization; disconnect runs at the caller's application TPL and may call
Free/Unmap for cleanup. Reentrant quiesce is refused. AP/MP callers and a first
call at another TPL are unsupported. ExitBootServices retries consume the
already completed result without reopening storage.

`test_ufs_shutdown.py` compiles the production shutdown, wire-policy and DMA
cores with simulated hardware and independent CPU/device addresses. It checks
real descriptors, complete CDB bytes, validated read copyback, partial sync,
timeouts, uncertain doorbells, quarantine, geometry and no replay.
`test_ufs_quiesce_adapter.py` executes extracted production provider functions
with a stub shutdown engine and UEFI services to verify the surrounding close,
TPL, HCE, disconnect, release, sticky-error and reentry rules. These are separate
software checks, not an end-to-end hardware qualification.

The earlier `ea82d126` hardware run qualified physical UEFI named-LUN read-only,
real device sync and the previous diagnostic stop. It did not exercise this new
private shutdown command path. A new source-bound candidate and raw evidence
are required for that path; XNU physical ownership, scratch writes and reboot
persistence remain unqualified. Media writes remain disabled.
