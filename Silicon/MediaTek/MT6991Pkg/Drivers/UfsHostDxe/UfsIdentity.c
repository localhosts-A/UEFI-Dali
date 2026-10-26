// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "UfsIdentity.h"
#include <Library/BaseMemoryLib.h>

EFI_STATUS DaliUfsIdentityCapture (DALI_UFS_IDENTITY *Identity,
  CONST UINT8 *Device, UINT32 DeviceBytes, CONST UINT8 *Serial, UINT32 SerialBytes) {
  if (Identity == NULL) { return EFI_INVALID_PARAMETER; }
  Identity->Ready = FALSE;
  ZeroMem (Identity->Digest, sizeof Identity->Digest);
  if (Identity->Attempted) { return EFI_ALREADY_STARTED; }
  Identity->Attempted = TRUE;
  // Device Descriptor may be a prefix of a newer, longer descriptor. Only the
  // actual prefix containing SerialName and ManufacturerId is required here.
  if (Device == NULL || Serial == NULL || DeviceBytes < 26 || DeviceBytes > 255 ||
      Device[0] < 26 || Device[1] != 0 || Device[22] == 0 || Device[22] == 0xFF ||
      SerialBytes < 4 || SerialBytes > 254) { return EFI_COMPROMISED_DATA; }
  UINT32 Length = Serial[0];
  if (Length < 4 || Length > SerialBytes || (Length & 1) || Serial[1] != 5) { return EFI_COMPROMISED_DATA; }
  UINT8 Nonzero = 0;
  for (UINT32 Index = 2; Index < Length; ++Index) { Nonzero |= Serial[Index]; }
  if (Nonzero == 0) { return EFI_SECURITY_VIOLATION; }

  // Same input as source-only XNU: two big-endian ManufacturerId bytes followed
  // by the complete raw string descriptor, including its length/type header.
  UINT8 Binding[256];
  Binding[0] = Device[24]; Binding[1] = Device[25];
  CopyMem (Binding + 2, Serial, Length);
  BOOLEAN Hashed = Sha256HashAll (Binding, Length + 2, Identity->Digest);
  ZeroMem (Binding, sizeof Binding);
  if (!Hashed) {
    ZeroMem (Identity->Digest, sizeof Identity->Digest);
    return EFI_DEVICE_ERROR;
  }
  Identity->Ready = TRUE;
  return EFI_SUCCESS;
}
