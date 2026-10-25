// SPDX-License-Identifier: BSD-2-Clause-Patent
// Explicit QEMU test application, never part of the Dali FV/default boot path.
#include <Uefi.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/PciIo.h>
#include <Protocol/UsbIo.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/SimplePointer.h>
#include <Guid/FileInfo.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UniXNULib.h>

enum { Xhc, Bus, Kb, Mouse, Mass, Disk, Partition, Fat, DriverCount };
STATIC CONST CHAR16 *Names[] = {L"XhciDxe",L"UsbBusDxe",L"UsbKbDxe",L"UsbMouseDxe",
  L"UsbMassStorageDxe",L"DiskIoDxe",L"PartitionDxe",L"Fat"};
STATIC EFI_HANDLE Drivers[DriverCount + 1], Controller, Hub, MassHandle, KeyboardHandle;
STATIC EFI_DEVICE_PATH_PROTOCOL *MassPath;
STATIC EFI_SIMPLE_TEXT_INPUT_PROTOCOL *Keyboard;
STATIC EFI_SIMPLE_POINTER_PROTOCOL *Pointer;
STATIC EFI_USB_IO_PROTOCOL *KeyboardUsb;
STATIC UINT8 KeyboardInterface;
STATIC CONST CHAR8 *Stage = "entry";
STATIC BOOLEAN UnderPath (EFI_HANDLE Handle, EFI_DEVICE_PATH_PROTOCOL *Parent) {
  EFI_DEVICE_PATH_PROTOCOL *Path = DevicePathFromHandle (Handle);
  if (Path == NULL || Parent == NULL) { return FALSE; }
  UINTN Size = GetDevicePathSize (Parent);
  return Size > END_DEVICE_PATH_LENGTH && GetDevicePathSize (Path) >= Size &&
    CompareMem (Path, Parent, Size - END_DEVICE_PATH_LENGTH) == 0;
}
STATIC UINTN UsbDepth (EFI_HANDLE Handle) {
  EFI_DEVICE_PATH_PROTOCOL *Path = DevicePathFromHandle (Handle); UINTN Ports = 0;
  if (Path == NULL) { return 0; }
  for (; !IsDevicePathEnd (Path); Path = NextDevicePathNode (Path)) {
    if (DevicePathType (Path) == MESSAGING_DEVICE_PATH && DevicePathSubType (Path) == MSG_USB_DP) { Ports++; }
  }
  return Ports;
}
STATIC BOOLEAN UsbPath (EFI_HANDLE Handle, BOOLEAN RequirePartition) {
  EFI_DEVICE_PATH_PROTOCOL *Path = DevicePathFromHandle (Handle);
  BOOLEAN HasPartition = FALSE;
  if (Controller == NULL || Hub == NULL || !UnderPath (Handle, DevicePathFromHandle (Controller)) ||
      !UnderPath (Handle, DevicePathFromHandle (Hub)) || UsbDepth (Handle) < 2) { return FALSE; }
  for (; !IsDevicePathEnd (Path); Path = NextDevicePathNode (Path)) {
    if (DevicePathType (Path) == MEDIA_DEVICE_PATH && DevicePathSubType (Path) == MEDIA_HARDDRIVE_DP) {
      if (DevicePathNodeLength (Path) != sizeof (HARDDRIVE_DEVICE_PATH)) { return FALSE; }
      HARDDRIVE_DEVICE_PATH *PartitionPath = (HARDDRIVE_DEVICE_PATH *)Path;
      if (PartitionPath->MBRType != MBR_TYPE_EFI_PARTITION_TABLE_HEADER ||
          PartitionPath->SignatureType != SIGNATURE_TYPE_GUID) { return FALSE; }
      HasPartition = TRUE;
    }
  }
  return !RequirePartition || HasPartition;
}
STATIC BOOLEAN Bound (EFI_HANDLE Handle, EFI_HANDLE Driver) {
  EFI_GUID **Protocols; UINTN Count;
  if (EFI_ERROR (gBS->ProtocolsPerHandle (Handle, &Protocols, &Count))) { return FALSE; }
  BOOLEAN Found = FALSE;
  for (UINTN I = 0; I < Count; I++) {
    EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *Info; UINTN Entries;
    if (EFI_ERROR (gBS->OpenProtocolInformation (Handle, Protocols[I], &Info, &Entries))) { continue; }
    for (UINTN J = 0; J < Entries; J++) {
      if (Info[J].AgentHandle == Driver && (Info[J].Attributes & EFI_OPEN_PROTOCOL_BY_DRIVER)) { Found = TRUE; }
    }
    FreePool (Info);
  }
  FreePool (Protocols); return Found;
}
STATIC EFI_STATUS LoadDrivers (EFI_HANDLE Image) {
  EFI_LOADED_IMAGE_PROTOCOL *Loaded;
  EFI_STATUS Status = gBS->HandleProtocol (Image, &gEfiLoadedImageProtocolGuid, (VOID **)&Loaded);
  if (EFI_ERROR (Status)) { return Status; }
  for (UINTN I = 0; I < DriverCount; I++) {
    CHAR16 File[128]; UnicodeSPrint (File, sizeof File, L"\\EFI\\DALI\\%s.efi", Names[I]);
    EFI_DEVICE_PATH_PROTOCOL *Path = FileDevicePath (Loaded->DeviceHandle, File);
    if (Path == NULL) { return EFI_OUT_OF_RESOURCES; }
    Status = gBS->LoadImage (FALSE, Image, Path, NULL, 0, &Drivers[I]); FreePool (Path);
    if (EFI_ERROR (Status)) { return Status; }
    Status = gBS->StartImage (Drivers[I], NULL, NULL); if (EFI_ERROR (Status)) { return Status; }
  }
  return EFI_SUCCESS;
}
STATIC EFI_STATUS ConnectSource (VOID) {
  EFI_HANDLE *Handles; UINTN Count;
  EFI_STATUS Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiPciIoProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) { return Status; }
  for (UINTN I = 0; I < Count; I++) {
    EFI_PCI_IO_PROTOCOL *Pci; UINT8 Class[3];
    if (EFI_ERROR (gBS->HandleProtocol (Handles[I], &gEfiPciIoProtocolGuid, (VOID **)&Pci)) ||
        EFI_ERROR (Pci->Pci.Read (Pci, EfiPciIoWidthUint8, 9, 3, Class))) { continue; }
    if (Class[0] == 0x30 && Class[1] == 3 && Class[2] == 0xc) {
      if (Controller != NULL) { FreePool (Handles); return EFI_UNSUPPORTED; }
      Controller = Handles[I];
    }
  }
  FreePool (Handles);
  if (Controller == NULL) { return EFI_NOT_FOUND; }
  Status = gBS->DisconnectController (Controller, NULL, NULL);
  if (EFI_ERROR (Status)) { return Status; }
  Status = gBS->ConnectController (Controller, Drivers, NULL, TRUE);
  if (EFI_ERROR (Status) || !Bound (Controller, Drivers[Xhc]) || !Bound (Controller, Drivers[Bus])) { return EFI_DEVICE_ERROR; }
  return EFI_SUCCESS;
}
STATIC EFI_STATUS FindHub (VOID) {
  EFI_HANDLE *Handles; UINTN Count;
  EFI_STATUS Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiUsbIoProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) { return Status; }
  for (UINTN I = 0; I < Count; I++) {
    EFI_USB_IO_PROTOCOL *Usb; EFI_USB_INTERFACE_DESCRIPTOR Desc;
    if (!UnderPath (Handles[I], DevicePathFromHandle (Controller)) || UsbDepth (Handles[I]) != 1 ||
        EFI_ERROR (gBS->HandleProtocol (Handles[I], &gEfiUsbIoProtocolGuid, (VOID **)&Usb)) ||
        EFI_ERROR (Usb->UsbGetInterfaceDescriptor (Usb, &Desc)) || Desc.InterfaceClass != 9) { continue; }
    if (Hub != NULL) { FreePool (Handles); return EFI_UNSUPPORTED; }
    Hub = Handles[I];
  }
  FreePool (Handles); return Hub != NULL ? EFI_SUCCESS : EFI_NOT_FOUND;
}
STATIC VOID ConnectUsbChildren (VOID) {
  // UEFI recursive ConnectController need not propagate the preferred driver
  // list. Walk successive USB/LUN/disk/partition layers explicitly instead.
  for (UINTN Pass = 0; Pass < 6; Pass++) {
    EFI_HANDLE *Handles; UINTN Count;
    if (EFI_ERROR (gBS->LocateHandleBuffer (AllHandles, NULL, NULL, &Count, &Handles))) { return; }
    for (UINTN I = 0; I < Count; I++) {
      if (UsbPath (Handles[I], FALSE)) { gBS->ConnectController (Handles[I], Drivers, NULL, FALSE); }
    }
    FreePool (Handles);
  }
}
STATIC EFI_STATUS RebindClasses (VOID) {
  EFI_HANDLE *Handles; UINTN Count;
  EFI_STATUS Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiUsbIoProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) { return Status; }
  for (UINTN I = 0; I < Count; I++) {
    EFI_USB_IO_PROTOCOL *Usb; EFI_USB_INTERFACE_DESCRIPTOR Desc;
    if (!UsbPath (Handles[I], FALSE) || EFI_ERROR (gBS->HandleProtocol (Handles[I], &gEfiUsbIoProtocolGuid, (VOID **)&Usb)) ||
        EFI_ERROR (Usb->UsbGetInterfaceDescriptor (Usb, &Desc)) || (Desc.InterfaceClass != 3 && Desc.InterfaceClass != 8)) { continue; }
    Status = gBS->DisconnectController (Handles[I], NULL, NULL);
    if (!EFI_ERROR (Status)) { Status = gBS->ConnectController (Handles[I], Drivers, NULL, FALSE); }
    if (EFI_ERROR (Status)) { FreePool (Handles); return Status; }
  }
  FreePool (Handles); ConnectUsbChildren (); return EFI_SUCCESS;
}
STATIC EFI_STATUS FindInputs (VOID) {
  EFI_HANDLE *Handles; UINTN Count, Keyboards = 0, Mice = 0, Disks = 0;
  Keyboard = NULL; Pointer = NULL; MassHandle = NULL; KeyboardUsb = NULL;
  EFI_STATUS Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiUsbIoProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) { return Status; }
  for (UINTN I = 0; I < Count; I++) {
    EFI_USB_IO_PROTOCOL *Usb; EFI_USB_INTERFACE_DESCRIPTOR Desc;
    if (EFI_ERROR (gBS->HandleProtocol (Handles[I], &gEfiUsbIoProtocolGuid, (VOID **)&Usb)) ||
        EFI_ERROR (Usb->UsbGetInterfaceDescriptor (Usb, &Desc))) { continue; }
    Print (L"DALI_USB_IF class=%u sub=%u protocol=%u hubpath=%u kb=%u mouse=%u mass=%u\n",
      Desc.InterfaceClass, Desc.InterfaceSubClass, Desc.InterfaceProtocol, UsbPath (Handles[I], FALSE),
      Bound (Handles[I], Drivers[Kb]), Bound (Handles[I], Drivers[Mouse]), Bound (Handles[I], Drivers[Mass]));
    if (!UsbPath (Handles[I], FALSE)) { continue; }
    if (Desc.InterfaceClass == 3 && Desc.InterfaceSubClass == 1 && Desc.InterfaceProtocol == 1 && Bound (Handles[I], Drivers[Kb])) {
      Keyboards++;
      Status = gBS->HandleProtocol (Handles[I], &gEfiSimpleTextInProtocolGuid, (VOID **)&Keyboard);
      if (!EFI_ERROR (Status)) { KeyboardUsb = Usb; KeyboardInterface = Desc.InterfaceNumber; KeyboardHandle = Handles[I]; }
    }
    if (Desc.InterfaceClass == 3 && Desc.InterfaceSubClass == 1 && Desc.InterfaceProtocol == 2 && Bound (Handles[I], Drivers[Mouse])) {
      Mice++;
      gBS->HandleProtocol (Handles[I], &gEfiSimplePointerProtocolGuid, (VOID **)&Pointer);
    }
    if (Desc.InterfaceClass == 8 && Desc.InterfaceSubClass == 6 && Desc.InterfaceProtocol == 0x50 && Bound (Handles[I], Drivers[Mass])) {
      Disks++; MassHandle = Handles[I];
    }
  }
  FreePool (Handles);
  Print (L"DALI_USB_INPUTS hubs=%u kb=%u mouse=%u mass=%u\n", Hub != NULL, Keyboards, Mice, Disks);
  if (Keyboards != 1 || Mice != 1 || Disks != 1 || Keyboard == NULL || Pointer == NULL || MassHandle == NULL) { return EFI_NOT_FOUND; }
  MassPath = DuplicateDevicePath (DevicePathFromHandle (MassHandle));
  if (MassPath == NULL) { return EFI_OUT_OF_RESOURCES; }
  return EFI_SUCCESS;
}
STATIC EFI_STATUS HidTest (VOID) {
  EFI_STATUS Status = Keyboard->Reset (Keyboard, FALSE);
  if (!EFI_ERROR (Status)) { Status = Pointer->Reset (Pointer, FALSE); }
  if (EFI_ERROR (Status)) { return Status; }
  Print (L"DALI_USB_HID_READY\n");
  BOOLEAN Key = FALSE, Released = FALSE, Moved = FALSE, Down = FALSE, Up = FALSE;
  for (UINTN I = 0; I < 15000; I++) {
    EFI_INPUT_KEY Input; EFI_SIMPLE_POINTER_STATE MouseState;
    if (!EFI_ERROR (Keyboard->ReadKeyStroke (Keyboard, &Input)) && Input.UnicodeChar == L'a') { Key = TRUE; }
    if (!EFI_ERROR (Pointer->GetState (Pointer, &MouseState))) {
      Moved |= MouseState.RelativeMovementX != 0 || MouseState.RelativeMovementY != 0;
      if (MouseState.LeftButton && !Down) { Down = TRUE; Print (L"DALI_USB_MOUSE_DOWN\n"); }
      else if (Down && !MouseState.LeftButton) { Up = TRUE; }
    }
    if (Key && !Released) {
      EFI_USB_DEVICE_REQUEST Request = {0xa1, 1, 0x100, KeyboardInterface, 8}; UINT8 Report[8]; UINT32 Result;
      SetMem (Report, sizeof Report, 0xff); // A successful short report cannot prove release.
      if (!EFI_ERROR (KeyboardUsb->UsbControlTransfer (KeyboardUsb, &Request, EfiUsbDataIn, 1000, Report, sizeof Report, &Result)) && Result == 0) {
        Released = TRUE; for (UINTN J = 0; J < sizeof Report; J++) { if (Report[J]) { Released = FALSE; } }
      }
    }
    if (Key && Released && Moved && Down && Up) { Print (L"DALI_USB_HID_PASS\n"); return EFI_SUCCESS; }
    gBS->Stall (1000);
  }
  return EFI_TIMEOUT;
}
STATIC EFI_STATUS OpenUsbRoot (EFI_FILE_PROTOCOL **Root) {
  EFI_HANDLE *Handles; UINTN Count;
  EFI_STATUS Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleFileSystemProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) { return Status; }
  Status = EFI_NOT_FOUND; *Root = NULL;
  for (UINTN I = 0; I < Count; I++) {
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs; EFI_FILE_PROTOCOL *Candidate, *Tag;
    if (!UsbPath (Handles[I], TRUE) || !UnderPath (Handles[I], MassPath) || !Bound (Handles[I], Drivers[Fat]) ||
        EFI_ERROR (gBS->HandleProtocol (Handles[I], &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs)) ||
        EFI_ERROR (Fs->OpenVolume (Fs, &Candidate))) { continue; }
    if (!EFI_ERROR (Candidate->Open (Candidate, &Tag, L"USBTEST.TAG", EFI_FILE_MODE_READ, 0))) {
      CHAR8 Marker[20]; UINTN Size = sizeof Marker;
      EFI_STATUS Read = Tag->Read (Tag, &Size, Marker); Tag->Close (Tag);
      if (!EFI_ERROR (Read) && Size == 19 && CompareMem (Marker, "DALI USB QEMU ONLY\n", 19) == 0) {
        if (*Root != NULL) {
          Candidate->Close (Candidate); (*Root)->Close (*Root); *Root = NULL;
          Status = EFI_UNSUPPORTED; break;
        }
        *Root = Candidate; Status = EFI_SUCCESS; continue;
      }
    }
    Candidate->Close (Candidate);
  }
  FreePool (Handles); return Status;
}
STATIC EFI_STATUS NoMountedUsbVolume (VOID) {
  EFI_HANDLE *Handles; UINTN Count;
  EFI_STATUS Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleFileSystemProtocolGuid, NULL, &Count, &Handles);
  if (Status == EFI_NOT_FOUND) { return EFI_SUCCESS; }
  if (EFI_ERROR (Status)) { return Status; }
  Status = EFI_SUCCESS;
  for (UINTN I = 0; I < Count; I++) {
    if (UnderPath (Handles[I], MassPath)) { Status = EFI_DEVICE_ERROR; break; }
  }
  FreePool (Handles); return Status;
}
STATIC VOID Pattern (UINT8 *Data) {
  for (UINTN I = 0; I < 32768; I++) { Data[I] = (UINT8)(I * 37 + 11); }
  for (UINTN I = 0; I < 1024; I++) { Data[4096 + I] = (UINT8)(I * 19 + 7); }
}
STATIC EFI_STATUS FileTest (VOID) {
  EFI_FILE_PROTOCOL *Root, *File; UINT8 *Data = AllocatePool (32768);
  if (Data == NULL) { return EFI_OUT_OF_RESOURCES; }
  EFI_STATUS Status = OpenUsbRoot (&Root); if (EFI_ERROR (Status)) { FreePool (Data); return Status; }
  CONST CHAR16 *Reserved[] = { L"UEFIUSB.BIN", L"UEFIUSB.DAT", L"DELETE.ME" };
  for (UINTN I = 0; I < ARRAY_SIZE (Reserved); I++) {
    Status = Root->Open (Root, &File, (CHAR16 *)Reserved[I], EFI_FILE_MODE_READ, 0);
    if (Status == EFI_NOT_FOUND) { continue; }
    if (!EFI_ERROR (Status)) { File->Close (File); Status = EFI_ACCESS_DENIED; }
    Root->Close (Root); FreePool (Data); return Status;
  }
  Status = Root->Open (Root, &File, L"UEFIUSB.BIN", EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
  if (EFI_ERROR (Status)) { Root->Close (Root); FreePool (Data); return Status; }
  for (UINTN I = 0; I < 32768; I++) { Data[I] = (UINT8)(I * 37 + 11); }
  UINTN Size = 32768;
  Status = File->Write (File, &Size, Data);
  if (!EFI_ERROR (Status) && Size != 32768) { Status = EFI_BAD_BUFFER_SIZE; }
  if (!EFI_ERROR (Status)) { Status = File->Flush (File); }
  if (!EFI_ERROR (Status)) { Status = File->SetPosition (File, 4096); }
  Pattern (Data); Size = 1024;
  if (!EFI_ERROR (Status)) { Status = File->Write (File, &Size, Data + 4096); }
  if (!EFI_ERROR (Status) && Size != 1024) { Status = EFI_BAD_BUFFER_SIZE; }
  UINTN InfoSize = SIZE_OF_EFI_FILE_INFO + 128; EFI_FILE_INFO *Info = AllocateZeroPool (InfoSize);
  if (Info == NULL) { Status = EFI_OUT_OF_RESOURCES; }
  if (!EFI_ERROR (Status)) { Status = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, Info); }
  if (!EFI_ERROR (Status)) {
    StrCpyS (Info->FileName, 64, L"UEFIUSB.DAT"); Info->Size = SIZE_OF_EFI_FILE_INFO + StrSize (Info->FileName);
    Status = File->SetInfo (File, &gEfiFileInfoGuid, (UINTN)Info->Size, Info);
  }
  if (!EFI_ERROR (Status)) { Status = File->Flush (File); }
  if (Info != NULL) { FreePool (Info); } File->Close (File); Root->Close (Root);
  if (EFI_ERROR (Status)) { FreePool (Data); return Status; }
  // True filesystem/partition/BOT stop then rebind, while the USB bus stays live.
  Status = gBS->DisconnectController (MassHandle, NULL, NULL);
  if (!EFI_ERROR (Status)) { Status = NoMountedUsbVolume (); }
  if (!EFI_ERROR (Status)) { Status = gBS->ConnectController (MassHandle, Drivers, NULL, FALSE); }
  if (!EFI_ERROR (Status)) { ConnectUsbChildren (); }
  if (!EFI_ERROR (Status)) { Status = OpenUsbRoot (&Root); }
  if (EFI_ERROR (Status)) { FreePool (Data); return Status; }
  Status = Root->Open (Root, &File, L"UEFIUSB.DAT", EFI_FILE_MODE_READ, 0);
  if (!EFI_ERROR (Status)) {
    UINT8 *Read = AllocatePool (32769); Size = 32769;
    if (Read == NULL) { Status = EFI_OUT_OF_RESOURCES; }
    else {
      Status = File->Read (File, &Size, Read);
      if (!EFI_ERROR (Status) && (Size != 32768 || CompareMem (Data, Read, Size))) { Status = EFI_CRC_ERROR; }
      FreePool (Read);
    }
    File->Close (File);
  }
  if (!EFI_ERROR (Status)) {
    EFI_STATUS Renamed = Root->Open (Root, &File, L"UEFIUSB.BIN", EFI_FILE_MODE_READ, 0);
    if (Renamed != EFI_NOT_FOUND) { if (!EFI_ERROR (Renamed)) { File->Close (File); } Status = EFI_DEVICE_ERROR; }
  }
  if (!EFI_ERROR (Status)) { Status = Root->Open (Root, &File, L"DELETE.ME", EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0); }
  if (!EFI_ERROR (Status)) {
    Size = 1024; Status = File->Write (File, &Size, Data);
    if (!EFI_ERROR (Status) && Size != 1024) { Status = EFI_BAD_BUFFER_SIZE; }
    if (!EFI_ERROR (Status)) { Status = File->Flush (File); }
    EFI_STATUS DeleteStatus = File->Delete (File);
    if (!EFI_ERROR (Status)) { Status = DeleteStatus; }
  }
  if (!EFI_ERROR (Status)) {
    EFI_STATUS Deleted = Root->Open (Root, &File, L"DELETE.ME", EFI_FILE_MODE_READ, 0);
    if (Deleted != EFI_NOT_FOUND) { if (!EFI_ERROR (Deleted)) { File->Close (File); } Status = EFI_DEVICE_ERROR; }
  }
  Root->Close (Root); FreePool (Data);
  if (!EFI_ERROR (Status)) { Print (L"DALI_USB_FAT_REMOUNT_PASS\n"); }
  return Status;
}
STATIC EFI_STATUS VerifySourceBindings (VOID) {
  EFI_HANDLE *Handles; UINTN Count; BOOLEAN Seen[DriverCount] = {0};
  EFI_STATUS Status = gBS->LocateHandleBuffer (AllHandles, NULL, NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) { return Status; }
  for (UINTN I = 0; I < Count; I++) {
    if (!UsbPath (Handles[I], FALSE)) { continue; }
    for (UINTN J = Kb; J < DriverCount; J++) {
      if (J >= Mass && !UnderPath (Handles[I], MassPath)) { continue; }
      Seen[J] |= Bound (Handles[I], Drivers[J]);
    }
  }
  FreePool (Handles);
  for (UINTN J = Kb; J < DriverCount; J++) { if (!Seen[J]) { Print (L"DALI_USB_UNBOUND %s\n", Names[J]); return EFI_NOT_FOUND; } }
  Print (L"DALI_USB_SOURCE_BINDINGS_PASS\n"); return EFI_SUCCESS;
}
STATIC EFI_STATUS MenuTest (EFI_HANDLE Image) {
  // QEMU-only consumer binding: use the actual source keyboard protocol as
  // system ConIn while exercising the public, complete UniXNU boot manager.
  EFI_SIMPLE_TEXT_INPUT_PROTOCOL *Previous = gST->ConIn;
  EFI_HANDLE PreviousHandle = gST->ConsoleInHandle; UINT32 Crc = gST->Hdr.CRC32;
  gST->ConIn = Keyboard; gST->ConsoleInHandle = KeyboardHandle; gST->Hdr.CRC32 = 0;
  EFI_STATUS Status = gBS->CalculateCrc32 (gST, gST->Hdr.HeaderSize, &gST->Hdr.CRC32);
  if (!EFI_ERROR (Status)) { Status = UniXNURun (Image, UniXNUHostApplication); }
  gST->ConIn = Previous; gST->ConsoleInHandle = PreviousHandle; gST->Hdr.CRC32 = Crc;
  if (!EFI_ERROR (Status)) { Print (L"DALI_USB_MENU_PASS\n"); }
  return Status;
}
EFI_STATUS EFIAPI UsbQemuTestMain (EFI_HANDLE Image, EFI_SYSTEM_TABLE *SystemTable) {
  (VOID)SystemTable;
  Stage = "load"; EFI_STATUS Status = LoadDrivers (Image);
  if (!EFI_ERROR (Status)) { Stage = "connect"; Status = ConnectSource (); }
  if (!EFI_ERROR (Status)) { Stage = "hub"; Status = FindHub (); }
  if (!EFI_ERROR (Status)) { Stage = "rebind"; Status = RebindClasses (); }
  if (!EFI_ERROR (Status)) { Stage = "enumerate"; Status = FindInputs (); }
  if (!EFI_ERROR (Status)) { Stage = "hid"; Status = HidTest (); }
  if (!EFI_ERROR (Status)) { Stage = "menu"; Status = MenuTest (Image); }
  if (!EFI_ERROR (Status)) { Stage = "files"; Status = FileTest (); }
  if (!EFI_ERROR (Status)) { Stage = "bindings"; Status = VerifySourceBindings (); }
  if (!EFI_ERROR (Status)) { Stage = "stop"; Status = gBS->DisconnectController (Controller, NULL, NULL); }
  Print (EFI_ERROR (Status) ? L"DALI_USB_FAIL %a %r\n" : L"DALI_USB_QEMU_PASS\n", Stage, Status);
  gRT->ResetSystem (EfiResetShutdown, Status, 0, NULL);
  return Status;
}
