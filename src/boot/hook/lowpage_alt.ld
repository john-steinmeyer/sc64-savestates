/* SC64SS: the alternate vector-page layout (lowpage.S SC64SS_LP_ALT): the engine at 0x360
 * (the boot patcher emits it there), the gate + DMA_PRIM at 0x200, EXIT at 0x1DC from the
 * start (no pre-install check: the front at 0x3C0 asks the cart installer instead), and
 * the front at 0x3C0. build.sh checks each fits its hole and exports the words. */
OUTPUT_ARCH(mips)
ENTRY(lp_gate)
SECTIONS {
    . = 0x800000F0;
    .f0f0 : { KEEP(*(.text.f0f0)) }
    . = 0x80000110;
    .f110 : { KEEP(*(.text.f110)) }
    . = 0x80000130;
    .f130 : { KEEP(*(.text.f130)) }
    . = 0x800001DC;
    .fexit : { KEEP(*(.text.fexit)) }   /* EXIT at its home from the start */
    . = 0x80000200;
    .f360 : { KEEP(*(.text.f360)) }     /* the gate + DMA_PRIM, at 0x200 in this layout */
    . = 0x800003C0;
    .f3c0 : { KEEP(*(.text.f3c0)) }     /* the front */
    . = 0x80000400;
    .f1dc : { KEEP(*(.text.f1dc)) }     /* the pre-install check: unused here, a dummy home */
    /DISCARD/ : { *(.MIPS.abiflags) *(.reginfo) *(.pdr) *(.comment) *(.gnu.attributes) *(.mdebug*) }
}
