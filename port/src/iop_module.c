/* Host side of the recompiled IOP modules: loading, calls into module code,
 * and the IOP library functions they import (kernel, SIF RPC, libsd -> SPU2). */
#include "iop_mod.h"
#include "spu2.h"
#include <pthread.h>

extern const uint32_t sndn_base, sndn_size, sndn_gp, sndn_entry;
extern const uint8_t sndn_image[];
extern const IopFn sndn_funcs[];

static pthread_mutex_t iop_lock;
static uint32_t iop_stack_top;
static int iop_depth;
static int nfuncs;

void iop_call(IopCtx *c, uint32_t addr) {
    int lo = 0, hi = nfuncs - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (sndn_funcs[mid].addr == addr) { sndn_funcs[mid].fn(c); return; }
        if (sndn_funcs[mid].addr < addr) lo = mid + 1; else hi = mid - 1;
    }
    WARN("[iop] call to unknown module address %08x (ra %08x)\n", addr, c->r[31]);
}
void iop_break(IopCtx *c, uint32_t pc) { (void)c; WARN("[iop] break at %08x\n", pc); }
void iop_syscall(IopCtx *c, uint32_t pc) { (void)c; WARN("[iop] syscall at %08x\n", pc); }
uint32_t iop_mfc0(IopCtx *c, int reg) { (void)c; (void)reg; return 0; }
void iop_mtc0(IopCtx *c, int reg, uint32_t v) { (void)c; (void)reg; (void)v; }
void iop_rfe(IopCtx *c) { (void)c; }

/* Run module code at addr with up to four arguments; returns v0. Nested calls
 * (callbacks fired from inside module code) get their own stack area. */
static uint32_t iop_run(uint32_t addr, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
    pthread_mutex_lock(&iop_lock);
    IopCtx c; memset(&c, 0, sizeof c);
    c.r[4] = a0; c.r[5] = a1; c.r[6] = a2; c.r[7] = a3;
    c.r[28] = sndn_gp;
    c.r[29] = iop_stack_top - (uint32_t)iop_depth * 0x3000 - 0x40;
    c.r[31] = 0;
    iop_depth++;
    iop_call(&c, addr);
    iop_depth--;
    pthread_mutex_unlock(&iop_lock);
    return c.r[2];
}

#define A(n) (c->r[4 + (n)])
#define SARG(n) IR32(c->r[29] + 16 + 4 * ((n) - 4))
#define RET(v) (c->r[2] = (uint32_t)(v))

/* ---- intrman / sifman / sifcmd / thbase / sysclib */
void iop_imp_intrman_6(IopCtx *c) { RET(0); }
void iop_imp_intrman_9(IopCtx *c) { RET(0); }
void iop_imp_sifman_5(IopCtx *c) { RET(0); }
void iop_imp_sifman_29(IopCtx *c) { RET(1); }
void iop_imp_sifcmd_14(IopCtx *c) { RET(0); }
void iop_imp_sifcmd_19(IopCtx *c) { RET(0); }
void iop_imp_sifcmd_22(IopCtx *c) { RET(0); }   /* sceSifRpcLoop: requests are dispatched by the host */

