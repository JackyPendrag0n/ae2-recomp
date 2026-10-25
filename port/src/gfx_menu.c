/* Native Graphics options, inside the game's own Options screen.
 *
 * The Options screen (0x23f4d0 per frame) keeps its state in a menu object:
 *   +0xb50  cursor row (1..9: Subtitles, Vibration, Sound, Background music,
 *           Sound effects, Screen ratio, Screen position, Reset settings, Back)
 *   +0xb54  row count, +0xb5c previous cursor row
 *   +0xb61..+0xb66  the values it shows (1 = left choice for the On/Off and
 *           Stereo/Mono pairs; 0..10 for the two bars; 1 = 16:9 for the ratio)
 *   +0x8f0..+0x978  sprite handles of the pair choices (see pair_show)
 * 0x23d588 moves the cursor (up/down, with its sound and the hand animation),
 * 0x23c498 handles left/right for the row under the cursor, then applies the
 * music and effects volumes; 0x23c400(menu, n) plays the menu sounds.
 *
 * "Screen position" (meaningless on a PC) becomes "Graphics". Choosing it turns
 * the same page into the Graphics page: its words are replaced in the game's own
 * text textures (gs_texpatch_set, words from menu_words.h), the game keeps drawing
 * the cursor, highlights, choices and bars, and this file handles left/right and
 * the buttons (the game sees only up/down meanwhile) and applies the settings.
 * Settings persist in ae2-settings.ini. */
#include "rt.h"
#include "menu_words.h"
#include <math.h>

void gs_texpatch_set(int tex, int slot, const uint8_t *cell);
void gs_write_priv64(uint32_t a, uint64_t v);
void gs_set_frameskip(int on);
uint16_t host_pad_buttons(int port);
void host_pad_analog(int port, uint8_t out[4]);
int host_use_gl(void);
extern int g_hw_request, g_hw_scale;
extern const char *g_hw_fail;
extern volatile int g_show_fps;
extern float g_gfx_fov;
float g_gfx_gamma = 1.0f;   /* presentation: colour^gamma */
static float gamma_of(int level) { return powf(0.88f, (float)(level - 5)); }
extern uint32_t g_vblank_us;

enum { P_SELECT = 1 << 0, P_START = 1 << 3, P_UP = 1 << 4, P_RIGHT = 1 << 5, P_DOWN = 1 << 6, P_LEFT = 1 << 7,
       P_L2 = 1 << 8, P_R2 = 1 << 9, P_L1 = 1 << 10, P_R1 = 1 << 11, P_TRIANGLE = 1 << 12, P_CIRCLE = 1 << 13,
       P_CROSS = 1 << 14, P_SQUARE = 1 << 15 };

/* ------------------------------------------------------------------ settings */
typedef struct {
    int renderer;     /* 1 GPU, 0 software, -1 not chosen (automatic) */
    int scale;        /* GPU internal resolution 1..8, 0 automatic */
    int frameskip;    /* 1 auto, 0 off */
    int show_fps;
    int bright;       /* 0..10 brightness, 5 = as the game */
    int fov;          /* 0..10: 45 + 5 * level degrees (3 = the game's 60) */
    int hz60;         /* 0 50 Hz (PAL), 1 60 Hz */
} GfxSettings;
static GfxSettings gs_cfg = {-1, 0, 1, 0, 5, 3, 0};
static const GfxSettings gs_defaults = {-1, 0, 1, 0, 5, 3, 0};

static const char *settings_path(void) {
    const char *e = getenv("AE2_SETTINGS");
    return e && *e ? e : "ae2-settings.ini";
}
static void settings_apply(void) {
    g_show_fps = gs_cfg.show_fps;
    gs_set_frameskip(gs_cfg.frameskip);
    g_gfx_gamma = gamma_of(gs_cfg.bright);
    float deg = 45.0f + 5.0f * (float)gs_cfg.fov;
    g_gfx_fov = tanf(deg * 0.5f * 3.14159265f / 180.0f) / tanf(30.0f * 3.14159265f / 180.0f);
    g_vblank_us = gs_cfg.hz60 ? 16667u : 20000u;
    extern int g_headless, g_auto_scale;
    if (gs_cfg.renderer != -2) g_hw_request = (gs_cfg.renderer < 0 ? !g_headless : gs_cfg.renderer) && host_use_gl() && !g_hw_fail;
    if (gs_cfg.scale != -1) g_hw_scale = gs_cfg.scale >= 1 && gs_cfg.scale <= 8 ? gs_cfg.scale : g_auto_scale;
}
static void settings_save(void) {
    FILE *f = fopen(settings_path(), "w");
    if (!f) { WARN("[gfx] cannot write %s\n", settings_path()); return; }
    fprintf(f, "# Ape Escape 2 port settings, written by Options > Graphics in the game.\n"
               "# Command-line options and AE2_* environment variables override these.\n");
    fprintf(f, "renderer = %s\n", gs_cfg.renderer < 0 ? "auto" : gs_cfg.renderer ? "gpu" : "software");
    if (gs_cfg.scale >= 1) fprintf(f, "resolution = %d\n", gs_cfg.scale); else fprintf(f, "resolution = auto\n");
    fprintf(f, "frame_skipping = %s\n", gs_cfg.frameskip ? "on" : "off");
    fprintf(f, "fps_counter = %s\n", gs_cfg.show_fps ? "on" : "off");
    fprintf(f, "brightness = %d       # 0-10, 5 = as the game\n", gs_cfg.bright);
    fprintf(f, "field_of_view = %d   # degrees, 45-95 (the game's is 60)\n", 45 + 5 * gs_cfg.fov);
    fprintf(f, "frame_rate = %d      # 50 (PAL) or 60 (game logic runs 20%% faster, as in the NTSC versions)\n", gs_cfg.hz60 ? 60 : 50);
    { extern volatile int g_monkey_mode; const char *monkey_mode_key(int m);
      fprintf(f, "play_as = %s   # hikaru, monkey_yellow/white/blue/black/red/lightblue/green, professor, natalie or specter (debug menu: Play as)\n", monkey_mode_key(g_monkey_mode < 0 ? 0 : g_monkey_mode)); }
    fclose(f);
}
/* the debug menu's Play as is kept with these settings */
void monkey_mode_changed(void) { settings_save(); }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
/* Called from main() before the renderer is chosen.  Returns bit 0 if the file
 * chose the renderer, bit 1 if it chose the resolution; choices given on the
 * command line or in the environment (*_set) win for this run. */
int gfx_settings_load(int renderer_set, int scale_set) {
    FILE *f = fopen(settings_path(), "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof line, f)) {
            char key[64], val[64];
            if (line[0] == '#' || sscanf(line, " %63[a-z_] = %63s", key, val) != 2) continue;
            int on = !strcmp(val, "on") || !strcmp(val, "1") || !strcmp(val, "yes");
            if (!strcmp(key, "renderer")) gs_cfg.renderer = !strcmp(val, "gpu") ? 1 : !strcmp(val, "software") ? 0 : -1;
            else if (!strcmp(key, "resolution")) gs_cfg.scale = clampi(atoi(val), 0, 8);
            else if (!strcmp(key, "frame_skipping")) gs_cfg.frameskip = on;
            else if (!strcmp(key, "fps_counter")) gs_cfg.show_fps = on;
            else if (!strcmp(key, "brightness")) gs_cfg.bright = clampi(atoi(val), 0, 10);
            else if (!strcmp(key, "field_of_view")) gs_cfg.fov = clampi((atoi(val) - 45 + 2) / 5, 0, 10);
            else if (!strcmp(key, "frame_rate")) gs_cfg.hz60 = atoi(val) == 60;
            else if (!strcmp(key, "play_as") && !getenv("AE2_MONKEY")) { extern volatile int g_monkey_mode; int monkey_mode_parse(const char *v); g_monkey_mode = monkey_mode_parse(val); }
        }
        fclose(f);
        INFO("[gfx] settings from %s\n", settings_path());
    }
    if (renderer_set) gs_cfg.renderer = -2;   /* -2: decided outside the file for this run */
    if (scale_set) gs_cfg.scale = -1;
    int r = (gs_cfg.renderer >= 0) | ((gs_cfg.scale >= 1) << 1);
    if (gs_cfg.renderer >= 0) g_hw_request = gs_cfg.renderer;
    if (gs_cfg.scale >= 1) g_hw_scale = gs_cfg.scale;
    g_show_fps = gs_cfg.show_fps;
    g_gfx_gamma = gamma_of(gs_cfg.bright);
    float deg = 45.0f + 5.0f * (float)gs_cfg.fov;
    g_gfx_fov = tanf(deg * 0.5f * 3.14159265f / 180.0f) / tanf(30.0f * 3.14159265f / 180.0f);
    if (getenv("AE2_FOV")) g_gfx_fov = (float)atof(getenv("AE2_FOV"));
    g_vblank_us = gs_cfg.hz60 ? 16667u : 20000u;
    if (!getenv("AE2_FRAMESKIP")) gs_set_frameskip(gs_cfg.frameskip);
    return r;
}

/* ------------------------------------------------------------------ the page */
static int gfx_mode;                 /* the Graphics page is showing */
static uint32_t opt_seen;            /* game frame (pad poll) of the last Options input call */
extern uint32_t g_pad_polls;
static int opt_cursor;               /* its cursor row then */
static uint8_t saved_vals[8];        /* +0xb60.. of the Options page while the Graphics page shows */
static uint16_t prev_pad;
static uint16_t held_mask;           /* buttons that closed the page: hidden from the game until released */
static int rep_dir, rep_t;

static const MenuWord *res_word(int s) {
    static const MenuWord *w[8] = {&mw_label_resolution_1x, &mw_label_resolution_2x, &mw_label_resolution_3x, &mw_label_resolution_4x,
                                   &mw_label_resolution_5x, &mw_label_resolution_6x, &mw_label_resolution_7x, &mw_label_resolution_8x};
    return w[clampi(s, 1, 8) - 1];
}
/* live values: the F1 debug menu can change these too */
static int cur_scale(void) { return clampi(g_hw_scale, 1, 8); }
static int cur_renderer(void) { return g_hw_request == 1; }

static void patches_normal(void) {
    for (int k = 0; k < 9; k++) gs_texpatch_set(0, k, k == 6 ? mw_label_graphics.px : NULL);
    for (int k = 0; k < 9; k++) gs_texpatch_set(1, k, NULL);
    gs_write_priv64(0x12001ff8, 1);
}
static void patches_gfx(void) {
    const MenuWord *lab[7] = {&mw_label_frame_skipping, &mw_label_fps_counter, &mw_label_renderer, &mw_label_brightness,
                              &mw_label_field_of_view, &mw_label_frame_rate, res_word(cur_scale())};
    for (int k = 0; k < 9; k++) gs_texpatch_set(0, k, k < 7 ? lab[k]->px : NULL);
    /* value rows: 0 On/Off (kept), 1 Stereo/Mono -> GPU/Software, 2 4:3/16:9 -> 50/60 Hz */
    gs_texpatch_set(1, 2, mw_value_gpu.px); gs_texpatch_set(1, 3, mw_value_software.px);
    gs_texpatch_set(1, 4, mw_value_50_hz.px); gs_texpatch_set(1, 5, mw_value_60_hz.px);
    gs_write_priv64(0x12001ff8, 1);
}

