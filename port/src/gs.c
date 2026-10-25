/* Graphics Synthesizer: GIF tag processing, GS registers, software rasterizer,
 * 4 MB local memory with exact PS2 swizzling, transfers, and display readout. */
#include "rt.h"
#include "platform.h"
#include <stdarg.h>
#include <math.h>

/* ------------------------------------------------------------------ memory */
static uint32_t *vram;   /* 4 MB = 1M words */
#define VRAM_WORDS (1u << 20)

static const uint8_t bt32[4][8] = {{0,1,4,5,16,17,20,21},{2,3,6,7,18,19,22,23},{8,9,12,13,24,25,28,29},{10,11,14,15,26,27,30,31}};
static const uint8_t bt16[8][4] = {{0,2,8,10},{1,3,9,11},{4,6,12,14},{5,7,13,15},{16,18,24,26},{17,19,25,27},{20,22,28,30},{21,23,29,31}};
static const uint8_t bt16s[8][4] = {{0,2,16,18},{1,3,17,19},{8,10,24,26},{9,11,25,27},{4,6,20,22},{5,7,21,23},{12,14,28,30},{13,15,29,31}};
static const uint8_t bt8[4][8] = {{0,1,4,5,16,17,20,21},{2,3,6,7,18,19,22,23},{8,9,12,13,24,25,28,29},{10,11,14,15,26,27,30,31}};
static const uint8_t bt4[8][4] = {{0,2,8,10},{1,3,9,11},{4,6,12,14},{5,7,13,15},{16,18,24,26},{17,19,25,27},{20,22,28,30},{21,23,29,31}};
static const uint8_t ct32[8][8] = {{0,1,4,5,8,9,12,13},{2,3,6,7,10,11,14,15},{16,17,20,21,24,25,28,29},{18,19,22,23,26,27,30,31},
    {32,33,36,37,40,41,44,45},{34,35,38,39,42,43,46,47},{48,49,52,53,56,57,60,61},{50,51,54,55,58,59,62,63}};
static const uint8_t ct16[8][16] = {
    {0,2,8,10,16,18,24,26,1,3,9,11,17,19,25,27},{4,6,12,14,20,22,28,30,5,7,13,15,21,23,29,31},
    {32,34,40,42,48,50,56,58,33,35,41,43,49,51,57,59},{36,38,44,46,52,54,60,62,37,39,45,47,53,55,61,63},
    {64,66,72,74,80,82,88,90,65,67,73,75,81,83,89,91},{68,70,76,78,84,86,92,94,69,71,77,79,85,87,93,95},
    {96,98,104,106,112,114,120,122,97,99,105,107,113,115,121,123},{100,102,108,110,116,118,124,126,101,103,109,111,117,119,125,127}};
static const uint8_t ct8[16][16] = {
    {0,4,16,20,32,36,48,52,2,6,18,22,34,38,50,54},{8,12,24,28,40,44,56,60,10,14,26,30,42,46,58,62},
    {33,37,49,53,1,5,17,21,35,39,51,55,3,7,19,23},{41,45,57,61,9,13,25,29,43,47,59,63,11,15,27,31},
    {96,100,112,116,64,68,80,84,98,102,114,118,66,70,82,86},{104,108,120,124,72,76,88,92,106,110,122,126,74,78,90,94},
    {65,69,81,85,97,101,113,117,67,71,83,87,99,103,115,119},{73,77,89,93,105,109,121,125,75,79,91,95,107,111,123,127},
    {128,132,144,148,160,164,176,180,130,134,146,150,162,166,178,182},{136,140,152,156,168,172,184,188,138,142,154,158,170,174,186,190},
    {161,165,177,181,129,133,145,149,163,167,179,183,131,135,147,151},{169,173,185,189,137,141,153,157,171,175,187,191,139,143,155,159},
    {224,228,240,244,192,196,208,212,226,230,242,246,194,198,210,214},{232,236,248,252,200,204,216,220,234,238,250,254,202,206,218,222},
    {193,197,209,213,225,229,241,245,195,199,211,215,227,231,243,247},{201,205,217,221,233,237,249,253,203,207,219,223,235,239,251,255}};
static const uint16_t ct4[16][32] = {
    {0,8,32,40,64,72,96,104,2,10,34,42,66,74,98,106,4,12,36,44,68,76,100,108,6,14,38,46,70,78,102,110},
    {16,24,48,56,80,88,112,120,18,26,50,58,82,90,114,122,20,28,52,60,84,92,116,124,22,30,54,62,86,94,118,126},
    {65,73,97,105,1,9,33,41,67,75,99,107,3,11,35,43,69,77,101,109,5,13,37,45,71,79,103,111,7,15,39,47},
    {81,89,113,121,17,25,49,57,83,91,115,123,19,27,51,59,85,93,117,125,21,29,53,61,87,95,119,127,23,31,55,63},
    {192,200,224,232,128,136,160,168,194,202,226,234,130,138,162,170,196,204,228,236,132,140,164,172,198,206,230,238,134,142,166,174},
    {208,216,240,248,144,152,176,184,210,218,242,250,146,154,178,186,212,220,244,252,148,156,180,188,214,222,246,254,150,158,182,190},
    {129,137,161,169,193,201,225,233,131,139,163,171,195,203,227,235,133,141,165,173,197,205,229,237,135,143,167,175,199,207,231,239},
    {145,153,177,185,209,217,241,249,147,155,179,187,211,219,243,251,149,157,181,189,213,221,245,253,151,159,183,191,215,223,247,255},
    {256,264,288,296,320,328,352,360,258,266,290,298,322,330,354,362,260,268,292,300,324,332,356,364,262,270,294,302,326,334,358,366},
    {272,280,304,312,336,344,368,376,274,282,306,314,338,346,370,378,276,284,308,316,340,348,372,380,278,286,310,318,342,350,374,382},
    {321,329,353,361,257,265,289,297,323,331,355,363,259,267,291,299,325,333,357,365,261,269,293,301,327,335,359,367,263,271,295,303},
    {337,345,369,377,273,281,305,313,339,347,371,379,275,283,307,315,341,349,373,381,277,285,309,317,343,351,375,383,279,287,311,319},
    {448,456,480,488,384,392,416,424,450,458,482,490,386,394,418,426,452,460,484,492,388,396,420,428,454,462,486,494,390,398,422,430},
    {464,472,496,504,400,408,432,440,466,474,498,506,402,410,434,442,468,476,500,508,404,412,436,444,470,478,502,510,406,414,438,446},
    {385,393,417,425,449,457,481,489,387,395,419,427,451,459,483,491,389,397,421,429,453,461,485,493,391,399,423,431,455,463,487,495},
    {401,409,433,441,465,473,497,505,403,411,435,443,467,475,499,507,405,413,437,445,469,477,501,509,407,415,439,447,471,479,503,511}};

enum { PSMCT32 = 0x00, PSMCT24 = 0x01, PSMCT16 = 0x02, PSMCT16S = 0x0A, PSMT8 = 0x13, PSMT4 = 0x14,
       PSMT8H = 0x1B, PSMT4HL = 0x24, PSMT4HH = 0x2C, PSMZ32 = 0x30, PSMZ24 = 0x31, PSMZ16 = 0x32, PSMZ16S = 0x3A };

static inline uint32_t addr32(uint32_t bp, uint32_t bw, int x, int y, int z) {
    uint32_t page = (uint32_t)(x >> 6) + (uint32_t)(y >> 5) * bw;
    uint32_t block = bt32[(y >> 3) & 3][(x >> 3) & 7] ^ (z ? 0x18 : 0);
    return ((bp + page * 32 + block) * 64 + ct32[y & 7][x & 7]) & (VRAM_WORDS - 1);
}
static inline uint32_t addr16(uint32_t bp, uint32_t bw, int x, int y, int psm) {
    uint32_t page = (uint32_t)(x >> 6) + (uint32_t)(y >> 6) * bw;
    uint32_t block;
    switch (psm) {
    case PSMCT16S: block = bt16s[(y >> 3) & 7][(x >> 4) & 3]; break;
    case PSMZ16: block = bt16[(y >> 3) & 7][(x >> 4) & 3] ^ 0x18; break;
    case PSMZ16S: block = bt16s[(y >> 3) & 7][(x >> 4) & 3] ^ 0x18; break;
    default: block = bt16[(y >> 3) & 7][(x >> 4) & 3]; break;
    }
    return ((bp + page * 32 + block) * 128 + ct16[y & 7][x & 15]) & (VRAM_WORDS * 2 - 1);
}
static inline uint32_t addr8(uint32_t bp, uint32_t bw, int x, int y) {
    uint32_t pw = bw >> 1; if (!pw) pw = 1;
    uint32_t page = (uint32_t)(x >> 7) + (uint32_t)(y >> 6) * pw;
    uint32_t block = bt8[(y >> 4) & 3][(x >> 4) & 7];
    return ((bp + page * 32 + block) * 256 + ct8[y & 15][x & 15]) & (VRAM_WORDS * 4 - 1);
}
static inline uint32_t addr4(uint32_t bp, uint32_t bw, int x, int y) {
    uint32_t pw = bw >> 1; if (!pw) pw = 1;
    uint32_t page = (uint32_t)(x >> 7) + (uint32_t)(y >> 7) * pw;
    uint32_t block = bt4[(y >> 4) & 7][(x >> 5) & 3];
    return ((bp + page * 32 + block) * 512 + ct4[y & 15][x & 31]) & (VRAM_WORDS * 8 - 1);
}

/* raw pixel access by format: returns the stored value (color or index) */
static inline uint32_t vread(int psm, uint32_t bp, uint32_t bw, int x, int y) {
    uint16_t *v16 = (uint16_t *)vram; uint8_t *v8 = (uint8_t *)vram;
    switch (psm) {
    case PSMCT32: case PSMZ32: return vram[addr32(bp, bw, x, y, psm == PSMZ32)];
    case PSMCT24: case PSMZ24: return vram[addr32(bp, bw, x, y, psm == PSMZ24)] & 0xffffff;
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: return v16[addr16(bp, bw, x, y, psm)];
    case PSMT8: return v8[addr8(bp, bw, x, y)];
    case PSMT4: { uint32_t a = addr4(bp, bw, x, y); return (v8[a >> 1] >> ((a & 1) * 4)) & 15; }
    case PSMT8H: return vram[addr32(bp, bw, x, y, 0)] >> 24;
    case PSMT4HL: return (vram[addr32(bp, bw, x, y, 0)] >> 24) & 15;
    case PSMT4HH: return vram[addr32(bp, bw, x, y, 0)] >> 28;
    default: return vram[addr32(bp, bw, x, y, 0)];
    }
}
static uint64_t page_seq[512];
static uint64_t global_seq = 1;
#define TOUCH(wordaddr) (page_seq[((wordaddr) >> 11) & 511] = global_seq)
static inline void vwrite(int psm, uint32_t bp, uint32_t bw, int x, int y, uint32_t v) {
    uint16_t *v16 = (uint16_t *)vram; uint8_t *v8 = (uint8_t *)vram;
    switch (psm) {
    case PSMCT32: case PSMZ32: { uint32_t a = addr32(bp, bw, x, y, psm == PSMZ32); vram[a] = v; TOUCH(a); return; }
    case PSMCT24: case PSMZ24: { uint32_t a = addr32(bp, bw, x, y, psm == PSMZ24); vram[a] = (vram[a] & 0xff000000) | (v & 0xffffff); TOUCH(a); break; }
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: { uint32_t a = addr16(bp, bw, x, y, psm); v16[a] = (uint16_t)v; TOUCH(a >> 1); break; }
    case PSMT8: { uint32_t a = addr8(bp, bw, x, y); v8[a] = (uint8_t)v; TOUCH(a >> 2); break; }
    case PSMT4: { uint32_t a = addr4(bp, bw, x, y); uint8_t *p = &v8[a >> 1];
                  *p = (a & 1) ? (uint8_t)((*p & 0x0f) | (v << 4)) : (uint8_t)((*p & 0xf0) | (v & 15)); TOUCH(a >> 3); break; }
    case PSMT8H: { uint32_t a = addr32(bp, bw, x, y, 0); vram[a] = (vram[a] & 0x00ffffff) | (v << 24); TOUCH(a); break; }
    case PSMT4HL: { uint32_t a = addr32(bp, bw, x, y, 0); vram[a] = (vram[a] & 0xf0ffffff) | ((v & 15) << 24); TOUCH(a); break; }
    case PSMT4HH: { uint32_t a = addr32(bp, bw, x, y, 0); vram[a] = (vram[a] & 0x0fffffff) | ((v & 15) << 28); TOUCH(a); break; }
    default: { uint32_t a = addr32(bp, bw, x, y, 0); vram[a] = v; TOUCH(a); break; }
    }
}

/* ------------------------------------------------------------------ registers */
typedef struct {
    uint64_t tex0, clamp, tex1, tex2, xyoffset, miptbp1, miptbp2, scissor, alpha, test, fba, frame, zbuf;
} GSCtx;

static struct {
    GSCtx c[2];
    uint64_t prim, prmode, prmodecont, rgbaq, st, uv, fog, texclut, scanmsk, texa, fogcol, dimx, dthe, colclamp, pabe;
    uint64_t bitbltbuf, trxpos, trxreg, trxdir, signal, label;
    float q;
    uint64_t priv[0x20];   /* PMODE.. at 0x12000000 + 16*i (i<16) */
    uint64_t csr, imr, busdir, siglblid;
    uint32_t clut[256];    /* internal CLUT buffer, 32-bit entries */
    uint64_t cbp0, cbp1;
} gs;

typedef struct {
    int32_t x, y;      /* 12.4 fixed, offset applied */
    uint32_t z;
    uint8_t r, g, b, a;
    float s, t, q;
    int32_t u, v;      /* 10.4 fixed */
    uint8_t f;
} Vtx;

static uint64_t clut_hash = 1;
/* GPU renderer hooks (gs_hw.h) */
static int hw_on;
static int hw_pending_start = 1;   /* first primitive: start the GPU renderer if requested */
static int tc_last_idx;
static void hw_sync_vram_range(uint32_t bp, uint32_t nblocks);
static const char *hw_sync_why;
static void gs_frame_ready(const uint32_t *p, int w, int h);
static void gs_mt_start(void);
static int ee_field;
static inline void flush_if_pending(uint32_t bp, uint32_t bw, uint32_t psm, int x0, int y0, int x1, int y1, int reason);
static Vtx vq[3];
static int nv;
static int csr_field;
static uint64_t draw_count;

/* AE2_GSSTATS: per-state breakdown of rasterised pixels */
typedef struct { uint32_t key; uint64_t px, prims, bbox; } GStat;
static GStat gstats[256]; static int gstats_on = -1;
static void gstat_begin(uint32_t key, uint64_t bbox) {
    if (gstats_on < 0) gstats_on = getenv("AE2_GSSTATS") != NULL;
    if (!gstats_on) return;
    int i; for (i = 0; i < 256 && gstats[i].prims && gstats[i].key != key; i++);
    if (i == 256) i = 255;
    gstats[i].key = key; gstats[i].prims++; gstats[i].bbox += bbox;
}
static void gstat_dump(void) {
    if (gstats_on <= 0) return;
    for (int i = 0; i < 256 && gstats[i].prims; i++)
        for (int j = i + 1; j < 256 && gstats[j].prims; j++)
            if (gstats[j].bbox > gstats[i].bbox) { GStat t = gstats[i]; gstats[i] = gstats[j]; gstats[j] = t; }
    for (int i = 0; i < 12 && gstats[i].prims; i++) {
        uint32_t k = gstats[i].key;
        fprintf(stderr, "[gsstat] fbp=%03x fpsm=%02x prim=%u tme=%u tpsm=%02x abe=%u zt=%u bilin=%u : prims=%llu bbox_px=%llu\n",
            k & 0x1ff, (k >> 9) & 0x3f, (k >> 15) & 7, (k >> 18) & 1, (k >> 19) & 0x3f, (k >> 25) & 1, (k >> 26) & 3, (k >> 28) & 1,
            (unsigned long long)gstats[i].prims, (unsigned long long)gstats[i].bbox);
    }
    memset(gstats, 0, sizeof gstats);
}


/* transfer state */
static struct { int active, x, y, w, h, sx, dbp, dbw, dpsm; uint8_t pend[16]; int npend; } trx;

static inline uint32_t bits(uint64_t v, int lo, int n) { return (uint32_t)((v >> lo) & ((1ull << n) - 1)); }

/* ------------------------------------------------------------------ CLUT */
/* GPU renderer: a CLUT load whose source only exists on the GPU is deferred until a
 * palette texture is drawn (the game resets TEX0 to "8-bit at 0, load CLUT from 0" every
 * frame, and address 0 is the display buffer); a later full 256-entry load cancels it */
