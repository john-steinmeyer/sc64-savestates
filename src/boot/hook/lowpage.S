# SC64SS borrowed-RAM mode: the code that must live in RDRAM, in the exception-vector
# page, because the monitor on the cart cannot do it (build.sh links this at the
# absolute addresses below; the boot patcher copies each fragment into place).
#
# Vector page use in borrowed mode (all below 0x400, which no game sweeps and the
# IPL3s leave alone; the 6105 IPL3's window 0x200..0x2FF is not touched):
#   0x010..0x060  the monitor's register scratch (10 x 64-bit: t0-t3, a0-a3, v0, ra);
#                 transient, so it may overlap a game's own exception scratch (Factor 5's
#                 handlers spill v0/ra/t0-t7 to 0x020..0x070: Indiana Jones)
#   0x060..0x080  the virtual pak's stub (hook.c pak_stub_place, 8 words: wait for the PI,
#                 then to monitor.S mon_pak_hit) while the pak serves in borrowed mode;
#                 nothing else (EXIT once lived here: Indiana Jones' page-in
#                 handler wrote its t6/t7 over it and the first tick's way out ran them;
#                 the game's Count/Compare across a borrow moved to the context on the
#                 cart, CTX_CLOCK)
#   0x090..0x0F0  the engine's tail (cheats.c SC64SS_LOWPAGE_ENGINE_ADDRESS, 24 words: the
#                 vector's `j 0x90` lands here on every exception; the watch redirect,
#                 then the gate at 0x360). Not spare: dev counters put at 0x0C0 hung a game.
#   0x0F0..0x100  EXIT_ERET, part 1, and 0x110..0x118 part 2 (around libultra's
#                 cache-error vector, 0x100): the load epilogue's eret, same restore first
#   0x118..0x120  the pre-install check's tail (see 0x1DC): an interrupt goes on to the
#                 cart installer through 0x178 (jr k0), anything else falls into the
#                 game's vector words at 0x120
#   0x130..0x180  PIO_W: a cart register write that waits until the write has landed
#                 (0x178: jr k0 / nop, the way to the cart installer)
#   0x190..0x1D8  data: +0 GUARD (a borrow is in progress), +4 REQ (the action asked
#                 for), +8/+C/+10 the combo hold counters, +14 TICKS (VI ticks seen),
#                 +18/+1C the game's words at 0x807FFFF8/FC (entry.S writes its stage
#                 marks there), +20 RESUME_DONE, +24 (dev) the tick returns to its
#                 caller, +28 PAD (the last buttons read), +2C PADOK (one was read),
#                 +30 LASTVI (Count at the last tick taken), +34 SISEEN / +38 SIOK (SI
#                 completions entered / that gave the buttons), +3C SIBLK (dev: the
#                 first word of the block last looked at), +40 PIDRAM / +44 PICART (the
#                 game's PI address registers, put back on every way out)
#   0x1DC..0x200  PRE_INSTALL: the engine's tail lands here on every exception until the
#                 gate is in (see below); after that the tail is j 0x360 and the cart
#                 installer copies EXIT over these words: the way back to the game after
#                 a tick or a save (the PI address registers restored last, then the
#                 game's vector words at 0x120)
#   0x360..0x3F0  GATE (the engine's tail lands here on every exception) + DMA_PRIM
#
# Why the exits are here too: code fetched from the cart is PIO, and every PIO access
# lands in PI_CART_ADDR (measured: after a borrow the hook found the trigger's last
# instruction address there). A game caught between writing its DMA addresses and its
# length would then transfer from wherever the monitor last executed, so the gate takes
# PI_CART_ADDR before the first cart fetch and the restore is the last PI access before
# the game continues, which only RAM code can do (the instruction after a cart-fetched
# store is itself a fetch).
#
# Why two homes: code fetched from the cart runs at ~200 cycles an instruction and,
# measured on hardware, cannot start a PI DMA (its own fetches contend with the
# transfer and the console freezes) nor safely follow a cart PIO write with a fetch
# (the read returns the write's data while it is in flight). So the DMA, the cart
# write and the cache sweep are RAM routines the cart monitor calls.

