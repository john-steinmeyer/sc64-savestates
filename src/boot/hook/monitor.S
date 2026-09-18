# SC64SS borrowed-RAM mode: the monitor. Lives on the cart (SC64SS_MONITOR_PI, run in
# place through the uncached window, ~200 cycles an instruction) and orchestrates a
# borrow: the vector-page gate (lowpage.S) sends every VI interrupt here while the
# game runs; a held combo (or a pending resume) makes this code stash the hook's
# 128 KiB home to the cart, pull the hook in from its staging copy, and enter it as
# the engine's tail would; when the hook returns (entry.S jumps to mon_epilogue
# instead of the game) the region is put back and the game continues.
#
# Contract: entered with only k0/k1 free, every other register live game state.
# What this code uses (t0-t3, a0-a3, v0, ra) is parked in the vector page's scratch
# (0x80000010, 64-bit each) and restored before leaving. Anything that must run from
# RDRAM (a DMA, a cart register write, the cache sweep) is called through a register:
# a plain j/jal cannot reach the vector page from the cart's 256 MiB region.
#
# build.sh links this at SC64SS_MONITOR_KSEG1 and passes: CFG_ADDR (the staged hook's
# config block, kseg1), HOOK_LEN1 (the blob's 16-aligned size minus one), STAGING_PI, STASH_PI,
# LP_DMA, LP_PIO_W, LP_EXIT, LP_EXIT_ERET (lowpage.S), CTX_KSEG1 (the
# loaded context's cart copy). Layout: mon_epilogue at +0, mon_tick at +0x100 (the
# gate's target), mon_pak_hit at +0x1200 (the virtual pak's server), mon_epi_load at
# +0x2200 (the hook's target after a load), mon_install at +0x3000 (monitor.ld; the
# engine's tail until the gate is in). 16 KiB in all (hook.c MONITOR_LEN).
# Every way back to the game goes through a RAM stub that restores the PI address
# registers last: code fetched from the cart is PIO, and every PIO access lands in
# PI_CART_ADDR (measured: the hook found the trigger's last instruction address there),
# so a game caught between writing its DMA addresses and its length would otherwise
# transfer from wherever the monitor last executed.

.set noreorder
.set noat

# hook_cfg offsets (hook.c, checked there with static asserts)
.set CFG_COMBO_SAVE,    0x28
.set CFG_COMBO_LOAD,    0x2C
.set CFG_COMBO_HOLD,    0x30
.set CFG_COMBO_ENABLED, 0x34
.set CFG_COMBO_MENU,    0x60
.set CFG_COMBO_SHOT,    0xA4
.set SHOT_FLAG,         0xBFFE0F60      # cart buffer word: the screenshot button was down at the last tick
.set CFG_SPARE,         0x6C
.set CFG_WATCH0,        0x70                # (dev) four RAM addresses the heartbeat samples
.set CFG_PC_REQ,        0x80                # a request from the PC: op in the low byte
.set CFG_PC_PAD,        0x8C                # the PC's pad: buttons << 16 | x << 8 | y ...
.set CFG_PC_PADN,       0x90                # ... for these polls (sequence << 16 | polls)
# vector-page data (lowpage.S)
.set D_GUARD,   0x190
.set D_REQ,     0x194
.set D_HOLD_S,  0x198
.set D_HOLD_L,  0x19C
.set D_HOLD_M,  0x1A0
.set D_TICKS,   0x1A4
.set D_DIAG0,   0x1A8
.set D_DIAG1,   0x1AC
.set D_RESUMED, 0x1B0
.set D_PAD,     0x1B8
.set D_PADOK,   0x1BC
.set D_LASTVI,  0x1C0                   # Count at the last VI tick taken
.set D_SISEEN,  0x1C4                   # SI completions entered
.set D_SIOK,    0x1C8                   # ... that gave channel 0's buttons
.set D_PCPAD,   0x1CC                   # the PC's pad request: the sequence seen << 16 | polls left
.set D_PIDRAM,  0x1D0                   # the game's PI_DRAM_ADDR (the tick takes it) ...
.set D_PICART,  0x1D4                   # ... and PI_CART_ADDR (the gate takes it): restored last
.set D_SIAGE,   0x1D8                   # ticks since an SI completion last gave the buttons
.set D_SITERA,  0x1B4                   # the pak server's entry from the game's handler: its ra
# the virtual Controller Pak's control block on the cart (hook.c VPAK_CTL_PI; build.sh
# passes PAK_CTL, its kseg1 address, and VPAK_KSEG1, the image's): the menu writes it at
# launch, the hook fills the site words in a service borrow, the server below reads them
# the register scratch: 0x010..0x058 (t0-t3, a0-a3, v0, ra, k0, k1 as 64-bit words) unless the
# build says otherwise (build.sh MON_SCRATCH -> --defsym SCRATCH): a game whose own code
# lives at 0x000..0x07F (GoldenEye's TLB refill handler) needs it at 0x2A8, the free
# words after the IPL3 epilogue
.ifndef SCRATCH
.set SCRATCH, 0x10
.endif
.set PAK_CTL_STATE,  0x04               # 0 install pending, 1 the game's handler serves, 2 no site, 3 changed
.set PAK_CTL_DIRTY,  0x08               # 0 clean, else 0x80000000 | the tick of the last write
.set PAK_CTL_BANK,   0x0C               # (unused: reads at 0x8000 and above answer zeros)
.set PAK_CTL_SCHEME, 0x10               # 1: the site's jal comes here and goes back to its own send_mesg;
                                        # 2: the four words were ours (a kuseg handler): send_mesg from here
.set PAK_CTL_VEC,    0x28               # send_mesg, the word after the jal's delay slot, a0, rX | lo << 16
.set PAK_CTL_LIVE,   0x38               # the live word (hook.c PAK_LIVE_*): bits 0..1 the channel the pak sits in,
                                        # 0x10 inserted, 0x20 "pak changed" to report on the channel's next identify
.set CTX_CLOCK,      0x220              # after the context: the game's Count and Compare across a borrow

.macro SAVE_REGS
    lui     $k0, 0x8000
    sd      $t0, (SCRATCH + 0x00)($k0)
    sd      $t1, (SCRATCH + 0x08)($k0)
    sd      $t2, (SCRATCH + 0x10)($k0)
    sd      $t3, (SCRATCH + 0x18)($k0)
    sd      $a0, (SCRATCH + 0x20)($k0)
    sd      $a1, (SCRATCH + 0x28)($k0)
    sd      $a2, (SCRATCH + 0x30)($k0)
    sd      $a3, (SCRATCH + 0x38)($k0)
    sd      $v0, (SCRATCH + 0x40)($k0)
    sd      $ra, (SCRATCH + 0x48)($k0)
.endm

.macro LOAD_REGS
    lui     $k0, 0x8000
    ld      $t0, (SCRATCH + 0x00)($k0)
    ld      $t1, (SCRATCH + 0x08)($k0)
    ld      $t2, (SCRATCH + 0x10)($k0)
    ld      $t3, (SCRATCH + 0x18)($k0)
    ld      $a0, (SCRATCH + 0x20)($k0)
    ld      $a1, (SCRATCH + 0x28)($k0)
    ld      $a2, (SCRATCH + 0x30)($k0)
    ld      $a3, (SCRATCH + 0x38)($k0)
    ld      $v0, (SCRATCH + 0x40)($k0)
    ld      $ra, (SCRATCH + 0x48)($k0)
.endm

# call a vector-page routine (t2 carries the address; t0/t1/k0/k1 are theirs)
.macro CALL sym
    lui     $t2, %hi(\sym)
    addiu   $t2, $t2, %lo(\sym)
    jalr    $t2
    nop
.endm

# PIO_W(addr, value) through lowpage.S
.macro PIOW addr, value
    lui     $a0, %hi(\addr)
    addiu   $a0, $a0, %lo(\addr)
    lui     $a1, %hi(\value)
    addiu   $a1, $a1, %lo(\value)
    CALL    LP_PIO_W
.endm

.macro TO_GAME
    lui     $k0, %hi(LP_EXIT)
    addiu   $k0, $k0, %lo(LP_EXIT)      # RAM: the PI address registers back, then the game's vector words
    jr      $k0
    nop
.endm

# (dev) the game's frame rate under the engine: the VI tick keeps it (DEVFPS below,
# from mt_vi and from the tick-off path at mon_tick), read from a PC over USB
.macro DEVFPS
.endm

.section .text, "ax"

# ---- +0: the epilogue -------------------------------------------------------------
# From entry.S after the hook's run (its registers restored: the game's, or a loaded
# state's). Put the game's 128 KiB back over the hook and continue as the engine's
# tail would have.
.global mon_epilogue
mon_epilogue:
    SAVE_REGS
    jal     mon_cache_flush             # the hook's dirty lines out, every line dropped
    nop
    lui     $a0, 0x007D                 # RDRAM 0x007D0000
    lui     $a1, %hi(STASH_PI)
    addiu   $a1, $a1, %lo(STASH_PI)
    lui     $a2, 0x0001
    ori     $a2, $a2, 0xFFFF            # 128 KiB - 1
    addiu   $a3, $zero, 0x0C            # cart -> RDRAM (PI_WR_LEN)
    CALL    LP_DMA
    lui     $k0, 0x8000
    lw      $t1, D_PIDRAM($k0)          # the game's PI_DRAM_ADDR back (PI_CART_ADDR: the exit stub)
    lui     $t0, 0xA460
    sw      $t1, 0x0000($t0)
    lw      $t1, D_DIAG0($k0)           # the game's words under entry.S's stage marks
    lui     $t0, 0xA080
    sw      $t1, -8($t0)
    lw      $t1, D_DIAG1($k0)
    sw      $t1, -4($t0)
    sw      $zero, D_GUARD($k0)
    lui     $t0, %hi(CTX_KSEG1 + CTX_CLOCK)
    lw      $t1, %lo(CTX_KSEG1 + CTX_CLOCK)($t0)       # the game's clock as mt_trigger found it: the hook
    addiu   $t1, $t1, 400                               # hid its own span (vi_frz_end, borrow_run), this
    mtc0    $t1, $9                                     # hides the copies around it (Episode I Racer's
    lw      $t1, %lo(CTX_KSEG1 + CTX_CLOCK + 4)($t0)   # physics ran on every frame the game was held)
    mtc0    $t1, $11
    LOAD_REGS
    TO_GAME

# a combo: mask in a0, hold count in a1, buttons in t3; the counter restarts when the
# combo is not held and fires exactly once when it reaches the hold (holding longer
# does nothing, as the resident hook does it)
# a combo fully held is the monitor's, not the game's: its buttons are cleared in the
# block the game is about to read (a1 = the mask, t3 = the buttons, a2 = the entry)
.macro COMBO hold, op
    beqz    $a0, 1f
    and     $a2, $t3, $a0
    bne     $a2, $a0, 1f
    nop
    lw      $a3, \hold($k0)
    addiu   $a3, $a3, 1
    sw      $a3, \hold($k0)
    bne     $a3, $a1, 2f
    lui     $a2, 0x0001
    sw      $a2, \hold($k0)             # held long enough: armed (0x10000 + the VI ticks since);
    b       2f                          # mt_armed takes the first clean entry
    nop
1:  lw      $a3, \hold($k0)
    andi    $a3, $a3, 0x4000            # bit 14: armed by the PC, not by these buttons
    bnez    $a3, 2f
    nop
    sw      $zero, \hold($k0)
2:
.endm

# ---- +0x100: the tick -------------------------------------------------------------
# Every VI interrupt while no borrow is in progress and the PI is idle (lowpage.S
# checked). Watches the game's own controller block for the combos and counts VI
# ticks for a pending resume.
.balign 256
.global mon_tick
mon_tick:
    lui     $k0, 0x8000
    lui     $k1, 0xA460
    lw      $k1, 0x0000($k1)            # PI_DRAM_ADDR: still the game's (fetches leave it alone;
    sw      $k1, D_PIDRAM($k0)          # the gate took PI_CART_ADDR); every exit puts both back
    lui     $k1, 0xA430
    lw      $k1, 0x0008($k1)            # MI_INTR: what brought us here
    andi    $k1, $k1, 0x000A
    bnez    $k1, 1f                     # an SI completion or the VI: the tick proper
    lw      $k1, D_HOLD_S($k0)          # an RSP- or RDP-done interrupt (the gate admits them
    srl     $k1, $k1, 16                # too): worth a look only while a borrow is armed
    addiu   $k1, $k1, -1                # (counter 0x1xxxx; 0x2xxxx is fired-and-still-held)
    beqz    $k1, 1f
    lw      $k1, D_HOLD_L($k0)
    srl     $k1, $k1, 16
    addiu   $k1, $k1, -1
    beqz    $k1, 1f
    lw      $k1, D_HOLD_M($k0)
    srl     $k1, $k1, 16
    addiu   $k1, $k1, -1
    beqz    $k1, 1f
    lui     $k0, %hi(LP_EXIT)
    addiu   $k0, $k0, %lo(LP_EXIT)      # nothing armed: the PI registers back, the game's handler
    jr      $k0
    nop
1:  SAVE_REGS
    lui     $t0, 0xA430
    lw      $t0, 0x0008($t0)            # MI_INTR: what brought us here
    andi    $t1, $t0, 0x0002
    beqz    $t1, mt_vi                  # no SI transfer completed: straight to the tick
    nop
    # ---- a completed SI transfer (one entry per transfer: the gate admits interrupt
    # exceptions only, and libultra acknowledges the SI in its handler): the game's
    # block is fresh. Find channel 0's read reply the way the resident hook's
    # pad_parse does, in the block that ends at SI_DRAM_ADDR (start + 0x38 after a
    # 64-byte transfer) or the one that starts there.
    lw      $t1, D_SISEEN($k0)
    addiu   $t1, $t1, 1
    sw      $t1, D_SISEEN($k0)
    lui     $t2, 0xA480
    lw      $t2, 0x0000($t2)
    lui     $t3, 0x0100
    addiu   $t3, $t3, -1
    and     $t2, $t2, $t3               # the 24-bit RDRAM address
    sltiu   $t1, $t2, 0x38
    bnez    $t1, mt_vi                  # nothing sensible there yet (boot)
    lui     $t3, 0x0080
    addiu   $t3, $t3, -0x40
    sltu    $t1, $t3, $t2
    bnez    $t1, mt_vi                  # beyond 8 MiB - 64: not a block of the game's
    lui     $t3, 0xA000
    or      $t2, $t2, $t3
    addiu   $a3, $t2, -0x38
    # the virtual Controller Pak (cfg spare bit 2): the accessory command of the channel
    # the pak sits in, in the block that ends here, is answered now, unless the game's
    # handler has been patched to do it at its own SI acknowledge (control block state 1:
    # mon_pak_hit below), which
    # also catches the completions libultra finds already pending while in for another
    # interrupt. mp_serve clobbers the tick's working registers: the block is found again.
    lui     $t1, %hi(CFG_ADDR)
    addiu   $t1, $t1, %lo(CFG_ADDR)
    lw      $t1, CFG_SPARE($t1)
    andi    $t1, $t1, 0x0004
    beqz    $t1, 3f
    lui     $t1, %hi(PAK_CTL)
    lw      $t1, %lo(PAK_CTL + PAK_CTL_STATE)($t1)
    addiu   $t1, $t1, -1
    beqz    $t1, 3f                     # the handler serves
    nop
    jal     mp_serve
    move    $a0, $a3
    lui     $k0, 0x8000
    lui     $t2, 0xA480
    lw      $t2, 0x0000($t2)
    lui     $t3, 0x0100
    addiu   $t3, $t3, -1
    and     $t2, $t2, $t3
    lui     $t3, 0xA000
    or      $t2, $t2, $t3
    addiu   $a3, $t2, -0x38
3:  jal     mt_parse
    move    $a0, $a3
    bnez    $v0, 1f
    nop
    jal     mt_parse
    addiu   $a0, $a3, 0x38
    beqz    $v0, mt_vi                  # no controller data in either block (by name: the hide
    nop                                 # block below has numeric labels of its own)
1:  sw      $t3, D_PAD($k0)             # the buttons, for the ticks to come
    addiu   $t1, $zero, 1
    sw      $t1, D_PADOK($k0)
    sw      $zero, D_SIAGE($k0)         # (the tick's refresh stands down)
    lw      $t1, D_SIOK($k0)
    addiu   $t1, $t1, 1
    sw      $t1, D_SIOK($k0)
    # a fully held combo is cleared from the game's block (as the resident hook does):
    # no pause under the panel combo, no menu moves under L+R+D-pad. mt_parse left a0 = the
    # block and t0 = channel 0's entry; the buttons are its bytes 3 and 4.
    lui     $t1, %hi(CFG_ADDR)
    addiu   $t1, $t1, %lo(CFG_ADDR)
    lw      $a1, CFG_COMBO_ENABLED($t1)
    beqz    $a1, 6f
    addu    $a2, $a0, $t0
    # (t1 = the cfg block, a2 = channel 0's entry, t3 = the buttons) a0: the buttons to
    # hide, the masks of the combos fully held
    lw      $a1, CFG_COMBO_SAVE($t1)
    lw      $a3, CFG_COMBO_LOAD($t1)
    lw      $v0, CFG_COMBO_MENU($t1)
    move    $a0, $zero
    lw      $t2, CFG_COMBO_SHOT($t1)    # the screenshot button, held: the routine's alone
    beqz    $t2, 0f                     # (a2 = channel 0's entry stays; a0 is the temp)
    nop
    and     $a0, $t3, $t2
    bne     $a0, $t2, 0f
    move    $a0, $zero
    move    $a0, $t2
0:  and     $t2, $t3, $a1
    bne     $t2, $a1, 1f
    nop
    or      $a0, $a0, $a1
1:  and     $t2, $t3, $a3
    bne     $t2, $a3, 2f
    nop
    or      $a0, $a0, $a3
2:  and     $t2, $t3, $v0
    bne     $t2, $v0, 3f
    nop
    or      $a0, $a0, $v0
3:  beqz    $a0, 6f                     # none held
    ori     $t2, $zero, 0xFFFF          # the buttons all the combos share (L + R as configured)
    beqz    $a1, 4f                     # stay visible: the game saw them held already, and would
    nop                                 # see them released and pressed again around a tap of the
    and     $t2, $t2, $a1               # D-pad or Start. A lone combo hides all of itself.
4:  beqz    $a3, 5f
    nop
    and     $t2, $t2, $a3
5:  beqz    $v0, 7f
    nop
    and     $t2, $t2, $v0
7:  beq     $t2, $a0, 8f
    nor     $t2, $t2, $zero             # (the delay slot: ~common, unused past the branch)
    and     $a0, $a0, $t2
8:  lbu     $a1, 3($a2)
    lbu     $a3, 4($a2)
    sll     $a1, $a1, 8
    or      $a1, $a1, $a3
    nor     $a0, $a0, $zero
    and     $a1, $a1, $a0
    srl     $a3, $a1, 8
    sb      $a3, 3($a2)
    sb      $a1, 4($a2)
6:  # a PC's pad over USB (press/hold/stick): cfg pc_pad = buttons << 16 |
    # x << 8 | y, pc_padn = a sequence << 16 | the polls it lasts. The count runs in
    # RAM (D_PCPAD: the sequence seen, polls left; the monitor cannot write the cart's
    # ROM space) and the bytes go over channel 0's answer, so the game reads them.
    # D_PAD keeps the real buttons: the combos are the player's, not the PC's.
    lw      $a1, CFG_PC_PADN($t1)
    beqz    $a1, 2f
    nop
    lw      $a3, D_PCPAD($k0)
    srl     $a0, $a1, 16
    srl     $t2, $a3, 16
    beq     $a0, $t2, 4f                # the request already seen: its polls left are in RAM
    nop
    move    $a3, $a1                    # a new request: the count from the cart
4:  andi    $t2, $a3, 0xFFFF
    beqz    $t2, 5f                     # spent
    nop
    addiu   $a3, $a3, -1
    sw      $a3, D_PCPAD($k0)
    lw      $a3, CFG_PC_PAD($t1)
    srl     $a0, $a3, 24
    sb      $a0, 3($a2)                 # buttons, high byte (A B Z Start, the D-pad)
    srl     $a0, $a3, 16
    sb      $a0, 4($a2)                 # buttons, low byte (L R, the C buttons)
    srl     $a0, $a3, 8
    sb      $a0, 5($a2)                 # stick x
    b       2f
    sb      $a3, 6($a2)                 # stick y
5:  sw      $a3, D_PCPAD($k0)
2:
mt_vi:
    # the gate admits interrupt exceptions only, so this is the VI's own; still, a
    # tick is a tick only half a frame or more after the last one, as the resident
    # hook counts them (a backstop: Zelda showed two thousand entries a second before)
    lui     $t0, 0xA430
    lw      $t0, 0x0008($t0)
    andi    $t0, $t0, 0x0008
    beqz    $t0, mt_armed               # not the VI's: an RSP/RDP-done entry for an armed borrow
    mfc0    $t0, $9
    lw      $t1, D_LASTVI($k0)
    subu    $t1, $t0, $t1
    lui     $t2, 0x0005
    ori     $t2, $t2, 0xF370            # 390,000 Count ticks: half a frame
    sltu    $t1, $t1, $t2
    bnez    $t1, mt_armed
    nop
    sw      $t0, D_LASTVI($k0)
    lw      $t0, D_TICKS($k0)
    addiu   $t0, $t0, 1
    sw      $t0, D_TICKS($k0)
    DEVFPS
    # the buttons from the block of the last completed transfer, at every tick as well:
    # a libdragon game serves most of its controller transfers with interrupts off and
    # no SI exception at all (its synchronous joybus path polls the SI and acknowledges
    # it itself). With the SI idle, the block is complete: the one that ends at
    # SI_DRAM_ADDR, or the one that starts there. Not within a few ticks of a completion
    # that gave the buttons: a held combo was cleared from that block, and a game that
    # polls every other frame would read as released on the frames between.
    lw      $t0, D_SIAGE($k0)
    addiu   $t0, $t0, 1
    sw      $t0, D_SIAGE($k0)
    sltiu   $t1, $t0, 4
    bnez    $t1, 8f
    nop
    lui     $t1, 0xA480
    lw      $t0, 0x0018($t1)            # SI_STATUS
    andi    $t0, $t0, 0x0003
    bnez    $t0, 8f                     # a transfer in flight
    lw      $t2, 0x0000($t1)            # SI_DRAM_ADDR
    lui     $t3, 0x0100
    addiu   $t3, $t3, -1
    and     $t2, $t2, $t3
    sltiu   $t1, $t2, 0x38
    bnez    $t1, 8f                     # nothing sensible there yet (boot)
    lui     $t3, 0x0080
    addiu   $t3, $t3, -0x40
    sltu    $t1, $t3, $t2
    bnez    $t1, 8f                     # beyond 8 MiB - 64: not a block of the game's
    lui     $t3, 0xA000
    or      $t2, $t2, $t3
    addiu   $a3, $t2, -0x38
    jal     mt_parse
    move    $a0, $a3
    bnez    $v0, 7f
    nop
    jal     mt_parse
    addiu   $a0, $a3, 0x38
    beqz    $v0, 8f
    nop
7:  sw      $t3, D_PAD($k0)
    addiu   $t1, $zero, 1
    sw      $t1, D_PADOK($k0)
8:  lui     $t1, %hi(CFG_ADDR)
    addiu   $t1, $t1, %lo(CFG_ADDR)     # the staged hook's config block (PIO reads)
    lw      $t3, D_PAD($k0)             # the buttons from the last completed read
    # the virtual Controller Pak's service borrow (op 6; cfg spare bit 2): at the first
    # tick the hook patches the game's handler at its SI acknowledge to serve the pak
    # (control block state 0 until then; a game probes its pak within a second of its
    # first interrupt), and 90 ticks after the last pak write (the dirty stamp) it mirrors
    # the image to the card. Armed on the panel's counter the PC's way (bit 14: the VI
    # entries count its ticks, no buttons cancel it) with bit 13 for the op; never while
    # a counter is armed or fired.
    lw      $a2, CFG_SPARE($t1)
    andi    $a2, $a2, 0x0004
    beqz    $a2, mt_padck
    lw      $a2, D_HOLD_S($k0)
    lw      $a0, D_HOLD_L($k0)
    or      $a2, $a2, $a0
    lw      $a0, D_HOLD_M($k0)
    or      $a2, $a2, $a0
    srl     $a2, $a2, 16
    bnez    $a2, mt_padck
    lui     $a0, %hi(PAK_CTL)
    lw      $a2, %lo(PAK_CTL + PAK_CTL_STATE)($a0)
    beqz    $a2, 2f                     # the install: now (the game's handler is in before its first
    nop                                 # interrupt, and its pak probes start right after: Robotron 64)
    lw      $a2, %lo(PAK_CTL + PAK_CTL_DIRTY)($a0)
    beqz    $a2, mt_padck               # clean
    lw      $a3, D_TICKS($k0)
    sll     $a2, $a2, 1
    srl     $a2, $a2, 1                 # the stamp's tick (bit 31 marks a stamp)
    subu    $a3, $a3, $a2
    sltiu   $a3, $a3, 90
    bnez    $a3, mt_padck               # written less than 90 ticks ago: the burst may go on
    nop
2:  lui     $a1, 0x0001
    ori     $a1, $a1, 0x6000            # armed, the PC's way (bit 14), the pak's op (bit 13)
    b       mt_resume
    sw      $a1, D_HOLD_M($k0)
mt_padck:
    lw      $a2, D_PADOK($k0)
    beqz    $a2, mt_resume              # the game has not polled its controller yet
    nop
    lw      $a2, CFG_COMBO_ENABLED($t1)
    beqz    $a2, mt_resume
    lw      $a1, CFG_COMBO_HOLD($t1)
    lw      $a0, CFG_COMBO_SAVE($t1)
    COMBO   D_HOLD_S, 1
    lw      $a0, CFG_COMBO_LOAD($t1)
    COMBO   D_HOLD_L, 2
    lw      $a0, CFG_COMBO_MENU($t1)
    COMBO   D_HOLD_M, 3
    # the screenshot button: its press (down now, up at the last tick: a flag in the cart
    # buffer, the vector page has no word to spare) arms the panel's counter the PC's way
    # (bit 14: not cancelled by the release) with bit 11, op 7 at the trigger. Not while a
    # borrow is in flight or a counter is armed or fired.
    lw      $a0, CFG_COMBO_SHOT($t1)
    beqz    $a0, mt_pcreq
    lui     $a3, %hi(SHOT_FLAG)
    and     $a2, $t3, $a0
    bne     $a2, $a0, 4f                # not down
    lw      $a2, %lo(SHOT_FLAG)($a3)
    bnez    $a2, mt_pcreq               # down since the last tick: nothing new
    lui     $a0, %hi(SHOT_FLAG)
    addiu   $a0, $a0, %lo(SHOT_FLAG)
    addiu   $a1, $zero, 1
    CALL    LP_PIO_W                    # the flag up (clobbers t0-t2, k0, k1)
    lui     $k0, 0x8000
    lui     $t1, %hi(CFG_ADDR)
    addiu   $t1, $t1, %lo(CFG_ADDR)
    lw      $a2, D_GUARD($k0)
    bnez    $a2, mt_pcreq
    lw      $a2, D_HOLD_S($k0)
    lw      $a1, D_HOLD_L($k0)
    or      $a2, $a2, $a1
    lw      $a1, D_HOLD_M($k0)
    or      $a2, $a2, $a1
    srl     $a2, $a2, 16
    bnez    $a2, mt_pcreq
    lui     $a1, 0x0001
    ori     $a1, $a1, 0x4800            # armed, the PC's way, op 7 at the trigger
    b       mt_pcreq
    sw      $a1, D_HOLD_M($k0)
4:  beqz    $a2, mt_pcreq               # up, and the flag down already
    nop
    lui     $a0, %hi(SHOT_FLAG)
    addiu   $a0, $a0, %lo(SHOT_FLAG)
    move    $a1, $zero
    CALL    LP_PIO_W                    # the flag down
    lui     $k0, 0x8000
    lui     $t1, %hi(CFG_ADDR)
    addiu   $t1, $t1, %lo(CFG_ADDR)
mt_pcreq:
    # a request from the PC (hook_cfg.pc_req on the cart: op in the low byte, 1 save,
    # 2 load, 3 panel, 5 screenshot for the PC, 7 screenshot to the card): armed like a
    # combo on that op's counter, with bit
    # 14 (the PC's, not cancelled by released buttons) and, for a screenshot, bit 15
    # (mt_trigger makes it op 5 on the panel's counter). Left alone while a borrow is
    # in flight or a counter is armed or fired; the hook clears the word and answers in
    # pc_ack when the borrow is done.
    lw      $a0, CFG_PC_REQ($t1)
    beqz    $a0, mt_resume
    andi    $a0, $a0, 0x00FF
    lw      $a2, D_GUARD($k0)
    bnez    $a2, mt_resume
    lw      $a2, D_HOLD_S($k0)
    lw      $a1, D_HOLD_L($k0)
    or      $a2, $a2, $a1
    lw      $a1, D_HOLD_M($k0)
    or      $a2, $a2, $a1
    srl     $a2, $a2, 16
    bnez    $a2, mt_resume
    lui     $a1, 0x0001
    ori     $a1, $a1, 0x4000            # armed, the PC's, tick 0
    addiu   $a2, $zero, 1
    beq     $a0, $a2, 5f
    addiu   $a2, $zero, 2
    beq     $a0, $a2, 6f
    addiu   $a2, $zero, 3
    beq     $a0, $a2, 7f
    addiu   $a2, $zero, 5
    beq     $a0, $a2, 8f
    addiu   $a2, $zero, 7
    bne     $a0, $a2, mt_resume         # not an op of ours: ignored
    nop
    b       7f
    ori     $a1, $a1, 0x0800            # (delay slot) the screenshot to the card: bit 11, op 7 at the trigger
8:  ori     $a1, $a1, 0x8000            # a screenshot for the PC: the panel's counter, op 5 at the trigger
7:  b       mt_resume
    sw      $a1, D_HOLD_M($k0)
6:  b       mt_resume
    sw      $a1, D_HOLD_L($k0)
5:  sw      $a1, D_HOLD_S($k0)
mt_resume:
    lui     $k0, 0x8000
    lui     $t1, %hi(CFG_ADDR)              # (mt_check and the heartbeat clobber t1)
    addiu   $t1, $t1, %lo(CFG_ADDR)
    # a suspended state (cfg spare bits 8..11): after 60 VI ticks, once the game has
    # polled its controller, its load is ARMED the way a held combo's is (the load's
    # hold word: 0x10000 armed, bit 14 keeps it past the combo code, bit 12 names the
    # resume, op 4 at mt_trigger), so mt_armed enters the hook at a clean tick only.
    # The borrow used to begin right here, at whatever tick this was: on
    # Super Mario 64's boot the RCP was mid-frame, the hook found no clean moment and
    # gave up, and nothing tried again (the dev build's heartbeat delay hid it). A try
    # that fizzles (dropped unarmed after 120 ticks, or fired and failed in the hook)
    # is repeated every 180 ticks until a load goes through (the load epilogue writes
    # -1 to D_RESUMED) or 1800 ticks have passed.
    lw      $t2, D_PADOK($k0)
    beqz    $t2, mt_armed
    lw      $t3, D_TICKS($k0)           # (delay slot)
    lw      $t2, CFG_SPARE($t1)
    srl     $t2, $t2, 8
    andi    $t2, $t2, 0x000F
    beqz    $t2, mt_armed
    sltiu   $t2, $t3, 60                # (delay slot) not before 60 ticks
    bnez    $t2, mt_armed
    sltiu   $t2, $t3, 1801              # (delay slot) and not after 1800
    beqz    $t2, mt_armed
    lw      $t2, D_RESUMED($k0)         # (delay slot) 0: no try yet; else the tick of the next try (-1: done)
    sltu    $t2, $t3, $t2
    bnez    $t2, mt_armed               # not yet
    addiu   $t2, $t3, 180               # (delay slot) the next try, 180 ticks on
    sw      $t2, D_RESUMED($k0)
    lui     $t2, 0x0001
    ori     $t2, $t2, 0x5000            # armed, the PC's way (bit 14), the resume (bit 12)
    b       mt_armed
    sw      $t2, D_HOLD_L($k0)          # (delay slot)
mt_out:
    LOAD_REGS
    TO_GAME

# A borrow can only start at a moment a state can be taken from: one RCP interrupt
# pending (the VI's, or the RSP's or the RDP's done), the RSP halted with no DMA, the
# RDP idle, the SI idle (the PI is idle already, the gate checked). Frozen inside one
# interrupt the hook could never wait such a moment out, as the resident one does; so
# an armed borrow (a hold counter at 0x10000 + the VI ticks since the combo was held
# long enough) takes the first entry that will do: the VI's own for the first 30
# ticks, then an RSP- or RDP-done interrupt too (the resident hook's preference for a
# frame boundary, and its fallback mid-pipeline: Perfect Dark's RSP or RDP is busy at
# every VI). After 120 ticks with no such moment it is dropped, and the next 12 held
# ticks arm it again. Reached from a VI tick (after the heartbeat) and from the other
# entries.
mt_armed:
    lui     $k0, 0x8000
    lui     $t0, 0xA430
    lw      $t0, 0x0008($t0)
    andi    $t3, $t0, 0x0008            # t3: entered on the VI?
    addiu   $v0, $zero, 1               # the action: 1 save (D_HOLD_S), 2 load (+4), 3 panel (+8)
1:  sll     $a2, $v0, 2
    addiu   $a2, $a2, D_HOLD_S - 4      # the counter's offset
    addu    $t0, $k0, $a2
    lw      $t1, 0($t0)
    srl     $t2, $t1, 16
    addiu   $t2, $t2, -1
    bnez    $t2, 3f                     # not armed (0: counting or idle; 2: fired, still held)
    andi    $t2, $t1, 0x4000            # the PC's: nobody holds buttons to count its ticks,
    beqz    $t2, 5f                     # so the VI entries count them here
    nop
    beqz    $t3, 5f
    nop
    addiu   $t1, $t1, 1
    sw      $t1, 0($t0)
5:  andi    $t1, $t1, 0x07FF            # the ticks armed (bit 11 the screenshot op, 12 the resume,
                                        # 13 the pak op, 14 and 15 the PC flags: all outside the mask)
    sltiu   $t2, $t1, 120
    beqz    $t2, 4f                     # two seconds without a clean moment: dropped
    sltiu   $t2, $t1, 30
    beqz    $t2, 2f                     # armed long enough: any entry will do
    nop
    beqz    $t3, 3f                     # early: the VI's own entries only
    nop
2:  jal     mt_why
    nop
    beqz    $t2, mt_trigger             # clean: v0 = the action, a2 = its counter
    nop
    b       3f
    nop
4:  sw      $zero, 0($t0)
3:  addiu   $v0, $v0, 1
    sltiu   $t1, $v0, 4
    bnez    $t1, 1b
    nop
    b       mt_out
    nop

# mt_why: t2 = why this tick will not do for a borrow (0: it will). Uses t0/t1.
mt_why:
    addiu   $t2, $zero, 1
    lui     $t1, 0xA430
    lw      $t0, 0x0008($t1)            # MI_INTR ...
    lw      $t1, 0x000C($t1)            # ... of the interrupts the game has enabled (MI_MASK):
    and     $t0, $t0, $t1               # one it masks and polls instead is no event to keep
    andi    $t0, $t0, 0x003F
    addiu   $t1, $zero, 0x0008
    beq     $t0, $t1, 1f                # the VI alone: a frame boundary
    addiu   $t1, $zero, 0x0001
    beq     $t0, $t1, 1f                # the RSP done, alone (the hook re-raises it on load)
    addiu   $t1, $zero, 0x0020
    bne     $t0, $t1, 9f                # 1: some other mix of RCP interrupts pending (the RDP
    nop                                 #    done alone passes too)
1:  addiu   $t2, $zero, 2
    lui     $t0, 0xA404
    lw      $t1, 0x0010($t0)            # SP_STATUS
    andi    $t1, $t1, 0x0001            # halted
    beqz    $t1, 9f                     # 2: the RSP is running
    nop                                 # (the reason is set before each branch, not in its delay slot)
    addiu   $t2, $zero, 3
    lw      $t1, 0x0010($t0)
    andi    $t1, $t1, 0x000C            # no SP DMA busy / full
    bnez    $t1, 9f                     # 3: an SP DMA
    nop
    addiu   $t2, $zero, 4
    lui     $t0, 0xA410
    lw      $t1, 0x000C($t0)            # DPC_STATUS
    andi    $t1, $t1, 0x0140            # command, DMA busy (pipe busy alone stays up until a
    bnez    $t1, 9f                     # 4: the RDP is busy   full sync, which a libdragon game never sends)
    nop
    addiu   $t2, $zero, 5
    lui     $t0, 0xA480
    lw      $t1, 0x0018($t0)            # SI_STATUS
    andi    $t1, $t1, 0x0003
    bnez    $t1, 9f                     # 5: an SI transfer
    nop
    addiu   $t2, $zero, 6
    mfc0    $t0, $13                    # Cause
    mfc0    $t1, $12                    # Status
    and     $t0, $t0, $t1
    andi    $t0, $t0, 0x8000
    bnez    $t0, 9f                     # 6: a Compare match pending and enabled (the hook would want it kept)
    nop
    move    $t2, $zero
9:  jr      $ra
    nop

# mt_parse(a0 = the block, uncached): walks the joybus block as the resident hook's
# pad_parse does and returns v0 = 1 with channel 0's buttons in t3, or v0 = 0 (no read
# reply for channel 0, no controller, or the PIF has not answered the block yet).
# Uses t0 (offset), t1 (channel), t2, a1, a2.
mt_parse:
    move    $t0, $zero
    move    $t1, $zero
3:  sltiu   $a2, $t0, 63
    beqz    $a2, 9f                     # off the end
    addu    $a2, $a0, $t0
    lbu     $a1, 0($a2)                 # the entry's first byte
    addiu   $a2, $zero, 0xFE
    beq     $a1, $a2, 9f                # end of the block
    addiu   $a2, $zero, 0xFF
    beq     $a1, $a2, 4f                # padding
    nop
    beqz    $a1, 5f                     # channel skipped
    addiu   $a2, $zero, 0xFD
    beq     $a1, $a2, 5f                # channel reset
    nop
    andi    $v0, $a1, 0x3F              # tx (the command byte counted)
    addu    $a2, $a0, $t0
    lbu     $a1, 1($a2)                 # rx byte (with the PIF's error bits)
    lbu     $t2, 2($a2)                 # the command
    bnez    $t1, 6f                     # not channel 0
    andi    $a2, $a1, 0x3F              # rx
    addiu   $t2, $t2, -1
    bnez    $t2, 9f                     # channel 0's entry is not a read (a pak command): nothing to see
    sltiu   $t2, $a2, 4
    bnez    $t2, 9f                     # too short for buttons
    andi    $t2, $a1, 0xC0
    bnez    $t2, 9f                     # no controller / overrun: nothing new
    addu    $a2, $a0, $t0
    lbu     $t3, 3($a2)
    lbu     $a2, 4($a2)
    sll     $t3, $t3, 8
    or      $t3, $t3, $a2               # the buttons
    ori     $a2, $zero, 0xFFFF          # (ori: addiu would sign-extend and never match)
    beq     $t3, $a2, 9f                # libultra's fill: the PIF has not answered yet
    nop
    jr      $ra
    addiu   $v0, $zero, 1
6:  andi    $a2, $a1, 0x3F              # skip the entry: 2 + tx + rx, next channel
    addu    $t0, $t0, $v0
    addu    $t0, $t0, $a2
    addiu   $t0, $t0, 2
    b       3b
    addiu   $t1, $t1, 1
4:  b       3b
    addiu   $t0, $t0, 1
5:  addiu   $t1, $t1, 1
    b       3b
    addiu   $t0, $t0, 1
9:  jr      $ra
    move    $v0, $zero

# ---- the borrow ------------------------------------------------------------------
mt_trigger:                             # v0 = the action (a2 = its hold counter for 1..3)
    mfc0    $t3, $9                     # the game's clock at the borrow's first instant (kept in
                                        # t3 until the cart is writable; mon_epilogue puts it back
                                        # last, so the borrow never happened)
    sltiu   $t0, $v0, 4
    beqz    $t0, 1f
    nop
    addu    $t0, $k0, $a2
    lw      $t1, 0($t0)
    andi    $t1, $t1, 0x8000            # bit 15: the PC asked for a screenshot (op 5)
    beqz    $t1, 3f
    nop
    addiu   $v0, $zero, 5
3:  lw      $t1, 0($t0)
    andi    $t1, $t1, 0x2000            # bit 13: the virtual pak's service (op 6)
    beqz    $t1, mt_trig_res
    nop
    addiu   $v0, $zero, 6
mt_trig_res:
    lw      $t1, 0($t0)
    andi    $t1, $t1, 0x1000            # bit 12: a suspended state's resume (op 4)
    beqz    $t1, 2f
    nop
    addiu   $v0, $zero, 4
2:  lw      $t1, 0($t0)
    andi    $t1, $t1, 0x0800            # bit 11: the screenshot button (op 7: the frame to the card)
    beqz    $t1, 6f
    nop
    addiu   $v0, $zero, 7
6:  lui     $t1, 0x0002
    sw      $t1, 0($t0)                 # fired: counts on while held (never the threshold
1:  lui     $t0, 0xA430                 # again, not armed); released, the COMBO zeroes it
    lw      $t0, 0x0008($t0)            # the RCP interrupt this moment is (MI_INTR, one bit),
    andi    $t0, $t0, 0x003F            # with the request: the hook's moment is this one, not
    sll     $t0, $t0, 8                 # the live bits after the stash and hook DMAs
    or      $t0, $t0, $v0
    sw      $t0, D_REQ($k0)
    addiu   $v0, $zero, 1
    sw      $v0, D_GUARD($k0)
    mfc0    $v0, $11                    # Compare (the game does not run from here: any instant)
    lui     $t0, 0xA080                 # the game's words under entry.S's stage marks
    lw      $t1, -8($t0)
    sw      $t1, D_DIAG0($k0)
    lw      $t1, -4($t0)
    sw      $t1, D_DIAG1($k0)
    # the register block (idempotent unlock)
    PIOW    0xBFFF0010, 0x00000000
    PIOW    0xBFFF0010, 0x5F554E4C
    PIOW    0xBFFF0010, 0x4F434B5F
    # ROM writes on: CONFIG_SET(ROM_WRITE_ENABLE, 1)
    jal     mon_cfg_rw
    addiu   $a2, $zero, 1
    # the game's clock to the cart, after the context area (0x60/0x64 in the vector page
    # held it until the pak server's entry stub took 0x60..0x80)
    move    $a1, $t3
    lui     $a0, %hi(CTX_KSEG1 + CTX_CLOCK)
    addiu   $a0, $a0, %lo(CTX_KSEG1 + CTX_CLOCK)
    CALL    LP_PIO_W
    move    $a1, $v0
    lui     $a0, %hi(CTX_KSEG1 + CTX_CLOCK + 4)
    addiu   $a0, $a0, %lo(CTX_KSEG1 + CTX_CLOCK + 4)
    CALL    LP_PIO_W
    # the game's dirty cache lines out, then its region to the stash
    jal     mon_cache_flush
    nop
    lui     $a0, 0x007D
    lui     $a1, %hi(STASH_PI)
    addiu   $a1, $a1, %lo(STASH_PI)
    lui     $a2, 0x0001
    ori     $a2, $a2, 0xFFFF
    addiu   $a3, $zero, 0x08            # RDRAM -> cart (PI_RD_LEN)
    CALL    LP_DMA
    # ROM writes off again (the hook turns them on when it needs them)
    jal     mon_cfg_rw
    move    $a2, $zero
    # the hook from its staging copy
    lui     $a0, 0x007D
    lui     $a1, %hi(STAGING_PI)
    addiu   $a1, $a1, %lo(STAGING_PI)
    lui     $a2, %hi(HOOK_LEN1)
    addiu   $a2, $a2, %lo(HOOK_LEN1)
    addiu   $a3, $zero, 0x0C            # cart -> RDRAM (PI_WR_LEN)
    CALL    LP_DMA
    # the game's registers back, then the hook's entry (k0/k1 are scratch there)
    LOAD_REGS
    lui     $k0, 0x807D
    jr      $k0
    nop

# ---- MON+0x1000: the virtual Controller Pak, served from the cart ----------------------
# The resident hook answers channel 0's accessory commands from a RAM copy of the pak
# image at every SI completion; here the image stays on the cart (VPAK_KSEG1, 32 KiB,
# the menu's copy of the pak file) and this code answers from it, run in place. Two ways
# in: the tick's SI branch (mon_tick, while the game's handler is not patched), and
# mon_pak_hit, reached from the game's own __osException at its SI acknowledge through
# a RAM stub at 0x060 (hook.c pak_stub_place: waits for the PI to go idle, since a cart
# fetch under a game's DMA freezes the console, then comes here) once the service borrow
# (hook.c pak_borrow, op 6) has found the site and patched it. Dirty blocks go straight
# to the image on the cart; the mirror to the card is the service borrow's, 90 ticks
# after the last write.
#
# (dev) counters in the cart buffer: +0x1F40 identifies answered, +0x1F44 reads, +0x1F48
# writes, +0x1F4C entries from the handler (read from a PC over USB).
.macro DEVCOUNT addr
.endm


# (dev) a trace of the commands served: 128 entries of two words in the cart buffer at
# +0x1800 (the index at +0x1F50): cmd << 24 | tx << 16 | the rx byte as left << 8 | flags,
# then the block << 16 | the first data or status byte << 8 | the CRC as answered. Used
# where a1 is the entry and nothing else is live (the helper's registers, v0, a2, a3 go).
.macro DEVTRACE flags
.endm

.section .text.pak, "ax"
.global mon_pak_hit
mon_pak_hit:
    # From the stub at 0x060, the PI idle, inside the game's handler at its SI acknowledge:
    # the block just landed. The handler's registers are live except at and a0 (and ra:
    # scheme 1 keeps the handler's own `jal send_mesg` and comes here from a jal in its
    # place, so ra is the way back; scheme 2 replaced the four words through the jal's
    # delay slot, as the resident trampoline does, and ra is dead). What the server uses
    # is parked in the vector page's scratch and put back.
    lui     $at, 0x8000
    sd      $k0, (SCRATCH + 0x00)($at)
    sd      $k1, (SCRATCH + 0x08)($at)
    sd      $v0, (SCRATCH + 0x10)($at)
    sd      $a1, (SCRATCH + 0x18)($at)
    sd      $a2, (SCRATCH + 0x20)($at)
    sd      $a3, (SCRATCH + 0x28)($at)
    sd      $t0, (SCRATCH + 0x30)($at)
    sd      $t1, (SCRATCH + 0x38)($at)
    sd      $t2, (SCRATCH + 0x40)($at)
    sd      $t3, (SCRATCH + 0x48)($at)
    sw      $ra, D_SITERA($at)
    DEVCOUNT 0xBFFE1F4C
    lui     $t2, 0xA480
    lw      $t2, 0x0000($t2)            # SI_DRAM_ADDR: start + 0x38 after a 64-byte transfer
    lui     $t3, 0x0100
    addiu   $t3, $t3, -1
    and     $t2, $t2, $t3
    sltiu   $t1, $t2, 0x38
    bnez    $t1, 1f
    lui     $t3, 0x0080
    addiu   $t3, $t3, -0x40
    sltu    $t1, $t3, $t2
    bnez    $t1, 1f                     # beyond 8 MiB - 64: not a block
    lui     $t3, 0xA000
    or      $a0, $t2, $t3
    jal     mp_serve
    addiu   $a0, $a0, -0x38
1:  lui     $at, 0x8000
    ld      $k0, (SCRATCH + 0x00)($at)
    ld      $k1, (SCRATCH + 0x08)($at)
    ld      $v0, (SCRATCH + 0x10)($at)
    ld      $a1, (SCRATCH + 0x18)($at)
    ld      $a2, (SCRATCH + 0x20)($at)
    ld      $a3, (SCRATCH + 0x28)($at)
    ld      $t0, (SCRATCH + 0x30)($at)
    ld      $t1, (SCRATCH + 0x38)($at)
    ld      $t2, (SCRATCH + 0x40)($at)
    ld      $t3, (SCRATCH + 0x48)($at)
    lw      $ra, D_SITERA($at)
    # the words the site lost, all harmless to repeat: the acknowledge, the SI bit out
    # of the handler's pending word, rX as the acknowledge left it (t0 or t1, from the
    # control block; at is set last, dead either way)
    lui     $a0, 0xa480
    sw      $zero, 0x0018($a0)          # SI_STATUS = 0
    andi    $s1, $s1, 0x003d
    lui     $at, %hi(PAK_CTL)
    lw      $a0, %lo(PAK_CTL + PAK_CTL_VEC + 12)($at)   # rX | lo << 16
    andi    $at, $a0, 0x001F
    addiu   $at, $at, -8
    srl     $a0, $a0, 16
    beqz    $at, 6f                     # rX = t0
    addiu   $at, $at, -1
    bnez    $at, 7f                     # rX = at: nothing to do here
    lui     $at, 0xa480
    b       7f
    or      $t1, $at, $a0               # rX = t1
6:  lui     $at, 0xa480
    or      $t0, $at, $a0
7:  lui     $at, %hi(PAK_CTL)
    lw      $a0, %lo(PAK_CTL + PAK_CTL_SCHEME)($at)
    addiu   $a0, $a0, -1
    beqz    $a0, 8f                     # scheme 1: the handler's own jal send_mesg follows
    nop
    lw      $a0, %lo(PAK_CTL + PAK_CTL_VEC + 8)($at)    # scheme 2: a0 for send_mesg ...
    lw      $at, %lo(PAK_CTL + PAK_CTL_VEC + 0)($at)    # ... send_mesg itself ...
    jalr    $at
    nop
    lui     $at, %hi(PAK_CTL)
    lw      $ra, %lo(PAK_CTL + PAK_CTL_VEC + 4)($at)    # ... and the word after the jal's delay slot
8:  jr      $ra
    lui     $at, 0xa480

# mp_serve(a0 = the joybus block, kseg1): answers the accessory command of the channel
# the virtual pak sits in, in place, as the resident hook's vpak_serve does: identify
# (0x00/0xFF, tx 1, rx 3) -> a pak present; read 0x02 (tx 3, rx 33) -> 32 bytes of the
# image and their CRC; write 0x03 (tx 35, rx 1) -> the image (ROM writes enabled around
# the PIO writes), the dirty stamp, the CRC back. The channel, and whether the pak is in
# at all, come from the control block's live word (PAK_CTL_LIVE: the menu writes it at
# launch, the panel in a borrow): the block is walked to that channel (0x00 and 0xFD
# count a channel, 0xFF is padding, 0xFE the end, an entry is 2 + tx + rx bytes; an
# accessory block carries one command, so nothing past the entry is looked at). Removed,
# the channel is left to whatever is in the real slot, but for one thing: after a change
# the word carries the "pak changed" bit, the channel's next identify gets the status
# bit the PIF gives after a swap (0x02) and the bit is cleared on the cart, so a game
# that keeps its own idea of the slot looks again; in, the same one report rides the
# identify that says a pak is present. Blocks at 0x8000 and above echo the last byte
# written there (the bank select) on a Rumble Pak; a Controller Pak reads zeros there,
# and so does this one (the echo made libultra's osMotorInit take it for a Rumble Pak:
# Ocarina of Time then probed it every few frames at a heavy cost). A write there is
# answered and dropped. The site fires at both SI interrupts of a transaction: after the
# block went to the PIF (the game's copy still holds its placeholders and the control
# byte 1 it set at offset 63) and after the PIF's answer came back (the PIF clears that
# byte); only the second is served, the first would be overwritten by the answer anyway
# (and cost the same). Clobbers at, a0-a3, t0-t3, v0, k0, k1 and ra (kept in D_DIAG0,
# 0x1A8: a borrow's word, free while the game runs). From the cart a read costs about
# half a millisecond, a write about the same plus the ROM-write round trips.
mp_serve:
    lui     $k0, 0x8000
    sw      $ra, D_DIAG0($k0)
    lbu     $at, 63($a0)                # the PIF's control byte: still 1, the PIF has not run
    bnez    $at, mp_done
    lui     $at, %hi(PAK_CTL)
    lw      $t3, %lo(PAK_CTL + PAK_CTL_LIVE)($at)   # t3: the live word (the channel, in, a change to report)
    andi    $t1, $t3, 0x0003            # t1: channels before the pak's
    move    $t0, $zero                  # t0: the offset into the block
1:  sltiu   $a2, $t0, 63
    beqz    $a2, mp_done                # padding to the end: nothing
    addu    $a1, $a0, $t0
    lbu     $a1, 0($a1)                 # the entry's first byte
    addiu   $a2, $zero, 0xFF
    bne     $a1, $a2, 2f
    addiu   $a2, $zero, 0xFE
    b       1b                          # padding
    addiu   $t0, $t0, 1
2:  beq     $a1, $a2, mp_done           # end of the block
    addiu   $a2, $zero, 0xFD
    beq     $a1, $a2, 3f                # a channel reset: one byte, the channel counted
    nop
    bnez    $a1, 4f                     # an entry
    andi    $t2, $a1, 0x3F              # tx (the command byte counted)
3:  beqz    $t1, mp_done                # the pak's channel skipped or reset: nothing to answer
    addiu   $t1, $t1, -1
    b       1b
    addiu   $t0, $t0, 1
4:  beqz    $t2, mp_done                # not a block after all
    addu    $a1, $a0, $t0               # a1: the entry
    lbu     $a2, 1($a1)                 # rx byte (with the PIF's error bits)
    lbu     $v0, 2($a1)                 # the command
    andi    $a3, $a2, 0x3F              # rx
    addu    $a2, $t0, $t2
    addu    $a2, $a2, $a3
    sltiu   $a2, $a2, 63                # i + 2 + tx + rx <= 64
    beqz    $a2, mp_done                # not a block after all
    nop
    beqz    $t1, 5f                     # the pak's channel: this entry
    addiu   $t1, $t1, -1
    addu    $t0, $t0, $t2               # another channel's: past it
    addu    $t0, $t0, $a3
    b       1b
    addiu   $t0, $t0, 2
5:  lbu     $at, 1($a1)
    andi    $at, $at, 0xC0              # the PIF's error bits: no controller answered there
    bnez    $at, mp_done                # (a pak in an empty port changes nothing)
    addiu   $a2, $zero, 0x02
    beq     $v0, $a2, mp_read
    addiu   $a2, $zero, 0x03
    beq     $v0, $a2, mp_write
    addiu   $a2, $zero, 0xFF
    beq     $v0, $a2, mp_probe
    nop
    bnez    $v0, mp_done                # not a pak command (a controller read: the usual)
    nop
mp_probe:                               # identify: a pak is present (in); "pak changed" once after a change
    addiu   $a2, $zero, 1
    bne     $t2, $a2, mp_done
    addiu   $a2, $zero, 3
    bne     $a3, $a2, mp_done
    andi    $at, $t3, 0x30              # in, or a change to report
    beqz    $at, mp_done                # neither: the real slot's answer stands
    andi    $at, $t3, 0x10
    beqz    $at, 6f                     # removed: only the report
    lbu     $a2, 5($a1)
    addiu   $at, $zero, 3
    sb      $at, 1($a1)                 # no receive error
    ori     $a2, $a2, 0x01              # a pak present ...
    andi    $a2, $a2, 0xFD              # ... not "pulled" ...
6:  andi    $at, $t3, 0x20
    beqz    $at, 7f
    nop
    ori     $a2, $a2, 0x02              # ... but for the one report after a change
7:  sb      $a2, 5($a1)
    DEVTRACE 1                          # (the trace first: the counter's helper clobbers a1, the entry)
    DEVCOUNT 0xBFFE1F40
    andi    $at, $t3, 0x20
    beqz    $at, mp_done
    nop
    # the report made: the bit off in the control block (the register block unlocked,
    # ROM writes on around the PIO write, as mp_write does; t3 survives the helpers)
    jal     mp_unlock
    nop
    jal     mon_cfg_rw
    addiu   $a2, $zero, 1
    lui     $a0, %hi(PAK_CTL + PAK_CTL_LIVE)
    addiu   $a0, $a0, %lo(PAK_CTL + PAK_CTL_LIVE)
    andi    $a1, $t3, 0xFFDF
    CALL    LP_PIO_W
    jal     mon_cfg_rw
    move    $a2, $zero
    b       mp_done
    nop
mp_read:                                # 32 bytes of the image and their CRC
    andi    $at, $t3, 0x10
    beqz    $at, mp_done                # removed: the real slot's answer stands
    addiu   $a2, $zero, 3
    bne     $t2, $a2, mp_done
    addiu   $a2, $zero, 33
    bne     $a3, $a2, mp_done
    nop
    sb      $a2, 1($a1)                 # rx 33, no error
    lbu     $v0, 3($a1)
    lbu     $a2, 4($a1)
    sll     $v0, $v0, 8
    or      $v0, $v0, $a2
    srl     $v0, $v0, 5                 # the block (0..1023 the image, 1024+ the bank area)
    addiu   $a2, $a1, 5                 # a2: where the bytes go
    move    $a3, $zero                  # a3: the CRC
    lui     $t3, %hi(mp_crc_tab)
    addiu   $t3, $t3, %lo(mp_crc_tab)   # t3: the CRC table (crc = T[crc ^ byte], a word each)
    sltiu   $t1, $v0, 1024
    beqz    $t1, 3f
    addiu   $t1, $zero, 8               # t1: words left
    sll     $v0, $v0, 5
    lui     $k1, %hi(VPAK_KSEG1)
    addiu   $k1, $k1, %lo(VPAK_KSEG1)
    addu    $k1, $k1, $v0               # k1: the block in the image
2:  lw      $k0, 0($k1)
    addiu   $k1, $k1, 4
    srl     $at, $k0, 24
    sb      $at, 0($a2)
    xor     $a3, $a3, $at
    sll     $a3, $a3, 2
    addu    $a3, $a3, $t3
    lw      $a3, 0($a3)
    srl     $at, $k0, 16
    andi    $at, $at, 0xFF
    sb      $at, 1($a2)
    xor     $a3, $a3, $at
    sll     $a3, $a3, 2
    addu    $a3, $a3, $t3
    lw      $a3, 0($a3)
    srl     $at, $k0, 8
    andi    $at, $at, 0xFF
    sb      $at, 2($a2)
    xor     $a3, $a3, $at
    sll     $a3, $a3, 2
    addu    $a3, $a3, $t3
    lw      $a3, 0($a3)
    andi    $at, $k0, 0xFF
    sb      $at, 3($a2)
    xor     $a3, $a3, $at
    sll     $a3, $a3, 2
    addu    $a3, $a3, $t3
    lw      $a3, 0($a3)
    addiu   $t1, $t1, -1
    bnez    $t1, 2b
    addiu   $a2, $a2, 4
    sb      $a3, 37($a1)                # the CRC after the data
    DEVTRACE 2
    DEVCOUNT 0xBFFE1F44
    b       mp_done
    nop
3:  move    $at, $zero                  # the bank area: zeros, as a Controller Pak answers
    addiu   $t1, $zero, 32
4:  sb      $at, 0($a2)
    xor     $a3, $a3, $at
    sll     $a3, $a3, 2
    addu    $a3, $a3, $t3
    lw      $a3, 0($a3)
    addiu   $t1, $t1, -1
    bnez    $t1, 4b
    addiu   $a2, $a2, 1
    sb      $a3, 37($a1)
    DEVTRACE 2
    DEVCOUNT 0xBFFE1F44
    b       mp_done
    nop
mp_write:                               # 32 bytes into the image, the CRC back
    andi    $at, $t3, 0x10
    beqz    $at, mp_done                # removed: the real slot's answer stands
    addiu   $a2, $zero, 35
    bne     $t2, $a2, mp_done
    addiu   $a2, $zero, 1
    bne     $a3, $a2, mp_done
    nop
    sb      $a2, 1($a1)                 # rx 1, no error
    addiu   $a2, $a1, 5                 # the CRC of the 32 bytes, for the reply
    move    $a3, $zero
    lui     $t3, %hi(mp_crc_tab)
    addiu   $t3, $t3, %lo(mp_crc_tab)
    addiu   $t1, $zero, 32
2:  lbu     $at, 0($a2)
    xor     $a3, $a3, $at
    sll     $a3, $a3, 2
    addu    $a3, $a3, $t3
    lw      $a3, 0($a3)
    addiu   $t1, $t1, -1
    bnez    $t1, 2b
    addiu   $a2, $a2, 1
    sb      $a3, 37($a1)
    lbu     $v0, 3($a1)
    lbu     $a2, 4($a1)
    sll     $v0, $v0, 8
    or      $v0, $v0, $a2
    srl     $v0, $v0, 5                 # the block
    move    $t3, $a1                    # t3: the entry (a0/a1 are the helpers' from here)
    sltiu   $t1, $v0, 1024
    beqz    $t1, 3f                     # the bank area: a byte in RAM, nothing for the cart
    sll     $v0, $v0, 5
    # the register block unlocked, ROM writes on (the image and the control block are
    # ROM space to the cart), the words in, ROM writes off again
    jal     mp_unlock
    nop
    jal     mon_cfg_rw
    addiu   $a2, $zero, 1
    lui     $a3, %hi(VPAK_KSEG1)
    addiu   $a3, $a3, %lo(VPAK_KSEG1)
    addu    $a3, $a3, $v0               # a3: the block in the image
    addiu   $v0, $zero, 8               # v0: words left (the helper uses t0-t2)
    addiu   $a2, $t3, 5                 # a2: the data in the entry
2:  lbu     $a1, 0($a2)
    lbu     $at, 1($a2)
    sll     $a1, $a1, 8
    or      $a1, $a1, $at
    lbu     $at, 2($a2)
    sll     $a1, $a1, 8
    or      $a1, $a1, $at
    lbu     $at, 3($a2)
    sll     $a1, $a1, 8
    or      $a1, $a1, $at               # a word, big-endian as the image is
    move    $a0, $a3
    CALL    LP_PIO_W
    addiu   $a3, $a3, 4
    addiu   $v0, $v0, -1
    bnez    $v0, 2b
    addiu   $a2, $a2, 4
    b       4f
    nop
3:  b       5f                          # the bank area: nothing to keep, no dirty stamp
    nop
4:  lui     $k0, 0x8000                 # the dirty stamp: this tick, bit 31 set (never 0)
    lw      $a1, D_TICKS($k0)
    lui     $a0, 0x8000
    or      $a1, $a1, $a0
    lui     $a0, %hi(PAK_CTL + PAK_CTL_DIRTY)
    addiu   $a0, $a0, %lo(PAK_CTL + PAK_CTL_DIRTY)
    CALL    LP_PIO_W
    jal     mon_cfg_rw
    move    $a2, $zero
5:  DEVCOUNT 0xBFFE1F48
    move    $a1, $t3                    # (dev) the entry, for the trace
    DEVTRACE 3
mp_done:
    lui     $k0, 0x8000
    lw      $ra, D_DIAG0($k0)
    jr      $ra
    nop

# the cart's register block unlocked (the key in three writes) before a cart write from
# this section: ra kept in a3, which every caller has free (clobbers a0, a1, t2 and what
# LP_PIO_W does: t0-t2, k0, k1)
mp_unlock:
    move    $a3, $ra
    PIOW    0xBFFF0010, 0x00000000
    PIOW    0xBFFF0010, 0x5F554E4C
    PIOW    0xBFFF0010, 0x4F434B5F
    jr      $a3
    nop
.balign 16
mp_crc_tab:
    .include "pakcrc.inc"

# ---- MON+0x2000: the load epilogue -------------------------------------------------
# From the hook at the end of a load (borrow_load_exit): no register is live, the
# loaded world's context sits on the cart at CTX_KSEG1 (0x220 bytes: the GPRs, LO/HI,
# the FPRs, Status/EPC/FCR31, the layout of state.S). Put the loaded region back over
# the hook, then do what state.S's state_resume does, reading every 64-bit value as
# two 32-bit cart loads (the PI is a 32-bit bus).
.set CTX_LO,      0x100
.set CTX_HI,      0x108
.set CTX_FPR,     0x110
.set CTX_STATUS,  0x210
.set CTX_EPC,     0x214
.set CTX_FCR31,   0x218

