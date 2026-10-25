/* Player 2 (experimental).
 *
 * The game has one player: its state is a fixed block at 0x4d5420 (0x10e0 bytes; the
 * game's accessor 0x2c43f0 indexes an array of these, but only slot 0 exists), and the
 * stage frame (0x19e190) runs it in phases through 0x29a1f0(phase), which passes that
 * block to the phase dispatcher. Controllers are parsed by 0x1ae8c8(port) into
 * 0x240-byte records at 0x53f900 + port * 0x240; the player reads port 0's.
 *
 * Player 2 is a second copy of the player block, kept here, and its own controller
 * record. Every time the game runs a player phase, the phase runs a second time with
 * player 2's block and controller record swapped into place, so all of the game's code
 * that reads "the player" sees player 2 for that call. The controller record is
 * filled by running the game's own parser a second time on the second pad's data. */
#include "rt.h"

#define SAVE_CHAR 0x3e1974u   /* character in the save: 1 Hikaru, 3 or more Kakeru */
#define PL_BASE   0x4d5420u
#define PL_SIZE   0x10e0u
#define PL_POS    0x850u      /* position (vec4), then the last three positions at +0x8a0 */
#define PL_HIST   0x8a0u
#define PAD_BASE  0x53f900u
#define PAD_SIZE  0x240u
#define GS_MODE   0x3b35d4u
#define GS_REQMODE 0x3b35d8u   /* requested mode, -1 none */
#define GS_SCENE  0x3b361cu   /* current scene; the scene table (0x3b17a0, 64-byte records) starts with its code */
#define IN_BASE   0x3e14d0u   /* input records (see hle_input_update) */
#define IN_SIZE   0x230u
#define CAM_BASE  0x4cc000u   /* cameras (0x4cccf0, 0x270 each) and the camera manager (0x4d1af0) */
#define CAM_END   0x4d2000u
#define KICK_STATE 0x5396b0u
#define ACTOR_HEAD_T (0x535af0u + 6620u)   /* the stage actors' list (see hle_actors_step) */  /* 0x193100's state: buffer index, list pending */

volatile int g_p2_enable = -1;   /* -1: AE2_P2 decides */
int g_p2_mode;                   /* "2 Players: On" on the title menu: player 2, split screen, Kakeru, own save */
int g_p2_pad_source;
int g_pad_swap;                  /* hle_dbc_read: the players' pads trade places (split-screen pass) */             /* hle_dbc_read: 1 = return the second pad's data */
static int p2_live;              /* player 2 exists in the current stage */
static int p2_intro;             /* ... and is in the stage intro */
static uint8_t p2_pl[PL_SIZE], p2_pad[PAD_SIZE], sv_pl[PL_SIZE], sv_pad[PAD_SIZE];
/* player state kept outside the block: the control step's camera-relative input
 * (0x4d6510..0x4d6550), its small-data globals (gp+0xdb0..gp+0xdca), and the gadgets'
 * working state (13 objects of 0x220 bytes at 0x5d8fb0, "GMEHCA"), so each player has
 * their own club swing, net, and so on */
#define EX_MAX 0x1ba0
/* the player's health (save record +12, 0..50; 0x2c4df8 takes damage, 0x2c4e80 heals) is
 * one word, kept per player like the rest, so each brother has his own */
#define LIFE 0x3e197cu
/* the gadgets on the buttons and which one is in hand (save record +0x34..+0x3f), which
 * each player picks for himself in his own pause menu; player 2 keeps his from stage to
 * stage */
#define GADGETS 0x3e19a4u
#define EX_GADGETS (NEXTRA - 2)
/* the player's ride (0x5e03f0, stepped by 0x293310 after the player's phases): the
 * object that seats the player in a vehicle and drives it, one for each player so each
 * brother can ride one (see hle_ride_step) */
#define RIDE 0x5e03f0u
static const struct { uint32_t a, n; } extra[] = {{0x4d6500u, 0x60}, {0x5368a0u, 0x30}, {0x5d8fb0u, 13 * 0x220}, {RIDE, 0x590}, {GADGETS, 12}, {LIFE, 4}};
#define NEXTRA (sizeof extra / sizeof extra[0])
static uint8_t p2_ex[NEXTRA][EX_MAX], sv_ex[NEXTRA][EX_MAX];
static int in_p2;
static uint32_t p1_model = ~0u;   /* player 1's model, for player 2's collision (hle_model_coll) */
static int kakeru_loading;
static void kakeru_setup(Ctx *c);
extern int g_ghost; static int split_on(void);
static uint8_t p2_in[IN_SIZE], sv_in[IN_SIZE];
static int diff_due(void); static void diff_begin(void); static void diff_end(const char *, uint32_t);

int p2_active(void) { return p2_live; }
int p2_enabled(void) {
    if (g_p2_enable < 0) g_p2_enable = getenv("AE2_P2") && atoi(getenv("AE2_P2")) != 0;
    return g_p2_enable || g_p2_mode;
}
/* the player block in place (PL_BASE) is player 1's: in player 2's steps (in_p2) the
 * two trade places, and in the second-view pass (g_ghost) they trade again */
int p2_block_is_p1(void) { return in_p2 == (g_ghost != 0); }
/* Vehicles (the Vita-Z Factory robot, ...): two slots of 1520 bytes at 0x5e0cd0, stepped
 * by 0x2c5958 (hle_veh_step). Boarding stores the rider as a pointer (+92), and that is the
 * one fixed player block, so whoever was in it next also rode: player 2 got in and
 * player 1 was pulled along. Who boarded is kept here, and the pointer is shown only
 * while that player's block is in place (0 otherwise, which is "no rider" to the other
 * one) */
#define VEH_BASE  0x5e0cd0u
#define VEH_SIZE  1520u
#define VEH_N     2
#define VEH_RIDER 92u
static uint8_t veh_who[VEH_N];   /* 0 none, else the player riding */
static uint32_t veh_scene = ~0u;
static int at_base(void) { return p2_block_is_p1() ? 1 : 2; }
static void veh_note(void) {
    if (MR32(GS_SCENE) != veh_scene) { veh_scene = MR32(GS_SCENE); memset(veh_who, 0, sizeof veh_who); }
    for (int i = 0; i < VEH_N; i++) {
        uint32_t v = MR32(VEH_BASE + i * VEH_SIZE + VEH_RIDER);
        if (v == PL_BASE) { if (veh_who[i] != at_base()) { extern uint32_t g_pad_polls; INFO("[p2] poll %u: player %d is in vehicle %d\n", g_pad_polls, at_base(), i); } veh_who[i] = (uint8_t)at_base(); }
        else if (v || veh_who[i] == at_base()) veh_who[i] = 0;   /* got out (or something else) */
    }
}
static void veh_show(void) {
    for (int i = 0; i < VEH_N; i++)
        if (veh_who[i]) MW32(VEH_BASE + i * VEH_SIZE + VEH_RIDER, veh_who[i] == at_base() ? PL_BASE : 0);
}
static void swap_in(void) {
    veh_note();
    gread(sv_pl, PL_BASE, PL_SIZE); gread(sv_pad, PAD_BASE, PAD_SIZE);
    /* the game locks and unlocks control (cutscenes, the intro) only on player 1's
     * block (0x2c4ab8 writes +882/+892), so player 2 follows player 1's lock */
    p2_pl[882] = sv_pl[882]; p2_pl[892] = sv_pl[892];
    gwrite(PL_BASE, p2_pl, PL_SIZE); gwrite(PAD_BASE, p2_pad, PAD_SIZE);
    for (unsigned i = 0; i < NEXTRA; i++) { gread(sv_ex[i], extra[i].a, extra[i].n); gwrite(extra[i].a, p2_ex[i], extra[i].n); }
    gread(sv_in, IN_BASE, IN_SIZE); gwrite(IN_BASE, p2_in, IN_SIZE);
    in_p2 = 1;
    veh_show();
}
static void swap_out(void) {
    veh_note();
    gread(p2_pl, PL_BASE, PL_SIZE); gread(p2_pad, PAD_BASE, PAD_SIZE);
    gwrite(PL_BASE, sv_pl, PL_SIZE); gwrite(PAD_BASE, sv_pad, PAD_SIZE);
    for (unsigned i = 0; i < NEXTRA; i++) { gread(p2_ex[i], extra[i].a, extra[i].n); gwrite(extra[i].a, sv_ex[i], extra[i].n); }
    gread(p2_in, IN_BASE, IN_SIZE); gwrite(IN_BASE, sv_in, IN_SIZE);
    in_p2 = 0;
    veh_show();
}
/* player 2's camera state between passes (hle_stage_frame). In split screen, player 2's
 * own steps in the real frame run with it in place, since the stick moves the player
 * relative to the camera: with player 1's camera there, player 2's stick turned with
 * player 1's view */
static uint8_t cam2[CAM_END - CAM_BASE], cam1[CAM_END - CAM_BASE]; static int have_cam2, cam2_frame, cam2_in;
#define CAM_MTX 0x3de6d0u   /* the view's rotation, which 0x29b298 turns the stick by */
static uint8_t mtx2[64], mtx1[64];
/* In the second-view pass the other player (player 1 there) steps with player 2's camera in
 * place; whatever that step does to the cameras (a gotcha switches the view to its own
 * camera, 0x2716d0) is undone, so one player's gotcha shot stays in their own half */
static void cam_in(void) {
    if (cam2_in) return;
    if (g_ghost) { gread(cam1, CAM_BASE, sizeof cam1); gread(mtx1, CAM_MTX, 64); cam2_in = 2; return; }
    if (!cam2_frame) return;
    gread(cam1, CAM_BASE, sizeof cam1); gwrite(CAM_BASE, cam2, sizeof cam2);
    gread(mtx1, CAM_MTX, 64); gwrite(CAM_MTX, mtx2, 64); cam2_in = 1; }
static void cam_out(void) {
    if (!cam2_in) return;
    if (cam2_in == 1) { gread(cam2, CAM_BASE, sizeof cam2); gread(mtx2, CAM_MTX, 64); }
    gwrite(CAM_BASE, cam1, sizeof cam1); gwrite(CAM_MTX, mtx1, 64); cam2_in = 0; }
static void wf(uint8_t *b, uint32_t off, float f) { memcpy(b + off, &f, 4); }
static float gf(uint32_t a) { uint32_t u = MR32(a); float f; memcpy(&f, &u, 4); return f; }
static void wf_g(uint32_t a, float f) { uint32_t u; memcpy(&u, &f, 4); MW32(a, u); }
static float rf(const uint8_t *b, uint32_t off) { float f; memcpy(&f, b + off, 4); return f; }
/* player 2 starts as a copy of player 1, a little to the side */
static void spawn(Ctx *c) {
    for (int i = 0; i < VEH_N; i++) if (veh_who[i] == 2) veh_who[i] = 0;   /* a new player 2 is in no vehicle */
    gread(p2_pl, PL_BASE, PL_SIZE);
    gread(p2_pad, PAD_BASE, PAD_SIZE);
    static uint8_t gad[12]; static int have_gad;
    if (have_gad) memcpy(gad, p2_ex[EX_GADGETS], 12);
    for (unsigned i = 0; i < NEXTRA; i++) gread(p2_ex[i], extra[i].a, extra[i].n);
    if (have_gad) memcpy(p2_ex[EX_GADGETS], gad, 12);
    have_gad = 1;
    { int32_t l; memcpy(&l, p2_ex[NEXTRA - 1], 4); if (l < 10) { l = 10; memcpy(p2_ex[NEXTRA - 1], &l, 4); } }   /* never rejoin with no health */
    gread(p2_in, IN_BASE, IN_SIZE);
    for (int h = 0; h < 4; h++) {
        uint32_t o = h == 0 ? PL_POS : PL_HIST + (h - 1) * 16;
        wf(p2_pl, o, rf(p2_pl, o) + 25.0f);
    }
    p2_live = 1;
    have_cam2 = 0;   /* a camera kept from another stage would look at that stage's places */
    kakeru_setup(c);
    { extern uint32_t g_pad_polls; INFO("[p2] poll %u: ", g_pad_polls); } INFO("[p2] player 2 joined at %.1f %.1f %.1f\n", rf(p2_pl, PL_POS), rf(p2_pl, PL_POS + 4), rf(p2_pl, PL_POS + 8));
}

