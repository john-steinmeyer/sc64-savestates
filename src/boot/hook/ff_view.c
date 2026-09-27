/* ---- Frozen frame: VIEW on a loaded state, and the photo pause (SC64SS_VIEW) ---------------
 * Right after a load, before the loaded world's hardware setup and resume: the frame the
 * state's graphics task drew is drawn again every field, its perspective projection matrices
 * rewritten in place for a camera the pad (or the PC, through the staged cfg's spare words)
 * flies. Then the state is loaded once more, so RAM and the RCP are exactly the state's
 * again, and the game resumes as after any load. The photo pause saves the live world into a
 * scratch cart slot first and drops it after. Nothing here writes to a card file. The generic
 * part (the walk, the matrix maths) is ff_core.c, host-tested. */
#include "ff_core.c"

#define SP_MEM_ADDR   (*(vu32 *)0xA4040000u)
#define SP_DRAM_ADDR  (*(vu32 *)0xA4040004u)
#define SP_RD_LEN     (*(vu32 *)0xA4040008u)

#define FF_STEP       (6 * FX_ONE)          /* world units a frame at plain speed */
#define FF_TURN       1966                  /* radians a frame at full stick: 0.03 */
#define FF_PITCH_MAX  (3 * FX_ONE / 2)      /* 1.5 rad */
#define FF_TENTH_DEG  114                   /* 16.16 radians per tenth of a degree (the PC's unit) */
#define FF_MAX_REC    12u                   /* task records of the game's kept from the RAM scan */

struct ff_view {
    struct ff_map map;
    uint32_t task[16];
    fx yaw, pitch, pos[3];
    fx delta[4][4], out[4][4];
    uint8_t raw[64];
    uint32_t buf[2], cur, frames, cmd_seq, far_x, prev, line_off;
    fx zoom;                                /* the field of view: 1.0 = the game's */
    fx roll;                                /* the tilt about the view axis */
    uint32_t r_stick;                       /* the stick moved during the R hold (then R was not a tap) */
    uint32_t msg_frames;                    /* frames the screenshot's message still shows for */
    const char *msg;
    uint32_t spare;                         /* a colour buffer of our own in memory the frame never touches (0: none) */
    uint32_t cfg_base;                      /* the staged cfg on the cart, KSEG1: the PC's words live there */
    uint32_t data_src, data_len;            /* the microcode's data segment in RAM (the original, not a yield buffer) */
    uint32_t segtab_off;                    /* the family's segment table in DMEM */
    uint32_t segtab[16];                    /* as the state left it: put back after the data goes in */
    uint32_t seed_ok;                       /* DMEM is this task's (the RSP ran it last): the table above applies */
    uint32_t src_ram;                       /* the record came from RAM (its 16 words go to DMEM 0xFC0 for the boot) */
    uint32_t n_rec, rec[FF_MAX_REC];        /* the game's task records in RAM (type 1, plausible) */
    uint32_t rec_cimg[FF_MAX_REC];          /* the main colour image of each record's list (0: no clean walk) */
    uint32_t hud_off, fog_off;              /* the layers: the 2D rectangles and ortho passes hidden; the fog words zeroed */
    uint32_t zc[16] __attribute__((aligned(16)));   /* the depth clear: six RDP commands of our own (whole cache lines) */
    uint32_t width, height;                 /* the frame's colour image, pixels */
    uint32_t rect_w0[FF_MAX_RECT];          /* the originals, to put back */
    uint32_t fog_w1[FF_MAX_FOG];
    uint32_t ortho_save[FF_MAX_ORTHO][16];
};
#define ffv (*(struct ff_view *)(void *)thumb_buf)              /* thumb_buf (9.6 KiB) is idle during a view */
typedef char ff_view_fits_thumb[(sizeof(struct ff_view) <= THUMB_BYTES) ? 1 : -1];

/* progress for the PC in the staged cfg (+0x94: over in bit 31, stage in bits 24..30, the RSP's
 * PC in bits 12..23, frames in bits 0..11) */
static void view_pc_report(uint32_t done, uint32_t stage, uint32_t pc) {
    pio_write(ffv.cfg_base + 0x94u, (done ? 0x80000000u : 0u) | ((stage & 0x7Fu) << 24) | ((pc & 0xFFFu) << 12) | (ffv.frames & 0xFFFu));
}

