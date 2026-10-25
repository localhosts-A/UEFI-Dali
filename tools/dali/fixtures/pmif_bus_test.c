/* Actual DXE singleton and keypad source with modeled Boot Services/MMIO only.
 * Empty include shims are supplied by test_pmif_bus.py; no device is accessed. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef void VOID;
typedef uint8_t UINT8;
typedef uint16_t UINT16;
typedef uint32_t UINT32;
typedef uint64_t UINT64;
typedef size_t UINTN;
typedef int32_t INT32;
typedef char CHAR8;
typedef bool BOOLEAN;
typedef UINTN EFI_STATUS;
typedef UINTN EFI_TPL;
typedef VOID *EFI_EVENT;
typedef VOID *EFI_HANDLE;
typedef struct { UINT32 Value; } EFI_GUID;
typedef struct { UINT32 Unused; } EFI_SYSTEM_TABLE;
#define IN
#define OUT
#define STATIC static
#define CONST const
#define EFIAPI
#define TRUE true
#define FALSE false
#define TPL_APPLICATION 4U
#define TPL_CALLBACK 8U
#define TPL_NOTIFY 16U
#define TPL_HIGH_LEVEL 31U
#define EVT_NOTIFY_SIGNAL 0x200U
#define EFI_SUCCESS 0U
#define EFIERR(n) ((UINTN)(n) | ((UINTN)1 << 63))
#define EFI_INVALID_PARAMETER EFIERR(2)
#define EFI_UNSUPPORTED EFIERR(3)
#define EFI_NOT_READY EFIERR(6)
#define EFI_DEVICE_ERROR EFIERR(7)
#define EFI_OUT_OF_RESOURCES EFIERR(9)
#define EFI_NOT_FOUND EFIERR(14)
#define EFI_ACCESS_DENIED EFIERR(15)
#define EFI_TIMEOUT EFIERR(18)
#define EFI_ALREADY_STARTED EFIERR(20)
#define EFI_ERROR(s) (((s) >> 63) != 0)
#define BIT0 1U
#define BIT4 0x10U
#define BIT14 0x4000U
#define BIT18 0x40000U
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define DEBUG(args) ((void)0)
#define DALI_PMIF_BASE 0x1c01a000U

#include <Protocol/DaliPmicBus.h>
EFI_GUID gDaliPmicBusProtocolGuid = {1};
EFI_GUID gEfiEventExitBootServicesGuid = {2};
typedef VOID (*NOTIFY)(EFI_EVENT, VOID *);
typedef struct {
  EFI_TPL (*RaiseTPL)(EFI_TPL);
  VOID (*RestoreTPL)(EFI_TPL);
  EFI_STATUS (*LocateProtocol)(EFI_GUID *, VOID *, VOID **);
  EFI_STATUS (*CreateEventEx)(UINT32, EFI_TPL, NOTIFY, VOID *, EFI_GUID *, EFI_EVENT *);
  EFI_STATUS (*InstallMultipleProtocolInterfaces)(EFI_HANDLE *, ...);
  EFI_STATUS (*CloseEvent)(EFI_EVENT);
} EFI_BOOT_SERVICES;

static EFI_TPL current_tpl = TPL_APPLICATION;
static EFI_STATUS locate_error, create_error, install_error;
static DALI_PMIC_BUS_PROTOCOL *published;
static NOTIFY exit_notify;
static unsigned creates, installs, closes, commands, acks, data_reads, status_reads, delays;
static UINT32 sw_state;
static UINT8 key_raw = 0x1e;
static UINT8 mt6379[0xe00];
static UINT32 active_command;
static bool bad_fdt, bad_ranges, missing_mt6379, disabled_pmif, disabled_pmic, bad_cells, bad_names;
static UINT32 route_mask = 0x85c0;
static bool command_pending, respond = true, nack, hold_ack;
static unsigned response_after = 1, response_polls;

static EFI_TPL raise_tpl(EFI_TPL level)
{ assert(level >= current_tpl); EFI_TPL old = current_tpl; current_tpl = level; return old; }
static VOID restore_tpl(EFI_TPL old) { assert(old <= current_tpl); current_tpl = old; }
static EFI_STATUS locate(EFI_GUID *guid, VOID *registration, VOID **interface)
{
  assert(current_tpl <= TPL_NOTIFY && guid == &gDaliPmicBusProtocolGuid && !registration);
  if (locate_error) return locate_error;
  if (!published) return EFI_NOT_FOUND;
  *interface = published; return EFI_SUCCESS;
}
static EFI_STATUS create_event(UINT32 type, EFI_TPL tpl, NOTIFY handler, VOID *context, EFI_GUID *guid, EFI_EVENT *event)
{
  assert(current_tpl <= TPL_NOTIFY && type == EVT_NOTIFY_SIGNAL && tpl == TPL_NOTIFY && !context);
  assert(guid == &gEfiEventExitBootServicesGuid); ++creates;
  if (create_error) return create_error;
  exit_notify = handler; *event = &exit_notify; return EFI_SUCCESS;
}
static EFI_STATUS install(EFI_HANDLE *handle, ...)
{
  assert(current_tpl <= TPL_NOTIFY); ++installs;
  va_list args; va_start(args, handle);
  assert(va_arg(args, EFI_GUID *) == &gDaliPmicBusProtocolGuid);
  DALI_PMIC_BUS_PROTOCOL *interface = va_arg(args, DALI_PMIC_BUS_PROTOCOL *);
  assert(va_arg(args, VOID *) == NULL); va_end(args);
  if (install_error) return install_error;
  assert(!published); published = interface; *handle = &published; return EFI_SUCCESS;
}
static EFI_STATUS close_event(EFI_EVENT event)
{ assert(event == &exit_notify && current_tpl <= TPL_NOTIFY); ++closes; return EFI_SUCCESS; }
static EFI_BOOT_SERVICES services = {raise_tpl, restore_tpl, locate, create_event, install, close_event};
static EFI_BOOT_SERVICES *gBS = &services;
static UINT32 InterlockedCompareExchange32(volatile UINT32 *value, UINT32 compare, UINT32 exchange)
{ __atomic_compare_exchange_n(value, &compare, exchange, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); return compare; }

#include "PmicBusDxe.c"

static UINT32 MmioRead32(UINTN address)
{
  if (address == DALI_PMIF_BASE) return 1;
  assert(current_tpl == TPL_HIGH_LEVEL && mBusy == 1 && mOwner);
  if (address == DALI_PMIF_BASE + 0x8a8) {
    ++status_reads;
    if (command_pending && respond && ++response_polls >= response_after) sw_state = 12U | (nack ? BIT18 : 0);
    return sw_state;
  }
  assert(address == DALI_PMIF_BASE + 0x894); ++data_reads;
  if (((active_command >> 24) & 15) == 4) return key_raw;
  UINT32 reg = active_command & 0xffff;
  UINT32 value = mt6379[reg];
  if (active_command & 0x10000) value |= (UINT32)mt6379[reg + 1] << 8;
  return value;
}
static UINT32 MmioWrite32(UINTN address, UINT32 value)
{
  assert(current_tpl == TPL_HIGH_LEVEL && mBusy == 1 && mOwner);
  if (address == DALI_PMIF_BASE + 0x880) {
    assert((value == 0xc400001e || (value & 0xdffe0000U) == 0xce000000U) && sw_state == 0 && !command_pending);
    assert((value & 0xffff) <= 0xdfe); active_command = value;
    command_pending = true; response_polls = 0; sw_state = 2; ++commands;
  } else {
    assert(address == DALI_PMIF_BASE + 0x8a4 && value == 1 && (sw_state & 14) == 12);
    command_pending = false; if (!hold_ack) sw_state = 0; ++acks;
  }
  return value;
}
static UINTN MicroSecondDelay(UINTN us)
{ assert(current_tpl == TPL_HIGH_LEVEL && (us == 10 || us == 8)); ++delays; return us; }
static VOID MemoryFence(VOID) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }
static VOID *ZeroMem(VOID *buffer, UINTN length) { return memset(buffer, 0, length); }
static INT32 FdtCheckHeader(CONST VOID *fdt) { return !fdt || bad_fdt ? -1 : 0; }
/* FDT API seam supplies one valid fixed board contract; production owns the
 * exact compatibility/resource/routing/key checks. No parser is replaced in FW. */
