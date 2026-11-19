/* Standalone test for t2_atari.h (no game needed):
 *   gcc -std=c99 -Wall -Wextra -pthread -o /tmp/atari_core_test tests/atari_core_test.c -lm && /tmp/atari_core_test
 */
#define _POSIX_C_SOURCE 200809L
#define T2_ATARI_SERVER 1
#include <stdio.h>
#include <unistd.h>
#include "../t2_atari.h"

static int failures, checks;

#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define CHECK_EQ(got, want, what) CHECK((long)(got) == (long)(want), "%s: got 0x%lX, want 0x%lX", what, (long)(got), (long)(want))

/* a 4K cartridge with `code` at $1000 and the reset vector pointing at it */
static void load_prog(atari_t *a, const uint8_t *code, size_t n) {
    static uint8_t rom[4096];
    memset(rom, 0xEA, sizeof rom);
    memcpy(rom, code, n);
    rom[0xFFC] = 0x00; rom[0xFFD] = 0x10;
    rom[0xFFE] = 0x00; rom[0xFFF] = 0x10;
    const char *err = atari_load_rom(a, rom, sizeof rom);
    CHECK(err == 0, "load failed: %s", err ? err : "");
}

static void run_steps(atari_t *a, int steps) { for (int i = 0; i < steps; i++) atari_step(a); }

