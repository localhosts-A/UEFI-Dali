// Actual DXE worker and native host lifecycle with modeled Dali resources.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define main io_model_regressions
#include "usb_xhci_io_test.c"
#undef main
#define DALI_USB_NATIVE_WORKER_TEST
#define main native_host_model_main
#include "usb_native_host_test.c"
#undef main
#include <Library/DaliUsbNativeHostWorker.h>

EFI_GUID gDaliUsbResourceContextProtocolGuid =
  {0x5bc615a2, 0x3e49, 0x4d9e, {0x86, 0x59, 0x7b, 0xdd, 0xe4, 0x8d, 0xc9, 0xc6}};
EFI_GUID gDaliUsbNativeHostProviderProtocolGuid =
  {0xa5127a9e, 0x691e, 0x4b22, {0xa9, 0x79, 0x11, 0xa4, 0xf8, 0x27, 0x10, 0xd1}};
EFI_GUID gDaliUsbNativeHostWorkerProtocolGuid =
  {0xe94a1771, 0x7db7, 0x4825, {0xbd, 0x2f, 0xf7, 0x3f, 0xb3, 0x69, 0xdc, 0xe0}};

static DALI_USB_NATIVE_HOST_WORKER worker;
static DALI_USB_RESOURCE_CONTEXT_PROTOCOL resources;
static DALI_USB_NATIVE_HOST_PROVIDER_PROTOCOL provider;
static DALI_USB_RESOURCE_CONTEXT_STATUS context_status;
static DALI_USB_XHCI_RESOURCES xhci_resources;
static UINTN acquire_calls, release_calls, acquire_active_on_error;
static EFI_STATUS context_status_result, xhci_result, acquire_result, release_result;
static UINT64 qualification_mask;
static UINT64 lease_base, lease_size;
static UINT8 port_count, ports[DALI_USB_NATIVE_HOST_MAX_PORTS];

