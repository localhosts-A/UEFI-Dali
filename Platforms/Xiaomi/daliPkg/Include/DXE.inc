  INF MdeModulePkg/Core/Dxe/DxeMain.inf
  INF MdeModulePkg/Universal/PCD/Dxe/Pcd.inf
  INF ArmPkg/Drivers/CpuDxe/CpuDxe.inf
  INF MdeModulePkg/Core/RuntimeDxe/RuntimeDxe.inf
  INF MdeModulePkg/Universal/SecurityStubDxe/SecurityStubDxe.inf
  INF MdeModulePkg/Universal/DevicePathDxe/DevicePathDxe.inf
  INF MdeModulePkg/Universal/ResetSystemRuntimeDxe/ResetSystemRuntimeDxe.inf
  # Complete the architectural services required before DXE can call BDS.
  # Variable storage is emulated RAM; RTC and capsule use the null libraries
  # already selected by SiliciumPkg. This watchdog uses UEFI timer events.
  INF MdeModulePkg/Universal/Variable/RuntimeDxe/VariableRuntimeDxe.inf
  INF MdeModulePkg/Universal/CapsuleRuntimeDxe/CapsuleRuntimeDxe.inf
  INF MdeModulePkg/Universal/WatchdogTimerDxe/WatchdogTimer.inf
  INF EmbeddedPkg/EmbeddedMonotonicCounter/EmbeddedMonotonicCounter.inf
  INF EmbeddedPkg/RealTimeClockRuntimeDxe/RealTimeClockRuntimeDxe.inf
  INF EmbeddedPkg/MetronomeDxe/MetronomeDxe.inf
  INF ArmPkg/Drivers/ArmGicDxe/ArmGicDxe.inf
  INF ArmPkg/Drivers/TimerDxe/TimerDxe.inf
  INF daliPkg/Drivers/PmicBusDxe/PmicBusDxe.inf
!if $(DALI_USB_RESOURCE_CONTEXT) == TRUE
  INF daliPkg/Drivers/UsbResourceContextDxe/UsbResourceContextDxe.inf
!endif
!if $(DALI_USB_NATIVE_HOST) == TRUE
  INF daliPkg/Drivers/UsbNativeHostWorkerDxe/UsbNativeHostWorkerDxe.inf
!endif
  INF daliPkg/Drivers/DisplayDxe/DisplayDxe.inf
  INF SiliciumPkg/Drivers/KeypadDeviceDxe/KeypadDeviceDxe.inf
  INF SiliciumPkg/Drivers/KeypadDxe/KeypadDxe.inf
  INF MdeModulePkg/Universal/HiiDatabaseDxe/HiiDatabaseDxe.inf
  INF MdeModulePkg/Universal/Console/GraphicsConsoleDxe/GraphicsConsoleDxe.inf
  INF daliPkg/Drivers/DiagnosticBdsDxe/DiagnosticBdsDxe.inf
  INF daliPkg/Drivers/PayloadDxe/PayloadDxe.inf
  INF DarwinBootPkg/Driver/DarwinBoot.inf
  INF MdeModulePkg/Universal/Disk/DiskIoDxe/DiskIoDxe.inf
  INF FatPkg/EnhancedFatDxe/Fat.inf
  INF MdeModulePkg/Universal/Disk/UnicodeCollation/EnglishDxe/EnglishDxe.inf
!if $(DALI_USB_HOST_STACK) == TRUE
  INF MdeModulePkg/Bus/Pci/XhciDxe/XhciDxe.inf
  INF MdeModulePkg/Bus/Usb/UsbBusDxe/UsbBusDxe.inf
  INF MdeModulePkg/Bus/Usb/UsbKbDxe/UsbKbDxe.inf
  INF MdeModulePkg/Bus/Usb/UsbMouseDxe/UsbMouseDxe.inf
  INF MdeModulePkg/Bus/Usb/UsbMassStorageDxe/UsbMassStorageDxe.inf
  INF MdeModulePkg/Universal/Disk/PartitionDxe/PartitionDxe.inf
  INF MdeModulePkg/Universal/Console/ConPlatformDxe/ConPlatformDxe.inf
  INF MdeModulePkg/Universal/Console/ConSplitterDxe/ConSplitterDxe.inf
!endif
  INF UniXNUPkg/Application/UniXNU/UniXNU.inf
  INF UniXNUPkg/Driver/UniXNUDxe/UniXNUDxe.inf