/* ---------------------------------------------------------------------------------------------- CPU */
static void test_cpu(void) {
    atari_t a;
    memset(&a, 0, sizeof a);

    /* load, store, transfers, flags */
    static const uint8_t p1[] = {
        0xA9, 0x80,             /* LDA #$80 */
        0x85, 0x80,             /* STA $80 */
        0xAA,                   /* TAX */
        0xA8,                   /* TAY */
        0xE8,                   /* INX */
        0x88,                   /* DEY */
        0xA5, 0x80,             /* LDA $80 */
    };
    load_prog(&a, p1, sizeof p1);
    run_steps(&a, 7);
    CHECK_EQ(a.a, 0x80, "LDA/STA/LDA");
    CHECK_EQ(a.x, 0x81, "TAX INX");
    CHECK_EQ(a.y, 0x7F, "TAY DEY");
    CHECK_EQ(a.ram[0], 0x80, "STA zp");
    CHECK(a.p & FL_N, "N flag after LDA #$80");

    /* ADC / SBC, binary */
    static const uint8_t p2[] = {
        0x18, 0xA9, 0x50, 0x69, 0x50,         /* CLC LDA #$50 ADC #$50 -> $A0, V set, N set, C clear */
    };
    load_prog(&a, p2, sizeof p2);
    run_steps(&a, 3);
    CHECK_EQ(a.a, 0xA0, "ADC result");
    CHECK(a.p & FL_V, "ADC overflow");
    CHECK(!(a.p & FL_C), "ADC carry clear");
    static const uint8_t p3[] = {0x38, 0xA9, 0x10, 0xE9, 0x20};   /* SEC LDA #$10 SBC #$20 -> $F0, C clear */
    load_prog(&a, p3, sizeof p3);
    run_steps(&a, 3);
    CHECK_EQ(a.a, 0xF0, "SBC result");
    CHECK(!(a.p & FL_C), "SBC borrow");

    /* decimal mode */
    static const uint8_t p4[] = {0xF8, 0x18, 0xA9, 0x45, 0x69, 0x38};          /* SED CLC LDA #$45 ADC #$38 -> $83 */
    load_prog(&a, p4, sizeof p4);
    run_steps(&a, 4);
    CHECK_EQ(a.a, 0x83, "BCD 45+38");
    static const uint8_t p5[] = {0xF8, 0x18, 0xA9, 0x99, 0x69, 0x01};          /* 99+01 -> $00 with carry */
    load_prog(&a, p5, sizeof p5);
    run_steps(&a, 4);
    CHECK_EQ(a.a, 0x00, "BCD 99+01");
    CHECK(a.p & FL_C, "BCD carry");
    static const uint8_t p6[] = {0xF8, 0x38, 0xA9, 0x50, 0xE9, 0x21};          /* 50-21 -> $29 */
    load_prog(&a, p6, sizeof p6);
    run_steps(&a, 4);
    CHECK_EQ(a.a, 0x29, "BCD 50-21");
    CHECK(a.p & FL_C, "BCD no borrow");

    /* stack, JSR/RTS, branches: count X down to zero, calling a subroutine each time */
    /* the subroutine at $1010 increments RAM $90; layout: main at $1000, sub at $1010 */
    {
        uint8_t code[0x20];
        memset(code, 0xEA, sizeof code);
        const uint8_t main_code[] = {0xA2, 0x03, 0x20, 0x10, 0x10, 0xCA, 0xD0, 0xFA, 0x4C, 0x08, 0x10};
        const uint8_t sub[] = {0xE6, 0x90, 0x60};   /* INC $90 / RTS */
        memcpy(code, main_code, sizeof main_code);
        memcpy(code + 0x10, sub, sizeof sub);
        load_prog(&a, code, sizeof code);
        run_steps(&a, 3 * 5 + 3);     /* three loops of JSR INC RTS DEX BNE */
        CHECK_EQ(a.ram[0x10], 3, "subroutine ran three times");
        CHECK_EQ(a.x, 0, "loop counter");
        CHECK_EQ(a.sp, 0xFD, "stack balanced after JSR/RTS");
    }

    /* read-modify-write and shifts */
    static const uint8_t p8[] = {
        0xA9, 0x81, 0x85, 0x80,   /* LDA #$81 STA $80 */
        0x06, 0x80,               /* ASL $80 -> $02, C set */
        0x26, 0x80,               /* ROL $80 -> $05, C clear */
        0xE6, 0x80,               /* INC $80 -> $06 */
        0xC6, 0x80, 0xC6, 0x80,   /* DEC DEC -> $04 */
    };
    load_prog(&a, p8, sizeof p8);
    run_steps(&a, 7);
    CHECK_EQ(a.ram[0], 0x04, "ASL ROL INC DEC chain");

    /* indirect indexed: LDA ($80),Y */
    static const uint8_t p9[] = {
        0xA9, 0x90, 0x85, 0x80,   /* pointer low = $90 */
        0xA9, 0x00, 0x85, 0x81,   /* pointer high = $00 -> $0090 */
        0xA9, 0x77, 0x85, 0x91,   /* RAM[$91] = $77 */
        0xA0, 0x01,               /* LDY #1 */
        0xB1, 0x80,               /* LDA ($80),Y -> $0091 */
    };
    load_prog(&a, p9, sizeof p9);
    run_steps(&a, 8);
    CHECK_EQ(a.a, 0x77, "LDA (zp),Y");

    /* BIT and CMP */
    static const uint8_t p10[] = {
        0xA9, 0xC0, 0x85, 0x80,   /* RAM[$80] = $C0 */
        0xA9, 0x00, 0x24, 0x80,   /* BIT $80 -> Z set, N V set */
        0xA9, 0x10, 0xC9, 0x10,   /* CMP #$10 -> Z set, C set */
    };
    load_prog(&a, p10, sizeof p10);
    run_steps(&a, 4);
    CHECK((a.p & FL_Z) && (a.p & FL_N) && (a.p & FL_V), "BIT flags");
    run_steps(&a, 2);
    CHECK((a.p & FL_Z) && (a.p & FL_C), "CMP equal flags");

    /* JMP (indirect) wraps within the page */
    {
        uint8_t code[0x100];
        memset(code, 0xEA, sizeof code);
        /* the pointer lives in the cartridge so the test does not depend on RAM wrapping: */
        const uint8_t jm[] = {0x6C, 0xFF, 0x10};   /* JMP ($10FF) */
        memcpy(code, jm, sizeof jm);
        code[0xFF] = 0x20;                          /* low byte at $10FF */
        load_prog(&a, code, sizeof code);
        /* high byte comes from $1000 (page wrap) = $6C, not $1100 */
        atari_step(&a);
        CHECK_EQ(a.pc, 0x6C20, "JMP indirect page wrap");
    }

    /* unofficial LAX and SAX */
    static const uint8_t p11[] = {
        0xA9, 0x5A, 0x85, 0x80,   /* RAM[$80] = $5A */
        0xA7, 0x80,               /* LAX $80 -> A = X = $5A */
        0x87, 0x81,               /* SAX $81 -> RAM[$81] = A & X */
    };
    load_prog(&a, p11, sizeof p11);
    run_steps(&a, 4);
    CHECK(a.a == 0x5A && a.x == 0x5A, "LAX");
    CHECK_EQ(a.ram[1], 0x5A, "SAX");
    atari_free(&a);
}

/* base cycle count per official opcode (branches not taken, no page cross), 0 = not an official opcode */
static const uint8_t cycle_table[256] = {
    /*0x00*/ 7,6,0,0,0,3,5,0, 3,2,2,0,0,4,6,0,
    /*0x10*/ 2,5,0,0,0,4,6,0, 2,4,0,0,0,4,7,0,
    /*0x20*/ 6,6,0,0,3,3,5,0, 4,2,2,0,4,4,6,0,
    /*0x30*/ 2,5,0,0,0,4,6,0, 2,4,0,0,0,4,7,0,
    /*0x40*/ 6,6,0,0,0,3,5,0, 3,2,2,0,3,4,6,0,
    /*0x50*/ 2,5,0,0,0,4,6,0, 2,4,0,0,0,4,7,0,
    /*0x60*/ 6,6,0,0,0,3,5,0, 4,2,2,0,5,4,6,0,
    /*0x70*/ 2,5,0,0,0,4,6,0, 2,4,0,0,0,4,7,0,
    /*0x80*/ 0,6,0,0,3,3,3,0, 2,0,2,0,4,4,4,0,
    /*0x90*/ 2,6,0,0,4,4,4,0, 2,5,2,0,0,5,0,0,
    /*0xA0*/ 2,6,2,0,3,3,3,0, 2,2,2,0,4,4,4,0,
    /*0xB0*/ 2,5,0,0,4,4,4,0, 2,4,2,0,4,4,4,0,
    /*0xC0*/ 2,6,0,0,3,3,5,0, 2,2,2,0,4,4,6,0,
    /*0xD0*/ 2,5,0,0,0,4,6,0, 2,4,0,0,0,4,7,0,
    /*0xE0*/ 2,6,0,0,3,3,5,0, 2,2,2,0,4,4,6,0,
    /*0xF0*/ 2,5,0,0,0,4,6,0, 2,4,0,0,0,4,7,0,
};

static void test_cycles(void) {
    atari_t a;
    memset(&a, 0, sizeof a);
    int tested = 0;
    for (int op = 0; op < 256; op++) {
        if (!cycle_table[op]) continue;
        uint8_t code[4] = {(uint8_t)op, 0x80, 0x00, 0};
        load_prog(&a, code, sizeof code);
        a.ram[0] = 0x90;
        a.ram[1] = 0x00;
        a.x = 0; a.y = 0; a.sp = 0xF0;
        /* make conditional branches not taken */
        a.p = FL_U;
        if (op == 0x10) a.p |= FL_N;
        if (op == 0x50) a.p |= FL_V;
        if (op == 0x90) a.p |= FL_C;
        if (op == 0xD0) a.p |= FL_Z;
        uint64_t before = a.cycles;
        atari_step(&a);
        int got = (int)(a.cycles - before);
        CHECK(got == cycle_table[op], "opcode %02X took %d cycles, expected %d", op, got, cycle_table[op]);
        tested++;
    }
    CHECK(tested >= 150, "tested %d opcodes", tested);

    /* page-crossing reads cost one more cycle; stores always pay it */
    static const uint8_t lda_abx[] = {0xBD, 0xFF, 0x10};   /* LDA $10FF,X */
    load_prog(&a, lda_abx, sizeof lda_abx);
    a.x = 1;
    uint64_t c0 = a.cycles; atari_step(&a);
    CHECK_EQ(a.cycles - c0, 5, "LDA abs,X page cross");
    a.x = 0; a.pc = 0x1000;
    c0 = a.cycles; atari_step(&a);
    CHECK_EQ(a.cycles - c0, 4, "LDA abs,X no cross");
    static const uint8_t sta_abx[] = {0x9D, 0x80, 0x00};   /* STA $0080,X */
    load_prog(&a, sta_abx, sizeof sta_abx);
    c0 = a.cycles; atari_step(&a);
    CHECK_EQ(a.cycles - c0, 5, "STA abs,X always 5");
    /* taken branch: 3 cycles, 4 across a page */
    {
        uint8_t code[0x100];
        memset(code, 0xEA, sizeof code);
        code[0] = 0xD0; code[1] = 0x10;                       /* BNE +16, Z clear */
        load_prog(&a, code, sizeof code);
        a.p = FL_U;
        c0 = a.cycles; atari_step(&a);
        CHECK_EQ(a.cycles - c0, 3, "branch taken");
        CHECK_EQ(a.pc, 0x1012, "branch target");
    }
    {
        uint8_t code[0x100];
        memset(code, 0xEA, sizeof code);
        code[0xFD] = 0xD0; code[0xFE] = 0x10;                 /* BNE at $10FD, target $110F: crosses a page */
        static uint8_t rom[4096];
        memset(rom, 0xEA, sizeof rom);
        memcpy(rom, code, sizeof code);
        rom[0xFFC] = 0xFD; rom[0xFFD] = 0x10;
        atari_load_rom(&a, rom, sizeof rom);
        a.p = FL_U;
        c0 = a.cycles; atari_step(&a);
        CHECK_EQ(a.cycles - c0, 4, "branch taken across a page");
    }
    atari_free(&a);
}

/* ---------------------------------------------------------------------------------------------- RIOT */
static void test_riot(void) {
    atari_t a;
    memset(&a, 0, sizeof a);
    static const uint8_t p[] = {0xEA};
    load_prog(&a, p, sizeof p);
    atari_write(&a, 0x296, 5);                       /* TIM64T = 5 */
    uint8_t first = atari_read(&a, 0x284);
    CHECK(first == 4 || first == 5, "INTIM right after TIM64T is %u", first);
    for (int i = 0; i < 64; i++) atari_tick(&a);
    uint8_t later = atari_read(&a, 0x284);
    CHECK(later < first, "INTIM counts down every 64 cycles (%u -> %u)", first, later);
    for (int i = 0; i < 64 * 6; i++) atari_tick(&a);
    CHECK(atari_read(&a, 0x285) & 0x80, "TIMINT set after the interval has run out");
    uint8_t after = atari_read(&a, 0x284);
    CHECK(a.timer < 0, "timer has run past zero");
    CHECK_EQ(after, (uint8_t)(a.timer & 0xFF), "after underflow INTIM counts down from 0xFF once per cycle");

    atari_write(&a, 0x294, 100);                     /* TIM1T = 100 */
    for (int i = 0; i < 10; i++) atari_tick(&a);
    uint8_t t1 = atari_read(&a, 0x284);
    CHECK(t1 >= 85 && t1 <= 90, "TIM1T counts every cycle (0x%02X)", t1);

    /* joystick and switches */
    atari_set_joystick(&a, 0, ATARI_UP | ATARI_FIRE);
    atari_set_joystick(&a, 1, ATARI_LEFT);
    uint8_t swcha = atari_read(&a, 0x280);
    CHECK_EQ(swcha & 0xF0, 0xE0, "port 0 up pulls bit 4 low");
    CHECK_EQ(swcha & 0x0F, 0x0B, "port 1 left pulls bit 2 low");
    CHECK_EQ(atari_read(&a, 0x0C) & 0x80, 0, "fire pressed reads low on INPT4");
    CHECK_EQ(atari_read(&a, 0x0D) & 0x80, 0x80, "fire released reads high on INPT5");
    atari_set_switches(&a, ATARI_SW_RESET | ATARI_SW_SELECT);
    CHECK_EQ(atari_read(&a, 0x282) & 3, 0, "reset and select pull SWCHB low");
    atari_free(&a);
}

/* ---------------------------------------------------------------------------------------------- cartridges */
static void test_banks(void) {
    atari_t a;
    memset(&a, 0, sizeof a);
    static uint8_t rom[32768];
    struct { size_t size; int cart; uint16_t first, last; } cases[] = {
        {8192, ATARI_CART_F8, 0x1FF8, 0x1FF9},
        {16384, ATARI_CART_F6, 0x1FF6, 0x1FF9},
        {32768, ATARI_CART_F4, 0x1FF4, 0x1FFB},
    };
    for (unsigned c = 0; c < sizeof cases / sizeof cases[0]; c++) {
        size_t banks = cases[c].size / 4096;
        for (size_t b = 0; b < banks; b++) {
            memset(rom + b * 4096, (int)(0xA0 + b), 4096);
            rom[b * 4096 + 0xFFC] = 0x00;
            rom[b * 4096 + 0xFFD] = 0x10;
        }
        CHECK(atari_load_rom(&a, rom, cases[c].size) == 0, "load %zu", cases[c].size);
        CHECK_EQ(a.cart, cases[c].cart, "cart type");
        CHECK_EQ(atari_peek(&a, 0x1000), 0xA0 + banks - 1, "starts in the last bank");
        for (uint16_t addr = cases[c].first; addr <= cases[c].last; addr++) {
            atari_read(&a, addr);
            CHECK_EQ(atari_peek(&a, 0x1000), 0xA0 + (addr - cases[c].first), "switch by read");
        }
        atari_write(&a, cases[c].first, 0);
        CHECK_EQ(atari_peek(&a, 0x1000), 0xA0, "switch by write");
    }
    CHECK(atari_load_rom(&a, rom, 5000) != 0, "odd ROM sizes are refused");
    CHECK(atari_load_rom(&a, rom, 2048) == 0, "2K ROM loads");
    CHECK_EQ(atari_peek(&a, 0x1000), atari_peek(&a, 0x1800), "2K ROM mirrors");
    atari_free(&a);
}

/* ---------------------------------------------------------------------------------------------- TIA */

/* render one scanline with the registers as they are, starting from the left edge */
static void render_line(atari_t *a, int line) {
    a->line = line;
    a->clk = 0;
    for (int i = 0; i < ATARI_CLOCKS_PER_LINE - 1; i++) atari_tia_clock(a);
    a->clk = ATARI_CLOCKS_PER_LINE - 1;
    atari_tia_clock(a);        /* last clock ends the line */
    a->line = line;            /* the line counter advanced; keep reporting the same one */
}

static void tia_reset(atari_t *a) {
    memset(a, 0, sizeof *a);
    a->p = FL_U;
    memset(a->raw, 0xFF, sizeof a->raw);
    atari_build_palette(a);
}

static int first_set(const uint8_t *row, int color) {
    for (int x = 0; x < ATARI_W; x++) if (row[x] == color) return x;
    return -1;
}

static int count_color(const uint8_t *row, int color) {
    int n = 0;
    for (int x = 0; x < ATARI_W; x++) if (row[x] == color) n++;
    return n;
}

static void test_tia(void) {
    atari_t a;
    const int L = 50;

    /* background only */
    tia_reset(&a);
    a.colubk = 0x40;
    render_line(&a, L);
    CHECK_EQ(count_color(a.raw[L], 0x20), ATARI_W, "background fills the line");

    /* VBLANK blanks everything */
    tia_reset(&a);
    a.colubk = 0x40; a.vblank = 2;
    render_line(&a, L);
    CHECK_EQ(count_color(a.raw[L], 0xFF), ATARI_W, "VBLANK blanks the line");

    /* playfield: PF0 bit 4 is the first block of four pixels */
    tia_reset(&a);
    a.colubk = 0x00; a.colupf = 0x1E; a.pf[0] = 0x10;
    render_line(&a, L);
    CHECK_EQ(first_set(a.raw[L], 0x0F), 0, "PF0 bit 4 starts at pixel 0");
    CHECK_EQ(count_color(a.raw[L], 0x0F), 8, "PF0 bit 4 appears on both halves (4 + 4 pixels)");
    CHECK_EQ(a.raw[L][80], 0x0F, "non-reflected right half repeats the pattern");
    CHECK_EQ(a.raw[L][79], 0x00, "no playfield just before the right half");
    /* reflected */
    tia_reset(&a);
    a.colupf = 0x1E; a.pf[0] = 0x10; a.ctrlpf = 1;
    render_line(&a, L);
    CHECK_EQ(a.raw[L][159], 0x0F, "reflected right half mirrors the left");
    CHECK_EQ(a.raw[L][80], 0x00, "reflected right half does not repeat at its start");
    /* PF1 is MSB first, PF2 LSB first */
    tia_reset(&a);
    a.colupf = 0x1E; a.pf[1] = 0x80;
    render_line(&a, L);
    CHECK_EQ(first_set(a.raw[L], 0x0F), 16, "PF1 bit 7 is block 4");
    tia_reset(&a);
    a.colupf = 0x1E; a.pf[2] = 0x01;
    render_line(&a, L);
    CHECK_EQ(first_set(a.raw[L], 0x0F), 48, "PF2 bit 0 is block 12");
    /* score mode */
    tia_reset(&a);
    a.colup[0] = 0x44; a.colup[1] = 0x88; a.colupf = 0x1E; a.pf[0] = 0xF0; a.ctrlpf = 2;
    render_line(&a, L);
    CHECK_EQ(a.raw[L][0], 0x22, "score mode left half uses COLUP0");
    CHECK_EQ(a.raw[L][80], 0x44, "score mode right half uses COLUP1");

    /* player: RESP sets the position from the colour clock, the sprite is 8 pixels, MSB first */
    tia_reset(&a);
    a.clk = 100;
    atari_tia_write(&a, 0x10, 0);
    CHECK_EQ(a.pos[0], 100 - 68 + 5, "RESP0 position");
    a.colup[0] = 0x2E; a.grp[0] = 0xFF;
    render_line(&a, L);
    CHECK_EQ(first_set(a.raw[L], 0x17), 37, "player starts at its position");
    CHECK_EQ(count_color(a.raw[L], 0x17), 8, "player is eight pixels wide");
    tia_reset(&a);
    a.clk = 100; atari_tia_write(&a, 0x10, 0);
    a.colup[0] = 0x2E; a.grp[0] = 0x80;
    render_line(&a, L);
    CHECK_EQ(first_set(a.raw[L], 0x17), 37, "GRP0 bit 7 is the leftmost pixel");
    tia_reset(&a);
    a.clk = 100; atari_tia_write(&a, 0x10, 0);
    a.colup[0] = 0x2E; a.grp[0] = 0x01; a.refp[0] = 8;
    render_line(&a, L);
    CHECK_EQ(first_set(a.raw[L], 0x17), 37, "reflected GRP0 bit 0 is the leftmost pixel");
    /* hblank strobe */
    tia_reset(&a);
    a.clk = 10; atari_tia_write(&a, 0x10, 0);
    CHECK_EQ(a.pos[0], 3, "RESP0 in hblank lands at pixel 3");
    atari_tia_write(&a, 0x14, 0);
    CHECK_EQ(a.pos[4], 2, "RESBL in hblank lands at pixel 2");
    /* copies and sizes */
    tia_reset(&a);
    a.clk = 100; atari_tia_write(&a, 0x10, 0);
    a.colup[0] = 0x2E; a.grp[0] = 0x80; a.nusiz[0] = 1;
    render_line(&a, L);
    CHECK_EQ(count_color(a.raw[L], 0x17), 2, "two copies, close");
    CHECK_EQ(a.raw[L][37 + 16], 0x17, "second copy is 16 pixels on");
    tia_reset(&a);
    a.clk = 100; atari_tia_write(&a, 0x10, 0);
    a.colup[0] = 0x2E; a.grp[0] = 0x80; a.nusiz[0] = 3;
    render_line(&a, L);
    CHECK_EQ(count_color(a.raw[L], 0x17), 3, "three copies, close");
    tia_reset(&a);
    a.clk = 100; atari_tia_write(&a, 0x10, 0);
    a.colup[0] = 0x2E; a.grp[0] = 0x80; a.nusiz[0] = 5;
    render_line(&a, L);
    CHECK_EQ(count_color(a.raw[L], 0x17), 2, "double-size sprite pixel is two wide");
    tia_reset(&a);
    a.clk = 100; atari_tia_write(&a, 0x10, 0);
    a.colup[0] = 0x2E; a.grp[0] = 0x80; a.nusiz[0] = 7;
    render_line(&a, L);
    CHECK_EQ(count_color(a.raw[L], 0x17), 4, "quad-size sprite pixel is four wide");
    /* vertical delay uses the old copy */
    tia_reset(&a);
    a.clk = 100; atari_tia_write(&a, 0x10, 0);
    a.colup[0] = 0x2E; a.vdelp[0] = 1;
    atari_tia_write(&a, 0x1B, 0xFF);        /* new GRP0 */
    CHECK_EQ(count_color(a.raw[L], 0x17), 0, "nothing yet");
    render_line(&a, L);
    CHECK_EQ(count_color(a.raw[L], 0x17), 0, "VDELP0 still shows the old (empty) GRP0");
    atari_tia_write(&a, 0x1C, 0x00);        /* writing GRP1 latches GRP0 into the old copy */
    render_line(&a, L + 1);
    CHECK_EQ(count_color(a.raw[L + 1], 0x17), 8, "VDELP0 shows the latched GRP0 after a GRP1 write");

    /* missile and ball sizes */
    tia_reset(&a);
    a.clk = 100; atari_tia_write(&a, 0x12, 0);
    a.colup[0] = 0x2E; a.enam[0] = 2; a.nusiz[0] = 0x20;     /* size 4 */
    render_line(&a, L);
    CHECK_EQ(count_color(a.raw[L], 0x17), 4, "missile size 4");
    CHECK_EQ(a.pos[2], 100 - 68 + 4, "missile position");
    tia_reset(&a);
    a.clk = 100; atari_tia_write(&a, 0x14, 0);
    a.colupf = 0x3E; a.enabl = 2; a.ctrlpf = 0x30;        /* size 8 */
    render_line(&a, L);
    CHECK_EQ(count_color(a.raw[L], 0x1F), 8, "ball size 8");

    /* priority: players over playfield, unless CTRLPF bit 2 */
    tia_reset(&a);
    a.clk = 100; atari_tia_write(&a, 0x10, 0);
    a.colup[0] = 0x2E; a.grp[0] = 0xFF; a.colupf = 0x1E; a.pf[1] = 0xFF;
    render_line(&a, L);
    CHECK_EQ(a.raw[L][40], 0x17, "player in front of the playfield");
    CHECK_EQ(a.raw[L][20], 0x0F, "playfield visible where there is no player");
    tia_reset(&a);
    a.clk = 100; atari_tia_write(&a, 0x10, 0);
    a.colup[0] = 0x2E; a.grp[0] = 0xFF; a.colupf = 0x1E; a.pf[1] = 0xFF; a.ctrlpf = 4;
    render_line(&a, L);
    CHECK_EQ(a.raw[L][40], 0x0F, "playfield priority puts it in front of the player");

    /* collisions are latched and cleared by CXCLR */
    tia_reset(&a);
    a.clk = 100; atari_tia_write(&a, 0x10, 0);
    a.colup[0] = 0x2E; a.grp[0] = 0xFF; a.pf[1] = 0xFF;
    render_line(&a, L);
    CHECK_EQ(atari_tia_read(&a, 0x02) & 0x80, 0x80, "CXP0FB: P0 hit the playfield");
    CHECK_EQ(atari_tia_read(&a, 0x07) & 0xC0, 0, "no P0-P1 collision");
    atari_tia_write(&a, 0x2C, 0);
    CHECK_EQ(atari_tia_read(&a, 0x02), 0, "CXCLR clears the latch");
    tia_reset(&a);
    a.clk = 100; atari_tia_write(&a, 0x10, 0); a.clk = 100; atari_tia_write(&a, 0x11, 0);
    a.grp[0] = 0xFF; a.grp[1] = 0xFF;
    render_line(&a, L);
    CHECK_EQ(atari_tia_read(&a, 0x07) & 0x80, 0x80, "CXPPMM: P0 and P1 overlap");

    /* HMOVE: motion applies only in hblank; the first eight pixels are blanked */
    tia_reset(&a);
    a.clk = 100; atari_tia_write(&a, 0x10, 0);
    a.hm[0] = 0x70;                           /* 7 to the left */
    a.clk = 10; atari_tia_write(&a, 0x2A, 0);
    CHECK_EQ(a.pos[0], 37 - 7, "HMP0 +7 left");
    a.hm[0] = 0x80;                           /* 8 to the right */
    a.clk = 10; atari_tia_write(&a, 0x2A, 0);
    CHECK_EQ(a.pos[0], 37 - 7 + 8, "HMP0 8 right");
    a.colubk = 0x40;
    render_line(&a, L);
    a.hmove_blank = 1;
    a.line = L; a.clk = 0;
    for (int i = 0; i < ATARI_CLOCKS_PER_LINE - 1; i++) atari_tia_clock(&a);
    CHECK_EQ(a.raw[L][0], 0xFF, "HMOVE blanks the left edge");
    CHECK_EQ(a.raw[L][8], 0x20, "HMOVE blanking is eight pixels");
    atari_tia_write(&a, 0x2B, 0);
    CHECK_EQ(a.hm[0], 0, "HMCLR");
}

