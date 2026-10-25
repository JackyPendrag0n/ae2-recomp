/* Host replacements for specific guest functions (see recomp/overrides.txt). */
#include "rt.h"

/* int write(int fd, const void *buf, int len) for the debug TTY */
void hle_tty_write(Ctx *c) {
    uint32_t buf = c->r[5].uw[0];
    int len = (int)c->r[6].uw[0];
    if (len > 0 && len < 65536 && g_log_level >= 1) {
        char tmp[4096];
        int n = len < (int)sizeof tmp ? len : (int)sizeof tmp;
        gread(tmp, buf, n);
        fwrite("[tty] ", 1, 6, stderr);
        fwrite(tmp, 1, n, stderr);
        if (n && tmp[n - 1] != '\n') fputc('\n', stderr);
    }
    c->r[2].sd[0] = len;
}

/* Movie playback start: if the MOVIE folder isn't present, report the movie as
 * already finished (state 3) so the game's normal post-movie flow runs. */
void f_001A5DB0_orig(Ctx *c);
#define GP_MOVIE_STATE (0x535af0u - 0x388u)
static int have_movies = -1;
/* FMVs play when the MOVIE folder holds real movie data (a PSS file starts with
 * an MPEG pack header, 00 00 01 BA); AE2_MOVIES=0 skips them anyway. */
static int movies_present(void) {
    if (have_movies < 0) {
        const char *e = getenv("AE2_MOVIES");
        unsigned char hd[4] = {0};
        FILE *f = host_open_game_file("MOVIE/PUB.PSS", "rb");
        if (f) { if (fread(hd, 1, 4, f) != 4) memset(hd, 0, 4); fclose(f); }
        int real = hd[0] == 0 && hd[1] == 0 && hd[2] == 1 && hd[3] == 0xba;
        have_movies = real && !(e && *e == '0');
        INFO("[hle] FMVs %s\n", have_movies ? "enabled" : f ? (real ? "disabled (AE2_MOVIES=0)" : "skipped (MOVIE/PUB.PSS has no movie data)") : "skipped (no MOVIE folder)");
    }
    return have_movies;
}
static uint32_t mv_last_ret;      /* FMV texture page (see hle_mvconv; 0 = unknown) */
void hle_movie_start(Ctx *c) {
    mv_last_ret = 0;
    if (movies_present()) { f_001A5DB0_orig(c); return; }
    MW32(GP_MOVIE_STATE, 3);
    c->r[2].sd[0] = 0;
}

/* Per-movie companion bank (MOVIE/MOVIE_x.BIN: subtitles/voice for the FMV).
 * Without the MOVIE folder there is nothing to load; parsing an all-zero
 * file sends the archive reader off into garbage, so report "no bank". */
void f_001A6B78_orig(Ctx *c);
void hle_movie_bank_load(Ctx *c) {
    if (movies_present()) { f_001A6B78_orig(c); return; }
    MW32(0x535af0u - 0x350u, 0);
}

/* ---- Controller (libdbc) HLE.
 * Data block as the game parses it: [0..1] buttons (active low, standard PS2
 * bit order), [2..5] RX RY LX LY, [6..17] pressure values. */
static void pad_block(int port, uint8_t *d) {
    uint16_t b = host_pad_buttons(port);   /* active high, PS2 bit order */
    uint8_t an[4];
    host_pad_analog(port, an);             /* RX RY LX LY */
    if (port == 1 && getenv("AE2_P2TEST")) {   /* testing: player 2's left stick "lx,ly" (-127..127) */
        int x = 0, y = 0; sscanf(getenv("AE2_P2TEST"), "%d,%d", &x, &y); an[2] = (uint8_t)(0x80 + x); an[3] = (uint8_t)(0x80 + y); }
    memset(d, 0, 20);
    d[0] = (uint8_t)~(b & 0xff);
    d[1] = (uint8_t)~(b >> 8);
    d[2] = an[0]; d[3] = an[1]; d[4] = an[2]; d[5] = an[3];
    /* pressure: right, left, up, down, triangle, circle, cross, square, L1, R1, L2, R2 */
    static const int pbit[12] = {5, 7, 4, 6, 12, 13, 14, 15, 10, 11, 8, 9};
    for (int i = 0; i < 12; i++) d[6 + i] = (b >> pbit[i]) & 1 ? 0xff : 0;
}
/* the Graphics options page takes some buttons for itself (gfx_menu.c) */
static void pad_menu_mask(uint8_t *d) {
    uint16_t gfx_menu_pad_hide(void); int gfx_menu_hide_stick_x(void);
    uint16_t hide = gfx_menu_pad_hide();
    if (hide) { d[0] |= (uint8_t)(hide & 0xff); d[1] |= (uint8_t)(hide >> 8);   /* active low */
        static const int pbit[12] = {5, 7, 4, 6, 12, 13, 14, 15, 10, 11, 8, 9};
        for (int i = 0; i < 12; i++) if (hide & (1u << pbit[i])) d[6 + i] = 0; }
    if (gfx_menu_hide_stick_x()) { d[2] = 0x80; d[4] = 0x80; }
}
void hle_dbc_state(Ctx *c) {
    int sock = (int)c->r[4].uw[0];
    { static int n; if ((n++ % 50) == 0) INFO("[pad] state poll #%d sock=%d frame=%u\n", n, sock, g_frame); }
    c->r[2].sd[0] = (sock == 0) ? 1 : 0;
}
void hle_dbc_read(Ctx *c) {
    int sock = (int)c->r[4].uw[0];
    uint32_t buf = c->r[5].uw[0];
    if (sock != 0) { c->r[2].sd[0] = -1; return; }
    uint8_t d[20];
    extern int g_p2_pad_source, g_pad_swap;
    if (g_p2_pad_source ^ g_pad_swap) pad_block(1, d);   /* player 2's parse (p2.c): the second pad, no new poll */
    else {
        extern int g_ghost; if (!g_ghost) host_pad_poll();   /* the second-view pass reuses this frame's poll */
        extern volatile int g_p2_paused;
        pad_block(g_p2_paused ? 1 : 0, d);   /* player 2's pause menu reads the second pad */
        pad_menu_mask(d);
    }
    gwrite(buf, d, 18);
    c->r[2].sd[0] = 18;
}
void hle_dbc_element(Ctx *c) {
    int sock = (int)c->r[4].uw[0];
    int idx = (int)c->r[6].uw[0];
    if (sock != 0) { c->r[2].sd[0] = -1; return; }
    uint8_t d[20];
    extern int g_p2_pad_source, g_pad_swap;
    extern volatile int g_p2_paused;
    if (g_p2_pad_source ^ g_pad_swap) pad_block(1, d);
    else { pad_block(g_p2_paused ? 1 : 0, d); pad_menu_mask(d); }
    if (idx >= 0x10 && idx <= 0x13) c->r[2].sd[0] = d[2 + (idx - 0x10)];
    else if (idx < 16) c->r[2].sd[0] = ((d[idx >> 3] >> (idx & 7)) & 1) ? 0 : 1;
    else c->r[2].sd[0] = -1;
}
void hle_dbc_send(Ctx *c) { (void)c; c->r[2].sd[0] = 1; }

void f_0019CBF8_orig(Ctx *c);
void hle_set_game_mode(Ctx *c) {
    static const char *names[] = {"title", "norma", "playmain", "retry", "gate", "result", "befnorma", "gameover", "?8", "?9", "movie", "extra", "?12"};
    uint32_t m = c->r[4].uw[0];
    int p2_mode_request(uint32_t m); if (p2_mode_request(m)) return;
    { extern uint32_t g_pad_polls; INFO("[game] mode -> %u (%s) at frame %u (game frame %u)\n", m, m < 13 ? names[m] : "?", g_frame, g_pad_polls); }
    host_game_mode((int)m);
    f_0019CBF8_orig(c);
}

