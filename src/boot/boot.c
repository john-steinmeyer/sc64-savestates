#include <libdragon.h>

#include "boot_io.h"
#include "boot.h"
#include "cheats.h"
#include "cic.h"
#include "reboot.h"
#include "vr4300_asm.h"
#include "../flashcart/sc64/sc64_ll.h"

/**
 * Selects the base IO address for the configured boot device.
 *
 * @param params Boot parameters whose `device_type` determines the base.
 * @returns Pointer to the boot device base IO registers corresponding to `params->device_type` (e.g., `ROM_CART` or `ROM_DDIPL`).
 */
static io32_t *boot_get_device_base (boot_params_t *params) {
    io32_t *device_base_address = ROM_CART;
    if (params->device_type == BOOT_DEVICE_TYPE_64DD) {
        device_base_address = ROM_DDIPL;
    }
    return device_base_address;
}

static cic_type_t boot_detect_cic (boot_params_t *params) {
    io32_t *base = boot_get_device_base(params);

    uint8_t ipl3[IPL3_LENGTH] __attribute__((aligned(8)));

    data_cache_hit_writeback_invalidate(ipl3, sizeof(ipl3));
    dma_read_raw_async(ipl3, (uint32_t) (&base[16]), sizeof(ipl3));
    dma_wait();

    return cic_detect(ipl3);
}

// SC64SS: libdragon's IPL3 carries a 16-byte aligned banner inside the boot code
static bool boot_ipl3_is_libdragon (io32_t *base) {
    for (uint32_t off = 0x40; off < 0x1000; off += 16) {
        if ((io_read((uint32_t) (base) + off) == 0x204C6962UL) && (io_read((uint32_t) (base) + off + 4) == 0x64726167UL)) {
            return true;   // " Lib" "drag"
        }
    }
    return false;
}

// SC64SS: the ROM offset of the hand-off in libdragon's IPL3 stage three: a `jr a0` whose
// delay slot sets the stack pointer, between the boot code and the ELF header. 0 unless it
// is found exactly once.
static uint32_t boot_libdragon_handoff (io32_t *base) {
    uint32_t elf = 0;
    for (uint32_t off = 0x1000; off < 0x20000; off += 0x100) {
        if (io_read((uint32_t) (base) + off) == 0x7F454C46UL) {
            elf = off;
            break;
        }
    }
    if (elf == 0) {
        return 0;
    }
    uint32_t found = 0;
    uint32_t n = 0;
    for (uint32_t off = 0x1000; (off + 4) < elf; off += 4) {
        if (io_read((uint32_t) (base) + off) == 0x00800008UL) {
            uint32_t slot = io_read((uint32_t) (base) + off + 4) & 0xFC1FFFFFUL;
            if ((slot == 0x0000E825UL) || (slot == 0x0000E821UL)) {
                found = off;
                n++;
            }
        }
    }
    return (n == 1) ? found : 0;
}

// SC64SS: everything the stub restores goes to the cart now, while the patcher and the
// engine's temporary copy are still in RAM; then the hand-off is pointed at the stub.
static void boot_libdragon_stage (io32_t *base, uint32_t handoff) {
    // the menu locked the cart's registers and disabled ROM writes on its way out
    io_write(0x1FFF0010UL, 0);
    io_write(0x1FFF0010UL, 0x5F554E4CUL);
    io_write(0x1FFF0010UL, 0x4F434B5FUL);
    sc64_ll_set_config(CFG_ID_ROM_WRITE_ENABLE, true);
    // (uncached reads: the emitters wrote their caches back, and the rest of each 4 KiB
    // is whatever RAM held, never run)
    const volatile uint32_t *patcher = (const volatile uint32_t *) (0xA0000000UL | (SC64SS_LDBOOT_PATCHER_RAM & 0x1FFFFFFFUL));
    const volatile uint32_t *engine = (const volatile uint32_t *) (0xA0000000UL | (SC64SS_LDBOOT_ENGINE_RAM & 0x1FFFFFFFUL));
    for (uint32_t i = 0; i < SC64SS_LDBOOT_REGION_WORDS; i++) {
        io_write(SC64SS_LDBOOT_PI + SC64SS_LDBOOT_PATCHER_OFF + (4 * i), patcher[i]);
        io_write(SC64SS_LDBOOT_PI + SC64SS_LDBOOT_ENGINE_OFF + (4 * i), engine[i]);
    }
    uint32_t stub[32];
    uint32_t n = cheats_libdragon_stub(stub);
    for (uint32_t i = 0; i < n; i++) {
        io_write(SC64SS_LDBOOT_PI + (4 * i), stub[i]);
    }
    io_write((uint32_t) (base) + handoff, I_J(0xB0000000UL | SC64SS_LDBOOT_PI));
    // breadcrumbs in the cart's block RAM, for a PC to read: the hand-off's offset, a tag
    io_write(0x1FFE0FF0UL, handoff);
    io_write(0x1FFE0FF4UL, 0x4C444254UL);
}

/**
 * Prepare system hardware, load reboot code and IPL3, install cheats, and transfer control to the reboot routine using the provided boot parameters.
 *
 * This performs CIC detection (and optionally populates the CIC seed), normalizes passthrough TV type, configures and resets CPU/SP/PI/VI/AI state, programs PI DOM timing from the boot device, copies the reboot routine into SP IMEM and IPL3 into SP DMEM, installs cheats, arranges boot-time registers, and jumps to the in-memory reboot entry. If control returns, the function loops indefinitely.
 *
 * @param params Boot configuration and state. Fields read include device_type, tv_type, detect_cic_seed, and cheat_list. On return this function may modify params->tv_type (when passthrough normalization is applied) and params->cic_seed (when detect_cic_seed is true).
 */
void boot (boot_params_t *params) {
    cic_type_t cic_type = boot_detect_cic(params);
    // SC64SS: whether the ROM boots with libdragon's boot code, read here (the CIC detection
    // reads the ROM the same way), not after the PI reset below: read there, the console hung
    // on every game.
    bool libdragon = boot_ipl3_is_libdragon(boot_get_device_base(params));

    if (params->detect_cic_seed) {
        params->cic_seed = cic_get_seed(cic_type);
    }

    if (params->tv_type == BOOT_TV_TYPE_PASSTHROUGH) {
        switch (get_tv_type()) {
            case TV_PAL:
                params->tv_type = BOOT_TV_TYPE_PAL;
                break;
            case TV_NTSC:
                params->tv_type = BOOT_TV_TYPE_NTSC;
                break;
            case TV_MPAL:
                params->tv_type = BOOT_TV_TYPE_MPAL;
                break;
            default:
                params->tv_type = BOOT_TV_TYPE_NTSC;
                break;
        }
    }

    while (!(cpu_io_read(&SP->SR) & SP_SR_HALT));

    cpu_io_write(&SP->SR,
        SP_SR_CLR_SIG7 |
        SP_SR_CLR_SIG6 |
        SP_SR_CLR_SIG5 |
        SP_SR_CLR_SIG4 |
        SP_SR_CLR_SIG3 |
        SP_SR_CLR_SIG2 |
        SP_SR_CLR_SIG1 |
        SP_SR_CLR_SIG0 |
        SP_SR_CLR_INTR_BREAK |
        SP_SR_CLR_SSTEP |
        SP_SR_CLR_INTR |
        SP_SR_CLR_BROKE |
        SP_SR_SET_HALT
    );
    cpu_io_write(&SP->SEMAPHORE, 0);
    cpu_io_write(&SP->PC, 0);

    while (cpu_io_read(&SP->DMA_BUSY));

    cpu_io_write(&PI->SR, PI_SR_CLR_INTR | PI_SR_RESET);

    // Wait for the VI to finish its current frame before proceeding. 
    // This ensures that the VI is not actively reading from RDRAM, 
    // which could lead to data corruption when we clear RDRAM.
    while ((cpu_io_read(&VI->CURR_LINE) & ~(VI_CURR_LINE_FIELD)) != 0);

    /* Fully re-Initialize Audio registers (all booted ROMs should do their own initialization) */
    cpu_io_write(&AI->MADDR, 0);
    cpu_io_write(&AI->LEN, 0);

    /* Fully re-Initialize VI registers (all booted ROMs should do their own initialization) */
    cpu_io_write(&VI->V_INTR, 0x3FF); /*< Vertical Interrupt. */
    cpu_io_write(&VI->H_LIMITS, 0); /*< Horizontal Limits. */
    cpu_io_write(&VI->CURR_LINE, 0); /*< Current Scanline. */
    // SC64SS: not for a ROM with libdragon's boot code. Its video setup is applied from the
    // vblank interrupt unless the VI is off, and with the VI left on and every timing zeroed
    // that interrupt never comes: FlappyBird stayed a blank screen. Those ROMs keep the three
    // writes above (the behaviour every libdragon title was tested with); the rest get the
    // full reset, which stops crashes in Ocarina of Time.
    if (!libdragon) {
    cpu_io_write(&VI->MADDR, 0); /**< Memory Address. */
    cpu_io_write(&VI->H_WIDTH, 0); /**< Horizontal Width. */
    cpu_io_write(&VI->TIMING, 0); /**< Timings. */
    cpu_io_write(&VI->V_SYNC, 0); /**< Vertical Sync. */
    cpu_io_write(&VI->H_SYNC, 0); /**< Horizontal Sync. (this one is particularly important for RD RAM init) */
    cpu_io_write(&VI->H_SYNC_LEAP, 0); /**< Horizontal Sync Leap. */
    cpu_io_write(&VI->V_LIMITS, 0); /**< Vertical Limits. */
    cpu_io_write(&VI->COLOR_BURST, 0); /**< Color Burst. */
    cpu_io_write(&VI->H_SCALE, 0); /**< Horizontal Scale. */
    cpu_io_write(&VI->V_SCALE, 0); /**< Vertical Scale. */
    }

    while (cpu_io_read(&SP->SR) & SP_SR_DMA_BUSY);

    uint32_t *reboot_src = &reboot_start;
    io32_t *reboot_dst = SP_MEM->IMEM;
    size_t reboot_instructions = (size_t) (&reboot_size) / sizeof(uint32_t);

    for (unsigned int i = 0; i < reboot_instructions; i++) {
        cpu_io_write(&reboot_dst[i], reboot_src[i]);
    }

    cpu_io_write(&PI->DOM[0].LAT, 0xFF);
    cpu_io_write(&PI->DOM[0].PWD, 0xFF);
    cpu_io_write(&PI->DOM[0].PGS, 0x0F);
    cpu_io_write(&PI->DOM[0].RLS, 0x03);

    io32_t *base = boot_get_device_base(params);
    uint32_t pi_config = io_read((uint32_t) (base));

    cpu_io_write(&PI->DOM[0].LAT, pi_config & 0xFF);
    cpu_io_write(&PI->DOM[0].PWD, pi_config >> 8);
    cpu_io_write(&PI->DOM[0].PGS, pi_config >> 16);
    cpu_io_write(&PI->DOM[0].RLS, pi_config >> 20);

    if (cpu_io_read(&DPC->SR) & DPC_SR_XBUS_DMEM_DMA) {
        while (cpu_io_read(&DPC->SR) & DPC_SR_PIPE_BUSY);
    }

    io32_t *ipl3_src = base;
    io32_t *ipl3_dst = SP_MEM->DMEM;

    for (int i = 16; i < 1024; i++) {
        cpu_io_write(&ipl3_dst[i], io_read((uint32_t) (&ipl3_src[i])));
    }

    // SC64SS: a ROM with libdragon's IPL3 gets the engine through a stub on the cart
    // (cheats.h): nothing parked in RAM survives that boot code, and its hand-off to the
    // game runs from the cart, where the menu can point it at the stub.
    uint32_t handoff = libdragon ? boot_libdragon_handoff(base) : 0;

    // SC64SS: libdragon's boot code without the hand-off (a boot code newer than this finder
    // knows): nothing is installed, the retail way would patch boot code that is not the
    // retail one's, and the game runs as it always did (the menu keeps the routine out too)
    bool cheats_installed = (libdragon && (handoff == 0)) ? false :
        cheats_install(cic_type, params->cheat_list, params->hook_blob, params->hook_size, params->boot_patches, params->boot_patch_count, params->hook_borrowed, params->watch_reads, libdragon);

    if (libdragon && (handoff != 0) && cheats_installed) {
        boot_libdragon_stage(base, handoff);
    }

    register uint32_t clear_rdram asm ("s1");
    register uint32_t skip_rdram_reset asm ("a0");
    register uint32_t boot_device asm ("s3");
    register uint32_t tv_type asm ("s4");
    register uint32_t reset_type asm ("s5");
    register uint32_t cic_seed asm ("s6");
    register uint32_t version asm ("s7");

    // SC64SS: the per-ROM "clear RDRAM" option keeps working with cheats or
    // save states installed. The clear covers the lower 4 MiB only; the patcher
    // and the engine's temporary copy sit at 0x80700000 and 0x80710000 and the
    // hook is fetched from the cart afterwards, so nothing of ours is lost.
    clear_rdram = params->clear_rdram;
    // (a libdragon ROM's engine waits on the cart, so its RAM can be reset as usual)
    skip_rdram_reset = cheats_installed && !libdragon;
    boot_device = (params->device_type & 0x01);
    tv_type = (params->tv_type & 0x03);
    reset_type = BOOT_RESET_TYPE_COLD;
    cic_seed = (params->cic_seed & 0xFF);
    version = (params->tv_type == BOOT_TV_TYPE_PAL) ? 6
            : (params->tv_type == BOOT_TV_TYPE_NTSC) ? 1
            : (params->tv_type == BOOT_TV_TYPE_MPAL) ? 4
            : 0;

    asm volatile (
        "li $t3, %[c0_status] \n"
        "mtc0 $t3, $12 \n"
        "ctc1 $zero, $f31 \n"
        "la $t3, reboot \n"
        "jr $t3 \n" ::
        [c0_status] "i" (C0_STATUS_CU1 | C0_STATUS_CU0 | C0_STATUS_FR),
        [clear_rdram] "r" (clear_rdram),
        [skip_rdram_reset] "r" (skip_rdram_reset),
        [boot_device] "r" (boot_device),
        [tv_type] "r" (tv_type),
        [reset_type] "r" (reset_type),
        [cic_seed] "r" (cic_seed),
        [version] "r" (version) :
        "t3"
    );

    while (1);
}
