/* SC64SS borrowed-RAM monitor: one text section, run in place from the cart at
 * MON_BASE (build.sh: --defsym MON_BASE=<kseg1 of SC64SS_MONITOR_PI>). */
OUTPUT_ARCH(mips)
ENTRY(mon_epilogue)
SECTIONS {
    . = MON_BASE;
    .text : { KEEP(*(.text)) }
    /* the virtual Controller Pak's server (mon_pak_hit: the RAM stub at 0x060 jumps to
     * MONITOR_KSEG1 + 0x1000, hook.c MON_PAK_OFF) */
    . = MON_BASE + 0x1000;
    .pak : { KEEP(*(.text.pak)) }
    /* the load epilogue (mon_epi_load, hook.c jumps to MONITOR_KSEG1 + 0x2000, MON_EPIL_OFF) */
    . = MON_BASE + 0x2000;
    .epil : { KEEP(*(.text.epil)) }
    /* the gate installer (mon_install) at a fixed offset: build.sh passes MON_INSTALL
     * (= MON_BASE + 0x3000) to lowpage.S, whose trampoline jumps there */
    . = MON_BASE + 0x3000;
    .install : { KEEP(*(.text.install)) }
    /DISCARD/ : { *(.MIPS.abiflags) *(.reginfo) *(.pdr) *(.comment) *(.gnu.attributes) *(.mdebug*) *(.data*) *(.bss*) }
}
