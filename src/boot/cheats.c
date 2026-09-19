/**
 * @file cheats.c
 * @brief Cheat Engine Implementation
 * @ingroup boot
 */

#include <libdragon.h>
#include "boot_io.h"
#include "cheats.h"
#include "vr4300_asm.h"
#include "hook_blob.h"

#define HIT_INVALIDATE_I ((4 << 2) | 0)
#define HIT_WRITE_BACK_D ((6 << 2) | 1)

#define D_CACHE_LINE_SIZE (16)

#define CAUSE_IRQ_PRE_NMI (1 << 12)
#define CAUSE_EXC_CODE_MASK (0x7C)
#define CAUSE_EXC_CODE_WATCH (0x5C)

#define WATCHLO_W (1 << 0)
#define WATCHLO_R (1 << 1)

// SC64SS: extra instruction encoders (vr4300_asm.h lacks these)
#define I_ADDU(rd, rs, rt) __ASM_R_INST(OP_SPECIAL, rs, rt, rd, 0, FUNCT_ADDU)
#define I_SUBU(rd, rs, rt) __ASM_R_INST(OP_SPECIAL, rs, rt, rd, 0, FUNCT_SUBU)
#define I_SLL(rd, rt, sa) __ASM_R_INST(OP_SPECIAL, 0, rt, rd, sa, FUNCT_SSL)
#define I_SLTIU(rt, rs, immediate) __ASM_I_INST(OP_SLTIU, rs, rt, immediate)


#define RELOCATED_EXCEPTION_HANDLER_ADDRESS (0x80000120)
#define EXCEPTION_HANDLER_ADDRESS (0x80000180)
#define PATCHER_ADDRESS (0x80700000)
#define ENGINE_TEMPORARY_ADDRESS (PATCHER_ADDRESS + 0x10000)
#define DEFAULT_ENGINE_ADDRESS (0x807C5C00)
// SC64SS: games such as Zelda OoT zero all RAM above their boot segment up to
// osMemSize at boot (and DK64 clears 0x805FB300..0x80800000), which wipes an
// engine at 0x807C5C00 and the hook before the first interrupt. With no cheat
// codes the engine is 24 words and fits the unused tail of the general-exception
// vector slot (libultra writes 16 bytes at 0x80000180; the CPU only ever jumps
// to 0x180; the 6105 IPL3's RAM window is 0x200..0x2FF), which no game sweeps.
// Its tail then runs a stub in the hole between osAppNMIBuffer and the 6105
// memsize word that re-copies the hook from its cart staging area whenever the
// hook's first word is missing, so the hook survives the game's sweep too.
// SC64SS: the engine used to sit at 0x80000190 (tail of the general-exception
// slot). Banjo-Kazooie's copy-protection reads the 6103 IPL3's RAM copy at
// 0x1D8 (and 0x200, 0x22C, 0x238) and, when 0x1D8 is not the IPL3 word, builds
// every camera with sabotage near/far planes (black sky, a thin slice of the
// world). The tail of the XTLB-refill slot (0x090..0x0EF) is not checked by any
// game seen so far and is never executed: libultra writes 16 bytes at 0x080.
#define SC64SS_LOWPAGE_ENGINE_ADDRESS   (0x80000090)
// SC64SS bisect (development, set by build_ce.sh from SC64SS_BISECT; 0 = the real
// thing): borrowed-mode installs with parts left out, used to find Perfect Dark's
// black screen. 1 = the engine only (tail straight to the game, no
// fragments, no gate), 2 = the fragments too but still no gate, 3 = no engine at all,
// 4 = no watchpoint, 5 = the watchpoint fires but the engine only disarms it (no
// relocation), 6/7 = the base register hardcoded to t7 (7: disarmed after), 9 = the
// real relocation, disarmed at the first exception, 10 = 5 plus the self-modifying
// store and cache ops, 11 = 5 with the placeholder built as ori t7,k0,0x120 (no
// runtime self-modification), 12 = 11 with the tail straight to the game.
#ifndef SC64SS_BISECT
#define SC64SS_BISECT 0
#endif
// SC64SS diagnostics (development, set by build_ce.sh): the engine writes the Cause of
// every exception it sees to the cart buffer at +0x1FC8 in place of the pre-NMI
// disarm, so a hang before the first interrupt shows where the game loops.
#define SC64SS_DIAG_WATCHEPC 0
// SC64SS: the engine's watchpoint covers reads of the vector too (see the WatchLo
// setup below); build_ce.sh can switch it off for a bisect.
#define SC64SS_WATCH_READS 1
#define SC64SS_REINSTALL_STUB_ADDRESS   (0x80000360)
// SC64SS borrowed mode: the engine's tail first runs the vector page's pre-install
// check (lowpage.S lp_preinstall at 0x1DC, then 0x118 and 0x178), which goes on to the
// monitor's gate installer in the cart (monitor.S mon_install) only for an interrupt
// with the PI idle - a cart fetch under a game's DMA freezes the console (Indiana
// Jones) - and to the game's vector words otherwise; the installer copies
// the gate to 0x360 and rewrites the tail to j 0x360. The patcher cannot leave the
// gate there itself: Perfect Dark's boot stack lives in the low page (sp =
// 0x80000F10, frames down to 0x35C) and overwrote it before the first interrupt.
#define SC64SS_LOWPAGE_TRAMP_ADDRESS    (SC64SS_LOWPAGE_110_ADDRESS + 8)
// The hook's staging copy in cart SDRAM: laid out by the hook itself (hook.c) and
// exported through hook_blob.h, so the menu's slot table and the patcher agree.
#define SC64SS_HOOK_STAGING_PI_ADDRESS  (SC64SS_HOOK_STAGING_PI)
_Static_assert((SC64SS_HOOK_STAGING_PI_ADDRESS & 0xFFFF) == 0, "the staging address must be 64 KiB aligned (lui only)");

/** @brief Cheat structure */
typedef struct {
    uint8_t type; /**< Cheat type */
    uint32_t address; /**< Cheat address */
    uint16_t value; /**< Cheat value */
} cheat_t;

/** @brief Cheat entry structure */
typedef struct {
    cheat_t main; /**< Main cheat */
    cheat_t sub; /**< Sub cheat */
} cheat_entry_t;

/** @brief Special cheat types enumeration */
typedef enum {
    SPECIAL_CLEAR_MEMORY = 0x20, /**< Clear memory between 0x80000200-0x80000300 on boot */
    SPECIAL_SECONDARY_EXCEPTION_HANDLER = 0xCC, /**< Use alternate exception handler */
    SPECIAL_SET_ENTRYPOINT_ADDR = 0xDE, /**< Set boot entrypoint address */
    SPECIAL_DISABLE_EXPANSION_PAK = 0xEE, /**< Disable Expansion Pak */
    SPECIAL_WRITE_BYTE_ON_BOOT = 0xF0, /**< Write byte on boot */
    SPECIAL_WRITE_SHORT_ON_BOOT = 0xF1, /**< Write short on boot */
    SPECIAL_SET_STORE_LOCATION = 0xFF, /**< Set store location */
} cheat_type_special_t;

#define IS_WIDTH_16(t) ((t) & (1 << 0))
#define IS_CONDITION_NOT_EQUAL(t) ((t) & (1 << 1))
#define IS_CONDITION_GS_BUTTON(t) ((t) & (1 << 3))

#define IS_TYPE_REPEATER(t) ((t) == 0x50)
#define IS_TYPE_WRITE(t) ((((t)&0xF0) == 0x80) || (((t)&0xF0) == 0xA0))
#define IS_TYPE_CONDITIONAL(t) (((t)&0xF0) == 0xD0)

#define IS_DOUBLE_ENTRY(t) (IS_TYPE_CONDITIONAL(t) || IS_TYPE_REPEATER(t))

#define X106_XOR_CONSTANT (0x0260BCD5)
#define X106_ENC_START (0x13C)

/**
 * @brief Get the XOR value for a given offset in the CIC x106 encrypted area.
 *
 * Calls to this function ought to always be reduced to constants.
 *
 * @param seed The IPL3 checksum seed (should always be 0x85 for x106; see cic_get_seed()).
 * @param offset The offset in the encrypted area to calculate for.
 * @return the calculated XOR value.
 */
__attribute__((always_inline))
static inline uint32_t cheats_calc_x106_xor(uint8_t seed, uint8_t offset) {
    uint32_t val = X106_XOR_CONSTANT * seed + 1;
    #pragma GCC unroll 256
    for (uint8_t i = 0; i < offset; i++) {
        val *= X106_XOR_CONSTANT;
    }
    return val;
}

/**
 * @brief Patch the IPL3 with the cheat engine.
 * 
 * @param cic_type The CIC type.
 * @param target The target address.
 * @return true if successful, false otherwise.
 */
static bool cheats_patch_ipl3 (cic_type_t cic_type, io32_t *target) {
    uint32_t patch_offset = 0;
    uint32_t j_instruction = I_J((uint32_t)(target));

    io32_t *ipl3 = SP_MEM->DMEM;

    switch (cic_type) {
    case CIC_5101: patch_offset = 476; break;
    // SC64SS: the 6101 IPL3 (CRC 6170A4A1, Star Fox 64) has its `jr $t1` at word
    // 476, not 466; with 466 the check below saw `bne` and the game silently booted
    // without the engine. 7102 (PAL) keeps upstream's value until verified.
    case CIC_6101: patch_offset = 476; break;
    case CIC_7102: patch_offset = 466; break;
    case CIC_x102: patch_offset = 475; break;
    case CIC_x103: patch_offset = 472; break;
    case CIC_x105: patch_offset = 499; break;
    case CIC_x106: patch_offset = 488; break;
    default: return true;
    }

    // NOTE: Check for "jr $t1" instruction
    //       Libdragon IPL3 could be brute-force signed with any retail
    //       CIC seed and checksum, and we support only retail libultra IPL3
    uint32_t test_instruction = cpu_io_read(&ipl3[patch_offset]);
    if (cic_type == CIC_x106) {
        // NOTE: CIC x106 IPL3 is partially scrambled
        test_instruction ^= cheats_calc_x106_xor(cic_get_seed(cic_type), patch_offset - X106_ENC_START);
    }

    if ((test_instruction != I_JR(REG_T1)) && (cic_type != CIC_x106)) {
        // SC64SS: the table missed (an IPL3 variant we have not measured). Retail
        // IPL3s end their game launch with a single `jr $t1` in this neighbourhood;
        // use it if it is unique rather than booting without the engine.
        uint32_t found = 0, n = 0;
        for (uint32_t i = 440; i < 520; i++) {
            if (cpu_io_read(&ipl3[i]) == I_JR(REG_T1)) {
                found = i;
                n++;
            }
        }
        if (n == 1) {
            patch_offset = found;
            test_instruction = I_JR(REG_T1);
        }
    }
    if (test_instruction != I_JR(REG_T1)) {
        return false;
    }

    switch (cic_type) {
    case CIC_x105:
        // NOTE: This disables game code checksum verification
        cpu_io_write(&ipl3[486], I_NOP());
        break;

    case CIC_x106:
        // NOTE: CIC x106 IPL3 is partially scrambled
        j_instruction ^= cheats_calc_x106_xor(cic_get_seed(cic_type), patch_offset - X106_ENC_START);
        break;

    default: break;
    }

    cpu_io_write(&ipl3[patch_offset], j_instruction);

    return false;
}

/**
 * @brief Get the next cheat entry from the cheat list.
 * 
 * @param cheat_list Pointer to the cheat list.
 * @param cheat Pointer to the cheat entry structure.
 * @return true if successful, false otherwise.
 */
static bool cheats_get_next (uint32_t **cheat_list, cheat_entry_t *cheat) {
    cheat_t *c = &cheat->main;
    cheat->sub.type = 0;

    for (int i = 0; i < 2; i++) {
        uint32_t raw[2] = {(*cheat_list)[0], (*cheat_list)[1]};

        (*cheat_list) += 2;

        if ((raw[0] == 0) && (raw[1] == 0)) {
            return false;
        }

        c->type = ((raw[0] >> 24) & 0xFF);
        c->address = (raw[0] & 0xA07FFFFF);
        c->value = (raw[1] & 0xFFFF);

        if (!IS_DOUBLE_ENTRY(c->type)) {
            break;
        }

        c = &cheat->sub;
    }

    return true;
}

/**
 * @brief Get the engine address from the cheat list.
 * 
 * @param cheat_list Pointer to the cheat list.
 * @return io32_t* The engine address.
 */
static io32_t *cheats_get_engine_address (uint32_t *cheat_list) {
    cheat_entry_t cheat;
    uint32_t entries = 0;
    while (cheats_get_next(&cheat_list, &cheat)) {
        entries++;
        if (cheat.main.type == SPECIAL_SET_STORE_LOCATION) {
            return (io32_t *)(cheat.main.address & 0x807FFFFF);
        }
    }
    if (entries == 0) {
        // SC64SS: save states only, no codes: the engine fits the vector page
        return (io32_t *)(SC64SS_LOWPAGE_ENGINE_ADDRESS);
    }
    return (io32_t *)(DEFAULT_ENGINE_ADDRESS);
}

/**
 * @brief Update the cache for the specified memory range.
 * 
 * @param start The start address.
 * @param end The end address.
 */
static void cheats_update_cache (volatile void *start, volatile void *end) {
    data_cache_hit_writeback(start, (end - start));
    inst_cache_hit_invalidate(start, (end - start));
}

/**
 * @brief Install the cheat engine.
 * 
 * @param cic_type The CIC type.
 * @param cheat_list Pointer to the cheat list.
 * @return true if successful, false otherwise.
 */
// SC64SS borrowed mode: patcher code that copies `words` words from a temporary
// copy at `src` to their home at `dst` after IPL3 has run, then writes the lines
// back and drops them from the I-cache (the same sequence the engine copy uses).
static io32_t *cheats_emit_copy (io32_t *p, uint32_t src, uint32_t dst, uint32_t words) {
    uint32_t src_end = src + 4 * words;
    *p++ = I_LUI(REG_T3, A_BASE(src));
    *p++ = I_ADDIU(REG_T3, REG_T3, A_OFFSET(src));
    *p++ = I_LUI(REG_T4, A_BASE(src_end));
    *p++ = I_ADDIU(REG_T4, REG_T4, A_OFFSET(src_end));
    *p++ = I_LUI(REG_T5, A_BASE(dst));
    *p++ = I_ADDIU(REG_T5, REG_T5, A_OFFSET(dst));
    *p++ = I_ORI(REG_T6, REG_ZERO, 0);
    *p++ = I_LW(REG_K1, 0, REG_T3);
    *p++ = I_SW(REG_K1, 0, REG_T5);
    *p++ = I_ADDIU(REG_T3, REG_T3, 4);
    *p++ = I_ADDIU(REG_T5, REG_T5, 4);
    *p++ = I_BNE(REG_T3, REG_T4, -5);
    *p++ = I_ADDIU(REG_T6, REG_T6, 4);
    *p++ = I_LUI(REG_T5, A_BASE(dst));
    *p++ = I_ADDIU(REG_T5, REG_T5, A_OFFSET(dst));
    *p++ = I_CACHE(HIT_WRITE_BACK_D, 0, REG_T5);
    *p++ = I_CACHE(HIT_INVALIDATE_I, 0, REG_T5);
    *p++ = I_ADDIU(REG_T6, REG_T6, -D_CACHE_LINE_SIZE);
    *p++ = I_BGTZ(REG_T6, -4);
    *p++ = I_ADDIU(REG_T5, REG_T5, D_CACHE_LINE_SIZE);
    return p;
}

_Static_assert(PATCHER_ADDRESS == SC64SS_LDBOOT_PATCHER_RAM, "cheats.h: the libdragon stub restores the patcher here");
_Static_assert(ENGINE_TEMPORARY_ADDRESS == SC64SS_LDBOOT_ENGINE_RAM, "cheats.h: the libdragon stub restores the engine's copy here");

bool cheats_install (cic_type_t cic_type, uint32_t *cheat_list, const uint32_t *hook_blob, uint32_t hook_size, const uint32_t *boot_patches, uint32_t boot_patch_count, bool hook_borrowed, bool watch_reads, bool libdragon) {
    if (!cheat_list) {
        return false;
    }

    io32_t *engine_start = (io32_t *)(ENGINE_TEMPORARY_ADDRESS);
    io32_t *engine_p = engine_start;

    io32_t *patcher_start = (io32_t *)(PATCHER_ADDRESS);
    io32_t *patcher_p = patcher_start;

    // SC64SS: libdragon's IPL3 is not patched in DMEM; its third stage on the cart is
    // pointed at the stub instead (boot.c), which enters this patcher with the
    // entrypoint in t1, as a retail IPL3 does.
    if (!libdragon && cheats_patch_ipl3(cic_type, patcher_start)) {
        return false;
    }

    io32_t *final_engine_address = cheats_get_engine_address(cheat_list);

    // Original watch exception handler code written by Jay Oster 'Parasyte'
    // https://github.com/parasyte/alt64/blob/master/utils.c#L1024-L1054

    uint32_t ori_placeholder_instruction = I_ORI(REG_ZERO, REG_K0, A_OFFSET(RELOCATED_EXCEPTION_HANDLER_ADDRESS));
    uint32_t ori_placeholder_address = (uint32_t)(final_engine_address + 20);

#if SC64SS_DIAG_WATCHEPC
    // the EPC of the last WATCH exception at +0x1FC8 (a second store here would need a
    // PI wait: a PIO write while the previous one is still busy is dropped). Six words
    // more than the real engine: only for the resident placement, where 0x0F0..0x117
    // is free (the borrowed exit stub lives there).
    *engine_p++ = I_MFC0(REG_K1, C0_REG_CAUSE);
    *engine_p++ = I_ANDI(REG_K1, REG_K1, CAUSE_EXC_CODE_MASK);
    *engine_p++ = I_ADDIU(REG_K1, REG_K1, -CAUSE_EXC_CODE_WATCH);
    *engine_p++ = I_BNE(REG_K1, REG_ZERO, 4);       // not a Watch exception: no record
    *engine_p++ = I_NOP();
    *engine_p++ = I_MFC0(REG_K1, C0_REG_EPC);
    *engine_p++ = I_LUI(REG_K0, 0xBFFE);
    *engine_p++ = I_SW(REG_K1, 0x1FC8, REG_K0);
    *engine_p++ = I_MFC0(REG_K0, C0_REG_CAUSE);
#else
    // Load cause register
    *engine_p++ = I_MFC0(REG_K0, C0_REG_CAUSE);

    // Disable watch exception when reset button is pressed
#if SC64SS_BISECT == 9
    // bisect 9: the real relocation, and the watchpoint disarmed at the first exception
    *engine_p++ = I_NOP();
    *engine_p++ = I_NOP();
    *engine_p++ = I_MTC0(REG_ZERO, C0_REG_WATCH_LO);
#else
    *engine_p++ = I_ANDI(REG_K1, REG_K0, CAUSE_IRQ_PRE_NMI);
    *engine_p++ = I_BNEL(REG_K1, REG_ZERO, 1);
    *engine_p++ = I_MTC0(REG_ZERO, C0_REG_WATCH_LO);
#endif
#endif

    // Check if watch exception occurred, if yes then proceed to relocate the game exception handler
    *engine_p++ = I_ANDI(REG_K0, REG_K0, CAUSE_EXC_CODE_MASK);
    *engine_p++ = I_ORI(REG_K1, REG_ZERO, CAUSE_EXC_CODE_WATCH);
    *engine_p++ = I_BNE(REG_K0, REG_K1, 15); // Skips to after the 'eret' instruction

    // Extract base register number from the store instruction
#if SC64SS_BISECT == 5 || SC64SS_BISECT == 11 || SC64SS_BISECT == 12
    // bisect 5: the watchpoint fires, the engine only disarms it and continues (the
    // placeholder then runs as ori $zero: a no-op); same word count as the real path.
    // bisect 11: the same, but the placeholder is built as ori t7, k0, 0x120 (below):
    // the relocation happens with no runtime self-modification
    *engine_p++ = I_MFC0(REG_K1, C0_REG_EPC);
    *engine_p++ = I_MTC0(REG_ZERO, C0_REG_WATCH_LO);
    for (uint32_t i = 0; i < 10; i++) {
        *engine_p++ = I_NOP();
    }
#elif SC64SS_BISECT == 10
    // bisect 10: like 5 (no relocation), but the self-modifying store and the two cache
    // operations run, writing the no-op ori $zero into the placeholder
    *engine_p++ = I_MFC0(REG_K1, C0_REG_EPC);
    *engine_p++ = I_MTC0(REG_ZERO, C0_REG_WATCH_LO);
    *engine_p++ = I_OR(REG_K1, REG_ZERO, REG_ZERO);
    *engine_p++ = I_NOP();
    *engine_p++ = I_NOP();
    *engine_p++ = I_LUI(REG_K0, ori_placeholder_instruction >> 16);
    *engine_p++ = I_ORI(REG_K0, REG_K0, ori_placeholder_instruction);
    *engine_p++ = I_OR(REG_K0, REG_K0, REG_K1);
    *engine_p++ = I_LUI(REG_K1, A_BASE(ori_placeholder_address));
    *engine_p++ = I_SW(REG_K0, A_OFFSET(ori_placeholder_address), REG_K1);
    *engine_p++ = I_CACHE(HIT_WRITE_BACK_D, A_OFFSET(ori_placeholder_address), REG_K1);
    *engine_p++ = I_CACHE(HIT_INVALIDATE_I, A_OFFSET(ori_placeholder_address), REG_K1);
#else
    *engine_p++ = I_MFC0(REG_K1, C0_REG_EPC);
#if SC64SS_BISECT == 6 || SC64SS_BISECT == 7
    // bisect 6: the base register is not read from the instruction at EPC (a TLB-mapped
    // kuseg address in Perfect Dark) but hardcoded to t7, the register PD uses;
    // bisect 7: the same, and the watchpoint is disarmed once the relocation is done
    *engine_p++ = I_LUI(REG_K1, 0x000F);
    *engine_p++ = (SC64SS_BISECT == 7) ? I_MTC0(REG_ZERO, C0_REG_WATCH_LO) : I_NOP();
    *engine_p++ = I_NOP();
    *engine_p++ = I_NOP();
#else
    *engine_p++ = I_LW(REG_K1, 0, REG_K1);
    *engine_p++ = I_LUI(REG_K0, 0x03E0);
    *engine_p++ = I_AND(REG_K1, REG_K0, REG_K1);
    *engine_p++ = I_SRL(REG_K1, REG_K1, 5);
#endif

    // Update create final instruction and update its target register number
    *engine_p++ = I_LUI(REG_K0, ori_placeholder_instruction >> 16);
    *engine_p++ = I_ORI(REG_K0, REG_K0, ori_placeholder_instruction);
    *engine_p++ = I_OR(REG_K0, REG_K0, REG_K1);

    // Write created instruction into placeholder
    *engine_p++ = I_LUI(REG_K1, A_BASE(ori_placeholder_address));
    *engine_p++ = I_SW(REG_K0, A_OFFSET(ori_placeholder_address), REG_K1);

    // Force write and instruction cache invalidation
    *engine_p++ = I_CACHE(HIT_WRITE_BACK_D, A_OFFSET(ori_placeholder_address), REG_K1);
    *engine_p++ = I_CACHE(HIT_INVALIDATE_I, A_OFFSET(ori_placeholder_address), REG_K1);
#endif

    // Load address base and execute created instruction
    *engine_p++ = I_LUI(REG_K0, A_BASE(RELOCATED_EXCEPTION_HANDLER_ADDRESS));
    *engine_p++ = (SC64SS_BISECT == 11 || SC64SS_BISECT == 12) ? I_ORI(REG_T7, REG_K0, A_OFFSET(RELOCATED_EXCEPTION_HANDLER_ADDRESS)) : I_NOP();

    // Return from the exception
    *engine_p++ = I_ERET();

    // SC64SS: the engine (and the save-state hook) require the Expansion Pak,
    // and the boot path that keeps RDRAM alive for them makes IPL3 skip its
    // memory sizing, which leaves osMemSize at 0 (measured). Games that check
    // it - every Expansion Pak title - would refuse to start. Write the real
    // size to both locations (0x318 for most IPL3s, 0x3F0 for the x105 one).
    // An EE code in the list below still overrides this with 4 MiB.
    *patcher_p++ = I_LUI(REG_K0, 0xA000);
    *patcher_p++ = I_LUI(REG_K1, 0x0080);
    *patcher_p++ = I_SW(REG_K1, 0x318, REG_K0);
    *patcher_p++ = I_SW(REG_K1, 0x3F0, REG_K0);

    cheat_entry_t cheat;

    while (cheats_get_next(&cheat_list, &cheat)) {
        cheat_t *c = &cheat.main;

        switch (c->type) {
            case SPECIAL_WRITE_BYTE_ON_BOOT:
            case SPECIAL_WRITE_SHORT_ON_BOOT: {
                *patcher_p++ = I_LUI(REG_K0, A_BASE(c->address));
                *patcher_p++ = I_ORI(REG_K1, REG_ZERO, c->value);
                *patcher_p++ = IS_WIDTH_16(c->type) ? I_SH(REG_K1, A_OFFSET(c->address), REG_K0)
                                                    : I_SB(REG_K1, A_OFFSET(c->address), REG_K0);
                break;
            }
            case SPECIAL_CLEAR_MEMORY: {
                *patcher_p++ = I_LUI(REG_K0, 0xA000);
                *patcher_p++ = I_ORI(REG_K1, REG_K0, (0x300 - 0x200) - 4);
                *patcher_p++ = I_SW(REG_ZERO, 0x0200, REG_K0);
                *patcher_p++ = I_BNE(REG_K0, REG_K1, -2); // could be BNEL
                *patcher_p++ = I_ADDIU(REG_K0, REG_K0, 4);
                break;
            }
            // N/A
            case SPECIAL_SECONDARY_EXCEPTION_HANDLER:
            // not needed with N64FlashcartMenu's boot method
            case SPECIAL_SET_ENTRYPOINT_ADDR:
            // already handled
            case SPECIAL_SET_STORE_LOCATION: {
                // do nothing
                break;
            }
            case SPECIAL_DISABLE_EXPANSION_PAK: {
                *patcher_p++ = I_LUI(REG_K0, 0xA000);
                *patcher_p++ = I_LUI(REG_K1, 0x0040);
                *patcher_p++ = I_SW(REG_K1, 0x318, REG_K0);
                *patcher_p++ = I_SW(REG_K1, 0x3F0, REG_K0);
                break;
            }
            default: {
                if (IS_TYPE_REPEATER(c->type)) {
                    if ((!IS_TYPE_WRITE(cheat.sub.type)) || IS_CONDITION_GS_BUTTON(cheat.sub.type)) {
                        continue;
                    }

                    int count = ((c->address >> 8) & 0xFF);
                    int step = (c->address & 0xFF);
                    int16_t increment = (int16_t)(c->value);

                    c = &cheat.sub;

                    for (int i = 0; i < count; i++) {
                        *engine_p++ = I_LUI(REG_K0, A_BASE(c->address));
                        *engine_p++ = I_ORI(REG_K1, REG_ZERO, c->value);
                        *engine_p++ = IS_WIDTH_16(c->type) ? I_SH(REG_K1, A_OFFSET(c->address), REG_K0)
                                                        : I_SB(REG_K1, A_OFFSET(c->address), REG_K0);

                        c->address += step;
                        c->value += increment;
                    }

                    continue;
                }

                if (IS_TYPE_CONDITIONAL(c->type)) {
                    if ((!IS_TYPE_WRITE(cheat.sub.type)) || IS_CONDITION_GS_BUTTON(cheat.sub.type)) {
                        continue;
                    }

                    *engine_p++ = I_LUI(REG_K0, A_BASE(c->address));
                    *engine_p++ = IS_WIDTH_16(c->type) ? I_LHU(REG_K0, A_OFFSET(c->address), REG_K0)
                                                    : I_LBU(REG_K0, A_OFFSET(c->address), REG_K0);
                    *engine_p++ = I_ORI(REG_K1, REG_ZERO, c->value & (IS_WIDTH_16(c->type) ? 0xFFFF : 0xFF));
                    *engine_p++ = IS_CONDITION_NOT_EQUAL(c->type) ? I_BEQ(REG_K0, REG_K1, 3) : I_BNE(REG_K0, REG_K1, 3);

                    c = &cheat.sub;
                }

                if (IS_TYPE_WRITE(c->type)) {
                    if (IS_CONDITION_GS_BUTTON(c->type)) {
                        continue;
                    }

                    *engine_p++ = I_LUI(REG_K0, A_BASE(c->address));
                    *engine_p++ = I_ORI(REG_K1, REG_ZERO, c->value);
                    *engine_p++ = IS_WIDTH_16(c->type) ? I_SH(REG_K1, A_OFFSET(c->address), REG_K0)
                                                    : I_SB(REG_K1, A_OFFSET(c->address), REG_K0);

                    continue;
                }
            }
        }
    }

    // SC64SS: place the USB hook blob at its linked base and route the
    // engine tail through it; the hook ends with j 0x80000120 itself.
    uint32_t hook_address = 0;
    if (!hook_borrowed && (hook_blob != NULL) && (hook_size > 0) && (hook_size <= SC64SS_HOOK_STAGING_LEN)) {
        hook_address = 0x807D0000;
        io32_t *hook_dst = (io32_t *) (hook_address);
        uint32_t hook_words = ((hook_size + 3) / 4);
        for (uint32_t i = 0; i < hook_words; i++) {
            hook_dst[i] = hook_blob[i];
        }
        cheats_update_cache(hook_dst, hook_dst + hook_words);
        // SC64SS DIAG: zero the RDRAM diagnostic words at 0x807FFFF0:
        //   +4 engine trace pass counter, +8 hook entry counter, +C hook stage
        io32_t *diag_flags = (io32_t *) (0x807FFFF0);
        diag_flags[0] = 0;
        diag_flags[1] = 0;
        diag_flags[2] = 0;
        diag_flags[3] = 0;
        cheats_update_cache(diag_flags, diag_flags + 4);
    }


    // NOTE: evaluate the target first; the stock I_J macro used to expand its
    // argument unparenthesized, so a ?: expression inside it was mis-shifted
    // (emitted j 0x81F40000 instead of j 0x807D0000).
    uint32_t lowpage = hook_address && ((uint32_t)(final_engine_address) == SC64SS_LOWPAGE_ENGINE_ADDRESS);
    // SC64SS borrowed mode: no resident hook; the engine's tail runs the borrow gate
    // at the reinstall stub's address, the other fragments go to their vector-page
    // homes, and the hook stays in its staging copy for the cart monitor to pull in.
    uint32_t borrow = hook_borrowed && (hook_blob != NULL) && ((uint32_t)(final_engine_address) == SC64SS_LOWPAGE_ENGINE_ADDRESS);
    uint32_t tail_target = (borrow && (SC64SS_BISECT == 1 || SC64SS_BISECT == 2 || SC64SS_BISECT == 12)) ? RELOCATED_EXCEPTION_HANDLER_ADDRESS
                         : (lowpage || borrow) ? SC64SS_REINSTALL_STUB_ADDRESS
                         : (hook_address ? hook_address : RELOCATED_EXCEPTION_HANDLER_ADDRESS);
    if (borrow && (tail_target == SC64SS_REINSTALL_STUB_ADDRESS)) {
        // borrowed mode: through the pre-install check to the installer (k0 = lui of
        // the monitor's kseg1 base, the check adds the low half); j 0x360 / nop once
        // the installer has run
        *engine_p++ = I_J(SC64SS_LOWPAGE_1DC_ADDRESS);
        *engine_p++ = I_LUI(REG_K0, ((0xA0000000UL | SC64SS_MONITOR_PI) >> 16));
    } else {
        *engine_p++ = I_J(tail_target);
        *engine_p++ = I_NOP();
    }
    io32_t *engine_end = engine_p;

    io32_t *borrow_130 = (io32_t *)(ENGINE_TEMPORARY_ADDRESS + 0x800);
    io32_t *borrow_1dc = (io32_t *)(ENGINE_TEMPORARY_ADDRESS + 0x900);
    io32_t *borrow_0f0 = (io32_t *)(ENGINE_TEMPORARY_ADDRESS + 0xB00);
    io32_t *borrow_110 = (io32_t *)(ENGINE_TEMPORARY_ADDRESS + 0xC00);
    if (borrow && (SC64SS_BISECT != 1)) {
        // the gate + the DMA primitive: exactly the reinstall stub's 36 words at 0x360
        for (uint32_t i = 0; i < sc64ss_lowpage_360_words; i++) {
            *engine_p++ = sc64ss_lowpage_360[i];
        }
        while ((engine_p - engine_end) < 36) {
            *engine_p++ = I_NOP();
        }
        // the other fragments wait above the engine's temporary copy for the patcher
        for (uint32_t i = 0; i < sc64ss_lowpage_130_words; i++) {
            borrow_130[i] = sc64ss_lowpage_130[i];
        }
        for (uint32_t i = 0; i < sc64ss_lowpage_1dc_words; i++) {
            borrow_1dc[i] = sc64ss_lowpage_1dc[i];
        }
        for (uint32_t i = 0; i < sc64ss_lowpage_0f0_words; i++) {
            borrow_0f0[i] = sc64ss_lowpage_0f0[i];
        }
        for (uint32_t i = 0; i < sc64ss_lowpage_110_words; i++) {
            borrow_110[i] = sc64ss_lowpage_110[i];
        }
        cheats_update_cache(borrow_130, borrow_130 + sc64ss_lowpage_130_words);
        cheats_update_cache(borrow_1dc, borrow_1dc + sc64ss_lowpage_1dc_words);
        cheats_update_cache(borrow_0f0, borrow_0f0 + sc64ss_lowpage_0f0_words);
        cheats_update_cache(borrow_110, borrow_110 + sc64ss_lowpage_110_words);
    } else if (lowpage) {
        // SC64SS reinstall stub (34 words, k0/k1 only, runs on every exception
        // after the engine): hook present -> jump to it; a PI transfer busy or
        // a PI interrupt pending -> leave the game alone this time; otherwise
        // flush every cache line (the sweep's dirty zero lines must not land
        // on the fresh copy), DMA the staged blob back, clear the interrupt
        // that DMA raised, and enter the hook.
        uint32_t magic = hook_blob[0];
        uint32_t dma_len = ((hook_size + 15u) & ~15u) - 1u;
        *engine_p++ = I_LUI(REG_K0, A_BASE(hook_address));                 //  0
        *engine_p++ = I_LW(REG_K0, A_OFFSET(hook_address), REG_K0);        //  1
        *engine_p++ = I_LUI(REG_K1, magic >> 16);                          //  2
        *engine_p++ = I_ORI(REG_K1, REG_K1, magic & 0xFFFF);               //  3
        *engine_p++ = I_BEQ(REG_K0, REG_K1, 25);                           //  4 -> HOOK (30)
        *engine_p++ = I_LUI(REG_K0, 0xA460);                               //  5 (delay slot)
        *engine_p++ = I_LW(REG_K1, 0x0010, REG_K0);                        //  6 PI_STATUS
        *engine_p++ = I_ANDI(REG_K1, REG_K1, 0x000B);                      //  7 dma/io busy | intr
        *engine_p++ = I_BNE(REG_K1, REG_ZERO, 23);                         //  8 -> GAME (32)
        *engine_p++ = I_LUI(REG_K0, 0x8000);                               //  9 (delay slot)
        *engine_p++ = I_ORI(REG_K1, REG_K0, 0x4000);                       // 10 end: 16 KiB of index ops
        *engine_p++ = I_CACHE(0x01, 0, REG_K0);                            // 11 L1: Index_Writeback_Invalidate_D
        *engine_p++ = I_CACHE(0x00, 0, REG_K0);                            // 12     Index_Invalidate_I
        *engine_p++ = I_ADDIU(REG_K0, REG_K0, 16);                         // 13
        *engine_p++ = I_BNE(REG_K0, REG_K1, -4);                           // 14 -> L1
        *engine_p++ = I_NOP();                                             // 15
        *engine_p++ = I_LUI(REG_K0, 0xA460);                               // 16
        *engine_p++ = I_LUI(REG_K1, (hook_address & 0x1FFFFFFF) >> 16);    // 17
        *engine_p++ = I_SW(REG_K1, 0x0000, REG_K0);                        // 18 PI_DRAM_ADDR
        *engine_p++ = I_LUI(REG_K1, SC64SS_HOOK_STAGING_PI_ADDRESS >> 16);  // 19
        *engine_p++ = I_SW(REG_K1, 0x0004, REG_K0);                        // 20 PI_CART_ADDR
        *engine_p++ = I_LUI(REG_K1, dma_len >> 16);                        // 21 (the blob may exceed 64 KiB)
        *engine_p++ = I_ORI(REG_K1, REG_K1, dma_len & 0xFFFF);             // 22
        *engine_p++ = I_SW(REG_K1, 0x000C, REG_K0);                        // 22 PI_WR_LEN: cart -> RDRAM
        *engine_p++ = I_LW(REG_K1, 0x0010, REG_K0);                        // 23 L2: PI_STATUS
        *engine_p++ = I_ANDI(REG_K1, REG_K1, 0x0001);                      // 24
        *engine_p++ = I_BNE(REG_K1, REG_ZERO, -3);                         // 25 -> L2
        *engine_p++ = I_NOP();                                             // 26
        *engine_p++ = I_ORI(REG_K1, REG_ZERO, 0x0002);                     // 27
        *engine_p++ = I_SW(REG_K1, 0x0010, REG_K0);                        // 28 clear our PI interrupt
        *engine_p++ = I_J(hook_address);                                   // 29 HOOK
        *engine_p++ = I_NOP();                                             // 30
        *engine_p++ = I_J(RELOCATED_EXCEPTION_HANDLER_ADDRESS);            // 31 GAME
        *engine_p++ = I_NOP();                                             // 32
    }

    uint32_t j_engine_from_handler = I_J((uint32_t)(final_engine_address));

    if (SC64SS_BISECT != 3) {   // bisect 3: no engine, no vector patch, no watchpoint
    // Copy engine to the final location
    *patcher_p++ = I_LUI(REG_T3, A_BASE((uint32_t)(engine_start)));
    *patcher_p++ = I_ADDIU(REG_T3, REG_T3, A_OFFSET((uint32_t)(engine_start)));

    *patcher_p++ = I_LUI(REG_T4, A_BASE((uint32_t)(engine_end)));
    *patcher_p++ = I_ADDIU(REG_T4, REG_T4, A_OFFSET((uint32_t)(engine_end)));

    *patcher_p++ = I_LUI(REG_T5, A_BASE((uint32_t)(final_engine_address)));
    *patcher_p++ = I_ADDIU(REG_T5, REG_T5, A_OFFSET((uint32_t)(final_engine_address)));

    *patcher_p++ = I_ORI(REG_T6, REG_ZERO, 0);

    *patcher_p++ = I_LW(REG_K1, 0, REG_T3);
    *patcher_p++ = I_SW(REG_K1, 0, REG_T5);
    *patcher_p++ = I_ADDIU(REG_T3, REG_T3, 4);
    *patcher_p++ = I_ADDIU(REG_T5, REG_T5, 4);
    *patcher_p++ = I_BNE(REG_T3, REG_T4, -5);
    *patcher_p++ = I_ADDIU(REG_T6, REG_T6, 4);

    // Force write and invalidate instruction cache
    *patcher_p++ = I_LUI(REG_T5, A_BASE((uint32_t)(final_engine_address)));
    *patcher_p++ = I_ADDIU(REG_T5, REG_T5, A_OFFSET((uint32_t)(final_engine_address)));

    *patcher_p++ = I_CACHE(HIT_WRITE_BACK_D, 0, REG_T5);
    *patcher_p++ = I_CACHE(HIT_INVALIDATE_I, 0, REG_T5);
    *patcher_p++ = I_ADDIU(REG_T6, REG_T6, -D_CACHE_LINE_SIZE);
    *patcher_p++ = I_BGTZ(REG_T6, -4);
    *patcher_p++ = I_ADDIU(REG_T5, REG_T5, D_CACHE_LINE_SIZE);

    if (lowpage || borrow) {
        // SC64SS: copy the reinstall stub or the borrow gate (temporary copy after the engine) to its home
        *patcher_p++ = I_LUI(REG_T3, A_BASE((uint32_t)(engine_end)));
        *patcher_p++ = I_ADDIU(REG_T3, REG_T3, A_OFFSET((uint32_t)(engine_end)));
        *patcher_p++ = I_LUI(REG_T4, A_BASE((uint32_t)(engine_p)));
        *patcher_p++ = I_ADDIU(REG_T4, REG_T4, A_OFFSET((uint32_t)(engine_p)));
        *patcher_p++ = I_LUI(REG_T5, A_BASE(SC64SS_REINSTALL_STUB_ADDRESS));
        *patcher_p++ = I_ADDIU(REG_T5, REG_T5, A_OFFSET(SC64SS_REINSTALL_STUB_ADDRESS));
        *patcher_p++ = I_ORI(REG_T6, REG_ZERO, 0);
        *patcher_p++ = I_LW(REG_K1, 0, REG_T3);
        *patcher_p++ = I_SW(REG_K1, 0, REG_T5);
        *patcher_p++ = I_ADDIU(REG_T3, REG_T3, 4);
        *patcher_p++ = I_ADDIU(REG_T5, REG_T5, 4);
        *patcher_p++ = I_BNE(REG_T3, REG_T4, -5);
        *patcher_p++ = I_ADDIU(REG_T6, REG_T6, 4);
        *patcher_p++ = I_LUI(REG_T5, A_BASE(SC64SS_REINSTALL_STUB_ADDRESS));
        *patcher_p++ = I_ADDIU(REG_T5, REG_T5, A_OFFSET(SC64SS_REINSTALL_STUB_ADDRESS));
        *patcher_p++ = I_CACHE(HIT_WRITE_BACK_D, 0, REG_T5);
        *patcher_p++ = I_CACHE(HIT_INVALIDATE_I, 0, REG_T5);
        *patcher_p++ = I_ADDIU(REG_T6, REG_T6, -D_CACHE_LINE_SIZE);
        *patcher_p++ = I_BGTZ(REG_T6, -4);
        *patcher_p++ = I_ADDIU(REG_T5, REG_T5, D_CACHE_LINE_SIZE);
    }
    if (borrow && (SC64SS_BISECT != 1)) {
        // SC64SS borrowed mode: the helper, the pre-install check and the eret exit
        // stubs into place (EXIT itself comes with the cart installer: 0x1DC once the
        // gate is in), the monitor's data words (0x190..0x1D8) and the pak stub's deferral
        // flag (0x3F4) zeroed
        patcher_p = cheats_emit_copy(patcher_p, (uint32_t)(borrow_130), SC64SS_LOWPAGE_130_ADDRESS, sc64ss_lowpage_130_words);
        patcher_p = cheats_emit_copy(patcher_p, (uint32_t)(borrow_1dc), SC64SS_LOWPAGE_1DC_ADDRESS, sc64ss_lowpage_1dc_words);
        patcher_p = cheats_emit_copy(patcher_p, (uint32_t)(borrow_0f0), SC64SS_LOWPAGE_0F0_ADDRESS, sc64ss_lowpage_0f0_words);
        patcher_p = cheats_emit_copy(patcher_p, (uint32_t)(borrow_110), SC64SS_LOWPAGE_110_ADDRESS, sc64ss_lowpage_110_words);
        *patcher_p++ = I_LUI(REG_K0, 0x8000);
        for (uint32_t i = 0; i < 18; i++) {
            *patcher_p++ = I_SW(REG_ZERO, 0x190 + 4 * i, REG_K0);
        }
        *patcher_p++ = I_SW(REG_ZERO, 0x3F4, REG_K0);
    }

    if (libdragon) {
        // SC64SS: a libdragon game's exception vectors arrive with its ELF, by DMA, before
        // this runs, so the watchpoint below never sees the store it would redirect. Move
        // the game's vector (`j` to its handler, and the delay slot) to the relocated
        // address now; the engine's jump then takes the vector as for any other game.
        *patcher_p++ = I_LUI(REG_K0, 0xA000);
        *patcher_p++ = I_LW(REG_K1, A_OFFSET(EXCEPTION_HANDLER_ADDRESS), REG_K0);
        *patcher_p++ = I_SW(REG_K1, A_OFFSET(RELOCATED_EXCEPTION_HANDLER_ADDRESS), REG_K0);
        *patcher_p++ = I_LW(REG_K1, A_OFFSET(EXCEPTION_HANDLER_ADDRESS) + 4, REG_K0);
        *patcher_p++ = I_SW(REG_K1, A_OFFSET(RELOCATED_EXCEPTION_HANDLER_ADDRESS) + 4, REG_K0);
        *patcher_p++ = I_LUI(REG_K0, A_BASE(RELOCATED_EXCEPTION_HANDLER_ADDRESS));
        *patcher_p++ = I_CACHE(HIT_INVALIDATE_I, A_OFFSET(RELOCATED_EXCEPTION_HANDLER_ADDRESS), REG_K0);
    }

    // Write jump instruction to the exception handler
    *patcher_p++ = I_LUI(REG_K0, A_BASE(EXCEPTION_HANDLER_ADDRESS));
    *patcher_p++ = I_ADDIU(REG_K0, REG_K0, A_OFFSET(EXCEPTION_HANDLER_ADDRESS));

    *patcher_p++ = I_LUI(REG_K1, j_engine_from_handler >> 16);
    *patcher_p++ = I_ORI(REG_K1, REG_K1, j_engine_from_handler);
    *patcher_p++ = I_SW(REG_K1, 0, REG_K0);
    *patcher_p++ = I_SW(REG_ZERO, 4, REG_K0);

    *patcher_p++ = I_CACHE(HIT_WRITE_BACK_D, 0, REG_K0);
    *patcher_p++ = I_CACHE(HIT_INVALIDATE_I, 0, REG_K0);

    // Set watch exception on address 0x80000180
    // SC64SS: reads of the vector too. Indiana Jones and the Infernal
    // Machine copies the four words at 0x180 before it installs its own handler
    // there and chains to that copy from it; with the engine's jump in the vector
    // the copy chained straight back into the engine and every interrupt looped
    // gate -> game front-end -> gate, unacknowledged (a black screen). The watch
    // handler below treats a load like a store: the base register becomes
    // 0x80000120, so the game copies the words it wrote there itself.
    // Not for every game: Banjo-Kazooie reads the vector at boot in a form that
    // redirection breaks (a black screen before the first interrupt, in either
    // placement; it booted before the read watch existed), so the menu passes
    // watch_reads = false for the titles on its list and for watch_reads=0 in a
    // ROM's ini: the watchpoint then covers writes only, as in the first release.
    // SC64SS: not for a libdragon game. Its vectors came with its ELF and are never
    // rewritten, so the watchpoint has nothing to catch, and an access to the vector
    // words from inside an exception (deferred by the CPU until the game resumes) made
    // the relocation above rewrite a register of the game's next instruction instead:
    // a Watch exception reported at the return from enable_interrupts, every time.
    if ((SC64SS_BISECT != 4) && !libdragon) {   // bisect 4: no watchpoint (the game's own vector write goes through)
    *patcher_p++ = I_ORI(REG_K1, REG_ZERO, EXCEPTION_HANDLER_ADDRESS | WATCHLO_W | ((SC64SS_WATCH_READS && watch_reads) ? WATCHLO_R : 0));
    *patcher_p++ = I_MTC0(REG_K1, C0_REG_WATCH_LO);
    *patcher_p++ = I_MTC0(REG_ZERO, C0_REG_WATCH_HI);
    }
    }   // bisect 3

    // SC64SS: unlock the cart, leave breadcrumbs, then install the hook from
    // its staging copy in cart SDRAM (PI SC64SS_HOOK_STAGING_PI) with explicit cache
    // flushes - after IPL3, exactly like the engine. Cached CPU stores made
    // before IPL3 do not reliably reach RDRAM here.
    #define DIAG_PI_WAIT() do { \
        *patcher_p++ = I_LUI(REG_T5, 0xA460); \
        *patcher_p++ = I_LW(REG_T5, 0x0010, REG_T5); \
        *patcher_p++ = I_ANDI(REG_T5, REG_T5, 0x0003); \
        *patcher_p++ = I_BNE(REG_T5, REG_ZERO, -4); \
        *patcher_p++ = I_NOP(); \
    } while (0)
    *patcher_p++ = I_LUI(REG_T3, 0xBFFF);
    DIAG_PI_WAIT();
    *patcher_p++ = I_SW(REG_ZERO, 0x0010, REG_T3);
    *patcher_p++ = I_LUI(REG_T4, 0x5F55);
    *patcher_p++ = I_ORI(REG_T4, REG_T4, 0x4E4C);
    DIAG_PI_WAIT();
    *patcher_p++ = I_SW(REG_T4, 0x0010, REG_T3);
    *patcher_p++ = I_LUI(REG_T4, 0x4F43);
    *patcher_p++ = I_ORI(REG_T4, REG_T4, 0x4B5F);
    DIAG_PI_WAIT();
    *patcher_p++ = I_SW(REG_T4, 0x0010, REG_T3);
    // PI RULE (measured on hardware): a cart PIO read issued while a
    // cart PIO write is still in flight returns the WRITE's data (bus latch)
    // and sets PI_STATUS.error. The copy loop below must not start until the
    // KEY write has completed: settle ~8.5 us, then wait for PI idle.
    *patcher_p++ = I_MFC0(REG_T5, C0_REG_COUNT);
    *patcher_p++ = I_MFC0(REG_T4, C0_REG_COUNT);
    *patcher_p++ = I_SUBU(REG_T4, REG_T4, REG_T5);
    *patcher_p++ = I_SLTIU(REG_T4, REG_T4, 400);
    *patcher_p++ = I_BNE(REG_T4, REG_ZERO, -4);
    *patcher_p++ = I_NOP();
    DIAG_PI_WAIT();
    // SC64SS: boot patches from the menu (the WatchLo write that would disarm
    // the engine's watchpoint): uncached word stores after IPL3 has loaded the
    // game, plus an I-cache invalidate of the line so no stale copy runs.
    for (uint32_t i = 0; (boot_patches != NULL) && (i < boot_patch_count); i++) {
        uint32_t addr = boot_patches[2 * i] | 0x80000000u;
        uint32_t val = boot_patches[2 * i + 1];
        *patcher_p++ = I_LUI(REG_K0, A_BASE(addr | 0x20000000u));
        *patcher_p++ = I_LUI(REG_K1, val >> 16);
        *patcher_p++ = I_ORI(REG_K1, REG_K1, val & 0xFFFFu);
        *patcher_p++ = I_SW(REG_K1, A_OFFSET(addr), REG_K0);
        *patcher_p++ = I_LUI(REG_K0, A_BASE(addr));
        *patcher_p++ = I_CACHE(HIT_INVALIDATE_I, A_OFFSET(addr), REG_K0);
    }
    if (hook_address) {
        // copy: cart SDRAM (PIO reads, synchronous) -> RDRAM (cached stores)
        *patcher_p++ = I_LUI(REG_T3, (0xA0000000 | SC64SS_HOOK_STAGING_PI_ADDRESS) >> 16);   // KSEG1 view of the staging copy
        *patcher_p++ = I_LUI(REG_T5, 0x807D);
        *patcher_p++ = I_LUI(REG_T4, ((hook_size + 3) & ~3u) >> 16);
        *patcher_p++ = I_ORI(REG_T4, REG_T4, ((hook_size + 3) & ~3u) & 0xFFFF);
        *patcher_p++ = I_LW(REG_K1, 0, REG_T3);
        *patcher_p++ = I_SW(REG_K1, 0, REG_T5);
        *patcher_p++ = I_ADDIU(REG_T3, REG_T3, 4);
        *patcher_p++ = I_ADDIU(REG_T5, REG_T5, 4);
        *patcher_p++ = I_ADDIU(REG_T4, REG_T4, -4);
        *patcher_p++ = I_BGTZ(REG_T4, -6);
        *patcher_p++ = I_NOP();
        // flush: write back D-cache and invalidate I-cache over the hook
        *patcher_p++ = I_LUI(REG_T5, 0x807D);
        *patcher_p++ = I_LUI(REG_T6, ((hook_size + 15) & ~15u) >> 16);
        *patcher_p++ = I_ORI(REG_T6, REG_T6, ((hook_size + 15) & ~15u) & 0xFFFF);
        *patcher_p++ = I_CACHE(HIT_WRITE_BACK_D, 0, REG_T5);
        *patcher_p++ = I_CACHE(HIT_INVALIDATE_I, 0, REG_T5);
        *patcher_p++ = I_ADDIU(REG_T6, REG_T6, -D_CACHE_LINE_SIZE);
        *patcher_p++ = I_BGTZ(REG_T6, -4);
        *patcher_p++ = I_ADDIU(REG_T5, REG_T5, D_CACHE_LINE_SIZE);
    }
    DIAG_PI_WAIT();
    #undef DIAG_PI_WAIT

    if (libdragon && !hook_borrowed) {
        // SC64SS: a libdragon game takes its memory size from the boot flags in DMEM (a
        // retail game reads the word at 0x318) and puts its stack at the top of it. With
        // the hook resident there, the game gets 256 KiB less, its stack moved down with
        // it: still an Expansion Pak by libdragon's own test.
        *patcher_p++ = I_LUI(REG_K0, 0xA400);
        *patcher_p++ = I_LW(REG_K1, 0x0000, REG_K0);
        *patcher_p++ = I_LUI(REG_T3, 0x0004);
        *patcher_p++ = I_SUBU(REG_K1, REG_K1, REG_T3);
        *patcher_p++ = I_SW(REG_K1, 0x0000, REG_K0);
        // and the stack pointer with it: the loader set it to the top of the full size one
        // instruction before this stub took over, and the entry code keeps it (a stack up
        // there sits inside the hook's home and outside the saved image: a load put back
        // registers that pointed into a stack the state never held, and games with a call
        // depth that varies from frame to frame crashed on the first load)
        *patcher_p++ = I_SUBU(REG_SP, REG_SP, REG_T3);
    }

    // Jump back to the game code
    *patcher_p++ = I_JR(REG_T1);
    *patcher_p++ = I_NOP();

    cheats_update_cache(engine_start, engine_p);
    cheats_update_cache(patcher_start, patcher_p);

    return true;
}

