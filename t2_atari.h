/* Atari 2600 core: 6507 CPU, RIOT (6532) and TIA, with no dependencies beyond libc, so the same code runs in
 * the hook (on the dedicated server) and in the standalone test (tests/atari_core_test.c).
 *
 * Timing: every CPU bus access (including internal "dummy" cycles) advances the system by one CPU cycle =
 * three TIA colour clocks, so TIA writes land on the right colour clock and WSYNC stalls the CPU exactly to
 * the end of the scanline. One scanline = 228 colour clocks (68 hblank + 160 visible) = 76 CPU cycles.
 *
 * Output: 160 x ATARI_H RGBA pixels (top row first), produced when VSYNC ends. The visible window starts at
 * the first scanline that was not VBLANKed in the previous frame. The palettes are an approximation (YIQ
 * generated), not Stella's measured tables.
 *
 * Not emulated: audio, paddles, bank-switching schemes beyond 2K/4K/F8/FA/F6/F4, illegal opcodes other than
 * the common LAX/SAX/DCP/ISB/SLO/SRE/RLA/RRA and the NOP variants.
 */
#ifndef T2_ATARI_H
#define T2_ATARI_H

#include <stdint.h>
#include <stddef.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define ATARI_W 160
#define ATARI_H 192
#define ATARI_MAX_LINES 320
#define ATARI_CLOCKS_PER_LINE 228
#define ATARI_HBLANK 68

/* joystick / console input bits (set = pressed) */
#define ATARI_UP 1
#define ATARI_DOWN 2
#define ATARI_LEFT 4
#define ATARI_RIGHT 8
#define ATARI_FIRE 16

/* console switches (set = pressed / on) */
#define ATARI_SW_RESET 1
#define ATARI_SW_SELECT 2
#define ATARI_SW_BW 4        /* 1 = black and white */
#define ATARI_SW_P0_DIFF_A 8
#define ATARI_SW_P1_DIFF_A 16

enum { ATARI_CART_NONE = 0, ATARI_CART_2K, ATARI_CART_4K, ATARI_CART_F8, ATARI_CART_FA, ATARI_CART_F6, ATARI_CART_F4 };

typedef struct atari {
    /* CPU */
    uint16_t pc;
    uint8_t a, x, y, sp, p;
    uint64_t cycles;               /* total CPU cycles since reset */
    /* memory */
    uint8_t ram[128];
    uint8_t *rom;
    size_t rom_size;
    int cart;
    int bank;
    uint8_t cart_ram[256];         /* FA cartridge RAM */
    /* RIOT */
    uint8_t swcha_out, swacnt, swchb_out, swbcnt;
    uint8_t joy[2];                /* pressed bits per port */
    uint8_t switches;              /* ATARI_SW_* */
    int64_t timer;                 /* counts down one per CPU cycle; INTIM = timer >> shift while >= 0 */
    int timer_shift;
    int timer_underflow;           /* TIMINT flag (INSTAT bit 7) */
    /* TIA registers */
    uint8_t vsync, vblank;
    uint8_t nusiz[2], colup[2], colupf, colubk, ctrlpf, refp[2];
    uint8_t pf[3];
    uint8_t grp[2], grp_old[2];
    uint8_t enam[2], enabl, enabl_old;
    uint8_t hm[5];                 /* HMP0 HMP1 HMM0 HMM1 HMBL */
    uint8_t vdelp[2], vdelbl;
    uint8_t resmp[2];
    int pos[5];                    /* P0 P1 M0 M1 BL : pixel x of the leftmost pixel, 0..159 */
    uint16_t coll;                 /* collision latches, see ATARI_CX_* */
    int fire_latch[2];
    /* beam */
    int clk;                       /* colour clock within the line, 0..227 */
    int line;                      /* scanline within the frame */
    int wsync;
    int hmove_blank;               /* hide the first 8 pixels of this line */
    /* frame */
    uint8_t raw[ATARI_MAX_LINES][ATARI_W];   /* palette index (0..127) per pixel, 0xFF = blanked */
    uint8_t line_blank[ATARI_MAX_LINES];
    int first_visible;             /* first non-VBLANK line seen this frame */
    int prev_first_visible;
    int lines_last_frame;
    int pal;                       /* 1: PAL palette */
    int frame_ready;
    uint64_t frames;
    uint8_t idx[ATARI_W * ATARI_H];     /* palette index (0..127) per visible pixel, 0xFF = blanked */
    uint8_t rgba[ATARI_W * ATARI_H * 4];
    uint8_t palette[128][3];
    int pal_built;
} atari_t;

/* collision latch bits (internal layout; CX read registers are assembled from them) */
enum {
    CX_M0P0 = 1 << 0, CX_M0P1 = 1 << 1, CX_M1P0 = 1 << 2, CX_M1P1 = 1 << 3,
    CX_P0PF = 1 << 4, CX_P0BL = 1 << 5, CX_P1PF = 1 << 6, CX_P1BL = 1 << 7,
    CX_M0PF = 1 << 8, CX_M0BL = 1 << 9, CX_M1PF = 1 << 10, CX_M1BL = 1 << 11,
    CX_BLPF = 1 << 12, CX_P0P1 = 1 << 13, CX_M0M1 = 1 << 14
};

#define FL_C 0x01
#define FL_Z 0x02
#define FL_I 0x04
#define FL_D 0x08
#define FL_B 0x10
#define FL_U 0x20
#define FL_V 0x40
#define FL_N 0x80

/* ------------------------------------------------------------------------------------------------- palette */

static void atari_build_palette(atari_t *s) {
    /* hue 0 is grey; hues 1..15 step 24 degrees round the YIQ circle starting at gold (345 deg) */
    static const double pi = 3.14159265358979;
    for (int hue = 0; hue < 16; hue++) {
        for (int lum = 0; lum < 8; lum++) {
            double y = (hue == 0 ? 0.0 : 0.10) + (hue == 0 ? 0.95 : 0.82) * lum / 7.0;   /* grey 0 is true black */
            double i = 0, q = 0;
            if (hue > 0) {
                double deg = (s->pal ? 24.0 : 24.0) * (hue - 1) + 345.0;
                double th = deg * pi / 180.0;
                double chroma = 0.26;
                i = chroma * cos(th);
                q = chroma * sin(th);
            }
            double r = y + 0.956 * i + 0.621 * q;
            double g = y - 0.272 * i - 0.647 * q;
            double b = y - 1.106 * i + 1.703 * q;
            double v[3] = {r, g, b};
            for (int c = 0; c < 3; c++) {
                if (v[c] < 0) v[c] = 0;
                if (v[c] > 1) v[c] = 1;
                s->palette[hue * 8 + lum][c] = (uint8_t)(v[c] * 255.0 + 0.5);
            }
        }
    }
    s->pal_built = 1;
}

/* ------------------------------------------------------------------------------------------------- bus */

static void atari_tia_clock(atari_t *s);

/* one CPU cycle: three colour clocks, and the RIOT timer */
static inline void atari_tick(atari_t *s) {
    s->cycles++;
    s->timer--;
    atari_tia_clock(s);
    atari_tia_clock(s);
    atari_tia_clock(s);
}