static uint64_t clut_deferred; static int clut_deferred_on;
static void load_clut_data(uint64_t tex0);
static int hw_clut_src_dirty(uint32_t cbp);
static void load_clut(uint64_t tex0) {
    uint32_t cbp = bits(tex0, 37, 14), cld = bits(tex0, 61, 3);
    uint32_t psm = bits(tex0, 20, 6);
    if (psm != PSMT8 && psm != PSMT4 && psm != PSMT8H && psm != PSMT4HL && psm != PSMT4HH) return;
    switch (cld) {
    case 0: return;
    case 1: break;
    case 2: gs.cbp0 = cbp; break;
    case 3: gs.cbp1 = cbp; break;
    case 4: if (gs.cbp0 == cbp) return; gs.cbp0 = cbp; break;
    case 5: if (gs.cbp1 == cbp) return; gs.cbp1 = cbp; break;
    default: return;
    }
    int full = (psm == PSMT8 || psm == PSMT8H);
    if (unlikely(hw_on)) {
        clut_deferred_on = 0;   /* superseded (a deferred load of GPU-only memory is never forced by a later load) */
        (void)full;
        if (hw_clut_src_dirty(cbp)) { clut_deferred = tex0; clut_deferred_on = 1; return; }
    }
    load_clut_data(tex0);
}
/* before drawing with a palette texture */
static void clut_flush_deferred(void) {
    if (!clut_deferred_on) return;
    clut_deferred_on = 0;
    hw_sync_why = "clut"; hw_sync_vram_range(bits(clut_deferred, 37, 14), 8);
    load_clut_data(clut_deferred);
}
static void load_clut_data(uint64_t tex0) {
    uint32_t cbp = bits(tex0, 37, 14), cpsm = bits(tex0, 51, 4), csm = bits(tex0, 55, 1), csa = bits(tex0, 56, 5);
    uint32_t psm = bits(tex0, 20, 6);
    int is8 = (psm == PSMT8 || psm == PSMT8H);
    int n = is8 ? 256 : 16;
    if (csm == 0) flush_if_pending(cbp, 1, cpsm, 0, 0, 15, 15, 8);
    else flush_if_pending(cbp, bits(gs.texclut, 0, 6), cpsm, 0, 0, 1023, (int)bits(gs.texclut, 12, 10), 8);
    if (csm == 0) {
        /* CSM1: CLUT stored as 16x16 (8-bit) or 8x2 (4-bit) block in buffer */
        for (int i = 0; i < n; i++) {
            int x, y;
            if (is8) {
                int j = i;
                /* swap bits 3 and 4 (CSM1 entry ordering) */
                int k = (j & 0xe7) | ((j & 0x08) << 1) | ((j & 0x10) >> 1);
                x = k & 15; y = k >> 4;
            } else { x = i & 7; y = i >> 3; }
            uint32_t v;
            if (cpsm == PSMCT32 || cpsm == PSMCT24) v = vread(PSMCT32, cbp, 1, x, y);
            else {
                uint32_t c16 = vread(cpsm, cbp, 1, x, y);
                v = ((c16 & 0x1f) << 3) | (((c16 >> 5) & 0x1f) << 11) | (((c16 >> 10) & 0x1f) << 19) | ((c16 & 0x8000) ? 0x80000000u : 0);
                v |= 0x40000000u * 0;  /* alpha handled via TEXA at lookup: store raw A bit in bit 31 */
            }
            int dst = is8 ? i : ((csa & 15) * 16 + i) & 255;
            if (cpsm == PSMCT32 || cpsm == PSMCT24) dst = is8 ? i : ((csa & 15) * 16 + i) & 255;
            gs.clut[dst] = v;
        }
    } else {
        /* CSM2: linear, from TEXCLUT */
        uint32_t cbw = bits(gs.texclut, 0, 6), cou = bits(gs.texclut, 6, 6) * 16, cov = bits(gs.texclut, 12, 10);
        for (int i = 0; i < n; i++) {
            uint32_t c16 = vread(cpsm == PSMCT32 ? PSMCT32 : cpsm, cbp, cbw, cou + i, cov);
            uint32_t v = (cpsm == PSMCT32 || cpsm == PSMCT24) ? c16 :
                ((c16 & 0x1f) << 3) | (((c16 >> 5) & 0x1f) << 11) | (((c16 >> 10) & 0x1f) << 19) | ((c16 & 0x8000) ? 0x80000000u : 0);
            gs.clut[is8 ? i : ((csa & 15) * 16 + i) & 255] = v;
        }
    }
    (void)csa;
    uint64_t hsh = 1469598103934665603ull;
    for (int i = 0; i < 256; i++) hsh = (hsh ^ gs.clut[i]) * 1099511628211ull;
    clut_hash = hsh;
}

/* ------------------------------------------------------------------ texturing */
typedef struct {
    uint32_t tbp, tbw, psm, tw, th, tcc, tfx, cpsm, csa;
    uint32_t wms, wmt, minu, maxu, minv, maxv;
    int bilinear;
    uint32_t ta0, ta1, aem;
    const uint32_t *cache;
    /* derived wrap parameters: x' = repeat ? ((x & am) | ao) : clamp(x, lo, hi) */
    int u_rep, v_rep, u_lo, u_hi, v_lo, v_hi;
    uint32_t u_am, u_ao, v_am, v_ao;
} TexState;

static inline uint32_t expand16(uint32_t c, const TexState *t) {
    uint32_t r = (c & 0x1f) << 3, g = ((c >> 5) & 0x1f) << 3, b = ((c >> 10) & 0x1f) << 3;
    uint32_t a = (c & 0x8000) ? t->ta1 : ((t->aem && !(c & 0x7fff)) ? 0 : t->ta0);
    return r | (g << 8) | (b << 16) | (a << 24);
}

static inline int wrapc(int v, int size, uint32_t mode, uint32_t mn, uint32_t mx) {
    switch (mode) {
    case 0: return v & (size - 1);
    case 1: return v < 0 ? 0 : v >= size ? size - 1 : v;
    case 2: return v < (int)mn ? (int)mn : v > (int)mx ? (int)mx : v;
    default: return (int)(((uint32_t)v & mn) | mx);
    }
}

static inline uint32_t texel_raw(const TexState *t, int x, int y) {
    uint32_t v = vread(t->psm, t->tbp, t->tbw, x, y);
    switch (t->psm) {
    case PSMCT32: case PSMZ32: return v;
    case PSMCT24: case PSMZ24: return v | (((t->aem && !(v & 0xffffff)) ? 0 : t->ta0) << 24);
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: return expand16(v, t);
    default: {
        uint32_t idx = v;
        if (t->psm == PSMT4 || t->psm == PSMT4HL || t->psm == PSMT4HH) idx = ((t->csa & 15) * 16 + idx) & 255;
        uint32_t c = gs.clut[idx & 255];
        if (t->cpsm == PSMCT32 || t->cpsm == PSMCT24) return c;
        uint32_t a = (c & 0x80000000u) ? t->ta1 : ((t->aem && !(c & 0xffffff)) ? 0 : t->ta0);
        return (c & 0xffffff) | (a << 24);
    }
    }
}

static inline int ffloor(float f) { int i = (int)f; return i - (f < (float)i); }
#include "gs_raster.h"

/* ---- texture cache: whole textures decoded to linear RGBA, hashed by
 * (address, format, size, CLUT contents, TEXA); revalidated against the VRAM
 * page sequence numbers, so uploads and render-to-texture invalidate them. */
typedef struct {
    uint32_t tbp, tbw, psm, tw, th, cpsm, csa, texa_key;
    uint64_t clut_hash, seq, lru, epoch;
    uint32_t *data;
    int bucket;
} TexCacheEnt;
#define TC_N 1024
#define TC_HASH 4096
static TexCacheEnt tcache[TC_N];
static int16_t tc_head[TC_HASH], tc_next[TC_N];
static int tc_used;
static uint64_t tc_tick, tc_decodes;

static int pages_newer(uint32_t bp, uint32_t bw, uint32_t psm, int w, int h, uint64_t seq) {
    int pw, ph, half; page_dims(psm, &pw, &ph, &half);
    uint32_t ppr = half ? (bw >> 1) : bw; if (!ppr) ppr = 1;
    uint32_t base = bp >> 5;
    int px1 = (w - 1) / pw, py1 = (h - 1) / ph;
    int extra = 0;
    if (bp & 31) {
        int maxblk = 31;
        if (px1 == 0 && py1 == 0) {
            int lx = w - 1, ly = h - 1;
            switch (psm) {
            case PSMCT32: case PSMCT24: case PSMT8H: case PSMT4HL: case PSMT4HH: maxblk = bt32[(ly >> 3) & 3][(lx >> 3) & 7]; break;
            case PSMCT16: maxblk = bt16[(ly >> 3) & 7][(lx >> 4) & 3]; break;
            case PSMCT16S: maxblk = bt16s[(ly >> 3) & 7][(lx >> 4) & 3]; break;
            case PSMT8: maxblk = bt8[(ly >> 4) & 3][(lx >> 4) & 7]; break;
            case PSMT4: maxblk = bt4[(ly >> 4) & 7][(lx >> 5) & 3]; break;
            default: maxblk = 31; break;
            }
        }
        extra = (int)(bp & 31) + maxblk > 31;
    }
    if (px1 > 32) px1 = 32;
    if (py1 > 64) py1 = 64;
    for (int py = 0; py <= py1; py++)
        for (int px = 0; px <= px1; px++)
            for (int e = 0; e <= extra; e++)
                if (page_seq[(base + (uint32_t)py * ppr + (uint32_t)px + (uint32_t)e) & 511] > seq) return 1;
    return 0;
}

/* Fast texture decode: swizzle via per-tile offset tables, indexed formats via
 * a pre-expanded palette. Falls back to texel_raw for the rarer formats. */
static uint16_t X8[64][128], X4[128][128];
static void init_tex_tables(void) {
    for (int y = 0; y < 64; y++) for (int x = 0; x < 128; x++) X8[y][x] = bt8[(y >> 4) & 3][(x >> 4) & 7] * 256 + ct8[y & 15][x & 15];
    for (int y = 0; y < 128; y++) for (int x = 0; x < 128; x++) X4[y][x] = bt4[(y >> 4) & 7][(x >> 5) & 3] * 512 + ct4[y & 15][x & 31];
}
static void tex_decode(const TexState *t, uint32_t *out, int w, int h) {
    uint32_t bp = t->tbp, bw = t->tbw;
    uint32_t pal[256];
    int indexed = (t->psm == PSMT8 || t->psm == PSMT4 || t->psm == PSMT8H || t->psm == PSMT4HL || t->psm == PSMT4HH);
    if (indexed) {
        for (int i = 0; i < 256; i++) {
            uint32_t c = gs.clut[i];
            if (t->cpsm == PSMCT32 || t->cpsm == PSMCT24) pal[i] = c;
            else { uint32_t a = (c & 0x80000000u) ? t->ta1 : ((t->aem && !(c & 0xffffff)) ? 0 : t->ta0); pal[i] = (c & 0xffffff) | (a << 24); }
        }
    }
    const uint8_t *v8 = (const uint8_t *)vram;
    const uint16_t *v16 = (const uint16_t *)vram;
    switch (t->psm) {
    case PSMT8: {
        uint32_t pw = bw >> 1; if (!pw) pw = 1;
        for (int y = 0; y < h; y++) {
            uint32_t rb = bp * 256 + (uint32_t)(y >> 6) * pw * 8192;
            const uint16_t *xt = X8[y & 63];
            for (int x = 0; x < w; x++) out[y * w + x] = pal[v8[(rb + (uint32_t)(x >> 7) * 8192 + xt[x & 127]) & (VRAM_WORDS * 4 - 1)]];
        }
        return;
    }
    case PSMT4: {
        uint32_t pw = bw >> 1; if (!pw) pw = 1;
        const uint32_t *p16 = pal + ((t->csa & 15) * 16);
        uint32_t pal4[16]; for (int i = 0; i < 16; i++) pal4[i] = pal[((t->csa & 15) * 16 + i) & 255];
        (void)p16;
        for (int y = 0; y < h; y++) {
            uint32_t rb = bp * 512 + (uint32_t)(y >> 7) * pw * 16384;
            const uint16_t *xt = X4[y & 127];
            for (int x = 0; x < w; x++) {
                uint32_t a = (rb + (uint32_t)(x >> 7) * 16384 + xt[x & 127]) & (VRAM_WORDS * 8 - 1);
                out[y * w + x] = pal4[(v8[a >> 1] >> ((a & 1) * 4)) & 15];
            }
        }
        return;
    }
    case PSMCT32: case PSMCT24: case PSMT8H: case PSMT4HL: case PSMT4HH: {
        for (int y = 0; y < h; y++) {
            RowAddr r; row_addr(&r, bp, bw, PSMCT32, y);
            uint32_t *o = out + y * w;
            switch (t->psm) {
            case PSMCT32: for (int x = 0; x < w; x++) o[x] = vram[ra32(&r, x)]; break;
            case PSMCT24: for (int x = 0; x < w; x++) { uint32_t v = vram[ra32(&r, x)] & 0xffffff; o[x] = v | (((t->aem && !v) ? 0 : t->ta0) << 24); } break;
            case PSMT8H: for (int x = 0; x < w; x++) o[x] = pal[vram[ra32(&r, x)] >> 24]; break;
            case PSMT4HL: for (int x = 0; x < w; x++) o[x] = pal[((t->csa & 15) * 16 + ((vram[ra32(&r, x)] >> 24) & 15)) & 255]; break;
            default: for (int x = 0; x < w; x++) o[x] = pal[((t->csa & 15) * 16 + (vram[ra32(&r, x)] >> 28)) & 255]; break;
            }
        }
        return;
    }
    case PSMCT16: case PSMCT16S: {
        for (int y = 0; y < h; y++) {
            RowAddr r; row_addr(&r, bp, bw, t->psm, y);
            uint32_t *o = out + y * w;
            for (int x = 0; x < w; x++) o[x] = expand16(v16[ra16(&r, x)], t);
        }
        return;
    }
    default:
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) out[y * w + x] = texel_raw(t, x, y);
        return;
    }
}

static const uint32_t *tex_lookup(TexState *t) {
    int indexed = (t->psm == PSMT8 || t->psm == PSMT4 || t->psm == PSMT8H || t->psm == PSMT4HL || t->psm == PSMT4HH);
    uint32_t texa_key = t->ta0 | (t->ta1 << 8) | (t->aem << 16);
    uint64_t ch = indexed ? clut_hash : 0;
    uint32_t cpsm = indexed ? t->cpsm : 0, csa = indexed ? t->csa : 0;
    tc_tick++;
    uint32_t hk = (t->tbp * 2654435761u) ^ (t->psm << 3) ^ (t->tw << 9) ^ (t->th << 13) ^ (uint32_t)(ch * 0x9E3779B97F4A7C15ull >> 40) ^ (csa << 20);
    int bucket = (int)(hk & (TC_HASH - 1));
    int w = 1 << t->tw, h = 1 << t->th;
    TexCacheEnt *e = NULL;
    for (int i = tc_head[bucket]; i >= 0; i = tc_next[i]) {
        TexCacheEnt *c = &tcache[i];
        if (c->tbp == t->tbp && c->tbw == t->tbw && c->psm == t->psm && c->tw == t->tw && c->th == t->th &&
            c->cpsm == cpsm && c->csa == csa && c->texa_key == texa_key && c->clut_hash == ch) { e = c; break; }
    }
    if (e) {
        if (!pages_newer(t->tbp, t->tbw, t->psm, w, h, e->seq)) { e->lru = tc_tick; e->epoch = flush_epoch; tc_last_idx = (int)(e - tcache); return e->data; }
        if (nworkers && e->epoch == flush_epoch) {
            /* stale but queued prims may still sample the old copy: orphan it and decode into a fresh entry */
            int ei = (int)(e - tcache);
            int16_t *pp = &tc_head[e->bucket];
            while (*pp >= 0 && *pp != ei) pp = &tc_next[*pp];
            if (*pp == ei) *pp = tc_next[ei];
            e->bucket = -1;
            e = NULL;
        }
    }
    if (!e) {
        int vi = -1;
        if (tc_used < TC_N) vi = tc_used++;
        else {
            for (int i = 0; i < TC_N; i++)
                if ((!nworkers || tcache[i].epoch != flush_epoch) && (vi < 0 || tcache[i].lru < tcache[vi].lru)) vi = i;
            if (vi < 0) {   /* every entry is in use by queued prims */
                gs_flush_r(2);
                vi = 0;
                for (int i = 1; i < TC_N; i++) if (tcache[i].lru < tcache[vi].lru) vi = i;
            }
            TexCacheEnt *v = &tcache[vi];
            if (v->bucket >= 0) {
                int16_t *pp = &tc_head[v->bucket];
                while (*pp >= 0 && *pp != vi) pp = &tc_next[*pp];
                if (*pp == vi) *pp = tc_next[vi];
            }
        }
        e = &tcache[vi];
        if (e->data && (e->tw != t->tw || e->th != t->th)) { free(e->data); e->data = NULL; }
        if (!e->data) e->data = malloc((size_t)w * h * 4);
        e->tbp = t->tbp; e->tbw = t->tbw; e->psm = t->psm; e->tw = t->tw; e->th = t->th;
        e->cpsm = cpsm; e->csa = csa; e->texa_key = texa_key; e->clut_hash = ch;
        e->bucket = bucket; tc_next[vi] = tc_head[bucket]; tc_head[bucket] = (int16_t)vi;
    }
    /* make sure queued draws into the texture's pages have landed */
    flush_if_pending(t->tbp, t->tbw, t->psm, 0, 0, w - 1, h - 1, 9);
    tc_decodes++;
    e->seq = global_seq;
    tex_decode(t, e->data, w, h);
    e->lru = tc_tick; e->epoch = flush_epoch;
    tc_last_idx = (int)(e - tcache);
    return e->data;
}
/* ------------------------------------------------------------------ draw state */
static int hw_texture_prepare(TexState *t);
/* ---- texture patches (native Graphics options page).  The game's option labels
 * and values are pre-rendered words in two 4-bit textures (256x512 labels, 256x128
 * values) that it re-uploads freely, at addresses that may vary.  The EE side
 * registers replacement cells; whenever a draw uses one of those textures (known
 * by a signature: the bar-segment picture both carry at x 240-255, y 0-27) and its
 * VRAM changed, the cells are written into it before the texture is decoded. */
static int texpatch_on;
static pthread_mutex_t texpatch_m = PTHREAD_MUTEX_INITIALIZER;
static const uint8_t *texpatch_cell[2][9];      /* [labels/values][slot] -> 4-bit cells, or NULL */
static uint32_t texpatch_gen = 1;
static const uint32_t texpatch_sig[2] = {0x92544129u, 0x3002b65au};
static struct { uint32_t tbp, gen, hash; uint64_t seq; int have; uint8_t backup[256 * 512]; } tpst[2];
static void hw_vram_written(uint32_t bp, uint32_t bw, int psm, int x0, int y0, int w, int h);
static uint32_t fnv32(const uint8_t *p, size_t n, uint32_t h) { for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u; return h; }
void gs_texpatch_set(int tex, int slot, const uint8_t *cell) {
    if (tex < 0 || tex > 1 || slot < 0 || slot > 8) return;
    pthread_mutex_lock(&texpatch_m);
    if (texpatch_cell[tex][slot] != cell) { texpatch_cell[tex][slot] = cell; texpatch_gen++; }
    texpatch_on = 1;
    pthread_mutex_unlock(&texpatch_m);
}
static void gs_flush_r(int reason);
static void texpatch_check(TexState *t) {
    int which = t->th == 9 ? 0 : 1, W = 256, H = 1 << t->th;
    /* pages: 128x128 texels each for PSMT4 */
    uint64_t mseq = 0; int ppr = (int)(t->tbw ? t->tbw : 4) / 2;
    for (int py = 0; py < H / 128; py++) for (int px = 0; px < W / 128; px++) {
        uint64_t q = page_seq[((t->tbp >> 5) + (uint32_t)(py * ppr + px)) & 511]; if (q > mseq) mseq = q; }
    pthread_mutex_lock(&texpatch_m);
    uint32_t gen = texpatch_gen; const uint8_t *cells[9]; memcpy(cells, texpatch_cell[which], sizeof cells);
    pthread_mutex_unlock(&texpatch_m);
    if (tpst[which].tbp == t->tbp && mseq <= tpst[which].seq && tpst[which].gen == gen) return;
    static int tl = -1; if (tl < 0) tl = getenv("AE2_TPLOG") != NULL;
    if (tl) INFO("[texpatch] check %s tbp %x tbw %u gen %u/%u seq %llu/%llu\n", which ? "values" : "labels", t->tbp, t->tbw, gen, tpst[which].gen,
                 (unsigned long long)mseq, (unsigned long long)tpst[which].seq);
    static uint8_t cur[256 * 512], out[256 * 512];
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) cur[y * W + x] = (uint8_t)vread(PSMT4, t->tbp, t->tbw, x, y);
    uint32_t sig = 2166136261u;
    for (int y = 0; y < 28; y++) for (int x = 240; x < 256; x++) sig = (sig ^ cur[y * W + x]) * 16777619u;
    tpst[which].gen = gen; tpst[which].seq = mseq;
    if (sig != texpatch_sig[which]) { if (tl) INFO("[texpatch]   signature %08x: not ours\n", sig); tpst[which].tbp = t->tbp; tpst[which].have = 0; return; }   /* some other texture */
    uint32_t h = fnv32(cur, (size_t)W * H, 2166136261u);
    if (!(tpst[which].have && tpst[which].tbp == t->tbp && h == tpst[which].hash)) {   /* the game's original */
        memcpy(tpst[which].backup, cur, (size_t)W * H); tpst[which].have = 1;
    }
    tpst[which].tbp = t->tbp;
    memcpy(out, tpst[which].backup, (size_t)W * H);
    for (int k = 0; k < 9; k++) if (cells[k]) {
        int cw = which == 0 ? 192 : 96, x0 = which == 0 ? 0 : 1 + 96 * (k & 1), y0 = which == 0 ? 29 + 28 * k : 1 + 28 * (k >> 1);
        for (int y = 0; y < 27; y++) memcpy(&out[(y0 + y) * W + x0], &cells[k][y * cw], (size_t)cw);
    }
    int changed = 0;
    for (int i = 0; i < W * H; i++) if (out[i] != cur[i]) { changed = 1; break; }
    if (tl) INFO("[texpatch]   %s, writing %d\n", h == tpst[which].hash ? "patched copy" : "fresh", changed);
    if (changed) {
        gs_flush_r(10);
        global_seq++;
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) if (out[y * W + x] != cur[y * W + x]) vwrite(PSMT4, t->tbp, t->tbw, x, y, out[y * W + x]);
        if (unlikely(hw_on)) hw_vram_written(t->tbp, t->tbw, PSMT4, 0, 0, W, H);
    }
    tpst[which].hash = fnv32(out, (size_t)W * H, 2166136261u);
    tpst[which].seq = global_seq;
}
/* ---- the title menu's "2 Players" item: the title words are one 256x128 8-bit texture
 * (2d_title_slct) with a 32-row word per row: New game, Continue, Options and the unused
 * Quit game.  The fourth row is overwritten with the word registered here whenever that
 * texture (known by its first three rows, English) is used after its VRAM changed. */
static const uint8_t *volatile titlepatch_row;
static struct { uint32_t tbp; uint64_t seq; } tlst;
void gs_titlepatch_set(const uint8_t *row) { titlepatch_row = row; tlst.tbp = ~0u; }
static void titlepatch_check(TexState *t) {
    uint64_t mseq = 0; int ppr = (int)(t->tbw ? t->tbw : 4) / 2;
    for (int py = 0; py < 2; py++) for (int px = 0; px < 2; px++) {     /* PSMT8 pages are 128x64 */
        uint64_t q = page_seq[((t->tbp >> 5) + (uint32_t)(py * ppr + px)) & 511]; if (q > mseq) mseq = q; }
    if (tlst.tbp == t->tbp && mseq <= tlst.seq) return;
    tlst.tbp = t->tbp; tlst.seq = mseq;
    uint32_t h = 2166136261u;
    for (int y = 0; y < 96; y++) for (int x = 0; x < 256; x++) h = (h ^ (uint32_t)vread(PSMT8, t->tbp, t->tbw, x, y)) * 16777619u;
    if (h != 0xa4fa3ce0u) return;
    const uint8_t *row = titlepatch_row; int changed = 0;
    for (int y = 0; y < 32 && !changed; y++) for (int x = 0; x < 256; x++) if ((uint8_t)vread(PSMT8, t->tbp, t->tbw, x, 96 + y) != row[y * 256 + x]) { changed = 1; break; }
    if (!changed) return;
    gs_flush_r(10);
    global_seq++;
    for (int y = 0; y < 32; y++) for (int x = 0; x < 256; x++) vwrite(PSMT8, t->tbp, t->tbw, x, 96 + y, row[y * 256 + x]);
    if (unlikely(hw_on)) hw_vram_written(t->tbp, t->tbw, PSMT8, 0, 96, 256, 32);
    tlst.seq = global_seq;
}
/* AE2_TEXDUMP=frame:tbp(hex) - write the texture at tbp (as a 32-bit image of its size) the
 * first time it is used in that frame: compares render-to-texture between the renderers */
static long texdump_frame = -2; static uint32_t texdump_bp;
static void texdump_check(const TexState *t);
/* ---- mipmaps: the PS2 picks a level per pixel from Q:  LOD = log2(1/|Q|) * 2^L + K
 * (or K alone with LCM).  Here it is chosen per primitive from its vertices' Q, which
 * is close for the small polygons the game draws, and both renderers then sample
 * that level.  -1 means magnified (MMAG filter on level 0). */
static int mip_req = -1;
static int mip_effective(const DrawState *d, int req) {
    if (req < 0) return -1;
    if (d->mip_mmin < 2 || d->mip_max == 0) return 0;
    return req < d->mip_max ? req : d->mip_max;
}
static int mip_pick(const DrawState *d, int nv, const Vtx *const *v) {
    float lod;
    if (d->mip_lcm) lod = d->mip_k;
    else {
        float sum = 0;
        for (int i = 0; i < nv; i++) { float q = fabsf(v[i]->q); if (q < 1e-20f) q = 1e-20f; sum += -log2f(q); }
        lod = sum / (float)nv * (float)(1 << d->mip_l) + d->mip_k;
    }
    if (lod <= 0.0f) return -1;
    int lv = (int)(lod + 0.5f);
    return lv > 6 ? 6 : lv;
}
static void setup_draw(DrawState *d) {
    memset(d, 0, sizeof *d);
    uint64_t prim = gs.prim;
    uint64_t attr = (gs.prmodecont & 1) ? prim : gs.prmode;
    d->iip = bits(attr, 3, 1); d->tme = bits(attr, 4, 1); d->fge = bits(attr, 5, 1); d->abe = bits(attr, 6, 1);
    d->fst = bits(attr, 8, 1); d->ctx = bits(attr, 9, 1);
    GSCtx *c = &gs.c[d->ctx];
    d->fbp = bits(c->frame, 0, 9) * 32; d->fbw = bits(c->frame, 16, 6); d->fpsm = bits(c->frame, 24, 6); d->fbmsk = bits(c->frame, 32, 32);
    d->zbp = bits(c->zbuf, 0, 9) * 32; d->zpsm = bits(c->zbuf, 24, 4) | 0x30; d->zmsk = bits(c->zbuf, 32, 1);
    d->ate = bits(c->test, 0, 1); d->atst = bits(c->test, 1, 3); d->aref = bits(c->test, 4, 8); d->afail = bits(c->test, 12, 2);
    d->date = bits(c->test, 14, 1); d->datm = bits(c->test, 15, 1); d->zte = bits(c->test, 16, 1); d->ztst = bits(c->test, 17, 2);
    if (!d->zte) d->ztst = 1;
    d->a_a = bits(c->alpha, 0, 2); d->a_b = bits(c->alpha, 2, 2); d->a_c = bits(c->alpha, 4, 2); d->a_d = bits(c->alpha, 6, 2);
    d->fix = bits(c->alpha, 32, 8);
    d->pabe = bits(gs.pabe, 0, 1); d->colclamp = bits(gs.colclamp, 0, 1); d->fba = bits(c->fba, 0, 1);
    d->scx0 = bits(c->scissor, 0, 11); d->scx1 = bits(c->scissor, 16, 11);
    d->scy0 = bits(c->scissor, 32, 11); d->scy1 = bits(c->scissor, 48, 11);
    d->fogcol = (uint32_t)gs.fogcol;
    if (d->tme) {
        TexState *t = &d->tex;
        uint64_t tex0 = c->tex0;
        t->tbp = bits(tex0, 0, 14); t->tbw = bits(tex0, 14, 6); t->psm = bits(tex0, 20, 6);
        t->tw = bits(tex0, 26, 4); t->th = bits(tex0, 30, 4); t->tcc = bits(tex0, 34, 1); t->tfx = bits(tex0, 35, 2);
        t->cpsm = bits(tex0, 51, 4); t->csa = bits(tex0, 56, 5);
        if (t->tw > 10) t->tw = 10;
        if (t->th > 10) t->th = 10;
        t->wms = bits(c->clamp, 0, 2); t->wmt = bits(c->clamp, 2, 2);
        t->minu = bits(c->clamp, 4, 10); t->maxu = bits(c->clamp, 14, 10);
        t->minv = bits(c->clamp, 24, 10); t->maxv = bits(c->clamp, 34, 10);
        t->bilinear = bits(c->tex1, 5, 1);
        {   /* mipmapping (TEX1): the level was chosen for the primitive by emit_prim (mip_req) */
            uint64_t tex1 = c->tex1;
            d->mip_lcm = bits(tex1, 0, 1); d->mip_max = bits(tex1, 2, 3); if (d->mip_max > 6) d->mip_max = 6;
            d->mip_mmag = bits(tex1, 5, 1); d->mip_mmin = bits(tex1, 6, 3); d->mip_l = bits(tex1, 19, 2);
            int k = (int)bits(tex1, 32, 12); if (k & 0x800) k -= 0x1000; d->mip_k = (float)k / 16.0f;
            int lv = mip_effective(d, mip_req);
            d->mip_eff = d->mip_want = lv;
            if (lv >= 0) t->bilinear = d->mip_mmin == 1 || d->mip_mmin == 4 || d->mip_mmin == 5;
            if (lv > 0) {
                /* level addresses: MIPTBP1/2, or packed after level 0 with MTBA */
                uint32_t lbp[7], lbw[7]; lbp[0] = t->tbp; lbw[0] = t->tbw;
                int bpp = t->psm == PSMT4 ? 4 : (t->psm == PSMT8 || t->psm == PSMT8H) ? 8 : psm_is16(t->psm) ? 16 : 32;
                for (int n = 1; n <= lv; n++) {
                    if (bits(tex1, 9, 1) && n <= 3) {
                        uint32_t w = (1u << t->tw) >> (n - 1), h = (1u << t->th) >> (n - 1);
                        lbp[n] = lbp[n - 1] + (w ? w : 1) * (h ? h : 1) * (uint32_t)bpp / 8 / 256;
                        lbw[n] = lbw[n - 1] > 1 ? lbw[n - 1] / 2 : 1;
                    } else {
                        uint64_t m = n <= 3 ? c->miptbp1 : c->miptbp2; int sh = ((n - 1) % 3) * 20;
                        lbp[n] = bits(m, sh, 14); lbw[n] = bits(m, sh + 14, 6);
                    }
                }
                /* no check against level 0: for distant surfaces the game often uploads only the
                 * smaller levels, so level 0 can be stale while the level the PS2 samples is fine
                 * (sampling level 0 there is the "broken textures at a distance" of emulators
                 * without mipmapping) */
                uint32_t bp = lbp[lv], bw = lbw[lv];
                t->tbp = bp; t->tbw = bw ? bw : 1;
                t->tw = t->tw > (uint32_t)lv ? t->tw - (uint32_t)lv : 0;
                t->th = t->th > (uint32_t)lv ? t->th - (uint32_t)lv : 0;
                t->minu >>= lv; t->maxu >>= lv; t->minv >>= lv; t->maxv >>= lv;
                {   static long dumpbp = -2; if (dumpbp == -2) { const char *e = getenv("AE2_MIPDUMP"); dumpbp = e ? strtol(e, NULL, 16) : -1; }
                    if (dumpbp >= 0 && bits(c->tex0, 0, 14) == (uint64_t)dumpbp) {   /* debug: level 0 and this level, side by side files */
                        static int done[7]; if (!done[lv]) { done[lv] = 1;
                            TexState t0 = *t; t0.tbp = bits(c->tex0, 0, 14); t0.tbw = bits(c->tex0, 14, 6); t0.tw = bits(c->tex0, 26, 4); t0.th = bits(c->tex0, 30, 4);
                            for (int pass = 0; pass < 2; pass++) { const TexState *tt = pass ? t : &t0; int w = 1 << tt->tw, h = 1 << tt->th;
                                uint32_t *px = malloc((size_t)w * h * 4); tex_decode(tt, px, w, h);
                                char path[64]; snprintf(path, sizeof path, "mip_%x_L%d_%dx%d.rgba", (unsigned)dumpbp, pass ? lv : 0, w, h);
                                FILE *f = fopen(path, "wb"); if (f) { fwrite(px, 4, (size_t)w * h, f); fclose(f); } free(px); } } } }
            }
        }
        t->ta0 = bits(gs.texa, 0, 8); t->aem = bits(gs.texa, 15, 1); t->ta1 = bits(gs.texa, 32, 8);
        if (unlikely(clut_deferred_on) && (t->psm == PSMT8 || t->psm == PSMT4 || t->psm == PSMT8H || t->psm == PSMT4HL || t->psm == PSMT4HH)) clut_flush_deferred();
        if (unlikely(texdump_frame >= 0)) texdump_check(t);
        if (unlikely(texpatch_on) && t->psm == PSMT4 && t->tw == 8 && (t->th == 9 || t->th == 7)) texpatch_check(t);
        if (unlikely(titlepatch_row != NULL) && t->psm == PSMT8 && t->tw == 8 && t->th == 7) titlepatch_check(t);
        t->cache = (unlikely(hw_on) && hw_texture_prepare(t)) ? NULL : tex_lookup(t);
        {
            int W = 1 << t->tw, H = 1 << t->th;
            #define WRAPSET(mode, mn, mx, size, rep, lo, hi, am, ao) do { \
                switch (mode) { \
                case 0: rep = 1; am = (size) - 1; ao = 0; break; \
                case 1: rep = 0; lo = 0; hi = (size) - 1; break; \
                case 2: rep = 0; lo = (int)(mn) & ((size) - 1); hi = (int)(mx) & ((size) - 1); if (lo > hi) hi = lo; break; \
                default: rep = 1; am = (mn) & ((size) - 1); ao = (mx) & ((size) - 1); break; } } while (0)
            WRAPSET(t->wms, t->minu, t->maxu, W, t->u_rep, t->u_lo, t->u_hi, t->u_am, t->u_ao);
            WRAPSET(t->wmt, t->minv, t->maxv, H, t->v_rep, t->v_lo, t->v_hi, t->v_am, t->v_ao);
            #undef WRAPSET
        }
    }
    d->fb16 = psm_is16(d->fpsm);
    d->fb24 = (d->fpsm == PSMCT24 || d->fpsm == PSMZ24);
    d->zmax = d->zpsm == PSMZ24 ? 0xffffff : psm_is16(d->zpsm) ? 0xffff : 0xffffffffu;
    d->zread = d->zte && d->ztst >= 2;
    d->zwrite = d->zte && !d->zmsk;
    d->need_dst = d->date || (d->abe && (d->a_a == 1 || d->a_b == 1 || d->a_c == 1 || d->a_d == 1));
    d->simple_fill = !d->tme && !d->abe && !d->ate && !d->date && !d->fbmsk && !d->fge && d->ztst == 1;
}

#define GSTAT(d, type, bb) do { if (gstats_on) gstat_begin(((d)->fbp >> 5) | ((d)->fpsm << 9) | ((uint32_t)(type) << 15) | ((uint32_t)(d)->tme << 18) | \
    ((d)->tme ? (d)->tex.psm << 19 : 0) | ((uint32_t)(d)->abe << 25) | ((uint32_t)(d)->ztst << 26) | ((d)->tme ? (uint32_t)(d)->tex.bilinear << 28 : 0), (bb)); } while (0)

static int get_state(void) {
    static int nocache = -1; if (nocache < 0) nocache = getenv("AE2_NOSTATECACHE") != NULL;
    if (!state_dirty && st_cur >= 0 && !nocache) return st_cur;
    int s = (st_cur + 1) & (ST_N - 1);
    if (nworkers && st_lastq[s] > min_rpos()) gs_flush_r(3);
    setup_draw(&st_ring[s]);
    st_cur = s; state_dirty = 0; st_lastq[s] = 0;
    return s;
}

/* ---- frame skipping: primitives of a skipped game frame are dropped; frame-buffer
 * pages drawn only by skipped frames are not presented (the window keeps the last image) */
static int mtgs;
static int gs_skip;              /* GS side, set in stream order through a private register */
static uint8_t fb_stale[512];    /* FBP (2048-word pages) -> last frame drawn there was skipped */
static int fskip_mode = -2, fskip_max = 1;   /* -1 auto, 0 off, N: draw 1 frame of N+1 */
static void texlog_frame(void);
static int fskip_cur;                 /* skipping currently requested */
uint32_t g_fmv_vsync;                /* last vsync an FMV frame was shown (hle_mvconv) */
static int fskip_user = -1;           /* Graphics options: 1 auto, 0 off (-1: AE2_FRAMESKIP / default) */
void gs_set_frameskip(int on) { fskip_user = on ? 1 : 0; }
void gs_game_frame(void) {
    if (!mtgs) texlog_frame();   /* per game frame; only meaningful with AE2_MTGS=0 */
    if (fskip_mode == -2) {
        const char *e = getenv("AE2_FRAMESKIP");
        fskip_mode = (!e || !strcmp(e, "auto")) ? -1 : atoi(e);
        if (rt_deterministic() && fskip_mode < 0) fskip_mode = 0;   /* auto skipping depends on host speed */
        if ((e = getenv("AE2_FRAMESKIP_MAX"))) fskip_max = atoi(e);
        if (fskip_mode > 0) fskip_max = fskip_mode;
    }
    if (fskip_user >= 0) { fskip_mode = fskip_user && !rt_deterministic() ? -1 : 0; fskip_user = -1; if (!fskip_mode && fskip_cur) { fskip_cur = 0; gs_write_priv64(0x12001ff0, 0); } }
    if (!fskip_mode) return;
    static uint64_t last_t, last_idle; static int run;
    uint64_t now = host_usec(), idle = g_idle_us;
    uint64_t busy = (now - last_t) > (idle - last_idle) ? (now - last_t) - (idle - last_idle) : 0;
    int want;
    if (fskip_mode > 0) want = run < fskip_max;          /* fixed pattern */
    else { extern uint32_t g_vblank_us; want = last_t && busy > g_vblank_us - 1000 && run < fskip_max; }   /* only when a frame overran the vsync budget */
    /* never during FMVs: the movie draws once per vsync from its own thread, and
     * skipping in step with it would drop every movie frame */
    if (g_fmv_vsync && g_frame - g_fmv_vsync < 8) want = 0;
    run = want ? run + 1 : 0;
    last_t = now; last_idle = idle;
    if (want != fskip_cur) { fskip_cur = want; gs_write_priv64(0x12001ff0, (uint64_t)want); }
}

/* AE2_TEXLOG=from-to (game frames): per frame, list the draw states used (texture,
 * blending, tests) with primitive counts - for finding objects that stop drawing */
static int texlog_on = -1; static uint32_t texlog_from, texlog_to;
static struct { uint64_t key; uint32_t n; } texlog_tab[256]; static int texlog_n;
static void texlog_add(const DrawState *d, int type) {
    uint64_t key = (uint64_t)(d->tme ? d->tex.tbp : 0x3fff) | ((uint64_t)d->tex.psm << 14) | ((uint64_t)d->abe << 20) |
        ((uint64_t)(d->a_a | d->a_b << 2 | d->a_c << 4 | d->a_d << 6) << 21) | ((uint64_t)d->ate << 29) | ((uint64_t)d->atst << 30) |
        ((uint64_t)(d->aref & 255) << 33) | ((uint64_t)d->ztst << 41) | ((uint64_t)d->zwrite << 43) | ((uint64_t)d->tex.tfx << 44) |
        ((uint64_t)d->fge << 46) | ((uint64_t)type << 47) | ((uint64_t)(d->fbp >> 5) << 50) | ((uint64_t)d->tex.tcc << 59) | ((uint64_t)d->afail << 60);
    for (int i = 0; i < texlog_n; i++) if (texlog_tab[i].key == key) { texlog_tab[i].n++; return; }
    if (texlog_n < 256) { texlog_tab[texlog_n].key = key; texlog_tab[texlog_n].n = 1; texlog_n++; }
}
static void texlog_frame(void) {
    extern uint32_t g_pad_polls;
    if (texlog_on < 0) { const char *e = getenv("AE2_TEXLOG"); texlog_on = e && sscanf(e, "%u-%u", &texlog_from, &texlog_to) == 2; }
    if (!texlog_on) return;
    uint32_t p = g_pad_polls;
    if (p >= texlog_from && p <= texlog_to && texlog_n) {
        fprintf(stderr, "[texlog] poll %u:", p);
        for (int i = 0; i < texlog_n; i++) {
            uint64_t k = texlog_tab[i].key;
            fprintf(stderr, " %04llx/p%02llx/abe%llu:%02llx/at%llu.%llu.%02llx.f%llu/zt%llu%c/tfx%llu%s/fog%llu/pr%llu/fb%03llx=%u",
                k & 0x3fff, (k >> 14) & 63, (k >> 20) & 1, (k >> 21) & 255, (k >> 29) & 1, (k >> 30) & 7, (k >> 33) & 255, (k >> 60) & 3,
                (k >> 41) & 3, ((k >> 43) & 1) ? 'w' : '-', (k >> 44) & 3, ((k >> 59) & 1) ? "a" : "", (k >> 46) & 1, (k >> 47) & 7, (k >> 50) & 511, texlog_tab[i].n);
        }
        fprintf(stderr, "\n");
    }
    texlog_n = 0;
}

/* AE2_GSSURVEY=1: histogram of GS features the game uses (guides the GPU renderer) */
static int survey_on = -1;
static struct { char k[96]; uint64_t n; } survey[4096]; static int nsurvey;
static uint8_t survey_rt_page[512];    /* VRAM pages (2048 words) ever used as a render target */
static void survey_add(const char *fmt, ...) {
    char k[96]; va_list ap; va_start(ap, fmt); vsnprintf(k, sizeof k, fmt, ap); va_end(ap);
    unsigned h = 5381; for (const char *p = k; *p; p++) h = h * 33 + (unsigned char)*p;
    for (int i = 0; i < nsurvey; i++) if (!strcmp(survey[i].k, k)) { survey[i].n++; return; }
    if (nsurvey < 4096) { snprintf(survey[nsurvey].k, 96, "%s", k); survey[nsurvey++].n = 1; }
    (void)h;
}
static int survey_cmp(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }
static void survey_dump(void) {
    qsort(survey, nsurvey, sizeof survey[0], survey_cmp);
    for (int i = 0; i < nsurvey; i++) fprintf(stderr, "[survey] %-60s %llu\n", survey[i].k, (unsigned long long)survey[i].n);
}
static void survey_prim(const DrawState *d, int type) {
    survey_add("prim type=%d iip=%d fge=%d tme=%d fst=%d", type, d->iip, d->fge, d->tme, d->fst);
    survey_add("fb fbp=%03x fbw=%u psm=%02x msk=%08x", d->fbp, d->fbw, d->fpsm, d->fbmsk);
    if (d->zte) survey_add("z zbp=%03x zpsm=%02x ztst=%d zmsk=%d", d->zbp, d->zpsm, d->ztst, d->zmsk);
    else survey_add("z off");
    if (d->abe) survey_add("blend a%d b%d c%d d%d fix=%02x pabe=%d clamp=%d fba=%d", d->a_a, d->a_b, d->a_c, d->a_d, d->a_c == 2 ? d->fix : 0, d->pabe, d->colclamp, d->fba);
    else survey_add("blend off fba=%d", d->fba);
    if (d->ate) survey_add("atest atst=%d aref=%02x afail=%d", d->atst, d->aref, d->afail);
    if (d->date) survey_add("date datm=%d", d->datm);
    uint64_t tex1 = gs.c[d->ctx].tex1;
    if (d->tme) {
        const TexState *t = &d->tex;
        survey_add("tex psm=%02x cpsm=%02x %dx%d tbw=%u tfx=%u tcc=%u wm=%u%u lcm=%u mxl=%u mmag=%u mmin=%u", t->psm, t->cpsm, 1 << t->tw, 1 << t->th, t->tbw, t->tfx, t->tcc, t->wms, t->wmt,
                   bits(tex1, 0, 1), bits(tex1, 2, 3), bits(tex1, 5, 1), bits(tex1, 6, 3));
        uint32_t page = (t->tbp >> 5) & 511;
        if (survey_rt_page[page]) survey_add("rtt tbp=%04x psm=%02x tbw=%u %dx%d (render target as texture)", t->tbp, t->psm, t->tbw, 1 << t->tw, 1 << t->th);
    }
    survey_rt_page[(d->fbp >> 5) & 511] = 1;
}

/* AE2_DRAWLOG=vsync[,count]: log the sequence of draws (state runs) of those frames */
static int drawlog_on = -1, drawlog_active; static uint32_t drawlog_from, drawlog_n = 1, drawlog_vs;
static char drawlog_last[256]; static uint32_t drawlog_cnt; static int drawlog_bx0, drawlog_by0, drawlog_bx1, drawlog_by1;
static void drawlog_flush(void) {
    if (drawlog_cnt) fprintf(stderr, "[draw] %5u x %s  bbox %d,%d-%d,%d\n", drawlog_cnt, drawlog_last, drawlog_bx0, drawlog_by0, drawlog_bx1, drawlog_by1);
    drawlog_cnt = 0;
}
static void drawlog_prim(const DrawState *d, int type, int bx0, int by0, int bx1, int by1, const Vtx *a) {
    char k[256];
    snprintf(k, sizeof k, "t%d fb%04x/%02x msk%08x z%04x/%s%d%s tex%s%04x/%02x/%dx%d tfx%d tcc%d bil%d ab%s%d%d%d%d f%02x at%s%d/%02x/%d da%d fst%d rgba%02x%02x%02x%02x z0=%08x",
             type, d->fbp, d->fpsm, d->fbmsk, d->zbp, d->zte ? "t" : "-", d->ztst, d->zmsk ? "m" : "w",
             d->tme ? "" : "-", d->tme ? d->tex.tbp : 0, d->tme ? d->tex.psm : 0, d->tme ? 1 << d->tex.tw : 0, d->tme ? 1 << d->tex.th : 0,
             d->tme ? d->tex.tfx : 0, d->tme ? d->tex.tcc : 0, d->tme ? d->tex.bilinear : 0,
             d->abe ? "" : "-", d->a_a, d->a_b, d->a_c, d->a_d, d->fix, d->ate ? "" : "-", d->atst, d->aref, d->afail, d->date, d->fst, a->r, a->g, a->b, a->a, a->z);
    /* the z of the first vertex is shown only for the first prim of a run */
    char *zp = strstr(k, " z0="); size_t kl = zp ? (size_t)(zp - k) : strlen(k);
    if (drawlog_cnt && !strncmp(k, drawlog_last, kl) && strlen(drawlog_last) >= kl && (drawlog_last[kl] == ' ' || !drawlog_last[kl])) {
        drawlog_cnt++;
        if (bx0 < drawlog_bx0) drawlog_bx0 = bx0; if (by0 < drawlog_by0) drawlog_by0 = by0;
        if (bx1 > drawlog_bx1) drawlog_bx1 = bx1; if (by1 > drawlog_by1) drawlog_by1 = by1;
        return;
    }
    drawlog_flush();
    snprintf(drawlog_last, sizeof drawlog_last, "%s", k); drawlog_cnt = 1;
    drawlog_bx0 = bx0; drawlog_by0 = by0; drawlog_bx1 = bx1; drawlog_by1 = by1;
}

static void hw_prim(const DrawState *d, int type, const Vtx *a, const Vtx *b, const Vtx *c, int bx0, int by0, int bx1, int by1);
static int points_direct;   /* the last primitive was written straight to VRAM */
static int split_mode; static uint32_t split_fbp, split_h;
/* In split screen each view is drawn a quarter of the height up (top half) or down
 * (bottom half) of where the game puts it (split_apply). A screen effect that reads the
 * game's render target back, or the frame on show (an afterimage blends the last frame
 * into the new one, as when a brother falls), copies it to a texture or pastes it over
 * the picture at the game's coordinates: it would paste the picture a quarter screen off, with part of the
 * other view's half. Those reads move by the same amount as the view. The frame's
 * one-to-one copy of the whole render target to the display buffer stays as it is. */
static int split_reads_fb(int type, const Vtx **a, const Vtx **b, const Vtx **c, Vtx *hv) {
    uint64_t prim = (gs.prmodecont & 1) ? gs.prim : gs.prmode;
    int ctx = bits(prim, 9, 1);
    if (!bits(prim, 4, 1)) return 0;   /* untextured */
    uint64_t t0 = gs.c[ctx].tex0;
    uint32_t tbp = bits(t0, 0, 14), disp = bits(gs.priv[7], 0, 9) * 32;
    if (tbp != split_fbp * 32 && tbp != disp) return 0;   /* the render target, or the frame on show (an afterimage blends it back in) */
    static int on = -1, log = -1; if (on < 0) { on = !getenv("AE2_SPLITFX") || atoi(getenv("AE2_SPLITFX")); log = getenv("AE2_SPLITLOG") != NULL; }
    int fst = bits(prim, 8, 1), dest = (int)bits(gs.c[ctx].frame, 0, 9);
    if (dest != (int)split_fbp && type == 6 && fst && (*a)->y == (*a)->v && (*b)->y == (*b)->v && abs((*b)->y - (*a)->y) >= (int)split_h * 16) return 0;   /* the display copy */
    if (log) { extern uint32_t g_pad_polls; static uint32_t last; if (g_pad_polls != last) { last = g_pad_polls;
        fprintf(stderr, "[split] poll %u mode %d: type %d reads the render target into fbp %x, y %d..%d v %d..%d\n", g_pad_polls, split_mode, type, dest, (*a)->y / 16, (*b)->y / 16, (*a)->v / 16, (*b)->v / 16); } }
    if (!on) return 0;
    int shift = (int)split_h / 4 * (split_mode == 1 ? 1 : -1);
    float th = (float)(1 << bits(t0, 30, 4));
    const Vtx *vv[3] = {*a, *b, *c}; int n = type == 3 ? 3 : type == 0 ? 1 : 2;
    for (int i = 0; i < n; i++) {
        if (vv[i] != &hv[i]) hv[i] = *vv[i];
        if (fst) hv[i].v += shift * 16; else hv[i].t += (float)shift / th * hv[i].q;
    }
    *a = &hv[0]; if (n > 1) *b = &hv[1]; if (n > 2) *c = &hv[2];
    return 1;
}
static int mip_force = -100;   /* the level for the piece of a triangle mip_split is drawing */
static int mip_split(const DrawState *d, const Vtx *a, const Vtx *b, const Vtx *c);
static void emit_prim(int type, const Vtx *a, const Vtx *b, const Vtx *c) {
    static int nodraw = -1; if (unlikely(nodraw < 0)) nodraw = getenv("AE2_NODRAW") != NULL;   /* profiling: the game without drawing */
    if (gs_skip || nodraw) { fb_stale[bits(gs.c[bits((gs.prmodecont & 1) ? gs.prim : gs.prmode, 9, 1)].frame, 0, 9)] = 1; return; }
    Vtx hv[3];
    if (mip_force != -100) goto pieces;   /* a piece of a triangle already placed */
    if (unlikely(split_mode >= 3) && bits(gs.c[bits((gs.prmodecont & 1) ? gs.prim : gs.prmode, 9, 1)].frame, 0, 9) == split_fbp) {
        /* a player's HUD in their half: each piece keeps its size and stays by the top or
         * bottom edge it belongs to (by which half of the full screen it is in) */
        const Vtx *vv[3] = {a, b, c}; int n = type == 3 ? 3 : type == 0 ? 1 : 2, sum = 0;
        for (int i = 0; i < n; i++) sum += vv[i]->y;
        int half = (int)split_h / 2, cy = sum / n / 16;
        int dy = split_mode == 3 ? (cy < half ? 0 : -half) : (cy < half ? half : 0);
        if (dy) {
            for (int i = 0; i < n; i++) { hv[i] = *vv[i]; hv[i].y += dy * 16; }
            a = &hv[0]; if (n > 1) b = &hv[1]; if (n > 2) c = &hv[2];
        }
    }
    if (unlikely(split_mode == 1 || split_mode == 2) && split_reads_fb(type, &a, &b, &c, hv)) {}
pieces:;
    int s = get_state();
    const DrawState *d = &st_ring[s];
    Vtx mv[3];
    if (d->tme && (d->mip_max > 0 || d->mip_mmag != (d->mip_mmin == 1 || d->mip_mmin == 4 || d->mip_mmin == 5))) {
        /* a triangle whose corners want different levels is drawn in pieces, one per level */
        if (type == 3 && mip_force == -100 && !d->fst && !d->mip_lcm && d->mip_mmin >= 2 && d->mip_max > 0 && mip_split(d, a, b, c)) return;
        const Vtx *vv[3] = {a, b, c}; int nv = type == 3 ? 3 : type == 0 ? 1 : 2;
        int r = mip_force != -100 ? mip_force : mip_pick(d, nv, vv);
        if (mip_effective(d, r) != d->mip_want) { mip_req = r; state_dirty = 1; s = get_state(); d = &st_ring[s]; }
        if (d->fst && d->mip_eff > 0) {   /* UV coordinates are level-0 texels */
            for (int i = 0; i < nv; i++) { mv[i] = *vv[i]; mv[i].u >>= d->mip_eff; mv[i].v >>= d->mip_eff; }
            a = &mv[0]; if (nv > 1) b = &mv[1]; if (nv > 2) c = &mv[2];
        }
    }
    fb_stale[(d->fbp >> 5) & 511] = 0;
    if (d->zte && d->ztst == 0) return;   /* depth test NEVER */
    draw_count++;
    /* screen bbox (pixels), clipped to the scissor */
    int n = type == 3 ? 3 : type == 0 ? 1 : 2;
    int x0 = a->x, x1 = a->x, y0 = a->y, y1 = a->y;
    const Vtx *vs[3] = {a, b, c};
    for (int i = 1; i < n; i++) {
        if (vs[i]->x < x0) x0 = vs[i]->x; if (vs[i]->x > x1) x1 = vs[i]->x;
        if (vs[i]->y < y0) y0 = vs[i]->y; if (vs[i]->y > y1) y1 = vs[i]->y;
    }
    int bx0 = x0 >> 4, bx1 = (x1 + 15) >> 4, by0 = y0 >> 4, by1 = (y1 + 15) >> 4;
    if (bx0 < d->scx0) bx0 = d->scx0; if (bx1 > d->scx1) bx1 = d->scx1;
    if (by0 < d->scy0) by0 = d->scy0; if (by1 > d->scy1) by1 = d->scy1;
    if (bx0 > bx1 || by0 > by1) return;
    if (type == 3 && (int64_t)(bx1 - bx0) * (by1 - by0) > 4000000) return;
    if (gstats_on < 0) gstats_on = getenv("AE2_GSSTATS") != NULL;
    GSTAT(d, type, (uint64_t)(bx1 - bx0 + 1) * (by1 - by0 + 1));
    if (unlikely(texlog_on)) texlog_add(d, type);
    if (unlikely(drawlog_active)) drawlog_prim(d, type, bx0, by0, bx1, by1, a);
    if (unlikely(drawlog_active) && d->tme) { static long uvt = -2; if (uvt == -2) { const char *e = getenv("AE2_UVLOG"); uvt = e ? strtol(e, NULL, 16) : -1; }
        if (uvt >= 0 && d->tex.tbp == (uint32_t)uvt) {
            const Vtx *v[3] = {a, b, c}; int nn = type == 3 ? 3 : 2;
            fprintf(stderr, "[uv] t%d", type);
            for (int i = 0; i < nn && v[i]; i++) {
                float uu = d->fst ? v[i]->u / 16.0f : v[i]->s / (v[i]->q ? v[i]->q : 1) * (1 << d->tex.tw);
                float vv = d->fst ? v[i]->v / 16.0f : v[i]->t / (v[i]->q ? v[i]->q : 1) * (1 << d->tex.th);
                fprintf(stderr, " (%.1f,%.1f uv %.1f,%.1f a%02x)", v[i]->x / 16.0, v[i]->y / 16.0, uu, vv, v[i]->a);
            }
            fprintf(stderr, "\n"); } }
    if (unlikely(survey_on)) { if (survey_on < 0) survey_on = getenv("AE2_GSSURVEY") != NULL; if (survey_on) survey_prim(d, type); }
    global_seq++;
    { static int nl; static int dbg = -1; if (dbg < 0) dbg = getenv("AE2_FLUSHDBG") != NULL;
      if (dbg && nl < 20 && !pend_page[0x1ff]) { uint8_t was = pend_page[0x1ff]; (void)was; }
      if (dbg && nl < 20 && (by1 > 600 || bx1 > 700)) { nl++; fprintf(stderr, "[bigprim] type %d bbox %d,%d-%d,%d fbp %x fbw %u psm %x scissor %d,%d-%d,%d v0=(%d,%d) v1=(%d,%d)\n", type, bx0, by0, bx1, by1, d->fbp, d->fbw, d->fpsm, d->scx0, d->scy0, d->scx1, d->scy1, a->x >> 4, a->y >> 4, b ? b->x >> 4 : 0, b ? b->y >> 4 : 0); } }
    if (unlikely(hw_on)) { hw_prim(d, type, a, b, c, bx0, by0, bx1, by1); return; }
    if (unlikely(hw_pending_start)) { hw_pending_start = 0; extern int hw_try_start(void); if (hw_try_start()) { hw_prim(d, type, a, b, c, bx0, by0, bx1, by1); return; } }
    /* single-pixel points with no tests, blending or depth write (the FMV player draws each
     * movie frame as 512x512 of these): write VRAM directly instead of queueing them */
    if (type == 0 && d->simple_fill && !d->zwrite && !d->fba && (d->fpsm == PSMCT32 || d->fpsm == PSMCT24)) {
        int px = a->x >> 4, py = a->y >> 4;
        if (px < d->scx0 || px > d->scx1 || py < d->scy0 || py > d->scy1) return;
        if (!points_direct) { gs_flush_r(0); points_direct = 1; }   /* queued work first, for ordering */
        vwrite(d->fpsm, d->fbp, d->fbw, px, py, (uint32_t)a->r | ((uint32_t)a->g << 8) | ((uint32_t)a->b << 16) | ((uint32_t)a->a << 24));
        return;
    }
    points_direct = 0;
    pages_rect(d->fbp, d->fbw, d->fpsm, bx0, by0, bx1, by1, 1);
    if (d->zread || d->zwrite) pages_rect(d->zbp, d->fbw, d->zpsm, bx0, by0, bx1, by1, 1);
    { static int nl2; static uint8_t prev1ff; static int dbg2 = -1; if (dbg2 < 0) dbg2 = getenv("AE2_FLUSHDBG") != NULL;
      if (dbg2 && pend_page[0x1ff] && !prev1ff && nl2 < 10) { nl2++; fprintf(stderr, "[p1ff] type %d bbox %d,%d-%d,%d fbp %x fbw %u fpsm %x zbp %x zpsm %x zr %d zw %d\n", type, bx0, by0, bx1, by1, d->fbp, d->fbw, d->fpsm, d->zbp, d->zpsm, d->zread, d->zwrite); }
      prev1ff = pend_page[0x1ff]; }
    Prim p; p.type = (uint8_t)type; p.st = (uint16_t)s; p.v[0] = *a;
    if (n > 1) p.v[1] = *b;
    if (n > 2) p.v[2] = *c;
    q_push(&p);
    st_lastq[s] = q_w;
}

/* The PS2 picks the mip level per pixel, from Q; a triangle drawn with one level for all
 * of it switches level along its own edges, which shows on big ground polygons as
 * patches of the sharper or the blurrier texture with polygon-shaped borders.  Q (like
 * every other vertex value) changes linearly across the screen, so where the level
 * changes is a straight line across the triangle: the triangle is cut along those lines
 * and each piece drawn with its own level.  The cut points are worked out once, on the
 * triangle's own edges, so neighbouring pieces share them exactly (no cracks). */
static void vtx_lerp(Vtx *o, const Vtx *p, const Vtx *q, double t) {
    #define LI(f) (p->f + (q->f - p->f) * t)
    o->x = (int32_t)lrint(LI(x)); o->y = (int32_t)lrint(LI(y));
    o->z = (uint32_t)llrint((double)p->z + ((double)q->z - (double)p->z) * t);
    o->r = (uint8_t)lrint(LI(r)); o->g = (uint8_t)lrint(LI(g)); o->b = (uint8_t)lrint(LI(b)); o->a = (uint8_t)lrint(LI(a));
    o->s = (float)LI(s); o->t = (float)LI(t); o->q = (float)LI(q);
    o->u = (int32_t)lrint(LI(u)); o->v = (int32_t)lrint(LI(v)); o->f = (uint8_t)lrint(LI(f));
    #undef LI
}
static int mip_split(const DrawState *d, const Vtx *a, const Vtx *b, const Vtx *c) {
    const Vtx *v[3] = {a, b, c};
    float sc = (float)(1 << d->mip_l);
    int lv[3], lo = 99, hi = -99;
    for (int i = 0; i < 3; i++) {
        if (!(v[i]->q > 1e-20f)) return 0;
        float lod = -log2f(v[i]->q) * sc + d->mip_k;
        int l = lod <= 0.0f ? -1 : (int)(lod + 0.5f);
        if (l > d->mip_max) l = d->mip_max;
        lv[i] = l; if (l < lo) lo = l; if (l > hi) hi = l;
    }
    if (lo == hi) return 0;
    /* boundary k (between level k-1 and k, k = lo+1..hi) sits at LOD 0 for k = 0, else k - 0.5 */
    double qb[10]; int nb = 0;
    for (int k = lo + 1; k <= hi && nb < 10; k++) qb[nb++] = exp2(-(((k == 0 ? 0.0 : k - 0.5)) - d->mip_k) / sc);
    /* the outline with every cut point on it, in order */
    Vtx pt[3 + 3 * 10]; double pq[3 + 3 * 10]; int np = 0;
    for (int i = 0; i < 3; i++) {
        const Vtx *p = v[i], *q = v[(i + 1) % 3];
        pt[np] = *p; pq[np] = p->q; np++;
        /* the cut points are worked out from the edge's ends in a fixed order (by position),
         * so the triangle on the other side of the edge gets the very same points */
        int sw = p->x > q->x || (p->x == q->x && p->y > q->y);
        const Vtx *e0 = sw ? q : p, *e1 = sw ? p : q;
        double dq = (double)e1->q - (double)e0->q; if (dq == 0) continue;
        double ts[10]; int tk[10], nt = 0;
        for (int k = 0; k < nb; k++) { double t = (qb[k] - e0->q) / dq; if (t > 0 && t < 1) { ts[nt] = sw ? 1 - t : t; tk[nt++] = k; } }
        for (int x = 1; x < nt; x++) for (int y = x; y > 0 && ts[y] < ts[y - 1]; y--) {
            double w = ts[y]; ts[y] = ts[y - 1]; ts[y - 1] = w; int z = tk[y]; tk[y] = tk[y - 1]; tk[y - 1] = z; }
        for (int x = 0; x < nt; x++) {   /* in order from p to q */
            vtx_lerp(&pt[np], e0, e1, sw ? 1 - ts[x] : ts[x]); pq[np] = qb[tk[x]]; np++;
        }
    }
    /* each level's piece: the outline points with Q in its range (Q falls as LOD rises) */
    for (int l = lo; l <= hi; l++) {
        double qmax = l == lo ? 1e300 : qb[l - lo - 1], qmin = l == hi ? -1e300 : qb[l - lo];
        const Vtx *poly[3 + 3 * 10]; int n = 0;
        for (int i = 0; i < np; i++) {
            if (pq[i] <= qmax && pq[i] >= qmin) poly[n++] = &pt[i];   /* cut points (exactly at a boundary) go in both pieces */
        }
        if (n < 3) continue;
        mip_force = l;
        for (int i = 1; i + 1 < n; i++) emit_prim(3, poly[0], poly[i], poly[i + 1]);
        mip_force = -100;
    }
    return 1;
}

static void vertex_kick(int draw, uint32_t x, uint32_t y, uint32_t z, uint32_t f) {
    int ctx = bits((gs.prmodecont & 1) ? gs.prim : gs.prmode, 9, 1);
    uint64_t xyo = gs.c[ctx].xyoffset;
    Vtx v;
    v.x = (int32_t)x - (int32_t)bits(xyo, 0, 16);
    v.y = (int32_t)y - (int32_t)bits(xyo, 32, 16);
    v.z = z; v.f = (uint8_t)f;
    v.r = gs.rgbaq & 255; v.g = (gs.rgbaq >> 8) & 255; v.b = (gs.rgbaq >> 16) & 255; v.a = (gs.rgbaq >> 24) & 255;
    float fs, ft; uint32_t us = (uint32_t)gs.st, ut = (uint32_t)(gs.st >> 32);
    memcpy(&fs, &us, 4); memcpy(&ft, &ut, 4);
    v.s = fs; v.t = ft; v.q = gs.q;
    v.u = bits(gs.uv, 0, 14); v.v = bits(gs.uv, 16, 14);
    int type = bits(gs.prim, 0, 3);
    vq[nv < 3 ? nv : 2] = v;
    if (nv < 3) nv++;
    switch (type) {
    case 0: if (draw) emit_prim(0, &vq[0], NULL, NULL); nv = 0; break;
    case 1: if (nv == 2) { if (draw) emit_prim(1, &vq[0], &vq[1], NULL); nv = 0; } break;
    case 2: if (nv == 2) { if (draw) emit_prim(1, &vq[0], &vq[1], NULL); vq[0] = vq[1]; nv = 1; } break;
    case 3: if (nv == 3) { if (draw) emit_prim(3, &vq[0], &vq[1], &vq[2]); nv = 0; } break;
    case 4: if (nv == 3) { if (draw) emit_prim(3, &vq[0], &vq[1], &vq[2]); vq[0] = vq[1]; vq[1] = vq[2]; nv = 2; } break;
    case 5: if (nv == 3) { if (draw) emit_prim(3, &vq[0], &vq[1], &vq[2]); vq[1] = vq[2]; nv = 2; } break;
    case 6: if (nv == 2) { if (draw) emit_prim(6, &vq[0], &vq[1], NULL); nv = 0; } break;
    default: nv = 0; break;
    }
}

/* ------------------------------------------------------------------ transfers */
static void hw_vram_written(uint32_t bp, uint32_t bw, int psm, int x0, int y0, int w, int h);
static void trx_start(void) {
    uint32_t dir = bits(gs.trxdir, 0, 2);
    trx.dbp = bits(gs.bitbltbuf, 32, 14); trx.dbw = bits(gs.bitbltbuf, 48, 6); trx.dpsm = bits(gs.bitbltbuf, 56, 6);
    trx.sx = bits(gs.trxpos, 32, 11); trx.y = bits(gs.trxpos, 48, 11); trx.x = trx.sx;
    trx.w = bits(gs.trxreg, 0, 12); trx.h = bits(gs.trxreg, 32, 12);
    trx.npend = 0;
    state_dirty = 1;
    global_seq++;
    flush_if_pending(trx.dbp, trx.dbw, trx.dpsm, trx.sx, trx.y, trx.sx + trx.w - 1, trx.y + trx.h - 1, 10);
    if (unlikely(survey_on > 0)) survey_add("xfer dir=%u psm=%02x %dx%d%s", dir, trx.dpsm, trx.w, trx.h, survey_rt_page[(trx.dbp >> 5) & 511] ? " (into a render target)" : "");
    if (unlikely(hw_on) && dir != 1) {   /* drawing that only exists on the GPU must reach VRAM before VRAM is written */
        int bpp = trx.dpsm == PSMT4 ? 4 : trx.dpsm == PSMT8 ? 8 : psm_is16(trx.dpsm) ? 16 : 32;
        hw_sync_why = "transfer"; hw_sync_vram_range(trx.dbp, (uint32_t)((trx.dbw ? trx.dbw : 1) * 64 * (trx.y + trx.h) * bpp / 8) / 256 + 32);
    }
    if (unlikely(drawlog_active)) { drawlog_flush(); fprintf(stderr, "[draw] xfer dir%u %04x/%02x %d,%d -> %04x/%02x bw%u %d,%d %dx%d\n", dir, (uint32_t)bits(gs.bitbltbuf, 0, 14), (uint32_t)bits(gs.bitbltbuf, 24, 6), (int)bits(gs.trxpos, 0, 11), (int)bits(gs.trxpos, 16, 11), trx.dbp, trx.dpsm, trx.dbw, trx.sx, trx.y, trx.w, trx.h); }
    if (dir != 1) fb_stale[(trx.dbp >> 5) & 511] = 0;
   /* a transfer into a frame buffer makes it current (FMVs are shown by copying) */
    if (dir == 0) { trx.active = 1; return; }
    trx.active = 0;
    if (dir == 2) {
        uint32_t sbp = bits(gs.bitbltbuf, 0, 14), sbw = bits(gs.bitbltbuf, 16, 6), spsm = bits(gs.bitbltbuf, 24, 6);
        int ssx = bits(gs.trxpos, 0, 11), ssy = bits(gs.trxpos, 16, 11);
        if (unlikely(split_mode == 1 || split_mode == 2) && sbp == split_fbp * 32 && trx.dbp != sbp) {   /* a copy of the picture: the view's place (split_reads_fb) */
            static int on = -1; if (on < 0) on = !getenv("AE2_SPLITFX") || atoi(getenv("AE2_SPLITFX"));
            if (getenv("AE2_SPLITLOG")) { extern uint32_t g_pad_polls; fprintf(stderr, "[split] poll %u mode %d: copy of the render target %d,%d %dx%d to %x\n", g_pad_polls, split_mode, ssx, ssy, trx.w, trx.h, trx.dbp); }
            if (on) { ssy += (int)split_h / 4 * (split_mode == 1 ? 1 : -1); if (ssy < 0) ssy = 0; }
        }
        if (unlikely(hw_on)) hw_sync_why = "copy", hw_sync_vram_range(sbp, (uint32_t)((sbw ? sbw : 1) * 64 * (ssy + trx.h) * 4 / 256) + 32);
        flush_if_pending(sbp, sbw, spsm, ssx, ssy, ssx + trx.w - 1, ssy + trx.h - 1, 11);
        for (int yy = 0; yy < trx.h; yy++)
            for (int xx = 0; xx < trx.w; xx++)
                vwrite(trx.dpsm, trx.dbp, trx.dbw, (trx.sx + xx) & 2047, (trx.y + yy) & 2047,
                       vread(spsm, sbp, sbw, (ssx + xx) & 2047, (ssy + yy) & 2047));
        if (unlikely(hw_on)) hw_vram_written(trx.dbp, trx.dbw, trx.dpsm, trx.sx, trx.y, trx.w, trx.h);
    }
}


/* write n consecutive pixels of the current row (x0.., y) from a packed source.
 * The format switch and the end-of-row checks happen once per row segment. */
