/* In-game debug menu (F1, or SELECT+START on a gamepad).
 *
 * Navigation and drawing run on the main (window) thread; anything that touches game
 * memory is queued and carried out on the game thread at the next pad poll, which is a
 * safe point in the game's frame. While the menu is open the game sees no input.
 *
 * Game facts used here (PAL SCES-50885):
 *   0x3b35a0  global game state: +0x34 current mode, +0x38 requested mode (-1 = none),
 *             +0x80 requested scene, +0x84 scene to enter after the stage intro
 *   0x3b17a0  scene table (64-byte records: code string, +52 stage number)
 *   0x3e1970  save record: +0x4 player character (1-4; 3 and up is Kakeru, whose model is
 *             SYSTEM/HIKARU2.BIN; read when a stage loads), +0x14 max_stage .. +0x24 last_stage_in, +0x44 f_wide
 *   0x4d24c0  gadget table: 13 records of 96 bytes, +0x13 owned, +0x14 stage that unlocks it
 *             (rebuilt from max_stage at every stage load; the Magic Punch needs stage 29,
 *             which progress never reaches, so "unlock all" sets the owned bytes directly)
 * Mode 2 is the stage intro ("Go ape and capture N monkeys!"), mode 3 is play.
 */
#include "rt.h"
#include "overlay.h"
#include "font_data.h"
#include <pthread.h>
#include <math.h>

volatile int g_show_fps;
volatile int g_widescreen;
volatile float g_fps_game, g_fps_draw;
int g_force_widescreen = -1;          /* -1: follow the game's own setting; 0/1: force */

#define GS_MODE      0x3b35d4u
#define GS_REQMODE   0x3b35d8u
#define GS_REQSCENE  0x3b3620u
#define GS_NEXTSCENE 0x3b3624u
#define SAVE_BASE    0x3e1970u
#define SAVE_F_WIDE  (SAVE_BASE + 0x44)
#define SAVE_CHAR    (SAVE_BASE + 0x4)
#define GADGETS      0x4d24c0u
static int unlock_gadgets;

/* ------------------------------------------------------------------ stage list */
typedef struct { const char *name; const char *code; int scene; } Stage;
static const Stage stages[] = {
    {"Time Station (hub)", "STA", 113},
    {"Liberty Island", "LIB", 10},          {"Breezy Village", "HOL", 11},
    {"Port Calm", "VEN", 12},               {"Viva Apespania!", "SPA", 14},
    {"Blue Monkey Battle!", "BOS", 89},     {"Castle Frightmare!", "MON", 20},
    {"Vita-Z Factory", "FAC", 23},          {"Casino City", "AME", 26},
    {"Ninja Hideout", "JAP", 17},           {"Yellow Monkey Battle!", "BOS", 90},
    {"Snowball Mountain", "WIN", 37},       {"Lookout Valley", "HIK", 34},
    {"The Blue Baboon", "BEA", 29},         {"Pink Monkey Battle!", "BOS", 91},
    {"Enter the Monkey", "CHI", 40},        {"Simian Citadel", "GRE", 44},
    {"Panic Pyramid", "EGY", 49},           {"White Monkey Battle!", "BOS", 93},
    {"Pirate Isle", "OCE", 54},             {"Land of the Apes", "ANT", 59},
    {"The Lost Valley", "LOS", 63},         {"Red Monkey Battle!", "BOS", 94},
    {"Skyscraper City", "CIT", 67},         {"Code C.H.I.M.P.", "NAZ", 72},
    {"Giant Yellow Monkey Battle!", "BOS", 95}, {"Moon Base", "MOO", 78},
    {"Showdown with Specter", "BOS", 96},   {"Final Showdown with Specter", "BOS", 97},
    {"Training (tutorial)", "TUT", 98},
};
#define NSTAGES ((int)(sizeof stages / sizeof stages[0]))
/* the areas of a stage: consecutive scene-table records with the same 3-letter code */
static int stage_areas(int si, int *first) {
    *first = stages[si].scene;
    if (!strcmp(stages[si].code, "BOS") || !strcmp(stages[si].code, "STA")) return 1;
    int n = 0;
    for (int s = stages[si].scene; s < 120; s++) {
        char code[17]; gread(code, 0x3b17a0u + s * 64, 16); code[16] = 0;
        if (strncmp(code, stages[si].code, 3)) break;
        n++;
    }
    return n ? n : 1;
}

