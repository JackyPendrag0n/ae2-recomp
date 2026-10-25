/* Core definitions for recompiled Emotion Engine code. */
#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <setjmp.h>

#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

typedef union {
    uint64_t ud[2]; int64_t sd[2];
    uint32_t uw[4]; int32_t sw[4];
    uint16_t uh[8]; int16_t sh[8];
    uint8_t ub[16]; int8_t sb[16];
    float f[4];
} __attribute__((aligned(16))) r128;

typedef union { float f; uint32_t u; int32_t s; } fpr_t;

typedef struct Ctx {
    r128 r[32];
    r128 hi, lo;
    uint64_t sa;
    fpr_t f[32];
    float facc;
    uint32_t fcr31;
    /* VU0 macro mode */
    r128 vf[32];
    uint32_t vi[32];
    r128 vacc;
    float vq;
    uint32_t vr;      /* random */
    int vu0_busy;
    uint32_t pc;
    uint32_t cop0_status;
    int budget;
    int tid;          /* EE thread id owning this context */
    void *hostthread;
} Ctx;

typedef void (*RecFn)(Ctx *);
typedef struct { uint32_t start, end; RecFn fn; } FuncEntry;
extern const FuncEntry g_functable[];
extern const int g_functable_n;

/* ---------- registers */
#define RD(n)  (c->r[n].ud[0])
#define RS(n)  (c->r[n].sd[0])
#define RW(n)  (c->r[n].uw[0])
#define RSW(n) (c->r[n].sw[0])
#define SETW(n, v) (c->r[n].sd[0] = (int64_t)(int32_t)(uint32_t)(v))

/* ---------- memory */
/* EE RAM. The PS2 has 32 MB (the dev kits 128 MB); the port gives the game g_ram_size
 * bytes (AE2_RAM_MB, default 128 MB). The whole window below the hardware registers
 * (0x10000000) is reserved up front, so the fast path below needs no variable bound. */
#define EE_RAM_MAX 0x10000000u
extern uint8_t *g_ram;
extern uint32_t g_ram_size;
extern uint8_t g_spr[16384];    /* scratchpad */

uint8_t  mem_r8_slow(uint32_t a);
uint16_t mem_r16_slow(uint32_t a);
uint32_t mem_r32_slow(uint32_t a);
uint64_t mem_r64_slow(uint32_t a);
void mem_r128_slow(uint32_t a, r128 *o);
void mem_w8_slow(uint32_t a, uint8_t v);
void mem_w16_slow(uint32_t a, uint16_t v);
void mem_w32_slow(uint32_t a, uint32_t v);
void mem_w64_slow(uint32_t a, uint64_t v);
void mem_w128_slow(uint32_t a, const r128 *v);
uint8_t *mem_ptr(uint32_t a);   /* host pointer for RAM/SPR, NULL for I/O */

#define RAMOK(a) likely((a) < EE_RAM_MAX)
/* CHECKS=1 builds: AE2_RWATCH=addr,len[,stride,off] logs which game functions load from
 * that range (with a stride: only offset off of each record) */
#ifdef AE2_FPCHECK
extern uint32_t g_rw_lo, g_rw_hi;
void rt_rwatch(uint32_t a, int n);
#define RWCK(a, n) do { if (unlikely((a) < g_rw_hi && (a) + (n) > g_rw_lo)) rt_rwatch(a, n); } while (0)
#else
#define RWCK(a, n) ((void)0)
#endif
static inline uint8_t  MR8(uint32_t a)  { RWCK(a, 1); return RAMOK(a) ? g_ram[a] : mem_r8_slow(a); }
static inline uint16_t MR16(uint32_t a) { RWCK(a, 2); uint16_t v; if (RAMOK(a)) { memcpy(&v, g_ram + a, 2); return v; } return mem_r16_slow(a); }
static inline uint32_t MR32(uint32_t a) { RWCK(a, 4); uint32_t v; if (RAMOK(a)) { memcpy(&v, g_ram + a, 4); return v; } return mem_r32_slow(a); }
static inline uint64_t MR64(uint32_t a) { RWCK(a, 8); uint64_t v; if (RAMOK(a)) { memcpy(&v, g_ram + a, 8); return v; } return mem_r64_slow(a); }
/* CHECKS=1 builds: AE2_WWATCH=addr,len logs every guest store into that range */
#ifdef AE2_FPCHECK
extern uint32_t g_ww_lo, g_ww_hi;
void rt_wwatch(uint32_t a, int n, uint64_t v);
#define WWCK(a, n, v) do { if (unlikely((a) < g_ww_hi && (a) + (n) > g_ww_lo)) rt_wwatch(a, n, v); } while (0)
#else
#define WWCK(a, n, v) ((void)0)
#endif
static inline void MW8(uint32_t a, uint32_t v)  { WWCK(a, 1, v); if (RAMOK(a)) g_ram[a] = (uint8_t)v; else mem_w8_slow(a, (uint8_t)v); }
static inline void MW16(uint32_t a, uint32_t v) { WWCK(a, 2, v); uint16_t x = (uint16_t)v; if (RAMOK(a)) memcpy(g_ram + a, &x, 2); else mem_w16_slow(a, x); }
static inline void MW32(uint32_t a, uint32_t v) { WWCK(a, 4, v); if (RAMOK(a)) memcpy(g_ram + a, &v, 4); else mem_w32_slow(a, v); }
static inline void MW64(uint32_t a, uint64_t v) { WWCK(a, 8, v); if (RAMOK(a)) memcpy(g_ram + a, &v, 8); else mem_w64_slow(a, v); }
static inline void mr128(uint32_t a, r128 *o) { a &= ~15u; RWCK(a, 16); if (RAMOK(a)) memcpy(o, g_ram + a, 16); else mem_r128_slow(a, o); }
static inline void MW128(uint32_t a, const r128 *v) { a &= ~15u; WWCK(a, 16, v->ud[0]); if (RAMOK(a)) memcpy(g_ram + a, v, 16); else mem_w128_slow(a, v); }
#define MR128(rt, a) mr128((a), &c->r[rt])
#define MR128VF(ft, a) do { r128 _t; mr128((a), &_t); if (ft) c->vf[ft] = _t; } while (0)

/* unaligned load/store (little endian) */
#define OP_LWL(rt, addr) do { uint32_t _a = (addr); uint32_t _s = (_a & 3) * 8; uint32_t _m = MR32(_a & ~3u); \
    uint32_t _o = RW(rt); uint32_t _r = (_o & (0x00ffffffu >> _s)) | (_m << (24 - _s)); SETW(rt, _r); } while (0)
#define OP_LWR(rt, addr) do { uint32_t _a = (addr); uint32_t _s = (_a & 3) * 8; uint32_t _m = MR32(_a & ~3u); \
    uint32_t _o = RW(rt); uint32_t _r = (_o & ~(0xffffffffu >> _s)) | (_m >> _s); \
    if (_s == 0) SETW(rt, _r); else c->r[rt].uw[0] = _r; } while (0)
#define OP_SWL(rt, addr) do { uint32_t _a = (addr); uint32_t _s = (_a & 3) * 8; uint32_t _m = MR32(_a & ~3u); \
    MW32(_a & ~3u, (RW(rt) >> (24 - _s)) | (_m & ~(0xffffffffu >> (24 - _s)))); } while (0)
#define OP_SWR(rt, addr) do { uint32_t _a = (addr); uint32_t _s = (_a & 3) * 8; uint32_t _m = MR32(_a & ~3u); \
    MW32(_a & ~3u, (RW(rt) << _s) | (_m & ~(0xffffffffu << _s))); } while (0)
#define OP_LDL(rt, addr) do { uint32_t _a = (addr); uint32_t _s = (_a & 7) * 8; uint64_t _m = MR64(_a & ~7u); \
    RD(rt) = (RD(rt) & (0x00ffffffffffffffull >> _s)) | (_m << (56 - _s)); } while (0)
#define OP_LDR(rt, addr) do { uint32_t _a = (addr); uint32_t _s = (_a & 7) * 8; uint64_t _m = MR64(_a & ~7u); \
    RD(rt) = (RD(rt) & ~(0xffffffffffffffffull >> _s)) | (_m >> _s); } while (0)
#define OP_SDL(rt, addr) do { uint32_t _a = (addr); uint32_t _s = (_a & 7) * 8; uint64_t _m = MR64(_a & ~7u); \
    MW64(_a & ~7u, (RD(rt) >> (56 - _s)) | (_m & ~(0xffffffffffffffffull >> (56 - _s)))); } while (0)
#define OP_SDR(rt, addr) do { uint32_t _a = (addr); uint32_t _s = (_a & 7) * 8; uint64_t _m = MR64(_a & ~7u); \
    MW64(_a & ~7u, (RD(rt) << _s) | (_m & ~(0xffffffffffffffffull << _s))); } while (0)

/* ---------- multiply / divide (pipe 0 or 1) */
#define OP_MULT(rd, rs, rt, p) do { int64_t _p = (int64_t)RSW(rs) * RSW(rt); \
    c->lo.sd[p] = (int32_t)_p; c->hi.sd[p] = (int32_t)(_p >> 32); if (rd) RS(rd) = c->lo.sd[p]; } while (0)
#define OP_MULTU(rd, rs, rt, p) do { uint64_t _p = (uint64_t)RW(rs) * RW(rt); \
    c->lo.sd[p] = (int32_t)_p; c->hi.sd[p] = (int32_t)(_p >> 32); if (rd) RS(rd) = c->lo.sd[p]; } while (0)
#define OP_MADD(rd, rs, rt, p) do { int64_t _acc = ((int64_t)c->hi.uw[p*2] << 32) | c->lo.uw[p*2]; \
    _acc += (int64_t)RSW(rs) * RSW(rt); c->lo.sd[p] = (int32_t)_acc; c->hi.sd[p] = (int32_t)(_acc >> 32); \
    if (rd) RS(rd) = c->lo.sd[p]; } while (0)
#define OP_MADDU(rd, rs, rt, p) do { uint64_t _acc = ((uint64_t)c->hi.uw[p*2] << 32) | c->lo.uw[p*2]; \
    _acc += (uint64_t)RW(rs) * RW(rt); c->lo.sd[p] = (int32_t)_acc; c->hi.sd[p] = (int32_t)(_acc >> 32); \
    if (rd) RS(rd) = c->lo.sd[p]; } while (0)
static inline void op_div(Ctx *c, int32_t n, int32_t d, int p) {
    if (d == 0) { c->lo.sd[p] = n < 0 ? 1 : -1; c->hi.sd[p] = n; }
    else if (n == (int32_t)0x80000000 && d == -1) { c->lo.sd[p] = (int32_t)0x80000000; c->hi.sd[p] = 0; }
    else { c->lo.sd[p] = n / d; c->hi.sd[p] = n % d; }
}
static inline void op_divu(Ctx *c, uint32_t n, uint32_t d, int p) {
    if (d == 0) { c->lo.sd[p] = -1; c->hi.sd[p] = (int32_t)n; }
    else { c->lo.sd[p] = (int32_t)(n / d); c->hi.sd[p] = (int32_t)(n % d); }
}
#define OP_DIV(rs, rt, p) op_div(c, RSW(rs), RSW(rt), p)
#define OP_DIVU(rs, rt, p) op_divu(c, RW(rs), RW(rt), p)

/* ---------- FPU (PS2 single precision, no inf/nan) */
#define FMAXV 3.4028234663852886e38f
static inline float fclamp(float x) {
    uint32_t u; memcpy(&u, &x, 4);
    if ((u & 0x7f800000) == 0x7f800000) { u = (u & 0x80000000) | 0x7f7fffff; memcpy(&x, &u, 4); }
    else if ((u & 0x7f800000) == 0) { u &= 0x80000000; memcpy(&x, &u, 4); }
    return x;
}
/* AE2_FPCHECK builds (make CHECKS=1) report FPU operands that are NaN/Inf on the host:
 * on the PS2 those bit patterns are ordinary (huge) numbers, so the host result can differ */
#ifdef AE2_FPCHECK
void rt_fpcheck(const char *op, float a, float b);
#define FPCK(op, a, b) rt_fpcheck(op, a, b)
#else
#define FPCK(op, a, b) ((void)0)
#endif
/* operands: an exponent-255 pattern is a (huge) number on the PS2, NaN/Inf on the host;
 * treat it as +-FLT_MAX, as PCSX2's operand clamping does */
static inline float fop(float x) {
    uint32_t u; memcpy(&u, &x, 4);
    if (unlikely((u & 0x7f800000) == 0x7f800000)) { u = (u & 0x80000000) | 0x7f7fffff; memcpy(&x, &u, 4); }
    return x;
}
static inline float FADD(float a, float b) { FPCK("add.s", a, b); return fclamp(fop(a) + fop(b)); }
static inline float FSUB(float a, float b) { FPCK("sub.s", a, b); return fclamp(fop(a) - fop(b)); }
static inline float FMUL(float a, float b) { FPCK("mul.s", a, b); return fclamp(fop(a) * fop(b)); }
static inline float FDIV(float a, float b) {
    FPCK("div.s", a, b);
    a = fop(a); b = fop(b);
    if (b == 0.0f) { uint32_t ua, ub, r; memcpy(&ua, &a, 4); memcpy(&ub, &b, 4); r = ((ua ^ ub) & 0x80000000) | 0x7f7fffff; float f; memcpy(&f, &r, 4); return f; }
    return fclamp(a / b);
}
static inline float FSQRT(float a) { FPCK("sqrt.s", a, 0.0f); return sqrtf(fabsf(fop(a))); }
static inline float FRSQRT(float a, float b) { float s = sqrtf(fabsf(fop(b))); return FDIV(a, s); }
/* PS2 compare / min / max: there is no NaN, and denormals count as zero, so values are
 * ordered as sign-magnitude numbers (exponent 255 is just bigger than FLT_MAX). */
static inline int32_t fkey(float f) {
    uint32_t u; memcpy(&u, &f, 4);
    if ((u & 0x7f800000) == 0) return 0;               /* +-0 and denormals */
    return (u & 0x80000000) ? -(int32_t)(u & 0x7fffffff) : (int32_t)u;
}
static inline int FCEQ(float a, float b) { FPCK("c.eq.s", a, b); return fkey(a) == fkey(b); }
static inline int FCLT(float a, float b) { FPCK("c.lt.s", a, b); return fkey(a) < fkey(b); }
static inline int FCLE(float a, float b) { FPCK("c.le.s", a, b); return fkey(a) <= fkey(b); }
static inline float FMAX(float a, float b) { FPCK("max.s", a, b); return fkey(a) > fkey(b) ? a : b; }
static inline float FMIN(float a, float b) { FPCK("min.s", a, b); return fkey(a) < fkey(b) ? a : b; }
static inline int32_t CVTWS(float f) {
    if (f >= 2147483648.0f) return 0x7fffffff;
    if (f <= -2147483648.0f) return (int32_t)0x80000000;
    if (f != f) return 0x7fffffff;
    return (int32_t)f;
}
#define FCOND(x) do { if (x) c->fcr31 |= 0x800000u; else c->fcr31 &= ~0x800000u; } while (0)

/* ---------- runtime hooks */
void rt_call(Ctx *c, uint32_t addr);
void rt_syscall(Ctx *c);
void rt_break(Ctx *c, uint32_t pc);
void rt_badop(Ctx *c, uint32_t pc, uint32_t w);
uint32_t rt_mfc0(Ctx *c, int r);
void rt_mtc0(Ctx *c, int r, uint32_t v);
void rt_eret(Ctx *c);
int rt_cpcond0(Ctx *c);
uint32_t rt_cfc2(Ctx *c, int r);
void rt_ctc2(Ctx *c, int r, uint32_t v);
void rt_vcallms(Ctx *c, uint32_t addr);
void rt_poll(Ctx *c);
void rt_trace_pc(Ctx *c, uint32_t pc);
void rt_longjmp(Ctx *c);
jmp_buf *rt_setjmp_buf(Ctx *c, uint32_t guestbuf);
void rt_setjmp_save(Ctx *c, uint32_t guestbuf);

/* function-entry hook for debugging (AE2_FTRACE): one predictable branch per call */
extern int g_ftrace;
void rt_ftrace(Ctx *c, uint32_t addr);
#ifdef AE2_FPCHECK
extern uint32_t g_last_fn;
/* check builds keep a shadow call stack (per EE thread) for the write watch */
extern __thread uint32_t g_fstack[256]; extern __thread int g_fdepth;
static inline uint32_t rt_fpush(uint32_t a) { if (g_fdepth < 256) g_fstack[g_fdepth] = a; g_fdepth++; g_last_fn = a; return a; }
static inline void rt_fpop(uint32_t *p) { (void)p; if (g_fdepth > 0) g_fdepth--; g_last_fn = (g_fdepth > 0 && g_fdepth <= 256) ? g_fstack[g_fdepth - 1] : 0; }
#define FENTER(a) uint32_t _fenter __attribute__((cleanup(rt_fpop), unused)) = rt_fpush(a); if (unlikely(g_ftrace)) rt_ftrace(c, a)
#else
#define FENTER(a) do { if (unlikely(g_ftrace)) rt_ftrace(c, a); } while (0)
#endif
#define POLL() do { if (unlikely(--c->budget < 0)) rt_poll(c); } while (0)
/* guest setjmp: host setjmp in the caller's frame */
#define RT_SETJMP(c) do { uint32_t _gb = RW(4); jmp_buf *_jb = rt_setjmp_buf(c, _gb); \
    if (setjmp(*_jb) == 0) { rt_setjmp_save(c, _gb); RD(2) = 0; } } while (0)

#include "mmi.h"
#include "vu0.h"
