# SC64SS hook: exception-context entry/exit.
#
# Reached via the Datel engine's tail jump (fork patch replaces its final
# `j 0x80000120` with `j 0x807D0000`). Contract at entry: EXL=1 (interrupts
# hardware-masked), k0/k1 are scratch (the engine used them, the game's
# relocated handler at 0x80000120 will overwrite them first thing), every
# other register is live game state and must be preserved bit-exact,
# including 64-bit widths and HI/LO. No stack exists; we bring our own.

.set noreorder
.set noat

# Stage marker in RDRAM (0x807FFFFC): a plain store, no cart access, so the last
# value survives a hook that never returns (the development engine trace copies
# it to BRAM on the next exception).
.macro STAGE n
    lui     $k0, 0x8080
    ori     $k1, $zero, \n
    sw      $k1, -4($k0)
.endm


.section .text.entry
.global _start
_start:
    lui     $k0, 0x8080                 # DIAG: hook entry counter (0x807FFFF8)++
    lw      $k1, -8($k0)
    addiu   $k1, $k1, 1
    sw      $k1, -8($k0)
    STAGE   1                           # stub entered

    lui     $k0, %hi(save_area)
    addiu   $k0, $k0, %lo(save_area)
    sd      $at, 0x00($k0)
    sd      $v0, 0x08($k0)
    sd      $v1, 0x10($k0)
    sd      $a0, 0x18($k0)
    sd      $a1, 0x20($k0)
    sd      $a2, 0x28($k0)
    sd      $a3, 0x30($k0)
    sd      $t0, 0x38($k0)
    sd      $t1, 0x40($k0)
    sd      $t2, 0x48($k0)
    sd      $t3, 0x50($k0)
    sd      $t4, 0x58($k0)
    sd      $t5, 0x60($k0)
    sd      $t6, 0x68($k0)
    sd      $t7, 0x70($k0)
    sd      $t8, 0x78($k0)
    sd      $t9, 0x80($k0)
    sd      $s0, 0x88($k0)
    sd      $s1, 0x90($k0)
    sd      $s2, 0x98($k0)
    sd      $s3, 0xA0($k0)
    sd      $s4, 0xA8($k0)
    sd      $s5, 0xB0($k0)
    sd      $s6, 0xB8($k0)
    sd      $s7, 0xC0($k0)
    sd      $gp, 0xC8($k0)
    sd      $sp, 0xD0($k0)
    sd      $fp, 0xD8($k0)
    sd      $ra, 0xE0($k0)
    mflo    $k1
    sd      $k1, 0xE8($k0)
    mfhi    $k1
    sd      $k1, 0xF0($k0)

    STAGE   2                           # registers saved

    lui     $sp, %hi(stack_top - 32)
    addiu   $sp, $sp, %lo(stack_top - 32)
    jal     hook_tick
    nop

    lui     $k0, %hi(save_area)
    addiu   $k0, $k0, %lo(save_area)
    ld      $k1, 0xE8($k0)
    mtlo    $k1
    ld      $k1, 0xF0($k0)
    mthi    $k1
    ld      $at, 0x00($k0)
    ld      $v0, 0x08($k0)
    ld      $v1, 0x10($k0)
    ld      $a0, 0x18($k0)
    ld      $a1, 0x20($k0)
    ld      $a2, 0x28($k0)
    ld      $a3, 0x30($k0)
    ld      $t0, 0x38($k0)
    ld      $t1, 0x40($k0)
    ld      $t2, 0x48($k0)
    ld      $t3, 0x50($k0)
    ld      $t4, 0x58($k0)
    ld      $t5, 0x60($k0)
    ld      $t6, 0x68($k0)
    ld      $t7, 0x70($k0)
    ld      $t8, 0x78($k0)
    ld      $t9, 0x80($k0)
    ld      $s0, 0x88($k0)
    ld      $s1, 0x90($k0)
    ld      $s2, 0x98($k0)
    ld      $s3, 0xA0($k0)
    ld      $s4, 0xA8($k0)
    ld      $s5, 0xB0($k0)
    ld      $s6, 0xB8($k0)
    ld      $s7, 0xC0($k0)
    ld      $gp, 0xC8($k0)
    ld      $sp, 0xD0($k0)
    ld      $fp, 0xD8($k0)
    ld      $ra, 0xE0($k0)

    STAGE   6                           # restored, about to return to the game

    lui     $k0, %hi(borrow_epilogue)
    lw      $k0, %lo(borrow_epilogue)($k0)
    beqz    $k0, 1f
    nop
    jr      $k0                         # borrowed mode: the cart monitor puts the game's RAM back
    nop