/* ------------------------------------------------------------------ actions (game thread) */
enum { ACT_NONE, ACT_WARP, ACT_UNLOCK, ACT_WIDE, ACT_FREECAM, ACT_CHAR, ACT_PLAYAS };
typedef struct { int kind, a, b; } Action;
static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static Action aq[16]; static int aq_n;
static char status_msg[160]; static uint64_t status_until;
static void set_status_for(const char *m, int secs) {
    pthread_mutex_lock(&mtx); snprintf(status_msg, sizeof status_msg, "%s", m); status_until = host_usec() + (uint64_t)secs * 1000000; pthread_mutex_unlock(&mtx);
}
static void set_status(const char *m) { set_status_for(m, 4); }
static void push_action(int kind, int a, int b) {
    pthread_mutex_lock(&mtx);
    if (aq_n < 16) aq[aq_n++] = (Action){kind, a, b};
    pthread_mutex_unlock(&mtx);
}
static int in_game(void) { uint32_t m = MR32(GS_MODE); return m == 2 || m == 3 || m == 4 || m == 5; }
static void run_action(Action a) {
    switch (a.kind) {
    case ACT_WARP:
        if (!in_game()) { set_status("Start or continue a game first, then warp."); break; }
        MW32(GS_REQSCENE, (uint32_t)a.a);
        MW32(GS_NEXTSCENE, (uint32_t)a.a);
        MW32(GS_REQMODE, a.b ? 2u : 3u);
        INFO("[debug] warp to scene %d (%s)\n", a.a, a.b ? "with intro" : "direct");
        set_status("Warping...");
        break;
    case ACT_UNLOCK:
        /* progress fields: max_stage, max_stage_clear, max_stage_in, last_stage_clear, last_stage_in */
        for (int i = 0; i < 5; i++) if ((int32_t)MR32(SAVE_BASE + 0x14 + i * 4) < 28) MW32(SAVE_BASE + 0x14 + i * 4, 28);
        unlock_gadgets = 1;
        INFO("[debug] all stages and gadgets unlocked\n");
        set_status("All stages and gadgets unlocked.");
        break;
    case ACT_WIDE:
        g_force_widescreen = a.a;
        break;
    case ACT_FREECAM:
        g_freecam = 1; g_noclip = 0;
        break;
    case ACT_PLAYAS: { extern volatile int g_monkey_mode; g_monkey_mode = a.a; break; }
    case ACT_CHAR: {   /* a.a: 1 Kakeru, 0 Hikaru (the Hikaru value the save had is kept) */
        static uint32_t hikaru_id = 1;
        uint32_t v = MR32(SAVE_CHAR);
        if (v >= 1 && v < 3) hikaru_id = v;
        MW32(SAVE_CHAR, a.a ? 3u : hikaru_id);
        INFO("[debug] character: %s\n", a.a ? "Kakeru" : "Hikaru");
        set_status(in_game() ? "Character changes at the next stage load (warp to reload this one)." : "Character set.");
        break;
    }
    }
}
/* AE2_DEBUG_ACTION="poll:warp=scene[/intro],poll:unlock,..." - for scripted testing */
static void scripted_actions(uint32_t poll) {
    static int init; static struct { uint32_t poll; Action a; } sa[32]; static int n;
    if (!init) {
        init = 1;
        const char *e = getenv("AE2_DEBUG_ACTION");
        while (e && *e && n < 32) {
            unsigned p = 0, sc = 0, intro = 0; char what[16] = {0};
            if (sscanf(e, "%u:%15[a-z]", &p, what) < 2) break;
            Action a = {0};
            if (!strcmp(what, "warp")) { const char *q = strchr(e, '='); if (q) sscanf(q + 1, "%u/%u", &sc, &intro); a = (Action){ACT_WARP, (int)sc, (int)intro}; }
            else if (!strcmp(what, "unlock")) a = (Action){ACT_UNLOCK, 0, 0};
            else if (!strcmp(what, "wide")) a = (Action){ACT_WIDE, 1, 0};
            else if (!strcmp(what, "freecam")) a = (Action){ACT_FREECAM, 1, 0};
            else if (!strcmp(what, "kakeru")) a = (Action){ACT_CHAR, 1, 0};
            else if (!strcmp(what, "hikaru")) a = (Action){ACT_CHAR, 0, 0};
            else if (!strcmp(what, "playas")) { const char *q = strchr(e, '='); a = (Action){ACT_PLAYAS, q ? atoi(q + 1) : 0, 0}; }
            sa[n].poll = p; sa[n].a = a; n++;
            const char *c = strchr(e, ','); e = c ? c + 1 : NULL;
        }
    }
    for (int i = 0; i < n; i++) if (sa[i].poll == poll) run_action(sa[i].a);
}

