// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Uefi.h>
#include <Protocol/GraphicsOutput.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/FdtLib.h>
#include <Library/FrameBufferBltLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/Mt6991PlatformLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Configuration/BootDevices.h>

// MT6991 offsets are from MiCode bsp-dali-v-oss mtk_disp_ovl_exdma.[ch].
// Reuse inherited scanout. Clock/reset/PHY/overlay configuration stays in LK.
#define FB_WIDTH  1280U
#define FB_HEIGHT 2772U
#define FB_PITCH  (FB_WIDTH * 4U)
STATIC EFI_GRAPHICS_OUTPUT_MODE_INFORMATION mInfo;
STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE mMode;
STATIC FRAME_BUFFER_CONFIGURE *mBlt;
STATIC CONST UINTN mMutexBanks[] = {0x32020000, 0x32420000, 0x32820000, 0x32C20000};
STATIC UINT64 mScanouts[4];
STATIC UINTN mScanoutCount, mMutexId, mRefreshCount;
STATIC UINT32 mMutexMask;
STATIC BOOLEAN mDirty, mRefreshActive, mObserveSubmit;
STATIC EFI_EVENT mRefreshEvent, mExitEvent;

STATIC VOID EFIAPI StopRefresh (EFI_EVENT Event, VOID *Context) {
  mRefreshActive = FALSE;
}

STATIC VOID EFIAPI Refresh (EFI_EVENT Event, VOID *Context) {
  UINT32 Status;
  if (!mRefreshActive) { return; }
  if (mObserveSubmit) {
    DEBUG ((DEBUG_WARN, "DALI: frame completion sample submit=%lu DSI=%x mutex-en=%x/%x/%x/%x\n",
      mRefreshCount, MmioRead32 (0x3249000C),
      MmioRead32 (mMutexBanks[0] + 0x20 + 0x20 * mMutexId),
      MmioRead32 (mMutexBanks[1] + 0x20 + 0x20 * mMutexId),
      MmioRead32 (mMutexBanks[2] + 0x20 + 0x20 * mMutexId),
      MmioRead32 (mMutexBanks[3] + 0x20 + 0x20 * mMutexId)));
    mObserveSubmit = FALSE;
  }
  if (!mDirty) { return; }
  Status = MmioRead32 (0x3249000C);
  if (Status & BIT31) { return; } // Never wait for or restart a busy transfer.
  // LK has two validated full-sized layers. Mirror the canonical GOP buffer
  // into those buffers so a still-selected LK overlay cannot cover new text.
  for (UINTN I = 0; I < mScanoutCount; I++) {
    if (mScanouts[I] != DALI_FB_BASE) {
      CopyMem ((VOID *)(UINTN)mScanouts[I], (VOID *)(UINTN)DALI_FB_BASE, mMode.FrameBufferSize);
      WriteBackDataCacheRange ((VOID *)(UINTN)mScanouts[I], mMode.FrameBufferSize);
    }
  }
  MemoryFence ();
  // MiCode mtk_disp_mutex_trigger and mtk_dsi_trigger: arm the inherited
  // mutex groups, then pulse DSI_START 0 -> 1. Do not enable any interrupts.
  for (UINTN I = 0; I < ARRAY_SIZE (mMutexBanks); I++) {
    if (mMutexMask & (1U << I)) { MmioWrite32 (mMutexBanks[I] + 0x20 + 0x20 * mMutexId, 1); }
  }
  MmioWrite32 (0x32490000, 0);
  MmioWrite32 (0x32490000, 1);
  MemoryFence ();
  mDirty = FALSE;
  mRefreshCount++;
  if (mRefreshCount <= 3) {
    mObserveSubmit = TRUE;
    DEBUG ((DEBUG_WARN, "DALI: frame submit=%lu mutex=%lu banks=%x DSI status=%x->%x\n",
      mRefreshCount, mMutexId, mMutexMask, Status, MmioRead32 (0x3249000C)));
  }
}

