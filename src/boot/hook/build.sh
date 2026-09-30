#!/bin/bash
# Build the save-state hook blobs, the borrowed-mode monitors and the vector-page
# fragments, and regenerate src/boot/hook_blob.c / hook_blob.h. Needs the libdragon
# toolchain the menu itself is built with (N64_INST, or /opt/libdragon). Inputs in this
# directory: entry.S, state.S, hook.c, ss_overlay.c, ss_font.h, ff_core.c, ff_core.h,
# ff_ucode.h, ff_view.c, hook.ld (the hook),
# lowpage.S, lowpage.ld, lowpage_alt.ld (the vector-page fragments), monitor.S, monitor.ld,
# pakcrc.inc (the cart monitor). One run builds the 64 MiB games' blob first (the same
# sources with SC64SS_CARD_DIRECT=1: hook64.bin and its monitors), then the one for every
# other game, and writes both into the generated files. Those are checked in, so this only
# has to run after a change to these sources.
set -euo pipefail
cd "$(dirname "$0")"

export PATH="$PATH:${N64_INST:-/opt/libdragon}/bin"
CC=mips64-elf-gcc
OBJCOPY=mips64-elf-objcopy

# The hook runs inside the game's exception handler with no runtime of its own:
# freestanding, soft-float (the game's FPU state must not be touched), no jump tables
# (no GOT), zeros kept in .data (nothing zeroes .bss at runtime).
CFLAGS="-march=vr4300 -mtune=vr4300 -mfix4300 -mabi=o64 -msoft-float \
 -mno-abicalls -fno-pic -G0 -ffreestanding -nostdlib -fno-builtin \
 -fno-jump-tables -fno-zero-initialized-in-bss -Os -Wall -Wextra -Werror \
 -DSC64SS_CARD_DIRECT=${SC64SS_CARD_DIRECT:-0}"

BASE=0x807D0000
CD="${SC64SS_CARD_DIRECT:-0}"
if [ "$CD" != "1" ]; then
    SC64SS_CARD_DIRECT=1 bash "$0"   # the 64 MiB games' blob first (hook64.bin, hook64.cfgoff): this run embeds it
fi
OUT=hook.bin
if [ "$CD" = "1" ]; then OUT=hook64.bin; fi
$CC $CFLAGS -c entry.S -o entry.o
$CC $CFLAGS -c state.S -o state.o
$CC $CFLAGS -c hook.c -o hook.o
$CC $CFLAGS -nostartfiles -Wl,-T,hook.ld -Wl,--defsym,HOOK_BASE=$BASE -Wl,-Map=hook.map -o hook.elf entry.o state.o hook.o

# The blob must be self-contained: an occupied .bss would mean uninitialized
# state that nothing zeroes at runtime.
BSS_SIZE=$(mips64-elf-size -A hook.elf | awk '$1==".bss"{print $2}')
if [ -n "${BSS_SIZE:-}" ] && [ "$BSS_SIZE" != "0" ]; then
    echo "ERROR: .bss is $BSS_SIZE bytes (must be 0 - give every static an initialiser)" >&2
    exit 1
fi

$OBJCOPY -O binary hook.elf $OUT
SIZE=$(stat -c%s $OUT)
# 128 KiB: the RDRAM home (0x807D0000..0x807F0000) and the cart staging area.
if [ "$SIZE" -gt 131072 ]; then
    echo "ERROR: $OUT is $SIZE bytes (must fit 128 KiB)" >&2
    exit 1
fi
echo "$OUT: $SIZE bytes (base $BASE, entry _start)"
# the boot-time config block (slot table, trigger) the menu patches in the staged blob
CFG_ADDR=$(mips64-elf-nm hook.elf | awk '$3=="hook_cfg"{print $1}')
CFG_OFF=$(( 0x$CFG_ADDR - $BASE ))
echo "hook_cfg at offset $CFG_OFF"
if [ "$CD" = "1" ]; then echo "$CFG_OFF" > hook64.cfgoff; exit 0; fi   # the card-direct blob: the plain build embeds it

