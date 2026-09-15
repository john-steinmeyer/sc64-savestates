/* SC64SS save-state hook: runs in exception context on a real N64.
 *
 * Reached from entry.S on EVERY exception of the game, after the Datel engine
 * (installed by the menu's boot patcher) has applied its codes. Hard rules:
 *   - EXL=1: the game is frozen while we run; nothing else can start a PI op.
 *   - Never fault: only dereference validated KSEG0/KSEG1 RDRAM and known
 *     KSEG1 I/O; no FPU, no div/mod, no libc, no stack beyond our own.
 *   - Touch the PI bus only when PI_STATUS is idle, with timeouts; if the
 *     cart stalls, bail out for this tick rather than blocking the game.
 *   - Do real work only on VI interrupts, rate-limited, so cost stays at a
 *     few microseconds when idle and well under a frame when active.
 *
 * What it does: watches the controller for the save/load/panel combos (read
 * from the game's own joybus block in RAM), takes and restores whole-machine
 * states through the cart's SDRAM (the state engine below), mirrors them to
 * the SD card, draws the panel and the feedback text (ss_overlay.c), and keeps
 * the picture steady while the game is frozen (the VI field service).
 */

#include <stdint.h>


typedef volatile uint32_t vu32;
typedef volatile uint16_t vu16;

#define MI_INTERRUPT    (*(vu32 *)0xA4300008u)
#define PI_STATUS       (*(vu32 *)0xA4600010u)

#define BRAM_BASE       0xBFFE0000u
#define SC64_REGS_BASE  0xBFFF0000u
#define SC64_IDENTIFIER (SC64_REGS_BASE + 0x0Cu)
#define SC64_KEY        (SC64_REGS_BASE + 0x10u)

#define SC64_SR_CMD     (SC64_REGS_BASE + 0x00u)
#define SC64_DATA0      (SC64_REGS_BASE + 0x04u)
#define SC64_DATA1      (SC64_REGS_BASE + 0x08u)
#define SC64_SR_CMD_ERROR (1u << 30)
#define SC64_SR_CPU_BUSY  (1u << 31)
#define SC64_CMD_CONFIG_SET 0x43u /* 'C' */
#define SC64_CFG_ROM_WRITE_ENABLE 1u

#define MI_INTR_MASK    (*(vu32 *)0xA430000Cu)
#define MI_MASK_PI      (1u << 4)
#define MI_MASK_CLR_PI  (1u << 8)
#define MI_MASK_SET_PI  (1u << 9)
#define MI_INTR_PI      (1u << 4)
#define PI_DRAM_ADDR    (*(vu32 *)0xA4600000u)
#define PI_CART_ADDR    (*(vu32 *)0xA4600004u)
#define PI_RD_LEN       (*(vu32 *)0xA4600008u) /* RDRAM -> cart */
#define PI_WR_LEN       (*(vu32 *)0xA460000Cu) /* cart -> RDRAM */
#define PI_STATUS_DMA_BUSY 1u
#define PI_STATUS_INTR  8u
#define PI_STATUS_W_RESET 1u
#define PI_STATUS_W_CLR_INTR 2u

/* v6: the RCP state the save-state engine checks, drops and re-raises. */
#define SP_STATUS       (*(vu32 *)0xA4040010u)
#define SP_SEMAPHORE_REG (*(vu32 *)0xA404001Cu)
#define SP_PC_REG        (*(vu32 *)0xA4080000u)
#define SP_HALT         (1u << 0)
#define SP_DMA_BUSY     (1u << 2)
#define SP_DMA_FULL     (1u << 3)
#define SP_CLR_INTR     (1u << 3)  /* write */
#define SP_SET_INTR     (1u << 4)  /* write */
#define DPC_START       (*(vu32 *)0xA4100000u)
#define DPC_END         (*(vu32 *)0xA4100004u)
#define DPC_CURRENT     (*(vu32 *)0xA4100008u)
#define DPC_STATUS      (*(vu32 *)0xA410000Cu)
#define DPC_CLR_XBUS    (1u << 0)  /* write: commands come from RDRAM */
#define DPC_PIPE_BUSY   (1u << 5)
#define DPC_CMD_BUSY    (1u << 6)
#define DPC_DMA_BUSY    (1u << 8)
#define MI_INIT_MODE    (*(vu32 *)0xA4300000u)
#define MI_MODE_CLR_DP  (1u << 11)
#define MI_INTR_SP      (1u << 0)
#define MI_INTR_DP      (1u << 5)
#define VI_BASE         0xA4400000u
#define VI_CURRENT      (*(vu32 *)0xA4400010u)
#define AI_STATUS       (*(vu32 *)0xA450000Cu)
#define AI_DRAM_ADDR    (*(vu32 *)0xA4500000u)
#define AI_LEN          (*(vu32 *)0xA4500004u)
#define SI_STATUS       (*(vu32 *)0xA4800018u)
#define SI_DRAM_ADDR    (*(vu32 *)0xA4800000u)

#define SC64_V2_ID      0x53437632u /* "SCv2" */
#define KEY_RESET       0x00000000u
#define KEY_UNLOCK_1    0x5F554E4Cu
#define KEY_UNLOCK_2    0x4F434B5Fu

#define HOOK_VERSION    10u /* in every state header: 10 = the RSP's memories and PC as region 1; 9 = the RDP's command pointers and SP_PC (rcp[]); 8 = format v2 (header first); 7 = PI/SI address
                            * registers, RCP status and the clock carried by the state; 6: the first states */

#define ST_OK           0u
#define ST_BAD_CMD      2u
#define ST_BAD_ARGS     3u
#define ST_CART_STALL   4u
#define ST_BUSY         5u
#define ST_DMA_FAIL     6u
#define ST_NO_ROM_WRITE 7u
#define ST_NO_STATE     8u /* v6: slot holds no state */
#define ST_BAD_STATE    9u /* v6: state belongs to another ROM */
#define ST_TIMEOUT      10u /* v6: no clean moment within STATE_WAIT_MAX frames */
#define ST_NO_ROOM      12u /* a screenshot: no cart space above the last slot (11 = ST_SD_FAIL) */

/* Cart SDRAM above the ROM (PI addresses). build.sh exports these to the menu
 * (hook_blob.h), which lays the slot table out from the ROM size:
 *   ROM end (1 MiB aligned) .. 0x13F60000  state slots, STATE_SLOT_LEN each
 *   0x13F60000 .. 0x13F80000  the hook's staging copy (the boot patcher and the
 *                             reinstall stub copy it into RDRAM; blob <= 128 KiB,
 *                             its RDRAM home 0x807D0000..0x807F0000)
 *   0x13F80000 .. 0x13FB0000  (free: the frame stash once lived here)
 *   0x13FB0000 .. 0x13FB8000  the virtual Controller Pak image (32 KiB)
 *   0x13FB8000 .. 0x13FB9000  its SD file's run table (the menu writes it)
 *   0x13FB9000 .. 0x13FB9100  borrowed-RAM mode: the pak's control block (VPAK_CTL_PI)
 *   0x13FBA000 .. 0x13FBE000  borrowed-RAM mode: the monitor, run in place from the cart (16 KiB:
 *                             +0x1000 the pak server, +0x2000 the load epilogue, +0x3000 the installer)
 *   0x13FBE000 .. 0x13FBE400  borrowed-RAM mode: a loaded state's CPU context for the monitor
 *                             (+0x220 the game's Count/Compare across a borrow)
 *   0x13FC0000 .. 0x13FE0000  borrowed-RAM mode: the stash of the hook's home (128 KiB)
 *   0x13FE0000 .. 0x14000000  the SC64's SRAM/FlashRAM save area, never touched
 * Slots: 7 for ROMs up to 8 MiB, 6 up to 16 MiB, 4 for 32 MiB, 3 for 40 MiB
 * (Paper Mario), none for 64 MiB. */
#define HOOK_STAGING_PI 0x13F60000u
#define HOOK_HOME_OFF   0x007D0000u /* the hook's RDRAM home (resident: lives there; borrowed: borrows it) */
#define FRAME_STASH_PI  0x13E20000u  /* the picture under the panel: just below the staging, 1.25 MiB (a 640x480x4
                                     * frame is 1.2 MiB; the 192 KiB it had at 0x13F80000 did not fit Racer's
                                     * 640x237x2 buffer, so a save from its panel kept the panel).
                                     * The menu stops its slots below this address. */
#define FRAME_STASH_LEN 0x00140000u
#define VPAK_PI         0x13FB0000u
#define VPAK_CTL_PI     0x13FB9000u  /* borrowed mode: the pak's control block (monitor.S PAK_CTL_*): 'VPK1',
                                     * state, the dirty stamp, the bank byte, the site's scheme, address, words */
#define MON_PAK_OFF     0x00001000u  /* the monitor's pak server (monitor.ld .pak): the RAM stub jumps there */
#define MON_EPIL_OFF    0x00002000u  /* the monitor's load epilogue (mon_epi_load): borrow_load_exit jumps there */
#define PAK_STUB_ADDR   0x80000060u  /* the stub's home in the vector page: 8 words, 0x060..0x080 */
/* borrowed-RAM mode (games that use all 8 MiB): the monitor runs in place from the
 * cart; the hook's 128 KiB home is stashed here for the length of one action */
#define MONITOR_PI      0x13FBA000u
#define MONITOR_LEN     0x00004000u
#define MONITOR_KSEG1   (0xA0000000u | MONITOR_PI)
#define STASH_PI        0x13FC0000u
#define STASH_LEN       0x00020000u
#define CTX_PI          0x13FBE000u  /* a loaded state's CPU context, for the monitor's load epilogue */
#define VPAK_LEN        0x00008000u
#define DMA_TIMEOUT_TICKS (46875u * 40u) /* 40 ms */
#define CMD_TIMEOUT_TICKS (46875u * 5u)  /* 5 ms for an SC64 MCU command */

/* DIAG: PI-free stage marker in RDRAM, reported by the engine trace on the
 * next exception (1/2/6 are set by entry.S). */
#define TR_STAGE        (*(vu32 *)0x807FFFFCu)   /* top of RAM: the hook may fill 0x807D0000..0x807F0000 */

#define CAUSE_EXC_MASK  0x7Cu
#define CAUSE_IP2_RCP   (1u << 10)
#define CAUSE_IP4_PRENMI (1u << 12)
#define MI_INTR_VI      (1u << 3)

/* COUNT runs at 46.875 MHz: 187500 ticks = 4 ms, 93750 = 2 ms. */
#define RATE_LIMIT_TICKS 187500u
#define PI_TIMEOUT_TICKS 93750u

static volatile uint32_t reentry = 0;
static uint32_t last_count = 0;
static uint32_t ticks = 0;
static uint32_t epoch = 0;
static uint32_t cart_stalled = 0; /* set on PI timeout; retried next tick */

static uint32_t dbg_entries = 0;
static uint32_t dbg_gate_pass = 0;
static uint32_t dbg_unlock_ok = 0;
static uint32_t dbg_unlock_fail = 0;
static uint32_t dbg_last_cause = 0;
static uint32_t flt_cause = 0, flt_epc = 0, flt_bad = 0, flt_count = 0, flt_n = 0;   /* the last real fault seen */
static uint32_t dbg_last_mi = 0;
static uint32_t dbg_last_pi = 0;

static uint32_t rom_write_enabled = 0; /* SC64 ROM_WRITE_ENABLE set by us */
static uint32_t snap_active = 0;       /* a PC snapshot/upload in flight (dev builds; 0 otherwise) */
static uint32_t st_pending;           /* v6: a save/load waiting for a clean interrupt (defined below) */
#define SPEED_STEP      0xFFu
static uint32_t speed_div = 1;        /* slow motion: fields per game frame (1 normal; SPEED_STEP = frame step) */
static uint32_t speed_sel = 0;        /* the panel's choice, index into its table */
static uint32_t step_prev = 0xFFFFu;  /* frame step: the last pad word our own poll saw */
static uint32_t slow_sound = 0;       /* slow motion sound: 0 pitch down (the DAC slowed with the game), 1 leave it (gaps) */


static inline uint32_t c0_cause(void) {
    uint32_t v;
    __asm__ volatile("mfc0 %0, $13" : "=r"(v));
    return v;
}

static inline uint32_t c0_count(void) {
    uint32_t v;
    __asm__ volatile("mfc0 %0, $9" : "=r"(v));
    return v;
}

static inline uint32_t c0_status(void) {
    uint32_t v;
    __asm__ volatile("mfc0 %0, $12" : "=r"(v));
    return v;
}

static inline uint32_t c0_epc(void) {
    uint32_t v;
    __asm__ volatile("mfc0 %0, $14" : "=r"(v));
    return v;
}

static inline uint32_t c0_badvaddr(void) {
    uint32_t v;
    __asm__ volatile("mfc0 %0, $8" : "=r"(v));
    return v;
}

static inline uint32_t c0_compare(void) {
    uint32_t v;
    __asm__ volatile("mfc0 %0, $11" : "=r"(v));
    return v;
}

static inline void c0_set_count(uint32_t v) {
    __asm__ volatile("mtc0 %0, $9" : : "r"(v));
}

static inline void c0_set_compare(uint32_t v) {
    __asm__ volatile("mtc0 %0, $11" : : "r"(v));
}

static inline void c0_set_entryhi(uint32_t v) {
    __asm__ volatile("mtc0 %0, $10" : : "r"(v));
}

static inline uint32_t c0_watchlo(void) {
    uint32_t v;
    __asm__ volatile("mfc0 %0, $18" : "=r"(v));
    return v;
}

static inline void c0_set_watchlo(uint32_t v) {
    __asm__ volatile("mtc0 %0, $18" : : "r"(v));
}

static inline void c0_set_cause(uint32_t v) {
    __asm__ volatile("mtc0 %0, $13" : : "r"(v));
}

#define SEC_T0()    do { } while (0)
#define SEC_END(m)  do { } while (0)

static uint32_t rom_write_set(uint32_t on);   /* below, with the SC64 commands */

/* ---- The VI during a freeze --------------------------------------------------
 * Interlaced modes (Turok 2's hi-res) need the VI re-aimed every field: the
 * game's VI manager writes a different V_START, V_BURST, Y_SCALE and ORIGIN
 * (one line down) for the odd field so its lines land between the even ones.
 * Registers left alone through a 2 s freeze put one field a line off and the
 * picture bobs at 60 Hz for the whole save, load or panel. The hook records,
 * at every VI interrupt tick, the set the game wrote for the field that just
 * ended (the registers still hold it: the hook runs before the VI manager),
 * keyed by the field bit of VI_CURRENT the way libultra keys its tables, and
 * while the game is frozen writes the recorded set for each new field. ORIGIN
 * alternates around the buffer displayed at the freeze (the one buffer known
 * to be complete) by the one-line offset learned from two consecutive fields
 * showing the same buffer; before that is known both fields show its even
 * lines. Every freeze ends at a field start, where the game's VI handler
 * (resumed or restored) expects to be running. */
#define VI_CTRL_SERRATE 0x40u
static uint32_t vi_fld[2][4] = {{0}};         /* [field]: V_START, V_BURST, Y_SCALE, INTR (initialised: no .bss) */
static uint32_t vi_fld_origin[2] = {0};
static uint32_t vi_fld_seen = 0;              /* bit f: vi_fld[f] recorded since SERRATE came on */
static int32_t vi_fld_lineoff = 0;            /* ORIGIN(field 1) - ORIGIN(field 0) */
static uint32_t vi_fld_line = 0;              /* the line size that offset was learned with */
static uint32_t vi_frz_frozen = 0;            /* a freeze is in progress */
static uint32_t vi_frz_active = 0, vi_frz_base0 = 0;
static uint32_t vi_frz_prev = 0;              /* last line seen while frozen (wrap = new field) */
static uint32_t vi_frz_count = 0;             /* fields aimed during the current freeze */
static uint32_t frz_count0 = 0, frz_compare0 = 0;   /* the game's clock when the freeze began */
static uint32_t dbg_vi_tick = 0, dbg_vi_flip = 0, dbg_vi_wrap = 0, dbg_vi_intr = 0, dbg_vi_lastbit = 0;   /* probes */

static uint32_t vi_line_bytes(uint32_t ctrl) {
    return (*(vu32 *)(VI_BASE + 0x08u) & 0xFFFu) * (((ctrl & 3u) == 3u) ? 4u : 2u);
}

/* at a VI interrupt tick, before the game's handler */
static void vi_fld_record(void) {
    uint32_t ctrl = *(vu32 *)VI_BASE;
    if (!(ctrl & VI_CTRL_SERRATE)) {
        vi_fld_seen = 0;
        return;
    }
    dbg_vi_tick = VI_CURRENT;
    uint32_t f = (VI_CURRENT & 1u) ^ 1u;
    vi_fld[f][0] = *(vu32 *)(VI_BASE + 0x28u);
    vi_fld[f][1] = *(vu32 *)(VI_BASE + 0x2Cu);
    vi_fld[f][2] = *(vu32 *)(VI_BASE + 0x34u);
    vi_fld[f][3] = *(vu32 *)(VI_BASE + 0x0Cu);
    vi_fld_origin[f] = *(vu32 *)(VI_BASE + 0x04u) & 0x00FFFFFFu;
    vi_fld_seen |= 1u << f;
    if (vi_fld_seen == 3u) {
        int32_t d = (int32_t)vi_fld_origin[1] - (int32_t)vi_fld_origin[0];
        int32_t line = (int32_t)vi_line_bytes(ctrl);
        if (line && ((d == line) || (d == -line))) {
            vi_fld_lineoff = d;
            vi_fld_line = (uint32_t)line;
        }
    }
}

/* aim the VI for field f the way the game does at its VI interrupt */
static void vi_frz_aim(uint32_t f) {
    *(vu32 *)(VI_BASE + 0x28u) = vi_fld[f][0];
    *(vu32 *)(VI_BASE + 0x2Cu) = vi_fld[f][1];
    *(vu32 *)(VI_BASE + 0x34u) = vi_fld[f][2];
    *(vu32 *)(VI_BASE + 0x0Cu) = vi_fld[f][3];
    if (vi_frz_base0) {
        *(vu32 *)(VI_BASE + 0x04u) = vi_frz_base0 + (f ? (uint32_t)vi_fld_lineoff : 0u);
    }
    vi_frz_count++;
}

/* after the half-line counter wrapped: wait for the half-line the game's VI
 * interrupt fires on, so the field bit is read at the phase the game reads it */
static uint32_t vi_wait_intr_line(void) {
    uint32_t intr = (vi_fld[0][3] & 0x3FFu) >> 1;
    uint32_t t0 = c0_count();
    while (((VI_CURRENT >> 1) < intr) && ((c0_count() - t0) < (46875u / 2u))) {
    }
    return VI_CURRENT;
}

/* with the game frozen, from every wait loop: a new field gets its set at the
 * moment the game's own VI interrupt would have programmed it */
static void vi_frz_service(void) {
    if (!vi_frz_active) return;
    uint32_t cur = VI_CURRENT;
    if ((cur & 1u) != dbg_vi_lastbit) {       /* probe: where in the field the bit flips */
        dbg_vi_lastbit = cur & 1u;
        dbg_vi_flip = cur;
    }
    uint32_t line = cur >> 1;
    if (line >= vi_frz_prev) {
        vi_frz_prev = line;
        return;
    }
    dbg_vi_wrap = (vi_frz_prev << 16) | cur;
    cur = vi_wait_intr_line();
    dbg_vi_intr = cur;
    vi_frz_prev = cur >> 1;
    vi_frz_aim(cur & 1u);
}

/* at_vi: the freeze starts at a VI interrupt, where the game would program the
 * field now; otherwise mid-field, already programmed */
static void vi_frz_begin(uint32_t at_vi) {
    uint32_t ctrl = *(vu32 *)VI_BASE;
    vi_frz_frozen = 1;
    frz_count0 = c0_count();
    frz_compare0 = c0_compare();
    vi_frz_active = 0;
    vi_frz_base0 = 0;
    vi_frz_count = 0;
    if (!(ctrl & VI_CTRL_SERRATE) || (vi_fld_seen != 3u)) return;
    uint32_t cur = VI_CURRENT;
    uint32_t f_cur = (cur & 1u) ^ (at_vi ? 1u : 0u);    /* the field the registers describe */
    if (vi_fld_lineoff && (vi_fld_line == vi_line_bytes(ctrl))) {
        uint32_t base = *(vu32 *)(VI_BASE + 0x04u) & 0x00FFFFFFu;
        vi_frz_base0 = base - (f_cur ? (uint32_t)vi_fld_lineoff : 0u);
    }
    vi_frz_prev = cur >> 1;
    dbg_vi_lastbit = cur & 1u;
    vi_frz_active = 1;
    if (at_vi) {
        vi_frz_aim(cur & 1u);
    }
}

/* one full field with the game held: the counter wraps, the field service aims the
 * new field the way the game would have */
static void vi_hold_field(void) {
    uint32_t t0 = c0_count();
    if (!((*(vu32 *)VI_BASE) & 3u)) {                 /* VI off: just the time */
        while ((c0_count() - t0) < (46875u * 17u)) {
        }
        return;
    }
    uint32_t prev = VI_CURRENT >> 1;
    while ((c0_count() - t0) < (46875u * 18u)) {
        uint32_t line = VI_CURRENT >> 1;
        vi_frz_service();
        if (line < prev) return;
        prev = line;
    }
}

/* the freeze ends where a VI interrupt would be handled: the game's own handler
 * (resumed or restored) programs the new field from there */
static void vi_frz_end(void) {
    if (!vi_frz_frozen) return;
    rom_write_set(0);                 /* the cart's ROM is read-only again for the game */
    if ((*(vu32 *)VI_BASE) & 3u) {       /* VI on: bounded wait for the counter to wrap */
        uint32_t t0 = c0_count(), prev = VI_CURRENT >> 1;
        while ((c0_count() - t0) < (46875u * 18u)) {
            uint32_t line = VI_CURRENT >> 1;
            if (line < prev) {
                vi_wait_intr_line();
                break;
            }
            prev = line;
            vi_frz_service();
        }
    }
    /* As far as the game's clock is concerned the freeze never happened: Count
     * goes back to where it was (plus a few microseconds of handler) and Compare
     * is re-armed where it was. Cutscenes paced by osGetTime (Turok 2) otherwise
     * skip the freeze's length when the game resumes, and a minute spent in the
     * panel would be a minute of cutscene lost. The load handler puts the saved
     * world's clock back after this (it has to come last: Episode I Racer's physics
     * ran on the gap otherwise). Rewriting Compare also drops a timer interrupt
     * that came due during the freeze; it comes due again at the same distance. */
    c0_set_count(frz_count0 + 400u);
    c0_set_compare(frz_compare0);
    vi_frz_frozen = 0;
    vi_frz_active = 0;
    vi_frz_base0 = 0;
}

/* Wait for the PI bus to go idle. PI_STATUS itself is an RCP register and is
 * always safe to read; bits 0/1 are DMA/IO busy. Returns 0 on timeout. */
static uint32_t pi_wait(void) {
    if (cart_stalled) {
        return 0;
    }
    uint32_t start = c0_count();
    while (PI_STATUS & 3u) {
        if ((c0_count() - start) > PI_TIMEOUT_TICKS) {
            cart_stalled = 1;
            return 0;
        }
    }
    return 1;
}

/* The PI latches the address of EVERY CPU access to the cart into PI_CART_ADDR
 * (measured: after the hook's mailbox polls the register reads 0x1FFE0014, our
 * BRAM address). libultra's __osPiRawStartDma writes PI_DRAM_ADDR, PI_CART_ADDR
 * and then the length register as separate stores with interrupts enabled, so a
 * hook exception landing between the address and the length store sends the
 * game's DMA to OUR address instead of the ROM. Star Fox 64 streams audio in
 * thousands of 128-byte DMAs per second and hit that within seconds of the
 * music starting (its sequence player then chewed on a flag table). The same
 * two-step exists for the SI (SI_DRAM_ADDR, then the PIF address register).
 * So: every hook entry records the three address registers and puts them back
 * before returning to the game; a saved state carries them as well. */
static uint32_t pi_saved_dram = 0, pi_saved_cart = 0, pi_touched = 0;
static uint32_t si_saved_dram = 0, si_touched = 0;

static uint32_t pio_read(uint32_t kseg1_addr, uint32_t *value) {
    if (!pi_wait()) {
        return 0;
    }
    pi_touched = 1;
    *value = *(vu32 *)kseg1_addr;
    return 1;
}

/* DIAG: after a cart write, wait ~8.5 us so the next PI_STATUS poll cannot
 * race the PI's busy flag (a write issued while busy is silently dropped). */
static void pi_settle(void) {
    uint32_t start = c0_count();
    while ((c0_count() - start) < 400u) {
    }
}

static uint32_t pio_write(uint32_t kseg1_addr, uint32_t value) {
    if (!pi_wait()) {
        return 0;
    }
    pi_touched = 1;
    *(vu32 *)kseg1_addr = value;
    pi_settle();
    return 1;
}


/* Stage crumbs in BRAM (+0xF40): readable from the PC even when the hook has
 * died, so a hang can be placed. Written only on rare events (state ops, SD
 * work, a re-entered tick), never per frame. */
#define CRUMB_BASE      0xF40u
#define CRUMB_MAGIC     0x53544731u /* "STG1" */
static uint32_t crumb_count = 0;
static uint32_t reentry_marked = 0;
static void crumb(uint32_t stage, uint32_t a, uint32_t b) {
    crumb_count++;
    pio_write(BRAM_BASE + CRUMB_BASE + 0x00, CRUMB_MAGIC);
    pio_write(BRAM_BASE + CRUMB_BASE + 0x04, stage);
    pio_write(BRAM_BASE + CRUMB_BASE + 0x08, crumb_count);
    pio_write(BRAM_BASE + CRUMB_BASE + 0x0C, a);
    pio_write(BRAM_BASE + CRUMB_BASE + 0x10, b);
    pio_write(BRAM_BASE + CRUMB_BASE + 0x14, c0_count());
}

/* Always replay the unlock sequence (idempotent when already unlocked, and
 * the register block reads as nothing while locked), then verify. */
static uint32_t ensure_unlocked(void) {
    uint32_t id;
    if (!pio_write(SC64_KEY, KEY_RESET)) return 0;
    if (!pio_write(SC64_KEY, KEY_UNLOCK_1)) return 0;
    if (!pio_write(SC64_KEY, KEY_UNLOCK_2)) return 0;
    if (!pio_read(SC64_IDENTIFIER, &id)) return 0;
    return (id == SC64_V2_ID) ? 1u : 0u;
}

/* Run an SC64 MCU command (registers unlocked). Bounded wait; 0 on failure. */
static uint32_t sc64_command(uint32_t id, uint32_t arg0, uint32_t arg1, uint32_t *rsp1) {
    uint32_t sr, start;
    if (!pio_write(SC64_DATA0, arg0)) return 0;
    if (!pio_write(SC64_DATA1, arg1)) return 0;
    if (!pio_write(SC64_SR_CMD, id & 0xFFu)) return 0;
    start = c0_count();
    for (;;) {
        if (!pio_read(SC64_SR_CMD, &sr)) return 0;
        if (!(sr & SC64_SR_CPU_BUSY)) break;
        if ((c0_count() - start) > CMD_TIMEOUT_TICKS) return 0;
    }
    if (sr & SC64_SR_CMD_ERROR) return 0;
    if (rsp1 != 0) {
        if (!pio_read(SC64_DATA1, rsp1)) return 0;
    }
    return 1;
}

/* The cart's ROM area is writable from the N64 only while the hook needs it:
 * inside a freeze (state, thumbnail and frame copies) and during a PC snapshot.
 * Left on for the whole game, a copy-protection write to ROM sticks and the game
 * learns it is not running from a mask ROM (Banjo-Kazooie's intro went black). */
static uint32_t rom_write_set(uint32_t on) {
    uint32_t prev;
    if (rom_write_enabled == on) return 1;
    if (!sc64_command(SC64_CMD_CONFIG_SET, SC64_CFG_ROM_WRITE_ENABLE, on, &prev)) return 0;
    rom_write_enabled = on;
    return 1;
}

/* Push every dirty D-cache line to RDRAM so the PI DMA sees what the CPU
 * sees (Index_Writeback_Invalidate over all 512 lines; ~100 us). */
static void dcache_writeback_all(void) {
    for (uint32_t a = 0x80000000u; a < 0x80002000u; a += 16u) {
        __asm__ volatile("cache 0x01, 0(%0)" : : "r"(a) : "memory");
    }
}



/* ---- Save states (hook v6) --------------------------------------------------
 * A state is the machine as the CPU sees it: RDRAM below the cheat engine
 * (0x80000000..0x807C0000) plus the interrupted CPU context (every GPR, HI/LO,
 * the FPU, CP0 Status/EPC, the Compare countdown, the TLB) and the MI mask.
 * It is taken inside the exception (EXL=1: nothing else runs) at a moment when
 * the RCP holds no work of its own: a VI or SP-done interrupt with the RSP
 * halted, RDP/PI/SI idle and no other interrupt pending, so RAM alone
 * describes everything. Loading DMAs the image back, puts the hardware bits
 * back, drops the pending interrupts of the world being replaced (re-raising
 * the SP-done a snapshot was taken on) and ERETs straight into the saved
 * context.
 *
 * Slot layout (format v2, cart SDRAM PI addresses, the SD file is a copy of the
 * first STATE_IMAGE_OFF + image_len bytes; docs/state-format.md):
 *   +0x0000  header (4 KiB, written last so a torn save never carries a valid magic)
 *   +0x1000  thumbnail (80x60 RGBA5551)
 *   +0x3E00  a zeroed sector: the mirror writes it over the file's header first
 *   +0x4000  the RAM image, image_len bytes (7.75 MiB here; a full-RAM mode may
 *            write 8 MiB, and the loader takes whatever the header says fits)
 *   +0x7C7000  the SD file's run table, written by the menu (never mirrored) */
#define STATE_OP_SAVE   1u
#define STATE_OP_LOAD   2u
#define STATE_OP_QUERY  3u
#define STATE_OP_OVTEST 8u   /* dev: overlay experiment */
#define STATE_MAGIC     0x53543634u /* "ST64" */
#define STATE_FMT       2u          /* v2: header first; the loader accepts any version >= 2 it can restore */
#define STATE_IMAGE_BASE 0x80000000u
#define STATE_IMAGE_LEN 0x007C0000u /* what this build saves, and the most it restores (the hook lives above) */
#define STATE_HDR_OFF   0x00000000u
#define STATE_HDR_LEN   0x1000u
#define STATE_THUMB_OFF 0x00001000u
#define STATE_ZERO_OFF  0x00003E00u
#define STATE_IMAGE_OFF 0x00004000u
#define STATE_SLOT_LEN  0x007C8000u /* the first release's stride (head 16 KiB + image 7.75 MiB + spare); both placements now use STATE_SLOT_LEN_B */
#define STATE_IMAGE_LEN_B 0x00800000u /* borrowed mode: all of RDRAM (the region under the hook comes from the stash) */
#define STATE_SLOT_LEN_B  0x00810000u /* borrowed mode: head 16 KiB + image 8 MiB + spare, run table in the last 4 KiB */
#define THUMB_W         80u
#define THUMB_H         60u
#define THUMB_BYTES     (THUMB_W * THUMB_H * 2u)
#define STATE_CHUNK     0x40000u
/* v10: the RSP's memories (DMEM then IMEM, as the CPU sees them at 0xA4000000) and its
 * PC travel with the state as region 1, right after the image. A moment between a
 * game's osSpTaskLoad and its osSpTaskStartGo (the task DMAed into the RSP and not
 * started: halted at PC 0) passes every idle check, and a load of such a state started
 * the task on whatever the RSP held at the time: Episode I Racer's RDP never finished
 * and the game waited for it (a save from the panel). */
#define REGION_RSP      1u
#define STATE_RSP_LEN   0x2000u
#define STATE_DMA_TIMEOUT_TICKS (46875u * 2000u) /* 2 s per chunk */
#define STATE_WAIT_MAX  600u                     /* VI frames to wait for a clean moment */
#define STATE_PREFER_VI 30u                      /* VI frames to hold out for a frame boundary before taking an SP/DP-done moment */
static uint32_t borrow_mi = 0;                   /* borrowed mode: the RCP interrupt (one MI bit) the monitor's moment is */

struct state_ctx {                        /* offsets mirrored by state.S */
    uint64_t gpr[32];                     /* 0x000: by register number (0, 26, 27 unused) */
    uint64_t lo, hi;                      /* 0x100 */
    uint64_t fpr[32];                     /* 0x110: even registers always, odd ones when Status.FR */
    uint32_t status, epc, fcr31, entryhi; /* 0x210 */
    uint32_t tlb[32][4];                  /* 0x220: pagemask, entryhi, entrylo0, entrylo1 */
};

struct state_hdr {                        /* everything a reader needs is in the first 512 bytes */
    uint32_t magic, version, flags, image_len;            /* 0x00: flags bit0 valid, bit1 thumbnail present */
    uint32_t image_base, rom_crc1, rom_crc2, stamp;       /* 0x10: stamp = RTC date word */
    uint32_t cause, badvaddr, compare_delta, mi_mask;     /* 0x20 */
    uint32_t reraise_sp, memsize, dma_ticks, wait_frames; /* 0x30 */
    uint32_t hook_version, stamp_time, reserved[6];       /* 0x40: reserved = PI_DRAM, PI_CART, SI_DRAM, DPC_STATUS, SP_STATUS, Count */
    uint32_t hdr_len, image_off, thumb_off, thumb_size;   /* 0x60: the layout, so a reader never assumes it */
    uint32_t checksum, slot_len, regions_n, spare0;       /* 0x70: checksum over the header with this word 0 (0 = none) */
    uint32_t vi[16];                                      /* 0x80 */
    struct state_ctx ctx;                                 /* 0xC0 */
    struct { uint32_t kind, off, len, arg; } regions[8];  /* 0x4E0: extra regions in the file (none defined yet) */
    uint32_t rcp[8];                                      /* 0x560: v9: DPC_START, DPC_END, DPC_CURRENT, SP_PC at the moment, then zeros */
    uint8_t pad[0x1000u - 0x580u];
};

_Static_assert(sizeof(struct state_ctx) == 0x420u, "state_ctx layout");
_Static_assert(__builtin_offsetof(struct state_ctx, fpr) == 0x110u, "state_ctx fpr");
_Static_assert(__builtin_offsetof(struct state_ctx, status) == 0x210u, "state_ctx status");
_Static_assert(__builtin_offsetof(struct state_ctx, tlb) == 0x220u, "state_ctx tlb");
_Static_assert(__builtin_offsetof(struct state_hdr, hdr_len) == 0x60u, "state_hdr layout words");
_Static_assert(__builtin_offsetof(struct state_hdr, vi) == 0x80u, "state_hdr vi");
_Static_assert(__builtin_offsetof(struct state_hdr, ctx) == 0xC0u, "state_hdr ctx");
_Static_assert(__builtin_offsetof(struct state_hdr, regions) == 0x4E0u, "state_hdr regions");
_Static_assert(__builtin_offsetof(struct state_hdr, rcp) == 0x560u, "state_hdr rcp");
_Static_assert(sizeof(struct state_hdr) == 0x1000u, "state_hdr size");

extern uint64_t save_area[32];   /* entry.S: the interrupted GPRs (see save_map), then lo, hi */
extern void fpu_save(struct state_ctx *ctx);
extern void tlb_save(struct state_ctx *ctx);
extern void tlb_restore(const struct state_ctx *ctx);
extern void state_resume(const struct state_ctx *ctx) __attribute__((noreturn));

static struct state_hdr st_hdr __attribute__((aligned(16))) = {0};     /* initialised: keeps it out of .bss */
static struct state_ctx st_live __attribute__((aligned(16))) = {.gpr = {0}};
static uint32_t st_pending = 0;      /* STATE_OP_SAVE / STATE_OP_LOAD waiting for a clean interrupt */
static uint32_t st_slot = 0;         /* cart PI address of the slot */
static uint32_t st_seq = 0;
static uint32_t st_wait = 0;         /* VI frames spent waiting */
static uint32_t st_dma_ticks = 0;
static uint32_t st_last_op = 0;
static uint32_t st_last_status = 0;
static uint32_t st_last_dma = 0;
static uint32_t st_last_wait = 0;
/* a one-command RDP list (G_RDPFULLSYNC): running it re-raises the DP interrupt a snapshot was taken on */
static uint64_t st_sync_cmd[2] __attribute__((aligned(16))) = {0xE900000000000000ull, 0};
/* a few samples of silence: played after a load so the AI raises an interrupt for the restored world */
static uint64_t st_ai_silence[2] __attribute__((aligned(16))) = {0, 0};
/* why the clean moment is not found: counters since the request was queued, last register values */
static uint32_t st_dbg[12] = {0};   /* seen, not-int, ip, mi, sp, dp, pi/si, cause, status, mi_val, sp_val, dp_val */
/* SD mirror (defined after the trigger block) */
static uint32_t sd_state;
static uint32_t sd_read_slot(uint32_t slot_base);
static uint32_t sd_write_begin(uint32_t slot_base);
static uint32_t sd_write_begin_head(uint32_t slot_base);   /* the header's sectors only */
static void pif_poll_block_send(void);         /* the standard poll block into the PIF (after a load) */
static uint32_t sd_pending_slot;              /* a state mirror waiting for the pak's to finish */
static uint32_t st_suspend = 0;               /* the state being saved is a suspend: the next launch resumes it */
static uint32_t resume_pending = 0;           /* the slot (+1) being loaded for that resume */
static uint32_t resume_done = 0;              /* the boot-time resume was attempted */
static uint32_t vpak_serve(uint32_t base);    /* the virtual Controller Pak (defined after the SD mirror) */
static void vpak_tramp_apply(uint32_t on);     /* its jump in the game's handler: in place, or the game's own words */
static uint32_t tramp_state = 0;               /* 0 not tried, 1 the handler is patched, 2 no libultra site, 3 the site changed */
static uint32_t tramp_scheme = 0;              /* 0 resident: four words to vpak_tramp; borrowed: 1 a jal to the stub in place
                                                * of the handler's own jal, 2 four words to the stub (a kuseg handler) */
static uint32_t pak_ctl_ok = 0;                /* borrowed: the control block on the cart was read (pak_ctl_load) */
static uint32_t pak_ctl_load(void);
static uint32_t pak_flush_sync(void);
static void exit_wait_ms(uint32_t ms);
static uint32_t vpak_loaded;
/* overlay (ss_overlay.c, included after the SD mirror) */
static uint32_t thumb_ready;
static void thumb_capture(void);
static void thumb_store(uint32_t slot_base);
static void ov_stamp_now(uint32_t *date, uint32_t *time);
static uint32_t menu_run(uint32_t cause, uint32_t mi);
static void feedback_tick(void);
static void feedback_show(const char *text);
static void feedback_reset(uint32_t origin, uint32_t width);
static uint32_t frame_stash(void);
static void ov_disp_range(uint32_t *start, uint32_t *len);
static void feedback_service(void);
#define STATE_OP_MENU   9u
static uint32_t image_len(void);
static uint32_t image_len_max(void);
static uint32_t slot_len_cur(void);
static uint32_t borrowed_mode(void);
static uint32_t borrow_image_copy(uint32_t slot, uint32_t ioff, uint32_t ilen, uint32_t to_cart);
static void borrow_diag_words(uint32_t place);
static void borrow_load_exit(void) __attribute__((noreturn));
static void borrow_pc_ack(uint32_t status);    /* a PC-requested borrow: its answer on the cart */

/* One PI DMA run to completion with the game frozen: PI interrupt masked, our
 * completion swallowed, the game's pending PI address registers put back. */
static uint32_t pi_dma(uint32_t rdram, uint32_t cart, uint32_t len, uint32_t to_cart) {
    if (!pi_wait()) {
        return 0;
    }
    uint32_t mask = MI_INTR_MASK;
    uint32_t saved_dram = PI_DRAM_ADDR;
    uint32_t saved_cart = PI_CART_ADDR;
    MI_INTR_MASK = MI_MASK_CLR_PI;
    uint32_t t0 = c0_count();
    PI_DRAM_ADDR = rdram & 0x1FFFFFFFu;
    PI_CART_ADDR = cart & 0x1FFFFFFFu;
    if (to_cart) {
        PI_RD_LEN = len - 1u;
    } else {
        PI_WR_LEN = len - 1u;
    }
    uint32_t ok = 1;
    while (PI_STATUS & PI_STATUS_DMA_BUSY) {
        if ((c0_count() - t0) > STATE_DMA_TIMEOUT_TICKS) {
            ok = 0;
            break;
        }
        vi_frz_service();                /* a frozen interlaced game still gets its fields */
    }
    st_dma_ticks += (c0_count() - t0);
    PI_STATUS = ok ? PI_STATUS_W_CLR_INTR : PI_STATUS_W_RESET;
    PI_DRAM_ADDR = saved_dram;
    PI_CART_ADDR = saved_cart;
    if (mask & MI_MASK_PI) {
        MI_INTR_MASK = MI_MASK_SET_PI;
    }
    return ok;
}

static void icache_invalidate_all(void) {
    for (uint32_t a = 0x80000000u; a < 0x80004000u; a += 32u) {
        __asm__ volatile("cache 0x00, 0(%0)" : : "r"(a) : "memory");   /* Index_Invalidate_I */
    }
}

static void state_capture(uint32_t cause, uint32_t mi) {
    static const uint8_t save_map[29] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 24, 25,
                                         16, 17, 18, 19, 20, 21, 22, 23, 28, 29, 30, 31};
    struct state_hdr *h = &st_hdr;
    for (uint32_t i = 0; i < 32u; i++) {
        h->ctx.gpr[i] = 0;
    }
    for (uint32_t i = 0; i < 29u; i++) {
        h->ctx.gpr[save_map[i]] = save_area[i];
    }
    h->ctx.lo = save_area[29];
    h->ctx.hi = save_area[30];
    h->ctx.status = c0_status();
    h->ctx.epc = c0_epc();
    fpu_save(&h->ctx);
    tlb_save(&h->ctx);
    h->magic = STATE_MAGIC;
    h->version = STATE_FMT;
    h->image_len = image_len();
    h->image_base = STATE_IMAGE_BASE;
    h->rom_crc1 = 0;
    h->rom_crc2 = 0;
    pio_read(0xB0000010u, &h->rom_crc1);
    pio_read(0xB0000014u, &h->rom_crc2);
    ov_stamp_now(&h->stamp, &h->stamp_time);
    h->flags = 1u | (thumb_ready ? 2u : 0u) | (st_suspend ? 4u : 0u);   /* bit 2: resume on the next launch */
    h->cause = cause;
    h->badvaddr = c0_badvaddr();
    /* The world's clock is the one the game last saw: the freeze's start, not now.
     * A save from the panel comes minutes into the freeze, and the state used to carry
     * the current Count; a load then handed the game the whole visit as one frame
     * (Episode I Racer's pod stopped dead or flew). */
    uint32_t clk_count = vi_frz_frozen ? (frz_count0 + 400u) : c0_count();
    uint32_t clk_compare = vi_frz_frozen ? frz_compare0 : c0_compare();
    h->compare_delta = clk_compare - clk_count;
    h->mi_mask = MI_INTR_MASK & 0x3Fu;
    h->reraise_sp = (mi == MI_INTR_SP) ? 1u : ((mi == MI_INTR_DP) ? 2u : 0u);
    h->memsize = *(vu32 *)0x80000318u;
    h->hook_version = HOOK_VERSION;
    for (uint32_t i = 0; i < 14u; i++) {
        h->vi[i] = *(vu32 *)(VI_BASE + 4u * i);
    }
    h->vi[14] = 0;
    h->vi[15] = 0;
    h->reserved[0] = pi_saved_dram;   /* v7: the game's DMA address registers at the clean moment */
    h->reserved[1] = pi_saved_cart;
    h->reserved[2] = si_saved_dram;
    h->reserved[3] = DPC_STATUS;      /* v7: RDP freeze/xbus and RSP signal bits: schedulers that
                                       * freeze the RDP between frames (Rare) track them in hardware */
    h->reserved[4] = SP_STATUS;
    h->reserved[5] = clk_count;       /* v7: the world's clock (osGetTime = Count + RAM); restored on load */
    h->rcp[0] = DPC_START;            /* v9: the RDP's command pointers and the RSP's PC at the moment. A
                                       * gfx task yielded for audio resumes its command ring where the RDP
                                       * left it; without them Jet Force Gemini's first task after a load
                                       * hung on the command bus and its watchdog halted the game */
    h->rcp[1] = DPC_END;
    h->rcp[2] = DPC_CURRENT;
    h->rcp[3] = SP_PC_REG;
    h->hdr_len = STATE_HDR_LEN;       /* v2: the layout travels with the state */
    h->image_off = STATE_IMAGE_OFF;
    h->thumb_off = STATE_THUMB_OFF;
    h->thumb_size = THUMB_BYTES;
    h->slot_len = slot_len_cur();
    h->regions_n = 0;
    h->spare0 = 0;
    h->checksum = 0;
    for (uint32_t i = 0; i < 8u; i++) {
        h->regions[i].kind = 0;
        h->regions[i].off = 0;
        h->regions[i].len = 0;
        h->regions[i].arg = 0;
    }
}

