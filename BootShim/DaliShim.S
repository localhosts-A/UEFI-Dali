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
  // Append an independently recoverable marker, retaining a valid prior ring.
  ldr x7, =0xAEC0F000
  movz w8, #0x4244
  movk w8, #0x4347, lsl #16
  ldr w9, [x7]
  mov w12, #0x200000
  sub w12, w12, #12
  cmp w9, w8
  b.ne 4f
  ldr w10, [x7, #4]
  ldr w11, [x7, #8]
  cmp w10, w12
  b.hs 4f
  cmp w11, w12
  b.ls 5f
4:
  mov w10, wzr
  mov w11, wzr
  str wzr, [x7, #4]
  str wzr, [x7, #8]
  dsb sy
  str w8, [x7]
5:
  adr x8, _marker
  add x9, x7, #12
1:
  ldrb w13, [x8], #1
  cbz w13, 2f
  strb w13, [x9, x10]
  add w10, w10, #1
  cmp w10, w12
  csel w10, wzr, w10, eq
  cmp w11, w12
  cinc w11, w11, lo
  b 1b
2:
  str w10, [x7, #4]
  str w11, [x7, #8]
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