# ---- borrowed-RAM mode: the vector-page fragments (lowpage.S) and the cart monitor
# (monitor.S). Layout constants come from hook.c; the monitor is linked at the kseg1
# alias of MONITOR_PI and reaches the RAM routines and the staged config by address.
hc() { python3 -c "import re,sys;print(int(re.search(r'#define\s+%s\s+(0x[0-9A-Fa-f]+)u' % sys.argv[1], open('hook.c').read()).group(1), 0))" "$1"; }
MON_PI=$(hc MONITOR_PI); STASH_PI=$(hc STASH_PI); STAGING_PI=$(hc HOOK_STAGING_PI); CTX_PI=$(hc CTX_PI)
VPAK_PI=$(hc VPAK_PI); VPAK_CTL_PI=$(hc VPAK_CTL_PI)
CTX_KSEG1=$(( 0xA0000000 + CTX_PI ))
VPAK_KSEG1=$(( 0xA0000000 + VPAK_PI ))    # the pak image, read in place by the monitor's server
VPAK_CRC=$(( 0xA0000000 + $(hc VPAK_CRC_PI) ))   # its block CRC table (the menu computes it at launch)
PAK_CTL=$(( 0xA0000000 + VPAK_CTL_PI ))   # its control block (state, dirty stamp, the site words)
MON_BASE=$(( 0xA0000000 + MON_PI ))
MON_TICK=$(( MON_BASE + 0x100 ))
MON_INSTALL=$(( MON_BASE + 0x3000 ))   # the gate installer (monitor.S, monitor.ld): reached by lui/ori, so it must share MON_BASE's upper half
[ "$(( MON_INSTALL >> 16 ))" -eq "$(( MON_BASE >> 16 ))" ] || { echo "ERROR: the installer's address crosses a 64 KiB boundary" >&2; exit 1; }
CFG_ADDR=$(( 0xA0000000 + STAGING_PI + CFG_OFF ))
HOOK_LEN1=$(( ((SIZE + 15) & ~15) - 1 ))
$CC $CFLAGS -c lowpage.S -o lowpage.o
$CC $CFLAGS -nostartfiles -Wl,-T,lowpage.ld -Wl,--defsym,MON_TICK=$MON_TICK -Wl,--defsym,MON_INSTALL=$MON_INSTALL -Wl,--defsym,GAME_TAIL=0x80000120 -o lowpage.elf lowpage.o
for f in f0f0 f110 f130 f1dc f360 fexit; do $OBJCOPY -O binary -j .$f lowpage.elf lp_$f.bin; done
fsz() { stat -c%s "$1"; }
[ "$(fsz lp_f0f0.bin)" -le 16 ]  || { echo "ERROR: lowpage 0x0F0 fragment is $(fsz lp_f0f0.bin) bytes (max 16)" >&2; exit 1; }
[ "$(fsz lp_f110.bin)" -le 16 ]  || { echo "ERROR: lowpage 0x110 fragment is $(fsz lp_f110.bin) bytes (max 16)" >&2; exit 1; }
[ "$(fsz lp_f130.bin)" -le 80 ]  || { echo "ERROR: lowpage 0x130 fragment is $(fsz lp_f130.bin) bytes (max 80)" >&2; exit 1; }
[ "$(fsz lp_f1dc.bin)" -le 36 ]  || { echo "ERROR: lowpage 0x1DC fragment is $(fsz lp_f1dc.bin) bytes (max 36)" >&2; exit 1; }
[ "$(fsz lp_f360.bin)" -le 144 ] || { echo "ERROR: lowpage 0x360 fragment is $(fsz lp_f360.bin) bytes (max 144)" >&2; exit 1; }
[ "$(fsz lp_fexit.bin)" -le 36 ] || { echo "ERROR: lowpage EXIT fragment is $(fsz lp_fexit.bin) bytes (max 36)" >&2; exit 1; }
lpsym() { mips64-elf-nm lowpage.elf | awk -v s="$1" '$3==s{print "0x"$1}'; }
LP_DMA=$(lpsym lp_dma); LP_PIO_W=$(lpsym lp_pio_w)
LP_EXIT=0x800001DC; LP_EXIT_ERET=$(lpsym lp_exit_eret)   # EXIT's home after the install (mon_install copies lp_fexit.bin over the pre-install check)
$CC $CFLAGS -Wa,--defsym,SCRATCH=0x10 -c monitor.S -o monitor.o   # the register scratch's home (see monitor.S)
$CC $CFLAGS -nostartfiles -Wl,-T,monitor.ld -Wl,--defsym,MON_BASE=$MON_BASE -Wl,--defsym,CFG_ADDR=$CFG_ADDR \
    -Wl,--defsym,HOOK_LEN1=$HOOK_LEN1 -Wl,--defsym,STAGING_PI=$STAGING_PI -Wl,--defsym,STASH_PI=$STASH_PI \
    -Wl,--defsym,LP_DMA=$LP_DMA -Wl,--defsym,LP_PIO_W=$LP_PIO_W \
    -Wl,--defsym,LP_EXIT=$LP_EXIT -Wl,--defsym,LP_EXIT_ERET=$LP_EXIT_ERET \
    -Wl,--defsym,CTX_KSEG1=$CTX_KSEG1 -Wl,--defsym,VPAK_KSEG1=$VPAK_KSEG1 -Wl,--defsym,PAK_CTL=$PAK_CTL \
    -Wl,--defsym,VPAK_CRC=$VPAK_CRC -o monitor.elf monitor.o
MON_TICK_AT=$(mips64-elf-nm monitor.elf | awk '$3=="mon_tick"{print "0x"$1}')
[ "$(( MON_TICK_AT ))" -eq "$MON_TICK" ] || { echo "ERROR: mon_tick at $MON_TICK_AT, expected $(printf 0x%x $MON_TICK)" >&2; exit 1; }
MON_PAK_AT=$(mips64-elf-nm monitor.elf | awk '$3=="mon_pak_hit"{print "0x"$1}')
[ "$(( MON_PAK_AT ))" -eq "$(( MON_BASE + 0x1200 ))" ] || { echo "ERROR: mon_pak_hit at $MON_PAK_AT, expected $(printf 0x%x $(( MON_BASE + 0x1200 )))" >&2; exit 1; }
# the sections must not run into each other (ld does not check fixed placements)
secend() { mips64-elf-objdump -h monitor.elf | awk -v s="$1" '$2==s{print "0x"$4"+0x"$3}'; }
[ "$(( $(secend .text) ))" -le "$(( MON_BASE + 0x1200 ))" ] || { echo "ERROR: the monitor's .text runs past +0x1200 (the pak server)" >&2; exit 1; }
[ "$(( $(secend .pak) ))" -le "$(( MON_BASE + 0x2200 ))" ] || { echo "ERROR: the pak server runs past +0x2200 (the load epilogue)" >&2; exit 1; }
[ "$(( $(secend .epil) ))" -le "$(( MON_BASE + 0x3000 ))" ] || { echo "ERROR: the load epilogue runs past +0x3000 (the installer)" >&2; exit 1; }
MON_EPIL_AT=$(mips64-elf-nm monitor.elf | awk '$3=="mon_epi_load"{print "0x"$1}')
[ "$(( MON_EPIL_AT ))" -eq "$(( MON_BASE + 0x2200 ))" ] || { echo "ERROR: mon_epi_load at $MON_EPIL_AT, expected $(printf 0x%x $(( MON_BASE + 0x2200 )))" >&2; exit 1; }
MON_INST_AT=$(mips64-elf-nm monitor.elf | awk '$3=="mon_install"{print "0x"$1}')
[ "$(( MON_INST_AT ))" -eq "$MON_INSTALL" ] || { echo "ERROR: mon_install at $MON_INST_AT, expected $(printf 0x%x $MON_INSTALL)" >&2; exit 1; }
$OBJCOPY -O binary monitor.elf monitor.bin
[ "$(fsz monitor.bin)" -le 16384 ] || { echo "ERROR: monitor.bin is $(fsz monitor.bin) bytes (max 16384)" >&2; exit 1; }
echo "borrowed mode: monitor $(fsz monitor.bin) bytes at $(printf 0x%08x $MON_PI); lowpage fragments 0F0/110/130/1DC/360/EXIT = $(fsz lp_f0f0.bin)/$(fsz lp_f110.bin)/$(fsz lp_f130.bin)/$(fsz lp_f1dc.bin)/$(fsz lp_f360.bin)/$(fsz lp_fexit.bin) bytes"

