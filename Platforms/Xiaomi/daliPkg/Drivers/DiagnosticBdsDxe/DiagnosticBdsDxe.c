// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Uefi.h>
#include <Protocol/Bds.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/SimpleTextOut.h>
#include <Protocol/EfiKeypadDevice.h>
#include <Protocol/BlockIo.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/LoadedImage.h>
#include <Guid/Fdt.h>
#include <Guid/EventGroup.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/Mt6991PlatformLib.h>
#include <Library/UefiBootManagerLib.h>
#include <Library/UefiLib.h>
#include <Library/TimerLib.h>
#include <Configuration/BootDevices.h>

STATIC volatile UINTN mTicks;
STATIC EFI_HANDLE mBdsImage;
STATIC UINTN mHeartbeats;
STATIC VOID EFIAPI Tick (IN EFI_EVENT Event, IN VOID *Context) { mTicks++; }
STATIC VOID EFIAPI Heartbeat (IN EFI_EVENT Event, IN VOID *Context) {
  if (++mHeartbeats <= 30) {
    DEBUG ((DEBUG_WARN, "DALI: app heartbeat=%lu time-ms=%lu\n", mHeartbeats,
      GetTimeInNanoSecond (GetPerformanceCounter ()) / 1000000));
  }
}

STATIC VOID DiagnosticHalt (VOID) {
  // Run 2 lost its ramoops evidence after the automatic reset path. Keep the
  // final result available until the operator uses the proven fastboot keys.
  DEBUG ((DEBUG_WARN, "DALI: diagnostic halted; enter fastboot with hardware keys\n"));
  CpuDeadLoop ();
}

STATIC VOID ConnectProtocol (EFI_GUID *Protocol) {
  EFI_HANDLE *Handles;
  UINTN Count;
  EFI_STATUS Status = gBS->LocateHandleBuffer (ByProtocol, Protocol, NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) { return; }
  for (UINTN I = 0; I < Count; I++) {
    Status = gBS->ConnectController (Handles[I], NULL, NULL, TRUE);
    DEBUG ((DEBUG_WARN, "DALI: connect controller %g status=%r\n", Protocol, Status));
  }
  FreePool (Handles);
}

STATIC BOOLEAN PrepareUniXNU (VOID) {
  EFI_STATUS Status;
  EFI_HANDLE *Handles;
  UINTN Count;
  BOOLEAN PayloadOk = FALSE;
  Status = EfiBootManagerUpdateConsoleVariable (ConIn, (VOID *)&KeypadDevicePath, NULL);
  if (!EFI_ERROR (Status)) {
    Status = EfiBootManagerUpdateConsoleVariable (ConOut, (VOID *)&DisplayDevicePath, NULL);
  }
  if (!EFI_ERROR (Status)) {
    Status = EfiBootManagerUpdateConsoleVariable (ErrOut, (VOID *)&DisplayDevicePath, NULL);
  }
  if (!EFI_ERROR (Status)) { Status = EfiBootManagerConnectAllDefaultConsoles (); }
  DEBUG ((DEBUG_WARN, "DALI: system consoles status=%r input=%p output=%p\n", Status, gST->ConIn, gST->ConOut));
  if (EFI_ERROR (Status) || gST->ConIn == NULL || gST->ConOut == NULL) { return FALSE; }
  ConnectProtocol (&gEfiBlockIoProtocolGuid);
  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleFileSystemProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) { return FALSE; }
  for (UINTN I = 0; I < Count; I++) {
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs;
    EFI_FILE_PROTOCOL *Root, *File;
    CHAR8 Header[64];
    UINTN Size = sizeof Header;
    Status = gBS->HandleProtocol (Handles[I], &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs);
    if (EFI_ERROR (Status)) { continue; }
    Status = Fs->OpenVolume (Fs, &Root);
    if (EFI_ERROR (Status)) { continue; }
    Status = Root->Open (Root, &File, L"\\EFI\\UniXNU\\unixnu.toml", EFI_FILE_MODE_READ, 0);
    if (!EFI_ERROR (Status)) {
      Status = File->Read (File, &Size, Header);
      File->Close (File);
      if (!EFI_ERROR (Status) && Size >= 11 && CompareMem (Header, "version = 1", 11) == 0) {
        PayloadOk = TRUE;
      }
    }
    Root->Close (Root);
  }
  FreePool (Handles);
  DEBUG ((DEBUG_WARN, "DALI: payload filesystem=%a volumes=%lu\n", PayloadOk ? "PASS" : "FAIL", Count));
  Status = gBS->InstallConfigurationTable (&gFdtTableGuid, (VOID *)(UINTN)DALI_FDT_BASE);
  DEBUG ((DEBUG_WARN, "DALI: EFI_DTB_TABLE status=%r\n", Status));
  return PayloadOk && !EFI_ERROR (Status);
}

