/* SPU2 sound processor emulation, driven at the libsd API level.
 *
 *  - 2 cores x 24 voices: VAG ADPCM decoding, ADSR envelopes, pitch with
 *    4-point interpolation, fixed/sweep volumes, loop flags, ENDX, KON/KOFF.
 *  - Core sound-data input (ADMA): 16-bit PCM blocks (256 L + 256 R samples)
 *    read from IOP memory in a looping double buffer, with the libsd transfer
 *    callback fired at every half-buffer boundary.
 *  - Output: 48 kHz stereo to SDL audio, or paced in real time without audio
 *    (headless) so that streaming timing stays correct; optional WAV capture.
 *  - Reverb: the SPU2's effect unit (the same algorithm as the PS1's SPU), set
 *    up from libsd's presets (sceSdSetEffectAttr), running at half rate in each
 *    core's effect work area in sound memory.
 *
 * Locking: spu_mtx protects all SPU state. Transfer callbacks run IOP module
 * code, so they are invoked outside spu_mtx through spu2_cb_hook. */
#include "rt.h"
#include "spu2.h"
#include <pthread.h>
#include <math.h>
#include <unistd.h>
#ifdef HAVE_SDL
#include <SDL.h>
#endif

#define SPU_RAM_BYTES (4u << 20)   /* 2 MB on the PS2; the upper 2 MB holds player 2's voice bank (p2.c) */
static uint8_t *spu_ram;
static pthread_mutex_t spu_mtx = PTHREAD_MUTEX_INITIALIZER;

enum { ENV_OFF, ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE };

typedef struct {
    uint16_t voll, volr, pitch, adsr1, adsr2;
    int32_t volxl, volxr;          /* current volumes (0..0x7fff signed) */
    uint32_t ssa, lsa, nax;        /* byte addresses */
    int lsa_set;                   /* LSAX written by software since key on */
    /* playback */
    int on;
    int env_phase; int32_t env; uint32_t env_counter;
    uint32_t counter;              /* 12-bit fraction position within decoded block */
    int16_t buf[28]; int bufpos;
    int32_t s1, s2;                /* ADPCM history */
    int16_t hist[4];               /* interpolation history */
    uint32_t cur_block;            /* address of decoded block */
    uint8_t block_flags;
} Voice;

typedef struct {
    Voice v[24];
    uint32_t vmixl, vmixr, vmixel, vmixer, pmon, non, endx;
    uint16_t mvoll, mvolr, evoll, evolr, avoll, avolr, bvoll, bvolr, mmix;
    uint32_t esa, eea, tsa, irqa;
    uint16_t attr;
    /* reverb (see "reverb" below) */
    int fx_on;                     /* core attribute: effects enabled */
    int16_t rv[32];                /* the effect registers, in the PS1 order (dAPF1 .. vRIN) */
    uint32_t rv_start, rv_end;     /* work area, in 16-bit words (inclusive) */
    uint32_t rv_pos;               /* current base offset in the work area */
    int rv_mode;
    int32_t rv_in[2][64], rv_out[2][64];   /* down/upsampling histories (24 kHz <-> 48 kHz) */
    uint32_t rv_hp, rv_phase;
    /* ADMA */
    int adma_on, adma_loop;
    uint32_t adma_base, adma_size, adma_pos;   /* pos in samples */
    int adma_half;
} Core;

static Core core[2];
static uint32_t trans_cb[2];
static void (*cb_hook)(int chan, uint32_t func);
static int voice_trans_busy[2];
static int mvol_written;
uint64_t g_spu_samples;

void spu2_set_cb_hook(void (*h)(int chan, uint32_t func)) { cb_hook = h; }

/* ------------------------------------------------------------------ ADPCM */
static const int f0[5] = {0, 60, 115, 98, 122}, f1[5] = {0, 0, -52, -55, -60};

static void decode_block(Voice *v) {
    const uint8_t *b = spu_ram + (v->nax & (SPU_RAM_BYTES - 1) & ~15u);
    int shift = b[0] & 15, filter = (b[0] >> 4) & 7;
    if (filter > 4) filter = 4;
    if (shift > 12) shift = 9;
    v->block_flags = b[1];
    v->cur_block = v->nax & ~15u;
    if (b[1] & 4) { if (!v->lsa_set) v->lsa = v->cur_block; }
    for (int i = 0; i < 28; i++) {
        int nib = (b[2 + i / 2] >> ((i & 1) * 4)) & 15;
        int32_t s = (int16_t)(nib << 12) >> shift;
        s += (v->s1 * f0[filter] + v->s2 * f1[filter] + 32) >> 6;
        if (s > 32767) s = 32767; if (s < -32768) s = -32768;
        v->s2 = v->s1; v->s1 = s;
        v->buf[i] = (int16_t)s;
    }
    v->bufpos = 0;
}

/* ------------------------------------------------------------------ ADSR */
static void env_tick(Voice *v) {
    int up, expo, shift, step;
    switch (v->env_phase) {
    case ENV_ATTACK:  up = 1; expo = (v->adsr1 >> 15) & 1; shift = (v->adsr1 >> 10) & 31; step = 7 - ((v->adsr1 >> 8) & 3); break;
    /* decay and release store only the shift (their step is fixed at -8): the 4-bit decay
     * and 5-bit release fields ARE the shift, not shift/4 */
    case ENV_DECAY:   up = 0; expo = 1; shift = (v->adsr1 >> 4) & 15; step = -8; break;
    case ENV_SUSTAIN: up = !((v->adsr2 >> 14) & 1); expo = (v->adsr2 >> 15) & 1; shift = (v->adsr2 >> 8) & 31;
                      step = up ? 7 - ((v->adsr2 >> 6) & 3) : -8 + ((v->adsr2 >> 6) & 3); break;
    case ENV_RELEASE: up = 0; expo = (v->adsr2 >> 5) & 1; shift = v->adsr2 & 31; step = -8; break;
    default: return;
    }
    uint32_t cycles = 1u << (shift > 11 ? shift - 11 : 0);   /* shift <= 31, so at most 1 << 20 */
    int32_t st = step << (shift < 11 ? 11 - shift : 0);
    if (expo && up && v->env > 0x6000) cycles <<= 2;
    if (expo && !up) st = (int32_t)(((int64_t)st * v->env) >> 15);
    if (++v->env_counter < cycles) return;
    v->env_counter = 0;
    v->env += st;
    if (v->env > 0x7fff) v->env = 0x7fff;
    if (v->env < 0) v->env = 0;
    switch (v->env_phase) {
    case ENV_ATTACK: if (v->env >= 0x7fff) { v->env = 0x7fff; v->env_phase = ENV_DECAY; } break;
    case ENV_DECAY: { int32_t sl = ((v->adsr1 & 15) + 1) << 11; if (v->env <= sl) { v->env_phase = ENV_SUSTAIN; } break; }
    case ENV_RELEASE: if (v->env <= 0) { v->env = 0; v->env_phase = ENV_OFF; v->on = 0; } break;
    default: break;
    }
}

/* volume register -> current level (sweeps approximated linearly) */
/* voice and master volumes: bit 15 clear = fixed 15-bit volume (0x3fff = full) */
static inline int32_t fixed_vol(uint16_t reg) { return (reg & 0x8000) ? 0x7fff : (int16_t)(reg << 1); }
static int32_t vol_step(uint16_t reg, int32_t cur) {
    if (!(reg & 0x8000)) return fixed_vol(reg);
    int dec = (reg >> 13) & 1, rate = reg & 0x7f;
    int32_t target = dec ? 0 : 0x7fff;
    int32_t st = 1 + ((0x7f - rate) >> 2);
    if (cur < target) { cur += st; if (cur > target) cur = target; }
    else if (cur > target) { cur -= st; if (cur < target) cur = target; }
    return cur;
}

/* ------------------------------------------------------------------ voices */
static void key_on(Core *c, int vi) {
    Voice *v = &c->v[vi];
    v->nax = v->ssa;
    if (!v->lsa_set) v->lsa = v->ssa;
    v->lsa_set = 0;
    v->s1 = v->s2 = 0;
    memset(v->hist, 0, sizeof v->hist);
    v->counter = 0;
    v->env = 0; v->env_counter = 0; v->env_phase = ENV_ATTACK;
    v->on = 1;
    c->endx &= ~(1u << vi);
    decode_block(v);
}

static inline int16_t voice_sample(Core *c, int vi) {
    Voice *v = &c->v[vi];
    if (!v->on) return 0;
    /* 4-point (Catmull-Rom) interpolation between hist[1] and hist[2] */
    int32_t t = (int32_t)(v->counter & 0xfff);
    int32_t p0 = v->hist[0], p1 = v->hist[1], p2 = v->hist[2], p3 = v->hist[3];
    int64_t a = -p0 + 3 * p1 - 3 * p2 + p3, b = 2 * p0 - 5 * p1 + 4 * p2 - p3, cc = -p0 + p2, d = 2 * p1;
    int64_t s = (((((a * t) >> 12) + b) * t >> 12) + cc) * t >> 12;
    int32_t out = (int32_t)((s + d) / 2);
    if (out > 32767) out = 32767; if (out < -32768) out = -32768;
    uint32_t pitch = v->pitch > 0x3fff ? 0x3fff : v->pitch;
    v->counter += pitch;
    while (v->counter >= 0x1000) {
        v->counter -= 0x1000;
        v->hist[0] = v->hist[1]; v->hist[1] = v->hist[2]; v->hist[2] = v->hist[3];
        v->hist[3] = v->buf[v->bufpos++];
        if (v->bufpos >= 28) {
            /* end of block: handle loop flags, then fetch the next one */
            uint8_t fl = v->block_flags;
            if (fl & 1) {
                c->endx |= 1u << vi;
                if (fl & 2) v->nax = v->lsa;
                else { v->on = 0; v->env = 0; v->env_phase = ENV_OFF; v->nax = v->cur_block + 16; break; }
            } else v->nax = v->cur_block + 16;
            decode_block(v);
        }
    }
    env_tick(v);
    return (int16_t)((out * v->env) >> 15);
}

/* ------------------------------------------------------------------ mixer */
static uint32_t adma_pending;   /* bit per core: half-buffer boundary crossed */

/* ------------------------------------------------------------------ reverb
 * The PS1/PS2 effect unit.  It runs at half the output rate, one channel per
 * output sample (left on even samples, right on odd ones), through a 39-tap
 * half-band filter each way, with all of its delay lines in the core's work
 * area of sound memory (ESA..EEA), which moves on by one word every two samples.
 * Registers in the order of libsd's presets (and the PS1's SPU): */
enum { R_dAPF1, R_dAPF2, R_vIIR, R_vCOMB1, R_vCOMB2, R_vCOMB3, R_vCOMB4, R_vWALL, R_vAPF1, R_vAPF2,
       R_mLSAME, R_mRSAME, R_mLCOMB1, R_mRCOMB1, R_mLCOMB2, R_mRCOMB2, R_dLSAME, R_dRSAME,
       R_mLDIFF, R_mRDIFF, R_mLCOMB3, R_mRCOMB3, R_mLCOMB4, R_mRCOMB4, R_dLDIFF, R_dRDIFF,
       R_mLAPF1, R_mRAPF1, R_mLAPF2, R_mRAPF2, R_vLIN, R_vRIN };
/* libsd's presets (OFF, ROOM, STUDIO_A/B/C, HALL, SPACE, ECHO, DELAY, PIPE): work area
 * size in 16-byte units, then the 32 registers (addresses in 8-byte units) */
static const uint16_t rv_size[10] = { 0x1, 0x26C, 0x1F4, 0x484, 0x6FE, 0xADE, 0xF6C, 0x1804, 0x1804, 0x3C0 };
static const uint16_t rv_preset[10][32] = {
    { 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0001, 0x0001, 0x0000, 0x0000, 0x0000, 0x0000,
      0x0000, 0x0000, 0x0001, 0x0001, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000 },
    { 0x007D, 0x005B, 0x6D80, 0x54B8, 0xBED0, 0x0000, 0x0000, 0xBA80, 0x5800, 0x5300, 0x04D6, 0x0333, 0x03F0, 0x0227, 0x0374, 0x01EF,
      0x0336, 0x01B7, 0x0335, 0x01B6, 0x0334, 0x01B5, 0x0334, 0x01B5, 0x0334, 0x01B5, 0x01B4, 0x0136, 0x00B8, 0x005C, 0x8000, 0x8000 },
    { 0x0033, 0x0025, 0x70F0, 0x4FA8, 0xBCE0, 0x4410, 0xC0F0, 0x9C00, 0x5280, 0x4EC0, 0x03E4, 0x031B, 0x03A4, 0x02AF, 0x0372, 0x0266,
      0x031C, 0x025D, 0x025C, 0x018E, 0x022F, 0x0135, 0x01D2, 0x00B7, 0x018F, 0x00B5, 0x00B4, 0x0080, 0x004C, 0x0026, 0x8000, 0x8000 },
    { 0x00B1, 0x007F, 0x70F0, 0x4FA8, 0xBCE0, 0x4510, 0xBEF0, 0xB4C0, 0x5280, 0x4EC0, 0x0904, 0x076B, 0x0824, 0x065F, 0x07A2, 0x0616,
      0x076C, 0x05ED, 0x05EC, 0x042E, 0x050F, 0x0305, 0x0462, 0x02B7, 0x042F, 0x0265, 0x0264, 0x01B2, 0x0100, 0x0080, 0x8000, 0x8000 },
    { 0x00E3, 0x00A9, 0x6F60, 0x4FA8, 0xBCE0, 0x4510, 0xBEF0, 0xA680, 0x5680, 0x52C0, 0x0DFB, 0x0B58, 0x0D09, 0x0A3C, 0x0BD9, 0x0973,
      0x0B59, 0x08DA, 0x08D9, 0x05E9, 0x07EC, 0x04B0, 0x06EF, 0x03D2, 0x05EA, 0x031D, 0x031C, 0x0238, 0x0154, 0x00AA, 0x8000, 0x8000 },
    { 0x01A5, 0x0139, 0x6000, 0x5000, 0x4C00, 0xB800, 0xBC00, 0xC000, 0x6000, 0x5C00, 0x15BA, 0x11BB, 0x14C2, 0x10BD, 0x11BC, 0x0DC1,
      0x11C0, 0x0DC3, 0x0DC0, 0x09C1, 0x0BC4, 0x07C1, 0x0A00, 0x06CD, 0x09C2, 0x05C1, 0x05C0, 0x041A, 0x0274, 0x013A, 0x8000, 0x8000 },
    { 0x033D, 0x0231, 0x7E00, 0x5000, 0xB400, 0xB000, 0x4C00, 0xB000, 0x6000, 0x5400, 0x1ED6, 0x1A31, 0x1D14, 0x183B, 0x1BC2, 0x16B2,
      0x1A32, 0x15EF, 0x15EE, 0x1055, 0x1334, 0x0F2D, 0x11F6, 0x0C5D, 0x1056, 0x0AE1, 0x0AE0, 0x07A2, 0x0464, 0x0232, 0x8000, 0x8000 },
    { 0x0003, 0x0003, 0x7FFF, 0x7FFF, 0x0000, 0x0000, 0x0000, 0x8100, 0x0000, 0x0000, 0x1FFD, 0x0FFD, 0x1009, 0x0009, 0x0000, 0x0000,
      0x1009, 0x0009, 0x1FFF, 0x1FFF, 0x1FFE, 0x1FFE, 0x1FFE, 0x1FFE, 0x1FFE, 0x1FFE, 0x1008, 0x1004, 0x0008, 0x0004, 0x8000, 0x8000 },
    { 0x0003, 0x0003, 0x7FFF, 0x7FFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x1FFD, 0x0FFD, 0x1009, 0x0009, 0x0000, 0x0000,
      0x1009, 0x0009, 0x1FFF, 0x1FFF, 0x1FFE, 0x1FFE, 0x1FFE, 0x1FFE, 0x1FFE, 0x1FFE, 0x1008, 0x1004, 0x0008, 0x0004, 0x8000, 0x8000 },
    { 0x0017, 0x0013, 0x70F0, 0x4FA8, 0xBCE0, 0x4510, 0xBEF0, 0x8500, 0x5F80, 0x54C0, 0x0371, 0x02AF, 0x02E5, 0x01DF, 0x02B0, 0x01D7,
      0x0358, 0x026A, 0x01D6, 0x011E, 0x012D, 0x00B1, 0x011F, 0x0059, 0x01A0, 0x00E3, 0x0058, 0x0040, 0x0028, 0x0014, 0x8000, 0x8000 },
};
/* the half-band filter (symmetric, 39 taps; the odd taps are zero but the centre's) */
static const int16_t rv_fir[20] = { -0x0001, 0, 0x0002, 0, -0x000A, 0, 0x0023, 0, -0x0067, 0, 0x010A, 0, -0x0268, 0, 0x0534, 0, -0x0B90, 0, 0x2806, 0x4000 };
static inline int32_t clamp16(int32_t v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : v; }
static inline int32_t rv_fir_at(const int32_t *h, uint32_t pos) {   /* h: 64-entry ring, newest at pos */
    int64_t a = (int64_t)rv_fir[19] * h[(pos - 19) & 63];
    for (int k = 0; k < 19; k++) a += (int64_t)rv_fir[k] * (h[(pos - k) & 63] + h[(pos - 38 + k) & 63]);
    return (int32_t)(a >> 15);
}
static void sd_set_effect(Core *c, int mode, int16_t depth_l, int16_t depth_r, int delay, int feedback) {
    /* sceSdSetEffectAttr: the preset's registers, the work area ending at EEA, depth as the effect volume */
    uint16_t d[32]; memcpy(d, rv_preset[mode], sizeof d);
    if (mode == 7 || mode == 8) {   /* echo and delay: the delay time and feedback are the caller's */
        uint16_t dl = (uint16_t)((delay + 1) << 5), fb = (uint16_t)((((feedback << 7) + feedback) << 1));
        d[R_vWALL] = fb;
        d[R_mLSAME] = (uint16_t)((dl << 1) - d[R_dAPF1]); d[R_mRSAME] = (uint16_t)(dl - d[R_dAPF2]);
        d[R_dLSAME] = (uint16_t)(d[R_dRSAME] + dl); d[R_mLCOMB1] = (uint16_t)(d[R_mRCOMB1] + dl);
        d[R_mRAPF1] = (uint16_t)(d[R_mRAPF2] + dl); d[R_mLAPF1] = (uint16_t)(d[R_mLAPF2] + dl);
    }
    for (int i = 0; i < 32; i++) c->rv[i] = (int16_t)d[i];
    uint32_t eea_bytes = ((c->eea >> 17) << 17) | 0x1ffff;
    uint32_t size = ((uint32_t)rv_size[mode] << 4) - 2;
    c->rv_end = eea_bytes >> 1;
    c->rv_start = eea_bytes > size ? (eea_bytes - size) >> 1 : c->rv_end;   /* no room below EEA: off */
    c->rv_pos = 0;
    c->esa = c->rv_start << 1;
    c->rv_mode = mode;
    c->evoll = (uint16_t)depth_l; c->evolr = (uint16_t)depth_r;
}
static inline uint32_t rv_addr(const Core *c, uint32_t span, int32_t off) {
    int64_t x = ((int64_t)c->rv_pos + off) % (int64_t)span; if (x < 0) x += span;
    return (c->rv_start + (uint32_t)x) & (SPU_RAM_BYTES / 2 - 1);
}
/* one reverb step for channel R (0 left, 1 right) on the wet input `in`; returns its output */
static int32_t rv_step(Core *c, int R, int32_t in) {
    if (c->rv_end <= c->rv_start || c->rv_mode == 0) return 0;
    uint32_t span = c->rv_end - c->rv_start + 1;
    int16_t *m = (int16_t *)spu_ram;
#define RA(off) rv_addr(c, span, (off))
#define REG(n) ((int32_t)(uint16_t)c->rv[n] * 4)   /* address registers: 8-byte units -> words */
#define MUL(a, b) (((int32_t)(a) * (int32_t)(b)) >> 15)
    int32_t same_dst = REG(R ? R_mRSAME : R_mLSAME), same_src = REG(R ? R_dRSAME : R_dLSAME);
    int32_t diff_dst = REG(R ? R_mRDIFF : R_mLDIFF), diff_src = REG(R ? R_dLDIFF : R_dRDIFF);
    int32_t apf1_dst = REG(R ? R_mRAPF1 : R_mLAPF1), apf2_dst = REG(R ? R_mRAPF2 : R_mLAPF2);
    int32_t apf1_src = apf1_dst - REG(R_dAPF1), apf2_src = apf2_dst - REG(R_dAPF2);
    int32_t x = MUL(c->rv[R ? R_vRIN : R_vLIN], in);
    int32_t sp = m[RA(same_dst - 1)], dp = m[RA(diff_dst - 1)];
    int32_t same = MUL(c->rv[R_vIIR], x + MUL(c->rv[R_vWALL], m[RA(same_src)]) - sp) + sp;
    int32_t diff = MUL(c->rv[R_vIIR], x + MUL(c->rv[R_vWALL], m[RA(diff_src)]) - dp) + dp;
    int32_t out = MUL(c->rv[R_vCOMB1], m[RA(REG(R ? R_mRCOMB1 : R_mLCOMB1))]) + MUL(c->rv[R_vCOMB2], m[RA(REG(R ? R_mRCOMB2 : R_mLCOMB2))])
                + MUL(c->rv[R_vCOMB3], m[RA(REG(R ? R_mRCOMB3 : R_mLCOMB3))]) + MUL(c->rv[R_vCOMB4], m[RA(REG(R ? R_mRCOMB4 : R_mLCOMB4))]);
    int32_t a1s = m[RA(apf1_src)], a1 = out - MUL(c->rv[R_vAPF1], a1s);
    out = a1s + MUL(c->rv[R_vAPF1], a1);
    int32_t a2s = m[RA(apf2_src)], a2 = out - MUL(c->rv[R_vAPF2], a2s);
    out = a2s + MUL(c->rv[R_vAPF2], a2);
    if (c->fx_on) {
        m[RA(same_dst)] = (int16_t)clamp16(same); m[RA(diff_dst)] = (int16_t)clamp16(diff);
        m[RA(apf1_dst)] = (int16_t)clamp16(a1); m[RA(apf2_dst)] = (int16_t)clamp16(a2);
    }
#undef RA
#undef REG
#undef MUL
    return clamp16(out);
}
/* the core's effect output for this 48 kHz sample, from its wet input */
static void reverb(Core *c, int32_t wl, int32_t wr, int32_t *ol, int32_t *or_) {
    uint32_t p = ++c->rv_hp & 63;
    c->rv_in[0][p] = clamp16(wl); c->rv_in[1][p] = clamp16(wr);
    int R = c->rv_phase & 1;
    int32_t y = rv_step(c, R, rv_fir_at(c->rv_in[R], p));
    c->rv_out[R][p] = y; c->rv_out[!R][p] = 0;
    if (!(++c->rv_phase & 1) && ++c->rv_pos > c->rv_end - c->rv_start) c->rv_pos = 0;   /* one word on every two samples */
    *ol = clamp16(rv_fir_at(c->rv_out[0], p) * 2);
    *or_ = clamp16(rv_fir_at(c->rv_out[1], p) * 2);
}
static int rv_disabled = -1;

static void mix_one(int32_t *l, int32_t *r) {
    int32_t ol = 0, orr = 0;
    if (rv_disabled < 0) rv_disabled = getenv("AE2_REVERB") && !strcmp(getenv("AE2_REVERB"), "0");
    int32_t ext_l = 0, ext_r = 0;   /* core 0's output, core 1's external input */
    for (int ci = 0; ci < 2; ci++) {
        Core *c = &core[ci];
        int32_t dl = 0, dr = 0, wl = 0, wr = 0;
        for (int vi = 0; vi < 24; vi++) {
            Voice *v = &c->v[vi];
            if (!v->on) continue;
            int32_t s = voice_sample(c, vi);
            v->volxl = vol_step(v->voll, v->volxl); v->volxr = vol_step(v->volr, v->volxr);
            int32_t sl = (s * v->volxl) >> 15, sr = (s * v->volxr) >> 15;
            if (c->vmixl & (1u << vi)) dl += sl;
            if (c->vmixr & (1u << vi)) dr += sr;
            if (c->vmixel & (1u << vi)) wl += sl;
            if (c->vmixer & (1u << vi)) wr += sr;
        }
        /* MMIX gates for the effect input: voices (0x200/0x100), sound data input (0x20/0x10), core 0 into core 1 (2/1) */
        if (!(c->mmix & 0x200)) wl = 0;
        if (!(c->mmix & 0x100)) wr = 0;
        if (ci == 1) { if (c->mmix & 2) wl += ext_l; if (c->mmix & 1) wr += ext_r; }
        if (c->adma_on && c->adma_size >= 0x400) {
            uint32_t p = c->adma_pos, blk = p >> 8, k = p & 255;
            uint32_t a = c->adma_base + blk * 0x400 + k * 2;
            int16_t sl, sr; memcpy(&sl, IOP(a), 2); memcpy(&sr, IOP(a + 0x200), 2);
            int32_t il = (sl * (int16_t)c->bvoll) >> 15, ir = (sr * (int16_t)c->bvolr) >> 15;   /* input volumes are plain signed 16-bit */
            dl += il; dr += ir;
            if (c->mmix & 0x20) wl += il;
            if (c->mmix & 0x10) wr += ir;
            uint32_t total = c->adma_size / 4;   /* samples in the whole buffer */
            uint32_t half = total / 2;
            p++;
            if (p == half || p >= total) {
                if (p >= total) { if (c->adma_loop) p = 0; else { c->adma_on = 0; } }
                c->adma_half = p >= half;
                adma_pending |= 1u << ci;
            }
            c->adma_pos = p;
        }
        if (c->fx_on && !rv_disabled) {
            int32_t el, er; reverb(c, wl, wr, &el, &er);
            dl += (el * (int16_t)c->evoll) >> 15; dr += (er * (int16_t)c->evolr) >> 15;
        }
        int32_t mvl = mvol_written ? fixed_vol(c->mvoll) : 0x7fff, mvr = mvol_written ? fixed_vol(c->mvolr) : 0x7fff;
        if (ci == 0) { ext_l = (dl * mvl) >> 15; ext_r = (dr * mvr) >> 15; }
        ol += (dl * mvl) >> 15;
        orr += (dr * mvr) >> 15;
    }
    *l = ol; *r = orr;
}

void spu2_render(int16_t *out, int frames) {
    { static int st = -1; static uint64_t total; if (st < 0) st = getenv("AE2_SPUSTAT") != NULL;   /* active voices once a second */
      if (st && (total += frames) >= 48000) { total = 0; int n = 0, rel = 0;
          for (int k = 0; k < 2; k++) for (int i = 0; i < 24; i++) { n += core[k].v[i].on; rel += core[k].v[i].on && core[k].v[i].env_phase == ENV_RELEASE; }
          extern uint32_t g_pad_polls; fprintf(stderr, "[spustat] poll %u: %d voices on (%d releasing)\n", g_pad_polls, n, rel); } }
    while (frames > 0) {
        int n = frames > 64 ? 64 : frames;
        pthread_mutex_lock(&spu_mtx);
        int i;
        for (i = 0; i < n; i++) {
            int32_t l, r;
            mix_one(&l, &r);
            if (l > 32767) l = 32767; if (l < -32768) l = -32768;
            if (r > 32767) r = 32767; if (r < -32768) r = -32768;
            if (out) { out[i * 2] = (int16_t)l; out[i * 2 + 1] = (int16_t)r; }
            if (adma_pending) { i++; break; }
        }
        g_spu_samples += i;
        uint32_t pend = adma_pending; adma_pending = 0;
        uint32_t cb[2] = {trans_cb[0], trans_cb[1]};
        pthread_mutex_unlock(&spu_mtx);
        for (int ci = 0; ci < 2; ci++) if ((pend >> ci) & 1 && cb[ci] && cb_hook) cb_hook(ci, cb[ci]);
        if (out) out += i * 2;
        frames -= i;
    }
}

/* ------------------------------------------------------------------ libsd API */
void sd_init(int flag) {
    (void)flag;
    pthread_mutex_lock(&spu_mtx);
    for (int ci = 0; ci < 2; ci++) {
        Core *c = &core[ci];
        memset(c, 0, sizeof *c);
        /* libsd's init routes every voice to the dry and the effect mix */
        c->vmixl = c->vmixr = c->vmixel = c->vmixer = 0xffffff;
        for (int i = 0; i < 24; i++) c->v[i].pitch = 0x3fff;
    }
    pthread_mutex_unlock(&spu_mtx);
}

void sd_set_param(uint32_t entry, uint32_t value) {
    int ci = entry & 1, vi = (entry >> 1) & 31, p = (entry >> 8) & 0xff;
    pthread_mutex_lock(&spu_mtx);
    Core *c = &core[ci];
    if (vi < 24) {
        Voice *v = &c->v[vi];
        switch (p) {
        case 0x00: v->voll = value; if (!(value & 0x8000)) v->volxl = fixed_vol(value); break;
        case 0x01: v->volr = value; if (!(value & 0x8000)) v->volxr = fixed_vol(value); break;
        case 0x02: v->pitch = value; break;
        case 0x03: v->adsr1 = value; break;
        case 0x04: v->adsr2 = value; break;
        case 0x05: v->env = value & 0x7fff; break;
        default: break;
        }
    }
    switch (p) {
    case 0x08: c->mmix = value; if (getenv("AE2_SPULOG")) INFO("[spu] core %d mmix %03x\n", ci, value); break;
    case 0x09: c->mvoll = value; mvol_written = 1; break;
    case 0x0A: c->mvolr = value; mvol_written = 1; break;
    case 0x0B: c->evoll = value; if (getenv("AE2_SPULOG")) INFO("[spu] core %d effect volume %04x\n", ci, value); break;
    case 0x0C: c->evolr = value; break;
    case 0x0D: c->avoll = value; break;
    case 0x0E: c->avolr = value; break;
    case 0x0F: c->bvoll = value; break;
    case 0x10: c->bvolr = value; break;
    default: break;
    }
    pthread_mutex_unlock(&spu_mtx);
}

uint32_t sd_get_param(uint32_t entry) {
    int ci = entry & 1, vi = (entry >> 1) & 31, p = (entry >> 8) & 0xff;
    uint32_t r = 0;
    pthread_mutex_lock(&spu_mtx);
    Core *c = &core[ci];
    if (vi < 24) {
        Voice *v = &c->v[vi];
        switch (p) {
        case 0x00: r = v->voll; break; case 0x01: r = v->volr; break; case 0x02: r = v->pitch; break;
        case 0x03: r = v->adsr1; break; case 0x04: r = v->adsr2; break;
        case 0x05: r = v->on ? (uint32_t)v->env : 0; break;
        case 0x06: r = (uint32_t)v->volxl & 0xffff; break; case 0x07: r = (uint32_t)v->volxr & 0xffff; break;
        }
    }
    switch (p) {
    case 0x08: r = c->mmix; break; case 0x09: r = c->mvoll; break; case 0x0A: r = c->mvolr; break;
    case 0x0F: r = c->bvoll; break; case 0x10: r = c->bvolr; break;
    }
    pthread_mutex_unlock(&spu_mtx);
    return r;
}

void sd_set_switch(uint32_t entry, uint32_t value) {
    int ci = entry & 1, p = (entry >> 8) & 0xff;
    value &= 0xffffff;
    pthread_mutex_lock(&spu_mtx);
    Core *c = &core[ci];
    switch (p) {
    case 0x13: c->pmon = value; break;
    case 0x14: c->non = value; break;
    case 0x15:
        for (int i = 0; i < 24; i++) if ((value >> i) & 1) {
            key_on(c, i);
            static int nl, tr = -1; if (tr < 0) tr = getenv("AE2_SPUTRACE") != NULL;
            if (tr && nl < 40) { nl++; Voice *v = &c->v[i];
                fprintf(stderr, "[spu2] KON core %d voice %d ssa %05x pitch %04x vol %04x/%04x adsr %04x/%04x vmix %d%d\n", ci, i, v->ssa, v->pitch, v->voll, v->volr, v->adsr1, v->adsr2, (c->vmixl >> i) & 1, (c->vmixr >> i) & 1); }
        }
        break;
    case 0x16: for (int i = 0; i < 24; i++) if ((value >> i) & 1 && c->v[i].on && c->v[i].env_phase != ENV_OFF) { c->v[i].env_phase = ENV_RELEASE; c->v[i].env_counter = 0; } break;
    case 0x17: c->endx = value; break;
    case 0x18: c->vmixl = value; break;
    case 0x19: c->vmixel = value; break;
    case 0x1A: c->vmixr = value; break;
    case 0x1B: c->vmixer = value; break;
    }
    pthread_mutex_unlock(&spu_mtx);
}

uint32_t sd_get_switch(uint32_t entry) {
    int ci = entry & 1, p = (entry >> 8) & 0xff;
    Core *c = &core[ci];
    switch (p) {
    case 0x13: return c->pmon; case 0x14: return c->non; case 0x17: return c->endx;
    case 0x18: return c->vmixl; case 0x19: return c->vmixel; case 0x1A: return c->vmixr; case 0x1B: return c->vmixer;
    }
    return 0;
}

void sd_set_addr(uint32_t entry, uint32_t value) {
    int ci = entry & 1, vi = (entry >> 1) & 31, p = (entry >> 8) & 0xff;
    pthread_mutex_lock(&spu_mtx);
    Core *c = &core[ci];
    switch (p) {
    case 0x1C: c->esa = value; if (getenv("AE2_SPULOG")) INFO("[spu] core %d effect area start %06x\n", ci, value); break;
    case 0x1D: c->eea = value; if (getenv("AE2_SPULOG")) INFO("[spu] core %d effect area end %06x\n", ci, value); break;
    case 0x1E: c->tsa = value; break;
    case 0x1F: c->irqa = value; break;
    case 0x20: if (vi < 24) c->v[vi].ssa = value & ~15u; { static int lg = -1; if (lg < 0) lg = getenv("AE2_SPULOG") != NULL; if (lg) INFO("[spu] core %d voice %d start %06x\n", ci, vi, value); } break;
    case 0x21: if (vi < 24) { c->v[vi].lsa = value & ~15u; c->v[vi].lsa_set = 1; } break;
    case 0x22: if (vi < 24) c->v[vi].nax = value; break;
    }
    pthread_mutex_unlock(&spu_mtx);
}

uint32_t sd_get_addr(uint32_t entry) {
    int ci = entry & 1, vi = (entry >> 1) & 31, p = (entry >> 8) & 0xff;
    uint32_t r = 0;
    pthread_mutex_lock(&spu_mtx);
    Core *c = &core[ci];
    switch (p) {
    case 0x1C: r = c->esa; break; case 0x1D: r = c->eea; break; case 0x1E: r = c->tsa; break; case 0x1F: r = c->irqa; break;
    case 0x20: if (vi < 24) r = c->v[vi].ssa; break;
    case 0x21: if (vi < 24) r = c->v[vi].lsa; break;
    case 0x22: if (vi < 24) r = c->v[vi].nax; break;
    }
    pthread_mutex_unlock(&spu_mtx);
    return r;
}

void sd_set_core_attr(uint32_t entry, uint32_t value) {
    pthread_mutex_lock(&spu_mtx);
    Core *c = &core[entry & 1];
    c->attr = (uint16_t)value;
    if ((entry & ~1u) == 2) {   /* SD_CORE_EFFECT_ENABLE */
        c->fx_on = value != 0;
        if (getenv("AE2_SPULOG")) INFO("[spu] core %d effects %s\n", (int)(entry & 1), value ? "on" : "off");
    }
    pthread_mutex_unlock(&spu_mtx);
}

int sd_set_effect_attr(int ci, int mode, int16_t depth_l, int16_t depth_r, int delay, int feedback) {
    mode &= ~0x100;   /* SD_EFFECT_MODE_CLEAR: the work area is cleared below anyway */
    if (mode < 0 || mode > 9) return -1;
    pthread_mutex_lock(&spu_mtx);
    Core *c = &core[ci & 1];
    sd_set_effect(c, mode, depth_l, depth_r, delay, feedback);
    /* a new mode starts from a silent work area (the old one's echoes would be noise in it) */
    if (mode && c->rv_start < c->rv_end && c->rv_end < SPU_RAM_BYTES / 2) memset(spu_ram + c->rv_start * 2, 0, (c->rv_end - c->rv_start + 1) * 2);
    memset(c->rv_in, 0, sizeof c->rv_in); memset(c->rv_out, 0, sizeof c->rv_out);
    { static int lg = -1; if (lg < 0) lg = getenv("AE2_SPULOG") != NULL;
      if (lg) INFO("[spu] core %d reverb mode %d depth %d/%d delay %d feedback %d, area %06x-%06x\n", ci & 1, mode, depth_l, depth_r, delay, feedback, c->rv_start * 2, c->rv_end * 2 + 1); }
    pthread_mutex_unlock(&spu_mtx);
    return 0;
}

int sd_voice_trans(int chan, uint32_t mode, uint32_t iopaddr, uint32_t spuaddr, uint32_t size) {
    chan &= 1;
    pthread_mutex_lock(&spu_mtx);
    spuaddr &= SPU_RAM_BYTES - 1;
    if (spuaddr + size > SPU_RAM_BYTES) size = SPU_RAM_BYTES - spuaddr;
    { static int lg = -1; if (lg < 0) lg = getenv("AE2_SPULOG") != NULL; if (lg) INFO("[spu] %s %06x-%06x (%x)\n", mode & 1 ? "read" : "write", spuaddr, spuaddr + size, size); }
    if (mode & 1) memcpy(IOP(iopaddr), spu_ram + spuaddr, size);     /* read */
    else memcpy(spu_ram + spuaddr, IOP(iopaddr), size);
    uint32_t cb = trans_cb[chan];
    pthread_mutex_unlock(&spu_mtx);
    voice_trans_busy[chan] = 0;
    if (cb && cb_hook) cb_hook(chan, cb);
    return (int)size;
}

int sd_voice_trans_status(int chan, int flag) { (void)flag; return voice_trans_busy[chan & 1] ? 0 : 1; }

uint32_t sd_block_trans(int chan, uint32_t mode, uint32_t iopaddr, uint32_t size, uint32_t start) {
    chan &= 1;
    pthread_mutex_lock(&spu_mtx);
    Core *c = &core[chan];
    uint32_t m = mode & 0xf;
    if (m == 2) {            /* stop */
        uint32_t pos = c->adma_base + c->adma_pos * 2;
        c->adma_on = 0;
        pthread_mutex_unlock(&spu_mtx);
        return pos;
    }
    c->adma_base = iopaddr; c->adma_size = size; c->adma_loop = (mode & 0x10) != 0;
    c->adma_pos = (m == 3 && start >= iopaddr) ? ((start - iopaddr) / 4) : 0;
    c->adma_half = 0;
    c->adma_on = 1;
    pthread_mutex_unlock(&spu_mtx);
    return 0;
}

uint32_t sd_block_trans_status(int chan, int flag) {
    (void)flag;
    Core *c = &core[chan & 1];
    uint32_t half = c->adma_half;
    return (half << 24) | ((c->adma_base + half * (c->adma_size / 2)) & 0xffffff);
}

uint32_t sd_set_trans_callback(int chan, uint32_t func) { uint32_t o = trans_cb[chan & 1]; trans_cb[chan & 1] = func; return o; }

/* ------------------------------------------------------------------ output */
static FILE *wav; static uint32_t wav_bytes;
static void wav_write(const int16_t *s, int frames) {
    if (!wav) return;
    fwrite(s, 4, frames, wav); wav_bytes += frames * 4;
    if ((wav_bytes & 0xffff) < 4096) {   /* keep the header roughly up to date */
        long pos = ftell(wav);
        uint32_t riff = 36 + wav_bytes; fseek(wav, 4, SEEK_SET); fwrite(&riff, 4, 1, wav);
        fseek(wav, 40, SEEK_SET); fwrite(&wav_bytes, 4, 1, wav); fseek(wav, pos, SEEK_SET); fflush(wav);
    }
}
static void wav_open(const char *path) {
    wav = fopen(path, "wb"); if (!wav) return;
    uint8_t h[44] = {'R','I','F','F',0,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,1,0,2,0,0x80,0xbb,0,0,0,0xee,2,0,4,0,16,0,'d','a','t','a',0,0,0,0};
    fwrite(h, 1, 44, wav);
}

/* In deterministic mode the SPU is mixed on the EE thread (spu2_pump, driven by the
 * virtual clock) so that sound-driver state never depends on host timing; the audio
 * device then plays from this ring buffer instead of mixing itself. */
#define RING_FRAMES 16384
static int16_t ring[RING_FRAMES * 2];
static volatile uint32_t ring_w, ring_r;
static int det_audio;
#ifdef HAVE_SDL
static void sdl_audio_cb(void *ud, Uint8 *stream, int len) {
    (void)ud;
    int frames = len / 4;
    int16_t *out = (int16_t *)stream;
    if (det_audio) {
        uint32_t w = __atomic_load_n(&ring_w, __ATOMIC_ACQUIRE), r = ring_r;
        int i = 0;
        for (; i < frames && r != w; i++, r++) { out[i * 2] = ring[(r % RING_FRAMES) * 2]; out[i * 2 + 1] = ring[(r % RING_FRAMES) * 2 + 1]; }
        for (; i < frames; i++) out[i * 2] = out[i * 2 + 1] = 0;
        __atomic_store_n(&ring_r, r, __ATOMIC_RELEASE);
        return;
    }
    spu2_render(out, frames);
    wav_write(out, frames);
}
#endif

/* deterministic mode: mix up to the (virtual) current time, on the caller's thread */
void spu2_pump(void) {
    static uint64_t t0, done; static int16_t buf[256 * 2];
    uint64_t now = host_usec();
    if (!t0) t0 = now;
    uint64_t due = (now - t0) * 48000 / 1000000;
    while (done < due) {
        int n = (int)(due - done > 256 ? 256 : due - done);
        spu2_render(buf, n); wav_write(buf, n); done += n;
        if (det_audio) {
            uint32_t w = ring_w;
            for (int i = 0; i < n; i++, w++) { ring[(w % RING_FRAMES) * 2] = buf[i * 2]; ring[(w % RING_FRAMES) * 2 + 1] = buf[i * 2 + 1]; }
            __atomic_store_n(&ring_w, w, __ATOMIC_RELEASE);
            /* keep the latency bounded (~170 ms) if the device falls behind */
            uint32_t r = __atomic_load_n(&ring_r, __ATOMIC_ACQUIRE);
            if (w - r > 8192) __atomic_compare_exchange_n(&ring_r, &r, w - 4096, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
        }
    }
}
static void *paced_thread(void *arg) {
    (void)arg;
    int16_t buf[256 * 2];
    uint64_t t0 = host_usec(), done = 0;
    for (;;) {
        uint64_t due = (host_usec() - t0) * 48000 / 1000000;
        if (done >= due) { usleep(2000); continue; }
        int n = (int)(due - done > 256 ? 256 : due - done);
        spu2_render(buf, n);
        wav_write(buf, n);
        done += n;
        if (due - done > 48000) done = due;   /* fell far behind: skip ahead */
    }
    return NULL;
}

void spu2_init(int headless) {
    spu_ram = calloc(1, SPU_RAM_BYTES);
    const char *w = getenv("AE2_WAVOUT");
    if (w) wav_open(w);
#ifdef HAVE_SDL
    if (!headless && !getenv("AE2_NOAUDIO")) {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) == 0) {
            SDL_AudioSpec want = {0}, have;
            want.freq = 48000; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 512;
            want.callback = sdl_audio_cb;
            det_audio = rt_deterministic();
            SDL_AudioDeviceID dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
            if (dev) { SDL_PauseAudioDevice(dev, 0); INFO("[spu2] audio output: %d Hz, %d-sample buffer\n", have.freq, have.samples); return; }
            WARN("[spu2] could not open an audio device (%s); running silent\n", SDL_GetError());
        } else WARN("[spu2] SDL audio init failed (%s); running silent\n", SDL_GetError());
    }
#endif
    (void)headless;
    if (rt_deterministic()) { INFO("[spu2] deterministic mode: mixing on the EE thread\n"); return; }
    pthread_t th; pthread_create(&th, NULL, paced_thread, NULL); pthread_detach(th);
    INFO("[spu2] no audio device: mixing silently in real time\n");
}