/* call a guest function with up to two arguments */
static void call2(Ctx *c, uint32_t fn, uint32_t a0, uint32_t a1) {
    uint64_t save[8]; for (int i = 0; i < 8; i++) save[i] = c->r[4 + (i & 3)].ud[0];
    uint32_t ra = c->r[31].uw[0];
    c->r[4].sd[0] = (int32_t)a0; c->r[5].sd[0] = (int32_t)a1;
    rt_call(c, fn);
    for (int i = 0; i < 4; i++) c->r[4 + i].ud[0] = save[i];
    c->r[31].uw[0] = ra;
}
static void menu_sound(Ctx *c, uint32_t o, int n) { call2(c, 0x23c400u, o, (uint32_t)n); }
/* move the cursor the way 0x23d588 does, so the draw code moves the highlight */
static void move_cursor(uint32_t o, int row) {
    uint32_t old = MR32(o + 0xb50);
    if ((int)old == row) return;
    MW32(o + 0xb5c, old); MW32(o + 0xb50, (uint32_t)row); MW16(o + 0xb58, 1);
}
/* show which of a row's two choices is selected, as the game does (0x26b7d8 on a 16-byte handle pair) */
static void pair_show(Ctx *c, uint32_t o, int row, int left) {
    static const uint16_t off[8][2] = {{0, 0}, {0x8f0, 0x920}, {0x900, 0x930}, {0x940, 0x950}, {0, 0}, {0, 0}, {0x960, 0x970}, {0, 0}};
    if (row < 1 || row > 6 || !off[row][0]) return;
    /* rows 1-3: the first handle pair is shown for value 1 (left); row 6: the second for value 1 (16:9, right) */
    uint32_t h = row == 6 ? (left ? off[row][0] : off[row][1]) : (left ? off[row][0] : off[row][1]);
    uint32_t sp = c->r[29].uw[0];
    c->r[29].ud[0] = (uint64_t)(int64_t)(int32_t)(sp - 0x40);
    uint32_t buf = sp - 0x40;
    for (int i = 0; i < 16; i += 4) MW32(buf + i, MR32(o + h + i));
    call2(c, 0x26b7d8u, buf, 0);
    c->r[29].ud[0] = (uint64_t)(int64_t)(int32_t)sp;
}
static void show_values(Ctx *c, uint32_t o) {
    gs_cfg.show_fps = g_show_fps != 0;
    int v[7] = {0, gs_cfg.frameskip, gs_cfg.show_fps, cur_renderer(), gs_cfg.bright, gs_cfg.fov, gs_cfg.hz60};
    MW8(o + 0xb61, (uint32_t)v[1]); MW8(o + 0xb62, (uint32_t)v[2]); MW8(o + 0xb63, (uint32_t)v[3]);
    MW8(o + 0xb64, (uint32_t)v[4]); MW8(o + 0xb65, (uint32_t)v[5]); MW8(o + 0xb66, (uint32_t)v[6]);
    pair_show(c, o, 1, v[1]); pair_show(c, o, 2, v[2]); pair_show(c, o, 3, v[3]); pair_show(c, o, 6, !v[6]);
}
static void enter_gfx(Ctx *c, uint32_t o) {
    held_mask = 0;
    for (int i = 0; i < 8; i++) saved_vals[i] = (uint8_t)MR8(o + 0xb60 + (uint32_t)i);
    gfx_mode = 1;
    patches_gfx();
    show_values(c, o);
    move_cursor(o, 1);
    menu_sound(c, o, 2);
    INFO("[gfx] Graphics page opened\n");
}
static void leave_gfx(Ctx *c, uint32_t o, int sound) {
    held_mask = host_pad_buttons(0);
    for (int i = 0; i < 8; i++) MW8(o + 0xb60 + (uint32_t)i, saved_vals[i]);
    pair_show(c, o, 1, saved_vals[1] == 1); pair_show(c, o, 2, saved_vals[2] == 1); pair_show(c, o, 3, saved_vals[3] == 1);
    pair_show(c, o, 6, saved_vals[6] != 1);
    gfx_mode = 0;
    patches_normal();
    move_cursor(o, 7);
    if (sound) menu_sound(c, o, 5);
    settings_save();
    INFO("[gfx] Graphics page closed\n");
}

/* the buttons the game may see this frame (pad HLE): during the Graphics page only
 * up/down (the game moves the cursor); on the Graphics row, not the confirm buttons */