STATIC VOID LaunchUniXNU (VOID) {
  STATIC CONST CHAR16 Marker[] = L"unixnu:firmware";
  EFI_BOOT_MANAGER_LOAD_OPTION *Options;
  UINTN Count;
  BOOLEAN Found = FALSE;
  Options = EfiBootManagerGetLoadOptions (&Count, LoadOptionTypeBoot);
  for (UINTN I = 0; I < Count; I++) {
    if (Options[I].OptionalDataSize != sizeof Marker ||
        CompareMem (Options[I].OptionalData, Marker, sizeof Marker) != 0) { continue; }
    Found = TRUE;
    EfiSignalEventReadyToBoot ();
    DEBUG ((DEBUG_WARN, "DALI: starting registered UniXNU Boot%04x\n", Options[I].OptionNumber));
    EFI_HANDLE App = NULL;
    EFI_LOADED_IMAGE_PROTOCOL *Loaded;
    // Launch the registered FV application without the generic PC setup and
    // driver-health policy path. Keep its firmware load-option marker intact.
    EFI_STATUS Status = gBS->LoadImage (FALSE, mBdsImage, Options[I].FilePath, NULL, 0, &App);
    DEBUG ((DEBUG_WARN, "DALI: UniXNU LoadImage status=%r\n", Status));
    if (!EFI_ERROR (Status)) {
      Status = gBS->HandleProtocol (App, &gEfiLoadedImageProtocolGuid, (VOID **)&Loaded);
      if (!EFI_ERROR (Status)) {
        Loaded->LoadOptions = Options[I].OptionalData;
        Loaded->LoadOptionsSize = (UINT32)Options[I].OptionalDataSize;
        EFI_EVENT Alive = NULL;
        mHeartbeats = 0;
        EFI_STATUS AliveStatus = gBS->CreateEvent (EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK, Heartbeat, NULL, &Alive);
        if (!EFI_ERROR (AliveStatus)) {
          AliveStatus = gBS->SetTimer (Alive, TimerPeriodic, 10000000);
        }
        DEBUG ((DEBUG_WARN, "DALI: app heartbeat setup=%r\n", AliveStatus));
        Status = gBS->StartImage (App, NULL, NULL);
        if (Alive != NULL) { gBS->CloseEvent (Alive); }
      }
      gBS->UnloadImage (App);
    }
    // The application deliberately returns EFI_ABORTED for its Exit action.
    DEBUG ((DEBUG_WARN, "DALI: UniXNU returned status=%r\n", Status));
    if (Status == EFI_ABORTED || !EFI_ERROR (Status)) { break; }
  }
  if (Options != NULL) { EfiBootManagerFreeLoadOptions (Options, Count); }
  if (!Found) { DEBUG ((DEBUG_ERROR, "DALI: UEFI_STAGE_FAIL UniXNU option not registered\n")); }
}