/* Rotate-xor over the header words with the checksum word taken as 0; never 0. */
static uint32_t hdr_checksum(const struct state_hdr *h) {
    const uint32_t *w = (const uint32_t *)h;
    uint32_t s = STATE_MAGIC;
    for (uint32_t i = 0; i < STATE_HDR_LEN / 4u; i++) {
        uint32_t v = (i == (__builtin_offsetof(struct state_hdr, checksum) / 4u)) ? 0u : w[i];
        s = ((s << 1) | (s >> 31)) ^ v;
    }
    return s ? s : 1u;
}

/* Can this build restore the state the header describes? Tolerant on read: any
 * version from 2 up, any image length that fits below the hook, a checksum only
 * when the writer stored one. */
static uint32_t hdr_status(const struct state_hdr *h, uint32_t crc1, uint32_t crc2) {
    if ((h->magic != STATE_MAGIC) || (h->version < 2u)) return ST_NO_STATE;
    if (h->checksum && (hdr_checksum(h) != h->checksum)) return ST_NO_STATE;
    if ((h->rom_crc1 != crc1) || (h->rom_crc2 != crc2)) return ST_BAD_STATE;
    if ((h->image_base != STATE_IMAGE_BASE) || (h->image_off < STATE_HDR_LEN) || (h->image_off & 0xFu) ||
        (h->image_len == 0) || (h->image_len & 0xFu) || (h->image_len > STATE_IMAGE_LEN_B) ||
        ((h->image_off + h->image_len) > (slot_len_cur() - 0x1000u))) {
        return ST_NO_STATE;           /* not a state this build can read at all */
    }
    /* a full-RAM state (saved with the routine on the cartridge) loads on the resident
     * hook too: the load restores what fits below the hook and leaves the top as it is */
    return ST_OK;
}

static uint32_t zero_sector[128] __attribute__((aligned(16))) = {0};

/* the RSP's memories, one 4 KiB half at a time through this buffer (the RSP is halted
 * at any moment a save or a load takes, so the CPU may touch them) */
static uint32_t st_spmem[1024] __attribute__((aligned(16))) = {0};

static uint32_t rsp_mem_save(uint32_t cart) {
    for (uint32_t half = 0; half < 2u; half++) {
        const vu32 *src = (const vu32 *)(0xA4000000u + half * 0x1000u);
        for (uint32_t i = 0; i < 1024u; i++) {
            st_spmem[i] = src[i];
        }
        dcache_writeback_all();
        if (!pi_dma((uint32_t)(uintptr_t)st_spmem, cart + half * 0x1000u, 0x1000u, 1u)) return 0;
    }
    return 1u;
}

static uint32_t rsp_mem_load(uint32_t cart) {
    const vu32 *src = (const vu32 *)(0xA0000000u | ((uint32_t)(uintptr_t)st_spmem & 0x1FFFFFFFu));
    for (uint32_t half = 0; half < 2u; half++) {
        dcache_writeback_all();
        if (!pi_dma((uint32_t)(uintptr_t)st_spmem, cart + half * 0x1000u, 0x1000u, 0u)) return 0;
        vu32 *dst = (vu32 *)(0xA4000000u + half * 0x1000u);
        for (uint32_t i = 0; i < 1024u; i++) {
            dst[i] = src[i];
        }
    }
    return 1u;
}

static uint32_t state_do_save_body(uint32_t cause, uint32_t mi) {
    state_capture(cause, mi);
    dcache_writeback_all();
    uint32_t ilen = st_hdr.image_len;
    if (borrowed_mode()) {
        borrow_diag_words(1u);        /* the game's words under entry.S's stage marks, for the image */
        if (!borrow_image_copy(st_slot, STATE_IMAGE_OFF, ilen, 1u)) {
            return ST_DMA_FAIL;
        }
    } else {
        for (uint32_t off = 0; off < ilen; off += STATE_CHUNK) {
            if (!pi_dma(STATE_IMAGE_BASE + off, st_slot + STATE_IMAGE_OFF + off, STATE_CHUNK, 1u)) {
                return ST_DMA_FAIL;
            }
        }
    }
    /* v10: the RSP's memories and PC, right after the image (region 1) */
    {
        uint32_t roff = STATE_IMAGE_OFF + ilen;
        if (!rsp_mem_save(st_slot + roff)) {
            return ST_DMA_FAIL;
        }
        st_hdr.regions[0].kind = REGION_RSP;
        st_hdr.regions[0].off = roff;
        st_hdr.regions[0].len = STATE_RSP_LEN;
        st_hdr.regions[0].arg = SP_PC_REG;
        st_hdr.regions_n = 1u;
    }
    /* the head: thumbnail, the zeroed sector for the mirror, then the header last */
    thumb_store(st_slot);
    if (!pi_dma((uint32_t)(uintptr_t)zero_sector, st_slot + STATE_ZERO_OFF, 512u, 1u)) {
        return ST_DMA_FAIL;
    }
    st_hdr.dma_ticks = st_dma_ticks;
    st_hdr.wait_frames = st_wait;
    st_hdr.checksum = 0;
    st_hdr.checksum = hdr_checksum(&st_hdr);
    dcache_writeback_all();
    if (!pi_dma((uint32_t)(uintptr_t)&st_hdr, st_slot + STATE_HDR_OFF, STATE_HDR_LEN, 1u)) {
        return ST_DMA_FAIL;
    }
    return ST_OK;
}

static uint32_t state_do_save(uint32_t cause, uint32_t mi) {
    vpak_tramp_apply(0);          /* the image carries the game's own handler words, never the pak's jump */
    uint32_t r = state_do_save_body(cause, mi);
    vpak_tramp_apply(1u);
    return r;
}

/* The cheat engine's words in the exception-vector page belong to the boot, not
 * to the state: 0x180 jumps to wherever this boot put the engine (the vector page
 * with no codes loaded, 0x807C5C00 with codes), 0x090..0x0EF holds the vector-page
 * engine and 0x360..0x3EF its reinstall stub. A state saved under the other
 * placement would otherwise send every exception the wrong way after a load: the
 * codes silently stopped (a no-codes state loaded with codes on), or the game
 * jumped into junk (the other way round). Keep this boot's words across the copy. */
#define BOOT_WORDS_N    (24u + 4u + 36u + 28u + 20u + 28u)
static uint32_t boot_words[BOOT_WORDS_N] = {0};
/* RAM offset, words: the engine, the 0x180 vector, the reinstall stub or the borrow
 * gate, and in borrowed mode the monitor's scratch (0x010), its helper (0x130) and
 * its data plus the pre-install check (0x190..0x200) */
static const uint32_t boot_ranges[6][2] = {{0x090u, 24u}, {0x180u, 4u}, {0x360u, 36u},
                                           {0x010u, 28u}, {0x130u, 20u}, {0x190u, 28u}};
static void boot_words_copy(uint32_t to_ram) {
    uint32_t k = 0;
    /* The engine's watchpoint covers 0x180..0x187, and a store there from inside an
     * exception is not ignored but DEFERRED (Cause.WP): the watch exception fires the
     * moment the eret lands in the loaded world, with the game's EPC, and the engine
     * then decodes the game's instruction as the vector store and overwrites whichever
     * register it names with 0x80000120 (DK64: `slt at, t2, s4` at the resume address,
     * t2 clobbered, the game spun with interrupts off; a resume at `sll t8, t9, 6`,
     * rs = zero, survived by luck). Disarmed around the writes, and Cause.WP cleared
     * afterwards in case anything else deferred one. The watchpoint covers loads too
     * (games that copy the vector before chaining to it), so the
     * reads of a save are treated the same. */
    uint32_t watch = c0_watchlo();
    c0_set_watchlo(0);
    for (uint32_t r = 0; r < 6u; r++) {
        vu32 *p = (vu32 *)(0xA0000000u + boot_ranges[r][0]);
        for (uint32_t i = 0; i < boot_ranges[r][1]; i++, k++) {
            if (to_ram) {
                p[i] = boot_words[k];
            } else {
                boot_words[k] = p[i];
            }
        }
    }
    c0_set_cause(c0_cause() & ~(1u << 22));       /* WP: a deferred watch exception */
    c0_set_watchlo(watch);
}

/* Reads the slot header and, when it belongs to this ROM, replaces RAM with
 * the image (past that point a failure leaves the game half replaced). */
static uint32_t state_do_load_body(void) {
    struct state_hdr *h = &st_hdr;
    uint32_t crc1 = 0, crc2 = 0;
    dcache_writeback_all();
    if (!pi_dma((uint32_t)(uintptr_t)h, st_slot + STATE_HDR_OFF, STATE_HDR_LEN, 0u)) {
        return ST_DMA_FAIL;
    }
    pio_read(0xB0000010u, &crc1);
    pio_read(0xB0000014u, &crc2);
    uint32_t st = hdr_status(h, crc1, crc2);
    if (st != ST_OK) {
        /* nothing usable in SDRAM (power cycle, other ROM): the SD file may have it */
        if (!sd_read_slot(st_slot)) {
            return st;
        }
        dcache_writeback_all();
        if (!pi_dma((uint32_t)(uintptr_t)h, st_slot + STATE_HDR_OFF, STATE_HDR_LEN, 0u)) {
            return ST_DMA_FAIL;
        }
        st = hdr_status(h, crc1, crc2);
        if (st != ST_OK) {
            return st;
        }
    }
    dcache_writeback_all();
    boot_words_copy(0);           /* this boot's engine entry words, put back after the copy */
    uint32_t ilen = h->image_len, ioff = h->image_off;
    if (ilen > image_len_max()) {
        ilen = image_len_max();       /* a full-RAM state on the resident hook: the top 256 KiB (the hook's
                                       * home and the engine's) stays as the running game has it */
    }
    /* the displayed buffer is restored last, so whatever is on screen (the panel, or the
     * frame the quick combo was pressed on) holds until the loaded moment appears */
    uint32_t ds, dl;
    ov_disp_range(&ds, &dl);
    if (borrowed_mode()) {
        /* the region under the hook goes to the stash (the epilogue puts it in RAM);
         * writing the stash needs the cart writable */
        if (!rom_write_set(1u) || !borrow_image_copy(st_slot, ioff, ilen, 0u)) {
            return ST_DMA_FAIL;
        }
    } else {
        for (uint32_t pass = 0; pass < 2u; pass++) {
            for (uint32_t off = 0; off < ilen; off += STATE_CHUNK) {
                uint32_t n = ((ilen - off) < STATE_CHUNK) ? (ilen - off) : STATE_CHUNK;
                uint32_t overlaps = dl && (off < ds + dl) && (ds < off + n);
                if (overlaps != pass) continue;
                if (!pi_dma(STATE_IMAGE_BASE + off, st_slot + ioff + off, n, 0u)) {
                    return ST_DMA_FAIL;
                }
            }
        }
    }
    boot_words_copy(1u);
    if (borrowed_mode()) {
        borrow_diag_words(0);         /* the loaded world's words under the stage marks (after the boot words went back) */
    }
    /* v10: the RSP's memories and PC as the saved world had them (older states carry none:
     * the RSP keeps this boot's, right for every moment but a task loaded and not started) */
    if (h->regions_n && (h->regions[0].kind == REGION_RSP) && (h->regions[0].len == STATE_RSP_LEN) &&
        ((h->regions[0].off + STATE_RSP_LEN) <= (slot_len_cur() - 0x1000u))) {
        if (!rsp_mem_load(st_slot + h->regions[0].off)) {
            return ST_DMA_FAIL;
        }
        SP_PC_REG = h->regions[0].arg;
    }
    icache_invalidate_all();
    return ST_OK;
}

static uint32_t state_do_load(void) {
    uint32_t r = state_do_load_body();
    vpak_tramp_apply(1u);         /* the site holds the game's words again (or still ours, if the copy stopped short) */
    return r;
}

/* The bits that live outside RAM, then the pending interrupts of the world
 * being replaced. */
static void state_finish_load(void) {
    struct state_hdr *h = &st_hdr;
    uint32_t cur = MI_INTR_MASK & 0x3Fu;
    uint32_t w = 0;
    for (uint32_t i = 0; i < 6u; i++) {
        uint32_t want = (h->mi_mask >> i) & 1u;
        uint32_t have = (cur >> i) & 1u;
        if (want && !have) {
            w |= 2u << (2u * i);
        }
        if (!want && have) {
            w |= 1u << (2u * i);
        }
    }
    if (w) {
        MI_INTR_MASK = w;
    }
    SP_STATUS = SP_CLR_INTR;
    MI_INIT_MODE = MI_MODE_CLR_DP;
    PI_STATUS = PI_STATUS_W_CLR_INTR;
    SI_STATUS = 0;
    AI_STATUS = 0;
    if (h->reraise_sp != 0u) {
        VI_CURRENT = 0;               /* an SP/DP moment: no VI event belongs to it */
    }
    /* a VI moment keeps the VI interrupt pending (nothing cleared it during the
     * freeze, and vi_frz_end ends at the next one): the restored VI manager runs
     * at once and aims the field, instead of a field later with stale timing */
    if (h->reraise_sp == 1u) {
        SP_STATUS = SP_SET_INTR;
    } else if (h->reraise_sp == 2u) {
        /* the snapshot was taken on the RDP-done interrupt: have the idle RDP retire a
         * full sync from RDRAM so the same interrupt arrives in the restored world */
        __asm__ volatile("cache 0x19, 0(%0)" : : "r"(st_sync_cmd) : "memory");   /* Hit_Writeback_D */
        DPC_STATUS = DPC_CLR_XBUS;
        DPC_START = (uint32_t)(uintptr_t)st_sync_cmd & 0x1FFFFFFFu;
        DPC_END = ((uint32_t)(uintptr_t)st_sync_cmd & 0x1FFFFFFFu) + 8u;
    }
    pif_poll_block_send();            /* the PIF holds this boot's last command, not the loaded world's */
    if (h->hook_version >= 7u) {
        /* the restored world may be between writing its DMA address and its length */
        PI_DRAM_ADDR = h->reserved[0];
        PI_CART_ADDR = h->reserved[1];
        SI_DRAM_ADDR = h->reserved[2];
        if (h->reserved[3] || h->reserved[4]) {
            /* RDP: the saved world's freeze and command-source bits. A re-raised
             * full sync above runs with the RDP unfrozen; wait for it, then set
             * the bits the restored scheduler believes are in effect. */
            uint32_t t0 = c0_count();
            while ((DPC_STATUS & (DPC_PIPE_BUSY | DPC_CMD_BUSY | DPC_DMA_BUSY)) && ((c0_count() - t0) < 46875u * 20u)) {
            }
            uint32_t dpc = h->reserved[3];
            DPC_STATUS = (dpc & 2u) ? 8u : 4u;         /* freeze: set (bit 3) / clear (bit 2) */
            DPC_STATUS = (dpc & 1u) ? 2u : 1u;         /* xbus:   set (bit 1) / clear (bit 0) */
            /* RSP: signals 0..7 (read bits 7..14 -> write clear/set pairs from bit 9),
             * interrupt-on-break, and a stale 'broke' flag of the replaced world */
            uint32_t sp = h->reserved[4];
            uint32_t w = 0;
            for (uint32_t i = 0; i < 8u; i++) {
                w |= ((sp >> (7u + i)) & 1u) ? (2u << (9u + 2u * i)) : (1u << (9u + 2u * i));
            }
            w |= (sp & 0x40u) ? (1u << 8) : (1u << 7);  /* intr on break: set / clear */
            if (!(sp & 0x2u) && (SP_STATUS & 0x2u)) {
                w |= 1u << 2;                           /* clear broke */
            }
            SP_STATUS = w;
        }
    }
    if ((h->hook_version >= 9u) && !(h->reserved[3] & (DPC_PIPE_BUSY | DPC_CMD_BUSY | DPC_DMA_BUSY)) &&
        (h->rcp[2] == h->rcp[1])) {
        /* v9: the RDP's command pointers. The saved world's RDP was idle at rcp[1]; a
         * gfx task yielded for audio resumes its command ring from there and only ever
         * writes DPC_END, so the RDP must sit where the saved world left it, not where
         * this boot's last list ended (Jet Force Gemini: the first task after a load
         * hung on the command bus, and its watchdog halted the game).
         * Not with an empty list (START then END, equal): that kick leaves PIPE_BUSY
         * set until the next full sync, and a game that spins on the busy bits before
         * its next frame never gets there (Wave Race 64, Rayman 2). The
         * saved list's last command is that full sync in nearly every game: run it
         * again in place, which ends at rcp[1] with the pipe idle (the interrupt it
         * raises is this run's, cleared below, unless the moment was the RDP's own). */
        uint32_t dpc0 = h->reserved[3];
        uint32_t last = h->rcp[1] - 8u;
        uint32_t cmd = 0;
        if (dpc0 & 1u) {                          /* xbus: the ring is in DMEM (Jet Force Gemini) */
            if ((h->rcp[1] >= 8u) && (h->rcp[1] <= 0x1000u)) {
                cmd = *(vu32 *)(0xA4000000u + last);   /* v10 put the saved DMEM back already */
            }
        } else if ((h->rcp[1] >= 8u) && (h->rcp[1] <= h->memsize)) {
            cmd = *(vu32 *)(0xA0000000u | last);
        }
        if ((cmd >> 24) == 0xE9u) {
            /* a scheduler that keeps the RDP frozen between frames (Rare) had it frozen
             * at the moment and the freeze went back in above: run the sync unfrozen,
             * now, and freeze again after (a kick latched under the freeze ran at the
             * game's next unfreeze and its interrupt arrived unasked: Banjo-Tooie's
             * next list stalled on the command bus) */
            if (dpc0 & 2u) {
                DPC_STATUS = 4u;
            }
            DPC_START = last;
            DPC_END = h->rcp[1];
            uint32_t t0 = c0_count();
            while ((DPC_STATUS & (DPC_PIPE_BUSY | DPC_CMD_BUSY | DPC_DMA_BUSY)) && ((c0_count() - t0) < 46875u * 20u)) {
            }
            if (h->reraise_sp != 2u) {
                MI_INIT_MODE = MI_MODE_CLR_DP;
            }
            if (dpc0 & 2u) {
                DPC_STATUS = 8u;
            }
        }
        /* No full sync there: the pointers are stale (Rayman 2's pointed into a
         * framebuffer) and the empty kick would only leave the pipe busy: the RDP
         * keeps this boot's position. */
    }
    /* The feedback text is drawn into the framebuffers the hook has seen. Those
     * belong to the world being replaced: after a load from GoldenEye's main menu
     * into a mission they pointed at mission data, and the text landed there
     * (the crash's bad address was two pixel values). Start over from the loaded
     * world's own displayed buffer. */
    /* The VI registers describe the replaced world's video mode until its VI
     * manager runs; a mission loaded from GoldenEye's main menu (a different
     * mode) had its first frame shown through the menu's geometry and the
     * feedback text rasterised with the wrong stride, straight into game data.
     * Put the loaded world's VI setup in place now (VI_CURRENT stays cleared). */
    /* Only the mode, the displayed buffer, the width and the x scale: the timing
     * registers alternate per field in interlaced modes (Turok 2's hi-res) and
     * restoring the saved field's values shifted the picture for one frame. */
    *(vu32 *)(VI_BASE + 0x00u) = h->vi[0];
    *(vu32 *)(VI_BASE + 0x04u) = h->vi[1];
    *(vu32 *)(VI_BASE + 0x08u) = h->vi[2];
    *(vu32 *)(VI_BASE + 0x30u) = h->vi[12];
    feedback_reset(h->vi[1] & 0x00FFFFFFu, h->vi[2]);
    /* The AI is the one RCP unit whose pending work is not re-raised: the load
     * takes ~1.6 s, the audio buffers run dry meanwhile and the completion
     * interrupt fired into the world being replaced (and was cleared above).
     * Audio engines that refill on that interrupt (Turok 2) then stay silent
     * for good. If the AI is idle now, play 8 bytes of silence so a fresh
     * interrupt reaches the restored world right after the resume; drivers
     * paced by the VI simply ignore it. */
    if (!(AI_STATUS & 0xC0000000u)) {
        __asm__ volatile("cache 0x19, 0(%0)" : : "r"(st_ai_silence) : "memory");   /* Hit_Writeback_D */
        AI_DRAM_ADDR = (uint32_t)(uintptr_t)st_ai_silence & 0x1FFFFFFFu;
        AI_LEN = 8u;
    }
    if ((h->hook_version >= 7u) && h->reserved[5]) {
        /* continue the saved world's clock: without this the game's next frame
         * sees the whole load (and everything since the save) as elapsed time
         * (GoldenEye's camera lurched, interpolations snapped) */
        c0_set_count(h->reserved[5]);
        c0_set_compare(h->reserved[5] + h->compare_delta);
    } else {
        c0_set_compare(c0_count() + h->compare_delta);
    }
    tlb_save(&st_live);
    uint32_t same = 1;
    for (uint32_t i = 0; i < 32u; i++) {
        for (uint32_t j = 0; j < 4u; j++) {
            if (st_live.tlb[i][j] != h->ctx.tlb[i][j]) {
                same = 0;
            }
        }
    }
    if (same) {
        c0_set_entryhi(h->ctx.entryhi);
    } else {
        tlb_restore(&h->ctx);
    }
}