# the same monitor with its register scratch at 0x2A8 (monitor.S SCRATCH), for games whose
# own code lives at 0x000..0x07F (GoldenEye keeps its TLB refill handler there); the menu
# picks it by title (load_rom.c). Same code, other immediates: the sizes must agree.
$CC $CFLAGS -Wa,--defsym,SCRATCH=0x2A8 -c monitor.S -o monitor_hi.o
$CC $CFLAGS -nostartfiles -Wl,-T,monitor.ld -Wl,--defsym,MON_BASE=$MON_BASE -Wl,--defsym,CFG_ADDR=$CFG_ADDR -Wl,--defsym,HOOK_LEN1=$HOOK_LEN1 -Wl,--defsym,STAGING_PI=$STAGING_PI -Wl,--defsym,STASH_PI=$STASH_PI -Wl,--defsym,LP_DMA=$LP_DMA -Wl,--defsym,LP_PIO_W=$LP_PIO_W -Wl,--defsym,LP_EXIT=$LP_EXIT -Wl,--defsym,LP_EXIT_ERET=$LP_EXIT_ERET -Wl,--defsym,CTX_KSEG1=$CTX_KSEG1 -Wl,--defsym,VPAK_KSEG1=$VPAK_KSEG1 -Wl,--defsym,PAK_CTL=$PAK_CTL -Wl,--defsym,VPAK_CRC=$VPAK_CRC -o monitor_hi.elf monitor_hi.o
$OBJCOPY -O binary monitor_hi.elf monitor_hi.bin
[ "$(fsz monitor_hi.bin)" -eq "$(fsz monitor.bin)" ] || { echo "ERROR: monitor_hi.bin ($(fsz monitor_hi.bin) bytes) differs in size from monitor.bin" >&2; exit 1; }