/* 0x1ae8c8(port): controller parser (one port; called by the input step below) */
void f_001AE8C8_orig(Ctx *c);
void hle_pad_update(Ctx *c) { f_001AE8C8_orig(c); }

/* 0x1af2f8: the frame's input step. It parses both ports and merges them into the one
 * input record the game reads (0x3e15f0; per-port copies at 0x3e14d0 + port * 0x90).
 * For player 2 it runs again on the second pad, with player 2's records in place. */
void f_001AF2F8_orig(Ctx *c);
void hle_input_update(Ctx *c) {
    f_001AF2F8_orig(c);
    if (!p2_live || in_p2) return;
    r128 v0 = c->r[2];
    gread(sv_pad, PAD_BASE, PAD_SIZE); gread(sv_in, IN_BASE, IN_SIZE);
    gwrite(PAD_BASE, p2_pad, PAD_SIZE); gwrite(IN_BASE, p2_in, IN_SIZE);
    g_p2_pad_source = 1;
    f_001AF2F8_orig(c);
    g_p2_pad_source = 0;
    gread(p2_pad, PAD_BASE, PAD_SIZE); gread(p2_in, IN_BASE, IN_SIZE);
    gwrite(PAD_BASE, sv_pad, PAD_SIZE); gwrite(IN_BASE, sv_in, IN_SIZE);
    c->r[2] = v0;
}

/* The frame's collision spheres (0x1e36ec0, 0x150 each: +0x44 kind, +0x58 owner) tag a
 * character's body with its model's index, and stage objects (items, switches, ...) look
 * for the player's model ("player"). Player 2's models are registered under their own
 * names, so after player 2's step their body is tagged as player 1's model, and the
 * stage would react to either brother. Off unless AE2_P2BODYTAG=1: the same entries also
 * pick the model each half draws, so with the tag each half shows both players as the
 * same brother. */
#define SPH_LIST MR32(0x5376c0u)   /* the list is on the heap; this global points at it */
static void body_tag(void) {
    static int on = -1; if (on < 0) on = getenv("AE2_P2BODYTAG") && atoi(getenv("AE2_P2BODYTAG"));
    uint32_t mine = MR32(PL_BASE + 148), theirs; memcpy(&theirs, p2_pl + 148, 4);
    if (!on || mine == theirs || mine > 255 || theirs > 255) return;
    for (int i = 0; i < 64; i++) {
        uint32_t b = SPH_LIST + i * 0x150u, k = MR32(b + 0x44);
        if (MR32(b + 0x58) == PL_BASE && (k & 0xff) == theirs) MW32(b + 0x44, (k & ~0xffu) | mine);
    }
}

/* Losing all health (or falling out of the stage) ends in a mode request: 4 (retry from
 * the last checkpoint, one life less) or 8 (game over). When that comes from player 2's
 * step, the stage goes on: player 2 drops out and comes back next to player 1 on the next
 * frame, with player 1's health. hle_set_game_mode asks here first. */
/* Falling out of the stage puts the player in state 45 (+868, with +891 = 1), which fades
 * the screen out for 150 frames and then restarts from the checkpoint (mode 4). In
 * two-player mode either brother instead comes straight back where he last stood safely,
 * one cookie (10) of health down: each player's block is kept from the last moment he
 * stood still on the ground in his normal state, and put back. With too little health
 * left for that, the fall goes on as in one-player mode (player 1) or player 2 drops out
 * and rejoins next to player 1 (p2_down). AE2_P2FALL=0: the game's own falls. */
#define FALL_COST 10
typedef struct { uint8_t pl[PL_SIZE]; int have, still; float y; } SafeSpot;
static SafeSpot safe[2];
static void safe_track(SafeSpot *sp, const uint8_t *pl) {
    float y = rf(pl, PL_POS + 4); int32_t st; memcpy(&st, pl + 868, 4);
    sp->still = (st == 0 && fabsf(y - sp->y) < 0.5f) ? sp->still + 1 : 0;
    sp->y = y;
    if (sp->still >= 15 && sp->still % 15 == 0) { memcpy(sp->pl, pl, PL_SIZE); sp->have = 1; }
}
static int32_t rf32(const uint8_t *b, uint32_t o) { int32_t v; memcpy(&v, b + o, 4); return v; }
static int fell(const uint8_t *pl) { int32_t st; memcpy(&st, pl + 868, 4); return st == 45 && pl[891] == 1; }
static int fall_on(void) { static int v = -1; if (v < 0) v = !getenv("AE2_P2FALL") || atoi(getenv("AE2_P2FALL")); return v; }
static int p2_down;
int p2_mode_request(uint32_t m) {
    if (!in_p2 || g_ghost || !p2_live || (m != 4 && m != 8)) return 0;
    MW8(0x3b366bu, 0);   /* set with the retry request (0x2a85d0) */
    if (!p2_down) INFO("[p2] player 2 is down (mode %u request dropped); back next to player 1\n", m);
    p2_down = 1;
    return 1;
}
static void spawn(Ctx *c);

/* 0x29a1f0(phase, arg): runs one phase of the player's frame (only in play mode) */
void f_0029A1F0_orig(Ctx *c);
void hle_player_phase(Ctx *c) {
    r128 a0 = c->r[4], a1 = c->r[5];
    int dd = !in_p2 && diff_due(); if (dd) diff_begin();
    f_0029A1F0_orig(c);
    if (dd) { char w[32]; snprintf(w, sizeof w, "phase %u/%u", a0.uw[0], a1.uw[0]); diff_end(w, c->r[29].uw[0]); }
    if (in_p2) return;
    int mode = MR32(GS_MODE), playing = mode == 3;
    if (!playing || !p2_enabled()) { if (mode != 2) { p2_live = 0; p2_intro = 0; } return; }
    if (p2_intro) { p2_live = 0; p2_intro = 0; }   /* out of the stage intro: a fresh start in the stage */
    if (p2_live && p2_down && !g_ghost && !(MR32(PL_BASE + 868) == 45 && MR8(PL_BASE + 891) == 1)) { p2_down = 0; spawn(c); safe[1].have = 0; }
    if (p2_live && fall_on() && !g_ghost) {   /* (in the second-view pass the players trade places) */
        static uint32_t last; extern uint32_t g_pad_polls;
        static uint8_t p1[PL_SIZE];
        gread(p1, PL_BASE, PL_SIZE);
        if (g_pad_polls != last) { last = g_pad_polls; safe_track(&safe[0], p1); safe_track(&safe[1], p2_pl); }
        int32_t l1 = (int32_t)MR32(LIFE), l2; memcpy(&l2, p2_ex[NEXTRA - 1], 4);
        { static long w0 = -2, w1; if (w0 == -2) { w0 = -1; if (getenv("AE2_FALLLOG")) sscanf(getenv("AE2_FALLLOG"), "%ld,%ld", &w0, &w1); }
          if (w0 >= 0 && g_pad_polls >= (uint32_t)w0 && g_pad_polls <= (uint32_t)w1)
            INFO("[fall] poll %u: p1 state %d/%d y %.0f health %d | p2 state %d/%d y %.0f health %d\n", g_pad_polls, rf32(p1, 868), p1[891], rf(p1, PL_POS + 4), l1, rf32(p2_pl, 868), p2_pl[891], rf(p2_pl, PL_POS + 4), l2); }
        if (fell(p1) && safe[0].have && l1 > FALL_COST) {
            gwrite(PL_BASE, safe[0].pl, PL_SIZE); MW32(LIFE, (uint32_t)(l1 - FALL_COST)); safe[0].still = 0;
            INFO("[p2] player 1 fell: back at %.0f %.0f %.0f, health %d\n", rf(safe[0].pl, PL_POS), rf(safe[0].pl, PL_POS + 4), rf(safe[0].pl, PL_POS + 8), l1 - FALL_COST);
        }
        if (fell(p2_pl) && !fell(p1)) {   /* (while player 1 is falling too, a rejoin would copy his fall) */
            if (safe[1].have && l2 > FALL_COST) {
                memcpy(p2_pl, safe[1].pl, PL_SIZE); l2 -= FALL_COST; memcpy(p2_ex[NEXTRA - 1], &l2, 4); safe[1].still = 0;
                INFO("[p2] player 2 fell: back at %.0f %.0f %.0f, health %d\n", rf(p2_pl, PL_POS), rf(p2_pl, PL_POS + 4), rf(p2_pl, PL_POS + 8), l2);
            } else { INFO("[p2] player 2 fell with %d health (player 1 state %d/%d, health %d): rejoins next to player 1\n", l2, (int)rf32(p1, 868), p1[891], l1); spawn(c); safe[1].have = 0; }
        }
    }
    { static long hurt = -2; extern uint32_t g_pad_polls; if (hurt == -2) hurt = getenv("AE2_P2HURT") ? atol(getenv("AE2_P2HURT")) : -1;   /* testing */
      if (p2_live && hurt >= 0 && g_pad_polls >= (uint32_t)hurt) { hurt = -1; void f_0019CBF8(Ctx *); Ctx t = *c;
        swap_in(); MW32(LIFE, 7); INFO("[p2] test: player 2 falls out (health 7, mode 4 request)\n"); t.r[4].sd[0] = 4; f_0019CBF8(&t); swap_out(); } }
    { static long dp = -2; static int dw; extern uint32_t g_pad_polls;   /* testing: AE2_TEST_DROP=poll,who drops a player 3000 units */
      if (dp == -2) { dp = -1; if (getenv("AE2_TEST_DROP")) sscanf(getenv("AE2_TEST_DROP"), "%ld,%d", &dp, &dw); }
      if (p2_live && dp >= 0 && g_pad_polls >= (uint32_t)dp) { dp = -1; INFO("[p2] test: player %d drops\n", dw);
        for (int h = 0; h < 4; h++) { uint32_t o = h == 0 ? PL_POS : PL_HIST + (h - 1) * 16;
          if (dw == 2) wf(p2_pl, o + 4, rf(p2_pl, o + 4) - 3000); else wf_g(PL_BASE + o + 4, gf(PL_BASE + o + 4) - 3000); } } }
    if (!p2_live) {   /* player 2 joins once player 1 has control (player 1 is in place by then) */
        if (MR8(PL_BASE + 882)) return;
        spawn(c);
    }
    r128 v0 = c->r[2];
    p1_model = MR32(PL_BASE + 148);
    swap_in(); cam_in();
    if (getenv("AE2_P2LOG")) { static int n; if (n++ % 300 == 0) INFO("[p2] phase %u model %u hair %u\n", a0.uw[0], MR32(PL_BASE + 148), MR32(PL_BASE + 844)); }
    c->r[4] = a0; c->r[5] = a1;
    if (dd) diff_begin();
    f_0029A1F0_orig(c);
    if (dd) { char w[32]; snprintf(w, sizeof w, "p2 phase %u/%u", a0.uw[0], a1.uw[0]); diff_end(w, c->r[29].uw[0]); }
    cam_out(); swap_out();
    body_tag();
    c->r[2] = v0;
    { static uint32_t last; extern uint32_t g_pad_polls; if (getenv("AE2_P2LOG") && g_pad_polls != last && g_pad_polls % 50 == 0) { last = g_pad_polls;
        float s0, s1; memcpy(&s0, p2_pad + 368, 4); memcpy(&s1, p2_pad + 372, 4); float t0, t1; uint8_t p1pad[16]; gread(p1pad, PAD_BASE + 368, 16); memcpy(&t0, p1pad, 4); memcpy(&t1, p1pad + 4, 4);
        INFO("[p2] in %.2f %.2f vel %.2f %.2f | ", rf(p2_in, 0x130), rf(p2_in, 0x134), rf(p2_pl, 0x8f0), rf(p2_pl, 0x8f8)); INFO("[p2] poll %u p2 pos %.1f %.1f %.1f stick %.2f %.2f | p1 pos %.1f %.1f %.1f stick %.2f %.2f\n", g_pad_polls, rf(p2_pl, PL_POS), rf(p2_pl, PL_POS + 4), rf(p2_pl, PL_POS + 8), s0, s1,
             gf(PL_BASE + PL_POS), gf(PL_BASE + PL_POS + 4), gf(PL_BASE + PL_POS + 8), t0, t1); } }
}

