/* SC64SS save states: the in-game overlay and everything drawn on the screen.
 * Included by hook.c after the state engine and the SD mirror (needs pio_*,
 * pi_dma, sd_*, st_hdr, state_do_save, hook_cfg, pad_dma_poll, si_wait).
 *
 * All of this runs inside the exception with the game frozen: the VI keeps
 * showing the frame it was showing, we draw straight into that buffer through
 * the uncached segment, poll the controller with our own SI transfers, and when
 * the player leaves, the game continues and its next frame simply overwrites
 * what we drew. */

#include "ss_font.h"

#define STATE_OP_MENU   9u
#define VI_Y_SCALE_REG  (*(vu32 *)0xA4400034u)
#define SC64_CMD_TIME_GET 0x74u                /* 't' */
#define MENU_MAX_ROWS   8u
#define MSG_TICKS       75u                    /* combo feedback stays ~1.25 s */

/* a frozen frame */
struct ov_screen {
    uint32_t fb;        /* KSEG1 address of the displayed buffer */
    uint32_t width;     /* the stride (VI_WIDTH) */
    uint32_t vis;       /* the columns on screen: what the panel lays itself out in */
    uint32_t height;
    uint32_t bpp;       /* 2 or 4 */
    uint32_t scale;     /* 1 up to 511 visible columns, 2 from 512 */
    uint32_t height_all;   /* the lines as the VI shows them, the hook's home included (a screenshot in
                            * borrowed mode reads those from the stash; the panel never draws there) */
};

static uint32_t thumb_buf[THUMB_BYTES / 4u] __attribute__((aligned(16))) = {0};
static uint32_t thumb_ready = 0;               /* thumb_buf holds the frame the next save belongs to */
/* (fb_origins, the last few displayed buffers, is defined ahead of ov_field_base below) */
static uint32_t fb_expect_width = 0;             /* the loaded world's VI_WIDTH: draw only once the live VI agrees */
static const char *msg_text = 0;
static uint32_t msg_ticks = 0;
static uint32_t menu_saves = 0, menu_loads = 0, menu_opens = 0;