STATIC EFI_STATUS PrepareRefresh (VOID) {
  CONST VOID *Fdt = (VOID *)(UINTN)DALI_FDT_BASE;
  INT32 Length, Node = FdtPathOffset (Fdt, "/soc/disp1-mutex0@32020000");
  CONST UINT32 *Reg = FdtGetProp (Fdt, Node, "reg", &Length);
  UINTN Matches = 0;
  EFI_STATUS Status;
  if (Reg == NULL || Length != 64) { return EFI_UNSUPPORTED; }
  for (UINTN I = 0; I < ARRAY_SIZE (mMutexBanks); I++) {
    if (Fdt32ToCpu (Reg[I * 4]) != 0 || Fdt32ToCpu (Reg[I * 4 + 1]) != mMutexBanks[I] ||
        Fdt32ToCpu (Reg[I * 4 + 2]) != 0 || Fdt32ToCpu (Reg[I * 4 + 3]) != 0x1000) { return EFI_UNSUPPORTED; }
    DEBUG ((DEBUG_WARN, "DALI: mutex bank%lu cfg=%x inten=%x\n", I,
      MmioRead32 (mMutexBanks[I] + 8), MmioRead32 (mMutexBanks[I])));
  }
  for (UINTN Id = 0; Id < 10; Id++) {
    UINT32 Mod[4], Sof[4], Mask = 0;
    BOOLEAN Single = TRUE;
    for (UINTN I = 0; I < ARRAY_SIZE (mMutexBanks); I++) {
      UINTN Base = mMutexBanks[I] + Id * 0x20;
      Mod[I] = MmioRead32 (Base + 0x34);
      Sof[I] = MmioRead32 (Base + 0x30);
      if (Mod[I] != 0 || MmioRead32 (Base + 0x38) != 0) {
        Mask |= 1U << I;
        DEBUG ((DEBUG_WARN, "DALI: mutex id=%lu bank=%lu en=%x sof=%x mod0=%x mod2=%x\n",
          Id, I, MmioRead32 (Base + 0x20), Sof[I], Mod[I], MmioRead32 (Base + 0x38)));
        // SOF=0 selects command/single mode. CFG bit0 is not a mode test:
        // MiCode's ordinary CPU mutex-enable path preserves CFG (LK uses 3).
        if ((Sof[I] & 7) != 0) { Single = FALSE; }
      }
    }
    // MT6991: DISPSYS1 MOD0[23]=DSI0, OVLSYS0 MOD0[3:4]=EXDMA3/4.
    if (Single && (Mod[1] & BIT23) && (Mod[2] & (BIT3 | BIT4))) {
      mMutexId = Id; mMutexMask = Mask; Matches++;
    }
  }
  if (Matches != 1 || (MmioRead32 (0x32490034) & 3) != 0) {
    DEBUG ((DEBUG_WARN, "DALI: frame submission disabled; matching single-mode mutexes=%lu\n", Matches));
    return EFI_UNSUPPORTED;
  }
  Status = gBS->CreateEvent (EVT_SIGNAL_EXIT_BOOT_SERVICES, TPL_NOTIFY, StopRefresh, NULL, &mExitEvent);
  if (EFI_ERROR (Status)) { return Status; }
  Status = gBS->CreateEvent (EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK, Refresh, NULL, &mRefreshEvent);
  if (!EFI_ERROR (Status)) { Status = gBS->SetTimer (mRefreshEvent, TimerPeriodic, 1000000); }
  if (EFI_ERROR (Status)) {
    if (mRefreshEvent != NULL) { gBS->CloseEvent (mRefreshEvent); }
    gBS->CloseEvent (mExitEvent);
    return Status;
  }
  mRefreshActive = TRUE;
  DEBUG ((DEBUG_WARN, "DALI: frame submission enabled; mutex=%lu banks=%x scanouts=%lu\n",
    mMutexId, mMutexMask, mScanoutCount));
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI QueryMode (
  EFI_GRAPHICS_OUTPUT_PROTOCOL *This, UINT32 Number, UINTN *Size,
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **Info) {
  if (This == NULL || Size == NULL || Info == NULL) { return EFI_INVALID_PARAMETER; }
  if (Number != 0) { return EFI_UNSUPPORTED; }
  *Info = AllocateCopyPool (sizeof mInfo, &mInfo);
  if (*Info == NULL) { return EFI_OUT_OF_RESOURCES; }
  *Size = sizeof mInfo;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI Blt (
  EFI_GRAPHICS_OUTPUT_PROTOCOL *This, EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Buffer,
  EFI_GRAPHICS_OUTPUT_BLT_OPERATION Operation, UINTN Sx, UINTN Sy,
  UINTN Dx, UINTN Dy, UINTN Width, UINTN Height, UINTN Delta) {
  EFI_STATUS Status;
  EFI_TPL Tpl;
  if (This == NULL || mBlt == NULL) { return EFI_NOT_READY; }
  Tpl = gBS->RaiseTPL (TPL_NOTIFY);
  Status = FrameBufferBlt (mBlt, Buffer, Operation, Sx, Sy, Dx, Dy, Width, Height, Delta);
  if (!EFI_ERROR (Status) && Operation != EfiBltVideoToBltBuffer && Width != 0 && Height != 0) {
    WriteBackDataCacheRange ((VOID *)(UINTN)(mMode.FrameBufferBase + Dy * FB_PITCH + Dx * 4),
      (Height - 1) * FB_PITCH + Width * 4);
    MemoryFence ();
    mDirty = TRUE;
  }
  gBS->RestoreTPL (Tpl);
  return Status;
}

STATIC EFI_STATUS EFIAPI SetMode (EFI_GRAPHICS_OUTPUT_PROTOCOL *This, UINT32 Number) {
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL Black = {0};
  if (This == NULL) { return EFI_INVALID_PARAMETER; }
  if (Number != 0) { return EFI_UNSUPPORTED; }
  return Blt (This, &Black, EfiBltVideoFill, 0, 0, 0, 0, FB_WIDTH, FB_HEIGHT, 0);
}

STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL mGop = {QueryMode, SetMode, Blt, &mMode};

STATIC BOOLEAN CheckFramebuffer (VOID) {
  CONST VOID *Fdt = (VOID *)(UINTN)DALI_FDT_BASE;
  INT32 Chosen, Reserved, Length;
  CONST UINT32 *Low, *High, *Size, *Reg;
  if (FdtCheckHeader (Fdt)) { return FALSE; }
  Chosen = FdtPathOffset (Fdt, "/chosen");
  Reserved = FdtPathOffset (Fdt, "/reserved-memory/mblock-15-framebuffer");
  Low = FdtGetProp (Fdt, Chosen, "atag,videolfb-fb_base_l", &Length);
  if (Low == NULL || Length != 4 || Fdt32ToCpu (*Low) != DALI_FB_BASE) { return FALSE; }
  High = FdtGetProp (Fdt, Chosen, "atag,videolfb-fb_base_h", &Length);
  if (High == NULL || Length != 4 || Fdt32ToCpu (*High) != 0) { return FALSE; }
  Size = FdtGetProp (Fdt, Chosen, "atag,videolfb-vramSize", &Length);
  if (Size == NULL || Length != 4 || Fdt32ToCpu (*Size) != DALI_FB_SIZE) { return FALSE; }
  Reg = FdtGetProp (Fdt, Reserved, "reg", &Length);
  return Reg != NULL && Length == 16 && Fdt32ToCpu (Reg[0]) == 0 &&
    Fdt32ToCpu (Reg[1]) == DALI_FB_BASE && Fdt32ToCpu (Reg[2]) == 0 &&
    Fdt32ToCpu (Reg[3]) == DALI_FB_SIZE;
}

EFI_STATUS EFIAPI DaliDisplayInitialize (EFI_HANDLE Image, EFI_SYSTEM_TABLE *SystemTable) {
  EFI_STATUS Status;
  UINTN ConfigSize = 0;
  BOOLEAN Found = FALSE;
  if (!CheckFramebuffer ()) {
    DEBUG ((DEBUG_ERROR, "DALI: display FDT layout mismatch; GOP disabled\n"));
    return EFI_UNSUPPORTED;
  }
  for (UINTN I = 0; I < 3; I++) {
    UINTN Dsi = 0x32490000 + I * 0x10000;
    DEBUG ((DEBUG_WARN, "DALI: probing DSI%lu at %lx\n", I, Dsi));
    // MT6991 reg_20_ofs=0x20: CON_CTRL=0x30, MODE_CTRL=0x34.
    DEBUG ((DEBUG_WARN, "DALI: DSI%lu ctrl=%x mode=%x status=%x\n", I,
      MmioRead32 (Dsi + 0x30), MmioRead32 (Dsi + 0x34), MmioRead32 (Dsi + 0x0C)));
    DEBUG ((DEBUG_WARN, "DALI: DSI%lu start=%x irq=%x legacy-ctrl=%x legacy-mode=%x txrx=%x psctrl=%x\n", I,
      MmioRead32 (Dsi), MmioRead32 (Dsi + 8), MmioRead32 (Dsi + 0x10), MmioRead32 (Dsi + 0x14),
      MmioRead32 (Dsi + 0x38), MmioRead32 (Dsi + 0x3C)));
  }
  for (UINTN Side = 0; Side < 2; Side++) {
    for (UINTN Unit = 0; Unit < 10; Unit++) {
      UINTN Base;
      UINT32 Enabled, Layer, Size, Pitch, Format, Con;
      UINT64 Address;
      if (Unit == 1 || (Side == 1 && Unit == 0)) { continue; } // absent in the verified FDT
      Base = 0x32830000 + Side * 0x400000 + Unit * 0x10000;
      DEBUG ((DEBUG_WARN, "DALI: probing OVL%lu EXDMA%lu at %lx\n", Side, Unit, Base));
      Enabled = MmioRead32 (Base + 0x20);
      Layer = MmioRead32 (Base + 0x40);
      if (!(Enabled & BIT0) || !(Layer & BIT0)) { continue; }
      Size = MmioRead32 (Base + 0x48);
      Format = MmioRead32 (Base + 0x50);
      Pitch = MmioRead32 (Base + 0x2F4) & 0xFFFF;
      Con = MmioRead32 (Base + 0x300);
      Address = MmioRead32 (Base + 0xF40) | ((UINT64)(MmioRead32 (Base + 0xF4C) & 15) << 32);
      DEBUG ((DEBUG_WARN, "DALI: OVL%lu EXDMA%lu layer=%x size=%x pitch=%u fmt=%x con=%x addr=%lx\n",
        Side, Unit, Layer, Size, Pitch, Format, Con, Address));
      if ((Layer & (BIT4 | BIT8)) || (Con & BIT9) || Pitch != FB_PITCH ||
          (Size & 0xFFF) != FB_WIDTH || ((Size >> 16) & 0xFFF) != FB_HEIGHT ||
          Address < DALI_FB_BASE || Address > DALI_FB_BASE + DALI_FB_SIZE - (UINTN)FB_PITCH * FB_HEIGHT ||
          (Address - DALI_FB_BASE) % ((UINTN)FB_PITCH * ALIGN_VALUE (FB_HEIGHT, 32)) != 0 ||
          (Format & 0x3031F) != 2) { continue; }
      // This board's inherited format was measured as BGRX (fmt=2).
      BOOLEAN Duplicate = FALSE;
      for (UINTN J = 0; J < mScanoutCount; J++) { if (mScanouts[J] == Address) { Duplicate = TRUE; } }
      if (!Duplicate && mScanoutCount < ARRAY_SIZE (mScanouts)) { mScanouts[mScanoutCount++] = Address; }
      if (Address != DALI_FB_BASE) { continue; }
      if (Found && mInfo.PixelFormat != ((Format & BIT16) ?
          PixelRedGreenBlueReserved8BitPerColor : PixelBlueGreenRedReserved8BitPerColor)) {
        return EFI_UNSUPPORTED;
      }
      mInfo.PixelFormat = (Format & BIT16) ? PixelRedGreenBlueReserved8BitPerColor :
        PixelBlueGreenRedReserved8BitPerColor;
      Found = TRUE;
    }
  }
  if (!Found) {
    DEBUG ((DEBUG_WARN, "DALI: no validated linear LK scanout; GOP disabled\n"));
    return EFI_NOT_FOUND;
  }
  mInfo.HorizontalResolution = FB_WIDTH;
  mInfo.VerticalResolution = FB_HEIGHT;
  mInfo.PixelsPerScanLine = FB_PITCH / 4;
  mMode.MaxMode = 1;
  mMode.Info = &mInfo;
  mMode.SizeOfInfo = sizeof mInfo;
  mMode.FrameBufferBase = DALI_FB_BASE;
  mMode.FrameBufferSize = (UINTN)FB_PITCH * FB_HEIGHT;
  Status = FrameBufferBltConfigure ((VOID *)(UINTN)DALI_FB_BASE, &mInfo, NULL, &ConfigSize);
  if (Status != EFI_BUFFER_TOO_SMALL || ConfigSize == 0) { return EFI_DEVICE_ERROR; }
  mBlt = AllocatePool (ConfigSize);
  if (mBlt == NULL) { return EFI_OUT_OF_RESOURCES; }
  Status = FrameBufferBltConfigure ((VOID *)(UINTN)DALI_FB_BASE, &mInfo, mBlt, &ConfigSize);
  if (!EFI_ERROR (Status)) {
    Status = gBS->InstallMultipleProtocolInterfaces (&Image, &gEfiDevicePathProtocolGuid,
      &DisplayDevicePath, &gEfiGraphicsOutputProtocolGuid, &mGop, NULL);
  }
  if (EFI_ERROR (Status)) { FreePool (mBlt); mBlt = NULL; }
  DEBUG ((DEBUG_WARN, "DALI: GOP status=%r %ux%u pitch=%u format=%u\n",
    Status, FB_WIDTH, FB_HEIGHT, FB_PITCH, mInfo.PixelFormat));
  if (!EFI_ERROR (Status)) {
    EFI_STATUS RefreshStatus = PrepareRefresh ();
    DEBUG ((DEBUG_WARN, "DALI: refresh setup status=%r\n", RefreshStatus));
  }
  return Status;
}
