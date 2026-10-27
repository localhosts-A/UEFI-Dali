// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Library/BaseCryptLib.h>
#include <Library/BaseMemoryLib.h>
#include "UfsProvenance.h"

STATIC CONST UINT8 Magic[16] = "DALIUFSSRC1";
STATIC BOOLEAN SourceValid (CONST UINT8 *Source, UINTN Bytes) {
  if (Source == NULL || Bytes != DALI_UFS_SOURCE_BYTES || CompareMem (Source, Magic, sizeof Magic)) { return FALSE; }
  for (UINTN Pin = 0; Pin < 4; ++Pin) {
    CONST UINT8 *P = Source + sizeof Magic + Pin * 41;
    for (UINTN I = 0; I < 40; ++I) {
      if (!((P[I] >= '0' && P[I] <= '9') || (P[I] >= 'a' && P[I] <= 'f'))) { return FALSE; }
    }
    if (P[40]) { return FALSE; }
  }
  return TRUE;
}
STATIC BOOLEAN Append (UINT8 *Buffer, UINTN *Used, CONST VOID *Data, UINTN Bytes) {
  if (Bytes > 512U - *Used) { return FALSE; }
  CopyMem (Buffer + *Used, Data, Bytes); *Used += Bytes; return TRUE;
}
EFI_STATUS DaliUfsMeasureKernel (CONST VOID *Kernel, UINTN KernelBytes,
  CONST VOID *CompiledSource, UINTN SourceBytes, DALI_UFS_PROVENANCE *Observed) {
  if (Observed == NULL) { return EFI_INVALID_PARAMETER; }
  ZeroMem (Observed, sizeof *Observed);
  if (Kernel == NULL || KernelBytes < 32 || KernelBytes > DALI_UFS_KERNEL_MAX_BYTES ||
      !SourceValid (CompiledSource, SourceBytes)) { return EFI_SECURITY_VIOLATION; }
  CONST UINT8 *K = Kernel, *S = CompiledSource;
  // Native ARM64 MH_EXECUTE, as accepted by this source-only loader profile.
  STATIC CONST UINT8 Header[8] = {0xCF,0xFA,0xED,0xFE,0x0C,0,0,1};
  STATIC CONST UINT8 Execute[4] = {2,0,0,0};
  if (CompareMem (K, Header, sizeof Header) || CompareMem (K + 12, Execute, sizeof Execute)) { return EFI_UNSUPPORTED; }
  UINTN Matches = 0;
  for (UINTN At = 0; At + sizeof Magic <= KernelBytes; ++At) {
    if (CompareMem (K + At, Magic, sizeof Magic)) { continue; }
    // The transport also contains the marker as an ordinary string constant.
    // Match complete well-formed records, exactly as the build artifact gate.
    if (DALI_UFS_SOURCE_BYTES > KernelBytes - At || !SourceValid (K + At, DALI_UFS_SOURCE_BYTES)) { continue; }
    if (++Matches != 1 || CompareMem (K + At, S, DALI_UFS_SOURCE_BYTES)) {
      return EFI_SECURITY_VIOLATION;
    }
  }
  if (Matches != 1) { return EFI_SECURITY_VIOLATION; }
  DALI_UFS_PROVENANCE Result;
  ZeroMem (&Result, sizeof Result);
  if (!Sha256HashAll (Kernel, KernelBytes, Result.CandidateSha256)) { return EFI_DEVICE_ERROR; }
  UINT8 Input[512], Hex[64]; UINTN Used = 0;
  STATIC CONST CHAR8 Digits[] = "0123456789abcdef";
  for (UINTN I = 0; I < 32; ++I) { Hex[I * 2] = Digits[Result.CandidateSha256[I] >> 4]; Hex[I * 2 + 1] = Digits[Result.CandidateSha256[I] & 15]; }
  STATIC CONST CHAR8 Domain[] = "DALI-UFS-SOURCE-V1";
  STATIC CONST CHAR8 Device[] = "{\"device_commit\":\"";
  STATIC CONST CHAR8 KernelKey[] = "\",\"kernel_sha256\":\"";
  STATIC CONST CHAR8 Uefi[] = "\",\"uefi_commit\":\"";
  STATIC CONST CHAR8 Unixnu[] = "\",\"unixnu_commit\":\"";
  STATIC CONST CHAR8 Xnu[] = "\",\"xnu_commit\":\"";
  STATIC CONST CHAR8 End[] = "\"}";
  // The domain's terminating NUL and JSON field order match the host/XNU wire.
  BOOLEAN Valid = Append (Input, &Used, Domain, sizeof Domain) &&
    Append (Input, &Used, Device, sizeof Device - 1) && Append (Input, &Used, S + 16, 40) &&
    Append (Input, &Used, KernelKey, sizeof KernelKey - 1) && Append (Input, &Used, Hex, sizeof Hex) &&
    Append (Input, &Used, Uefi, sizeof Uefi - 1) && Append (Input, &Used, S + 98, 40) &&
    Append (Input, &Used, Unixnu, sizeof Unixnu - 1) && Append (Input, &Used, S + 57, 40) &&
    Append (Input, &Used, Xnu, sizeof Xnu - 1) && Append (Input, &Used, S + 139, 40) &&
    Append (Input, &Used, End, sizeof End - 1);
  if (Valid) { Valid = Sha256HashAll (Input, Used, Result.SourceSha256); }
  ZeroMem (Input, sizeof Input); ZeroMem (Hex, sizeof Hex);
  if (!Valid) { ZeroMem (&Result, sizeof Result); return EFI_DEVICE_ERROR; }
  Result.Ready = TRUE; CopyMem (Observed, &Result, sizeof Result);
  ZeroMem (&Result, sizeof Result); return EFI_SUCCESS;
}