/* 0x29a260(task, message, arg): the player's task handler. In play the stage frame
 * reaches it through 0x29a1f0 (above); in the stage intro ("Go ape and capture...")
 * the game's task list calls it directly, and there player 2 tumbles through the
 * intro beside player 1 */
void f_0029A260_orig(Ctx *c);
static uint32_t p1_task;   /* player 1's task, for AE2_TEST_HIT */
void hle_player_task(Ctx *c) {
    r128 a0 = c->r[4], a1 = c->r[5], a2 = c->r[6];
    static int dmglog = -1; if (dmglog < 0) dmglog = getenv("AE2_DMGLOG") != NULL;   /* testing: who gets hurt */
    if (dmglog && a1.uw[0] == 8) { extern uint32_t g_pad_polls; char st[200] = ""; int k = 0;
#ifdef AE2_FPCHECK
        for (int d = g_fdepth - 1; d >= 0 && d >= g_fdepth - 12; d--) if (d < 256) k += snprintf(st + k, sizeof st - (size_t)k, " <%06x", g_fstack[d]);
#endif
        (void)k; INFO("[dmg] poll %u: message 8 to %x (p2 %d) arg %x from %x, health %d%s\n", g_pad_polls, MR32(a0.uw[0] + 48), in_p2, a2.uw[0], c->r[31].uw[0], (int32_t)MR32(LIFE), st); }
    if (!in_p2 && MR32(a0.uw[0] + 48) == PL_BASE) p1_task = a0.uw[0];
    int monkey_shield_blocks(uint32_t player, uint32_t msg);
    if (!in_p2 && monkey_shield_blocks(MR32(a0.uw[0] + 48), a1.uw[0])) memset(&c->r[2], 0, sizeof c->r[2]);   /* the light blue monkey's shield */
    else f_0029A260_orig(c);
    if (in_p2 || MR32(GS_MODE) != 2 || !p2_enabled() || MR32(a0.uw[0] + 48) != PL_BASE) return;
    if (!p2_live) { spawn(c); p2_intro = 1; }
    r128 v0 = c->r[2];
    swap_in();
    c->r[4] = a0; c->r[5] = a1; c->r[6] = a2;
    f_0029A260_orig(c);
    swap_out();
    c->r[2] = v0;
}

/* 0x1adde0: in the stage intro, aims the tunnel camera (0x3e1270) at player 1, then
 * poses and draws player 1. It runs again with player 2 swapped in, with the camera put
 * back afterwards, so both brothers tumble through the tunnel */
void f_001ADDE0_orig(Ctx *c);
#define INTRO_CAM 0x3e1270u
static void intro_shift(float k) {
    static float off[3] = {-1e9f};
    if (off[0] == -1e9f) { const char *e = getenv("AE2_P2INTRO"); off[0] = 9; off[1] = 0; off[2] = 0; if (e) sscanf(e, "%f,%f,%f", &off[0], &off[1], &off[2]); }
    for (int i = 0; i < 3; i++) for (uint32_t t = 224; t <= 240; t += 16) wf_g(INTRO_CAM + t + i * 4, gf(INTRO_CAM + t + i * 4) + k * off[i]);
}
void hle_intro_player_draw(Ctx *c) {
    r128 a[4] = {c->r[4], c->r[5], c->r[6], c->r[7]};
    int both = !in_p2 && p2_live && p2_intro && MR32(GS_MODE) == 2;
    /* the pose follows the camera target (+224, eased toward +240): player 1 floats a
     * little to the left and player 2 as far to the right */
    if (both) intro_shift(-1);
    f_001ADDE0_orig(c);
    if (!both) return;
    intro_shift(1);
    static uint8_t cam[0x200];
    r128 v0 = c->r[2];
    gread(cam, INTRO_CAM, sizeof cam);
    intro_shift(1);
    swap_in();
    for (int i = 0; i < 4; i++) c->r[4 + i] = a[i];
    f_001ADDE0_orig(c);
    swap_out();
    gwrite(INTRO_CAM, cam, sizeof cam);
    c->r[2] = v0;
}

/* ---- analysis (AE2_P2DIFF=poll): which memory each player phase writes */
static uint8_t *dsnap;
static void diff_begin(void) { if (!dsnap) dsnap = malloc(g_ram_size); memcpy(dsnap, g_ram, g_ram_size); }
static void diff_end(const char *what, uint32_t sp) {
    uint32_t a = 0x100000; int n = 0;
    while (a < g_ram_size && n < 60) {
        if (g_ram[a] == dsnap[a] || (a >= sp - 0x10000 && a < sp + 0x1000)) { a++; continue; }
        uint32_t b = a; while (b < g_ram_size && (b - a < 16 || memcmp(g_ram + b, dsnap + b, 16))) b += 16;
        INFO("[p2diff] %s: %07x-%07x (%u)\n", what, a, b, b - a); n++; a = b;
    }
}
static int diff_due(void) { extern uint32_t g_pad_polls; static long p = -2, q;
    if (p == -2) { const char *e = getenv("AE2_P2DIFF"); p = e ? atol(e) : -1; q = e && strchr(e, '-') ? atol(strchr(e, '-') + 1) : p; }
    return p >= 0 && g_pad_polls >= (uint32_t)p && g_pad_polls <= (uint32_t)q; }
void f_0029B298_orig(Ctx *c);
void hle_player_control(Ctx *c) {
    int dd = diff_due(); if (dd) diff_begin();
    r128 a0 = c->r[4];
    f_0029B298_orig(c);
    if (dd) diff_end("control", c->r[29].uw[0]);
    if (in_p2 || !p2_live) return;
    r128 v0 = c->r[2];
    swap_in(); cam_in(); c->r[4] = a0;
    if (dd) diff_begin();
    f_0029B298_orig(c);
    if (dd) diff_end("p2 control", c->r[29].uw[0]);
    { extern uint32_t g_pad_polls; if (getenv("AE2_P2LOG") && g_pad_polls % 50 == 0) INFO("[p2] control: flag882 %d port876 %d mag %.2f %.2f %.2f dir %.2f %.2f %.2f mtx %.2f %.2f\n", (int8_t)MR8(PL_BASE + 882), MR32(PL_BASE + 876), gf(0x5368a0), gf(0x5368a4), gf(0x5368a8), gf(0x4d6510), gf(0x4d6514), gf(0x4d6518), gf(CAM_MTX), gf(CAM_MTX + 8)); }
    cam_out(); swap_out();
    c->r[2] = v0;
}

/* 0x293310(message): steps the player's ride (see RIDE). The stage frame calls it after
 * the player's phases, outside them, so it runs again here with player 2 in place */
void f_00293310_orig(Ctx *c);
void hle_ride_step(Ctx *c) {
    r128 a0 = c->r[4];
    f_00293310_orig(c);
    if (in_p2 || !p2_live || MR32(GS_MODE) != 3) return;
    r128 v0 = c->r[2];
    static uint8_t camsv[CAM_END - CAM_BASE], mtxsv[64]; int keep = 0;   /* camera changes: as in actor_call */
    swap_in(); if (cam2_frame || g_ghost) cam_in(); else { gread(camsv, CAM_BASE, sizeof camsv); gread(mtxsv, CAM_MTX, 64); keep = 1; }
    c->r[4] = a0;
    f_00293310_orig(c);
    if (keep) { gwrite(CAM_BASE, camsv, sizeof camsv); gwrite(CAM_MTX, mtxsv, 64); } else cam_out();
    swap_out();
    c->r[2] = v0;
}

/* 0x2c5958(message): steps the vehicles (0x2c59c0 for each slot in use); the stage frame
 * calls it outside the player's phases. A ridden vehicle seats and drives its rider
 * through the rider pointer, so it runs with the brother inside it in place */
void f_002C59C0(Ctx *c);
void hle_veh_step(Ctx *c) {
    uint32_t msg = c->r[4].uw[0];
    for (int i = 0; i < VEH_N; i++) {
        uint32_t v = VEH_BASE + i * VEH_SIZE;
        if (!MR32(v + 60)) continue;
        veh_note();
        int other = p2_live && !in_p2 && veh_who[i] && veh_who[i] != at_base();
        static uint8_t camsv[CAM_END - CAM_BASE], mtxsv[64]; int keep = 0;
        if (other) { swap_in(); if (cam2_frame || g_ghost) cam_in(); else { gread(camsv, CAM_BASE, sizeof camsv); gread(mtxsv, CAM_MTX, 64); keep = 1; } }
        c->r[4].sd[0] = (int32_t)v; c->r[5].sd[0] = (int32_t)msg; memset(&c->r[6], 0, sizeof c->r[6]);
        f_002C59C0(c);
        if (other) { if (keep) { gwrite(CAM_BASE, camsv, sizeof camsv); gwrite(CAM_MTX, mtxsv, 64); } else cam_out(); swap_out(); }
    }
    memset(&c->r[2], 0, sizeof c->r[2]);
}

/* 0x284888(mode): the player's attack for this frame. It checks the current gadget
 * (club, net, ...) against everything in reach of player 0 and sends the hits, and
 * the stage frame calls it outside the player phases, so it runs again here for
 * player 2: without that, player 2's swings and nets touch nothing. */
void f_00284888_orig(Ctx *c);
void hle_player_attack(Ctx *c) {
    r128 a0 = c->r[4];
    f_00284888_orig(c);
    if (in_p2 || !p2_live) return;
    r128 v0 = c->r[2];
    swap_in(); cam_in(); c->r[4] = a0;
    f_00284888_orig(c);
    cam_out(); swap_out();
    c->r[2] = v0;
}

/* The gameplay camera (hle_camlook): while player 2 is in the stage, the camera looks
 * at the point between the two players and backs off as they move apart
 * (AE2_P2CAM=0: follow player 1 only). */
void p2_camera_adjust(float eye[3], float target[3]) {
    static int mode = -1; if (mode < 0) mode = getenv("AE2_P2CAM") ? atoi(getenv("AE2_P2CAM")) : 1;
    if (!p2_live || !mode) return;
    if (split_on()) {   /* split screen: each view has the game's own camera */
        return;
    }
    float d[3], dist = 0, back[3], blen = 0;
    for (int k = 0; k < 3; k++) { d[k] = (rf(p2_pl, PL_POS + k * 4) - gf(PL_BASE + PL_POS + k * 4)) * 0.5f; dist += d[k] * d[k]; }
    dist = sqrtf(dist) * 2;
    for (int k = 0; k < 3; k++) { back[k] = eye[k] - target[k]; blen += back[k] * back[k]; }
    blen = sqrtf(blen);
    float pull = dist > 40 ? (dist - 40) * 0.8f : 0;   /* how far the eye moves back */
    for (int k = 0; k < 3; k++) {
        target[k] += d[k];
        eye[k] += d[k] + (blen > 0 ? back[k] / blen * pull : 0);
    }
}

/* ---- Split screen (AE2_P2SPLIT=1)
 * The game draws each object while it updates it, so there is no separate "draw the
 * scene" step to run twice. Instead each stage frame runs twice: first a second-view
 * pass, from player 2's camera, whose game-state changes are all thrown away (memory,
 * registers and player 2's state are restored afterwards; it sends nothing to the IOP
 * and no other thread runs), then the real frame. The pass's draw list goes to the GS
 * straight away, drawn into the bottom half of the screen; the real frame's list (sent
 * at the start of the next frame) is drawn into the top half. */
