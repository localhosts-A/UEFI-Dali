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
real device sync and the previous diagnostic stop. The subsequent
`run-20261025T134202Z-4d77705d` exercised this provider through BDS with firmware
`cffefda`, device `38ab920` and generic `b54db17` / DarwinBoot `fb0e77f`.
The source-bound 49,877-byte raw console SHA256 is
`f82f14dd814205d8990cfe49c519f1e1315f9c2efbf3caeb5ac822dc1f75fca2`.
It records `closed=1 drained-synced=1 mask=7 commands=3 result=Success`, followed
by successful quiesce with HCE=0, RAM menu and firmware return. LUN-2 known
blocks, all 96 partition paths/GUIDs and ordinary LUN-0/1/2 syncs also passed.

The 96 partition-driver teardown flushes were refused after admission closed;
they are not successful device syncs. The private provider sync completed before
stop/disconnect, without reopening public submission. The original complete
boot_b, Android B/root and all 18 original hashes were restored; an independent
audit rehashed backup bytes, checked candidate/baseline/raw bindings and verified
the cleared pending record and consumed token.

This qualifies the BDS-invoked provider's private sync/stop/DMA cleanup path.
DarwinBoot-to-XNU physical ownership, scratch writes and reboot persistence
remain unqualified. Media writes remain disabled.
