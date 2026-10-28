// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DaliUsbNativeHostWorker.h>

#define DALI_XHCI_BASE 0x16700000ULL
#define DALI_XHCI_SIZE 0x1000ULL
#define DEVICE_PATH_VENDOR_NODE_SIZE 20U
#define DEVICE_PATH_END_NODE_SIZE 4U

STATIC DALI_USB_NATIVE_HOST_WORKER mWorker;

STATIC EFI_TPL CurrentTpl(VOID) {
  EFI_TPL Tpl = gBS->RaiseTPL(TPL_HIGH_LEVEL);
  gBS->RestoreTPL(Tpl);
  return Tpl;
}

STATIC BOOLEAN ValidPorts(CONST DALI_USB_NATIVE_HOST_LEASE *Lease) {
  if (Lease->Usb2PortCount == 0 || Lease->Usb2PortCount > DALI_USB_NATIVE_HOST_MAX_PORTS) {
    return FALSE;
  }
  for (UINTN I = 0; I < Lease->Usb2PortCount; I++) {
    for (UINTN J = 0; J < I; J++) {
      if (Lease->Usb2Ports[I] == Lease->Usb2Ports[J]) { return FALSE; }
    }
  }
  return TRUE;
}

STATIC BOOLEAN ValidLease(CONST DALI_USB_NATIVE_HOST_LEASE *Lease,
  CONST DALI_USB_XHCI_RESOURCES *Resources) {
  return Lease->Active && Lease->Revision == DALI_USB_NATIVE_HOST_LEASE_REVISION &&
    Lease->Size >= sizeof(*Lease) &&
    (Lease->QualificationMask & DALI_USB_HOST_REQUIRED_QUALIFICATIONS) ==
      DALI_USB_HOST_REQUIRED_QUALIFICATIONS &&
    Lease->RegisterBase == Resources->Registers.Base &&
    Lease->RegisterLength == Resources->Registers.Size &&
    Lease->RegisterBase == DALI_XHCI_BASE && Lease->RegisterLength == DALI_XHCI_SIZE &&
    Lease->RegisterBase <= MAX_UINTN && Lease->RegisterLength <= MAX_UINTN &&
    Lease->Dma.Qualified && Lease->Dma.Allocate != NULL && Lease->Dma.Sync != NULL &&
    Lease->Dma.Free != NULL && Lease->Guard != NULL && ValidPorts(Lease);
}

STATIC BOOLEAN ValidXhciDescription(CONST DALI_USB_XHCI_RESOURCES *Resources) {
  CONST UINT32 Expected[DALI_USB_XHCI_INTERRUPT_CELLS] = {0, 600, 4, 0};
  if (Resources->Registers.Base != DALI_XHCI_BASE || Resources->Registers.Size != DALI_XHCI_SIZE) {
    return FALSE;
  }
  for (UINTN I = 0; I < DALI_USB_XHCI_INTERRUPT_CELLS; I++) {
    if (Resources->InterruptSpecifier[I] != Expected[I]) { return FALSE; }
  }
  return TRUE;
}

STATIC VOID InitPath(DALI_USB_NATIVE_HOST_WORKER *Worker) {
  Worker->Path.VendorNode.Type = HARDWARE_DEVICE_PATH;
  Worker->Path.VendorNode.SubType = HW_VENDOR_DP;
  Worker->Path.VendorNode.Length[0] = (UINT8)DEVICE_PATH_VENDOR_NODE_SIZE;
  Worker->Path.VendorNode.Length[1] = 0;
  Worker->Path.Vendor = gDaliUsbNativeHostProviderProtocolGuid;
  Worker->Path.End.Type = END_DEVICE_PATH_TYPE;
  Worker->Path.End.SubType = END_ENTIRE_DEVICE_PATH_SUBTYPE;
  Worker->Path.End.Length[0] = (UINT8)DEVICE_PATH_END_NODE_SIZE;
  Worker->Path.End.Length[1] = 0;
}