/* ---- camera hooks (free camera, see debug_menu.c) */
static int camlog_on(void) {
    static long a = -2, b; if (a == -2) { const char *e = getenv("AE2_CAMLOG"); a = -1; if (e) sscanf(e, "%ld-%ld", &a, &b); }
    extern uint32_t g_pad_polls; return a >= 0 && g_pad_polls >= (uint32_t)a && g_pad_polls <= (uint32_t)b;
}
/* camera look-at from two points: a0 = eye, a1 = target (128-bit registers) */
void f_001A10E0_orig(Ctx *c);
void hle_camlook(Ctx *c) {
    if (camlog_on()) { extern uint32_t g_pad_polls; INFO("[cam] %u camlook ra %08x eye %.1f %.1f %.1f target %.1f %.1f %.1f\n", g_pad_polls, c->r[31].uw[0],
        c->r[4].f[0], c->r[4].f[1], c->r[4].f[2], c->r[5].f[0], c->r[5].f[1], c->r[5].f[2]); }
    /* 0x270830: the gameplay camera (other callers: cut-scenes, menus) */
    if (c->r[31].uw[0] == 0x270830u) { int debug_camera_override(float eye[3], float target[3]); debug_camera_override(c->r[4].f, c->r[5].f);
        void p2_camera_adjust(float eye[3], float target[3]); p2_camera_adjust(c->r[4].f, c->r[5].f); }
    { extern uint32_t g_pad_polls; static long dp = -2; if (dp == -2) dp = getenv("AE2_CAMDIFF") ? atol(getenv("AE2_CAMDIFF")) : -1;
      if (dp >= 0 && g_pad_polls == (uint32_t)dp && c->r[31].uw[0] == 0x270830u) { void p2_diff_begin(void); void p2_diff_end(const char *, uint32_t);
          p2_diff_begin(); f_001A10E0_orig(c); p2_diff_end("camlook", c->r[29].uw[0]); return; } }
    f_001A10E0_orig(c);
}
/* character mover (a0 = actor): moves, applies gravity and collision, writes the position */
void f_0029B5C0_orig(Ctx *c);
void hle_mover(Ctx *c) {
    uint32_t actor = c->r[4].uw[0];
    if (camlog_on()) { extern uint32_t g_pad_polls; INFO("[mover] %u actor %08x ra %08x\n", g_pad_polls, actor, c->r[31].uw[0]); }
    void monkey_move_end(uint32_t);
    f_0029B5C0_orig(c);
    monkey_move_end(actor);
    void debug_noclip_after_move(uint32_t actor); debug_noclip_after_move(actor);
}

/* FMV frame conversion (0x1a8008).  Every vsync the movie display code calls
 * this to turn the oldest decoded frame (libmpeg output: 384-byte YCbCr 4:2:0
 * macroblocks, stored column by column) into an RGB texture in VRAM.  It
 * returns the texture page, which the caller then copies to the screen.
 * The original does this with a VU1 microprogram drawing 262144 one-pixel
 * points, every vsync, even when the frame has not changed.  Here the frame is
 * converted on the host (BT.601, chroma interpolated) and uploaded with one
 * image transfer, and only when a new frame arrives.  AE2_MVHLE=0 restores the
 * original path. */
void f_001A8008_orig(Ctx *c);
static uint8_t clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v; }
static void mv_convert(const uint8_t *mb, int w, int h, uint32_t *out /* 512 x h */) {
    int mbw = (w + 15) / 16, mbh = (h + 15) / 16;
    int cw = mbw * 8, ch = mbh * 8;
    static uint8_t Y[512 * 512], U[256 * 256], V[256 * 256];
    for (int mx = 0; mx < mbw; mx++) for (int my = 0; my < mbh; my++) {
        const uint8_t *m = mb + (size_t)(mx * mbh + my) * 384;
        for (int y = 0; y < 16; y++) memcpy(Y + (my * 16 + y) * 512 + mx * 16, m + y * 16, 16);
        for (int y = 0; y < 8; y++) {
            memcpy(U + (my * 8 + y) * 256 + mx * 8, m + 256 + y * 8, 8);
            memcpy(V + (my * 8 + y) * 256 + mx * 8, m + 320 + y * 8, 8);
        }
    }
    for (int y = 0; y < mbh * 16; y++) {
        /* MPEG-2 4:2:0: chroma rows sit between luma rows, columns on even luma */
        int cy0, cy1, wy1;   /* wy1 = weight of the lower chroma row, out of 4 */
        if (y & 1) { cy0 = y >> 1; cy1 = cy0 + 1; wy1 = 1; } else { cy0 = (y >> 1) - 1; cy1 = y >> 1; wy1 = 3; }
        if (cy0 < 0) cy0 = 0;
        if (cy1 >= ch) cy1 = ch - 1;
        const uint8_t *u0 = U + cy0 * 256, *u1 = U + cy1 * 256, *v0 = V + cy0 * 256, *v1 = V + cy1 * 256;
        const uint8_t *yr = Y + y * 512;
        uint32_t *o = out + y * 512;
        for (int x = 0; x < mbw * 16; x++) {
            int cx0 = x >> 1, cx1 = (x & 1) && cx0 + 1 < cw ? cx0 + 1 : cx0;
            int uu = ((u0[cx0] + u0[cx1]) * (4 - wy1) + (u1[cx0] + u1[cx1]) * wy1 + 4) >> 3;
            int vv = ((v0[cx0] + v0[cx1]) * (4 - wy1) + (v1[cx0] + v1[cx1]) * wy1 + 4) >> 3;
            int yy = (yr[x] - 16) * 76309, cb = uu - 128, cr = vv - 128;   /* 16.16 fixed point */
            int r = (yy + 104597 * cr + 32768) >> 16;
            int g = (yy - 25675 * cb - 53279 * cr + 32768) >> 16;
            int bl = (yy + 132201 * cb + 32768) >> 16;
            o[x] = clamp255(r) | (uint32_t)clamp255(g) << 8 | (uint32_t)clamp255(bl) << 16;
        }
    }
}
void gif_transfer(const uint8_t *data, uint32_t qwc, int path);
static void mv_upload(uint32_t bp, const uint32_t *img, int w, int h) {
    /* A+D setup (BITBLTBUF, TRXPOS, TRXREG, TRXDIR) then IMAGE data, 32 rows per tag */
    static uint64_t pkt[2 * (5 + 512 / 32 * (1 + 32 * 512 / 4))];
    int n = 0;
    pkt[n++] = 4 | (1ull << 60); pkt[n++] = 0xe;                  /* GIFtag: NLOOP 4, PACKED, A+D */
    pkt[n++] = ((uint64_t)bp << 32) | ((uint64_t)(512 / 64) << 48); pkt[n++] = 0x50;   /* PSMCT32 */
    pkt[n++] = 0; pkt[n++] = 0x51;
    pkt[n++] = (uint64_t)w | ((uint64_t)h << 32); pkt[n++] = 0x52;
    pkt[n++] = 0; pkt[n++] = 0x53;
    for (int y = 0; y < h; y += 32) {
        int rows = h - y < 32 ? h - y : 32;
        uint32_t qw = (uint32_t)(rows * w / 4);
        pkt[n++] = qw | (2ull << 58) | ((uint64_t)(y + rows >= h) << 15); pkt[n++] = 0;   /* IMAGE */
        for (int r = 0; r < rows; r++) { memcpy(&pkt[n], img + (y + r) * 512, (size_t)w * 4); n += w / 2; }
    }
    gif_transfer((const uint8_t *)pkt, (uint32_t)(n / 2), 3);
}
void hle_mvconv(Ctx *c) {
    static uint32_t last_frame, last_vbl;
    static uint64_t last_hash;
    uint32_t last_ret = mv_last_ret;
    extern uint32_t g_fmv_vsync; g_fmv_vsync = g_frame ? g_frame : 1;
    static int use_hle = -1, lg = -1, nconv;
    if (use_hle < 0) { const char *e = getenv("AE2_MVHLE"); use_hle = !(e && *e == '0'); lg = getenv("AE2_MVLOG") != NULL; }
    const uint32_t q = 0x3e08d0u;           /* frame ring: base 0, write 8, count 0xc, entry size 0x18, entries 0x1c */
    uint32_t base = MR32(q), wr = MR32(q + 8), cnt = MR32(q + 0xc);
    uint32_t sz = MR32(q + 0x18), n = MR32(q + 0x1c);
    if (cnt == 0 || n == 0 || sz == 0) { f_001A8008_orig(c); last_frame = 0; return; }
    uint32_t ent = base + sz * ((wr - cnt + n) % n);    /* the oldest queued frame */
    uint32_t fa = ent & (EE_RAM_MAX - 1);
    /* picture size (the caller clamps to 512 x 512) */
    uint32_t a0 = c->r[4].uw[0], ra = c->r[31].uw[0];
    c->r[4].sd[0] = -1; rt_call(c, 0x1a5588u); int w = (int)c->r[2].sw[0];
    c->r[4].sd[0] = -1; rt_call(c, 0x1a55a8u); int h = (int)c->r[2].sw[0];
    c->r[4].uw[0] = a0; c->r[31].uw[0] = ra;
    uint32_t fsz = (uint32_t)(((w + 15) / 16) * ((h + 15) / 16) * 384);
    int ok = w > 0 && h > 0 && w <= 512 && h <= 512 && (w & 15) == 0 && fsz <= sz && fa + fsz <= g_ram_size;
    uint64_t hs = 1469598103934665603ull ^ (uint64_t)w << 32 ^ (uint64_t)h;
    if (ok) { const uint64_t *p = (const uint64_t *)(g_ram + fa); for (uint32_t i = 0; i < fsz / 8; i += 3) hs = (hs ^ p[i]) * 1099511628211ull; }
    if (ok && last_ret && ent == last_frame && hs == last_hash) { c->r[2].sd[0] = (int32_t)last_ret; return; }
    if (!use_hle || !ok || !last_ret) {
        /* first frame of a movie (learns the texture page) or unusual sizes: the original */
        f_001A8008_orig(c);
        last_ret = c->r[2].uw[0];
        if (!ok) last_ret = 0;
    } else {
        static uint32_t img[512 * 512];
        mv_convert(g_ram + fa, w, h, img);
        mv_upload(last_ret * 32, img, w, (h + 15) & ~15);
        c->r[2].sd[0] = (int32_t)last_ret;
    }
    if (lg && (++nconv % 25 == 0 || nconv < 4)) INFO("[movie] %d frames (%dx%d), page %x, vsync %u, %llu ms\n", nconv, w, h, last_ret, g_frame, (unsigned long long)(host_usec() / 1000));
    last_frame = ent; last_hash = hs; last_vbl = g_frame; mv_last_ret = last_ret;
}