/* ---- colours --------------------------------------------------------------- */
static uint32_t ov_rgb(const struct ov_screen *s, uint32_t r, uint32_t g, uint32_t b) {   /* 0..255 each */
    if (s->bpp == 2u) {
        return ((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | 1u;
    }
    return (r << 24) | (g << 16) | (b << 8) | 0xFFu;
}

static uint32_t fb_origins[3] = {0};           /* the last few displayed buffers: feedback is drawn on all */

/* An interlaced mode shows one buffer from two origins a line apart (the odd field's a
 * line in). The buffer begins at the lower one: anything drawn over its full height from
 * the other runs a line past its end, into whatever the game keeps there (a 640x480
 * game's heap: the panel's dimming corrupted a chunk header and its next free crashed). */
static uint32_t ov_field_base(uint32_t origin) {
    if (!vi_fld_lineoff || (vi_fld_seen != 3u)) return origin;
    uint32_t d = (vi_fld_lineoff > 0) ? (uint32_t)vi_fld_lineoff : (uint32_t)(-vi_fld_lineoff);
    if (((vi_fld_origin[0] + d) == vi_fld_origin[1]) || ((vi_fld_origin[1] + d) == vi_fld_origin[0])) {
        uint32_t lo = (vi_fld_origin[0] < vi_fld_origin[1]) ? vi_fld_origin[0] : vi_fld_origin[1];
        if ((origin == lo) || (origin == lo + d)) return lo;
    }
    for (uint32_t k = 0; k < 3u; k++) {
        if (fb_origins[k] && (origin == fb_origins[k] + d)) return fb_origins[k];
        if (fb_origins[k] && (fb_origins[k] == origin + d)) return origin;
    }
    return origin;
}

static void ov_screen_read(struct ov_screen *s) {
    uint32_t status = VI_STATUS_REG, origin = VI_ORIGIN_REG & 0x00FFFFFFu, width = VI_WIDTH_REG;
    uint32_t vv = VI_V_VIDEO_REG, ys = VI_Y_SCALE_REG & 0xFFFu;
    if (vi_frz_active && vi_frz_base0) origin = vi_frz_base0;   /* ORIGIN alternates per field during a freeze */
    origin = ov_field_base(origin);
    s->fb = 0xA0000000u | origin;
    s->width = width;
    s->bpp = ((status & 3u) == 3u) ? 4u : 2u;
    uint32_t lines = (((vv & 0x3FFu) - (vv >> 16)) / 2u);
    uint32_t h = (ys ? (lines * ys) / 1024u : lines);
    if (h < 200u) h = 240u;
    if (h > 480u) h = 480u;
    /* never a line past RDRAM */
    if (width && (origin < 0x00800000u)) {
        uint32_t max_all = (0x00800000u - origin) / (width * s->bpp);
        if (h > max_all) h = max_all;
    } else {
        h = 0;
    }
    s->height_all = h;
    /* never a line past the hook's home (0x807D0000, resident and borrowed alike): a
     * buffer that reaches the top of RAM (Castlevania LoD's interlaced hi-res mode keeps
     * one there; Rush 2049's and Indiana Jones' hi-res menus too) must not have the
     * panel dim or draw over the hook. A screenshot in borrowed mode still gets those
     * lines, from the stash (hook.c shot_take). */
    if (width && (origin < HOOK_HOME_OFF)) {
        uint32_t max_h = (HOOK_HOME_OFF - origin) / (width * s->bpp);
        if (h > max_h) h = max_h;
    } else {
        h = 0;
    }
    s->height = h;
    /* the columns the VI shows, from its window and horizontal scale: how much of the
     * stride is on screen. Castlevania LoD's hi-res draws 490 columns of a 640 stride
     * and shows just those (X_SCALE 0.77 over the full window); the rest is never on
     * screen nor repainted by the game, so a panel laid out over the stride was a
     * third too big, cut off on the right, and left flashing remnants there */
    uint32_t hv = VI_H_VIDEO_REG, xs = VI_X_SCALE_REG & 0xFFFu;
    uint32_t cols = ((hv & 0x3FFu) > (hv >> 16)) ? ((hv & 0x3FFu) - (hv >> 16)) : 0u;
    uint32_t vis = xs ? (cols * xs) / 1024u : width;
    if ((vis < 256u) || (vis > width)) vis = width;
    s->vis = vis;
    s->scale = (vis >= 512u) ? 2u : 1u;
}

static uint32_t ov_screen_ok(const struct ov_screen *s) {
    return ((s->fb & 0x00FFFFFFu) != 0) && (s->width >= 256u) && (s->width <= 640u) && (s->vis >= 256u) &&
           (s->height >= 160u);
}

#define OV_STASH_CART   FRAME_STASH_PI         /* the picture under the panel: cart SDRAM above the slots (hook.c) */
static uint32_t stash_origin = 0, stash_len = 0;

/* the displayed frame -> cart, so it can be put back later (before a save from the
 * panel: states then never contain the panel; around a load: the picture holds until
 * the restored game draws its first frame) */
static uint32_t frame_stash(void) {
    struct ov_screen s;
    ov_screen_read(&s);
    stash_len = 0;
    if (!ov_screen_ok(&s)) return 0;
    uint32_t len = (s.width * s.height * s.bpp + 15u) & ~15u;
    if (len > FRAME_STASH_LEN) return 0;    /* (1.25 MiB: every mode fits; a save from the panel would otherwise keep the panel) */
    stash_origin = s.fb & 0x00FFFFFFu;
    dcache_writeback_all();
    for (uint32_t off = 0; off < len; off += STATE_CHUNK) {
        uint32_t n = ((len - off) < STATE_CHUNK) ? (len - off) : STATE_CHUNK;
        if (!pi_dma(0x80000000u | (stash_origin + off), OV_STASH_CART + off, n, 1u)) return 0;
    }
    stash_len = len;
    return 1;
}

/* the displayed buffer's RDRAM range (start offset from 0x80000000, byte length) */
static void ov_disp_range(uint32_t *start, uint32_t *len) {
    struct ov_screen s;
    ov_screen_read(&s);
    *start = 0; *len = 0;
    if (!ov_screen_ok(&s)) return;
    *start = s.fb & 0x00FFFFFFu;
    *len = (s.width * s.height * s.bpp + 15u) & ~15u;
}

/* After a save made with the panel on screen: the slot's copy of the displayed
 * buffer gets the clean frame stashed when the panel opened (cart -> bounce -> slot),
 * so the state never carries the panel while the screen never changes. */
#define bounce_buf bounce                 /* hook.c's 8 KiB bounce: never in use at the same time (the borrowed
                                           * copies of a save end before this runs, the PNG writer has its own op) */
static void slot_patch_clean_frame(uint32_t slot_base) {
    if (!stash_len) return;
    uint32_t image = st_hdr.image_len;
    for (uint32_t off = 0; off < stash_len; off += sizeof(bounce_buf)) {
        uint32_t n = ((stash_len - off) < sizeof(bounce_buf)) ? (stash_len - off) : sizeof(bounce_buf);
        uint32_t dst = stash_origin + off;
        if (dst + n > image) break;              /* the buffer lies outside the saved image */
        if (!pi_dma((uint32_t)(uintptr_t)bounce_buf, OV_STASH_CART + off, n, 0u)) return;
        if (!pi_dma((uint32_t)(uintptr_t)bounce_buf, slot_base + STATE_IMAGE_OFF + dst, n, 1u)) return;
    }
}


static void ov_px(const struct ov_screen *s, uint32_t x, uint32_t y, uint32_t color) {
    if ((x >= s->width) || (y >= s->height)) return;
    if (s->bpp == 2u) {
        *(vu16 *)(s->fb + (y * s->width + x) * 2u) = (uint16_t)color;
    } else {
        *(vu32 *)(s->fb + (y * s->width + x) * 4u) = color;
    }
}

static void ov_rect(const struct ov_screen *s, uint32_t x0, uint32_t y0, uint32_t w, uint32_t h, uint32_t color) {
    for (uint32_t y = y0; y < y0 + h; y++) {
        if (y >= s->height) break;
        if (s->bpp == 2u) {
            vu16 *row = (vu16 *)(s->fb + (y * s->width + x0) * 2u);
            for (uint32_t x = 0; (x < w) && (x0 + x < s->width); x++) row[x] = (uint16_t)color;
        } else {
            vu32 *row = (vu32 *)(s->fb + (y * s->width + x0) * 4u);
            for (uint32_t x = 0; (x < w) && (x0 + x < s->width); x++) row[x] = color;
        }
    }
}

/* darken the visible frame so the panel stands out (columns past the visible ones are
 * never on screen; left alone, the game need not repaint them) */
static void ov_dim(const struct ov_screen *s) {
    for (uint32_t y = 0; y < s->height; y++) {
        if (s->bpp == 2u) {
            vu16 *p = (vu16 *)(s->fb + y * s->width * 2u);
            for (uint32_t x = 0; x < s->vis; x++) {
                uint32_t v = p[x];
                p[x] = (uint16_t)(((v >> 2) & 0x39CEu) | 1u);      /* each channel / 4 */
            }
        } else {
            vu32 *p = (vu32 *)(s->fb + y * s->width * 4u);
            for (uint32_t x = 0; x < s->vis; x++) {
                uint32_t v = p[x];
                p[x] = ((v >> 2) & 0x3F3F3F00u) | 0xFFu;
            }
        }
    }
}

static void ov_text(const struct ov_screen *s, uint32_t x, uint32_t y, const char *str, uint32_t color) {
    uint32_t sc = s->scale;
    for (; *str; str++, x += 7u * sc) {          /* the glyphs are 6 px wide */
        uint32_t c = (uint32_t)(uint8_t)*str;
        if ((c < 32u) || (c > 126u)) c = 63u;
        const uint8_t *g = font8x8[c - 32u];
        for (uint32_t gy = 0; gy < 8u; gy++) {
            uint32_t bits = g[gy];
            if (!bits) continue;
            for (uint32_t gx = 0; gx < 8u; gx++) {
                if (!(bits & (0x80u >> gx))) continue;
                for (uint32_t dy = 0; dy < sc; dy++) {
                    for (uint32_t dx = 0; dx < sc; dx++) {
                        ov_px(s, x + gx * sc + dx, y + gy * sc + dy, color);
                    }
                }
            }
        }
    }
}

/* ---- text helpers (no libc) -------------------------------------------------- */
static char *ov_cat(char *d, const char *src) {
    while (*src) *d++ = *src++;
    *d = 0;
    return d;
}

static char *ov_num2(char *d, uint32_t bcd) {           /* two BCD digits */
    *d++ = (char)('0' + ((bcd >> 4) & 0xFu));
    *d++ = (char)('0' + (bcd & 0xFu));
    *d = 0;
    return d;
}

static char *ov_dec(char *d, uint32_t v) {
    char tmp[12];
    uint32_t n = 0;
    do { tmp[n++] = (char)('0' + (v % 10u)); v /= 10u; } while (v);
    while (n) *d++ = tmp[--n];
    *d = 0;
    return d;
}

/* "20YY-MM-DD HH:MM" from the two SC64 RTC words, or "NO CLOCK" */
static void ov_stamp_text(char *d, uint32_t date, uint32_t time) {
    uint32_t year = (date >> 16) & 0xFFu, month = (date >> 8) & 0xFFu, day = date & 0xFFu;
    uint32_t hour = (time >> 16) & 0xFFu, minute = (time >> 8) & 0xFFu;
    if ((date == 0) || (month == 0) || (month > 0x12u) || (day == 0) || (day > 0x31u)) {
        ov_cat(d, "NO CLOCK");
        return;
    }
    d = ov_cat(d, "20"); d = ov_num2(d, year); *d++ = '-'; d = ov_num2(d, month); *d++ = '-'; d = ov_num2(d, day);
    *d++ = ' '; d = ov_num2(d, hour); *d++ = ':'; d = ov_num2(d, minute); *d = 0;
}

/* the cart's clock into the header (0 when unavailable) */
static void ov_stamp_now(uint32_t *date, uint32_t *time) {
    uint32_t d0 = 0, d1 = 0;
    *date = 0; *time = 0;
    if (!ensure_unlocked()) return;
    if (!sc64_command_long(SC64_CMD_TIME_GET, 0, 0, CMD_TIMEOUT_TICKS)) return;
    pio_read(SC64_DATA0, &d0);
    pio_read(SC64_DATA1, &d1);
    *time = d0;
    *date = d1;
}

/* ---- thumbnails --------------------------------------------------------------- */
/* the displayed frame, sampled down to 80x60 RGBA5551 into thumb_buf */
static void thumb_capture(void) {
    struct ov_screen s;
    ov_screen_read(&s);
    thumb_ready = 0;
    if (!ov_screen_ok(&s)) return;
    uint32_t sx = s.vis / THUMB_W, sy = s.height / THUMB_H;
    if (sx == 0) sx = 1;
    if (sy == 0) sy = 1;
    vu16 *dst = (vu16 *)thumb_buf;
    for (uint32_t y = 0; y < THUMB_H; y++) {
        vi_frz_service();
        for (uint32_t x = 0; x < THUMB_W; x++) {
            uint32_t px = x * sx, py = y * sy, v;
            if (s.bpp == 2u) {
                v = *(vu16 *)(s.fb + (py * s.width + px) * 2u);
            } else {
                uint32_t c = *(vu32 *)(s.fb + (py * s.width + px) * 4u);
                v = (((c >> 27) & 31u) << 11) | (((c >> 19) & 31u) << 6) | (((c >> 11) & 31u) << 1) | 1u;
            }
            dst[y * THUMB_W + x] = (uint16_t)v;
        }
    }
    thumb_ready = 1;
}

/* the captured thumbnail into the slot (after a successful save) */
static void thumb_store(uint32_t slot_base) {
    if (!thumb_ready) return;
    dcache_writeback_all();
    pi_dma((uint32_t)(uintptr_t)thumb_buf, slot_base + STATE_THUMB_OFF, THUMB_BYTES, 1u);
}

/* a slot's thumbnail from cart SDRAM straight onto the screen */
static void thumb_draw(const struct ov_screen *s, uint32_t slot_base, uint32_t x0, uint32_t y0) {
    uint32_t src = 0xA0000000u | (slot_base + STATE_THUMB_OFF);
    uint32_t sc = s->scale;
    for (uint32_t y = 0; y < THUMB_H; y++) {
        for (uint32_t x = 0; x < THUMB_W; x += 2u) {
            uint32_t w;
            if (!pio_read(src + (y * THUMB_W + x) * 2u, &w)) return;
            uint32_t p0 = w >> 16, p1 = w & 0xFFFFu;
            uint32_t c0 = p0, c1 = p1;
            if (s->bpp == 4u) {
                c0 = ((((p0 >> 11) & 31u) * 255u / 31u) << 24) | ((((p0 >> 6) & 31u) * 255u / 31u) << 16) | ((((p0 >> 1) & 31u) * 255u / 31u) << 8) | 0xFFu;
                c1 = ((((p1 >> 11) & 31u) * 255u / 31u) << 24) | ((((p1 >> 6) & 31u) * 255u / 31u) << 16) | ((((p1 >> 1) & 31u) * 255u / 31u) << 8) | 0xFFu;
            }
            ov_rect(s, x0 + x * sc, y0 + y * sc, sc, sc, c0);
            ov_rect(s, x0 + (x + 1u) * sc, y0 + y * sc, sc, sc, c1);
        }
    }
}

/* ---- the menu ---------------------------------------------------------------------- */
static void menu_draw_frame(const struct ov_screen *s, uint32_t x0, uint32_t y0, uint32_t w, uint32_t h) {
    uint32_t sc = s->scale;
    ov_rect(s, x0, y0, w, h, ov_rgb(s, 235, 235, 235));
    ov_rect(s, x0 + 2u * sc, y0 + 2u * sc, w - 4u * sc, h - 4u * sc, ov_rgb(s, 16, 24, 64));
}

/* the footer row: cleared, then written (text only paints lit pixels) */
static void menu_footer(const struct ov_screen *s, uint32_t x0, uint32_t y0, uint32_t w, uint32_t h, const char *text, uint32_t color) {
    uint32_t sc = s->scale;
    ov_rect(s, x0 + 2u * sc, y0 + h - 18u * sc, w - 4u * sc, 16u * sc, ov_rgb(s, 16, 24, 64));
    ov_text(s, x0 + 8u * sc, y0 + h - 14u * sc, text, color);
}

static void menu_rom_title(char *d) {
    uint32_t w;
    char *p = d;
    for (uint32_t i = 0; i < 5u; i++) {
        if (!pio_read(0xB0000020u + 4u * i, &w)) break;
        for (uint32_t k = 0; k < 4u; k++) {
            char c = (char)((w >> (24u - 8u * k)) & 0xFFu);
            if ((c < 32) || (c > 126)) c = ' ';
            *p++ = c;
        }
    }
    *p = 0;
    while ((p > d) && (p[-1] == ' ')) *--p = 0;   /* trailing blanks */
}

/* a button mask as text, in the menu's order: "L+R+START" */
static const char *const btn_names[14] = {"L", "R", "Z", "A", "B", "START", "UP", "DOWN", "LEFT", "RIGHT",
                                          "C-UP", "C-DOWN", "C-LEFT", "C-RIGHT"};
static const uint8_t btn_bits[14] = {5, 4, 13, 15, 14, 12, 11, 10, 9, 8, 3, 2, 1, 0};
static char *combo_text(uint32_t mask, char *d) {
    char *p = d;
    *p = 0;
    for (uint32_t i = 0; i < 14u; i++) {
        if (!(mask & (1u << btn_bits[i]))) continue;
        if (p != d) *p++ = '+';
        p = ov_cat(p, btn_names[i]);
    }
    if (p == d) ov_cat(d, "NONE");
    return d;
}

static const char *const speed_names[5] = {"NORMAL", "1/2", "1/4", "1/8", "STEP"};
static const uint32_t speed_divs[5] = {1u, 2u, 4u, 8u, SPEED_STEP};
static const char *const sound_names[2] = {"PITCH DOWN", "STUTTER"};

/* the empty card slots, one of them (`except`) left out of the count (SLX_NONE: none) */
static uint32_t card_empties(uint32_t n, uint32_t except) {
    uint32_t c = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t st = 0;
        if (i == except) continue;
        slx_rd(SLX_CARD + 12u * i, &st);
        if (!(st & 0xFFu)) c++;
    }
    return c;
}