static INT32 FdtPathOffset(CONST VOID *fdt, CONST CHAR8 *path)
{
  assert(fdt);
  if (!strcmp(path, "/")) return 0;
  if (!strcmp(path, "/spmi@1c01a000")) return 1;
  if (!strcmp(path, "/spmi@1c01a000/pmic@4")) return 2;
  if (!strcmp(path, "/spmi@1c01a000/pmic@4/mt6363keys")) return 3;
  if (!strcmp(path, "/spmi@1c01a000/pmic@4/mt6363keys/power")) return 4;
  if (!strcmp(path, "/spmi@1c01a000/pmic@4/mt6363keys/home")) return 5;
  if (!strcmp(path, "/spmi@1c01a000/mt6379@e")) return missing_mt6379 ? -1 : 6;
  return -1;
}
static INT32 FdtStringListSearch(CONST VOID *fdt, INT32 node, CONST CHAR8 *property, CONST CHAR8 *value)
{
  assert(fdt);
  if (!strcmp(property, "reg-names")) {
    if (bad_names || node != 1) return -1;
    return !strcmp(value,"pmif") ? 0 : !strcmp(value,"spmimst") ? 1 : !strcmp(value,"pmif-p") ? 2 : !strcmp(value,"spmimst-p") ? 3 : -1;
  }
  assert(!strcmp(property, "compatible"));
  return (node == 1 && !strcmp(value, "mediatek,mt6991-spmi")) ||
    (node == 2 && !strcmp(value, "mediatek,mt6363")) ||
    (node == 3 && !strcmp(value, "mediatek,mt6363-keys")) ||
    (node == 6 && !strcmp(value, "mediatek,mt6379")) ? 0 : -1;
}
static CONST VOID *FdtGetProp(CONST VOID *fdt, INT32 node, CONST CHAR8 *property, INT32 *length)
{
  static CONST UINT32 ranges[] = {0,0x1c01a000,0,0x8ff,0,0x1c01c000,0,0x100,0,0x1c018000,0,0x8ff,0,0x1c01c800,0,0x100};
  static CONST UINT32 sid[] = {4,0}, usb_sid[] = {14,0}, power = 116, home = 114;
  static CONST UINT32 two = 2, zero = 0, invalid = 1;
  assert(fdt);
  if (!strcmp(property, "status")) {
    if ((node == 1 && disabled_pmif) || (node == 6 && disabled_pmic)) { *length = 9; return "disabled"; }
    *length = 0; return NULL;
  }
  if ((node == 0 || node == 1) && !strcmp(property, "#address-cells")) { *length = 4; return bad_cells ? &invalid : &two; }
  if ((node == 0 || node == 1) && !strcmp(property, "#size-cells")) { *length = 4; return bad_cells ? &invalid : node == 0 ? &two : &zero; }
  if (node == 1 && !strcmp(property, "reg")) { *length = bad_ranges ? 4 : sizeof(ranges); return ranges; }
  if (node == 1 && !strcmp(property, "spmi-dev-mask")) { *length = 4; return &route_mask; }
  if (node == 2 && !strcmp(property, "reg")) { *length = sizeof(sid); return sid; }
  if (node == 6 && !strcmp(property, "reg")) { *length = sizeof(usb_sid); return usb_sid; }
  if ((node == 4 || node == 5) && !strcmp(property, "linux,keycodes")) { *length = 4; return node == 4 ? &power : &home; }
  *length = 0; return NULL;
}
static UINT32 Fdt32ToCpu(UINT32 value) { return value; }