/* The resumed state's "resume on the next launch" mark comes off: the header on the cart,
 * then that header's sectors of the file (the image is untouched). Torn, the slot reads
 * as empty from the card, a window of a few milliseconds. */
static void state_resume_flag_clear(void) {
    /* unconditional: the mark the menu read is the file's, and the slot's copy of the
     * header can already be clear (an earlier clear whose head mirror was lost, or a
     * state loaded from the slot while the file still carries the mark): the header
     * goes out either way, or every launch would resume again */
    st_hdr.flags &= ~4u;
    st_hdr.checksum = 0;
    st_hdr.checksum = hdr_checksum(&st_hdr);
    dcache_writeback_all();
    if (rom_write_set(1u)) {
        pi_dma((uint32_t)(uintptr_t)&st_hdr, st_slot + STATE_HDR_OFF, STATE_HDR_LEN, 1u);
        rom_write_set(0);
    }
    sd_write_begin_head(st_slot);
    crumb(18u, st_slot, sd_state);
}

static void state_done(uint32_t status) {
    vi_frz_end();                 /* every freeze ends here or just before state_finish_load */
    st_last_op = st_pending;
    st_last_status = status;
    st_last_dma = st_dma_ticks;
    st_last_wait = st_wait;
    st_pending = 0;
}

/* Every exception: a pending save/load takes the first clean RCP interrupt. */
static void state_service(uint32_t cause) {
    if (!st_pending) {
        return;
    }
    st_dbg[0]++;
    if ((cause & CAUSE_EXC_MASK) != 0) {
        st_dbg[1]++;
        return;                                   /* not an interrupt */
    }
    uint32_t sr = c0_status();
    uint32_t ip = cause & 0xFF00u;
    if (!(sr & (1u << 15))) {
        ip &= ~(1u << 15);                        /* a masked Compare match: nobody is waiting for it */
    }
    if (borrowed_mode()) {
        ip &= ~(1u << 15);                        /* the monitor saw none at its moment; one that matured
                                                   * during the borrow's DMAs is the frozen world's future */
    }
    st_dbg[7] = cause;
    st_dbg[8] = sr;
    if (ip != CAUSE_IP2_RCP) {
        st_dbg[2]++;
        return;                                   /* only the RCP line, nothing else pending */
    }
    /* borrowed mode: the moment is the monitor's, from before its stash and hook DMAs
     * (tens of milliseconds, in which the next VI and the AI's buffer swaps arrive: the
     * live bits would never again read as one interrupt, and every borrow timed out
     * unless it began on a VI with no audio swap due) */
    uint32_t mi = borrowed_mode() ? borrow_mi : (MI_INTERRUPT & 0x3Fu);
    st_dbg[9] = mi;
    if ((mi != MI_INTR_VI) && (mi != MI_INTR_SP) && (mi != MI_INTR_DP)) {
        st_dbg[3]++;
        return;
    }
    if (mi == MI_INTR_VI) {
        st_wait++;
    }
    uint32_t sp = SP_STATUS;
    uint32_t dp = DPC_STATUS;
    st_dbg[10] = sp;
    st_dbg[11] = dp;
    uint32_t bad = 0;
    if (!(sp & SP_HALT) || (sp & (SP_DMA_BUSY | SP_DMA_FULL))) {
        st_dbg[4]++;
        bad = 1;
    }
    if (dp & (DPC_PIPE_BUSY | DPC_CMD_BUSY | DPC_DMA_BUSY)) {
        st_dbg[5]++;
        bad = 1;
    }
    if ((PI_STATUS & 3u) || (SI_STATUS & 3u)) {
        st_dbg[6]++;
        bad = 1;
    }
    if (snap_active) {
        bad = 1;
    }
    if (bad) {
        if (st_wait > STATE_WAIT_MAX) {
            state_done(ST_TIMEOUT);
        }
        return;
    }
    if ((mi != MI_INTR_VI) && (st_wait < STATE_PREFER_VI) && !borrowed_mode()) {
        /* (borrowed mode: the monitor already held out for a frame boundary before it
         * entered the hook on an SP/DP-done moment; frozen here, no VI would come) */
        /* A clean SP/DP-done moment is mid-pipeline: schedulers that freeze and
         * unfreeze the RDP across frames (Banjo-Tooie) keep state in RDP registers
         * we do not capture, and a load there deadlocked at a boss cutscene. Hold
         * out for a VI interrupt with the RCP idle first; games whose RDP is busy
         * at every VI (SM64's castle grounds) still get the fallback after
         * STATE_PREFER_VI frames. */
        return;
    }
    st_dma_ticks = 0;
    vi_frz_begin(mi == MI_INTR_VI);
    if (st_pending != STATE_OP_LOAD) {
        rom_write_set(1u);            /* the state, thumbnail and frame copies go to the cart */
    }
    crumb(2u, st_pending, mi);
    if (st_pending == STATE_OP_MENU) {
        if (menu_run(cause, mi) != 1u) {
            state_done(ST_OK);            /* closed, or saved from inside: back to the game */
            return;
        }
        /* a load was chosen: st_slot is set, fall through into the load path */
    } else if (st_pending == STATE_OP_SAVE) {
        thumb_capture();
        uint32_t saved = state_do_save(cause, mi);
        crumb(3u, saved, st_slot);
        if (saved == ST_OK) {
            rom_write_set(0);             /* before the mirror: the cart is busy with that write for
                                           * longer than a command's timeout, so a later "off" is lost */
            if (!sd_write_begin(st_slot) && (sd_state == 1u)) {
                sd_pending_slot = st_slot;    /* the pak's mirror is streaming: this one follows it */
            }
            feedback_show("STATE SAVED");
        } else {
            feedback_show("SAVE FAILED");
        }
        state_done(saved);
        return;
    }
    uint32_t status = state_do_load();
    if (status != ST_OK) {
        state_done(status);       /* after the header check this leaves RAM half replaced; the PC at least learns why */
        return;
    }
    if (resume_pending) {
        resume_pending = 0;
        state_resume_flag_clear();
    }
    vi_frz_end();                 /* at a field start, before the loaded world's VI setup goes in */
    state_finish_load();
    crumb(11u, st_hdr.ctx.epc, st_hdr.ctx.status);
    feedback_show("STATE LOADED");
    state_done(ST_OK);            /* the reply goes out before the world changes */
    TR_STAGE = 7;
    reentry = 0;
    if ((st_hdr.hook_version >= 7u) && st_hdr.reserved[5]) {
        /* The saved world's clock, once more and last: state_do_load_body set it, then
         * vi_frz_end put the freeze's own clock back (the present one), so the game's
         * next frame saw everything since the save as elapsed time. Episode I Racer
         * integrates its physics over that delta: after a load the pod exploded or
         * came down from the sky). Borrowed mode adds the
         * epilogue's region copy after this, a few frames at most. */
        c0_set_count(st_hdr.reserved[5]);
        c0_set_compare(st_hdr.reserved[5] + st_hdr.compare_delta);
    }
    if (borrowed_mode()) {
        borrow_pc_ack(ST_OK);     /* a load the PC asked for: answered before the world changes */
        borrow_load_exit();       /* the region under us is the stash's: the monitor puts it back, then erets */
    }
    state_resume(&st_hdr.ctx);
}


/* ---- Controller trigger (untethered save/load) ------------------------------
 * The game polls the controllers by DMAing a 64-byte joybus block between its
 * RAM buffer and PIF RAM. The SI keeps the RDRAM address of its last DMA in
 * SI_DRAM_ADDR, so that buffer can be found for any game without knowing
 * where libultra put it. Once per VI tick the block is read from RDRAM
 * (uncached, so the DMA's bytes are seen), channel 0's button word is taken,
 * and a save/load is queued when a combo has been held combo_hold ticks.
 * PIF RAM itself is never touched: a PIO read there while the PIF is in the
 * middle of a transaction wedges the SI and the game's controller read never
 * completes (seen twice on hardware). Before the PIF has answered, the block
 * still holds libultra's 0xFF fill bytes, which read as "every button": that
 * value is rejected. */

/* Boot-time configuration block. The menu fork patches it inside the staged
 * blob (hook_blob.h carries its offset) with the slot table it computes from
 * the ROM size; the PC can rewrite the trigger part over the mailbox. The
 * defaults suit an 8 MiB ROM (SM64). Not static so nm reports it. */
#define CFG_MAGIC       0x43464731u /* "CFG1" */
#define CFG_SLOTS_MAX   8u
struct hook_cfg {
    uint32_t magic;
    uint32_t slots_n;
    uint32_t slots[CFG_SLOTS_MAX];   /* cart PI addresses of 8 MiB state slots (0 = none) */
    uint32_t combo_save;
    uint32_t combo_load;
    uint32_t combo_hold;             /* ticks a combo must be held */
    uint32_t combo_enabled;
    uint32_t cur_slot;               /* slot the combos use */
    uint32_t rom_size;               /* informational, set by the menu */
    uint32_t sd_sectors[CFG_SLOTS_MAX]; /* sectors in the slot's SD file table (0 = no file) */
    uint32_t combo_menu;             /* opens the overlay */
    uint32_t image_len;              /* 0 = whole image, 0x400000 = the game never uses the Expansion Pak */
    uint32_t feedback;               /* on-screen "STATE SAVED/LOADED" after the quick combos */
    uint32_t spare;
    uint32_t dev_watch[4];           /* development: RAM words the monitor's heartbeat copies every tick
                                      * (0 = none); the PC sets them in the staged copy on the cart */
    uint32_t pc_req;                 /* a request from a PC over USB, in the staged copy: op in
                                      * the low byte (1 save, 2 load, 3 panel, 5 screenshot), a sequence in
                                      * the high half; the monitor arms it, the hook clears it and answers */
    uint32_t pc_ack;                 /* sequence << 16 | 0x100 | status of the last request */
    uint32_t pc_shot;                /* where the last screenshot went (cart PI address; 0 = none) */
    uint32_t pc_pad;                 /* the PC's pad, over channel 0's answer: buttons << 16 | x << 8 | y ... */
    uint32_t pc_padn;                /* ... for these polls: sequence << 16 | polls (monitor.S keeps the count) */
    uint32_t pc_spare[3];
};
struct hook_cfg hook_cfg __attribute__((aligned(16))) = {
    CFG_MAGIC, 7u, {0x10800000u, 0x10FC8000u, 0x11790000u, 0x11F58000u, 0x12720000u, 0x12EE8000u, 0x136B0000u, 0u},
    0x0830u /* L + R + D-pad up */, 0x0430u /* L + R + D-pad down */, 12u, 1u, 0u, 0u, {0},
    0x1030u /* L + R + Start */, 0u, 1u, 0u, {0u, 0u, 0u, 0u}, 0u, 0u, 0u, 0u, 0u, {0u, 0u, 0u}};
#define CFG_WORDS       40u
_Static_assert(sizeof(struct hook_cfg) == CFG_WORDS * 4u, "CFG_WORDS");
/* monitor.S reads these fields from the staged copy by offset */
_Static_assert(__builtin_offsetof(struct hook_cfg, combo_save) == 0x28u, "monitor.S: combo_save");
_Static_assert(__builtin_offsetof(struct hook_cfg, combo_load) == 0x2Cu, "monitor.S: combo_load");
_Static_assert(__builtin_offsetof(struct hook_cfg, combo_hold) == 0x30u, "monitor.S: combo_hold");
_Static_assert(__builtin_offsetof(struct hook_cfg, combo_enabled) == 0x34u, "monitor.S: combo_enabled");
_Static_assert(__builtin_offsetof(struct hook_cfg, combo_menu) == 0x60u, "monitor.S: combo_menu");
_Static_assert(__builtin_offsetof(struct hook_cfg, dev_watch) == 0x70u, "monitor.S: dev_watch");
_Static_assert(__builtin_offsetof(struct hook_cfg, pc_req) == 0x80u, "monitor.S: pc_req");
_Static_assert(__builtin_offsetof(struct hook_cfg, pc_pad) == 0x8Cu, "monitor.S: pc_pad");
_Static_assert(__builtin_offsetof(struct hook_cfg, pc_padn) == 0x90u, "monitor.S: pc_padn");
_Static_assert(__builtin_offsetof(struct hook_cfg, spare) == 0x6Cu, "monitor.S: spare");
static uint32_t borrowed_mode(void) {
    return (hook_cfg.spare & 0x20u) ? 1u : 0u;   /* hook_cfg.spare bit 5: no resident hook, the monitor borrows */
}
static uint32_t image_len_max(void) {
    return borrowed_mode() ? STATE_IMAGE_LEN_B : STATE_IMAGE_LEN;
}
static uint32_t slot_len_cur(void) {
    /* one slot layout for both placements (the borrowed one, the larger):
     * the menu's slot table and the SD files are the same whichever way a game boots, so
     * the Slow motion option (the resident hook) can be switched without losing a state */
    return STATE_SLOT_LEN_B;
}
static uint32_t image_len(void) {
    if (borrowed_mode()) return STATE_IMAGE_LEN_B;
    return (hook_cfg.image_len == 0x400000u) ? 0x400000u : STATE_IMAGE_LEN;
}
static uint32_t pad_buttons = 0;
static uint32_t pad_valid = 0;
static uint32_t combo_cnt_save = 0;
static uint32_t combo_cnt_load = 0;
static uint32_t combo_cnt_menu = 0;

static inline uint32_t pif_byte(const uint32_t *w, uint32_t i) {
    return (w[i >> 2] >> (24u - 8u * (i & 3u))) & 0xFFu;
}

static uint32_t pad_buf = 0;            /* RDRAM address of the game's joybus block (diagnostic) */
static uint32_t pad_btn_addr = 0;       /* RDRAM address of channel 0's button word in that block */

/* Parse the joybus block at RDRAM address `base`; 1 when channel 0's buttons were taken. */
static uint32_t pad_parse(uint32_t base) {
    if ((base == 0) || (base & 7u) || (base > 0x007FFFC0u)) {
        return 0;
    }
    const vu32 *src = (const vu32 *)(0xA0000000u | base);
    uint32_t w[16];
    for (uint32_t i = 0; i < 16u; i++) {
        w[i] = src[i];
    }
    uint32_t i = 0, ch = 0;
    while (i < 63u) {
        uint32_t t = pif_byte(w, i);
        if (t == 0xFEu) {
            break;                       /* end of the block */
        }
        if (t == 0xFFu) {
            i++;                         /* padding */
            continue;
        }
        if ((t == 0x00u) || (t == 0xFDu)) {
            ch++;                        /* channel skipped / reset */
            i++;
            continue;
        }
        uint32_t tx = t & 0x3Fu;
        uint32_t rxb = pif_byte(w, i + 1u);
        uint32_t rx = rxb & 0x3Fu;
        uint32_t cmd = pif_byte(w, i + 2u);
        if ((ch == 0u) && (cmd == 0x01u) && (rx >= 4u) && ((i + 4u) < 64u)) {
            if (rxb & 0xC0u) {
                return 1;                /* no controller / overrun: keep the last value */
            }
            uint32_t b = (pif_byte(w, i + 3u) << 8) | pif_byte(w, i + 4u);
            if (b == 0xFFFFu) {
                return 1;                /* libultra's fill bytes: the PIF has not answered this block yet */
            }
            pad_buttons = b;
            pad_valid = 1;
            pad_buf = 0x80000000u | base;
            pad_btn_addr = 0xA0000000u | (base + i + 3u);
            return 1;
        }
        i += 2u + tx + rx;          /* the command byte is counted in tx */
        ch++;
    }
    return 0;
}

/* Every SI-transfer completion: the game's block is fresh, read it before libultra
 * hands it to the game. When a combo is fully held, its buttons are cleared in the
 * block so the game never reacts to them (no pause on Start, no crouch on Z). */
static uint32_t pad_combo_held(void) {
    uint32_t m;
    m = hook_cfg.combo_save;
    if (m && ((pad_buttons & m) == m)) return 1;
    m = hook_cfg.combo_load;
    if (m && ((pad_buttons & m) == m)) return 1;
    m = hook_cfg.combo_menu;
    if (m && ((pad_buttons & m) == m)) return 1;
    return 0;
}

static void pad_si_tick(void) {
    if (!hook_cfg.combo_enabled && !(hook_cfg.spare & 4u)) {
        return;
    }
    /* After a 64-byte transfer the SI's address register holds the address of the
     * last 8-byte unit (start + 0x38). Try the block that ends there first. */
    uint32_t a = SI_DRAM_ADDR & 0x00FFFFFFu;
    if ((hook_cfg.spare & 4u) && vpak_loaded && (a >= 0x38u) && (tramp_state != 1u)) {
        vpak_serve(a - 0x38u);       /* the virtual Controller Pak answers channel 0's accessory commands
                                      * (a fallback: with the game's handler patched, it serves from there) */
    }
    if (!hook_cfg.combo_enabled) {
        return;
    }
    uint32_t got = ((a >= 0x38u) && pad_parse(a - 0x38u)) || pad_parse(a);
    if (got && pad_btn_addr) {
        if (pad_combo_held()) {
            *(volatile uint8_t *)pad_btn_addr = 0;
            *(volatile uint8_t *)(pad_btn_addr + 1u) = 0;
        } else if (speed_div == SPEED_STEP) {
            *(volatile uint8_t *)pad_btn_addr &= (uint8_t)~0x20u;   /* Z advances frames; the game never sees it */
        }
    }
}

/* Console-side request (combo or, later, the overlay): no mailbox reply. */
static void state_queue(uint32_t op) {
    if (st_pending || snap_active || (sd_state == 1u)) {
        return;
    }
    if ((op != STATE_OP_LOAD) && (dbg_unlock_ok == 0)) {
        return;                       /* the cart has never answered */
    }
    if ((hook_cfg.cur_slot >= hook_cfg.slots_n) || (hook_cfg.slots[hook_cfg.cur_slot] == 0)) {
        return;                      /* no slot for this ROM (or none configured) */
    }
    st_slot = hook_cfg.slots[hook_cfg.cur_slot];
    crumb(1u, op, pad_buttons);
    st_seq = 0;
    st_wait = 0;
    st_dma_ticks = 0;
    for (uint32_t i = 0; i < 12u; i++) {
        st_dbg[i] = 0;
    }
    st_pending = op;
}

static void combo_service(void) {
    if (!hook_cfg.combo_enabled || !pad_valid) {
        return;
    }
    if (hook_cfg.combo_save && ((pad_buttons & hook_cfg.combo_save) == hook_cfg.combo_save)) {
        if (++combo_cnt_save == hook_cfg.combo_hold) {
            state_queue(STATE_OP_SAVE);
        }
    } else {
        combo_cnt_save = 0;
    }
    if (hook_cfg.combo_load && ((pad_buttons & hook_cfg.combo_load) == hook_cfg.combo_load)) {
        if (++combo_cnt_load == hook_cfg.combo_hold) {
            state_queue(STATE_OP_LOAD);
        }
    } else {
        combo_cnt_load = 0;
    }
    if (hook_cfg.combo_menu && ((pad_buttons & hook_cfg.combo_menu) == hook_cfg.combo_menu)) {
        if (++combo_cnt_menu == hook_cfg.combo_hold) {
            state_queue(STATE_OP_MENU);
        }
    } else {
        combo_cnt_menu = 0;
    }
}


/* ---- SD persistence ----------------------------------------------------------
 * Each slot may have a file on the SD card. The menu allocates it once (8 MiB,
 * marker "STFR" + ROM CRC + slot in its first sector) and, at every launch,
 * writes the file's run table (its contiguous sector extents) into the last
 * 4 KiB of the slot, outside the mirrored range. A save is mirrored to the file
 * right after the SDRAM copy, asynchronously (the MCU streams SDRAM -> SD while
 * the game runs), in this order: a zeroed sector over the file's header, the
 * image, the rest of the head, the header last; a power cut in the middle leaves
 * a file that reads as empty rather than as a plausible corrupt state. A load
 * whose SDRAM slot holds no valid state reads the file back in first. Nothing is
 * ever written before the file's first sector has been read and found to carry
 * our marker or a state of this ROM, so a wrong table cannot touch anything
 * else on the card. */
#define SD_TABLE_OFF        0x007C7000u    /* STATE_SLOT_LEN - 0x1000: the run table (menu-written) */
#define SD_RUNS_MAGIC       0x53445231u    /* "SDR1", then n, total sectors, n x {sector, file_sector, count} */
#define SD_RUNS_MAX         64u
#define SD_FILE_SECTORS     0x3E30u        /* sectors of a normal state file: head (16 KiB) + image (7.75 MiB) + the RSP's memories (8 KiB) */
#define SD_MAGIC_FREE       0x53544652u    /* "STFR" */
#define SC64_CMD_SD_CARD_OP 0x69u          /* 'i' */
#define SC64_CMD_SD_SECTOR_SET 0x49u       /* 'I' */
#define SC64_CMD_SD_READ    0x73u          /* 's' */
#define SC64_CMD_SD_WRITE   0x53u          /* 'S' */
#define SD_OP_INIT          1u
#define SD_INIT_TIMEOUT_TICKS (46875u * 3000u)  /* 3 s */
#define SD_XFER_TIMEOUT_TICKS (46875u * 8000u)  /* 8 s */
#define SD_BRAM_SECTOR      (BRAM_BASE + 0x1000u)   /* scratch sector in the data buffer */
#define SD_BRAM_SECTOR_PI   0x1FFE1000u
#define ST_SD_FAIL          11u
_Static_assert(SD_TABLE_OFF == STATE_SLOT_LEN - 0x1000u, "the run table sits in the slot's last 4 KiB");
_Static_assert(SD_FILE_SECTORS == (STATE_IMAGE_OFF + STATE_IMAGE_LEN + STATE_RSP_LEN) / 512u, "normal file size");
#define SD_TABLE_OFF_B      0x0080F000u    /* borrowed mode: STATE_SLOT_LEN_B - 0x1000 */
#define SD_FILE_SECTORS_B   0x4030u        /* borrowed mode: head (16 KiB) + image (8 MiB) + the RSP's memories (8 KiB) */
_Static_assert(SD_TABLE_OFF_B == STATE_SLOT_LEN_B - 0x1000u, "the run table sits in the borrowed slot's last 4 KiB");
_Static_assert(SD_FILE_SECTORS_B == (STATE_IMAGE_OFF + STATE_IMAGE_LEN_B + STATE_RSP_LEN) / 512u, "borrowed file size");
static uint32_t sd_table_off(void) {
    return slot_len_cur() - 0x1000u;
}
_Static_assert((STATE_ZERO_OFF + 512u) <= STATE_IMAGE_OFF, "the zero sector lies in the head");

struct sd_run {
    uint32_t sector;        /* first SD sector of the run */
    uint32_t file_sector;   /* file offset in sectors */
    uint32_t count;
};
static struct sd_run sd_runs[SD_RUNS_MAX] = {{0, 0, 0}};
static uint32_t sd_run_n = 0;
static uint32_t sd_total = 0;              /* file sectors the table covers */
/* a mirror writes a list of file segments in order (each a contiguous slot range) */
struct sd_seg {
    uint32_t file_sector;
    uint32_t count;
    uint32_t src;           /* cart PI address of the segment's first sector */
};
static struct sd_seg sd_segs[4] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
static uint32_t sd_seg_n = 0, sd_seg_i = 0, sd_seg_off = 0;
/* The mirror is chunked and paced: one 8 MiB write monopolises the cart's SDRAM and
 * games that stream from the cartridge (Turok 2) stutter for the whole write. */
#define SD_CHUNK_SECTORS 256u              /* 128 KiB per SD write command */
#define SD_GAP_TICKS     1u                /* chunks follow each other on consecutive idle ticks */
static uint32_t sd_chunk_len = 0, sd_gap = 0, sd_inflight = 0;
static uint32_t sd_state = 0;         /* 0 idle, 1 mirror write in flight, 2 last operation failed */
static uint32_t sd_slot = 0;          /* slot (cart PI address) being mirrored */
static uint32_t sd_inited = 0;
static uint32_t sd_last_error = 0;    /* DATA0 << 16 | DATA1 of the failing command, or a local code */
static uint32_t sd_writes_done = 0;
static uint32_t sd_reads_done = 0;
static uint32_t sd_pending_slot = 0;  /* a state mirror waiting behind the pak's */

/* Like sc64_command but with its own timeout and the error words captured. */
static uint32_t sc64_command_long(uint32_t id, uint32_t arg0, uint32_t arg1, uint32_t timeout) {
    uint32_t sr, start, d0 = 0, d1 = 0;
    if (!pio_write(SC64_DATA0, arg0)) return 0;
    if (!pio_write(SC64_DATA1, arg1)) return 0;
    if (!pio_write(SC64_SR_CMD, id & 0xFFu)) return 0;
    start = c0_count();
    for (;;) {
        if (!pio_read(SC64_SR_CMD, &sr)) return 0;
        if (!(sr & SC64_SR_CPU_BUSY)) break;
        vi_frz_service();
        if ((c0_count() - start) > timeout) {
            sd_last_error = 0xFFFF0000u | id;
            return 0;
        }
    }
    if (sr & SC64_SR_CMD_ERROR) {
        pio_read(SC64_DATA0, &d0);
        pio_read(SC64_DATA1, &d1);
        sd_last_error = (d0 << 16) | (d1 & 0xFFFFu);
        return 0;
    }
    return 1;
}

/* Fire a command and return at once; sd_service() collects the result. */
static uint32_t sc64_command_issue(uint32_t id, uint32_t arg0, uint32_t arg1) {
    if (!pio_write(SC64_DATA0, arg0)) return 0;
    if (!pio_write(SC64_DATA1, arg1)) return 0;
    if (!pio_write(SC64_SR_CMD, id & 0xFFu)) return 0;
    return 1;
}

static uint32_t sd_slot_index(uint32_t slot_base) {
    for (uint32_t i = 0; i < hook_cfg.slots_n; i++) {
        if (hook_cfg.slots[i] == slot_base) return i;
    }
    return CFG_SLOTS_MAX;
}

static uint32_t sd_ensure_init(void) {
    if (sd_inited) return 1;
    if (!ensure_unlocked()) return 0;
    if (!sc64_command_long(SC64_CMD_SD_CARD_OP, 0, SD_OP_INIT, SD_INIT_TIMEOUT_TICKS)) return 0;
    sd_inited = 1;
    return 1;
}

/* The run table the menu wrote for the slot's file (PIO reads): the runs must tile
 * the file from sector 0 in order. */
static uint32_t sd_load_runs_at(uint32_t table_pi) {
    uint32_t table = table_pi | 0xA0000000u;
    uint32_t magic = 0, n = 0, total = 0, next = 0;
    sd_run_n = 0;
    sd_total = 0;
    if (!pio_read(table, &magic) || (magic != SD_RUNS_MAGIC) || !pio_read(table + 4u, &n) || !pio_read(table + 8u, &total)) {
        sd_last_error = 0xEEEE0000u;
        return 0;
    }
    if ((n == 0) || (n > SD_RUNS_MAX) || (total == 0)) {
        sd_last_error = 0xEEEF0000u | (n & 0xFFFFu);
        return 0;
    }
    for (uint32_t i = 0; i < n; i++) {
        uint32_t s = 0, f = 0, c = 0;
        if (!pio_read(table + 12u + 12u * i, &s) || !pio_read(table + 16u + 12u * i, &f) || !pio_read(table + 20u + 12u * i, &c)) {
            sd_last_error = 0xEEED0000u | (i & 0xFFFFu);
            return 0;
        }
        if ((s == 0) || (c == 0) || (f != next)) {
            sd_last_error = 0xEEEC0000u | (i & 0xFFFFu);
            return 0;
        }
        sd_runs[i].sector = s;
        sd_runs[i].file_sector = f;
        sd_runs[i].count = c;
        next = f + c;
    }
    sd_run_n = n;
    sd_total = (next < total) ? next : total;
    return n;
}

static uint32_t sd_load_runs(uint32_t slot_base) {
    return sd_load_runs_at(slot_base + sd_table_off());
}

/* the run holding a file sector, or SD_RUNS_MAX */
static uint32_t sd_run_of(uint32_t file_sector) {
    for (uint32_t r = 0; r < sd_run_n; r++) {
        if ((file_sector >= sd_runs[r].file_sector) && (file_sector < sd_runs[r].file_sector + sd_runs[r].count)) {
            return r;
        }
    }
    return SD_RUNS_MAX;
}

static uint32_t sd_file_sector(uint32_t file_sector) {
    uint32_t r = sd_run_of(file_sector);
    return (r < SD_RUNS_MAX) ? (sd_runs[r].sector + (file_sector - sd_runs[r].file_sector)) : 0;
}

/* The file's first sector (the header) into the BRAM scratch sector. */
static uint32_t sd_read_header_sector(void) {
    uint32_t sec = sd_file_sector(0);
    if (sec == 0) return 0;
    if (!sc64_command_long(SC64_CMD_SD_SECTOR_SET, sec, 0, CMD_TIMEOUT_TICKS)) return 0;
    if (!sc64_command_long(SC64_CMD_SD_READ, SD_BRAM_SECTOR_PI, 1u, SD_XFER_TIMEOUT_TICKS)) return 0;
    return 1;
}

/* The file's first sector must carry our fresh-file marker for this ROM and
 * slot, or a state of this ROM. */
static uint32_t sd_verify_file(uint32_t slot_idx) {
    uint32_t w0 = 0, w1 = 0, w2 = 0, w3 = 0, crc1 = 0, crc2 = 0;
    if (!sd_read_header_sector()) return 0;
    pio_read(SD_BRAM_SECTOR + 0x0, &w0);
    pio_read(0xB0000010u, &crc1);
    pio_read(0xB0000014u, &crc2);
    if (w0 == STATE_MAGIC) {
        pio_read(SD_BRAM_SECTOR + 0x14, &w1);
        pio_read(SD_BRAM_SECTOR + 0x18, &w2);
        if ((w1 == crc1) && (w2 == crc2)) return 1;
    } else if (w0 == SD_MAGIC_FREE) {
        pio_read(SD_BRAM_SECTOR + 0x4, &w1);
        pio_read(SD_BRAM_SECTOR + 0x8, &w2);
        pio_read(SD_BRAM_SECTOR + 0xC, &w3);
        if ((w1 == crc1) && (w2 == crc2) && (w3 == slot_idx)) return 1;
    }
    sd_last_error = 0xDDDD0000u | (w0 >> 16);
    return 0;
}

/* One SD_WRITE: the next chunk of the current segment, never across a run boundary. */
static void sd_issue_chunk(void) {
    struct sd_seg *g = &sd_segs[sd_seg_i];
    uint32_t fs = g->file_sector + sd_seg_off;
    uint32_t r = sd_run_of(fs);
    if (r >= SD_RUNS_MAX) {
        sd_last_error = 0xEEEB0000u | (fs & 0xFFFFu);
        sd_state = 2;
        return;
    }
    uint32_t left = g->count - sd_seg_off;
    uint32_t run_left = sd_runs[r].file_sector + sd_runs[r].count - fs;
    sd_chunk_len = left;
    if (sd_chunk_len > run_left) sd_chunk_len = run_left;
    if (sd_chunk_len > SD_CHUNK_SECTORS) sd_chunk_len = SD_CHUNK_SECTORS;
    if (!sc64_command_long(SC64_CMD_SD_SECTOR_SET, sd_runs[r].sector + (fs - sd_runs[r].file_sector), 0, CMD_TIMEOUT_TICKS) ||
        !sc64_command_issue(SC64_CMD_SD_WRITE, g->src + sd_seg_off * 512u, sd_chunk_len)) {
        sd_state = 2;
        return;
    }
    sd_inflight = 1;
}

/* Start mirroring a slot to its file (after a save, or on request). */
static uint32_t sd_write_begin2(uint32_t slot_base, uint32_t head_only) {
    uint32_t idx = sd_slot_index(slot_base), image_off = 0, image_len = 0;
    if ((idx >= CFG_SLOTS_MAX) || (hook_cfg.sd_sectors[idx] == 0)) return 0;   /* no file for this slot */
    if (sd_state == 1) return 0;
    crumb(4u, slot_base, hook_cfg.sd_sectors[idx]);
    if (!sd_ensure_init()) {
        crumb(13u, 5u, sd_last_error);
        sd_state = 2;
        return 0;
    }
    if (!sd_load_runs(slot_base)) {
        crumb(13u, 6u, sd_last_error);
        sd_state = 2;
        return 0;
    }
    /* the slot's header says how much of it is the state */
    uint32_t hb = slot_base | 0xA0000000u;
    if (!pio_read(hb + 0x0Cu, &image_len) || !pio_read(hb + 0x64u, &image_off) ||
        (image_off < STATE_HDR_LEN) || (image_off & 511u) || (image_len == 0)) {
        sd_last_error = 0xCCCC0000u;
        crumb(13u, 14u, sd_last_error);
        sd_state = 2;
        return 0;
    }
    uint32_t head = image_off / 512u, isect = (image_len + 511u) / 512u;
    /* v10 states keep the RSP's memories right after the image (STATE_RSP_LEN): they go to
     * the card with it when the file has the room (every file this release makes has;
     * before this the mirror stopped at the image and a state loaded after a power cycle
     * took the RSP's memories from whatever the cartridge memory still held) */
    if ((head + isect + STATE_RSP_LEN / 512u) <= sd_total) isect += STATE_RSP_LEN / 512u;
    if ((head + isect) > sd_total) {
        sd_last_error = 0xCCCD0000u | ((head + isect) & 0xFFFFu);   /* the file is too small for this state */
        crumb(13u, 15u, sd_last_error);
        sd_state = 2;
        return 0;
    }
    if (!sd_verify_file(idx)) {
        crumb(13u, 7u, sd_last_error);
        sd_state = 2;
        return 0;
    }
    if (head_only) {
        /* the header's sectors over the file's, the rest of the file as it is */
        sd_segs[0].file_sector = 0; sd_segs[0].count = STATE_HDR_LEN / 512u; sd_segs[0].src = slot_base;
        sd_seg_n = 1;
    } else {
        /* the zeroed sector over the file's header first, the image, the rest of the head, the header last */
        sd_segs[0].file_sector = 0;    sd_segs[0].count = 1;        sd_segs[0].src = slot_base + STATE_ZERO_OFF;
        sd_segs[1].file_sector = head; sd_segs[1].count = isect;    sd_segs[1].src = slot_base + image_off;
        sd_segs[2].file_sector = 1;    sd_segs[2].count = head - 1; sd_segs[2].src = slot_base + 512u;
        sd_segs[3].file_sector = 0;    sd_segs[3].count = 1;        sd_segs[3].src = slot_base;
        sd_seg_n = 4;
    }
    sd_pending_slot = 0;
    sd_seg_i = 0;
    sd_seg_off = 0;
    sd_slot = slot_base;
    sd_inflight = 0;
    sd_gap = 0;
    sd_state = 1;
    sd_issue_chunk();
    crumb(8u, sd_state, isect);
    return (sd_state == 1) ? 1u : 0u;
}

static uint32_t sd_write_begin(uint32_t slot_base) {
    return sd_write_begin2(slot_base, 0);
}

static uint32_t sd_write_begin_head(uint32_t slot_base) {
    return sd_write_begin2(slot_base, 1u);
}

/* Every VI tick: collect the in-flight write, start the next chunk. */
static void sd_service(void) {
    uint32_t sr, d0 = 0, d1 = 0;
    if (sd_state != 1) return;
    if (sd_inflight) {
        if (!pio_read(SC64_SR_CMD, &sr)) return;
        if (sr & SC64_SR_CPU_BUSY) return;
        if (sr & SC64_SR_CMD_ERROR) {
            pio_read(SC64_DATA0, &d0);
            pio_read(SC64_DATA1, &d1);
            sd_last_error = (d0 << 16) | (d1 & 0xFFFFu);
            sd_state = 2;
            return;
        }
        sd_inflight = 0;
        sd_seg_off += sd_chunk_len;
        while ((sd_seg_i < sd_seg_n) && (sd_seg_off >= sd_segs[sd_seg_i].count)) {
            sd_seg_i++;
            sd_seg_off = 0;
        }
        if (sd_seg_i >= sd_seg_n) {
            sd_state = 0;
            sd_writes_done++;
            crumb(12u, sd_writes_done, 0);
            rom_write_set(0);             /* in case the "off" at the end of the freeze was lost */
            if (sd_pending_slot) {        /* a state mirror waited behind this one */
                uint32_t slot = sd_pending_slot;
                sd_pending_slot = 0;
                sd_write_begin(slot);
            }
            return;
        }
        sd_gap = 0;
        return;
    }
    if (++sd_gap < SD_GAP_TICKS) return;        /* let the game's cartridge reads through */
    sd_issue_chunk();
}

/* Read a slot's file back into SDRAM (synchronous, the game is frozen meanwhile).
 * 0 when the file holds no state of this ROM. */
static uint32_t sd_read_slot(uint32_t slot_base) {
    uint32_t idx = sd_slot_index(slot_base);
    uint32_t w0 = 0, ver = 0, image_len = 0, image_off = 0, h1 = 0, h2 = 0, crc1 = 0, crc2 = 0;
    if ((idx >= CFG_SLOTS_MAX) || (hook_cfg.sd_sectors[idx] == 0)) return 0;
    if (sd_state == 1) return 0;
    crumb(9u, slot_base, hook_cfg.sd_sectors[idx]);
    if (!sd_ensure_init() || !sd_load_runs(slot_base) || !sd_read_header_sector()) {
        crumb(13u, 9u, sd_last_error);
        sd_state = 2;
        return 0;
    }
    /* the header sector says whether it is a state and how long */
    pio_read(SD_BRAM_SECTOR + 0x00, &w0);
    pio_read(SD_BRAM_SECTOR + 0x04, &ver);
    pio_read(SD_BRAM_SECTOR + 0x0C, &image_len);
    pio_read(SD_BRAM_SECTOR + 0x14, &h1);
    pio_read(SD_BRAM_SECTOR + 0x18, &h2);
    pio_read(SD_BRAM_SECTOR + 0x64, &image_off);
    pio_read(0xB0000010u, &crc1);
    pio_read(0xB0000014u, &crc2);
    if ((w0 != STATE_MAGIC) || (ver < 2u) || (h1 != crc1) || (h2 != crc2) ||
        (image_off < STATE_HDR_LEN) || (image_off & 511u) || (image_len == 0) || (image_len > image_len_max()) ||
        ((image_off + image_len) > (slot_len_cur() - 0x1000u))) {
        sd_last_error = 0xDDDE0000u | (w0 >> 16);
        return 0;
    }
    uint32_t total = (image_off + image_len + 511u) / 512u;
    if ((total + STATE_RSP_LEN / 512u) <= sd_total) total += STATE_RSP_LEN / 512u;   /* the RSP's memories after the image */
    if (total > sd_total) {
        sd_last_error = 0xCCCE0000u | (total & 0xFFFFu);
        return 0;
    }
    for (uint32_t r = 0; r < sd_run_n; r++) {
        if (sd_runs[r].file_sector >= total) break;
        uint32_t c = sd_runs[r].count;
        if ((sd_runs[r].file_sector + c) > total) c = total - sd_runs[r].file_sector;
        if (!sc64_command_long(SC64_CMD_SD_SECTOR_SET, sd_runs[r].sector, 0, CMD_TIMEOUT_TICKS) ||
            !sc64_command_long(SC64_CMD_SD_READ, slot_base + sd_runs[r].file_sector * 512u, c, SD_XFER_TIMEOUT_TICKS)) {
            sd_state = 2;
            return 0;
        }
    }
    sd_reads_done++;
    crumb(10u, sd_reads_done, 0);
    return 1;
}


/* ---- Virtual Controller Pak ---------------------------------------------------
 * A 32 KiB pak image lives here in the hook (vpak_img), loaded once from the cart,
 * where the menu put the game's pak file, and answered from RAM at every SI
 * interrupt: the game's own joybus block is rewritten before its handler runs, so
 * an accessory read (command 2) gets the image's block and its CRC, an accessory
 * write (command 3) lands in the image and gets its CRC back, and the identify
 * command (0 or 0xFF) reports a pak present. Addresses at 0x8000 and above (the
 * bank select and the rumble probe) read as zeros, which is what a Controller Pak
 * answers; a Rumble Pak is therefore never seen while this is on. Port 1 only.
 * A dirty image goes back to the cart in 4 KiB chunks per tick, a second and a
 * half after the last write, and then to the SD file through the mirror. */
#define VPAK_TABLE_OFF    0x8000u          /* the pak file's run table, after the image */
#define VPAK_BLOCKS       (VPAK_LEN / 32u)
#define VPAK_SETTLE_TICKS 90u
static uint32_t vpak_img[VPAK_LEN / 4u] __attribute__((aligned(16))) = {0};
static uint32_t vpak_loaded = 0;
static uint32_t vpak_dirty = 0, vpak_dirty_tick = 0, vpak_flushing = 0, vpak_sd_pending = 0;
static uint32_t vpak_dirty_mask = 0, vpak_flush_mask = 0;   /* 2 KiB regions written since the last flush / still to copy in this one */
static uint32_t vpak_reads = 0, vpak_writes = 0, vpak_probes = 0;   /* diagnostics */
#define VPAK_TRACE(cmd, rxb, d0, blk) do { } while (0)

/* the accessory data CRC, as libdragon and libultra compute it */
static uint32_t vpak_crc(const uint8_t *d) {
    uint32_t crc = 0;
    for (uint32_t i = 0; i < 32u; i++) {
        uint32_t x = crc ^ d[i];
        crc = 0;
        if (x & 0x80u) crc ^= 0x89u;
        if (x & 0x40u) crc ^= 0x86u;
        if (x & 0x20u) crc ^= 0x43u;
        if (x & 0x10u) crc ^= 0xE3u;
        if (x & 0x08u) crc ^= 0xB3u;
        if (x & 0x04u) crc ^= 0x9Bu;
        if (x & 0x02u) crc ^= 0x8Fu;
        if (x & 0x01u) crc ^= 0x85u;
    }
    return crc & 0xFFu;
}

/* The joybus block at RDRAM `base` (uncached): channel 0's accessory commands are
 * answered in place. Returns how many were. */
