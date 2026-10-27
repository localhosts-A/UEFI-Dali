# SPDX-License-Identifier: BSD-2-Clause-Patent
[Defines]
  PLATFORM_NAME = dali
  PLATFORM_GUID = 0C3B3BA4-DC12-4DEE-B999-362B64BFAFBA
  PLATFORM_VERSION = 0.1
  DSC_SPECIFICATION = 0x00010005
  OUTPUT_DIRECTORY = Build/daliPkg
  SUPPORTED_ARCHITECTURES = AARCH64
  BUILD_TARGETS = RELEASE|DEBUG
  SKUID_IDENTIFIER = DEFAULT
  FLASH_DEFINITION = daliPkg/dali.fdf
  USE_CUSTOM_DISPLAY_DRIVER = 1
!ifndef DALI_UFS_PROBE
  DEFINE DALI_UFS_PROBE = FALSE
!endif
!ifndef DALI_UFS_PROBE_TOKEN
  DEFINE DALI_UFS_PROBE_TOKEN = "none"
!endif
!ifndef DALI_UFS_READ_ONLY
  DEFINE DALI_UFS_READ_ONLY = FALSE
!endif
!ifndef DALI_UFS_STORAGE_TOKEN
  DEFINE DALI_UFS_STORAGE_TOKEN = "none"
!endif
!ifndef DALI_UFS_MEASURE_PAYLOAD
  DEFINE DALI_UFS_MEASURE_PAYLOAD = FALSE
!endif
!ifndef DALI_UFS_SOURCE_RECORD
  DEFINE DALI_UFS_SOURCE_RECORD = {0x00}
!endif
!ifndef DALI_UFS_SCRATCH_ENABLED
  DEFINE DALI_UFS_SCRATCH_ENABLED = FALSE
!endif
!ifndef DALI_UFS_SCRATCH_WIRE
  DEFINE DALI_UFS_SCRATCH_WIRE = {0x00}
!endif
!ifndef DALI_UFS_LOG_ENABLED
  DEFINE DALI_UFS_LOG_ENABLED = FALSE
!endif

!include MT6991Pkg/MT6991Pkg.dsc.inc

[LibraryClasses]
  UefiScsiLib|MdePkg/Library/UefiScsiLib/UefiScsiLib.inf
  # Keep event waits polling until MT6991 WFI wakeup is validated separately.
  CpuLib|daliPkg/Library/PollingCpuLib/PollingCpuLib.inf
  KeypadDeviceLib|daliPkg/Library/KeypadDeviceLib/KeypadDeviceLib.inf
  UniXNULib|UniXNUPkg/Library/UniXNULib/UniXNULib.inf
  Mt6991UfsDmaLib|MT6991Pkg/Library/UfsDmaLib/UfsDmaLib.inf

[PcdsFixedAtBuild]
  gDaliTokenSpaceGuid.PcdDaliUfsProbeEnabled|$(DALI_UFS_PROBE)
  gDaliTokenSpaceGuid.PcdDaliUfsProbeToken|$(DALI_UFS_PROBE_TOKEN)
  gDaliTokenSpaceGuid.PcdDaliUfsReadOnlyEnabled|$(DALI_UFS_READ_ONLY)
  gDaliTokenSpaceGuid.PcdDaliUfsStorageToken|$(DALI_UFS_STORAGE_TOKEN)
  gDaliTokenSpaceGuid.PcdDaliUfsMeasurePayload|$(DALI_UFS_MEASURE_PAYLOAD)
  gDaliTokenSpaceGuid.PcdDaliUfsSourceRecord|$(DALI_UFS_SOURCE_RECORD)
  gDaliTokenSpaceGuid.PcdDaliUfsScratchEnabled|$(DALI_UFS_SCRATCH_ENABLED)
  gDaliTokenSpaceGuid.PcdDaliUfsScratchWire|$(DALI_UFS_SCRATCH_WIRE)
  gDaliTokenSpaceGuid.PcdDaliUfsLogEnabled|$(DALI_UFS_LOG_ENABLED)
  gUniXNUPkgTokenSpaceGuid.PcdUniXNUDebugLevel|0x2
  gArmTokenSpaceGuid.PcdSystemMemoryBase|0x80000000
  gArmPlatformTokenSpaceGuid.PcdCPUCoresStackBase|0xC5000000
  gArmPlatformTokenSpaceGuid.PcdCPUCorePrimaryStackSize|0x40000
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemManufacturer|"Xiaomi"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemModel|"REDMI K80 Ultra"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailModel|"dali"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailSku|"25060RK16C"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemBoardModel|"dali"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosProcessorModel|"MediaTek Dimensity 9400+"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosProcessorPartNumber|"MT6991"

[Components]
!if $(DALI_UFS_READ_ONLY) == TRUE
  MT6991Pkg/Drivers/UfsHostDxe/UfsHostDxe.inf {
    <LibraryClasses>
      # Media evidence needs real SHA-256 even when Secure Boot is disabled.
      # Scope crypto to this diagnostic; preserve other modules' policy.
      BaseCryptLib|OpensslPkg/Library/BaseCryptLib/BaseCryptLib.inf
      OpensslLib|OpensslPkg/Library/OpensslLib/OpensslLib.inf
      IntrinsicLib|CryptoPkg/Library/IntrinsicLib/IntrinsicLib.inf
      RngLib|SiliciumPkg/Library/DxeRngLib/DxeRngLib.inf
  }
!else
  MT6991Pkg/Drivers/UfsHostDxe/UfsHostDxe.inf
!endif
  MdeModulePkg/Bus/Ufs/UfsPassThruDxe/UfsPassThruDxe.inf
  MdeModulePkg/Bus/Scsi/ScsiBusDxe/ScsiBusDxe.inf
  MdeModulePkg/Bus/Scsi/ScsiDiskDxe/ScsiDiskDxe.inf
  MdeModulePkg/Universal/Disk/PartitionDxe/PartitionDxe.inf
  SecurityPkg/RandomNumberGenerator/RngDxe/RngDxe.inf {
    <LibraryClasses>
      # RNDR is used only when the CPU capability register advertises support;
      # otherwise the provider exposes no algorithm and persistent logging fails closed.
      RngLib|MdePkg/Library/BaseRngLib/BaseRngLib.inf
  }
  daliPkg/Drivers/PayloadDxe/PayloadDxe.inf
  DarwinBootPkg/Driver/DarwinBoot.inf
  UniXNUPkg/Application/UniXNU/UniXNU.inf
  UniXNUPkg/Driver/UniXNUDxe/UniXNUDxe.inf
  daliPkg/Drivers/DisplayDxe/DisplayDxe.inf
  SiliciumPkg/Drivers/KeypadDeviceDxe/KeypadDeviceDxe.inf
  SiliciumPkg/Drivers/KeypadDxe/KeypadDxe.inf
  daliPkg/Drivers/DiagnosticBdsDxe/DiagnosticBdsDxe.inf