// SC64SS: the stub a libdragon ROM's third boot stage jumps to, on the cart (cheats.h).
// It runs from the cart, so it only reads the cart: the patcher's 4 KiB and the engine's
// temporary copy go back to their RAM addresses with uncached stores (the boot code
// emptied both caches), then the entrypoint moves to t1 and the patcher runs as after a
// retail IPL3.
uint32_t cheats_libdragon_stub (uint32_t *out) {
    uint32_t n = 0;
    const uint32_t regions[2][2] = {
        { 0xB0000000UL | (SC64SS_LDBOOT_PI + SC64SS_LDBOOT_PATCHER_OFF), 0xA0000000UL | (PATCHER_ADDRESS & 0x1FFFFFFFUL) },
        { 0xB0000000UL | (SC64SS_LDBOOT_PI + SC64SS_LDBOOT_ENGINE_OFF), 0xA0000000UL | (ENGINE_TEMPORARY_ADDRESS & 0x1FFFFFFFUL) },
    };
    for (int r = 0; r < 2; r++) {
        out[n++] = I_LUI(REG_T3, regions[r][0] >> 16);
        out[n++] = I_ORI(REG_T3, REG_T3, regions[r][0] & 0xFFFF);
        out[n++] = I_LUI(REG_T5, regions[r][1] >> 16);
        out[n++] = I_ORI(REG_T5, REG_T5, regions[r][1] & 0xFFFF);
        out[n++] = I_ORI(REG_T4, REG_ZERO, SC64SS_LDBOOT_REGION_WORDS);
        out[n++] = I_LW(REG_K1, 0, REG_T3);
        out[n++] = I_SW(REG_K1, 0, REG_T5);
        out[n++] = I_ADDIU(REG_T3, REG_T3, 4);
        out[n++] = I_ADDIU(REG_T5, REG_T5, 4);
        out[n++] = I_ADDIU(REG_T4, REG_T4, -1);
        out[n++] = I_BGTZ(REG_T4, -6);
        out[n++] = I_NOP();
    }
    out[n++] = I_OR(REG_T1, REG_A0, REG_ZERO);
    out[n++] = I_LUI(REG_K0, A_BASE(PATCHER_ADDRESS));
    out[n++] = I_ADDIU(REG_K0, REG_K0, A_OFFSET(PATCHER_ADDRESS));
    out[n++] = I_JR(REG_K0);
    out[n++] = I_NOP();
    return n;
}
