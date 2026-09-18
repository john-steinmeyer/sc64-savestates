/**
 * @file virtual_pak.c
 * @brief The virtual Controller Paks view: every game's pak on the card, its notes beside the
 *        notes on a real Controller Pak in any port, and copies of single notes or whole paks
 *        either way.
 * @ingroup view
 *
 * A game's virtual pak is a plain one-bank pak image, sd:/savestates/paks/<check code>.pak,
 * with the ROM's file name in a text file of the same name beside it (written at launch, so
 * the list can call the pak by its game). Both paks are handled as images: the virtual one
 * read from and written back to its file, the real one read raw from the pak (bank 0) and
 * written back a page at a time, the way the Controller Pak manager backs up and restores.
 * A note moves by copying its pages into free pages of the other image and adding its entry
 * to the note table; the page table and its mirror are resealed. Before the first write to a
 * real pak in a visit its contents go to sd:/cpak_saves. Reached from the browser's Start
 * menu (every pak) or from a game's Virtual Controller Pak options (that game's pak).
 */

#include <dir.h>
#include <libdragon.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include "../fonts.h"
#include "../sound.h"
#include "../ui_components/constants.h"
#include "utils/cpakfs_utils.h"
#include "utils/fs.h"
#include "views.h"

#define PAK_LEN         (32768)
#define PAGE_LEN        (256)
#define PAK_NOTES       (16)
#define FIRST_DATA_PAGE (5)                 // the ID area, the page table, its mirror, the note table's two pages
#define PAK_PAGES       (128 - FIRST_DATA_PAGE)
#define FAT_OFF         (0x100)
#define FAT_MIRROR_OFF  (0x200)
#define DIR_OFF         (0x300)
#define FAT_LAST        (1)                 // a note's last page
#define FAT_FREE        (3)
#define PAKS_DIR        "/savestates/paks"
#define BACKUP_DIR      "/cpak_saves"
#define LIST_ROWS       (15)
#define NOTE_ROWS       (14)
#define LABEL_LEN       (64)
#define TEXT_WIDTH      (VISIBLE_AREA_WIDTH - (TEXT_MARGIN_HORIZONTAL * 2))
#define COLUMN_WIDTH    (TEXT_WIDTH / 2)

enum { SIDE_VIRTUAL = 0, SIDE_REAL = 1 };

typedef struct {
    char name[40];      // the note as the Controller Pak manager shows it: NAME.EXT
    char code[12];      // its game and publisher codes: NKTE.01
    int pages;
    int slot;           // its entry in the note table
    uint16_t start;     // its first page
} note_t;

typedef struct {
    uint8_t img[PAK_LEN] __attribute__((aligned(16)));
    bool ok;            // an image is loaded
    bool fs;            // and it holds a pak file system
    int banks;
    note_t notes[PAK_NOTES];
    int n;
} side_t;

typedef struct {
    char file[24];      // <check code>.pak
    char label[LABEL_LEN];
    int notes;          // -1: the file could not be read
} pak_t;

static pak_t *paks = NULL;
static int paks_n = 0;
static int paks_cap = 0;
static int list_sel = 0;
static int list_top = 0;
static int list_hidden = 0;         // unnamed empty paks left out of the list
static char list_keep[24];          // the pak to stay on across a rebuild
static bool list_screen = true;

static side_t sides[2] __attribute__((aligned(16)));
static char vpath[160];
static char vlabel[LABEL_LEN];
static uint64_t vcode = 0;          // the game's check code, for a fresh image

static int port = -1;               // the port whose real pak is shown (0..3), or none
static bool pak_in[4];
static bool real_backed_up = false; // this visit's first write to the real pak saved its contents
static char backup_name[192];

static int col = SIDE_REAL;         // the column the cursor is in
static int cur[2];                  // the highlighted note per column
static int top[2];                  // the first note shown per column

enum { ASK_NONE, ASK_COPY, ASK_REPLACE, ASK_DELETE_NOTE, ASK_CLONE_TO_VIRTUAL, ASK_CLONE_TO_REAL, ASK_CLONE_TO_REAL_SURE, ASK_DELETE_PAK };
enum { DO_NONE, DO_LIST, DO_READ_REAL, DO_COPY, DO_DELETE_NOTE, DO_CLONE_TO_VIRTUAL, DO_CLONE_TO_REAL, DO_DELETE_PAK };

static int ask = ASK_NONE;
static int todo = DO_NONE;
static int op_side = 0;             // the side of the note an operation is about
static int op_slot = 0;             // its entry
static int op_replace = -1;         // the destination's entry a copy replaces, or none
static char op_name[40];
static char message[320];
static bool message_on = false;
static bool leave_after_message = false;


// The pak's ID block, kept in four copies (0x20, 0x60, 0x80, 0xC0): one with both
// checksums right means a formatted pak, and says how many banks it has.
static bool pak_id_read (const uint8_t *img, int *banks) {
    static const int at[4] = { 0x20, 0x60, 0x80, 0xC0 };
    for (int k = 0; k < 4; k++) {
        const uint8_t *id = img + at[k];
        uint32_t sum = 0;
        for (int i = 0; i < 28; i += 2) {
            sum += (uint32_t) ((id[i] << 8) | id[i + 1]);
        }
        sum &= 0xFFFF;
        uint32_t chk = (uint32_t) ((id[28] << 8) | id[29]);
        uint32_t inv = (uint32_t) ((id[30] << 8) | id[31]);
        if ((chk == sum) && (inv == ((0xFFF2 - sum) & 0xFFFF)) && (id[26] >= 1)) {
            *banks = id[26];
            return true;
        }
    }
    return false;
}

static uint16_t fat_get (const uint8_t *img, int page) {
    return (uint16_t) ((img[FAT_OFF + (2 * page)] << 8) | img[FAT_OFF + (2 * page) + 1]);
}

static void fat_set (uint8_t *img, int page, uint16_t v) {
    img[FAT_OFF + (2 * page)] = (uint8_t) (v >> 8);
    img[FAT_OFF + (2 * page) + 1] = (uint8_t) v;
}

// the page table's checksum byte (the sum of the data pages' entries, as libultra keeps
// it) and its mirror on the next page
static void fat_seal (uint8_t *img) {
    uint32_t sum = 0;
    for (int p = FIRST_DATA_PAGE; p < 128; p++) {
        sum += img[FAT_OFF + (2 * p)] + img[FAT_OFF + (2 * p) + 1];
    }
    img[FAT_OFF] = 0;
    img[FAT_OFF + 1] = (uint8_t) sum;
    memcpy(img + FAT_MIRROR_OFF, img + FAT_OFF, PAGE_LEN);
}

// a note's pages in order; -1 when the chain does not end where it should
static int chain_read (const uint8_t *img, uint16_t start, uint8_t *pages) {
    int n = 0;
    uint32_t p = start;
    while ((n < PAK_PAGES) && (p >= FIRST_DATA_PAGE) && (p < 128)) {
        pages[n++] = (uint8_t) p;
        uint16_t next = fat_get(img, (int) p);
        if (next == FAT_LAST) {
            return n;
        }
        p = next;
    }
    return -1;
}

static int pages_free (const uint8_t *img) {
    int n = 0;
    for (int p = FIRST_DATA_PAGE; p < 128; p++) {
        if (fat_get(img, p) == FAT_FREE) {
            n++;
        }
    }
    return n;
}

static bool entry_used (const uint8_t *e) {
    return (e[0] || e[1] || e[2] || e[3]) && (e[4] || e[5]);
}

static int slot_free (const uint8_t *img) {
    for (int i = 0; i < PAK_NOTES; i++) {
        const uint8_t *e = img + DIR_OFF + (32 * i);
        if (!e[0] && !e[1] && !e[2] && !e[3] && !e[4] && !e[5]) {
            return i;
        }
    }
    return -1;
}

// The note table: 16 entries of 32 bytes, at page 3 of a one-bank pak (after the page
// tables of every bank and their mirrors on a bigger one). Each used entry names the
// game, the publisher, the note and its first page; the page table chains the rest.
static int notes_read (const uint8_t *img, int banks, note_t *out) {
    int n = 0;
    if ((banks < 1) || (banks > 8)) {
        return 0;
    }
    uint32_t dir = 0x100u * (uint32_t) (1 + (2 * banks));
    for (int i = 0; i < PAK_NOTES; i++) {
        const uint8_t *e = img + dir + (32 * i);
        uint32_t start = (uint32_t) ((e[6] << 8) | e[7]);
        if (!entry_used(e) || (start < FIRST_DATA_PAGE) || ((start >> 8) >= (uint32_t) banks) || ((start & 0xFF) >= 128)) {
            continue;
        }
        note_t *note = &out[n++];
        cpakfs_path_t path;
        cpakfs_path_strings_t parts;
        char full[64];
        memcpy(path.gamecode, e, 4);
        memcpy(path.pubcode, e + 4, 2);
        memcpy(path.ext, e + 12, 4);
        memcpy(path.filename, e + 16, 16);
        cpakfs_path_format(&path, full, sizeof(full));
        if (parse_cpakfs_fullname(full, &parts) == 0) {
            snprintf(note->name, sizeof(note->name), "%s%s%s", parts.filename, parts.ext[0] ? "." : "", parts.ext);
            snprintf(note->code, sizeof(note->code), "%s.%s", parts.gamecode, parts.pubcode);
        } else {
            snprintf(note->name, sizeof(note->name), "%s", full);
            note->code[0] = '\0';
        }
        note->slot = i;
        note->start = (uint16_t) start;
        note->pages = 0;
        uint32_t p = start;
        while ((note->pages < (128 * banks)) && ((p & 0xFF) >= 1) && ((p & 0xFF) < 128) && ((p >> 8) < (uint32_t) banks)) {
            note->pages++;
            const uint8_t *f = img + 0x100u + (0x100u * (p >> 8)) + (2 * (p & 0xFF));
            uint32_t next = (uint32_t) ((f[0] << 8) | f[1]);
            if (next == FAT_LAST) {
                break;
            }
            p = next;
        }
    }
    return n;
}

static void side_parse (side_t *s) {
    s->fs = s->ok && pak_id_read(s->img, &s->banks);
    s->n = s->fs ? notes_read(s->img, s->banks, s->notes) : 0;
    for (int c = 0; c < 2; c++) {
        if (&sides[c] == s) {
            if (cur[c] >= s->n) {
                cur[c] = s->n ? (s->n - 1) : 0;
            }
            if (top[c] > cur[c]) {
                top[c] = cur[c];
            }
        }
    }
}

// the note in a table entry with the same game, publisher, name and extension as `entry`
static int slot_same (const side_t *s, const uint8_t *entry) {
    for (int i = 0; i < s->n; i++) {
        const uint8_t *e = s->img + DIR_OFF + (32 * s->notes[i].slot);
        if ((memcmp(e, entry, 6) == 0) && (memcmp(e + 12, entry + 12, 20) == 0)) {
            return s->notes[i].slot;
        }
    }
    return -1;
}

// The note in `src`'s entry `slot` copied into `dst`: its pages into free pages (the lowest
// first), its entry into a free slot with the new first page, the page table resealed.
// `changed` marks the pages written, for the pak.
static bool note_copy (const side_t *src, int slot, uint8_t *dst, bool *changed, char *why, size_t len) {
    const uint8_t *e = src->img + DIR_OFF + (32 * slot);
    uint8_t chain[PAK_PAGES];
    uint8_t out[PAK_PAGES];
    int n = chain_read(src->img, (uint16_t) ((e[6] << 8) | e[7]), chain);
    if (n <= 0) {
        snprintf(why, len, "The note's page chain is damaged, so it was not copied.");
        return false;
    }
    int dst_slot = slot_free(dst);
    if (dst_slot < 0) {
        snprintf(why, len, "No room: the note table there is full (16 notes).");
        return false;
    }
    int free = pages_free(dst);
    if (free < n) {
        snprintf(why, len, "No room: the note needs %d page%s and %d %s free there.", n, (n == 1) ? "" : "s", free, (free == 1) ? "is" : "are");
        return false;
    }
    int k = 0;
    for (int p = FIRST_DATA_PAGE; (p < 128) && (k < n); p++) {
        if (fat_get(dst, p) == FAT_FREE) {
            out[k++] = (uint8_t) p;
        }
    }
    for (int i = 0; i < n; i++) {
        memcpy(dst + (out[i] * PAGE_LEN), src->img + (chain[i] * PAGE_LEN), PAGE_LEN);
        changed[out[i]] = true;
        fat_set(dst, out[i], (i + 1 < n) ? out[i + 1] : FAT_LAST);
    }
    fat_seal(dst);
    changed[1] = true;
    changed[2] = true;
    uint8_t *d = dst + DIR_OFF + (32 * dst_slot);
    memcpy(d, e, 32);
    d[6] = 0;
    d[7] = out[0];
    changed[3 + (dst_slot / 8)] = true;
    return true;
}

// a note's pages freed and its entry cleared, the page table resealed
static void note_remove (uint8_t *img, int slot, bool *changed) {
    uint8_t *e = img + DIR_OFF + (32 * slot);
    uint8_t chain[PAK_PAGES];
    int n = chain_read(img, (uint16_t) ((e[6] << 8) | e[7]), chain);
    for (int i = 0; i < n; i++) {
        fat_set(img, chain[i], FAT_FREE);
    }
    fat_seal(img);
    memset(e, 0, 32);
    changed[1] = true;
    changed[2] = true;
    changed[3 + (slot / 8)] = true;
}

static void label_path (const char *pak_path, char *out, size_t len) {
    snprintf(out, len, "%s", pak_path);
    char *dot = strrchr(out, '.');
    if (dot && (strlen(dot) == 4)) {
        strcpy(dot, ".txt");
    }
}

// the game's file name for a pak, from the text file beside it; false when there is none
static bool label_load (const char *pak_path, char *out, size_t len) {
    char p[160];
    label_path(pak_path, p, sizeof(p));
    FILE *f = fopen(p, "rb");
    if (!f) {
        return false;
    }
    size_t n = fread(out, 1, len - 1, f);
    fclose(f);
    out[n] = '\0';
    char *nl = strpbrk(out, "\r\n");
    if (nl) {
        *nl = '\0';
    }
    char *dot = strrchr(out, '.');
    if (dot && (dot != out)) {
        *dot = '\0';                        // the ROM's extension
    }
    return out[0] != '\0';
}

void sc64ss_vpak_label_store (const char *pak_path, const char *rom_name) {
    char p[160];
    char cur_name[160];
    if (!rom_name || !rom_name[0]) {
        return;
    }
    label_path(pak_path, p, sizeof(p));
    FILE *f = fopen(p, "rb");
    if (f) {
        size_t n = fread(cur_name, 1, sizeof(cur_name) - 1, f);
        fclose(f);
        cur_name[n] = '\0';
        char *nl = strpbrk(cur_name, "\r\n");
        if (nl) {
            *nl = '\0';
        }
        if (strcmp(cur_name, rom_name) == 0) {
            return;                         // the same name already
        }
    }
    f = fopen(p, "wb");
    if (f) {
        fprintf(f, "%s\n", rom_name);
        fclose(f);
    }
}

static void paks_dir (menu_t *menu, char *out, size_t len) {
    snprintf(out, len, "%s%s", menu->storage_prefix, PAKS_DIR);
}

// paks with notes first, by name; the empty and unreadable ones after them
static int pak_compare (const void *a, const void *b) {
    const pak_t *pa = (const pak_t *) a;
    const pak_t *pb = (const pak_t *) b;
    int ga = (pa->notes > 0) ? 0 : 1;
    int gb = (pb->notes > 0) ? 0 : 1;
    if (ga != gb) {
        return ga - gb;
    }
    return strcasecmp(pa->label, pb->label);
}

// every .pak in the paks folder, labelled by its game (else by its first note), sorted;
// an empty pak without a game's name is left out, since nothing says whose it is
static void list_build (menu_t *menu) {
    static uint8_t head[0x500] __attribute__((aligned(16)));
    char dir[160];
    dir_t entry;
    paks_n = 0;
    list_hidden = 0;
    paks_dir(menu, dir, sizeof(dir));
    if (!directory_exists(dir)) {
        return;
    }
    int r = dir_findfirst(dir, &entry);
    while (r == 0) {
        size_t l = strlen(entry.d_name);
        if ((entry.d_type == DT_REG) && (l > 4) && (l < sizeof(paks[0].file)) && (strcasecmp(entry.d_name + l - 4, ".pak") == 0)) {
            if (paks_n == paks_cap) {
                int cap = paks_cap ? (paks_cap * 2) : 32;
                pak_t *grown = realloc(paks, (size_t) cap * sizeof(pak_t));
                if (!grown) {
                    break;
                }
                paks = grown;
                paks_cap = cap;
            }
            pak_t *pak = &paks[paks_n++];
            char path[192];
            snprintf(pak->file, sizeof(pak->file), "%s", entry.d_name);
            snprintf(path, sizeof(path), "%s/%s", dir, entry.d_name);
            pak->notes = -1;
            snprintf(pak->label, sizeof(pak->label), "%s", entry.d_name);
            FILE *f = fopen(path, "rb");
            if (f) {
                if (fread(head, 1, sizeof(head), f) == sizeof(head)) {
                    note_t first[PAK_NOTES];
                    pak->notes = notes_read(head, 1, first);
                    if (!label_load(path, pak->label, sizeof(pak->label))) {
                        if (pak->notes > 0) {
                            snprintf(pak->label, sizeof(pak->label), "%s", first[0].name);
                        } else {
                            paks_n--;
                            list_hidden++;
                        }
                    }
                }
                fclose(f);
            }
        }
        r = dir_findnext(dir, &entry);
    }
    if (paks_n > 1) {
        qsort(paks, (size_t) paks_n, sizeof(pak_t), pak_compare);
    }
    for (int i = 0; list_keep[0] && (i < paks_n); i++) {
        if (strcmp(paks[i].file, list_keep) == 0) {
            list_sel = i;
        }
    }
    list_keep[0] = '\0';
    if (list_sel >= paks_n) {
        list_sel = paks_n ? (paks_n - 1) : 0;
    }
    if (list_sel < list_top) {
        list_top = list_sel;
    } else if (list_sel >= (list_top + LIST_ROWS)) {
        list_top = list_sel - LIST_ROWS + 1;
    }
}

static void virtual_load (void) {
    side_t *vs = &sides[SIDE_VIRTUAL];
    vs->ok = false;
    FILE *f = fopen(vpath, "rb");
    if (f) {
        vs->ok = (fread(vs->img, 1, PAK_LEN, f) == PAK_LEN);
        fclose(f);
    }
    side_parse(vs);
}

// the virtual pak's image to its file (made, with the folders and the game's name, when new)
static bool virtual_commit (menu_t *menu, char *why, size_t len) {
    bool existed = file_exists(vpath);
    if (!existed) {
        char dir[160];
        snprintf(dir, sizeof(dir), "%s/savestates", menu->storage_prefix);
        if (!directory_exists(dir)) {
            directory_create(dir);
        }
        paks_dir(menu, dir, sizeof(dir));
        if (!directory_exists(dir)) {
            directory_create(dir);
        }
    }
    FILE *f = fopen(vpath, existed ? "r+b" : "wb");
    if (!f) {
        snprintf(why, len, "Could not open %s for writing.", vpath);
        return false;
    }
    bool ok = (fwrite(sides[SIDE_VIRTUAL].img, 1, PAK_LEN, f) == PAK_LEN);
    fclose(f);
    if (!ok) {
        snprintf(why, len, "Writing %s failed.", vpath);
        return false;
    }
    if (!existed && menu->vpak_view.from_rom && menu->load.rom_path) {
        sc64ss_vpak_label_store(vpath, path_last_get(menu->load.rom_path));
    }
    return true;
}

static int port_next (int from, int step) {
    for (int k = 1; k <= 4; k++) {
        int p = (from + (step * k) + 8) % 4;
        if (pak_in[p]) {
            return p;
        }
    }
    return -1;
}

static bool real_read (char *why, size_t len) {
    side_t *rs = &sides[SIDE_REAL];
    rs->ok = false;
    if (port < 0) {
        side_parse(rs);
        snprintf(why, len, "No Controller Pak in any port.");
        return false;
    }
    int got = cpak_read((joypad_port_t) port, 0, 0, rs->img, PAK_LEN);
    rs->ok = (got == PAK_LEN);
    side_parse(rs);
    if (!rs->ok) {
        snprintf(why, len, "The Controller Pak in port %d could not be read (%d).", port + 1, got);
        return false;
    }
    return true;
}