static inline uint8_t atari_cart_read(atari_t *s, uint16_t addr) {
    addr &= 0x0FFF;
    switch (s->cart) {
    case ATARI_CART_2K: return s->rom[addr & 0x7FF];
    case ATARI_CART_4K: return s->rom[addr];
    case ATARI_CART_F8:
        if (addr == 0xFF8) s->bank = 0; else if (addr == 0xFF9) s->bank = 1;
        return s->rom[s->bank * 0x1000 + addr];
    case ATARI_CART_F6:
        if (addr >= 0xFF6 && addr <= 0xFF9) s->bank = addr - 0xFF6;
        return s->rom[s->bank * 0x1000 + addr];
    case ATARI_CART_F4:
        if (addr >= 0xFF4 && addr <= 0xFFB) s->bank = addr - 0xFF4;
        return s->rom[s->bank * 0x1000 + addr];
    case ATARI_CART_FA:
        if (addr >= 0xFF8 && addr <= 0xFFA) s->bank = addr - 0xFF8;
        if (addr < 0x100) return 0;                      /* write port */
        if (addr < 0x200) return s->cart_ram[addr - 0x100];
        return s->rom[s->bank * 0x1000 + addr];
    default: return 0xFF;
    }
}

static inline void atari_cart_write(atari_t *s, uint16_t addr, uint8_t v) {
    addr &= 0x0FFF;
    switch (s->cart) {
    case ATARI_CART_F8:
        if (addr == 0xFF8) s->bank = 0; else if (addr == 0xFF9) s->bank = 1;
        break;
    case ATARI_CART_F6:
        if (addr >= 0xFF6 && addr <= 0xFF9) s->bank = addr - 0xFF6;
        break;
    case ATARI_CART_F4:
        if (addr >= 0xFF4 && addr <= 0xFFB) s->bank = addr - 0xFF4;
        break;
    case ATARI_CART_FA:
        if (addr >= 0xFF8 && addr <= 0xFFA) s->bank = addr - 0xFF8;
        if (addr < 0x100) s->cart_ram[addr] = v;
        break;
    default: break;
    }
}

static uint8_t atari_tia_read(atari_t *s, uint8_t reg) {
    switch (reg & 0x0F) {
    case 0x0: return ((s->coll & CX_M0P1) ? 0x80 : 0) | ((s->coll & CX_M0P0) ? 0x40 : 0);
    case 0x1: return ((s->coll & CX_M1P0) ? 0x80 : 0) | ((s->coll & CX_M1P1) ? 0x40 : 0);
    case 0x2: return ((s->coll & CX_P0PF) ? 0x80 : 0) | ((s->coll & CX_P0BL) ? 0x40 : 0);
    case 0x3: return ((s->coll & CX_P1PF) ? 0x80 : 0) | ((s->coll & CX_P1BL) ? 0x40 : 0);
    case 0x4: return ((s->coll & CX_M0PF) ? 0x80 : 0) | ((s->coll & CX_M0BL) ? 0x40 : 0);
    case 0x5: return ((s->coll & CX_M1PF) ? 0x80 : 0) | ((s->coll & CX_M1BL) ? 0x40 : 0);
    case 0x6: return (s->coll & CX_BLPF) ? 0x80 : 0;
    case 0x7: return ((s->coll & CX_P0P1) ? 0x80 : 0) | ((s->coll & CX_M0M1) ? 0x40 : 0);
    case 0xC: {
        int pressed = (s->joy[0] & ATARI_FIRE) != 0;
        if (s->vblank & 0x40) { if (pressed) s->fire_latch[0] = 1; pressed = s->fire_latch[0]; }
        return pressed ? 0x00 : 0x80;
    }
    case 0xD: {
        int pressed = (s->joy[1] & ATARI_FIRE) != 0;
        if (s->vblank & 0x40) { if (pressed) s->fire_latch[1] = 1; pressed = s->fire_latch[1]; }
        return pressed ? 0x00 : 0x80;
    }
    default: return 0x00;     /* paddle inputs: nothing connected */
    }
}

static void atari_set_pos(atari_t *s, int obj) {
    int c = s->clk;
    int base = obj < 2 ? 5 : 4;            /* players take one more clock to start than missiles and the ball */
    int p;
    if (c < ATARI_HBLANK) p = obj < 2 ? 3 : 2;
    else p = (c - ATARI_HBLANK + base) % ATARI_W;
    s->pos[obj] = p;
}

static void atari_finish_frame(atari_t *s);

static void atari_tia_write(atari_t *s, uint8_t reg, uint8_t v) {
    switch (reg & 0x3F) {
    case 0x00: {
        uint8_t old = s->vsync;
        s->vsync = v;
        if ((old & 2) && !(v & 2)) atari_finish_frame(s);
        break;
    }
    case 0x01:
        s->vblank = v;
        if (!(v & 0x40)) { s->fire_latch[0] = 0; s->fire_latch[1] = 0; }
        break;
    case 0x02: s->wsync = 1; break;
    case 0x03: s->clk = ATARI_CLOCKS_PER_LINE - 3; break;   /* RSYNC: reset the beam to the end of the line */
    case 0x04: s->nusiz[0] = v; break;
    case 0x05: s->nusiz[1] = v; break;
    case 0x06: s->colup[0] = v; break;
    case 0x07: s->colup[1] = v; break;
    case 0x08: s->colupf = v; break;
    case 0x09: s->colubk = v; break;
    case 0x0A: s->ctrlpf = v; break;
    case 0x0B: s->refp[0] = v; break;
    case 0x0C: s->refp[1] = v; break;
    case 0x0D: s->pf[0] = v; break;
    case 0x0E: s->pf[1] = v; break;
    case 0x0F: s->pf[2] = v; break;
    case 0x10: atari_set_pos(s, 0); break;
    case 0x11: atari_set_pos(s, 1); break;
    case 0x12: atari_set_pos(s, 2); break;
    case 0x13: atari_set_pos(s, 3); break;
    case 0x14: atari_set_pos(s, 4); break;
    case 0x1B: s->grp[0] = v; s->grp_old[1] = s->grp[1]; break;
    case 0x1C: s->grp[1] = v; s->grp_old[0] = s->grp[0]; s->enabl_old = s->enabl; break;
    case 0x1D: s->enam[0] = v; break;
    case 0x1E: s->enam[1] = v; break;
    case 0x1F: s->enabl = v; break;
    case 0x20: s->hm[0] = v; break;
    case 0x21: s->hm[1] = v; break;
    case 0x22: s->hm[2] = v; break;
    case 0x23: s->hm[3] = v; break;
    case 0x24: s->hm[4] = v; break;
    case 0x25: s->vdelp[0] = v & 1; break;
    case 0x26: s->vdelp[1] = v & 1; break;
    case 0x27: s->vdelbl = v & 1; break;
    case 0x28: s->resmp[0] = v & 2; break;
    case 0x29: s->resmp[1] = v & 2; break;
    case 0x2A:
        /* HMOVE: motion is applied only when strobed during horizontal blank, which also lengthens it */
        if (s->clk < ATARI_HBLANK) {
            for (int i = 0; i < 5; i++) {
                int move = (int)(((s->hm[i] >> 4) ^ 8) - 8);   /* -8..+7, positive moves left */
                s->pos[i] = ((s->pos[i] - move) % ATARI_W + ATARI_W) % ATARI_W;
            }
            s->hmove_blank = 1;
        }
        break;
    case 0x2B: for (int i = 0; i < 5; i++) s->hm[i] = 0; break;
    case 0x2C: s->coll = 0; break;
    default: break;
    }
}

static void atari_timer_set(atari_t *s, int shift, uint8_t v) {
    s->timer_shift = shift;
    s->timer = ((int64_t)v << shift) - 1;
    s->timer_underflow = 0;
}

static uint8_t atari_riot_read(atari_t *s, uint16_t addr) {
    if (!(addr & 4)) {
        switch (addr & 3) {
        case 0: {
            /* SWCHA: joystick bits are active low; port 0 on the high nibble, port 1 on the low */
            uint8_t in = 0xFF;
            if (s->joy[0] & ATARI_RIGHT) in &= ~0x80;
            if (s->joy[0] & ATARI_LEFT) in &= ~0x40;
            if (s->joy[0] & ATARI_DOWN) in &= ~0x20;
            if (s->joy[0] & ATARI_UP) in &= ~0x10;
            if (s->joy[1] & ATARI_RIGHT) in &= ~0x08;
            if (s->joy[1] & ATARI_LEFT) in &= ~0x04;
            if (s->joy[1] & ATARI_DOWN) in &= ~0x02;
            if (s->joy[1] & ATARI_UP) in &= ~0x01;
            return (uint8_t)((s->swcha_out & s->swacnt) | (in & ~s->swacnt));
        }
        case 1: return s->swacnt;
        case 2: {
            uint8_t in = 0x3F;      /* bits 2, 4 and 5 are not connected and read high */
            if (s->switches & ATARI_SW_RESET) in &= ~0x01;
            if (s->switches & ATARI_SW_SELECT) in &= ~0x02;
            if (!(s->switches & ATARI_SW_BW)) in |= 0x08; else in &= ~0x08;
            if (s->switches & ATARI_SW_P0_DIFF_A) in |= 0x40;
            if (s->switches & ATARI_SW_P1_DIFF_A) in |= 0x80;
            return (uint8_t)((s->swchb_out & s->swbcnt) | (in & ~s->swbcnt));
        }
        default: return s->swbcnt;
        }
    }
    if (!(addr & 1)) {
        /* INTIM */
        s->timer_underflow = 0;
        if (s->timer >= 0) return (uint8_t)(s->timer >> s->timer_shift);
        return (uint8_t)(s->timer & 0xFF);     /* after underflow it counts down once per cycle */
    }
    /* INSTAT / TIMINT */
    return (s->timer < 0 || s->timer_underflow) ? 0x80 : 0x00;
}

static void atari_riot_write(atari_t *s, uint16_t addr, uint8_t v) {
    if (addr & 0x10) {
        static const int shifts[4] = {0, 3, 6, 10};
        atari_timer_set(s, shifts[addr & 3], v);
        return;
    }
    if (!(addr & 4)) {
        switch (addr & 3) {
        case 0: s->swcha_out = v; break;
        case 1: s->swacnt = v; break;
        case 2: s->swchb_out = v; break;
        default: s->swbcnt = v; break;
        }
    }
}

/* every access to the bus takes one cycle; the access happens at the end of it */
static inline uint8_t atari_read(atari_t *s, uint16_t addr) {
    /* WSYNC halts the CPU on its next read cycle until the end of the scanline */
    while (s->wsync) atari_tick(s);
    atari_tick(s);
    addr &= 0x1FFF;
    if (addr & 0x1000) return atari_cart_read(s, addr);
    if (!(addr & 0x80)) return atari_tia_read(s, (uint8_t)addr);
    if (!(addr & 0x200)) return s->ram[addr & 0x7F];
    return atari_riot_read(s, addr);
}

static inline void atari_write(atari_t *s, uint16_t addr, uint8_t v) {
    atari_tick(s);
    addr &= 0x1FFF;
    if (addr & 0x1000) { atari_cart_write(s, addr, v); return; }
    if (!(addr & 0x80)) { atari_tia_write(s, (uint8_t)addr, v); return; }
    if (!(addr & 0x200)) { s->ram[addr & 0x7F] = v; return; }
    atari_riot_write(s, addr, v);
}

/* ------------------------------------------------------------------------------------------------- TIA */

static inline int atari_player_bit(const atari_t *s, int n, int x) {
    uint8_t g = s->vdelp[n] ? s->grp_old[n] : s->grp[n];
    if (!g) return 0;
    uint8_t nu = s->nusiz[n] & 7;
    int scale = nu == 5 ? 2 : nu == 7 ? 4 : 1;
    static const int copies[8][3] = {{0, -1, -1}, {0, 16, -1}, {0, 32, -1}, {0, 16, 32},
                                     {0, 64, -1}, {0, -1, -1}, {0, 32, 64}, {0, -1, -1}};
    for (int c = 0; c < 3; c++) {
        int off = copies[nu][c];
        if (off < 0) continue;
        int d = ((x - s->pos[n] - off) % ATARI_W + ATARI_W) % ATARI_W;
        if (d < 8 * scale) {
            int idx = d / scale;
            int bit = (s->refp[n] & 8) ? idx : 7 - idx;
            if ((g >> bit) & 1) return 1;
        }
    }
    return 0;
}

static inline int atari_missile_bit(const atari_t *s, int n, int x) {
    if (!(s->enam[n] & 2) || s->resmp[n]) return 0;
    int size = 1 << ((s->nusiz[n] >> 4) & 3);
    int d = ((x - s->pos[2 + n]) % ATARI_W + ATARI_W) % ATARI_W;
    return d < size;
}

static inline int atari_ball_bit(const atari_t *s, int x) {
    int en = s->vdelbl ? s->enabl_old : s->enabl;
    if (!(en & 2)) return 0;
    int size = 1 << ((s->ctrlpf >> 4) & 3);
    int d = ((x - s->pos[4]) % ATARI_W + ATARI_W) % ATARI_W;
    return d < size;
}

static inline int atari_playfield_bit(const atari_t *s, int x) {
    int block = x >> 2;                 /* 40 blocks of 4 pixels */
    int bit;
    if (block >= 20) {
        block -= 20;
        if (s->ctrlpf & 1) block = 19 - block;     /* reflected right half */
    }
    if (block < 4) bit = (s->pf[0] >> (4 + block)) & 1;
    else if (block < 12) bit = (s->pf[1] >> (7 - (block - 4))) & 1;
    else bit = (s->pf[2] >> (block - 12)) & 1;
    return bit;
}

static void atari_render_pixel(atari_t *s, int x) {
    int blank = (s->vblank & 2) || (s->hmove_blank && x < 8);
    if (blank) { s->raw[s->line < ATARI_MAX_LINES ? s->line : ATARI_MAX_LINES - 1][x] = 0xFF; return; }
    /* resmp: a missile locked to its player follows the player's centre */
    for (int n = 0; n < 2; n++)
        if (s->resmp[n]) {
            uint8_t nu = s->nusiz[n] & 7;
            int scale = nu == 5 ? 2 : nu == 7 ? 4 : 1;
            s->pos[2 + n] = (s->pos[n] + (scale == 1 ? 3 : scale == 2 ? 6 : 10)) % ATARI_W;
        }
    int p0 = atari_player_bit(s, 0, x), p1 = atari_player_bit(s, 1, x);
    int m0 = atari_missile_bit(s, 0, x), m1 = atari_missile_bit(s, 1, x);
    int bl = atari_ball_bit(s, x), pf = atari_playfield_bit(s, x);
    if (m0 && p1) s->coll |= CX_M0P1;
    if (m0 && p0) s->coll |= CX_M0P0;
    if (m1 && p0) s->coll |= CX_M1P0;
    if (m1 && p1) s->coll |= CX_M1P1;
    if (p0 && pf) s->coll |= CX_P0PF;
    if (p0 && bl) s->coll |= CX_P0BL;
    if (p1 && pf) s->coll |= CX_P1PF;
    if (p1 && bl) s->coll |= CX_P1BL;
    if (m0 && pf) s->coll |= CX_M0PF;
    if (m0 && bl) s->coll |= CX_M0BL;
    if (m1 && pf) s->coll |= CX_M1PF;
    if (m1 && bl) s->coll |= CX_M1BL;
    if (bl && pf) s->coll |= CX_BLPF;
    if (p0 && p1) s->coll |= CX_P0P1;
    if (m0 && m1) s->coll |= CX_M0M1;
    uint8_t color = s->colubk;
    uint8_t pfcolor = s->colupf;
    if ((s->ctrlpf & 2) && !(s->ctrlpf & 4)) pfcolor = x < 80 ? s->colup[0] : s->colup[1];   /* score mode */
    if (s->ctrlpf & 4) {
        /* playfield and ball in front of the players */
        if (pf) color = pfcolor;
        if (bl) color = s->colupf;
        if (!(pf || bl)) {
            if (p1 || m1) color = s->colup[1];
            if (p0 || m0) color = s->colup[0];
        }
    } else {
        if (pf) color = pfcolor;
        if (bl) color = s->colupf;
        if (p1 || m1) color = s->colup[1];
        if (p0 || m0) color = s->colup[0];
    }
    s->raw[s->line < ATARI_MAX_LINES ? s->line : ATARI_MAX_LINES - 1][x] = (uint8_t)((color >> 1) & 0x7F);
}

static void atari_finish_frame(atari_t *s) {
    if (!s->pal_built) atari_build_palette(s);
    int y0 = s->prev_first_visible > 0 ? s->prev_first_visible - 1 : (s->first_visible > 0 ? s->first_visible - 1 : 34);
    if (y0 + ATARI_H > ATARI_MAX_LINES) y0 = ATARI_MAX_LINES - ATARI_H;
    for (int y = 0; y < ATARI_H; y++) {
        for (int x = 0; x < ATARI_W; x++) {
            uint8_t c = s->raw[y0 + y][x];
            s->idx[y * ATARI_W + x] = c;
            uint8_t *o = &s->rgba[(y * ATARI_W + x) * 4];
            if (c == 0xFF) { o[0] = o[1] = o[2] = 0; }
            else { o[0] = s->palette[c][0]; o[1] = s->palette[c][1]; o[2] = s->palette[c][2]; }
            o[3] = 255;
        }
    }
    s->prev_first_visible = s->first_visible;
    s->lines_last_frame = s->line;
    s->first_visible = 0;
    s->line = 0;
    memset(s->raw, 0xFF, sizeof s->raw);
    s->frame_ready = 1;
    s->frames++;
}

static void atari_tia_clock(atari_t *s) {
    if (s->clk >= ATARI_HBLANK) atari_render_pixel(s, s->clk - ATARI_HBLANK);
    s->clk++;
    if (s->clk >= ATARI_CLOCKS_PER_LINE) {
        s->clk = 0;
        s->wsync = 0;
        s->hmove_blank = 0;
        if (!(s->vblank & 2) && s->first_visible == 0) s->first_visible = s->line + 1;   /* stored as line + 1 */
        s->line++;
        if (s->line >= ATARI_MAX_LINES - 1) atari_finish_frame(s);   /* no VSYNC: do not run away */
    }
}

/* ------------------------------------------------------------------------------------------------- CPU */

#define AT_FLAG(cond, f) do { if (cond) s->p |= (f); else s->p &= (uint8_t)~(f); } while (0)

static inline void atari_nz(atari_t *s, uint8_t v) {
    s->p = (uint8_t)((s->p & ~(FL_N | FL_Z)) | (v & 0x80) | (v == 0 ? FL_Z : 0));
}

static inline uint8_t atari_fetch(atari_t *s) { return atari_read(s, s->pc++); }
static inline void atari_push(atari_t *s, uint8_t v) { atari_write(s, 0x100 | s->sp, v); s->sp--; }
static inline uint8_t atari_pull(atari_t *s) { s->sp++; return atari_read(s, 0x100 | s->sp); }

static void atari_adc(atari_t *s, uint8_t m) {
    uint8_t a = s->a;
    int c = s->p & FL_C;
    if (s->p & FL_D) {
        int low = (a & 0xF) + (m & 0xF) + c;
        if (low > 9) low += 6;
        int hi = (a & 0xF0) + (m & 0xF0) + (low > 0xF ? 0x10 : 0);
        AT_FLAG(((a + m + c) & 0xFF) == 0, FL_Z);
        AT_FLAG(hi & 0x80, FL_N);
        AT_FLAG(((a ^ hi) & (m ^ hi) & 0x80), FL_V);
        if (hi > 0x90) hi += 0x60;
        AT_FLAG(hi > 0xFF, FL_C);
        s->a = (uint8_t)((hi & 0xF0) | (low & 0x0F));
    } else {
        int r = a + m + c;
        AT_FLAG(r > 0xFF, FL_C);
        AT_FLAG(((a ^ r) & (m ^ r) & 0x80), FL_V);
        s->a = (uint8_t)r;
        atari_nz(s, s->a);
    }
}

static void atari_sbc(atari_t *s, uint8_t m) {
    uint8_t a = s->a;
    int borrow = 1 - (s->p & FL_C);
    int r = a - m - borrow;
    if (s->p & FL_D) {
        int lo = (a & 0xF) - (m & 0xF) - borrow;
        int hi = (a >> 4) - (m >> 4);
        if (lo < 0) { lo -= 6; hi--; }
        if (hi < 0) hi -= 6;
        AT_FLAG(r >= 0, FL_C);
        AT_FLAG(((a ^ m) & (a ^ r) & 0x80), FL_V);
        atari_nz(s, (uint8_t)r);
        s->a = (uint8_t)(((hi << 4) | (lo & 0xF)) & 0xFF);
    } else {
        AT_FLAG(r >= 0, FL_C);
        AT_FLAG(((a ^ m) & (a ^ r) & 0x80), FL_V);
        s->a = (uint8_t)r;
        atari_nz(s, s->a);
    }
}

static void atari_cmp(atari_t *s, uint8_t reg, uint8_t m) {
    int r = reg - m;
    AT_FLAG(reg >= m, FL_C);
    atari_nz(s, (uint8_t)r);
}

/* addressing modes: they perform the intermediate bus cycles and return the effective address */
static inline uint16_t atari_ea_zp(atari_t *s) { return atari_fetch(s); }
static inline uint16_t atari_ea_zpi(atari_t *s, uint8_t idx) {
    uint8_t z = atari_fetch(s);
    atari_read(s, z);                       /* dummy read while the index is added */
    return (uint8_t)(z + idx);
}
static inline uint16_t atari_ea_abs(atari_t *s) {
    uint16_t lo = atari_fetch(s);
    uint16_t hi = atari_fetch(s);
    return (uint16_t)(lo | (hi << 8));
}
static inline uint16_t atari_ea_absi(atari_t *s, uint8_t idx, int always) {
    uint16_t lo = atari_fetch(s);
    uint16_t hi = atari_fetch(s);
    uint16_t base = (uint16_t)(lo | (hi << 8));
    uint16_t ea = (uint16_t)(base + idx);
    if (always || (ea & 0xFF00) != (base & 0xFF00))
        atari_read(s, (uint16_t)((base & 0xFF00) | (ea & 0x00FF)));    /* page-cross / write dummy read */
    return ea;
}
static inline uint16_t atari_ea_indx(atari_t *s) {
    uint8_t z = atari_fetch(s);
    atari_read(s, z);
    z = (uint8_t)(z + s->x);
    uint16_t lo = atari_read(s, z);
    uint16_t hi = atari_read(s, (uint8_t)(z + 1));
    return (uint16_t)(lo | (hi << 8));
}
static inline uint16_t atari_ea_indy(atari_t *s, int always) {
    uint8_t z = atari_fetch(s);
    uint16_t lo = atari_read(s, z);
    uint16_t hi = atari_read(s, (uint8_t)(z + 1));
    uint16_t base = (uint16_t)(lo | (hi << 8));
    uint16_t ea = (uint16_t)(base + s->y);
    if (always || (ea & 0xFF00) != (base & 0xFF00))
        atari_read(s, (uint16_t)((base & 0xFF00) | (ea & 0x00FF)));
    return ea;
}

/* mode ids used by the decoder */
enum { M_IMM, M_ZP, M_ZPX, M_ZPY, M_ABS, M_ABSX, M_ABSY, M_INDX, M_INDY, M_ACC };

/* address for a mode; for stores and read-modify-write the indexed modes always take the extra cycle */
static uint16_t atari_ea(atari_t *s, int mode, int write) {
    switch (mode) {
    case M_IMM: return s->pc++;
    case M_ZP: return atari_ea_zp(s);
    case M_ZPX: return atari_ea_zpi(s, s->x);
    case M_ZPY: return atari_ea_zpi(s, s->y);
    case M_ABS: return atari_ea_abs(s);
    case M_ABSX: return atari_ea_absi(s, s->x, write);
    case M_ABSY: return atari_ea_absi(s, s->y, write);
    case M_INDX: return atari_ea_indx(s);
    case M_INDY: return atari_ea_indy(s, write);
    default: return 0;
    }
}

static void atari_branch(atari_t *s, int cond) {
    int8_t off = (int8_t)atari_fetch(s);
    if (cond) {
        atari_read(s, s->pc);                         /* dummy cycle for the taken branch */
        uint16_t t = (uint16_t)(s->pc + off);
        if ((t & 0xFF00) != (s->pc & 0xFF00)) atari_read(s, (uint16_t)((s->pc & 0xFF00) | (t & 0xFF)));
        s->pc = t;
    }
}

static uint8_t atari_shift_op(atari_t *s, int op, uint8_t v) {
    uint8_t r;
    switch (op) {
    case 0: AT_FLAG(v & 0x80, FL_C); r = (uint8_t)(v << 1); break;                                   /* ASL */
    case 1: { uint8_t c = s->p & FL_C; AT_FLAG(v & 0x80, FL_C); r = (uint8_t)((v << 1) | c); break; } /* ROL */
    case 2: AT_FLAG(v & 1, FL_C); r = (uint8_t)(v >> 1); break;                                      /* LSR */
    default: { uint8_t c = (s->p & FL_C) ? 0x80 : 0; AT_FLAG(v & 1, FL_C); r = (uint8_t)((v >> 1) | c); break; } /* ROR */
    }
    atari_nz(s, r);
    return r;
}

static void atari_alu(atari_t *s, int op, uint8_t m) {
    switch (op) {
    case 0: s->a |= m; atari_nz(s, s->a); break;                    /* ORA */
    case 1: s->a &= m; atari_nz(s, s->a); break;                    /* AND */
    case 2: s->a ^= m; atari_nz(s, s->a); break;                    /* EOR */
    case 3: atari_adc(s, m); break;                                 /* ADC */
    case 5: s->a = m; atari_nz(s, s->a); break;                     /* LDA */
    case 6: atari_cmp(s, s->a, m); break;                           /* CMP */
    case 7: atari_sbc(s, m); break;                                 /* SBC */
    default: break;
    }
}

/* executes one instruction (its bus cycles advance the system) */
static void atari_step(atari_t *s) {
    uint8_t op = atari_fetch(s);
    int cc = op & 3, bbb = (op >> 2) & 7, aaa = op >> 5;

    /* single-byte and control instructions */
    switch (op) {
    case 0x00: /* BRK */
        atari_fetch(s);
        atari_push(s, (uint8_t)(s->pc >> 8));
        atari_push(s, (uint8_t)s->pc);
        atari_push(s, (uint8_t)(s->p | FL_B | FL_U));
        s->p |= FL_I;
        { uint16_t lo = atari_read(s, 0xFFFE), hi = atari_read(s, 0xFFFF); s->pc = (uint16_t)(lo | (hi << 8)); }
        return;
    case 0x20: { /* JSR */
        uint16_t lo = atari_fetch(s);
        atari_read(s, 0x100 | s->sp);
        atari_push(s, (uint8_t)(s->pc >> 8));
        atari_push(s, (uint8_t)s->pc);
        uint16_t hi = atari_fetch(s);
        s->pc = (uint16_t)(lo | (hi << 8));
        return;
    }
    case 0x40: /* RTI */
        atari_read(s, s->pc); atari_read(s, 0x100 | s->sp);
        s->p = (uint8_t)((atari_pull(s) & ~FL_B) | FL_U);
        { uint16_t lo = atari_pull(s), hi = atari_pull(s); s->pc = (uint16_t)(lo | (hi << 8)); }
        return;
    case 0x60: { /* RTS */
        atari_read(s, s->pc); atari_read(s, 0x100 | s->sp);
        uint16_t lo = atari_pull(s), hi = atari_pull(s);
        s->pc = (uint16_t)(lo | (hi << 8));
        atari_read(s, s->pc);
        s->pc++;
        return;
    }
    case 0x4C: s->pc = atari_ea_abs(s); return;                      /* JMP abs */
    case 0x6C: { /* JMP (ind), with the page-wrap bug */
        uint16_t ptr = atari_ea_abs(s);
        uint16_t lo = atari_read(s, ptr);
        uint16_t hi = atari_read(s, (uint16_t)((ptr & 0xFF00) | ((ptr + 1) & 0x00FF)));
        s->pc = (uint16_t)(lo | (hi << 8));
        return;
    }
    case 0x08: atari_read(s, s->pc); atari_push(s, (uint8_t)(s->p | FL_B | FL_U)); return;     /* PHP */
    case 0x28: atari_read(s, s->pc); atari_read(s, 0x100 | s->sp); s->p = (uint8_t)((atari_pull(s) & ~FL_B) | FL_U); return; /* PLP */
    case 0x48: atari_read(s, s->pc); atari_push(s, s->a); return;                               /* PHA */
    case 0x68: atari_read(s, s->pc); atari_read(s, 0x100 | s->sp); s->a = atari_pull(s); atari_nz(s, s->a); return; /* PLA */
    default: break;
    }
    if ((op & 0x1F) == 0x10) {   /* branches */
        int flag = op >> 6;
        int want = (op >> 5) & 1;
        int cond;
        switch (flag) {
        case 0: cond = ((s->p & FL_N) != 0) == want; break;
        case 1: cond = ((s->p & FL_V) != 0) == want; break;
        case 2: cond = ((s->p & FL_C) != 0) == want; break;
        default: cond = ((s->p & FL_Z) != 0) == want; break;
        }
        atari_branch(s, cond);
        return;
    }
    if ((op & 0x0F) == 0x08 || (op & 0x0F) == 0x0A || ((op & 0x1F) == 0x1A && cc == 2)) {
        /* implied and accumulator instructions: two cycles (the second is a dummy read) */
        atari_read(s, s->pc);
        switch (op) {
        case 0x88: s->y--; atari_nz(s, s->y); return;       /* DEY */
        case 0xA8: s->y = s->a; atari_nz(s, s->y); return;  /* TAY */
        case 0xC8: s->y++; atari_nz(s, s->y); return;       /* INY */
        case 0xE8: s->x++; atari_nz(s, s->x); return;       /* INX */
        case 0x18: s->p &= (uint8_t)~FL_C; return;
        case 0x38: s->p |= FL_C; return;
        case 0x58: s->p &= (uint8_t)~FL_I; return;
        case 0x78: s->p |= FL_I; return;
        case 0x98: s->a = s->y; atari_nz(s, s->a); return;  /* TYA */
        case 0xB8: s->p &= (uint8_t)~FL_V; return;
        case 0xD8: s->p &= (uint8_t)~FL_D; return;
        case 0xF8: s->p |= FL_D; return;
        case 0x8A: s->a = s->x; atari_nz(s, s->a); return;  /* TXA */
        case 0x9A: s->sp = s->x; return;                    /* TXS */
        case 0xAA: s->x = s->a; atari_nz(s, s->x); return;  /* TAX */
        case 0xBA: s->x = s->sp; atari_nz(s, s->x); return; /* TSX */
        case 0xCA: s->x--; atari_nz(s, s->x); return;       /* DEX */
        case 0x0A: s->a = atari_shift_op(s, 0, s->a); return;
        case 0x2A: s->a = atari_shift_op(s, 1, s->a); return;
        case 0x4A: s->a = atari_shift_op(s, 2, s->a); return;
        case 0x6A: s->a = atari_shift_op(s, 3, s->a); return;
        default: return;                                    /* NOP and the unofficial NOPs */
        }
    }

    if (cc == 1) {
        /* ORA AND EOR ADC STA LDA CMP SBC */
        static const int modes[8] = {M_INDX, M_ZP, M_IMM, M_ABS, M_INDY, M_ZPX, M_ABSY, M_ABSX};
        int mode = modes[bbb];
        if (aaa == 4) {                                     /* STA (0x89 is an unofficial immediate NOP) */
            if (mode == M_IMM) { atari_fetch(s); return; }
            uint16_t ea = atari_ea(s, mode, 1);
            atari_write(s, ea, s->a);
            return;
        }
        uint16_t ea = atari_ea(s, mode, 0);
        atari_alu(s, aaa, atari_read(s, ea));
        return;
    }
    if (cc == 2) {
        /* ASL ROL LSR ROR STX LDX DEC INC */
        int mode;
        switch (bbb) {
        case 0: mode = M_IMM; break;
        case 1: mode = M_ZP; break;
        case 3: mode = M_ABS; break;
        case 5: mode = (aaa == 4 || aaa == 5) ? M_ZPY : M_ZPX; break;
        case 7: mode = (aaa == 4 || aaa == 5) ? M_ABSY : M_ABSX; break;
        default: mode = M_IMM; break;
        }
        if (aaa == 4) {                                     /* STX */
            if (mode == M_IMM) { atari_fetch(s); return; }
            atari_write(s, atari_ea(s, mode, 1), s->x);
            return;
        }
        if (aaa == 5) {                                     /* LDX */
            s->x = atari_read(s, atari_ea(s, mode, 0));
            atari_nz(s, s->x);
            return;
        }
        if (mode == M_IMM) { atari_fetch(s); return; }             /* unofficial NOP immediate */
        uint16_t ea = atari_ea(s, mode, 1);
        uint8_t v = atari_read(s, ea);
        atari_write(s, ea, v);                              /* the dummy write of the old value */
        if (aaa == 6) v--; else if (aaa == 7) v++; else v = atari_shift_op(s, aaa, v);
        if (aaa >= 6) atari_nz(s, v);
        atari_write(s, ea, v);
        return;
    }
    if (cc == 0) {
        /* BIT STY LDY CPY CPX (JMP handled above) */
        int mode;
        switch (bbb) {
        case 0: mode = M_IMM; break;
        case 1: mode = M_ZP; break;
        case 3: mode = M_ABS; break;
        case 5: mode = M_ZPX; break;
        case 7: mode = M_ABSX; break;
        default: mode = M_IMM; break;
        }
        if (aaa == 4) {                                     /* STY */
            if (mode == M_IMM) { atari_fetch(s); return; }
            atari_write(s, atari_ea(s, mode, 1), s->y);
            return;
        }
        if (aaa == 5) { s->y = atari_read(s, atari_ea(s, mode, 0)); atari_nz(s, s->y); return; }   /* LDY */
        if (aaa == 6 || aaa == 7) {
            if (mode == M_ZPX || mode == M_ABSX) { atari_read(s, atari_ea(s, mode, 0)); return; } /* NOP zp,x / abs,x */
            uint8_t m = atari_read(s, atari_ea(s, mode, 0));
            atari_cmp(s, aaa == 6 ? s->y : s->x, m);                                               /* CPY / CPX */
            return;
        }
        if (aaa == 1) {                                     /* BIT */
            if (mode != M_ZP && mode != M_ABS) { atari_read(s, atari_ea(s, mode, 0)); return; }
            uint8_t m = atari_read(s, atari_ea(s, mode, 0));
            AT_FLAG((s->a & m) == 0, FL_Z);
            s->p = (uint8_t)((s->p & ~(FL_N | FL_V)) | (m & (FL_N | FL_V)));
            return;
        }
        /* unofficial NOPs in the cc=00 column */
        if (mode == M_IMM) { atari_fetch(s); return; }
        atari_read(s, atari_ea(s, mode, 0));
        return;
    }
    /* cc == 3: the common unofficial opcodes */
    {
        static const int modes3[8] = {M_INDX, M_ZP, M_IMM, M_ABS, M_INDY, M_ZPX, M_ABSY, M_ABSX};
        int mode = modes3[bbb];
        if (mode == M_IMM) {
            /* ANC/ALR/ARR/XAA/LAX #/AXS/SBC #: only SBC #imm (0xEB) is implemented */
            uint8_t m = atari_fetch(s);
            if (op == 0xEB) atari_sbc(s, m);
            return;
        }
        if (aaa == 4 || aaa == 5) {
            if (bbb == 5) mode = M_ZPY;
            if (bbb == 7) mode = M_ABSY;
        }
        if (aaa == 4) {                                     /* SAX */
            if (mode == M_ABSY) { atari_ea(s, mode, 1); return; }
            atari_write(s, atari_ea(s, mode, 1), (uint8_t)(s->a & s->x));
            return;
        }
        if (aaa == 5) {                                     /* LAX */
            uint8_t v = atari_read(s, atari_ea(s, mode, 0));
            s->a = s->x = v;
            atari_nz(s, v);
            return;
        }
        uint16_t ea = atari_ea(s, mode, 1);
        uint8_t v = atari_read(s, ea);
        atari_write(s, ea, v);
        switch (aaa) {
        case 0: v = atari_shift_op(s, 0, v); s->a |= v; atari_nz(s, s->a); break;       /* SLO */
        case 1: v = atari_shift_op(s, 1, v); s->a &= v; atari_nz(s, s->a); break;       /* RLA */
        case 2: v = atari_shift_op(s, 2, v); s->a ^= v; atari_nz(s, s->a); break;       /* SRE */
        case 3: v = atari_shift_op(s, 3, v); atari_adc(s, v); break;                    /* RRA */
        case 6: v--; atari_cmp(s, s->a, v); break;                                      /* DCP */
        default: v++; atari_sbc(s, v); break;                                           /* ISB */
        }
        atari_write(s, ea, v);
    }
}