static EFI_STATUS EFIAPI get_status(DALI_USB_RESOURCE_CONTEXT_PROTOCOL *This,
  DALI_USB_RESOURCE_CONTEXT_STATUS *Out) {
  assert(This == &resources && Out != NULL);
  if (EFI_ERROR(context_status_result)) { return context_status_result; }
  *Out = context_status; return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI get_xhci(DALI_USB_RESOURCE_CONTEXT_PROTOCOL *This,
  DALI_USB_XHCI_RESOURCES *Out) {
  assert(This == &resources && Out != NULL);
  if (EFI_ERROR(xhci_result)) { return xhci_result; }
  *Out = xhci_resources; return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI acquire(DALI_USB_NATIVE_HOST_PROVIDER_PROTOCOL *This,
  CONST DALI_USB_XHCI_RESOURCES *Expected, DALI_USB_NATIVE_HOST_LEASE *Lease) {
  assert(This == &provider && Expected != NULL && Lease != NULL);
  assert(Expected->Registers.Base == 0x16700000 && Expected->Registers.Size == 0x1000);
  assert(Expected->InterruptSpecifier[0] == 0 && Expected->InterruptSpecifier[1] == 600 &&
    Expected->InterruptSpecifier[2] == 4 && Expected->InterruptSpecifier[3] == 0);
  acquire_calls++;
  if (EFI_ERROR(acquire_result)) {
    if (acquire_active_on_error) {
      Lease->Revision = DALI_USB_NATIVE_HOST_LEASE_REVISION;
      Lease->Size = sizeof(*Lease);
      Lease->Active = TRUE;
    }
    return acquire_result;
  }
  Lease->Revision = DALI_USB_NATIVE_HOST_LEASE_REVISION;
  Lease->Size = sizeof(*Lease);
  Lease->QualificationMask = qualification_mask;
  Lease->RegisterBase = lease_base;
  Lease->RegisterLength = lease_size;
  Lease->Dma = (DALI_USB_XHCI_DMA_OPS){&m, TRUE, allocate, sync_buffer, free_buffer};
  Lease->Guard = guard;
  Lease->GuardContext = &m;
  Lease->Usb2PortCount = port_count;
  CopyMem(Lease->Usb2Ports, ports, sizeof(ports));
  Lease->Active = TRUE;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI release(DALI_USB_NATIVE_HOST_PROVIDER_PROTOCOL *This,
  DALI_USB_NATIVE_HOST_LEASE *Lease) {
  assert(This == &provider && Lease != NULL && Lease->Active);
  release_calls++;
  if (EFI_ERROR(release_result)) { return release_result; }
  Lease->Active = FALSE;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI worker_install(EFI_HANDLE *handle, ...) {
  assert(handle != NULL);
  va_list args; va_start(args, handle);
  BOOLEAN hc = FALSE, path = FALSE, handoff = FALSE;
  EFI_GUID *guid;
  while ((guid = va_arg(args, EFI_GUID *)) != NULL) {
    VOID *interface = va_arg(args, VOID *);
    if (guid == &gEfiUsb2HcProtocolGuid) { assert(interface == &worker.Host.Protocol); hc = TRUE; }
    else if (guid == &gEfiDevicePathProtocolGuid) {
      assert(interface == &worker.Path.VendorNode && worker.Path.VendorNode.Type == HARDWARE_DEVICE_PATH &&
        worker.Path.VendorNode.SubType == HW_VENDOR_DP && worker.Path.VendorNode.Length[0] == 20 &&
        worker.Path.End.Type == END_DEVICE_PATH_TYPE && worker.Path.End.SubType == END_ENTIRE_DEVICE_PATH_SUBTYPE);
      path = TRUE;
    } else if (guid == &gDaliUsbNativeHostHandoffProtocolGuid) {
      assert(interface == &worker.Host.HandoffProtocol); handoff = TRUE;
    } else { assert(FALSE); }
  }
  va_end(args); assert(hc && path && handoff);
  *handle = &worker.Host;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI worker_uninstall(EFI_HANDLE handle, ...) {
  assert(handle == &worker.Host);
  va_list args; va_start(args, handle);
  BOOLEAN hc = FALSE, path = FALSE, handoff = FALSE;
  EFI_GUID *guid;
  while ((guid = va_arg(args, EFI_GUID *)) != NULL) {
    VOID *interface = va_arg(args, VOID *);
    if (guid == &gEfiUsb2HcProtocolGuid) { assert(interface == &worker.Host.Protocol); hc = TRUE; }
    else if (guid == &gEfiDevicePathProtocolGuid) { assert(interface == &worker.Path.VendorNode); path = TRUE; }
    else if (guid == &gDaliUsbNativeHostHandoffProtocolGuid) { assert(interface == &worker.Host.HandoffProtocol); handoff = TRUE; }
    else { assert(FALSE); }
  }
  va_end(args); assert(hc && path && handoff);
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI worker_disconnect(EFI_HANDLE handle, EFI_HANDLE driver, EFI_HANDLE child) {
  assert(handle == &worker.Host && driver == NULL && child == NULL);
  return EFI_SUCCESS;
}

static void worker_setup(void) {
  native_reset();
  ZeroMem(&worker, sizeof(worker)); ZeroMem(&resources, sizeof(resources));
  ZeroMem(&provider, sizeof(provider));
  context_status = (DALI_USB_RESOURCE_CONTEXT_STATUS){EFI_SUCCESS, 10, 10, TRUE, FALSE};
  xhci_resources.Registers = (DALI_USB_MMIO_RANGE){0x16700000, 0x1000};
  xhci_resources.InterruptSpecifier[0] = 0; xhci_resources.InterruptSpecifier[1] = 600;
  xhci_resources.InterruptSpecifier[2] = 4; xhci_resources.InterruptSpecifier[3] = 0;
  context_status_result = xhci_result = acquire_result = release_result = EFI_SUCCESS;
  acquire_calls = release_calls = acquire_active_on_error = 0;
  qualification_mask = DALI_USB_HOST_REQUIRED_QUALIFICATIONS;
  lease_base = 0x16700000; lease_size = 0x1000;
  port_count = 2; ports[0] = 1; ports[1] = 0;
  resources.Revision = 6; resources.GetStatus = get_status; resources.GetXhciResources = get_xhci;
  provider.Revision = DALI_USB_NATIVE_HOST_PROVIDER_REVISION;
  provider.Acquire = acquire; provider.Release = release;
  services.InstallMultipleProtocolInterfaces = worker_install;
  services.UninstallMultipleProtocolInterfaces = worker_uninstall;
  services.DisconnectController = worker_disconnect;
}

static void resource_and_role_refusal(void) {
  worker_setup(); tpl = TPL_CALLBACK;
  assert(DaliUsbNativeHostWorkerStart(&worker, &resources, &provider) == EFI_UNSUPPORTED);
  assert(acquire_calls == 0 && !m.reads && !m.writes);
  tpl = TPL_APPLICATION;

  worker_setup(); resources.Revision = 5;
  assert(DaliUsbNativeHostWorkerStart(&worker, &resources, &provider) == EFI_UNSUPPORTED);
  assert(acquire_calls == 0 && !m.reads && !m.writes);

  worker_setup(); provider.Revision = 1;
  assert(DaliUsbNativeHostWorkerStart(&worker, &resources, &provider) == EFI_UNSUPPORTED);
  assert(acquire_calls == 0 && !m.reads && !m.writes);

  worker_setup(); context_status.InitializationStatus = EFI_DEVICE_ERROR;
  assert(DaliUsbNativeHostWorkerStart(&worker, &resources, &provider) == EFI_DEVICE_ERROR);
  assert(acquire_calls == 0 && release_calls == 0 && !m.reads && !m.writes);

  worker_setup(); context_status.SnapshotValid = FALSE;
  assert(DaliUsbNativeHostWorkerStart(&worker, &resources, &provider) == EFI_NOT_READY);
  assert(worker.Protocol.State == DaliUsbHostWorkerWaitingForRole && acquire_calls == 0 && !m.reads && !m.writes);

  worker_setup(); xhci_resources.InterruptSpecifier[1] = 599;
  assert(DaliUsbNativeHostWorkerStart(&worker, &resources, &provider) == EFI_UNSUPPORTED);
  assert(worker.Protocol.State == DaliUsbHostWorkerFailed && acquire_calls == 0 && !release_calls && !m.reads && !m.writes);

  worker_setup(); acquire_result = EFI_NOT_READY;
  assert(DaliUsbNativeHostWorkerStart(&worker, &resources, &provider) == EFI_NOT_READY);
  assert(worker.Protocol.State == DaliUsbHostWorkerWaitingForRole && acquire_calls == 1 && !release_calls && !m.reads && !m.writes);

  worker_setup(); acquire_result = EFI_TIMEOUT; acquire_active_on_error = 1;
  assert(DaliUsbNativeHostWorkerStart(&worker, &resources, &provider) == EFI_TIMEOUT);
  assert(release_calls == 1 && !worker.Lease.Active && !m.reads && !m.writes);
}

static void unqualified_lease_refusal(void) {
  for (UINTN n = 0; n < 4; n++) {
    worker_setup();
    if (n == 0) { qualification_mask &= ~DALI_USB_HOST_QUAL_IRQ; }
    if (n == 1) { lease_base++; }
    if (n == 2) { port_count = 2; ports[0] = ports[1] = 1; }
    if (n == 3) { qualification_mask &= ~DALI_USB_HOST_QUAL_DMA32; }
    assert(DaliUsbNativeHostWorkerStart(&worker, &resources, &provider) == EFI_UNSUPPORTED);
    assert(release_calls == 1 && !worker.Lease.Active && !m.reads && !m.writes);
  }
}

static void start_stop_and_retention(void) {
  worker_setup();
  assert(DaliUsbNativeHostWorkerStart(&worker, &resources, &provider) == EFI_SUCCESS);
  assert(worker.Protocol.State == DaliUsbHostWorkerStarted && worker.Host.Published &&
    worker.Io.Owned && worker.Lease.Active && !release_calls && worker.Protocol.QualificationMask == qualification_mask);
  assert(worker.Host.HandoffProtocol.PrepareHandoff(&worker.Host.HandoffProtocol) == EFI_SUCCESS && worker.Io.HaltProven);
  tpl = TPL_CALLBACK;
  assert(DaliUsbNativeHostWorkerStop(&worker) == EFI_UNSUPPORTED && worker.Lease.Active && worker.Host.Published);
  tpl = TPL_APPLICATION;
  assert(DaliUsbNativeHostWorkerStop(&worker) == EFI_SUCCESS);
  assert(worker.Protocol.State == DaliUsbHostWorkerStopped && !worker.Host.Engine && !worker.Io.Initialized &&
    !worker.Lease.Active && release_calls == 1 && m.allocs == m.frees && !worker.Io.RecordCount);

  worker_setup();
  assert(DaliUsbNativeHostWorkerStart(&worker, &resources, &provider) == EFI_SUCCESS);
  m.halt_timeout = TRUE;
  assert(DaliUsbNativeHostWorkerStop(&worker) == EFI_TIMEOUT);
  assert(worker.Protocol.State == DaliUsbHostWorkerRetained && worker.Host.Engine != NULL &&
    worker.Io.Initialized && worker.Lease.Active && release_calls == 0);
  m.halt_timeout = FALSE;
  assert(DaliUsbNativeHostWorkerStop(&worker) == EFI_SUCCESS && release_calls == 1 && !worker.Lease.Active);

  worker_setup(); m.regs[0xa00/4] = 0x01010001; // inherited legacy ownership rejects HC start.
  assert(DaliUsbNativeHostWorkerStart(&worker, &resources, &provider) != EFI_SUCCESS);
  assert(worker.Protocol.State == DaliUsbHostWorkerFailed && !worker.Host.Engine && !worker.Io.Initialized &&
    !worker.Lease.Active && release_calls == 1);

  worker_setup(); m.regs[0xa00/4] = 0x01010001; release_result = EFI_DEVICE_ERROR;
  assert(DaliUsbNativeHostWorkerStart(&worker, &resources, &provider) == EFI_DEVICE_ERROR);
  assert(worker.Protocol.State == DaliUsbHostWorkerRetained && worker.Lease.Active &&
    !worker.Io.Initialized && release_calls == 1);
  release_result = EFI_SUCCESS;
  assert(DaliUsbNativeHostWorkerStop(&worker) == EFI_SUCCESS && !worker.Lease.Active && release_calls == 2);
}

int main(void) {
  resource_and_role_refusal();
  unqualified_lease_refusal();
  start_stop_and_retention();
  native_reset(); reset();
  puts("PASS: production native-host worker fail-closed qualification, start/stop ordering and lease retention; modeled provider only");
  return 0;
}
