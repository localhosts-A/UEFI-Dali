# SPDX-License-Identifier: BSD-2-Clause-Patent
# Offline production-library build only; no FV, application or hardware action.
[Defines]
  PLATFORM_NAME = DaliUsbLibraries
  PLATFORM_GUID = EA7F5AA4-13AD-4285-BE29-1E7FC3A125D8
  PLATFORM_VERSION = 0.1
  DSC_SPECIFICATION = 0x00010019
  OUTPUT_DIRECTORY = Build/DaliUsbLibraries
  SUPPORTED_ARCHITECTURES = AARCH64
  BUILD_TARGETS = DEBUG|RELEASE
  SKUID_IDENTIFIER = DEFAULT

!include MdePkg/MdeLibs.dsc.inc
[LibraryClasses]
  UsbCoreLib|UsbCore/UsbCoreLib.inf
  DaliUsbCdcLogLib|DaliUsbGadgetLog/DaliUsbCdcLogLib.inf
  DaliUsbLib|DaliUsb/DaliUsbLib.inf
  DaliUsbPmicLib|daliPkg/Library/UsbPmicLib/UsbPmicLib.inf
  DaliUsbPlatformLib|daliPkg/Library/UsbPlatformLib/UsbPlatformLib.inf
  DaliUsbI2cLib|daliPkg/Library/UsbI2cLib/UsbI2cLib.inf
  DaliUsbXhciIoLib|daliPkg/Library/UsbXhciIoLib/UsbXhciIoLib.inf
  DaliUsbNativeHostLib|daliPkg/Library/UsbNativeHostLib/UsbNativeHostLib.inf
  BaseLib|MdePkg/Library/BaseLib/BaseLib.inf
  BaseMemoryLib|MdePkg/Library/BaseMemoryLib/BaseMemoryLib.inf
  DebugLib|MdePkg/Library/BaseDebugLibNull/BaseDebugLibNull.inf
  PcdLib|MdePkg/Library/BasePcdLibNull/BasePcdLibNull.inf
  FdtLib|MdePkg/Library/BaseFdtLib/BaseFdtLib.inf
  IoLib|MdePkg/Library/BaseIoLibIntrinsic/BaseIoLibIntrinsic.inf
  TimerLib|ArmPkg/Library/ArmArchTimerLib/ArmArchTimerLib.inf
  ArmGenericTimerCounterLib|ArmPkg/Library/ArmGenericTimerVirtCounterLib/ArmGenericTimerVirtCounterLib.inf
  UefiBootServicesTableLib|MdePkg/Library/UefiBootServicesTableLib/UefiBootServicesTableLib.inf
[Components]
  UsbCore/UsbCoreLib.inf
  DaliUsbGadgetLog/DaliUsbCdcLogLib.inf
  DaliUsb/DaliUsbLib.inf
  daliPkg/Library/UsbPmicLib/UsbPmicLib.inf
  daliPkg/Library/UsbPlatformLib/UsbPlatformLib.inf
  daliPkg/Library/UsbI2cLib/UsbI2cLib.inf
  daliPkg/Library/UsbXhciIoLib/UsbXhciIoLib.inf
  daliPkg/Library/UsbNativeHostLib/UsbNativeHostLib.inf
  MdePkg/Library/BaseFdtLib/BaseFdtLib.inf
