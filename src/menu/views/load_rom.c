#include "../bookkeeping.h"
// SC64SS diagnostics: Perfect Dark boot probe (set by build_ce.sh from SC64SS_PDPROBE)
#ifndef SC64SS_PDPROBE
#define SC64SS_PDPROBE 0
// SC64SS diagnostics (build_ce.sh): Indiana Jones' vector chain copy patched to read
// the relocated words at 0x120 (a bisect against the engine's read watch)
#endif
#include "../cart_load.h"
#include "../datel_codes.h"
#include "../path.h"
#include "../rom_info.h"
#include "../sound.h"
#include "boot/boot.h"
#include "utils/fs.h"
#include "views.h"
#include "../../boot/hook_blob.h"
#include "../../flashcart/flashcart_utils.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>


// SC64SS: one file per save-state slot on the SD card (sd:/savestates/<checkcode>.stN,
// 8 MiB, allocated once with a fresh-file marker in its first sector; the state's
// header lives there too, format v2). At every launch the file's run table (its
// contiguous sector extents) is written into the slot's last 4 KiB (SC64SS_SD_TABLE_OFF)
// so the hook can mirror saves to it and read them back after a power cycle.
// cfg[16 + i] receives the sectors the table covers (0 = no file).
#define SC64SS_STATE_FILE_SIZE  (8 * 1024 * 1024)
#define SC64SS_STATE_FILE_SIZE_B (SC64SS_STATE_IMAGE_OFF + SC64SS_STATE_IMAGE_LEN_B + 0x2000)   /* borrowed mode: head + all 8 MiB + the RSP's memories (hook v10) */
#define SC64SS_STATE_SECTORS    (SC64SS_SD_FILE_SECTORS)
#define SC64SS_STATE_MARKER     (0x53544652UL)   /* "STFR" */
#define SC64SS_STATE_MAGIC      (0x53543634UL)   /* "ST64" */

// SC64SS: the marker sector (STFR, ROM check code, slot) at the start of a state file.
static void sc64ss_state_file_mark (char *fp, uint32_t crc1, uint32_t crc2, uint32_t slot) {
    FILE *f = fopen(fp, "r+b");
    if (f) {
        static uint32_t marker[128];
        memset(marker, 0, sizeof(marker));
        marker[0] = SC64SS_STATE_MARKER;
        marker[1] = crc1;
        marker[2] = crc2;
        marker[3] = slot;
        if (fseek(f, 0, SEEK_SET) == 0) {
            fwrite(marker, 1, sizeof(marker), f);
        }
        fclose(f);
    }
}

// SC64SS: the file's sectors as a run table for the hook: SDR1, n, total, n x {sector,
// file sector, count}. false when the file is missing or too fragmented (64 runs).
static bool sc64ss_state_file_runs (char *fp, uint32_t *table, uint32_t *total, uint32_t max_sectors) {
    uint32_t *secs = malloc(max_sectors * sizeof(uint32_t));
    uint32_t n = 0;
    bool ok = false;
    *total = 0;
    if (secs == NULL) {
        return false;
    }
    memset(secs, 0, max_sectors * sizeof(uint32_t));
    if (!fatfs_get_file_sectors(fp, secs, ADDRESS_TYPE_MEM, max_sectors)) {
        ok = true;
        for (uint32_t s = 0; s < max_sectors; s++) {
            if (secs[s] == 0) {
                break;
            }
            if ((n > 0) && (secs[s] == table[3 + 3 * (n - 1)] + table[5 + 3 * (n - 1)])) {
                table[5 + 3 * (n - 1)]++;
            } else {
                if (n >= SC64SS_SD_RUNS_MAX) {
                    ok = false;
                    break;
                }
                table[3 + 3 * n] = secs[s];
                table[4 + 3 * n] = s;
                table[5 + 3 * n] = 1;
                n++;
            }
            (*total)++;
        }
    }
    free(secs);
    if (!ok || (n == 0)) {
        return false;
    }
    table[0] = SC64SS_SD_RUNS_MAGIC;
    table[1] = n;
    table[2] = *total;
    return true;
}

// SC64SS: the state slots live in cart SDRAM above the ROM, SC64SS_STATE_SLOT_LEN apart
// (1 MiB aligned after the ROM), below the hook's staging copy; hook_blob.h carries the
// layout the hook was built with. Returns the slot count (0: the ROM leaves no room).
// A libdragon ROM keeps its slots below the last 8 MiB of the cart's memory: libdragon's USB
// logging puts a game's prints there (usb.c, DEBUG_ADDRESS), inside the slot that reaches it.
#define SC64SS_LIBDRAGON_SLOT_TOP 0x13800000UL
static uint32_t sc64ss_slot_table (int64_t rom_size, uint32_t *slots, uint32_t stride, bool libdragon) {
    uint64_t top = libdragon ? SC64SS_LIBDRAGON_SLOT_TOP : SC64SS_FRAME_STASH_PI;
    uint32_t n = 0;
    if (rom_size <= 0) {
        return 0;
    }
    uint64_t base = 0x10000000ULL + (((uint64_t) rom_size + 0xFFFFFULL) & ~0xFFFFFULL);
    for (uint64_t a = base; ((a + stride) <= top) && (n < SC64SS_SLOTS_MAX); a += stride) {   // below the frame stash (hook.c), which sits below the staging
        slots[n++] = (uint32_t) a;
    }
    return n;
}

static void sc64ss_prepare_state_files (menu_t *menu, uint32_t *cfg, uint32_t n, bool borrowed) {
    // one slot layout and one file size for both placements: the Slow motion
    // option (the resident hook) can be switched on and off without losing a state
    (void) borrowed;
    uint32_t file_size = SC64SS_STATE_FILE_SIZE_B;
    uint32_t file_sectors = SC64SS_SD_FILE_SECTORS_B;
    uint32_t table_off = SC64SS_SD_TABLE_OFF_B;
    uint64_t check_code = (uint64_t) menu->load.rom_info.check_code;
    uint32_t crc1 = (uint32_t) (check_code >> 32);
    uint32_t crc2 = (uint32_t) (check_code & 0xFFFFFFFFULL);
    static uint32_t table[4 + 3 * SC64SS_SD_RUNS_MAX] __attribute__((aligned(16)));
    path_t *dir = path_init(menu->storage_prefix, "/savestates");   // the state files at the top of it, the paks in paks/
    if (!directory_exists(path_get(dir))) {
        directory_create(path_get(dir));
    }
    cfg[27] &= ~0xF00UL;               // hook_cfg.spare bits 8..11: the slot (+1) to resume at boot
    for (uint32_t i = 0; i < n; i++) {
        char name[48];
        uint32_t total = 0;
        cfg[16 + i] = 0;
        snprintf(name, sizeof(name), "%08lX%08lX.st%lu", (unsigned long) crc1, (unsigned long) crc2, (unsigned long) i);
        path_t *file = path_clone(dir);
        path_push(file, name);
        char *fp = path_get(file);
        if (file_exists(fp) && (file_get_size(fp) < (int64_t) file_size)) {
            // a file from the first release (16 KiB shorter)
            remove(fp);
            debugf("SC64SS: state file %s was too small, remade\n", fp);
        }
        if (!file_exists(fp)) {
            if (file_allocate(fp, file_size)) {
                debugf("SC64SS: could not allocate %s\n", fp);
                path_free(file);
                continue;
            }
            sc64ss_state_file_mark(fp, crc1, crc2, i);
            debugf("SC64SS: state file %s created\n", fp);
        } else {
            // a file from before format v2 (header at 7.75 MiB) or anything else that
            // does not start with our marker or a state: mark it fresh
            uint32_t head[3] = {0, 0, 0};   // magic, version, flags
            uint32_t w0 = 0;
            FILE *f = fopen(fp, "rb");
            if (f) {
                if (fread(head, 1, sizeof(head), f) == sizeof(head)) {
                    w0 = head[0];
                }
                fclose(f);
            }
            if ((w0 != SC64SS_STATE_MARKER) && (w0 != SC64SS_STATE_MAGIC)) {
                sc64ss_state_file_mark(fp, crc1, crc2, i);
                debugf("SC64SS: state file %s re-marked\n", fp);
            } else if ((w0 == SC64SS_STATE_MAGIC) && (head[2] & 4) && !(cfg[27] & 0xF00UL)) {
                cfg[27] |= (i + 1) << 8;   // a suspended state: the hook loads it once the game is up
                debugf("SC64SS: state file %s resumes at boot\n", fp);
            }
        }
        memset(table, 0, sizeof(table));
        if (sc64ss_state_file_runs(fp, table, &total, file_sectors)) {
            data_cache_hit_writeback(table, sizeof(table));
            dma_write(table, cfg[2 + i] + table_off, sizeof(table));
            cfg[16 + i] = total;
        } else {
            debugf("SC64SS: no run table for %s\n", fp);
        }
        path_free(file);
    }
    path_free(dir);
}

// SC64SS: screenshots. A game appends each one as a PNG to sd:/screenshots/pending.bin, a
// file the menu keeps allocated (the routine in the game can write into a file but not
// create one): its run table on the cart at SC64SS_SHOT_TABLE_PI and its 4 KiB header
// block at SC64SS_SHOT_HDR_PI (magic, the ROM's check code, the count, the capacity, the
// fill point, an entry {first sector, length} per shot, the launching ROM's file name at
// SC64SS_SHOTS_OWNER_OFF), which the routine keeps up to date and copies to the file's
// first sectors after every shot. When the menu starts again, sc64ss_shots_finish copies
// the shots out to sd:/screenshots/<the ROM's file name>/, named after the time stamp the
// routine put in them, and empties the file.
#define SC64SS_SHOTS_DIR      "/screenshots"
#define SC64SS_SHOTS_FILE     "pending.bin"
#define SC64SS_SHOTS_OLD_DIR  "pending"       // (an earlier layout: a few files; cleared away)
#ifndef SC64SS_SHOT_HDR_PI                    // (a hook blob from before screenshots: no file)
#define SC64SS_SHOT_TABLE_PI    (0x13F80000UL)
#define SC64SS_SHOT_HDR_PI      (0x13F81000UL)
#define SC64SS_SHOT_HDR_SECTORS (8UL)
#define SC64SS_SHOT_ENTRIES_MAX (508UL)
#define SC64SS_SD_MAGIC_SHOT    (0x53484354UL)
#endif
#define SC64SS_SHOTS_SIZE_MAX  (64UL * 1024 * 1024)
#define SC64SS_SHOTS_SIZE_MIN  (4UL * 1024 * 1024)
#define SC64SS_SHOTS_OWNER_OFF (2048)

// a file's cluster runs as a run table for the hook (SDR1, n, total, n x {sector, file
// sector, count}), without a list of every sector: a large file fits only when it lies in
// SC64SS_SD_RUNS_MAX pieces or fewer. false when it is missing or more fragmented than that.
static bool sc64ss_file_runs (char *fp, uint32_t *table, uint32_t *total, uint32_t max_sectors) {
    FIL fil;
    uint32_t n = 0;
    bool ok = true;
    *total = 0;
    if (f_open(&fil, strip_fs_prefix(fp), FA_READ) != FR_OK) {
        return false;
    }
    fatfs_fix_file_size(&fil);
    FATFS *fs = fil.obj.fs;
    uint32_t csize = fs->csize;
    uint32_t sectors = (uint32_t) (f_size(&fil) / 512);
    if (sectors > max_sectors) {
        sectors = max_sectors;
    }
    for (uint32_t fsec = 0; fsec < sectors; fsec += csize) {
        if (f_lseek(&fil, ((FSIZE_t) fsec * 512) + 256) != FR_OK) {
            ok = false;
            break;
        }
        uint32_t cluster = fil.clust;
        if ((cluster < 2) || (cluster >= fs->n_fatent)) {
            ok = false;
            break;
        }
        uint32_t sector = (uint32_t) (fs->database + ((LBA_t) csize * (cluster - 2)));
        uint32_t count = (csize < (sectors - fsec)) ? csize : (sectors - fsec);
        if ((n > 0) && (sector == table[3 + 3 * (n - 1)] + table[5 + 3 * (n - 1)])) {
            table[5 + 3 * (n - 1)] += count;
        } else {
            if (n >= SC64SS_SD_RUNS_MAX) {
                ok = false;
                break;
            }
            table[3 + 3 * n] = sector;
            table[4 + 3 * n] = fsec;
            table[5 + 3 * n] = count;
            n++;
        }
        *total += count;
    }
    f_close(&fil);
    if (!ok || (n == 0)) {
        return false;
    }
    table[0] = SC64SS_SD_RUNS_MAGIC;
    table[1] = n;
    table[2] = *total;
    return true;
}

// the header block as the menu writes it at launch: no shots, the fill point after the block
static void sc64ss_shot_header_init (uint32_t *hdr, uint32_t crc1, uint32_t crc2, uint32_t sectors, const char *owner) {
    memset(hdr, 0, 4096);
    hdr[0] = SC64SS_SD_MAGIC_SHOT;
    hdr[1] = crc1;
    hdr[2] = crc2;
    hdr[3] = 0;
    hdr[4] = sectors;
    hdr[5] = SC64SS_SHOT_HDR_SECTORS;
    snprintf((char *) hdr + SC64SS_SHOTS_OWNER_OFF, 256, "%s", owner);
}

// the screenshot file ready for the routine (allocated when missing, its run table and
// header block on the cart, hook_cfg's fill point and count at their starts); returns
// its size in sectors, 0 when there is none
static uint32_t __attribute__((unused)) sc64ss_prepare_shot_file (menu_t *menu, uint32_t *cfg) {
    static uint32_t table[4 + 3 * SC64SS_SD_RUNS_MAX] __attribute__((aligned(16)));
    static uint32_t hdr[1024] __attribute__((aligned(16)));
    uint64_t check_code = (uint64_t) menu->load.rom_info.check_code;
    uint32_t crc1 = (uint32_t) (check_code >> 32);
    uint32_t crc2 = (uint32_t) (check_code & 0xFFFFFFFFULL);
    uint32_t sectors = 0, total = 0;
    char owner[256];

    sc64ss_shots_finish(menu->storage_prefix);   // (nothing of the last game's is left inside)

    path_t *dir = path_init(menu->storage_prefix, SC64SS_SHOTS_DIR);
    if (!directory_exists(path_get(dir)) && directory_create(path_get(dir))) {
        debugf("SC64SS: could not create %s (%d)\n", path_get(dir), errno);
    }
    path_t *old = path_clone(dir);                // the earlier layout's files, if any
    path_push(old, SC64SS_SHOTS_OLD_DIR);
    if (directory_exists(path_get(old))) {
        for (uint32_t i = 0; i < 8; i++) {
            char name[24];
            snprintf(name, sizeof(name), "shot-%lu.png", (unsigned long) i);
            path_t *f = path_clone(old);
            path_push(f, name);
            remove(path_get(f));
            path_free(f);
        }
        path_t *o = path_clone(old);
        path_push(o, "owner.txt");
        remove(path_get(o));
        path_free(o);
        remove(path_get(old));
    }
    path_free(old);

    path_t *stem = path_clone(menu->load.rom_path);
    path_ext_remove(stem);
    snprintf(owner, sizeof(owner), "%s", path_last_get(stem));
    path_free(stem);

    path_t *file = path_clone(dir);
    path_push(file, SC64SS_SHOTS_FILE);
    char *fp = path_get(file);
    int64_t have = file_exists(fp) ? file_get_size(fp) : 0;
    if (have >= (int64_t) SC64SS_SHOTS_SIZE_MIN) {
        memset(table, 0, sizeof(table));
        if (sc64ss_file_runs(fp, table, &total, (uint32_t) (have / 512))) {
            sectors = total;
        } else {
            remove(fp);
        }
    } else if (have > 0) {
        remove(fp);
    }
    for (uint32_t size = SC64SS_SHOTS_SIZE_MAX; !sectors && (size >= SC64SS_SHOTS_SIZE_MIN); size /= 2) {
        if (file_allocate(fp, size)) {            // no room for one this big
            remove(fp);
            continue;
        }
        memset(table, 0, sizeof(table));
        if (sc64ss_file_runs(fp, table, &total, size / 512)) {
            sectors = total;
        } else {
            remove(fp);                           // in too many pieces for the run table: smaller
        }
    }
    if (sectors) {
        sc64ss_shot_header_init(hdr, crc1, crc2, sectors, owner);
        FILE *f = fopen(fp, "r+b");
        if (f) {
            if (fwrite(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
                sectors = 0;
            }
            fclose(f);
        } else {
            sectors = 0;
        }
    }
    if (sectors) {
        data_cache_hit_writeback(table, sizeof(table));
        dma_write(table, SC64SS_SHOT_TABLE_PI, sizeof(table));
        data_cache_hit_writeback(hdr, sizeof(hdr));
        dma_write(hdr, SC64SS_SHOT_HDR_PI, sizeof(hdr));
        cfg[43] = SC64SS_SHOT_HDR_SECTORS;        // hook_cfg.shot_fill
        cfg[44] = 0;                              // hook_cfg.shot_count
        debugf("SC64SS: screenshot file %s, %lu sectors\n", fp, (unsigned long) sectors);
    } else {
        debugf("SC64SS: no screenshot file (%d)\n", errno);
    }
    path_free(file);
    path_free(dir);
    return sectors;
}

// the last game's screenshots out of the pending file, into the game's folder
void sc64ss_shots_finish (const char *storage_prefix) {
    static uint32_t hdr[1024] __attribute__((aligned(16)));
    char owner[80];
    path_t *file = path_init((char *) storage_prefix, SC64SS_SHOTS_DIR);
    path_push(file, SC64SS_SHOTS_FILE);
    char *fp = path_get(file);
    FILE *f = fopen(fp, "r+b");
    if (!f) {
        path_free(file);
        return;
    }
    setbuf(f, NULL);
    if ((fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) || (hdr[0] != SC64SS_SD_MAGIC_SHOT) ||
        (hdr[3] == 0) || (hdr[3] > SC64SS_SHOT_ENTRIES_MAX)) {
        fclose(f);
        path_free(file);
        return;
    }
    uint32_t count = hdr[3];
    int64_t size = (fseek(f, 0, SEEK_END) == 0) ? ftell(f) : 0;
    snprintf(owner, sizeof(owner), "%.79s", (const char *) hdr + SC64SS_SHOTS_OWNER_OFF);
    for (char *p = owner; *p; p++) {
        if (strchr("\\/:*?\"<>|\r\n", *p)) {
            *p = '_';
        }
    }
    if (!owner[0]) {
        snprintf(owner, sizeof(owner), "unknown");
    }
    uint8_t *buf = malloc(32768);
    path_t *gdir = path_init((char *) storage_prefix, SC64SS_SHOTS_DIR);
    path_push(gdir, owner);
    if (!directory_exists(path_get(gdir))) {
        directory_create(path_get(gdir));
    }
    uint32_t seq = 0;
    for (uint32_t i = 0; (buf != NULL) && (i < count); i++) {
        uint32_t sector = hdr[8 + 2 * i], len = hdr[9 + 2 * i];
        char stamp[32] = "", fname[160];
        uint8_t ch[8];
        uint32_t off = 8;
        bool ok = false;
        if (!len || (sector < SC64SS_SHOT_HDR_SECTORS) || (((int64_t) sector * 512 + len) > size)) {
            continue;
        }
        if ((fseek(f, (long) sector * 512, SEEK_SET) != 0) || (fread(ch, 1, 8, f) != 8) ||
            (memcmp(ch, "\x89PNG\r\n\x1a\n", 8) != 0)) {
            continue;
        }
        for (int k = 0; k < 64; k++) {    // the chunks up to IEND: the time stamp
            uint32_t clen;
            if (fread(ch, 1, 8, f) != 8) {
                break;
            }
            clen = ((uint32_t) ch[0] << 24) | ((uint32_t) ch[1] << 16) | ((uint32_t) ch[2] << 8) | ch[3];
            if ((off + 12 + clen) > len) {
                break;
            }
            off += 12 + clen;
            if ((memcmp(ch + 4, "tEXt", 4) == 0) && (clen < 64)) {
                char t[64];
                if (fread(t, 1, clen, f) != clen) {
                    break;
                }
                t[clen] = 0;
                if ((clen > 14) && (memcmp(t, "Creation Time", 14) == 0)) {
                    snprintf(stamp, sizeof(stamp), "%s", t + 14);
                }
                fseek(f, 4, SEEK_CUR);
            } else if (memcmp(ch + 4, "IEND", 4) == 0) {
                ok = true;
                break;
            } else {
                fseek(f, (long) (clen + 4), SEEK_CUR);
            }
        }
        if (!ok) {
            continue;
        }
        if (stamp[0]) {
            for (char *p = stamp; *p; p++) {
                if (*p == ':') {
                    *p = '-';
                }
            }
            snprintf(fname, sizeof(fname), "%s %s.png", owner, stamp);
        } else {
            snprintf(fname, sizeof(fname), "%s %04lu.png", owner, (unsigned long) ++seq);
        }
        path_t *dst = path_clone(gdir);
        path_push(dst, fname);
        for (uint32_t k = 2; file_exists(path_get(dst)) && (k < 100); k++) {   // the same second twice: a suffix
            path_pop(dst);
            if (stamp[0]) {
                snprintf(fname, sizeof(fname), "%s %s (%lu).png", owner, stamp, (unsigned long) k);
            } else {
                snprintf(fname, sizeof(fname), "%s %04lu (%lu).png", owner, (unsigned long) seq, (unsigned long) k);
            }
            path_push(dst, fname);
        }
        FILE *o = fopen(path_get(dst), "wb");
        if (o) {
            uint32_t left = len;
            if (fseek(f, (long) sector * 512, SEEK_SET) != 0) {
                left = 0;
            }
            while (left) {
                uint32_t n = (left > 32768) ? 32768 : left;
                if ((fread(buf, 1, n, f) != n) || (fwrite(buf, 1, n, o) != n)) {
                    break;
                }
                left -= n;
            }
            fclose(o);
            debugf("SC64SS: screenshot %lu -> %s (%s)\n", (unsigned long) i, path_get(dst), left ? "short" : "ok");
        }
        path_free(dst);
    }
    free(buf);
    path_free(gdir);
    hdr[3] = 0;                                   // the file empty again
    hdr[5] = SC64SS_SHOT_HDR_SECTORS;
    memset(&hdr[8], 0, (SC64SS_SHOTS_OWNER_OFF / 4 - 8) * 4);
    if (fseek(f, 0, SEEK_SET) == 0) {
        fwrite(hdr, 1, sizeof(hdr), f);
    }
    fclose(f);
    path_free(file);
}

// SC64SS: a freshly formatted Controller Pak image (one bank), laid out the way
// libdragon's cpakfs_format writes a real one: the ID block at 0x20/0x60/0x80/0xC0
// (serial, device id 1, one bank, two checksums), the FAT at 0x100 with a backup at
// 0x200 (five reserved pages, the rest free, a checksum byte in entry 0), an empty
// note table at 0x300.
void sc64ss_pak_format (uint8_t *img, uint32_t crc1, uint32_t crc2) {
    uint8_t id[32];
    uint32_t seed = crc1 ^ (crc2 * 2654435761u) ^ (uint32_t) get_ticks();
    uint32_t sum = 0;
    memset(img, 0, SC64SS_VPAK_LEN);
    memset(id, 0, sizeof(id));
    for (int i = 0; i < 16; i++) {
        seed = seed * 1103515245u + 12345u;
        id[i] = (uint8_t) (seed >> 24);
    }
    memcpy(id + 16, "SC64SS", 6);
    id[24] = 0x00; id[25] = 0x01;      // device id
    id[26] = 0x01; id[27] = 0x00;      // one bank
    for (int i = 0; i < 28; i += 2) {
        sum += (uint32_t) ((id[i] << 8) | id[i + 1]);
    }
    sum &= 0xFFFF;
    id[28] = (uint8_t) (sum >> 8); id[29] = (uint8_t) sum;
    sum = (0xFFF2 - sum) & 0xFFFF;
    id[30] = (uint8_t) (sum >> 8); id[31] = (uint8_t) sum;
    memcpy(img + 0x20, id, 32);
    memcpy(img + 0x60, id, 32);
    memcpy(img + 0x80, id, 32);
    memcpy(img + 0xC0, id, 32);
    uint8_t *fat = img + 0x100;
    uint32_t chk = 0;
    for (int i = 5; i < 128; i++) {
        fat[2 * i] = 0;
        fat[2 * i + 1] = 3;
    }
    for (int i = 1; i < 128; i++) {
        chk += fat[2 * i] + fat[2 * i + 1];
    }
    fat[0] = 0;
    fat[1] = (uint8_t) chk;
    memcpy(img + 0x200, fat, 256);
}

// SC64SS: the accessory data CRC (polynomial 0x85) of a 32-byte block, as the controller
// answers it; the borrowed-mode server answers a read with the block's bytes and this
// word from a table on the cart (hook/monitor.S mp_read), its write path keeps the table
static uint8_t sc64ss_pak_data_crc (const uint8_t *d) {
    uint32_t crc = 0;
    for (int i = 0; i < 32; i++) {
        uint32_t x = crc ^ d[i];
        crc = 0;
        if (x & 0x80) crc ^= 0x89;
        if (x & 0x40) crc ^= 0x86;
        if (x & 0x20) crc ^= 0x43;
        if (x & 0x10) crc ^= 0xE3;
        if (x & 0x08) crc ^= 0xB3;
        if (x & 0x04) crc ^= 0x9B;
        if (x & 0x02) crc ^= 0x8F;
        if (x & 0x01) crc ^= 0x85;
    }
    return (uint8_t) (crc & 0xFF);
}

// SC64SS: the game's virtual Controller Pak: sd:/savestates/paks/<checkcode>.pak (32 KiB, a
// plain pak image the menu's Controller Pak tools can read), created formatted, loaded
// into cart memory at SC64SS_VPAK_PI with its run table after it. true = the hook may
// enable the pak (cfg spare bit 2).
static bool sc64ss_prepare_pak (menu_t *menu) {
    static uint8_t img[SC64SS_VPAK_LEN] __attribute__((aligned(16)));
    static uint32_t table[4 + 3 * SC64SS_SD_RUNS_MAX] __attribute__((aligned(16)));
    uint64_t check_code = (uint64_t) menu->load.rom_info.check_code;
    uint32_t crc1 = (uint32_t) (check_code >> 32);
    uint32_t crc2 = (uint32_t) (check_code & 0xFFFFFFFFULL);
    uint32_t total = 0;
    bool ok = false;
    char name[48];
    path_t *dir = path_init(menu->storage_prefix, "/savestates");
    if (!directory_exists(path_get(dir))) {
        directory_create(path_get(dir));
    }
    path_push(dir, "paks");
    if (!directory_exists(path_get(dir))) {
        directory_create(path_get(dir));
    }
    snprintf(name, sizeof(name), "%08lX%08lX.pak", (unsigned long) crc1, (unsigned long) crc2);
    path_t *file = path_clone(dir);
    path_push(file, name);
    char *fp = path_get(file);
    FILE *f = fopen(fp, "rb");
    bool foreign = false;
    if (f) {
        long size = (fseek(f, 0, SEEK_END) == 0) ? ftell(f) : -1L;
        rewind(f);
        if ((size != 0) && (size != (long) SC64SS_VPAK_LEN)) {
            // not a plain one-bank image but a file copied here by hand: never overwritten (the
            // launch refuses before this); an empty one, a power cut while it was made, is remade
            foreign = true;
        } else {
            size_t got = fread(img, 1, SC64SS_VPAK_LEN, f);
            ok = (got == SC64SS_VPAK_LEN);
        }
        fclose(f);
    }
    if (!ok && !foreign) {
        sc64ss_pak_format(img, crc1, crc2);
        f = fopen(fp, "wb");
        if (f) {
            ok = (fwrite(img, 1, SC64SS_VPAK_LEN, f) == SC64SS_VPAK_LEN);
            fclose(f);
        }
        debugf("SC64SS: pak file %s %s\n", fp, ok ? "created" : "NOT created");
    }
    if (ok) {
        sc64ss_vpak_label_store(fp, path_last_get(menu->load.rom_path));   // the game's name for the paks view
        memset(table, 0, sizeof(table));
        ok = sc64ss_state_file_runs(fp, table, &total, SC64SS_VPAK_LEN / 512);
        if (ok && (total >= SC64SS_VPAK_LEN / 512)) {
            data_cache_hit_writeback(img, sizeof(img));
            dma_write(img, SC64SS_VPAK_PI, sizeof(img));
            data_cache_hit_writeback(table, sizeof(table));
            dma_write(table, SC64SS_VPAK_PI + SC64SS_VPAK_LEN, sizeof(table));
            // the block CRCs, a word a block, for the borrowed-mode server's reads
            static uint32_t crcs[SC64SS_VPAK_LEN / 32] __attribute__((aligned(16)));
            for (uint32_t b = 0; b < SC64SS_VPAK_LEN / 32; b++) {
                crcs[b] = sc64ss_pak_data_crc(img + b * 32);
            }
            data_cache_hit_writeback(crcs, sizeof(crcs));
            dma_write(crcs, SC64SS_VPAK_CRC_PI, sizeof(crcs));
            // borrowed mode: the pak's control block starts clean ('VPK1', state 0: the
            // hook installs the game's handler patch in a service borrow, the monitor's
            // server keeps its dirty stamp and bank byte here)
            static uint32_t ctl[16] __attribute__((aligned(16)));
            memset(ctl, 0, sizeof(ctl));
            ctl[0] = 0x56504B31UL;
            // +0x38 the live word (hook.c PAK_LIVE_*): the port's channel, inserted; the
            // panel changes it in the game
            ctl[14] = (uint32_t) ((menu->load.rom_info.settings.vpak_port - 1) & 3) | 0x10;
            data_cache_hit_writeback(ctl, sizeof(ctl));
            dma_write(ctl, SC64SS_VPAK_CTL_PI, sizeof(ctl));
            debugf("SC64SS: virtual pak loaded from %s\n", fp);
        } else {
            debugf("SC64SS: no run table for %s\n", fp);
            ok = false;
        }
    }
    path_free(file);
    path_free(dir);
    return ok;
}

// SC64SS: the size of this game's virtual pak file on the card, -1 with no file, and its
// name (<checkcode>.pak) for a message
static long sc64ss_pak_file_size (menu_t *menu, char *name, size_t name_len) {
    uint64_t check_code = (uint64_t) menu->load.rom_info.check_code;
    snprintf(name, name_len, "%08lX%08lX.pak", (unsigned long) (check_code >> 32), (unsigned long) (check_code & 0xFFFFFFFFULL));
    path_t *file = path_init(menu->storage_prefix, "/savestates/paks");
    path_push(file, name);
    long size = -1L;
    FILE *f = fopen(path_get(file), "rb");
    if (f) {
        size = (fseek(f, 0, SEEK_END) == 0) ? ftell(f) : -1L;
        fclose(f);
    }
    path_free(file);
    return size;
}
static char vpak_file_text[320];

static bool show_extra_info_message = false;
static bool show_advanced_info_message = false;
static bool show_expansion_pak_warning = false;
static component_boxart_t *boxart;
static char *rom_filename = NULL;

static int16_t current_metadata_image_index = 0;
static const file_image_type_t metadata_image_filename_cache[] = {
    IMAGE_BOXART_FRONT,
    IMAGE_BOXART_BACK,
    IMAGE_BOXART_LEFT,
    IMAGE_BOXART_RIGHT,
    IMAGE_BOXART_TOP,
    IMAGE_BOXART_BOTTOM,
    IMAGE_GAMEPAK_FRONT,
    IMAGE_GAMEPAK_BACK
};
static const uint16_t metadata_image_filename_cache_length = sizeof(metadata_image_filename_cache) / sizeof(metadata_image_filename_cache[0]);
static bool metadata_image_available[sizeof(metadata_image_filename_cache) / sizeof(metadata_image_filename_cache[0])] = {false};
static bool metadata_images_scanned = false;

static void scan_metadata_images(menu_t *menu) {
    if (metadata_images_scanned) {
        return;
    }

    path_t *path = path_init(menu->storage_prefix, "menu/metadata"); // should be METADATA_BASE_DIRECTORY
    char game_code_path[50];

    if (menu->load.rom_info.game_code[1] == 'E' && menu->load.rom_info.game_code[2] == 'D') {
        // This is using a homebrew ROM ID, use the title for the file name instead.
        // Create a null-terminated copy of the title for safe string operations
        char safe_title[21];  // 20 chars + null terminator
        memcpy(safe_title, menu->load.rom_info.title, 20);
        safe_title[20] = '\0';
        
        snprintf(game_code_path, sizeof(game_code_path), "homebrew/%s", safe_title); // should be HOMEBREW_ID_SUBDIRECTORY
        path_push(path, game_code_path);
    }
    else {
        snprintf(game_code_path, sizeof(game_code_path), "%c/%c/%c/%c",
            menu->load.rom_info.game_code[0],
            menu->load.rom_info.game_code[1],
            menu->load.rom_info.game_code[2],
            menu->load.rom_info.game_code[3]);
        path_push(path, game_code_path);

        if (!directory_exists(path_get(path))) { // Allow boxart to not specify the region code.
            path_pop(path);
        }
    }

    bool dir_exists = directory_exists(path_get(path));

    if (dir_exists) {
        // Filenames array matches metadata_image_filename_cache order for indexed access
        // Note: This mapping is also present in boxart.c but duplicated here
        // for efficient scanning without calling into the component layer
        char *filenames[] = {
            "boxart_front.png",
            "boxart_back.png",
            "boxart_left.png",
            "boxart_right.png",
            "boxart_top.png",
            "boxart_bottom.png",
            "gamepak_front.png",
            "gamepak_back.png"
        };

        for (uint16_t i = 0; i < metadata_image_filename_cache_length; i++) {
            path_push(path, filenames[i]);
            metadata_image_available[i] = file_exists(path_get(path));
            path_pop(path);
        }
    } else {
        // No directory exists, mark all images as unavailable
        for (uint16_t i = 0; i < metadata_image_filename_cache_length; i++) {
            metadata_image_available[i] = false;
        }
    }

    debugf("Metadata: Scanned metadata for ROM ID %s. \n", game_code_path);

    path_free(path);
    metadata_images_scanned = true;
}

static const char *format_rom_description(menu_t *menu) {
    const char *rom_description = NULL;

    if (menu->load.rom_info.meta.short_description != NULL && strlen(menu->load.rom_info.meta.short_description) > 0) {
        rom_description = menu->load.rom_info.meta.short_description;
    }

    return rom_description ? rom_description : "No description available.";
}

static char *convert_error_message (rom_err_t err) {
    switch (err) {
        case ROM_ERR_LOAD_IO: return "I/O error during loading ROM information and/or options";
        case ROM_ERR_SAVE_IO: return "I/O error during storing ROM options";
        case ROM_ERR_NO_FILE: return "Couldn't open ROM file";
        default: return "Unknown ROM info load error";
    }
}

static const char *format_rom_endianness (rom_endianness_t endianness) {
    switch (endianness) {
        case ENDIANNESS_BIG: return "Big (default)";
        case ENDIANNESS_LITTLE: return "Little (unsupported)";
        case ENDIANNESS_BYTE_SWAP: return "Byte swapped";
        default: return "Unknown";
    }
}

static const char *format_rom_media_type (rom_category_type_t media_type) {
    switch (media_type) {
        case N64_CART: return "Cartridge";
        case N64_DISK: return "Disk";
        case N64_CART_EXPANDABLE: return "Cartridge (Expandable)";
        case N64_DISK_EXPANDABLE: return "Disk (Expandable)";
        case N64_ALECK64: return "Aleck64";
        default: return "Unknown";
    }
}

static const char *format_rom_destination_market (rom_destination_type_t market_type) {
    // TODO: These are all assumptions and should be corrected if required.
    // From http://n64devkit.square7.ch/info/submission/pal/01-01.html
    switch (market_type) {
        case MARKET_JAPANESE_MULTI: return "Japanese & English"; // 1080 Snowboarding JPN
        case MARKET_BRAZILIAN: return "Brazilian (Portuguese)";
        case MARKET_CHINESE: return "Chinese";
        case MARKET_GERMAN: return "German";
        case MARKET_NORTH_AMERICA: return "American English";
        case MARKET_FRENCH: return "French";
        case MARKET_DUTCH: return "Dutch";
        case MARKET_ITALIAN: return "Italian";
        case MARKET_JAPANESE: return "Japanese";
        case MARKET_KOREAN: return "Korean";
        case MARKET_CANADIAN: return "Canadaian (English & French)";
        case MARKET_SPANISH: return "Spanish";
        case MARKET_AUSTRALIAN: return "Australian (English)";
        case MARKET_SCANDINAVIAN: return "Scandinavian";
        case MARKET_GATEWAY64_NTSC: return "LodgeNet/Gateway (NTSC)";
        case MARKET_GATEWAY64_PAL: return "LodgeNet/Gateway (PAL)";
        case MARKET_EUROPEAN_BASIC: return "PAL (includes English)"; // Mostly EU but is used on some Australian ROMs
        case MARKET_OTHER_X: return "Regional (non specific)"; // FIXME: AUS HSV Racing ROM's and Asia Top Gear Rally use this so not only EUR
        case MARKET_OTHER_Y: return "European (non specific)";
        case MARKET_OTHER_Z: return "Regional (unknown)";
        default: return "Unknown";
    }
}

static const char *format_rom_save_type (rom_save_type_t save_type, bool supports_cpak) {
    switch (save_type) {
        case SAVE_TYPE_NONE: return supports_cpak ? "Controller PAK" : "None";
        case SAVE_TYPE_EEPROM_4KBIT: return supports_cpak ?   "EEPROM 4kbit | Controller PAK" : "EEPROM 4kbit";
        case SAVE_TYPE_EEPROM_16KBIT: return supports_cpak ?  "EEPROM 16kbit | Controller PAK" : "EEPROM 16kbit";
        case SAVE_TYPE_SRAM_256KBIT: return supports_cpak ?   "SRAM 256kbit | Controller PAK" : "SRAM 256kbit";
        case SAVE_TYPE_SRAM_BANKED: return supports_cpak ?    "SRAM 768kbit / 3 banks | Controller PAK" : "SRAM 768kbit / 3 banks";
        case SAVE_TYPE_SRAM_1MBIT: return supports_cpak ?     "SRAM 1Mbit | Controller PAK" : "SRAM 1Mbit";
        case SAVE_TYPE_FLASHRAM_1MBIT: return supports_cpak ? "FlashRAM 1Mbit | Controller PAK" : "FlashRAM 1Mbit";
        case SAVE_TYPE_FLASHRAM_PKST2: return supports_cpak ? "FlashRAM (Pokemon Stadium 2) | Controller PAK" : "FlashRAM (Pokemon Stadium 2)";
        default: return "Unknown";
    }
}

static const char *format_rom_tv_type (rom_tv_type_t tv_type) {
    switch (tv_type) {
        case ROM_TV_TYPE_PAL: return "PAL";
        case ROM_TV_TYPE_NTSC: return "NTSC";
        case ROM_TV_TYPE_MPAL: return "MPAL";
        default: return "Unknown";
    }
}

static const char *format_rom_expansion_pak_info (rom_expansion_pak_t expansion_pak_info) {
    switch (expansion_pak_info) {
        case EXPANSION_PAK_REQUIRED: return "Required";
        case EXPANSION_PAK_RECOMMENDED: return "Recommended";
        case EXPANSION_PAK_SUGGESTED: return "Suggested";
        case EXPANSION_PAK_FAULTY: return "May require ROM patch";
        default: return "Not required";
    }
}

static const char *format_rom_pak_feature_info (bool pak_feature_info) {
    if (pak_feature_info) {
        return "Supported";
    } else {
        return "Not used";
    }
}

static const char *format_cic_type (rom_cic_type_t cic_type) {
    switch (cic_type) {
        case ROM_CIC_TYPE_5101: return "5101";
        case ROM_CIC_TYPE_5167: return "5167";
        case ROM_CIC_TYPE_6101: return "6101";
        case ROM_CIC_TYPE_7102: return "7102";
        case ROM_CIC_TYPE_x102: return "6102 / 7101";
        case ROM_CIC_TYPE_x103: return "6103 / 7103";
        case ROM_CIC_TYPE_x105: return "6105 / 7105";
        case ROM_CIC_TYPE_x106: return "6106 / 7106";
        case ROM_CIC_TYPE_8301: return "8301";
        case ROM_CIC_TYPE_8302: return "8302";
        case ROM_CIC_TYPE_8303: return "8303";
        case ROM_CIC_TYPE_8401: return "8401";
        case ROM_CIC_TYPE_8501: return "8501";
        default: return "Unknown";
    }
}

static const char *format_age_rating (uint32_t age_rating) {
    if (age_rating >= 18) {
        return "Adults Only";
    }
    else if (age_rating >= 17) {
        return "Mature";
    }
    else if (age_rating >= 13) {
        return "Teen";
    }
    else if (age_rating >= 10) {
        return "Everyone 10+";
    }
    else if (age_rating > 0) {
        return "Everyone";
    }
    else if (age_rating == 0) {
        return "None";
    }
    else {
        return "Unknown";
    }
}

static inline const char *format_boolean_type (bool bool_value) {
    return bool_value ? "On" : "Off";
}

// Forward declarations for default selection helpers (defined after context menu structs)
static int get_rom_cic_override_current_selection (menu_t *menu);
static int get_rom_save_override_current_selection (menu_t *menu);
static int get_rom_tv_override_current_selection (menu_t *menu);
static int get_rom_cheat_override_current_selection (menu_t *menu);
static int get_rom_savestate_current_selection (menu_t *menu);
static int get_rom_vpak_current_selection (menu_t *menu);
static int get_rom_slowmotion_current_selection (menu_t *menu);
#ifdef FEATURE_PATCHER_GUI_ENABLED
static int get_rom_patch_override_current_selection (menu_t *menu);
#endif

static void set_cic_type (menu_t *menu, void *arg) {
    rom_cic_type_t cic_type = (rom_cic_type_t) (arg);
    rom_err_t err = rom_config_override_cic_type(menu->load.rom_path, &menu->load.rom_info, cic_type);
    if (err != ROM_OK) {
        menu_show_error(menu, convert_error_message(err));
    }
    menu->browser.reload = true;
}

static void set_save_type (menu_t *menu, void *arg) {
    rom_save_type_t save_type = (rom_save_type_t) (arg);
    rom_err_t err = rom_config_override_save_type(menu->load.rom_path, &menu->load.rom_info, save_type);
    if (err != ROM_OK) {
        menu_show_error(menu, convert_error_message(err));
    }
    menu->browser.reload = true;
}

static void set_tv_type (menu_t *menu, void *arg) {
    rom_tv_type_t tv_type = (rom_tv_type_t) (arg);
    rom_err_t err = rom_config_override_tv_type(menu->load.rom_path, &menu->load.rom_info, tv_type);
    if (err != ROM_OK) {
        menu_show_error(menu, convert_error_message(err));
    }
    menu->browser.reload = true;
}
#ifdef FEATURE_AUTOLOAD_ROM_ENABLED
static void set_autoload_type (menu_t *menu, void *arg) {
    // SC64SS: the ROM this screen shows, wherever the screen was opened from. The browser's
    // folder and highlighted entry are that ROM only when it was opened from the browser;
    // from the history or the favourites they are whatever the browser last showed, and
    // the next boot could not open what was stored.
    path_t *dir = path_clone(menu->load.rom_path);
    path_pop(dir);
    free(menu->settings.rom_autoload_path);
    menu->settings.rom_autoload_path = strdup(strip_fs_prefix(path_get(dir)));
    free(menu->settings.rom_autoload_filename);
    menu->settings.rom_autoload_filename = strdup(path_last_get(menu->load.rom_path));
    path_free(dir);
    // FIXME: add a confirmation box here! (press start on reboot)
    menu->settings.rom_autoload_enabled = true;
    settings_save(&menu->settings);
    menu->browser.reload = true;
}
#endif

static void set_cheat_option(menu_t *menu, void *arg) {
    debugf("Load Rom: setting cheat option to %d\n", (int)arg);
    if (!is_memory_expanded()) {
        // If the Expansion pak is not installed, we cannot use cheats, and force it to off (just incase).
        rom_config_setting_set_cheats(menu->load.rom_path, &menu->load.rom_info, false);
        menu_show_error(menu, "Datel Cheats require an Expansion Pak");
        menu->browser.reload = true;
    }
    else {
        bool enabled = (bool)arg;
        rom_config_setting_set_cheats(menu->load.rom_path, &menu->load.rom_info, enabled);
        menu->browser.reload = true;
    }
}

// SC64SS: save states need the Expansion Pak (the hook lives in its top 64 KiB) and
// room for at least one slot in cart SDRAM above the ROM.
static void set_savestate_option (menu_t *menu, void *arg) {
    bool enabled = (bool)arg;
    if (enabled && !is_memory_expanded()) {
        rom_config_setting_set_savestates(menu->load.rom_path, &menu->load.rom_info, false);
        menu_show_error(menu, "Save States require an Expansion Pak");
        menu->browser.reload = true;
        return;
    }
    if (enabled) {
        uint32_t slots[SC64SS_SLOTS_MAX];
        if (sc64ss_slot_table(file_get_size(path_get(menu->load.rom_path)), slots, SC64SS_STATE_SLOT_LEN_B, menu->load.rom_info.libdragon) == 0) {
            rom_config_setting_set_savestates(menu->load.rom_path, &menu->load.rom_info, false);
            menu_show_error(menu, "No room for save states:\nthe ROM fills the cartridge memory");
            menu->browser.reload = true;
            return;
        }
    }
    rom_config_setting_set_savestates(menu->load.rom_path, &menu->load.rom_info, enabled);
    menu->browser.reload = true;
}

// SC64SS: the virtual Controller Pak rides on the same engine as save states (in
// borrowed mode the monitor serves it from the cart). The choice is the port it sits in
// at launch (1..4: a controller must be in that port when the game boots, since a game
// only talks to the ports it found then) or off; the panel's Game page can take it out
// and put it back in any port while the game runs.
// SC64SS: a real Controller Pak in a port (1..4), as the joypad subsystem sees it this frame.
// The virtual pak in that port answers the game first, and the game's writes would land in
// the real pak as well: such a port is refused, in the port list and at launch.
static bool vpak_port_holds_pak (int port) {
    joypad_port_t p = (joypad_port_t) (port - 1);
    return (port >= 1) && (port <= 4) && joypad_is_connected(p) && (joypad_get_accessory_type(p) == JOYPAD_ACCESSORY_TYPE_CONTROLLER_PAK);
}
static char vpak_conflict_text[224];

static void set_vpak_option (menu_t *menu, void *arg) {
    int port = (int) (uintptr_t) arg;
    bool enabled = (port != 0);
    if (enabled && !is_memory_expanded()) {
        rom_config_setting_set_vpak(menu->load.rom_path, &menu->load.rom_info, false);
        menu_show_error(menu, "The virtual Controller Pak requires an Expansion Pak");
        menu->browser.reload = true;
        return;
    }
    if (enabled && vpak_port_holds_pak(port)) {
        snprintf(vpak_conflict_text, sizeof(vpak_conflict_text),
                 "A Controller Pak is plugged into port %d. The game's writes would land in it as well as in the virtual pak.\nTake it out to use this port, or pick another.", port);
        menu_show_error(menu, vpak_conflict_text);
        menu->browser.reload = true;
        return;
    }
    rom_config_setting_set_vpak(menu->load.rom_path, &menu->load.rom_info, enabled);
    if (enabled) {
        rom_config_setting_set_vpak_port(menu->load.rom_path, &menu->load.rom_info, port);
    }
    menu->browser.reload = true;
}

// SC64SS: the virtual pak in the ROM's info text: "Off", or the port it starts in
static const char *format_vpak_info (rom_info_t *rom_info) {
    static char text[12];
    if (!rom_info->settings.vpak_enabled) {
        return "Off";
    }
    snprintf(text, sizeof(text), "port %d", rom_info->settings.vpak_port);
    return text;
}

// SC64SS: Slow motion (the panel's Game page: 1/2, 1/4, 1/8, frame step, the sound
// slowed to match) needs the routine resident at the top of RAM, where it can hold the
// game between frames; the cartridge-side engine cannot. The option is the placement:
// on = the resident hook (hook_borrowed=0), off = the borrowed engine, the default.
// Slots and files have one layout for both, so switching loses no state; a state saved
// with it on holds the RAM below the routine (7.75 MiB) and loads either way. Games
// that use all of the Expansion Pak (Donkey Kong 64, Perfect Dark, Indiana Jones, Rush
// 2049) never boot with it on, since the routine's home at the top of RAM is theirs:
// the option is refused for them, and a launch keeps the borrowed engine for them
// whatever an old ini says.
static bool sc64ss_full_ram_title (const rom_info_t *rom_info) {
    static const char codes[][3] = {
        { 'N', 'P', 'D' },      // Perfect Dark
        { 'N', 'D', 'O' },      // Donkey Kong 64
        { 'N', 'I', 'J' },      // Indiana Jones and the Infernal Machine
        { 'N', 'R', 'U' },      // San Francisco Rush 2049
        { 'N', '3', 'T' },      // Tony Hawk's Pro Skater 3 (its high-resolution mode)
    };
    for (uint32_t k = 0; k < sizeof(codes) / sizeof(codes[0]); k++) {
        if (memcmp(rom_info->game_code, codes[k], 3) == 0) {
            return true;
        }
    }
    return false;
}

static void set_slowmotion_option (menu_t *menu, void *arg) {
    bool enabled = (bool)arg;
    if (enabled && !is_memory_expanded()) {
        rom_config_setting_set_hook_borrowed(menu->load.rom_path, &menu->load.rom_info, true);
        menu_show_error(menu, "Slow motion requires an Expansion Pak");
        menu->browser.reload = true;
        return;
    }
    if (enabled && sc64ss_full_ram_title(&menu->load.rom_info)) {
        rom_config_setting_set_hook_borrowed(menu->load.rom_path, &menu->load.rom_info, true);
        menu_show_error(menu, "Slow motion is not available for this game: it uses all of the Expansion Pak, where the routine would sit");
        menu->browser.reload = true;
        return;
    }
    rom_config_setting_set_hook_borrowed(menu->load.rom_path, &menu->load.rom_info, !enabled);
    menu->browser.reload = true;
}

// SC64SS: this ROM's own hotkeys and its screenshot button
// SC64SS: a page opened from the options menu brings the menu back on its return, at the
// row it was opened from
static bool options_reopen = false;
static int options_last_row = 0;

static void open_hotkeys (menu_t *menu, void *arg) {
    options_reopen = true;
    (void)arg;
    menu->hotkeys_for_rom = true;
    menu->next_mode = MENU_MODE_HOTKEYS;
}

// SC64SS: this game's virtual pak beside a real Controller Pak, for copies either way
static void open_vpak_copy (menu_t *menu, void *arg) {
    options_reopen = true;
    (void)arg;
    menu->vpak_view.check_code = menu->load.rom_info.check_code;
    menu->vpak_view.from_rom = true;
    menu->next_mode = MENU_MODE_VIRTUAL_PAK;
}

// SC64SS: the buttons a launch hands the routine: the ROM's own setting when it reads as
// buttons, else the menu's, else the built-in one
static uint16_t sc64ss_key_effective (const char *rom_text, const char *menu_text, const char *builtin) {
    uint16_t m = sc64ss_keys_parse(rom_text);
    if (!m) {
        m = sc64ss_keys_parse(menu_text);
    }
    if (!m) {
        m = sc64ss_keys_parse(builtin);
    }
    return m;
}

// SC64SS: a game's own hotkey text when it counts (set no earlier than the menu's last set of
// that hotkey for every game), else empty; k = 0 save, 1 load, 2 panel, 3 step, 4 screenshot
static const char *sc64ss_rom_key (menu_t *menu, int k) {
    const char *t;
    int menu_set;
    switch (k) {
        case 0: t = menu->load.rom_info.settings.hotkey_save; menu_set = menu->settings.ss_key_set_save; break;
        case 1: t = menu->load.rom_info.settings.hotkey_load; menu_set = menu->settings.ss_key_set_load; break;
        case 2: t = menu->load.rom_info.settings.hotkey_panel; menu_set = menu->settings.ss_key_set_panel; break;
        case 3: t = menu->load.rom_info.settings.hotkey_step; menu_set = menu->settings.ss_key_set_step; break;
        default: t = menu->load.rom_info.settings.screenshot_button; menu_set = menu->settings.ss_key_set_shot; break;
    }
    return sc64ss_key_own(t, menu->load.rom_info.settings.hotkey_set[k], menu_set) ? t : "";
}

static void open_datel_code_editor (menu_t *menu, void *arg) {
    (void)arg;

    if (!is_memory_expanded()) {
        rom_config_setting_set_cheats(menu->load.rom_path, &menu->load.rom_info, false);
        menu_show_error(menu, "Datel Cheats require an Expansion Pak");
        menu->browser.reload = true;
        return;
    }

    menu->next_mode = MENU_MODE_DATEL_CODE_EDITOR;
}

#ifdef FEATURE_PATCHER_GUI_ENABLED
static void set_patcher_option(menu_t *menu, void *arg) {
    bool enabled = (bool)arg;
    rom_config_setting_set_patches(menu->load.rom_path, &menu->load.rom_info, enabled);
    menu->browser.reload = true;
}
#endif

static void set_clear_rdram_option(menu_t *menu, void *arg) {
    bool enabled = (bool)arg;
    rom_config_setting_set_clear_rdram(menu->load.rom_path, &menu->load.rom_info, enabled);
    menu->browser.reload = true;
}

static void add_favorite (menu_t *menu, void *arg) {
    bookkeeping_favorite_add(&menu->bookkeeping, menu->load.rom_path, NULL, BOOKKEEPING_TYPE_ROM);
}

static void iterate_metadata_image(menu_t *menu, int direction) {
    scan_metadata_images(menu);
    bool low_memory_mode = !is_memory_expanded();
    int16_t previous_metadata_image_index = current_metadata_image_index;

    // Transverse to next/previous available image based on direction (1 = next, -1 = previous)
    int16_t start_metadata_image_index = current_metadata_image_index;
    int16_t new_metadata_image_index = (current_metadata_image_index + direction + metadata_image_filename_cache_length) % metadata_image_filename_cache_length;

    // Find next available image from our cached list
    while (new_metadata_image_index != start_metadata_image_index) {
        if (metadata_image_available[new_metadata_image_index]) {
            if (low_memory_mode && boxart != NULL) {
                // On Jumper Pak, avoid holding old and new boxart textures at once.
                ui_components_boxart_free(boxart);
                boxart = NULL;
            }

            // ui_components_boxart_init returns NULL if PNG decoder is busy
            component_boxart_t *new_boxart = ui_components_boxart_init(
                menu->storage_prefix,
                menu->load.rom_info.game_code,
                menu->load.rom_info.title,
                metadata_image_filename_cache[new_metadata_image_index]
            );

            if (new_boxart != NULL) {
                // Only free old boxart after successful new allocation
                if (!low_memory_mode) {
                    ui_components_boxart_free(boxart);
                }
                boxart = new_boxart;
                current_metadata_image_index = new_metadata_image_index;
                sound_play_effect(SFX_SETTING);
                break;
            } else if (low_memory_mode) {
                // Best effort restore of previous image after a failed low-memory swap.
                if (metadata_image_available[previous_metadata_image_index]) {
                    boxart = ui_components_boxart_init(
                        menu->storage_prefix,
                        menu->load.rom_info.game_code,
                        menu->load.rom_info.title,
                        metadata_image_filename_cache[previous_metadata_image_index]
                    );
                }
                if (boxart == NULL) {
                    menu_show_error(menu, "Could not swap boxart image");
                }
                break;
            }
        }
        new_metadata_image_index = (new_metadata_image_index + direction + metadata_image_filename_cache_length) % metadata_image_filename_cache_length;
    }
}

static component_context_menu_t set_cic_type_context_menu = {
    .get_default_selection = get_rom_cic_override_current_selection,
    .list = {
    {.text = "Automatic", .action = set_cic_type, .arg = (void *) (ROM_CIC_TYPE_AUTOMATIC), .stay = true },
    {.text = "CIC-6101", .action = set_cic_type, .arg = (void *) (ROM_CIC_TYPE_6101), .stay = true },
    {.text = "CIC-7102", .action = set_cic_type, .arg = (void *) (ROM_CIC_TYPE_7102), .stay = true },
    {.text = "CIC-6102 / CIC-7101", .action = set_cic_type, .arg = (void *) (ROM_CIC_TYPE_x102), .stay = true },
    {.text = "CIC-6103 / CIC-7103", .action = set_cic_type, .arg = (void *) (ROM_CIC_TYPE_x103), .stay = true },
    {.text = "CIC-6105 / CIC-7105", .action = set_cic_type, .arg = (void *) (ROM_CIC_TYPE_x105), .stay = true },
    {.text = "CIC-6106 / CIC-7106", .action = set_cic_type, .arg = (void *) (ROM_CIC_TYPE_x106), .stay = true },
    {.text = "Aleck64 CIC-5101", .action = set_cic_type, .arg = (void *) (ROM_CIC_TYPE_5101), .stay = true },
    {.text = "64DD ROM conversion CIC-5167", .action = set_cic_type, .arg = (void *) (ROM_CIC_TYPE_5167), .stay = true },
    {.text = "NDDJ0 64DD IPL", .action = set_cic_type, .arg = (void *) (ROM_CIC_TYPE_8301), .stay = true },
    {.text = "NDDJ1 64DD IPL", .action = set_cic_type, .arg = (void *) (ROM_CIC_TYPE_8302), .stay = true },
    {.text = "NDDJ2 64DD IPL", .action = set_cic_type, .arg = (void *) (ROM_CIC_TYPE_8303), .stay = true },
    {.text = "NDXJ0 64DD IPL", .action = set_cic_type, .arg = (void *) (ROM_CIC_TYPE_8401), .stay = true },
    {.text = "NDDE0 64DD IPL", .action = set_cic_type, .arg = (void *) (ROM_CIC_TYPE_8501), .stay = true },
    COMPONENT_CONTEXT_MENU_LIST_END,
}};

static component_context_menu_t set_save_type_context_menu = {
    .get_default_selection = get_rom_save_override_current_selection,
    .list = {
    { .text = "Automatic", .action = set_save_type, .arg = (void *) (SAVE_TYPE_AUTOMATIC), .stay = true },
    { .text = "None", .action = set_save_type, .arg = (void *) (SAVE_TYPE_NONE), .stay = true },
    { .text = "EEPROM 4kbit", .action = set_save_type, .arg = (void *) (SAVE_TYPE_EEPROM_4KBIT), .stay = true },
    { .text = "EEPROM 16kbit", .action = set_save_type, .arg = (void *) (SAVE_TYPE_EEPROM_16KBIT), .stay = true },
    { .text = "SRAM 256kbit", .action = set_save_type, .arg = (void *) (SAVE_TYPE_SRAM_256KBIT), .stay = true },
    { .text = "SRAM 768kbit / 3 banks", .action = set_save_type, .arg = (void *) (SAVE_TYPE_SRAM_BANKED), .stay = true },
    { .text = "SRAM 1Mbit", .action = set_save_type, .arg = (void *) (SAVE_TYPE_SRAM_1MBIT), .stay = true },
    { .text = "FlashRAM 1Mbit", .action = set_save_type, .arg = (void *) (SAVE_TYPE_FLASHRAM_1MBIT), .stay = true },
    COMPONENT_CONTEXT_MENU_LIST_END,
}};

static component_context_menu_t set_tv_type_context_menu = {
    .get_default_selection = get_rom_tv_override_current_selection,
    .list = {
    { .text = "Automatic", .action = set_tv_type, .arg = (void *) (ROM_TV_TYPE_AUTOMATIC), .stay = true },
    { .text = "PAL", .action = set_tv_type, .arg = (void *) (ROM_TV_TYPE_PAL), .stay = true },
    { .text = "NTSC", .action = set_tv_type, .arg = (void *) (ROM_TV_TYPE_NTSC), .stay = true },
    { .text = "MPAL", .action = set_tv_type, .arg = (void *) (ROM_TV_TYPE_MPAL), .stay = true },
    COMPONENT_CONTEXT_MENU_LIST_END,
}};

static component_context_menu_t set_savestate_options_menu = {
    .get_default_selection = get_rom_savestate_current_selection,
    .list = {
    { .text = "Enabled", .action = set_savestate_option, .arg = (void *) (true), .stay = true},
    { .text = "Disabled", .action = set_savestate_option, .arg = (void *) (false), .stay = true},
    COMPONENT_CONTEXT_MENU_LIST_END,
}};

// SC64SS: the port entries' texts carry what is plugged into each port (vpak_ports_refresh,
// every frame the view runs, so a pak pulled out or put in shows at once)
static char vpak_port_text[4][40] = { "Port 1", "Port 2", "Port 3", "Port 4" };

static component_context_menu_t set_vpak_options_menu = {
    .get_default_selection = get_rom_vpak_current_selection,
    .list = {
    { .text = vpak_port_text[0], .action = set_vpak_option, .arg = (void *) (1), .stay = true},
    { .text = vpak_port_text[1], .action = set_vpak_option, .arg = (void *) (2), .stay = true},
    { .text = vpak_port_text[2], .action = set_vpak_option, .arg = (void *) (3), .stay = true},
    { .text = vpak_port_text[3], .action = set_vpak_option, .arg = (void *) (4), .stay = true},
    { .text = "Off", .action = set_vpak_option, .arg = (void *) (0), .stay = true},
    { .text = "Manage saves", .action = open_vpak_copy, .arg = (void *) (0x100)},
    COMPONENT_CONTEXT_MENU_LIST_END,
}};

static component_context_menu_t set_slowmotion_options_menu = {
    .get_default_selection = get_rom_slowmotion_current_selection,
    .list = {
    { .text = "Enabled", .action = set_slowmotion_option, .arg = (void *) (true), .stay = true},
    { .text = "Disabled", .action = set_slowmotion_option, .arg = (void *) (false), .stay = true},
    COMPONENT_CONTEXT_MENU_LIST_END,
}};

// SC64SS: the engine's default placement is borrowed: no resident hook; the vector-page
// gate and the cart monitor borrow the top of the Expansion Pak for the length of each
// action, so games that use all 8 MiB work too. The Slow motion option puts the hook
// resident at the top of RAM instead (hook_borrowed=0 in the ROM's ini). Codes (Use
// Cheats with a .datel) keep the classic placement: the engine at the top of RAM.

static component_context_menu_t set_cheat_options_menu = {
    .get_default_selection = get_rom_cheat_override_current_selection,
    .list = {
    { .text = "Enabled", .action = set_cheat_option, .arg = (void *) (true), .stay = true},
    { .text = "Disabled", .action = set_cheat_option, .arg = (void *) (false), .stay = true},
    COMPONENT_CONTEXT_MENU_LIST_END,
}};

#ifdef FEATURE_PATCHER_GUI_ENABLED
static component_context_menu_t set_patcher_options_menu = {
    .get_default_selection = get_rom_patch_override_current_selection,
    .list = {
    { .text = "Enabled", .action = set_patcher_option, .arg = (void *) (true), .stay = true},
    { .text = "Disabled", .action = set_patcher_option, .arg = (void *) (false), .stay = true},
    COMPONENT_CONTEXT_MENU_LIST_END,
}};
#endif

static int get_rom_clear_rdram_current_selection (menu_t *menu);

static component_context_menu_t set_clear_rdram_options_menu = {
    .get_default_selection = get_rom_clear_rdram_current_selection,
    .list = {
    { .text = "Enabled", .action = set_clear_rdram_option, .arg = (void *) (true), .stay = true},
    { .text = "Disabled", .action = set_clear_rdram_option, .arg = (void *) (false), .stay = true},
    COMPONENT_CONTEXT_MENU_LIST_END,
}};

static component_context_menu_t options_context_menu = { .list = {
    { .text = "Set CIC Type", .submenu = &set_cic_type_context_menu },
    { .text = "Set Save Type", .submenu = &set_save_type_context_menu },
    { .text = "Set TV Type", .submenu = &set_tv_type_context_menu },
#ifdef FEATURE_AUTOLOAD_ROM_ENABLED
    { .text = "Set ROM to autoload", .action = set_autoload_type, .stay = true },
#endif
    { .text = "Save States", .submenu = &set_savestate_options_menu },
    { .text = "Virtual Controller Pak", .submenu = &set_vpak_options_menu },
    { .text = "Slow motion", .submenu = &set_slowmotion_options_menu },
    { .text = "Hotkeys and Screenshot", .action = open_hotkeys },
    { .text = "Use Cheats", .submenu = &set_cheat_options_menu },
    { .text = "Datel Code Editor", .action = open_datel_code_editor },
#ifdef FEATURE_PATCHER_GUI_ENABLED
    { .text = "Use Patches", .submenu = &set_patcher_options_menu },
#endif
    { .text = "Clear RDRAM on boot", .submenu = &set_clear_rdram_options_menu },
    { .text = "Add to favorites", .action = add_favorite },
    COMPONENT_CONTEXT_MENU_LIST_END,
}};

// Generic helper: search context menu for item matching a target argument value.
// Returns the index of the first matching item, or 0 if not found (default to first).
static int find_menu_item_index_by_arg (const component_context_menu_t *menu, void *target_arg) {
    for (int i = 0; menu->list[i].text != NULL; i++) {
        if (menu->list[i].arg == target_arg) {
            return i;
        }
    }
    return 0; // Not found; default to first item
}

// Default selection helpers: find the menu item matching the current override value
static int get_rom_cic_override_current_selection (menu_t *menu) {
    if (!menu->load.rom_info.boot_override.cic) {
        return 0;
    }
    return find_menu_item_index_by_arg(
        &set_cic_type_context_menu,
        (void *) (menu->load.rom_info.boot_override.cic_type));
}

static int get_rom_save_override_current_selection (menu_t *menu) {
    if (!menu->load.rom_info.boot_override.save) {
        return 0;
    }
    return find_menu_item_index_by_arg(
        &set_save_type_context_menu,
        (void *) (menu->load.rom_info.boot_override.save_type));
}

static int get_rom_tv_override_current_selection (menu_t *menu) {
    if (!menu->load.rom_info.boot_override.tv) {
        return 0;
    }
    return find_menu_item_index_by_arg(
        &set_tv_type_context_menu,
        (void *) (menu->load.rom_info.boot_override.tv_type));
}

static int get_rom_cheat_override_current_selection (menu_t *menu) {
    return find_menu_item_index_by_arg(
        &set_cheat_options_menu,
        (void *) (menu->load.rom_info.settings.cheats_enabled ? true : false));
}

static int get_rom_savestate_current_selection (menu_t *menu) {
    return find_menu_item_index_by_arg(
        &set_savestate_options_menu,
        (void *) (menu->load.rom_info.settings.savestates_enabled ? true : false));
}

static int get_rom_vpak_current_selection (menu_t *menu) {
    int port = menu->load.rom_info.settings.vpak_enabled ? menu->load.rom_info.settings.vpak_port : 0;
    return find_menu_item_index_by_arg(&set_vpak_options_menu, (void *) (uintptr_t) port);
}

static int get_rom_slowmotion_current_selection (menu_t *menu) {
    return find_menu_item_index_by_arg(
        &set_slowmotion_options_menu,
        (void *) (menu->load.rom_info.settings.hook_borrowed ? false : true));
}

#ifdef FEATURE_PATCHER_GUI_ENABLED
static int get_rom_patch_override_current_selection (menu_t *menu) {
    return find_menu_item_index_by_arg(
        &set_patcher_options_menu,
        (void *) (menu->load.rom_info.settings.patches_enabled ? true : false));
}
#endif

static int get_rom_clear_rdram_current_selection (menu_t *menu) {
    return find_menu_item_index_by_arg(
        &set_clear_rdram_options_menu,
        (void *) (menu->load.rom_info.settings.clear_rdram_enabled ? true : false));
}

static bool rom_requires_missing_expansion_pak (menu_t *menu) {
    return (menu->load.rom_info.features.expansion_pak == EXPANSION_PAK_REQUIRED) && !is_memory_expanded();
}

// SC64SS: what each port holds, beside its entry in the Virtual Controller Pak list. A real
// Controller Pak grays the port (and its choice is refused); a Rumble Pak or Transfer Pak is
// noted only, since the virtual pak in that port is the panel's pull-out use (Beetle Adventure
// Racing wants the Rumble Pak at other times); an empty port is noted because a port other
// than 1 needs a controller in it when the game boots.
static void vpak_ports_refresh (void) {
    for (int p = 0; p < 4; p++) {
        const char *note = "";
        bool gray = false;
        if (!joypad_is_connected((joypad_port_t) p)) {
            note = "   no controller";
        } else {
            switch (joypad_get_accessory_type((joypad_port_t) p)) {
                case JOYPAD_ACCESSORY_TYPE_CONTROLLER_PAK: note = "   Controller Pak plugged in"; gray = true; break;
                case JOYPAD_ACCESSORY_TYPE_RUMBLE_PAK: note = "   Rumble Pak plugged in"; break;
                case JOYPAD_ACCESSORY_TYPE_TRANSFER_PAK: note = "   Transfer Pak plugged in"; break;
                default: break;
            }
        }
        snprintf(vpak_port_text[p], sizeof(vpak_port_text[p]), "Port %d%s", p + 1, note);
        set_vpak_options_menu.list[p].gray = gray;
    }
}

static void process (menu_t *menu) {
    vpak_ports_refresh();
    if (options_context_menu.row_selected >= 0) {
        options_last_row = options_context_menu.row_selected;   // SC64SS: where a page's return reopens it
    }
    if (ui_components_context_menu_process(menu, &options_context_menu)) {
        return;
    }

    if (show_expansion_pak_warning) {
        if (menu->actions.enter) {
            show_expansion_pak_warning = false;
            menu->load_pending.rom_file = true;
        } else if (menu->actions.back) {
            show_expansion_pak_warning = false;
            sound_play_effect(SFX_EXIT);
        }
        return;
    }

    if (menu->actions.enter) {
        if (rom_requires_missing_expansion_pak(menu)) {
            show_expansion_pak_warning = true;
            sound_play_effect(SFX_ERROR);
        } else {
            menu->load_pending.rom_file = true;
        }
    } else if (menu->actions.back) {
        sound_play_effect(SFX_EXIT);
        menu->next_mode = MENU_MODE_BROWSER;
    } else if (menu->actions.options) {
        ui_components_context_menu_show(&options_context_menu);
        sound_play_effect(SFX_SETTING);
    } else if (menu->actions.lz_context) {
        if (show_extra_info_message) {
            show_extra_info_message = false;
        } else {
            show_extra_info_message = true;
        }
        sound_play_effect(SFX_SETTING);
    } else if (menu->actions.settings) { // TODO: change to go_right/go_left when those are implemented
        if (show_advanced_info_message) {
            show_advanced_info_message = false;
        } else {
            show_advanced_info_message = true;
        }
        sound_play_effect(SFX_SETTING);
    } else if (menu->actions.go_right) {
        iterate_metadata_image(menu, 1);
        sound_play_effect(SFX_CURSOR);
    } else if (menu->actions.go_left) {
        iterate_metadata_image(menu, -1);
        sound_play_effect(SFX_CURSOR);
    }
}

static void draw (menu_t *menu, surface_t *d) {
    char key_save_text[40], key_load_text[40];
    rdpq_attach(d, NULL);

    ui_components_background_draw();
#ifdef FEATURE_AUTOLOAD_ROM_ENABLED
    if (menu->load_pending.rom_file && menu->settings.loading_progress_bar_enabled) {
        ui_components_loader_draw(0.0f, NULL);
    } else {
#endif
        ui_components_layout_draw();

        ui_components_main_text_draw(
            STL_DEFAULT,
            ALIGN_CENTER, VALIGN_TOP,
            "%s\n"
            "%.20s\n",
            rom_filename,
            menu->load.rom_info.title
        );

        ui_components_main_text_draw(
            STL_DEFAULT,
            ALIGN_LEFT, VALIGN_TOP,
            "\n\n\n\t%.120s\n",
            format_rom_description(menu)
            
        );

        ui_components_main_text_draw(
            STL_DEFAULT,
            ALIGN_LEFT, VALIGN_TOP,
            "\n\n\n\n\n\n\n\n\n\n\n"
            "Save type:\t\t%s\n"
            "TV region:\t\t%s\n"
            "\n"
            "Expansion PAK:\t%s\n"
            "Rumble PAK:\t\t%s\n"
            "Transfer PAK:\t\t%s\n"
            "Save States:\t\t%s, pak %s\n"
            "Hotkeys:\t\t\t%s save, %s load\n"
            "Datel Cheats:\t\t%s\n"
            "Patches:\t\t\t%s\n"
            "Clear RDRAM:\t\t%s\n"
            ,
            
            format_rom_save_type(rom_info_get_save_type(&menu->load.rom_info), menu->load.rom_info.features.controller_pak),
            format_rom_tv_type(rom_info_get_tv_type(&menu->load.rom_info)),
            format_rom_expansion_pak_info(menu->load.rom_info.features.expansion_pak),
            format_rom_pak_feature_info(menu->load.rom_info.features.rumble_pak),
            format_rom_pak_feature_info(menu->load.rom_info.features.transfer_pak),
            format_boolean_type(menu->load.rom_info.settings.savestates_enabled),
            format_vpak_info(&menu->load.rom_info),
            sc64ss_keys_text(sc64ss_key_effective(sc64ss_rom_key(menu, 0), menu->settings.ss_key_save, SC64SS_KEY_DEFAULT_SAVE), key_save_text, sizeof(key_save_text)),
            sc64ss_keys_text(sc64ss_key_effective(sc64ss_rom_key(menu, 1), menu->settings.ss_key_load, SC64SS_KEY_DEFAULT_LOAD), key_load_text, sizeof(key_load_text)),
            format_boolean_type(menu->load.rom_info.settings.cheats_enabled),
            format_boolean_type(menu->load.rom_info.settings.patches_enabled),
            format_boolean_type(menu->load.rom_info.settings.clear_rdram_enabled)
        );

        ui_components_actions_bar_text_draw(
            STL_DEFAULT,
            ALIGN_LEFT, VALIGN_TOP,
            "A: Load and run ROM\n"
            "B: Back\n"
        );

        ui_components_actions_bar_text_draw(
            STL_DEFAULT,
            ALIGN_CENTER, VALIGN_TOP,
            "Start: Adv. Info\n"
            "◀ Change game image ▶\n"
        );

        ui_components_actions_bar_text_draw(
            STL_DEFAULT,
            ALIGN_RIGHT, VALIGN_TOP,
            "L|Z: Extra Info\n"
            "R: Adv. Options\n"
        );

        if (boxart != NULL) {
            ui_components_boxart_draw(boxart);
        }

        if (show_extra_info_message) {
            ui_components_messagebox_draw(
                "EXTRA ROM INFO\n"
                "\n"
                "Title: %.20s\n"
                "Age Rating: %s\n"
                "Players: %u\n"
                "Release Date: %s\n"
                "Author: %s\n"
                "Website: %s\n"
                "License: %s\n"
                "Game code: %c%c%c%c\n"
                "Media type: %s\n"
                "Variant: %s\n"
                "Version: %hhu\n"
                "CIC: %s\n\n\n"
                "Press L|Z to return.\n",
                menu->load.rom_info.title,
                format_age_rating(menu->load.rom_info.meta.age_rating),
                menu->load.rom_info.meta.num_players,
                menu->load.rom_info.meta.release_date,
                menu->load.rom_info.meta.author,
                menu->load.rom_info.meta.website,
                menu->load.rom_info.meta.osi_license,
                menu->load.rom_info.game_code[0], menu->load.rom_info.game_code[1], menu->load.rom_info.game_code[2], menu->load.rom_info.game_code[3],
                format_rom_media_type(menu->load.rom_info.category_code),
                format_rom_destination_market(menu->load.rom_info.destination_code),
                menu->load.rom_info.version,
                format_cic_type(rom_info_get_cic_type(&menu->load.rom_info))
            );
        }

        if (show_advanced_info_message) {
            ui_components_messagebox_draw(
                "ADVANCED ROM INFO\n"
                "\n"
                "Boot address: 0x%08lX\n"
                "SDK version: %.1f%c\n"
                "Clock Rate: %.2fMHz\n"
                "Check code: 0x%016llX\n"
                "Endianness: %s\n\n\n"
                "Press START to return.\n",
                menu->load.rom_info.boot_address,
                (menu->load.rom_info.libultra.version / 10.0f), menu->load.rom_info.libultra.revision,
                menu->load.rom_info.clock_rate,
                menu->load.rom_info.check_code,
                format_rom_endianness(menu->load.rom_info.endianness)
            );
        }

        if (show_expansion_pak_warning) {
            ui_components_messagebox_draw(
                "This ROM requires an Expansion Pak\n"
                "which was not detected.\n\n"
                "It may not run correctly without one.\n\n"
                "A: Continue anyway, B: Cancel\n"
            );
        }

        ui_components_context_menu_draw(&options_context_menu);
#ifdef FEATURE_AUTOLOAD_ROM_ENABLED
    }
#endif

    rdpq_detach_show();
}

static void draw_progress (float progress) {
    surface_t *d = (progress >= 1.0f) ? display_get() : display_try_get();

    if (d) {
        rdpq_attach(d, NULL);

        ui_components_background_draw();

        ui_components_loader_draw(progress, "Loading ROM...");

        rdpq_detach_show();
    }
}

static void draw_creating_save (float progress) {
    surface_t *d = display_get();

    if (d) {
        rdpq_attach(d, NULL);

        ui_components_background_draw();

        ui_components_loader_draw(progress, "Creating initial save file...");

        rdpq_detach_show();
    }
}

static void load (menu_t *menu) {
    debugf("Load ROM: load function called\n");
    cart_load_err_t err;
#ifdef FEATURE_AUTOLOAD_ROM_ENABLED
    if (!menu->settings.loading_progress_bar_enabled) {
        err = cart_load_n64_rom_and_save(menu, NULL, NULL);
    } else  {
        err = cart_load_n64_rom_and_save(menu, draw_progress, draw_creating_save);
    }
#else
    err = cart_load_n64_rom_and_save(menu, draw_progress, draw_creating_save);
#endif

    if (err != CART_LOAD_OK) {
        menu_show_error(menu, cart_load_convert_error_message(err));
        return;
    }

    bookkeeping_history_add(&menu->bookkeeping, menu->load.rom_path, NULL, BOOKKEEPING_TYPE_ROM);

    menu->next_mode = MENU_MODE_BOOT;

    menu->boot_params->device_type = BOOT_DEVICE_TYPE_ROM;
    menu->boot_params->detect_cic_seed = rom_info_get_cic_seed(&menu->load.rom_info, &menu->boot_params->cic_seed);
    switch (rom_info_get_tv_type(&menu->load.rom_info)) {
        case ROM_TV_TYPE_PAL: menu->boot_params->tv_type = BOOT_TV_TYPE_PAL; break;
        case ROM_TV_TYPE_NTSC: menu->boot_params->tv_type = BOOT_TV_TYPE_NTSC; break;
        case ROM_TV_TYPE_MPAL: menu->boot_params->tv_type = BOOT_TV_TYPE_MPAL; break;
        default: menu->boot_params->tv_type = BOOT_TV_TYPE_PASSTHROUGH; break;
    }

    // SC64SS: the Datel engine boots when cheats are enabled (codes) or save states
    // are enabled (the engine's exception path carries the resident hook); both need
    // the Expansion Pak. With codes the engine sits at its classic 0x807C5C00; with
    // save states alone it fits the exception-vector page (cheats.c).
    bool ce_cheats = is_memory_expanded() && menu->load.rom_info.settings.cheats_enabled;
    bool ce_states = is_memory_expanded() && menu->load.rom_info.settings.savestates_enabled;
    bool ce_vpak = is_memory_expanded() && menu->load.rom_info.settings.vpak_enabled;
    // SC64SS: the hotkeys (the ROM's own, else the menu's, else built in) and the screenshot
    // button; a screenshot button alone arms the routine as save states do
    uint16_t ce_key_save = sc64ss_key_effective(sc64ss_rom_key(menu, 0), menu->settings.ss_key_save, SC64SS_KEY_DEFAULT_SAVE);
    uint16_t ce_key_load = sc64ss_key_effective(sc64ss_rom_key(menu, 1), menu->settings.ss_key_load, SC64SS_KEY_DEFAULT_LOAD);
    uint16_t ce_key_panel = sc64ss_key_effective(sc64ss_rom_key(menu, 2), menu->settings.ss_key_panel, SC64SS_KEY_DEFAULT_PANEL);
    uint16_t ce_key_step = sc64ss_key_effective(sc64ss_rom_key(menu, 3), menu->settings.ss_key_step, SC64SS_KEY_DEFAULT_STEP);
    uint16_t ce_key_shot = sc64ss_key_effective(sc64ss_rom_key(menu, 4), menu->settings.ss_key_shot, "");
    bool ce_shots = is_memory_expanded() && (ce_key_shot != 0);
    if (menu->load.rom_info.libdragon_old && (ce_states || ce_vpak || ce_shots)) {
        // SC64SS: libdragon's entry code from before its own boot code copies the game's
        // exception vectors in over the routine's way in; the game never came up with the
        // routine installed (a black screen). It stays out: states, pak and screenshots off.
        debugf("SC64SS: libdragon entry code from before its own boot code: the routine stays out\n");
        ce_states = false;
        ce_vpak = false;
        ce_shots = false;
    }
    if (menu->load.rom_info.libdragon && !menu->load.rom_info.libdragon_handoff && (ce_states || ce_vpak || ce_shots)) {
        // SC64SS: libdragon's boot code, but not the hand-off the routine rides in on (a
        // boot code newer than this menu knows): the routine stays out, the game runs as it
        // always did (boot.c guards the same way)
        debugf("SC64SS: libdragon boot code without the hand-off the routine knows: the routine stays out\n");
        ce_states = false;
        ce_vpak = false;
        ce_shots = false;
    }
    // SC64SS: the codes first. With codes the engine sits at its classic place at the top
    // of RAM and the hook with it (resident, as before borrowed mode); without codes the
    // release's one placement is borrowed.
    uint32_t tmp_cheats[MAX_CHEAT_CODE_ARRAYLIST_SIZE];
    size_t cheat_item_count = 0;
    if (ce_cheats) {
        // SC64SS: load the ROM's .datel file here if nothing is loaded yet, so
        // booting with cheats does not depend on visiting the Datel Code Editor.
        cheat_file_code_t *loaded_codes = get_cheat_codes();
        if ((loaded_codes[0].address == 0) && (loaded_codes[0].value == 0)) {
            path_t *datel_path = path_clone(menu->load.rom_path);
            path_ext_replace(datel_path, "datel");
            if (file_exists(path_get(datel_path))) {
                debugf("Load ROM: auto-loading cheats from %s\n", path_get(datel_path));
                load_cheats_from_file(path_get(datel_path));
            }
            path_free(datel_path);
        }
        cheat_item_count = generate_enabled_cheats_array(get_cheat_codes(), tmp_cheats);
    }
    if (cheat_item_count < 2) {
        // no codes: the list is just its terminator (the engine still boots for the hook)
        tmp_cheats[0] = 0;
        tmp_cheats[1] = 0;
        cheat_item_count = 2;
    }
    bool ce_codes = (cheat_item_count > 2);
    // SC64SS: a built-in list of titles the menu treats specially, by game code (the player
    // sees one placement and no switch). Two things a title can need:
    //  - the watchpoint on writes only. The engine's watchpoint covers reads of the
    //    exception vector at 0x180 as well as writes (Indiana Jones copies the vector's
    //    words and chains to the copy; the watch handler hands it the words it wrote
    //    itself). Rare's Banjo-Kazooie and GoldenEye 007 die at boot with the read watch
    //    on, in either placement: the last Watch exception is libultra's first store to
    //    the vector (the engine's own trigger) and the store is then retried against an
    //    unmapped address, so the relocation goes wrong in a way not understood
    //    yet. With the watch on writes only, as in the first release, both boot.
    //    watch_reads=0 in a ROM's ini does the same for a title not on the list.
    //  - the monitor's register scratch at 0x2A8 instead of 0x010 (the cart monitor parks
    //    ten registers in the vector page on every entry). GoldenEye 007 keeps its own TLB
    //    refill handler at 0x000..0x07F and died at boot with the scratch over it; the
    //    words after the IPL3 epilogue are free in its page. The virtual pak's stub would
    //    sit at 0x060, in the same handler, so such a title gets no virtual pak.
    //  - the resident hook instead of the borrowed engine (no title needs it any more;
    //    the Slow motion option is the player's way to it).
    static const struct { char code[4]; bool watch_reads; bool resident; bool scratch_hi; } sc64ss_titles[] = {
        { {'N', 'B', 'K', 'E'}, false, false, false },     // Banjo-Kazooie
        { {'N', 'G', 'E', 'E'}, false, false, true },      // GoldenEye 007
    };
    bool ce_watch_reads = menu->load.rom_info.settings.watch_reads;
    bool ce_resident_title = false;
    bool ce_scratch_hi = false;
    for (uint32_t k = 0; k < sizeof(sc64ss_titles) / sizeof(sc64ss_titles[0]); k++) {
        if (memcmp(menu->load.rom_info.game_code, sc64ss_titles[k].code, 4) == 0) {
            if (!sc64ss_titles[k].watch_reads) {
                ce_watch_reads = false;
                debugf("SC64SS: %.4s boots with the watchpoint on writes only (a title on the built-in list)\n", sc64ss_titles[k].code);
            }
            if (sc64ss_titles[k].resident) {
                ce_resident_title = true;
                debugf("SC64SS: %.4s takes the resident hook (a title on the built-in list)\n", sc64ss_titles[k].code);
            }
            if (sc64ss_titles[k].scratch_hi) {
                ce_scratch_hi = true;
                debugf("SC64SS: %.4s gets the monitor with its scratch at 0x2A8, and no virtual pak (a title on the built-in list)\n", sc64ss_titles[k].code);
            }
        }
    }
    if (ce_scratch_hi && ce_vpak) {
        ce_vpak = false;                      // the pak stub's home (0x060) is inside the game's own handler
    }
    if (!menu->load.rom_info.settings.watch_reads) {
        debugf("SC64SS: watch_reads=0 in the ini, the watchpoint covers writes only\n");
    }
    // SC64SS: this game's pak file on the card must be a plain one-bank image. A file of another
    // size was copied there by hand (a multi-bank image, a dump with a header) and the launch
    // stops rather than have it replaced, or misread by the game
    if (ce_vpak) {
        char pak_name[48];
        long pak_size = sc64ss_pak_file_size(menu, pak_name, sizeof(pak_name));
        if ((pak_size > 0) && (pak_size != (long) SC64SS_VPAK_LEN)) {
            snprintf(vpak_file_text, sizeof(vpak_file_text),
                     "This game's virtual pak file is not a plain 32 KiB pak image (%ld bytes):\nsd:/savestates/paks/\n%s\nThe launch leaves it alone. Replace it with a one-bank image, or delete it and the game starts with a fresh pak.",
                     pak_size, pak_name);
            menu_show_error(menu, vpak_file_text);
            return;
        }
    }
    // SC64SS: a real Controller Pak in the virtual pak's port would take the game's writes as
    // well as the virtual one: the launch waits until it is out, or the pak set elsewhere
    if (ce_vpak && vpak_port_holds_pak(menu->load.rom_info.settings.vpak_port)) {
        snprintf(vpak_conflict_text, sizeof(vpak_conflict_text),
                 "A Controller Pak is plugged into port %d, where this game's virtual pak goes. The game's writes would land in it.\nTake it out, or set the virtual pak to another port or Off in the ROM's options.",
                 menu->load.rom_info.settings.vpak_port);
        menu_show_error(menu, vpak_conflict_text);
        return;
    }
    menu->boot_params->watch_reads = ce_watch_reads;
    // SC64SS borrowed-RAM mode: no resident hook; the vector-page gate and the cart
    // monitor borrow the hook's home per action, for states and for the virtual pak alike
    // (the monitor serves the pak from the cart). Codes need the classic placement.
    // The Slow motion option (hook_borrowed=0 in the ROM's ini) keeps the resident hook.
    bool ce_borrowed = (ce_states || ce_vpak || ce_shots) && menu->load.rom_info.settings.hook_borrowed && !ce_codes && !ce_resident_title;
    if (!ce_borrowed && (ce_states || ce_vpak || ce_shots) && !ce_codes && !ce_resident_title && sc64ss_full_ram_title(&menu->load.rom_info)) {
        ce_borrowed = true;                   // the resident hook's home is this game's RAM: the borrowed engine, whatever an old ini says
        debugf("SC64SS: %.4s uses all of the Expansion Pak: the borrowed engine, whatever the ini says\n", menu->load.rom_info.game_code);
    }
    if (ce_borrowed && menu->load.rom_info.libdragon) {
        ce_borrowed = false;                  // a libdragon ROM keeps the resident hook: its boot code clears all of
        debugf("SC64SS: libdragon ROM, resident placement\n");   // RAM and the borrowed placement is not settled for it yet
    }
    menu->boot_params->hook_borrowed = ce_borrowed;
    uint32_t ce_slots[SC64SS_SLOTS_MAX];
    uint32_t ce_slots_n = 0;
    int64_t rom_size = 0;
    menu->boot_params->cheat_list = NULL;
    menu->boot_params->hook_blob = NULL;
    menu->boot_params->hook_size = 0;
    menu->boot_params->boot_patches = NULL;
    menu->boot_params->boot_patch_count = 0;
    if (ce_states || ce_vpak || ce_shots) {
        // SC64SS: the engine's furniture on the cart (the frame stash, the hook staging,
        // the virtual pak, the monitor, the stash) sits from SC64SS_FRAME_STASH_PI up; a
        // ROM that reaches it (64 MiB: Conker, Resident Evil 2) boots without the engine.
        rom_size = file_get_size(path_get(menu->load.rom_path));
        if ((rom_size <= 0) || (rom_size > (int64_t) (SC64SS_FRAME_STASH_PI - 0x10000000UL))) {
            debugf("SC64SS: a %lld byte ROM leaves no room on the cart, save states and the virtual pak off\n", rom_size);
            ce_states = false;
            ce_vpak = false;
            ce_shots = false;
        }
    }
    if (ce_states) {
        ce_slots_n = sc64ss_slot_table(rom_size, ce_slots, SC64SS_STATE_SLOT_LEN_B, menu->load.rom_info.libdragon);   // one layout for both placements
        if (ce_slots_n == 0) {
            debugf("SC64SS: no room for a state slot above a %lld byte ROM, save states off\n", rom_size);
            ce_states = false;
        } else if (sc64ss_hook_blob_size > SC64SS_HOOK_STAGING_LEN) {
            debugf("SC64SS: hook blob too large (%lu bytes), save states off\n", sc64ss_hook_blob_size);
            ce_states = false;
        }
    }
    if ((ce_vpak || ce_shots) && (sc64ss_hook_blob_size > SC64SS_HOOK_STAGING_LEN)) {
        ce_vpak = false;
        ce_shots = false;
    }
    if (ce_cheats || ce_states || ce_vpak || ce_shots) {
        if ((cheat_item_count == 2) && !ce_states && !ce_vpak && !ce_shots) {
            debugf("Cheats enabled, but no cheats found\n");
        } else {
            uint32_t *cheats = malloc(cheat_item_count * sizeof(uint32_t));
            if (cheats) {
                memcpy(cheats, tmp_cheats, cheat_item_count * sizeof(uint32_t));
                for (size_t i = 0; i + 1 < cheat_item_count; i += 2) {
                    debugf("Cheat %u: Address: 0x%08lX, Value: 0x%08lX\n", i / 2, cheats[i], cheats[i + 1]);
                }
                debugf("Cheats enabled, %u cheats found\n", cheat_item_count / 2);
                menu->boot_params->cheat_list = cheats;
                if (ce_states || ce_vpak || ce_shots) {
                    menu->boot_params->hook_blob = sc64ss_hook_blob;
                    menu->boot_params->hook_size = sc64ss_hook_blob_size;
                    // SC64SS: stage the hook in cart SDRAM (SC64SS_HOOK_STAGING_PI); the boot
                    // patcher copies it into RDRAM after IPL3 has run, and the reinstall stub
                    // copies it again if the game's boot-time RAM sweep wipes it.
                    data_cache_hit_writeback(sc64ss_hook_blob, sc64ss_hook_blob_size);
                    dma_write(sc64ss_hook_blob, SC64SS_HOOK_STAGING_PI, ((sc64ss_hook_blob_size + 1) & ~1));
                    debugf("SC64SS hook armed, %lu bytes, %lu slots for a %lld byte ROM\n", sc64ss_hook_blob_size, ce_slots_n, rom_size);
                    // SC64SS: patch the hook's config block in the staged copy with the slot
                    // table (the boot patcher copies that copy into RDRAM).
                    if (SC64SS_HOOK_CFG_OFFSET + SC64SS_HOOK_CFG_WORDS * 4 <= sc64ss_hook_blob_size) {
                        static uint32_t sc64ss_cfg[SC64SS_HOOK_CFG_WORDS] __attribute__((aligned(16)));
                        memcpy(sc64ss_cfg, &sc64ss_hook_blob[SC64SS_HOOK_CFG_OFFSET / 4], sizeof(sc64ss_cfg));
                        if (sc64ss_cfg[0] == SC64SS_HOOK_CFG_MAGIC) {
                            for (uint32_t i = 0; i < SC64SS_SLOTS_MAX; i++) {
                                sc64ss_cfg[2 + i] = (i < ce_slots_n) ? ce_slots[i] : 0;
                            }
                            sc64ss_cfg[1] = ce_slots_n;
                            sc64ss_cfg[15] = (uint32_t) rom_size;
                            sc64ss_cfg[10] = ce_key_save;     // hook_cfg.combo_save
                            sc64ss_cfg[11] = ce_key_load;     // hook_cfg.combo_load
                            sc64ss_cfg[24] = ce_key_panel;    // hook_cfg.combo_menu
#if SC64SS_HOOK_CFG_WORDS > 44
                            sc64ss_cfg[40] = ce_key_step;     // hook_cfg.combo_step
                            sc64ss_cfg[41] = ce_key_shot;     // hook_cfg.combo_shot
                            sc64ss_cfg[42] = ce_shots ? sc64ss_prepare_shot_file(menu, sc64ss_cfg) : 0;   // hook_cfg.shot_sectors (+ shot_fill, shot_count)
#endif
                            // Always capture the full 8 MiB. The 4 MiB "half the
                            // freeze" shortcut trusted rom_info's Expansion Pak
                            // flag, but that database is incomplete: Banjo-Tooie
                            // (and any Pak game the DB does not list) fell through
                            // to EXPANSION_PAK_NONE, so its states caught only the
                            // lower half of RAM and the game drifted into a crash a
                            // few seconds after a load. Correctness over 0.7 s.
                            sc64ss_cfg[25] = 0;
                            if (ce_borrowed) {
                                sc64ss_cfg[25] = SC64SS_STATE_IMAGE_LEN_B;   // hook_cfg.image_len: all 8 MiB
                                sc64ss_cfg[27] |= 0x20;                      // hook_cfg.spare bit 5: borrowed
                            }
                            if (ce_states) {
                                sc64ss_prepare_state_files(menu, sc64ss_cfg, ce_slots_n, ce_borrowed);
                            }
                            if (ce_vpak && sc64ss_prepare_pak(menu)) {
                                sc64ss_cfg[27] |= 4;       // hook_cfg.spare bit 2: the virtual pak is in
                                sc64ss_cfg[27] |= (uint32_t) ((menu->load.rom_info.settings.vpak_port - 1) & 3) << 12;   // bits 12..13: its port at launch
                            }
                            data_cache_hit_writeback(sc64ss_cfg, sizeof(sc64ss_cfg));
                            dma_write(sc64ss_cfg, SC64SS_HOOK_STAGING_PI + SC64SS_HOOK_CFG_OFFSET, sizeof(sc64ss_cfg));
                            if (menu->load.rom_info.check_code_from_content) {
                                // the hook tells a ROM by the check code words of its header. A ROM
                                // whose header has none gets the computed one written there (nothing
                                // of the ROM's reads those words), so its states and files agree.
                                static uint32_t sc64ss_ident[4] __attribute__((aligned(16)));
                                sc64ss_ident[0] = (uint32_t) (menu->load.rom_info.check_code >> 32);
                                sc64ss_ident[1] = (uint32_t) (menu->load.rom_info.check_code & 0xFFFFFFFFULL);
                                data_cache_hit_writeback(sc64ss_ident, sizeof(sc64ss_ident));
                                dma_write(sc64ss_ident, 0x10000010, 8);
                            }
                            if (ce_borrowed) {
                                // the monitor, run in place from the cart by the vector-page gate
                                const uint32_t *mon = sc64ss_monitor_blob;
                                static uint32_t sc64ss_monitor_copy[SC64SS_MONITOR_LEN / 4] __attribute__((aligned(16)));
                                if (ce_scratch_hi) {
                                    // the same code with its register scratch at 0x2A8: the words that
                                    // differ, applied to a copy (hook_blob.c, from build.sh)
                                    memcpy(sc64ss_monitor_copy, sc64ss_monitor_blob, sc64ss_monitor_blob_size);
                                    for (uint32_t i = 0; i < sc64ss_monitor_hi_patch_pairs; i++) {
                                        sc64ss_monitor_copy[sc64ss_monitor_hi_patch[2 * i]] = sc64ss_monitor_hi_patch[2 * i + 1];
                                    }
                                    mon = sc64ss_monitor_copy;
                                }
                                data_cache_hit_writeback((void *) mon, sc64ss_monitor_blob_size);
                                dma_write(mon, SC64SS_MONITOR_PI, ((sc64ss_monitor_blob_size + 1) & ~1));
                                debugf("SC64SS: borrowed-RAM mode, monitor %lu bytes at 0x%08lX%s\n", (unsigned long) sc64ss_monitor_blob_size, (unsigned long) SC64SS_MONITOR_PI, ce_scratch_hi ? " (scratch at 0x2A8)" : "");
                            }
                        }
                    }
                }
                // SC64SS: libultra 2.0K+ (Smash Bros, Paper Mario, Pokemon Stadium,
                // Turok 2, Resident Evil 2, Diddy Kong Racing...) writes CP0 WatchLo
                // in __osInitialize_common, which disarms the engine's watchpoint
                // before the game installs its exception handler: neither codes nor
                // the hook then ever run. Find every `mtc0 rt, $18` in the boot segment
                // (the 1 MiB IPL3 copies from ROM 0x1000 to the entry point) and have
                // the boot patcher NOP it after that copy. Nothing else in a game writes
                // WatchLo.
                {
                    static uint32_t sc64ss_boot_patches[2 * 192];
                    uint32_t n_patches = 0;
                    uint8_t *raw = malloc(65536 + 16);
                    if (raw) {
                        uint32_t *abuf = (uint32_t *) (((uintptr_t) raw + 15) & ~(uintptr_t) 15);
                        for (uint32_t off = 0; (off < 0x100000) && (n_patches < 8); off += 65536) {
                            data_cache_hit_writeback_invalidate(abuf, 65536);
                            dma_read(abuf, 0x10001000 + off, 65536);
                            for (uint32_t i = 0; (i < 65536 / 4) && (n_patches < 8); i++) {
                                if ((abuf[i] & 0xFFE0FFFF) == 0x40809000) {
                                    uint32_t ram = (uint32_t) menu->load.rom_info.boot_address + off + 4 * i;
                                    sc64ss_boot_patches[2 * n_patches] = ram;
                                    sc64ss_boot_patches[2 * n_patches + 1] = 0;
                                    n_patches++;
                                    debugf("SC64SS: WatchLo write at 0x%08lX -> nop\n", ram);
                                }
                            }
                        }
                        free(raw);
                    }
                    // SC64SS: games that inflate their libultra at boot (Mario Tennis, Excitebike
                    // 64) write WatchLo from code the scan above cannot see, and the engine's
                    // watchpoint is gone before their vectors go in (README, "The compressed-code
                    // scrub"). For those a 20-word stub at 0x2A8 (the 6105 IPL3's window: these
                    // are 6102/6103 titles) scans the inflated program for `mtc0 rt, $18`, NOPs
                    // every hit (written back, dropped from the I-cache) and goes on where the boot
                    // code was going, hooked where that code leaves its decompressor: Mario Tennis
                    // at its `j 0x80031000`, Excitebike after its four-segment inflate loop (a `jal`;
                    // the stub ends with a `j` to the same routine, ra is the jal's). The site's
                    // word is checked in the ROM first. Not at 0x200: the IPL3 leaves its epilogue
                    // at 0x200..0x2A7 (the routine it runs from RAM once it is done with the RSP's
                    // memories; the menu's patcher hooks its last jump), and Excitebike 64 checks
                    // that its first word is still 0xAC290000: with the stub over it the game set
                    // its cheat-device flag (0x800C3960) and spun a busy loop that grew by 100
                    // iterations every pass, the intro down from 30 to 3 frames a second in two
                    // minutes. 0x2A8..0x2FF is free, and the stub fits when it uses t0..t4.
                    {
                        static const struct { char code[4]; uint32_t site, word, hook, start, end, next; } scrubs[] = {
                            { "NM8E", 0x80300070, 0x0800C400, 0x080000AA, 0x80031000, 0x80200000, 0x0800C400 },
                            { "NMXE", 0x800014E8, 0x0C0006A9, 0x0C0000AA, 0x80300000, 0x80600000, 0x080006A9 },
                        };
                        static uint32_t sbuf[8] __attribute__((aligned(16)));
                        for (uint32_t k = 0; k < sizeof(scrubs) / sizeof(scrubs[0]); k++) {
                            const typeof(scrubs[0]) *s = &scrubs[k];
                            if (memcmp(menu->load.rom_info.game_code, s->code, 4) != 0) continue;
                            debugf("SC64SS: scrub table match %.4s, CIC %d, boot 0x%08lX\n", s->code, (int) rom_info_get_cic_type(&menu->load.rom_info), (unsigned long) (uint32_t) menu->load.rom_info.boot_address);
                            if (rom_info_get_cic_type(&menu->load.rom_info) == ROM_CIC_TYPE_x105) continue;
                            uint32_t rom_off = 0x1000 + (s->site - (uint32_t) menu->load.rom_info.boot_address);
                            data_cache_hit_writeback_invalidate(sbuf, sizeof(sbuf));
                            dma_read(sbuf, (0x10000000 + rom_off) & ~0xFUL, sizeof(sbuf));
                            uint32_t found = sbuf[(rom_off & 0xF) / 4];
                            if (found != s->word) {
                                debugf("SC64SS: scrub site 0x%08lX holds %08lX, not %08lX: no scrub\n", (unsigned long) s->site, (unsigned long) found, (unsigned long) s->word);
                                continue;
                            }
                            // t0..t4: caller-saved, so both sites allow them (Excitebike's site is
                            // a jal to a C function that sets its own arguments; Mario Tennis' is a
                            // jump into the inflated entry with its arguments in a0..a2), and an
                            // interrupt handler saves them, unlike k0/k1 (the exception scratch: a
                            // first stage that runs with interrupts on, Excitebike's, wrecked the
                            // walk's pointers until the stub ran with Status cleared; no need now).
                            const uint32_t stub[20] = {
                                0x3C080000 | (s->start >> 16),          // 0x2A8 lui  t0, hi(start)
                                0x35080000 | (s->start & 0xFFFF),       // 0x2AC ori  t0, t0, lo(start)
                                0x3C090000 | (s->end >> 16),            // 0x2B0 lui  t1, hi(end)
                                0x35290000 | (s->end & 0xFFFF),         // 0x2B4 ori  t1, t1, lo(end)
                                0x3C0BFFE0,                             // 0x2B8 lui  t3, 0xFFE0
                                0x356BFFFF,                             // 0x2BC ori  t3, t3, 0xFFFF     (the mask: rt out)
                                0x3C0C4080,                             // 0x2C0 lui  t4, 0x4080
                                0x358C9000,                             // 0x2C4 ori  t4, t4, 0x9000     (mtc0 rt, $18)
                                0x8D0A0000,                             // 0x2C8 lw   t2, 0(t0)          (L)
                                0x014B5024,                             // 0x2CC and  t2, t2, t3
                                0x154C0004,                             // 0x2D0 bne  t2, t4, next
                                0x00000000,                             // 0x2D4 nop
                                0xAD000000,                             // 0x2D8 sw   zero, 0(t0)        (mtc0 rt, $18 -> nop)
                                0xBD190000,                             // 0x2DC cache Hit_Writeback_D, 0(t0)
                                0xBD100000,                             // 0x2E0 cache Hit_Invalidate_I, 0(t0)
                                0x25080004,                             // 0x2E4 addiu t0, t0, 4        (next)
                                0x1509FFF7,                             // 0x2E8 bne  t0, t1, L
                                0x00000000,                             // 0x2EC nop
                                s->next,                                // 0x2F0 j    where the boot code was going
                                0x00000000,                             // 0x2F4 nop
                            };
                            for (uint32_t i = 0; i < 20; i++) {
                                sc64ss_boot_patches[2 * n_patches] = 0x800002A8 + 4 * i;
                                sc64ss_boot_patches[2 * n_patches + 1] = stub[i];
                                n_patches++;
                            }
                            sc64ss_boot_patches[2 * n_patches] = s->site;
                            sc64ss_boot_patches[2 * n_patches + 1] = s->hook;
                            n_patches++;
                            debugf("SC64SS: compressed-code scrub for %.4s: site 0x%08lX, scan 0x%08lX..0x%08lX, stub at 0x2A8\n", s->code, (unsigned long) s->site, (unsigned long) s->start, (unsigned long) s->end);
                            break;
                        }
                    }
                    // A boot patch can also plant a probe in a game's own code (a generator script
                    // emits such a list: Indiana Jones' page-in handler dumping its registers to the
                    // cart buffer). Nothing of that stays in the build.
#if SC64SS_PDPROBE
                    // SC64SS diagnostics (SC64SS_PDPROBE, build_ce.sh): Perfect Dark boot probe.
                    // A stub at RAM 0x200 (the 6105 IPL3 window; PD reads only its word 0x2E8)
                    // calls osInitialize in PD's place and then writes 'INI2', Cause and Status
                    // to the cart buffer +0x1FD0 through the vector-page PIO writer at 0x130
                    // (borrowed mode), both reached through PD's TLB alias 0x70000000; the
                    // call at 0x8000182C in PD's boot segment is redirected to the stub.
                    if (memcmp(menu->load.rom_info.game_code, "NPDE", 4) == 0) {
                        static const uint32_t pdprobe_stub[] = {
                            0x27BDFFE0, 0xAFBF001C, 0x0C0016D8, 0x00000000, 0x3C04BFFF, 0x24840010,
                            0x3C055F55, 0x0C00004C, 0x24A54E4C, 0x3C054F43, 0x0C00004C, 0x24A54B5F,
                            0x3C04BFFE, 0x24841FD0, 0x3C05494E, 0x0C00004C, 0x24A54932, 0x24840004,
                            0x0C00004C, 0x40056800, 0x24840004, 0x0C00004C, 0x40056000, 0x40084800,
                            0x3C090400, 0x01094021, 0x40885800, 0x8FBF001C, 0x27BD0020, 0x03E00008,
                            0x00000000
                        };
                        static const uint32_t pdprobe_stub2[] = {
                            0x00808025, 0x3C04BFFF, 0x24840010, 0x3C055F55, 0x0C00004C, 0x24A54E4C,
                            0x3C054F43, 0x0C00004C, 0x24A54B5F, 0x3C04BFFE, 0x24841FE0, 0x3C055448,
                            0x0C00004C, 0x24A55231, 0x02002025, 0x080006A9, 0x00000000
                        };
                        for (uint32_t i = 0; i < sizeof(pdprobe_stub) / sizeof(pdprobe_stub[0]); i++) {
                            sc64ss_boot_patches[2 * n_patches] = 0x80000200 + 4 * i;
                            sc64ss_boot_patches[2 * n_patches + 1] = pdprobe_stub[i];
                            n_patches++;
                        }
                        for (uint32_t i = 0; i < sizeof(pdprobe_stub2) / sizeof(pdprobe_stub2[0]); i++) {
                            sc64ss_boot_patches[2 * n_patches] = 0x80000280 + 4 * i;
                            sc64ss_boot_patches[2 * n_patches + 1] = pdprobe_stub2[i];
                            n_patches++;
                        }
                        sc64ss_boot_patches[2 * n_patches] = 0x80001878;    // the main thread's entry -> stub 2 (addiu a2, a2, 0x0280)
                        sc64ss_boot_patches[2 * n_patches + 1] = 0x24C60280;
                        n_patches++;
                        sc64ss_boot_patches[2 * n_patches] = 0x8000182C;
                        sc64ss_boot_patches[2 * n_patches + 1] = 0x0C000080;    // jal 0x70000200
                        n_patches++;
                        debugf("SC64SS: PD boot probe planted (%lu patches)\n", (unsigned long) n_patches);
                    }
#endif
                    menu->boot_params->boot_patches = n_patches ? sc64ss_boot_patches : NULL;
                    menu->boot_params->boot_patch_count = n_patches;
                }
            } else {
                debugf("Failed to allocate memory for cheat list\n");
            }
        }
    } else {
        debugf("Cheats and save states disabled, or Expansion Pak not present\n");
    }

    menu->boot_params->clear_rdram = menu->load.rom_info.settings.clear_rdram_enabled;
}

static void deinit (void) {
    ui_components_boxart_free(boxart);
    boxart = NULL;
    current_metadata_image_index = 0;
    metadata_images_scanned = false;

    // Clear availability cache
    for (uint16_t i = 0; i < metadata_image_filename_cache_length; i++) {
        metadata_image_available[i] = false;
    }
}


// SC64SS: a ROM chosen over USB (usb_comm.c "load-rom"): the view takes the path as its
// own and boots it without the A press, the way the autoload feature does at startup.
// Leaving the current mode first makes the main loop run this view's init again even
// when the menu already sits on a ROM's page.
static bool usb_preset = false;

void view_load_rom_preset (menu_t *menu, path_t *path) {
    if (menu->load.rom_path) {
        rom_info_free_meta(&menu->load.rom_info);
        path_free(menu->load.rom_path);
    }
    menu->load.rom_path = path;
    menu->load.load_history_id = -1;
    menu->load.load_favorite_id = -1;
    usb_preset = true;
    menu->load_pending.rom_file = true;
    if (menu->mode == MENU_MODE_LOAD_ROM) {
        menu->mode = MENU_MODE_NONE;
    }
    menu->next_mode = MENU_MODE_LOAD_ROM;
}

void view_load_rom_init (menu_t *menu) {
    if (usb_preset) {
        usb_preset = false;                 // SC64SS: the path is the view's already
        rom_filename = path_last_get(menu->load.rom_path);
    } else
#ifdef FEATURE_AUTOLOAD_ROM_ENABLED
    if (!menu->settings.rom_autoload_enabled)
#endif
    {
        if (menu->load.rom_path) {
            rom_info_free_meta(&menu->load.rom_info);
            path_free(menu->load.rom_path);
        }

        if(menu->load.load_history_id != -1) {
            menu->load.rom_path = path_clone(menu->bookkeeping.history_items[menu->load.load_history_id].primary_path);
        } else if(menu->load.load_favorite_id != -1) {
            menu->load.rom_path = path_clone(menu->bookkeeping.favorite_items[menu->load.load_favorite_id].primary_path);
        } else {
            menu->load.rom_path = path_clone_push(menu->browser.directory, menu->browser.entry->name);
        }

        rom_filename = path_last_get(menu->load.rom_path);
    } 

    if (show_extra_info_message) {
        show_extra_info_message = false;
    }
    if (show_advanced_info_message) {
        show_advanced_info_message = false;
    }
    show_expansion_pak_warning = false;

    debugf("Load ROM: loading ROM info from %s\n", path_get(menu->load.rom_path));
    rom_err_t err = rom_config_load(menu->load.rom_path, &menu->load.rom_info);
    if (err != ROM_OK) {
        rom_info_free_meta(&menu->load.rom_info);
        path_free(menu->load.rom_path);
        menu->load.rom_path = NULL;
        //disable the attempt at loading the favorite / history
        menu->load.load_history_id = -1;
        menu->load.load_favorite_id = -1;
        // FIXME: use bookkeeping_favorite_remove() here instead of just showing an error and leaving the broken favorite / history item in place
        menu_show_error(menu, convert_error_message(err));
        return;
    }

    if (!is_memory_expanded()) {
        menu->load.rom_info.settings.cheats_enabled = false;
        menu->load.rom_info.settings.savestates_enabled = false;
        menu->load.rom_info.settings.vpak_enabled = false;
    }

    if (menu->load.rom_info.meta.size_limit_exceeded) {
        menu_show_error(menu, "ROM metadata was skipped\nmetadata.ini exceeds size limit");
    }

#ifdef FEATURE_AUTOLOAD_ROM_ENABLED
    if (!menu->settings.rom_autoload_enabled) {
#endif
        current_metadata_image_index = 0;
        boxart = ui_components_boxart_init(menu->storage_prefix, menu->load.rom_info.game_code, menu->load.rom_info.title, IMAGE_BOXART_FRONT);
        ui_components_context_menu_init(&options_context_menu);
        if (options_reopen) {   // SC64SS: back from a page the options menu opened: the menu again, at that row
            options_reopen = false;
            ui_components_context_menu_show(&options_context_menu);
            options_context_menu.row_selected = options_last_row;
        }
#ifdef FEATURE_AUTOLOAD_ROM_ENABLED
    }
#endif

}

void view_load_rom_display (menu_t *menu, surface_t *display) {
    process(menu);

    draw(menu, display);

    if (menu->load_pending.rom_file) {
        menu->load_pending.rom_file = false;
        load(menu);
    }

    if (menu->next_mode != MENU_MODE_LOAD_ROM && menu->next_mode != MENU_MODE_DATEL_CODE_EDITOR) {
        if (menu->next_mode != MENU_MODE_HOTKEYS) {   // SC64SS: the hotkeys page comes back to this ROM,
            menu->load.load_history_id = -1;          // wherever it was opened from
            menu->load.load_favorite_id = -1;
        }
        deinit();
    }
}
