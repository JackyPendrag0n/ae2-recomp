/* Guest memory map: RAM mirrors, scratchpad, VU memory, hardware registers. */
#include "rt.h"
#include <execinfo.h>

uint8_t *g_ram;
uint32_t g_ram_size = 32u << 20;
uint8_t g_spr[16384] __attribute__((aligned(64)));
uint8_t g_vu0mem[4096] __attribute__((aligned(64)));
uint8_t g_vu0code[4096] __attribute__((aligned(64)));
uint8_t g_vu1mem[16384] __attribute__((aligned(64)));
uint8_t g_vu1code[16384] __attribute__((aligned(64)));

static uint32_t bad_count;

/* Returns host pointer for plain memory regions, NULL for I/O. */
uint8_t *mem_ptr(uint32_t a) {
    if ((a >> 28) == 7) {
        if (a < 0x70004000) return g_spr + (a & 0x3fff);
        return NULL;
    }
    uint32_t top = a >> 28;
    uint32_t p = ((top & 0xe) == 2) ? (a & 0x0fffffff) : (a & 0x1fffffff);
    if (p < EE_RAM_MAX) return g_ram + p;
    if (p >= 0x11000000 && p < 0x11010000) {
        uint32_t o = p & 0xffff;
        if (o < 0x1000) return g_vu0code + o;
        if (o >= 0x4000 && o < 0x5000) return g_vu0mem + (o & 0xfff);
        /* VU1 memory belongs to the GS thread while it runs VU1 (MTVU): let it finish first */
        if (o >= 0x8000) { void gs_mt_sync(void); gs_mt_sync(); }
        if (o >= 0x8000 && o < 0xC000) return g_vu1code + (o & 0x3fff);
        if (o >= 0xC000) return g_vu1mem + (o & 0x3fff);
        return NULL;
    }
    if (p >= 0x1fc00000 && p < 0x20000000) return NULL;
    return NULL;
}
void *gptr(uint32_t a) { return mem_ptr(a); }

static int is_hw(uint32_t p) { return (p >= 0x10000000 && p < 0x10010000) || (p >= 0x12000000 && p < 0x12002000); }

static void bad_access(const char *what, uint32_t a) {
    if (bad_count++ < 50) WARN("[mem] unmapped %s 0x%08x\n", what, a);
    if (bad_count <= 2 && getenv("AE2_BADTRACE")) {
        void *bt[24]; int n = backtrace(bt, 24);
        backtrace_symbols_fd(bt, n, 2);
    }
}

#define SLOW_READ(T, NAME, HWEXPR)                                     \
    T NAME(uint32_t a) {                                               \
        uint8_t *p = mem_ptr(a);                                       \
        if (p) { T v; memcpy(&v, p, sizeof(T)); return v; }            \
        uint32_t ph = a & 0x1fffffff;                                  \
        if (is_hw(ph)) return (T)(HWEXPR);                             \
        if (ph >= 0x1fc00000 && ph < 0x20000000) return 0;             \
        bad_access("read", a);                                         \
        return 0;                                                      \
    }
SLOW_READ(uint8_t, mem_r8_slow, hw_read32(ph & ~3u) >> ((ph & 3) * 8))
SLOW_READ(uint16_t, mem_r16_slow, hw_read32(ph & ~3u) >> ((ph & 2) * 8))
SLOW_READ(uint32_t, mem_r32_slow, hw_read32(ph))
SLOW_READ(uint64_t, mem_r64_slow, hw_read64(ph))

void mem_r128_slow(uint32_t a, r128 *o) {
    uint8_t *p = mem_ptr(a);
    if (p) { memcpy(o, p, 16); return; }
    uint32_t ph = a & 0x1fffffff;
    if (is_hw(ph)) { hw_read128(ph, o); return; }
    memset(o, 0, 16);
    if (!(ph >= 0x1fc00000 && ph < 0x20000000)) bad_access("read128", a);
}

void mem_w8_slow(uint32_t a, uint8_t v) {
    uint8_t *p = mem_ptr(a);
    if (p) { *p = v; return; }
    uint32_t ph = a & 0x1fffffff;
    if (is_hw(ph)) {
        uint32_t sh = (ph & 3) * 8;
        uint32_t old = hw_read32(ph & ~3u);
        hw_write32(ph & ~3u, (old & ~(0xffu << sh)) | ((uint32_t)v << sh));
        return;
    }
    bad_access("write8", a);
}
void mem_w16_slow(uint32_t a, uint16_t v) {
    uint8_t *p = mem_ptr(a);
    if (p) { memcpy(p, &v, 2); return; }
    uint32_t ph = a & 0x1fffffff;
    if (is_hw(ph)) {
        uint32_t sh = (ph & 2) * 8;
        uint32_t old = hw_read32(ph & ~3u);
        hw_write32(ph & ~3u, (old & ~(0xffffu << sh)) | ((uint32_t)v << sh));
        return;
    }
    bad_access("write16", a);
}
void mem_w32_slow(uint32_t a, uint32_t v) {
    uint8_t *p = mem_ptr(a);
    if (p) { memcpy(p, &v, 4); return; }
    uint32_t ph = a & 0x1fffffff;
    if (is_hw(ph)) { hw_write32(ph, v); return; }
    bad_access("write32", a);
}
void mem_w64_slow(uint32_t a, uint64_t v) {
    uint8_t *p = mem_ptr(a);
    if (p) { memcpy(p, &v, 8); return; }
    uint32_t ph = a & 0x1fffffff;
    if (is_hw(ph)) { hw_write64(ph, v); return; }
    bad_access("write64", a);
}
void mem_w128_slow(uint32_t a, const r128 *v) {
    uint8_t *p = mem_ptr(a);
    if (p) { memcpy(p, v, 16); return; }
    uint32_t ph = a & 0x1fffffff;
    if (is_hw(ph)) { hw_write128(ph, v); return; }
    bad_access("write128", a);
}

void gread(void *dst, uint32_t src, uint32_t n) {
    uint8_t *d = dst;
    while (n) {
        uint8_t *p = mem_ptr(src);
        uint32_t chunk = 4096 - (src & 4095);
        if (chunk > n) chunk = n;
        if (p) memcpy(d, p, chunk); else memset(d, 0, chunk);
        d += chunk; src += chunk; n -= chunk;
    }
}
void gwrite(uint32_t dst, const void *src, uint32_t n) {
    const uint8_t *s = src;
    while (n) {
        uint8_t *p = mem_ptr(dst);
        uint32_t chunk = 4096 - (dst & 4095);
        if (chunk > n) chunk = n;
        if (p) memcpy(p, s, chunk);
        s += chunk; dst += chunk; n -= chunk;
    }
}
int gstrcpy(char *dst, uint32_t src, int max) {
    int i;
    for (i = 0; i < max - 1; i++) {
        uint8_t ch = MR8(src + i);
        dst[i] = ch;
        if (!ch) return i;
    }
    dst[i] = 0;
    return i;
}