/* ---------------------------------------------------------------------------------------------- whole program */

/* a minimal frame loop: VSYNC, 37 blank lines, 192 lines of colour bars with a sprite, 30 overscan lines */
static void emit(uint8_t *rom, int *n, const uint8_t *b, int len) { memcpy(rom + *n, b, (size_t)len); *n += len; }

static void test_program(void) {
    atari_t a;
    memset(&a, 0, sizeof a);
    static uint8_t rom[4096];
    memset(rom, 0xEA, sizeof rom);
    int n = 0;
    const uint8_t init[] = {0x78, 0xD8, 0xA2, 0xFF, 0x9A};                    /* SEI CLD LDX #$FF TXS */
    emit(rom, &n, init, sizeof init);
    int frame = n;
    const uint8_t vsync[] = {
        0xA9, 0x02, 0x85, 0x00,                 /* LDA #2 STA VSYNC */
        0x85, 0x02, 0x85, 0x02, 0x85, 0x02,     /* 3 x STA WSYNC */
        0xA9, 0x00, 0x85, 0x00,                 /* LDA #0 STA VSYNC */
        0xA9, 0x02, 0x85, 0x01,                 /* VBLANK on */
        0xA2, 0x25,                             /* LDX #37 */
        0x85, 0x02, 0xCA, 0xD0, 0xFB,           /* vb: STA WSYNC DEX BNE vb */
        0xA9, 0x00, 0x85, 0x01,                 /* VBLANK off */
        0xA9, 0x1E, 0x85, 0x08,                 /* COLUPF = $1E */
        0xA9, 0xFF, 0x85, 0x0F,                 /* PF2 = $FF */
        0xA9, 0x00, 0x85, 0x09,                 /* COLUBK = 0 */
        0xA2, 0xC0,                             /* LDX #192 */
        0x85, 0x02, 0x8A, 0x85, 0x09,           /* line: STA WSYNC TXA STA COLUBK  (bar colour follows the line number) */
        0xCA, 0xD0, 0xF8,                       /* DEX BNE line */
        0xA9, 0x02, 0x85, 0x01,                 /* VBLANK on (overscan) */
        0xA2, 0x1E,                             /* LDX #30 */
        0x85, 0x02, 0xCA, 0xD0, 0xFB,           /* os: STA WSYNC DEX BNE os */
        0x4C, (uint8_t)(0x00 + frame), 0x10,    /* JMP frame */
    };
    emit(rom, &n, vsync, sizeof vsync);
    rom[0xFFC] = 0x00; rom[0xFFD] = 0x10;
    CHECK(atari_load_rom(&a, rom, sizeof rom) == 0, "program loads");
    int got = 0;
    for (int f = 0; f < 4; f++) got += atari_run_frame(&a);
    CHECK_EQ(got, 4, "four frames produced");
    CHECK_EQ(a.frames, 4, "frame counter");
    CHECK(a.lines_last_frame >= 259 && a.lines_last_frame <= 264, "frame is %d lines", a.lines_last_frame);
    /* playfield PF2 = $FF: right half of the left side and the whole right half are lit with COLUPF */
    int row = 100;
    const uint8_t *px = &a.rgba[(row * ATARI_W + 60) * 4];
    CHECK(px[0] == a.palette[0x1E >> 1][0] && px[1] == a.palette[0x1E >> 1][1], "playfield pixel colour in the frame");
    /* the bar colour changes line to line: two different rows differ at pixel 0 (no playfield there) */
    const uint8_t *r1 = &a.rgba[(10 * ATARI_W + 0) * 4];
    const uint8_t *r2 = &a.rgba[(12 * ATARI_W + 0) * 4];     /* two rows apart: the palette drops the low bit */
    CHECK(memcmp(r1, r2, 3) != 0, "colour bars differ between lines");
    CHECK_EQ(memcmp(&a.rgba[(10 * ATARI_W + 0) * 4], &a.rgba[(10 * ATARI_W + 20) * 4], 3), 0, "a bar is the same colour across the line outside the playfield cells");
    CHECK_EQ(r1[3], 255, "opaque alpha");
    atari_free(&a);
}

