/* Host platform: SDL2 window, presentation, keyboard + gamepad input.
 * Also supports headless runs (frame dumps + scripted input) for testing. */
#include "rt.h"
#include "overlay.h"
#include "font_data.h"
#include "host_gl.h"
#include <pthread.h>
#include <time.h>
#include <SDL2/SDL.h>
#include <math.h>

static SDL_Window *win;
static SDL_Renderer *ren;
static SDL_Texture *tex;
static int tex_w, tex_h;
static int headless;
static SDL_GameController *pads[2];
static int dump_every;
extern uint32_t g_pad_polls;
static const char *dump_dir;
static int fullscreen;
static int use_gl;
int host_use_gl(void) { return use_gl; }
int g_display_h;   /* desktop height, for the default GPU scale */

/* PS2 button bits (active high here): byte0 SELECT L3 R3 START UP RIGHT DOWN LEFT, byte1 L2 R2 L1 R1 TRI CIR CRO SQU */
enum { B_SELECT = 1 << 0, B_L3 = 1 << 1, B_R3 = 1 << 2, B_START = 1 << 3, B_UP = 1 << 4, B_RIGHT = 1 << 5, B_DOWN = 1 << 6, B_LEFT = 1 << 7,
       B_L2 = 1 << 8, B_R2 = 1 << 9, B_L1 = 1 << 10, B_R1 = 1 << 11, B_TRI = 1 << 12, B_CIR = 1 << 13, B_CRO = 1 << 14, B_SQU = 1 << 15 };

/* ---------------------------------------------------------------- scripted input (testing) */
/* AE2_INPUT="300:start;310:-start;400:cross+lup;..."  (frame:buttons, '-' releases, '+' joins)
 * Time can also be given as mM+N: N game frames (pad polls) after the game
 * enters mode M, which is independent of how fast the host runs:
 *   AE2_INPUT="m1+40:start;m1+44:-start;m1+90:cross;m1+94:-cross"
 * An event written "2@..." is for the second controller (player 2). */
typedef struct { uint32_t frame; int mode, port; uint16_t press, release; int8_t lx, ly, rx, ry; int setstick, setrstick; } ScriptEv;
static int cur_mode = -1;
static uint32_t mode_polls;
void host_game_mode(int m) { cur_mode = m; mode_polls = 0; }
static ScriptEv script[4096];
static int nscript, script_pos;
static uint16_t script_buttons[2];
static int8_t script_lx[2], script_ly[2], script_rx[2], script_ry[2];

static uint16_t name_to_bit(const char *n) {
    static const struct { const char *n; uint16_t b; } t[] = {
        {"select", B_SELECT}, {"l3", B_L3}, {"r3", B_R3}, {"start", B_START}, {"up", B_UP}, {"right", B_RIGHT},
        {"down", B_DOWN}, {"left", B_LEFT}, {"l2", B_L2}, {"r2", B_R2}, {"l1", B_L1}, {"r1", B_R1},
        {"triangle", B_TRI}, {"circle", B_CIR}, {"cross", B_CRO}, {"square", B_SQU}};
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) if (!strcmp(n, t[i].n)) return t[i].b;
    return 0;
}
static void parse_script(const char *s) {
    static char buf[65536]; snprintf(buf, sizeof buf, "%s", s);
    for (char *tok = strtok(buf, ";"); tok && nscript < 4096; tok = strtok(NULL, ";")) {
        char *colon = strchr(tok, ':'); if (!colon) continue;
        ScriptEv *e = &script[nscript++];
        memset(e, 0, sizeof *e);
        e->mode = -1;
        if (tok[0] == '2' && tok[1] == '@') { e->port = 1; tok += 2; }
        if (tok[0] == 'm') { e->mode = atoi(tok + 1); char *pl = strchr(tok, '+'); e->frame = pl && pl < colon ? (uint32_t)atoi(pl + 1) : 0; }
        else e->frame = (uint32_t)atoi(tok);
        char *p = colon + 1;
        for (char *b = strtok_r(p, "+", &p); b; b = strtok_r(NULL, "+", &p)) {
            if (!strncmp(b, "stick=", 6)) { int x, y; if (sscanf(b + 6, "%d,%d", &x, &y) == 2) { e->lx = x; e->ly = y; e->setstick = 1; } continue; }
            if (!strncmp(b, "rstick=", 7)) { int x, y; if (sscanf(b + 7, "%d,%d", &x, &y) == 2) { e->rx = x; e->ry = y; e->setrstick = 1; } continue; }
            if (b[0] == '-') e->release |= name_to_bit(b + 1); else e->press |= name_to_bit(b);
        }
    }
}
static void run_script(void) {
    while (script_pos < nscript) {
        ScriptEv *e = &script[script_pos];
        if (e->mode < 0 ? e->frame > g_frame : (e->mode != cur_mode || e->frame > mode_polls)) break;
        script_pos++;
        int p = e->port;
        script_buttons[p] |= e->press; script_buttons[p] &= ~e->release;
        if (e->setstick) { script_lx[p] = e->lx; script_ly[p] = e->ly; }
        if (e->setrstick) { script_rx[p] = e->rx; script_ry[p] = e->ry; }
        { extern uint32_t g_pad_polls; INFO("[input] frame %u poll %u (mode %d +%u): pad %d buttons=%04x\n", g_frame, g_pad_polls, cur_mode, mode_polls, p + 1, script_buttons[p]); }
    }
}

