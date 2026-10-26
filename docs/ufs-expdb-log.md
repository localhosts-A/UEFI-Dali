# Bounded expdb diagnostic log

The requested rollout keeps ramoops while adding expdb logs. Only after actual
write, device sync and same-candidate reboot/readback have passed should the
automatic-fastboot path be connected again and ramoops logging disabled.
QR export is cancelled. Current recovery continues to collect ramoops after
restoring original boot_b and starting Android.

`UfsLog.c` is a consumer of an already runtime-authorized expdb scratch BlockIo.
It does not issue a grant or select an arbitrary partition. It requires the
explicit expdb target and exact one-MiB child geometry. The 1 MiB head and all
bytes following the window remain outside the child and lower SCSI write gate.
The optional log protocol now calls the core through this driver's own published
child and exact live grant. It freezes the raw console ring in a bounded copy,
restores application TPL before allocations or I/O, and keeps body/readback in
separate buffers. Ramoops is unchanged. This integration is default-off and has
not yet been qualified on physical hardware.

The window contains an attempt record at relative block 0, a header at block 1,
body bytes starting at block 2, zero padding and a commit record in the final
block. Each record fills an actual logical block and includes its own SHA-256.
All fields use explicit little-endian encoding; no native C struct is written.

| Byte offset | Field |
| --- | --- |
| 0 | 16-byte `DALIUFSLOG1` magic with zero padding |
| 16, 20 | UINT32 kind (attempt 1/header 2/commit 3), version 1 |
| 24 | 32 lowercase hex token bytes |
| 56, 60 | UINT32 owner (UEFI 1/XNU 2), UINT64 sequence |
| 68, 72, 76 | UINT32 body length, UINT32 block bytes, UINT64 window blocks |
| 84, 116, 148 | 32-byte device, whole-kernel and source digests |
| 180, 212 | Body SHA-256 and record SHA-256 |
| 244 through block end | Zero padding |

The record hash covers the whole logical block with its own digest field zero.
The body hash covers exactly the declared raw body bytes. The host decoder
reconstructs all three records against expected identity/geometry fields and
requires every remaining padding byte to be zero. These hashes establish byte
integrity and consistency, not device authentication or successful persistence.

Write mode checks the entire original window is zero, writes attempt and
synchronizes, writes header/body/padding and synchronizes, writes commit and
synchronizes, then reads and compares the complete window. Every call is single
submission; a warning, timeout, partial write, failed hash or failed sync stops
the attempt. The result object is single-use. An existing nonzero or incomplete
window is never overwritten. Readback-only mode makes no write or sync calls
and cannot repair or replay an incomplete attempt.

A valid commit read after a failed synchronization only proves the bytes now
read are consistent. The first-boot error must remain a failure; a successful
later read cannot rewrite that history. Qualification needs the runtime command
and sync statuses plus a genuine new boot with the same sealed candidate/token.

The native harness uses production grant policy and record code for 512, 4096
and 65536-byte blocks. It injects each write/sync/read/hash failure, torn records,
corrupt payload/padding/commit, wrong binding and an already-used window. A
493380-byte historical raw console was replayed through native mock write/sync,
fresh-instance readback and independent Python decoding without filtering or
changing its bytes. Source/host results are separate from hardware qualification.

The same candidate offers an explicit UEFI log menu before host quiesce:
Volume Up selects WRITE, Volume Down selects READBACK, Power confirms. The
initial selection is READBACK; a 30-second timeout skips without submitting a
log request. Display or protocol errors cannot submit an unseen choice. The
optional protocol has a separate GUID/revision so the old diagnostic ABI is
unchanged. Each boot permits only one log attempt.

READBACK irreversibly inhibits writes at BlockIo, SCSI admission and the owned
descriptor doorbell while retaining reads and shutdown sync. It cannot switch
back to WRITE in the same boot. Any later grant revocation rejects subsequent
I/O. No selected mode, storage failure or partial log automatically retries or
initializes an existing window.

Compile opt-in requires `DALI_UFS_LOG_ENABLED=true`, a regular exact236-byte
`DALI_UFS_LOG_WIRE`, `DALI_UFS_PROFILE=expdb-log-diagnostic`, payload measurement,
the matching token and manual recovery. The builder checks the explicit v3
target/shape; runtime provenance, UFS identity, live geometry/WP and reciprocal
GPT inspection still issue the actual write grant. Ordinary read-only packaging
continues to reject scratch-enabled builds. The dedicated write-trial packager
and runner are still required before a physical experiment.

Remaining integration: XNU use of this record format, durable expdb restoration
and Android collection/decoding. Trial restoration must not clear pending state
until both boot_b and the expdb test window are verified.
