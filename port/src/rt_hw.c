/* EE hardware registers: timers, INTC, DMAC (+ channel transfers), GIF/VIF FIFOs, GS privileged regs. */
#include "rt.h"

static uint32_t hwreg[0x10000 / 4];       /* 0x10000000..0x1000FFFF shadow */
#define HW(a) hwreg[((a) & 0xffff) >> 2]

/* ---------------------------------------------------------------- timers */
typedef struct { uint64_t base_us; uint32_t base_count; } Timer;
static Timer timers[4];
static uint32_t timer_addr(int t) { return t < 2 ? 0x10000000 + t * 0x800 : 0x10001000 + (t - 2) * 0x800; }
static uint32_t timer_rate(int t) {
    uint32_t mode = HW(timer_addr(t) + 0x10);
    switch (mode & 3) {
    case 0: return 147456000; case 1: return 147456000 / 16; case 2: return 147456000 / 256;
    default: return 15625;  /* PAL hblank */
    }
}
static uint32_t timer_count(int t) {
    uint32_t mode = HW(timer_addr(t) + 0x10);
    if (!(mode & 0x80)) return timers[t].base_count;
    uint64_t el = host_usec() - timers[t].base_us;
    return (uint32_t)(timers[t].base_count + el * timer_rate(t) / 1000000ull) & 0xffff;
}

/* ---------------------------------------------------------------- DMA */
static uint32_t dma_base[10] = {0x10008000, 0x10009000, 0x1000A000, 0x1000B000, 0x1000B400,
                                0x1000C000, 0x1000C400, 0x1000C800, 0x1000D000, 0x1000D400};
static int dma_channel(uint32_t a) {
    for (int i = 0; i < 10; i++) if ((a & ~0xffu) == dma_base[i]) return i;
    return -1;
}

static uint8_t *dma_ptr(uint32_t addr, int spr) {
    if (spr || (addr & 0x80000000u)) return g_spr + (addr & 0x3ff0);
    uint8_t *p = mem_ptr(addr & (EE_RAM_MAX - 16));
    return p ? p : g_ram;
}

static void dma_deliver(int ch, const uint8_t *data, uint32_t qwc) {
    if (!qwc) return;
    switch (ch) {
    case 0: vif_transfer(0, data, qwc); break;
    case 1: vif1_submit(data, qwc); break;
    case 2: gif_transfer(data, qwc, 3); break;
    default: break;
    }
}