STATIC VOID DisplayAndInputProbe (VOID) {
  EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop = NULL;
  EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *Out = NULL;
  EFI_SIMPLE_TEXT_INPUT_PROTOCOL *In = NULL;
  EFI_STATUS Status;
  EFI_INPUT_KEY Key;
  UINTN Keys = 0;
  CHAR16 Text[128];
  STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL Colors[] = {
    {0, 0, 255, 255}, {0, 255, 0, 255}, {255, 0, 0, 255}, {255, 255, 255, 255}
  };
  DEBUG ((DEBUG_WARN, "DALI: connecting display and input drivers\n"));
  ConnectProtocol (&gEfiGraphicsOutputProtocolGuid);
  ConnectProtocol (&gKeypadDeviceProtocolGuid);
  Status = gBS->LocateProtocol (&gEfiSimpleTextOutProtocolGuid, NULL, (VOID **)&Out);
  DEBUG ((DEBUG_WARN, "DALI: text output status=%r\n", Status));
  if (Out != NULL) {
    Out->SetAttribute (Out, EFI_TEXT_ATTR (EFI_WHITE, EFI_BLACK));
    Out->ClearScreen (Out);
    Out->EnableCursor (Out, FALSE);
    Out->OutputString (Out, L"\r\nMu-Silicium UEFI / REDMI K80 Ultra\r\n\r\n");
    Out->OutputString (Out, L"Core, memory map and timer diagnostics complete.\r\n");
    Out->OutputString (Out, L"UniXNU: tap Volume up for UP, hold 0.7 seconds for ENTER.\r\n");
    Out->OutputString (Out, L"Color bars below: RED / GREEN / BLUE / WHITE.\r\n");
    Out->OutputString (Out, L"Power + Volume down: return to fastboot.\r\n\r\n");
  }
  Status = gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop);
  DEBUG ((DEBUG_WARN, "DALI: GOP locate status=%r\n", Status));
  if (Gop != NULL) {
    UINTN Width = Gop->Mode->Info->HorizontalResolution;
    UINTN Height = Gop->Mode->Info->VerticalResolution;
    BOOLEAN ReadbackOk = TRUE;
    for (UINTN I = 0; I < ARRAY_SIZE (Colors); I++) {
      EFI_GRAPHICS_OUTPUT_BLT_PIXEL Pixel;
      UINTN X = Width * I / ARRAY_SIZE (Colors), W = Width / ARRAY_SIZE (Colors);
      Status = Gop->Blt (Gop, &Colors[I], EfiBltVideoFill, 0, 0, X, Height - 128, W, 96, 0);
      if (EFI_ERROR (Status)) { ReadbackOk = FALSE; continue; }
      Status = Gop->Blt (Gop, &Pixel, EfiBltVideoToBltBuffer, X + W / 2, Height - 80, 0, 0, 1, 1, 0);
      if (EFI_ERROR (Status) || CompareMem (&Pixel, &Colors[I], 3) != 0) { ReadbackOk = FALSE; }
    }
    DEBUG ((DEBUG_WARN, "DALI: GOP_BLT_%a (CPU readback; visibility requires observation)\n",
      ReadbackOk ? "PASS" : "FAIL"));
  }
  Status = gBS->LocateProtocol (&gEfiSimpleTextInProtocolGuid, NULL, (VOID **)&In);
  DEBUG ((DEBUG_WARN, "DALI: text input status=%r\n", Status));
  if (PrepareUniXNU ()) {
    DEBUG ((DEBUG_WARN, "DALI: UEFI_UNIXNU_READY\n"));
    LaunchUniXNU ();
    if (Out != NULL) {
      Out->OutputString (Out, L"\r\nReturned to dali firmware. Hold Volume up to reopen UniXNU.\r\n");
      Out->OutputString (Out, L"Power + Volume down: enter fastboot for Android recovery.\r\n");
    }
  } else {
    DEBUG ((DEBUG_ERROR, "DALI: UEFI_STAGE_FAIL consoles or payload filesystem\n"));
  }
  DEBUG ((DEBUG_WARN, "DALI: DISPLAY_INPUT_PROBE_READY\n"));
  // Keep timer-driven input alive while waiting for the manual recovery keys.
  for (;;) {
    if (In != NULL && !EFI_ERROR (In->ReadKeyStroke (In, &Key))) {
      Keys++;
      DEBUG ((DEBUG_WARN, "DALI: INPUT key=%lu scan=%x unicode=%x\n", Keys, Key.ScanCode, Key.UnicodeChar));
      if (Key.UnicodeChar == CHAR_CARRIAGE_RETURN) { LaunchUniXNU (); }
      if (Out != NULL) {
        UnicodeSPrint (Text, sizeof Text, L"Key %lu: scan=%x unicode=%x          \r\n", Keys, Key.ScanCode, Key.UnicodeChar);
        Out->OutputString (Out, Text);
      }
    }
    gBS->Stall (10000);
  }
}