#include "PmicKeys.c"
#include "UsbPmicLib.c"

static VOID test_publication(VOID)
{
  locate_error = EFI_DEVICE_ERROR;
  assert(DaliPmicBusInitialize(NULL, NULL) == EFI_DEVICE_ERROR && !creates);
  locate_error = 0; create_error = EFI_OUT_OF_RESOURCES;
  assert(DaliPmicBusInitialize(NULL, NULL) == EFI_OUT_OF_RESOURCES && creates == 1 && !installs);
  create_error = 0; install_error = EFI_OUT_OF_RESOURCES;
  assert(DaliPmicBusInitialize(NULL, NULL) == EFI_OUT_OF_RESOURCES && closes == 1 && !published);
  install_error = 0;
  assert(DaliPmicBusInitialize(NULL, NULL) == EFI_SUCCESS && published && published->Revision == 1);
  unsigned before = creates;
  assert(DaliPmicBusInitialize(NULL, NULL) == EFI_ALREADY_STARTED && creates == before);
}
static VOID test_leases(VOID)
{
  DALI_PMIC_BUS_TOKEN usb = {0}, key = {0}, copy;
  assert(published->TryAcquire(NULL, &usb) == EFI_INVALID_PARAMETER);
  assert(published->TryAcquire(published, NULL) == EFI_INVALID_PARAMETER);
  current_tpl = TPL_CALLBACK;
  assert(published->TryAcquire(published, &usb) == EFI_SUCCESS && current_tpl == TPL_HIGH_LEVEL && usb.Active);
  assert(published->TryAcquire(published, &key) == EFI_NOT_READY && current_tpl == TPL_HIGH_LEVEL && !key.Active);
  copy = usb;
  assert(published->Release(published, &copy, false) == EFI_ACCESS_DENIED && current_tpl == TPL_HIGH_LEVEL);
  assert(published->Release(published, &usb, true) == EFI_SUCCESS && current_tpl == TPL_CALLBACK && !usb.Active);
  assert(published->TryAcquire(published, &key) == EFI_NOT_READY && current_tpl == TPL_CALLBACK);
  assert(published->TryAcquire(published, &usb) == EFI_SUCCESS);
  assert(published->Release(published, &usb, false) == EFI_SUCCESS && current_tpl == TPL_CALLBACK);
  assert(published->Release(published, &usb, false) == EFI_ACCESS_DENIED);
  current_tpl = TPL_NOTIFY;
  assert(published->TryAcquire(published, &key) == EFI_SUCCESS);
  assert(published->Release(published, &key, false) == EFI_SUCCESS && current_tpl == TPL_NOTIFY);
}
static VOID test_keypad(VOID)
{
  UINT8 raw = 0x55;
  UINT64 now = 20000000;
  DALI_PMIC_BUS_TOKEN usb = {0};
  DaliPmicKeysInitialize(&raw);
  assert(mReady);
  assert(DaliPmicKeysPoll(now, &raw) == EFI_SUCCESS && raw == 0x1e && current_tpl == TPL_NOTIFY);
  assert(commands == 1 && acks == 1);
  assert(published->TryAcquire(published, &usb) == EFI_SUCCESS);
  unsigned before = status_reads;
  raw = 0x55;
  assert(DaliPmicKeysPoll(now += 20000000, &raw) == EFI_NOT_READY && mReady && raw == 0x55 && status_reads == before);
  assert(published->Release(published, &usb, false) == EFI_SUCCESS);
  assert(DaliPmicKeysPoll(now += 20000000, &raw) == EFI_SUCCESS && raw == 0x1e);
  sw_state = 12; before = acks; unsigned reads = data_reads;
  assert(DaliPmicKeysPoll(now += 20000000, &raw) == EFI_NOT_READY && mReady && acks == before && data_reads == reads);
  sw_state = 0; respond = false; raw = 0x55; before = delays;
  assert(DaliPmicKeysPoll(now += 20000000, &raw) == EFI_TIMEOUT && raw == 0x55 && mReady && mPendingOwned);
  assert(delays - before == 1000 && current_tpl == TPL_NOTIFY && !mBusToken.Active);
  // Even if our timed-out read vanishes into idle, another client cannot start
  // before we explicitly clear our own retained transaction ownership.
  sw_state = 0; command_pending = false;
  assert(published->TryAcquire(published, &usb) == EFI_NOT_READY);
  assert(DaliPmicKeysPoll(now += 20000000, &raw) == EFI_NOT_READY && !mPendingOwned && raw == 0x55);
  assert(published->TryAcquire(published, &usb) == EFI_SUCCESS);
  assert(published->Release(published, &usb, false) == EFI_SUCCESS);
  respond = true;
  assert(DaliPmicKeysPoll(now += 20000000, &raw) == EFI_SUCCESS && raw == 0x1e);
  respond = false; raw = 0x55;
  assert(DaliPmicKeysPoll(now += 20000000, &raw) == EFI_TIMEOUT && mPendingOwned);
  sw_state = 12; before = acks;
  assert(DaliPmicKeysPoll(now += 20000000, &raw) == EFI_NOT_READY && !mPendingOwned && acks == before + 1 && raw == 0x55);
  respond = true; hold_ack = true;
  assert(DaliPmicKeysPoll(now += 20000000, &raw) == EFI_TIMEOUT && mPendingOwned && raw == 0x55);
  assert(published->TryAcquire(published, &usb) == EFI_NOT_READY);
  hold_ack = false;
  assert(DaliPmicKeysPoll(now += 20000000, &raw) == EFI_NOT_READY && !mPendingOwned && raw == 0x55);
  nack = true;
  assert(DaliPmicKeysPoll(now += 20000000, &raw) == EFI_DEVICE_ERROR && raw == 0x55 && !mReady && !mPendingOwned);
  assert(published->TryAcquire(published, &usb) == EFI_SUCCESS);
  assert(published->Release(published, &usb, false) == EFI_SUCCESS);
}
static VOID test_ebs(VOID)
{
  DALI_PMIC_BUS_TOKEN usb = {0};
  unsigned before = commands;
  exit_notify(&exit_notify, NULL);
  assert(published->TryAcquire(published, &usb) == EFI_NOT_READY && current_tpl == TPL_NOTIFY && !usb.Active);
  exit_notify(&exit_notify, NULL);
  assert(published->TryAcquire(published, &usb) == EFI_NOT_READY && commands == before);
}
static VOID test_usb_binding(VOID)
{
  DALI_USB_PMIC context = {0}; UINT8 raw = 0x55, bytes[2] = {0xa5, 0x5a};
  unsigned before = commands;
  assert(DaliUsbPmicOpen(NULL, &raw) == EFI_INVALID_PARAMETER);
  bad_fdt = true;
  assert(DaliUsbPmicOpen(&context, &raw) == EFI_UNSUPPORTED);
  bad_fdt = false; bad_ranges = true;
  assert(DaliUsbPmicOpen(&context, &raw) == EFI_UNSUPPORTED);
  bad_ranges = false; route_mask |= BIT14;
  assert(DaliUsbPmicOpen(&context, &raw) == EFI_UNSUPPORTED);
  route_mask &= ~BIT14; missing_mt6379 = true;
  assert(DaliUsbPmicOpen(&context, &raw) == EFI_UNSUPPORTED && commands == before);
  missing_mt6379 = false; nack = false; mt6379[0] = 0x73; mt6379[0x400] = 0xcf; mt6379[0x401] = 0x29;
  disabled_pmif = true;
  assert(DaliUsbPmicOpen(&context, &raw) == EFI_UNSUPPORTED);
  disabled_pmif = false; disabled_pmic = true;
  assert(DaliUsbPmicOpen(&context, &raw) == EFI_UNSUPPORTED);
  disabled_pmic = false; bad_cells = true;
  assert(DaliUsbPmicOpen(&context, &raw) == EFI_UNSUPPORTED);
  bad_cells = false; bad_names = true;
  assert(DaliUsbPmicOpen(&context, &raw) == EFI_UNSUPPORTED && commands == before);
  bad_names = false;
  assert(DaliUsbPmicOpen(&context, &raw) == EFI_SUCCESS && context.Ready && context.Transport.revision == 3 && !context.Token.Active);
  assert(DaliUsbPmicOpen(&context, &raw) == EFI_ALREADY_STARTED);
  assert(dali_mt6379_read(&context.Transport, 0x400, bytes, 2) == USB_OK && bytes[0] == 0xcf && bytes[1] == 0x29);
  DaliPmicKeysInitialize(&raw); /* Re-enable keypad after the deliberate NACK. */
  respond = false; bytes[0] = 0xa5; before = commands;
  assert(dali_mt6379_read(&context.Transport, 0x400, bytes, 2) == USB_TIMEOUT && context.Transport.pending_owned && bytes[0] == 0xa5);
  assert(mReserved == &context.Token && !context.Token.Active && current_tpl == TPL_NOTIFY);
  assert(DaliPmicKeysPoll(1000000000, &raw) == EFI_NOT_READY && commands == before + 1);
  assert(DaliUsbPmicClose(&context) == EFI_TIMEOUT && context.Bus && context.Transport.pending_owned);
  respond = true;
  assert(DaliUsbPmicClose(&context) == EFI_SUCCESS && !context.Bus && !mReserved);
  assert(DaliPmicKeysPoll(1020000000, &raw) == EFI_SUCCESS && raw == 0x1e);
  respond = false;
  assert(DaliPmicKeysPoll(1040000000, &raw) == EFI_TIMEOUT && mPendingOwned);
  before = commands;
  assert(DaliUsbPmicOpen(&context, &raw) == EFI_NOT_READY && !context.Bus && commands == before);
  respond = true;
  assert(DaliPmicKeysPoll(1060000000, &raw) == EFI_NOT_READY && !mPendingOwned && !mReserved);
  // A partial open can own an unanswered identification read, and must retain
  // its context until close explicitly recovers that same command.
  respond = false;
  assert(DaliUsbPmicOpen(&context, &raw) == EFI_TIMEOUT && context.Bus && !context.Ready && context.Transport.pending_owned);
  respond = true;
  assert(DaliUsbPmicClose(&context) == EFI_SUCCESS && !mReserved);
  mt6379[0] = 0;
  assert(DaliUsbPmicOpen(&context, &raw) == EFI_UNSUPPORTED && context.Transport.initialized && !context.Ready);
  assert(DaliUsbPmicClose(&context) == EFI_SUCCESS && !context.Bus);
  assert(DaliUsbPmicClose(&context) == EFI_SUCCESS);
}
int main(void)
{
  UINT8 raw = 0x55;
  DaliPmicKeysInitialize(&raw);
  assert(DaliPmicKeysPoll(0, &raw) == EFI_NOT_READY && mReady && raw == 0x55 && !commands);
  test_publication(); test_leases(); test_keypad(); test_usb_binding(); test_ebs();
  puts("PASS: production DXE shared PMIF leases/keypad ownership/TPL/EBS; Boot Services/MMIO model only");
  return 0;
}