int (*g_dma_top_call)(uint32_t tadr, uint32_t addr);
static void dma_run(int ch) {
    uint32_t b = dma_base[ch];
    uint32_t chcr = HW(b);
    int mode = (chcr >> 2) & 3;
    int tte = (chcr >> 6) & 1;
    uint32_t qwc = HW(b + 0x20) & 0xffff;
    uint32_t madr = HW(b + 0x10);
#ifdef AE2_FPCHECK
    if (getenv("AE2_DMASTACK") && ch == 8) {   /* scratchpad to memory: where packets are built */
        extern uint32_t g_pad_polls; static uint32_t last_fn, lastp; char st[200]; int k = 0;
        for (int d = g_fdepth - 1; d >= 0 && d >= g_fdepth - 10; d--) if (d < 256) k += snprintf(st + k, sizeof st - (size_t)k, " <%06x", g_fstack[d]);
        uint32_t fn = g_fdepth > 0 ? g_fstack[g_fdepth - 1] : 0;
        if (g_pad_polls == 1400 && (fn != last_fn || lastp != g_pad_polls)) INFO("[spr] %u madr %08x qwc %u%s\n", g_pad_polls, madr, qwc, st);
        last_fn = fn; lastp = g_pad_polls;
    }
    if (getenv("AE2_DMASTACK") && (ch == 1 || ch == 2)) {   /* who starts the VIF1/GIF transfers */
        extern uint32_t g_pad_polls; char st[200]; int k = 0;
        for (int d = g_fdepth - 1; d >= 0 && d >= g_fdepth - 14; d--) if (d < 256) k += snprintf(st + k, sizeof st - (size_t)k, " <%06x", g_fstack[d]);
        if (getenv("AE2_DMACHAIN") && g_pad_polls == (uint32_t)atoi(getenv("AE2_DMACHAIN"))) {
            uint32_t t = HW(b + 0x30), stk[4]; int sp = 0;
            for (int i = 0; i < 200000; i++) {
                uint32_t lo = MR32(t), addr = MR32(t + 4), q = lo & 0xffff, id = (lo >> 28) & 7;
                if (sp == 0 || getenv("AE2_DMACHAINALL")) INFO("[chain] %08x id%u qwc %u addr %08x\n", t, id, q, addr);
                if (id == 1) t = t + 16 + q * 16; else if (id == 2) t = addr;
                else if (id == 3 || id == 4) t += 16;
                else if (id == 5) { stk[sp++ & 3] = t + 16 + q * 16; t = addr; }
                else if (id == 6) { if (!sp) break; t = stk[--sp & 3]; }
                else break;
            }
        }
        INFO("[dma] %u ch%d madr %08x tadr %08x chcr %08x%s\n", g_pad_polls, ch, madr, HW(b + 0x30), chcr, st);
    }
#endif

    if (ch == 8 || ch == 9) {   /* scratchpad transfers */
        uint32_t sadr = HW(b + 0x80) & 0x3ff0;
        if (mode == 0 || mode == 2) {
            /* normal, or interleave: TQWC qwords moved, then SQWC qwords of main memory skipped */
            uint32_t sq = HW(0x1000E030), skip = sq & 0xff, take = (sq >> 16) & 0xff;
            if (mode == 0 || !take) { take = qwc ? qwc : 1; skip = 0; }
            uint32_t m = madr;
            for (uint32_t i = 0, run = 0; i < qwc; i++) {
                uint8_t *ram = dma_ptr(m, 0);
                if (ch == 9) memcpy(g_spr + ((sadr + i * 16) & 0x3ff0), ram, 16);
                else memcpy(ram, g_spr + ((sadr + i * 16) & 0x3ff0), 16);
                m += 16;
                if (++run == take) { run = 0; m += skip * 16; }
            }
            HW(b + 0x10) = m; HW(b + 0x80) = (sadr + qwc * 16) & 0x3ff0;
            HW(b + 0x20) = 0;
        } else if (ch == 9) {     /* toSPR chain */
            uint32_t tadr = HW(b + 0x30);
            for (int guard = 0; guard < 100000; guard++) {
                uint8_t *tp = dma_ptr(tadr, 0);
                uint64_t tag; memcpy(&tag, tp, 8);
                uint32_t q = tag & 0xffff, id = (tag >> 28) & 7, addr = (uint32_t)(tag >> 32) & 0x7ffffff0;
                if (tte) { memcpy(g_spr + sadr, tp, 16); sadr = (sadr + 16) & 0x3ff0; }
                uint8_t *src = (id == 1) ? tp + 16 : dma_ptr(addr, 0);
                for (uint32_t i = 0; i < q; i++) { memcpy(g_spr + sadr, src + i * 16, 16); sadr = (sadr + 16) & 0x3ff0; }
                if (id == 1) tadr = tadr + 16 + q * 16;
                else if (id == 2) tadr = addr;
                else tadr += 16;
                if (id == 7 || id == 0 || ((tag >> 31) & 1 && (chcr & 0x80))) break;
            }
            HW(b + 0x30) = tadr; HW(b + 0x80) = sadr;
        }
        goto done;
    }

    if (mode == 0) {           /* normal */
        dma_deliver(ch, dma_ptr(madr, 0), qwc);
        HW(b + 0x10) = madr + qwc * 16;
        HW(b + 0x20) = 0;
    } else if (mode == 1) {    /* source chain */
        uint32_t tadr = HW(b + 0x30);
        uint32_t asr[2] = {HW(b + 0x40), HW(b + 0x50)};
        int asp = (chcr >> 4) & 3;
        if (qwc) { dma_deliver(ch, dma_ptr(madr, 0), qwc); HW(b + 0x20) = 0; }
        for (int guard = 0; guard < 1000000; guard++) {
            uint8_t *tp = dma_ptr(tadr, 0);
            uint64_t tag; memcpy(&tag, tp, 8);
            uint32_t q = tag & 0xffff, id = (tag >> 28) & 7, irq = (tag >> 31) & 1;
            uint32_t addr = (uint32_t)(tag >> 32) & 0x7ffffff0;
            int spr = (tag >> 63) & 1;
            HW(b) = (HW(b) & 0xffff) | (uint32_t)(tag & 0xffff0000u);
            if (tte && (ch == 0 || ch == 1)) {
                /* upper 64 bits of the tag are VIF codes: deliver as a qword with low half zero (NOPs) */
                uint8_t qw[16] = {0};
                memcpy(qw + 8, tp + 8, 8);
                if (ch == 1) vif1_submit(qw, 1); else vif_transfer(ch, qw, 1);
            }
            uint32_t next = tadr + 16;
            /* p2.c (split screen) sees each top-level call of the frame's draw list and can drop one */
            if (ch == 1 && asp == 0 && id == 5 && g_dma_top_call && !g_dma_top_call(tadr, addr)) { tadr = next; continue; }
            switch (id) {
            case 0: dma_deliver(ch, dma_ptr(addr, spr), q); next = 0; break;                  /* refe */
            case 1: dma_deliver(ch, tp + 16, q); next = tadr + 16 + q * 16; break;            /* cnt */
            case 2: dma_deliver(ch, tp + 16, q); next = addr; break;                          /* next */
            case 3: case 4: dma_deliver(ch, dma_ptr(addr, spr), q); next = tadr + 16; break;  /* ref/refs */
            case 5: dma_deliver(ch, tp + 16, q);                                              /* call */
                if (asp < 2) asr[asp++] = tadr + 16 + q * 16;
                next = addr; break;
            case 6: dma_deliver(ch, tp + 16, q);                                              /* ret */
                if (asp > 0) next = asr[--asp]; else next = 0;
                if (!next) id = 7;
                break;
            case 7: dma_deliver(ch, tp + 16, q); next = 0; break;                             /* end */
            }
            tadr = next;
            if (id == 0 || id == 7 || (irq && (chcr & 0x80))) break;
        }
        HW(b + 0x30) = tadr; HW(b + 0x40) = asr[0]; HW(b + 0x50) = asr[1];
    } else {
        WARN("[dma] ch%d unsupported mode %d\n", ch, mode);
    }
done:
    HW(b) &= ~0x100u;
    HW(0x1000E010) |= 1u << ch;
    kernel_raise_dmac(ch);
}

