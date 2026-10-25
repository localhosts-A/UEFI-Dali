// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Library/FdtLib.h>
/* Pinned BaseFdtLib exports upstream's full checker, UINTN=size_t ABI. */
INT32 fdt_check_full (CONST VOID *Fdt, UINTN BufferSize);

STATIC BOOLEAN Enabled (CONST VOID *Fdt, INT32 Node) {
  INT32 Length;
  CONST CHAR8 *S = FdtGetProp (Fdt, Node, "status", &Length);
  if (S == NULL) { return Length == -FDT_ERR_NOTFOUND; }
  return (Length == 3 && S[0] == 'o' && S[1] == 'k' && S[2] == 0) ||
    (Length == 5 && S[0] == 'o' && S[1] == 'k' && S[2] == 'a' && S[3] == 'y' && S[4] == 0);
}
STATIC BOOLEAN Words (CONST VOID *Fdt, INT32 Node, CONST CHAR8 *Name, CONST UINT32 *Expected, UINTN Count) {
  INT32 Length;
  CONST UINT32 *P = FdtGetProp (Fdt, Node, Name, &Length);
  if (P == NULL || Length != (INT32)(Count * 4)) { return FALSE; }
  for (UINTN I = 0; I < Count; ++I) { if (Fdt32ToCpu (P[I]) != Expected[I]) { return FALSE; } }
  return TRUE;
}
STATIC BOOLEAN Cell (CONST VOID *Fdt, INT32 Node, CONST CHAR8 *Name, UINT32 Value) {
  return Words (Fdt, Node, Name, &Value, 1);
}
STATIC BOOLEAN Empty (CONST VOID *Fdt, INT32 Node, CONST CHAR8 *Name) {
  INT32 Length;
  CONST VOID *P = FdtGetProp (Fdt, Node, Name, &Length);
  return P != NULL && Length == 0;
}
STATIC BOOLEAN Compatible (CONST VOID *Fdt, INT32 Node, CONST CHAR8 *Value) {
  return Node >= 0 && Enabled (Fdt, Node) && FdtStringListCount (Fdt, Node, "compatible") > 0 &&
    FdtStringListSearch (Fdt, Node, "compatible", Value) >= 0;
}
STATIC INT32 UniqueHandle (CONST VOID *Fdt, UINT32 Handle) {
  if (Handle == 0 || Handle == 0xffffffff) { return -1; }
  INT32 Node = -1, Found = -1;
  while ((Node = FdtNextNode (Fdt, Node, NULL)) >= 0) {
    CONST CHAR8 *Names[] = {"phandle", "linux,phandle"};
    UINT32 Current = 0;
    for (UINTN I = 0; I < 2; ++I) {
      INT32 Length;
      CONST UINT32 *P = FdtGetProp (Fdt, Node, Names[I], &Length);
      if (P == NULL) { if (Length != -FDT_ERR_NOTFOUND) { return -1; } continue; }
      if (Length != 4) { return -1; }
      UINT32 Value = Fdt32ToCpu (*P);
      if (Value == 0 || Value == 0xffffffff || (Current != 0 && Current != Value)) { return -1; }
      Current = Value;
    }
    if (Current == Handle) { if (Found >= 0) { return -1; } Found = Node; }
  }
  return Node == -FDT_ERR_NOTFOUND ? Found : -1;
}