1:  j       0x80000120
    nop

# ---- The virtual Controller Pak, from inside the game's SI interrupt handling ------
# hook.c (vpak_tramp_install) turns four words of libultra's __osException, where it
# acknowledges the SI interrupt, into `lui ra / ori ra / jr ra / nop` to here:
#     lui rX, 0xa480 / sw zero, 0x18(rX) / jal send_mesg / li a0, OS_EVENT_SI*8
# Context: EXL=1, the interrupted thread is already saved by the handler, k0 is its
# thread pointer, s0/s1 the handler's pending-interrupt words. at, a0 and ra are dead
# at the site (those four words write them before anything reads them), and what
# remains that the C code may touch is saved here and put back. Then the four words' work:
# the acknowledge (rX set as it would have been), send_mesg through the recorded
# address with a0 as its delay slot set it, and back to the word after that delay
# slot with ra as the jal would have left it.
.balign 16
.global vpak_tramp
vpak_tramp:
    lui     $ra, %hi(tramp_area)
    addiu   $ra, $ra, %lo(tramp_area)
    sd      $v0, 0x00($ra)
    sd      $v1, 0x08($ra)
    sd      $a1, 0x10($ra)
    sd      $a2, 0x18($ra)
    sd      $a3, 0x20($ra)
    sd      $t0, 0x28($ra)
    sd      $t1, 0x30($ra)
    sd      $t2, 0x38($ra)
    sd      $t3, 0x40($ra)
    sd      $t4, 0x48($ra)
    sd      $t5, 0x50($ra)
    sd      $t6, 0x58($ra)
    sd      $t7, 0x60($ra)
    sd      $t8, 0x68($ra)
    sd      $t9, 0x70($ra)
    sd      $gp, 0x78($ra)
    sd      $sp, 0x80($ra)
    mflo    $a0
    sd      $a0, 0x88($ra)
    mfhi    $a0
    sd      $a0, 0x90($ra)
    lui     $sp, %hi(stack_top - 32)
    addiu   $sp, $sp, %lo(stack_top - 32)
    jal     vpak_si_hit
    nop
    lui     $ra, %hi(tramp_area)
    addiu   $ra, $ra, %lo(tramp_area)
    ld      $a0, 0x88($ra)
    mtlo    $a0
    ld      $a0, 0x90($ra)
    mthi    $a0
    ld      $v0, 0x00($ra)
    ld      $v1, 0x08($ra)
    ld      $a1, 0x10($ra)
    ld      $a2, 0x18($ra)
    ld      $a3, 0x20($ra)
    ld      $t0, 0x28($ra)
    ld      $t1, 0x30($ra)
    ld      $t2, 0x38($ra)
    ld      $t3, 0x40($ra)
    ld      $t4, 0x48($ra)
    ld      $t5, 0x50($ra)
    ld      $t6, 0x58($ra)
    ld      $t7, 0x60($ra)
    ld      $t8, 0x68($ra)
    ld      $t9, 0x70($ra)
    ld      $gp, 0x78($ra)
    ld      $sp, 0x80($ra)
    lui     $at, %hi(vpak_tramp_vec)
    addiu   $at, $at, %lo(vpak_tramp_vec)
    lw      $ra, 12($at)                # rX (1 at, 8 t0, 9 t1) | rX's low half << 16
    lui     $a0, 0xa480
    sw      $zero, 0x18($a0)            # SI_STATUS = 0: the acknowledge
    andi    $s1, $s1, 0x3d              # the SI bit out of the handler's pending word (idempotent)
    srl     $a0, $ra, 16
    andi    $ra, $ra, 0x1f
    addiu   $ra, $ra, -8
    beqz    $ra, 1f
    addiu   $ra, $ra, -1
    beqz    $ra, 2f
    nop
    b       3f
    nop