/* ---------------------------------------------------------------- IPU (ipu.c) */
int ipu_handles(uint32_t a); uint32_t ipu_read32(uint32_t a); uint64_t ipu_read64(uint32_t a);
void ipu_write32(uint32_t a, uint32_t v); void ipu_fifo_read(uint8_t out[16]); void ipu_fifo_write(const uint8_t in[16]);
void ipu_dma_start(int ch); void ipu_poll(void);
uint32_t *hw_regp(uint32_t a) { return &HW(a); }
uint8_t *hw_dma_mem(uint32_t addr) { return dma_ptr(addr, 0); }
void hw_dma_done(int ch) { HW(0x1000E010) |= 1u << ch; kernel_raise_dmac(ch); }

/* ---------------------------------------------------------------- access */
static uint32_t poll_ctr;
uint32_t hw_read32(uint32_t a) {
    if (ipu_handles(a)) return ipu_read32(a);
    if (a == 0x1000B000u || a == 0x1000B400u) ipu_poll();   /* IPU DMA CHCR polled for completion */
    if ((++poll_ctr & 63) == 0 && !g_in_intr) sched_check_timers();
    if (a >= 0x12000000) return (uint32_t)gs_read_priv64(a & ~7u) >> ((a & 4) * 8);
    uint32_t off = a & 0xffff;
    if (off < 0x2000) {
        int t = off < 0x1000 ? (off >> 11) : 2 + ((off - 0x1000) >> 11);
        if ((off & 0x7ff) == 0) return timer_count(t);
    }
    if (a == 0x1000F000) return HW(a);                /* INTC_STAT */
    if (a == 0x1000F230) return sif_get_reg(4);       /* SBUS SMFLG */
    if (a == 0x1000F130) return 0;
    if (a == 0x10003020) return 0;                    /* GIF_STAT idle */
    if (a == 0x10003C00) return HW(a) & ~0x3u;        /* VIF1_STAT idle */
    if (a == 0x10003800) return HW(a) & ~0x3u;
    return HW(a);
}