int g_ghost, g_ghost_switched;
/* the frame's list is sent during the next stage frame (0x19cf28 calls 0x193100 early on):
 * split_list says whether the list being sent had a second-view pass, split_next
 * whether the one being built has */
static int split_list, split_next;
/* an AE2_* setting if given, otherwise on in the title menu's 2-player mode */
static int env_or_mode(const char *name) { const char *e = getenv(name); return e ? atoi(e) != 0 : g_p2_mode; }
static int split_on(void) { return env_or_mode("AE2_P2SPLIT"); }
void gs_write_priv64(uint32_t a, uint64_t v);
static int kicked_in_pass, real_kick;   /* real_kick: the real frame's list being sent during the pass */
static void gs_split(int mode) { static int cur; if (mode != cur) { cur = mode; gs_write_priv64(0x12001fe0, (uint64_t)mode); } }

/* 0x193100: sends the frame's draw list (built during the previous frame) to the GS */
void f_00193100_orig(Ctx *c);
void hle_frame_kick(Ctx *c) {
    if (g_ghost) {
        /* the second-view pass: the real frame's pending list (built last frame) is sent
         * now, ahead of the pass's own list (sent by hle_stage_frame), so the GS gets the
         * lists in the order they were built. The game uploads a texture only when its
         * list is the first to need it; sent out of order, player 2's half drew with
         * textures that weren't there yet (a scrambled sky in the boss arenas) */
        real_kick = 1; gs_split(split_list ? 1 : 0); f_00193100_orig(c); real_kick = 0;
        kicked_in_pass = 1;
        return;
    }
    gs_split(split_list ? 1 : 0);
    f_00193100_orig(c);
}

extern uint8_t g_vu0mem[];
void f_0019CF28_orig(Ctx *c);
/* testing aid: AE2_TEST_TELE=addr[,who[,from]]: from poll `from` on, keep player `who`
 * (1 or 2) within reach of the object at addr (its position at +16), e.g. a boss */
/* testing: AE2_TEST_PULL=obj,who,from,dz keeps a monkey (object at obj, position +0x30) just
 * in front of a player, to test captures */
static void test_pull(void) {
    static int init; static uint32_t obj, who = 1, from; static float dz = -8.0f;
    if (!init) { init = 1; const char *e = getenv("AE2_TEST_PULL"); if (e) sscanf(e, "%x,%u,%u,%f", &obj, &who, &from, &dz); }
    extern uint32_t g_pad_polls;
    if (!obj || g_pad_polls < from || g_pad_polls > from + 300 || (who == 2 && !p2_live)) return;
    float x, y, z;
    if (who == 2) { x = rf(p2_pl, PL_POS); y = rf(p2_pl, PL_POS + 4); z = rf(p2_pl, PL_POS + 8); }
    else { x = gf(PL_BASE + PL_POS); y = gf(PL_BASE + PL_POS + 4); z = gf(PL_BASE + PL_POS + 8); }
    wf_g(obj + 0x30, x); wf_g(obj + 0x34, y); wf_g(obj + 0x38, z + dz);
}
/* AE2_TEST_VISIT=from[,step]: from game frame "from", every "step" frames (default 150)
 * player 2 is put on the next stage actor (monkeys aside) and stays near it; the actors'
 * list is logged before each visit ([visit]), to see which ones react to player 2 */
static void test_visit(void) {
    static int init; static uint32_t from, step = 150, cur, k; static float tx, ty, tz;
    if (!init) { init = 1; const char *e = getenv("AE2_TEST_VISIT"); if (e) sscanf(e, "%u,%u", &from, &step); }
    extern uint32_t g_pad_polls;
    if (!from || g_pad_polls < from || !p2_live || in_p2) return;
    if ((g_pad_polls - from) % step == step - 1 && cur) {   /* the visited actor's state at the end */
        uint64_t h = 1469598103934665603ull; for (uint32_t i = 0; i < 0x300; i++) h = (h ^ MR8(cur + i)) * 1099511628211ull;
        INFO("[visit] poll %u: actor %x state %016llx at %.1f %.1f %.1f; player 2 at %.1f %.1f %.1f\n", g_pad_polls, cur, (unsigned long long)h, gf(cur + 0x30), gf(cur + 0x34), gf(cur + 0x38),
             rf(p2_pl, PL_POS), rf(p2_pl, PL_POS + 4), rf(p2_pl, PL_POS + 8));
    }
    if ((g_pad_polls - from) % step == 0) {
        uint32_t n = 0, pick = 0, j = 0;
        INFO("[visit] poll %u: actors", g_pad_polls);
        for (uint32_t a = MR32(ACTOR_HEAD_T); a && n < 400; a = MR32(a + 152), n++) {
            INFO(" %x:%x/%x", a, MR32(a + 144), MR32(a + 148));
            if (MR32(a + 144) != 0x1ffbe8u && (gf(a + 0x30) != 0 || gf(a + 0x38) != 0) && j++ == k) pick = a;
        }
        INFO("\n");
        k++; cur = pick;
        if (pick) { tx = gf(pick + 0x30); ty = gf(pick + 0x34) + 5; tz = gf(pick + 0x38);
            INFO("[visit] poll %u: player 2 to actor %x (handler %x) at %.1f %.1f %.1f; player 1 at %.1f %.1f %.1f\n", g_pad_polls, pick, MR32(pick + 144), tx, ty, tz,
                 gf(PL_BASE + PL_POS), gf(PL_BASE + PL_POS + 4), gf(PL_BASE + PL_POS + 8)); }
    }
    if (!cur || (g_pad_polls - from) % step > 40) return;
    static int who = -1; if (who < 0) who = getenv("AE2_TEST_VISIT_P1") ? 1 : 2;
    for (int h = 0; h < 4; h++) { uint32_t o = h == 0 ? PL_POS : PL_HIST + (h - 1) * 16;
        if (who == 2) { wf(p2_pl, o, tx); wf(p2_pl, o + 4, ty); wf(p2_pl, o + 8, tz); } else { wf_g(PL_BASE + o, tx); wf_g(PL_BASE + o + 4, ty); wf_g(PL_BASE + o + 8, tz); } }
}
static void test_tele_env(const char *env) {
    static int init[2]; int k = env[13] == '2'; static uint32_t objs[2], whos[2] = {1, 1}, froms[2], untils[2] = {~0u, ~0u}, axes[2]; static float offs[2] = {15.0f, 15.0f};
    if (!init[k]) { init[k] = 1; const char *e = getenv(env); if (e) sscanf(e, "%x,%u,%u,%f,%u,%u", &objs[k], &whos[k], &froms[k], &offs[k], &untils[k], &axes[k]); }
    uint32_t obj = objs[k], who = whos[k], from = froms[k], until = untils[k], axis = axes[k]; float off = offs[k];
    extern uint32_t g_pad_polls;
    if (!obj || g_pad_polls < from || g_pad_polls > until || (who == 2 && !p2_live)) return;
    uint8_t buf[PL_SIZE], *b = who == 2 ? p2_pl : buf;
    if (who != 2) gread(buf, PL_BASE, PL_SIZE);
    float ox = gf(obj + 16), oy = gf(obj + 20), oz = gf(obj + 24);
    float dx = rf(b, PL_POS) - ox, dz = rf(b, PL_POS + 8) - oz;
    if (dx * dx + dz * dz < 30.0f * 30.0f) return;
    for (int h = 0; h < 4; h++) {
        uint32_t o = h == 0 ? PL_POS : PL_HIST + (h - 1) * 16;
        float d = who == 2 ? -off : off;
        wf(b, o, ox + (axis ? 0 : d)); wf(b, o + 4, oy); wf(b, o + 8, oz + (axis ? d : 0));
    }
    if (who != 2) gwrite(PL_BASE, buf, PL_SIZE);
}

/* The pass's changes to EE RAM are undone page by page: RAM is made read-only for the
 * pass, and the first write to a page (from any thread) saves the page and unlocks it.
 * A frame writes a few hundred pages, so this is much cheaper than copying all of RAM
 * twice a frame. */
#include "platform.h"
#define RAM_PAGES (EE_RAM_MAX / 4096)
static uint8_t *ram_save, ram_dirty[RAM_PAGES];
static uint32_t ram_list[RAM_PAGES]; static int ram_n, ram_guard;
/* pages are unlocked in blocks of 1 << ram_shift: writes cluster, so a bigger block means
 * fewer faults for a little more copying (AE2_P2RAMBLOCK = pages per block, a power of 2) */
static int ram_shift = 2;
static int ram_fault(void *addr) {
    uint8_t *a = (uint8_t *)addr;
    if (__atomic_load_n(&ram_guard, __ATOMIC_ACQUIRE) && a >= g_ram && a < g_ram + g_ram_size) {
        uint32_t pg = (uint32_t)((a - g_ram) >> 12) >> ram_shift << ram_shift, sz = 4096u << ram_shift;
        if (pg * 4096u + sz > g_ram_size) sz = g_ram_size - pg * 4096u;
        if (!__atomic_exchange_n(&ram_dirty[pg], 1, __ATOMIC_ACQ_REL)) {
            memcpy(ram_save + pg * 4096u, g_ram + pg * 4096u, sz);
            ram_list[__atomic_fetch_add(&ram_n, 1, __ATOMIC_ACQ_REL)] = pg;
        }
        plat_protect(g_ram + pg * 4096u, sz, 1);
        return 1;
    }
    return 0;   /* not ours */
}
static uint8_t *ram_full;   /* AE2_P2RAMCHECK=1: a full copy as well, to check the page-wise undo */
static int ram_check(void) { static int v = -1; if (v < 0) v = getenv("AE2_P2RAMCHECK") != NULL; return v; }
static void ram_protect(void) {
    if (!ram_save) {
        ram_save = malloc(g_ram_size);
        { const char *e = getenv("AE2_P2RAMBLOCK"); if (e) { int b = atoi(e), k = 0; while (k < 8 && (2 << k) <= b) k++; ram_shift = b > 0 ? k : 0; } }
        plat_set_write_fault_handler(ram_fault);
    }
    ram_n = 0;
    if (ram_check()) { static uint8_t *full; if (!full) full = malloc(g_ram_size); memcpy(full, g_ram, g_ram_size); ram_full = full; }
    __atomic_store_n(&ram_guard, 1, __ATOMIC_RELEASE);
    plat_protect(g_ram, g_ram_size, 0);
}
static void ram_restore(void) {
    plat_protect(g_ram, g_ram_size, 1);
    __atomic_store_n(&ram_guard, 0, __ATOMIC_RELEASE);
    for (int i = 0; i < ram_n; i++) {
        uint32_t pg = ram_list[i], sz = 4096u << ram_shift;
        if (pg * 4096u + sz > g_ram_size) sz = g_ram_size - pg * 4096u;
        memcpy(g_ram + pg * 4096u, ram_save + pg * 4096u, sz); ram_dirty[pg] = 0;
    }
    if (ram_check() && memcmp(g_ram, ram_full, g_ram_size)) { uint32_t i = 0; while (g_ram[i] == ram_full[i]) i++; WARN("[p2] RAM not restored at %07x\n", i); }
    if (getenv("AE2_P2LOG")) { static int k; if (k++ % 250 == 0) INFO("[p2] second view wrote %d blocks of %d pages\n", ram_n, 1 << ram_shift); }
}

#define GS_PAUSE  0x3b3600u   /* non-zero while the pause menu (and the like) is up: shown whole */
/* scenes shown whole: boss arenas ("BOS_*", whose camera frames the arena for both
 * players) and the travel station ("STA_A", a fixed camera that player 2's view can't
 * place) */
static int boss_arena(void) {
    uint32_t sc = MR32(GS_SCENE);
    if (sc >= 128) return 0;
    const char *code = (const char *)&g_ram[0x3b17a0u + sc * 64];
    static int boss_split = -1; if (boss_split < 0) boss_split = !getenv("AE2_P2BOSSSPLIT") || atoi(getenv("AE2_P2BOSSSPLIT"));
    return (!boss_split && !memcmp(code, "BOS", 3)) || !memcmp(code, "STA", 3);
}