/* ------------------------------------------------------------------ menu model (main thread) */
enum { PG_MAIN, PG_STAGES, PG_AREAS };
static int open_, page, cursor[3], scroll[3], area_stage;
static int warp_intro = 1;
int debug_menu_is_open(void) { return open_; }

typedef struct { char label[96]; int id; } Row;
extern volatile int g_monkey_mode;   /* monkey.c */
const char *monkey_mode_name(int m);
void monkey_mode_changed(void);   /* gfx_menu.c: saved with the settings */
/* who the keyboard and controllers drive in two-player mode (host_sdl.c pad_for) */
int g_control_mode;   /* 0: keyboard + first pad player 1, second pad player 2 (one pad: keyboard player 1, pad player 2); 1: everything drives player 1; 2: everything drives player 2 */
static const char *control_name(int m) { return m == 1 ? "everything drives player 1" : m == 2 ? "everything drives player 2" : "one player per device"; }
int g_p2_halfrate = -1;   /* split screen: player 2's view drawn every other frame (-1: not set yet, from AE2_P2HALFRATE) */
enum { R_WARP = 1, R_INTRO, R_UNLOCK, R_CHAR, R_PLAYAS, R_FREECAM, R_NOCLIP, R_CONTROL, R_HALFRATE, R_WIDE, R_RENDER, R_SCALE, R_FPS, R_REC, R_CLOSE, R_STAGE0 = 100, R_AREA0 = 300 };
static int build_rows(Row *r) {
    int n = 0;
    if (page == PG_MAIN) {
        snprintf(r[n].label, 96, "Warp to stage..."); r[n++].id = R_WARP;
        snprintf(r[n].label, 96, "Warp with stage intro: %s", warp_intro ? "Yes" : "No"); r[n++].id = R_INTRO;
        snprintf(r[n].label, 96, "Unlock all stages and gadgets"); r[n++].id = R_UNLOCK;
        snprintf(r[n].label, 96, "Character: %s", MR32(SAVE_CHAR) >= 3 ? "Kakeru" : "Hikaru"); r[n++].id = R_CHAR;
        snprintf(r[n].label, 96, "Play as: %s", g_monkey_mode > 0 ? monkey_mode_name(g_monkey_mode) : MR32(SAVE_CHAR) >= 3 ? "Kakeru" : "Hikaru"); r[n++].id = R_PLAYAS;
        snprintf(r[n].label, 96, "Free camera (F2): %s", g_freecam ? "On" : "Off"); r[n++].id = R_FREECAM;
        snprintf(r[n].label, 96, "No clip (F3): %s", g_noclip ? "On" : "Off"); r[n++].id = R_NOCLIP;
        snprintf(r[n].label, 96, "Controls (F4): %s", control_name(g_control_mode)); r[n++].id = R_CONTROL;
        if (g_p2_halfrate < 0) g_p2_halfrate = getenv("AE2_P2HALFRATE") && atoi(getenv("AE2_P2HALFRATE"));
        snprintf(r[n].label, 96, "Split screen bottom view: %s", g_p2_halfrate ? "every other frame (faster)" : "every frame"); r[n++].id = R_HALFRATE;
        snprintf(r[n].label, 96, "Widescreen 16:9: %s", g_force_widescreen < 0 ? "Game setting" : g_force_widescreen ? "On" : "Off"); r[n++].id = R_WIDE;
        if (host_use_gl() && g_hw_fail) {
            snprintf(r[n].label, 96, "Renderer: Software (GPU failed: %.50s)", g_hw_fail); r[n++].id = R_RENDER;
        } else if (host_use_gl()) {
            snprintf(r[n].label, 96, "Renderer: %s", g_hw_request == 1 ? "GPU (OpenGL)" : "Software"); r[n++].id = R_RENDER;
            if (g_hw_request == 1) {
                static const char *res[] = {"", "PS2 native", "~1080p", "~1440p", "~4K", "5K", "6K", "7K", "8K"};
                snprintf(r[n].label, 96, "Internal resolution: %dx  (%s)", g_hw_scale, res[g_hw_scale]);
                r[n++].id = R_SCALE;
            }
        } else { snprintf(r[n].label, 96, "Renderer: Software (no OpenGL 3.3)"); r[n++].id = R_RENDER; }
        snprintf(r[n].label, 96, "Show FPS: %s", g_show_fps ? "On" : "Off"); r[n++].id = R_FPS;
        snprintf(r[n].label, 96, "Input recording: %s", getenv("AE2_RECORD") ? "On (this session)" : "Off (start with --record)"); r[n++].id = R_REC;
        snprintf(r[n].label, 96, "Close"); r[n++].id = R_CLOSE;
    } else if (page == PG_STAGES) {
        for (int i = 0; i < NSTAGES; i++) { snprintf(r[n].label, 96, "%s", stages[i].name); r[n++].id = R_STAGE0 + i; }
    } else {
        int first, na = stage_areas(area_stage, &first);
        for (int i = 0; i < na; i++) {
            char code[17]; gread(code, 0x3b17a0u + (first + i) * 64, 16); code[16] = 0;
            snprintf(r[n].label, 96, "Area %c  (%s)", 'A' + i, code); r[n++].id = R_AREA0 + first + i;
        }
    }
    return n;
}
static void activate(int id, int dir) {
    if (id == R_WARP) { page = PG_STAGES; return; }
    if (id == R_INTRO) { warp_intro ^= 1; return; }
    if (id == R_UNLOCK) { push_action(ACT_UNLOCK, 0, 0); return; }
    if (id == R_CHAR) { push_action(ACT_CHAR, MR32(SAVE_CHAR) < 3, 0); return; }
    if (id == R_PLAYAS) { g_monkey_mode = ((g_monkey_mode < 0 ? 0 : g_monkey_mode) + 11 + (dir < 0 ? -1 : 1)) % 11; monkey_mode_changed(); return; }
    if (id == R_WIDE) { int v = g_force_widescreen; g_force_widescreen = dir < 0 ? (v == -1 ? 1 : v - 1) : (v == 1 ? -1 : v + 1); return; }
    if (id == R_FPS) { g_show_fps ^= 1; return; }
    if (id == R_FREECAM) { g_freecam ^= 1; if (g_freecam) g_noclip = 0; return; }
    if (id == R_NOCLIP) { g_noclip ^= 1; if (g_noclip) g_freecam = 0; return; }
    if (id == R_HALFRATE) { g_p2_halfrate = !(g_p2_halfrate > 0); return; }
    if (id == R_CONTROL) { g_control_mode = (g_control_mode + 3 + (dir < 0 ? -1 : 1)) % 3; return; }
    if (id == R_RENDER) { if (host_use_gl() && !g_hw_fail) g_hw_request = g_hw_request == 1 ? 0 : 1; return; }
    if (id == R_SCALE) { int v = g_hw_scale + (dir < 0 ? -1 : 1); g_hw_scale = v < 1 ? 8 : v > 8 ? 1 : v; return; }
    if (id == R_CLOSE) { open_ = 0; return; }
    if (id >= R_STAGE0 && id < R_STAGE0 + NSTAGES) {
        int si = id - R_STAGE0, first, na = stage_areas(si, &first);
        if (na > 1) { area_stage = si; page = PG_AREAS; cursor[PG_AREAS] = scroll[PG_AREAS] = 0; return; }
        push_action(ACT_WARP, first, warp_intro); open_ = 0; return;
    }
    if (id >= R_AREA0) { push_action(ACT_WARP, id - R_AREA0, warp_intro); open_ = 0; page = PG_MAIN; return; }
}
/* menu state belongs to the main thread; the mutex only guards the action queue and status line */
void debug_menu_key(int key) {
    if (key == DMK_TOGGLE) { open_ ^= 1; if (open_) page = PG_MAIN; return; }
    if (key == DMK_FREECAM) { activate(R_FREECAM, 1); set_status(g_freecam ? "Free camera on: sticks fly, R1/L1 up/down, hold R2 fast, L2 slow" : "Free camera off"); return; }
    if (key == DMK_CONTROL) { activate(R_CONTROL, 1); char m[96]; snprintf(m, sizeof m, "Controls: %s", control_name(g_control_mode)); set_status(m); return; }
    if (key == DMK_NOCLIP) { activate(R_NOCLIP, 1); set_status(g_noclip ? "No clip on: left stick moves Hikaru, R1/L1 up/down, hold R2 fast" : "No clip off"); return; }
    if (!open_) return;
    Row rows[64]; int n = build_rows(rows);
    int *c = &cursor[page];
    if (*c >= n) *c = n - 1;
    int id = rows[*c].id, toggle = id == R_WIDE || id == R_INTRO || id == R_FPS || id == R_RENDER || id == R_SCALE || id == R_FREECAM || id == R_NOCLIP || id == R_CONTROL || id == R_HALFRATE || id == R_CHAR || id == R_PLAYAS;
    switch (key) {
    case DMK_UP: *c = (*c + n - 1) % n; break;
    case DMK_DOWN: *c = (*c + 1) % n; break;
    case DMK_LEFT: if (toggle) activate(id, -1); break;
    case DMK_RIGHT: if (toggle) activate(id, 1); break;
    case DMK_OK: activate(id, 1); break;
    case DMK_BACK: if (page == PG_AREAS) page = PG_STAGES; else if (page == PG_STAGES) page = PG_MAIN; else open_ = 0; break;
    }
}

/* ------------------------------------------------------------------ drawing (main thread) */
int ov_text(OvList *l, int x, int y, const char *s, uint32_t rgba) {
    for (; *s; s++, x += FONT_CW) {
        int g = (unsigned char)*s - FONT_FIRST;
        if (g > 0 && g < FONT_N && l->n < 4096) l->it[l->n++] = (OvItem){(int16_t)x, (int16_t)y, FONT_CW, FONT_CH, rgba, (int16_t)g};
    }
    return x;
}
void debug_menu_draw(OvList *l) {
    { static int told; if (g_hw_fail && !told) { told = 1; char m[160]; snprintf(m, sizeof m, "GPU renderer unavailable (%.80s) - using software", g_hw_fail); set_status_for(m, 10); } }
    char status[160] = ""; uint64_t now = host_usec();
    pthread_mutex_lock(&mtx); if (now < status_until) snprintf(status, sizeof status, "%s", status_msg); pthread_mutex_unlock(&mtx);
    if (g_show_fps) {
        char b[96], r[32];
        if (g_hw_active) snprintf(r, sizeof r, "GPU %dx", g_hw_active); else snprintf(r, sizeof r, "software");
        snprintf(b, sizeof b, "game %.1f fps  drawn %.1f fps  %s", g_fps_game, g_fps_draw, r);
        ov_rect(l, 8, 8, (int)strlen(b) * FONT_CW + 12, FONT_CH + 8, 0xa0000000);
        ov_text(l, 14, 12, b, 0xffffffff);
    }
    {   extern volatile int g_p2_paused;   /* p2.c: player 2 opened the pause menu */
        if (g_p2_paused) { const char *m = "Player 2"; int w = (int)strlen(m) * FONT_CW + 24;
            ov_rect(l, (OV_W - w) / 2, 14, w, FONT_CH + 12, 0xc0302080); ov_text(l, (OV_W - w) / 2 + 12, 20, m, 0xffffffff); } }
    if (status[0] && !open_) {
        int w = (int)strlen(status) * FONT_CW + 16;
        ov_rect(l, (OV_W - w) / 2, OV_H - 60, w, FONT_CH + 12, 0xc0000000);
        ov_text(l, (OV_W - w) / 2 + 8, OV_H - 54, status, 0xffffffff);
    }
    if (!open_) return;
    Row rows[64]; int n = build_rows(rows);
    int *c = &cursor[page], *sc = &scroll[page];
    if (*c >= n) *c = n - 1;
    const int vis = 24, x0 = 40, y0 = 40, w = 520, lh = FONT_CH + 4;
    if (*c < *sc) *sc = *c;
    if (*c >= *sc + vis) *sc = *c - vis + 1;
    int shown = n - *sc < vis ? n - *sc : vis;
    int h = 56 + shown * lh + 40;
    ov_rect(l, x0, y0, w, h, 0xd8181020);
    ov_rect(l, x0, y0, w, 36, 0xff3a2a6a);
    const char *title = page == PG_MAIN ? "Debug menu" : page == PG_STAGES ? "Warp to stage" : stages[area_stage].name;
    ov_text(l, x0 + 14, y0 + 8, title, 0xffffffff);
    if (page != PG_MAIN) ov_text(l, x0 + w - 14 - 13 * FONT_CW, y0 + 8, warp_intro ? "(with intro)" : "(direct)", 0xffb0b0d0);
    for (int i = 0; i < shown; i++) {
        int r = *sc + i, y = y0 + 48 + i * lh;
        if (r == *c) ov_rect(l, x0 + 6, y - 2, w - 12, lh, 0xff5a46a8);
        ov_text(l, x0 + 16, y, rows[r].label, r == *c ? 0xffffffff : 0xffd8d8e8);
    }
    if (*sc > 0) ov_text(l, x0 + w - 30, y0 + 44, "^", 0xffb0b0d0);
    if (*sc + shown < n) ov_text(l, x0 + w - 30, y0 + 48 + (shown - 1) * lh, "v", 0xffb0b0d0);
    ov_text(l, x0 + 14, y0 + h - 30, "Up/Down select  Enter/X ok  Esc/O back  F1 close", 0xff9090b0);
    if (status[0]) ov_text(l, x0 + 14, y0 + h + 8, status, 0xffffff80);
}