static void trx_row(int x0, int y, int n, const uint8_t *p, uint32_t k0) {
    uint32_t bp = trx.dbp, bw = trx.dbw; int psm = trx.dpsm;
    uint16_t *v16 = (uint16_t *)vram; uint8_t *v8 = (uint8_t *)vram;
    y &= 2047;
    /* The row is written in runs that stay inside one block column (8 pixels for 32-bit,
     * 16 for 16- and 8-bit, 32 for 4-bit): the block's address is worked out once per run
     * and each pixel adds its column offset from the ct table. A block never spans two
     * pages, so TOUCH once per run. W = run width, CT = the table row for this y. */
#define RUNS(W, ADDR, CT) \
    for (int i = 0; i < n; ) { \
        int x = (x0 + i) & 2047, run = (W) - (x & ((W) - 1)); if (run > n - i) run = n - i; \
        uint32_t a0 = (ADDR), b = a0 - (CT)[x & ((W) - 1)]; (void)b;
#define RUNS_END(W) i += run; }
    switch (psm) {
    case PSMCT32: case PSMZ32: { int z = psm == PSMZ32; const uint8_t *ct = ct32[y & 7];
        RUNS(8, addr32(bp, bw, x, y, z), ct)
            for (int j = 0; j < run; j++) { uint32_t v; memcpy(&v, p + (k0 + i + j) * 4, 4); vram[(b + ct[(x + j) & 7]) & (VRAM_WORDS - 1)] = v; }
            TOUCH(a0);
        RUNS_END(8)
        break; }
    case PSMCT24: case PSMZ24: { int z = psm == PSMZ24; const uint8_t *ct = ct32[y & 7];
        RUNS(8, addr32(bp, bw, x, y, z), ct)
            for (int j = 0; j < run; j++) { const uint8_t *q = p + (k0 + i + j) * 3; uint32_t a = (b + ct[(x + j) & 7]) & (VRAM_WORDS - 1);
                vram[a] = (vram[a] & 0xff000000) | q[0] | (q[1] << 8) | (q[2] << 16); }
            TOUCH(a0);
        RUNS_END(8)
        break; }
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: { const uint8_t *ct = ct16[y & 7];
        RUNS(16, addr16(bp, bw, x, y, psm), ct)
            for (int j = 0; j < run; j++) { uint16_t v; memcpy(&v, p + (k0 + i + j) * 2, 2); v16[(b + ct[(x + j) & 15]) & (VRAM_WORDS * 2 - 1)] = v; }
            TOUCH(a0 >> 1);
        RUNS_END(16)
        break; }
    case PSMT8: { const uint8_t *ct = ct8[y & 15];
        RUNS(16, addr8(bp, bw, x, y), ct)
            for (int j = 0; j < run; j++) v8[(b + ct[(x + j) & 15]) & (VRAM_WORDS * 4 - 1)] = p[k0 + i + j];
            TOUCH(a0 >> 2);
        RUNS_END(16)
        break; }
    case PSMT8H: { const uint8_t *ct = ct32[y & 7];
        RUNS(8, addr32(bp, bw, x, y, 0), ct)
            for (int j = 0; j < run; j++) { uint32_t a = (b + ct[(x + j) & 7]) & (VRAM_WORDS - 1); vram[a] = (vram[a] & 0x00ffffff) | ((uint32_t)p[k0 + i + j] << 24); }
            TOUCH(a0);
        RUNS_END(8)
        break; }
    case PSMT4: { const uint16_t *ct = ct4[y & 15];
        RUNS(32, addr4(bp, bw, x, y), ct)
            for (int j = 0; j < run; j++) { uint32_t k = k0 + i + j, v = (p[k >> 1] >> ((k & 1) * 4)) & 15;
                uint32_t a = (b + ct[(x + j) & 31]) & (VRAM_WORDS * 8 - 1); uint8_t *d = &v8[a >> 1];
                *d = (a & 1) ? (uint8_t)((*d & 0x0f) | (v << 4)) : (uint8_t)((*d & 0xf0) | v); }
            TOUCH(a0 >> 3);
        RUNS_END(32)
        break; }
#undef RUNS
#undef RUNS_END
    default:   /* PSMT4HL / PSMT4HH and anything odd: generic path */
        for (int i = 0; i < n; i++) { uint32_t k = k0 + i, v = (p[k >> 1] >> ((k & 1) * 4)) & 15; vwrite(psm, bp, bw, (x0 + i) & 2047, y, v); }
        break;
    }
}

static void trx_data_impl(const uint8_t *p, uint32_t nbytes);
static void trx_data(const uint8_t *p, uint32_t nbytes) {
    if (!trx.active) return;
    int y_before = trx.y;
    trx_data_impl(p, nbytes);
    if (unlikely(hw_on)) hw_vram_written(trx.dbp, trx.dbw, trx.dpsm, trx.sx, y_before, trx.w, trx.y - y_before + 1);
}
static void trx_data_impl(const uint8_t *p, uint32_t nbytes) {
    if (!trx.active) return;
    if (trx.w <= 0) { trx.active = 0; return; }
    global_seq++;
    int bpp;
    switch (trx.dpsm) {
    case PSMCT32: case PSMZ32: bpp = 32; break;
    case PSMCT24: case PSMZ24: bpp = 24; break;
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: bpp = 16; break;
    case PSMT8: case PSMT8H: bpp = 8; break;
    default: bpp = 4; break;
    }
    uint8_t tmp[64];
    if (bpp == 24 && (trx.npend || nbytes % 3)) {
        /* 24-bit pixels straddle the 16-byte qwords: carry the leftover bytes over */
        while (nbytes) {
            uint32_t take = nbytes < (uint32_t)(48 - trx.npend) ? nbytes : (uint32_t)(48 - trx.npend);
            memcpy(tmp, trx.pend, trx.npend); memcpy(tmp + trx.npend, p, take);
            uint32_t tot = trx.npend + take, whole = tot / 3 * 3;
            trx.npend = 0;
            int npx = whole / 3;
            uint32_t k = 0;
            int ye = trx.h + (int)bits(gs.trxpos, 48, 11);
            while (npx > 0) {
                if (trx.y >= ye) { trx.active = 0; return; }
                int seg = trx.sx + trx.w - trx.x; if (seg > npx) seg = npx;
                trx_row(trx.x, trx.y, seg, tmp, k);
                k += seg; npx -= seg; trx.x += seg;
                if (trx.x >= trx.sx + trx.w) { trx.x = trx.sx; trx.y++; }
            }
            memcpy(trx.pend, tmp + whole, tot - whole); trx.npend = tot - whole;
            p += take; nbytes -= take;
        }
        return;
    }
    uint32_t npx = bpp == 4 ? nbytes * 2 : nbytes * 8 / bpp, k = 0;
    int ye = trx.h + (int)bits(gs.trxpos, 48, 11);
    while (npx > 0) {
        if (trx.y >= ye) { trx.active = 0; return; }
        uint32_t seg = (uint32_t)(trx.sx + trx.w - trx.x); if (seg > npx) seg = npx;
        trx_row(trx.x, trx.y, (int)seg, p, k);
        k += seg; npx -= seg; trx.x += seg;
        if (trx.x >= trx.sx + trx.w) { trx.x = trx.sx; trx.y++; }
    }
}

/* ------------------------------------------------------------------ register writes */
/* Split screen (port-private register 0x1fe0, set by p2.c around each frame's draw
 * list): 1 draws the list into the top half of the render target, 2 into the bottom
 * half; 3 and 4 are the HUD for the top and bottom half (see emit_prim). Each half shows the middle band of its view (the picture moves up or down by a
 * quarter of the height and is clipped to its half), so nothing is squashed. Only
 * drawing into the game's render target moves; render-to-texture passes do not. */
static void split_vram(int save);
static uint32_t split_fbp = 0x80, split_h = 512;
static uint64_t raw_xyoffset[2], raw_scissor[2];
static void split_apply(int k) {
    GSCtx *c = &gs.c[k];
    c->xyoffset = raw_xyoffset[k]; c->scissor = raw_scissor[k];
    if (!split_mode || bits(c->frame, 0, 9) != split_fbp) return;
    int half = (int)split_h / 2, shift = half / 2, top = split_mode == 1 || split_mode == 3;
    if (split_mode <= 2) {
        int64_t ofy = (int64_t)bits(raw_xyoffset[k], 32, 16) + (top ? shift : -shift) * 16;
        c->xyoffset = (raw_xyoffset[k] & ~(0xffffull << 32)) | ((uint64_t)(ofy & 0xffff) << 32);
    }
    int y0 = (int)bits(raw_scissor[k], 32, 11), y1 = (int)bits(raw_scissor[k], 48, 11);
    int lo = top ? 0 : half, hi = top ? half - 1 : (int)split_h - 1;
    if (y0 < lo) y0 = lo;
    if (y1 > hi) y1 = hi;
    if (y1 < y0) y1 = y0;   /* nothing of this pass falls in the half: one row, still clipped below */
    c->scissor = (raw_scissor[k] & 0xffffffffull) | ((uint64_t)y0 << 32) | ((uint64_t)y1 << 48);
}
static void gs_write_reg(uint32_t reg, uint64_t v) {
    if (reg == 0x00) { if (v != gs.prim) state_dirty = 1; }
    else if (!(reg <= 0x05 || reg == 0x0A || reg == 0x0C || reg == 0x0D || (reg >= 0x50 && reg <= 0x54) || reg >= 0x60)) state_dirty = 1;
    switch (reg) {
    case 0x00: gs.prim = v; nv = 0; break;
    case 0x01: gs.rgbaq = v; { uint32_t u = (uint32_t)(v >> 32); memcpy(&gs.q, &u, 4); } break;
    case 0x02: gs.st = v; break;
    case 0x03: gs.uv = v; break;
    case 0x04: vertex_kick(1, bits(v, 0, 16), bits(v, 16, 16), bits(v, 32, 24), bits(v, 56, 8)); break;
    case 0x05: vertex_kick(1, bits(v, 0, 16), bits(v, 16, 16), (uint32_t)(v >> 32), bits(gs.fog, 56, 8)); break;
    case 0x0C: vertex_kick(0, bits(v, 0, 16), bits(v, 16, 16), bits(v, 32, 24), bits(v, 56, 8)); break;
    case 0x0D: vertex_kick(0, bits(v, 0, 16), bits(v, 16, 16), (uint32_t)(v >> 32), bits(gs.fog, 56, 8)); break;
    case 0x06: case 0x07: gs.c[reg - 6].tex0 = v; load_clut(v); break;
    case 0x08: case 0x09: gs.c[reg - 8].clamp = v; break;
    case 0x0A: gs.fog = v; break;
    case 0x14: case 0x15: gs.c[reg - 0x14].tex1 = v; break;
    case 0x16: case 0x17: {  /* TEX2: update PSM and CLUT fields of TEX0 */
        GSCtx *c = &gs.c[reg - 0x16];
        uint64_t mask = (0x3full << 20) | (0x7ffffffull << 37);
        c->tex0 = (c->tex0 & ~mask) | (v & mask);
        load_clut(c->tex0);
        break;
    }
    case 0x18: case 0x19: raw_xyoffset[reg - 0x18] = v; split_apply(reg - 0x18); break;
    case 0x1A: gs.prmodecont = v; break;
    case 0x1B: gs.prmode = v; break;
    case 0x1C: gs.texclut = v; break;
    case 0x22: gs.scanmsk = v; break;
    case 0x34: case 0x35: gs.c[reg - 0x34].miptbp1 = v; break;
    case 0x36: case 0x37: gs.c[reg - 0x36].miptbp2 = v; break;
    case 0x3B: gs.texa = v; break;
    case 0x3D: gs.fogcol = v; break;
    case 0x3F: break;
    case 0x40: case 0x41: raw_scissor[reg - 0x40] = v; split_apply(reg - 0x40); break;
    case 0x42: case 0x43: gs.c[reg - 0x42].alpha = v; break;
    case 0x44: gs.dimx = v; break;
    case 0x45: gs.dthe = v; break;
    case 0x46: gs.colclamp = v; break;
    case 0x47: case 0x48: gs.c[reg - 0x47].test = v; break;
    case 0x49: gs.pabe = v; break;
    case 0x4A: case 0x4B: gs.c[reg - 0x4A].fba = v; break;
    case 0x4C: case 0x4D: gs.c[reg - 0x4C].frame = v; split_apply(reg - 0x4C); break;
    case 0x4E: case 0x4F: gs.c[reg - 0x4E].zbuf = v; break;
    case 0x50: gs.bitbltbuf = v; break;
    case 0x51: gs.trxpos = v; break;
    case 0x52: gs.trxreg = v; break;
    case 0x53: gs.trxdir = v; trx_start(); break;
    case 0x54: { uint8_t b[8]; memcpy(b, &v, 8); trx_data(b, 8); break; }
    case 0x60: gs.siglblid = (gs.siglblid & ~0xffffffffull) | ((uint32_t)v & (uint32_t)(v >> 32)); __atomic_or_fetch(&gs.csr, 1, __ATOMIC_SEQ_CST);
        if (!(gs.imr & 0x100)) kernel_raise_intc(0); break;
    case 0x61: __atomic_or_fetch(&gs.csr, 2, __ATOMIC_SEQ_CST); if (!(gs.imr & 0x200)) kernel_raise_intc(0); break;
    case 0x62: gs.siglblid = (gs.siglblid & 0xffffffffull) | ((uint64_t)((uint32_t)v & (uint32_t)(v >> 32)) << 32); break;
    default: break;
    }
}

/* ------------------------------------------------------------------ GIF */
typedef struct {
    int busy;          /* inside a packet */
    uint32_t nloop, nreg, flg, eop;
    uint64_t regs;
    uint32_t reg_i;
} GifPath;
static GifPath paths[4];

static void gif_packed(const uint8_t *q, uint32_t reg) {
    uint64_t lo, hi; memcpy(&lo, q, 8); memcpy(&hi, q + 8, 8);
    uint32_t w0 = (uint32_t)lo, w1 = (uint32_t)(lo >> 32), w2 = (uint32_t)hi, w3 = (uint32_t)(hi >> 32);
    switch (reg) {
    case 0x0: gs_write_reg(0x00, lo & 0x7ff); break;
    case 0x1: gs.rgbaq = (w0 & 255) | ((uint64_t)(w1 & 255) << 8) | ((uint64_t)(w2 & 255) << 16) | ((uint64_t)(w3 & 255) << 24);
              { uint32_t u; memcpy(&u, &gs.q, 4); gs.rgbaq |= (uint64_t)u << 32; } break;
    case 0x2: gs.st = lo; memcpy(&gs.q, &w2, 4); break;
    case 0x3: gs.uv = (w0 & 0x3fff) | ((uint64_t)(w1 & 0x3fff) << 16); break;
    case 0x4: {
        int adc = (w3 >> 15) & 1;
        vertex_kick(!adc, w0 & 0xffff, w1 & 0xffff, (w2 >> 4) & 0xffffff, (w3 >> 4) & 0xff);
        break;
    }
    case 0x5: { int adc = (w3 >> 15) & 1;
        static int vstats_on = -1; if (vstats_on < 0) vstats_on = getenv("AE2_VSTATS") != NULL;
        if (vstats_on) { static uint64_t n, nadc, ncenter, nfar; static int nq0; n++; nadc += adc;
            { extern uint32_t g_vu1_last_entry; static uint64_t perprog[0x800][2];
              uint32_t e = (g_vu1_last_entry >> 3) & 0x7ff; perprog[e][0]++;
              if (!adc && (abs((int)(w0 & 0xffff) - 0x8000) > 0x3000 || abs((int)(w1 & 0xffff) - 0x8000) > 0x3000)) perprog[e][1]++;
              if ((n % 300000) == 0) for (int i = 0; i < 0x800; i++) if (perprog[i][0]) fprintf(stderr, "[vstats] prog %04x: verts=%llu far_drawn=%llu\n", i * 8, (unsigned long long)perprog[i][0], (unsigned long long)perprog[i][1]); }
            if ((w0 & 0xffff) == 0x8000 && (w1 & 0xffff) == 0x8000) ncenter++;
            if (abs((int)(w0 & 0xffff) - 0x8000) > 0x2000 || abs((int)(w1 & 0xffff) - 0x8000) > 0x2000) nfar++;
            if (gs.q == 0.0f) nq0++;
            if ((n % 200000) == 0) fprintf(stderr, "[vstats] n=%llu adc=%llu center=%llu far=%llu q0=%d\n", (unsigned long long)n, (unsigned long long)nadc, (unsigned long long)ncenter, (unsigned long long)nfar, nq0); }
        vertex_kick(!adc, w0 & 0xffff, w1 & 0xffff, w2, bits(gs.fog, 56, 8)); break; }
    case 0xA: gs.fog = (uint64_t)((w3 >> 4) & 0xff) << 56; break;
    case 0xC: vertex_kick(0, w0 & 0xffff, w1 & 0xffff, (w2 >> 4) & 0xffffff, (w3 >> 4) & 0xff); break;
    case 0xD: vertex_kick(0, w0 & 0xffff, w1 & 0xffff, w2, bits(gs.fog, 56, 8)); break;
    case 0xE: gs_write_reg((uint32_t)(hi & 0xff), lo); break;
    case 0xF: break;
    default: gs_write_reg(reg, lo); break;  /* TEX0_1/2, CLAMP_1/2 map 1:1 (0x6..0x9) */
    }
}

/* AE2_GIFREC=file: record the GS input stream (GIF data, privileged writes,
 * vsyncs) for offline replay/regression tests (tools/gsreplay). */
static FILE *gifrec; static int gifrec_init;
static void gifrec_open(void) { gifrec_init = 1; }
void gs_save_state(FILE *f);
static void gifrec_put(uint32_t type, uint32_t a, uint64_t b, const void *data, uint32_t n) {
    uint32_t h[4] = {type, a, (uint32_t)b, (uint32_t)(b >> 32)};
    fwrite(h, 4, 4, gifrec);
    if (n) fwrite(data, 1, n, gifrec);
}
static void gif_transfer_impl(const uint8_t *data, uint32_t qwc, int path) {
    if (!gifrec_init) gifrec_open();
    if (gifrec) gifrec_put(1, (uint32_t)path, qwc, data, qwc * 16);
    GifPath *p = &paths[path & 3];
    for (uint32_t i = 0; i < qwc; i++) {
        const uint8_t *q = data + i * 16;
        if (!p->busy) {
            uint64_t lo, hi; memcpy(&lo, q, 8); memcpy(&hi, q + 8, 8);
            p->nloop = lo & 0x7fff; p->eop = (lo >> 15) & 1;
            uint32_t pre = (lo >> 46) & 1, prim = (lo >> 47) & 0x7ff;
            p->flg = (lo >> 58) & 3; p->nreg = (lo >> 60) & 15; if (!p->nreg) p->nreg = 16;
            p->regs = hi; p->reg_i = 0;
            if (pre && p->flg == 0) gs_write_reg(0x00, prim);
            if (p->nloop) p->busy = 1;
            continue;
        }
        if (p->flg == 0) {
            gif_packed(q, (uint32_t)(p->regs >> (p->reg_i * 4)) & 15);
            if (++p->reg_i >= p->nreg) { p->reg_i = 0; if (--p->nloop == 0) p->busy = 0; }
        } else if (p->flg == 1) {
            for (int h = 0; h < 2 && p->busy; h++) {
                uint64_t v; memcpy(&v, q + h * 8, 8);
                uint32_t reg = (uint32_t)(p->regs >> (p->reg_i * 4)) & 15;
                if (reg == 0xE || reg == 0xF) { /* NOP in reglist */ }
                else if (reg == 0x4 || reg == 0x5 || reg == 0xC || reg == 0xD || reg <= 0x3 || reg == 0xA) {
                    static const uint8_t map[16] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0A, 0x0C, 0x0D, 0, 0};
                    gs_write_reg(map[reg], v);
                } else gs_write_reg(reg, v);
                if (++p->reg_i >= p->nreg) { p->reg_i = 0; if (--p->nloop == 0) p->busy = 0; }
            }
        } else {   /* IMAGE: hand over the whole run of qwords at once */
            uint32_t k = qwc - i; if (k > p->nloop) k = p->nloop;
            trx_data(q, k * 16);
            i += k - 1;
            if ((p->nloop -= k) == 0) p->busy = 0;
        }
    }
}

/* ------------------------------------------------------------------ privileged registers */
static void gs_write_priv_impl(uint32_t a, uint64_t v) {
    if (!gifrec_init) gifrec_open();
    if (gifrec) gifrec_put(2, a, v, NULL, 0);
    uint32_t o = a & 0x1fff;
    if (o < 0x1000) { gs.priv[(o >> 4) & 0x1f] = v; return; }
    switch (o) {
    case 0x1000:
        __atomic_and_fetch(&gs.csr, ~(v & 0x1f), __ATOMIC_SEQ_CST);   /* clear SIGNAL/FINISH/HSINT/VSINT/EDWINT */
        if (v & 0x200) { gs_flush_r(4); memset(paths, 0, sizeof paths); nv = 0; }
        break;
    case 0x1010: gs.imr = v; break;
    case 0x1040: gs.busdir = v; break;
    case 0x1080: gs.siglblid = v; break;
    case 0x1ff0: gs_skip = (int)v; break;   /* port-private: frame skip on/off */
    case 0x1fe0:   /* port-private: split-screen half */
        {   /* 2 and 4 are the second view's list (see split_vram) */
            int was = split_mode == 2 || split_mode == 4, now = (int)v == 2 || (int)v == 4;
            if (now && !was) split_vram(1);
            if (!now && was) split_vram(0);
        }
        split_mode = (int)v; split_apply(0); split_apply(1); state_dirty = 1; break;
    case 0x1ff8: state_dirty = 1; break;    /* port-private: texture patches changed (re-check textures) */
    }
}
static uint64_t gs_read_priv_impl(uint32_t a) {
    uint32_t o = a & 0x1fff;
    if (o < 0x1000) return gs.priv[(o >> 4) & 0x1f];
    switch (o) {
    case 0x1000: return (gs.csr & 0x1f) | ((uint64_t)csr_field << 13) | (1ull << 14) | (0x1bull << 16) | (0x55ull << 24);
    case 0x1010: return gs.imr;
    case 0x1080: return gs.siglblid;
    }
    return 0;
}

void gs_set_crt(int interlace, int mode, int field) {
    INFO("[gs] SetGsCrt interlace=%d mode=%d field=%d\n", interlace, mode, field);
}

/* ------------------------------------------------------------------ display */
static uint32_t *outbuf;
static int out_w, out_h;

static void read_display(int n, uint32_t *dst, int w, int h) {
    uint64_t fb = gs.priv[n ? 9 : 7], disp = gs.priv[n ? 10 : 8];
    uint32_t fbp = bits(fb, 0, 9) * 32, fbw = bits(fb, 9, 6), psm = bits(fb, 15, 5);
    uint32_t dbx = bits(fb, 32, 11), dby = bits(fb, 43, 11);
    (void)disp;
    if (psm == PSMCT32 || psm == PSMCT24 || psm == PSMCT16 || psm == PSMCT16S) {
        const uint16_t *v16 = (const uint16_t *)vram;
        for (int y = 0; y < h; y++) {
            RowAddr r; row_addr(&r, fbp, fbw, psm, (int)((y + dby) & 2047));
            uint32_t *o = dst + y * w;
            if (r.is16) for (int x = 0; x < w; x++) {
                uint32_t c = v16[ra16(&r, (int)((x + dbx) & 2047))];
                o[x] = ((c & 0x1f) << 3) | (((c >> 5) & 0x1f) << 11) | (((c >> 10) & 0x1f) << 19) | 0xff000000u;
            } else for (int x = 0; x < w; x++) o[x] = vram[ra32(&r, (int)((x + dbx) & 2047))] | 0xff000000u;
        }
        return;
    }
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint32_t c = vread(psm, fbp, fbw, x + dbx, y + dby);
            if (psm == PSMCT16 || psm == PSMCT16S)
                c = ((c & 0x1f) << 3) | (((c >> 5) & 0x1f) << 11) | (((c >> 10) & 0x1f) << 19);
            dst[y * w + x] = c | 0xff000000u;
        }
    }
}

int g_dump_frames;
static void dump_vram_fb(const char *path, uint32_t fbp, uint32_t fbw, uint32_t psm, int w, int h) {
    FILE *f = fopen(path, "wb"); if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        uint32_t c = vread(psm, fbp, fbw, x, y);
        if (psm == PSMCT16 || psm == PSMCT16S) c = ((c & 0x1f) << 3) | (((c >> 5) & 0x1f) << 11) | (((c >> 10) & 0x1f) << 19);
        uint8_t px[3] = {c & 255, (c >> 8) & 255, (c >> 16) & 255}; fwrite(px, 1, 3, f);
    }
    fclose(f);
}
#include "gs_hw.h"

/* Split screen: the second view's draw list uploads the textures it needs into the
 * texture area of VRAM (above the two frame buffers and the Z buffer), where the game's
 * own bookkeeping (thrown away with the rest of that pass) does not know about them.
 * The texture area is put back as it was once that list has been drawn. */
#define SPLIT_TEX_W0 0xc0000u   /* first word of the texture area */
static uint32_t *split_save;
static void split_vram(int save) {
    const uint32_t n = VRAM_WORDS - SPLIT_TEX_W0, bp = SPLIT_TEX_W0 / 64, nb = n / 64;
    if (!split_save) split_save = malloc((size_t)n * 4);
    gs_flush_r(4);
    if (hw_on) {   /* drawing into the texture area lives on the GPU until read back */
        hw_flush();
        for (int i = 0; i < HW_NRT; i++) if (hw_rt[i].used && hw_rt_overlaps(&hw_rt[i], bp, nb)) { hw_download(&hw_rt[i]); if (!save) hw_rt_drop(&hw_rt[i]); }
    }
    /* only the pages the list wrote are put back and marked changed, so the textures
     * cached from the other pages stay valid */
    static uint64_t since;
    if (save) { memcpy(split_save, vram + SPLIT_TEX_W0, (size_t)n * 4); since = ++global_seq; return; }
    global_seq++;
    int any = 0;
    for (uint32_t pg = SPLIT_TEX_W0 >> 11; pg < 512; pg++) {
        if (page_seq[pg] < since) continue;
        uint32_t w = (pg << 11) - SPLIT_TEX_W0;
        memcpy(vram + SPLIT_TEX_W0 + w, split_save + w, 2048 * 4);
        page_seq[pg] = global_seq; any = 1;
    }
    if (any) state_dirty = 1;
}
static void texdump_check(const TexState *t) {
    static int done;
    if (done || g_frame < (uint32_t)texdump_frame || t->tbp != texdump_bp) return;
    done = 1;
    if (hw_on) hw_sync_all();
    char path[64]; snprintf(path, sizeof path, "texdump_%x_%s.ppm", t->tbp, hw_on ? "gpu" : "sw");
    dump_vram_fb(path, t->tbp, t->tbw, PSMCT32, 1 << t->tw, 1 << t->th);
    {   /* decoded through the texture path (palette applied), RGBA */
        int w = 1 << t->tw, h = 1 << t->th; uint32_t *px = malloc((size_t)w * h * 4);
        tex_decode(t, px, w, h);
        snprintf(path, sizeof path, "texdump_%x.rgba", t->tbp);
        FILE *f = fopen(path, "wb"); if (f) { fwrite(px, 4, (size_t)w * h, f); fclose(f); }
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) px[y * w + x] = vread(t->psm, t->tbp, t->tbw, x, y);   /* raw texels (palette indices) */
        snprintf(path, sizeof path, "texdump_%x.idx", t->tbp);
        f = fopen(path, "wb"); if (f) { fwrite(px, 4, (size_t)w * h, f); fclose(f); }
        INFO("[gs] texture %x: tbw %u psm %x cpsm %x csa %u\n", t->tbp, t->tbw, t->psm, t->cpsm, t->csa);
        free(px);
    }
    INFO("[gs] texture %x (psm %x, %dx%d) written to %s\n", t->tbp, t->psm, 1 << t->tw, 1 << t->th, path);
}

static void gs_vsync_impl(void) {
    if (unlikely(drawlog_on)) {
        if (drawlog_on < 0) { const char *e = getenv("AE2_DRAWLOG"); drawlog_on = e != NULL; if (e) sscanf(e, "%u,%u", &drawlog_from, &drawlog_n); }
        drawlog_vs++;
        if (drawlog_active) { drawlog_flush(); fprintf(stderr, "[draw] ---- vsync %u  pmode %llx dispfb1 %llx dispfb2 %llx\n", drawlog_vs, (unsigned long long)gs.priv[0], (unsigned long long)gs.priv[7], (unsigned long long)gs.priv[9]); }
        drawlog_active = drawlog_on && drawlog_vs >= drawlog_from && drawlog_vs < drawlog_from + drawlog_n;
    }
    { static uint32_t n; if (survey_on > 0 && ++n % 2000 == 0) { fprintf(stderr, "[survey] ---- after %u vsyncs\n", n); survey_dump(); } }
    { static int start = -2; if (start == -2) start = getenv("AE2_GIFREC") ? (getenv("AE2_GIFREC_START") ? atoi(getenv("AE2_GIFREC_START")) : 1) : -1;
      if (start >= 0 && g_frame == (uint32_t)start && !gifrec) {
          gs_flush_r(5);
          gifrec = fopen(getenv("AE2_GIFREC"), "wb");
          if (gifrec) { gifrec_put(4, 0, 0, NULL, 0); gs_save_state(gifrec); INFO("[gs] GIF recording started at vsync %u\n", g_frame); }
      } }
    if (gifrec) { gifrec_put(3, 0, 0, NULL, 0);
        static int left = -2; if (left == -2) left = getenv("AE2_GIFREC_FRAMES") ? atoi(getenv("AE2_GIFREC_FRAMES")) : 1000;
        if (--left <= 0) { fclose(gifrec); gifrec = NULL; INFO("[gs] GIF recording finished\n"); } }
    gs_flush_r(6);
    csr_field ^= 1;
    __atomic_or_fetch(&gs.csr, 8, __ATOMIC_SEQ_CST);   /* VSINT */
    uint64_t pmode = gs.priv[0];
    int en1 = pmode & 1, en2 = (pmode >> 1) & 1;
    int n = en1 ? 0 : (en2 ? 1 : 0);
    uint64_t disp = gs.priv[n ? 10 : 8];
    int magh = bits(disp, 23, 4) + 1, magv = bits(disp, 27, 2) + 1;
    int w = (bits(disp, 32, 12) + 1) / magh, h = (bits(disp, 44, 11) + 1) / magv;
    if (w <= 0 || w > 1024) w = 640;
    if (h <= 0 || h > 1024) h = 448;
    uint64_t smode2 = gs.priv[2];
    int interlace = smode2 & 1, ffmd = (smode2 >> 1) & 1;
    if (interlace && ffmd) h /= 2;   /* field mode: framebuffer holds one field */
    if (w != out_w || h != out_h || !outbuf) {
        free(outbuf);
        outbuf = malloc((size_t)w * h * 4);
        out_w = w; out_h = h;
    }
    /* renderer switches happen here, between frames */
    if (unlikely(g_hw_request >= 0) && g_hw_request != hw_on && !hw_failed) {
        if (g_hw_request) { if (gpu_init()) { hw_on = 1; hw_drop_targets(); } else g_hw_request = 0; }
        else { hw_sync_all(); hw_drop_targets(); hw_on = 0;
               for (int i = 0; i < TC_N; i++) tcache[i].seq = 0;   /* software decodes start afresh */ }
    }
    if (hw_on && g_hw_scale != hw_scale && g_hw_scale >= 1 && g_hw_scale <= 8) {
        hw_sync_all(); hw_drop_targets(); hw_scale = g_hw_scale;   /* targets come back from VRAM at the new scale */
    }
    g_hw_active = hw_on ? hw_scale : 0;
    if (hw_on) { hw_vsync(n, w, h, !en1 && !en2); goto skip_present; }
    if (!en1 && !en2) memset(outbuf, 0, (size_t)w * h * 4);
    else if (fb_stale[bits(gs.priv[n ? 9 : 7], 0, 9)]) goto skip_present;   /* drawn by a skipped frame */
    else read_display(n, outbuf, w, h);
    gs_frame_ready(outbuf, w, h);
skip_present:;
    static int gsdump = -2; if (gsdump == -2) gsdump = getenv("AE2_GSDUMP") ? atoi(getenv("AE2_GSDUMP")) : -1;
    if (gsdump >= 0 && g_frame == (uint32_t)gsdump) {
        fprintf(stderr, "[gsdump] PMODE=%llx SMODE1=%llx SMODE2=%llx DISPFB1=%llx DISPLAY1=%llx DISPFB2=%llx DISPLAY2=%llx BGCOLOR=%llx\n",
            (unsigned long long)gs.priv[0], (unsigned long long)gs.priv[1], (unsigned long long)gs.priv[2], (unsigned long long)gs.priv[7],
            (unsigned long long)gs.priv[8], (unsigned long long)gs.priv[9], (unsigned long long)gs.priv[10], (unsigned long long)gs.priv[14]);
        for (int k = 0; k < 2; k++) fprintf(stderr, "[gsdump] ctx%d FRAME=%llx ZBUF=%llx SCISSOR=%llx XYOFFSET=%llx\n", k,
            (unsigned long long)gs.c[k].frame, (unsigned long long)gs.c[k].zbuf, (unsigned long long)gs.c[k].scissor, (unsigned long long)gs.c[k].xyoffset);
        dump_vram_fb("vram0.ppm", 0, 8, PSMCT32, 512, 1024); dump_vram_fb("vram1.ppm", 0x1000, 8, PSMCT32, 512, 1024);
    }
    static uint32_t vs_count;   /* counted here: g_frame belongs to the EE thread under MTGS */
    if ((++vs_count % 50) == 0) {
        static uint64_t last_wall, last_pix; static uint32_t last_polls; uint64_t now = host_usec(), pix = gs_pixels_drawn();
        extern uint32_t g_pad_polls;
        INFO("[gs] frame %u: game fps %.1f, %llu prims, %llu px, EE waited %llu ms in %llu flushes, %llu tex decodes, %llu ms wall (last 50 vsyncs), display %dx%d fb%d\n", g_frame,
             last_wall ? (g_pad_polls - last_polls) * 1e6 / (double)(now - last_wall) : 0.0,
             (unsigned long long)draw_count, (unsigned long long)(pix - last_pix), (unsigned long long)flush_us / 1000,
             (unsigned long long)flush_count, (unsigned long long)tc_decodes,
             (unsigned long long)(last_wall ? (now - last_wall) / 1000 : 0), w, h, n + 1);
        last_wall = now; last_pix = pix; last_polls = g_pad_polls;
        if (g_log_level >= 2 && nworkers) { fprintf(stderr, "[gs] flushes:"); for (int i = 0; i < 12; i++) if (flush_reason[i]) fprintf(stderr, " %s=%llu", flush_reason_name[i], (unsigned long long)flush_reason[i]); fprintf(stderr, "\n"); memset(flush_reason, 0, sizeof flush_reason); }
        draw_count = 0; flush_us = 0; flush_count = 0; tc_decodes = 0;
        gstat_dump();
    }
}