static void p2_unpause(void);
void monkey_frame(Ctx *c);
void hle_stage_frame(Ctx *c) {
    monkey_frame(c);
    { static long at = -2; if (at == -2) at = getenv("AE2_TEST_HIT") ? atol(getenv("AE2_TEST_HIT")) : -1;   /* testing: an enemy's hit on player 1 */
      extern uint32_t g_pad_polls;
      if (at >= 0 && g_pad_polls == (uint32_t)at && p1_task) { Ctx s = *c; int32_t l = (int32_t)MR32(LIFE);
          c->r[4].sd[0] = (int32_t)p1_task; c->r[5].sd[0] = 8; c->r[6].sd[0] = 0; c->r[31].sd[0] = 0;
          hle_player_task(c); INFO("[hit] poll %u: test hit on player 1, health %d -> %d\n", g_pad_polls, l, (int32_t)MR32(LIFE)); *c = s; } }
    p2_unpause();
    if (getenv("AE2_P2LOG")) { static int l = -1; int v = MR8(PL_BASE + 882); extern uint32_t g_pad_polls; if (v != l) INFO("[p2] poll %u: player 1 lock %d\n", g_pad_polls, v); l = v; }
    split_list = split_next; split_next = 0;
    test_tele_env("AE2_TEST_TELE"); test_tele_env("AE2_TEST_TELE2"); test_pull(); test_visit();
    if (!split_on() || !p2_live || MR32(GS_MODE) != 3) { gs_split(0); f_0019CF28_orig(c); return; }
    /* not while the game is changing mode or scene: that frame starts loads that wait on
     * other threads, and the pass must not let them run (its memory is thrown away) */
    if ((int32_t)MR32(GS_REQMODE) != -1) { f_0019CF28_orig(c); return; }
    /* cut-scenes (player 1's control locked) are shown whole, and so are the boss
     * arenas (scene code "BOS"): their camera frames the arena the same for both players */
    if (MR8(PL_BASE + 882) || boss_arena() || MR32(GS_PAUSE)) { gs_split(0); f_0019CF28_orig(c); return; }
    /* half rate (debug menu, AE2_P2HALFRATE=1): every other frame skips the second-view
     * pass, the slow part of split screen, and the bottom half keeps the last one */
    { extern int g_p2_halfrate; static int odd;
      if (g_p2_halfrate < 0) g_p2_halfrate = getenv("AE2_P2HALFRATE") && atoi(getenv("AE2_P2HALFRATE"));
      if (g_p2_halfrate > 0 && (odd ^= 1)) {
          split_next = 1;
          cam2_frame = !getenv("AE2_P2CAMSTICK") || atoi(getenv("AE2_P2CAMSTICK"));
          f_0019CF28_orig(c);
          cam2_frame = 0;
          return;
      } }
    static uint8_t spr[16384], vu0[4096];
    static uint8_t s_pl[PL_SIZE], s_pad[PAD_SIZE], s_in[IN_SIZE], s_ex[NEXTRA][EX_MAX];
    Ctx sc = *c;
    ram_protect(); memcpy(spr, g_spr, sizeof spr); memcpy(vu0, g_vu0mem, sizeof vu0);
    memcpy(s_pl, p2_pl, PL_SIZE); memcpy(s_pad, p2_pad, PAD_SIZE); memcpy(s_in, p2_in, IN_SIZE); memcpy(s_ex, p2_ex, sizeof s_ex);
    /* in the pass the two players trade places, so the game's camera follows player 2,
     * and player 2's camera state (kept from the last pass) is put back */
    static uint8_t t[PL_SIZE > EX_MAX ? PL_SIZE : EX_MAX];
    veh_note(); uint8_t s_veh[VEH_N]; memcpy(s_veh, veh_who, sizeof s_veh);
    gread(t, PL_BASE, PL_SIZE); gwrite(PL_BASE, p2_pl, PL_SIZE); memcpy(p2_pl, t, PL_SIZE);
    gread(t, PAD_BASE, PAD_SIZE); gwrite(PAD_BASE, p2_pad, PAD_SIZE); memcpy(p2_pad, t, PAD_SIZE);
    gread(t, IN_BASE, IN_SIZE); gwrite(IN_BASE, p2_in, IN_SIZE); memcpy(p2_in, t, IN_SIZE);
    for (unsigned i = 0; i < NEXTRA; i++) { gread(t, extra[i].a, extra[i].n); gwrite(extra[i].a, p2_ex[i], extra[i].n); memcpy(p2_ex[i], t, extra[i].n); }
    if (have_cam2) gwrite(CAM_BASE, cam2, sizeof cam2);
    g_ghost = 1; g_pad_swap = 1;
    veh_show();
    f_0019CF28_orig(c);
    g_pad_swap = 0;
    gread(cam2, CAM_BASE, sizeof cam2); have_cam2 = 1; gread(mtx2, CAM_MTX, 64);
    gs_split(2);
    MW32(KICK_STATE + 4, 1);
    f_00193100_orig(c);   /* send the pass's list now */
    g_ghost = 0;
    ram_restore(); memcpy(g_spr, spr, sizeof spr); memcpy(g_vu0mem, vu0, sizeof vu0);
    memcpy(veh_who, s_veh, sizeof veh_who);
    memcpy(p2_pl, s_pl, PL_SIZE); memcpy(p2_pad, s_pad, PAD_SIZE); memcpy(p2_in, s_in, IN_SIZE); memcpy(p2_ex, s_ex, sizeof s_ex);
    p2_live = 1;
    *c = sc;
    /* the list the pass sent for the real frame stays sent: only its bookkeeping (swap buffers) */
    if (kicked_in_pass && MR32(KICK_STATE + 4)) { MW32(KICK_STATE, (MR32(KICK_STATE) + 1) & 1); MW32(KICK_STATE + 4, 0); }
    kicked_in_pass = 0;
    split_next = 1;
    cam2_frame = !getenv("AE2_P2CAMSTICK") || atoi(getenv("AE2_P2CAMSTICK"));
    f_0019CF28_orig(c);
    cam2_frame = 0;
}

/* 0x189ed8: the frame's hand-off to the sound thread (it waits for that thread, which
 * talks to the IOP), so the second-view pass skips it */
void f_00189ED8_orig(Ctx *c);
void hle_sound_frame(Ctx *c) {
    if (getenv("AE2_SNDQLOG")) { uint32_t gp = c->r[28].uw[0]; extern uint32_t g_pad_polls; static uint32_t n; if (n++ % 50 == 0) INFO("[sndq] poll %u gp %x state %d head %u tail %u busy %d\n", g_pad_polls, gp, MR32(gp - 1820), MR32(gp + 6056), MR32(gp + 6060), MR32(gp - 1824)); }
    if (!g_ghost) f_00189ED8_orig(c); }

void p2_diff_begin(void) { diff_begin(); }
void p2_diff_end(const char *w, uint32_t sp) { diff_end(w, sp); }

/* Top-level calls of the frame's draw list (rt_hw.c): AE2_P2CALLS=list of call numbers to
 * drop (testing which call draws what) */
extern int (*g_dma_top_call)(uint32_t tadr, uint32_t addr);
static uint32_t call_head; static int call_n;
#define HUD_CALL 8
static void gs_split(int mode);
static int top_call(uint32_t tadr, uint32_t addr) {
    uint32_t head = tadr & ~0xfffu;
    if (head != call_head || tadr - head < 0x6c0 + 16 * (uint32_t)call_n) { call_head = head; call_n = 0; }
    int i = (int)((tadr - (head + 0x6c0)) / 16); call_n = i + 1;
    const char *e = getenv("AE2_P2CALLS");
    if (e) { char k[8]; snprintf(k, sizeof k, ",%d,", i); char buf[64]; snprintf(buf, sizeof buf, ",%s,", e); if (strstr(buf, k)) return 0; }
    (void)addr;
    /* split screen: call 8 on is the HUD. Each view draws its own in its half (the
     * second view's shows player 2's gadgets), each piece kept by its screen edge */
    if (i >= HUD_CALL) {
        if (g_ghost && !real_kick) { gs_split(4); return 1; }   /* player 2's HUD in the bottom half */
        if (split_list) gs_split(3);              /* player 1's in the top half */
    }
    return 1;
}
static void __attribute__((constructor)) top_call_init(void) { g_dma_top_call = top_call; }

/* 0x348f28(data, name, type, group): adds a model to the game's model registry (fails
 * with -2 when the name is taken) */
void f_00348F28_orig(Ctx *c);
void hle_model_register(Ctx *c) {
    if (getenv("AE2_P2REGLOG")) {
        char nm[17] = {0}; memcpy(nm, c->r[5].ub, 16);
        char st[200] = ""; int k = 0;
#ifdef AE2_FPCHECK
        for (int d = g_fdepth - 1; d >= 0 && d >= g_fdepth - 10; d--) if (d < 256) k += snprintf(st + k, sizeof st - (size_t)k, " <%06x", g_fstack[d]);
#endif
        (void)k; INFO("[reg] '%s' data %08x type %d group %d%s\n", nm, c->r[4].uw[0], c->r[6].sw[0], c->r[7].sw[0], st);
    }
    if (kakeru_loading) {   /* player 2's models go in under their own names: "player" -> "player_k" (or "_h") */
        char nm[17] = {0}; memcpy(nm, c->r[5].ub, 16);
        size_t n = strlen(nm); if (n <= 13) { nm[n] = '_'; nm[n + 1] = (char)kakeru_loading; memcpy(c->r[5].ub, nm, 16); }
    }
    f_00348F28_orig(c);
}

/* ---- Player 2 as Kakeru (AE2_P2KAKERU=1)
 * A character is the archive HIKARU or KAKERU; loading it registers its models as
 * "player" and "player_hair". Player 2's archive is loaded the same way, with its models
 * renamed (hle_model_register), and player 2's block is pointed at them. */
void f_00193DF0(Ctx *c); void f_00351B90(Ctx *c); void f_00351BC0(Ctx *c); void f_00193D78(Ctx *c);
void f_00351BA8(Ctx *c); void f_00349348(Ctx *c); void f_001938F0(Ctx *c); void f_001B6D30(Ctx *c); void f_001A4DF8(Ctx *c);
static void name128(r128 *r, const char *s) { memset(r, 0, sizeof *r); memcpy(r->ub, s, strlen(s)); }
static int model_find(Ctx *c, const char *name) {   /* 0x349348: model index by name, -1 if none */
    name128(&c->r[4], name); f_00349348(c); return c->r[2].sw[0];
}
/* ---- player 2's voice. The game's sound banks sit in eight slots (0x3eb910, 848 bytes
 * each: name, SEH_/SEB_ names, sound RAM address at +48, driver handle at +836); slot 3
 * holds the player character's bank (Hikaru's or Kakeru's voice and sounds). The sound
 * RAM is full, so the emulated one is twice the PS2's (spu2.c) and the other brother's
 * bank is loaded once, through slot 7 (unused in stages) borrowed for the moment, above
 * the PS2's 2 MB. Sounds that player 2's frame plays from slot 3 then come from it. */
