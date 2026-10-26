// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Library/BaseMemoryLib.h>
#include "UfsGrant.h"

STATIC UINT32 Le32 (CONST UINT8 *P) {
  return (UINT32)P[0] | (UINT32)P[1] << 8 | (UINT32)P[2] << 16 | (UINT32)P[3] << 24;
}
STATIC UINT64 Le64 (CONST UINT8 *P) { return Le32 (P) | (UINT64)Le32 (P + 4) << 32; }

EFI_STATUS DaliUfsGrantPrepare (DALI_UFS_GRANT_STATE *S,
  CONST VOID *Wire, UINTN Bytes, CONST CHAR8 *Token, UINTN TokenBytes,
  CONST DALI_UFS_PROVENANCE *Provenance) {
  if (S == NULL) { return EFI_INVALID_PARAMETER; }
  if (S->Attempted) { S->Prepared = FALSE; return EFI_ALREADY_STARTED; }
  ZeroMem (S, sizeof *S); S->Attempted = TRUE;
  CONST UINT8 *P = Wire;
  if (P == NULL || (Bytes != DALI_UFS_EXPDB_WIRE_BYTES && Bytes != DALI_UFS_USERDATA_LOG_WIRE_BYTES) ||
      Token == NULL || TokenBytes != 33 || Token[32] != 0 || Provenance == NULL || !Provenance->Ready ||
      CompareMem (P, "DALIUFSW", 8) || CompareMem (P + 16, Token, 32)) {
    return EFI_SECURITY_VIOLATION;
  }
  MT6991_UFS_SCRATCH_GRANT G;
  UINT32 Target = Bytes == DALI_UFS_EXPDB_WIRE_BYTES ? MT6991_UFS_TARGET_EXPDB : MT6991_UFS_TARGET_USERDATA_LOGS;
  if ((Target == MT6991_UFS_TARGET_EXPDB && (Le32 (P + 8) != 3 || Le32 (P + 12) != 2 ||
       Le32 (P + 232) != MT6991_UFS_TARGET_EXPDB)) ||
      (Target == MT6991_UFS_TARGET_USERDATA_LOGS && (Le32 (P + 8) != 4 || Le32 (P + 12) != 4 ||
       Le32 (P + 232) != MT6991_UFS_TARGET_USERDATA_LOGS ||
       Le32 (P + 236) != MT6991_UFS_DATA_LOG_SLOT_BYTES))) { return EFI_SECURITY_VIOLATION; }
  ZeroMem (&G, sizeof G); G.Enabled = TRUE; G.Target = Target;
  CopyMem (G.Token, P + 16, 32); CopyMem (G.DeviceBinding, P + 48, 32);
  CopyMem (G.CandidateSha256, P + 80, 32); CopyMem (G.SourceSha256, P + 112, 32);
  CopyMem (G.PartitionGuid, P + 144, 16);
  UINT32 Lun = Le32 (P + 160);
  if (Lun >= 8) { return EFI_SECURITY_VIOLATION; }
  G.Lun = (UINT8)Lun; G.BlockSize = Le32 (P + 164); G.CapacityBlocks = Le64 (P + 168);
  G.BootStartLba = Le64 (P + 176); G.BootBlocks = Le64 (P + 184); G.CandidateBytes = Le64 (P + 192);
  G.ScratchStartLba = Le64 (P + 200); G.ScratchBlocks = Le64 (P + 208); G.TailGuardBlocks = Le64 (P + 216);
  UINT64 Sequence = Le64 (P + 224);
  G.Sequence = Sequence;
  if (!Mt6991UfsScratchGrantValid (&G) || Sequence == 0 || Sequence > G.ScratchBlocks / 2 ||
      CompareMem (G.CandidateSha256, Provenance->CandidateSha256, 32) ||
      CompareMem (G.SourceSha256, Provenance->SourceSha256, 32)) { return EFI_SECURITY_VIOLATION; }
  S->Proposal = G; S->Sequence = Sequence; S->Provenance = *Provenance;
  S->Prepared = TRUE; return EFI_SUCCESS;
}

EFI_STATUS DaliUfsGrantAuthorize (DALI_UFS_GRANT_STATE *S,
  CONST DALI_UFS_IDENTITY *Identity, CONST DALI_UFS_PROVENANCE *Provenance,
  CONST MT6991_UFS_GEOMETRY *Geometry, CONST DALI_UFS_LAYOUT *L,
  BOOLEAN Closed, MT6991_UFS_SCRATCH_GRANT *Live) {
  if (Live == NULL) { return EFI_INVALID_PARAMETER; }
  ZeroMem (Live, sizeof *Live);
  if (S == NULL || !S->Attempted || !S->Prepared || S->AuthorizationAttempted) { return EFI_ACCESS_DENIED; }
  S->AuthorizationAttempted = TRUE;
  CONST MT6991_UFS_SCRATCH_GRANT *G = &S->Proposal;
  if (Closed || !Mt6991UfsScratchGrantValid (G) || Identity == NULL || !Identity->Attempted || !Identity->Ready ||
      Provenance == NULL || !Provenance->Ready || CompareMem (Provenance, &S->Provenance, sizeof *Provenance) ||
      CompareMem (Identity->Digest, G->DeviceBinding, 32) || Geometry == NULL || L == NULL || !L->Verified ||
      L->Target != G->Target || L->Lun != G->Lun ||
      L->BlockSize != G->BlockSize || L->CapacityBlocks != G->CapacityBlocks ||
      L->BootStartLba != G->BootStartLba || L->BootBlocks != G->BootBlocks ||
      L->ScratchStartLba != G->ScratchStartLba || L->ScratchBlocks != G->ScratchBlocks ||
      L->TailGuardBlocks != G->TailGuardBlocks || CompareMem (L->PartitionGuid, G->PartitionGuid, 16)) {
    return EFI_SECURITY_VIOLATION;
  }
  UINT64 Absolute, Count;
  EFI_STATUS Status = Mt6991UfsScratchMapRange (Geometry, G, G->Lun, 0, G->BlockSize, TRUE, &Absolute, &Count);
  if (Status != EFI_SUCCESS || Absolute != G->ScratchStartLba || Count != 1) { return EFI_SECURITY_VIOLATION; }
  *Live = *G; return EFI_SUCCESS;
}