# ---- the card-direct mode's borrowed placement: the same monitor and fragments linked for the
# monitor's home in the flash tables' block, the stash on the card (monitor.S CARD_STASH), the
# context and the cfg mirror in the data buffer (hook.c CD_*). Built when hook64.bin is here.
if [ -f hook64.bin ]; then
    MON64_PI=$(hc CD_MONITOR_PI)
    MON64_BASE=$(( 0xA0000000 + MON64_PI ))
    MON64_TICK=$(( MON64_BASE + 0x100 ))
    MON64_INSTALL=$(( MON64_BASE + 0x3000 ))
    [ "$(( MON64_INSTALL >> 16 ))" -eq "$(( MON64_BASE >> 16 ))" ] || { echo "ERROR: the card-direct installer's address crosses a 64 KiB boundary" >&2; exit 1; }
    CFG64_ADDR=$(hc CD_CFG_MIRROR)
    CTX64=$(hc CD_CTX_KSEG1)
    STAGING64=$(hc CD_STAGING_PI)
    STASH64_SEC=$(( 0xA0000000 + $(hc CD_STASH_INFO_PI) + 4 ))
    HOOK64_LEN1=$(( (($(stat -c%s hook64.bin) + 15) & ~15) - 1 ))
    $CC $CFLAGS -nostartfiles -Wl,-T,lowpage.ld -Wl,--defsym,MON_TICK=$MON64_TICK -Wl,--defsym,MON_INSTALL=$MON64_INSTALL -Wl,--defsym,GAME_TAIL=0x80000120 -o lowpage64.elf lowpage.o
    for f in f0f0 f110 f130 f1dc f360 fexit; do $OBJCOPY -O binary -j .$f lowpage64.elf lp64_$f.bin; done
    $CC $CFLAGS -Wa,--defsym,SCRATCH=0x10 -Wa,--defsym,CARD_STASH=1 -c monitor.S -o monitor64.o
    $CC $CFLAGS -nostartfiles -Wl,-T,monitor.ld -Wl,--defsym,MON_BASE=$MON64_BASE -Wl,--defsym,CFG_ADDR=$CFG64_ADDR \
        -Wl,--defsym,HOOK_LEN1=$HOOK64_LEN1 -Wl,--defsym,STAGING_PI=$STAGING64 -Wl,--defsym,STASH_PI=$STASH_PI \
        -Wl,--defsym,LP_DMA=$LP_DMA -Wl,--defsym,LP_PIO_W=$LP_PIO_W \
        -Wl,--defsym,LP_EXIT=$LP_EXIT -Wl,--defsym,LP_EXIT_ERET=$LP_EXIT_ERET \
        -Wl,--defsym,CTX_KSEG1=$CTX64 -Wl,--defsym,VPAK_KSEG1=$VPAK_KSEG1 -Wl,--defsym,PAK_CTL=$PAK_CTL \
        -Wl,--defsym,VPAK_CRC=$VPAK_CRC -Wl,--defsym,STASH_SEC=$STASH64_SEC -o monitor64.elf monitor64.o
    MON64_TICK_AT=$(mips64-elf-nm monitor64.elf | awk '$3=="mon_tick"{print "0x"$1}')
    [ "$(( MON64_TICK_AT ))" -eq "$MON64_TICK" ] || { echo "ERROR: card-direct mon_tick at $MON64_TICK_AT" >&2; exit 1; }
    [ "$(( $(mips64-elf-objdump -h monitor64.elf | awk '$2==".text"{print "0x"$4"+0x"$3}') ))" -le "$(( MON64_BASE + 0x1200 ))" ] || { echo "ERROR: the card-direct monitor's .text runs past +0x1200" >&2; exit 1; }
    MON64_EPIL_AT=$(mips64-elf-nm monitor64.elf | awk '$3=="mon_epi_load"{print "0x"$1}')
    [ "$(( MON64_EPIL_AT ))" -eq "$(( MON64_BASE + 0x2200 ))" ] || { echo "ERROR: card-direct mon_epi_load at $MON64_EPIL_AT" >&2; exit 1; }
    MON64_INST_AT=$(mips64-elf-nm monitor64.elf | awk '$3=="mon_install"{print "0x"$1}')
    [ "$(( MON64_INST_AT ))" -eq "$MON64_INSTALL" ] || { echo "ERROR: card-direct mon_install at $MON64_INST_AT" >&2; exit 1; }
    $OBJCOPY -O binary monitor64.elf monitor64.bin
    [ "$(fsz monitor64.bin)" -le 16384 ] || { echo "ERROR: monitor64.bin is $(fsz monitor64.bin) bytes (max 16384)" >&2; exit 1; }
    echo "card-direct borrowed placement: monitor $(fsz monitor64.bin) bytes at $(printf 0x%08x $MON64_PI), hook64 $(stat -c%s hook64.bin) bytes"
    # the alternate vector-page layout (lowpage.S SC64SS_LP_ALT, lowpage_alt.ld; monitor.S LP_ALT): the
    # engine at 0x360, the gate at 0x200, a front at 0x3C0 that has the cart installer put the fragments
    # back after a game's clear of the page (Pokemon Stadium 2). The same monitor with the installer's
    # alt version; the menu picks it by title.
    $CC $CFLAGS -DSC64SS_LP_ALT=1 -c lowpage.S -o lowpage_alt.o
    $CC $CFLAGS -nostartfiles -Wl,-T,lowpage_alt.ld -Wl,--defsym,MON_TICK=$MON64_TICK -Wl,--defsym,MON_INSTALL=$MON64_INSTALL -Wl,--defsym,GAME_TAIL=0x80000120 -o lowpage64b.elf lowpage_alt.o
    for f in f0f0 f110 f130 fexit f360 f3c0; do $OBJCOPY -O binary -j .$f lowpage64b.elf lp64b_$f.bin; done
    [ "$(fsz lp64b_f360.bin)" -le 144 ] || { echo "ERROR: the alt gate fragment is $(fsz lp64b_f360.bin) bytes (max 144)" >&2; exit 1; }
    [ "$(fsz lp64b_f3c0.bin)" -le 48 ] || { echo "ERROR: the alt front fragment is $(fsz lp64b_f3c0.bin) bytes (max 48)" >&2; exit 1; }
    [ "$(fsz lp64b_fexit.bin)" -le 36 ] || { echo "ERROR: the alt EXIT fragment is $(fsz lp64b_fexit.bin) bytes (max 36)" >&2; exit 1; }
    lpsym64b() { mips64-elf-nm lowpage64b.elf | awk -v s="$1" '$3==s{print "0x"$1}'; }
    LP64B_DMA=$(lpsym64b lp_dma); LP64B_PIO_W=$(lpsym64b lp_pio_w); LP64B_EXIT_ERET=$(lpsym64b lp_exit_eret)
    [ "$(( LP64B_PIO_W ))" -eq "$(( LP_PIO_W ))" ] || { echo "ERROR: the alt helper moved" >&2; exit 1; }
    $CC $CFLAGS -Wa,--defsym,SCRATCH=0x10 -Wa,--defsym,CARD_STASH=1 -Wa,--defsym,LP_ALT=1 -c monitor.S -o monitor64b.o
    $CC $CFLAGS -nostartfiles -Wl,-T,monitor.ld -Wl,--defsym,MON_BASE=$MON64_BASE -Wl,--defsym,CFG_ADDR=$CFG64_ADDR \
        -Wl,--defsym,HOOK_LEN1=$HOOK64_LEN1 -Wl,--defsym,STAGING_PI=$STAGING64 -Wl,--defsym,STASH_PI=$STASH_PI \
        -Wl,--defsym,LP_DMA=$LP64B_DMA -Wl,--defsym,LP_PIO_W=$LP64B_PIO_W \
        -Wl,--defsym,LP_EXIT=$LP_EXIT -Wl,--defsym,LP_EXIT_ERET=$LP64B_EXIT_ERET \
        -Wl,--defsym,CTX_KSEG1=$CTX64 -Wl,--defsym,VPAK_KSEG1=$VPAK_KSEG1 -Wl,--defsym,PAK_CTL=$PAK_CTL \
        -Wl,--defsym,VPAK_CRC=$VPAK_CRC -Wl,--defsym,STASH_SEC=$STASH64_SEC -o monitor64b.elf monitor64b.o
    for sym in mon_tick:0x100 mon_epi_load:0x2200 mon_install:0x3000; do
        at=$(mips64-elf-nm monitor64b.elf | awk -v s="${sym%%:*}" '$3==s{print "0x"$1}')
        [ "$(( at ))" -eq "$(( MON64_BASE + ${sym##*:} ))" ] || { echo "ERROR: alt monitor ${sym%%:*} at $at" >&2; exit 1; }
    done
    [ "$(( $(mips64-elf-objdump -h monitor64b.elf | awk '$2==".text"{print "0x"$4"+0x"$3}') ))" -le "$(( MON64_BASE + 0x1200 ))" ] || { echo "ERROR: the alt monitor's .text runs past +0x1200" >&2; exit 1; }
    $OBJCOPY -O binary monitor64b.elf monitor64b.bin
    [ "$(fsz monitor64b.bin)" -le 16384 ] || { echo "ERROR: monitor64b.bin is $(fsz monitor64b.bin) bytes (max 16384)" >&2; exit 1; }
    echo "alt vector-page layout: monitor $(fsz monitor64b.bin) bytes; gate/front/EXIT = $(fsz lp64b_f360.bin)/$(fsz lp64b_f3c0.bin)/$(fsz lp64b_fexit.bin) bytes"