STATIC VOID EFIAPI DiagnosticEntry (IN EFI_BDS_ARCH_PROTOCOL *This) {
  UINTN Size = 0, Key, DescriptorSize = 0;
  UINT32 Version;
  EFI_MEMORY_DESCRIPTOR *Map;
  EFI_STATUS Status;
  BOOLEAN MapOk;
  EFI_EVENT Event;
  DEBUG ((DEBUG_WARN, "DALI: BDS entered; DXE core, GIC and timer protocols dispatched\n"));
  EFI_TPL EntryTpl = gBS->RaiseTPL (TPL_HIGH_LEVEL);
  gBS->RestoreTPL (EntryTpl);
  DEBUG ((DEBUG_WARN, "DALI: BDS entry TPL=%lu; idle policy=polling\n", EntryTpl));
  if (EntryTpl != TPL_APPLICATION) {
    DEBUG ((DEBUG_ERROR, "DALI: UEFI_STAGE_FAIL BDS application TPL\n"));
    DiagnosticHalt ();
    return;
  }
  // Complete the standard DXE boundary before loading filesystem EFI images.
  // SecurityStub deliberately defers those images until this event is signaled.
  EfiEventGroupSignal (&gEfiEndOfDxeEventGroupGuid);
  Mt6991LogRegisters ();
  Status = gBS->GetMemoryMap (&Size, NULL, &Key, &DescriptorSize, &Version);
  if (Status != EFI_BUFFER_TOO_SMALL || DescriptorSize == 0) {
    DEBUG ((DEBUG_ERROR, "DALI: UEFI_DIAGNOSTIC_FAIL GetMemoryMap probe=%r\n", Status));
    DiagnosticHalt ();
    return;
  }
  Size += 4 * DescriptorSize;
  Map = AllocatePool (Size);
  if (Map == NULL) {
    DEBUG ((DEBUG_ERROR, "DALI: UEFI_DIAGNOSTIC_FAIL map allocation\n"));
    DiagnosticHalt ();
    return;
  }
  Status = gBS->GetMemoryMap (&Size, Map, &Key, &DescriptorSize, &Version);
  MapOk = !EFI_ERROR (Status);
  DEBUG ((DEBUG_WARN, "DALI: GetMemoryMap status=%r descriptors=%lu\n", Status, Size / DescriptorSize));
  FreePool (Map);
  Status = gBS->CreateEvent (EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK, Tick, NULL, &Event);
  if (!EFI_ERROR (Status)) {
    Status = gBS->SetTimer (Event, TimerPeriodic, 1000000); // 100 ms
    for (UINTN I = 0; !EFI_ERROR (Status) && I < 200 && mTicks < 5; I++) { gBS->Stall (10000); }
    gBS->SetTimer (Event, TimerCancel, 0);
    gBS->CloseEvent (Event);
  }
  DEBUG ((DEBUG_WARN, "DALI: timer status=%r interrupts=%lu\n", Status, mTicks));
  if (!EFI_ERROR (Status)) {
    Status = gBS->CreateEvent (EVT_TIMER, TPL_CALLBACK, NULL, NULL, &Event);
    if (!EFI_ERROR (Status)) {
      Status = gBS->SetTimer (Event, TimerRelative, 1000000);
      if (!EFI_ERROR (Status)) {
        UINTN Index;
        DEBUG ((DEBUG_WARN, "DALI: WaitForEvent timer probe starting\n"));
        Status = gBS->WaitForEvent (1, &Event, &Index);
      }
      gBS->CloseEvent (Event);
    }
    DEBUG ((DEBUG_WARN, "DALI: WaitForEvent timer status=%r\n", Status));
  }
  if (MapOk && !EFI_ERROR (Status) && mTicks >= 5) {
    DEBUG ((DEBUG_WARN, "DALI: UEFI_DIAGNOSTIC_PASS\n"));
  } else {
    DEBUG ((DEBUG_ERROR, "DALI: UEFI_DIAGNOSTIC_FAIL memory-or-timer\n"));
  }
  DisplayAndInputProbe ();
}

STATIC EFI_BDS_ARCH_PROTOCOL mBds = { DiagnosticEntry };
EFI_STATUS EFIAPI DiagnosticInitialize (IN EFI_HANDLE Image, IN EFI_SYSTEM_TABLE *SystemTable) {
  EFI_HANDLE Handle = NULL;
  mBdsImage = Image;
  DEBUG ((DEBUG_WARN, "DALI: diagnostic DXE loaded\n"));
  return gBS->InstallMultipleProtocolInterfaces (&Handle, &gEfiBdsArchProtocolGuid, &mBds, NULL);
}
