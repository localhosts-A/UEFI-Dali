// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Library/DaliUsbI2cLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
enum { Vlp, Top, Apmixed, Impw, Perao, Gpio, Pins, Controller, Dma };
STATIC int Failed(DALI_USB_NATIVE_I2C *C, int Result) {
  C->Faulted = TRUE; C->Ready = FALSE; C->Gpio.level_known = false; return Result;
}
STATIC int Policy(DALI_USB_NATIVE_I2C *C, DALI_USB_I2C_POLICY_PHASE Phase) {
  if (C->Closed) { return USB_UNSUPPORTED; }
  EFI_STATUS Status = C->Policy(C->PolicyContext, Phase);
  if (C->Closed) { return USB_UNSUPPORTED; }
  return !EFI_ERROR(Status) ? USB_OK : Status == EFI_NOT_READY ? USB_BUSY :
    Status == EFI_UNSUPPORTED ? USB_UNSUPPORTED : Status == EFI_TIMEOUT ? USB_TIMEOUT : USB_IO;
}
STATIC int Quiesce(VOID *Context) {
  DALI_USB_NATIVE_I2C *C = Context;
  int Result = Policy(C,DaliUsbI2cQuiesce);
  return !Result && C->Faulted && !C->Cleaning ? USB_IO : Result;
}
STATIC BOOLEAN Access(DALI_USB_NATIVE_I2C_BANK *B, UINT32 Offset, UINT32 Width) {
  return !B->Owner->Closed && B->Range.Size >= Width && Offset <= B->Range.Size - Width;
}
STATIC int Read32(VOID *Context, UINT32 Offset, UINT32 *Out) {
  DALI_USB_NATIVE_I2C_BANK *B = Context;
  if (B->Owner->Closed) { return USB_UNSUPPORTED; }
  if ((Offset & 3) || !Access(B,Offset,4)) { return USB_RANGE; }
  MemoryFence(); UINT32 Value = MmioRead32((UINTN)B->Range.Base + Offset); MemoryFence();
  if (B->Owner->Closed) { return USB_UNSUPPORTED; }
  *Out = Value; return USB_OK;
}
STATIC int Write32(VOID *Context, UINT32 Offset, UINT32 Value) {
  DALI_USB_NATIVE_I2C_BANK *B = Context;
  if (B->Owner->Closed) { return USB_UNSUPPORTED; }
  if ((Offset & 3) || !Access(B,Offset,4)) { return USB_RANGE; }
  MemoryFence(); MmioWrite32((UINTN)B->Range.Base + Offset,Value); MemoryFence();
  return B->Owner->Closed ? USB_UNSUPPORTED : USB_OK;
}
STATIC int Read8(VOID *Context, UINT32 Offset, UINT8 *Out) {
  DALI_USB_NATIVE_I2C_BANK *B = Context;
  if (B->Owner->Closed) { return USB_UNSUPPORTED; }
  if (Offset != 0 || !Access(B,Offset,1)) { return USB_RANGE; }
  MemoryFence(); UINT8 Value = MmioRead8((UINTN)B->Range.Base); MemoryFence();
  if (B->Owner->Closed) { return USB_UNSUPPORTED; }
  *Out = Value; return USB_OK;
}
STATIC int Write8(VOID *Context, UINT32 Offset, UINT8 Value) {
  DALI_USB_NATIVE_I2C_BANK *B = Context;
  if (B->Owner->Closed) { return USB_UNSUPPORTED; }
  if (Offset != 0 || !Access(B,Offset,1)) { return USB_RANGE; }
  MemoryFence(); MmioWrite8((UINTN)B->Range.Base,Value); MemoryFence();
  return B->Owner->Closed ? USB_UNSUPPORTED : USB_OK;
}
STATIC VOID Barrier(VOID *Context) { (VOID)Context; MemoryFence(); }
STATIC VOID Delay(VOID *Context, UINT32 Us) {
  DALI_USB_NATIVE_I2C_BANK *B = Context;
  if (!B->Owner->Closed) { MicroSecondDelay(Us); }
}
STATIC struct usb_mmio Map(DALI_USB_NATIVE_I2C *C, UINTN Index, BOOLEAN Writable) {
  struct usb_mmio M = {&C->Bank[Index],C->Bank[Index].Range.Size,Read32,
    Writable ? Write32 : NULL,Barrier,Delay}; return M;
}
STATIC int Validate(DALI_USB_NATIVE_I2C *C, BOOLEAN Compare, BOOLEAN Enabled, BOOLEAN Cleanup) {
  int Result = Policy(C,DaliUsbI2cValidate);
  struct dali_usb_clock_rates Rates;
  struct dali_i2c6_pin_snapshot State;
  if (!Result) { Result = dali_usb_clock_rates_read(&C->Maps,C->SwVersion,C->ReferenceHz,C->UlposcHz,true,&Rates); }
  struct usb_mmio G = Map(C,Gpio,FALSE), P = Map(C,Pins,FALSE);
  if (!Result) { Result = dali_usb_i2c6_pins_read(&G,&P,&State); }
  UINT32 Parent;
  if (!Result) { Result = usb_mmio_read(&C->Maps.top,0xc0,&Parent); }
  if (!Result && (Parent & (1U << 23))) { Result = USB_UNSUPPORTED; }
  UINT32 Mode, Direction, Output;
  if (!Result) { Result = usb_mmio_read(&G,0x390,&Mode); }
  if (!Result && (Mode & 15)) { Result = USB_UNSUPPORTED; }
  if (!Result && C->Gpio.initialized && C->Gpio.level_known) {
    Result = usb_mmio_read(&G,0x20,&Direction);
    if (!Result) { Result = usb_mmio_read(&G,0x120,&Output); }
    if (!Result && (!(Direction & 0x100) || ((Output & 0x100) != 0) != C->Gpio.enabled)) { Result = USB_BUSY; }
  }
  if (!Result && Enabled && (Rates.enabled & 0x18) != 0x18) { Result = USB_BUSY; }
  if (!Result && Compare && CompareMem(&State,&C->Pins,sizeof State) != 0) { Result = USB_BUSY; }
  for (UINTN I = DALI_CLOCK_I2C6; !Result && Compare && I <= DALI_CLOCK_I2C_DMA; ++I) {
    if (Rates.hz[I] != C->Rates.hz[I] || Rates.parent[I] != C->Rates.parent[I]) { Result = USB_BUSY; }
  }
  if (!Result) { Result = Policy(C,DaliUsbI2cValidate); }
  if (!Result && C->Faulted && !Cleanup) { Result = USB_IO; }
  if (!Result && !Compare) { C->Rates = Rates; C->Pins = State; }
  return Result;
}
STATIC BOOLEAN Resources(CONST DALI_USB_CLOCK_RESOURCES *Clocks, CONST DALI_USB_I2C_RESOURCES *I2c) {
  CONST DALI_USB_MMIO_RANGE Expected[] = {{0x1c016000,0x1000},{0x10000000,0x800},{0x10000800,0x1000},
    {0x131b0000,0x1000},{0x16640000,0x1000}};
  for (UINTN I = 0; I < 5; ++I) {
    if (Clocks->Bank[I].Base != Expected[I].Base || Clocks->Bank[I].Size != Expected[I].Size) { return FALSE; }
  }
  return Clocks->ReferenceHz == 26000000 && Clocks->Calibration.qualified && Clocks->Calibration.sw_ver <= 1 &&
    I2c->Gpio.Base == 0x1002d000 && I2c->Gpio.Size == 0x1000 && I2c->IocfgLb2.Base == 0x13110000 &&
    I2c->IocfgLb2.Size == 0x1000 && I2c->Controller.Base == 0x13170000 && I2c->Controller.Size == 0x20000 &&
    I2c->Dma.Base == 0x16410000 && I2c->Dma.Size == 0x10000 &&
    I2c->SclPin == 104 && I2c->SdaPin == 105 && I2c->ResetPin == 72;
}
int DaliUsbNativeI2cOpen(DALI_USB_NATIVE_I2C *C, CONST DALI_USB_CLOCK_RESOURCES *Clocks,
  CONST DALI_USB_I2C_RESOURCES *I2c, DALI_USB_I2C_POLICY Guard, VOID *Context, UINT32 UlposcHz) {
  if (C == NULL || Clocks == NULL || I2c == NULL || Guard == NULL) { return USB_INVALID; }
  if (C->Closed) { return USB_UNSUPPORTED; }
  if (C->Initialized) { return USB_BUSY; }
  C->Initialized = TRUE; C->Ready = C->Faulted = C->WriteUncertain = C->Cleaning = FALSE;
  C->Policy = Guard; C->PolicyContext = Context; C->UlposcHz = UlposcHz;
  ZeroMem(&C->Rates,sizeof C->Rates); ZeroMem(&C->Pins,sizeof C->Pins);
  if (!Resources(Clocks,I2c)) { return Failed(C,USB_UNSUPPORTED); }
  C->SwVersion = Clocks->Calibration.sw_ver; C->ReferenceHz = Clocks->ReferenceHz;
  for (UINTN I = 0; I < 5; ++I) { C->Bank[I].Owner = C; C->Bank[I].Range = Clocks->Bank[I]; }
  CONST DALI_USB_MMIO_RANGE Ranges[] = {I2c->Gpio,I2c->IocfgLb2,{I2c->Controller.Base,0x1000},{I2c->Dma.Base,0x1000}};
  for (UINTN I = 0; I < 4; ++I) { C->Bank[I + 5].Owner = C; C->Bank[I + 5].Range = Ranges[I]; }
  C->Maps = (struct dali_usb_clock_maps){Map(C,Vlp,FALSE),Map(C,Top,FALSE),Map(C,Apmixed,FALSE),Map(C,Impw,TRUE),Map(C,Perao,TRUE)};
  int Result = Validate(C,FALSE,FALSE,FALSE);
  struct dali_repeater_guard G = {C,Quiesce};
  if (!Result) { Result = dali_i2c_clocks_init(&C->Clocks,&C->Maps.impw,&C->Maps.perao,&G,true); }
  if (!Result) { Result = dali_i2c_clocks_enable(&C->Clocks); }
  if (!Result) { Result = Validate(C,TRUE,TRUE,FALSE); }
  struct usb_mmio GpioMap = Map(C,Gpio,TRUE);
  if (!Result) { Result = dali_usb_gpio72_init(&C->Gpio,&GpioMap,&G,true); }
  struct dali_i2c6_io Io = {Map(C,Controller,TRUE),Map(C,Dma,TRUE),Read8,Write8};
  if (!Result) { Result = dali_usb_i2c6_init(&C->I2c,&Io,&G,C->Rates.hz[DALI_CLOCK_I2C6],true); }
  if (!Result) { Result = dali_nxp_io_bind(&C->Link,&C->I2c,&C->Gpio,&C->Bus); }
  if (!Result) { Result = Validate(C,TRUE,TRUE,FALSE); }
  if (Result) { return Failed(C,Result); }
  C->Ready = TRUE; return USB_OK;
}
int DaliUsbNativeI2cCheck(DALI_USB_NATIVE_I2C *C) {
  if (C == NULL || !C->Initialized) { return USB_INVALID; }
  if (C->Closed) { return USB_UNSUPPORTED; }
  if (C->Faulted) { return USB_IO; }
  if (!C->Ready) { return USB_INVALID; }
  int Result = Validate(C,TRUE,TRUE,FALSE); return Result ? Failed(C,Result) : USB_OK;
}
int DaliUsbNativeI2cRead(DALI_USB_NATIVE_I2C *C, UINT8 Reg, UINT8 *Out) {
  if (Out == NULL) { return USB_INVALID; }
  int Result = DaliUsbNativeI2cCheck(C); UINT8 Value;
  if (!Result) { Result = C->Bus.read8(C->Bus.context,Reg,&Value); }
  if (!Result) { Result = DaliUsbNativeI2cCheck(C); }
  if (Result) { return C != NULL && C->Initialized ? Failed(C,Result) : Result; }
  *Out = Value; return USB_OK;
}
int DaliUsbNativeI2cWrite(DALI_USB_NATIVE_I2C *C, UINT8 Reg, UINT8 Value) {
  int Result = DaliUsbNativeI2cCheck(C);
  if (Result) { return Result; }
  Result = C->Bus.write8(C->Bus.context,Reg,Value);
  BOOLEAN Completed = Result == USB_OK;
  if (!Result) { Result = DaliUsbNativeI2cCheck(C); }
  if (Result) {
    if (Completed || C->I2c.write_uncertain) { C->WriteUncertain = TRUE; }
    return Failed(C,Result);
  }
  return USB_OK;
}
int DaliUsbNativeI2cReset(DALI_USB_NATIVE_I2C *C, BOOLEAN Enable) {
  if (C == NULL || !C->Initialized || !C->Gpio.initialized) { return USB_INVALID; }
  if (C->Closed) { return USB_UNSUPPORTED; }
  if (Enable && C->Faulted) { return USB_IO; }
  C->Cleaning = !Enable;
  int Result = Validate(C,TRUE,TRUE,!Enable);
  if (!Result) { Result = dali_usb_gpio72_set(&C->Gpio,Enable != FALSE); }
  if (!Result) { Result = Validate(C,TRUE,TRUE,!Enable); }
  C->Cleaning = FALSE;
  if (Result) { return Failed(C,Result); }
  return USB_OK;
}
int DaliUsbNativeI2cRecover(DALI_USB_NATIVE_I2C *C) {
  if (C == NULL || !C->Initialized || !C->I2c.initialized) { return USB_INVALID; }
  if (C->Closed) { return USB_UNSUPPORTED; }
  C->Cleaning = TRUE;
  int Result = Validate(C,TRUE,TRUE,TRUE);
  if (!Result) { Result = dali_usb_i2c6_recover(&C->I2c); }
  if (!Result) { Result = Validate(C,TRUE,TRUE,TRUE); }
  C->Cleaning = FALSE;
  if (Result) { return Failed(C,Result); }
  return USB_OK;
}
int DaliUsbNativeI2cClose(DALI_USB_NATIVE_I2C *C) {
  if (C == NULL || !C->Initialized) { return USB_INVALID; }
  if (C->Closed) { return USB_UNSUPPORTED; }
  C->Cleaning = TRUE;
  int Result = Policy(C,DaliUsbI2cRelease);
  C->Ready = FALSE;
  if (!Result && C->Clocks.on) {
    Result = Validate(C,TRUE,TRUE,TRUE);
    if (!Result && C->I2c.initialized) { Result = dali_usb_i2c6_destroy(&C->I2c); }
    else if (!Result) {
      // A failed init may have observed foreign work without adopting it.
      // Never gate that engine off or reset it merely to close this lease.
      struct dali_i2c6_io Io = {Map(C,Controller,TRUE),Map(C,Dma,TRUE),Read8,Write8};
      Result = dali_usb_i2c6_check_idle(&Io);
    }
  }
  if (!Result && C->Gpio.initialized) { Result = dali_usb_gpio72_destroy(&C->Gpio); }
  // After partial clock enable no controller read is safe. The clocks' Quiesce
  // callback must prove all shared consumers stopped before restoring gates.
  if (!Result && C->Clocks.initialized) { Result = dali_i2c_clocks_destroy(&C->Clocks); }
  if (!Result && C->Closed) { Result = USB_UNSUPPORTED; }
  C->Cleaning = FALSE;
  if (Result) { return Failed(C,Result); }
  C->Initialized = FALSE; C->Policy = NULL; C->PolicyContext = NULL;
  return USB_OK;
}
int DaliUsbNativeI2cInfo(DALI_USB_NATIVE_I2C *C, DALI_USB_I2C_INFO *Out) {
  if (C == NULL || Out == NULL || !C->Initialized) { return USB_INVALID; }
  if (C->Closed) { return USB_UNSUPPORTED; }
  DALI_USB_I2C_INFO Value = {C->Rates.hz[DALI_CLOCK_I2C6],C->Rates.hz[DALI_CLOCK_I2C_DMA],
    C->Rates.parent[DALI_CLOCK_I2C6],C->Rates.parent[DALI_CLOCK_I2C_DMA],C->Clocks.owned,
    C->Ready,C->Faulted,C->WriteUncertain || C->I2c.write_uncertain,C->Gpio.level_known,C->Gpio.enabled,C->Pins};
  *Out = Value; return USB_OK;
}
VOID DaliUsbNativeI2cDelay(DALI_USB_NATIVE_I2C *C, UINT32 Us) {
  if (C != NULL && C->Initialized && !C->Closed && Us <= 10000) { MicroSecondDelay(Us); }
}