#define SND_SLOTS 0x3eb910u
#define SND_SLOT  848u
#define P2_VOICE_SPU 0x200000u
static int p2_voice = -1;
#define P2_GROUP  0x800000   /* load group for player 2's character (the game's own character uses 0x200000) */
void f_00351B90(Ctx *c); void f_00351BC0(Ctx *c); void f_00351BA8(Ctx *c); void f_001938F0(Ctx *c);
void f_001B90A8(Ctx *c); void f_001B9068(Ctx *c);
/* the bank is still registered with the sound driver (state 3) and its header is intact */
static int voice_ok(void) {
    if (p2_voice <= 0 || p2_voice >= 128) return 0;
    uint32_t e = 0x610900u + 12u * (uint32_t)p2_voice, hd = MR32(e);
    return MR32(e + 8) == 3 && RAMOK(hd + 16) && MR32(hd + 12) == 0x64685353u && (MR32(e + 4) << 3 == P2_VOICE_SPU || MR32(e + 4) << 4 == P2_VOICE_SPU);
}
static void voice_setup(Ctx *c, const char *arch) {
    static int tries;
    if (voice_ok()) return;
    if (p2_voice >= 0) { INFO("[p2] %s's voice bank %d was released; loading it again\n", arch, p2_voice); p2_voice = -1; }
    if (getenv("AE2_P2VOICE") && !atoi(getenv("AE2_P2VOICE"))) return;
    if (tries >= 8) return;   /* checked on each join; a bank that never loads is given up on */
    tries++;
    uint32_t s7 = SND_SLOTS + 7 * SND_SLOT, s3 = SND_SLOTS + 3 * SND_SLOT;
    static uint8_t keep[SND_SLOT], rec[SND_SLOT];
    gread(keep, s7, SND_SLOT); gread(rec, s3, SND_SLOT);
    gwrite(s7, rec, SND_SLOT);
    MW32(s7 + 836, 0xffffffffu);
    Ctx sv = *c;
    c->r[4].sd[0] = 7; name128(&c->r[5], arch); f_001B90A8(c);
    MW32(s7 + 48, P2_VOICE_SPU);
    /* in player 2's own load group, and hidden from the archive table afterwards, so
     * that stage loads leave the bank's header (SEH_*) in place, as for the models */
    f_00351B90(c); c->r[4].sd[0] = P2_GROUP; f_00351BC0(c);
    c->r[4].sd[0] = 7; c->r[5].sd[0] = 1; f_001B9068(c);
    f_00351BA8(c);
    int h = (int)MR32(s7 + 836);
    { char n[16]; snprintf(n, sizeof n, "SEH_%s", arch); name128(&c->r[4], n); f_001938F0(c); if (c->r[2].uw[0]) MW8(c->r[2].uw[0] + 80, 0); }
    gwrite(s7, keep, SND_SLOT);
    *c = sv;
    if (h >= 0) p2_voice = h;
    INFO("[p2] %s's voice bank: handle %d\n", arch, h);
}
/* 0x1b8bf8(id): plays sound effect id (slot id >> 11, sound id & 0x7ff) */
void f_001B8BF8_orig(Ctx *c);
void hle_sound_play(Ctx *c) {
    int32_t id = c->r[4].sw[0];
    uint32_t h3 = SND_SLOTS + 3 * SND_SLOT + 836;
    { static int lg = -1; if (lg < 0) lg = getenv("AE2_SNDLOG") != NULL;
      if (lg && (id >> 11) == 3) { extern uint32_t g_pad_polls; INFO("[snd] poll %u player sound %x in_p2 %d ghost %d from %x\n", g_pad_polls, id, in_p2, g_ghost, c->r[31].uw[0]); } }
    if (!in_p2 || g_ghost || p2_voice < 0 || id < 0 || (id >> 11) != 3) {
        f_001B8BF8_orig(c);
        return; }
    uint32_t mine = MR32(h3);
    MW32(h3, (uint32_t)p2_voice);
    f_001B8BF8_orig(c);
    MW32(h3, mine);
    if (getenv("AE2_P2LOG")) INFO("[p2] player 2 sound %x -> %d (bank %d)\n", id, c->r[2].sw[0], p2_voice);
}

/* The game's heap (0x1a4aa0 allocates from it, 0x1a4df8 frees): blocks of {next, prev,
 * size, used} after a 64-byte header, in a ring from +64. Nothing in the game checks for
 * a failed allocation, and a stage archive that does not fit is decompressed to address 0
 * (which overwrites the kernel's thread records and hangs the game). */
#define HEAP_PTR (0x535af0u - 1376)
#define P2_BODY_GROUP 0xc00000   /* load group for player 2's models (P2_GROUP keeps the voice bank) */
static int kakeru_in;            /* player 2's character archive is in memory */
static const char *kakeru_arch;
static uint32_t heap_largest(void) {
    uint32_t h = MR32(HEAP_PTR), head = h + 64, big = 0, n = 0;
    for (uint32_t b = MR32(head); b != head && b && n < 100000; b = MR32(b), n++)
        if (!MR32(b + 12) && MR32(b + 8) > big) big = MR32(b + 8);
    return big;
}
static uint32_t arch_entry(Ctx *c, const char *name) { name128(&c->r[4], name); c->r[5].sd[0] = 0; f_001938F0(c); return c->r[2].uw[0]; }
static uint32_t arch_need(uint32_t e) { return e ? ((MR32(e + 84) + 2047) & ~2047u) + 64 : 0; }
/* takes player 2's character out of memory again (models, textures and the archive);
 * player 2 then looks like player 1 until there is room again */
static void kakeru_unload(Ctx *c) {
    if (!kakeru_in) return;
    Ctx sv = *c;
    c->r[4].sd[0] = P2_BODY_GROUP; f_001B6D30(c);
    uint32_t e = arch_entry(c, kakeru_arch);
    if (e && MR32(e + 92)) {
        c->r[4].sd[0] = (int32_t)MR32(e + 92); c->r[5].sd[0] = (int32_t)MR32(HEAP_PTR); f_001A4DF8(c);
        MW32(e + 92, 0); MW8(e + 80, 0);
    }
    *c = sv;
    kakeru_in = 0;
    if (!in_p2) {   /* the stashed player 2 goes back to player 1's models */
        memcpy(p2_pl + 848, g_ram + PL_BASE + 848, 16);
        memcpy(p2_pl + 80 + 68, g_ram + PL_BASE + 80 + 68, 4);
        memcpy(p2_pl + 844, g_ram + PL_BASE + 844, 4);
    }
    if (getenv("AE2_P2LOG")) INFO("[p2] %s's models unloaded for the stage load (heap: largest free %x)\n", kakeru_arch, heap_largest());
}
/* 0x193be0(name, heap, ., top): loads an archive. Archives load when a stage does, and the
 * stage then gets the heap as the game alone would leave it: player 2's character steps
 * aside first (its room would otherwise split the free space, and the Time Station's
 * 4.6 MB archive did not fit after some stages), and comes back when player 2 joins, at
 * the top of the heap, if it fits then. With more RAM than the PS2's (the default), or
 * AE2_P2KEEP=1, it stays loaded unless an archive would not fit. */
static int p2_setup_busy;
void f_00193BE0_orig(Ctx *c);
/* with more than the PS2's 32 MB (AE2_RAM_MB) there is room, so the character stays */
static int p2_keep(void) {
    static int v = -1;
    if (v < 0) v = getenv("AE2_P2KEEP") ? atoi(getenv("AE2_P2KEEP")) != 0 : g_ram_size > (32u << 20);
    return v;
}
void hle_archive_load(Ctx *c) {
    if (kakeru_in && !p2_setup_busy && c->r[5].uw[0] == MR32(HEAP_PTR) && !c->r[7].sw[0] && !p2_keep()) kakeru_unload(c);
    if (getenv("AE2_P2LOG")) { extern uint32_t g_pad_polls; char nm[17] = {0}; memcpy(nm, c->r[4].ub, 16);
        INFO("[p2] poll %u: archive %s heap %x top %d (largest free %x)\n", g_pad_polls, nm, c->r[5].uw[0], c->r[7].sw[0], heap_largest()); }
    if (kakeru_in && !p2_setup_busy && c->r[5].uw[0] == MR32(HEAP_PTR)) {
        Ctx sv = *c;
        c->r[5].sd[0] = 0; f_001938F0(c); uint32_t need = arch_need(c->r[2].uw[0]);
        *c = sv;
        if (need > heap_largest()) kakeru_unload(c);
    }
    f_00193BE0_orig(c);
}

static void kakeru_setup2(Ctx *c);
static void kakeru_setup(Ctx *c) { p2_setup_busy = 1; kakeru_setup2(c); p2_setup_busy = 0; }
static void kakeru_setup2(Ctx *c) {
    if (!env_or_mode("AE2_P2KAKERU")) return;
    /* player 2 is the other brother: Kakeru, or Hikaru when player 1 is Kakeru (new game+) */
    int k1 = MR32(SAVE_CHAR) >= 3;
    const char *arch = k1 ? "HIKARU" : "KAKERU", *bname = k1 ? "player_h" : "player_k", *hname = k1 ? "player_hair_h" : "player_hair_k";
    kakeru_arch = arch;
    Ctx sv = *c;
    int body = model_find(c, bname);
    /* room for the archive, and some to spare for what the stage allocates later */
    uint32_t need = arch_need(arch_entry(c, arch)), spare = 0x40000;
    if (getenv("AE2_P2SPARE")) spare = (uint32_t)strtoul(getenv("AE2_P2SPARE"), 0, 0);
    if (body < 0 && need && heap_largest() < need + spare) {
        INFO("[p2] no room for %s's models here (needs %x, largest free %x)\n", arch, need + spare, heap_largest());
        *c = sv; voice_setup(c, arch); return;
    }
    if (body < 0) {
        /* load KAKERU the way the stage loader loads a character, but in a load group of
         * its own and then hidden from the archive table: the stage loader unloads "the
         * other character" (and every model of the character group) when a stage loads,
         * and must not take Hikaru's models or these with it. Kept until a stage needs
         * the room (hle_archive_load). */
        f_00351B90(c);
        c->r[4].sd[0] = P2_BODY_GROUP; f_00351BC0(c);
        name128(&c->r[4], arch); c->r[5].sd[0] = (int32_t)MR32(0x535af0u - 1376);
        kakeru_loading = k1 ? 'h' : 'k'; f_00193D78(c); kakeru_loading = 0;
        f_00351BA8(c);
        name128(&c->r[4], arch); f_001938F0(c);
        if (c->r[2].uw[0]) MW8(c->r[2].uw[0] + 80, 0);
        body = model_find(c, bname);
    }
    if (body >= 0) kakeru_in = 1;
    int hair = model_find(c, hname);
    *c = sv;
    INFO("[p2] %s's models: body %d hair %d\n", k1 ? "Hikaru" : "Kakeru", body, hair);
    voice_setup(c, arch);
    if (body < 0) return;
    /* the body is looked up every frame by the name at +848 (0x29d208) */
    char nm[16] = {0}; memcpy(nm, bname, strlen(bname)); memcpy(p2_pl + 848, nm, 16);
    uint32_t v = (uint32_t)body; memcpy(p2_pl + 80 + 68, &v, 4);
    if (hair >= 0) { v = (uint32_t)hair; memcpy(p2_pl + 844, &v, 4); }
}

/* 0x29cbe8: sets up the player when a stage loads. Player 2 joins again afterwards (its
 * old copy belongs to the previous stage, and its models may have been unloaded). */
void f_0029CBE8_orig(Ctx *c);
extern volatile int g_p2_paused;
void hle_player_init(Ctx *c) {
    if (g_p2_paused) { swap_out(); g_p2_paused = 0; }   /* left the stage from player 2's pause menu */
    if (!in_p2 && !g_ghost) p2_live = 0;
    f_0029CBE8_orig(c);
}

/* ------------------------------------------------------------------ title menu
 * The title menu (state 2 of the title, 0x54b850) lists New game, Continue and Options
 * from a table of {word, height, enabled} at 0x3ed5e8; 0x1bfa60 moves the cursor
 * (0x54b87c) and stores the chosen word at +32, 0x1bfbd8 draws the words from the
 * texture 2d_title_slct, whose fourth word (Quit game) the PS2 menu never uses. Both
 * get a four-row table here, and gs.c draws "2 Players: Off/On" over that fourth word.
 * Choosing it switches the 2-player mode: player 2, split screen, Kakeru and a memory
 * card of its own (iop_memcard.c), for New game and Continue alike. */
