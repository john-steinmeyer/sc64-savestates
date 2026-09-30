/* The frozen frame's generic core (see ff_core.h). Freestanding, integer only. */
#include "ff_core.h"
#include "ff_ucode.h"

#if defined(__mips__)
static inline uint32_t rd32(const struct ff_mem *mem, uint32_t phys) {
    return *(const uint32_t *)(const void *)(mem->base + phys);
}
#else
static inline uint32_t rd32(const struct ff_mem *mem, uint32_t phys) {
    const uint8_t *p = mem->base + phys;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
#endif

static void zero(void *p, uint32_t n) {
    uint8_t *b = (uint8_t *)p;
    while (n--) *b++ = 0;
}

/* ---- 16.16 arithmetic ------------------------------------------------------------------ */

fx ff_mul(fx a, fx b) {
    return (fx)(((int64_t)a * (int64_t)b + 32768) >> 16);
}

fx ff_div(fx a, fx b) {
    if (b == 0) return (a < 0) ? (fx)0x80000000 : 0x7FFFFFFF;
    return (fx)(((int64_t)a << 16) / (int64_t)b);
}

static int64_t div64(int64_t a, int64_t b) {
    if (b == 0) return (a < 0) ? -0x7FFFFFFFFFFFFFFFLL : 0x7FFFFFFFFFFFFFFFLL;
    return a / b;
}

static fx clampfx(int64_t v) {
    if (v > 0x7FFFFFFFLL) return 0x7FFFFFFF;
    if (v < -0x80000000LL) return (fx)0x80000000;
    return (fx)v;
}

uint32_t ff_isqrt64(uint64_t v) {
    uint64_t r = 0, bit = (uint64_t)1 << 62;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return (uint32_t)r;
}

/* the 16.16 length of a 3-vector: squares are 32.32, the root of a 32.32 is 16.16 */
static fx norm3(const fx v[3]) {
    uint64_t s = (uint64_t)((int64_t)v[0] * v[0]) + (uint64_t)((int64_t)v[1] * v[1]) + (uint64_t)((int64_t)v[2] * v[2]);
    return (fx)ff_isqrt64(s);
}

static fx dot3(const fx a[3], const fx b[3]) {
    int64_t s = (int64_t)a[0] * b[0] + (int64_t)a[1] * b[1] + (int64_t)a[2] * b[2];
    return (fx)((s + 32768) >> 16);
}

static fx absfx(fx x) { return x < 0 ? -x : x; }

#define FX_PI     205887        /* pi in 16.16 */
#define FX_2PI    411775
#define FX_HALFPI 102944

static fx sin_reduced(fx x) {                   /* |x| <= pi/2: x - x^3/6 + x^5/120 - x^7/5040 (an error of 1e-4) */
    fx x2 = ff_mul(x, x);
    fx term = x, sum = x;
    term = ff_mul(term, x2) / 6;   sum -= term;
    term = ff_mul(term, x2) / 20;  sum += term;
    term = ff_mul(term, x2) / 42;  sum -= term;
    return sum;
}

fx ff_sin(fx x) {
    while (x > FX_PI) x -= FX_2PI;
    while (x < -FX_PI) x += FX_2PI;
    if (x > FX_HALFPI) x = FX_PI - x;
    else if (x < -FX_HALFPI) x = -FX_PI - x;
    return sin_reduced(x);
}

fx ff_cos(fx x) { return ff_sin(x + FX_HALFPI); }

/* ---- the microcode's family from its version string ---------------------------------- */

#if defined(FF_HOST)
/* the console tries both encodings and lets the walk decide; the name only saves a try */
int ff_family_of_string(const uint8_t *d, uint32_t len) {
    uint32_t i;
    if (len > 0x800u) len = 0x800u;
    for (i = 0; i + 20u <= len; i++) {
        const uint8_t *n;
        uint32_t j;
        if (d[i] != 'R' || d[i + 1] != 'S' || d[i + 2] != 'P' || d[i + 3] != ' ') continue;
        if (d[i + 4] == 'S') return FF_FAM_F3D;                       /* "RSP SW Version": Fast3D */
        if (d[i + 4] != 'G') return -1;
        n = d + i + 14;                                               /* "RSP Gfx ucode NAME ... d.dd" */
        if (n[0] == 'S') return -1;                                   /* S2DEX: 2D */
        for (j = 0; (j < 8u) && (n[j] > ' '); j++) {                  /* a 2 or a Z in the name: the F3DEX2 line */
            if (n[j] == '2' || n[j] == 'Z') return FF_FAM_F3DEX2;
        }
        for (j = 1; (i + 14u + j + 3u < len) && (j < 48u); j++) {     /* else the version: 2.xx is that line too */
            if (n[j] == '.' && n[j - 1] >= '0' && n[j - 1] <= '9' && n[j + 1] >= '0' && n[j + 1] <= '9' && n[j + 2] >= '0' && n[j + 2] <= '9')
                return (n[j - 1] >= '2') ? FF_FAM_F3DEX2 : FF_FAM_F3D;
        }
        return FF_FAM_F3D;
    }
    return -1;
}
#endif

/* ---- matrices ------------------------------------------------------------------------- */

void ff_mtx_decode(const uint8_t *raw, fx out[4][4]) {
    uint32_t i;
    for (i = 0; i < 16u; i++) {
        uint32_t hi = ((uint32_t)raw[2u * i] << 8) | raw[2u * i + 1u];
        uint32_t lo = ((uint32_t)raw[32u + 2u * i] << 8) | raw[32u + 2u * i + 1u];
        out[i >> 2][i & 3u] = (fx)((hi << 16) | lo);
    }
}

void ff_mtx_encode(const fx in[4][4], uint8_t *raw) {
    uint32_t i;
    for (i = 0; i < 16u; i++) {
        uint32_t u = (uint32_t)in[i >> 2][i & 3u];
        raw[2u * i] = (uint8_t)(u >> 24);
        raw[2u * i + 1u] = (uint8_t)(u >> 16);
        raw[32u + 2u * i] = (uint8_t)(u >> 8);
        raw[32u + 2u * i + 1u] = (uint8_t)u;
    }
}

static void mul4(const fx a[4][4], const fx b[4][4], fx out[4][4]) {
    uint32_t i, j, k;
    for (i = 0; i < 4u; i++)
        for (j = 0; j < 4u; j++) {
            int64_t s = 0;
            for (k = 0; k < 4u; k++) s += (int64_t)a[i][k] * (int64_t)b[k][j];
            out[i][j] = clampfx((s + 32768) >> 16);
        }
}

int ff_factor(const fx m[4][4], struct ff_persp *p) {
    fx n3[3], r2[3], c0[3], c1[3], r0[3], r1[3];
    fx s, e_s, f_s, a_s, b_s, c_s, t2, err, x;
    uint32_t i;
    p->ok = 0;
    for (i = 0; i < 3u; i++) n3[i] = m[i][3];
    s = norm3(n3);
    if (s < 131) return 0;                                          /* 0.002 */
    for (i = 0; i < 3u; i++) r2[i] = ff_div(-n3[i], s);
    {
        fx col0[3] = {m[0][0], m[1][0], m[2][0]}, col1[3] = {m[0][1], m[1][1], m[2][1]}, col2[3] = {m[0][2], m[1][2], m[2][2]};
        e_s = dot3(col0, r2);
        f_s = dot3(col1, r2);
        for (i = 0; i < 3u; i++) {
            c0[i] = col0[i] - ff_mul(e_s, r2[i]);
            c1[i] = col1[i] - ff_mul(f_s, r2[i]);
        }
        c_s = dot3(col2, r2);
    }
    a_s = norm3(c0);
    b_s = norm3(c1);
    if (a_s < 131 || b_s < 131) return 0;
    for (i = 0; i < 3u; i++) {
        r0[i] = ff_div(c0[i], a_s);
        r1[i] = ff_div(c1[i], b_s);
    }
    err = absfx(dot3(r0, r1));
    x = absfx(dot3(r0, r2));
    if (x > err) err = x;
    x = absfx(dot3(r1, r2));
    if (x > err) err = x;
    if (err > 3277) return 0;                                       /* 0.05 */
    t2 = ff_div(-m[3][3], s);
    p->s = s;
    p->a = ff_div(a_s, s);
    p->b = ff_div(b_s, s);
    p->c = ff_div(c_s, s);
    p->e = ff_div(e_s, s);
    p->f = ff_div(f_s, s);
    p->d = ff_div(m[3][2] - ff_mul(t2, c_s), s);
    p->t[0] = ff_div(m[3][0] - ff_mul(t2, e_s), a_s);
    p->t[1] = ff_div(m[3][1] - ff_mul(t2, f_s), b_s);
    p->t[2] = t2;
    for (i = 0; i < 3u; i++) {
        p->rot[i][0] = r0[i];
        p->rot[i][1] = r1[i];
        p->rot[i][2] = r2[i];
    }
    /* near = d / (c - 1), far = d / (c + 1), as whole units (64-bit: far can pass 32767) */
    p->near_i = (p->c - FX_ONE) ? (int32_t)(div64((int64_t)p->d, (int64_t)(p->c - FX_ONE))) : 0;
    p->far_i = (p->c + FX_ONE) ? (int32_t)(div64((int64_t)p->d, (int64_t)(p->c + FX_ONE))) : 0;
    p->has_view = 0;
#if defined(FF_HOST)
    for (i = 0; i < 3u; i++) {                                      /* reporting only: the console never asks */
        uint32_t j;
        for (j = 0; j < 3u; j++)
            if (absfx(p->rot[i][j] - ((i == j) ? FX_ONE : 0)) > 655) p->has_view = 1;   /* 0.01 */
        if (absfx(p->t[i]) > 655) p->has_view = 1;
    }
#endif
    p->ok = 1;
    return 1;
}

void ff_rebuild(const struct ff_persp *p, const fx delta[4][4], int32_t far_units, fx zoom, fx out[4][4]) {
    fx v[4][4], pr[4][4], tmp[4][4];
    fx c = p->c, d = p->d, sz = ff_mul(p->s, zoom);   /* the focal and oblique terms scaled: the field of view */
    uint32_t i, j;
    if (far_units > 0 && p->near_i > 0) {
        /* c = (n + f) / (n - f), d = 2 n f / (n - f), n from the factors (16.16), f whole units */
        int64_t n = ((int64_t)p->d << 16) / (int64_t)(p->c - FX_ONE);      /* 16.16 */
        int64_t f = (int64_t)far_units << 16;
        c = clampfx(div64((n + f) << 16, n - f));
        d = clampfx(div64(((2 * n * f) >> 16) << 16, n - f));
    }
    zero(v, sizeof v);
    zero(pr, sizeof pr);
    for (i = 0; i < 3u; i++)
        for (j = 0; j < 3u; j++) v[i][j] = p->rot[i][j];
    for (j = 0; j < 3u; j++) v[3][j] = p->t[j];
    v[3][3] = FX_ONE;
    pr[0][0] = ff_mul(p->a, sz);
    pr[1][1] = ff_mul(p->b, sz);
    pr[2][0] = ff_mul(p->e, sz);
    pr[2][1] = ff_mul(p->f, sz);
    pr[2][2] = ff_mul(c, p->s);
    pr[2][3] = -p->s;
    pr[3][2] = ff_mul(d, p->s);
    if (delta) {
        mul4(v, delta, tmp);
        mul4(tmp, pr, out);
    } else {
        mul4(v, pr, out);
    }
}

void ff_delta(fx yaw, fx pitch, fx roll, const fx pos[3], fx out[4][4]) {
    fx cy = ff_cos(yaw), sy = ff_sin(yaw), cp = ff_cos(pitch), sp = ff_sin(pitch), cr = ff_cos(roll), sr = ff_sin(roll);
    fx ry[3][3] = {{cy, 0, -sy}, {0, FX_ONE, 0}, {sy, 0, cy}};
    fx rx[3][3] = {{FX_ONE, 0, 0}, {0, cp, sp}, {0, -sp, cp}};
    fx o[3][3];
    uint32_t i, j, k;
    for (i = 0; i < 3u; i++)
        for (j = 0; j < 3u; j++) {
            int64_t s = 0;
            for (k = 0; k < 3u; k++) s += (int64_t)rx[i][k] * ry[k][j];
            o[i][j] = (fx)((s + 32768) >> 16);
        }
    /* the roll, applied first: the look then works in screen terms whatever the tilt (up on
     * the stick is up on the screen) */
    for (i = 0; i < 3u; i++) {
        fx a = o[i][0], b = o[i][1];
        o[i][0] = ff_mul(cr, a) - ff_mul(sr, b);
        o[i][1] = ff_mul(sr, a) + ff_mul(cr, b);
    }
    zero(out, 16u * sizeof(fx));
    for (i = 0; i < 3u; i++)
        for (j = 0; j < 3u; j++) out[i][j] = o[j][i];                 /* O transposed */
    for (j = 0; j < 3u; j++) {                                        /* -pos * O^T */
        int64_t s = 0;
        for (k = 0; k < 3u; k++) s += (int64_t)pos[k] * o[j][k];
        out[3][j] = clampfx(-((s + 32768) >> 16));
    }
    out[3][3] = FX_ONE;
}

/* ---- the walk ------------------------------------------------------------------------- */

void ff_mark(struct ff_map *m, uint32_t addr, uint32_t len) {
    uint32_t b = addr / FF_BLOCK, e = (addr + len - 1u) / FF_BLOCK;
    if (!len) return;
    for (; b <= e && b < 256u; b++) m->used[b >> 5] |= 1u << (b & 31u);
}

uint32_t ff_spare(const struct ff_map *m, uint32_t size, uint32_t limit) {
    uint32_t need = (size + FF_BLOCK - 1u) / FF_BLOCK, nblk = limit / FF_BLOCK;
    uint32_t b, run = 0, best = 0, best_end = 0;
    if (nblk > 256u) nblk = 256u;
    for (b = 0; b < nblk; b++) {
        if (m->used[b >> 5] & (1u << (b & 31u))) { run = 0; continue; }
        run++;
        if (run > best) { best = run; best_end = b + 1u; }
    }
    if (best < need) return 0;
    return (best_end - best + (best - need) / 2u) * FF_BLOCK;
}

static int valid_op(uint32_t family, uint32_t op) {
    if (op == 0xC0u || op >= 0xE4u) return 1;                          /* the RDP's own commands */
    if (family == FF_FAM_F3D)
        return (op == 0x00u || op == 0x01u || op == 0x03u || op == 0x04u || op == 0x06u || op == 0x09u || (op >= 0xAFu && op <= 0xBFu));
#if FF_CBFD
    if (family == FF_FAM_F3DEX2CBFD && op >= 0x10u && op <= 0x1Fu) return 1;   /* four triangles each */
#endif
    return (op <= 0x08u || (op >= 0xD3u && op <= 0xE3u) || op == 0xF1u);
}

#if FF_CBFD
static const uint32_t ff_cbfd_crc[] = {
    0xE1E93B36u,          /* Conker's Bad Fur Day (USA) */
};

int ff_cbfd_text(const struct ff_mem *mem, uint32_t text) {
    uint32_t crc = 0xFFFFFFFFu, a, i, k;
    text &= 0x1FFFFFFFu;
    if ((text & 7u) || (text < 0x400u) || (text + 3072u > mem->limit)) return 0;
    for (a = text; a < text + 3072u; a += 4u) {
        uint32_t w = rd32(mem, a);
        for (i = 0; i < 4u; i++) {
            crc ^= (w >> (24u - 8u * i)) & 0xFFu;
            for (k = 0; k < 8u; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    crc = ~crc;
    for (i = 0; i < sizeof ff_cbfd_crc / sizeof ff_cbfd_crc[0]; i++) if (crc == ff_cbfd_crc[i]) return 1;
    return 0;
}
#endif

static void anomaly(struct ff_map *m, uint32_t pc, uint32_t op) {
    if (m->anomalies == 0) {
        m->first_anomaly_pc = pc;
        m->first_anomaly_op = op;
    }
    m->anomalies++;
}

int ff_walk(const struct ff_mem *mem, uint32_t data_ptr, uint32_t family, const uint32_t *seg0, struct ff_map *m) {
    uint32_t stack[FF_MAX_DEPTH];
    uint32_t depth = 0, unknown = 0, pc = data_ptr, i;
    uint32_t op_dl, op_enddl, op_mtx, op_mw, op_lu, cur_cimg = 0;
#if FF_CBFD
    uint32_t half1 = 0;                     /* the last RDPHALF_1's word: the data a microcode switch names */
#endif
    zero(m, sizeof *m);
    m->family = family;
    m->data_ptr = data_ptr;
    if (seg0) for (i = 0; i < 16u; i++) m->seg[i] = seg0[i] & 0x00FFFFFFu;
    if (family == FF_FAM_F3D) {
        op_dl = FF_F3D_DL; op_enddl = FF_F3D_ENDDL; op_mtx = FF_F3D_MTX; op_mw = FF_F3D_MOVEWORD; op_lu = FF_F3D_LOAD_UCODE;
    } else {
        op_dl = FF_F3DEX2_DL; op_enddl = FF_F3DEX2_ENDDL; op_mtx = FF_F3DEX2_MTX; op_mw = FF_F3DEX2_MOVEWORD; op_lu = FF_F3DEX2_LOAD_UCODE;
#if FF_CBFD
        if (family == FF_FAM_F3DEX2CBFD) op_lu = 0x100u;   /* 0xDD switches the lighting there: an ordinary command */
#endif
    }
#define RESOLVE(a) ((m->seg[((a) >> 24) & 0xFu] + ((a) & 0x00FFFFFFu)) & 0x00FFFFFFu)
#define DENIED(a) ((mem->deny_hi > mem->deny_lo) && ((a) >= mem->deny_lo) && ((a) < mem->deny_hi))
    for (;;) {
        uint32_t w0, w1, op, ra;
        if (m->commands >= FF_MAX_CMDS) { anomaly(m, pc, 0x100u); break; }
        if ((pc & 7u) || pc + 8u > mem->limit) { anomaly(m, pc, 0x101u); break; }
        if (DENIED(pc)) { anomaly(m, pc, 0x108u); break; }
        w0 = rd32(mem, pc);
        w1 = rd32(mem, pc + 4u);
        op = w0 >> 24;
        m->commands++;
        ra = RESOLVE(w1);
        ff_mark(m, pc, 8u);
        /* what the command reads: a matrix, vertices, moved memory, a texture image (its
         * extent unknown here: a generous 256 KiB), a sprite base, a DMA */
        if (op == op_mtx) ff_mark(m, ra, 64u);
        else if (op == ((family == FF_FAM_F3D) ? 0x04u : 0x01u)) ff_mark(m, ra, 1024u);
        else if (op == ((family == FF_FAM_F3D) ? 0x03u : 0xDCu)) ff_mark(m, ra, 256u);
        else if (op == 0xFDu) ff_mark(m, ra, 0x40000u);
        else if ((family == FF_FAM_F3D) ? (op == 0x09u) : (op == 0xD6u)) ff_mark(m, ra, 0x2000u);
#if FF_CBFD
        if (family == FF_FAM_F3DEX2CBFD) {
            /* the switch to the frame's second microcode (Conker: F3DEXBG to F3DEX at the 178th
             * command): the RSP reads its code and overlays and the data the RDPHALF_1 before
             * it named; the spare buffer must not land there */
            if (op == FF_F3DEX2_RDPHALF_1) half1 = w1;
            if (op == FF_F3DEX2_LOAD_UCODE) {
                ff_mark(m, w1 & 0x00FFFFFFu, 0x1800u);
                ff_mark(m, half1 & 0x00FFFFFFu, (w0 & 0xFFFFu) + 1u);
            }
        }
#endif
        if (op == op_dl) {
            uint32_t target = ra;
            uint32_t branch = (w0 >> 16) & 0xFFu;
            if ((target & 7u) || target + 8u > mem->limit) {
                anomaly(m, pc, 0x102u);
                if (branch) break;
                pc += 8u;
                continue;
            }
            if (!branch) {
                if (depth >= FF_MAX_DEPTH) { anomaly(m, pc, 0x103u); break; }
                stack[depth++] = pc + 8u;
                if (depth > m->max_depth) m->max_depth = depth;
            }
            pc = target;
            continue;
        }
        if (op == op_enddl) {
            if (depth) { pc = stack[--depth]; continue; }
            m->ended = 1;
            break;
        }
        if (op == op_mtx) {
            uint32_t proj, load;
            if (family == FF_FAM_F3D) { proj = (w0 >> 16) & 1u; load = (w0 >> 17) & 1u; }
            else { proj = (w0 >> 2) & 1u; load = (w0 >> 1) & 1u; }
            if (proj && load) {
                uint32_t addr = ra;
                if ((addr & 7u) || addr + 64u > mem->limit) {
                    anomaly(m, pc, 0x104u);
                } else if (DENIED(addr)) {
                    anomaly(m, pc, 0x108u);
                } else {
                    uint32_t k;
                    for (k = 0; k < m->n_persp; k++) if (m->persp[k].addr == addr) break;
                    if (k < m->n_persp) {
                        m->persp[k].loads++;
                        m->n_persp_loads++;
                    } else {
                        fx f[4][4];
                        struct ff_persp p;
                        zero(&p, sizeof p);
                        ff_mtx_decode(mem->base + addr, f);
                        if (ff_factor(f, &p)) {
                            m->n_persp_loads++;
                            if (m->n_persp < FF_MAX_DISTINCT) {
                                uint32_t *dst = (uint32_t *)(void *)&m->persp[m->n_persp];
                                const uint32_t *src = (const uint32_t *)(const void *)&p;
                                uint32_t w;
                                p.addr = addr;
                                p.loads = 1u;
                                for (w = 0; w < sizeof p / 4u; w++) dst[w] = src[w];   /* (no memcpy in the blob) */
                                m->n_persp++;
                            } else {
                                anomaly(m, pc, 0x105u);
                            }
                        } else if (absfx(f[2][3]) < 131 && f[3][3] > 131) {
                            uint32_t j;
                            m->n_ortho++;
                            for (j = 0; j < m->n_orthom; j++) if (m->ortho_addr[j] == addr) break;
                            if (j == m->n_orthom && m->n_orthom < FF_MAX_ORTHO) m->ortho_addr[m->n_orthom++] = addr;
                        } else {
                            m->n_proj_other++;
                        }
                    }
                }
            }
            pc += 8u;
            continue;
        }
        if (op == op_mw) {
            uint32_t idx, off;
            if (family == FF_FAM_F3D) { idx = w0 & 0xFFu; off = (w0 >> 8) & 0xFFFFu; }
            else { idx = (w0 >> 16) & 0xFFu; off = w0 & 0xFFFFu; }
            if (idx == FF_MW_SEGMENT && (off >> 2) < 16u) m->seg[off >> 2] = w1 & 0x00FFFFFFu;
            if (idx == FF_MW_FOG && m->n_fog < FF_MAX_FOG) m->fog_pc[m->n_fog++] = pc;
            pc += 8u;
            continue;
        }
        if (op == op_lu) { anomaly(m, pc, 0x106u); break; }           /* a microcode switch mid-list: not walked */
        if (op == FF_RDP_SETCIMG) {
            uint32_t addr = ra;
            /* (a colour image inside the denied range is no anomaly: the viewer draws the frame
             * into spare buffers of its own then, ff_view.c; Pokemon Stadium 2's battle scenes draw
             * into a buffer under the hook's home. A depth image there still is: nothing stands
             * in for it.) */
            cur_cimg = addr;
            if (m->n_cimg < FF_MAX_CIMG) {
                m->cimg[m->n_cimg].pc = pc;
                m->cimg[m->n_cimg].addr = addr;
                m->cimg[m->n_cimg].w0 = w0;
                m->cimg[m->n_cimg].seg_base = m->seg[(w1 >> 24) & 0xFu];
                m->n_cimg++;
            }
            pc += 8u;
            continue;
        }
        if (op == FF_RDP_SETZIMG) {
            uint32_t addr = ra;
            uint32_t k;
            if (DENIED(addr)) anomaly(m, pc, 0x108u);
            for (k = 0; k < m->n_zimg; k++) if (m->zimg[k] == addr) break;
            if (k == m->n_zimg && m->n_zimg < 4u) m->zimg[m->n_zimg++] = addr;
            pc += 8u;
            continue;
        }
        if (op == FF_RDP_SETSCISSOR) {
            uint32_t lry = (w1 & 0xFFFu) >> 2;
            if (lry > m->scissor_h) m->scissor_h = lry;
            pc += 8u;
            continue;
        }
        if (op == FF_RDP_TEXRECT || op == FF_RDP_TEXRECTFLIP || op == FF_RDP_FILLRECT) {
            if (m->n_rect < FF_MAX_RECT) {
                uint32_t full = 0;
                uint32_t lrx = (w0 >> 12) & 0xFFFu, lry = w0 & 0xFFFu, ulx = (w1 >> 12) & 0xFFFu, uly = w1 & 0xFFFu;
                if (op == FF_RDP_FILLRECT) full = 0x80000000u;                /* a fill: a clear when it is large (the viewer decides) */
                m->rect_dim[m->n_rect] = (((lrx - ulx) >> 2) << 16) | ((lry - uly) >> 2);
                m->rect_cimg[m->n_rect] = cur_cimg;
                m->rect_pc[m->n_rect++] = pc | full;
            }
            pc += 8u;
            continue;
        }
        if (valid_op(family, op)) { pc += 8u; continue; }
        unknown++;
        anomaly(m, pc, op);
        if (unknown >= FF_MAX_UNKNOWN) break;
        pc += 8u;
    }
#undef RESOLVE
#undef DENIED
    if (!m->ended && m->anomalies == 0) anomaly(m, pc, 0x107u);
    {                                                                  /* the colour image named most often, the
                                                                        * depth image (cleared as a colour image) excluded */
        uint32_t best = 0, k, j;
        for (k = 0; k < m->n_cimg; k++) {
            uint32_t n = 0, isz = 0;
            for (j = 0; j < m->n_zimg; j++) if (m->zimg[j] == m->cimg[k].addr) isz = 1u;
            if (isz) continue;
            for (j = 0; j < m->n_cimg; j++) if (m->cimg[j].addr == m->cimg[k].addr) n++;
            if (n >= best) { best = n; m->main_cimg = m->cimg[k].addr; }
        }
    }
    return (m->ended && m->anomalies == 0) ? 1 : 0;
}