fi

python3 - "$SIZE" "$CFG_OFF" <<'EOF'
import re, struct, sys
size = int(sys.argv[1])
cfg_off = int(sys.argv[2])
words = []
with open("hook.bin", "rb") as f:
    data = f.read()
data += b"\x00" * ((4 - len(data) % 4) % 4)
for i in range(0, len(data), 4):
    words.append(struct.unpack(">I", data[i:i+4])[0])

def words_of(path):
    d = open(path, "rb").read()
    d += b"\x00" * ((4 - len(d) % 4) % 4)
    return [struct.unpack(">I", d[i:i+4])[0] for i in range(0, len(d), 4)]
# borrowed-RAM mode: the vector-page fragments (copied into place by the boot patcher)
# and the monitor (written to the cart by the menu)
frags = [("sc64ss_lowpage_0f0", 0x800000F0, words_of("lp_f0f0.bin")),
         ("sc64ss_lowpage_110", 0x80000110, words_of("lp_f110.bin")),
         ("sc64ss_lowpage_130", 0x80000130, words_of("lp_f130.bin")),
         ("sc64ss_lowpage_1dc", 0x800001DC, words_of("lp_f1dc.bin")),
         ("sc64ss_lowpage_360", 0x80000360, words_of("lp_f360.bin"))]
monitor = words_of("monitor.bin")
monitor_hi = words_of("monitor_hi.bin")   # the scratch at 0x2A8 (see build.sh)
# the card-direct blob for 64 MiB ROMs (SC64SS_CARD_DIRECT=1 build of the same sources, made just before)
import os
hook64 = words_of("hook64.bin") if os.path.exists("hook64.bin") else []
hook64_size = os.path.getsize("hook64.bin") if hook64 else 0
hook64_cfg = int(open("hook64.cfgoff").read().strip()) if hook64 else 0
monitor64 = words_of("monitor64.bin") if (hook64 and os.path.exists("monitor64.bin")) else []
frags64 = [("sc64ss_lowpage64_0f0", 0x800000F0, words_of("lp64_f0f0.bin")),
           ("sc64ss_lowpage64_110", 0x80000110, words_of("lp64_f110.bin")),
           ("sc64ss_lowpage64_130", 0x80000130, words_of("lp64_f130.bin")),
           ("sc64ss_lowpage64_1dc", 0x800001DC, words_of("lp64_f1dc.bin")),
           ("sc64ss_lowpage64_360", 0x80000360, words_of("lp64_f360.bin"))] if monitor64 else []
# the alternate vector-page layout (LP_ALT): the same monitor with the installer's alt version, the
# fragments linked for it (the gate at 0x200, the front at 0x3C0, EXIT at 0x1DC from the start)
monitor64b = words_of("monitor64b.bin") if (monitor64 and os.path.exists("monitor64b.bin")) else []
frags64b = [("sc64ss_lowpage64b_0f0", 0x800000F0, words_of("lp64b_f0f0.bin")),
            ("sc64ss_lowpage64b_110", 0x80000110, words_of("lp64b_f110.bin")),
            ("sc64ss_lowpage64b_130", 0x80000130, words_of("lp64b_f130.bin")),
            ("sc64ss_lowpage64b_1dc", 0x800001DC, words_of("lp64b_fexit.bin")),
            ("sc64ss_lowpage64b_200", 0x80000200, words_of("lp64b_f360.bin")),
            ("sc64ss_lowpage64b_3c0", 0x800003C0, words_of("lp64b_f3c0.bin"))] if monitor64b else []
FRAGS64B_NAMES = ("sc64ss_lowpage64b_0f0", "sc64ss_lowpage64b_110", "sc64ss_lowpage64b_130", "sc64ss_lowpage64b_1dc", "sc64ss_lowpage64b_200", "sc64ss_lowpage64b_3c0")

