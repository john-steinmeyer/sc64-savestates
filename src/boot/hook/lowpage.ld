/* SC64SS borrowed-RAM mode: the vector-page fragments at their absolute homes.
 * build.sh checks each fits its hole and exports the words for the boot patcher. */
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
    .f1dc : { KEEP(*(.text.f1dc)) }
    . = 0x80000360;
    .f360 : { KEEP(*(.text.f360)) }
    . = 0x80000400;
    .fexit : { KEEP(*(.text.fexit)) }   /* EXIT: a dummy home; the cart installer copies it to 0x1DC */
    /DISCARD/ : { *(.MIPS.abiflags) *(.reginfo) *(.pdr) *(.comment) *(.gnu.attributes) *(.mdebug*) }
}
