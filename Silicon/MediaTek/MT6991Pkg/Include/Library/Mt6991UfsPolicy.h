// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Library/Mt6991UfsDmaLib.h>

typedef struct { UINT64 Blocks; UINT32 BlockSize; BOOLEAN Valid; } MT6991_UFS_GEOMETRY;
typedef struct {
  UINT8 *Trd, *Request, *Response, *Data;
  UINT32 Expected;
  UINTN ResponseBytes;
  UINT8 Transaction, Lun, Tag;
} MT6991_UFS_PENDING;

EFI_STATUS Mt6991UfsReadOnlyCommand (CONST MT6991_UFS_GEOMETRY *Geometry,
  UINT8 Lun, CONST UINT8 *Cdb, UINTN CdbBytes, UINT32 ReadBytes);
EFI_STATUS Mt6991UfsReadOnlyQuery (UINT8 Opcode, UINT8 Idn, UINT8 Index,
  UINT8 Selector, UINT32 Bytes, BOOLEAN InitAllowed);
// Called beneath PassThru, immediately before ringing slot 0. Only exact owned
// common descriptors and one contiguous device-write mapping are accepted.
EFI_STATUS Mt6991UfsValidateTransfer (MT6991_UFS_DMA *, UINT64 Trl,
  CONST MT6991_UFS_GEOMETRY *, BOOLEAN InitAllowed, MT6991_UFS_PENDING *);
// Caller observed DBR=0 and executed the DMA completion barrier first.
EFI_STATUS Mt6991UfsValidateResponse (MT6991_UFS_PENDING *, MT6991_UFS_GEOMETRY *, BOOLEAN *DataValid);