/* ---------------------------------------------------------------- init / present */
void host_init(int h) {
    headless = h;
    const char *e = getenv("AE2_DUMP_EVERY");
    dump_every = e ? atoi(e) : 0;
    dump_dir = getenv("AE2_DUMP_DIR"); if (!dump_dir) dump_dir = "frames";
    if (getenv("AE2_INPUT")) parse_script(getenv("AE2_INPUT"));
    if (headless) return;
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        WARN("[host] SDL_Init failed (%s), running headless\n", SDL_GetError());
        headless = 1;
        return;
    }
    /* OpenGL 3.3 core for presentation (and the GPU renderer); SDL_Renderer as a fallback */
    if (!getenv("AE2_NOGL")) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
#ifdef __APPLE__
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);   /* macOS: core profiles only */
#endif
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
        win = SDL_CreateWindow("Ape Escape 2 (recompiled)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 960, 720,
                               SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_OPENGL);
        if (win && hostgl_init(win, 1) == 0) use_gl = 1;
        else if (win) { SDL_DestroyWindow(win); win = NULL; }
    }
    { SDL_DisplayMode m; if (SDL_GetDesktopDisplayMode(0, &m) == 0) g_display_h = m.h; }
    if (!use_gl) {
        win = SDL_CreateWindow("Ape Escape 2 (recompiled)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 960, 720,
                               SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
        ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
        if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
        INFO("[host] presenting with SDL_Renderer (no OpenGL)\n");
    }
    for (int i = 0; i < SDL_NumJoysticks() && i < 2; i++)
        if (SDL_IsGameController(i)) pads[i] = SDL_GameControllerOpen(i);
    INFO("[host] SDL window ready, %d gamepad(s)\n", (pads[0] != NULL) + (pads[1] != NULL));
}

static void dump_frame(const uint32_t *p, int w, int h) {
    static int by_polls = -1; if (by_polls < 0) by_polls = getenv("AE2_DUMP_POLLS") != NULL;   /* name by game frame */
    static long dfrom = -1, dto; if (dfrom < 0) { const char *r = getenv("AE2_DUMP_RANGE"); dfrom = 0; dto = 0x7fffffffL; if (r) sscanf(r, "%ld-%ld", &dfrom, &dto); }
    { long n = by_polls ? (long)g_pad_polls : (long)g_frame; if (n < dfrom || n > dto) return; }
    char path[512]; snprintf(path, sizeof path, "%s/f%05u.ppm", dump_dir, by_polls ? g_pad_polls : g_frame);
    FILE *f = fopen(path, "wb"); if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++) { uint8_t c[3] = {p[i] & 255, (p[i] >> 8) & 255, (p[i] >> 16) & 255}; fwrite(c, 1, 3, f); }
    fclose(f);
}

/* frame dumps for the GPU renderer: it reads the picture back itself when one is due */
int host_frame_dump_due(void) {
    if (!dump_every || (g_frame % dump_every)) return 0;
    static int by_polls = -1; if (by_polls < 0) by_polls = getenv("AE2_DUMP_POLLS") != NULL;
    static long dfrom = -1, dto; if (dfrom < 0) { const char *r = getenv("AE2_DUMP_RANGE"); dfrom = 0; dto = 0x7fffffffL; if (r) sscanf(r, "%ld-%ld", &dfrom, &dto); }
    long n = by_polls ? (long)g_pad_polls : (long)g_frame;
    return n >= dfrom && n <= dto;
}
void host_frame_dump(const uint32_t *px, int w, int h) {
    void ov_blit(uint32_t *px, int w, int h);
    uint32_t *tmp = malloc((size_t)w * h * 4);
    memcpy(tmp, px, (size_t)w * h * 4);   /* targets keep GS row 0 in GL row 0, so this is top-down */
    ov_blit(tmp, w, h);
    dump_frame(tmp, w, h);
    free(tmp);
}

/* Frames arrive from whichever guest thread runs the vsync; SDL rendering and
 * event handling stay on the main thread (host_ui_loop), which picks up the
 * most recent frame. */
static pthread_mutex_t pres_m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pres_cv = PTHREAD_COND_INITIALIZER;
static uint32_t *pres_buf; static int pres_w, pres_h, pres_cap, pres_new;
static int pres_hw; static unsigned pres_hw_tex; static float pres_hw_u, pres_hw_v; static void *pres_hw_fence;
/* a frame from the GPU renderer (GS thread): texture in the shared context + fence */
void host_present_hw(unsigned tex, float umax, float vmax, void *fence) {
    run_script();
    if (headless) return;
    pthread_mutex_lock(&pres_m);
    pres_hw = 1; pres_hw_tex = tex; pres_hw_u = umax; pres_hw_v = vmax; pres_hw_fence = fence; pres_new = 1;
    pthread_cond_signal(&pres_cv);
    pthread_mutex_unlock(&pres_m);
}

void host_present(const uint32_t *p, int w, int h) {
    run_script();
    if (dump_every && (g_frame % dump_every) == 0) {
        void ov_blit(uint32_t *px, int w, int h);
        static uint32_t *tmp; static int cap;
        if (w * h > cap) { free(tmp); tmp = malloc((size_t)w * h * 4); cap = w * h; }
        memcpy(tmp, p, (size_t)w * h * 4); ov_blit(tmp, w, h);
        dump_frame(tmp, w, h);
    }
    if (headless) return;
    pthread_mutex_lock(&pres_m);
    if (w * h > pres_cap) { free(pres_buf); pres_buf = malloc((size_t)w * h * 4); pres_cap = w * h; }
    memcpy(pres_buf, p, (size_t)w * h * 4); pres_w = w; pres_h = h; pres_new = 1; pres_hw = 0;
    pthread_cond_signal(&pres_cv);
    pthread_mutex_unlock(&pres_m);
}

