/* VU state and shared helpers for the interpreter (vu.c) and the
 * recompiled VU1 microprograms (gen/vu1_progs.c). */
#ifndef VU_IMPL_H
#define VU_IMPL_H
#include "rt.h"
#include <math.h>
typedef struct {
    r128 vf[32];
    uint32_t vi[16];
    float acc[4];
    float q, p, i;
    uint32_t r;
    uint8_t *mem; uint32_t memmask;   /* data memory (bytes) */
    uint8_t *code; uint32_t codemask;
    int unit;
    uint32_t pc;
    uint64_t cycle;
    uint64_t vf_ready[32];
    float q_new; uint64_t q_done;
    float p_new; uint64_t p_done;
    /* flags: latest written and visible history */
    struct { uint64_t at; uint32_t mac, status, clip; } fh[8];
    int fhead;
    uint32_t clip_latest, status_sticky;
    int running;
    uint32_t tpc;
} VU;

extern uint32_t g_vif1_top, g_vif1_itop;
void gif_path1_kick(const uint8_t *mem, uint32_t addr);

static inline float vclampf(float x) { return fclamp(x); }
/* vclampf on four lanes at once (SSE2), for the recompiled programs' full-width operations */
#include <emmintrin.h>
static inline __m128 vclamp4(__m128 x) {
    /* Inf/NaN (min returns its second operand for NaN) -> signed FLT_MAX. Denormals need
     * nothing: guest threads run with flush-to-zero and denormals-are-zero set
     * (guest_fpu_mode in rt_kernel.c), so an FMAC result is never denormal */
    __m128 sm = _mm_castsi128_ps(_mm_set1_epi32((int)0x80000000u));
    __m128 a = _mm_andnot_ps(sm, x), s = _mm_and_ps(sm, x);
    return _mm_or_ps(_mm_min_ps(a, _mm_castsi128_ps(_mm_set1_epi32(0x7f7fffff))), s);
}

static inline float getq(VU *u) { if (u->cycle >= u->q_done) u->q = u->q_new; return u->q; }
static inline float getp(VU *u) { if (u->cycle >= u->p_done) u->p = u->p_new; return u->p; }

static inline void flags_push(VU *u, uint32_t mac, uint32_t status, uint32_t clip) {
    u->fhead = (u->fhead + 1) & 7;
    u->fh[u->fhead].at = u->cycle + 4;
    u->fh[u->fhead].mac = mac; u->fh[u->fhead].status = status; u->fh[u->fhead].clip = clip;
}
static inline void flags_visible(VU *u, uint32_t *mac, uint32_t *status, uint32_t *clip) {
    for (int k = 0; k < 8; k++) {
        int idx = (u->fhead - k) & 7;
        if (u->fh[idx].at <= u->cycle) {
            if (mac) *mac = u->fh[idx].mac;
            if (status) *status = u->fh[idx].status | u->status_sticky;
            if (clip) *clip = u->fh[idx].clip;
            return;
        }
    }
    int idx = (u->fhead + 1) & 7;  /* oldest */
    if (mac) *mac = u->fh[idx].mac;
    if (status) *status = u->fh[idx].status | u->status_sticky;
    if (clip) *clip = u->fh[idx].clip;
}
static inline uint32_t cur_clip(VU *u) { return u->clip_latest; }

/* compute MAC/status for a 4-wide result under dest mask, and push */
static inline void fmac_flags(VU *u, const float *res, int dest) {
    uint32_t mac = 0, st = 0;
    for (int i = 0; i < 4; i++) {
        if (!(dest & (8 >> i))) continue;
        int bit = 3 - i;
        if (res[i] == 0.0f) mac |= 1u << bit;
        if (signbit(res[i])) mac |= 0x10u << bit;
    }
    if (mac & 0x0f) st |= 1;
    if (mac & 0xf0) st |= 2;
    u->status_sticky |= (st << 6);
    flags_push(u, mac, st, u->clip_latest);
}

static inline void stall_on(VU *u, int reg) {
    if (reg && u->vf_ready[reg] > u->cycle) u->cycle = u->vf_ready[reg];
}

#define VI(n) ((n) ? (u->vi[n] & 0xffff) : 0)
#define SETVI(n, v) do { if (n) u->vi[n] = (uint32_t)(v) & 0xffff; } while (0)

extern int g_vu_kick_capture;
void vu_kick_record(uint32_t addr);
static inline void vu_xgkick(VU *u, uint32_t a) { if (u->unit != 1) return; if (g_vu_kick_capture) vu_kick_record(a); else gif_path1_kick(u->mem, a); }
static inline r128 *vmem(VU *u, uint32_t qaddr) { return (r128 *)(u->mem + ((qaddr * 16) & u->memmask)); }
static inline void efu(VU *u, float v, int lat) { u->p_new = v; u->p_done = u->cycle + lat; }

typedef int (*VuProgFn)(VU *u);   /* returns 0 at E-bit end, 1 to continue interpreting at u->pc */
typedef struct { uint32_t entry; uint64_t hash; VuProgFn fn; } VuProg;
#endif