/* A cart slot as scratch for the photo pause: a free one, else the least recently used whose
 * mirror is not in flight (card_bind's rule), its header zeroed, the card slot it held
 * forgetting it (the card has that state), the cart table entry left free: no card slot is
 * bound, so nothing is mirrored and no later launch can take the scratch for a state. */
static uint32_t cart_scratch_take(void) {
    uint32_t k = hook_cfg.slots_n, p = CFG_SLOTS_MAX, best = SLX_NONE, i;
    if (k > CFG_SLOTS_MAX) k = CFG_SLOTS_MAX;
    for (i = 0; i < k; i++) {
        uint32_t base = hook_cfg.slots[i], c = 0, stamp = 0;
        if (!base) continue;
        if (((sd_state == 1u) && (sd_slot == base)) || (sd_pending_slot == base)) continue;
        slx_rd(SLX_CART + 8u * i, &c);
        if (c == 0) { p = i; break; }
        slx_rd(SLX_CART + 8u * i + 4u, &stamp);
        if ((best == SLX_NONE) || (stamp < best)) { best = stamp; p = i; }
    }
    if (p >= k) return 0;
    {
        uint32_t base = hook_cfg.slots[p], old = cart_card(p), was = slx_wren();
        if (was == 0xFFu) return 0;
        if (old != SLX_NONE) {
            uint32_t ost = 0;
            slx_rd(SLX_CARD + 12u * old, &ost);
            slx_wr(SLX_CARD + 12u * old, ost & 0xFFu);
        }
        pio_write(0xA0000000u | (base + STATE_HDR_OFF), 0);
        slx_wr(SLX_CART + 8u * p, 0);
        slx_wrdone(was);
        return base;
    }
}

static void cart_scratch_drop(uint32_t base) {
    uint32_t was = slx_wren();
    if (was == 0xFFu) return;
    pio_write(0xA0000000u | (base + STATE_HDR_OFF), 0);
    slx_wrdone(was);
}

/* the game's own task records in RAM: type 1, flags small, a list pointer and a text and a
 * data address that look like RAM, sizes that fit the RSP; the first FF_MAX_REC of them */
static void view_scan_records(uint32_t limit) {
    struct ff_view *v = &ffv;
    uint32_t a;
    v->n_rec = 0;
    for (a = 0x400u; (a + 64u <= limit) && (v->n_rec < FF_MAX_REC); a += 8u) {
        const vu32 *r = (const vu32 *)(0xA0000000u | a);
        uint32_t dp = r[12] & 0x1FFFFFFFu;
        if (r[0] != 1u || r[1] > 0xFFu) continue;
        if ((dp & 7u) || (dp < 0x400u) || (dp >= limit) || (r[13] > 0x200000u)) continue;
        if ((r[4] & 0x1FFFFFFFu) < 0x400u || (r[6] & 0x1FFFFFFFu) < 0x400u || !r[2]) continue;
        if (r[5] > 0x1000u || r[7] > 0x1000u || r[3] > 0x1000u) continue;
        v->rec[v->n_rec++] = a;
    }
}

/* walk the task in ffv.task with the family its microcode names (the other as the fallback),
 * the segment table seeded from DMEM when the record is the RSP's own; 1 when clean */
static uint32_t view_walk_task(struct ff_mem *mem, uint32_t seed) {
    struct ff_view *v = &ffv;
    uint32_t i, clean = 0;
    /* the eight address fields the OS converts on load (a game's own record may hold virtual
     * ones): boot, text, data, stack, output buffer and its end, list, yield buffer */
    for (i = 2; i < 16u; i += 2u) v->task[i] &= 0x1FFFFFFFu;
    v->task[11] &= 0x1FFFFFFFu;
    for (i = 0; i < 2u && !clean; i++) {
        uint32_t family = (i == 0) ? FF_FAM_F3DEX2 : FF_FAM_F3D;   /* the walk proves the encoding */
        uint32_t seg[16], k;
        uint32_t tab = (family == FF_FAM_F3D) ? FF_SEGTAB_F3D : FF_SEGTAB_F3DEX2;
        for (k = 0; k < 16u; k++) seg[k] = seed ? *(vu32 *)(0xA4000000u + tab + 4u * k) : 0u;
        clean = (uint32_t)ff_walk(mem, v->task[12], family, seg, &v->map);
    }
    return clean;
}

/* The microcode's data segment as the boot would load it. A task the game yielded to its
 * audio and resumed has its record's data field pointing at the yield buffer (a snapshot of
 * the RSP's memory mid-task). The original data is the other copy in RAM of the version
 * string the data carries (its offset inside the data is the string's offset in the buffer;
 * the data's first bytes change during a task, the string does not), outside the yield
 * buffer; failing that, a game record with the same text whose data is not the yield buffer. */
