/* Internal runtime interfaces. */
#pragma once
#include "ps2.h"
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

extern int g_log_level;
#define LOG(lvl, ...) do { if (g_log_level >= (lvl)) { fprintf(stderr, __VA_ARGS__); } } while (0)
#define WARN(...) LOG(1, __VA_ARGS__)
#define INFO(...) LOG(2, __VA_ARGS__)
#define TRACE(...) LOG(3, __VA_ARGS__)
void rt_fatal(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));

extern const char *g_game_dir;
extern pthread_mutex_t g_big;

/* guest memory helpers */
static inline uint32_t gphys(uint32_t a) {
    if (a >= 0x70000000 && a < 0x70004000) return a;  /* scratchpad: caller must special-case */
    return a & 0x1fffffff;
}
void *gptr(uint32_t a);              /* host pointer for guest RAM/SPR address (or NULL) */
static inline uint32_t gr32(uint32_t a) { return MR32(a); }
static inline void gw32(uint32_t a, uint32_t v) { MW32(a, v); }
void gread(void *dst, uint32_t src, uint32_t n);
void gwrite(uint32_t dst, const void *src, uint32_t n);
int gstrcpy(char *dst, uint32_t src, int max);

/* kernel */
void kernel_init(void);
void kernel_start_main(uint32_t entry);
Ctx *ctx_new(void);
void sched_check_timers(void);      /* fire due vblank/alarm interrupts */
void kernel_raise_dmac(int ch);
void kernel_raise_intc(int cause);
void kernel_deliver_pending(void);
uint64_t host_usec(void);
int rt_deterministic(void);   /* virtual clock (AE2_DETERMINISTIC / recording / replay) */
extern volatile int g_quit;
extern uint32_t g_frame;
extern int g_in_intr;
uint32_t kernel_gs_imr(void);

/* SIF / IOP */
void sif_init(void);
int32_t sif_set_dma(uint32_t trans, int count);
uint32_t sif_get_reg(uint32_t reg);
void sif_set_reg(uint32_t reg, uint32_t val);
void sif_poll(void);
extern uint8_t *g_iopram;            /* 2 MB */
#define IOP(a) (g_iopram + ((a) & 0x1fffff))
uint32_t iop_alloc(uint32_t size);
void iop_free(uint32_t addr);

/* IOP HLE services: return IOP address of result data */
typedef uint32_t (*RpcHandler)(uint32_t sid, int fno, uint32_t buf, int size, int *rsize_hint);
void iop_register_server(uint32_t sid, RpcHandler h, uint32_t bufsize);
void iop_register_server_buf(uint32_t sid, RpcHandler h, uint32_t buf, uint32_t bufsize);
extern int g_headless;
void iop_services_init(void);
void cdvd_init(void);
int cdvd_read_sectors(uint32_t lsn, uint32_t count, void *dst);
int cdvd_search(const char *name, uint32_t *lsn, uint32_t *size);
FILE *host_open_game_file(const char *rel, const char *mode);

/* hardware */
uint32_t hw_read32(uint32_t a);
void hw_write32(uint32_t a, uint32_t v);
uint64_t hw_read64(uint32_t a);
void hw_write64(uint32_t a, uint64_t v);
void hw_read128(uint32_t a, r128 *o);
void hw_write128(uint32_t a, const r128 *v);
void hw_init(void);
void hw_vblank(int start);

/* graphics */
void gs_init(void);
void gs_write_priv64(uint32_t a, uint64_t v);
uint64_t gs_read_priv64(uint32_t a);
void gs_set_crt(int interlace, int mode, int field);
void gif_transfer(const uint8_t *data, uint32_t qwc, int path);
void gs_vsync(void);
void vif_transfer(int unit, const uint8_t *data, uint32_t qwc);
void vu1_init(void);
void vif1_reset(void);
void vif1_submit(const uint8_t *data, uint32_t qwc);   /* VIF1 input: on the GS thread with MTVU */
uint64_t rt_fpu_enter_guest(void); void rt_fpu_leave_guest(uint64_t saved);
extern uint8_t g_vu1mem[16384], g_vu1code[16384], g_vu0code[4096];

/* host platform */
void host_init(int headless);
void host_present(const uint32_t *rgba, int w, int h);
void host_poll_events(void);
uint16_t host_pad_buttons(int port);
void host_pad_poll(void);
void gs_game_frame(void);       /* once per game frame: auto frame-skip decision */
extern uint64_t g_idle_us;
void host_game_mode(int m);
void host_pad_analog(int port, uint8_t out[4]);