void hw_write32(uint32_t a, uint32_t v) {
    if (a >= 0x12000000) {
        uint64_t old = gs_read_priv64(a & ~7u);
        uint64_t nv = (a & 4) ? ((old & 0xffffffffull) | ((uint64_t)v << 32)) : ((old & ~0xffffffffull) | v);
        gs_write_priv64(a & ~7u, nv);
        return;
    }
    if (ipu_handles(a)) { ipu_write32(a, v); return; }
    uint32_t off = a & 0xffff;
    if (off < 0x2000) {
        int t = off < 0x1000 ? (off >> 11) : 2 + ((off - 0x1000) >> 11);
        uint32_t r = off & 0x7ff;
        if (r == 0) { timers[t].base_count = v & 0xffff; timers[t].base_us = host_usec(); }
        if (r == 0x10) { timers[t].base_count = timer_count(t); timers[t].base_us = host_usec(); }
        HW(a) = v;
        return;
    }
    switch (a) {
    case 0x1000F000: HW(a) &= ~v; return;             /* INTC_STAT: write 1 to clear */
    case 0x1000F010: HW(a) ^= v; return;              /* INTC_MASK: toggle */
    case 0x1000E010: {                                /* D_STAT: clear CIS, toggle CIM */
        uint32_t s = HW(a);
        s &= ~(v & 0xffff);
        s ^= (v & 0xffff0000u);
        HW(a) = s; return;
    }
    case 0x1000F180: { char ch = (char)v; fputc(ch, stderr); return; }  /* KPUTCHAR */
    case 0x10003000: if (v & 1) vif1_reset(); HW(a) = v & ~1u; return; /* GIF_CTRL reset */
    case 0x10003C10: if (v & 1) vif1_reset(); HW(a) = 0; return;        /* VIF1_FBRST */
    default: break;
    }
    int ch = dma_channel(a);
    HW(a) = v;
    if ((ch == 3 || ch == 4) && (a & 0xff) == 0) { if (v & 0x100) ipu_dma_start(ch); return; }
    if (ch >= 0 && (a & 0xff) == 0 && (v & 0x100)) {
        if (ch == 5 || ch == 6 || ch == 7) { HW(a) &= ~0x100u; return; }  /* SIF channels: handled via syscalls */
        dma_run(ch);
    }
}

uint64_t hw_read64(uint32_t a) {
    if (a >= 0x12000000) return gs_read_priv64(a);
    if (ipu_handles(a)) return ipu_read64(a);
    return hw_read32(a) | ((uint64_t)hw_read32(a + 4) << 32);
}
void hw_write64(uint32_t a, uint64_t v) {
    if (a >= 0x12000000) { gs_write_priv64(a, v); return; }
    if (ipu_handles(a)) { ipu_write32(a, (uint32_t)v); return; }
    hw_write32(a, (uint32_t)v); hw_write32(a + 4, (uint32_t)(v >> 32));
}
void hw_read128(uint32_t a, r128 *o) {
    memset(o, 0, 16);
    if ((a & 0xfffffff0u) == 0x10007000u) { ipu_fifo_read(o->ub); return; }
    o->ud[0] = hw_read64(a);
}
void hw_write128(uint32_t a, const r128 *v) {
    uint32_t off = a & 0xffff;
    if (a < 0x12000000 && off >= 0x4000 && off < 0x8000) {
        switch (off & 0xf000) {
        case 0x4000: vif_transfer(0, v->ub, 1); return;
        case 0x5000: vif1_submit(v->ub, 1); return;
        case 0x6000: gif_transfer(v->ub, 1, 3); return;
        case 0x7000: if ((off & 0xff0) == 0x010) ipu_fifo_write(v->ub); return;   /* IPU input FIFO */
        }
    }
    hw_write64(a, v->ud[0]);
}

void hw_vblank(int start) {
    HW(0x1000F000) |= start ? 4 : 8;
    if (start) { void ipu_tick(void); ipu_tick(); }
}

void hw_init(void) {
    memset(hwreg, 0, sizeof hwreg);
    HW(0x1000E000) = 0;  /* D_CTRL */
    /* The EE kernel leaves timers 2/3 free-running (BUSCLK/256) for its own
     * use; games read T2 as an entropy source (AE2 seeds its VU0 RNG from it,
     * and a zero seed makes the LFSR stick at 0 and object placement loop). */
    for (int t = 2; t < 4; t++) {
        HW(timer_addr(t) + 0x10) = 0x82;
        timers[t].base_count = 0x1234 * t; timers[t].base_us = host_usec();
    }
}
