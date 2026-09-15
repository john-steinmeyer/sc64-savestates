# SC64SS hook: save-state CPU context helpers (hook v6). Called from hook.c
# inside the exception (EXL=1), so nothing else runs meanwhile.
#
# struct state_ctx (hook.c) layout, mirrored here:
#   +0x000 gpr[32]  u64, by register number (0/26/27 unused)
#   +0x100 lo, +0x108 hi
#   +0x110 fpr[32]  u64: even registers always, odd ones only when Status.FR=1
#   +0x210 status, +0x214 epc, +0x218 fcr31, +0x21C entryhi (live ASID)
#   +0x220 tlb[32]  {pagemask, entryhi, entrylo0, entrylo1}
#
# The blob is built -msoft-float; the FPU moves below opt back in per function.

.set noreorder
.set noat

.set CTX_LO,      0x100
.set CTX_HI,      0x108
.set CTX_FPR,     0x110
.set CTX_STATUS,  0x210
.set CTX_EPC,     0x214
.set CTX_FCR31,   0x218
.set CTX_ENTRYHI, 0x21C
.set CTX_TLB,     0x220

.section .text

# void fpu_save(struct state_ctx *ctx): FCR31 and the FPRs, with CU1 forced on
# for the duration (the interrupted thread need not own the FPU). Clobbers t0-t3.
.global fpu_save
fpu_save:
    .set push
    .set hardfloat
    mfc0    $t0, $12
    lui     $t1, 0x2000                 # SR_CU1
    or      $t2, $t0, $t1
    mtc0    $t2, $12
    nop
    nop
    nop
    nop
    cfc1    $t3, $31
    sw      $t3, CTX_FCR31($a0)
    .irp i,0,2,4,6,8,10,12,14,16,18,20,22,24,26,28,30
    sdc1    $f\i, (CTX_FPR + 8 * \i)($a0)
    .endr
    lui     $t1, 0x0400                 # SR_FR: odd registers are their own 64 bits
    and     $t1, $t0, $t1
    beqz    $t1, 1f
    nop
    .irp i,1,3,5,7,9,11,13,15,17,19,21,23,25,27,29,31
    sdc1    $f\i, (CTX_FPR + 8 * \i)($a0)
    .endr
1:
    mtc0    $t0, $12
    nop
    nop
    jr      $ra
    nop
    .set pop

# void tlb_save(struct state_ctx *ctx): all 32 entries plus the live EntryHi
# (tlbr overwrites it; the current ASID is put back). Clobbers t0-t7.
.global tlb_save
tlb_save:
    mfc0    $t6, $10
    sw      $t6, CTX_ENTRYHI($a0)
    addiu   $t1, $a0, CTX_TLB
    ori     $t0, $zero, 0
    ori     $t7, $zero, 32
1:
    mtc0    $t0, $0
    nop
    nop
    tlbr
    nop
    nop
    mfc0    $t2, $5
    mfc0    $t3, $10
    mfc0    $t4, $2
    mfc0    $t5, $3
    sw      $t2, 0($t1)
    sw      $t3, 4($t1)
    sw      $t4, 8($t1)
    sw      $t5, 12($t1)
    addiu   $t0, $t0, 1
    bne     $t0, $t7, 1b
    addiu   $t1, $t1, 16
    mtc0    $t6, $10
    nop
    jr      $ra
    nop

# void tlb_restore(const struct state_ctx *ctx): every entry is first parked on
# its own page at the top of kseg3 (never touched by a game), so no
# intermediate write can pair two entries on one VPN and shut the TLB down.
# Clobbers t0-t7.
.global tlb_restore
tlb_restore:
    ori     $t0, $zero, 0
    ori     $t7, $zero, 32
    lui     $t2, 0xFFC0
    mtc0    $zero, $5
    mtc0    $zero, $2
    mtc0    $zero, $3
1:
    mtc0    $t0, $0
    mtc0    $t2, $10
    nop
    nop
    tlbwi
    nop
    nop
    addiu   $t0, $t0, 1
    bne     $t0, $t7, 1b
    addiu   $t2, $t2, 0x2000
    ori     $t0, $zero, 0
    addiu   $t1, $a0, CTX_TLB
2:
    lw      $t2, 0($t1)
    lw      $t3, 4($t1)
    lw      $t4, 8($t1)
    lw      $t5, 12($t1)
    mtc0    $t0, $0
    mtc0    $t2, $5
    mtc0    $t3, $10
    mtc0    $t4, $2
    mtc0    $t5, $3
    nop
    nop
    tlbwi
    nop
    nop
    addiu   $t0, $t0, 1
    bne     $t0, $t7, 2b
    addiu   $t1, $t1, 16
    lw      $t6, CTX_ENTRYHI($a0)
    mtc0    $t6, $10
    nop
    jr      $ra
    nop

# void state_resume(const struct state_ctx *ctx): never returns. Loads the FPU
# (with CU1 forced on), HI/LO, EPC, the saved Status (EXL=1 as captured, so
# ERET clears it) and finally every GPR, then ERETs into the saved world.
.global state_resume
state_resume:
    .set push
    .set hardfloat
    or      $k0, $a0, $zero
    lw      $k1, CTX_STATUS($k0)
    lui     $t0, 0x2000
    or      $t1, $k1, $t0
    mtc0    $t1, $12
    nop
    nop
    nop
    nop
    lw      $t2, CTX_FCR31($k0)
    ctc1    $t2, $31
    nop
    .irp i,0,2,4,6,8,10,12,14,16,18,20,22,24,26,28,30
    ldc1    $f\i, (CTX_FPR + 8 * \i)($k0)
    .endr
    lui     $t0, 0x0400
    and     $t0, $k1, $t0
    beqz    $t0, 1f
    nop
    .irp i,1,3,5,7,9,11,13,15,17,19,21,23,25,27,29,31
    ldc1    $f\i, (CTX_FPR + 8 * \i)($k0)
    .endr
1:
    ld      $t0, CTX_LO($k0)
    mtlo    $t0
    ld      $t0, CTX_HI($k0)
    mthi    $t0
    lw      $t0, CTX_EPC($k0)
    mtc0    $t0, $14
    mtc0    $k1, $12
    nop
    nop
    nop
    .irp i,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,28,29,30,31
    ld      $\i, (8 * \i)($k0)
    .endr
    eret
    nop
    .set pop