/* ---------------------------------------------------------------- overlay (SDL_Renderer backend) */
static SDL_Texture *font_tex;
static void draw_overlay(int ww, int wh) {
    static OvList l; l.n = 0;
    debug_menu_draw(&l);
    if (!l.n) return;
    if (!font_tex) {
        static uint32_t px[FONT_N * FONT_CW * FONT_CH];
        for (int g = 0; g < FONT_N; g++) for (int y = 0; y < FONT_CH; y++) for (int x = 0; x < FONT_CW; x++)
            px[y * FONT_N * FONT_CW + g * FONT_CW + x] = 0x00ffffffu | ((uint32_t)font_alpha[g][y * FONT_CW + x] << 24);
        font_tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, FONT_N * FONT_CW, FONT_CH);
        SDL_UpdateTexture(font_tex, NULL, px, FONT_N * FONT_CW * 4);
        SDL_SetTextureBlendMode(font_tex, SDL_BLENDMODE_BLEND);
    }
    float sc = (float)ww / OV_W < (float)wh / OV_H ? (float)ww / OV_W : (float)wh / OV_H;
    int ox = (int)((ww - OV_W * sc) / 2), oy = (int)((wh - OV_H * sc) / 2);
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    for (int i = 0; i < l.n; i++) {
        OvItem *it = &l.it[i];
        SDL_Rect d = {ox + (int)(it->x * sc), oy + (int)(it->y * sc), (int)(it->w * sc + 0.999f), (int)(it->h * sc + 0.999f)};
        uint8_t r = it->rgba & 255, g = (it->rgba >> 8) & 255, b = (it->rgba >> 16) & 255, a = it->rgba >> 24;
        if (it->glyph < 0) { SDL_SetRenderDrawColor(ren, r, g, b, a); SDL_RenderFillRect(ren, &d); continue; }
        SDL_Rect src = {it->glyph * FONT_CW, 0, FONT_CW, FONT_CH};
        SDL_SetTextureColorMod(font_tex, r, g, b); SDL_SetTextureAlphaMod(font_tex, a);
        SDL_RenderCopy(ren, font_tex, &src, &d);
    }
}
static void count_draw_fps(void) {
    static uint64_t t0; static uint32_t frames, polls0;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now = (uint64_t)ts.tv_sec * 1000000ull + ts.tv_nsec / 1000;   /* wall clock, also in deterministic mode */
    frames++;
    if (!t0) { t0 = now; polls0 = g_pad_polls; return; }
    if (now - t0 >= 1000000) {
        g_fps_draw = frames * 1e6f / (float)(now - t0);
        g_fps_game = (g_pad_polls - polls0) * 1e6f / (float)(now - t0);
        t0 = now; frames = 0; polls0 = g_pad_polls;
    }
}

static void present_now(void) {
    pthread_mutex_lock(&pres_m);
    if (!pres_new) { pthread_mutex_unlock(&pres_m); return; }
    int w = pres_w, h = pres_h;
    if (use_gl) {
        if (pres_hw) { unsigned t = pres_hw_tex; float um = pres_hw_u, vm = pres_hw_v; void *f = pres_hw_fence; pres_new = 0; pthread_mutex_unlock(&pres_m); hostgl_present_hw(t, um, vm, f); }
        else { hostgl_present_sw(pres_buf, w, h); pres_new = 0; pthread_mutex_unlock(&pres_m); }
        count_draw_fps();
        return;
    }
    if (!tex || tex_w != w || tex_h != h) {
        if (tex) SDL_DestroyTexture(tex);
        tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, w, h);
        tex_w = w; tex_h = h;
    }
    {   /* brightness (Options > Graphics) */
        extern float g_gfx_gamma; static float lut_g; static uint8_t lut[256];
        if (g_gfx_gamma != 1.0f) {
            if (lut_g != g_gfx_gamma) { lut_g = g_gfx_gamma; for (int i = 0; i < 256; i++) lut[i] = (uint8_t)(powf(i / 255.0f, lut_g) * 255.0f + 0.5f); }
            uint8_t *b = (uint8_t *)pres_buf;
            for (int i = 0; i < w * h; i++) { b[4 * i] = lut[b[4 * i]]; b[4 * i + 1] = lut[b[4 * i + 1]]; b[4 * i + 2] = lut[b[4 * i + 2]]; }
        }
    }
    SDL_UpdateTexture(tex, NULL, pres_buf, w * 4);
    pres_new = 0;
    pthread_mutex_unlock(&pres_m);
    int ww, wh; SDL_GetRendererOutputSize(ren, &ww, &wh);
    /* PS2 output is 4:3 (or 16:9 in the game's widescreen mode) regardless of framebuffer size */
    int an = g_widescreen ? 16 : 4, ad = g_widescreen ? 9 : 3;
    int dw = ww, dh = ww * ad / an;
    if (dh > wh) { dh = wh; dw = wh * an / ad; }
    SDL_Rect dst = {(ww - dw) / 2, (wh - dh) / 2, dw, dh};
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    SDL_RenderCopy(ren, tex, NULL, &dst);
    draw_overlay(ww, wh);
    SDL_RenderPresent(ren);
    count_draw_fps();
}