/* ------------------------------------------------------------------ game thread hook */
/* AE2_MENU_KEYS="poll:key,..." (keys: f1 up down left right ok back) drives the menu in
 * headless tests; frame dumps then include the overlay */
static void scripted_keys(uint32_t poll) {
    static int init, n; static struct { uint32_t poll; int key; } k[64];
    if (!init) {
        init = 1;
        const char *e = getenv("AE2_MENU_KEYS");
        static const struct { const char *n; int k; } names[] = {{"f1", DMK_TOGGLE}, {"f2", DMK_FREECAM}, {"f3", DMK_NOCLIP}, {"f4", DMK_CONTROL}, {"up", DMK_UP}, {"down", DMK_DOWN}, {"left", DMK_LEFT}, {"right", DMK_RIGHT}, {"ok", DMK_OK}, {"back", DMK_BACK}};
        while (e && *e && n < 64) {
            unsigned p; char w[16] = {0};
            if (sscanf(e, "%u:%15[a-z0-9]", &p, w) < 2) break;
            for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++) if (!strcmp(w, names[i].n)) { k[n].poll = p; k[n].key = names[i].k; n++; }
            const char *c = strchr(e, ','); e = c ? c + 1 : NULL;
        }
    }
    for (int i = 0; i < n; i++) if (k[i].poll == poll) debug_menu_key(k[i].key);
}
/* software compositing of the overlay into an RGBA frame (headless dumps) */
void ov_blit(uint32_t *px, int w, int h) {
    static OvList l; l.n = 0;
    debug_menu_draw(&l);
    float sc = (float)w / OV_W < (float)h / OV_H ? (float)w / OV_W : (float)h / OV_H;
    int ox = (int)((w - OV_W * sc) / 2), oy = (int)((h - OV_H * sc) / 2);
    for (int i = 0; i < l.n; i++) {
        OvItem *it = &l.it[i];
        int x0 = ox + (int)(it->x * sc), y0 = oy + (int)(it->y * sc), x1 = ox + (int)((it->x + it->w) * sc), y1 = oy + (int)((it->y + it->h) * sc);
        for (int y = y0 < 0 ? 0 : y0; y < y1 && y < h; y++) for (int x = x0 < 0 ? 0 : x0; x < x1 && x < w; x++) {
            uint32_t a = it->rgba >> 24;
            if (it->glyph >= 0) {
                int gx = (int)((x - x0) / sc), gy = (int)((y - y0) / sc);
                if (gx >= FONT_CW) gx = FONT_CW - 1; if (gy >= FONT_CH) gy = FONT_CH - 1;
                a = a * font_alpha[it->glyph][gy * FONT_CW + gx] / 255;
            }
            uint32_t d = px[y * w + x], o = 0;
            for (int c = 0; c < 24; c += 8) { uint32_t sv = (it->rgba >> c) & 255, dv = (d >> c) & 255; o |= ((sv * a + dv * (255 - a)) / 255) << c; }
            px[y * w + x] = o | (d & 0xff000000u);
        }
    }
}


/* ------------------------------------------------------------------ free camera / no clip (game thread) */
/* The game's main camera ends in one look-at call (eye, target), hooked in hle_overrides.c.
 * World space is Y-up; Hikaru walks about 1.3 units a frame and the camera sits ~55 behind.
 * Hikaru's state lives at 0x4d5c70 (position; 0x4d5cc0.. the last three positions,
 * 0x4d5d10 / 0x4d5d40 velocity) - see noclip_after_update. */
volatile int g_freecam, g_noclip;
static struct { int valid; float pos[3], yaw, pitch; } fc;
static uint16_t dbg_btn; static uint8_t dbg_an[4];   /* input taken from the game while flying */
static float stick_axis(uint8_t v) {
    float f = ((int)v - 128) / 127.0f;
    if (f > 1) f = 1; if (f < -1) f = -1;
    if (fabsf(f) < 0.18f) return 0;
    return (f - (f > 0 ? 0.18f : -0.18f)) / 0.82f;
}
static float fly_speed(void) {
    float sp = 2.0f;
    if (dbg_btn & (1 << 9)) sp *= 5;            /* R2: fast */
    if (dbg_btn & (1 << 8)) sp *= 0.25f;        /* L2: slow */
    return sp;
}
static void fc_step(void) {
    if (!fc.valid) return;
    float rx = stick_axis(dbg_an[0]), ry = stick_axis(dbg_an[1]), lx = stick_axis(dbg_an[2]), ly = stick_axis(dbg_an[3]);
    fc.yaw -= rx * 0.04f;
    fc.pitch -= ry * 0.03f;
    if (fc.pitch > 1.5f) fc.pitch = 1.5f; if (fc.pitch < -1.5f) fc.pitch = -1.5f;
    float cp = cosf(fc.pitch), sp_ = sinf(fc.pitch), cy = cosf(fc.yaw), sy = sinf(fc.yaw);
    float dir[3] = {cp * sy, sp_, cp * cy}, right[3] = {-cy, 0, sy};
    float v = fly_speed();
    for (int i = 0; i < 3; i++) fc.pos[i] += (dir[i] * -ly + right[i] * lx) * v;
    if (dbg_btn & (1 << 11)) fc.pos[1] += v;      /* R1: up */
    if (dbg_btn & (1 << 10)) fc.pos[1] -= v;      /* L1: down */
}
/* no clip: Hikaru follows our position; the game's own move still runs (animation, state)
 * and its result is replaced right after it */
