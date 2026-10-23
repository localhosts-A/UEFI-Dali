// SPDX-License-Identifier: BSD-2-Clause-Patent
// Single-core writer for Linux persistent_ram_buffer's on-RAM console format.
// This is not a UART: it never touches the MTK UART or display registers.
#include <Library/BaseLib.h>
#include <Library/ArmLib.h>
#include <Library/SerialPortLib.h>
#include <Library/Mt6991PlatformLib.h>

typedef struct {
  volatile UINT32 Signature;
  volatile UINT32 Start;
  volatile UINT32 Size;
  volatile UINT8 Data[];
} RAM_CONSOLE;

UINTN EFIAPI SerialPortWrite (IN UINT8 *Buffer, IN UINTN NumberOfBytes) {
  RAM_CONSOLE *Ring = (RAM_CONSOLE *)(UINTN)DALI_CONSOLE_BASE;
  UINT32 Capacity = DALI_CONSOLE_SIZE - 12;
  BOOLEAN Interrupts = ArmGetInterruptState ();
  ArmDisableInterrupts ();
  if (Ring->Signature != 0x43474244 || Ring->Start >= Capacity || Ring->Size > Capacity) {
    Ring->Start = 0;
    Ring->Size = 0;
    MemoryFence ();
    Ring->Signature = 0x43474244;
  }
  for (UINTN I = 0; I < NumberOfBytes; I++) {
    UINT32 At = Ring->Start;
    Ring->Data[At] = Buffer[I];
    Ring->Start = At + 1 == Capacity ? 0 : At + 1;
    if (Ring->Size < Capacity) { Ring->Size++; }
  }
  MemoryFence ();
  if (Interrupts) { ArmEnableInterrupts (); }
  return NumberOfBytes;
}
RETURN_STATUS EFIAPI SerialPortInitialize (VOID) { return RETURN_SUCCESS; }
UINTN EFIAPI SerialPortRead (OUT UINT8 *Buffer, IN UINTN NumberOfBytes) { return 0; }
BOOLEAN EFIAPI SerialPortPoll (VOID) { return FALSE; }
RETURN_STATUS EFIAPI SerialPortSetControl (IN UINT32 Control) { return RETURN_UNSUPPORTED; }
RETURN_STATUS EFIAPI SerialPortGetControl (OUT UINT32 *Control) { return RETURN_UNSUPPORTED; }
RETURN_STATUS EFIAPI SerialPortSetAttributes (IN OUT UINT64 *BaudRate, IN OUT UINT32 *ReceiveFifoDepth,
  IN OUT UINT32 *Timeout, IN OUT EFI_PARITY_TYPE *Parity, IN OUT UINT8 *DataBits,
  IN OUT EFI_STOP_BITS_TYPE *StopBits) { return RETURN_UNSUPPORTED; }