/* Main-thread loop: window, rendering, input events. Never returns. */
void host_ui_loop(void) {
    for (;;) {
        pthread_mutex_lock(&pres_m);
        if (!pres_new) {
            struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 4000000; if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
            pthread_cond_timedwait(&pres_cv, &pres_m, &ts);
        }
        pthread_mutex_unlock(&pres_m);
        present_now();
        host_poll_events();
        if (g_quit) exit(0);
    }
}

void host_poll_events(void) {
    if (headless) return;
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
        case SDL_QUIT: g_quit = 1; exit(0);
        /* the GPU renderer's hidden "ae2 gs" window means closing this one is not the last
         * window, so SDL sends no SDL_QUIT: quit on the close itself */
        case SDL_WINDOWEVENT: if (ev.window.event == SDL_WINDOWEVENT_CLOSE) { g_quit = 1; exit(0); } break;
        case SDL_KEYDOWN:
            if (ev.key.keysym.sym == SDLK_F11 || (ev.key.keysym.sym == SDLK_RETURN && (ev.key.keysym.mod & KMOD_ALT))) {
                fullscreen ^= 1; SDL_SetWindowFullscreen(win, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
            }
            if (ev.key.keysym.sym == SDLK_ESCAPE && (ev.key.keysym.mod & KMOD_SHIFT)) { g_quit = 1; exit(0); }
            if (ev.key.keysym.sym == SDLK_F1) debug_menu_key(DMK_TOGGLE);
            else if (ev.key.keysym.sym == SDLK_F2) debug_menu_key(DMK_FREECAM);
            else if (ev.key.keysym.sym == SDLK_F3) debug_menu_key(DMK_NOCLIP);
            else if (ev.key.keysym.sym == SDLK_F4) debug_menu_key(DMK_CONTROL);
            else if (debug_menu_is_open()) {
                switch (ev.key.keysym.sym) {
                case SDLK_UP: debug_menu_key(DMK_UP); break;
                case SDLK_DOWN: debug_menu_key(DMK_DOWN); break;
                case SDLK_LEFT: debug_menu_key(DMK_LEFT); break;
                case SDLK_RIGHT: debug_menu_key(DMK_RIGHT); break;
                case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE: debug_menu_key(DMK_OK); break;
                case SDLK_ESCAPE: case SDLK_BACKSPACE: debug_menu_key(DMK_BACK); break;
                }
            }
            break;
        case SDL_CONTROLLERBUTTONDOWN: {
            SDL_GameController *g = SDL_GameControllerFromInstanceID(ev.cbutton.which);
            int b = ev.cbutton.button;
            if ((b == SDL_CONTROLLER_BUTTON_START && g && SDL_GameControllerGetButton(g, SDL_CONTROLLER_BUTTON_BACK)) ||
                (b == SDL_CONTROLLER_BUTTON_BACK && g && SDL_GameControllerGetButton(g, SDL_CONTROLLER_BUTTON_START))) { debug_menu_key(DMK_TOGGLE); break; }
            if (!debug_menu_is_open()) break;
            if (b == SDL_CONTROLLER_BUTTON_DPAD_UP) debug_menu_key(DMK_UP);
            else if (b == SDL_CONTROLLER_BUTTON_DPAD_DOWN) debug_menu_key(DMK_DOWN);
            else if (b == SDL_CONTROLLER_BUTTON_DPAD_LEFT) debug_menu_key(DMK_LEFT);
            else if (b == SDL_CONTROLLER_BUTTON_DPAD_RIGHT) debug_menu_key(DMK_RIGHT);
            else if (b == SDL_CONTROLLER_BUTTON_A) debug_menu_key(DMK_OK);
            else if (b == SDL_CONTROLLER_BUTTON_B) debug_menu_key(DMK_BACK);
            break;
        }
        case SDL_CONTROLLERDEVICEADDED: {
            /* SDL also reports the controllers already opened at start-up: opening one
             * twice made a single controller both pad 1 and pad 2 (it drove both players) */
            SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(ev.cdevice.which); int have = 0;
            for (int i = 0; i < 2; i++) if (pads[i] && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pads[i])) == id) have = 1;
            if (!have) for (int i = 0; i < 2; i++) if (!pads[i]) { pads[i] = SDL_GameControllerOpen(ev.cdevice.which); INFO("[host] gamepad %d connected\n", i + 1); break; }
            break;
        }
        case SDL_CONTROLLERDEVICEREMOVED:
            for (int i = 0; i < 2; i++)
                if (pads[i] && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pads[i])) == ev.cdevice.which) {
                    SDL_GameControllerClose(pads[i]); pads[i] = NULL;
                }
            break;
        }
    }
}

