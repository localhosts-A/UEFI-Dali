[Defines]
  # Use the architected virtual counter; do not inherit old MTK peripheral drivers.
  USE_PHYSICAL_TIMER = 0
!include SiliciumPkg/SiliciumPkg.dsc.inc

[BuildOptions]
  *_*_AARCH64_CC_FLAGS = -march=armv8-a

[PcdsFixedAtBuild]
  gArmPlatformTokenSpaceGuid.PcdCoreCount|1
  gArmPlatformTokenSpaceGuid.PcdClusterCount|1
  gArmTokenSpaceGuid.PcdGicDistributorBase|0x0C400000
  gArmTokenSpaceGuid.PcdGicRedistributorsBase|0x0C440000
  gArmTokenSpaceGuid.PcdGicInterruptInterfaceBase|0
  gArmTokenSpaceGuid.PcdArmArchTimerVirtIntrNum|27
  gArmTokenSpaceGuid.PcdArmArchTimerSecIntrNum|29
  gArmTokenSpaceGuid.PcdArmArchTimerIntrNum|30
  gArmTokenSpaceGuid.PcdArmArchTimerHypIntrNum|26

[LibraryClasses]
  Mt6991PlatformLib|MT6991Pkg/Library/PlatformLib/PlatformLib.inf
  MemoryMapLib|MT6991Pkg/Library/MemoryMapLib/MemoryMapLib.inf
  SerialPortLib|MT6991Pkg/Library/RamoopsSerialPortLib/RamoopsSerialPortLib.inf

[Components]
  MT6991Pkg/Sec/Sec.inf {
    <LibraryClasses>
      NULL|MdeModulePkg/Library/LzmaCustomDecompressLib/LzmaCustomDecompressLib.inf
  }