static void view_find_data(uint32_t limit) {
    struct ff_view *v = &ffv;
    const uint32_t *t = v->task;
    uint32_t ylo = t[14], yhi = t[14] + t[15];
    uint32_t needle[8], a, i, off = 0;
    v->data_src = t[6];
    v->data_len = (t[7] && (t[7] <= 0x800u)) ? t[7] : 0x800u;
    if (!t[14] || (t[6] != t[14]) || (t[6] + 0x800u > limit)) return;   /* the record's own data: not a yield buffer */
    for (a = 0; a + 4u <= 0x800u; a += 4u) {
        if (*(const vu32 *)(0xA0000000u | (t[6] + a)) == 0x52535020u) { off = a; break; }   /* "RSP " */
    }
    if (off) {
        for (i = 0; i < 8u; i++) needle[i] = *(const vu32 *)(0xA0000000u | (t[6] + off + 4u * i));
        for (a = 0x400u + off; a + 32u <= limit; a += 4u) {
            const vu32 *p = (const vu32 *)(0xA0000000u | a);
            if (p[0] != needle[0] || p[1] != needle[1]) continue;
            if ((a >= ylo) && (a < yhi)) continue;
            for (i = 2; i < 8u; i++) if (p[i] != needle[i]) break;
            if (i == 8u) {
                v->data_src = a - off;
                v->data_len = 0x800u;
                return;
            }
        }
    }
    for (i = 0; i < v->n_rec; i++) {
        const vu32 *r = (const vu32 *)(0xA0000000u | v->rec[i]);
        uint32_t d = r[6] & 0x1FFFFFFFu;
        if (((r[4] & 0x1FFFFFFFu) != t[4]) || (d == t[14]) || (d + 0x800u > limit) || !r[7] || (r[7] > 0x800u)) continue;
        v->data_src = d;
        v->data_len = r[7];
        return;
    }
}

static uint32_t view_sp_dma_wait(void) {
    uint32_t t0 = c0_count();
    while (SP_STATUS & (SP_DMA_BUSY | SP_DMA_FULL)) {
        if ((c0_count() - t0) > 46875u * 20u) return 0;
    }
    return 1u;
}

/* The frame's task once more, exactly as osSpTaskLoad and osSpTaskStartGo run it: the task
 * record in DMEM 0xFC0 (the state's own, or the game's from RAM; its data field repaired
 * when the last run resumed from a yield), the boot microcode into IMEM, PC 0, go. Then the
 * RSP's break and the RDP's idle, and the interrupts the two raised cleared. */
static uint32_t view_task_run(void) {
    struct ff_view *v = &ffv;
    const uint32_t *t = v->task;
    uint32_t t0, i;
    if (!view_sp_dma_wait()) return 0;
    if (v->src_ram) {
        for (i = 0; i < 16u; i++) *(vu32 *)(0xA4000FC0u + 4u * i) = t[i];   /* the game's record, for the boot */
    }
    *(vu32 *)0xA4000FC4u &= ~1u;            /* OS_TASK_YIELDED off: a whole task, from its start */
    *(vu32 *)0xA4000FD8u = v->data_src;     /* the data segment, not a yield buffer */
    *(vu32 *)0xA4000FDCu = v->data_len;
    SP_MEM_ADDR = 0x1000u;                  /* IMEM */
    SP_DRAM_ADDR = t[2];
    SP_RD_LEN = ((t[3] && (t[3] <= 0x1000u)) ? t[3] : 0xD0u) - 1u;
    if (!view_sp_dma_wait()) return 0;
    SP_PC_REG = 0;
    /* clear halt, clear broke, clear sstep, set interrupt on break, clear signals 0..2 */
    SP_STATUS = (1u << 0) | (1u << 2) | (1u << 5) | (1u << 8) | (1u << 9) | (1u << 11) | (1u << 13);
    t0 = c0_count();
    while (!(SP_STATUS & SP_HALT)) {
        if ((c0_count() - t0) > 46875u * 250u) {
            uint32_t pc = SP_PC_REG;
            SP_STATUS = (1u << 1);          /* halt it */
            view_pc_report(0, 6u, pc);
            return 0;
        }
    }
    t0 = c0_count();
    while (DPC_STATUS & (DPC_PIPE_BUSY | DPC_CMD_BUSY | DPC_DMA_BUSY)) {
        if ((c0_count() - t0) > 46875u * 250u) {
            view_pc_report(0, 7u, SP_PC_REG);
            return 0;
        }
    }
    SP_STATUS = SP_CLR_INTR;
    MI_INIT_MODE = MI_MODE_CLR_DP;
    return 1u;
}