#include "title_words.h"
#define TITLE      0x54b850u
#define TITLE_CUR  0x54b87cu
#define TITLE_TAB  0x3ed5e8u
void gs_titlepatch_set(const uint8_t *row);
static void title_table(uint32_t at) {
    for (uint32_t i = 0; i < 36; i += 4) MW32(at + i, MR32(TITLE_TAB + i));
    float y1 = gf(TITLE_TAB + 16), y2 = gf(TITLE_TAB + 28);
    MW32(at + 36, 3); wf_g(at + 40, y2 + (y2 - y1)); MW32(at + 44, 1);
}
static void title_call(Ctx *c, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2) {
    c->r[4].sd[0] = (int32_t)a0; c->r[5].sd[0] = (int32_t)a1; c->r[6].sd[0] = (int32_t)a2;
    rt_call(c, fn);
}
static void title_word(void) { gs_titlepatch_set(g_p2_mode ? tw_2p_on : tw_2p_off); }
/* 0x1c0368: the menu's input, as the game's with a fourth row */
void hle_title_menu(Ctx *c) {
    uint32_t sp = c->r[29].uw[0], ra = c->r[31].uw[0], gp = c->r[28].uw[0];
    title_word();
    c->r[29].sd[0] = (int32_t)(sp - 64);
    title_table(sp - 64);
    c->f[12].f = gf(0x3b0000u + 13924) * gf(gp - 27996) + 0.5f; c->f[13].f = gf(gp - 27992);
    title_call(c, 0x1bfa60u, TITLE_CUR, sp - 64, 4);
    c->r[29].sd[0] = (int32_t)sp;
    if (MR32(IN_BASE + 4) & 0x6008) {             /* circle, cross or start pressed */
        title_call(c, 0x1b8cc8u, 2, 0, 0);
        uint32_t word = MR32(TITLE + 32);
        int state = -1;
        switch (word) {
        case 0: if (MR32(IN_BASE) & 0x400) state = 9; else { MW32(TITLE + 36, 1); state = 7; } break;
        case 1: MW32(TITLE + 36, 2); state = 7; break;
        case 2: MW32(TITLE + 36, 3); state = 7; break;
        case 3: g_p2_mode ^= 1; title_word(); INFO("[p2] 2-player mode %s\n", g_p2_mode ? "on" : "off"); break;
        }
        if (state >= 0) title_call(c, 0x1bf5d8u, (uint32_t)state, 0, 0);
    }
    c->r[31].uw[0] = ra;
}
/* 0x1c04c0: the menu's words */
void hle_title_menu_draw(Ctx *c) {
    uint32_t sp = c->r[29].uw[0], ra = c->r[31].uw[0], gp = c->r[28].uw[0];
    title_word();
    c->r[29].sd[0] = (int32_t)(sp - 64);
    title_table(sp - 64);
    c->f[12].f = gf(0x3b0000u + 13924) * gf(gp - 27988) + 0.5f; c->f[13].f = gf(gp - 27984);
    title_call(c, 0x1bfbd8u, TITLE_CUR, sp - 64, 4);
    c->r[29].sd[0] = (int32_t)sp;
    c->r[31].uw[0] = ra;
}

/* ---- bosses: double health in 2-player mode ----
 * Every boss takes damage through 0x138ff0(boss, n): life (boss+84, 100 when
 * full) -= n, and below 6 the boss is beaten.  In 2-player mode each hit counts
 * half, so a boss lasts twice as many hits; a blow of 100 or more is the game's
 * own "finish him now" and goes through as it is. */
void f_00138FF0_orig(Ctx *c);
void hle_boss_damage(Ctx *c) {
    uint32_t boss = c->r[4].uw[0];
    int32_t n = c->r[5].sw[0], before = (int32_t)MR32(boss + 84);
    if (p2_enabled() && n > 0 && n < 100) n = (n + 1) / 2;
    c->r[5].sd[0] = n;
    f_00138FF0_orig(c);
    if (getenv("AE2_P2LOG")) { extern uint32_t g_pad_polls; INFO("[p2] poll %u: boss %08x hit %d: life %d -> %d%s\n", g_pad_polls, boss, n, before, (int32_t)MR32(boss + 84), g_ghost ? " (ghost)" : ""); }
}

/* 0x19c8f8: opens the pause menu when Start is pressed on the first pad (and the player
 * may pause). Player 2's Start opens it too, with player 2 swapped in for as long as the
 * menu is open: the menu then shows and changes player 2's gadgets, and reads the second
 * pad (hle_dbc_read). The menu sets the pause flag back to 0 when it closes. */
void f_0019C8F8_orig(Ctx *c);
volatile int g_p2_paused;
static void p2_unpause(void) {
    if (!g_p2_paused || MR32(GS_PAUSE)) return;
    swap_out(); g_p2_paused = 0;
}
void hle_pause_check(Ctx *c) {
    p2_unpause();
    r128 a0 = c->r[4];
    f_0019C8F8_orig(c);
    if (c->r[2].uw[0] || !p2_live || in_p2 || g_ghost || g_p2_paused) return;
    swap_in();
    c->r[4] = a0;
    f_0019C8F8_orig(c);
    if (c->r[2].uw[0] && MR32(GS_PAUSE)) { g_p2_paused = 1; INFO("[p2] player 2 paused\n"); return; }
    swap_out();
}


/* monkey sensors whose list follows player 2 this frame */
static uint32_t sense_p2[128]; static int n_sense_p2;
static void sense_mark(uint32_t s, int p2) {
    int i; for (i = 0; i < n_sense_p2 && sense_p2[i] != s; i++) {}
    if (p2 && i == n_sense_p2 && n_sense_p2 < 128) sense_p2[n_sense_p2++] = s;
    if (!p2 && i < n_sense_p2) sense_p2[i] = sense_p2[--n_sense_p2];
}
static int sense_is_p2(uint32_t s) { for (int i = 0; i < n_sense_p2; i++) if (sense_p2[i] == s) return 1; return 0; }
/* 0x1e4338(sensor): a monkey's senses. It lists what the monkey can notice at sensor+80
 * (48-byte entries, count at +276): first the player, which is always player 0 (0x2c43a0,
 * 0x2c4508), then gadgets. It runs again with player 2 in place, and the list from the
 * brother nearer the monkey (distance at entry +4) is kept, so monkeys notice either one. */
void f_001E4338_orig(Ctx *c);
void hle_monkey_sense(Ctx *c) {
    static int on = -1; if (on < 0) on = !getenv("AE2_P2SENSE") || atoi(getenv("AE2_P2SENSE"));
    r128 a[4] = {c->r[4], c->r[5], c->r[6], c->r[7]};
    f_001E4338_orig(c);
    if (!on || in_p2 || !p2_live || g_p2_paused) return;
    uint32_t s = a[0].uw[0];
    uint8_t one[200]; gread(one, s + 80, sizeof one);
    r128 v0 = c->r[2];
    swap_in();
    for (int i = 0; i < 4; i++) c->r[4 + i] = a[i];
    f_001E4338_orig(c);
    swap_out();
    c->r[2] = v0;
    float d1 = rf(one, 4), d2 = gf(s + 84);
    if (!(d2 < d1)) gwrite(s + 80, one, sizeof one);
    sense_mark(s, d2 < d1);
    { static uint32_t w = 1; if (w == 1) { const char *e = getenv("AE2_P2SENSELOG"); w = e ? (uint32_t)strtoul(e, 0, 16) : 0; }
      if (w == s || (w == 0xffffffffu && !g_ghost)) { extern uint32_t g_pad_polls; INFO("[sense] %08x poll %u ghost %d d1 %.1f d2 %.1f +352 %d +360 %d n %d\n", s, g_pad_polls, g_ghost, d1, d2, MR32(s + 352), MR32(s + 360), MR32(s + 276)); } }
    if (d2 < d1 && getenv("AE2_P2LOG")) { extern uint32_t g_pad_polls; static uint32_t last; if (g_pad_polls != last) { last = g_pad_polls; if (g_pad_polls % 25 == 0) INFO("[p2] poll %u: monkey sensor %08x follows player 2 (%.0f vs %.0f)\n", g_pad_polls, s, d2, d1); } }
}

/* 0x1e45f8(sensor, ...): whether the monkey sees what its list holds: a ray from its eyes
 * that ignores the player's own body (0x2c78f8, player 0). For a list that follows player 2
 * it runs with player 2 in place, so Kakeru's body does not hide Kakeru. */
void f_001E45F8_orig(Ctx *c);
void hle_monkey_sight(Ctx *c) {
    { uint32_t sn = c->r[4].uw[0]; static uint32_t w = 1; if (w == 1) { const char *e = getenv("AE2_P2SENSELOG"); w = e ? (uint32_t)strtoul(e, 0, 16) : 0; }
      if (w == sn && !g_ghost) { extern uint32_t g_pad_polls; uint32_t l = MR32(sn + 272), pr = MR32(sn); INFO("[sight] poll %u p2 %d dist %.1f range %.1f +360 %d n %d type %d\n", g_pad_polls, sense_is_p2(sn), gf(l + 4), gf(pr + 304), MR32(sn + 360), MR32(sn + 276), MR32(l + 8)); } }
    if (in_p2 || !p2_live || !sense_is_p2(c->r[4].uw[0])) { f_001E45F8_orig(c); return; }
    swap_in();
    f_001E45F8_orig(c);
    swap_out();
    if (getenv("AE2_P2LOG") && c->r[2].uw[0]) { extern uint32_t g_pad_polls; INFO("[p2] poll %u: monkey sensor %08x sees player 2\n", g_pad_polls, c->r[4].uw[0]); }
}

/* 0x270a00: the cameras' frame step; returns the camera the view is shot from (camera
 * manager 0x4d1af0) */
void f_00270A00_orig(Ctx *c);
void hle_camera_update(Ctx *c) {
    uint32_t ra = c->r[31].uw[0];
    f_00270A00_orig(c);
    if (getenv("AE2_P2CAMLOG")) { extern uint32_t g_pad_polls; INFO("[camupd] poll %u ra %x ghost %d in_p2 %d -> %d mgr %d %d %d %d\n", g_pad_polls, ra, g_ghost, in_p2, c->r[2].sw[0], MR32(0x4d1af0), MR32(0x4d1af4), MR32(0x4d1af8), MR32(0x4d1afc)); }
}

/* ------------------------------------------------------------------ stage objects
 * The stage's objects (items, switches, platforms, enemies, ...) are actors in one list
 * (gp+6620; +0x30 position, +144 handler, +148 flags, +152 next), and each frame
 * 0x1dc148 sends them message 5 (their step) and 0x1dc210 message 7. They find "the
 * player" at the one fixed block (through the 0x2c4xxx accessors), so they only ever
 * reacted to player 1. Here each actor that is nearer to player 2 runs its handler with
 * player 2 swapped into that block, the way player 2's own steps run: a switch Kakeru
 * stands on, an item he touches, an enemy next to him all see him. Monkeys keep their
 * own handling (hle_monkey_sense). Not while player 1 is held still (cut-scenes) or
 * player 2's pause menu is up. AE2_P2OBJECTS=0: player 1 only, as before. */
#define ACTOR_HEAD (0x535af0u + 6620u)
#define MONKEY_STEP 0x1ffbe8u
/* Bosses: each boss is driven by a controller actor (its own handler per boss), and the
 * shared per-frame boss routine 0x137258(boss) runs inside that actor's messages; the boss
 * itself is at +16. hle_boss_frame notes which boss each controller drives, so the choice
 * of player for a controller is made from the boss's position, and the boss goes after
 * the nearer brother. */