# the cart layout and slot geometry, straight from the hook sources (one source of truth)
src = open("hook.c").read() + open("ss_overlay.c").read()
def const(name):
    m = re.search(r"^#define\s+%s\s+(0x[0-9A-Fa-f]+|\d+)u?\b" % name, src, re.M)
    if not m:
        sys.exit("layout constant %s not found in the hook sources" % name)
    return int(m.group(1), 0)
layout = [("SC64SS_HOOK_STAGING_PI", const("HOOK_STAGING_PI"), "cart PI address of the hook's staging copy (128 KiB)"),
          ("SC64SS_FRAME_STASH_PI", const("FRAME_STASH_PI"), "cart PI address of the frame stash"),
          ("SC64SS_FRAME_STASH_LEN", const("FRAME_STASH_LEN"), "its size"),
          ("SC64SS_STATE_SLOT_LEN", const("STATE_SLOT_LEN"), "stride of a state slot in cart SDRAM"),
          ("SC64SS_STATE_HDR_OFF", const("STATE_HDR_OFF"), "the state header inside a slot / SD file"),
          ("SC64SS_STATE_THUMB_OFF", const("STATE_THUMB_OFF"), "the thumbnail inside a slot / SD file"),
          ("SC64SS_SD_TABLE_OFF", const("SD_TABLE_OFF"), "the SD file's sector table inside a slot (menu-written)"),
          ("SC64SS_SD_FILE_SECTORS", const("SD_FILE_SECTORS"), "sectors of a normal-mode state file (head + image)"),
          ("SC64SS_STATE_IMAGE_OFF", const("STATE_IMAGE_OFF"), "the RAM image inside a slot / SD file"),
          ("SC64SS_SD_RUNS_MAX", const("SD_RUNS_MAX"), "run table capacity"),
          ("SC64SS_SD_RUNS_MAGIC", const("SD_RUNS_MAGIC"), "run table magic 'SDR1'"),
          ("SC64SS_SLOTS_MAX", const("CFG_SLOTS_MAX"), "slot table capacity"),
          ("SC64SS_VPAK_PI", const("VPAK_PI"), "cart PI address of the virtual Controller Pak image (run table after it)"),
          ("SC64SS_VPAK_LEN", const("VPAK_LEN"), "its size"),
          ("SC64SS_VPAK_CRC_PI", const("VPAK_CRC_PI"), "the pak's block CRC table (a word a block; the menu computes it at launch)"),
          ("SC64SS_VPAK_CTL_PI", const("VPAK_CTL_PI"), "borrowed mode: the pak's control block (the menu zeroes it, 'VPK1' first)"),
          ("SC64SS_MONITOR_PI", const("MONITOR_PI"), "borrowed mode: cart PI address of the monitor (run in place)"),
          ("SC64SS_MONITOR_LEN", const("MONITOR_LEN"), "its reserved size"),
          ("SC64SS_STASH_PI", const("STASH_PI"), "borrowed mode: cart PI address of the stash of the hook's home"),
          ("SC64SS_STASH_LEN", const("STASH_LEN"), "its size"),
          ("SC64SS_CTX_PI", const("CTX_PI"), "borrowed mode: a loaded state's CPU context for the monitor"),
          ("SC64SS_STATE_SLOT_LEN_B", const("STATE_SLOT_LEN_B"), "borrowed mode: stride of a state slot"),
          ("SC64SS_STATE_IMAGE_LEN_B", const("STATE_IMAGE_LEN_B"), "borrowed mode: the RAM image (all 8 MiB)"),
          ("SC64SS_SD_TABLE_OFF_B", const("SD_TABLE_OFF_B"), "borrowed mode: the run table inside a slot"),
          ("SC64SS_SD_FILE_SECTORS_B", const("SD_FILE_SECTORS_B"), "borrowed mode: sectors of a state file"),
          ("SC64SS_SHOT_TABLE_PI", const("SHOT_TABLE_PI"), "the screenshot file's run table"),
          ("SC64SS_SHOT_HDR_PI", const("SHOT_HDR_PI"), "the screenshot file's header block (4 KiB)"),
          ("SC64SS_SHOT_HDR_SECTORS", const("SHOT_HDR_SECTORS"), "the header block's sectors"),
          ("SC64SS_SHOT_ENTRIES_MAX", const("SHOT_ENTRIES_MAX"), "screenshots the header block can name"),
          ("SC64SS_SD_MAGIC_SHOT", const("SD_MAGIC_SHOT"), "a fresh screenshot file's marker 'SHFR'"),
          ("SC64SS_SLOT_MAPS_PI", const("SLOT_MAPS_PI"), "the card slots' sector maps (the menu writes them at launch)"),
          ("SC64SS_SLOT_MAPS_LEN", const("SLOT_MAPS_LEN"), "its size"),
          ("SC64SS_SLOT_INDEX_PI", const("SLOT_INDEX_PI"), "the card slot index (the menu writes it at launch)"),
          ("SC64SS_SLOT_SCRATCH_PI", const("SLOT_SCRATCH_PI"), "a file's head for the panel (16 KiB)"),
          ("SC64SS_CARD_SLOTS_MAX", const("CARD_SLOTS_MAX"), "the most card slots a game lists"),
          ("SC64SS_CD_STAGING_PI", const("CD_STAGING_PI"), "card-direct: the staging copy in the cart's flash (block 0)"),
          ("SC64SS_CD_MAPS_PI", const("CD_MAPS_PI"), "card-direct: the slot maps in flash (block 1)"),
          ("SC64SS_CD_SHOT_TABLE_PI", const("CD_SHOT_TABLE_PI"), "card-direct: the screenshot file's run table in flash"),
          ("SC64SS_CD_INDEX_PI", const("CD_INDEX_PI"), "card-direct: the initial slot index in flash"),
          ("SC64SS_CD_SHOT_HDR_PI", const("CD_SHOT_HDR_PI"), "card-direct: the initial screenshot header in flash"),
          ("SC64SS_CD_SCRATCH_TABLE_PI", const("CD_SCRATCH_TABLE_PI"), "card-direct: the FreeCam scratch file's run table in flash"),
          ("SC64SS_CD_SCRATCH_N", const("CD_SCRATCH_N"), "card-direct: the scratch file's marker slot number"),
          ("SC64SS_CD_MONITOR_PI", const("CD_MONITOR_PI"), "card-borrowed: the monitor's flash home (16 KiB)"),
          ("SC64SS_CD_STASH_INFO_PI", const("CD_STASH_INFO_PI"), "card-borrowed: 'STSH', the stash file's first sector, its sectors"),
          ("SC64SS_CD_CTX_PI", const("CD_CTX_PI"), "card-borrowed: the load epilogue's context in the data buffer"),
          ("SC64SS_CD_CFG_MIRROR_PI", const("CD_CFG_MIRROR_PI"), "card-direct: the cfg mirror in the data buffer (192 bytes, the flags word after it)")]

with open("hook_blob.c", "w") as f:
    f.write("/* AUTOGENERATED by src/boot/hook/build.sh - do not edit.\n")
    f.write(" * SC64SS save-state hook blob; base 0x807D0000, %d bytes. */\n\n" % size)
    f.write("#include <stdint.h>\n#include \"hook_blob.h\"\n\n")
    f.write("const uint32_t sc64ss_hook_blob[] __attribute__((aligned(16))) = {\n")
    for i in range(0, len(words), 6):
        f.write("    " + ", ".join("0x%08X" % w for w in words[i:i+6]) + ",\n")
    f.write("};\n\nconst uint32_t sc64ss_hook_blob_size = %d;\n" % size)
    for name, addr, ws in frags:
        f.write("\n/* borrowed mode: vector-page fragment for 0x%08X (%d words) */\n" % (addr, len(ws)))
        f.write("const uint32_t %s[] = {\n" % name)
        for i in range(0, len(ws), 6):
            f.write("    " + ", ".join("0x%08X" % w for w in ws[i:i+6]) + ",\n")
        f.write("};\nconst uint32_t %s_words = %d;\n" % (name, len(ws)))
    f.write("\n/* borrowed mode: the monitor, run in place from the cart at SC64SS_MONITOR_PI */\n")
    f.write("const uint32_t sc64ss_monitor_blob[] __attribute__((aligned(16))) = {\n")
    for i in range(0, len(monitor), 6):
        f.write("    " + ", ".join("0x%08X" % w for w in monitor[i:i+6]) + ",\n")
    f.write("};\nconst uint32_t sc64ss_monitor_blob_size = %d;\n" % (4 * len(monitor)))
    # the monitor with its register scratch at 0x2A8 (for titles whose own code lives at
    # 0x000..0x07F) differs from the main one only in the immediates of the parking
    # stores and loads: those words as (index, value) pairs, applied by the menu to a
    # copy of the blob (a second full blob would cost 12.8 KB of menu image for 74 words)
    if len(monitor_hi) != len(monitor):
        sys.exit("monitor_hi.bin and monitor.bin differ in length")
    pairs = [(i, monitor_hi[i]) for i in range(len(monitor)) if monitor_hi[i] != monitor[i]]
    f.write("\n/* the monitor with its register scratch at 0x2A8: the words that differ (index, value) */\n")
    f.write("const uint32_t sc64ss_monitor_hi_patch[] = {\n")
    for i, v in pairs:
        f.write("    %d, 0x%08X,\n" % (i, v))
    f.write("};\nconst uint32_t sc64ss_monitor_hi_patch_pairs = %d;\n" % len(pairs))
    f.write("\n/* card-direct mode: the hook64 blob for 64 MiB ROMs (%d bytes; staged in the cart's flash) */\n" % hook64_size)
    f.write("const uint32_t sc64ss_hook64_blob[] __attribute__((aligned(16))) = {\n")
    for i in range(0, len(hook64), 6):
        f.write("    " + ", ".join("0x%08X" % w for w in hook64[i:i+6]) + ",\n")
    if not hook64:
        f.write("    0,\n")
    f.write("};\nconst uint32_t sc64ss_hook64_blob_size = %d;\n" % hook64_size)
    f.write("\n/* card-borrowed placement: the monitor linked for the flash tables' block (%d bytes) */\n" % (4 * len(monitor64)))
    f.write("const uint32_t sc64ss_monitor64_blob[] __attribute__((aligned(16))) = {\n")
    for i in range(0, len(monitor64), 6):
        f.write("    " + ", ".join("0x%08X" % w for w in monitor64[i:i+6]) + ",\n")
    if not monitor64:
        f.write("    0,\n")
    f.write("};\nconst uint32_t sc64ss_monitor64_blob_size = %d;\n" % (4 * len(monitor64)))
    for name, addr, ws in frags64:
        f.write("\n/* card-borrowed placement: vector-page fragment for 0x%08X (%d words) */\n" % (addr, len(ws)))
        f.write("const uint32_t %s[] = {\n" % name)
        for i in range(0, len(ws), 6):
            f.write("    " + ", ".join("0x%08X" % w for w in ws[i:i+6]) + ",\n")
        f.write("};\nconst uint32_t %s_words = %d;\n" % (name, len(ws)))
    if not frags64:
        for name in ("sc64ss_lowpage64_0f0", "sc64ss_lowpage64_110", "sc64ss_lowpage64_130", "sc64ss_lowpage64_1dc", "sc64ss_lowpage64_360"):
            f.write("const uint32_t %s[] = { 0 };\nconst uint32_t %s_words = 0;\n" % (name, name))
    f.write("\n/* the alternate vector-page layout: the monitor with the installer's alt version (%d bytes) */\n" % (4 * len(monitor64b)))
    f.write("const uint32_t sc64ss_monitor64b_blob[] __attribute__((aligned(16))) = {\n")
    for i in range(0, len(monitor64b), 6):
        f.write("    " + ", ".join("0x%08X" % w for w in monitor64b[i:i+6]) + ",\n")
    if not monitor64b:
        f.write("    0,\n")
    f.write("};\nconst uint32_t sc64ss_monitor64b_blob_size = %d;\n" % (4 * len(monitor64b)))
    for name, addr, ws in frags64b:
        f.write("\n/* the alternate vector-page layout: fragment for 0x%08X (%d words) */\n" % (addr, len(ws)))
        f.write("const uint32_t %s[] = {\n" % name)
        for i in range(0, len(ws), 6):
            f.write("    " + ", ".join("0x%08X" % w for w in ws[i:i+6]) + ",\n")
        f.write("};\nconst uint32_t %s_words = %d;\n" % (name, len(ws)))
    if not frags64b:
        for name in FRAGS64B_NAMES:
            f.write("const uint32_t %s[] = { 0 };\nconst uint32_t %s_words = 0;\n" % (name, name))