static struct { uint32_t sid, func; } rpc[4];
static int nrpc;
static uint32_t rpc_bridge(uint32_t sid, int fno, uint32_t buf, int size, int *rs) {
    (void)rs;
    { static int tr = -1; if (tr < 0) tr = getenv("AE2_SNDTRACE") != NULL;
      if (tr && fno == 100) { static int nl;
        for (int off = 0; off + 16 <= size && nl < 300; off += 16) { uint32_t t = IR32(buf + off); if (t) { nl++; INFO("[sndcmd] %u %08x %08x %08x (frame %u)\n", t, IR32(buf + off + 4), IR32(buf + off + 8), IR32(buf + off + 12), g_frame); } } } }
    for (int i = 0; i < nrpc; i++) if (rpc[i].sid == sid) {
        uint32_t st = iop_run(rpc[i].func, (uint32_t)fno, buf, (uint32_t)size, 0);
        if (sid == 0x736e646e && fno == 100 && st) {
            /* Stream read positions advance in 1 KB mixing blocks, but the EE refills by
             * 2 KB CD sectors and treats a zero-sector refill as the end of the stream.
             * At PS2 frame rates a poll never sees a 1 KB step; with this port's varying
             * frame rate it can, so report positions in whole sectors. */
            for (int ch = 0; ch < 16; ch++) IW32(st + 0x180 + ch * 4, IR32(st + 0x180 + ch * 4) & ~0x7ffu);
        }
        return st;
    }
    return 0;
}
void iop_imp_sifcmd_17(IopCtx *c) {   /* sceSifRegisterRpc(sd, sid, func, buf, cfunc, cbuf, qd) */
    uint32_t sid = A(1), func = A(2), buf = A(3);
    if (nrpc < 4) { rpc[nrpc].sid = sid; rpc[nrpc].func = func; nrpc++; }
    iop_register_server_buf(sid, rpc_bridge, buf, 0x10000);
    INFO("[iop] module registered RPC server %08x (handler %08x, buffer %08x)\n", sid, func, buf);
    RET(0);
}
static uint32_t th_entry;
void iop_imp_thbase_4(IopCtx *c) { th_entry = IR32(A(0) + 8); RET(1); }   /* CreateThread */
void iop_imp_thbase_6(IopCtx *c) {                                      /* StartThread: run it now */
    uint32_t e = th_entry, arg = A(1);
    IopCtx t; memset(&t, 0, sizeof t);
    t.r[4] = arg; t.r[28] = sndn_gp; t.r[29] = c->r[29] - 0x1000;
    iop_call(&t, e);
    RET(0);
}
void iop_imp_thbase_20(IopCtx *c) { RET(1); }
void iop_imp_sysclib_12(IopCtx *c) { memmove(IOP(A(0)), IOP(A(1)), A(2) & 0x1fffff); RET(A(0)); }
void iop_imp_sysclib_14(IopCtx *c) { memset(IOP(A(0)), (int)A(1), A(2) & 0x1fffff); RET(A(0)); }
void iop_imp_sysclib_36(IopCtx *c) {                                   /* strtol */
    if (!A(0)) { RET(0); return; }
    char tmp[32]; int i;
    for (i = 0; i < 31 && IR8(A(0) + i); i++) tmp[i] = (char)IR8(A(0) + i);
    tmp[i] = 0;
    RET(strtol(tmp, NULL, (int)A(2)));
}

/* ---- libsd */
void iop_imp_libsd_4(IopCtx *c) { sd_init((int)A(0)); RET(0); }
void iop_imp_libsd_5(IopCtx *c) { sd_set_param(A(0) & 0xffff, A(1) & 0xffff); RET(0); }
void iop_imp_libsd_6(IopCtx *c) { RET(sd_get_param(A(0) & 0xffff)); }
void iop_imp_libsd_7(IopCtx *c) { sd_set_switch(A(0) & 0xffff, A(1)); RET(0); }
void iop_imp_libsd_9(IopCtx *c) { sd_set_addr(A(0) & 0xffff, A(1)); RET(0); }
void iop_imp_libsd_10(IopCtx *c) { RET(sd_get_addr(A(0) & 0xffff)); }
void iop_imp_libsd_11(IopCtx *c) { sd_set_core_attr(A(0) & 0xffff, A(1)); RET(0); }
void iop_imp_libsd_17(IopCtx *c) { RET(sd_voice_trans((int)A(0), A(1), A(2), A(3), SARG(4))); }
void iop_imp_libsd_18(IopCtx *c) { RET(sd_block_trans((int)A(0), A(1), A(2), A(3), SARG(4))); }
void iop_imp_libsd_19(IopCtx *c) { RET(sd_voice_trans_status((int)A(0), (int)A(1))); }
void iop_imp_libsd_20(IopCtx *c) { RET(sd_block_trans_status((int)A(0), (int)A(1))); }
void iop_imp_libsd_21(IopCtx *c) { RET(sd_set_trans_callback((int)A(0), A(1))); }
void iop_imp_libsd_23(IopCtx *c) { RET(0); }   /* sceSdSetEffectAttr: reverb not emulated */

/* libsd transfer-complete callbacks: int cb(int channel) */
static void trans_cb_hook(int chan, uint32_t func) { iop_run(func, (uint32_t)chan, 0, 0, 0); }

/* Load SNDN2DRV.IRX's recompiled image and run its start routine. */
void iop_sndn_start(void) {
    pthread_mutexattr_t at; pthread_mutexattr_init(&at);
    pthread_mutexattr_settype(&at, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&iop_lock, &at);
    for (nfuncs = 0; sndn_funcs[nfuncs].fn; nfuncs++);
    memcpy(IOP(sndn_base), sndn_image, sndn_size);
    iop_stack_top = iop_alloc(0x10000) + 0x10000;
    spu2_set_cb_hook(trans_cb_hook);
    spu2_init(g_headless);
    /* argv = {"sndn2drv", NULL} */
    uint32_t argv = iop_alloc(32);
    IW32(argv, argv + 16); IW32(argv + 4, 0); memcpy(IOP(argv + 16), "sndn2drv", 9);
    uint32_t r = iop_run(sndn_entry, 1, argv, 0, 0);
    INFO("[iop] SNDN2DRV.IRX (recompiled, %d functions) started: %u\n", nfuncs, r);
}
