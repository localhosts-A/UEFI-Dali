# Live partition layout gate

`DaliUfsInspectScratchLayout` reads the selected LUN through a bounded callback.
The scratch BlockIo publisher calls it through the actual read-only parent
BlockIo before installing a child. The callback checks parent protocols, media
identity, the immutable grant snapshot and host closure before and after each
read. The caller supplies exclusive access; this is a serialized BSP interface.

The reader accepts observed logical blocks from 512 to 65536 bytes and 64-bit
LBAs. It reads two GPT headers and two entry arrays exactly once each. Both
headers need valid CRCs, reciprocal current/alternate LBAs and matching usable
range, disk GUID, entry shape and entry CRC. Arrays must fit outside the usable
range and both headers. Both arrays need valid CRCs and identical entry bytes.
The bounded profile supports at most 1024 entries and 128 KiB of entry data.
Unsupported larger tables fail closed.

Every active partition needs a nonzero unique GUID and a valid usable range.
Overlaps and duplicate GUIDs are refused. The explicit grant target selects
exactly one complete UTF-16 `boot_b` or `expdb` name, with the expected canonical GUID, start and length. GPT
read-only bit 60 and unknown or active Unit Descriptor write protection refuse
the layout. A zero disk GUID is accepted as observed on Android; it supplies no
device identity. Legacy boot_b policy determines the final one-MiB scratch
window followed by a one-MiB guard and keeps the candidate extent before scratch.

The separately selected expdb policy permits only one MiB at partition-relative
offset one MiB. The first MiB and all remaining bytes after the test window are
outside the grant, including at least four MiB at the partition tail. The legacy
`BootStartLba` and `BootBlocks` internal field names describe the selected
partition; `CandidateBytes` must be zero for expdb, since the boot image is not
stored there. Candidate/source SHA bindings remain required independently.
Unknown target values and cross-profile reuse fail closed. This temporary test
policy is explicitly user-requested; it does not identify reserved free space
in the MediaTek expdb format. A complete original backup and verified restoration
are required before the target can be used on hardware.

The output is an observed layout, not a write grant. Candidate/source measurement,
UFS device identity, original-image AVB/zero-scratch validation, a fresh token and
evidence export before restoration remain separate authorization requirements.
The host still has no runtime grant producer and its static grant remains zero.
The new expdb wire format and XNU integration are also pending; changing a LUN
or LBA in an existing boot_b grant cannot select expdb.

Prepublication reads set the child's busy state. A reentrant shutdown closes
admission and returns NOT_READY until the read returns. A shutdown requested
during allocation or protocol installation cannot reopen the child; a published
child is removed or retained closed if removal fails. Failed reads are never
retried. The workspace uses fixed 64-KiB-aligned sub-buffer offsets to preserve
I/O alignment for small logical blocks.

Native tests compile the actual reader and publisher with fixed Mu headers.
Independently encoded media covers block sizes, 64-bit geometry, CRC/reciprocal
corruption, duplicate/overlapping partitions, write protection, proposal mismatch,
bounded buffers, timeouts, warning statuses, media changes and reentrant shutdown.
An offline replay of captured Android GPT bytes is recorded separately from
device execution. These software and build checks do not qualify physical
scratch writes or reboot persistence.