/* ---------------------------------------------------------------- input */
static int key(SDL_Scancode s) { const Uint8 *k = SDL_GetKeyboardState(NULL); return k && k[s]; }

uint32_t g_pad_polls;
void rt_ftrace_frame(uint32_t n);
/* The pad is sampled once per game frame (when the game polls it), into a snapshot
 * that every read during that frame returns. Recording, replay and the debug bot work
 * on this snapshot (debug.c). */
static uint16_t snap_buttons; static uint8_t snap_an[4];
static uint16_t live_buttons(int port);
static void live_analog(int port, uint8_t out[4]);
void debug_pad_frame(uint32_t poll, uint16_t *buttons, uint8_t an[4]);
void host_pad_poll(void) {
    mode_polls++; g_pad_polls++; run_script(); gs_game_frame(); rt_ftrace_frame(g_pad_polls);
    uint16_t b = live_buttons(0); uint8_t an[4]; live_analog(0, an);
    debug_pad_frame(g_pad_polls, &b, an);
    snap_buttons = b; memcpy(snap_an, an, 4);
}
/* With player 2 in a stage and one controller, the keyboard is player 1 and the
 * controller player 2; with two controllers, each player has their own. The debug
 * setting "Controls" (F4, g_control_mode) can instead send every device to player 1 or
 * to player 2, for playing either brother alone. Without player 2, everything is player 1. */
