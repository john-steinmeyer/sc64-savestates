/* The frozen frame's generic core: a frame's display list walked for its perspective loads
 * and colour images, and the camera math that rewrites those matrices. No hardware and no
 * libc in here: memory comes through ff_mem, so the same code runs on the console (KSEG0)
 * and in the host test (a state file's image). All arithmetic is integer: matrices are the
 * RSP's own 16.16 fixed point, intermediates 64-bit, so the blob needs no float support.
 * The PC-side analyzer has the reference walk and camera; ff_host.c compiles this file on a PC
 * and checks the two agree. */
#ifndef FF_CORE_H
#define FF_CORE_H

#include <stdint.h>

typedef int32_t fx;              /* 16.16 fixed point */
#define FX_ONE 65536

#define FF_MAX_DISTINCT 16u      /* distinct projection matrices a frame may load (the library's most is 7) */
#define FF_MAX_CIMG     16u      /* SETCIMG commands in a frame */
#define FF_MAX_RECT     48u      /* 2D rectangles kept for the HUD layer */
#define FF_MAX_FOG      8u
#define FF_MAX_ORTHO    8u
#define FF_MAX_DEPTH    18u
#define FF_MAX_CMDS     200000u
#define FF_MAX_UNKNOWN  8u

#define FF_FAM_F3D      0u
#define FF_FAM_F3DEX2   1u
#define FF_FAM_F3DEX2CBFD 2u    /* Rare's dialect of F3DEX2 (Conker's Bad Fur Day): 0x10..0x1F draw four
                                 * triangles each, 0xDD switches the lighting mode (no microcode load);
                                 * everything the walk reads is F3DEX2's. FF_CBFD builds only. */
#if defined(FF_HOST) || (defined(SC64SS_CARD_DIRECT) && SC64SS_CARD_DIRECT)
#define FF_CBFD 1
#else
#define FF_CBFD 0                /* the plain blob is full, and no 64 MiB game runs on it */
#endif

struct ff_mem {                  /* the frame's RAM: base = a pointer to physical 0, limit = bytes */
    const uint8_t *base;
    uint32_t limit;
    uint32_t deny_lo, deny_hi;   /* a range the frame must not touch (the hook's home); 0,0 = none */
};

struct ff_cimg {
    uint32_t pc;                 /* the SETCIMG command's physical address */
    uint32_t addr;               /* the colour image it names */
    uint32_t w0;
    uint32_t seg_base;           /* the segment base the address resolved through (to re-point it) */
};

struct ff_persp {                /* one distinct perspective projection matrix */
    uint32_t addr;               /* physical address of the 64-byte Mtx */
    uint32_t loads;              /* how many load commands name it */
    /* the factors (the analyzer's camera): M = V * P_obl * s, all 16.16 */
    fx s, a, b, c, d, e, f;
    fx rot[3][3];                /* rows of the view's rotation */
    fx t[3];
    int32_t near_i, far_i;       /* the planes as whole units (reporting) */
    uint8_t ok;                  /* factored (a rigid view and a perspective) */
    uint8_t has_view;
};

struct ff_map {
    uint32_t family;
    uint32_t data_ptr;
    uint32_t commands, max_depth, anomalies, first_anomaly_pc, first_anomaly_op, ended;
    uint32_t seg[16];
    uint32_t n_persp_loads;      /* perspective LOAD commands seen */
    uint32_t n_persp;            /* distinct matrices */
    struct ff_persp persp[FF_MAX_DISTINCT];
    uint32_t n_cimg;
    struct ff_cimg cimg[FF_MAX_CIMG];
    uint32_t main_cimg;          /* the colour image most SETCIMGs name (0 when none), the depth image excluded */
    uint32_t n_zimg;
    uint32_t zimg[4];            /* SETZIMG addresses */
    uint32_t scissor_h;          /* the tallest scissor bottom seen: the buffer's used height */
    uint32_t n_ortho;            /* ortho/identity projection loads (2D passes) */
    uint32_t n_proj_other;       /* projection loads that factor as nothing we know */
    /* for the layers: the 2D rectangles (bit 31 of the word set = a full-screen fill, a clear
     * to keep), the fog words, the distinct ortho projection matrices */
    uint32_t n_rect;
    uint32_t rect_pc[FF_MAX_RECT];
    uint32_t rect_cimg[FF_MAX_RECT];   /* the colour image in effect at each rectangle */
    uint32_t rect_dim[FF_MAX_RECT];    /* its width << 16 | height, pixels */
    uint32_t n_fog;
    uint32_t fog_pc[FF_MAX_FOG];
    uint32_t n_orthom;
    uint32_t ortho_addr[FF_MAX_ORTHO];
    uint32_t used[8];            /* bit per 32 KiB block of RAM the frame's task reads (8 MiB) */
};
#define FF_BLOCK 32768u

/* mark [addr, addr+len) as the frame's */
void ff_mark(struct ff_map *m, uint32_t addr, uint32_t len);
/* a block-aligned address for a buffer of size bytes inside the longest clear run below
 * limit (the middle of it), or 0 when no run is long enough */
uint32_t ff_spare(const struct ff_map *m, uint32_t size, uint32_t limit);

/* the segment table's DMEM offset per family (found by the analyzer over the library) */
#define FF_SEGTAB_F3D    0x160u
#define FF_SEGTAB_F3DEX2 0x0F8u

/* the family a microcode's version string names: 0/1, or -1 for none */
int ff_family_of_string(const uint8_t *ucode_data, uint32_t len);

#if FF_CBFD
/* 1 when the microcode text at `text` (physical) is a known one of Rare's dialect: no version
 * string names it, the CRC32 of its first 3 KiB does (the analyzer's KNOWN_TEXT table) */
int ff_cbfd_text(const struct ff_mem *mem, uint32_t text);
#endif

/* walk the list at data_ptr with the family's encoding; seg = 16 initial segment bases
 * (physical, may be NULL); fills *m; returns 1 when the walk ended clean */
int ff_walk(const struct ff_mem *mem, uint32_t data_ptr, uint32_t family, const uint32_t *seg, struct ff_map *m);

/* a 64-byte Mtx to 16.16 (row major, as the RSP multiplies) and back */
void ff_mtx_decode(const uint8_t *raw, fx out[4][4]);
void ff_mtx_encode(const fx in[4][4], uint8_t *raw);
/* factor a decoded projection; returns 1 when it is a perspective */
int ff_factor(const fx m[4][4], struct ff_persp *p);
/* rebuild the matrix from its factors with a camera delta (or NULL) and a far plane in whole
 * units (0 = the game's own) */
void ff_rebuild(const struct ff_persp *p, const fx delta[4][4], int32_t far_units, fx zoom, fx out[4][4]);
/* the delta for a camera at pos (eye0 coordinates, 16.16) turned by yaw about y then pitch
 * about its x (angles in 16.16 radians) */
void ff_delta(fx yaw, fx pitch, fx roll, const fx pos[3], fx out[4][4]);

fx ff_mul(fx a, fx b);
fx ff_div(fx a, fx b);
fx ff_sin(fx x);
fx ff_cos(fx x);
uint32_t ff_isqrt64(uint64_t v);

#endif