// the real pak's contents before this visit's first write to it: sd:/cpak_saves/CPAK_<time>.pak,
// the Controller Pak manager's own naming, or a numbered file without a clock
static bool real_backup (menu_t *menu, char *why, size_t len) {
    char dir[160];
    if (real_backed_up) {
        return true;
    }
    snprintf(dir, sizeof(dir), "%s%s", menu->storage_prefix, BACKUP_DIR);
    if (!directory_exists(dir)) {
        directory_create(dir);
    }
    backup_name[0] = '\0';
    if (menu->current_time >= 0) {
        time_t t = menu->current_time;
        struct tm tm = *localtime(&t);
        snprintf(backup_name, sizeof(backup_name), "%s/CPAK_%04d-%02d-%02d_%02d%02d%02d.pak", dir, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
        if (file_exists(backup_name)) {
            backup_name[0] = '\0';
        }
    }
    for (int n = 1; !backup_name[0] && (n < 1000); n++) {
        snprintf(backup_name, sizeof(backup_name), "%s/CPAK_before_copy_%d.pak", dir, n);
        if (file_exists(backup_name)) {
            backup_name[0] = '\0';
        }
    }
    FILE *f = fopen(backup_name, "wb");
    bool saved = f && (fwrite(sides[SIDE_REAL].img, 1, PAK_LEN, f) == PAK_LEN);
    if (f) {
        fclose(f);
    }
    if (!saved) {
        snprintf(why, len, "The Controller Pak's contents could not be backed up to %s, so it was left alone.", backup_name);
        return false;
    }
    real_backed_up = true;
    return true;
}

// the changed pages of the real image written to the pak (the note table last for a copy,
// first for a removal), then the whole bank read back and compared
static bool real_commit (const bool *changed, bool dir_first, char *why, size_t len) {
    side_t *rs = &sides[SIDE_REAL];
    static uint8_t check[PAK_LEN] __attribute__((aligned(16)));
    for (int pass = 0; pass < 2; pass++) {
        for (int p = 0; p < 128; p++) {
            bool table = (p >= 3) && (p < FIRST_DATA_PAGE);
            if (!changed[p] || (table != (dir_first ? (pass == 0) : (pass == 1)))) {
                continue;
            }
            int put = cpak_write((joypad_port_t) port, 0, (uint16_t) (p * PAGE_LEN), rs->img + (p * PAGE_LEN), PAGE_LEN);
            if (put != PAGE_LEN) {
                snprintf(why, len, "Writing page %d of the Controller Pak in port %d failed (%d).%s%s", p, port + 1, put,
                         backup_name[0] ? " A backup of its original contents is at " : "", backup_name[0] ? backup_name : "");
                return false;
            }
        }
    }
    int got = cpak_read((joypad_port_t) port, 0, 0, check, PAK_LEN);
    if ((got != PAK_LEN) || (memcmp(check, rs->img, PAK_LEN) != 0)) {
        snprintf(why, len, "The Controller Pak in port %d did not read back what was written.%s%s", port + 1,
                 backup_name[0] ? " A backup of its original contents is at " : "", backup_name[0] ? backup_name : "");
        return false;
    }
    return true;
}

static const char *side_name (int side) {
    return (side == SIDE_REAL) ? "real pak" : "virtual pak";
}

// the real pak as a destination or a subject of a change: present, readable, one bank, formatted
static bool real_writable (char *why, size_t len) {
    if (!real_read(why, len)) {
        return false;
    }
    side_t *rs = &sides[SIDE_REAL];
    if (!rs->fs) {
        snprintf(why, len, "The Controller Pak in port %d holds no pak file system. Format it in the Controller Pak manager first.", port + 1);
        return false;
    }
    if (rs->banks > 1) {
        snprintf(why, len, "That Controller Pak has %d banks; notes are moved on one-bank paks only.", rs->banks);
        return false;
    }
    return true;
}

// a fresh virtual pak image for a game that has no file yet
static bool virtual_fresh (char *why, size_t len) {
    side_t *vs = &sides[SIDE_VIRTUAL];
    if (!vcode) {
        snprintf(why, len, "This pak has no file and no game to make one for.");
        return false;
    }
    sc64ss_pak_format(vs->img, (uint32_t) (vcode >> 32), (uint32_t) (vcode & 0xFFFFFFFFULL));
    vs->ok = true;
    side_parse(vs);
    return true;
}

static bool do_copy (menu_t *menu, char *why, size_t len) {
    static bool changed[128];
    int dst = 1 - op_side;
    side_t *src = &sides[op_side];
    side_t *dside = &sides[dst];
    memset(changed, 0, sizeof(changed));
    if (dst == SIDE_REAL) {
        // the pak read again first: it may have been swapped since the columns were drawn
        if (!real_writable(why, len) || !real_backup(menu, why, len)) {
            return false;
        }
        if (op_side == SIDE_REAL) {
            return false;
        }
    } else if (!dside->ok && !virtual_fresh(why, len)) {
        return false;
    } else if (!dside->fs) {
        snprintf(why, len, "This game's pak file is not a formatted pak, so nothing was copied to it.");
        return false;
    }
    if (op_slot >= PAK_NOTES) {
        return false;
    }
    if (op_replace >= 0) {
        note_remove(dside->img, op_replace, changed);
    }
    if (!note_copy(src, op_slot, dside->img, changed, why, len)) {
        if (op_replace >= 0) {
            // the removal stays undone: the image is reloaded from where it came
            if (dst == SIDE_REAL) {
                real_read(why + strlen(why), 0);
            } else {
                virtual_load();
            }
        }
        return false;
    }
    bool ok = (dst == SIDE_REAL) ? real_commit(changed, false, why, len) : virtual_commit(menu, why, len);
    if (dst == SIDE_REAL) {
        real_read(why + (ok ? 0 : strlen(why)), ok ? len : 0);
        if (ok) {
            snprintf(why, len, "Copied %s onto the real pak in port %d.%s%s", op_name, port + 1,
                     backup_name[0] ? " A backup of its original contents is at " : "", backup_name[0] ? backup_name : "");
        }
    } else {
        side_parse(dside);
        if (ok) {
            snprintf(why, len, "Copied %s into this game's virtual pak.", op_name);
        }
    }
    return ok;
}

static bool do_delete_note (menu_t *menu, char *why, size_t len) {
    static bool changed[128];
    side_t *s = &sides[op_side];
    memset(changed, 0, sizeof(changed));
    if (op_side == SIDE_REAL) {
        if (!real_writable(why, len) || !real_backup(menu, why, len)) {
            return false;
        }
    } else if (!s->fs) {
        snprintf(why, len, "This game's pak file is not a formatted pak.");
        return false;
    }
    if (op_slot >= PAK_NOTES) {
        return false;
    }
    note_remove(s->img, op_slot, changed);
    bool ok = (op_side == SIDE_REAL) ? real_commit(changed, true, why, len) : virtual_commit(menu, why, len);
    if (op_side == SIDE_REAL) {
        real_read(why + (ok ? 0 : strlen(why)), ok ? len : 0);
    } else {
        side_parse(s);
    }
    if (ok) {
        snprintf(why, len, "Deleted %s from the %s.", op_name, side_name(op_side));
    }
    return ok;
}

static bool do_clone_to_virtual (menu_t *menu, char *why, size_t len) {
    side_t *rs = &sides[SIDE_REAL];
    side_t *vs = &sides[SIDE_VIRTUAL];
    if (!real_read(why, len)) {
        return false;
    }
    if (!rs->fs) {
        snprintf(why, len, "The Controller Pak in port %d holds no pak file system, so a game would ask to repair it. Format it in the Controller Pak manager, or save something on it, first.", port + 1);
        return false;
    }
    if (rs->banks > 1) {
        snprintf(why, len, "That Controller Pak has %d banks. A virtual pak holds one, so its contents would not fit.", rs->banks);
        return false;
    }
    memcpy(vs->img, rs->img, PAK_LEN);
    vs->ok = true;
    if (!virtual_commit(menu, why, len)) {
        virtual_load();
        return false;
    }
    side_parse(vs);
    snprintf(why, len, "Done. This game's virtual pak is a copy of the Controller Pak in port %d: %d note%s.", port + 1, vs->n, (vs->n == 1) ? "" : "s");
    return true;
}

static bool do_clone_to_real (menu_t *menu, char *why, size_t len) {
    side_t *rs = &sides[SIDE_REAL];
    side_t *vs = &sides[SIDE_VIRTUAL];
    static bool changed[128];
    if (!vs->ok || !vs->fs) {
        snprintf(why, len, "This game has no formatted pak file, so there is nothing to copy.");
        return false;
    }
    if (!real_read(why, len)) {
        return false;
    }
    if (rs->fs && (rs->banks > 1)) {
        snprintf(why, len, "That Controller Pak has %d banks. This copy writes one-bank paks only, so it was left alone.", rs->banks);
        return false;
    }
    if (!real_backup(menu, why, len)) {
        return false;
    }
    memcpy(rs->img, vs->img, PAK_LEN);
    for (int p = 0; p < 128; p++) {
        changed[p] = true;
    }
    bool ok = real_commit(changed, false, why, len);
    real_read(why + (ok ? 0 : strlen(why)), ok ? len : 0);
    if (ok) {
        snprintf(why, len, "Done. The Controller Pak in port %d is a copy of this game's virtual pak.%s%s", port + 1,
                 backup_name[0] ? " A backup of its original contents is at " : "", backup_name[0] ? backup_name : "");
    }
    return ok;
}

static bool do_delete_pak (char *why, size_t len) {
    char p[160];
    if (file_exists(vpath) && (remove(vpath) != 0)) {
        snprintf(why, len, "Could not delete %s.", vpath);
        return false;
    }
    label_path(vpath, p, sizeof(p));
    if (file_exists(p)) {
        remove(p);
    }
    sides[SIDE_VIRTUAL].ok = false;
    side_parse(&sides[SIDE_VIRTUAL]);
    snprintf(why, len, "Deleted. The game starts with an empty pak the next time it is launched with the Virtual Controller Pak on.");
    return true;
}

static void show_message (const char *text) {
    snprintf(message, sizeof(message), "%s", text);
    message_on = true;
}

static void leave (menu_t *menu) {
    menu->next_mode = menu->vpak_view.from_rom ? MENU_MODE_LOAD_ROM : MENU_MODE_BROWSER;
}

static void open_pak (menu_t *menu, int index) {
    char dir[160];
    paks_dir(menu, dir, sizeof(dir));
    snprintf(vpath, sizeof(vpath), "%s/%s", dir, paks[index].file);
    snprintf(vlabel, sizeof(vlabel), "%s", paks[index].label);
    vcode = 0;
    if (strlen(paks[index].file) == 20) {
        vcode = strtoull(paks[index].file, NULL, 16);   // <check code>.pak: the code, for a fresh image
    }
    cur[0] = cur[1] = 0;
    top[0] = top[1] = 0;
    col = SIDE_VIRTUAL;
    virtual_load();
    list_screen = false;
}

// the highlighted note of the active column, or NULL
static const note_t *note_at_cursor (void) {
    side_t *s = &sides[col];
    if ((s->n == 0) || (cur[col] >= s->n)) {
        return NULL;
    }
    return &s->notes[cur[col]];
}

static void opt_delete_note (menu_t *menu, void *arg) {
    (void) menu;
    (void) arg;
    const note_t *note = note_at_cursor();
    if (!note) {
        show_message("No note is highlighted.");
        return;
    }
    op_side = col;
    op_slot = note->slot;
    snprintf(op_name, sizeof(op_name), "%s", note->name);
    ask = ASK_DELETE_NOTE;
}

static void opt_clone_to_virtual (menu_t *menu, void *arg) {
    (void) menu;
    (void) arg;
    if (port < 0) {
        show_message("No Controller Pak in any port.");
        return;
    }
    ask = ASK_CLONE_TO_VIRTUAL;
}

static void opt_clone_to_real (menu_t *menu, void *arg) {
    (void) menu;
    (void) arg;
    if (port < 0) {
        show_message("No Controller Pak in any port.");
        return;
    }
    if (!sides[SIDE_VIRTUAL].ok) {
        show_message("This game has no pak file yet, so there is nothing to copy onto the real pak.");
        return;
    }
    ask = ASK_CLONE_TO_REAL;
}

static void opt_delete_pak (menu_t *menu, void *arg) {
    (void) menu;
    (void) arg;
    if (!sides[SIDE_VIRTUAL].ok && !file_exists(vpath)) {
        show_message("This game has no pak file yet.");
        return;
    }
    ask = ASK_DELETE_PAK;
}

static component_context_menu_t options_menu = { .list = {
    { .text = "Delete this note", .action = opt_delete_note },
    { .text = "Clone real pak > virtual pak", .action = opt_clone_to_virtual },
    { .text = "Clone virtual pak > real pak", .action = opt_clone_to_real },
    { .text = "Delete the virtual pak", .action = opt_delete_pak },
    COMPONENT_CONTEXT_MENU_LIST_END,
}};

static void process (menu_t *menu) {
    // the real paks in the ports, as they come and go
    for (int p = 0; p < 4; p++) {
        pak_in[p] = (joypad_get_accessory_type((joypad_port_t) p) == JOYPAD_ACCESSORY_TYPE_CONTROLLER_PAK);
    }
    if ((port >= 0) && !pak_in[port]) {
        port = -1;
        real_backed_up = false;
        sides[SIDE_REAL].ok = false;
        side_parse(&sides[SIDE_REAL]);
    }
    if ((port < 0) && !list_screen && (todo == DO_NONE)) {
        port = port_next(3, 1);
        if (port >= 0) {
            todo = DO_READ_REAL;
        }
    }
    if (todo != DO_NONE) {
        return;
    }

    if (message_on) {
        if (menu->actions.enter || menu->actions.back) {
            message_on = false;
            if (leave_after_message) {
                leave_after_message = false;
                leave(menu);
            }
        }
        return;
    }

    if (ask != ASK_NONE) {
        if (menu->actions.enter) {
            sound_play_effect(SFX_ENTER);
            switch (ask) {
                case ASK_COPY: todo = DO_COPY; ask = ASK_NONE; break;
                case ASK_REPLACE: todo = DO_COPY; ask = ASK_NONE; break;
                case ASK_DELETE_NOTE: todo = DO_DELETE_NOTE; ask = ASK_NONE; break;
                case ASK_CLONE_TO_VIRTUAL: todo = DO_CLONE_TO_VIRTUAL; ask = ASK_NONE; break;
                case ASK_CLONE_TO_REAL: ask = ASK_CLONE_TO_REAL_SURE; break;
                case ASK_CLONE_TO_REAL_SURE: todo = DO_CLONE_TO_REAL; ask = ASK_NONE; break;
                case ASK_DELETE_PAK: todo = DO_DELETE_PAK; ask = ASK_NONE; break;
                default: ask = ASK_NONE; break;
            }
        } else if (menu->actions.back) {
            sound_play_effect(SFX_EXIT);
            ask = ASK_NONE;
        }
        return;
    }

    if (list_screen) {
        if (menu->actions.go_up && paks_n) {
            list_sel = (list_sel + paks_n - 1) % paks_n;
            sound_play_effect(SFX_CURSOR);
        } else if (menu->actions.go_down && paks_n) {
            list_sel = (list_sel + 1) % paks_n;
            sound_play_effect(SFX_CURSOR);
        } else if (menu->actions.enter && paks_n) {
            sound_play_effect(SFX_ENTER);
            open_pak(menu, list_sel);
            if (!sides[SIDE_VIRTUAL].ok) {
                show_message("This pak's file could not be read.");
            }
        } else if (menu->actions.back) {
            sound_play_effect(SFX_EXIT);
            leave(menu);
        }
        if (list_sel < list_top) {
            list_top = list_sel;
        } else if (list_sel >= (list_top + LIST_ROWS)) {
            list_top = list_sel - LIST_ROWS + 1;
        }
        return;
    }

    if (ui_components_context_menu_process(menu, &options_menu)) {
        return;
    }

    side_t *s = &sides[col];
    if (menu->actions.go_up && s->n) {
        cur[col] = (cur[col] + s->n - 1) % s->n;
        sound_play_effect(SFX_CURSOR);
    } else if (menu->actions.go_down && s->n) {
        cur[col] = (cur[col] + 1) % s->n;
        sound_play_effect(SFX_CURSOR);
    } else if (menu->actions.go_left || menu->actions.go_right) {
        col = 1 - col;
        sound_play_effect(SFX_CURSOR);
    } else if (menu->actions.enter) {
        const note_t *note = note_at_cursor();
        int dst = 1 - col;
        sound_play_effect(SFX_ENTER);
        if (!note) {
            show_message((s->n == 0) ? "No note here to copy. Left or right moves to the other pak." : "No note is highlighted.");
        } else if ((dst == SIDE_REAL) && (port < 0)) {
            show_message("No Controller Pak in any port. Plug one into a controller.");
        } else if ((dst == SIDE_REAL) && !sides[SIDE_REAL].fs) {
            show_message("The real pak holds no pak file system. Format it in the Controller Pak manager first.");
        } else {
            op_side = col;
            op_slot = note->slot;
            snprintf(op_name, sizeof(op_name), "%s", note->name);
            op_replace = sides[dst].fs ? slot_same(&sides[dst], s->img + DIR_OFF + (32 * note->slot)) : -1;
            ask = (op_replace >= 0) ? ASK_REPLACE : ASK_COPY;
        }
    } else if (menu->actions.lz_context) {
        int next = port_next((port < 0) ? 0 : port, 1);
        if ((next >= 0) && (next != port)) {
            sound_play_effect(SFX_SETTING);
            port = next;
            real_backed_up = false;
            todo = DO_READ_REAL;
        }
    } else if (menu->actions.options) {
        sound_play_effect(SFX_SETTING);
        ui_components_context_menu_show(&options_menu);
    } else if (menu->actions.back) {
        sound_play_effect(SFX_EXIT);
        if (menu->vpak_view.from_rom) {
            leave(menu);
        } else {
            if (list_sel < paks_n) {
                snprintf(list_keep, sizeof(list_keep), "%s", paks[list_sel].file);
            }
            list_screen = true;
            todo = DO_LIST;                 // its name and count may have changed
        }
    }
    if (cur[col] < top[col]) {
        top[col] = cur[col];
    } else if (cur[col] >= (top[col] + NOTE_ROWS)) {
        top[col] = cur[col] - NOTE_ROWS + 1;
    }
}

// a block of text in the main area, from x pixels in, width pixels wide (a column)
static void text_draw (int x, int width, menu_font_type_t style, rdpq_align_t align, const char *text) {
    rdpq_text_printn(
        &(rdpq_textparms_t) {
            .style_id = style,
            .width = width,
            .height = LAYOUT_ACTIONS_SEPARATOR_Y - OVERSCAN_HEIGHT - (TEXT_MARGIN_VERTICAL * 2),
            .align = align,
            .valign = VALIGN_TOP,
            .wrap = WRAP_ELLIPSES,
            .line_spacing = TEXT_LINE_SPACING_ADJUST,
        },
        FNT_DEFAULT,
        VISIBLE_AREA_X0 + TEXT_MARGIN_HORIZONTAL + x,
        VISIBLE_AREA_Y0 + TEXT_MARGIN_VERTICAL + TEXT_OFFSET_VERTICAL,
        text,
        strlen(text)
    );
}

static size_t add (char *buf, size_t size, size_t used, const char *fmt, ...) {
    va_list va;
    va_start(va, fmt);
    int n = vsnprintf(buf + used, (used < size) ? (size - used) : 0, fmt, va);
    va_end(va);
    return (n > 0) ? (used + (size_t) n) : used;
}

static void draw_list (menu_t *menu) {
    static char left[LIST_ROWS * (LABEL_LEN + 8) + 64];
    static char right[LIST_ROWS * 16 + 64];
    size_t l = 0, r = 0;
    (void) menu;
    ui_components_main_text_draw(STL_DEFAULT, ALIGN_CENTER, VALIGN_TOP, "VIRTUAL CONTROLLER PAKS\n");
    l = add(left, sizeof(left), l, "\n\n");
    r = add(right, sizeof(right), r, "\n\n");
    if ((paks_n == 0) && !list_hidden) {
        ui_components_main_text_draw(STL_DEFAULT, ALIGN_LEFT, VALIGN_TOP,
            "\n\nNo virtual paks yet.\n\n"
            "A game makes its own the first time it starts with the Virtual Controller Pak on. "
            "To put saves from a real pak into a game's before that, open the game's options, Virtual Controller Pak, Manage saves.");
    }
    for (int i = list_top; (i < paks_n) && (i < (list_top + LIST_ROWS)); i++) {
        l = add(left, sizeof(left), l, "%s %s\n", (i == list_sel) ? ">" : " ", paks[i].label);
        if (paks[i].notes < 0) {
            r = add(right, sizeof(right), r, "unreadable\n");
        } else if (paks[i].notes == 0) {
            r = add(right, sizeof(right), r, "empty\n");
        } else {
            r = add(right, sizeof(right), r, "%d note%s\n", paks[i].notes, (paks[i].notes == 1) ? "" : "s");
        }
    }
    if (list_hidden) {
        int shown = (paks_n < LIST_ROWS) ? paks_n : LIST_ROWS;
        for (int i = shown; i < LIST_ROWS; i++) {
            l = add(left, sizeof(left), l, "\n");
        }
        l = add(left, sizeof(left), l, "\n%d empty pak%s without a game's name not shown\n", list_hidden, (list_hidden == 1) ? "" : "s");
    }
    if (paks_n || list_hidden) {
        text_draw(0, TEXT_WIDTH - 20, STL_DEFAULT, ALIGN_LEFT, left);
        text_draw(0, TEXT_WIDTH - 20, STL_GRAY, ALIGN_RIGHT, right);
    }
    if (paks_n > LIST_ROWS) {
        ui_components_list_scrollbar_draw(list_top, paks_n, LIST_ROWS);
    }
    ui_components_actions_bar_text_draw(STL_DEFAULT, ALIGN_LEFT, VALIGN_TOP, "A: Open\nB: Back");
}

// a column's note rows: the names with the cursor, and the codes with the page counts
static void column_notes (int side, char *names, size_t nsize, char *sizes, size_t ssize) {
    const side_t *s = &sides[side];
    size_t a = 0, b = 0;
    names[0] = '\0';
    sizes[0] = '\0';
    for (int i = top[side]; (i < s->n) && (i < (top[side] + NOTE_ROWS)); i++) {
        bool here = (side == col) && (i == cur[side]);
        a = add(names, nsize, a, "%s%s\n", here ? "> " : "  ", s->notes[i].name);
        b = add(sizes, ssize, b, "%s  %d\n", s->notes[i].code, s->notes[i].pages);
    }
    if (s->n > (top[side] + NOTE_ROWS)) {
        a = add(names, nsize, a, "  and %d more\n", s->n - (top[side] + NOTE_ROWS));
    }
}

static void draw_pak (menu_t *menu) {
    static char names[PAK_NOTES * 48];
    static char sizes[PAK_NOTES * 24];
    static char block[PAK_NOTES * 48 + 8];
    char head[96];
    char counts[96];
    int ports_with = 0;
    (void) menu;
    for (int p = 0; p < 4; p++) {
        ports_with += pak_in[p] ? 1 : 0;
    }

    ui_components_main_text_draw(STL_DEFAULT, ALIGN_CENTER, VALIGN_TOP, "%s\n", vlabel);

    // the virtual pak: a header, a counts line, the notes
    const side_t *vs = &sides[SIDE_VIRTUAL];
    if (vs->ok && vs->fs) {
        snprintf(counts, sizeof(counts), "\n\n\n%d note%s, %d of %d pages\n", vs->n, (vs->n == 1) ? "" : "s", PAK_PAGES - pages_free(vs->img), PAK_PAGES);
        column_notes(SIDE_VIRTUAL, names, sizeof(names), sizes, sizeof(sizes));
    } else if (vs->ok) {
        snprintf(counts, sizeof(counts), "\n\n\nnot a formatted pak\n");
        names[0] = '\0';
        sizes[0] = '\0';
    } else {
        snprintf(counts, sizeof(counts), "\n\n\nno file yet\n");
        snprintf(names, sizeof(names), "  Made when the game first\n  runs with the virtual pak\n  on, or by a note copied\n  from the real pak now.\n");
        sizes[0] = '\0';
    }
    text_draw(0, COLUMN_WIDTH - 12, (col == SIDE_VIRTUAL) ? STL_GREEN : STL_DEFAULT, ALIGN_LEFT, "\n\nVirtual pak\n");
    text_draw(0, COLUMN_WIDTH - 12, STL_GRAY, ALIGN_LEFT, counts);
    snprintf(block, sizeof(block), "\n\n\n\n%s", names);
    text_draw(0, COLUMN_WIDTH - 12, vs->fs ? STL_DEFAULT : STL_GRAY, ALIGN_LEFT, block);
    snprintf(block, sizeof(block), "\n\n\n\n%s", sizes);
    text_draw(0, COLUMN_WIDTH - 12, STL_GRAY, ALIGN_RIGHT, block);

    // the real pak in a port
    const side_t *rs = &sides[SIDE_REAL];
    menu_font_type_t head_style = (col == SIDE_REAL) ? STL_GREEN : STL_DEFAULT;
    sizes[0] = '\0';
    if (port < 0) {
        snprintf(head, sizeof(head), "\n\nReal pak: none found\n");
        counts[0] = '\0';
        snprintf(names, sizeof(names), "  Plug a Controller Pak into\n  a controller in any port.\n");
        head_style = STL_ORANGE;
    } else if (!rs->ok) {
        snprintf(head, sizeof(head), "\n\nReal pak in port %d\n", port + 1);
        snprintf(counts, sizeof(counts), "\n\n\ncould not be read\n");
        names[0] = '\0';
        head_style = STL_ORANGE;
    } else if (!rs->fs) {
        snprintf(head, sizeof(head), "\n\nReal pak in port %d\n", port + 1);
        snprintf(counts, sizeof(counts), "\n\n\nno pak file system on it\n");
        snprintf(names, sizeof(names), "  Not formatted, or damaged.\n  Format it in the Controller\n  Pak manager first.\n");
        head_style = STL_ORANGE;
    } else {
        snprintf(head, sizeof(head), "\n\nReal pak in port %d\n", port + 1);
        if (rs->banks == 1) {
            snprintf(counts, sizeof(counts), "\n\n\n%d note%s, %d of %d pages\n", rs->n, (rs->n == 1) ? "" : "s", PAK_PAGES - pages_free(rs->img), PAK_PAGES);
        } else {
            snprintf(counts, sizeof(counts), "\n\n\n%d note%s, %d banks\n", rs->n, (rs->n == 1) ? "" : "s", rs->banks);
        }
        column_notes(SIDE_REAL, names, sizeof(names), sizes, sizeof(sizes));
    }
    text_draw(COLUMN_WIDTH, COLUMN_WIDTH - 12, head_style, ALIGN_LEFT, head);
    text_draw(COLUMN_WIDTH, COLUMN_WIDTH - 12, STL_GRAY, ALIGN_LEFT, counts);
    snprintf(block, sizeof(block), "\n\n\n\n%s", names);
    text_draw(COLUMN_WIDTH, COLUMN_WIDTH - 12, rs->fs ? STL_DEFAULT : STL_GRAY, ALIGN_LEFT, block);
    snprintf(block, sizeof(block), "\n\n\n\n%s", sizes);
    text_draw(COLUMN_WIDTH, COLUMN_WIDTH - 12, STL_GRAY, ALIGN_RIGHT, block);

    ui_components_actions_bar_text_draw(STL_DEFAULT, ALIGN_LEFT, VALIGN_TOP, (col == SIDE_VIRTUAL) ? "A: Copy note > real pak\nB: Back" : "A: Copy note > virtual pak\nB: Back");
    ui_components_actions_bar_text_draw(STL_DEFAULT, ALIGN_RIGHT, VALIGN_TOP, (ports_with > 1) ? "L|Z: Next port\nR: Options" : "\nR: Options");

    ui_components_context_menu_draw(&options_menu);
}

static void draw (menu_t *menu, surface_t *d) {
    rdpq_attach(d, NULL);

    ui_components_background_draw();

    ui_components_layout_draw();

    if (list_screen) {
        draw_list(menu);
    } else {
        draw_pak(menu);
    }

    switch (ask) {
        case ASK_COPY:
            ui_components_messagebox_draw(
                "Copy %s to the %s?\n\n"
                "A: Yes    B: No",
                op_name, side_name(1 - op_side)
            );
            break;
        case ASK_REPLACE:
            ui_components_messagebox_draw(
                "The %s already has a note %s.\n\n"
                "Replace it with this one?\n\n"
                "A: Yes    B: No",
                side_name(1 - op_side), op_name
            );
            break;
        case ASK_DELETE_NOTE:
            ui_components_messagebox_draw(
                "Delete %s from the %s?\n\n"
                "A: Yes    B: No",
                op_name, side_name(op_side)
            );
            break;
        case ASK_CLONE_TO_VIRTUAL:
            ui_components_messagebox_draw(
                "Replace this game's virtual pak with a copy of the Controller Pak in port %d?\n\n"
                "Every note in the virtual pak is lost.\n\n"
                "A: Yes    B: No",
                port + 1
            );
            break;
        case ASK_CLONE_TO_REAL:
            ui_components_messagebox_draw(
                "Replace the Controller Pak in port %d with a copy of this game's virtual pak?\n\n"
                "Everything on the real pak is replaced. Its contents are backed up to sd:%s first.\n\n"
                "A: Yes    B: No",
                port + 1, BACKUP_DIR
            );
            break;
        case ASK_CLONE_TO_REAL_SURE:
            ui_components_messagebox_draw(
                "The real pak in port %d holds %d note%s. They are all replaced.\n\n"
                "A: Go ahead    B: No",
                port + 1, sides[SIDE_REAL].n, (sides[SIDE_REAL].n == 1) ? "" : "s"
            );
            break;
        case ASK_DELETE_PAK:
            ui_components_messagebox_draw(
                "Delete this game's virtual pak?\n\n"
                "Its %d note%s gone for good.\n\n"
                "A: Yes    B: No",
                sides[SIDE_VIRTUAL].n, (sides[SIDE_VIRTUAL].n == 1) ? " is" : "s are"
            );
            break;
        default:
            break;
    }

    if (message_on) {
        ui_components_messagebox_draw("%s\n\nA: OK", message);
    }

    if (todo != DO_NONE) {
        char why[320];
        int job = todo;
        switch (job) {
            case DO_LIST: ui_components_loader_draw(0, "Reading the virtual paks..."); break;
            case DO_READ_REAL: ui_components_loader_draw(0, "Reading the Controller Pak..."); break;
            case DO_COPY: ui_components_loader_draw(0, "Copying the note..."); break;
            case DO_DELETE_NOTE: ui_components_loader_draw(0, "Deleting the note..."); break;
            case DO_CLONE_TO_VIRTUAL: ui_components_loader_draw(0, "Copying the Controller Pak into the virtual pak..."); break;
            case DO_CLONE_TO_REAL: ui_components_loader_draw(0, "Copying the virtual pak onto the Controller Pak..."); break;
            default: ui_components_loader_draw(0, "Deleting..."); break;
        }
        rdpq_detach_show();
        todo = DO_NONE;
        why[0] = '\0';
        switch (job) {
            case DO_LIST:
                list_build(menu);
                break;
            case DO_READ_REAL:
                if (!real_read(why, sizeof(why))) {
                    show_message(why);
                }
                break;
            case DO_COPY:
                do_copy(menu, why, sizeof(why));
                op_replace = -1;
                show_message(why);
                break;
            case DO_DELETE_NOTE:
                do_delete_note(menu, why, sizeof(why));
                show_message(why);
                break;
            case DO_CLONE_TO_VIRTUAL:
                do_clone_to_virtual(menu, why, sizeof(why));
                show_message(why);
                break;
            case DO_CLONE_TO_REAL:
                do_clone_to_real(menu, why, sizeof(why));
                show_message(why);
                break;
            default:
                if (do_delete_pak(why, sizeof(why)) && menu->vpak_view.from_rom) {
                    leave_after_message = true;
                } else if (!menu->vpak_view.from_rom) {
                    list_keep[0] = '\0';
                    list_build(menu);
                    list_screen = true;
                }
                show_message(why);
                break;
        }
        return;
    }

    rdpq_detach_show();
}


void view_virtual_pak_init (menu_t *menu) {
    ask = ASK_NONE;
    message_on = false;
    leave_after_message = false;
    port = -1;
    real_backed_up = false;
    backup_name[0] = '\0';
    op_replace = -1;
    sides[SIDE_REAL].ok = false;
    side_parse(&sides[SIDE_REAL]);
    sides[SIDE_VIRTUAL].ok = false;
    side_parse(&sides[SIDE_VIRTUAL]);
    for (int p = 0; p < 4; p++) {
        pak_in[p] = false;
    }
    cur[0] = cur[1] = 0;
    top[0] = top[1] = 0;
    ui_components_context_menu_init(&options_menu);
    if (menu->vpak_view.from_rom && menu->vpak_view.check_code) {
        char dir[160];
        vcode = (uint64_t) menu->vpak_view.check_code;
        paks_dir(menu, dir, sizeof(dir));
        snprintf(vpath, sizeof(vpath), "%s/%08lX%08lX.pak", dir, (unsigned long) (vcode >> 32), (unsigned long) (vcode & 0xFFFFFFFFULL));
        snprintf(vlabel, sizeof(vlabel), "%s", menu->load.rom_path ? path_last_get(menu->load.rom_path) : "");
        char *dot = strrchr(vlabel, '.');
        if (dot && (dot != vlabel)) {
            *dot = '\0';
        }
        col = SIDE_REAL;                    // the usual errand here: a save from the real pak into the game's
        virtual_load();
        list_screen = false;
        todo = DO_NONE;
    } else {
        list_screen = true;
        list_sel = 0;
        list_top = 0;
        list_keep[0] = '\0';
        todo = DO_LIST;
    }
}

void view_virtual_pak_display (menu_t *menu, surface_t *display) {
    process(menu);

    draw(menu, display);
}