.set noreorder
.set noat

# ---- 0x360: GATE + DMA_PRIM (36 words available) ----------------------------------
# GATE: reached from the engine's tail on every exception with only k0/k1 free.
# Goes to the cart monitor's tick entry when the exception is an interrupt with the
# RCP's line up (Cause: ExcCode 0, IP2), a VI tick or an SI transfer has completed,
# no borrow is in progress and the PI is idle (cart code needs the bus); else straight
# to the game (GAME_TAIL, the engine's continuation at 0x120). The Cause check is what
# the resident hook does before it looks at the pad: the MI bits stay up until the
# game's handler acknowledges them, and Zelda takes some two thousand other exceptions
# a second (libultra's lazy FPU enable on thread switches) that would see them too.
.section .text.f360, "ax"
.global lp_gate
lp_gate:
    mfc0    $k1, $13                    # Cause
    andi    $k0, $k1, 0x007C            # ExcCode: 0 is an interrupt
    bnez    $k0, GAME_TAIL              # a fault, a syscall, the lazy FPU enable: not ours
    lui     $k0, 0x8000
    lw      $k1, 0x190($k0)             # GUARD
    bnez    $k1, GAME_TAIL
    lui     $k1, 0xA430
    lw      $k1, 0x0008($k1)            # MI_INTR
    andi    $k1, $k1, 0x002B            # the VI, an SI transfer done, the RSP or the RDP done?
    beqz    $k1, GAME_TAIL
    lui     $k1, 0xA460
    lw      $k0, 0x0010($k1)            # PI_STATUS
    andi    $k0, $k0, 0x0003            # DMA or IO busy: the cart is not fetchable now
    bnez    $k0, GAME_TAIL
    lw      $k1, 0x0004($k1)            # PI_CART_ADDR: the game's, before the monitor's fetches replace it
    lui     $k0, 0x8000
    sw      $k1, 0x1D4($k0)             # D_PICART
    lui     $k0, %hi(MON_TICK)
    addiu   $k0, $k0, %lo(MON_TICK)
    jr      $k0
    nop

# DMA_PRIM(a0 = RDRAM physical, a1 = cart PI address, a2 = length - 1, a3 = the length
# register: 0x08 PI_RD_LEN for RDRAM -> cart, 0x0C PI_WR_LEN for cart -> RDRAM): waits
# for the PI, runs one transfer to completion, clears the PI interrupt it raised.
# Uses t0/t1 (the caller has them saved) and a3.
.global lp_dma
lp_dma:
    lui     $t0, 0xA460
    addu    $a3, $t0, $a3               # the length register to poke
1:  lw      $t1, 0x0010($t0)
    andi    $t1, $t1, 0x0003
    bnez    $t1, 1b
    nop
    sw      $a0, 0x0000($t0)            # PI_DRAM_ADDR
    sw      $a1, 0x0004($t0)            # PI_CART_ADDR
    sw      $a2, 0x0000($a3)            # the length - 1: the transfer starts
3:  lw      $t1, 0x0010($t0)
    andi    $t1, $t1, 0x0003
    bnez    $t1, 3b
    addiu   $a3, $zero, 2               # (the delay slot, every pass) the interrupt-clear value
    jr      $ra
    sw      $a3, 0x0010($t0)            # clear the PI interrupt the transfer raised

# ---- EXIT (9 words at 0x1DC after the install: the cart installer copies it over the
# pre-install check, dead once the gate is in) -------------------------------------
# The monitor's way back to the game after a tick or a save: PI_DRAM_ADDR and
# PI_CART_ADDR as the game had them (D_PIDRAM, D_PICART), the store to PI_CART_ADDR the
# last PI access, then the game's relocated vector words at 0x120. k0/k1 only and no
# relative branch: position-independent, linked at a dummy home (lowpage.ld). It lived
# at 0x060 at first: Indiana Jones' exception handlers spill t6/t7 over
# 0x060..0x070, and the first tick's way out executed those words.
.section .text.fexit, "ax"
.global lp_exit
lp_exit:
    lui     $k0, 0x8000
    lw      $k1, 0x1D0($k0)             # D_PIDRAM
    lui     $k0, 0xA460
    sw      $k1, 0x0000($k0)            # PI_DRAM_ADDR
    lui     $k1, 0x8000
    lw      $k1, 0x1D4($k1)             # D_PICART
    j       0x80000120
    sw      $k1, 0x0004($k0)            # PI_CART_ADDR: the last PI access before the game's handler

# ---- 0x0F0 + 0x110: EXIT_ERET (4 + 4 words available, libultra's cache error vector
# between them) --------------------------------------------------------------------
# The load epilogue's eret: it restored PI_DRAM_ADDR itself (cart fetches leave that
# one alone), PI_CART_ADDR goes back here, last. k0/k1 only.
.section .text.f0f0, "ax"
.global lp_exit_eret
lp_exit_eret:
    lui     $k0, 0x8000
    lw      $k1, 0x1D4($k0)             # D_PICART
    b       lp_exit_eret2
    lui     $k0, 0xA460

.section .text.f110, "ax"
.global lp_exit_eret2
lp_exit_eret2:
    sw      $k1, 0x0004($k0)            # PI_CART_ADDR
    eret
# 0x118: the pre-install check's tail (from lp_preinstall at 0x1DC: k1 = the ExcCode,
# k0 = the cart installer's address). An interrupt goes on to the installer through
# 0x178 (jr k0: a jr here would have the game's vector word at 0x120 as its delay
# slot); anything else falls straight into the game's vector words at 0x120.
.global lp_tramp
lp_tramp:
    beqz    $k1, lp_tramp2
    nop

# ---- 0x130: PIO_W (20 words available) ---------------------------------------------
# PIO_W(a0 = kseg1 cart address, a1 = value): wait for the PI, store, settle ~8.5 us
# (the busy flag lags the write), then wait for the PI again. Uses t0/t1, k0/k1.
.section .text.f130, "ax"
.global lp_pio_w
lp_pio_w:
    lui     $t0, 0xA460
1:  lw      $t1, 0x0010($t0)
    andi    $t1, $t1, 0x0003
    bnez    $t1, 1b
    nop
    sw      $a1, 0($a0)
    mfc0    $k1, $9
2:  mfc0    $k0, $9
    subu    $k0, $k0, $k1
    sltiu   $k0, $k0, 400
    bnez    $k0, 2b
    nop
3:  lw      $t1, 0x0010($t0)
    andi    $t1, $t1, 0x0003
    bnez    $t1, 3b
    nop
    jr      $ra
    nop
# 0x178: the way to the cart installer (see lp_tramp)
.global lp_tramp2
lp_tramp2:
    jr      $k0
    nop

# ---- 0x1DC: PRE_INSTALL (9 words available) ----------------------------------------
# The engine's tail lands here on every exception until the gate is in (k0 = lui of
# the monitor's kseg1 base). The gate installer lives in the cart, and code fetched
# from the cart while a PI DMA is in flight freezes the console (measured; Indiana
# Jones' loader thread DMAs while its first page-ins fault), so the
# decision is made here in RAM: the PI busy, or not an interrupt, and the game's own
# vector words run (0x120, no cart fetch at all); an interrupt with the PI idle goes
# on to the installer through 0x118 and 0x178. Nothing here may sit at 0x1FC: a
# branch there would have the 6105 IPL3's window (0x200) as its delay slot.
# The cache sweep this hole used to hold now runs from the cart (monitor.S).
.section .text.f1dc, "ax"
.global lp_preinstall
lp_preinstall:
    lui     $k1, 0xA460
    lw      $k1, 0x0010($k1)            # PI_STATUS
    andi    $k1, $k1, 0x0003            # DMA or IO busy: the cart is not fetchable now
    bnez    $k1, GAME_TAIL
    mfc0    $k1, $13                    # (delay slot) Cause
    andi    $k1, $k1, 0x007C            # ExcCode: 0 is an interrupt
    j       lp_tramp
    ori     $k0, $k0, %lo(MON_INSTALL)
