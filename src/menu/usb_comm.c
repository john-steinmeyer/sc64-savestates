/**
 * @file usb_comm.c
 * @brief USB communication component implementation
 * @ingroup ui_components
 */

// NOTE: This code doesn't implement EverDrive-64 USB protocol.
//       Main use of these functions is to aid menu development
//       (for example replace files on the SD card or reboot menu).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <usb.h>

#include "sound.h"
#include "usb_comm.h"
#include "utils/fs.h"
#include "utils/utils.h"
#include "../boot/hook_blob.h"
#if SC64SS_HOOK_DEV
#include <fatfs/ff.h>
#include <libdragon.h>
#include <errno.h>
#endif

#define MAX_FILE_SIZE   MiB(4)

/** @brief The supported USB commands structure. */
typedef struct {
    /** @brief The command identifier. */
    const char *id;

    /** @brief The command operation. */
    void (*op) (menu_t *menu);
} usb_comm_command_t;

/**
 * @brief Get a character from the USB input.
 * 
 * @return int The character read, or -1 if no character is available.
 */
static int usb_comm_get_char (void) {
    char c;

    if (USBHEADER_GETSIZE(usb_poll()) <= 0) {
        return -1;
    }

    usb_read(&c, sizeof(c));

    return (int) (c);
}

/**
 * @brief Read a string from the USB input.
 * 
 * @param string Buffer to store the string.
 * @param length Maximum length of the string.
 * @param end Character indicating the end of the string.
 * @return true if the string was read successfully, false otherwise.
 */
static bool usb_comm_read_string (char *string, int length, char end) {
    for (int i = 0; i < length; i++) {
        int c = usb_comm_get_char();

        if (c < 0) {
            return true;
        }

        string[i] = (char) (c);

        if (c == '\0' || c == end) {
            string[i] = '\0';
            break;
        }

        if (i == (length - 1)) {
            return true;
        }
    }

    return false;
}

/**
 * @brief Send an error message over USB.
 * 
 * @param message The error message.
 */
static void usb_comm_send_error (const char *message) {
    usb_purge();
    usb_write(DATATYPE_TEXT, message, strlen(message));
}

/**
 * @brief Reboot the system.
 * 
 * @param menu Pointer to the menu structure.
 */
static void command_reboot (menu_t *menu) {
    // The uploader overwrites SDRAM (including DFS) before sending this command;
    // stop audio immediately so the mixer stops reading from overwritten ROM data.
    sound_deinit();
    menu->next_mode = MENU_MODE_BOOT;

    menu->boot_params->device_type = BOOT_DEVICE_TYPE_ROM;
    menu->boot_params->tv_type = BOOT_TV_TYPE_PASSTHROUGH;
    menu->boot_params->detect_cic_seed = true;
    menu->boot_params->cheat_list = NULL;
    menu->boot_params->clear_rdram = false;
}

/**
 * @brief Receive a file over USB and save it to the storage.
 * 
 * @param menu Pointer to the menu structure.
 */
static void command_receive_file (menu_t *menu) {
    FILE *f;
    char buffer[256];
    uint8_t data[8192];
    char length[8];

    if (usb_comm_read_string(buffer, sizeof(buffer), '@')) {
        return usb_comm_send_error("Invalid path argument\n");
    }

    // NOTE: The path is terminated by '@' (not space) so filenames containing
    //       spaces work; trailing spaces are trimmed to keep senders that emit
    //       "send-file <path> @<len>@<data>" compatible.
    for (size_t n = strlen(buffer); (n > 0) && (buffer[n - 1] == ' '); n--) {
        buffer[n - 1] = '\0';
    }

    if (usb_comm_read_string(length, sizeof(length), '@')) {
        return usb_comm_send_error("Invalid file length argument\n");
    }

#if SC64SS_HOOK_DEV
    // SC64SS: a path with a leading '+' appends to the file, so a development tool can send
    // a file bigger than one payload in pieces (a large homebrew ROM); release builds keep
    // the stock command as it is.
    char *name = buffer;
    const char *mode = "wb";
    if (name[0] == '+') {
        name++;
        mode = "ab";
    }
    path_t *path = path_init(menu->storage_prefix, name);

    if ((f = fopen(path_get(path), mode)) == NULL) {
#else
    path_t *path = path_init(menu->storage_prefix, buffer);

    if ((f = fopen(path_get(path), "wb")) == NULL) {
#endif
        path_free(path);
        return usb_comm_send_error("Couldn't create file\n");
    }
    setbuf(f, NULL);
    path_free(path);

    int remaining = atoi(length);
    int total = remaining;

    if (remaining > MAX_FILE_SIZE) {
        fclose(f);
        return usb_comm_send_error("File size too big\n");
    }

    // SC64SS: the whole payload comes into RAM first and goes to the card in one write.
    // The cartridge drops the rest of a payload when the console takes more than a second
    // between two pieces of it, and a card write can take that long (the first write after
    // a boot took seven seconds for 256 KiB one afternoon); the stock loop, which wrote each
    // 8 KiB piece before asking for the next, then hung in usb_read for data that was gone.
    uint8_t *whole = (remaining > 0) ? malloc(remaining) : NULL;
    if (whole != NULL) {
        for (int offset = 0; offset < total; ) {
            int block_size = MIN(total - offset, (int) sizeof(data));
            usb_read(whole + offset, block_size);
            offset += block_size;
        }
        bool written = (fwrite(whole, 1, total, f) == (size_t) total);
        free(whole);
        if (!written) {
            fclose(f);
            return usb_comm_send_error("Couldn't write all required data to the file\n");
        }
    } else {
        while (remaining > 0) {
            int block_size = MIN(remaining, sizeof(data));
            usb_read(data, block_size);
            if (fwrite(data, 1, block_size, f) != block_size) {
                fclose(f);
                return usb_comm_send_error("Couldn't write all required data to the file\n");
            }
            remaining -= block_size;
        }
    }

    if (fclose(f)) {
        return usb_comm_send_error("Couldn't flush data to the file\n");
    }

    if (usb_comm_get_char() != '\0') {
        return usb_comm_send_error("Invalid token at the end of data stream\n");
    }

    // SC64SS: the sender waits for this line before it does anything else to the console:
    // the file is closed on the card only now, and a power cycle before this point leaves
    // a 0-byte file behind (it happened to the menu file itself).
    char done[48];
    snprintf(done, sizeof(done), "push ok %d\n", total);
    usb_write(DATATYPE_TEXT, done, strlen(done));
}

// SC64SS: driving the menu from a PC over USB.
// "ping" answers "pong": the menu is up and serving USB.
static void command_ping (menu_t *menu) {
    usb_write(DATATYPE_TEXT, "pong\n", 5);
}

// "load-rom <path>@": boot the ROM at that path on the card with the settings saved for
// it (save states, hook placement, cheats), without a press of A, the way the autoload
// feature boots at startup. The path ends at '@' so names with spaces work.
extern void view_load_rom_preset (menu_t *menu, path_t *path);

static void command_load_rom (menu_t *menu) {
    char buffer[256];

    if (usb_comm_read_string(buffer, sizeof(buffer), '@')) {
        return usb_comm_send_error("Invalid path argument\n");
    }
    for (size_t n = strlen(buffer); (n > 0) && (buffer[n - 1] == ' '); n--) {
        buffer[n - 1] = '\0';
    }

    path_t *path = path_init(menu->storage_prefix, buffer);

    if (!file_exists(path_get(path))) {
        path_free(path);
        return usb_comm_send_error("No such file\n");
    }

    view_load_rom_preset(menu, path);
    usb_write(DATATYPE_TEXT, "loading\n", 8);
}

#if SC64SS_HOOK_DEV
// SC64SS development build only: the card's free space, a directory listing and a file
// removal over USB, for the PC-driven test loop. "disk-free" answers "free <clusters>
// of <clusters> x <bytes>"; "list-dir <path>" answers one "d <name>" or "f <size> <name>"
// line per entry and "list end"; "remove-file <path>" answers "removed <path>" or
// "remove failed <errno> <path>".
static void command_disk_free (menu_t *menu) {
    (void) menu;
    FATFS *fs = NULL;
    DWORD nclst = 0;
    char line[96];
    FRESULT r = f_getfree("", &nclst, &fs);
    if ((r != FR_OK) || (fs == NULL)) {
        snprintf(line, sizeof(line), "disk-free failed %d\n", (int) r);
    } else {
        snprintf(line, sizeof(line), "free %lu of %lu x %lu\n", (unsigned long) nclst, (unsigned long) (fs->n_fatent - 2), (unsigned long) fs->csize * 512UL);
    }
    usb_write(DATATYPE_TEXT, line, strlen(line));
}

// "disk-scan": the free cluster count taken from the allocation table itself, not from the
// card's FSInfo hint (which FatFs trusts once it reads as valid, and which a card can carry
// wrong: a half-empty card that said 0 free refused every new file). The count goes back
// to the FSInfo sector with the next sync (the directory made and removed here).
static void command_disk_scan (menu_t *menu) {
    (void) menu;
    FATFS *fs = NULL;
    DWORD nclst = 0;
    char line[96];
    FRESULT r = f_getfree("", &nclst, &fs);
    if ((r == FR_OK) && (fs != NULL)) {
        fs->free_clst = 0xFFFFFFFF;
        r = f_getfree("", &nclst, &fs);
    }
    if (r == FR_OK) {
        f_mkdir("/zz_scan");
        f_unlink("/zz_scan");
    }
    snprintf(line, sizeof(line), "scan %d free %lu\n", (int) r, (unsigned long) nclst);
    usb_write(DATATYPE_TEXT, line, strlen(line));
}

static void command_list_dir (menu_t *menu) {
    char buffer[256], line[300];
    dir_t info;
    if (usb_comm_read_string(buffer, sizeof(buffer), '\0')) {
        return usb_comm_send_error("Invalid path argument\n");
    }
    path_t *path = path_init(menu->storage_prefix, buffer);
    int result = dir_findfirst(path_get(path), &info);
    while (result == 0) {
        if (info.d_type == DT_DIR) {
            snprintf(line, sizeof(line), "d %s\n", info.d_name);
        } else {
            snprintf(line, sizeof(line), "f %lld %s\n", (long long) info.d_size, info.d_name);
        }
        usb_write(DATATYPE_TEXT, line, strlen(line));
        result = dir_findnext(path_get(path), &info);
    }
    path_free(path);
    usb_write(DATATYPE_TEXT, "list end\n", 9);
}

static void command_remove_file (menu_t *menu) {
    char buffer[256], line[300];
    if (usb_comm_read_string(buffer, sizeof(buffer), '\0')) {
        return usb_comm_send_error("Invalid path argument\n");
    }
    path_t *path = path_init(menu->storage_prefix, buffer);
    if (remove(path_get(path)) == 0) {
        snprintf(line, sizeof(line), "removed %s\n", buffer);
    } else {
        snprintf(line, sizeof(line), "remove failed %d %s\n", errno, buffer);
    }
    path_free(path);
    usb_write(DATATYPE_TEXT, line, strlen(line));
}
#endif

static usb_comm_command_t commands[] = {
    { .id = "reboot", .op = command_reboot },
#if SC64SS_HOOK_DEV
    { .id = "disk-free", .op = command_disk_free },
    { .id = "disk-scan", .op = command_disk_scan },
    { .id = "list-dir", .op = command_list_dir },
    { .id = "remove-file", .op = command_remove_file },
#endif
    { .id = "send-file", .op = command_receive_file }, // Note that this is a crossover with the `id` related to the PC commands.
    { .id = "ping", .op = command_ping },
    { .id = "load-rom", .op = command_load_rom },
    { .id = NULL },
};

/**
 * @brief Poll the USB input for commands.
 * 
 * @param menu Pointer to the menu structure.
 */
void usb_comm_poll (menu_t *menu) {
    uint32_t header = usb_poll();

    if (USBHEADER_GETTYPE(header) != DATATYPE_TEXT) {
        usb_purge();
        return;
    }

    if (USBHEADER_GETSIZE(header) > 0) {
        char cmd_id[32];

        if (usb_comm_read_string(cmd_id, sizeof(cmd_id), ' ')) {
            usb_comm_send_error("Command id too long\n");
        } else {
            usb_comm_command_t *cmd = commands;

            while (cmd->id != NULL) {
                if (strcmp(cmd->id, cmd_id) == 0) {
                    cmd->op(menu);
                    break;
                }
                cmd++;
            }

            usb_purge();

            if (cmd->id == NULL) {
                usb_comm_send_error("Unknown command\n");
            }
        }
    }
}