with open("hook_blob.h", "w") as f:
    f.write("/* AUTOGENERATED by src/boot/hook/build.sh - do not edit. */\n")
    f.write("#ifndef HOOK_BLOB_H__\n#define HOOK_BLOB_H__\n\n#include <stdint.h>\n\n")
    f.write("#define SC64SS_HOOK_ADDRESS (0x807D0000UL)\n")
    f.write("#define SC64SS_HOOK_DEV (0)\n")
    f.write("#define SC64SS_HOOK_CFG_OFFSET (0x%XUL)   /* struct hook_cfg inside the blob */\n" % cfg_off)
    f.write("#define SC64SS_HOOK_CFG_MAGIC (0x43464731UL)\n")
    f.write("#define SC64SS_HOOK_CFG_WORDS (48)\n\n")
    f.write("/* cart SDRAM layout shared with the hook (hook.c) */\n")
    for name, val, what in layout:
        f.write("#define %s (0x%XUL)   /* %s */\n" % (name, val, what))
    f.write("#define SC64SS_HOOK_STAGING_LEN (0x20000UL)\n")
    f.write("#define SC64SS_HOOK64_CFG_OFFSET (0x%XUL)   /* struct hook_cfg inside the card-direct blob */\n" % hook64_cfg)
    f.write("#define SC64SS_HOOK64_PRESENT (%d)   /* 1: the card-direct blob was built into this menu */\n\n" % (1 if hook64 else 0))
    f.write("extern const uint32_t sc64ss_hook64_blob[];\n")
    f.write("extern const uint32_t sc64ss_hook64_blob_size;\n")
    f.write("#define SC64SS_HOOK64_CB_PRESENT (%d)   /* 1: the card-borrowed monitor and fragments are in */\n" % (1 if monitor64 else 0))
    f.write("extern const uint32_t sc64ss_monitor64_blob[];\nextern const uint32_t sc64ss_monitor64_blob_size;\n")
    for name in ("sc64ss_lowpage64_0f0", "sc64ss_lowpage64_110", "sc64ss_lowpage64_130", "sc64ss_lowpage64_1dc", "sc64ss_lowpage64_360"):
        f.write("extern const uint32_t %s[];\nextern const uint32_t %s_words;\n" % (name, name))
    f.write("#define SC64SS_HOOK64_ALT_PRESENT (%d)   /* 1: the alternate vector-page layout (engine at 0x360, gate at 0x200) is in */\n" % (1 if monitor64b else 0))
    f.write("extern const uint32_t sc64ss_monitor64b_blob[];\nextern const uint32_t sc64ss_monitor64b_blob_size;\n")
    for name in FRAGS64B_NAMES:
        f.write("extern const uint32_t %s[];\nextern const uint32_t %s_words;\n" % (name, name))
    f.write("\n")
    f.write("extern const uint32_t sc64ss_hook_blob[];\n")
    f.write("extern const uint32_t sc64ss_hook_blob_size;\n\n")
    f.write("/* borrowed-RAM mode (lowpage.S, monitor.S) */\n")
    for name, addr, ws in frags:
        f.write("#define %s_ADDRESS (0x%08XUL)\n" % (name.upper(), addr))
        f.write("extern const uint32_t %s[];\nextern const uint32_t %s_words;\n" % (name, name))
    f.write("extern const uint32_t sc64ss_monitor_blob[];\nextern const uint32_t sc64ss_monitor_blob_size;\n")
    f.write("extern const uint32_t sc64ss_monitor_hi_patch[];   /* (index, value) pairs: the scratch at 0x2A8 */\n")
    f.write("extern const uint32_t sc64ss_monitor_hi_patch_pairs;\n\n")
    f.write("#endif\n")
print("generated hook_blob.c / hook_blob.h")
EOF