/* Returns 0 = resume the game, 1 = load st_slot. Saves happen inside.
 * Two pages, L or R switches: Slots (states) and Game (speed). */
static uint32_t menu_run(uint32_t cause, uint32_t mi) {
    struct ov_screen s;
    char line[48], title[24];
    uint32_t n = card_count();
    menu_opens++;
    thumb_capture();                              /* the frame a save from here belongs to */
    ov_screen_read(&s);
    crumb(0x70u, (s.width << 16) | s.vis, (s.scale << 16) | s.height);   /* the panel's view of the screen */
    if (!ov_screen_ok(&s)) return 0;              /* (n == 0, states off: the Game page still serves) */
    frame_stash();                                /* the clean frame, put back under every save */
    uint32_t sc = s.scale;
    uint32_t x0 = 24u * sc, y0 = 20u * sc, w = s.vis - 48u * sc, h = s.height - 40u * sc;
    uint32_t white = ov_rgb(&s, 240, 240, 240), grey = ov_rgb(&s, 150, 150, 160), hi = ov_rgb(&s, 255, 220, 60);
    uint32_t dark = ov_rgb(&s, 16, 24, 64), red = ov_rgb(&s, 240, 80, 60);
    uint32_t tx = x0 + w - (THUMB_W + 8u) * sc, ty = y0 + 30u * sc;

    /* the frame is darkened only when it can be put back afterwards: a hi-res frame
     * too big for the stash keeps whatever the game never repaints (its outermost
     * row and column in Castlevania LoD), and a darkened sliver there flickers at the
     * edge of the picture until the mode changes */
    if (stash_len) ov_dim(&s);
    menu_draw_frame(&s, x0, y0, w, h);
    menu_rom_title(title);
    ov_text(&s, x0 + 8u * sc, y0 + 8u * sc, "SAVE STATES", white);
    ov_text(&s, x0 + 8u * sc, y0 + 8u * sc + 10u * sc, title, grey);

    /* the list is the card's: every file of this game, in a window of MENU_MAX_ROWS rows.
     * A row's date and time come from the index; the thumbnail from the cart slot holding
     * the state or, for one on the card only, from its file's head read into the scratch */
    uint32_t cur = (n && (hook_cfg.cur_slot < n)) ? hook_cfg.cur_slot : 0, top = 0;
    uint32_t prev = 0xFFFFu, confirm = 0, result = 0, redraw = 1, page = 0, grow = 0, exit_after = 0;
    uint32_t pak_on = (hook_cfg.spare & 4u) ? 1u : 0u, rows = pak_on ? 6u : 5u;   /* the Game page's rows: SPEED, SOUND, EXIT, SUSPEND, [PAK,] DELETE */
    uint32_t gdel = rows - 1u;                    /* the DELETE row */
    uint32_t thumb_n = SLX_NONE;                  /* the card slot whose head is in the scratch */
    uint32_t cst = 0;                             /* the highlighted row's state word */
    if (pak_on && !borrowed_mode()) pak_live_from_cfg();
    if (borrowed_mode()) grow = 2u;              /* the Game page's speed rows need the Slow motion option (the resident hook) */
    uint32_t last_move = c0_count();
    const char *note = 0;
    crumb(0x71u, cur, n);                         /* about to draw */
    for (;;) {
        if (sd_state == 1u) sd_service();        /* a mirror write may still be streaming */
        if (redraw) {
            ov_rect(&s, x0 + 2u * sc, y0 + 28u * sc, w - 4u * sc, h - 30u * sc, dark);
            {   /* the page tabs, on the title's second row */
                uint32_t tabx = x0 + w - 8u * sc - 7u * sc * 11u;
                ov_rect(&s, tabx, y0 + 18u * sc, 7u * sc * 11u, 8u * sc, dark);
                ov_text(&s, tabx, y0 + 18u * sc, "SLOTS", page ? grey : hi);
                ov_text(&s, tabx + 7u * sc * 7u, y0 + 18u * sc, "GAME", page ? hi : grey);
            }
            card_info(cur, &cst, 0, 0);
            if (page == 0) {
                if (cur < top) top = cur;
                if (cur >= top + MENU_MAX_ROWS) top = cur + 1u - MENU_MAX_ROWS;
                uint32_t empties = card_empties(n, SLX_NONE);
                if (n == 0) ov_text(&s, x0 + 8u * sc, y0 + 30u * sc, "  SAVE STATES ARE OFF FOR THIS GAME", grey);
                for (uint32_t i = 0; i < MENU_MAX_ROWS; i++) {
                    uint32_t idx = top + i, st = 0, d = 0, t = 0, y = y0 + 30u * sc + i * 14u * sc;
                    char *p = line;
                    if (idx >= n) {
                        if (n && (idx == n) && !empties) {   /* the list's end with no empty row left */
                            ov_text(&s, x0 + 8u * sc, y, (hook_cfg.card_flags & 1u) ? "  CARD FULL: NO EMPTY SLOTS" : "  NO EMPTY SLOTS: RELAUNCH FOR MORE", grey);
                        }
                        break;
                    }
                    card_info(idx, &st, &d, &t);
                    p = ov_cat(p, (idx == cur) ? "> " : "  ");
                    if (idx + 1u < 100u) *p++ = ' ';
                    if (idx + 1u < 10u) *p++ = ' ';
                    p = ov_dec(p, idx + 1u);
                    p = ov_cat(p, "  ");
                    if (st & 0xFFu) {
                        ov_stamp_text(p, d, t);
                        if (st & SLX_RESIDENT) ov_cat(p + 16u, " SLOW");   /* saved with Slow motion on */
                    } else {
                        ov_cat(p, "EMPTY");
                    }
                    ov_text(&s, x0 + 8u * sc, y, line, (idx == cur) ? hi : ((st & 0xFFu) ? white : grey));
                }
                ov_rect(&s, tx - 2u * sc, ty - 2u * sc, (THUMB_W + 4u) * sc, (THUMB_H + 4u) * sc, grey);
                uint32_t tsrc = 0;
                if ((cst & 0xFFu) && (cst & 2u)) {
                    if (cst & 0xFF00u) {
                        uint32_t p = ((cst >> 8) & 0xFFu) - 1u;
                        if ((p < hook_cfg.slots_n) && hook_cfg.slots[p]) tsrc = hook_cfg.slots[p];
                    }
                    if (!tsrc) {                          /* on the card only: its head into the scratch */
                        if (thumb_n != cur) thumb_n = sd_read_head(cur) ? cur : SLX_NONE;
                        if (thumb_n == cur) tsrc = SLOT_SCRATCH_PI;
                    }
                }
                if (tsrc) {
                    thumb_draw(&s, tsrc, tx, ty);
                } else {
                    ov_rect(&s, tx, ty, THUMB_W * sc, THUMB_H * sc, dark);
                    ov_text(&s, tx + 12u * sc, ty + 26u * sc, (cst & 0xFFu) ? "NO IMAGE" : "EMPTY", grey);
                }
                if ((30u + MENU_MAX_ROWS * 14u + 30u + 8u) * sc <= h) {   /* room under the last row */
                    if (confirm == 4u) {
                        ov_text(&s, x0 + 8u * sc, y0 + h - 30u * sc, "MORE AFTER A RELAUNCH FROM THE MENU", grey);
                    } else if (confirm == 6u) {
                        ov_text(&s, x0 + 8u * sc, y0 + h - 30u * sc, "SWITCH IT ON FOR THIS GAME TO LOAD IT", grey);
                    } else if (!confirm) {
                        char hk[80], kb[40];
                        ov_cat(ov_cat(ov_cat(ov_cat(hk, "SAVE "), combo_text(hook_cfg.combo_save, kb)), "  LOAD "), combo_text(hook_cfg.combo_load, kb));
                        ov_text(&s, x0 + 8u * sc, y0 + h - 30u * sc, hk, grey);   /* clear of the footer's box (h - 18) */
                    }
                }
                const char *foot = (confirm == 1u) ? "OVERWRITE?  A YES   B NO" :
                                   ((confirm == 4u) ? "LAST EMPTY SLOT: SAVE?  A YES  B NO" :
                                   ((confirm == 6u) ? "SAVED WITH SLOW MOTION ON   B BACK" : "A LOAD  Z SAVE  B CLOSE  > GAME"));
                menu_footer(&s, x0, y0, w, h, foot, confirm ? red : white);
            } else {
                char *p;
                if (borrowed_mode()) {
                    /* nothing of ours stays resident: no hold at the VI, no retuned DAC */
                    ov_text(&s, x0 + 8u * sc, y0 + 30u * sc, "  SPEED     SLOW MOTION IS OFF", grey);
                    ov_text(&s, x0 + 8u * sc, y0 + 44u * sc, "  SOUND     SLOW MOTION IS OFF", grey);
                } else {
                    p = ov_cat(line, (grow == 0) ? "> SPEED     < " : "  SPEED     < ");
                    p = ov_cat(p, speed_names[speed_sel]);
                    ov_cat(p, " >");
                    ov_text(&s, x0 + 8u * sc, y0 + 30u * sc, line, (grow == 0) ? hi : white);
                    p = ov_cat(line, (grow == 1u) ? "> SOUND     < " : "  SOUND     < ");
                    p = ov_cat(p, sound_names[slow_sound & 1u]);
                    ov_cat(p, " >");
                    ov_text(&s, x0 + 8u * sc, y0 + 44u * sc, line, (grow == 1u) ? hi : white);
                }
                ov_text(&s, x0 + 8u * sc, y0 + 58u * sc, (grow == 2u) ? "> EXIT TO MENU" : "  EXIT TO MENU", (grow == 2u) ? hi : white);
                p = ov_cat(line, (grow == 3u) ? "> SUSPEND TO SLOT " : "  SUSPEND TO SLOT ");
                ov_dec(p, cur + 1u);
                ov_text(&s, x0 + 8u * sc, y0 + 72u * sc, line, (grow == 3u) ? hi : white);
                if (pak_on) {
                    p = ov_cat(line, (grow == 4u) ? "> PAK       < " : "  PAK       < ");
                    if (vpak_in) {
                        p = ov_cat(p, "IN PORT ");
                        p = ov_dec(p, vpak_ch + 1u);
                    } else {
                        p = ov_cat(p, "OUT");
                    }
                    ov_cat(p, " >");
                    ov_text(&s, x0 + 8u * sc, y0 + 86u * sc, line, (grow == 4u) ? hi : white);
                }
                p = ov_cat(line, (grow == gdel) ? "> DELETE SLOT " : "  DELETE SLOT ");
                ov_dec(p, cur + 1u);
                ov_text(&s, x0 + 8u * sc, y0 + (pak_on ? 100u : 86u) * sc, line, (grow == gdel) ? hi : ((cst & 0xFFu) ? white : grey));
                if (confirm == 2u) {
                    menu_footer(&s, x0, y0, w, h, "LEAVE THE GAME?  A YES   B NO", red);
                } else if (confirm == 3u) {
                    menu_footer(&s, x0, y0, w, h, (cst & 0xFFu) ? "OVERWRITE AND LEAVE?  A YES  B NO" : "SAVE AND LEAVE?  A YES   B NO", red);
                } else if (confirm == 5u) {
                    p = ov_cat(line, "DELETE SLOT ");
                    p = ov_dec(p, cur + 1u);
                    ov_cat(p, "?  A YES   B NO");
                    menu_footer(&s, x0, y0, w, h, line, red);
                } else {
                    uint32_t hy = pak_on ? 120u : 106u;   /* the hint lines, under the last row */
                    if (speed_div == SPEED_STEP) {
                        char hk[80], kb[40];
                        ov_cat(ov_cat(ov_cat(hk, "TAP "), combo_text(step_button(), kb)), " FOR ONE FRAME");
                        ov_text(&s, x0 + 8u * sc, y0 + hy * sc, hk, grey);
                        ov_cat(ov_cat(hk, combo_text(hook_cfg.combo_menu, kb)), " FOR THIS PANEL");
                        ov_text(&s, x0 + 8u * sc, y0 + (hy + 10u) * sc, hk, grey);
                    } else if (speed_div > 1u) {
                        ov_text(&s, x0 + 8u * sc, y0 + hy * sc, slow_sound ? "SOUND PLAYS WITH GAPS" : "SOUND SLOWED WITH THE GAME", grey);
                    } else if (grow == 2u) {
                        ov_text(&s, x0 + 8u * sc, y0 + hy * sc, "BACK TO THE SC64 MENU", grey);
                    } else if (grow == 3u) {
                        ov_text(&s, x0 + 8u * sc, y0 + hy * sc, "THE NEXT LAUNCH RESUMES HERE", grey);
                    } else if (pak_on && (grow == 4u)) {
                        ov_text(&s, x0 + 8u * sc, y0 + hy * sc, vpak_in ? "IN: THE GAME SEES THIS PAK IN THE PORT" : "OUT: THE GAME SEES THE REAL SLOT", grey);
                    } else if (grow == gdel) {
                        ov_text(&s, x0 + 8u * sc, y0 + hy * sc, (cst & 0xFFu) ? "THE STATE IS GONE, THE SLOT EMPTY AGAIN" : "THE SLOT IS EMPTY ALREADY", grey);
                    }
                    menu_footer(&s, x0, y0, w, h, ((grow == 2u) || (grow == 3u) || (grow == gdel)) ? "A SELECT   B CLOSE   < SLOTS" : "< > CHANGE   B CLOSE   L:SLOTS", white);
                }
            }
            if (note) ov_text(&s, x0 + w - 8u * sc - 7u * sc * 12u, y0 + 8u * sc, note, hi);
            redraw = 0;
        }
        uint32_t b = 0;
        if (!pad_dma_poll(&b)) {
            uint32_t t1 = c0_count();
            while ((c0_count() - t1) < (46875u * 16u)) { vi_frz_service(); }
            continue;
        }
        uint32_t pressed = b & ~prev;
        if (prev == 0xFFFFu) pressed = 0;          /* the buttons that opened the menu do not count */
        prev = b;
        int32_t sy = (int32_t)(int8_t)(pif_stick_y), sx = (int32_t)(int8_t)(pif_stick_x);
        uint32_t up = (pressed & 0x0800u) != 0, down = (pressed & 0x0400u) != 0;
        uint32_t left = (pressed & 0x0200u) != 0, right = (pressed & 0x0100u) != 0;
        if ((c0_count() - last_move) > (46875u * 180u)) {
            if (sy > 40) up = 1;
            if (sy < -40) down = 1;
            if (sx < -40) left = 1;
            if (sx > 40) right = 1;
            if ((b & 0x0800u) && !up) up = 1;
            if ((b & 0x0400u) && !down) down = 1;
            if ((b & 0x0200u) && !left) left = 1;
            if ((b & 0x0100u) && !right) right = 1;
        }
        if (up || down || left || right) {
            last_move = c0_count();
        }
        if (!confirm && (pressed & 0x0030u)) {   /* L or R: the other page */
            page ^= 1u;
            redraw = 1;
        } else if ((confirm == 2u) || (confirm == 3u)) {   /* leaving: yes or no */
            if (pressed & 0x8000u) {
                if (confirm == 3u) {
                    confirm = 0;
                    exit_after = 1u;
                    st_suspend = 1u;
                    goto do_save;
                }
                confirm = 0;
                menu_footer(&s, x0, y0, w, h, "LEAVING...", hi);
                if (!menu_exit()) note = "NO EXIT HERE";   /* the cart did not switch */
                redraw = 1;
            } else if (pressed & 0x4000u) {
                confirm = 0;
                redraw = 1;
            }
        } else if (confirm == 5u) {              /* deleting: yes or no */
            if (pressed & 0x8000u) {
                confirm = 0;
                menu_footer(&s, x0, y0, w, h, "DELETING...", hi);
                note = card_delete(cur) ? "DELETED     " : "NOT DELETED ";
                thumb_n = SLX_NONE;
                redraw = 1;
            } else if (pressed & 0x4000u) {
                confirm = 0;
                redraw = 1;
            }
        } else if (page == 1u) {
            if (up || down) {
                if (borrowed_mode()) {                /* EXIT, SUSPEND, PAK and DELETE only */
                    uint32_t k = grow - 2u, m = rows - 2u;
                    grow = 2u + (up ? ((k + m - 1u) % m) : ((k + 1u) % m));
                } else {
                    grow = up ? ((grow + rows - 1u) % rows) : ((grow + 1u) % rows);
                }
                redraw = 1;
            }
            if (pak_on && (grow == 4u) && (left || right || (pressed & 0x8000u))) {   /* the pak: out, or in at port 1..4 */
                uint32_t st = vpak_in ? (vpak_ch + 1u) : 0u;
                st = left ? ((st + 4u) % 5u) : ((st + 1u) % 5u);
                pak_live_change(st);
                if (!pak_live_store()) note = "CART BUSY";
                redraw = 1;
            }
            if (((grow == 2u) || (grow == 3u) || (grow == gdel)) && (left || right)) {   /* no value here: the other page */
                page = 0;
                redraw = 1;
                continue;
            }
            if ((grow < 2u) && (left || right || (pressed & 0x8000u))) {
                if (grow == 0) {
                    speed_sel = (right || (pressed & 0x8000u)) ? ((speed_sel + 1u) % 5u) : ((speed_sel + 4u) % 5u);
                    speed_div = speed_divs[speed_sel];
                    step_prev = 0xFFFFu;
                } else {
                    slow_sound ^= 1u;
                }
                redraw = 1;
            } else if (((grow == 2u) || (grow == 3u)) && (pressed & 0x8000u)) {   /* A: exit, or suspend */
                if ((grow == 3u) && !n) { note = "NO SLOTS    "; redraw = 1; continue; }
                if ((grow == 3u) && (sd_state == 1u)) { note = "CARD BUSY"; redraw = 1; continue; }
                confirm = (grow == 2u) ? 2u : 3u;
                redraw = 1;
            } else if ((grow == gdel) && (pressed & 0x8000u)) {   /* A: delete the highlighted slot's state */
                if (!(cst & 0xFFu)) { note = "SLOT EMPTY  "; redraw = 1; continue; }
                if (sd_state == 1u) { note = "CARD BUSY"; redraw = 1; continue; }
                confirm = 5u;
                redraw = 1;
            }
            if (pressed & 0x5000u) {              /* B or Start: close */
                result = 0;
                break;
            }
        } else if (confirm == 6u) {              /* the refusal: back */
            if (pressed & 0xC000u) {
                confirm = 0;
                redraw = 1;
            }
        } else if (confirm) {                    /* 1 overwrite, 4 the last empty slot */
            if (pressed & 0x8000u) {              /* A: go on */
                confirm = 0;
                goto do_save;
            }
            if (pressed & 0x4000u) {              /* B */
                confirm = 0;
                redraw = 1;
            }
        } else {
            if (n && (up || down)) {
                cur = up ? ((cur + n - 1u) % n) : ((cur + 1u) % n);
                redraw = 1;
            }
            if (n && (pressed & 0x0008u)) {       /* C-up: a page up */
                cur = (cur >= MENU_MAX_ROWS) ? (cur - MENU_MAX_ROWS) : 0u;
                redraw = 1;
            }
            if (n && (pressed & 0x0004u)) {       /* C-down: a page down */
                cur = ((cur + MENU_MAX_ROWS) < n) ? (cur + MENU_MAX_ROWS) : (n - 1u);
                redraw = 1;
            }
            if (left || right) {                  /* the Game page, as L and R */
                page = 1u;
                redraw = 1;
                continue;
            }
            if (pressed & 0x4000u || pressed & 0x1000u) {   /* B or Start: close */
                result = 0;
                break;
            }
            if ((pressed & 0x8000u) && (cst & 0xFFu)) {   /* A: load */
                if (borrowed_mode() && (cst & SLX_RESIDENT)) { confirm = 6u; redraw = 1; continue; }   /* saved with Slow motion on: it needs it */
                if (sd_state == 1u) { note = "CARD BUSY"; redraw = 1; continue; }
                menu_footer(&s, x0, y0, w, h, (cst & 0xFF00u) ? "LOADING..." : "READING THE CARD...", hi);
                uint32_t base = card_bind(cur, 1u, 0xFFu);   /* its cart slot; the file read in when it is not there */
                if (!base) { note = "CARD ERROR  "; redraw = 1; continue; }
                st_slot = base;
                st_card = cur;
                hook_cfg.cur_slot = cur;
                result = 1;
                menu_loads++;
                menu_footer(&s, x0, y0, w, h, "LOADING...", hi);
                /* the press must be over before the load starts: the loaded game's first
                 * poll found the A that picked the slot and acted on it (Episode I Racer);
                 * a tap shorter than a poll never showed it */
                {
                    uint32_t t1 = c0_count();
                    while ((c0_count() - t1) < (46875u * 1000u)) {
                        if (!pad_dma_poll(&b) || !(b & 0x8000u)) break;
                        vi_frz_service();
                    }
                }
                break;
            }
            if (n && (pressed & 0x2000u)) {       /* Z: save */
                if (sd_state == 1u) { note = "CARD BUSY"; redraw = 1; continue; }
                if (cst & 0xFFu) { confirm = 1; redraw = 1; continue; }
                if (card_empties(n, cur) == 0) { confirm = 4u; redraw = 1; continue; }   /* the last empty slot: say so first */
                goto do_save;
            }
        }
        {
            uint32_t t1 = c0_count();
            while ((c0_count() - t1) < (46875u * 16u)) { vi_frz_service(); }
        }
        continue;
do_save:
        {
            menu_footer(&s, x0, y0, w, h, exit_after ? "SAVING, THEN LEAVING..." : "SAVING...", hi);
            uint32_t base = card_bind(cur, 0, 0xFFu);   /* a cart slot for it (the least recently used one goes) */
            if (!base) {
                note = "NO CART SLOT";
                exit_after = 0;
                st_suspend = 0;
                redraw = 1;
                continue;
            }
            st_slot = base;
            st_card = cur;
            hook_cfg.cur_slot = cur;
            st_dma_ticks = 0;
            uint32_t r = state_do_save(cause, mi);
            st_suspend = 0;
            if (r == ST_OK) {
                slot_patch_clean_frame(st_slot);  /* the state gets the frame under the panel */
                rom_write_set(0);                 /* before the mirror (see the combo save path) */
                card_set(cur, st_hdr.flags, st_hdr.stamp, st_hdr.stamp_time, st_hdr.image_len);
                if (!sd_write_card2(cur, st_slot, 0) && (sd_state == 1u)) {
                    sd_pending_slot = st_slot;
                }
                menu_saves++;
                note = "SAVED       ";
                if (exit_after) {
                    exit_after = 0;
                    menu_footer(&s, x0, y0, w, h, "LEAVING...", hi);
                    if (!menu_exit()) note = "NO EXIT HERE";
                }
            } else {
                note = "SAVE FAILED ";
                exit_after = 0;
            }
            /* the panel is still there; the frame under it is unchanged */
            redraw = 1;
        }
    }
    c0_set_compare(c0_count() + 46875u);          /* the OS timer after a long freeze */
    return result;
}