STATIC EFI_STATUS ReleaseProvider(DALI_USB_NATIVE_HOST_WORKER *Worker) {
  if (!Worker->Lease.Active) { return EFI_SUCCESS; }
  if (Worker->Provider == NULL || Worker->Provider->Release == NULL) { return EFI_NOT_READY; }
  EFI_STATUS Status = Worker->Provider->Release(Worker->Provider, &Worker->Lease);
  if (EFI_ERROR(Status)) { return Status; }
  return Worker->Lease.Active ? EFI_COMPROMISED_DATA : EFI_SUCCESS;
}

EFI_STATUS DaliUsbNativeHostWorkerStop(DALI_USB_NATIVE_HOST_WORKER *Worker) {
  if (Worker == NULL || Worker->Provider == NULL || Worker->Provider->Release == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  if (CurrentTpl() != TPL_APPLICATION) { return EFI_UNSUPPORTED; }
  EFI_STATUS Status = EFI_SUCCESS;
  if (Worker->Host.Engine != NULL || Worker->Host.Io != NULL || Worker->Host.Published ||
      Worker->Host.Timer != NULL || Worker->Host.ExitEvent != NULL) {
    Status = DaliUsbNativeHostStop(&Worker->Host);
    if (EFI_ERROR(Status)) { goto Retained; }
  }
  if (Worker->Io.Initialized) {
    Status = DaliUsbXhciIoClose(&Worker->Io);
    if (EFI_ERROR(Status)) { goto Retained; }
  }
  Status = ReleaseProvider(Worker);
  if (EFI_ERROR(Status)) { goto Retained; }

  ZeroMem(&Worker->Host, sizeof(Worker->Host));
  ZeroMem(&Worker->Io, sizeof(Worker->Io));
  ZeroMem(&Worker->Lease, sizeof(Worker->Lease));
  Worker->Protocol.State = DaliUsbHostWorkerStopped;
  Worker->Protocol.LastStatus = EFI_SUCCESS;
  Worker->Protocol.CleanupStatus = EFI_SUCCESS;
  return EFI_SUCCESS;

Retained:
  Worker->Protocol.State = DaliUsbHostWorkerRetained;
  Worker->Protocol.CleanupStatus = Status;
  return Status;
}

STATIC EFI_STATUS FailStart(DALI_USB_NATIVE_HOST_WORKER *Worker, EFI_STATUS Cause) {
  Worker->Protocol.LastStatus = Cause;
  Worker->Protocol.CleanupStatus = EFI_SUCCESS;
  EFI_STATUS Cleanup = DaliUsbNativeHostWorkerStop(Worker);
  if (EFI_ERROR(Cleanup)) {
    Worker->Protocol.State = DaliUsbHostWorkerRetained;
    Worker->Protocol.CleanupStatus = Cleanup;
    DEBUG((DEBUG_ERROR, "Dali USB host worker retained resources: cause=%r cleanup=%r\n", Cause, Cleanup));
    return Cleanup;
  }
  Worker->Protocol.State = Cause == EFI_NOT_READY ? DaliUsbHostWorkerWaitingForRole : DaliUsbHostWorkerFailed;
  Worker->Protocol.LastStatus = Cause;
  return Cause;
}

EFI_STATUS DaliUsbNativeHostWorkerStart(DALI_USB_NATIVE_HOST_WORKER *Worker,
  DALI_USB_RESOURCE_CONTEXT_PROTOCOL *Resources,
  DALI_USB_NATIVE_HOST_PROVIDER_PROTOCOL *Provider) {
  if (Worker == NULL || Resources == NULL || Provider == NULL) { return EFI_INVALID_PARAMETER; }
  if (CurrentTpl() != TPL_APPLICATION) { return EFI_UNSUPPORTED; }
  if (Worker->Protocol.State == DaliUsbHostWorkerStarting ||
      Worker->Protocol.State == DaliUsbHostWorkerStarted ||
      Worker->Protocol.State == DaliUsbHostWorkerRetained) { return EFI_ALREADY_STARTED; }
  if (Worker->Lease.Active || Worker->Io.Initialized || Worker->Host.Engine != NULL ||
      Worker->Host.Io != NULL || Worker->Host.Published) { return EFI_NOT_READY; }
  if (Resources->Revision < 6 || Resources->GetStatus == NULL ||
      Resources->GetXhciResources == NULL || Provider->Revision < DALI_USB_NATIVE_HOST_PROVIDER_REVISION ||
      Provider->Acquire == NULL || Provider->Release == NULL) { return EFI_UNSUPPORTED; }

  Worker->Resources = Resources;
  Worker->Provider = Provider;
  Worker->Protocol.Revision = DALI_USB_NATIVE_HOST_WORKER_REVISION;
  Worker->Protocol.State = DaliUsbHostWorkerStarting;
  Worker->Protocol.QualificationMask = 0;
  DALI_USB_RESOURCE_CONTEXT_STATUS ContextStatus;
  DALI_USB_XHCI_RESOURCES Xhci;
  ZeroMem(&ContextStatus, sizeof(ContextStatus));
  ZeroMem(&Xhci, sizeof(Xhci));
  EFI_STATUS Status = Resources->GetStatus(Resources, &ContextStatus);
  if (EFI_ERROR(Status)) { return FailStart(Worker, Status); }
  if (ContextStatus.BootServicesClosed) { return FailStart(Worker, EFI_UNSUPPORTED); }
  if (EFI_ERROR(ContextStatus.InitializationStatus)) {
    return FailStart(Worker, ContextStatus.InitializationStatus);
  }
  if (!ContextStatus.SnapshotValid) { return FailStart(Worker, EFI_NOT_READY); }
  Status = Resources->GetXhciResources(Resources, &Xhci);
  if (EFI_ERROR(Status)) { return FailStart(Worker, Status); }
  if (!ValidXhciDescription(&Xhci)) {
    return FailStart(Worker, EFI_UNSUPPORTED);
  }

  ZeroMem(&Worker->Lease, sizeof(Worker->Lease));
  Status = Provider->Acquire(Provider, &Xhci, &Worker->Lease);
  if (EFI_ERROR(Status)) { return FailStart(Worker, Status); }
  if (!ValidLease(&Worker->Lease, &Xhci)) { return FailStart(Worker, EFI_UNSUPPORTED); }
  Worker->Protocol.QualificationMask = Worker->Lease.QualificationMask;

  Status = DaliUsbXhciIoOpen(&Worker->Io, (UINTN)Worker->Lease.RegisterBase,
    (UINTN)Worker->Lease.RegisterLength, &Worker->Lease.Dma,
    Worker->Lease.Guard, Worker->Lease.GuardContext);
  if (EFI_ERROR(Status)) { return FailStart(Worker, Status); }
  if (!Worker->Io.Owned) { return FailStart(Worker, EFI_NOT_READY); }

  InitPath(Worker);
  Status = DaliUsbNativeHostStart(&Worker->Host, &Worker->Io,
    &Worker->Path.VendorNode, Worker->Lease.Usb2Ports, Worker->Lease.Usb2PortCount);
  if (EFI_ERROR(Status)) { return FailStart(Worker, Status); }
  Worker->Protocol.State = DaliUsbHostWorkerStarted;
  Worker->Protocol.LastStatus = EFI_SUCCESS;
  Worker->Protocol.CleanupStatus = EFI_SUCCESS;
  return EFI_SUCCESS;
}

EFI_STATUS DaliUsbNativeHostWorkerPoll(DALI_USB_NATIVE_HOST_WORKER *Worker) {
  if (Worker == NULL) { return EFI_INVALID_PARAMETER; }
  if (CurrentTpl() != TPL_APPLICATION) { return EFI_UNSUPPORTED; }
  if (Worker->Protocol.State != DaliUsbHostWorkerWaitingForRole) { return EFI_SUCCESS; }
  if (Worker->Resources == NULL || Worker->Provider == NULL) { return EFI_NOT_READY; }
  return DaliUsbNativeHostWorkerStart(Worker, Worker->Resources, Worker->Provider);
}

STATIC DALI_USB_NATIVE_HOST_WORKER *WorkerFromProtocol(DALI_USB_NATIVE_HOST_WORKER_PROTOCOL *Protocol) {
  return (DALI_USB_NATIVE_HOST_WORKER *)Protocol;
}

STATIC EFI_STATUS EFIAPI PlatformPoll(UNIXNU_PLATFORM_POLL_PROTOCOL *Protocol) {
  if (Protocol == NULL) { return EFI_INVALID_PARAMETER; }
  DALI_USB_NATIVE_HOST_WORKER *Worker = BASE_CR(Protocol, DALI_USB_NATIVE_HOST_WORKER, PlatformPoll);
  return DaliUsbNativeHostWorkerPoll(Worker);
}

STATIC EFI_STATUS EFIAPI ProtocolStart(DALI_USB_NATIVE_HOST_WORKER_PROTOCOL *Protocol) {
  if (Protocol == NULL) { return EFI_INVALID_PARAMETER; }
  DALI_USB_NATIVE_HOST_WORKER *Worker = WorkerFromProtocol(Protocol);
  return DaliUsbNativeHostWorkerStart(Worker, Worker->Resources, Worker->Provider);
}

STATIC EFI_STATUS EFIAPI ProtocolStop(DALI_USB_NATIVE_HOST_WORKER_PROTOCOL *Protocol) {
  if (Protocol == NULL) { return EFI_INVALID_PARAMETER; }
  return DaliUsbNativeHostWorkerStop(WorkerFromProtocol(Protocol));
}

EFI_STATUS EFIAPI DaliUsbNativeHostWorkerInitialize(EFI_HANDLE ImageHandle,
  EFI_SYSTEM_TABLE *SystemTable) {
  (VOID)SystemTable;
  if (ImageHandle == NULL) { return EFI_INVALID_PARAMETER; }
  mWorker.Protocol.Revision = DALI_USB_NATIVE_HOST_WORKER_REVISION;
  mWorker.Protocol.State = DaliUsbHostWorkerDormant;
  mWorker.Protocol.Start = ProtocolStart;
  mWorker.Protocol.Stop = ProtocolStop;
  mWorker.PlatformPoll.Revision = UNIXNU_PLATFORM_POLL_PROTOCOL_REVISION;
  mWorker.PlatformPoll.Size = sizeof(mWorker.PlatformPoll);
  mWorker.PlatformPoll.Poll = PlatformPoll;
  EFI_STATUS Status = gBS->InstallMultipleProtocolInterfaces(&ImageHandle,
    &gDaliUsbNativeHostWorkerProtocolGuid, &mWorker.Protocol,
    &gUniXNUPlatformPollProtocolGuid, &mWorker.PlatformPoll, NULL);
  if (EFI_ERROR(Status)) { return Status; }

  Status = gBS->LocateProtocol(&gDaliUsbResourceContextProtocolGuid, NULL,
    (VOID **)&mWorker.Resources);
  if (EFI_ERROR(Status)) { goto NoStart; }
  Status = gBS->LocateProtocol(&gDaliUsbNativeHostProviderProtocolGuid, NULL,
    (VOID **)&mWorker.Provider);
  if (EFI_ERROR(Status)) { goto NoStart; }
  Status = DaliUsbNativeHostWorkerStart(&mWorker, mWorker.Resources, mWorker.Provider);
  if (EFI_ERROR(Status) && mWorker.Protocol.State != DaliUsbHostWorkerWaitingForRole) {
    DEBUG((DEBUG_ERROR, "Dali USB host worker did not start: %r\n", Status));
  }
  return EFI_SUCCESS;

NoStart:
  mWorker.Protocol.State = DaliUsbHostWorkerFailed;
  mWorker.Protocol.LastStatus = Status;
  return EFI_SUCCESS;
}
