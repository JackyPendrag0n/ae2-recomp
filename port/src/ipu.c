/* IPU (the PS2's MPEG-2 decoder) for FMV playback.
 *
 * The game plays its movies with Sony's libmpeg, which parses the stream headers itself and
 * hands the IPU one command at a time: VDEC/FDEC to read symbols and fields, BDEC to decode a
 * macroblock's DCT blocks, CSC to turn finished YCbCr macroblocks into RGB, plus SETIQ/SETTH/
 * BCLR for setup. Data comes in through an 8-qword input FIFO (fed by DMA channel 4, toIPU) and
 * results leave through an 8-qword output FIFO (drained by channel 3, fromIPU).
 *
 * The decoder below is written as straight-line code running in a coroutine: whenever it needs
 * input that is not there yet, or the output FIFO is full, it yields back to the game, and it is
 * resumed when the game feeds or drains the FIFOs (by DMA or by FIFO register access). The FIFO
 * and DMA bookkeeping follow the hardware closely because libmpeg reads IPU_BP / IPU_CTRL and
 * the DMA registers back to know how much of the stream the IPU has consumed.
 *
 * VLC tables: ISO/IEC 13818-2 Annex B. Colour conversion: the IPU's documented BT.601 integer
 * method (Y bias 16, coefficients in 1/64ths). */
#include "rt.h"
#include "platform.h"
#include <math.h>

uint32_t *hw_regp(uint32_t a);
uint8_t *hw_dma_mem(uint32_t addr);
void hw_dma_done(int ch);

/* ------------------------------------------------------------------ registers */
#define R_CMD   0x10002000u
#define R_CTRL  0x10002010u
#define R_BP    0x10002020u
#define R_TOP   0x10002030u
#define D3      0x1000B000u     /* fromIPU */
#define D4      0x1000B400u     /* toIPU */
#define CHCR(b) (*hw_regp(b))
#define MADR(b) (*hw_regp((b) + 0x10))
#define QWC(b)  (*hw_regp((b) + 0x20))
#define TADR(b) (*hw_regp((b) + 0x30))
#define ASR0(b) (*hw_regp((b) + 0x40))
#define ASR1(b) (*hw_regp((b) + 0x50))

enum { C_BCLR, C_IDEC, C_BDEC, C_VDEC, C_FDEC, C_SETIQ, C_SETVQ, C_CSC, C_PACK, C_SETTH };

static struct {
    uint32_t cmd_data, cmd_busy;
    uint32_t ctrl;                       /* bits we keep: ECD 14, SCD 15, IDP 16-17, AS 20, IVF 21, QST 22, MP1 23, PCT 24-26, BUSY 31 */
    uint32_t top, top_busy;
    int cbp;
    /* input: FIFO plus the decoder's 2-qword internal buffer */
    uint8_t fifo[8][16]; int fr, ifc;
    uint8_t ib[2][16]; int fp, bp;       /* fp: qwords in ib; bp: bit position in ib (0..255) */
    /* output FIFO */
    uint8_t ofifo[8][16]; int or_, ofc;
    /* toIPU chain state */
    int d4_tag_end;                      /* the current tag ends the chain */
    int d4_tag_active;                   /* QWC belongs to a tag already read */
    /* tables */
    uint8_t iq[64], niq[64];
    uint16_t vqclut[16];
    uint16_t th0, th1;
    int dc_pred[3];
    uint32_t cur;                        /* command being executed */
} ipu;

#define CTRL_ECD (1u << 14)
#define CTRL_SCD (1u << 15)
#define CTRL_BUSY (1u << 31)
static int ctrl_idp(void) { return (ipu.ctrl >> 16) & 3; }
static int ctrl_as(void) { return (ipu.ctrl >> 20) & 1; }
static int ctrl_ivf(void) { return (ipu.ctrl >> 21) & 1; }
static int ctrl_qst(void) { return (ipu.ctrl >> 22) & 1; }
static int ctrl_mp1(void) { return (ipu.ctrl >> 23) & 1; }
static int ctrl_pct(void) { return (ipu.ctrl >> 24) & 7; }

/* ------------------------------------------------------------------ coroutine */
static Coro *co_ipu;         /* the decoder coroutine (platform.c) */
static int co_running;      /* a command is in progress inside the coroutine */
static int co_wait;         /* 1: waiting for input, 2: waiting for output space */
static void yield(int why) { co_wait = why; coro_yield(co_ipu); co_wait = 0; }

/* ------------------------------------------------------------------ DMA */
static int d4_active(void) { return (CHCR(D4) & 0x100) != 0; }
/* read the next toIPU tag; returns 0 when the chain is over */
static int d4_next_tag(void) {
    if (ipu.d4_tag_end) return 0;
    uint32_t t = TADR(D4);
    uint8_t *p = hw_dma_mem(t);
    uint32_t lo, hi; memcpy(&lo, p, 4); memcpy(&hi, p + 4, 4);
    uint32_t q = lo & 0xffff, id = (lo >> 28) & 7, irq = lo >> 31, addr = hi & 0x7ffffff0u;
    uint32_t chcr = CHCR(D4);
    CHCR(D4) = (chcr & 0xffff) | (lo & 0xffff0000u);
    QWC(D4) = q;
    int asp = (chcr >> 4) & 3;
    switch (id) {
    case 0: MADR(D4) = addr; TADR(D4) = t + 16; ipu.d4_tag_end = 1; break;          /* refe */
    case 1: MADR(D4) = t + 16; TADR(D4) = t + 16 + q * 16; break;                  /* cnt */
    case 2: MADR(D4) = t + 16; TADR(D4) = addr; break;                             /* next */
    case 3: case 4: MADR(D4) = addr; TADR(D4) = t + 16; break;                     /* ref, refs */
    case 5:                                                                         /* call */
        MADR(D4) = t + 16;
        if (asp == 0) ASR0(D4) = t + 16 + q * 16; else ASR1(D4) = t + 16 + q * 16;
        CHCR(D4) = (CHCR(D4) & ~0x30u) | ((uint32_t)(asp + 1) << 4);
        TADR(D4) = addr; break;
    case 6:                                                                         /* ret */
        MADR(D4) = t + 16;
        if (asp > 0) { asp--; TADR(D4) = asp ? ASR1(D4) : ASR0(D4); CHCR(D4) = (CHCR(D4) & ~0x30u) | ((uint32_t)asp << 4); }
        else { TADR(D4) = t + 16 + q * 16; ipu.d4_tag_end = 1; }
        break;
    case 7: MADR(D4) = t + 16; TADR(D4) = t + 16 + q * 16; ipu.d4_tag_end = 1; break; /* end */
    }
    if (irq && (chcr & 0x80)) ipu.d4_tag_end = 1;
    ipu.d4_tag_active = 1;
    return 1;
}
static void d4_finish(void) {
    CHCR(D4) &= ~0x100u;
    ipu.d4_tag_active = 0;
    hw_dma_done(4);
}
/* move data from the toIPU channel into the input FIFO; returns qwords moved */
static int d4_fill(void) {
    int moved = 0;
    while (ipu.ifc < 8 && d4_active()) {
        int chain = ((CHCR(D4) >> 2) & 3) == 1;
        if ((QWC(D4) & 0xffff) == 0) {
            if (!chain) { d4_finish(); break; }
            if (ipu.d4_tag_active && ipu.d4_tag_end) { d4_finish(); break; }
            if (!d4_next_tag()) { d4_finish(); break; }
            continue;
        }
        memcpy(ipu.fifo[(ipu.fr + ipu.ifc) & 7], hw_dma_mem(MADR(D4)), 16);
        ipu.ifc++; moved++;
        MADR(D4) += 16; QWC(D4) = (QWC(D4) - 1) & 0xffff;
        if (chain) {   /* cnt/end tags advance TADR along with MADR */
            uint32_t id = (CHCR(D4) >> 28) & 7;
            if (id == 1 || id == 7) TADR(D4) = MADR(D4);
        }
        if ((QWC(D4) & 0xffff) == 0 && (!chain || ipu.d4_tag_end)) { d4_finish(); break; }
    }
    return moved;
}
/* drain the output FIFO into the fromIPU channel (normal mode) */
static int d3_drain(void) {
    int moved = 0;
    while (ipu.ofc > 0 && (CHCR(D3) & 0x100) && (QWC(D3) & 0xffff)) {
        memcpy(hw_dma_mem(MADR(D3)), ipu.ofifo[ipu.or_], 16);
        ipu.or_ = (ipu.or_ + 1) & 7; ipu.ofc--; moved++;
        MADR(D3) += 16; QWC(D3) = (QWC(D3) - 1) & 0xffff;
        if ((QWC(D3) & 0xffff) == 0) { CHCR(D3) &= ~0x100u; hw_dma_done(3); }
    }
    return moved;
}

/* ------------------------------------------------------------------ bit reader */
/* make sure the internal buffer holds `bits` bits past bp (at most 32 + the current word) */
static void need(int bits) {
    while (ipu.fp * 128 < ipu.bp + bits) {
        if (!ipu.ifc) d4_fill();
        if (!ipu.ifc) { yield(1); continue; }
        memcpy(ipu.ib[ipu.fp], ipu.fifo[ipu.fr], 16);
        ipu.fr = (ipu.fr + 1) & 7; ipu.ifc--; ipu.fp++;
        d4_fill();
    }
}
/* the next n (<= 32) bits, left-aligned in 32, without consuming them; waits only for n bits */
static uint32_t peekn(int n) {
    need(n);
    int byte = ipu.bp >> 3, sh = ipu.bp & 7, have = ipu.fp * 16;
    const uint8_t *b = &ipu.ib[0][0];
    uint64_t v = 0;
    for (int i = 0; i < 5; i++) v = (v << 8) | (byte + i < have ? b[byte + i] : 0);
    return (uint32_t)(v >> (8 - sh));
}
static uint32_t peek32(void) { return peekn(32); }
static uint32_t ubits(int n) { return n ? peekn(n) >> (32 - n) : 0; }
static int32_t sbits(int n) { return n ? (int32_t)peekn(n) >> (32 - n) : 0; }
static void dump(int n) {
    ipu.bp += n;
    while (ipu.bp >= 128 && ipu.fp > 0) {
        memcpy(ipu.ib[0], ipu.ib[1], 16);
        ipu.fp--; ipu.bp -= 128;
    }
    if (ipu.bp >= 128 && ipu.fp == 0) {
        /* skipping past data that has not arrived: pull it in and drop it */
        while (ipu.bp >= 128) { need(1); memcpy(ipu.ib[0], ipu.ib[1], 16); ipu.fp--; ipu.bp -= 128; }
    }
}
static uint32_t gbits(int n) { uint32_t v = ubits(n); dump(n); return v; }

/* ------------------------------------------------------------------ output */
static void out_qw(const void *q) {
    while (ipu.ofc == 8) { d3_drain(); if (ipu.ofc == 8) yield(2); }
    memcpy(ipu.ofifo[(ipu.or_ + ipu.ofc) & 7], q, 16);
    ipu.ofc++;
    d3_drain();
}
static void out_block(const void *p, int qwords) { for (int i = 0; i < qwords; i++) out_qw((const uint8_t *)p + i * 16); }

/* ------------------------------------------------------------------ VLC tables (ISO 13818-2 Annex B) */
typedef struct { const char *code; int a, b; } VlcDef;
typedef struct { int16_t a, b; uint8_t len; } VlcEnt;
typedef struct { int bits; VlcEnt *e; } Vlc;
/* build a direct lookup on `bits` leading bits; codes may contain spaces */
static void vlc_build(Vlc *v, const VlcDef *d, int n, int bits) {
    v->bits = bits; v->e = calloc((size_t)1 << bits, sizeof(VlcEnt));
    for (int i = 0; i < n; i++) {
        uint32_t c = 0; int l = 0;
        for (const char *s = d[i].code; *s; s++) if (*s == '0' || *s == '1') { c = (c << 1) | (uint32_t)(*s - '0'); l++; }
        if (l > bits) { fprintf(stderr, "[ipu] VLC code too long\n"); continue; }
        uint32_t lo = c << (bits - l), hi = lo + (1u << (bits - l));
        for (uint32_t k = lo; k < hi; k++) { v->e[k].a = (int16_t)d[i].a; v->e[k].b = (int16_t)d[i].b; v->e[k].len = (uint8_t)l; }
    }
}
static const VlcEnt *vlc_get(const Vlc *v) { return &v->e[ubits(v->bits)]; }

/* macroblock_address_increment: value 1..33, 34 stuffing, 35 escape */
static const VlcDef d_mbai[] = {
    {"1",1}, {"011",2}, {"010",3}, {"0011",4}, {"0010",5}, {"00011",6}, {"00010",7},
    {"0000111",8}, {"0000110",9}, {"00001011",10}, {"00001010",11}, {"00001001",12}, {"00001000",13},
    {"00000111",14}, {"00000110",15}, {"0000010111",16}, {"0000010110",17}, {"0000010101",18},
    {"0000010100",19}, {"0000010011",20}, {"0000010010",21}, {"00000100011",22}, {"00000100010",23},
    {"00000100001",24}, {"00000100000",25}, {"00000011111",26}, {"00000011110",27}, {"00000011101",28},
    {"00000011100",29}, {"00000011011",30}, {"00000011010",31}, {"00000011001",32}, {"00000011000",33},
    {"00000001111",34}, {"00000001000",35},
};
/* macroblock_type: bit0 intra, 1 pattern, 2 backward, 3 forward, 4 quant */
static const VlcDef d_mbt_i[] = { {"1",0x01}, {"01",0x11} };
static const VlcDef d_mbt_p[] = { {"1",0x0a}, {"01",0x02}, {"001",0x08}, {"00011",0x01}, {"00010",0x1a}, {"00001",0x12}, {"000001",0x11} };
static const VlcDef d_mbt_b[] = { {"10",0x0c}, {"11",0x0e}, {"010",0x04}, {"011",0x06}, {"0010",0x08}, {"0011",0x0a},
                                  {"00011",0x01}, {"00010",0x1e}, {"000011",0x1a}, {"000010",0x16}, {"000001",0x11} };
static const VlcDef d_cbp[] = {
    {"111",60}, {"1101",4}, {"1100",8}, {"1011",16}, {"1010",32}, {"10011",12}, {"10010",48}, {"10001",20}, {"10000",40},
    {"01111",28}, {"01110",44}, {"01101",52}, {"01100",56}, {"01011",1}, {"01010",61}, {"01001",2}, {"01000",62},
    {"001111",24}, {"001110",36}, {"001101",3}, {"001100",63}, {"0010111",5}, {"0010110",9}, {"0010101",17}, {"0010100",33},
    {"0010011",6}, {"0010010",10}, {"0010001",18}, {"0010000",34}, {"00011111",7}, {"00011110",11}, {"00011101",19},
    {"00011100",35}, {"00011011",13}, {"00011010",49}, {"00011001",21}, {"00011000",41}, {"00010111",14}, {"00010110",50},
    {"00010101",22}, {"00010100",42}, {"00010011",15}, {"00010010",51}, {"00010001",23}, {"00010000",43}, {"00001111",25},
    {"00001110",37}, {"00001101",26}, {"00001100",38}, {"00001011",29}, {"00001010",45}, {"00001001",53}, {"00001000",57},
    {"00000111",30}, {"00000110",46}, {"00000101",54}, {"00000100",58}, {"000000111",31}, {"000000110",47}, {"000000101",55},
    {"000000100",59}, {"000000011",27}, {"000000010",39}, {"000000001",0},
};
/* motion_code magnitude (the sign bit follows) */
static const VlcDef d_mv[] = {
    {"1",0}, {"01",1}, {"001",2}, {"0001",3}, {"000011",4}, {"0000101",5}, {"0000100",6}, {"0000011",7},
    {"000001011",8}, {"000001010",9}, {"000001001",10}, {"0000010001",11}, {"0000010000",12},
    {"0000001111",13}, {"0000001110",14}, {"0000001101",15}, {"0000001100",16},
};
static const VlcDef d_dcl[] = { {"100",0}, {"00",1}, {"01",2}, {"101",3}, {"110",4}, {"1110",5}, {"11110",6}, {"111110",7},
                                {"1111110",8}, {"11111110",9}, {"111111110",10}, {"111111111",11} };
static const VlcDef d_dcc[] = { {"00",0}, {"01",1}, {"10",2}, {"110",3}, {"1110",4}, {"11110",5}, {"111110",6}, {"1111110",7},
                                {"11111110",8}, {"111111110",9}, {"1111111110",10}, {"1111111111",11} };
/* DCT coefficients: run, level (sign bit follows); run 64 = end of block, 65 = escape */
#define EOB 64
#define ESC 65
static const VlcDef d_dct_common[] = {   /* codes of 12+ bits, shared by both tables */
    {"000000011101",0,8}, {"000000011000",0,9}, {"000000010011",0,10}, {"000000010000",0,11}, {"000000011011",1,5},
    {"000000010100",2,4}, {"000000011100",3,3}, {"000000010010",4,3}, {"000000011110",6,2}, {"000000010101",7,2},
    {"000000010001",8,2}, {"000000011111",17,1}, {"000000011010",18,1}, {"000000011001",19,1}, {"000000010111",20,1},
    {"000000010110",21,1}, {"0000000011010",0,12}, {"0000000011001",0,13}, {"0000000011000",0,14}, {"0000000010111",0,15},
    {"0000000010110",1,6}, {"0000000010101",1,7}, {"0000000010100",2,5}, {"0000000010011",3,4}, {"0000000010010",5,3},
    {"0000000010001",9,2}, {"0000000010000",10,2}, {"0000000011111",22,1}, {"0000000011110",23,1}, {"0000000011101",24,1},
    {"0000000011100",25,1}, {"0000000011011",26,1},
    {"00000000011111",0,16}, {"00000000011110",0,17}, {"00000000011101",0,18}, {"00000000011100",0,19}, {"00000000011011",0,20},
    {"00000000011010",0,21}, {"00000000011001",0,22}, {"00000000011000",0,23}, {"00000000010111",0,24}, {"00000000010110",0,25},
    {"00000000010101",0,26}, {"00000000010100",0,27}, {"00000000010011",0,28}, {"00000000010010",0,29}, {"00000000010001",0,30},
    {"00000000010000",0,31}, {"000000000011000",0,32}, {"000000000010111",0,33}, {"000000000010110",0,34}, {"000000000010101",0,35},
    {"000000000010100",0,36}, {"000000000010011",0,37}, {"000000000010010",0,38}, {"000000000010001",0,39}, {"000000000010000",0,40},
    {"000000000011111",1,8}, {"000000000011110",1,9}, {"000000000011101",1,10}, {"000000000011100",1,11}, {"000000000011011",1,12},
    {"000000000011010",1,13}, {"000000000011001",1,14}, {"0000000000010011",1,15}, {"0000000000010010",1,16}, {"0000000000010001",1,17},
    {"0000000000010000",1,18}, {"0000000000010100",6,3}, {"0000000000011010",11,2}, {"0000000000011001",12,2}, {"0000000000011000",13,2},
    {"0000000000010111",14,2}, {"0000000000010110",15,2}, {"0000000000010101",16,2}, {"0000000000011111",27,1}, {"0000000000011110",28,1},
    {"0000000000011101",29,1}, {"0000000000011100",30,1}, {"0000000000011011",31,1},
};
static const VlcDef d_dct0[] = {   /* table B.14 (short codes); "1" for (0,1) is the first-coefficient form */
    {"10",EOB,0}, {"11",0,1}, {"011",1,1}, {"0100",0,2}, {"0101",2,1}, {"00101",0,3}, {"00111",3,1}, {"00110",4,1},
    {"000110",1,2}, {"000111",5,1}, {"000101",6,1}, {"000100",7,1}, {"0000110",0,4}, {"0000100",2,2}, {"0000111",8,1},
    {"0000101",9,1}, {"000001",ESC,0}, {"00100110",0,5}, {"00100001",0,6}, {"00100101",1,3}, {"00100100",3,2},
    {"00100111",10,1}, {"00100011",11,1}, {"00100010",12,1}, {"00100000",13,1}, {"0000001010",0,7}, {"0000001100",1,4},
    {"0000001011",2,3}, {"0000001111",4,2}, {"0000001001",5,2}, {"0000001110",14,1}, {"0000001101",15,1}, {"0000001000",16,1},
};
static const VlcDef d_dct1[] = {   /* table B.15 (intra_vlc_format = 1) */
    {"0110",EOB,0}, {"10",0,1}, {"010",1,1}, {"110",0,2}, {"00101",2,1}, {"0111",0,3}, {"00111",3,1}, {"000110",4,1},
    {"00110",1,2}, {"000111",5,1}, {"0000110",6,1}, {"0000100",7,1}, {"11100",0,4}, {"0000111",2,2}, {"0000101",8,1},
    {"1111000",9,1}, {"000001",ESC,0}, {"11101",0,5}, {"000101",0,6}, {"1111001",1,3}, {"00100110",3,2}, {"1111010",10,1},
    {"00100001",11,1}, {"00100101",12,1}, {"00100100",13,1}, {"000100",0,7}, {"00100111",1,4}, {"11111100",2,3},
    {"11111101",4,2}, {"000000100",5,2}, {"000000101",14,1}, {"000000111",15,1}, {"0000001101",16,1}, {"1111011",0,8},
    {"1111100",0,9}, {"00100011",0,10}, {"00100010",0,11}, {"00100000",1,5}, {"0000001100",2,4}, {"11111010",0,12},
    {"11111011",0,13}, {"11111110",0,14}, {"11111111",0,15},
};
static Vlc v_mbai, v_mbt[3], v_cbp, v_mv, v_dcl, v_dcc, v_dct0, v_dct0f, v_dct1;
static void vlc_build_dct(Vlc *v, const VlcDef *a, int na, int first) {
    VlcDef all[160]; int n = 0;
    for (int i = 0; i < na; i++) all[n++] = a[i];
    for (int i = 0; i < (int)(sizeof d_dct_common / sizeof d_dct_common[0]); i++) {
        /* table one drops the 12/13-bit codes it gives shorter codes to */
        int dup = 0; for (int k = 0; k < na; k++) if (a[k].a == d_dct_common[i].a && a[k].b == d_dct_common[i].b) dup = 1;
        if (!dup) all[n++] = d_dct_common[i];
    }
    if (first) { for (int i = 0; i < n; i++) if (all[i].a == EOB) { all[i] = all[--n]; break; }
                 for (int i = 0; i < n; i++) if (all[i].a == 0 && all[i].b == 1) all[i].code = "1"; }
    vlc_build(v, all, n, 16);
}
static void tables_init(void) {
    static int done; if (done) return; done = 1;
    #define N(x) (int)(sizeof x / sizeof x[0])
    vlc_build(&v_mbai, d_mbai, N(d_mbai), 11);
    vlc_build(&v_mbt[0], d_mbt_i, N(d_mbt_i), 6); vlc_build(&v_mbt[1], d_mbt_p, N(d_mbt_p), 6); vlc_build(&v_mbt[2], d_mbt_b, N(d_mbt_b), 6);
    vlc_build(&v_cbp, d_cbp, N(d_cbp), 9);
    vlc_build(&v_mv, d_mv, N(d_mv), 10);
    vlc_build(&v_dcl, d_dcl, N(d_dcl), 9); vlc_build(&v_dcc, d_dcc, N(d_dcc), 10);
    vlc_build_dct(&v_dct0, d_dct0, N(d_dct0), 0);
    vlc_build_dct(&v_dct0f, d_dct0, N(d_dct0), 1);
    vlc_build_dct(&v_dct1, d_dct1, N(d_dct1), 0);
}

/* ------------------------------------------------------------------ blocks */
static const uint8_t zigzag[64] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63 };
static const uint8_t altscan[64] = {
    0, 8, 16, 24, 1, 9, 2, 10, 17, 25, 32, 40, 48, 56, 57, 49, 41, 33, 26, 18, 3, 11, 4, 12, 19, 27, 34, 42, 50, 58, 35, 43,
    51, 59, 20, 28, 5, 13, 6, 14, 21, 29, 36, 44, 52, 60, 37, 45, 53, 61, 22, 30, 7, 15, 23, 31, 38, 46, 54, 62, 39, 47, 55, 63 };
static const uint8_t nonlinear_q[32] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 14, 16, 18, 20, 22, 24, 28, 32, 36, 40, 44, 48, 52,
                                         56, 64, 72, 80, 88, 96, 104, 112 };
static int quant_scale(int code) { return ctrl_qst() ? nonlinear_q[code & 31] : (code & 31) * 2; }

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static int dc_diff(int chroma) {
    const VlcEnt *e = vlc_get(chroma ? &v_dcc : &v_dcl);
    dump(e->len);
    int size = e->a;
    if (!size) return 0;
    int d = (int)gbits(size);
    if (!(d & (1 << (size - 1)))) d -= (1 << size) - 1;
    return d;
}
/* coefficients of one block into blk (natural order); returns 1 if any AC/DC nonzero */
static void read_block(int16_t *blk, int intra, int cc, int qs) {
    const uint8_t *scan = ctrl_as() ? altscan : zigzag;
    const uint8_t *qm = intra ? ipu.iq : ipu.niq;
    int mpeg1 = ctrl_mp1();
    int i = 0;
    if (intra) {
        ipu.dc_pred[cc] += dc_diff(cc != 0);
        blk[0] = (int16_t)(ipu.dc_pred[cc] << (3 - ctrl_idp()));
        i = 1;
    }
    const Vlc *tab = intra && ctrl_ivf() && !mpeg1 ? &v_dct1 : &v_dct0;
    int first = !intra;   /* a non-intra block's first coefficient has its own short code */
    for (;;) {
        const VlcEnt *e = vlc_get(first ? &v_dct0f : tab);
        first = 0;
        if (!e->len) break;                          /* invalid code: stop the block */
        dump(e->len);
        if (e->a == EOB) break;
        int run, val;
        if (e->a == ESC) {
            run = (int)gbits(6);
            if (mpeg1) {
                int l = sbits(8); dump(8);
                if (l == -128) { l = (int)gbits(8) - 256; } else if (l == 0) { l = (int)gbits(8); }
                val = l;
            } else { val = sbits(12); dump(12); }
        } else {
            run = e->a; val = e->b;
            if (gbits(1)) val = -val;
        }
        i += run;
        if (i > 63) break;
        int j = scan[i];
        int v;   /* the IPU keeps the quantiser matrices in scan order (as transmitted) */
        if (intra) v = (val * qs * qm[i]) / 16;      /* MPEG-2: (2 * level * qs * W) / 32 */
        else v = ((2 * val + (val > 0 ? 1 : val < 0 ? -1 : 0)) * qs * qm[i]) / 32;
        if (mpeg1 && v) v = (v & 1) ? v : v - (v > 0 ? 1 : -1);   /* oddification */
        blk[j] = (int16_t)clampi(v, -2048, 2047);
        i++;
    }
    if (!mpeg1) {   /* mismatch control */
        int sum = 0; for (int k = 0; k < 64; k++) sum += blk[k];
        if (!(sum & 1)) blk[63] ^= 1;
    }
}
/* separable 8x8 inverse DCT (double precision reference form) */
static double idct_c[8][8];
static void idct(int16_t *blk, int16_t *out) {
    static int init; if (!init) { init = 1; for (int u = 0; u < 8; u++) for (int x = 0; x < 8; x++) idct_c[x][u] = (u ? 1.0 : M_SQRT1_2) * cos((2 * x + 1) * u * M_PI / 16) / 2; }
    double tmp[64];
    for (int y = 0; y < 8; y++) {
        const int16_t *r = blk + y * 8;
        int nz = 0; for (int u = 0; u < 8; u++) nz |= r[u];
        for (int x = 0; x < 8; x++) {
            double s = 0;
            if (nz) for (int u = 0; u < 8; u++) s += idct_c[x][u] * r[u];
            tmp[y * 8 + x] = s;
        }
    }
    for (int x = 0; x < 8; x++) for (int y = 0; y < 8; y++) {
        double s = 0;
        for (int v = 0; v < 8; v++) s += idct_c[y][v] * tmp[v * 8 + x];
        out[y * 8 + x] = (int16_t)clampi((int)floor(s + 0.5), -256, 255);
    }
}

/* ------------------------------------------------------------------ macroblocks */
typedef struct { int16_t y[16][16], cb[8][8], cr[8][8]; } MB16;
typedef struct { uint8_t y[16][16], cb[8][8], cr[8][8]; } MB8;
static void put_block(MB16 *mb, int b, const int16_t *px, int field) {
    for (int r = 0; r < 8; r++) for (int c = 0; c < 8; c++) {
        int16_t v = px[r * 8 + c];
        if (b < 4) {
            int yy = field ? (b >> 1) + r * 2 : (b >> 1) * 8 + r, xx = (b & 1) * 8 + c;
            mb->y[yy][xx] = v;
        } else if (b == 4) mb->cb[r][c] = v;
        else mb->cr[r][c] = v;
    }
}
/* decode the blocks of one macroblock (after its header) into mb */
static void decode_mb_blocks(MB16 *mb, int intra, int cbp, int field, int qs) {
    memset(mb, 0, sizeof *mb);
    for (int b = 0; b < 6; b++) {
        if (!intra && !(cbp & (32 >> b))) continue;
        int16_t blk[64] = {0}, px[64];
        read_block(blk, intra, b < 4 ? 0 : b - 3, qs);
        idct(blk, px);
        if (intra) for (int k = 0; k < 64; k++) px[k] = (int16_t)clampi(px[k], 0, 255);
        put_block(mb, b, px, field);
    }
}
/* YCbCr (8-bit) -> RGB32 with the IPU's integer formula, then thresholds */
static void csc_rgb32(const MB8 *m, uint32_t *rgb) {
    for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) {
        int lum = (0x95 * (m->y[y][x] > 16 ? m->y[y][x] - 16 : 0)) >> 6;
        int cb = m->cb[y >> 1][x >> 1] - 128, cr = m->cr[y >> 1][x >> 1] - 128;
        int r = clampi((lum + ((0xcc * cr) >> 6) + 1) >> 1, 0, 255);
        int g = clampi((lum + ((-0x68 * cr) >> 6) + ((-0x32 * cb) >> 6) + 1) >> 1, 0, 255);
        int b = clampi((lum + ((0x102 * cb) >> 6) + 1) >> 1, 0, 255);
        uint32_t a = 0x80;
        if (ipu.th0 && r < ipu.th0 && g < ipu.th0 && b < ipu.th0) { rgb[y * 16 + x] = 0; continue; }
        if (ipu.th1 && r < ipu.th1 && g < ipu.th1 && b < ipu.th1) a = 0x40;
        rgb[y * 16 + x] = (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | (a << 24);
    }
}
static void rgb32_to_16(const uint32_t *rgb, uint16_t *o, int dither) {
    static const int dm[4][4] = { {-4, 0, -3, 1}, {2, -2, 3, -1}, {-3, 1, -4, 0}, {3, -1, 2, -2} };
    for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) {
        uint32_t c = rgb[y * 16 + x];
        int d = dither ? dm[y & 3][x & 3] : 0;
        int r = clampi((int)(c & 255) + d, 0, 255) >> 3, g = clampi((int)((c >> 8) & 255) + d, 0, 255) >> 3, b = clampi((int)((c >> 16) & 255) + d, 0, 255) >> 3;
        int a = (c >> 24) == 0x40 ? 0 : (c >> 24) ? 1 : 0;
        o[y * 16 + x] = (uint16_t)(r | (g << 5) | (b << 10) | (a << 15));
    }
}
/* after a macroblock: look for a start code at the next byte boundary (SCD) or garbage (ECD) */
static void check_start_code(void) {
    if (ubits(8) == 0) {
        ipu.bp = (ipu.bp + 7) & ~7; dump(0);
        for (;;) {
            uint32_t s = ubits(24);
            if (s) { if (s == 1) ipu.ctrl |= CTRL_SCD; else ipu.ctrl |= CTRL_ECD; break; }
            dump(8);
        }
    }
    ipu.top = peek32();
}

/* ------------------------------------------------------------------ commands */
static void cmd_bdec(uint32_t v) {
    int intra = (v >> 27) & 1, dcr = (v >> 26) & 1, field = (v >> 25) & 1, qsc = (v >> 16) & 31;
    if (dcr) ipu.dc_pred[0] = ipu.dc_pred[1] = ipu.dc_pred[2] = 128 << ctrl_idp();
    ipu.ctrl &= ~CTRL_ECD;
    int cbp = 0x3f;
    if (!intra) { const VlcEnt *e = vlc_get(&v_cbp); dump(e->len); cbp = e->a; }
    ipu.cbp = cbp;
    static MB16 mb;
    decode_mb_blocks(&mb, intra, cbp, field, quant_scale(qsc));
    ipu.ctrl &= ~CTRL_SCD;
    out_block(&mb, sizeof mb / 16);
    check_start_code();
}
static void cmd_idec(uint32_t v) {
    int qsc = (v >> 16) & 31, dtd = (v >> 24) & 1, sgn = (v >> 25) & 1, dte = (v >> 26) & 1, ofm = (v >> 27) & 1;
    int qs = quant_scale(qsc);
    ipu.ctrl = (ipu.ctrl & ~(7u << 24)) | (1u << 24);   /* PCT = I */
    ipu.dc_pred[0] = ipu.dc_pred[1] = ipu.dc_pred[2] = 128 << ctrl_idp();
    ipu.ctrl &= ~CTRL_ECD;
    static MB16 mb; static MB8 m8; static uint32_t rgb[256]; static uint16_t rgb16[256];
    for (;;) {
        const VlcEnt *e = vlc_get(&v_mbt[0]); dump(e->len);
        int modes = e->a;
        if (!e->len) { ipu.ctrl |= CTRL_ECD; break; }
        int field = dtd ? (int)gbits(1) : 0;
        if (modes & 0x10) qs = quant_scale((int)gbits(5));
        decode_mb_blocks(&mb, 1, 0x3f, field, qs);
        for (int i = 0; i < 256; i++) (&m8.y[0][0])[i] = (uint8_t)(&mb.y[0][0])[i];
        for (int i = 0; i < 64; i++) { (&m8.cb[0][0])[i] = (uint8_t)(&mb.cb[0][0])[i]; (&m8.cr[0][0])[i] = (uint8_t)(&mb.cr[0][0])[i]; }
        csc_rgb32(&m8, rgb);
        if (sgn) for (int i = 0; i < 256; i++) rgb[i] ^= 0x808080u;
        if (ofm) { rgb32_to_16(rgb, rgb16, dte); out_block(rgb16, 32); } else out_block(rgb, 64);
        /* next macroblock address increment: anything but "1" (no skips in I slices) ends the slice */
        int inc = 0;
        for (;;) {
            if (ubits(11) == 8) { dump(11); inc += 33; continue; }     /* escape */
            if (ubits(11) == 15) { dump(11); continue; }               /* stuffing */
            const VlcEnt *a = vlc_get(&v_mbai);
            if (!a->len || a->a > 33) { inc = -1; break; }
            dump(a->len); inc += a->a; break;
        }
        if (inc < 0) break;
        if (inc > 1) ipu.dc_pred[0] = ipu.dc_pred[1] = ipu.dc_pred[2] = 128 << ctrl_idp();
    }
    ipu.cbp = 0x3f;
    ipu.ctrl &= ~(CTRL_SCD | CTRL_ECD);
    check_start_code();
}
static void cmd_vdec(uint32_t v) {
    int tbl = (v >> 26) & 3;
    uint32_t data = 0;
    switch (tbl) {
    case 0: {   /* macroblock address increment */
        const VlcEnt *e = vlc_get(&v_mbai);
        if (e->len) { dump(e->len); data = (uint32_t)e->a | ((uint32_t)e->len << 16); }
        break;
    }
    case 1: {   /* macroblock type for the picture type in IPU_CTRL */
        int pct = ctrl_pct(); if (pct < 1 || pct > 3) pct = 1;
        const VlcEnt *e = vlc_get(&v_mbt[pct - 1]);
        if (e->len) {
            dump(e->len);
            data = (uint32_t)e->a;
            if (pct == 2 && (e->a & 0x08)) data |= 0x80;                     /* frame motion type */
            if (pct == 3) data |= 0x80 | ((uint32_t)e->len << 16);
        }
        break;
    }
    case 2: {   /* motion code */
        const VlcEnt *e = vlc_get(&v_mv);
        if (e->len) {
            dump(e->len);
            if (e->a == 0) data = 0x00010000;
            else { int neg = (int)gbits(1); int d = neg ? -e->a : e->a; data = (uint32_t)d | ((uint32_t)e->len << 16); }
        }
        break;
    }
    case 3: {   /* dual-prime vector */
        uint32_t b = ubits(2);
        if (b < 2) { dump(1); data = 0x00010000; }
        else { dump(2); data = (b == 2 ? 1u : 0xffffffffu) | (2u << 16); }
        break;
    }
    }
    ipu.cmd_data = data;
    if (data == 0) ipu.ctrl |= CTRL_ECD; else ipu.ctrl &= ~CTRL_ECD;
    ipu.top = peek32();
}
static void cmd_setiq(uint32_t v) {
    uint8_t *m = (v >> 27) & 1 ? ipu.niq : ipu.iq;
    for (int i = 0; i < 64; i++) m[i] = (uint8_t)gbits(8);
}
static void cmd_setvq(void) { for (int i = 0; i < 16; i++) { uint16_t lo = (uint16_t)gbits(8), hi = (uint16_t)gbits(8); ipu.vqclut[i] = (uint16_t)(lo | (hi << 8)); } }
static void cmd_csc(uint32_t v) {
    int mbc = v & 0x7ff, dte = (v >> 26) & 1, ofm = (v >> 27) & 1;
    static MB8 m; static uint32_t rgb[256]; static uint16_t rgb16[256];
    for (int n = 0; n < mbc; n++) {
        uint8_t *p = &m.y[0][0];
        for (int i = 0; i < 384; i++) p[i] = (uint8_t)gbits(8);
        csc_rgb32(&m, rgb);
        if (ofm) { rgb32_to_16(rgb, rgb16, dte); out_block(rgb16, 32); } else out_block(rgb, 64);
    }
}
static void cmd_pack(uint32_t v) {
    int mbc = v & 0x7ff, dte = (v >> 26) & 1, ofm = (v >> 27) & 1;
    static uint32_t rgb[256]; static uint16_t rgb16[256]; static uint8_t idx[128];
    for (int n = 0; n < mbc; n++) {
        for (int i = 0; i < 256; i++) { uint32_t c = 0; for (int k = 0; k < 4; k++) c |= gbits(8) << (8 * k); rgb[i] = c; }
        rgb32_to_16(rgb, rgb16, dte);
        if (ofm) { out_block(rgb16, 32); continue; }
        for (int i = 0; i < 256; i += 2) {   /* nearest VQ CLUT entry for each pixel, two per byte */
            int best[2] = {0, 0};
            for (int h = 0; h < 2; h++) {
                int bd = 1 << 30; uint16_t c = rgb16[i + h];
                for (int k = 0; k < 16; k++) {
                    uint16_t q = ipu.vqclut[k];
                    int dr = (c & 31) - (q & 31), dg = ((c >> 5) & 31) - ((q >> 5) & 31), db = ((c >> 10) & 31) - ((q >> 10) & 31);
                    int d = dr * dr + dg * dg + db * db;
                    if (d < bd) { bd = d; best[h] = k; }
                }
            }
            idx[i / 2] = (uint8_t)(best[0] | (best[1] << 4));
        }
        out_block(idx, 8);
    }
}

static int pending_skip;
static void co_entry(void) {
    for (;;) {
        uint32_t v = ipu.cur;
        if (pending_skip) { need(pending_skip); dump(pending_skip); pending_skip = 0; }
        switch (v >> 28) {
        case C_IDEC: cmd_idec(v); break;
        case C_BDEC: cmd_bdec(v); break;
        case C_VDEC: cmd_vdec(v); break;
        case C_FDEC: ipu.cmd_data = peek32(); ipu.top = ipu.cmd_data; break;
        case C_SETIQ: cmd_setiq(v); break;
        case C_SETVQ: cmd_setvq(); break;
        case C_CSC: cmd_csc(v); break;
        case C_PACK: cmd_pack(v); break;
        }
        co_running = 0;
        coro_yield(co_ipu);   /* finished; the next command restarts the loop */
    }
}

/* ------------------------------------------------------------------ driving */
static int ipulog = -1;
static void finish_cmd(void) {
    { static int n; if (ipulog > 1 && n++ < 400000) INFO("[ipu]   done %08x -> data %08x top %08x ctrl %08x bp %d fp %d ifc %d\n", ipu.cur, ipu.cmd_data, ipu.top, ipu.ctrl, ipu.bp, ipu.fp, ipu.ifc); }
    ipu.ctrl &= ~CTRL_BUSY;
    ipu.cmd_busy = 0; ipu.top_busy = 0;
    kernel_raise_intc(8);
}
/* run until nothing moves: DMA in, coroutine, DMA out */
void ipu_tick(void) {
    if (ipulog > 0) { static int n; if (++n % 50 == 0)
        INFO("[ipu] tick: cmd %08x running %d wait %d ifc %d fp %d bp %d ofc %d ctrl %08x | d3 chcr %08x madr %08x qwc %x | d4 chcr %08x madr %08x qwc %x tadr %08x end %d\n",
             ipu.cur, co_running, co_wait, ipu.ifc, ipu.fp, ipu.bp, ipu.ofc, ipu.ctrl, CHCR(D3), MADR(D3), QWC(D3), CHCR(D4), MADR(D4), QWC(D4), TADR(D4), ipu.d4_tag_end); }
}
static void pump(void) {
    if (ipulog > 0) { static uint64_t t0; uint64_t now = host_usec();
        if (now - t0 > 1000000) { t0 = now;
            INFO("[ipu] state: cmd %08x running %d wait %d ifc %d fp %d bp %d ofc %d ctrl %08x | d3 chcr %08x madr %08x qwc %x | d4 chcr %08x madr %08x qwc %x tadr %08x end %d\n",
                 ipu.cur, co_running, co_wait, ipu.ifc, ipu.fp, ipu.bp, ipu.ofc, ipu.ctrl, CHCR(D3), MADR(D3), QWC(D3), CHCR(D4), MADR(D4), QWC(D4), TADR(D4), ipu.d4_tag_end); } }
    for (int guard = 0; guard < 100000; guard++) {
        int progress = d4_fill() + d3_drain();
        if (co_running) {
            int can = co_wait == 1 ? (ipu.ifc > 0 || d4_active()) : co_wait == 2 ? (ipu.ofc < 8) : 1;
            if (can) {
                int ifc0 = ipu.ifc, ofc0 = ipu.ofc, fp0 = ipu.fp, bp0 = ipu.bp;
                coro_resume(co_ipu);
                if (!co_running) finish_cmd();
                progress += !co_running || ipu.ifc != ifc0 || ipu.ofc != ofc0 || ipu.fp != fp0 || ipu.bp != bp0;
            }
        }
        if (!progress) break;
    }
}
static void co_reset(void) {
    coro_free(co_ipu);
    co_ipu = coro_new(co_entry, 1 << 18);
    co_running = 0; co_wait = 0;
}
static void start_cmd(uint32_t v) {
    tables_init();
    if (!co_ipu || co_running) co_reset();   /* a new command abandons an unfinished one */
    if (ipulog < 0) ipulog = getenv("AE2_IPULOG") ? atoi(getenv("AE2_IPULOG")) : 0;
    { static int n; if (ipulog > 1 && n++ < 400000) INFO("[ipu] cmd %x (%08x) ifc %d fp %d bp %d\n", v >> 28, v, ipu.ifc, ipu.fp, ipu.bp); }
    { static uint32_t cnt[16], tot; static uint64_t t0; cnt[v >> 28]++; tot++;
      uint64_t now = host_usec(); if (!t0) t0 = now;
      if (ipulog && now - t0 > 1000000) { t0 = now;
          INFO("[ipu] 1s: %u cmds: bclr %u idec %u bdec %u vdec %u fdec %u setiq %u csc %u; d3 chcr %x qwc %x d4 chcr %x qwc %x\n", tot, cnt[0], cnt[1], cnt[2], cnt[3], cnt[4], cnt[5], cnt[7],
               CHCR(D3), QWC(D3), CHCR(D4), QWC(D4));
          memset(cnt, 0, sizeof cnt); tot = 0; } }
    ipu.ctrl &= ~(CTRL_ECD | CTRL_SCD);
    ipu.cur = v;
    int c = v >> 28;
    if (c == C_BCLR) {
        ipu.ifc = ipu.fr = 0; ipu.fp = 0; ipu.bp = v & 0x7f;
        ipu.ctrl &= ~CTRL_BUSY; ipu.cmd_busy = 0;
        kernel_raise_intc(8);
        pump();
        return;
    }
    if (c == C_SETTH) { ipu.th0 = v & 0x1ff; ipu.th1 = (v >> 16) & 0x1ff; ipu.ctrl &= ~CTRL_BUSY; kernel_raise_intc(8); return; }
    ipu.ctrl |= CTRL_BUSY;
    if (c == C_VDEC || c == C_FDEC) { ipu.cmd_busy = 0x80000000u; ipu.top_busy = 0x80000000u; }
    if (c == C_IDEC || c == C_BDEC) ipu.top_busy = 0x80000000u;
    pending_skip = (c == C_IDEC || c == C_BDEC || c == C_VDEC || c == C_FDEC || c == C_SETIQ) ? (int)(v & 0x3f) : 0;
    co_running = 1; co_wait = 0;
    pump();
}

/* the next 32 bits for a register read, without waiting: 0 if not enough data yet */
static int peek32_nb(uint32_t *out) {
    while (ipu.fp * 128 < ipu.bp + 32) {
        if (!ipu.ifc) d4_fill();
        if (!ipu.ifc || ipu.fp >= 2) return 0;
        memcpy(ipu.ib[ipu.fp], ipu.fifo[ipu.fr], 16);
        ipu.fr = (ipu.fr + 1) & 7; ipu.ifc--; ipu.fp++;
    }
    int byte = ipu.bp >> 3, sh = ipu.bp & 7;
    uint64_t v = 0;
    for (int i = 0; i < 5; i++) v = (v << 8) | (byte + i < 32 ? (&ipu.ib[0][0])[byte + i] : 0);
    *out = (uint32_t)(v >> (8 - sh));
    return 1;
}
static void soft_reset(void) {
    ipu.ifc = ipu.fr = ipu.fp = ipu.bp = 0;
    ipu.ofc = ipu.or_ = 0;
    ipu.cbp = 0; ipu.th0 = ipu.th1 = 0;
    ipu.ctrl &= 0x7f33f00u;
    ipu.top = 0; ipu.top_busy = 0; ipu.cmd_data = 0; ipu.cmd_busy = 0;
    if (co_ipu) co_reset();
    kernel_raise_intc(8);
}

/* ------------------------------------------------------------------ register interface (rt_hw.c) */
int ipu_handles(uint32_t a) { return (a & 0xffffff00u) == 0x10002000u; }
uint64_t ipu_read64(uint32_t a) {
    pump();
    switch (a & 0xf0) {
    case 0x00: {
        int c = ipu.cur >> 28;
        if (!(ipu.ctrl & CTRL_BUSY) && c != C_VDEC && c != C_FDEC) { uint32_t t; if (peek32_nb(&t)) ipu.cmd_data = t; }
        return ipu.cmd_data | ((uint64_t)ipu.cmd_busy << 32);
    }
    case 0x10: return (ipu.ctrl & ~0x3fffu) | (uint32_t)ipu.ifc | ((uint32_t)ipu.ofc << 4) | ((uint32_t)(ipu.cbp & 63) << 8);
    case 0x20: return (uint32_t)(ipu.bp & 0x7f) | ((uint32_t)ipu.ifc << 8) | ((uint32_t)ipu.fp << 16);
    case 0x30: {
        if (!(ipu.ctrl & CTRL_BUSY)) { uint32_t t; if (peek32_nb(&t)) { ipu.top = t; ipu.top_busy = 0; } else ipu.top_busy = 0x80000000u; }
        return ipu.top | ((uint64_t)ipu.top_busy << 32);
    }
    }
    return 0;
}
uint32_t ipu_read32(uint32_t a) { uint64_t v = ipu_read64(a & ~7u); return (a & 4) ? (uint32_t)(v >> 32) : (uint32_t)v; }
void ipu_write32(uint32_t a, uint32_t v) {
    switch (a & 0xf4) {
    case 0x00: start_cmd(v); break;
    case 0x10:
        ipu.ctrl = (v & 0x47f30000u) | (ipu.ctrl & 0x8000ffffu);
        if (((ipu.ctrl >> 16) & 3) == 3) ipu.ctrl = (ipu.ctrl & ~(3u << 16)) | (1u << 16);
        if (v & (1u << 30)) soft_reset();
        ipu.ctrl &= ~(1u << 30);
        break;
    }
}
/* FIFO registers: 0x10007000 output (read), 0x10007010 input (write) */
void ipu_fifo_read(uint8_t out[16]) {
    pump();
    if (ipu.ofc) { memcpy(out, ipu.ofifo[ipu.or_], 16); ipu.or_ = (ipu.or_ + 1) & 7; ipu.ofc--; }
    else memset(out, 0, 16);
    pump();
}
void ipu_fifo_write(const uint8_t in[16]) {
    if (ipu.ifc < 8) { memcpy(ipu.fifo[(ipu.fr + ipu.ifc) & 7], in, 16); ipu.ifc++; }
    pump();
}
/* a CHCR write that sets STR on channel 3 or 4 */
void ipu_dma_start(int ch) {
    if (ch == 4) {
        uint32_t chcr = CHCR(D4);
        int chain = ((chcr >> 2) & 3) == 1;
        if (!chain) { ipu.d4_tag_active = 1; ipu.d4_tag_end = 1; }
        else if ((QWC(D4) & 0xffff) == 0) { ipu.d4_tag_active = 0; ipu.d4_tag_end = 0; }
        else {   /* resuming inside a tag: does it end the chain? */
            uint32_t id = (chcr >> 28) & 7, irq = chcr >> 31;
            ipu.d4_tag_active = 1;
            ipu.d4_tag_end = id == 0 || id == 7 || (irq && (chcr & 0x80));
        }
    }
    pump();
}
void ipu_poll(void) { if (co_running || ipu.ofc || ipu.ifc) pump(); }