/* ---- quick-combo feedback drawn after the fact -------------------------------------
 * Shadowed text, drawn once into each buffer as it becomes the displayed one (checked
 * on every exception, so it lands within a few hundred microseconds of the swap). The
 * earlier version painted a box and the text into the three buffers last seen on every
 * VI tick: ~17000 uncached stores in Turok 2's hi-res mode (2.7 ms), which held the
 * game's own VI programming past the top of the field and made the picture judder for
 * as long as the message was up. */
static uint32_t fb_drawn = 0;

/* interlaced modes show one buffer from two origins a line apart: one name for both */
static uint32_t fb_same_buffer(uint32_t origin) {
    if (vi_fld_lineoff && (vi_fld_seen == 3u)) {
        uint32_t d = (uint32_t)vi_fld_lineoff;
        for (uint32_t k = 0; k < 3u; k++) {
            if (fb_origins[k] && ((origin == fb_origins[k] + d) || (origin == fb_origins[k] - d))) {
                return (fb_origins[k] < origin) ? fb_origins[k] : origin;   /* the buffer's base */
            }
        }
    }
    return ov_field_base(origin);
}

static void feedback_service(void) {
    if (!msg_ticks || !msg_text) return;
    uint32_t origin = fb_same_buffer(VI_ORIGIN_REG & 0x00FFFFFFu);
    if (!origin || (origin == fb_drawn)) return;
    if (fb_expect_width && (VI_WIDTH_REG != fb_expect_width)) return;   /* the game has not set its mode yet */
    if ((VI_STATUS_REG & 3u) == 0) return;                               /* blanked: the origin may be stale */
    struct ov_screen s;
    ov_screen_read(&s);
    if (!ov_screen_ok(&s)) return;
    s.fb = 0xA0000000u | origin;
    if ((origin + s.width * s.height * s.bpp) > 0x007C0000u) return;   /* never past the image (the hook lives above it) */
    uint32_t sc = s.scale, x = 22u * sc, y = s.height - 36u * sc;
    ov_text(&s, x + sc, y + sc, msg_text, ov_rgb(&s, 8, 8, 24));
    ov_text(&s, x, y, msg_text, ov_rgb(&s, 255, 220, 60));
    fb_drawn = origin;
}