static uint32_t vpak_serve(uint32_t base) {
    if ((base == 0) || (base & 7u) || (base > 0x007FFFC0u)) return 0;
    volatile uint8_t *b = (volatile uint8_t *)(0xA0000000u | base);
    uint8_t tmp[32];
    uint32_t i = 0, ch = 0, served = 0;
    while (i < 63u) {
        uint32_t t = b[i];
        if (t == 0xFEu) break;                       /* end of the block */
        if (t == 0xFFu) { i++; continue; }           /* padding */
        if ((t == 0x00u) || (t == 0xFDu)) { ch++; i++; continue; }   /* channel skipped / reset */
        uint32_t rxb = b[i + 1u];
        uint32_t tx = t & 0x3Fu, rx = rxb & 0x3Fu, cmd = b[i + 2u];
        if ((tx == 0) || ((i + 2u + tx + rx) > 64u)) {
            break;                                                    /* not a block after all */
        }
        if (ch == 0u) {
            if ((cmd == 0x02u) || (cmd == 0x03u)) {
                VPAK_TRACE(cmd, rxb, b[i + 5u], (((uint32_t)b[i + 3u] << 8) | b[i + 4u]) >> 5);
            }
            if (((cmd == 0x00u) || (cmd == 0xFFu)) && (tx == 1u) && (rx == 3u)) {
                b[i + 1u] = (uint8_t)rx;                                /* no receive error */
                b[i + 5u] = (uint8_t)((b[i + 5u] | 0x01u) & 0xFDu);     /* pak present, not "pulled" */
                vpak_probes++;
                served++;
            } else if ((cmd == 0x02u) && (tx == 3u) && (rx == 33u)) {
                uint32_t blk = (((uint32_t)b[i + 3u] << 8) | b[i + 4u]) >> 5;
                const uint8_t *src = (const uint8_t *)vpak_img + blk * 32u;
                for (uint32_t k = 0; k < 32u; k++) {
                    tmp[k] = (blk < VPAK_BLOCKS) ? src[k] : 0u;   /* 0x8000 and above read as zeros, as on a Controller
                                                                    * Pak (an echo of the bank byte is a Rumble Pak's answer
                                                                    * and made osMotorInit take this pak for one) */
                    b[i + 5u + k] = tmp[k];
                }
                b[i + 37u] = (uint8_t)vpak_crc(tmp);
                b[i + 1u] = (uint8_t)rx;
                vpak_reads++;
                served++;
            } else if ((cmd == 0x03u) && (tx == 35u) && (rx == 1u)) {
                uint32_t blk = (((uint32_t)b[i + 3u] << 8) | b[i + 4u]) >> 5;
                for (uint32_t k = 0; k < 32u; k++) tmp[k] = b[i + 5u + k];
                if (blk < VPAK_BLOCKS) {
                    uint8_t *dst = (uint8_t *)vpak_img + blk * 32u;
                    for (uint32_t k = 0; k < 32u; k++) dst[k] = tmp[k];
                    vpak_dirty = 1;
                    vpak_dirty_tick = ticks;
                    vpak_dirty_mask |= 1u << (blk >> 6);       /* its 2 KiB region: the flush copies only those */
                } else {
                }
                b[i + 37u] = (uint8_t)vpak_crc(tmp);
                b[i + 1u] = (uint8_t)rx;
                vpak_writes++;
                served++;
            }
        }
        i += 2u + tx + rx;          /* the command byte is counted in tx */
        ch++;
    }
    return served;
}

/* ---- Serving from inside the game's handler ----------------------------------------
 * The block is answered at the SI interrupt, before the game's handler runs. That
 * misses the completions libultra's __osException finds already pending while it is
 * in for another interrupt (every pending MI bit is serviced in one pass, no
 * exception is raised for the second one, the hook never sees it), and the game then
 * reads the real controller's answer to a pak command: no pak. Waiting for an
 * in-flight transfer at every exception closes that at up to a joybus transaction
 * (~1 ms) and shows as jitter when the exception was the VI's; masking SI in MI while
 * a transfer is in flight leaks into libultra's per-thread mask (saved from the
 * hardware at every exception, restored at every dispatch) and stalls every transfer
 * to the next tick. So the handler itself is patched: at its SI acknowledge the words
 *     lui rX, 0xa480 / sw zero, 0x18(rX) / jal send_mesg / li a0, OS_EVENT_SI*8
 * become a jump to vpak_tramp (entry.S), which serves the block and then does what
 * those words did. The site is found by pattern from the game's original 0x180 vector
 * (moved to 0x120 by the boot patcher); a game that runs its handler through a TLB
 * mapping (Turok 2, at 0x0029xxxx) is read and patched through the physical page.
 * Save states carry the game's own words (the jump is lifted for the copy and put
 * back after a load), so a state is good for any build. Without a match (a custom
 * handler) the exception-time serve above stays. */
static uint32_t tramp_site_va = 0, tramp_site_pa = 0, tramp_hits = 0;
static uint32_t tramp_orig[8] = {0};    /* [4..7]: the four words replaced, as found */
uint32_t vpak_tramp_vec[4] = {0, 0, 0, 0};   /* read by entry.S: send_mesg, the return word, a0 for send_mesg, rX | its low half << 16 */
extern void vpak_tramp(void);

/* The RAM behind a virtual address: kseg0/1 directly, anything else through the TLB
 * (a probe; the registers are put back as found). 0 when unmapped. */
static uint32_t tlb_translate(uint32_t va) {
    if ((va >= 0x80000000u) && (va < 0xC0000000u)) return va & 0x1FFFFFFFu;
    uint32_t s_ehi, s_lo0, s_lo1, s_pm, s_idx, idx, lo0, lo1, pm, pa = 0;
    __asm__ volatile("mfc0 %0, $10\n\tnop" : "=r"(s_ehi));
    __asm__ volatile("mfc0 %0, $2\n\tnop" : "=r"(s_lo0));
    __asm__ volatile("mfc0 %0, $3\n\tnop" : "=r"(s_lo1));
    __asm__ volatile("mfc0 %0, $5\n\tnop" : "=r"(s_pm));
    __asm__ volatile("mfc0 %0, $0\n\tnop" : "=r"(s_idx));
    uint32_t ehi = (va & 0xFFFFE000u) | (s_ehi & 0xFFu);
    __asm__ volatile("mtc0 %0, $10\n\tnop\n\tnop\n\tnop\n\ttlbp\n\tnop\n\tnop\n\tnop" : : "r"(ehi) : "memory");
    __asm__ volatile("mfc0 %0, $0\n\tnop" : "=r"(idx));
    if (!(idx & 0x80000000u)) {
        __asm__ volatile("tlbr\n\tnop\n\tnop\n\tnop" : : : "memory");
        __asm__ volatile("mfc0 %0, $2\n\tnop" : "=r"(lo0));
        __asm__ volatile("mfc0 %0, $3\n\tnop" : "=r"(lo1));
        __asm__ volatile("mfc0 %0, $5\n\tnop" : "=r"(pm));
        uint32_t psize = ((pm >> 13) + 1u) << 12;               /* 4 KiB, 16 KiB, ... */
        uint32_t lo = (va & psize) ? lo1 : lo0;
        if (lo & 2u) {                                           /* valid */
            pa = ((lo >> 6) << 12) | (va & (psize - 1u));
        }
    }
    __asm__ volatile("mtc0 %0, $0\n\tnop" : : "r"(s_idx));
    __asm__ volatile("mtc0 %0, $2\n\tnop" : : "r"(s_lo0));
    __asm__ volatile("mtc0 %0, $3\n\tnop" : : "r"(s_lo1));
    __asm__ volatile("mtc0 %0, $5\n\tnop" : : "r"(s_pm));
    __asm__ volatile("mtc0 %0, $10\n\tnop\n\tnop\n\tnop" : : "r"(s_ehi) : "memory");
    return pa;
}

/* a word of the game's handler, wherever its page is (uncached; 0 when unmapped) */
static uint32_t code_page_va = 0xFFFFFFFFu, code_page_pa = 0;
static uint32_t code_word(uint32_t va, uint32_t *w) {
    if ((va & 0xFFFFF000u) != code_page_va) {
        code_page_va = va & 0xFFFFF000u;
        code_page_pa = tlb_translate(code_page_va);
    }
    if ((code_page_pa == 0) || (code_page_pa >= 0x00800000u)) return 0;
    *w = *(vu32 *)(0xA0000000u | code_page_pa | (va & 0xFFFu));
    return 1;
}

/* libultra's SI branch as the assembler left it: `andi t1, s1, 2 / beqz t1`, then in some
 * order (the branch delay slot included, and the jal's) the acknowledge
 * (lui rX, 0xa480 [/ ori rX, rX, 0x18] / sw zero, 0x18|0(rX)), `andi s1, s1, ~2`, the a0
 * for send_mesg (addiu/ori a0, zero, n) and nops, with `jal send_mesg` among them. Turok 2
 * has nop / andi / lui / sw / jal / li; Robotron 64 has lui / andi / sw / jal / li. The four
 * words ending with the jal's delay slot are the ones replaced; whatever of the rest they
 * held, the trampoline redoes all of it (each is harmless to repeat). Returns the jal's
 * index (5..9) or 0; fills rx/lo (rX and its low half) and a0. */
static uint32_t tramp_match(const uint32_t *w, uint32_t *rx_out, uint32_t *lo_out, uint32_t *a0_out) {
    if ((w[0] != 0x32290002u) || ((w[1] >> 16) != 0x1120u)) return 0;
    uint32_t j = 0;
    for (uint32_t k = 5; k <= 9u; k++) {           /* the replaced words start at j-2 >= 3: past the branch and its slot */
        if ((w[k] >> 26) == 3u) { j = k; break; }
    }
    if (!j) return 0;
    uint32_t rx = 0, lo = 0, n_lui = 0, n_sw = 0, n_andi = 0, n_a0 = 0, a0 = 0;
    for (uint32_t k = 2; k <= j + 1u; k++) {
        uint32_t x = w[k];
        if (k == j) continue;
        if (x == 0) continue;                                                                   /* nop */
        if ((x & 0xFFE0FFFFu) == 0x3C00A480u) { rx = (x >> 16) & 0x1Fu; n_lui++; continue; }    /* lui rX, 0xa480 */
        if (n_lui && (x == (0x34000018u | (rx << 21) | (rx << 16)))) { lo = 0x18u; continue; }  /* ori rX, rX, 0x18 */
        if (n_lui && (x == (0xAC000000u | (rx << 21) | (0x18u - lo)))) { n_sw++; continue; }   /* sw zero, 0x18-lo(rX) */
        if (x == 0x3231003Du) { n_andi++; continue; }                                          /* andi s1, s1, ~MI_INTR_SI */
        if ((x >> 16) == 0x2404u) { a0 = (uint32_t)(int32_t)(int16_t)(x & 0xFFFFu); n_a0++; continue; }   /* addiu a0, zero, n */
        if ((x >> 16) == 0x3404u) { a0 = x & 0xFFFFu; n_a0++; continue; }                      /* ori a0, zero, n */
        return 0;                                                                               /* something else in the way */
    }
    if ((n_lui != 1u) || (n_sw != 1u) || (n_andi != 1u) || (n_a0 != 1u)) return 0;
    if ((rx != 1u) && (rx != 8u) && (rx != 9u)) return 0;                                       /* at, t0 or t1 */
    *rx_out = rx;
    *lo_out = lo;
    *a0_out = a0;
    return j;
}

#define TRAMP_SCAN_WORDS 0x200u        /* 2 KiB from the handler's entry (the rcp section sits around +0x400) */
static void vpak_tramp_install(void) {
    if (tramp_state != 0) return;
    tramp_state = 2;
    vu32 *pre = (vu32 *)0xA0000120u;   /* the game's original 0x180 words: lui k0 / addiu k0 / jr k0 / nop */
    uint32_t w0 = pre[0], w1 = pre[1];
    if (((w0 >> 16) != 0x3C1Au) || ((w1 >> 16) != 0x275Au) || (pre[2] != 0x03400008u)) {
        crumb(16u, 1u, w0);
        return;
    }
    uint32_t hva = (w0 << 16) + (uint32_t)(int32_t)(int16_t)(w1 & 0xFFFFu);
    code_page_va = 0xFFFFFFFFu;
    for (uint32_t i = 0; (i + 12u) <= TRAMP_SCAN_WORDS; i++) {
        uint32_t va = hva + 4u * i, w[12], rx, lo, a0;
        if (!code_word(va, &w[0])) { crumb(16u, 2u, va); return; }
        if (w[0] != 0x32290002u) continue;                     /* andi t1, s1, MI_INTR_SI */
        for (uint32_t k = 1; k < 12u; k++) {
            if (!code_word(va + 4u * k, &w[k])) { crumb(16u, 2u, va); return; }
        }
        uint32_t j = tramp_match(w, &rx, &lo, &a0);
        if (!j) continue;
        uint32_t sva = va + 4u * (j - 2u);
        /* borrowed mode: the server is on the cart (monitor.S mon_pak_hit) behind a RAM
         * stub at PAK_STUB_ADDR that waits for the PI first. A handler in kseg0 keeps its
         * own `jal send_mesg / li a0`: the two words before them become `jal stub / nop`
         * (scheme 1) and the server redoes what those two did. A handler run through the
         * TLB (Turok 2) cannot jal into kseg0, so its four words go the resident way, to
         * the stub (scheme 2), and the server calls send_mesg itself. */
        uint32_t scheme = 0, nw = 4u;
        if (borrowed_mode()) {
            scheme = ((sva & 0xF0000000u) == 0x80000000u) ? 1u : 2u;
            nw = (scheme == 1u) ? 2u : 4u;
        }
        if ((sva & 0xFFFu) > (0x1000u - 4u * nw)) { crumb(16u, 3u, sva); return; }   /* the words would cross a page */
        uint32_t spa = tlb_translate(sva & 0xFFFFF000u);
        if ((spa == 0) || (spa >= 0x00800000u)) { crumb(16u, 4u, sva); return; }
        spa |= sva & 0xFFFu;
        for (uint32_t k = 0; k < 4u; k++) {
            tramp_orig[k] = w[k];
            tramp_orig[4u + k] = w[j - 2u + k];
        }
        tramp_site_va = sva;
        tramp_site_pa = spa;
        tramp_scheme = scheme;
        vpak_tramp_vec[0] = (sva & 0xF0000000u) | ((w[j] & 0x03FFFFFFu) << 2);   /* send_mesg, in the handler's own region */
        vpak_tramp_vec[1] = va + 4u * (j + 2u);                                    /* where the jal would have come back */
        vpak_tramp_vec[2] = a0;
        vpak_tramp_vec[3] = rx | (lo << 16);
        tramp_state = 1;
        vpak_tramp_apply(1u);
        crumb(16u, sva, spa);
        return;
    }
    crumb(16u, 5u, hva);
}

/* on: the site jumps to the trampoline; off: the game's own words (for a state's copy) */
/* borrowed mode: the stub the patched site jumps to, in the vector page (0x060..0x080,
 * nothing else of ours lives there since EXIT moved out; the game's Count/Compare across
 * a borrow went to the cart for it). It waits for the PI to go idle (a cart fetch under
 * a game's DMA freezes the console) and jumps to the monitor's server. at and a0 are
 * dead at the site either way; ra is left alone (scheme 1's way back). */
static void pak_stub_place(void) {
    uint32_t t = MONITOR_KSEG1 + MON_PAK_OFF;
    vu32 *s = (vu32 *)(0xA0000000u | (PAK_STUB_ADDR & 0x1FFFFFFFu));
    dcache_writeback_all();                  /* no dirty line of the page may land on it later */
    s[0] = 0x3C04A460u;                      /* 1: lui a0, 0xA460 */
    s[1] = 0x8C840010u;                      /*    lw a0, PI_STATUS(a0) */
    s[2] = 0x30840003u;                      /*    andi a0, a0, 3 (DMA or IO busy) */
    s[3] = 0x1480FFFCu;                      /*    bnez a0, 1b */
    s[4] = 0x3C010000u | (t >> 16);          /*    lui at, hi (the delay slot) */
    s[5] = 0x34210000u | (t & 0xFFFFu);      /*    ori at, at, lo */
    s[6] = 0x00200008u;                      /*    jr at */
    s[7] = 0;                                /*    nop */
}

/* on: the site jumps to the trampoline; off: the game's own words (for a state's copy) */
static void vpak_tramp_apply(uint32_t on) {
    if (tramp_state != 1u) return;
    vu32 *s = (vu32 *)(0xA0000000u | tramp_site_pa);
    uint32_t j[4], nw = 4u;
    if (tramp_scheme == 1u) {
        j[0] = 0x0C000000u | ((PAK_STUB_ADDR >> 2) & 0x03FFFFFFu);                   /* jal stub */
        j[1] = 0; j[2] = 0; j[3] = 0;                                                /* nop */
        nw = 2u;
    } else if (tramp_scheme == 2u) {
        j[0] = 0x3C010000u | (PAK_STUB_ADDR >> 16);                                  /* lui at / ori at, at */
        j[1] = 0x34210000u | (PAK_STUB_ADDR & 0xFFFFu);
        j[2] = 0x00200008u; j[3] = 0;                                                /* jr at / nop */
    } else {
        uint32_t t = (uint32_t)(uintptr_t)vpak_tramp;
        j[0] = 0x3C1F0000u | (t >> 16); j[1] = 0x37FF0000u | (t & 0xFFFFu);         /* lui ra / ori ra, ra */
        j[2] = 0x03E00008u; j[3] = 0;                                                /* jr ra / nop */
    }
    uint32_t is_orig = 1u, is_jump = 1u;
    for (uint32_t k = 0; k < nw; k++) {
        if (s[k] != tramp_orig[4u + k]) is_orig = 0;
        if (s[k] != j[k]) is_jump = 0;
    }
    if (!is_orig && !is_jump) {                    /* not the handler that was patched: leave it alone */
        tramp_state = 3;
        crumb(16u, 6u, s[0]);
        return;
    }
    for (uint32_t k = 0; k < nw; k++) {
        s[k] = on ? j[k] : tramp_orig[4u + k];
    }
    if (on && tramp_scheme) {
        pak_stub_place();                          /* with the site, always: a loaded image brought its own 0x060 */
    }
    icache_invalidate_all();
}

/* From vpak_tramp, inside the game's handler at its SI acknowledge: the block just landed. */
void vpak_si_hit(void) {
    tramp_hits++;
    if (!vpak_loaded) return;
    uint32_t a = SI_DRAM_ADDR & 0x00FFFFFFu;
    if (a >= 0x38u) vpak_serve(a - 0x38u);
}

/* the pak file, from the cart (the menu wrote it there) to the SD card */
static uint32_t sd_write_begin_pak(void) {
    if (sd_state == 1u) return 0;
    if (!sd_ensure_init() || !sd_load_runs_at(VPAK_PI + VPAK_TABLE_OFF)) {
        crumb(13u, 16u, sd_last_error);
        sd_state = 2;
        return 0;
    }
    if (sd_total < (VPAK_LEN / 512u)) {
        sd_last_error = 0xCCCF0000u;
        sd_state = 2;
        return 0;
    }
    sd_segs[0].file_sector = 0; sd_segs[0].count = VPAK_LEN / 512u; sd_segs[0].src = VPAK_PI;
    sd_seg_n = 1;
    sd_seg_i = 0;
    sd_seg_off = 0;
    sd_slot = VPAK_PI;
    sd_inflight = 0;
    sd_gap = 0;
    sd_state = 1;
    sd_issue_chunk();
    crumb(14u, sd_state, 0);
    return (sd_state == 1) ? 1u : 0u;
}

/* VI tick, PI idle, cart unlocked: load the image once; flush a dirty one */
static void vpak_service(void) {
    if (!(hook_cfg.spare & 4u)) return;
    if (!vpak_loaded) {
        dcache_writeback_all();
        if (pi_dma((uint32_t)(uintptr_t)vpak_img, VPAK_PI, VPAK_LEN, 0u)) {
            vpak_loaded = 1;
            crumb(15u, VPAK_PI, 0);
            sd_ensure_init();           /* the card now, at boot, not inside a later tick: the init can
                                         * take hundreds of milliseconds and the game is frozen meanwhile */
            vpak_tramp_install();       /* the game's handler serves from now on */
        }
        return;
    }
    if (vpak_flushing) {
        /* one written 2 KiB region per tick (~0.5 ms with the write enable round trips): a
         * game's usual burst, the FAT and a note, is two or three ticks, not a frame-long stall */
        uint32_t r = 0;
        while ((r < 16u) && !(vpak_flush_mask & (1u << r))) r++;
        if (r >= 16u) {
            vpak_flushing = 0;
            vpak_sd_pending = 1;
            return;
        }
        if (!rom_write_set(1u)) return;
        dcache_writeback_all();
        uint32_t off = r << 11;
        uint32_t ok = pi_dma((uint32_t)(uintptr_t)vpak_img + off, VPAK_PI + off, 0x800u, 1u);
        rom_write_set(0);
        if (!ok) {
            vpak_flushing = 0;
            vpak_dirty = 1;                       /* try again later, all of it */
            vpak_dirty_mask |= vpak_flush_mask;
            return;
        }
        vpak_flush_mask &= ~(1u << r);
        if (!vpak_flush_mask) {
            vpak_flushing = 0;
            vpak_sd_pending = 1;
        }
        return;
    }
    if (vpak_sd_pending && (sd_state != 1u)) {
        vpak_sd_pending = 0;
        sd_write_begin_pak();
        return;
    }
    if (vpak_dirty && ((ticks - vpak_dirty_tick) >= VPAK_SETTLE_TICKS) && !st_pending && !vi_frz_frozen) {
        vpak_dirty = 0;
        vpak_flushing = 1;
        vpak_flush_mask = vpak_dirty_mask;
        vpak_dirty_mask = 0;
    }
}

/* ---- The virtual Controller Pak in borrowed mode ------------------------------------
 * The image never comes to RAM: the monitor's server (monitor.S mp_serve) answers from
 * the cart, at the tick's SI completions and, once the game's handler is patched, from
 * the handler itself through the stub at PAK_STUB_ADDR. Nothing of the hook survives a
 * borrow, so the site's words live in the control block on the cart (VPAK_CTL_PI: the
 * menu writes 'VPK1' and zeros at launch) and come back into the resident globals at
 * every visit. The service borrow (op 6, pak_borrow) installs the site once the game
 * has polled its controller and mirrors the image to the card when the monitor's dirty
 * stamp is 90 ticks old; the panel's exit mirrors it too. */
#define PAK_CTL_MAGIC   0x56504B31u   /* 'VPK1' */
#define PAK_CTL_KSEG1   (0xA0000000u | VPAK_CTL_PI)

static uint32_t pak_ctl_load(void) {
    uint32_t w[14];
    pak_ctl_ok = 0;
    if (!borrowed_mode() || !(hook_cfg.spare & 4u)) return 0;
    for (uint32_t i = 0; i < 14u; i++) {
        if (!pio_read(PAK_CTL_KSEG1 + 4u * i, &w[i])) return 0;
    }
    if (w[0] != PAK_CTL_MAGIC) return 0;
    pak_ctl_ok = 1;
    tramp_state = w[1];                 /* +4 state, +8 the dirty stamp, +C the bank byte (the monitor's) */
    tramp_scheme = w[4];                /* +10 */
    tramp_site_pa = w[5];               /* +14 */
    for (uint32_t k = 0; k < 4u; k++) {
        tramp_orig[4u + k] = w[6u + k]; /* +18..+24: the site's own words */
        vpak_tramp_vec[k] = w[10u + k]; /* +28..+34: send_mesg, the return word, a0, rX | lo << 16 */
    }
    return 1;
}

