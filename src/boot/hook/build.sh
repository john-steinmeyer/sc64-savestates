#!/bin/bash
# Build the save-state hook blob, the borrowed-mode monitor and the vector-page
# fragments, and regenerate src/boot/hook_blob.c / hook_blob.h. Needs the libdragon
# toolchain the menu itself is built with (N64_INST, or /opt/libdragon). Inputs in this
# directory: entry.S, state.S, hook.c, ss_overlay.c, ss_font.h, hook.ld (the hook),
# lowpage.S, lowpage.ld (the vector-page fragments), monitor.S, monitor.ld, pakcrc.inc
# (the cart monitor). The generated files are checked in, so this only has to run after
# a change to these sources.
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
 -fno-jump-tables -fno-zero-initialized-in-bss -Os -Wall -Wextra -Werror"

BASE=0x807D0000
$CC $CFLAGS -c entry.S -o entry.o
$CC $CFLAGS -c state.S -o state.o
$CC $CFLAGS -c hook.c -o hook.o
$CC $CFLAGS -nostartfiles -Wl,-T,hook.ld -Wl,--defsym,HOOK_BASE=$BASE -Wl,-Map=hook.map -o hook.elf entry.o state.o hook.o

BSS_SIZE=$(mips64-elf-size -A hook.elf | awk '$1==".bss"{print $2}')
if [ -n "${BSS_SIZE:-}" ] && [ "$BSS_SIZE" != "0" ]; then
    echo "ERROR: .bss is $BSS_SIZE bytes (must be 0 - give every static an initialiser)" >&2
    exit 1
fi

$OBJCOPY -O binary hook.elf hook.bin
SIZE=$(stat -c%s hook.bin)
# 128 KiB: the RDRAM home (0x807D0000..0x807F0000) and the cart staging area.
if [ "$SIZE" -gt 131072 ]; then
    echo "ERROR: hook.bin is $SIZE bytes (must fit 128 KiB)" >&2
    exit 1
fi
echo "hook.bin: $SIZE bytes (base $BASE, entry _start)"
CFG_ADDR=$(mips64-elf-nm hook.elf | awk '$3=="hook_cfg"{print $1}')
CFG_OFF=$(( 0x$CFG_ADDR - $BASE ))
echo "hook_cfg at offset $CFG_OFF"

# ---- borrowed-RAM mode: the vector-page fragments (lowpage.S) and the cart monitor
# (monitor.S). Layout constants come from hook.c; the monitor is linked at the kseg1
# alias of MONITOR_PI and reaches the RAM routines and the staged config by address.
hc() { python3 -c "import re,sys;print(int(re.search(r'#define\s+%s\s+(0x[0-9A-Fa-f]+)u' % sys.argv[1], open('hook.c').read()).group(1), 0))" "$1"; }
MON_PI=$(hc MONITOR_PI); STASH_PI=$(hc STASH_PI); STAGING_PI=$(hc HOOK_STAGING_PI); CTX_PI=$(hc CTX_PI)
VPAK_PI=$(hc VPAK_PI); VPAK_CTL_PI=$(hc VPAK_CTL_PI)
CTX_KSEG1=$(( 0xA0000000 + CTX_PI ))
VPAK_KSEG1=$(( 0xA0000000 + VPAK_PI ))    # the pak image, read in place by the monitor's server
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
    -o monitor.elf monitor.o
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
$CC $CFLAGS -nostartfiles -Wl,-T,monitor.ld -Wl,--defsym,MON_BASE=$MON_BASE -Wl,--defsym,CFG_ADDR=$CFG_ADDR -Wl,--defsym,HOOK_LEN1=$HOOK_LEN1 -Wl,--defsym,STAGING_PI=$STAGING_PI -Wl,--defsym,STASH_PI=$STASH_PI -Wl,--defsym,LP_DMA=$LP_DMA -Wl,--defsym,LP_PIO_W=$LP_PIO_W -Wl,--defsym,LP_EXIT=$LP_EXIT -Wl,--defsym,LP_EXIT_ERET=$LP_EXIT_ERET -Wl,--defsym,CTX_KSEG1=$CTX_KSEG1 -Wl,--defsym,VPAK_KSEG1=$VPAK_KSEG1 -Wl,--defsym,PAK_CTL=$PAK_CTL -o monitor_hi.elf monitor_hi.o
$OBJCOPY -O binary monitor_hi.elf monitor_hi.bin
[ "$(fsz monitor_hi.bin)" -eq "$(fsz monitor.bin)" ] || { echo "ERROR: monitor_hi.bin ($(fsz monitor_hi.bin) bytes) differs in size from monitor.bin" >&2; exit 1; }

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
          ("SC64SS_SD_MAGIC_SHOT", const("SD_MAGIC_SHOT"), "a fresh screenshot file's marker 'SHFR'")]

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
    f.write("#define SC64SS_HOOK_STAGING_LEN (0x20000UL)\n\n")
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