# a 64-bit context value into \rd, \tmp as the scratch (k0 = the context)
.macro LD64 rd, off, tmp
    lw      \rd, (\off)($k0)
    dsll32  \rd, \rd, 0
    lw      \tmp, (\off + 4)($k0)
    dsll32  \tmp, \tmp, 0
    dsrl32  \tmp, \tmp, 0
    or      \rd, \rd, \tmp
.endm

.section .text.epil, "ax"              # +0x2000 (monitor.ld): the hook's target after a load
.global mon_epi_load
mon_epi_load:
    jal     mon_cache_flush             # the hook's dirty lines out, every line dropped
    nop
    lui     $a0, 0x007D
    lui     $a1, %hi(STASH_PI)
    addiu   $a1, $a1, %lo(STASH_PI)
    lui     $a2, 0x0001
    ori     $a2, $a2, 0xFFFF
    addiu   $a3, $zero, 0x0C            # cart -> RDRAM (PI_WR_LEN): the loaded world's region
    CALL    LP_DMA
    lui     $k0, 0x8000
    lw      $t1, D_PIDRAM($k0)          # the loaded world's PI_DRAM_ADDR (the hook put it here)
    lui     $t0, 0xA460
    sw      $t1, 0x0000($t0)
    lw      $t1, D_DIAG0($k0)           # the loaded world's words under the stage marks
    lui     $t0, 0xA080
    sw      $t1, -8($t0)
    lw      $t1, D_DIAG1($k0)
    sw      $t1, -4($t0)
    sw      $zero, D_GUARD($k0)
    sw      $zero, D_REQ($k0)           # the image's monitor words are the saved moment's: no
    sw      $zero, D_HOLD_S($k0)        # request, no hold, no pad until the loaded game polls
    sw      $zero, D_HOLD_L($k0)        # its controller (a load with the combo still held
    sw      $zero, D_HOLD_M($k0)        # loaded again every twelve ticks: DK64)
    sw      $zero, D_PAD($k0)
    sw      $zero, D_PADOK($k0)
    addiu   $t1, $zero, -1
    sw      $t1, D_RESUMED($k0)         # a suspended state is loaded once: no try after a load
    # the context, as state_resume would load it
    lui     $k0, %hi(CTX_KSEG1)
    addiu   $k0, $k0, %lo(CTX_KSEG1)
    .set push
    .set hardfloat
    lw      $k1, CTX_STATUS($k0)
    lui     $t0, 0x2000
    or      $t1, $k1, $t0               # CU1 on while the FPRs load
    mtc0    $t1, $12
    nop
    nop
    nop
    nop
    lw      $t2, CTX_FCR31($k0)
    ctc1    $t2, $31
    nop
    .irp i,0,2,4,6,8,10,12,14,16,18,20,22,24,26,28,30
    LD64    $t0, (CTX_FPR + 8 * \i), $t1
    dmtc1   $t0, $f\i
    .endr
    lui     $t0, 0x0400
    and     $t0, $k1, $t0
    beqz    $t0, 1f
    nop
    .irp i,1,3,5,7,9,11,13,15,17,19,21,23,25,27,29,31
    LD64    $t0, (CTX_FPR + 8 * \i), $t1
    dmtc1   $t0, $f\i
    .endr
1:
    LD64    $t0, CTX_LO, $t1
    mtlo    $t0
    LD64    $t0, CTX_HI, $t1
    mthi    $t0
    lw      $t0, CTX_EPC($k0)
    mtc0    $t0, $14
    mfc0    $t0, $13                    # Cause.WP off: a watch exception deferred during the
    lui     $t1, 0xFFBF                 # borrow (a store into 0x180..0x187 with EXL set) would
    ori     $t1, $t1, 0xFFFF            # otherwise fire at the eret with the game's EPC and
    and     $t0, $t0, $t1               # the engine would clobber a game register
    mtc0    $t0, $13
    mtc0    $k1, $12
    nop
    nop
    nop
    .irp i,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,28,29,30,31
    LD64    $\i, (8 * \i), $k1
    .endr
    lui     $k0, %hi(LP_EXIT_ERET)
    addiu   $k0, $k0, %lo(LP_EXIT_ERET) # RAM: PI_CART_ADDR back, then eret
    jr      $k0
    nop
    .set pop

# CONFIG_SET(ROM_WRITE_ENABLE = a2): DATA0 = the config id (1), DATA1 = the value,
# the command byte 'C', then wait for the MCU (SR bit 31). ra kept in a3.
mon_cfg_rw:
    move    $a3, $ra
    PIOW    0xBFFF0004, 0x00000001
    lui     $a0, 0xBFFF
    addiu   $a0, $a0, 0x0008
    move    $a1, $a2
    CALL    LP_PIO_W
    PIOW    0xBFFF0000, 0x00000043
1:  lui     $t0, 0xBFFF
    lw      $t0, 0($t0)
    bltz    $t0, 1b
    nop
    jr      $a3
    nop

# Index writeback-invalidate of the whole D-cache and invalidate of the whole I-cache
# (16 KiB of index operations; from the cart, ~10 ms). It lived in the vector page
# (0x1DC) until that hole was needed for the pre-install check. Uses k0/k1.
mon_cache_flush:
    lui     $k0, 0x8000
    ori     $k1, $k0, 0x4000
1:  cache   0x01, 0($k0)                # Index_Writeback_Invalidate_D
    cache   0x00, 0($k0)                # Index_Invalidate_I
    addiu   $k0, $k0, 16
    bne     $k0, $k1, 1b
    nop
    jr      $ra
    nop

# ---- MON+0x3000: the gate installer -------------------------------------------------
# The boot patcher cannot leave the gate at 0x360: a game's boot may run with its stack
# in the low page (Perfect Dark: sp = 0x80000F10, its lib inflater's frames reach down
# to 0x35C), which overwrote the gate before the first interrupt (seen on hardware).
# So the engine's tail first runs the vector page's pre-install check (lowpage.S,
# 0x1DC then 0x118: RAM code, so a game's PI DMA in flight cannot freeze the console
# under a cart fetch) and comes here, k0 = this address, only for an interrupt with
# the PI idle - libultra enables interrupts only once its boot stack is dead. The 36
# gate words below are copied to 0x360, EXIT (lowpage.S) over the pre-install check at
# 0x1DC (dead from here on; it cannot live below 0x080, where Indiana Jones' exception
# handlers spill registers), the engine's tail is rewritten to j 0x360 / nop (the
# steady state), and the gate runs for this exception. Clobbers k0/k1 only;
# t0/t1 are parked in the vector page's scratch. (The ExcCode test below is kept as a
# backstop.)

.section .text.install, "ax"
.global mon_install
mon_install:
    mfc0    $k1, $13                    # Cause
    andi    $k1, $k1, 0x007C            # ExcCode: 0 is an interrupt
    bnez    $k1, 8f
    lui     $k0, 0x8000
    sd      $t0, (SCRATCH + 0x00)($k0)
    sd      $t1, (SCRATCH + 0x08)($k0)
    lui     $t0, %hi(mon_install_gate)
    addiu   $t0, $t0, %lo(mon_install_gate)
    ori     $k0, $k0, 0x0360
    ori     $t1, $zero, 36
1:  lw      $k1, 0($t0)
    addiu   $t0, $t0, 4
    sw      $k1, 0($k0)
    cache   0x19, 0($k0)                # Hit_Writeback_D
    cache   0x10, 0($k0)                # Hit_Invalidate_I
    addiu   $t1, $t1, -1
    bnez    $t1, 1b
    addiu   $k0, $k0, 4
    # EXIT over the pre-install check at 0x1DC (its words are done by now)
    lui     $t0, %hi(mon_install_exit)
    addiu   $t0, $t0, %lo(mon_install_exit)
    lui     $k0, 0x8000
    ori     $k0, $k0, 0x01DC
    ori     $t1, $zero, 9
2:  lw      $k1, 0($t0)
    addiu   $t0, $t0, 4
    sw      $k1, 0($k0)
    cache   0x19, 0($k0)                # Hit_Writeback_D
    cache   0x10, 0($k0)                # Hit_Invalidate_I
    addiu   $t1, $t1, -1
    bnez    $t1, 2b
    addiu   $k0, $k0, 4
    lui     $k0, 0x8000
    lui     $k1, 0x0800
    ori     $k1, $k1, 0x00D8            # j 0x80000360
    sw      $k1, 0x00E8($k0)            # the engine's tail (word 22 of the engine at 0x90)
    sw      $zero, 0x00EC($k0)          # nop in its delay slot
    cache   0x19, 0x00E8($k0)
    cache   0x10, 0x00E8($k0)
    ld      $t0, (SCRATCH + 0x00)($k0)
    ld      $t1, (SCRATCH + 0x08)($k0)
    ori     $k0, $k0, 0x0360
    jr      $k0                         # the gate, for this exception
    nop
8:  lui     $k0, 0x8000
    ori     $k0, $k0, 0x0120
    jr      $k0                         # not an interrupt: the game's vector words
    nop
.balign 16
mon_install_gate:
    .incbin "lp_f360.bin"               # the 0x360 fragment (gate + lp_dma), 36 words
    .space  144                         # (padding in case the fragment is shorter)
.balign 16
mon_install_exit:
    .incbin "lp_fexit.bin"              # EXIT (lowpage.S), 8 words; 9 are copied
    .space  36