uint16_t gfx_menu_pad_hide(void) {
    held_mask &= host_pad_buttons(0);
    if (g_pad_polls - opt_seen > 2) { if (gfx_mode) { gfx_mode = 0; patches_normal(); } return held_mask; }
    if (gfx_mode) return (uint16_t)~(P_UP | P_DOWN);
    if (opt_cursor == 7) return P_CROSS | P_CIRCLE | held_mask;
    return held_mask;
}
int gfx_menu_hide_stick_x(void) { return gfx_mode && g_pad_polls - opt_seen <= 2; }

void f_0023C498_orig(Ctx *c);
void hle_opt_input(Ctx *c) {
    uint32_t o = c->r[4].uw[0];
    static int init;
    if (!init) { init = 1; patches_normal(); }
    if (g_pad_polls - opt_seen > 2 && gfx_mode) { gfx_mode = 0; patches_normal(); }
    opt_seen = g_pad_polls;
    uint16_t pad = host_pad_buttons(0);
    uint8_t an[4]; host_pad_analog(0, an);
    if (an[2] < 0x40) pad |= P_LEFT; else if (an[2] > 0xc0) pad |= P_RIGHT;
    uint16_t edge = pad & (uint16_t)~prev_pad; prev_pad = pad;
    int cursor = (int)MR32(o + 0xb50);
    opt_cursor = cursor;
    if (!gfx_mode) {
        if (cursor == 7 && (edge & (P_CROSS | P_CIRCLE))) { enter_gfx(c, o); opt_cursor = 1; return; }
        f_0023C498_orig(c);
        return;
    }
    /* Graphics page: left/right with auto-repeat */
    int dir = 0;
    if (edge & P_LEFT) { dir = -1; rep_dir = -1; rep_t = 18; }
    else if (edge & P_RIGHT) { dir = 1; rep_dir = 1; rep_t = 18; }
    else if ((pad & (rep_dir < 0 ? P_LEFT : P_RIGHT)) && rep_dir) { if (--rep_t <= 0) { dir = rep_dir; rep_t = 4; } }
    else rep_dir = 0;
    int confirm = (edge & (P_CROSS | P_CIRCLE)) != 0, cancel = (edge & (P_TRIANGLE | P_SQUARE)) != 0;
    if (cancel) { leave_gfx(c, o, 1); opt_cursor = 7; return; }
    int changed = 0;
    switch (cursor) {
    case 1: if (dir) { int v = dir < 0; if (v != gs_cfg.frameskip) { gs_cfg.frameskip = v; changed = 1; } } break;
    case 2: if (dir) { int v = dir < 0; if (v != gs_cfg.show_fps) { gs_cfg.show_fps = v; changed = 1; } } break;
    case 3: if (dir) { int v = dir < 0; if (v && (!host_use_gl() || g_hw_fail)) v = 0;
                       if (v != cur_renderer()) { gs_cfg.renderer = v; changed = 1; } } break;
    case 4: if (dir) { int v = clampi(gs_cfg.bright + dir, 0, 10); if (v != gs_cfg.bright) { gs_cfg.bright = v; changed = 1; } } break;
    case 5: if (dir) { int v = clampi(gs_cfg.fov + dir, 0, 10); if (v != gs_cfg.fov) { gs_cfg.fov = v; changed = 1; } } break;
    case 6: if (dir) { int v = dir > 0; if (v != gs_cfg.hz60) { gs_cfg.hz60 = v; changed = 1; } } break;
    case 7: if (dir || confirm) { int v = cur_scale();
                                  v = confirm ? (v % 8) + 1 : clampi(v + dir, 1, 8);
                                  if (v != cur_scale()) { gs_cfg.scale = v; changed = 1; patches_gfx(); } } break;
    case 8: if (confirm) { gs_cfg = gs_defaults; changed = 1; settings_apply(); patches_gfx(); } break;
    case 9: if (confirm) { leave_gfx(c, o, 1); opt_cursor = 7; return; } break;
    }
    if (changed) {
        settings_apply();
        settings_save();
        show_values(c, o);
        menu_sound(c, o, cursor == 8 ? 2 : 3);
    }
}