static void test_wsync(void) {
    atari_t a;
    memset(&a, 0, sizeof a);
    static const uint8_t p[] = {0x85, 0x02, 0xEA, 0xEA};      /* STA WSYNC, NOP */
    load_prog(&a, p, sizeof p);
    atari_step(&a);                       /* the write only sets the flag */
    CHECK(a.wsync, "WSYNC pending");
    int line = a.line;
    atari_step(&a);                       /* the NOP's first read waits for the end of the line */
    CHECK_EQ(a.line, line + 1, "WSYNC runs to the next line");
    CHECK(a.clk <= 6, "the CPU resumes at the start of the line (clk %d)", a.clk);
    atari_free(&a);
}


/* ---------------------------------------------------------------------------------------------- server layer */
static void test_server(void) {
    static atari_server_t v;
    CHECK(atari_server_init(&v, 7, 48) != 0, "a grid that does not divide the frame is refused");
    CHECK(atari_server_init(&v, 40, 48) == 0, "40 x 48 grid is accepted");
    /* the same kind of program as test_program: bright playfield bars on a black background */
    static uint8_t rom[4096];
    memset(rom, 0xEA, sizeof rom);
    int n = 0;
    const uint8_t init[] = {0x78, 0xD8, 0xA2, 0xFF, 0x9A};
    emit(rom, &n, init, sizeof init);
    int frame = n;
    const uint8_t body[] = {
        0xA9, 0x02, 0x85, 0x00, 0x85, 0x02, 0x85, 0x02, 0x85, 0x02, 0xA9, 0x00, 0x85, 0x00,
        0xA9, 0x02, 0x85, 0x01, 0xA2, 0x25, 0x85, 0x02, 0xCA, 0xD0, 0xFB, 0xA9, 0x00, 0x85, 0x01,
        0xA9, 0x1E, 0x85, 0x08, 0xA9, 0xF0, 0x85, 0x0D,          /* PF0 = $F0: a bar at each side */
        0xA2, 0xC0, 0x85, 0x02, 0xCA, 0xD0, 0xFB,
        0xA9, 0x02, 0x85, 0x01, 0xA2, 0x1E, 0x85, 0x02, 0xCA, 0xD0, 0xFB,
        0x4C, (uint8_t)frame, 0x10,
    };
    emit(rom, &n, body, sizeof body);
    rom[0xFFC] = 0x00; rom[0xFFD] = 0x10;
    CHECK(atari_server_load(&v, rom, sizeof rom, "bars.bin") == 0, "server load");
    char out[ATARI_MAX_CELLS * 6 + 8];
    CHECK_EQ(atari_server_diff(&v, 10, out, sizeof out), 10, "a freshly loaded ROM redraws every cell (capped at 10)");
    __atomic_store_n(&v.running, 1, __ATOMIC_RELEASE);
    atari_server_start_thread(&v);
    { struct timespec ts = {0, 300000000L}; nanosleep(&ts, 0); }
    CHECK(v.frames >= 5, "the thread runs frames (%llu)", (unsigned long long)v.frames);
    CHECK(v.frames <= 30, "the thread is paced to roughly 60 Hz (%llu frames in 0.3 s)", (unsigned long long)v.frames);
    int total = 0;
    for (int round = 0; round < 400; round++) {
        int got = atari_server_diff(&v, 500, out, sizeof out);
        if (!got) break;
        for (int i = 0; i < got; i++) {
            char cell[5] = {out[i * 6], out[i * 6 + 1], out[i * 6 + 2], out[i * 6 + 3], 0};
            int idx = atoi(cell);
            CHECK(idx >= 0 && idx < v.cols * v.rows, "cell index %d in range", idx);
        }
        total += got;
    }
    CHECK(total >= v.cols * v.rows - 10, "all cells have been reported once (%d)", total);
    /* nothing changes in a static picture */
    CHECK_EQ(atari_server_diff(&v, 500, out, sizeof out), 0, "a static picture produces no further changes");
    /* PF0 = $F0 lights the first four 4-pixel blocks (cells 0..3) and, unreflected, repeats at cells 20..23 */
    CHECK(v.snap[24 * v.cols + 0] != 0, "left bar is lit");
    CHECK(v.snap[24 * v.cols + 3] != 0, "left bar is four cells wide");
    CHECK_EQ(v.snap[24 * v.cols + 10], 0, "between the bars is black");
    CHECK(v.snap[24 * v.cols + 20] != 0, "the pattern repeats on the right half");
    CHECK_EQ(v.snap[24 * v.cols + 39], 0, "the far right is black (not reflected)");
    /* a joystick press goes through to the core */
    atari_set_joystick(&v.core, 0, ATARI_FIRE);
    CHECK_EQ(v.core.joy[0], ATARI_FIRE, "joystick state");
    atari_server_shutdown(&v);
}

int main(void) {
    test_cpu();
    test_cycles();
    test_riot();
    test_banks();
    test_tia();
    test_program();
    test_wsync();
    test_server();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