/* libmpeg: wait for the current IPU command (0x375a78, a0 = mpeg context).
 * The original polls IPU_CMD/IPU_CTRL and, every 5000 polls, calls the
 * application's callback (which feeds more bitstream to the IPU).  Our IPU
 * decodes as soon as it is polled, so a command still busy after a poll is
 * waiting for data: call the callback straight away instead of spinning
 * 5000 times through slow register reads. */
uint64_t ipu_read64(uint32_t a);
void hle_mpeg_wait(Ctx *c) {
    uint32_t ctx = c->r[4].uw[0], ra = c->r[31].uw[0];
    for (;;) {
        uint64_t v = ipu_read64(0x10002000u);
        if (!(v >> 63) || (ipu_read64(0x10002010u) & 0x4000)) {
            c->r[2].ud[0] = v; c->r[31].uw[0] = ra; return;
        }
        c->r[4].uw[0] = MR32(ctx + 0x858);
        c->r[31].uw[0] = 0x375aecu;
        rt_call(c, 0x371958u);
    }
}

/* ---- Camera projection (struct at 0x3de5c0: +0 far plane, +4 near, +8/+0xc
 * tangents of the half field of view).  0x1a0d28 builds the projection matrix
 * from it; Options > Graphics scales the field of view for that build only, so
 * the game's own values are never changed.  (Scaling the far plane was tried for
 * a draw-distance option: the game already draws its levels to the horizon, and
 * it made no visible difference.) */