static uint32_t pak_ctl_store(void) {
    if (!pak_ctl_ok || !rom_write_set(1u)) return 0;
    uint32_t ok = pio_write(PAK_CTL_KSEG1 + 0x04u, tramp_state);
    ok &= pio_write(PAK_CTL_KSEG1 + 0x10u, tramp_scheme);
    ok &= pio_write(PAK_CTL_KSEG1 + 0x14u, tramp_site_pa);
    for (uint32_t k = 0; k < 4u; k++) {
        ok &= pio_write(PAK_CTL_KSEG1 + 0x18u + 4u * k, tramp_orig[4u + k]);
        ok &= pio_write(PAK_CTL_KSEG1 + 0x28u + 4u * k, vpak_tramp_vec[k]);
    }
    rom_write_set(0);
    return ok;
}

/* the image on the cart to the card, to the end, and the dirty stamp cleared */
static uint32_t pak_flush_sync(void) {
    uint32_t dirty = 0, st = ST_OK;
    if (!pak_ctl_ok || !pio_read(PAK_CTL_KSEG1 + 0x08u, &dirty) || !dirty) return ST_OK;
    if (!sd_write_begin_pak()) {
        st = ST_SD_FAIL;
    } else {
        uint32_t t0 = c0_count();
        while (sd_state == 1u) {
            sd_service();
            if ((c0_count() - t0) > 46875u * 30000u) break;
            exit_wait_ms(1u);
        }
        if (sd_state != 0) st = ST_SD_FAIL;
    }
    if ((st == ST_OK) && rom_write_set(1u)) {
        pio_write(PAK_CTL_KSEG1 + 0x08u, 0);
        rom_write_set(0);
    }
    crumb(0x67u, st, sd_last_error);
    return st;
}

/* op 6: the site once, the mirror when dirty */
static uint32_t pak_borrow(void) {
    if (!pak_ctl_ok) return ST_BAD_ARGS;
    if (tramp_state == 0) {
        vpak_tramp_install();               /* patches the site and places the stub, or says why not */
        if (tramp_state == 0) tramp_state = 2;
        pak_ctl_store();
        crumb(0x66u, tramp_state | (tramp_scheme << 8), tramp_site_pa);
        sd_ensure_init();                   /* the card now, not at the first flush */
    }
    return pak_flush_sync();
}


/* ---- Frozen-game I/O: the VI registers the panel reads, and controller polls by
 * our own SI transfers (the game is frozen, so the SI is ours for the moment). */
#define VI_STATUS_REG   (*(vu32 *)0xA4400000u)
#define VI_ORIGIN_REG   (*(vu32 *)0xA4400004u)
#define VI_WIDTH_REG    (*(vu32 *)0xA4400008u)
#define VI_V_VIDEO_REG  (*(vu32 *)0xA4400028u)
#define VI_X_SCALE_REG  (*(vu32 *)0xA4400030u)
#define SI_PIF_AD_RD64B (*(vu32 *)0xA4800004u)
#define SI_PIF_AD_WR64B (*(vu32 *)0xA4800010u)
#define PIF_RAM_PHYS    0x1FC007C0u

static uint32_t pif_block[16] __attribute__((aligned(16))) = {0};
static uint32_t pif_stick_y = 0;
static uint32_t pif_stick_x = 0;

static uint32_t si_wait(void) {
    uint32_t t0 = c0_count();
    while (SI_STATUS & 3u) {
        if ((c0_count() - t0) > (46875u * 20u)) {
            return 0;
        }
        vi_frz_service();
    }
    return 1;
}

/* One controller poll by our own SI DMAs (the game is frozen and the SI is ours);
 * the interrupts we raise are cleared so the game never sees them. */
static uint32_t pad_dma_poll(uint32_t *buttons) {
    uint32_t phys = (uint32_t)(uintptr_t)pif_block & 0x1FFFFFFFu;
    vu32 *blk = (vu32 *)(0xA0000000u | phys);
    for (uint32_t ch = 0; ch < 4u; ch++) {
        blk[2u * ch] = 0xFF010401u;            /* pad, tx 1, rx 4, cmd 1 (read buttons) */
        blk[2u * ch + 1u] = 0xFFFFFFFFu;       /* answer space */
    }
    blk[8] = 0xFE000000u;                      /* end of block */
    for (uint32_t i = 9; i < 15u; i++) {
        blk[i] = 0;
    }
    blk[15] = 0x00000001u;                     /* PIF: run the joybus commands */
    if (!si_wait()) return 0;
    SI_DRAM_ADDR = phys;
    si_touched = 1;
    SI_PIF_AD_WR64B = PIF_RAM_PHYS;
    if (!si_wait()) return 0;
    SI_STATUS = 0;
    SI_DRAM_ADDR = phys;
    si_touched = 1;
    SI_PIF_AD_RD64B = PIF_RAM_PHYS;
    if (!si_wait()) return 0;
    SI_STATUS = 0;
    uint32_t w0 = blk[0], w1 = blk[1];
    if ((w0 & 0x00C00000u) != 0) return 0;     /* rx error bits: no controller */
    *buttons = w1 >> 16;
    pif_stick_x = (w1 >> 8) & 0xFFu;
    pif_stick_y = w1 & 0xFFu;
    return 1;
}

/* The PIF's command block is hardware state outside RAM. libultra re-sends its poll
 * block only when its last command changed (__osContLastCmd), so a world whose last
 * command was a poll, loaded into a boot whose PIF holds something else (Mario 64 half
 * a second in: an EEPROM read of its save), polls forever through that command: EEPROM
 * bytes for buttons, no input for the game or for the hook's combos. Put the standard
 * four-channel poll block there before the loaded world runs; a world that was in the
 * middle of something else re-sends its own block anyway. */
static void pif_poll_block_send(void) {
    uint32_t phys = (uint32_t)(uintptr_t)pif_block & 0x1FFFFFFFu;
    vu32 *blk = (vu32 *)(0xA0000000u | phys);
    for (uint32_t ch = 0; ch < 4u; ch++) {
        blk[2u * ch] = 0xFF010401u;            /* pad, tx 1, rx 4, cmd 1 (read buttons) */
        blk[2u * ch + 1u] = 0xFFFFFFFFu;
    }
    blk[8] = 0xFE000000u;
    for (uint32_t i = 9; i < 15u; i++) {
        blk[i] = 0;
    }
    blk[15] = 0x00000001u;                     /* PIF: run the joybus commands */
    if (!si_wait()) return;
    SI_DRAM_ADDR = phys;
    SI_PIF_AD_WR64B = PIF_RAM_PHYS;
    si_wait();
    SI_STATUS = 0;                             /* that completion is ours, not the loaded world's */
    /* and the answer back: the PIF's poll is then complete, so the loaded game's first read
     * of PIF RAM sees this poll and not the panel's last one */
    SI_DRAM_ADDR = phys;
    si_touched = 1;
    SI_PIF_AD_RD64B = PIF_RAM_PHYS;
    si_wait();
    SI_STATUS = 0;
}

/* ---- Slow motion and frame step ---------------------------------------------
 * Set from the panel's Game page. At a VI interrupt the game is held inside the
 * exception for div-1 further fields, the picture kept steady by the field
 * service, then released where a VI interrupt would be handled: it sees one VI
 * event per div fields and runs at 60/div Hz while the screen refreshes at 60,
 * every game frame shown div times. Count is rolled back over the hold, so the
 * game's clock runs slow with it. The game makes less audio than the console
 * plays during a hold, so the sound stutters; that is the price. STEP holds
 * until Z is tapped (hidden from the game by pad_si_tick) or L+R+Start asks for
 * the panel. Never during a queued state operation or a running mirror. */
/* The sound of slow motion: the game makes one audio buffer per div fields, the AI
 * plays one per field, so either the sound has gaps or the AI is slowed with the
 * game. The DAC rate register is write-only, so the game's rate is measured from how
 * fast AI_LEN drains, converted with the AI clock of the console's TV type, then
 * scaled by div: continuous sound at 1/div pitch. Put back when the speed goes back.
 * The measurement needs the AI busy with a fresh buffer. At normal speed that is
 * always so; under slow motion only inside the game's frame, right after it queued
 * one (by the next VI tick the AI has finished it). So it runs from any exception
 * that finds the AI busy with at least 1.5 KiB to go, holding the game once for at
 * most 12 ms with the freeze machinery keeping the picture and the clock. */
#define AI_LEN_REG      (*(vu32 *)0xA4500004u)
#define AI_DACRATE_REG  (*(vu32 *)0xA4500010u)
static uint32_t ai_game_dacrate = 0;    /* the game's own value (0 = not measured) */
static uint32_t ai_slow_applied = 0;
static uint32_t ai_measure_pending = 0; /* a slow speed with pitch-down waits for a measurement */
static uint32_t ai_measure_tries = 0, ai_measured_bps = 0;   /* diagnostics */

static void ai_measure_now(void) {
    uint32_t prev = AI_LEN_REG, bytes = 0, t0 = c0_count(), t_last = t0;
    ai_measure_tries++;
    vi_frz_begin(0);
    while ((c0_count() - t0) < (46875u * 12u)) {
        uint32_t l = AI_LEN_REG;
        if (l != prev) {
            if (l > prev) break;                        /* the next buffer started: a clean end */
            bytes += prev - l;
            prev = l;
            t_last = c0_count();
        }
        vi_frz_service();
    }
    vi_frz_end();
    uint32_t dt = t_last - t0;
    if ((bytes < 1024u) || (dt < (46875u * 6u))) return;   /* too little: try again on a later exception */
    uint32_t tv = *(vu32 *)0x80000300u;                 /* osTvType: 0 PAL, 1 NTSC, 2 MPAL */
    uint32_t clock = (tv == 0u) ? 49656530u : ((tv == 2u) ? 48628316u : 48681812u);
    uint32_t bps = (bytes * 46875u) / (dt / 1000u);     /* bytes per second; 4 per stereo sample */
    if (bps < 16000u) return;
    ai_measured_bps = bps;
    /* the measurement is good to about half a percent; games use a few standard rates,
     * so snap to one within 1.5% (the restore is then exact), else keep what was seen */
    static const uint32_t rates[8] = {8000u, 11025u, 16000u, 22050u, 24000u, 32000u, 44100u, 48000u};
    uint32_t rate = bps / 4u;
    for (uint32_t i = 0; i < 8u; i++) {
        uint32_t diff = (rate > rates[i]) ? (rate - rates[i]) : (rates[i] - rate);
        if (diff * 200u <= rates[i] * 3u) {
            rate = rates[i];
            break;
        }
    }
    uint32_t d = clock / rate;                           /* dacrate + 1 = clock / rate, as libultra computes it */
    if (d < 2u) return;
    ai_game_dacrate = d - 1u;
    ai_measure_pending = 0;
}

static void ai_slow_set(uint32_t on) {
    if (on) {
        if (!ai_game_dacrate) {
            ai_measure_pending = 1;                     /* the next busy exception measures; gaps until then */
            return;
        }
        uint32_t r = (ai_game_dacrate + 1u) * speed_div - 1u;
        if (r > 0x3FFFu) r = 0x3FFFu;
        AI_DACRATE_REG = r;
        ai_slow_applied = 1;
    } else {
        ai_measure_pending = 0;
        if (ai_slow_applied) {
            AI_DACRATE_REG = ai_game_dacrate;
            ai_slow_applied = 0;
        }
        ai_game_dacrate = 0;                            /* measured afresh next time: the game may retune */
    }
}

static void speed_hold(void) {
    if ((speed_div <= 1u) || (speed_div == SPEED_STEP) || slow_sound) {
        ai_slow_set(0);
    }
    if ((speed_div <= 1u) || st_pending || (sd_state == 1u) || vi_frz_frozen) return;
    vi_frz_begin(1u);
    if ((speed_div != SPEED_STEP) && !slow_sound) {
        ai_slow_set(1u);                                /* every hold: the game may have rewritten the rate */
    }
    if (speed_div == SPEED_STEP) {
        uint32_t held = 0;
        for (;;) {
            uint32_t b = 0;
            if (pad_dma_poll(&b)) {
                uint32_t pressed = b & ~step_prev;
                if (step_prev == 0xFFFFu) pressed = 0;
                step_prev = b;
                if (pressed & 0x2000u) break;                     /* Z: one frame */
                if (hook_cfg.combo_menu && ((b & hook_cfg.combo_menu) == hook_cfg.combo_menu)) {
                    if (++held >= 8u) {
                        state_queue(STATE_OP_MENU);
                        break;
                    }
                } else {
                    held = 0;
                }
            }
            uint32_t t1 = c0_count();
            while ((c0_count() - t1) < (46875u * 16u)) { vi_frz_service(); }
        }
    } else {
        for (uint32_t f = 2u; f < speed_div; f++) vi_hold_field();   /* vi_frz_end holds the last one */
    }
    vi_frz_end();
}

/* ---- Exit to the SC64 menu ----------------------------------------------------------
 * What the reset button does, from software. Everything still on its way to the card is
 * written first (the pak's regions and its file, a state mirror, the cart's own save
 * writeback, which the SC64 does by itself a moment after the game's last write), then
 * the hardware is quieted the way the menu quiets it before it boots a ROM, the SC64's
 * bootloader is switched back in at 0x10000000 and its IPL3 is run from SP DMEM with the
 * menu's in-IMEM boot routine (reboot_blob, entry.S) and the registers the PIF would have
 * set. The bootloader then loads sc64menu.n64 from the card as after a reset. */
#define SC64_CFG_BOOTLOADER_SWITCH 0u
#define SC64_CMD_WRITEBACK_PENDING 0x77u /* 'w' */
#define VI_V_INTR_REG    (*(vu32 *)0xA440000Cu)
#define VI_H_VIDEO_REG   (*(vu32 *)0xA4400024u)
#define PI_DOM0_LAT      (*(vu32 *)0xA4600014u)
#define PI_DOM0_PWD      (*(vu32 *)0xA4600018u)
#define PI_DOM0_PGS      (*(vu32 *)0xA460001Cu)
#define PI_DOM0_RLS      (*(vu32 *)0xA4600020u)
extern const uint32_t reboot_blob[];
extern char reboot_blob_size[], reboot_blob_entry_off[];
static uint32_t ipl3w[1008] = {0};        /* the bootloader's IPL3 (4032 bytes), for its CIC seed */
static uint32_t exit_stage = 0;

/* the IPL3 checksum the CICs verify, as the menu computes it (src/boot/cic.c) */
static uint32_t cic_sum3(uint32_t a0, uint32_t a1, uint32_t a2) {
    uint64_t prod = (uint64_t)a0 * (uint64_t)((a1 == 0) ? a2 : a1);
    uint32_t hi = (uint32_t)(prod >> 32), lo = (uint32_t)prod, diff = hi - lo;
    return diff ? diff : a0;
}
#define ROL32(a, s) (((a) << ((s) & 31u)) | ((a) >> ((32u - (s)) & 31u)))
#define ROR32(a, s) (((a) >> ((s) & 31u)) | ((a) << ((32u - (s)) & 31u)))

static uint64_t cic_checksum(uint32_t seed) {
    const uint32_t MAGIC = 0x6C078965u;
    uint32_t data, prev, next, buf[16], fin[4];
    data = prev = next = ipl3w[0];
    uint32_t init = (MAGIC * seed + 1u) ^ data;
    for (uint32_t i = 0; i < 16u; i++) buf[i] = init;
    for (uint32_t i = 1; i <= 1008u; i++) {
        prev = data;
        data = next;
        buf[0] += cic_sum3(1007u - i, data, i);
        buf[1] = cic_sum3(buf[1], data, i);
        buf[2] ^= data;
        buf[3] += cic_sum3(data + 5u, MAGIC, i);
        buf[4] += ROR32(data, prev & 0x1Fu);
        buf[5] += ROL32(data, prev >> 27);
        buf[6] = (data < buf[6]) ? ((buf[3] + buf[6]) ^ (data + i)) : ((buf[4] + data) ^ buf[6]);
        buf[7] = cic_sum3(buf[7], ROL32(data, prev & 0x1Fu), i);
        buf[8] = cic_sum3(buf[8], ROR32(data, prev >> 27), i);
        buf[9] = (prev < data) ? cic_sum3(buf[9], data, i) : (buf[9] + data);
        if (i == 1008u) break;
        next = ipl3w[i];
        buf[10] = cic_sum3(buf[10] + data, next, i);
        buf[11] = cic_sum3(buf[11] ^ data, next, i);
        buf[12] += buf[8] ^ data;
        buf[13] += ROR32(data, data & 0x1Fu) + ROR32(next, next & 0x1Fu);
        buf[14] = cic_sum3(cic_sum3(buf[14], ROR32(data, prev & 0x1Fu), i), ROR32(next, data & 0x1Fu), i);
        buf[15] = cic_sum3(cic_sum3(buf[15], ROL32(data, prev >> 27), i), ROL32(next, data >> 27), i);
    }
    for (uint32_t i = 0; i < 4u; i++) fin[i] = buf[0];
    for (uint32_t i = 0; i < 16u; i++) {
        uint32_t d = buf[i];
        fin[0] += ROR32(d, d & 0x1Fu);
        fin[1] = (d < fin[0]) ? (fin[1] + d) : cic_sum3(fin[1], d, i);
        fin[2] = (((d & 2u) >> 1) == (d & 1u)) ? (fin[2] + d) : cic_sum3(fin[2], d, i);
        fin[3] = ((d & 1u) == 1u) ? (fin[3] ^ d) : cic_sum3(fin[3], d, i);
    }
    uint32_t fsum = cic_sum3(fin[0], fin[1], 16u), fxor = fin[3] ^ fin[2];
    return (((uint64_t)(fsum & 0xFFFFu)) << 32) | fxor;
}

static uint32_t cic_seed_detect(void) {
    uint64_t c = cic_checksum(0x3Fu);
    if ((c == 0x45CC73EE317AULL) || (c == 0x44160EC5D9AFULL) || (c == 0xA536C0F1D859ULL)) return 0x3Fu;
    if (cic_checksum(0x78u) == 0x586FD4709867ULL) return 0x78u;
    if (cic_checksum(0x85u) == 0x2BBAD4E6EB74ULL) return 0x85u;
    if (cic_checksum(0x91u) == 0x8618A45BC2D3ULL) return 0x91u;
    return 0x3Fu;
}

static void exit_wait_ms(uint32_t ms) {
    uint32_t t = c0_count();
    while ((c0_count() - t) < 46875u * ms) vi_frz_service();
}

/* From the panel, the game frozen. 0 when the cart did not switch (nothing has been
 * stopped yet then: back to the panel); otherwise never returns. */