static uint32_t cur_actor;   /* the actor whose message is running */
#define NBOSS 8
static struct { uint32_t actor, boss; } bossmap[NBOSS]; static int nboss; static uint32_t boss_scene = ~0u;
void f_00137258_orig(Ctx *c);
void hle_boss_frame(Ctx *c) {
    uint32_t b = c->r[4].uw[0], sc = MR32(GS_SCENE);
    if (sc != boss_scene) { boss_scene = sc; nboss = 0; }
    if (cur_actor) {
        int i; for (i = 0; i < nboss && bossmap[i].actor != cur_actor; i++) {}
        if (i == nboss && nboss < NBOSS) { bossmap[nboss].actor = cur_actor; bossmap[nboss].boss = b; nboss++;
            INFO("[p2] boss %08x is driven by actor %08x (handler %06x)\n", b, cur_actor, MR32(cur_actor + 144)); }
    }
    { static int lg = -1; if (lg < 0) lg = getenv("AE2_P2BOSSLOG") != NULL; extern uint32_t g_pad_polls;
      if (lg && !g_ghost && g_pad_polls % 25 == 0) INFO("[bosslog] poll %u boss %08x at %.0f %.0f %.0f; player 1 %.0f %.0f %.0f, player 2 %.0f %.0f %.0f; runs with player %d\n", g_pad_polls, b,
          gf(b + 16), gf(b + 20), gf(b + 24), in_p2 ? rf(sv_pl, PL_POS) : gf(PL_BASE + PL_POS), in_p2 ? rf(sv_pl, PL_POS + 4) : gf(PL_BASE + PL_POS + 4), in_p2 ? rf(sv_pl, PL_POS + 8) : gf(PL_BASE + PL_POS + 8),
          in_p2 ? gf(PL_BASE + PL_POS) : rf(p2_pl, PL_POS), in_p2 ? gf(PL_BASE + PL_POS + 4) : rf(p2_pl, PL_POS + 4), in_p2 ? gf(PL_BASE + PL_POS + 8) : rf(p2_pl, PL_POS + 8), in_p2 ? 2 : 1); }
    f_00137258_orig(c);
}
static uint32_t boss_of(uint32_t a) {
    if (MR32(GS_SCENE) != boss_scene) return 0;
    for (int i = 0; i < nboss; i++) if (bossmap[i].actor == a) return bossmap[i].boss;
    return 0;
}
/* Which brother an actor sees. The nearer one, but an actor keeps the brother it saw last
 * while he is within touching distance of it (a switch or platform he stands on, an item
 * he is next to) or while the other one is not clearly nearer, so that an object between
 * the two does not swap players every frame. Bosses swap only when the other brother is
 * clearly nearer. */
#define TOUCH_DIST 6.0f
#define STICK_TAB 1024
static struct { uint32_t a, poll; uint8_t who; } stick[STICK_TAB];
static int actor_wants_p2(uint32_t a, int msg) {
    static int on = -1; if (on < 0) on = !getenv("AE2_P2OBJECTS") || atoi(getenv("AE2_P2OBJECTS"));
    if (!on || !p2_live || in_p2 || g_p2_paused || MR32(GS_MODE) != 3 || MR8(PL_BASE + 882)) return 0;
    if (MR32(a + 144) == MONKEY_STEP) return 0;
    static int bosses = -1; if (bosses < 0) bosses = !getenv("AE2_P2BOSS") || atoi(getenv("AE2_P2BOSS"));
    uint32_t boss = bosses ? boss_of(a) : 0;
    if (msg == 4 && !boss) return 0;                  /* message 4: only for boss controllers */
    uint32_t pa = boss ? boss + 16 : a + 0x30;
    float ax = gf(pa), ay = gf(pa + 4), az = gf(pa + 8);
    if (ax == 0 && ay == 0 && az == 0) return 0;      /* no place of its own: a manager */
    float dx = gf(PL_BASE + PL_POS) - ax, dy = gf(PL_BASE + PL_POS + 4) - ay, dz = gf(PL_BASE + PL_POS + 8) - az;
    float ex = rf(p2_pl, PL_POS) - ax, ey = rf(p2_pl, PL_POS + 4) - ay, ez = rf(p2_pl, PL_POS + 8) - az;
    float d1 = dx * dx + dy * dy + dz * dz, d2 = ex * ex + ey * ey + ez * ez;
    extern uint32_t g_pad_polls;
    uint32_t h = (a >> 4) * 2654435761u >> 22;        /* STICK_TAB = 1 << 10 */
    int prev = stick[h].a == a && g_pad_polls - stick[h].poll <= 2 ? stick[h].who : 0;
    int p2;
    if (!prev) p2 = d2 < d1;
    else {
        float dp = prev == 2 ? d2 : d1, dn = prev == 2 ? d1 : d2;
        int keep = boss ? !(dn < 0.64f * dp)           /* the other one 20% nearer */
                        : dp < TOUCH_DIST * TOUCH_DIST || !(dn < 0.81f * dp);
        p2 = keep ? prev == 2 : prev != 2;
    }
    if (!g_ghost) { stick[h].a = a; stick[h].poll = g_pad_polls; stick[h].who = (uint8_t)(p2 ? 2 : 1); }
    return p2;
}
static uint32_t actor_call(Ctx *c, uint32_t a, int msg) {
    int p2 = actor_wants_p2(a, msg);
    if (p2 && getenv("AE2_P2LOG")) { extern uint32_t g_pad_polls; static uint32_t last_a, last_p;
        if (getenv("AE2_P2OBJLOG") && (a != last_a || g_pad_polls - last_p > 100)) INFO("[p2] poll %u: actor %x (handler %x) message %d sees player 2\n", g_pad_polls, a, MR32(a + 144), msg);
        last_a = a; last_p = g_pad_polls; }
    /* camera changes the object makes for player 2 (a fixed camera by a pole or on a
     * conveyor, say) go to player 2's own view in split screen, and are dropped with the
     * shared camera, which stays player 1's */
    static uint8_t camsv[CAM_END - CAM_BASE], mtxsv[64]; int keep = 0;
    if (p2) { swap_in(); if (cam2_frame || g_ghost) cam_in(); else { gread(camsv, CAM_BASE, sizeof camsv); gread(mtxsv, CAM_MTX, 64); keep = 1; } }
    c->r[4].sd[0] = (int32_t)a; c->r[5].sd[0] = msg; memset(&c->r[6], 0, sizeof c->r[6]);
    int32_t st0 = (int32_t)MR32(PL_BASE + 868), lf0 = (int32_t)MR32(LIFE);
    uint32_t outer = cur_actor; cur_actor = a;
    rt_call(c, MR32(a + 144));
    cur_actor = outer;
    static int dmglog = -1; if (dmglog < 0) dmglog = getenv("AE2_DMGLOG") != NULL;
    if (dmglog && ((int32_t)MR32(PL_BASE + 868) != st0 || (int32_t)MR32(LIFE) != lf0)) { extern uint32_t g_pad_polls; INFO("[dmg] poll %u: actor %x (handler %x) message %d, player %d: state %d -> %d, health %d -> %d\n", g_pad_polls, a, MR32(a + 144), msg, p2 + 1, st0, MR32(PL_BASE + 868), lf0, MR32(LIFE)); }
    if (p2) { if (keep) { gwrite(CAM_BASE, camsv, sizeof camsv); gwrite(CAM_MTX, mtxsv, 64); } else cam_out(); swap_out(); }
    return c->r[2].uw[0];
}
void f_001DBF08(Ctx *c);
/* 0x1dc148(only, skip): message 5 to the actors whose mask (+112) has a bit of "only"
 * (if given) and none of "skip" (if given), and whose flags (+148) are clear */
void hle_actors_step(Ctx *c) {
    r128 s0 = c->r[16], s1 = c->r[17], s2 = c->r[18], s3 = c->r[19], s4 = c->r[20];
    uint32_t only = c->r[4].uw[0], skip = c->r[5].uw[0];
    { static long lp = -2; extern uint32_t g_pad_polls; if (lp == -2) lp = getenv("AE2_ACTORLIST") ? atol(getenv("AE2_ACTORLIST")) : -1;   /* testing */
      if (lp >= 0 && g_pad_polls >= (uint32_t)lp) { lp = -1; for (uint32_t a = MR32(ACTOR_HEAD); a; a = MR32(a + 152))
          INFO("[actor] %08x handler %06x mask %x flags %x pos %.0f %.0f %.0f\n", a, MR32(a + 144), MR32(a + 112), MR32(a + 148), gf(a + 0x30), gf(a + 0x34), gf(a + 0x38)); } }
    for (uint32_t a = MR32(ACTOR_HEAD), next; a; a = next) {
        next = MR32(a + 152);
        if (only && !(MR32(a + 112) & only)) continue;
        if (skip && (MR32(a + 112) & skip)) continue;
        if (MR32(a + 148) & 3) continue;
        if (actor_call(c, a, 5) == 1) { c->r[4].sd[0] = (int32_t)a; f_001DBF08(c); }
    }
    c->r[16] = s0; c->r[17] = s1; c->r[18] = s2; c->r[19] = s3; c->r[20] = s4;
}
/* 0x1dc2a0(only, skip): message 6 to the same actors as the step. Enemies check their
 * attacks here (a punch, a swinging blade, a spike trap) and hurt "the player", so the
 * ones nearer player 2 run with player 2 in place, the way they step: without it Kakeru
 * could not be hurt by them */
void hle_actors_hit(Ctx *c) {
    r128 s0 = c->r[16], s1 = c->r[17], s2 = c->r[18];
    uint32_t only = c->r[4].uw[0], skip = c->r[5].uw[0];
    for (uint32_t a = MR32(ACTOR_HEAD); a; a = MR32(a + 152)) {
        if (only && !(MR32(a + 112) & only)) continue;
        if (skip && (MR32(a + 112) & skip)) continue;
        if (MR32(a + 148) & 3) continue;
        actor_call(c, a, 6);
    }
    c->r[16] = s0; c->r[17] = s1; c->r[18] = s2;
}
/* 0x1dc340(only, skip): message 4 to the same actors as the step (the boss controllers
 * run their boss in it) */
void hle_actors_msg4(Ctx *c) {
    r128 s0 = c->r[16], s1 = c->r[17], s2 = c->r[18];
    uint32_t only = c->r[4].uw[0], skip = c->r[5].uw[0];
    for (uint32_t a = MR32(ACTOR_HEAD), next; a; a = next) {
        next = MR32(a + 152);
        if (only && !(MR32(a + 112) & only)) continue;
        if (skip && (MR32(a + 112) & skip)) continue;
        if (MR32(a + 148) & 3) continue;
        actor_call(c, a, 4);
    }
    c->r[16] = s0; c->r[17] = s1; c->r[18] = s2;
}
/* 0x1dc210: message 7 to every actor; flag 1 (just created) is cleared instead, flag 2
 * skips the actor */
void hle_actors_late(Ctx *c) {
    r128 s0 = c->r[16], s1 = c->r[17];
    for (uint32_t a = MR32(ACTOR_HEAD); a; a = MR32(a + 152)) {
        uint32_t fl = MR32(a + 148);
        if (fl & 2) continue;
        if (fl & 1) { MW32(a + 148, fl ^ 1); continue; }
        if (actor_call(c, a, 7) == 1) { c->r[4].sd[0] = (int32_t)a; f_001DBF08(c); }
    }
    c->r[16] = s0; c->r[17] = s1;
}

/* 0x34a260(object): registers the collision volumes of the object's model (+68, the model
 * table at gp+7108, 3072 bytes each: +40 count, +44 volumes) for the frame, which is what
 * enemies' attacks and the stage's switches and pickups find. Player 2's own models
 * (registered under their own names) come without volumes, so player 2 registers player
 * 1's body: same rig, same size. */

void f_0034A260_orig(Ctx *c);
void hle_model_coll(Ctx *c) {
    uint32_t o = c->r[4].uw[0], m = MR32(o + 68), e = MR32(0x535af0u + 7108u) + m * 3072u;
    if (!in_p2 || o != PL_BASE + 80 || p1_model == ~0u || m == p1_model || (MR32(e + 44) && MR32(e + 40))) { f_0034A260_orig(c); return; }
    { static int said; if (!said++) INFO("[p2] player 2's model %u has no collision volumes: using player 1's (%u)\n", m, p1_model); }
    MW32(o + 68, p1_model);
    f_0034A260_orig(c);
    MW32(o + 68, m);
}