float g_gfx_fov = 1.0f;         /* field of view: half-angle tangents x this */
void f_001A0D28_orig(Ctx *c);
void hle_proj_build(Ctx *c) {
    const uint32_t p = 0x3de5c0u;
    if (g_gfx_fov == 1.0f) { f_001A0D28_orig(c); return; }
    uint32_t r8 = MR32(p + 8), rc = MR32(p + 12);
    float a, b; memcpy(&a, &r8, 4); memcpy(&b, &rc, 4);
    a *= g_gfx_fov; b *= g_gfx_fov;
    uint32_t u; memcpy(&u, &a, 4); MW32(p + 8, u); memcpy(&u, &b, 4); MW32(p + 12, u);
    f_001A0D28_orig(c);
    MW32(p + 8, r8); MW32(p + 12, rc);
}

/* 0x1a4350: the game's memory set-up. It takes one block for all its heaps from the C
 * library's malloc, sized from a constant: the end of the program (0x61aa58) up to
 * 0x1fa0000, where crt0 put the main thread's stack (the top 384 KB of the 32 MB). Then
 * the system heap (8.25 MB) comes out of it, and later the main heap gets the rest
 * (0x1937d0). This is the same code with the top taken from the RAM size instead, so
 * with more RAM (AE2_RAM_MB) every byte above 32 MB goes to the main heap, where stages,
 * characters and their textures are loaded. */
void f_0039E480(Ctx *c); void f_00393650(Ctx *c); void f_001A4518(Ctx *c); void f_001A4430(Ctx *c);
void hle_mem_init(Ctx *c) {
    const uint32_t gp = c->r[28].uw[0], end = 0x61aa58u;
    uint32_t top = g_ram_size - 0x60000u;   /* the bottom of the main thread's stack */
    r128 s0 = c->r[16];
    f_0039E480(c);
    uint32_t size = top - end;
    MW32(gp - 1004, end); MW32(gp - 980, size);
    c->r[4].sd[0] = 64; c->r[5].sd[0] = (int32_t)(size - 0x10000u);
    f_00393650(c);                           /* memalign(64, size - 64 KB) */
    uint32_t p = c->r[2].uw[0];
    uint32_t avail = end + size - 0x10000u - p;
    MW32(gp - 1008, p); MW32(gp - 1004, p); MW32(gp - 980, avail);
    MW32(p + 12, avail - 64); MW32(p + 8, avail);
    MW32(p, 0); MW32(p + 4, 0); MW32(p + 16, 0); MW32(p + 20, 0);
    c->r[2].sd[0] = (int32_t)p; c->r[3].sd[0] = (int32_t)avail; c->r[4].sd[0] = (int32_t)p; c->r[5].sd[0] = (int32_t)(avail - 64);
    f_001A4518(c);
    c->r[4].sd[0] = 0x840000; c->r[5].sd[0] = 0x530000 + 22304;
    f_001A4430(c);                           /* the system heap */
    MW32(gp - 988, 0); MW32(gp - 996, c->r[2].uw[0]);
    c->r[16] = s0;
    INFO("[mem] game heap 0x%x..0x%x (%u MB)\n", p, p + avail, avail >> 20);
}