/* a fill that covers most of the frame: a clear (Banjo-Tooie's is letterboxed) */
static __attribute__((noinline)) uint32_t view_rect_is_clear(uint32_t i) {
    struct ff_view *v = &ffv;
    uint32_t d = v->map.rect_dim[i];
    return (v->map.rect_pc[i] & 0x80000000u) && ((d >> 16) * 4u >= v->width * 3u) && ((d & 0xFFFFu) * 2u >= v->height);
}

/* The depth image cleared by six RDP commands of our own before every frame: games that clear
 * it by drawing a sky over the whole view (Banjo-Tooie) leave the last frame's depths wherever
 * the moved view shows no sky, and geometry then shows through geometry. */
static uint32_t view_zclear(void) {
    struct ff_view *v = &ffv;
    uint32_t *c = v->zc, w = v->width, h = v->height, t0, i;
    if (!v->map.n_zimg || !w || !h) return 1u;
    c[0] = 0xFF100000u | (w - 1u);            c[1] = v->map.zimg[0];      /* SETCIMG: 16-bit, the frame's width, the depth image */
    c[2] = 0xED000000u;                        c[3] = ((w << 2) << 12) | (h << 2);   /* SETSCISSOR (0,0)-(w,h) */
    c[4] = 0xEF300000u;                        c[5] = 0;                   /* SETOTHERMODE: fill cycle */
    c[6] = 0xF7000000u;                        c[7] = 0xFFFCFFFCu;         /* SETFILLCOLOR: the far depth */
    c[8] = 0xF6000000u | (((w - 1u) << 2) << 12) | ((h - 1u) << 2); c[9] = 0;   /* FILLRECT (0,0)-(w-1,h-1) */
    c[10] = 0xE9000000u;                       c[11] = 0;                  /* FULLSYNC */
    for (i = 0; i < 16u; i += 4u) __asm__ volatile("cache 0x19, 0(%0)" : : "r"(c + i) : "memory");   /* Hit_Writeback_D, four lines */
    DPC_STATUS = DPC_CLR_XBUS;
    DPC_START = (uint32_t)(uintptr_t)c & 0x1FFFFFFFu;
    DPC_END = ((uint32_t)(uintptr_t)c & 0x1FFFFFFFu) + 48u;
    t0 = c0_count();
    while (DPC_STATUS & (DPC_PIPE_BUSY | DPC_CMD_BUSY | DPC_DMA_BUSY)) {
        if ((c0_count() - t0) > 46875u * 50u) return 0;
    }
    MI_INIT_MODE = MI_MODE_CLR_DP;
    return 1u;
}

/* The HUD layer: every 2D rectangle that is not a full-screen clear becomes an RDP no-op, and
 * every ortho projection matrix a point (only w left), so its triangles have no area. The
 * originals go back when the layer returns. The fog: the fog words zeroed (multiplier and
 * offset 0 leave every vertex unfogged), the originals back after. */
static void view_apply_layers(void) {
    struct ff_view *v = &ffv;
    uint32_t i, j;
    for (i = 0; i < v->map.n_rect; i++) {
        uint32_t pc = v->map.rect_pc[i];
        if (view_rect_is_clear(i)) continue;                          /* a clear */
        *(vu32 *)(0xA0000000u | pc) = v->hud_off ? 0u : v->rect_w0[i];   /* opcode 0: a no-op in both families (0xC0 is not one in F3DEX2) */
    }
    for (i = 0; i < v->map.n_orthom; i++) {
        vu32 *m = (vu32 *)(0xA0000000u | v->map.ortho_addr[i]);
        for (j = 0; j < 16u; j++) m[j] = v->hud_off ? ((j == 7u) ? 0x00010000u : 0u) : v->ortho_save[i][j];
    }
    for (i = 0; i < v->map.n_fog; i++) {
        *(vu32 *)(0xA0000000u | (v->map.fog_pc[i] + 4u)) = v->fog_off ? 0u : v->fog_w1[i];
    }
}

/* A screenshot of the view, through the screenshot button's own path: the shown frame into
 * the frame stash as a PNG, the card write started and driven to its end here (the load's
 * exit never reaches the visit's tail), the file's fill and count into the staged cfg (the
 * menu files the shots at its next start from there). */
