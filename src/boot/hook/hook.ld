/* SC64SS hook: fixed-base freestanding blob at 0x807D0000 (top of Expansion
 * Pak RAM; the Datel engine's default home is 0x807C5C00, below us). Entry
 * point must be the first byte: .text.entry is placed first and KEEPed. */

OUTPUT_ARCH(mips)
ENTRY(_start)

SECTIONS {
    . = HOOK_BASE;   /* build.sh passes --defsym HOOK_BASE=0x807D0000 (boot blob) or 0x807E0000 (hot-swap build) */

    .text : {
        KEEP(*(.text.entry))
        *(.text*)
    }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) *(.sdata*) }
    /* -fno-zero-initialized-in-bss keeps C zeros in .data; the asm .space
     * blocks also live in .data, so the binary carries everything and no
     * runtime zeroing is needed. Catch strays so they fail the size check. */
    .bss : { *(.bss*) *(.sbss*) *(COMMON) }

    /DISCARD/ : { *(.MIPS.abiflags) *(.reginfo) *(.pdr) *(.comment) *(.gnu.attributes) *(.mdebug*) }
}