/* ------------------------------------------------------------------------------------------------- API */

static void atari_reset(atari_t *s) {
    uint8_t *rom = s->rom;
    size_t size = s->rom_size;
    int cart = s->cart, pal = s->pal;
    uint8_t joy0 = s->joy[0], joy1 = s->joy[1], sw = s->switches;
    memset(s, 0, sizeof *s);
    s->rom = rom;
    s->rom_size = size;
    s->cart = cart;
    s->pal = pal;
    s->joy[0] = joy0;
    s->joy[1] = joy1;
    s->switches = sw;
    s->sp = 0xFD;
    s->p = FL_I | FL_U;
    s->bank = cart == ATARI_CART_F8 ? 1 : cart == ATARI_CART_F6 ? 3 : cart == ATARI_CART_F4 ? 7 : cart == ATARI_CART_FA ? 2 : 0;
    s->timer = 1 << 20;
    s->timer_shift = 10;
    memset(s->raw, 0xFF, sizeof s->raw);
    atari_build_palette(s);
    if (s->cart != ATARI_CART_NONE) {
        uint16_t lo = atari_cart_read(s, 0x1FFC), hi = atari_cart_read(s, 0x1FFD);
        s->pc = (uint16_t)(lo | (hi << 8));
    }
}

/* copies the ROM; returns 0 on success, otherwise a message */
static const char *atari_load_rom(atari_t *s, const uint8_t *data, size_t size) {
    int cart;
    switch (size) {
    case 2048: cart = ATARI_CART_2K; break;
    case 4096: cart = ATARI_CART_4K; break;
    case 8192: cart = ATARI_CART_F8; break;
    case 12288: cart = ATARI_CART_FA; break;
    case 16384: cart = ATARI_CART_F6; break;
    case 32768: cart = ATARI_CART_F4; break;
    default: return "unsupported ROM size (2K, 4K, 8K, 12K, 16K or 32K expected)";
    }
    uint8_t *copy = (uint8_t *)malloc(size);
    if (!copy) return "out of memory";
    memcpy(copy, data, size);
    if (s->rom) free(s->rom);
    s->rom = copy;
    s->rom_size = size;
    s->cart = cart;
    atari_reset(s);
    return 0;
}

static void atari_free(atari_t *s) {
    if (s->rom) free(s->rom);
    s->rom = 0;
    s->rom_size = 0;
    s->cart = ATARI_CART_NONE;
}

static void atari_set_joystick(atari_t *s, int port, uint8_t mask) { __atomic_store_n(&s->joy[port & 1], mask, __ATOMIC_RELAXED); }
static void atari_set_switches(atari_t *s, uint8_t mask) { __atomic_store_n(&s->switches, mask, __ATOMIC_RELAXED); }

/* runs until a frame completes (or a safety limit); returns 1 when a new frame is ready */
static int atari_run_frame(atari_t *s) {
    if (s->cart == ATARI_CART_NONE) return 0;
    s->frame_ready = 0;
    uint64_t limit = s->cycles + 100000;
    while (!s->frame_ready && s->cycles < limit) atari_step(s);
    return s->frame_ready;
}

/* test helper: reads memory without cycles or side effects (RAM and cartridge only) */
static uint8_t atari_peek(const atari_t *s, uint16_t addr) {
    addr &= 0x1FFF;
    if (addr & 0x1000) {
        atari_t *w = (atari_t *)s;
        int bank = w->bank;
        uint8_t v = atari_cart_read(w, addr);
        w->bank = bank;
        return v;
    }
    if ((addr & 0x80) && !(addr & 0x200)) return s->ram[addr & 0x7F];
    return 0;
}

/* ================================================================================================= server layer
 * Runs the core on its own thread at 60 Hz (50 Hz PAL) and turns each finished frame into a coarse grid of
 * quantised colours for the force-field display. The game thread asks for what changed since it last looked
 * (atari_server_diff); the diff is returned as fixed-width text ("iiiicc" per change: cell index and colour,
 * both zero-padded decimal) so script can read it with getSubStr in constant time per change.
 */
#ifdef T2_ATARI_SERVER
#include <pthread.h>
#include <stdio.h>
#include <time.h>

#define ATARI_MAX_CELLS 4096
#define ATARI_QCOLORS 33              /* 0 is black (no block); 1..32 are the display colours */

typedef struct atari_server {
    atari_t core;
    pthread_t thread;
    pthread_mutex_t lock;
    int thread_started, stop, running;
    int cols, rows;
    uint8_t qmap[128];                /* palette index -> display colour, 0 = black */
    uint8_t qrgb[ATARI_QCOLORS][3];
    uint8_t snap[ATARI_MAX_CELLS];    /* newest frame, downsampled */
    uint8_t shown[ATARI_MAX_CELLS];   /* what the display has been told, 0xFF = unknown */
    int diff_cursor;
    char name[64];
    uint64_t frames;
} atari_server_t;

static void atari_server_build_quant(atari_server_t *v) {
    atari_t *c = &v->core;
    if (!c->pal_built) atari_build_palette(c);
    int n = 1;
    v->qrgb[0][0] = v->qrgb[0][1] = v->qrgb[0][2] = 0;
    static const int grey_lums[4] = {2, 4, 6, 7};
    for (int i = 0; i < 4; i++, n++) memcpy(v->qrgb[n], c->palette[grey_lums[i]], 3);
    for (int hue = 1; hue <= 14; hue++) {
        memcpy(v->qrgb[n++], c->palette[hue * 8 + 3], 3);
        memcpy(v->qrgb[n++], c->palette[hue * 8 + 6], 3);
    }
    for (int i = 0; i < 128; i++) {
        const uint8_t *rgb = c->palette[i];
        int best = 0, bestd = 1 << 30;
        int mx = rgb[0] > rgb[1] ? rgb[0] : rgb[1];
        if (rgb[2] > mx) mx = rgb[2];
        if (mx < 40) { v->qmap[i] = 0; continue; }          /* near black counts as background */
        for (int q = 1; q < ATARI_QCOLORS; q++) {
            int dr = rgb[0] - v->qrgb[q][0], dg = rgb[1] - v->qrgb[q][1], db = rgb[2] - v->qrgb[q][2];
            int d = dr * dr + dg * dg + db * db;
            if (d < bestd) { bestd = d; best = q; }
        }
        v->qmap[i] = (uint8_t)best;
    }
}

/* cols must divide 160 and rows must divide 192 */
static const char *atari_server_init(atari_server_t *v, int cols, int rows) {
    if (cols < 1 || rows < 1 || ATARI_W % cols || ATARI_H % rows || cols * rows > ATARI_MAX_CELLS)
        return "grid must divide 160 x 192 (for example 40 x 48 or 20 x 24)";
    memset(v, 0, sizeof *v);
    v->cols = cols;
    v->rows = rows;
    pthread_mutex_init(&v->lock, 0);
    atari_server_build_quant(v);
    memset(v->shown, 0xFF, sizeof v->shown);
    return 0;
}

/* builds the grid from the core's newest frame; the caller holds the lock */
static void atari_server_snapshot(atari_server_t *v) {
    int cw = ATARI_W / v->cols, ch = ATARI_H / v->rows;
    int total = cw * ch;
    for (int cy = 0; cy < v->rows; cy++) {
        for (int cx = 0; cx < v->cols; cx++) {
            int count[ATARI_QCOLORS] = {0};
            for (int y = 0; y < ch; y++) {
                const uint8_t *row = &v->core.idx[(cy * ch + y) * ATARI_W + cx * cw];
                for (int x = 0; x < cw; x++) count[row[x] == 0xFF ? 0 : v->qmap[row[x]]]++;
            }
            int best = 0, bestn = 0, lit = 0;
            for (int q = 1; q < ATARI_QCOLORS; q++) {
                lit += count[q];
                if (count[q] > bestn) { bestn = count[q]; best = q; }
            }
            /* thin sprites and playfield edges must not vanish: a quarter of the block is enough */
            v->snap[cy * v->cols + cx] = (uint8_t)(lit * 4 >= total ? best : 0);
        }
    }
}

static void *atari_server_thread(void *arg) {
    atari_server_t *v = (atari_server_t *)arg;
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    while (!__atomic_load_n(&v->stop, __ATOMIC_ACQUIRE)) {
        long period = v->core.pal ? 20000000L : 16666667L;
        if (__atomic_load_n(&v->running, __ATOMIC_ACQUIRE)) {
            pthread_mutex_lock(&v->lock);
            int got = atari_run_frame(&v->core);
            if (got) { atari_server_snapshot(v); v->frames++; }
            pthread_mutex_unlock(&v->lock);
        }
        next.tv_nsec += period;
        while (next.tv_nsec >= 1000000000L) { next.tv_nsec -= 1000000000L; next.tv_sec++; }
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if ((now.tv_sec - next.tv_sec) * 1000000000L + (now.tv_nsec - next.tv_nsec) > 4 * period)
            next = now;                                   /* far behind: skip instead of catching up */
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, 0);
    }
    return 0;
}

static void atari_server_start_thread(atari_server_t *v) {
    if (v->thread_started) return;
    v->stop = 0;
    if (pthread_create(&v->thread, 0, atari_server_thread, v) == 0) v->thread_started = 1;
}

static void atari_server_shutdown(atari_server_t *v) {
    if (v->thread_started) {
        __atomic_store_n(&v->stop, 1, __ATOMIC_RELEASE);
        pthread_join(v->thread, 0);
        v->thread_started = 0;
    }
    atari_free(&v->core);
}

static const char *atari_server_load(atari_server_t *v, const uint8_t *data, size_t size, const char *name) {
    pthread_mutex_lock(&v->lock);
    const char *err = atari_load_rom(&v->core, data, size);
    if (!err) {
        memset(v->shown, 0xFF, sizeof v->shown);        /* the next diff redraws every cell */
        memset(v->snap, 0, sizeof v->snap);
        snprintf(v->name, sizeof v->name, "%s", name ? name : "");
        v->frames = 0;
    }
    pthread_mutex_unlock(&v->lock);
    return err;
}

static void atari_server_reset(atari_server_t *v) {
    pthread_mutex_lock(&v->lock);
    if (v->core.cart != ATARI_CART_NONE) atari_reset(&v->core);
    pthread_mutex_unlock(&v->lock);
}

/* writes up to max changes since the last call, six characters each, and returns how many; the cursor rotates
 * so a busy region cannot starve the rest of the screen when the cap is hit */
static int atari_server_diff(atari_server_t *v, int max, char *out, size_t cap) {
    int n = 0, cells = v->cols * v->rows;
    if (max < 1) return 0;
    pthread_mutex_lock(&v->lock);
    for (int k = 0; k < cells && n < max && (size_t)(n + 1) * 6 < cap; k++) {
        int i = (v->diff_cursor + k) % cells;
        if (v->snap[i] == v->shown[i]) continue;
        v->shown[i] = v->snap[i];
        char *o = out + n * 6;                            /* fixed width: cell 0000..9999, colour 00..99 */
        o[0] = (char)('0' + (i / 1000) % 10); o[1] = (char)('0' + (i / 100) % 10);
        o[2] = (char)('0' + (i / 10) % 10);   o[3] = (char)('0' + i % 10);
        o[4] = (char)('0' + (v->snap[i] / 10) % 10); o[5] = (char)('0' + v->snap[i] % 10);
        n++;
        v->diff_cursor = (i + 1) % cells;
    }
    pthread_mutex_unlock(&v->lock);
    out[n * 6] = 0;
    return n;
}
#endif /* T2_ATARI_SERVER */

#endif /* T2_ATARI_H */
