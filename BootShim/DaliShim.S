// SPDX-License-Identifier: BSD-2-Clause-Patent
// Linux ARM64 entry -> relocate Mu-Silicium FD, preserving LK's x0/FDT.
.text
.global _start
_start:
  b _entry
  nop
  .quad 0x80000
  .quad FD_SIZE + (_payload - _start)
  .quad 0xA // little-endian, 4 KiB pages, relocatable physical placement
  .quad 0
  .quad 0
  .quad 0
  .ascii "ARM\x64"
  .long 0
_entry:
  msr daifset, #15
  mov x19, x0
  // Write an independently recoverable marker even if SEC cannot be reached.
  ldr x7, =0xAEC0F000
  movz w8, #0x4244
  movk w8, #0x4347, lsl #16
  str w8, [x7]
  adr x8, _marker
  add x9, x7, #12
  mov x10, #0
1:
  ldrb w11, [x8], #1
  cbz w11, 2f
  strb w11, [x9], #1
  add x10, x10, #1
  b 1b
2:
  str w10, [x7, #4]
  str w10, [x7, #8]
  dsb sy
  adr x1, _payload
  ldr x2, =FD_BASE
  ldr x3, =FD_SIZE
3:
  ldp x4, x5, [x1], #16
  stp x4, x5, [x2], #16
  subs x3, x3, #16
  b.ne 3b
  dsb sy
  ic iallu
  dsb sy
  isb
  mov x0, x19
  ldr x16, =FD_BASE
  br x16
_marker:
  .asciz "DALI: UEFI relocation shim entered\n"
  .ltorg
.balign 4096
_payload:
