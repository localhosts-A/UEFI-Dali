// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Base.h>

// Control-plane only. Callbacks serialize accesses and own their context.
// Write32/Fence must complete MMIO ordering; Stall is in microseconds. SiP
// receives x0-x3 and must zero x4-x7. No DMA or media command is issued here.
typedef struct {
  UINT32 (*Read32)(UINTN Address, VOID *Context);
  BOOLEAN (*Write32)(UINTN Address, UINT32 Value, VOID *Context);
  VOID (*Fence)(VOID *Context);
  VOID (*Stall)(UINT32 Microseconds, VOID *Context);
  UINT64 (*Sip)(UINT32 Service, UINT32 Selector, UINT32 Arg1, UINT32 Arg2, VOID *Context);
  VOID *Context;
} MT6991_UFS_IO;

typedef enum {
  Mt6991UfsOff, Mt6991UfsRejected, Mt6991UfsBusy, Mt6991UfsTimeout,
  Mt6991UfsHardwareError, Mt6991UfsUicError, Mt6991UfsSipError,
  Mt6991UfsLinkReady, Mt6991UfsStopped
} MT6991_UFS_RESULT;

typedef enum {
  Mt6991UfsValidate, Mt6991UfsPower, Mt6991UfsReferenceClock,
  Mt6991UfsHostReset, Mt6991UfsHce, Mt6991UfsPreLink,
  Mt6991UfsLinkStartup, Mt6991UfsPostLink,
  Mt6991UfsDeviceReset // Appended to preserve existing diagnostic stage values.
} MT6991_UFS_STAGE;

// Each event/failure snapshot is captured once before its acknowledgement or
// cleanup. LayerErrors can clear status; later logs/stop paths use saved copies.
typedef struct {
  UINT32 InterruptStatus, HostStatus, HostEnable;
  UINT32 Arguments[3], LayerErrors[5];
  BOOLEAN Captured;
} MT6991_UFS_UIC_SNAPSHOT;

// Initialize to zero ONCE for a new boot. Failure poisons this instance;
// neither Start nor Stop permits automatic resubmission after an uncertain UIC.
typedef struct {
  MT6991_UFS_IO Io;
  MT6991_UFS_RESULT Result, StopResult;
  MT6991_UFS_STAGE Stage, FaultStage;
  UINTN LastAddress, FaultAddress;
  UINT32 LastValue, FaultValue, InterruptStatus, UicOpcode, UicResult, LocalVersion;
  UINT64 SipStatus, RefClockCleanupStatus;
  BOOLEAN Attempted, Touched, Failed, Ready, Stopped;
  MT6991_UFS_UIC_SNAPSHOT UicFailure, LinkResetEvent;
  BOOLEAN LinkResetPending, LinkResetAccepted, LinkResetAcked;
  UINT32 LinkResetFirstIs, LinkResetPolls;
  UINT32 SipSelector, SipArg1, SipArg2;
  UINT64 DeviceResetCleanupStatus;
  BOOLEAN DeviceResetAsserted, DeviceResetReleaseAttempted, DeviceResetReleased, DeviceResetCleanupAttempted;
} MT6991_UFS_CONTROLLER;

MT6991_UFS_RESULT Mt6991UfsStart (
  CONST VOID *Fdt, BOOLEAN Enabled, CONST CHAR8 *Token,
  CONST MT6991_UFS_IO *Io, MT6991_UFS_CONTROLLER *Controller
  );
// Stops submission first, then legacy lists and HCE. Success means HCE=0 was
// observed; mapping teardown is the future host driver's separate responsibility.
MT6991_UFS_RESULT Mt6991UfsStop (MT6991_UFS_CONTROLLER *Controller);