#define PL_POS   0x4d5c70u   /* current position (vec4) */
#define PL_HIST  0x4d5cc0u   /* the last three positions */
#define PL_FACE  0x4d5cf0u   /* facing (two copies) */
#define PL_VEL   0x4d5d10u   /* velocity (two copies, at +0 and +0x30) */
static struct { int valid; float pos[3]; } nc;
static const uint32_t pl_actor = 0x4d5420u;   /* Hikaru's actor (the only one the mover handles; position at +0x850) */
static float cam_yaw;        /* the gameplay camera's heading, for stick-relative movement */
static void wf(uint32_t a, float f) { uint32_t u; memcpy(&u, &f, 4); MW32(a, u); }
static float rf_(uint32_t a) { uint32_t u = MR32(a); float f; memcpy(&f, &u, 4); return f; }
static void nc_step(void) {
    if (!nc.valid) { for (int i = 0; i < 3; i++) nc.pos[i] = rf_(PL_POS + 4 * i); nc.valid = 1; }
    float lx = stick_axis(dbg_an[2]), ly = stick_axis(dbg_an[3]);
    float cy = cosf(cam_yaw), sy = sinf(cam_yaw);
    float fwd[2] = {sy, cy}, right[2] = {-cy, sy};
    float v = fly_speed();
    float dx = (fwd[0] * -ly + right[0] * lx) * v, dz = (fwd[1] * -ly + right[1] * lx) * v;
    nc.pos[0] += dx; nc.pos[2] += dz;
    if (dbg_btn & (1 << 11)) nc.pos[1] += v;      /* R1: up */
    if (dbg_btn & (1 << 10)) nc.pos[1] -= v;      /* L1: down */
    if (dx * dx + dz * dz > 0.01f) {               /* face the way we fly */
        float l = sqrtf(dx * dx + dz * dz);
        for (int k = 0; k < 2; k++) { wf(PL_FACE + k * 16, dx / l); wf(PL_FACE + k * 16 + 4, 0); wf(PL_FACE + k * 16 + 8, dz / l); }
    }
}
void debug_noclip_after_move(uint32_t actor) {
    (void)actor;
    if (!g_noclip || !nc.valid) { if (!g_noclip) nc.valid = 0; return; }
    if (actor != pl_actor) return;
    for (int i = 0; i < 3; i++) {
        wf(PL_POS + 4 * i, nc.pos[i]);
        for (int h = 0; h < 3; h++) wf(PL_HIST + h * 16 + 4 * i, nc.pos[i]);
        wf(PL_VEL + 4 * i, 0); wf(PL_VEL + 0x30 + 4 * i, 0);
    }
}
/* called by the camera hook with the game's eye and target; returns 1 if replaced */
int debug_camera_override(float eye[3], float target[3]) {
    cam_yaw = atan2f(target[0] - eye[0], target[2] - eye[2]);
    if (!g_freecam) { fc.valid = 0; return 0; }
    if (!fc.valid) {
        float d[3] = {target[0] - eye[0], target[1] - eye[1], target[2] - eye[2]};
        fc.yaw = atan2f(d[0], d[2]); fc.pitch = atan2f(d[1], sqrtf(d[0] * d[0] + d[2] * d[2]));
        memcpy(fc.pos, eye, sizeof fc.pos); fc.valid = 1;
    }
    float cp = cosf(fc.pitch);
    float dir[3] = {cp * sinf(fc.yaw), sinf(fc.pitch), cp * cosf(fc.yaw)};
    for (int i = 0; i < 3; i++) { eye[i] = fc.pos[i]; target[i] = fc.pos[i] + dir[i] * 50; }
    return 1;
}