static uint32_t view_shot(void) {
    uint32_t t0, r;
    if (!shot_room()) return ST_NO_ROOM;
    r = shot_capture();
    if (r != ST_OK) return r;
    if (!sd_write_begin_shot(shot_len)) return ST_SD_FAIL;
    t0 = c0_count();
    while ((sd_state == 1u) || sd_shot_pending) {
        sd_service();
        if ((c0_count() - t0) > 46875u * 8000u) break;
    }
    rom_write_set(1u);
    pio_write(ffv.cfg_base + 0xACu, hook_cfg.shot_fill);
    pio_write(ffv.cfg_base + 0xB0u, hook_cfg.shot_count);
    pio_write(ffv.cfg_base + 0x88u, hook_cfg.pc_shot);
    return ST_OK;
}

/* move along the camera's own axes (the columns of the delta are the camera's axes in the
 * original eye space): forward = -z, right = x, up = y */
static void view_move(fx fwd, fx right, fx up) {
    uint32_t i;
    for (i = 0; i < 3u; i++) {
        ffv.pos[i] += ff_mul(right, ffv.delta[i][0]) + ff_mul(up, ffv.delta[i][1]) - ff_mul(fwd, ffv.delta[i][2]);
    }
}

/* the PC's command word (+0x9C: sequence in bits 24..31, command in 16..23, a signed value
 * below); 1 yaw, 2 pitch in tenths of a degree, 3 forward, 4 right, 5 up in units, 6 reset,
 * 7 leave, 8 the far plane four times out (1) or the game's (0) */
static uint32_t view_pc_command(void) {
    uint32_t w = 0, seq, cmd;
    int32_t val;
    if (!pio_read(ffv.cfg_base + 0x9Cu, &w)) return 0;
    seq = w >> 24;
    if (!seq || (seq == ffv.cmd_seq)) return 0;
    ffv.cmd_seq = seq;
    cmd = (w >> 16) & 0xFFu;
    val = (int32_t)(int16_t)(w & 0xFFFFu);
    switch (cmd) {
    case 1u: ffv.yaw += val * FF_TENTH_DEG; break;
    case 2u: ffv.pitch += val * FF_TENTH_DEG; break;
    case 3u: view_move(val * FX_ONE, 0, 0); break;
    case 4u: view_move(0, val * FX_ONE, 0); break;
    case 5u: view_move(0, 0, val * FX_ONE); break;
    case 6u: ffv.yaw = ffv.pitch = ffv.roll = 0; ffv.pos[0] = ffv.pos[1] = ffv.pos[2] = 0; ffv.zoom = FX_ONE; break;
    case 7u: return 1u;
    case 8u: ffv.far_x = val ? 1u : 0u; break;
    case 9u: view_shot(); break;
    case 10u: ffv.hud_off = val ? 1u : 0u; view_apply_layers(); break;
    case 11u: ffv.fog_off = val ? 1u : 0u; view_apply_layers(); break;
    default: break;
    }
    return 0;
}

