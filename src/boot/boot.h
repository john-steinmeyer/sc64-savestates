/**
 * @file boot.h
 * @brief Flashcart Boot Subsystem
 * @ingroup boot
 */

#ifndef BOOT_H__
#define BOOT_H__

#include <stdbool.h>
#include <stdint.h>

/** @brief Boot device type enumeration */
typedef enum {
    BOOT_DEVICE_TYPE_ROM = 0, /**< Boot from ROM */
    BOOT_DEVICE_TYPE_64DD = 1, /**< Boot from 64DD */
} boot_device_type_t;

/** @brief Reset type enumeration */
typedef enum {
    BOOT_RESET_TYPE_COLD = 0, /**< Cold reset */
    BOOT_RESET_TYPE_NMI = 1, /**< Non-maskable interrupt reset */
} boot_reset_type_t;

/** @brief TV type enumeration */
typedef enum {
    BOOT_TV_TYPE_PAL = 0, /**< PAL TV type */
    BOOT_TV_TYPE_NTSC = 1, /**< NTSC TV type */
    BOOT_TV_TYPE_MPAL = 2, /**< MPAL TV type */
    BOOT_TV_TYPE_PASSTHROUGH = 3, /**< Passthrough TV type */
} boot_tv_type_t;

/** @brief Boot Parameters Structure */
typedef struct {
    boot_device_type_t device_type; /**< Type of boot device */
    boot_tv_type_t tv_type; /**< TV type */
    uint8_t cic_seed; /**< CIC seed */
    bool detect_cic_seed; /**< Flag to detect CIC seed */
    uint32_t *cheat_list; /**< Pointer to the cheat list */
    const uint32_t *hook_blob; /**< SC64SS USB hook blob (word array) or NULL */
    uint32_t hook_size; /**< SC64SS USB hook blob size in bytes */
    bool clear_rdram; /**< Zero RDRAM before handing off to IPL3 */
    const uint32_t *boot_patches; /**< SC64SS: (RAM address, word) pairs the boot patcher stores after IPL3, or NULL */
    uint32_t boot_patch_count; /**< SC64SS: number of pairs */
    bool hook_borrowed; /**< SC64SS: borrowed-RAM mode: no resident hook; the vector-page gate and the cart monitor borrow its home per action */
    bool watch_reads; /**< SC64SS: the engine's watchpoint fires on reads of the vector at 0x180 too (false for titles whose own reads must not be redirected) */
} boot_params_t;

/**
 * @brief Boot the system with the specified parameters.
 * 
 * @param params Pointer to the boot parameters structure.
 */
void boot (boot_params_t *params);

#endif /* BOOT_H__ */