static uint32_t ready_shown = 0, ready_origin0 = 0, ready_ticks = 0;

static void feedback_tick(void) {
    uint32_t origin = fb_same_buffer(VI_ORIGIN_REG & 0x00FFFFFFu);
    if (!ready_shown && hook_cfg.feedback) {
        /* Once the game is up and showing its own picture, never earlier: right after
         * the hand-over the VI still points at the menu's leftover buffer, which is
         * where the game is loading its code (Mario 64 booted to a black screen when
         * the text went there). So: the origin has moved on from the one seen first,
         * the VI shows a picture, the game has polled the controller, two seconds in. */
        uint32_t o = VI_ORIGIN_REG & 0x00FFFFFFu;
        if (ready_origin0 == 0) ready_origin0 = o ? o : 1u;
        if ((++ready_ticks > 120u) && pad_valid && o && (o != ready_origin0) && ((VI_STATUS_REG & 3u) != 0)) {
            struct ov_screen s;
            ov_screen_read(&s);
            if (ov_screen_ok(&s)) {
                ready_shown = 1;
                feedback_show("SAVE STATES READY");
            }
        }
    }
    if (origin && (origin != fb_origins[0])) {
        fb_origins[2] = fb_origins[1];
        fb_origins[1] = fb_origins[0];
        fb_origins[0] = origin;
    }
    if (msg_ticks) msg_ticks--;
    feedback_service();
}

/* A load replaced the world: the buffers seen so far may be anything now. */
static void feedback_reset(uint32_t origin, uint32_t width) {
    fb_origins[0] = origin;
    fb_origins[1] = 0;
    fb_origins[2] = 0;
    fb_drawn = 0;
    fb_expect_width = width;
}

static void feedback_show(const char *text) {
    if (!hook_cfg.feedback) return;
    msg_text = text;
    msg_ticks = MSG_TICKS;
}