static uint32_t menu_exit(void) {
    uint32_t t0, w, name0 = 0, name1 = 0;
    exit_stage = 1;
    crumb(17u, 1u, 0);
    if (borrowed_mode()) {
        pak_flush_sync();                                    /* the image is on the cart already: the card copy */
    }
    if (vpak_loaded && (vpak_dirty || vpak_dirty_mask)) {   /* the pak's written regions now, no settling */
        vpak_dirty = 0;
        vpak_flushing = 1;
        vpak_flush_mask |= vpak_dirty_mask;
        vpak_dirty_mask = 0;
    }
    t0 = c0_count();
    while (vpak_flushing || vpak_sd_pending || (sd_state == 1u) || sd_pending_slot) {
        vpak_service();
        sd_service();
        if ((c0_count() - t0) > 46875u * 20000u) break;   /* 20 s: whatever is stuck stays stuck */
        exit_wait_ms(1u);
    }
    t0 = c0_count();
    for (;;) {                                              /* the cart's own save writeback */
        uint32_t pend = 0;
        if (!sc64_command(SC64_CMD_WRITEBACK_PENDING, 0, 0, 0) || !pio_read(SC64_DATA0, &pend) || !pend) break;
        if ((c0_count() - t0) > 46875u * 5000u) break;
        exit_wait_ms(10u);
    }
    rom_write_set(0);
    exit_stage = 2;
    /* the SC64's bootloader back at 0x10000000; the boot mode stays what the menu set */
    pio_read(0xB0000020u, &name0);
    pio_read(0xB0000024u, &name1);
    if (!sc64_command(SC64_CMD_CONFIG_SET, SC64_CFG_BOOTLOADER_SWITCH, 1u, 0)) {
        crumb(17u, 0xEu, 1u);
        return 0;
    }
    pio_read(0xB0000020u, &w);
    if (w == name0) {
        pio_read(0xB0000024u, &w);
        if (w == name1) {                                   /* the same image: this cart does not switch */
            sc64_command(SC64_CMD_CONFIG_SET, SC64_CFG_BOOTLOADER_SWITCH, 0, 0);
            crumb(17u, 0xEu, 2u);
            return 0;
        }
    }
    exit_stage = 3;
    crumb(17u, 3u, w);
    /* quiet: no interrupts, the RSP halted, the DP drained, audio and video stopped */
    MI_INTR_MASK = 0x555u;
    SP_STATUS = 0x00AAAAAEu;                                /* halt; clear broke, intr, sstep, intr-break, the signals */
    t0 = c0_count();
    while ((SP_STATUS & SP_DMA_BUSY) && ((c0_count() - t0) < 46875u * 100u)) {}
    SP_SEMAPHORE_REG = 0;
    SP_PC_REG = 0;
    PI_STATUS = PI_STATUS_W_CLR_INTR | PI_STATUS_W_RESET;
    t0 = c0_count();
    while (((VI_CURRENT & ~1u) != 0) && ((c0_count() - t0) < 46875u * 40u)) {}
    VI_V_INTR_REG = 0x3FFu;
    VI_H_VIDEO_REG = 0;
    VI_CURRENT = 0;
    AI_DRAM_ADDR = 0;
    AI_LEN = 0;
    if (DPC_STATUS & 1u) {
        t0 = c0_count();
        while ((DPC_STATUS & 0x20u) && ((c0_count() - t0) < 46875u * 100u)) {}
    }
    /* the boot routine into IMEM, the PI timing and the IPL3 from the new image */
    {
        uint32_t nw = (uint32_t)(uintptr_t)reboot_blob_size / 4u;
        for (uint32_t i = 0; i < nw; i++) *(vu32 *)(0xA4001000u + 4u * i) = reboot_blob[i];
    }
    PI_DOM0_LAT = 0xFFu; PI_DOM0_PWD = 0xFFu; PI_DOM0_PGS = 0x0Fu; PI_DOM0_RLS = 0x03u;
    pio_read(0xB0000000u, &w);
    PI_DOM0_LAT = w & 0xFFu; PI_DOM0_PWD = (w >> 8) & 0xFFu; PI_DOM0_PGS = (w >> 16) & 0x0Fu; PI_DOM0_RLS = (w >> 20) & 0x03u;
    for (uint32_t i = 16u; i < 1024u; i++) {
        pio_read(0xB0000000u + 4u * i, &w);
        ipl3w[i - 16u] = w;
        *(vu32 *)(0xA4000000u + 4u * i) = w;
    }
    uint32_t seed = cic_seed_detect();
    uint32_t tv = (*(vu32 *)0xA0000300u) & 3u;              /* osTvType: 0 PAL, 1 NTSC, 2 MPAL */
    uint32_t ver = (tv == 0) ? 6u : ((tv == 1u) ? 1u : 4u);
    crumb(17u, 4u, seed);
    exit_stage = 4;
    {
        register uint32_t r_s1 __asm__("s1") = 0;           /* clear RDRAM: no */
        register uint32_t r_a0 __asm__("a0") = 0;           /* keep RDRAM up: no, a cold boot's reset */
        register uint32_t r_s3 __asm__("s3") = 0;           /* boot device: cartridge */
        register uint32_t r_s4 __asm__("s4") = tv;
        register uint32_t r_s5 __asm__("s5") = 0;           /* reset type: cold */
        register uint32_t r_s6 __asm__("s6") = seed;
        register uint32_t r_s7 __asm__("s7") = ver;
        uint32_t entry = 0xA4001000u + (uint32_t)(uintptr_t)reboot_blob_entry_off;
        __asm__ volatile(
            "lui $t3, 0x3400\n\t"                          /* Status: CU1, CU0, FR; no interrupts, no EXL */
            "mtc0 $t3, $12\n\t"
            "nop\n\tnop\n\tnop\n\t"
            ".set push\n\t.set hardfloat\n\t"
            "ctc1 $zero, $f31\n\t"
            ".set pop\n\t"
            "jr %0\n\t"
            "nop"
            : : "r"(entry), "r"(r_s1), "r"(r_a0), "r"(r_s3), "r"(r_s4), "r"(r_s5), "r"(r_s6), "r"(r_s7)
            : "t3", "memory");
    }
    for (;;) {}
}

#include "ss_overlay.c"


/* ---- borrowed-RAM mode --------------------------------------------------------
 * Games that use all 8 MiB (DK64, Perfect Dark) get no resident hook. A monitor on
 * the cart (monitor.S, run in place) watches the combos from the vector page's gate
 * (lowpage.S); on a trigger it stashes our 128 KiB home to STASH_PI, DMAs the hook in
 * from its staging copy and enters it as the engine's tail would, and entry.S then
 * returns to the monitor's epilogue (borrow_epilogue) instead of the game, which puts
 * the region back. Everything below happens inside that one visit: the request comes
 * from the vector page, the image copies route the region under us through the stash,
 * and nothing of ours survives to the next visit except what goes to the cart. */
#define BORROW_GUARD    (*(vu32 *)0xA0000190u)
#define BORROW_REQ      (*(vu32 *)0xA0000194u)
#define BORROW_DIAG0    (*(vu32 *)0xA00001A8u)
#define BORROW_DIAG1    (*(vu32 *)0xA00001ACu)
#define BORROW_LO       0x007D0000u
#define BORROW_HI       0x007F0000u
extern uint32_t borrow_epilogue;          /* entry.S: where to go instead of the game, 0 = the game */
static uint32_t bounce[0x2000u / 4u] __attribute__((aligned(16))) = {0};   /* cart <-> cart, 8 KiB at a time */

/* The words at 0x807FFFF8/FC: entry.S keeps its stage marks there, the game's own
 * values sit in the vector page (the monitor saved them). 1: put the game's into RAM
 * for the image copy; 0: take a loaded image's out, for the epilogue to put back. */
static void borrow_diag_words(uint32_t place) {
    if (place) {
        *(vu32 *)0xA07FFFF8u = BORROW_DIAG0;
        *(vu32 *)0xA07FFFFCu = BORROW_DIAG1;
    } else {
        BORROW_DIAG0 = *(vu32 *)0xA07FFFF8u;
        BORROW_DIAG1 = *(vu32 *)0xA07FFFFCu;
    }
}

/* The image, with the region under the hook taken from / put into the stash. */
static uint32_t borrow_image_copy(uint32_t slot, uint32_t ioff, uint32_t ilen, uint32_t to_cart) {
    /* a load shows the replaced world's frame until the loaded moment appears: its
     * buffer is restored last (as the resident path's two passes do; DK64's menu <->
     * cutscene loads showed the incoming data through the old geometry meanwhile) */
    uint32_t ds = 0, dl = 0;
    if (!to_cart) {
        ov_disp_range(&ds, &dl);
    }
    for (uint32_t pass = 0; pass < 2u; pass++) {
    for (uint32_t off = 0; off < ilen; off += 0x10000u) {
        uint32_t n = ((ilen - off) < 0x10000u) ? (ilen - off) : 0x10000u;
        uint32_t cart = slot + ioff + off;
        uint32_t overlaps = dl && (off < ds + dl) && (ds < off + n);
        if (overlaps != pass) continue;
        if ((off >= BORROW_LO) && (off < BORROW_HI)) {
            uint32_t stash = STASH_PI + (off - BORROW_LO);
            uint32_t b = (uint32_t)(uintptr_t)bounce;
            for (uint32_t k = 0; k < n; k += sizeof(bounce)) {
                if (to_cart) {
                    if (!pi_dma(b, stash + k, sizeof(bounce), 0u) || !pi_dma(b, cart + k, sizeof(bounce), 1u)) return 0;
                } else {
                    if (!pi_dma(b, cart + k, sizeof(bounce), 0u) || !pi_dma(b, stash + k, sizeof(bounce), 1u)) return 0;
                }
            }
        } else if (!pi_dma(STATE_IMAGE_BASE + off, cart, n, to_cart)) {
            return 0;
        }
    }
    }
    return 1u;
}

/* The end of a load: state_resume would eret into the loaded world from the hook's
 * copy of its context, but the region under the hook still has to become the loaded
 * world's (it is in the stash). So the context goes to the cart and the monitor's load
 * epilogue restores the region and then the context, from there. Never returns. */
static void borrow_load_exit(void) {
    *(vu32 *)0x800001D0u = st_hdr.reserved[0];   /* the loaded world's PI address registers: the monitor's */
    *(vu32 *)0x800001D4u = st_hdr.reserved[1];   /* load epilogue and the RAM exit stub put them back last */
    rom_write_set(1u);
    dcache_writeback_all();
    pi_dma((uint32_t)(uintptr_t)&st_hdr.ctx, CTX_PI, 0x220u, 1u);
    rom_write_set(0);
    crumb(0x63u, st_hdr.ctx.epc, st_hdr.ctx.status);
    ((void (*)(void))(uintptr_t)(MONITOR_KSEG1 + MON_EPIL_OFF))();
    for (;;) {
    }
}

/* ---- The PC's requests (borrowed mode) ---------------------------------------
 * A PC writes hook_cfg.pc_req over USB in the staged copy on the cart; the
 * monitor arms a borrow for it (monitor.S) and this copy of the block carries it
 * in. The answer goes back the same way: pc_req cleared, pc_ack = the request's
 * sequence with 0x100 and the outcome, pc_shot = where a screenshot went. */
static uint32_t borrow_pc = 0;

static void borrow_pc_ack(uint32_t status) {
    if (!borrow_pc) return;
    uint32_t base = 0xA0000000u | (HOOK_STAGING_PI + ((uint32_t)(uintptr_t)&hook_cfg - 0x807D0000u));
    if (rom_write_set(1u)) {          /* the staged copy is ROM space: writes land only with the cart's
                                       * ROM-write enable on (without it the request stayed, and the
                                       * monitor took screenshot after screenshot) */
        pio_write(base + 0x88u, hook_cfg.pc_shot);
        pio_write(base + 0x80u, 0);
        pio_write(base + 0x84u, (borrow_pc & 0xFFFF0000u) | 0x100u | (status & 0xFFu));
        rom_write_set(0);
    }
    borrow_pc = 0;
}

/* A screenshot (op 5): the displayed frame as the VI shows it, to the cart just
 * above the last state slot: a 16-byte header ('SHOT', stride | visible << 16,
 * height | bpp << 16, the RDRAM origin) and the rows at their stride. */
/* A run of the displayed frame (src: RDRAM physical) to the cart. In borrowed mode the
 * lines under the hook's home are the stash's for this visit (the region was moved out
 * for the hook), so they come from there, through the bounce: a hi-res buffer high in
 * RAM (Rush 2049's pak screen, Indiana Jones' menus) is whole in a screenshot. */
static uint32_t shot_copy(uint32_t src, uint32_t dst, uint32_t n) {
    while (n) {
        uint32_t piece = (n < STATE_CHUNK) ? n : STATE_CHUNK;
        if (borrowed_mode() && (src < BORROW_HI) && ((src + piece) > BORROW_LO)) {
            if (src < BORROW_LO) {
                piece = BORROW_LO - src;                        /* up to the home: direct */
            } else {
                if (piece > (BORROW_HI - src)) piece = BORROW_HI - src;
                uint32_t b = (uint32_t)(uintptr_t)bounce;
                for (uint32_t k = 0; k < piece; k += sizeof(bounce)) {
                    uint32_t m = ((piece - k) < sizeof(bounce)) ? (piece - k) : sizeof(bounce);
                    if (!pi_dma(b, STASH_PI + (src + k - BORROW_LO), m, 0u) || !pi_dma(b, dst + k, m, 1u)) return 0;
                }
                src += piece; dst += piece; n -= piece;
                continue;
            }
        }
        if (!pi_dma(0x80000000u | src, dst, piece, 1u)) return 0;
        src += piece; dst += piece; n -= piece;
    }
    return 1u;
}

static uint32_t shot_take(void) {
    struct ov_screen s;
    static uint32_t hdr[4] __attribute__((aligned(16))) = {0x53484F54u, 0, 0, 0};   /* initialised: .data, not .bss */
    ov_screen_read(&s);
    uint32_t height = borrowed_mode() ? s.height_all : s.height;   /* the home's lines too, from the stash */
    if (((s.fb & 0x00FFFFFFu) == 0) || (s.width < 256u) || (s.width > 640u) || (s.vis < 256u) || (height < 160u)) {
        return ST_BAD_ARGS;
    }
    uint32_t n = hook_cfg.slots_n;
    if (!n || (n > CFG_SLOTS_MAX) || !hook_cfg.slots[n - 1u]) return ST_NO_ROOM;
    uint32_t dst = hook_cfg.slots[n - 1u] + slot_len_cur();
    uint32_t len = (s.width * height * s.bpp + 15u) & ~15u;
    if ((dst + 16u + len) > FRAME_STASH_PI) return ST_NO_ROOM;   /* the frame stash sits below the staging */
    hdr[0] = 0x53484F54u;
    hdr[1] = s.width | (s.vis << 16);
    hdr[2] = height | (s.bpp << 16);
    hdr[3] = s.fb & 0x00FFFFFFu;
    if (!rom_write_set(1u)) return ST_NO_ROM_WRITE;
    dcache_writeback_all();
    uint32_t ok = pi_dma((uint32_t)(uintptr_t)hdr, dst, 16u, 1u) && shot_copy(hdr[3], dst + 16u, len);
    rom_write_set(0);
    hook_cfg.pc_shot = dst;
    return ok ? ST_OK : ST_DMA_FAIL;
}

/* One visit: the action the monitor was asked for, synchronously. */
static void borrow_run(void) {
    uint32_t op = BORROW_REQ & 0xFFu;
    borrow_mi = (BORROW_REQ >> 8) & 0x3Fu;   /* the monitor's moment (mt_trigger) */
    uint32_t cause = c0_cause();
    uint32_t c_in = c0_count(), cmp_in = c0_compare();   /* the game's clock as we found it */
    uint32_t slot0 = hook_cfg.cur_slot;
    BORROW_REQ = 0;
    borrow_epilogue = MONITOR_KSEG1;      /* +0: mon_epilogue */
    crumb(0x60u, op, BORROW_GUARD);
    if (ensure_unlocked()) {
        dbg_unlock_ok++;                  /* state_queue wants to have heard from the cart */
    } else {
        crumb(0x61u, 0, 0);
        return;
    }
    if (op == 4u) {                       /* a suspended state: as the resident hook's resume */
        uint32_t slot = (hook_cfg.spare >> 8) & 0xFu;
        if (!slot || (slot > hook_cfg.slots_n) || !hook_cfg.slots[slot - 1u]) {
            return;
        }
        hook_cfg.cur_slot = slot - 1u;
        resume_pending = slot;
        op = 2u;
    }
    pak_ctl_load();                   /* the virtual pak's site words, for the state copies (and op 6) */
    if (op != 6u) {                   /* (the pak's own borrow answers no PC request: a pending one
                                       * stays on the cart for the monitor to arm after it) */
        borrow_pc = hook_cfg.pc_req;  /* the PC's request, if this borrow is one: answered at the end */
        hook_cfg.pc_req = 0;          /* (a write-back of the block must not repeat it) */
    }
    uint32_t pc_status = ST_OK;
    if (op == 5u) {
        pc_status = shot_take();      /* a screenshot: no state machinery involved */
        crumb(0x64u, pc_status, hook_cfg.pc_shot);
    } else if (op == 6u) {
        pc_status = pak_borrow();     /* the virtual pak's service: the site, the card mirror */
        crumb(0x65u, pc_status, tramp_state);
    } else if (op == 1u) {
        state_queue(STATE_OP_SAVE);
    } else if (op == 2u) {
        state_queue(STATE_OP_LOAD);
    } else if (op == 3u) {
        state_queue(STATE_OP_MENU);
    } else {
        return;
    }
    /* the monitor triggers at a clean VI moment (the RCP idle, the VI the only interrupt
     * pending), so the first look should take it; a short wait covers the odd case */
    uint32_t t0 = c0_count();
    while (st_pending) {
        state_service(cause);
        if (!st_pending) break;
        if ((c0_count() - t0) > 46875u * 2000u) {
            state_done(ST_TIMEOUT);
            break;
        }
        exit_wait_ms(1u);
    }
    /* the SD mirror of a save, to the end */
    t0 = c0_count();
    while ((sd_state == 1u) || sd_pending_slot) {
        sd_service();
        if ((c0_count() - t0) > 46875u * 30000u) break;
        exit_wait_ms(1u);
    }
    rom_write_set(0);
    if (hook_cfg.cur_slot != slot0) {
        /* the slot chosen on the panel, into the staged copy: the next visit starts from it */
        if (rom_write_set(1u)) {
            dcache_writeback_all();
            pi_dma((uint32_t)(uintptr_t)&hook_cfg, HOOK_STAGING_PI + ((uint32_t)(uintptr_t)&hook_cfg - 0x807D0000u), sizeof(hook_cfg), 1u);
            rom_write_set(0);
        }
    }
    /* The game's clock: the whole borrow never happened. The state machinery's freeze
     * hides only its own span (vi_frz_end), and in borrowed mode the SD mirror of a
     * save must run to the end before the game gets its RAM back: a second or more
     * with the game held and Count running. Episode I Racer integrates its physics
     * over that gap and the pod teleported ahead after every save); a screenshot
     * leaked its own span the same way. Count and Compare
     * go back to the borrow's first instant (the monitor's copies around this and
     * the epilogue's are a few frames at most). A load never gets here. */
    c0_set_count(c_in + 400u);
    c0_set_compare(cmp_in);
    borrow_pc_ack((op == 5u) ? pc_status : st_last_status);
    crumb(0x62u, st_last_status, sd_state);
}

void hook_tick(void) {
    if (reentry) {
        if (!reentry_marked) {
            reentry_marked = 1;
            crumb(0xEEu, c0_cause(), c0_epc());
        }
        return;
    }
    reentry = 1;
    TR_STAGE = 3; /* DIAG: C entered */
    dbg_entries++;
    if (borrowed_mode()) {
        /* the monitor took the game's PI address registers before its own cart
         * fetches and transfers replaced them (vector words 0x1D0/0x1D4, lowpage.S) */
        pi_saved_dram = *(vu32 *)0x800001D0u;
        pi_saved_cart = *(vu32 *)0x800001D4u;
    } else {
        pi_saved_dram = PI_DRAM_ADDR;
        pi_saved_cart = PI_CART_ADDR;
    }
    si_saved_dram = SI_DRAM_ADDR;
    pi_touched = 0;
    si_touched = 0;

    cart_stalled = 0;

    if (borrowed_mode()) {
        borrow_run();                     /* the cart monitor entered us for one action */
        goto out;
    }


    {
        uint32_t cz = c0_cause();
        if (((cz & CAUSE_EXC_MASK) != 0) && (((cz >> 2) & 0x1Fu) != 11u)) {
            flt_cause = cz;          /* diagnostics: a game fault (not the lazy FPU enable) */
            flt_epc = c0_epc();
            flt_bad = c0_badvaddr();
            flt_count = c0_count();
            flt_n++;
            if (flt_n <= 8u) {
                crumb(0xFAu, flt_epc, cz);   /* the first few faults of a boot: readable from BRAM after a crash */
            }
        }
        state_service(cz);           /* v6: a pending save/load takes the first clean RCP interrupt */
        if (ai_measure_pending && !st_pending && !vi_frz_frozen && (AI_STATUS & 0x40000000u) && (AI_LEN_REG >= 1536u)) {
            ai_measure_now();        /* slow-motion sound: the game's audio rate, once */
        }
        if (((cz & CAUSE_EXC_MASK) == 0) && (cz & CAUSE_IP2_RCP) && (MI_INTERRUPT & (1u << 1))) {
            pad_si_tick();           /* the game's controller block just landed */
        }
    }

    {
        SEC_T0();
        feedback_service();              /* the message into a buffer that just became the displayed one */
        SEC_END(dbg_sec_fb);
    }


    uint32_t cause = c0_cause();
    dbg_last_cause = cause;
    if ((cause & CAUSE_EXC_MASK) != 0) goto out;   /* not an interrupt */
    if (cause & CAUSE_IP4_PRENMI) goto out;        /* reset pressed: hands off */
    if (!(cause & CAUSE_IP2_RCP)) goto out;        /* RCP line not pending */
    dbg_last_mi = MI_INTERRUPT;
    if (!(dbg_last_mi & MI_INTR_VI)) goto out;     /* not a VI interrupt */

    uint32_t now = c0_count();
    if ((now - last_count) < RATE_LIMIT_TICKS) goto out;
    last_count = now;
    if (epoch == 0) {
        epoch = (now | 1u);
    }
    ticks++;
    vi_fld_record();                                 /* the set the game wrote for the field just ended */
    dbg_gate_pass++;
    TR_STAGE = 4; /* DIAG: VI gate passed */
    cart_stalled = 0; /* retry the cart each tick */
    combo_service();                                 /* v6: save/load/menu combos (pad read at SI time) */
    {
        SEC_T0();
        feedback_tick();
        SEC_END(dbg_sec_fb);
    }
    speed_hold();                                    /* slow motion / frame step: the game waits here */


    dbg_last_pi = PI_STATUS;
    if (dbg_last_pi & 3u) goto out; /* a game PI op is in flight: skip politely */
    {
        SEC_T0();
        sd_service();               /* the SD mirror in flight, if any. Only from here: its poll used
                                     * to wait on the game's own transfer (up to 2 ms in the handler,
                                     * every frame Turok 2 was streaming), and the mirror felt like lag */
        SEC_END(dbg_sec_sd);
    }

    uint32_t unlocked;
    {
        SEC_T0();
        unlocked = ensure_unlocked();
        SEC_END(dbg_sec_ul);
    }
    if (!unlocked) {
        dbg_unlock_fail++;
        goto out;
    }
    dbg_unlock_ok++;

    {
        SEC_T0();
        vpak_service();              /* the virtual Controller Pak: load once, flush when dirty */
        SEC_END(dbg_sec_vp);
    }
    if (!resume_done && ((hook_cfg.spare >> 8) & 0xFu) && !st_pending && !snap_active &&
        (((ticks >= 60u) && pad_valid) || (ticks >= 600u))) {
        /* a suspended state (the menu found its mark in the slot's file): load it as the
         * combo would, once the game is up and polling its controller (a second in at
         * least, ten at most), so its hardware is set up */
        resume_done = 1;
        uint32_t slot = ((hook_cfg.spare >> 8) & 0xFu) - 1u;
        if ((slot < hook_cfg.slots_n) && hook_cfg.slots[slot]) {
            hook_cfg.cur_slot = slot;
            resume_pending = slot + 1u;
            state_queue(STATE_OP_LOAD);
        }
    }
    /* ROM writes stay off here; rom_write_set(1) at a freeze or a snapshot only */

out:
    TR_STAGE = 5; /* DIAG: C leaving */
    if (pi_touched && pi_wait()) {
        PI_DRAM_ADDR = pi_saved_dram;
        PI_CART_ADDR = pi_saved_cart;
    }
    if (si_touched && !(SI_STATUS & 3u)) {
        SI_DRAM_ADDR = si_saved_dram;
    }
    reentry = 0;
}