/* Snapshot of everything the GS input stream depends on (for replays). */
void gs_save_state(FILE *f) {
    hw_sync_all();
    fwrite(vram, 4, VRAM_WORDS, f); fwrite(&gs, sizeof gs, 1, f); fwrite(paths, sizeof paths, 1, f);
    fwrite(vq, sizeof vq, 1, f); fwrite(&nv, sizeof nv, 1, f); fwrite(&trx, sizeof trx, 1, f); fwrite(&csr_field, sizeof csr_field, 1, f);
}
void gs_load_state(FILE *f) {
    if (fread(vram, 4, VRAM_WORDS, f) != VRAM_WORDS) return;
    if (fread(&gs, sizeof gs, 1, f) != 1 || fread(paths, sizeof paths, 1, f) != 1 || fread(vq, sizeof vq, 1, f) != 1 ||
        fread(&nv, sizeof nv, 1, f) != 1 || fread(&trx, sizeof trx, 1, f) != 1 || fread(&csr_field, sizeof csr_field, 1, f) != 1) return;
    state_dirty = 1;
}
void gs_init(void) {
    vram = aligned_alloc(64, VRAM_WORDS * 4);
    memset(vram, 0, VRAM_WORDS * 4);
    memset(&gs, 0, sizeof gs);
    { const char *e = getenv("AE2_TEXDUMP"); texdump_frame = -1; if (e) { unsigned bp; long f; if (sscanf(e, "%ld:%x", &f, &bp) == 2) { texdump_frame = f; texdump_bp = bp; } } }
    init_addr_tables();
    init_tex_tables();
    memset(tc_head, 0xff, sizeof tc_head);
    extern int g_hw_request;
    gs_start_workers(1);   /* idle (asleep) while the GPU renderer is active; needed if the menu switches back */
    gs_mt_start();
}

/* XGKICK: feed VU1 memory to GIF PATH1 up to and including the end-of-packet tag. */
void gif_path1_kick(const uint8_t *mem, uint32_t addr) {
    static uint8_t pkt[16384];
    uint32_t n = 0, pos = addr & 0x3ff;
    for (int guard = 0; guard < 64 && n < 1024; guard++) {
        const uint8_t *tq = mem + pos * 16;
        uint64_t lo; memcpy(&lo, tq, 8);
        uint32_t nloop = lo & 0x7fff, eop = (lo >> 15) & 1, flg = (lo >> 58) & 3, nreg = (lo >> 60) & 15;
        if (!nreg) nreg = 16;
        uint32_t len = flg == 0 ? nloop * nreg : flg == 1 ? (nloop * nreg + 1) / 2 : nloop;
        for (uint32_t i = 0; i <= len && n < 1024; i++) { memcpy(pkt + n * 16, mem + ((pos + i) & 0x3ff) * 16, 16); n++; }
        pos = (pos + 1 + len) & 0x3ff;
        if (eop) break;
    }
    { extern int g_vu1_stats; extern uint32_t g_vu1_cur; extern uint64_t g_vu1_st[2048][4];
      if (g_vu1_stats > 0) { g_vu1_st[g_vu1_cur][1]++; g_vu1_st[g_vu1_cur][2] += n;
        /* AE2_VU1SKIP=entry: drop that program's output, to see what it draws */
        static int skip = -2; if (skip == -2) { const char *e = getenv("AE2_VU1SKIP"); skip = e ? (int)strtoul(e, 0, 16) : -1; }
        if (skip >= 0 && g_vu1_cur == (uint32_t)skip >> 3) return; } }
    gif_transfer(pkt, n, 1);
}

/* ------------------------------------------------------------------ GS thread
 * The EE side only copies GS input (GIF packets, privileged register writes,
 * vsync) into a ring buffer; a dedicated thread parses it and feeds the
 * rasteriser workers. Reads of GS state that the EE needs synchronously are
 * served from shadows (privileged registers) or atomically shared state (CSR).
 *
 * MTVU: with a VU1 thread, VIF1 input is queued too, and the EE puts everything in the
 * VU1 thread's queue instead: that thread runs VIF1 and the VU1 microprograms and passes
 * their XGKICK/DIRECT output, and the EE's own GS input, on to the GS thread's queue,
 * so the GS front end sees one stream in the original order. EE, VU1 and GS front end
 * then run on three cores. */
#define GQ_SIZE (32u << 20)
enum { GQ_GIF = 1, GQ_PRIV = 2, GQ_VSYNC = 3, GQ_WRAP = 4, GQ_VIF1 = 5, GQ_VIF1RESET = 6 };
typedef struct { uint32_t type, len, a, b; uint64_t v, pad; } GQHdr;
typedef struct {
    uint8_t *buf;
    uint64_t w, r, need;      /* w and need: the producer's; r: the consumer's */
    pthread_mutex_t m;
    pthread_cond_t cv, cv_done;
    int sleeping, waiters;
    uint64_t last_wake, last_bc;
} Queue;
static Queue gsq = {NULL, 0, 0, 0, PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0, 0, 0};
static Queue vuq = {NULL, 0, 0, 0, PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0, 0, 0};
static int mtvu;
static __thread int on_gs_thread, on_vu_thread;
/* AE2_MTSTAT=1: every 250 vsyncs, how busy the VU1 and GS threads were and how long the EE waited */
static int mtstat; static uint64_t busy_us[2], ee_wait_total;
static uint64_t real_usec(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000; }
void vif1_reset_now(void);
static uint64_t gq_vs_pushed, gq_vs_done;
static uint64_t priv_shadow[0x20], imr_shadow;
static uint64_t ee_wait_us;
/* frame handoff to the presenting (EE) side */
static pthread_mutex_t fr_m = PTHREAD_MUTEX_INITIALIZER;
static uint32_t *fr_buf; static int fr_w, fr_h, fr_cap;

static void gs_frame_ready(const uint32_t *p, int w, int h) {
    if (!mtgs) { host_present(p, w, h); return; }
    pthread_mutex_lock(&fr_m);
    if (w * h > fr_cap) { free(fr_buf); fr_buf = malloc((size_t)w * h * 4); fr_cap = w * h; }
    memcpy(fr_buf, p, (size_t)w * h * 4); fr_w = w; fr_h = h;
    pthread_mutex_unlock(&fr_m);
}

static void mq_wake_consumer(Queue *q) {
    if (__atomic_load_n(&q->sleeping, __ATOMIC_SEQ_CST)) { pthread_mutex_lock(&q->m); pthread_cond_signal(&q->cv); pthread_mutex_unlock(&q->m); }
}
/* wait until cond(q) holds; q is the queue whose consumer makes progress towards it */
static void mq_wait(Queue *q, int (*cond)(Queue *)) {
    uint64_t t0 = host_usec(), r0 = mtstat ? real_usec() : 0;
    for (int spins = 0; !cond(q); spins++) {
        if (spins < 2000) { __builtin_ia32_pause(); continue; }
        mq_wake_consumer(q);
        pthread_mutex_lock(&q->m);
        q->waiters++;
        if (!cond(q)) {
            struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 2000000; if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
            pthread_cond_timedwait(&q->cv_done, &q->m, &ts);
        }
        q->waiters--;
        pthread_mutex_unlock(&q->m);
    }
    if (!on_vu_thread) { ee_wait_us += host_usec() - t0; if (mtstat) ee_wait_total += real_usec() - r0; }
}
static int mq_has_space(Queue *q) { return q->w + q->need - __atomic_load_n(&q->r, __ATOMIC_SEQ_CST) <= GQ_SIZE; }
static int mq_empty(Queue *q) { return __atomic_load_n(&q->r, __ATOMIC_SEQ_CST) == q->w; }
static int gq_vs_ok(Queue *q) { (void)q; return gq_vs_pushed - __atomic_load_n(&gq_vs_done, __ATOMIC_SEQ_CST) <= 2; }

static void mq_push(Queue *q, uint32_t type, uint32_t a, uint32_t b, uint64_t v, const void *data, uint32_t len) {
    uint32_t total = (uint32_t)sizeof(GQHdr) + ((len + 15) & ~15u);
    uint32_t off = (uint32_t)(q->w % GQ_SIZE);
    if (off + total > GQ_SIZE) {   /* not enough room before the end: wrap */
        q->need = (GQ_SIZE - off) + total;
        if (!mq_has_space(q)) mq_wait(q, mq_has_space);
        GQHdr *h = (GQHdr *)(q->buf + off); h->type = GQ_WRAP;
        __atomic_store_n(&q->w, q->w + (GQ_SIZE - off), __ATOMIC_SEQ_CST);
        off = 0;
    }
    q->need = total;
    if (!mq_has_space(q)) mq_wait(q, mq_has_space);
    GQHdr *h = (GQHdr *)(q->buf + off);
    h->type = type; h->len = len; h->a = a; h->b = b; h->v = v;
    if (len) memcpy(h + 1, data, len);
    __atomic_store_n(&q->w, q->w + total, __ATOMIC_SEQ_CST);
    /* wake a sleeping consumer once there is a worthwhile amount of work */
    if (type == GQ_VSYNC || q->w - q->last_wake >= 16384) { q->last_wake = q->w; mq_wake_consumer(q); }
}
/* the EE's GS input goes through the VU1 thread when there is one */
static Queue *ee_q(void) { return mtvu ? &vuq : &gsq; }

static void gs_item(GQHdr *h) {
    switch (h->type) {
    case GQ_GIF: gif_transfer_impl((const uint8_t *)(h + 1), h->len / 16, (int)h->a); break;
    case GQ_PRIV: gs_write_priv_impl(h->a, h->v); break;
    case GQ_VSYNC: gs_vsync_impl(); __atomic_add_fetch(&gq_vs_done, 1, __ATOMIC_SEQ_CST); break;
    }
}
static void vu_item(GQHdr *h) {
    switch (h->type) {
    case GQ_VIF1: vif_transfer(1, (const uint8_t *)(h + 1), h->len / 16); break;
    case GQ_VIF1RESET: vif1_reset_now(); break;
    default: mq_push(&gsq, h->type, h->a, h->b, h->v, h + 1, h->len);   /* the EE's GS input, in order */
             if (h->type == GQ_VSYNC) mq_wake_consumer(&gsq);
             break;
    }
}

/* consumer loop shared by the GS and VU1 threads */
static void mq_consume(Queue *q, void (*item)(GQHdr *), int stat) {
    uint64_t r = 0;
    for (;;) {
        if (r == __atomic_load_n(&q->w, __ATOMIC_SEQ_CST)) {
            if (q == &vuq) mq_wake_consumer(&gsq);   /* idle: let the GS thread have what is queued */
            int spins = 0;
            while (r == __atomic_load_n(&q->w, __ATOMIC_SEQ_CST) && spins < 4000) { spins++; __builtin_ia32_pause(); }
            if (r == __atomic_load_n(&q->w, __ATOMIC_SEQ_CST)) {
                pthread_mutex_lock(&q->m);
                __atomic_store_n(&q->sleeping, 1, __ATOMIC_SEQ_CST);
                while (r == __atomic_load_n(&q->w, __ATOMIC_SEQ_CST)) {
                    if (q->waiters) pthread_cond_broadcast(&q->cv_done);
                    pthread_cond_wait(&q->cv, &q->m);
                }
                __atomic_store_n(&q->sleeping, 0, __ATOMIC_SEQ_CST);
                pthread_mutex_unlock(&q->m);
                continue;
            }
        }
        GQHdr *h = (GQHdr *)(q->buf + r % GQ_SIZE);
        uint64_t tb = mtstat ? real_usec() : 0;
        uint32_t total = (uint32_t)sizeof(GQHdr) + ((h->len + 15) & ~15u);
        if (h->type == GQ_WRAP) total = (uint32_t)(GQ_SIZE - r % GQ_SIZE);
        else item(h);
        if (mtstat) busy_us[stat] += real_usec() - tb;
        r += total;
        __atomic_store_n(&q->r, r, __ATOMIC_SEQ_CST);
        /* waiters poll with a short timeout; wake them only at points they care about */
        if (__atomic_load_n(&q->waiters, __ATOMIC_SEQ_CST) &&
            (h->type == GQ_VSYNC || r == __atomic_load_n(&q->w, __ATOMIC_SEQ_CST) || r - q->last_bc > GQ_SIZE / 8)) {
            q->last_bc = r;
            pthread_mutex_lock(&q->m); pthread_cond_broadcast(&q->cv_done); pthread_mutex_unlock(&q->m);
        }
    }
}
static void *gs_thread_main(void *arg) { (void)arg; on_gs_thread = 1; mq_consume(&gsq, gs_item, 0); return NULL; }
/* VU1 runs in the guest's FPU mode (round toward zero, flush to zero) */
static void *vu_thread_main(void *arg) { (void)arg; on_vu_thread = 1; rt_fpu_enter_guest(); mq_consume(&vuq, vu_item, 1); return NULL; }

static void gs_mt_start(void) {
    const char *e = getenv("AE2_MTGS"); mtgs = e ? atoi(e) : plat_ncpu() >= 2;
    /* GS progress must not depend on host timing (AE2_MTGS=2 forces the threads, for testing) */
    if (rt_deterministic() && !(e && atoi(e) == 2)) mtgs = 0;
    mtstat = getenv("AE2_MTSTAT") != NULL;
    e = getenv("AE2_MTVU"); mtvu = mtgs && (e ? atoi(e) : 1);
    if (mtgs) {
        gsq.buf = aligned_alloc(64, GQ_SIZE);
        pthread_t th; pthread_create(&th, NULL, gs_thread_main, NULL); pthread_detach(th);
        if (mtvu) { vuq.buf = aligned_alloc(64, GQ_SIZE); pthread_create(&th, NULL, vu_thread_main, NULL); pthread_detach(th); }
        INFO("[gs] GS front-end runs on its own thread%s\n", mtvu ? ", VIF1 and VU1 on another" : "");
    }
}
/* wait until the VU1 and GS threads have done everything queued so far (EE side only) */
void gs_mt_sync(void) {
    if (!mtgs || on_gs_thread || on_vu_thread) return;
    if (mtvu && !mq_empty(&vuq)) mq_wait(&vuq, mq_empty);
    if (!mq_empty(&gsq)) mq_wait(&gsq, mq_empty);
}

void vif1_submit(const uint8_t *data, uint32_t qwc) {
    if (!mtvu || on_vu_thread) { vif_transfer(1, data, qwc); return; }
    mq_push(&vuq, GQ_VIF1, 0, 0, 0, data, qwc * 16);
}
void vif1_reset(void) {
    if (!mtvu || on_vu_thread) { vif1_reset_now(); return; }
    mq_push(&vuq, GQ_VIF1RESET, 0, 0, 0, NULL, 0);
}

void gif_transfer(const uint8_t *data, uint32_t qwc, int path) {
    if (on_gs_thread || !mtgs) { gif_transfer_impl(data, qwc, path); return; }
    if (on_vu_thread) { mq_push(&gsq, GQ_GIF, (uint32_t)path, 0, 0, data, qwc * 16); return; }   /* PATH1/PATH2 from VIF1 */
    mq_push(ee_q(), GQ_GIF, (uint32_t)path, 0, 0, data, qwc * 16);
}
void gs_write_priv64(uint32_t a, uint64_t v) {
    uint32_t o = a & 0x1fff;
    if (o < 0x1000) priv_shadow[(o >> 4) & 0x1f] = v;
    else if (o == 0x1010) imr_shadow = v;
    else if (o == 0x1000) __atomic_and_fetch(&gs.csr, ~(v & 0x1f), __ATOMIC_SEQ_CST);
    if (!mtgs) { gs_write_priv_impl(a, v); return; }
    mq_push(ee_q(), GQ_PRIV, a, 0, v, NULL, 0);
}
uint64_t gs_read_priv64(uint32_t a) {
    if (!mtgs) return gs_read_priv_impl(a);
    uint32_t o = a & 0x1fff;
    if (o < 0x1000) return priv_shadow[(o >> 4) & 0x1f];
    switch (o) {
    case 0x1000: return (__atomic_load_n(&gs.csr, __ATOMIC_SEQ_CST) & 0x1f) | ((uint64_t)ee_field << 13) | (1ull << 14) | (0x1bull << 16) | (0x55ull << 24);
    case 0x1010: return imr_shadow;
    case 0x1080: return __atomic_load_n(&gs.siglblid, __ATOMIC_SEQ_CST);
    }
    return 0;
}
void gs_vsync(void) {
    if (!mtgs) { gs_vsync_impl(); return; }
    ee_field ^= 1;
    __atomic_or_fetch(&gs.csr, 8, __ATOMIC_SEQ_CST);
    /* keep at most two frames in flight */
    if (!gq_vs_ok(NULL)) mq_wait(&gsq, gq_vs_ok);
    gq_vs_pushed++;
    mq_push(ee_q(), GQ_VSYNC, 0, 0, 0, NULL, 0);
    /* the GPU renderer presents from the GS thread (hw_vsync); a software frame
     * here would race it and flash an old or black picture */
    if (!__atomic_load_n(&g_hw_active, __ATOMIC_RELAXED)) {
        pthread_mutex_lock(&fr_m);
        if (fr_buf) host_present(fr_buf, fr_w, fr_h);
        else { static uint32_t black[640 * 448]; host_present(black, 640, 448); }
        pthread_mutex_unlock(&fr_m);
    }
    if (mtstat && (g_frame % 250) == 0) {
        static uint64_t last; uint64_t now = real_usec();
        fprintf(stderr, "[mtstat] frame %u: %.0f ms wall, VU1 thread busy %.0f ms, GS thread busy %.0f ms, EE waited %.0f ms\n", g_frame, (now - last) / 1e3,
                busy_us[1] / 1e3, busy_us[0] / 1e3, ee_wait_total / 1e3);
        last = now; busy_us[0] = busy_us[1] = 0; ee_wait_total = 0;
    }
    if ((g_frame % 50) == 0) { INFO("[gs] EE waited %llu ms for the GS thread (last 50 vsyncs)\n", (unsigned long long)ee_wait_us / 1000); ee_wait_us = 0; }
}

/* Read a 32-bit rectangle of VRAM (debug captures and host-side tools). */
void gs_debug_read32(uint32_t bp, uint32_t bw, int w, int h, uint32_t *out) {
    gs_mt_sync();
    if (hw_on) hw_sync_all();
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) out[y * w + x] = vread(PSMCT32, bp, bw, x, y);
}
