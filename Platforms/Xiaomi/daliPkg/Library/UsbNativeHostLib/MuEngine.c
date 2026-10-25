// SPDX-License-Identifier: BSD-2-Clause-Patent
// Actual pinned Mu protocol engine. Native owner never calls its PCI binding,
// PCI lifecycle, reset, async monitor or cancellation implementations.
#define DALI_NATIVE_XHCI 1
#include "../../../../../Mu_Basecore/MdeModulePkg/Bus/Pci/XhciDxe/Xhci.c"