1:  lui     $t0, 0xa480                 # rX = 0xa4800000 | low half, as the acknowledge left it
    b       3f
    or      $t0, $t0, $a0
2:  lui     $t1, 0xa480
    or      $t1, $t1, $a0
3:  lw      $a0, 8($at)                 # a0 for send_mesg
    lw      $at, 0($at)                 # send_mesg
    nop
    jalr    $at
    nop
    lui     $at, %hi(vpak_tramp_vec)
    addiu   $at, $at, %lo(vpak_tramp_vec)
    lw      $ra, 4($at)                 # the word after the jal's delay slot
    nop
    jr      $ra
    lui     $at, 0xa480                 # as an acknowledge through `at` leaves it (dead either way)

# ---- Exit to the SC64 menu: the boot routine that runs from SP IMEM. hook.c (menu_exit)
# copies it to 0xA4001000 and jumps to reboot_blob_entry with the registers the PIF would
# have set; it is the routine the menu boots ROMs with (the fork's src/boot/reboot.S).
# Position-dependent: assembled for 0xA4001000.
.section .rodata
.balign 16
.global reboot_blob
reboot_blob:
.set noreorder
    lui     $t5, 0xBFC0                 # IPL2, kept where a CIC x105 IPL3 expects it
1:  lw      $t0, 0x7FC($t5)
    addiu   $t5, $t5, 0x7C0
    andi    $t0, $t0, 0x80
    bnel    $t0, $zero, 1b
    lui     $t5, 0xBFC0
    lw      $t0, 0x24($t5)
    lui     $t3, 0xB000
.set reorder
reboot_blob_entry:
    li      $sp, 0xA4001FF0
.set noreorder
    beqz    $s1, 3f                     # s1: clear the lower 4 MiB first
    nop
    li      $t0, 0xA0000000
    li      $t1, 0x00400000
    addu    $t1, $t0, $t1
2:  sw      $zero, 0($t0)
    sw      $zero, 4($t0)
    sw      $zero, 8($t0)
    sw      $zero, 12($t0)
    sw      $zero, 16($t0)
    sw      $zero, 20($t0)
    sw      $zero, 24($t0)
    sw      $zero, 28($t0)
    addiu   $t0, $t0, 32
    bne     $t0, $t1, 2b
    nop
3:
.set reorder
    bnez    $a0, 4f                     # a0: keep RDRAM up
    bnez    $s5, 4f                     # s5: an NMI reset keeps it too
    li      $t0, 0xA4700000             # RI refresh and select off: IPL3 brings RDRAM up again
    sw      $zero, 0x10($t0)
    sw      $zero, 0x0C($t0)
4:  li      $t0, 1
    beq     $s4, $zero, 5f
    beq     $s4, $t0, 6f
    b       7f
5:  li      $ra, 0xA4001554             # PAL
    b       8f
6:  li      $ra, 0xA4001550             # NTSC
    b       8f
7:  li      $ra, 0xA4001554             # MPAL
8:  move    $at, $zero
    move    $v0, $zero
    move    $v1, $zero
    move    $a0, $zero
    move    $a1, $zero
    move    $a2, $zero
    move    $a3, $zero
    move    $t0, $zero
    move    $t1, $zero
    li      $t2, 0x40
    move    $t4, $zero
    move    $t5, $zero
    move    $t6, $zero
    move    $t7, $zero
    move    $s0, $zero
    move    $s1, $zero
    move    $s2, $zero
    move    $t8, $zero
    move    $t9, $zero
    move    $k0, $zero
    move    $k1, $zero
    move    $gp, $zero
    move    $fp, $zero
    li      $t3, 0xA4000040
    jr      $t3
.set noreorder
    .set reboot_blob_size, . - reboot_blob
    .global reboot_blob_size
    .set reboot_blob_entry_off, reboot_blob_entry - reboot_blob
    .global reboot_blob_entry_off

.section .data
.balign 8
.global save_area
save_area:
    .space 256
.balign 8
tramp_area:
    .space 160
# borrowed mode: where entry.S goes instead of the game after a visit (hook.c sets it)
.balign 4
.global borrow_epilogue
borrow_epilogue:
    .word 0
.balign 16
stack_bottom:
    .space 4096
stack_top:
