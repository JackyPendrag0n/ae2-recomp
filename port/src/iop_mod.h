/* Runtime interface for recompiled IOP modules (see recomp/iop_recomp.py). */
#ifndef IOP_MOD_H
#define IOP_MOD_H
#include "rt.h"

typedef struct { uint32_t r[32]; uint32_t hi, lo; } IopCtx;
typedef struct { uint32_t addr; void (*fn)(IopCtx *); } IopFn;

static inline uint8_t IR8(uint32_t a) { return *IOP(a); }
static inline uint16_t IR16(uint32_t a) { uint16_t v; memcpy(&v, IOP(a), 2); return v; }
static inline uint32_t IR32(uint32_t a) { uint32_t v; memcpy(&v, IOP(a), 4); return v; }
static inline void IW8(uint32_t a, uint32_t v) { *IOP(a) = (uint8_t)v; }
static inline void IW16(uint32_t a, uint32_t v) { uint16_t x = (uint16_t)v; memcpy(IOP(a), &x, 2); }
static inline void IW32(uint32_t a, uint32_t v) { memcpy(IOP(a), &v, 4); }

static inline void IOP_DIV(IopCtx *c, uint32_t a, uint32_t b) {
    int32_t x = (int32_t)a, y = (int32_t)b;
    if (y == 0) { c->lo = x >= 0 ? 0xffffffffu : 1; c->hi = a; }
    else if (x == INT32_MIN && y == -1) { c->lo = a; c->hi = 0; }
    else { c->lo = (uint32_t)(x / y); c->hi = (uint32_t)(x % y); }
}
static inline void IOP_DIVU(IopCtx *c, uint32_t a, uint32_t b) {
    if (b == 0) { c->lo = 0xffffffffu; c->hi = a; }
    else { c->lo = a / b; c->hi = a % b; }
}

void iop_call(IopCtx *c, uint32_t addr);
void iop_break(IopCtx *c, uint32_t pc);
void iop_syscall(IopCtx *c, uint32_t pc);
uint32_t iop_mfc0(IopCtx *c, int reg);
void iop_mtc0(IopCtx *c, int reg, uint32_t v);
void iop_rfe(IopCtx *c);

/* imports used by the recompiled modules */
#define IMP(lib, n) void iop_imp_##lib##_##n(IopCtx *c);
IMP(intrman, 6) IMP(intrman, 9)
IMP(libsd, 4) IMP(libsd, 5) IMP(libsd, 6) IMP(libsd, 7) IMP(libsd, 9) IMP(libsd, 10) IMP(libsd, 11)
IMP(libsd, 17) IMP(libsd, 18) IMP(libsd, 19) IMP(libsd, 20) IMP(libsd, 21) IMP(libsd, 23)
IMP(sifcmd, 14) IMP(sifcmd, 17) IMP(sifcmd, 19) IMP(sifcmd, 22)
IMP(sifman, 5) IMP(sifman, 29)
IMP(sysclib, 12) IMP(sysclib, 14) IMP(sysclib, 36)
IMP(thbase, 4) IMP(thbase, 6) IMP(thbase, 20)
#undef IMP
#endif
