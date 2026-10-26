// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef DALI_UFS_IDENTITY_H
#define DALI_UFS_IDENTITY_H
#include <Uefi.h>
#include <Library/BaseCryptLib.h>

// A boot-local observation, not permission to write. Retain only the digest.
typedef struct {
  BOOLEAN Attempted;
  BOOLEAN Ready;
  UINT8 Digest[SHA256_DIGEST_SIZE];
} DALI_UFS_IDENTITY;

EFI_STATUS DaliUfsIdentityCapture (DALI_UFS_IDENTITY *Identity,
  CONST UINT8 *Device, UINT32 DeviceBytes, CONST UINT8 *Serial, UINT32 SerialBytes);
#endif
