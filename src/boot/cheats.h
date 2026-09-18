/**
 * @file cheats.h
 * @brief Header file for cheat installation functions.
 * @ingroup boot
 */

#ifndef CHEATS_H__
#define CHEATS_H__

#include <stdint.h>
#include "cic.h"

/**
 * @brief Installs cheats based on the CIC type.
 *
 * This function installs the cheats provided in the cheat list based on the
 * specified CIC type.
 *
 * @param cic_type The type of CIC (Copy Protection Chip) used.
 * @param cheat_list A pointer to an array of cheats to be installed.
 * @return true if the cheats were successfully installed, false otherwise.
 */
bool cheats_install(cic_type_t cic_type, uint32_t *cheat_list, const uint32_t *hook_blob, uint32_t hook_size, const uint32_t *boot_patches, uint32_t boot_patch_count, bool hook_borrowed, bool watch_reads, bool libdragon);

// SC64SS: a ROM booted by libdragon's IPL3 (every libdragon ROM ships its own copy of that
// open-source boot code). It clears all of RAM and loads the game's ELF before it hands
// over, so nothing the menu parks in RAM survives it. Instead the hand-off in its third
// stage, which runs from the cart, is pointed at a stub on the cart: the stub copies the
// patcher and the engine's temporary copy back into RAM and enters the patcher with the
// entrypoint in t1, as a retail IPL3 does. The cart region, above the hook staging area:
//   +0x0000 the stub (read-only code: a PIO write from code running on the cart would
//           corrupt its next instruction fetch)
//   +0x1000 the patcher's 4 KiB (RAM SC64SS_LDBOOT_PATCHER_RAM)
//   +0x2000 the engine's temporary copy and the vector-page fragments (SC64SS_LDBOOT_ENGINE_RAM)
#define SC64SS_LDBOOT_PI            (0x13FA0000UL)
#define SC64SS_LDBOOT_PATCHER_OFF   (0x1000UL)
#define SC64SS_LDBOOT_ENGINE_OFF    (0x2000UL)
#define SC64SS_LDBOOT_REGION_WORDS  (1024UL)
#define SC64SS_LDBOOT_PATCHER_RAM   (0x80700000UL)
#define SC64SS_LDBOOT_ENGINE_RAM    (0x80710000UL)

// Emit the stub into `out` (32 words is enough); returns the word count.
uint32_t cheats_libdragon_stub(uint32_t *out);

#endif // CHEATS_H__