static uint32_t view_run(void) {
    struct ff_view *v = &ffv;
    struct ff_mem mem;
    uint32_t i, clean = 0;
    zero(v, sizeof *v);
    v->cfg_base = 0xA0000000u | (HOOK_STAGING_PI + ((uint32_t)(uintptr_t)&hook_cfg - 0x807D0000u));
    mem.base = (const uint8_t *)0xA0000000u;    /* uncached: the loaded image as the RSP sees it */
    mem.limit = image_len();
    if (borrowed_mode()) {
        mem.deny_lo = BORROW_LO;
        mem.deny_hi = BORROW_HI;
    } else {
        mem.deny_lo = 0x7C0000u;
        mem.deny_hi = 0x800000u;
    }
    view_scan_records(mem.limit);
    /* every record of the game's walked once: its list and the colour image its frame draws
     * (the second buffer of the swap must be one of these; nothing else is known to be a
     * whole frame's buffer). Then the frame's task: the record the RSP ran last (DMEM 0xFC0)
     * when that is a graphics one whose list walks clean; else the game's own records, the
     * clean one whose colour image the VI shows preferred, else the first clean one. */
    {
        uint32_t r;
        for (r = 0; r < v->n_rec; r++) {
            const vu32 *rec = (const vu32 *)(0xA0000000u | v->rec[r]);
            for (i = 0; i < 16u; i++) v->task[i] = rec[i];
            /* a whole frame's list, not one of the task-shaped scraps a game's memory holds
             * (Banjo-Kazooie: two-triangle lists naming garbage as their colour image) */
            v->rec_cimg[r] = (view_walk_task(&mem, 0) && v->map.n_persp && (v->map.commands >= 64u) && !(v->map.main_cimg & 7u))
                             ? v->map.main_cimg : 0u;
        }
    }
    for (i = 0; i < 16u; i++) v->task[i] = *(vu32 *)(0xA4000FC0u + 4u * i);
    if (v->task[0] == 1u) {
        clean = view_walk_task(&mem, 1u) && v->map.n_persp;   /* a 2D task last (Turok's HUD): the 3D frame is a record in RAM */
        v->seed_ok = clean;
    }
    if (!clean) {
        uint32_t best = FF_MAX_REC, r;
        uint32_t shown_lo = st_hdr.vi[1] & 0x00FFFFFFu;
        for (r = 0; r < v->n_rec; r++) {
            uint32_t c = v->rec_cimg[r];
            if (!c) continue;
            if (best == FF_MAX_REC) best = r;
            if ((shown_lo >= c) && (shown_lo < c + 0x100000u)) { best = r; break; }
        }
        if (best < FF_MAX_REC) {
            const vu32 *rec = (const vu32 *)(0xA0000000u | v->rec[best]);
            for (i = 0; i < 16u; i++) v->task[i] = rec[i];
            clean = view_walk_task(&mem, 0);
            v->src_ram = 1u;
        }
    }
    if (!clean) {
        return ST_BAD_STATE;
    }
    v->segtab_off = (v->map.family == FF_FAM_F3D) ? FF_SEGTAB_F3D : FF_SEGTAB_F3DEX2;
    for (i = 0; i < 16u; i++) v->segtab[i] = v->seed_ok ? *(vu32 *)(0xA4000000u + v->segtab_off + 4u * i) : 0u;
    view_find_data(mem.limit);
    if (!v->task[2] || !v->task[4]) {
        return ST_BAD_STATE;
    }
    if (!v->map.n_persp || !v->map.main_cimg) {
        return ST_BAD_STATE;
    }
    /* the segment table the state left, into the data copy the boot will load (the world is
     * disposable: the exit's reload puts the original bytes back) */
    if (v->seed_ok) {
        for (i = 0; i < 16u; i++) *(vu32 *)(0xA0000000u | (v->data_src + v->segtab_off + 4u * i)) = v->segtab[i];
    }
    /* Two colour buffers: the one this list draws (A) and one of our own (S) in memory the
     * frame's task never reads: the walk marked what it reads, here the task's own areas,
     * the images, the hook's home and the low page join, and S goes in the middle of the
     * longest clear run. Each frame is drawn off-screen and shown when complete: drawn
     * onto the buffer the TV scans it flickered (Banjo-Tooie's sky repaints the whole view
     * before the world goes back on top of it). With no room for S, the main colour image
     * (a 4 MiB console with a dense game) A alone serves, as before. The game shows its
     * buffers from some way in (Mario Kart: one line): the offset is where the VI origin
     * (O) lies inside A, whole lines only, a few at most, the same for both. */
    {
        uint32_t w0 = 0, line, size, a = v->map.main_cimg, o = st_hdr.vi[1] & 0x00FFFFFFu, b = a, off = 0;
        const uint32_t *t = v->task;
        for (i = 0; i < v->map.n_cimg; i++) if (v->map.cimg[i].addr == a) w0 = v->map.cimg[i].w0;
        v->width = (w0 & 0xFFFu) + 1u;
        if (v->width != (st_hdr.vi[2] & 0xFFFu)) return ST_BAD_STATE;   /* not the frame on screen (Episode I Racer: a 320-wide list under a 640-wide VI) */
        v->height = v->map.scissor_h ? v->map.scissor_h : ((v->width >= 400u) ? 480u : 240u);
        line = v->width * ((((w0 >> 19) & 3u) == 3u) ? 4u : 2u);
        size = line * v->height;
        for (i = 0; i < v->map.n_cimg; i++) ff_mark(&v->map, v->map.cimg[i].addr, size + 0x2000u);
        for (i = 0; i < v->map.n_zimg; i++) ff_mark(&v->map, v->map.zimg[i], size + 0x2000u);
        ff_mark(&v->map, t[2], t[3]);
        ff_mark(&v->map, t[4], 0x1000u);
        ff_mark(&v->map, v->data_src, 0x1000u);
        ff_mark(&v->map, t[8], t[9]);
        ff_mark(&v->map, t[10], (t[11] > t[10]) ? (t[11] - t[10]) : t[11]);   /* the output buffer: an end or a size */
        ff_mark(&v->map, 0, 0x10000u);
        ff_mark(&v->map, mem.deny_lo, mem.deny_hi - mem.deny_lo);
        v->spare = ff_spare(&v->map, size, mem.limit);
        if (v->spare) b = v->spare;
        if ((o >= a) && (o < a + size)) off = o - a;
        if (!line || (off % line) || (off > 8u * line)) off = 0;
        v->line_off = off;
        v->buf[0] = a;
        v->buf[1] = b;
    }
    /* the loaded state's video mode (the loader's own four registers), the origin per frame */
    *(vu32 *)(VI_BASE + 0x00u) = st_hdr.vi[0];
    *(vu32 *)(VI_BASE + 0x08u) = st_hdr.vi[2];
    *(vu32 *)(VI_BASE + 0x30u) = st_hdr.vi[12];
    DPC_STATUS = (1u << 2);                     /* the RDP unfrozen, whatever the moment left */
    rom_write_set(1u);                          /* the progress word goes to the staged cfg on the cart */
    for (i = 0; i < v->map.n_rect; i++) {
        uint32_t pc = v->map.rect_pc[i] & 0x00FFFFFFu;
        vu32 *w0 = (vu32 *)(0xA0000000u | pc);
        v->rect_w0[i] = *w0;
    }
    for (i = 0; i < v->map.n_fog; i++) v->fog_w1[i] = *(vu32 *)(0xA0000000u | (v->map.fog_pc[i] + 4u));
    for (i = 0; i < v->map.n_orthom; i++) {
        uint32_t j;
        for (j = 0; j < 16u; j++) v->ortho_save[i][j] = *(vu32 *)(0xA0000000u | (v->map.ortho_addr[i] + 4u * j));
    }
    v->prev = 0xFFFFu;                          /* whatever is held at entry does not count as a press */
    v->zoom = FX_ONE;
    v->roll = 0;
    v->r_stick = 0;
    v->msg_frames = 0;
    {
        uint32_t w = 0;                         /* the PC's command word as found: only new commands count
                                                 * (a stale exit from an earlier session ended the panel's view at once) */
        if (pio_read(v->cfg_base + 0x9Cu, &w)) v->cmd_seq = w >> 24;
    }
    for (;;) {
        uint32_t buttons = 0, k, target, exit = 0;
        fx step = FF_STEP;
        if (pad_dma_poll(&buttons)) {
            int32_t sx = (int32_t)(int8_t)pif_stick_x, sy = (int32_t)(int8_t)pif_stick_y;
            uint32_t edge = buttons & ~v->prev;                     /* pressed this frame */
            uint32_t up = v->prev & ~buttons;                       /* released this frame */
            uint32_t r_held = buttons & 0x0010u;
            fx turn = FF_TURN;
            v->prev = buttons;
            if (edge & 0x4000u) exit = 1u;                          /* B */
            if (edge & 0x8000u) {                                   /* A: the lens back (zoom, roll, far plane); with R held only that, else the camera too */
                v->zoom = FX_ONE; v->roll = 0; v->far_x = 0;
                if (r_held) v->r_stick = 1u; else { v->yaw = v->pitch = 0; v->pos[0] = v->pos[1] = v->pos[2] = 0; }
            }
            if (edge & 0x0010u) v->r_stick = 0;
            if ((up & 0x0010u) && !v->r_stick) v->far_x ^= 1u;      /* R tapped: the far plane four times out, and back */
            if ((edge & 0x1000u) && !v->msg_frames) {               /* Start: a screenshot of this view (not while the last one's text shows) */
                v->msg = (view_shot() == ST_OK) ? "SCREENSHOT SAVED" : "SCREENSHOT FAILED";
                v->msg_frames = 45u;
            }
            if (edge & 0x0200u) { v->hud_off ^= 1u; view_apply_layers(); }   /* D-left: the HUD layer */
            if (edge & 0x0100u) { v->fog_off ^= 1u; view_apply_layers(); }   /* D-right: the fog */
            if (buttons & 0x0020u) { step *= 4; turn *= 4; }        /* L: fast */
            if (buttons & 0x2000u) { step /= 4; turn /= 4; }        /* Z: slow, for lining up a shot */
            if (sx > 8 || sx < -8) {
                if (r_held) { v->r_stick = 1u; v->roll -= sx * turn / 80; }   /* R held: the stick sideways rolls (right tilts clockwise) */
                else v->yaw -= sx * turn / 80;                      /* right on the stick looks right */
            }
            if (sy > 8 || sy < -8) {
                if (r_held) {                                       /* R held: the stick forward zooms in, back out */
                    v->r_stick = 1u;
                    v->zoom += (fx)(((int64_t)sy * v->zoom) / 4000);
                    if (v->zoom < FX_ONE / 4) v->zoom = FX_ONE / 4;
                    if (v->zoom > FX_ONE * 4) v->zoom = FX_ONE * 4;
                } else {
                    v->pitch += sy * turn / 80;
                }
            }
            if (buttons & 0x0008u) view_move(step, 0, 0);            /* C-up */
            if (buttons & 0x0004u) view_move(-step, 0, 0);           /* C-down */
            if (buttons & 0x0001u) view_move(0, step, 0);            /* C-right */
            if (buttons & 0x0002u) view_move(0, -step, 0);           /* C-left */
            if (buttons & 0x0800u) view_move(0, 0, step);            /* D-up */
            if (buttons & 0x0400u) view_move(0, 0, -step);           /* D-down */
        }
        if (view_pc_command()) exit = 1u;
        if (exit) {
            uint32_t t1 = c0_count();                               /* the press over before the game polls */
            while ((c0_count() - t1) < (46875u * 1000u)) {
                if (!pad_dma_poll(&buttons) || !(buttons & 0x4000u)) break;
                vi_frz_service();
            }
            break;
        }
        if (v->pitch > FF_PITCH_MAX) v->pitch = FF_PITCH_MAX;
        if (v->pitch < -FF_PITCH_MAX) v->pitch = -FF_PITCH_MAX;
        ff_delta(v->yaw, v->pitch, v->roll, v->pos, v->delta);
        for (k = 0; k < v->map.n_persp; k++) {
            struct ff_persp *p = &v->map.persp[k];
            vu32 *dst = (vu32 *)(0xA0000000u | p->addr);
            const uint32_t *src = (const uint32_t *)(const void *)v->raw;
            ff_rebuild(p, v->delta, v->far_x ? p->far_i * 4 : 0, v->zoom, v->out);
            ff_mtx_encode(v->out, v->raw);
            for (i = 0; i < 16u; i++) dst[i] = src[i];
        }
        target = v->buf[v->cur];
        for (k = 0; k < v->map.n_cimg; k++) {
            struct ff_cimg *c = &v->map.cimg[k];
            if (c->addr != v->map.main_cimg) continue;
            {
                vu32 *w1 = (vu32 *)(0xA0000000u | (c->pc + 4u));
                *w1 = (*w1 & 0xFF000000u) | ((target - c->seg_base) & 0x00FFFFFFu);
            }
        }
        if (!view_zclear() || !view_task_run()) {
            {
                uint32_t w = 0;
                pio_read(v->cfg_base + 0x94u, &w);
                pio_write(v->cfg_base + 0x94u, w | 0x80000000u);   /* over, the stage kept */
            }
            return ST_TIMEOUT;
        }
        if (v->msg_frames) {                    /* the screenshot's message, as the game's, into this frame */
            struct ov_screen s;
            ov_screen_read(&s);
            s.fb = 0xA0000000u | target;
            if (ov_screen_ok(&s)) {
                uint32_t sc = s.scale, x = 22u * sc, y = s.height - 36u * sc;
                ov_text(&s, x + sc, y + sc, v->msg, ov_rgb(&s, 8, 8, 24));
                ov_text(&s, x, y, v->msg, ov_rgb(&s, 255, 220, 60));
            }
            v->msg_frames--;
        }
        vi_frz_base0 = target + v->line_off;    /* an interlaced freeze aims the fields at it */
        vi_hold_field();                        /* the field start */
        if (!vi_frz_active) {
            *(vu32 *)(VI_BASE + 0x04u) = target + v->line_off;
        }
        if (v->buf[1] != v->buf[0]) v->cur ^= 1u;
        v->frames++;
        if ((v->frames & 7u) == 0) view_pc_report(0, 2u, 0);
    }
    view_pc_report(1u, 3u, 0);
    thumb_ready = 0;                            /* the panel's thumbnail buffer was ours */
    return ST_OK;
}