void debug_menu_frame(uint32_t poll, uint16_t *buttons, uint8_t an[4]) {
    scripted_actions(poll);
    scripted_keys(poll);
    pthread_mutex_lock(&mtx);
    Action a[16]; int n = aq_n; memcpy(a, aq, sizeof a); aq_n = 0;
    pthread_mutex_unlock(&mtx);
    int open_now = open_;
    for (int i = 0; i < n; i++) run_action(a[i]);
    /* widescreen: optionally force the game's own 16:9 option, and report it to the presenter */
    if (g_force_widescreen >= 0 && MR8(SAVE_F_WIDE) != (uint8_t)g_force_widescreen) MW8(SAVE_F_WIDE, (uint8_t)g_force_widescreen);
    g_widescreen = MR8(SAVE_F_WIDE) != 0;
    /* keep every gadget owned (the table is rebuilt at each stage load) */
    if (unlock_gadgets && MR32(GADGETS) == 0x5f746e6au /* "jnt_" */)
        for (int i = 1; i < 13; i++) if (MR8(GADGETS + i * 96 + 0x13) != 1) MW8(GADGETS + i * 96 + 0x13, 1);
    if (open_now) { *buttons = 0; an[0] = an[1] = an[2] = an[3] = 0x80; }
    /* flying: the sticks and shoulder buttons drive the camera (or Hikaru), not the game */
    if ((g_freecam || g_noclip) && in_game()) {
        dbg_btn = *buttons; memcpy(dbg_an, an, 4);
        if (g_freecam) fc_step();
        if (g_noclip) nc_step();
        *buttons &= (uint16_t)(1 << 3);          /* only START reaches the game (pause) */
        an[0] = an[1] = an[2] = an[3] = 0x80;
    }
}
