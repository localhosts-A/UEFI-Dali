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
!ifndef DALI_USB_HOST_STACK
  # Opt-in source host/class stack only; native port/DMA registration is separate.
  DEFINE DALI_USB_HOST_STACK = FALSE
!endif
!ifndef DALI_USB_RESOURCE_CONTEXT
  DEFINE DALI_USB_RESOURCE_CONTEXT = FALSE
!endif
!ifndef DALI_USB_NATIVE_HOST
  DEFINE DALI_USB_NATIVE_HOST = FALSE
!endif
!ifndef DALI_USB_CDC_LOG
  DEFINE DALI_USB_CDC_LOG = FALSE
!endif
!if $(DALI_USB_NATIVE_HOST) == TRUE
  !if $(DALI_USB_HOST_STACK) != TRUE
    !error DALI_USB_NATIVE_HOST requires DALI_USB_HOST_STACK=TRUE
  !endif
  !if $(DALI_USB_RESOURCE_CONTEXT) != TRUE
    !error DALI_USB_NATIVE_HOST requires DALI_USB_RESOURCE_CONTEXT=TRUE
  !endif
!endif
!ifndef DALI_UFS_PROBE
  DEFINE DALI_UFS_PROBE = FALSE
!endif
!ifndef DALI_UFS_PROBE_TOKEN
  DEFINE DALI_UFS_PROBE_TOKEN = "none"
!endif

!include MT6991Pkg/MT6991Pkg.dsc.inc

[LibraryClasses]
  # Keep event waits polling until MT6991 WFI wakeup is validated separately.
  CpuLib|daliPkg/Library/PollingCpuLib/PollingCpuLib.inf
  KeypadDeviceLib|daliPkg/Library/KeypadDeviceLib/KeypadDeviceLib.inf
  UniXNULib|UniXNUPkg/Library/UniXNULib/UniXNULib.inf
!if $(DALI_USB_RESOURCE_CONTEXT) == TRUE || $(DALI_USB_CDC_LOG) == TRUE
  UsbCoreLib|UsbCore/UsbCoreLib.inf
!endif
!if $(DALI_USB_RESOURCE_CONTEXT) == TRUE
  DaliUsbLib|DaliUsb/DaliUsbLib.inf
  DaliUsbPlatformLib|daliPkg/Library/UsbPlatformLib/UsbPlatformLib.inf
  DaliUsbPmicLib|daliPkg/Library/UsbPmicLib/UsbPmicLib.inf
  DaliUsbI2cLib|daliPkg/Library/UsbI2cLib/UsbI2cLib.inf
  DaliUsbXhciIoLib|daliPkg/Library/UsbXhciIoLib/UsbXhciIoLib.inf
  DaliUsbNativeHostLib|daliPkg/Library/UsbNativeHostLib/UsbNativeHostLib.inf
  FdtLib|MdePkg/Library/BaseFdtLib/BaseFdtLib.inf
!endif
!if $(DALI_USB_CDC_LOG) == TRUE
  DaliUsbCdcLogLib|DaliUsbGadgetLog/DaliUsbCdcLogLib.inf
!endif

!if $(DALI_USB_CDC_LOG) == TRUE
[LibraryClasses.common.DXE_DRIVER, LibraryClasses.common.UEFI_DRIVER, LibraryClasses.common.UEFI_APPLICATION]
  SerialPortLib|daliPkg/Library/RamoopsCdcSerialPortLib/RamoopsCdcSerialPortLib.inf
!endif

[PcdsFixedAtBuild]
  gDaliTokenSpaceGuid.PcdDaliUfsProbeEnabled|$(DALI_UFS_PROBE)
  gDaliTokenSpaceGuid.PcdDaliUfsProbeToken|$(DALI_UFS_PROBE_TOKEN)
  gUniXNUPkgTokenSpaceGuid.PcdUniXNUDebugLevel|0x2
  gUniXNUPkgTokenSpaceGuid.PcdUniXNUPointerInputEnabled|$(DALI_USB_HOST_STACK)
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
!if $(DALI_USB_RESOURCE_CONTEXT) == TRUE
  daliPkg/Drivers/UsbResourceContextDxe/UsbResourceContextDxe.inf
!endif
!if $(DALI_USB_NATIVE_HOST) == TRUE
  daliPkg/Drivers/UsbNativeHostWorkerDxe/UsbNativeHostWorkerDxe.inf
!endif
!if $(DALI_USB_CDC_LOG) == TRUE
  daliPkg/Drivers/UsbCdcLogDxe/UsbCdcLogDxe.inf
!endif
!if $(DALI_USB_HOST_STACK) == TRUE
  MdeModulePkg/Bus/Pci/XhciDxe/XhciDxe.inf
  MdeModulePkg/Bus/Usb/UsbMouseDxe/UsbMouseDxe.inf
!endif
  daliPkg/Drivers/PmicBusDxe/PmicBusDxe.inf
  daliPkg/Drivers/PayloadDxe/PayloadDxe.inf
  DarwinBootPkg/Driver/DarwinBoot.inf
  UniXNUPkg/Application/UniXNU/UniXNU.inf
  UniXNUPkg/Driver/UniXNUDxe/UniXNUDxe.inf
  daliPkg/Drivers/DisplayDxe/DisplayDxe.inf
  SiliciumPkg/Drivers/KeypadDeviceDxe/KeypadDeviceDxe.inf
  SiliciumPkg/Drivers/KeypadDxe/KeypadDxe.inf
  daliPkg/Drivers/DiagnosticBdsDxe/DiagnosticBdsDxe.inf