static int solo_port(void) {   /* -1: one player per device; else the port everything drives */
    int p2_active(void);
    if (!p2_active()) return 0;
    return g_control_mode == 1 ? 0 : g_control_mode == 2 ? 1 : -1;
}
static int kbd_port(void) { int s = solo_port(); return s < 0 ? 0 : s; }
static int pads_for(int port, SDL_GameController *out[2]) {
    int s = solo_port(), n = 0;
    port &= 1;
    if (s >= 0) { if (s == port) for (int i = 0; i < 2; i++) if (pads[i]) out[n++] = pads[i]; return n; }
    if (!pads[1]) { if (port == 1) out[n++] = pads[0]; return n; }
    out[n++] = pads[port];
    return n;
}
uint16_t host_pad_buttons(int port) { return port == 0 ? snap_buttons : live_buttons(port); }
void host_pad_analog(int port, uint8_t out[4]) { if (port == 0) memcpy(out, snap_an, 4); else live_analog(port, out); }
int host_is_headless(void) { return headless; }
static uint16_t live_buttons(int port) {
    uint16_t b = 0;
    b |= script_buttons[port & 1];
    if (headless) return b;
    if (port == kbd_port()) {
        /* keyboard layout (see README) */
        if (key(SDL_SCANCODE_UP)) b |= B_UP;
        if (key(SDL_SCANCODE_DOWN)) b |= B_DOWN;
        if (key(SDL_SCANCODE_LEFT)) b |= B_LEFT;
        if (key(SDL_SCANCODE_RIGHT)) b |= B_RIGHT;
        if (key(SDL_SCANCODE_RETURN)) b |= B_START;
        if (key(SDL_SCANCODE_BACKSPACE)) b |= B_SELECT;
        if (key(SDL_SCANCODE_SPACE) || key(SDL_SCANCODE_KP_2)) b |= B_CRO;
        if (key(SDL_SCANCODE_E) || key(SDL_SCANCODE_KP_6)) b |= B_CIR;
        if (key(SDL_SCANCODE_Q) || key(SDL_SCANCODE_KP_4)) b |= B_SQU;
        if (key(SDL_SCANCODE_F) || key(SDL_SCANCODE_KP_8)) b |= B_TRI;
        if (key(SDL_SCANCODE_1)) b |= B_L1;
        if (key(SDL_SCANCODE_2)) b |= B_L2;
        if (key(SDL_SCANCODE_LSHIFT) || key(SDL_SCANCODE_3)) b |= B_R1;
        if (key(SDL_SCANCODE_4)) b |= B_R2;
        if (key(SDL_SCANCODE_Z)) b |= B_L3;
        if (key(SDL_SCANCODE_X)) b |= B_R3;
    }
    SDL_GameController *gs[2]; int ng = pads_for(port, gs);
    for (int gi = 0; gi < ng; gi++) {
        SDL_GameController *g = gs[gi];
        static const struct { SDL_GameControllerButton s; uint16_t b; } m[] = {
            {SDL_CONTROLLER_BUTTON_BACK, B_SELECT}, {SDL_CONTROLLER_BUTTON_LEFTSTICK, B_L3}, {SDL_CONTROLLER_BUTTON_RIGHTSTICK, B_R3},
            {SDL_CONTROLLER_BUTTON_START, B_START}, {SDL_CONTROLLER_BUTTON_DPAD_UP, B_UP}, {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, B_RIGHT},
            {SDL_CONTROLLER_BUTTON_DPAD_DOWN, B_DOWN}, {SDL_CONTROLLER_BUTTON_DPAD_LEFT, B_LEFT},
            {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, B_L1}, {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, B_R1},
            {SDL_CONTROLLER_BUTTON_Y, B_TRI}, {SDL_CONTROLLER_BUTTON_B, B_CIR}, {SDL_CONTROLLER_BUTTON_A, B_CRO}, {SDL_CONTROLLER_BUTTON_X, B_SQU}};
        for (unsigned i = 0; i < sizeof m / sizeof m[0]; i++) if (SDL_GameControllerGetButton(g, m[i].s)) b |= m[i].b;
        if (SDL_GameControllerGetAxis(g, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16000) b |= B_L2;
        if (SDL_GameControllerGetAxis(g, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000) b |= B_R2;
    }
    return b;
}

static uint8_t axis8(int v) { v = (v + 32768) >> 8; return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

static void live_analog(int port, uint8_t out[4]) {   /* RX RY LX LY, 0x80 = centre */
    out[0] = out[1] = out[2] = out[3] = 0x80;
    int sp = port & 1;
    if (script_lx[sp] || script_ly[sp]) { out[2] = (uint8_t)(0x80 + script_lx[sp]); out[3] = (uint8_t)(0x80 + script_ly[sp]); }
    if (script_rx[sp] || script_ry[sp]) { out[0] = (uint8_t)(0x80 + script_rx[sp]); out[1] = (uint8_t)(0x80 + script_ry[sp]); }
    if (headless) return;
    if (port == kbd_port()) {
        if (key(SDL_SCANCODE_A)) out[2] = 0x00; if (key(SDL_SCANCODE_D)) out[2] = 0xff;
        if (key(SDL_SCANCODE_W)) out[3] = 0x00; if (key(SDL_SCANCODE_S)) out[3] = 0xff;
        if (key(SDL_SCANCODE_J)) out[0] = 0x00; if (key(SDL_SCANCODE_L)) out[0] = 0xff;
        if (key(SDL_SCANCODE_I)) out[1] = 0x00; if (key(SDL_SCANCODE_K)) out[1] = 0xff;
    }
    SDL_GameController *gs[2]; int ng = pads_for(port, gs);
    for (int gi = 0; gi < ng; gi++) {
        SDL_GameController *g = gs[gi];
        int lx = SDL_GameControllerGetAxis(g, SDL_CONTROLLER_AXIS_LEFTX), ly = SDL_GameControllerGetAxis(g, SDL_CONTROLLER_AXIS_LEFTY);
        int rx = SDL_GameControllerGetAxis(g, SDL_CONTROLLER_AXIS_RIGHTX), ry = SDL_GameControllerGetAxis(g, SDL_CONTROLLER_AXIS_RIGHTY);
        if (abs(lx) > 3000 || abs(ly) > 3000) { out[2] = axis8(lx); out[3] = axis8(ly); }
        if (abs(rx) > 3000 || abs(ry) > 3000) { out[0] = axis8(rx); out[1] = axis8(ry); }
    }
}
