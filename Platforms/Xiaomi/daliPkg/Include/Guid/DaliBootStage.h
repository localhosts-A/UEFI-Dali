// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Uefi.h>

#define DALI_BOOT_STAGE_FILE_GUID \
  { 0x2a20dc84, 0x3c0b, 0x4750, { 0x95, 0xf9, 0x43, 0x14, 0x36, 0xd6, 0xad, 0x82 } }
extern EFI_GUID gDaliBootStageFileGuid;

#define DALI_BOOT_STAGE_DESCRIPTOR_MAGIC "DALIBOOT"
#define DALI_BOOT_STAGE_DESCRIPTOR_VERSION 1
#define DALI_BOOT_STAGE_DESCRIPTOR_BYTES 168

#pragma pack(1)
typedef struct {
  UINT8  Magic[8];
  UINT32 Version;
  UINT32 DescriptorBytes;
  UINT64 RootImageBytes;
  UINT64 BootOffsetBytes;
  UINT64 BootBytes;
  UINT64 PartitionBytes;
  UINT64 FirstLba;
  UINT64 LastLba;
  UINT32 Lun;
  UINT32 BlockSize;
  UINT8  RootManifestSha256[32];
  UINT8  BootSha256[32];
  UINT8  DescriptorSha256[32];
} DALI_BOOT_STAGE_DESCRIPTOR;
#pragma pack()

STATIC_ASSERT (sizeof (DALI_BOOT_STAGE_DESCRIPTOR) == DALI_BOOT_STAGE_DESCRIPTOR_BYTES,
  "Dali Boot stage descriptor ABI changed");
