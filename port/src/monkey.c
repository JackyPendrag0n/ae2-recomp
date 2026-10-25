/* Play as a monkey: player 1 is drawn as one of the game's monkeys (any of the seven
#include <math.h>
 * kinds of pants), with the monkeys' own motions, and holds the gadgets in its hand.
 * The player's logic is untouched: only its model, its part list and the motions its
 * pose is built from change.  Debug menu (F1) "Play as", settings play_as, or
 * AE2_MONKEY=y|w|b|k|r|c|g (0 = off). */
#include "rt.h"
#define PL_BASE 0x4d5420u
#define GS_MODE 0x3b35d4u
void f_00349348(Ctx *c);
static int model_index(Ctx *c, const char *name) {
    Ctx sv = *c; memset(&c->r[4], 0, 16); memcpy(c->r[4].ub, name, strlen(name) < 16 ? strlen(name) : 15);
    f_00349348(c); int m = c->r[2].sw[0]; *c = sv; return m;
}
#define ACTOR_HEAD (0x535af0u + 6620u)
#define MONKEY_STEP 0x1ffbe8u
static float gf(uint32_t a) { uint32_t v = MR32(a); float f; memcpy(&f, &v, 4); return f; }
static void sf(uint32_t a, float f) { uint32_t v; memcpy(&v, &f, 4); MW32(a, v); }
static uint32_t monkey_body(uint32_t a) {   /* actor -> its data -> the monkey -> its body */
    uint32_t d = MR32(a); if (!d || !RAMOK(d)) return 0;
    uint32_t o = MR32(d); if (!o || !RAMOK(o)) return 0;
    uint32_t b = MR32(o + 0x170); return RAMOK(b) ? b : 0;
}
static void monkey_log(void) {
    static int on = -1; if (on < 0) on = getenv("AE2_MKLOG") != NULL; if (!on) return;
    static uint16_t seen[4096]; static int ns; extern uint32_t g_pad_polls;
    for (uint32_t a = MR32(ACTOR_HEAD), n = 0; a && n < 500; a = MR32(a + 152), n++) {
        if (MR32(a + 144) != MONKEY_STEP) continue;
        uint32_t b = monkey_body(a); if (!b) continue;
        uint32_t m = MR32(b + 0x70); int k; for (k = 0; k < ns && seen[k] != m; k++) {}
        if (k == ns && ns < 4096) { seen[ns++] = m; INFO("[mk] poll %u monkey %x motion %u (model %u, %.0f %.0f %.0f)\n", g_pad_polls, a, m, MR32(b + 0x74), gf(b + 0x60), gf(b + 0x64), gf(b + 0x68)); }
    }
}
/* ---- motions.  The game's motion table (MR32(gp+7096), 64 bytes per id) names
 * every loaded motion: Spike's are pl_*, the monkeys' ps_*.  The player's pose
 * is built in 0x29d208 from the motion id at +1348 (time +1316) blended from
 * +1352 (time +1312); while it runs, those are swapped for the matching monkey
 * motion, at the same fraction of its length, and put back afterwards so the
 * player's own logic never sees them. */
#define MOT_TAB (0x535af0u + 7096u)
static int mot_find(const char *name) {
    static uint32_t tab; static char nm[64][16]; static int id[64], n;
    uint32_t t = MR32(MOT_TAB); if (!t || !RAMOK(t)) return -1;
    if (t != tab) { tab = t; n = 0; }
    for (int i = 0; i < n; i++)
        if (!strcmp(nm[i], name)) { if (id[i] < 0 || memcmp(g_ram + t + id[i] * 64, name, strlen(name) + 1)) break; return id[i]; }   /* not there yet: an archive loaded later may bring it */
    int r = -1;
    for (int i = 0; i < 1024 && RAMOK(t + i * 64 + 64); i++)
        if (!memcmp(g_ram + t + i * 64, name, strlen(name) + 1)) { r = i; break; }
    int k; for (k = 0; k < n && strcmp(nm[k], name); k++) {}
    if (k == n && n < 64) n++;
    if (k < 64) { snprintf(nm[k], 16, "%s", name); id[k] = r; }
    return r;
}
static float mot_len(int m) {   /* frames, as 0x3441e8 counts them */
    uint32_t e = MR32(MOT_TAB) + (uint32_t)m * 64;
    int n = (int)MR32(e + 16); if (MR32(e + 44) != 2 && n) n = 2 * n - 1;
    return n > 1 ? (float)(n - 1) : 1.0f;
}
static const char *mot_name(int m) {
    static char s[17]; uint32_t t = MR32(MOT_TAB);
    if (m < 0 || m >= 1024 || !t) return "";
    memcpy(s, g_ram + t + m * 64, 16); s[16] = 0; return s;
}
/* Spike's motion name -> the monkey motion that fits it */
static const char *mot_map(const char *p) {
    static const struct { const char *key, *mk; } map[] = {
        { "dmg_fall", "ps_fall_j" }, { "dmg_mid", "ps_dmglf_b" }, { "dmg_spin", "ps_dmglf_b" }, { "dmg", "ps_dmgsf_b" },
        { "div_dmg", "ps_dmgsf_b" }, { "land", "ps_runstop_b" }, { "jmp", "ps_jmp_b" }, { "hng", "ps_jmp_b" },
        { "fal", "ps_fall_j" }, { "tmb_fly", "ps_fall_j" }, { "hip_atk", "ps_fall_j" },
        { "run_start", "ps_run_b" }, { "run", "ps_run_b" }, { "hop_dash", "ps_run_b" }, { "slw", "ps_wlk_b" }, { "wlk", "ps_wlk_b" },
        { "swm", "ps_water1_b" }, { "net_", "ps_atk_b" }, { "spn", "ps_atk_b" }, { "bow_swing", "ps_atk_b" },
        { "bow_roll", "ps_handstand_b" }, { "hop_roll", "ps_handstand_b" }, { "pic", "ps_pickup_b" }, { "ban_pit", "ps_throw_b" },
        { "ban_lpit", "ps_throw_b" }, { "mag_", "ps_sht_mag_b" }, { "gun_", "ps_shtm1_b" }, { "mpn_sht", "ps_shtm1_b" },
        { "pac_shot", "ps_shtm1_b" }, { "clr", "ps_handstand_b" }, { "wit_a", "ps_yawn_b" }, { "wit_b", "ps_kayui_b" },
        { "wit_c", "ps_kusyami_b" }, { "wit", "ps_lookf_b" },
    };
    if (strncmp(p, "pl_", 3)) return NULL;
    for (size_t i = 0; i < sizeof map / sizeof *map; i++) if (strstr(p + 3, map[i].key)) return map[i].mk;
    return "ps_std_b";
}
#define PL_PARTS (PL_BASE + 80 + 128)
static int monkey_on;
static const char *kit_part;       /* the weapon model the monkey wears this frame, or none */
static const char *kit_mot;        /* its motion while aiming or firing */
static int kit_hide;               /* Spike's gadget is hidden (the monkey holds its own) */
static int ab_kind = -1;          /* the monkey whose abilities are on this frame, -1 none */
enum { MK_YELLOW, MK_WHITE, MK_BLUE, MK_BLACK, MK_RED, MK_LIGHTBLUE, MK_GREEN, MK_SPECTER };
int p2_block_is_p1(void);
static int pl_is_p1(void) { return p2_block_is_p1(); }   /* the block at PL_BASE is player 1's */           /* the player is drawn as a monkey this stage frame */
static int spt_on;                 /* player 1 is Specter this frame */
static const char *spt_map(const char *p);
static int mot_swap(uint32_t mo, uint32_t to) {   /* motion id field, its time field */
    int m = (int)MR32(mo); if (m < 0 || m >= 1024) return m;
    const char *k = spt_on ? spt_map(mot_name(m)) : mot_map(mot_name(m));
    if (k && ab_kind == MK_BLUE && !strcmp(k, "ps_run_b")) k = "ps_nmb_run_b";   /* the blue monkey's own sprint */
    if (k && ab_kind == MK_RED && strstr(mot_name(m), "mpn_"))   /* the red monkey's own punches where the stage has them */
        k = mot_find("ps_nmr_atk1_b") >= 0 ? "ps_nmr_atk1_b" : "ps_atk_b";
    int mm = k ? mot_find(k) : -1;
    if (mm < 0) return m;
    float t = gf(to), f = t / mot_len(m); f -= (float)(int)f; if (f < 0) f = 0;
    float nt = f * mot_len(mm); uint32_t v; memcpy(&v, &nt, 4);
    MW32(mo, (uint32_t)mm); MW32(to, v);
    return m;
}
/* 0x344688(motion, name): the matrix of a joint, by name, as the last pose left it (an
 * identity at 0x5e21e0 when the motion has no such joint).  The player's gadgets and
 * effects ask for Spike's joints; on a monkey they get the monkey's. */
void f_00344688_orig(Ctx *c);
static int mot_has_joint(int m, const r128 *name) {   /* 0x3445a0 answers joint 0 for a joint it does not find */
    uint32_t t = MR32(MOT_TAB); if (m < 0 || m >= 1024 || !t) return 1;
    uint32_t l = MR32(t + (uint32_t)m * 64 + 20); if (!l || !RAMOK(l)) return 1;
    int n = (int)MR32(l + 8);
    for (int i = 0; i < n && i < 256; i++) if (!memcmp(g_ram + l + 16 + i * 80, name->ub, 16)) return 1;
    return 0;
}
static int chars_bone(Ctx *c);
static float spt_C[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };   /* Specter's hand to Spike's, standing (measured in spt_measure) */
void hle_bone_find(Ctx *c) {
    if (chars_bone(c)) return;
    if (!monkey_on || !pl_is_p1() || mot_has_joint(c->r[4].sw[0], &c->r[5])) { f_00344688_orig(c); return; }
    /* only the hands: the player's other joints are posed by its own code, which
     * then reads them back, and the monkey's joints there look wrong */
    static const char *const jm[][2] = { { "jnt_rhand", "jnt_handR" }, { "jnt_lhand", "jnt_handL" } };
    static const char *const js[][2] = { { "jnt_rhand", "jnt_handr" }, { "jnt_lhand", "jnt_handl" } };   /* Specter's */
    char nm[17] = {0}; memcpy(nm, c->r[5].ub, 16);
    for (size_t i = 0; i < sizeof jm / sizeof *jm; i++) if (!strcmp(nm, jm[i][0])) {
        const char *const *jp = spt_on ? js[i] : jm[i];
        r128 a4 = c->r[4], a5 = c->r[5];
        memset(&c->r[5], 0, 16); memcpy(c->r[5].ub, jp[1], strlen(jp[1]));
        f_00344688_orig(c);
        c->r[4] = a4; c->r[5] = a5;
        if (!i) {
            /* the monkey's hand turned the way Spike's is when both stand (measured), so
             * what Spike holds sits in the monkey's hand the same way; into spare room
             * at the end of the player's part list */
            static const float CM[3][3] = { { 0.532f, -0.426f, -0.732f }, { 0.225f, 0.904f, -0.363f }, { 0.816f, 0.029f, 0.577f } };
            const float (*C)[3] = spt_on ? spt_C : CM;
            uint32_t a = c->r[2].uw[0], o = PL_PARTS + 16 + 7 * 16;   /* names 8 to 11 of the 11 it holds: unused */
            float m[16], r[16]; for (int k = 0; k < 16; k++) m[k] = gf(a + 4 * k);
            memcpy(r, m, sizeof r);
            for (int y = 0; y < 3; y++) for (int x = 0; x < 3; x++) r[y * 4 + x] = C[y][0] * m[x] + C[y][1] * m[4 + x] + C[y][2] * m[8 + x];
            if (kit_hide) for (int y = 0; y < 3; y++) for (int x = 0; x < 3; x++) r[y * 4 + x] = 0;   /* shrunk to nothing */
            for (int k = 0; k < 16; k++) { uint32_t v; memcpy(&v, &r[k], 4); MW32(o + 4 * k, v); }
            c->r[2].sd[0] = (int32_t)o;
        }
        return;
    }
    f_00344688_orig(c);
}
static void shield_pose(uint32_t p);
static void kit_pose(uint32_t p);
static void spt_pose(uint32_t p);
void f_0029D208_orig(Ctx *c);
void hle_player_model(Ctx *c) {
    uint32_t p = c->r[4].uw[0];
    if (!monkey_on || p != PL_BASE || !pl_is_p1()) { f_0029D208_orig(c); return; }
    uint32_t sv[4] = { MR32(p + 1348), MR32(p + 1316), MR32(p + 1352), MR32(p + 1312) };
    mot_swap(p + 1348, p + 1316); mot_swap(p + 1352, p + 1312);
    shield_pose(p);
    kit_pose(p);
    if (spt_on) spt_pose(p);
    { static int log = -1; if (log < 0) log = getenv("AE2_MKLOG") != NULL; static uint32_t last = ~0u;
      if (log && sv[0] != last) { last = sv[0]; extern uint32_t g_pad_polls; char a[17]; snprintf(a, 17, "%s", mot_name((int)sv[0]));
          INFO("[monkey] poll %u state %u %s -> %s\n", g_pad_polls, MR32(p + 868), a, mot_name((int)MR32(p + 1348))); } }
    f_0029D208_orig(c);
    /* the model draw (0x34a580) draws the part list only with flag 0x200 */
    if (MR32(PL_PARTS + 12)) MW32(p + 80 + 76, MR32(p + 80 + 76) | 0x200);
    MW32(p + 1348, sv[0]); MW32(p + 1316, sv[1]); MW32(p + 1352, sv[2]); MW32(p + 1312, sv[3]);
}

/* ---- outfits: the seven kinds of monkey, as the game lists their parts (0x502760) */
static const char *const outfits[][7] = {
    { "y", "ps_heru_b", "ps_body_b", "ps_nmy_eye_b", "ps_nmy_pants_b" },
    { "w", "ps_heru_b", "ps_body_b", "ps_nmw_glass_b", "ps_nmw_pants_b" },
    { "b", "ps_heru_b", "ps_body_b", "ps_nmb_gogle_b", "ps_nmb_pants_b", "ps_nmb_shoes_b", "ps_nmb_chief_b" },
    { "k", "ps_heru_b", "ps_body_b", "ps_nmk_sungla_b", "ps_nmk_pants_b", "ps_nmk_tie_b" },
    { "r", "ps_heru_b", "ps_nmr_body_b", "ps_nmr_eye_b", "ps_nmr_pants_b", "ps_nmr_glove_b" },
    { "c", "ps_body_b", "ps_heru_b", "ps_nmc_eye_b", "ps_nmc_pants_b" },
    { "g", "ps_body_b", "ps_heru_b", "ps_nmg_scp_b", "ps_nmg_pants_b" },
};
/* the model object's part list (as a monkey body's at +176): +12 count, +16 names */
static void set_parts(Ctx *c, int k) {
    int n = 0;
    for (int i = 1; i < 7 && outfits[k][i]; i++) {
        if (model_index(c, outfits[k][i]) < 0) continue;
        char nm[16] = {0}; snprintf(nm, 16, "%s", outfits[k][i]);
        gwrite(PL_PARTS + 16 + n * 16, nm, 16); n++;
    }
    if (kit_part && n < 7 && model_index(c, kit_part) >= 0) {   /* the monkey's own weapon, worn like its clothes */
        char nm[16] = {0}; snprintf(nm, 16, "%s", kit_part); gwrite(PL_PARTS + 16 + n * 16, nm, 16); n++; }
    MW32(PL_PARTS + 12, (uint32_t)n);
}

/* ---- the outfits' models.  A stage loads only the kinds of monkey it has; the title
 * screen's archive (TIT_A) has every kind.  It is loaded once, the way p2.c loads player
 * 2's character: in a load group of its own and hidden from the archive table, so stage
 * loads leave it alone (and the title screen loads its own copy as usual). */
#define MK_GROUP 0x1000000
#define HEAP_PTR (0x535af0u - 1376)
void f_00193D78(Ctx *c); void f_001938F0(Ctx *c); void f_00351B90(Ctx *c); void f_00351BC0(Ctx *c); void f_00351BA8(Ctx *c);
static void name128(r128 *r, const char *s) { memset(r, 0, sizeof *r); memcpy(r->ub, s, strlen(s)); }
static uint32_t heap_largest(void) {
    uint32_t h = MR32(HEAP_PTR), head = h + 64, big = 0, n = 0;
    for (uint32_t b = MR32(head); b != head && b && n < 100000; b = MR32(b), n++)
        if (!MR32(b + 12) && MR32(b + 8) > big) big = MR32(b + 8);
    return big;
}
static void archive_load(Ctx *c, const char *arc, uint32_t group, int *tries) {
    if (*tries >= 3) return;
    (*tries)++;
    Ctx sv = *c;
    name128(&c->r[4], arc); c->r[5].sd[0] = 0; f_001938F0(c);
    uint32_t e = c->r[2].uw[0], need = e ? ((MR32(e + 84) + 2047) & ~2047u) + 64 : 0;
    if (!e || need + 0x100000 > heap_largest()) {
        *c = sv; INFO("[monkey] no room for %s (needs %x, largest free %x)\n", arc, need, heap_largest()); return; }
    f_00351B90(c);
    c->r[4].sd[0] = (int32_t)group; f_00351BC0(c);
    name128(&c->r[4], arc); c->r[5].sd[0] = (int32_t)MR32(HEAP_PTR); f_00193D78(c);
    f_00351BA8(c);
    name128(&c->r[4], arc); c->r[5].sd[0] = 0; f_001938F0(c);
    if (c->r[2].uw[0]) MW8(c->r[2].uw[0] + 80, 0);
    *c = sv;
    INFO("[monkey] loaded %s (%x bytes)\n", arc, need);
}
static void outfits_load(Ctx *c) { static int tries; archive_load(c, "TIT_A", MK_GROUP, &tries); }

/* 0: the game's own character; 1..7: a monkey in outfits[mode - 1] */
volatile int g_monkey_mode = -1;
const char *monkey_mode_name(int m) {
    static const char *const n[] = { "Hikaru", "Monkey (yellow pants)", "Monkey (white pants: bombs)", "Monkey (blue pants: speed)",
        "Monkey (black pants: gun)", "Monkey (red pants: punch)", "Monkey (light blue pants: shield)", "Monkey (green pants: missiles)",
        "the Professor", "Natalie", "Specter" };
    return m >= 0 && m <= 10 ? n[m] : n[0];
}
const char *monkey_mode_key(int m) {
    static const char *const k[] = { "hikaru", "monkey_yellow", "monkey_white", "monkey_blue", "monkey_black", "monkey_red", "monkey_lightblue", "monkey_green",
        "professor", "natalie", "specter" };
    return m >= 0 && m <= 10 ? k[m] : k[0];
}
int monkey_mode_parse(const char *v) {
    for (int m = 0; m <= 10; m++) if (!strcmp(v, monkey_mode_key(m))) return m;
    for (int k = 0; k < 7; k++) if (v[0] == outfits[k][0][0] && !v[1]) return k + 1;
    if (!strcmp(v, "p")) return 8;
    if (!strcmp(v, "n")) return 9;
    if (!strcmp(v, "s")) return 10;
    return atoi(v) >= 1 && atoi(v) <= 10 ? atoi(v) : 0;
}
/* back to the game's own character (its models are registered as "player" and "player_hair") */
static void restore_player(Ctx *c) {
    char n[16] = {0};
    gread(n, PL_BASE + 848, 16);
    if (memcmp(n, "ps_nm", 6) && memcmp(n, "np_", 3) && memcmp(n, "bs_spector", 11)) return;
    int b = model_index(c, "player"), h = model_index(c, "player_hair");
    if (b < 0) return;
    memset(n, 0, 16); memcpy(n, "player", 6); gwrite(PL_BASE + 848, n, 16);
    MW32(PL_BASE + 148, (uint32_t)b);
    MW32(PL_BASE + 844, (uint32_t)h);
    MW32(PL_PARTS + 12, 0);
    static const uint8_t z[11 * 16]; gwrite(PL_PARTS + 16, z, sizeof z);
}

/* ---- abilities.  Every monkey nets with triangle (and R1); circle (and square) is its
 * own thing: white throws bombs, black shoots, green fires homing missiles (all three
 * with the slingshot, aimed with the right stick), red swings the Magic Punch, blue runs
 * fast and light blue holds up a shield while circle is held, and nothing hurts it.
 * Yellow is the plain monkey and keeps Spike's gadgets.  The button slots are the
 * player's own (PL+2812..2824); the game's are kept aside and are what gets
 * saved.  Ammo is lent for the shot and handed back, so the save never changes. */
#define G_NET 2      /* slot ids: the gadget table's record + 1, 0 none */
#define G_SLING 5
#define G_PUNCH 11
static int ab_have; static uint32_t ab_orig[4];   /* the game's slots, while ours are in */
static int shield_on;
/* the gadgets' owned bytes (0x4d24c0, 96-byte records, +0x13) are rebuilt from the
 * save's progress at every stage load and never saved: the monkey's own gadget is lent */
#define GADGETS 0x4d24c0u
static uint32_t lent; static uint8_t lent_was[32];   /* the gadgets lent (bits by id) */
static void lend_set(uint32_t want) {
    int ok = MR32(GADGETS) == 0x5f746e6au;   /* "jnt_" */
    for (int g = 0; g < 32; g++) if ((lent >> g & 1) && !(want >> g & 1)) { if (ok) MW8(GADGETS + (uint32_t)g * 96 + 0x13, lent_was[g]); lent &= ~(1u << g); }
    if (!ok) return;
    for (int g = 0; g < 32; g++) if (want >> g & 1) {
        uint32_t a = GADGETS + (uint32_t)g * 96 + 0x13;
        if (MR8(a) == 1 && !(lent >> g & 1)) continue;
        if (!(lent >> g & 1)) { lent |= 1u << g; lent_was[g] = MR8(a); }
        MW8(a, 1);
    }
}
static void lend(int g) { lend_set(g < 0 ? 0 : 1u << g); }
static void ab_layout(int k, uint32_t *w) {
    uint32_t a = k == MK_WHITE || k == MK_BLACK || k == MK_GREEN || k == MK_SPECTER ? G_SLING : k == MK_RED ? G_PUNCH : G_NET;
    w[0] = G_NET; w[1] = a; w[2] = G_NET; w[3] = k == MK_SPECTER ? G_PUNCH : a;   /* Specter: his shot on circle, his strike on square */
}
static int ab_ours(const uint32_t *s) {
    for (int k = 1; k <= MK_SPECTER; k++) { uint32_t w[4]; ab_layout(k, w); if (!memcmp(w, s, sizeof w)) return 1; }
    return 0;
}
void f_002B5A18(Ctx *c);
/* while aiming (state 31) and firing (32) with its own weapon, the monkey takes the
 * pose the game's armed monkeys use (looped) */
static void kit_pose(uint32_t p) {
    static float t;
    uint32_t st = MR32(p + 868);
    if (!kit_mot || (st != 31 && st != 32)) { t = 0; return; }
    int g = mot_find(kit_mot); if (g < 0) return;
    float l = mot_len(g); t += 1.0f; if (t >= l) t -= l;
    uint32_t v; memcpy(&v, &t, 4);
    MW32(p + 1348, (uint32_t)g); MW32(p + 1316, v);
    MW32(p + 1352, (uint32_t)g); MW32(p + 1312, v);
}
/* the monkeys' own kit: what each wears and shoots in place of Spike's slingshot */
static const struct { const char *part, *mot, *shot; } kits[8] = {
    [MK_WHITE] = { "ps_bombs0", "ps_bombs0_b", "ps_bombs0" },        /* bombs, thrown */
    [MK_BLACK] = { "ps_mgun0_b", "ps_mgun0_b", "pe_mach1" },        /* the uzi and its bullets */
    [MK_GREEN] = { "ps_mlauncher_b", "ps_missile_b", "pe_missile" }, /* the launcher and its missiles */
    [MK_LIGHTBLUE] = { "ps_nabebuta_b", NULL, NULL },               /* the pot-lid shield */
    [MK_SPECTER] = { NULL, "bs_spt_sht1", NULL },                   /* his shot, from the hand */
};
void hle_pachinko_shot(Ctx *c);
void f_001B8CC8(Ctx *c);
static void uzi(Ctx *c) {   /* the black monkey's uzi fires while the right stick is held */
    static uint32_t n;
    if (MR32(PL_BASE + 868) != 31) { n = 0; return; }
    if (n++ % 4) return;
    float d[3] = { gf(PL_BASE + 3040), gf(PL_BASE + 3044), gf(PL_BASE + 3048) };
    if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] < 0.5f) return;
    Ctx sv = *c;
    memset(&c->r[4], 0, 16 * 3);
    c->r[5].f[0] = gf(PL_BASE + 0x850) + d[0] * 16; c->r[5].f[1] = gf(PL_BASE + 0x854) + 10; c->r[5].f[2] = gf(PL_BASE + 0x858) + d[2] * 16; c->r[5].f[3] = 1;
    c->r[6].f[0] = d[0]; c->r[6].f[1] = d[1]; c->r[6].f[2] = d[2]; c->r[6].f[3] = 1;
    hle_pachinko_shot(c);
    c->r[4].sd[0] = 2097; f_001B8CC8(c);   /* the slingshot's sound */
    *c = sv;
}
/* the slingshot shots look their model up by name (0x4d2ac0, 16 bytes per kind) when their
 * task first runs, a frame after the shot, so the monkey's names stay in while it is played */
static void shot_names(Ctx *c, int k) {
    static uint8_t orig[3][16]; static int saved;
    if (!saved) { gread(orig, 0x4d2ac0u, sizeof orig); saved = 1; }
    int t = k == MK_WHITE ? 1 : k == MK_GREEN ? 2 : k == MK_BLACK ? 0 : -1;
    uint8_t want[3][16], now[3][16];
    memcpy(want, orig, sizeof want);
    if (t >= 0 && kits[k].shot && model_index(c, kits[k].shot) >= 0) { memset(want[t], 0, 16); snprintf((char *)want[t], 16, "%s", kits[k].shot); }
    gread(now, 0x4d2ac0u, sizeof now);
    if (memcmp(now, want, sizeof now)) gwrite(0x4d2ac0u, want, sizeof want);
}
static void abilities(Ctx *c, int k) {
    uint32_t s[4]; for (int i = 0; i < 4; i++) s[i] = MR32(PL_BASE + 2812 + 4 * i);
    if (k <= MK_YELLOW) {
        if (ab_have && ab_ours(s)) for (int i = 0; i < 4; i++) MW32(PL_BASE + 2812 + 4 * i, ab_orig[i]);
        ab_have = 0; ab_kind = -1; shield_on = 0; lend(-1); kit_part = kit_mot = NULL; kit_hide = 0;
        shot_names(c, -1);
        return;
    }
    uint32_t w[4]; ab_layout(k, w);
    if (memcmp(w, s, sizeof w)) {
        if (!ab_ours(s)) { memcpy(ab_orig, s, sizeof s); ab_have = 1; }   /* the game's, just set up by the stage */
        for (int i = 0; i < 4; i++) MW32(PL_BASE + 2812 + 4 * i, w[i]);
    }
    uint32_t h = MR32(PL_BASE + 2836);
    if (h != w[0] && h != w[1] && h != w[3] && MR32(PL_BASE + 868) == 0 && !MR8(PL_BASE + 882)) {
        r128 a0 = c->r[4], a1 = c->r[5], a2 = c->r[6], ra = c->r[31];
        c->r[4].sd[0] = (int32_t)PL_BASE; c->r[5].sd[0] = 0; c->r[6].sd[0] = 1;
        f_002B5A18(c);   /* as if triangle were pressed */
        c->r[4] = a0; c->r[5] = a1; c->r[6] = a2; c->r[31] = ra;
    }
    ab_kind = k;
    if (k == MK_SPECTER) lend_set(1u << G_SLING | 1u << G_PUNCH);
    else lend(w[1] != G_NET ? (int)w[1] : -1);
    shield_on = k == MK_LIGHTBLUE && (host_pad_buttons(0) & (1u << 13)) && MR8(PL_BASE + 882) == 0;
    int armed = w[1] == G_SLING ? MR32(PL_BASE + 2836) == G_SLING : k == MK_LIGHTBLUE && shield_on;
    kit_part = armed ? kits[k].part : NULL;
    kit_mot = armed ? kits[k].mot : NULL;
    kit_hide = armed && w[1] == G_SLING && kits[k].part && model_index(c, kits[k].part) >= 0;
    if (k == MK_SPECTER) kit_hide = armed || MR32(PL_BASE + 2836) == G_PUNCH;   /* he shoots and strikes bare-handed */
    shot_names(c, k);
    if (armed && k == MK_BLACK) uzi(c);
}
/* the slingshot's shot: f_0028A180(kind) with the kind from PL+3496 (0 pellet, 1 bomb,
 * 2 homing), which falls back to a pellet when the save holds none of that ammo */
void f_0028A180_orig(Ctx *c);
void hle_pachinko_shot(Ctx *c) {
    int t = ab_kind == MK_WHITE ? 1 : ab_kind == MK_GREEN || ab_kind == MK_SPECTER ? 2 : ab_kind == MK_BLACK ? 0 : -1;
    if (t < 0 || !pl_is_p1()) { f_0028A180_orig(c); return; }
    c->r[4].sd[0] = t;
    r128 sv5 = c->r[5], sv6 = c->r[6];
    uint32_t a = 0x3e19a8u + (uint32_t)t; uint8_t n = MR8(a);
    MW8(a, 99);
    /* the shot's model is looked up by name (0x4d2ac0, 16 bytes per kind) as it is made */
    f_0028A180_orig(c);
    MW8(a, n);
    if (getenv("AE2_GLOG")) { char nm[17] = {0}; gread(nm, 0x4d2ac0u + 16u * (uint32_t)t, 16); INFO("[glog] shot model %s idx %d\n", nm, model_index(c, nm)); }
    if (getenv("AE2_GLOG")) { INFO("[glog] shot kind %d a1 %.1f %.1f %.1f %.1f a2 %.2f %.2f %.2f %.2f pos %.1f %.1f %.1f p1664 %.1f %.1f %.1f p3040 %.2f %.2f %.2f\n", t,
        sv5.f[0], sv5.f[1], sv5.f[2], sv5.f[3], sv6.f[0], sv6.f[1], sv6.f[2], sv6.f[3], gf(PL_BASE + 0x850), gf(PL_BASE + 0x854), gf(PL_BASE + 0x858),
        gf(PL_BASE + 1664), gf(PL_BASE + 1668), gf(PL_BASE + 1672), gf(PL_BASE + 3040), gf(PL_BASE + 3044), gf(PL_BASE + 3048)); }
}
/* 0x287278(kind): the save's count of that slingshot ammo.  The monkeys' is endless
 * (a slingshot with no pellets can't even be taken in hand on a fresh save) */
void f_00287278_orig(Ctx *c);
void hle_ammo_count(Ctx *c) {
    int t = c->r[4].sw[0];
    int mine = ab_kind == MK_WHITE ? 1 : ab_kind == MK_GREEN || ab_kind == MK_SPECTER ? 2 : ab_kind == MK_BLACK ? 0 : -1;
    if (mine < 0 || (t != 0 && t != mine) || !pl_is_p1()) { f_00287278_orig(c); return; }
    c->r[2].sd[0] = 99;
}
/* 0x2b55c8(player): copies the button slots into the save; the game's, not ours */
void f_002B55C8_orig(Ctx *c);
void hle_slots_save(Ctx *c) {
    uint32_t p = c->r[4].uw[0], s[4];
    if (p != PL_BASE || !ab_have || !pl_is_p1()) { f_002B55C8_orig(c); return; }
    for (int i = 0; i < 4; i++) { s[i] = MR32(p + 2812 + 4 * i); MW32(p + 2812 + 4 * i, ab_orig[i]); }
    f_002B55C8_orig(c);
    for (int i = 0; i < 4; i++) MW32(p + 2812 + 4 * i, s[i]);
}
/* message 8 to the player task is a hit (0x2a6060); the shield turns it away */
static int spt_dash, spt_taunt;   /* Specter's warp dash (frames left) and his laugh */
int monkey_shield_blocks(uint32_t player, uint32_t msg) {
    return (shield_on || spt_dash > 0) && msg == 8 && player == PL_BASE && pl_is_p1();
}
/* blue runs at double speed and the shield stands still: the player's velocity
 * (PL+2288 x, +2296 z) is scaled at 0x29c498, an empty function the mover (0x29b5c0)
 * calls after working out the velocity and before moving by it, and put back after
 * the mover so the next frame's speed-up and friction work from the real one */
static float ab_k = 1.0f, ab_v[2];
void hle_pl_premove(Ctx *c) {
    ab_k = 1.0f;
    if (c->r[4].uw[0] != PL_BASE || ab_kind < 0 || !pl_is_p1()) return;
    float k = ab_kind == MK_BLUE ? 2.0f : shield_on || spt_taunt ? 0.0f : spt_dash > 0 ? 4.0f : 1.0f;
    if (k == 1.0f) return;
    for (int i = 0; i < 2; i++) { uint32_t a = PL_BASE + 2288 + 8 * i; ab_v[i] = gf(a); float v = ab_v[i] * k; uint32_t u; memcpy(&u, &v, 4); MW32(a, u); }
    ab_k = k;
}
void monkey_move_end(uint32_t actor) {
    if (actor != PL_BASE || ab_k == 1.0f) return;
    for (int i = 0; i < 2; i++) { uint32_t a = PL_BASE + 2288 + 8 * i; float v = ab_k > 0 ? gf(a) / ab_k : ab_v[i]; uint32_t u; memcpy(&u, &v, 4); MW32(a, u); }
    ab_k = 1.0f;
}
/* the light blue monkey's guard while the shield is up */
static void shield_pose(uint32_t p) {
    static int held;
    if (!shield_on) { held = 0; return; }
    int g = mot_find("ps_grd_b"); if (g < 0) return;
    float t = held < 30 ? (float)held : 30.0f, l = mot_len(g); if (t > l) t = l;   /* arms up over the head by frame 30 */
    held++;
    uint32_t v; memcpy(&v, &t, 4);
    MW32(p + 1348, (uint32_t)g); MW32(p + 1316, v);
    MW32(p + 1352, (uint32_t)g); MW32(p + 1312, v);
}

/* AE2_MODELS=1: list the model names registered (gp+4968: 256 hash buckets of
 * {name[16], index, next}) whenever the scene changes (testing) */
static void models_log(void) {
    static int on = -1; if (on < 0) on = getenv("AE2_MODELS") != NULL; if (!on) return;
    static uint32_t last = ~0u, seen; extern uint32_t g_pad_polls;
    uint32_t sc = MR32(0x3b35a0u + 0x80);
    if (MR32(GS_MODE) != 3) return;
    if (sc == last && g_pad_polls - seen != 60 && g_pad_polls - seen != 600) return;
    if (sc != last) { last = sc; seen = g_pad_polls; return; }
    uint32_t t = MR32(0x535af0u + 4968u); if (!t || !RAMOK(t)) return;
    INFO("[models] scene %u:", sc);
    for (int b = 0; b < 256; b++) for (uint32_t e = MR32(t + 4 * b), n = 0; e && RAMOK(e) && n < 1000; e = MR32(e + 20), n++) {
        char nm[17] = {0}; memcpy(nm, g_ram + e, 16); INFO(" %s=%d", nm, (int32_t)MR32(e + 16)); }
    for (int i = 0; i < 1024; i++) { const char *n = mot_name(i);
        if (strncmp(n, "np_", 3) && strcmp(n, "pl_run_shand") && strcmp(n, "ps_run_b") && strcmp(n, "bs_spt_wait") && strcmp(n, "bs_sptc_wait")) continue;
        uint32_t l = MR32(MR32(MOT_TAB) + (uint32_t)i * 64 + 20); if (!l || !RAMOK(l)) continue;
        INFO("\n[joints] %s:", n);
        for (int j = 0; j < (int)MR32(l + 8) && j < 256; j++) { char jn[17] = {0}; memcpy(jn, g_ram + l + 16 + j * 80, 16); INFO(" %s", jn); } }
    { uint32_t n = MR32(MR32(0x535af0u + 6136u) + 8), t2 = MR32(0x535af0u + 6144u); INFO("\n[archives]");
      for (uint32_t i = 0; i < n && i < 400; i++) { char nm[17] = {0}; memcpy(nm, g_ram + t2 + i * 112, 16); INFO(" %s:%x:%x:%x", nm, MR32(t2 + i * 112 + 84), MR8(t2 + i * 112 + 80), MR32(t2 + i * 112 + 88)); } }
    INFO("\n[motions] scene %u:", sc);
    for (int i = 0; i < 1024; i++) { const char *n = mot_name(i); if (n[0] && strncmp(n, "ps_", 3)) INFO(" %d=%s", i, n); }
    INFO("\n");
}
static void chars_frame(Ctx *c, int ci);
static void spt_frame(Ctx *c);
static int char_on = -1;   /* which of chars[] (the Professor, Natalie) player 1 is this frame, -1 none */
void monkey_frame(Ctx *c) {
    models_log();
    monkey_log();
    { static int on = -1; if (on < 0) on = getenv("AE2_GLOG") != NULL; static char last[256];
      if (on) { char b[256]; extern uint32_t g_pad_polls; snprintf(b, sizeof b, "slots %d %d %d %d cur %d hand %d/%d st %d/%d", MR32(PL_BASE + 2812), MR32(PL_BASE + 2816), MR32(PL_BASE + 2820), MR32(PL_BASE + 2824), MR32(PL_BASE + 2828), MR32(PL_BASE + 2836), MR32(PL_BASE + 2840), MR32(PL_BASE + 868), MR32(PL_BASE + 1296));
        if (strcmp(b, last)) { strcpy(last, b); INFO("[glog] %u %s\n", g_pad_polls, b); }
        if (g_pad_polls % 20 == 0) INFO("[glog] %u pos %.1f %.1f\n", g_pad_polls, gf(PL_BASE + 0x850), gf(PL_BASE + 0x858)); } }
    if (g_monkey_mode < 0) { const char *e = getenv("AE2_MONKEY"); g_monkey_mode = e ? monkey_mode_parse(e) : 0; }
    monkey_on = 0; char_on = -1; spt_on = 0;
    if (g_monkey_mode != 10) spt_dash = spt_taunt = 0;
    uint32_t mode = MR32(GS_MODE);
    if (mode < 2 || mode > 5 || !pl_is_p1()) { abilities(c, -1); return; }
    if (g_monkey_mode == 10) { spt_frame(c); return; }
    if (g_monkey_mode >= 8) { abilities(c, -1); chars_frame(c, g_monkey_mode - 8); return; }
    int k = g_monkey_mode - 1, m = k >= 0 ? model_index(c, "ps_nm") : -1;
    if (m < 0) { abilities(c, -1); restore_player(c); return; }
    /* the body model is looked up each frame by the name at +848; no hair */
    char n[16] = {0}; memcpy(n, "ps_nm", 5);
    gwrite(PL_BASE + 848, n, 16);
    MW32(PL_BASE + 148, (uint32_t)m);
    MW32(PL_BASE + 844, 0xffffffffu);
    if (model_index(c, outfits[k][4]) < 0 || (k == MK_GREEN && model_index(c, "ps_mlauncher_b") < 0)) outfits_load(c);
    set_parts(c, k);
    monkey_on = 1;
    abilities(c, k);
}
/* ---- the Professor and Natalie.  Their models (np_hks_model, np_ntm_model) come with the
 * Time Station (STA_A, loaded once like the outfits), with no animation but a standing
 * wait, and their skeletons are their own.  So Spike's motions are retargeted each time
 * the player's pose is built (0x343fe8 on the player's draw, PL+80): the game poses
 * Spike's skeleton into the scratchpad (world joint matrices, 64 bytes each, rows the
 * axes, the joint's scale in them); each of the character's joints turns the way its
 * matching joint of Spike's turns from standing (the two standing poses: Spike's
 * pl_std_shand1, the character's wait, both at time 0), keeps its own bone lengths, and
 * the joints Spike has no match for (fingers, hair, moustache) keep their standing pose
 * on their parent.  The draw then takes the character's own motion, so that its model's
 * joints find theirs by name. */
#define SPAD 0x70000000u
typedef struct { float r[3][3], p[3]; } Xf;   /* rotation rows (unit), position */
static const struct { const char *model, *rest, *const (*map)[2]; int nmap; } chars[2] = {
    { "np_hks_model", "np_hks_wait", (const char *const [][2]) {
        { "jnt_body1", "jnt_stomach" }, { "jnt_body2", "jnt_chest" }, { "jnt_head", "jnt_head" },
        { "jnt_sakotsuL", "jnt_lshou" }, { "jnt_armL1", "jnt_larm" }, { "jnt_armL2", "jnt_larm2" }, { "jnt_handL", "jnt_lhand" },
        { "jnt_sakotsuR", "jnt_rshou" }, { "jnt_armR1", "jnt_rarm" }, { "jnt_armR2", "jnt_rarm2" }, { "jnt_handR", "jnt_rhand" },
        { "jnt_siri", "jnt_hip" }, { "jnt_legL1", "jnt_Lfoot1" }, { "jnt_legL2", "jnt_Lfoot2" }, { "jnt_footL1", "jnt_lheel" }, { "jnt_footL2", "jnt_ltoe" },
        { "jnt_legR1", "jnt_Rfoot1" }, { "jnt_legR2", "jnt_Rfoot2" }, { "jnt_footR1", "jnt_rheel" }, { "jnt_footR2", "jnt_rtoe" } }, 20 },
    { "np_ntm_model", "np_ntm_wait", (const char *const [][2]) {
        { "jnt_kosi", "jnt_stomach" }, { "jnt_mune", "jnt_chest" }, { "jnt_kubi", "jnt_neck" }, { "jnt_atama", "jnt_head" },
        { "jnt_sakotul", "jnt_lshou" }, { "jnt_arml1", "jnt_larm" }, { "jnt_arml2", "jnt_larm2" }, { "jnt_handl", "jnt_lhand" },
        { "jnt_sakotur", "jnt_rshou" }, { "jnt_armr1", "jnt_rarm" }, { "jnt_armr2", "jnt_rarm2" }, { "jnt_handr", "jnt_rhand" },
        { "jnt_siri", "jnt_hip" }, { "jnt_legl1", "jnt_Lfoot1" }, { "jnt_legl2", "jnt_Lfoot2" }, { "jnt_kakatoL", "jnt_lheel" }, { "jnt_toeL", "jnt_ltoe" },
        { "jnt_legr1", "jnt_Rfoot1" }, { "jnt_legr2", "jnt_Rfoot2" }, { "jnt_kakatoR", "jnt_rheel" }, { "jnt_toeR", "jnt_rtoe" } }, 21 },
};
#define RT_MAX 64
static struct {
    uint32_t tab; int src_rest, tgt;           /* what it was built for */
    int n, ns, ok;
    int parent[RT_MAX], src[RT_MAX];           /* the character's parents; its match in Spike's skeleton, -1 none */
    float scale[RT_MAX];
    Xf t0[RT_MAX], s0[RT_MAX];                 /* the two standing poses, model space */
    float lr[RT_MAX][3][3], lp[RT_MAX][3];     /* the character's standing pose on its parents */
    float k;                                   /* the character's hip height over Spike's */
} rt;
static float rt_src_world[RT_MAX][16];
static float rt_out[RT_MAX][16], rt_W[16];   /* the character's posed joints this frame (world), and its W without the scale */   /* Spike's posed joints this frame (world), at the character's joints */
static int rt_posed;                     /* the player's pose this frame is the character's */
static int rt_src_mot = -1;              /* Spike's motion it was made from */
static void mul3(float o[3][3], const float a[3][3], const float b[3][3]) {
    float t[3][3]; for (int i = 0; i < 3; i++) for (int k = 0; k < 3; k++) t[i][k] = a[i][0] * b[0][k] + a[i][1] * b[1][k] + a[i][2] * b[2][k];
    memcpy(o, t, sizeof t);
}
static void tr3(float o[3][3], const float a[3][3]) { float t[3][3]; for (int i = 0; i < 3; i++) for (int k = 0; k < 3; k++) t[i][k] = a[k][i]; memcpy(o, t, sizeof t); }
static void vm3(float o[3], const float v[3], const float m[3][3]) {   /* row vector times matrix */
    float t[3]; for (int k = 0; k < 3; k++) t[k] = v[0] * m[0][k] + v[1] * m[1][k] + v[2] * m[2][k]; memcpy(o, t, sizeof t);
}
static int inv3(float o[3][3], const float m[3][3]) {
    float d = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    if (fabsf(d) < 1e-12f) return 0;
    float t[3][3] = {
        { (m[1][1] * m[2][2] - m[1][2] * m[2][1]) / d, (m[0][2] * m[2][1] - m[0][1] * m[2][2]) / d, (m[0][1] * m[1][2] - m[0][2] * m[1][1]) / d },
        { (m[1][2] * m[2][0] - m[1][0] * m[2][2]) / d, (m[0][0] * m[2][2] - m[0][2] * m[2][0]) / d, (m[0][2] * m[1][0] - m[0][0] * m[1][2]) / d },
        { (m[1][0] * m[2][1] - m[1][1] * m[2][0]) / d, (m[0][1] * m[2][0] - m[0][0] * m[2][1]) / d, (m[0][0] * m[1][1] - m[0][1] * m[1][0]) / d } };
    memcpy(o, t, sizeof t); return 1;
}
/* a scratchpad joint matrix, out of the world (inverse of the draw's W: wr, wp) */
static void spad_joint(Xf *x, float *scale, uint32_t a, const float wri[3][3], const float wp[3]) {
    float m[3][3], p[3];
    for (int i = 0; i < 3; i++) for (int k = 0; k < 3; k++) m[i][k] = gf(a + 16 * i + 4 * k);
    for (int k = 0; k < 3; k++) p[k] = gf(a + 48 + 4 * k) - wp[k];
    mul3(x->r, m, wri); vm3(x->p, p, wri);
    float s = 0;
    for (int i = 0; i < 3; i++) { float l = sqrtf(x->r[i][0] * x->r[i][0] + x->r[i][1] * x->r[i][1] + x->r[i][2] * x->r[i][2]);
        if (l > 1e-6f) for (int k = 0; k < 3; k++) x->r[i][k] /= l; s += l; }
    if (scale) *scale = s / 3;
}
static int joint_index(uint32_t list, const char *name) {
    for (int j = 0; j < (int)MR32(list + 8) && j < 256; j++) if (!strncmp((const char *)g_ram + list + 16 + j * 80, name, 16)) return j;
    return -1;
}
void f_00343FE8_orig(Ctx *c);
/* pose a motion at time 0 with no world matrix, through the player's own draw struct */
static void pose_rest(Ctx *c, uint32_t d, int mot) {
    uint8_t sv[128]; gread(sv, d, sizeof sv);
    static const float id[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    for (int k = 0; k < 16; k++) sf(d + 4 * k, id[k]);
    MW32(d + 64, (uint32_t)mot); sf(d + 72, 0);
    for (int f = 1; f <= 4; f <<= 1) { MW32(d + 76, (uint32_t)f); Ctx cs = *c; c->r[4].sd[0] = (int32_t)d; f_00343FE8_orig(c); *c = cs; }   /* keys, joints, world */
    gwrite(d, sv, sizeof sv);
}
static int rt_build(Ctx *c, uint32_t d, int ci) {
    uint32_t tab = MR32(MOT_TAB);
    int sr = mot_find("pl_std_shand1"), tg = mot_find(chars[ci].rest);
    if (rt.tab == tab && rt.src_rest == sr && rt.tgt == tg) return rt.ok;
    rt.tab = tab; rt.src_rest = sr; rt.tgt = tg; rt.ok = 0;
    if (sr < 0 || tg < 0) return 0;
    uint32_t ls = MR32(tab + (uint32_t)sr * 64 + 20), lt = MR32(tab + (uint32_t)tg * 64 + 20);
    if (!RAMOK(ls) || !RAMOK(lt)) return 0;
    rt.n = (int)MR32(lt + 8); rt.ns = (int)MR32(ls + 8);
    if (rt.n > RT_MAX || rt.ns > RT_MAX) return 0;
    static const float I3[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } }; static const float Z[3] = { 0, 0, 0 };
    pose_rest(c, d, sr);
    for (int j = 0; j < rt.ns; j++) spad_joint(&rt.s0[j], NULL, SPAD + 64 * j, I3, Z);
    pose_rest(c, d, tg);
    for (int j = 0; j < rt.n; j++) spad_joint(&rt.t0[j], &rt.scale[j], SPAD + 64 * j, I3, Z);
    for (int j = 0; j < rt.n; j++) {
        int pa = MR8(lt + 16 + j * 80 + 17); rt.parent[j] = pa < j ? pa : -1;
        rt.src[j] = -1;
        for (int m = 0; m < chars[ci].nmap; m++) if (!strncmp((const char *)g_ram + lt + 16 + j * 80, chars[ci].map[m][0], 16)) rt.src[j] = joint_index(ls, chars[ci].map[m][1]);
        if (rt.src[j] >= rt.ns) rt.src[j] = -1;
        if (rt.parent[j] >= 0) {   /* standing, on the parent */
            const Xf *P = &rt.t0[rt.parent[j]]; float pt[3][3]; tr3(pt, P->r);
            mul3(rt.lr[j], rt.t0[j].r, pt);
            float d3[3] = { rt.t0[j].p[0] - P->p[0], rt.t0[j].p[1] - P->p[1], rt.t0[j].p[2] - P->p[2] }; vm3(rt.lp[j], d3, pt);
        }
    }
    int th = -1, sh = -1;
    for (int j = 0; j < rt.n; j++) if (rt.parent[j] < 0 && rt.src[j] >= 0 && (th < 0 || rt.t0[j].p[1] < rt.t0[th].p[1])) { th = j; sh = rt.src[j]; }
    rt.k = th >= 0 && rt.s0[sh].p[1] > 1e-3f ? rt.t0[th].p[1] / rt.s0[sh].p[1] : 1;
    rt.ok = 1;
    INFO("[chars] %s on Spike's skeleton: %d joints (Spike's %d), height %.2f\n", chars[ci].model, rt.n, rt.ns, rt.k);
    return 1;
}
/* after the game has posed Spike's motion for the player's draw d */
static void retarget(Ctx *c, uint32_t d, const float *W) {
    if (!rt_build(c, d, char_on)) return;
    float wr[3][3], wri[3][3], wp[3];
    for (int i = 0; i < 3; i++) for (int k = 0; k < 3; k++) wr[i][k] = W[4 * i + k];
    for (int k = 0; k < 3; k++) wp[k] = W[12 + k];
    if (!inv3(wri, wr)) return;
    float wsc = sqrtf(wr[0][0] * wr[0][0] + wr[0][1] * wr[0][1] + wr[0][2] * wr[0][2]), wn[3][3];   /* the character keeps its own size */
    if (wsc < 1e-6f) return;
    for (int i = 0; i < 3; i++) for (int k = 0; k < 3; k++) wn[i][k] = wr[i][k] / wsc;
    Xf s[RT_MAX], o[RT_MAX];
    for (int j = 0; j < rt.ns; j++) spad_joint(&s[j], NULL, SPAD + 64 * j, wri, wp);
    for (int j = 0; j < rt.ns; j++) for (int k = 0; k < 16; k++) rt_src_world[j][k] = gf(SPAD + 64 * j + 4 * k);
    for (int j = 0; j < rt.n; j++) {
        int pa = rt.parent[j], sj = rt.src[j];
        if (sj >= 0) {   /* turned as Spike's turns: t0 . s0^-1 . s */
            float a[3][3], b[3][3]; tr3(b, rt.s0[sj].r); mul3(a, rt.t0[j].r, b); mul3(o[j].r, a, s[sj].r);
        } else if (pa >= 0) mul3(o[j].r, rt.lr[j], o[pa].r);
        else memcpy(o[j].r, rt.t0[j].r, sizeof o[j].r);
        if (pa >= 0) { vm3(o[j].p, rt.lp[j], o[pa].r); for (int k = 0; k < 3; k++) o[j].p[k] += o[pa].p[k]; }
        else if (sj >= 0) for (int k = 0; k < 3; k++) o[j].p[k] = rt.t0[j].p[k] + (s[sj].p[k] - rt.s0[sj].p[k]) * rt.k;
        else memcpy(o[j].p, rt.t0[j].p, sizeof o[j].p);
    }
    for (int j = 0; j < rt.n; j++) {   /* back into the world */
        float m[3][3], p[3]; mul3(m, o[j].r, wn); vm3(p, o[j].p, wn);
        if (rt.src[j] >= 0) for (int k = 0; k < 3; k++) rt_src_world[rt.src[j]][12 + k] = p[k] + wp[k];   /* Spike's joint, where the character's is */
        uint32_t a = SPAD + 64 * j;
        for (int i = 0; i < 3; i++) { for (int k = 0; k < 3; k++) sf(a + 16 * i + 4 * k, m[i][k] * rt.scale[j]); sf(a + 16 * i + 12, 0); }
        for (int k = 0; k < 3; k++) sf(a + 48 + 4 * k, p[k] + wp[k]);
        sf(a + 60, 1);
        for (int k = 0; k < 16; k++) rt_out[j][k] = gf(a + 4 * k);
    }
    for (int i = 0; i < 3; i++) { for (int k = 0; k < 3; k++) rt_W[4 * i + k] = wn[i][k]; rt_W[4 * i + 3] = 0; }
    for (int k = 0; k < 3; k++) rt_W[12 + k] = wp[k];
    rt_W[15] = 1;
}
/* a joint of Spike's asked for once the character's pose is in (gadgets in hand, effects):
 * Spike's joint as it is turned, where the character's matching joint is (into the spare
 * room at the end of the player's part list, as for the monkeys) */
static int chars_bone(Ctx *c) {
    if (char_on < 0 || !rt_posed || !rt.ok || !pl_is_p1()) return 0;
    int m = c->r[4].sw[0];
    if (m != rt.tgt && m != rt_src_mot) return 0;
    uint32_t l = MR32(MR32(MOT_TAB) + (uint32_t)rt_src_mot * 64 + 20), lt = MR32(MR32(MOT_TAB) + (uint32_t)rt.tgt * 64 + 20);
    if (!RAMOK(l) || !RAMOK(lt)) return 0;
    char nm[17] = {0}; memcpy(nm, c->r[5].ub, 16);
    const float *mx; int j;
    if ((j = joint_index(l, nm)) >= 0 && j < rt.ns) mx = rt_src_world[j];        /* Spike's, for his gadgets */
    else if ((j = joint_index(lt, nm)) >= 0 && j < rt.n) mx = rt_out[j];         /* the character's own */
    else mx = rt_W;   /* none: the model's place (the Professor's head piece asks with no name) */
    uint32_t o = PL_PARTS + 16 + 7 * 16;
    for (int k = 0; k < 16; k++) sf(o + 4 * k, mx[k]);
    c->r[2].sd[0] = (int32_t)o;
    return 1;
}
static void chars_frame(Ctx *c, int ci) {
    static int tries; static uint32_t scene = ~0u;
    uint32_t sc = MR32(0x3b35a0u + 0x80); if (sc != scene) { scene = sc; tries = 0; }
    int m = model_index(c, chars[ci].model);
    if ((m < 0 || mot_find(chars[ci].rest) < 0) && MR32(GS_MODE) == 3) { archive_load(c, "STA_COM", MK_GROUP << 1, &tries); tries = 3; m = model_index(c, chars[ci].model); }
    if (m < 0 || mot_find(chars[ci].rest) < 0) { static int said; if (MR32(GS_MODE) == 3 && said++ < 4) INFO("[chars] %s: model %d, motion %d, mode %u\n", chars[ci].model, m, mot_find(chars[ci].rest), MR32(GS_MODE)); char_on = -1; restore_player(c); return; }
    char n[16] = {0}; snprintf(n, 16, "%s", chars[ci].model);
    gwrite(PL_BASE + 848, n, 16);
    MW32(PL_BASE + 148, (uint32_t)m);
    MW32(PL_BASE + 844, 0xffffffffu);
    MW32(PL_PARTS + 12, 0);
    char_on = ci;
}
/* 0x343fe8(draw): queues a model at the matrix draw+0.  The slingshot shots (0x289d28) turn
 * the model's z toward their flight and tumble the homing ones; the monkeys' missile is
 * modelled nose +z, so for it the matrix is rebuilt: nose along the flight, no tumble */
static void spt_measure(Ctx *c, uint32_t d);
void hle_model_draw(Ctx *c) {
    uint32_t d = c->r[4].uw[0], ra = c->r[31].uw[0], fl = MR32(d + 76);
    if (d == PL_BASE + 80 && (fl & 1)) rt_posed = 0;   /* a new pose: Spike's until the world step */
    if (spt_on && d == PL_BASE + 80 && (fl & 1) && pl_is_p1()) spt_measure(c, d);
    if (ab_kind == MK_GREEN && (ra == 0x0028A10C || ra == 0x0028A11C) && MR32(d + 68) == (uint32_t)model_index(c, "pe_missile")) {
        uint32_t b = d - 80;
        float v[3] = { gf(b + 1008), gf(b + 1012), gf(b + 1016) }, l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        float r0[3] = { gf(d), gf(d + 4), gf(d + 8) }, s = sqrtf(r0[0] * r0[0] + r0[1] * r0[1] + r0[2] * r0[2]);
        if (l > 1e-4f && s > 1e-4f) {
            float z[3] = { v[0] / l, v[1] / l, v[2] / l };
            float x[3] = { z[2], 0, -z[0] }, xl = sqrtf(x[0] * x[0] + x[2] * x[2]);   /* level, across the flight */
            if (xl < 1e-3f) { x[0] = 1; x[2] = 0; xl = 1; }
            x[0] /= xl; x[2] /= xl;
            float y[3] = { z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0] };
            for (int i = 0; i < 3; i++) { sf(d + 4 * i, x[i] * s); sf(d + 16 + 4 * i, y[i] * s); sf(d + 32 + 4 * i, z[i] * s); }
        }
    }
    f_00343FE8_orig(c);
    static float m2[RT_MAX][16];   /* the joints step (flag 2) leaves Spike's joints in model space */
    if (char_on >= 0 && d == PL_BASE + 80 && (fl & 2) && pl_is_p1()) for (int j = 0; j < RT_MAX; j++) for (int k = 0; k < 16; k++) m2[j][k] = gf(SPAD + 64 * j + 4 * k);
    float W[16] = {0};
    if (char_on >= 0 && d == PL_BASE + 80 && (fl & 4) && pl_is_p1()) {   /* the world step's W, from the root: m4 = m2 . W */
        float a[3][3], ai[3][3], b[3][3], ap[3], bp[3];
        for (int i = 0; i < 3; i++) for (int k = 0; k < 3; k++) { a[i][k] = m2[0][4 * i + k]; b[i][k] = gf(SPAD + 16 * i + 4 * k); }
        for (int k = 0; k < 3; k++) { ap[k] = m2[0][12 + k]; bp[k] = gf(SPAD + 48 + 4 * k); }
        if (inv3(ai, a)) {
            float wr[3][3], t[3]; mul3(wr, ai, b);
            vm3(t, ap, wr); for (int k = 0; k < 3; k++) t[k] = bp[k] - t[k];
            for (int i = 0; i < 3; i++) for (int k = 0; k < 3; k++) W[4 * i + k] = wr[i][k];
            for (int k = 0; k < 3; k++) W[12 + k] = t[k];
            W[15] = 1;
        }
    }
    /* the player's pose is built in steps (0x29d208): keys (flag 1), joints (2), world (4) */
    if (char_on >= 0 && d == PL_BASE + 80 && (fl & 4) && pl_is_p1() && W[15] != 0) {
        rt_src_mot = (int)MR32(d + 64);
        retarget(c, d, W);
        if (rt.ok) { MW32(d + 64, (uint32_t)rt.tgt); rt_posed = 1; }   /* the draw binds the model's joints by name to the motion's */
    }
}

/* ---- Specter.  His model (bs_spector) and his motions (bs_spt_*) come with the final
 * showdown's archive (BOS_H); in other stages it is loaded once, like the Station's for
 * Natalie.  His skeleton is his own and his motions are made for it, so he goes the
 * monkeys' way: the player's motion is swapped for the Specter motion that fits it. */
#define SPT_MODEL "bs_spector"
#define SPT_DASH 14
static const char *spt_map(const char *p) {
    static const struct { const char *key, *mot; } map[] = {
        { "dmg_fall", "bs_spt_fall1" }, { "dmg", "bs_spt_dmg" }, { "div_dmg", "bs_spt_dmg" },
        { "jmp_sland", "bs_spt_wait" }, { "jmp_swland", "bs_spt_run" }, { "jmp", "bs_spt_avoid" }, { "hng", "bs_spt_fall1" },
        { "fal", "bs_spt_fall1" }, { "tmb_fly", "bs_spt_fall1" }, { "hip_atk", "bs_spt_fall1" },
        { "run", "bs_spt_run" }, { "hop_dash", "bs_spt_run" }, { "slw", "bs_spt_run" }, { "wlk", "bs_spt_run" }, { "swm", "bs_spt_run" },
        { "net_", "bs_spt_atk1" }, { "spn", "bs_spt_kawasi" }, { "bow_swing", "bs_spt_atk1" }, { "bow_roll", "bs_spt_kawasi" }, { "hop_roll", "bs_spt_kawasi" },
        { "ban_pit", "bs_spt_thrw1" }, { "ban_lpit", "bs_spt_thrw1" }, { "mag_", "bs_spt_sht1" }, { "gun_", "bs_spt_sht1" },
        { "mpn_sht", "bs_spt_atk1" }, { "pac_shot", "bs_spt_sht1" }, { "clr", "bs_spt_laugh" }, { "wit_a", "bs_spt_prov" }, { "wit", "bs_spt_laugh" },
    };
    static const char *test; static int ti = -1;   /* AE2_SPTTEST=n: every n frames the next bs_spt_ motion (testing) */
    { static int n = -1; if (n < 0) { const char *e = getenv("AE2_SPTTEST"); n = e ? atoi(e) : 0; }
      if (n > 0) { extern uint32_t g_pad_polls; int k = (int)(g_pad_polls / (uint32_t)n);
        if (k != ti) { ti = k; int seen = 0; test = NULL;
            for (int i = 0; i < 1024 && !test; i++) { const char *nm = mot_name(i); if (!strncmp(nm, "bs_spt", 6) && seen++ == k % 48) { static char b[17]; snprintf(b, 17, "%s", nm); test = b; } }
            if (test) INFO("[spt] test %s\n", test); }
        if (test) return test; } }
    if (strncmp(p, "pl_", 3)) return NULL;
    for (size_t i = 0; i < sizeof map / sizeof *map; i++) if (strstr(p + 3, map[i].key)) return map[i].mot;
    return "bs_spt_wait";
}
static void spt_frame(Ctx *c) {
    static int tries; static uint32_t scene = ~0u;
    uint32_t sc = MR32(0x3b35a0u + 0x80); if (sc != scene) { scene = sc; tries = 0; }
    int m = model_index(c, SPT_MODEL);
    if ((m < 0 || mot_find("bs_spt_wait") < 0) && MR32(GS_MODE) == 3) { archive_load(c, "BOS_H_COM", MK_GROUP << 2, &tries); tries = 3; m = model_index(c, SPT_MODEL); }
    if (m < 0 || mot_find("bs_spt_wait") < 0) { abilities(c, -1); spt_dash = spt_taunt = 0; restore_player(c); return; }
    char n[16] = {0}; snprintf(n, 16, "%s", SPT_MODEL);
    gwrite(PL_BASE + 848, n, 16);
    MW32(PL_BASE + 148, (uint32_t)m);
    MW32(PL_BASE + 844, 0xffffffffu);
    MW32(PL_PARTS + 12, 0);
    monkey_on = 1; spt_on = 1;
    abilities(c, MK_SPECTER);
    /* L1: a warp dash (fast, untouchable, his dodge); R3 while standing: his laugh (L2 is the look-around view) */
    static uint16_t last; static int cool;
    uint16_t b = host_pad_buttons(0);
    int busy = MR8(PL_BASE + 882) != 0;
    if (spt_dash > 0) spt_dash--;
    if (cool > 0) cool--;
    if ((b & 1u << 10) && !(last & 1u << 10) && !cool && !busy) { spt_dash = SPT_DASH; cool = SPT_DASH + 20; }
    spt_taunt = (b & 1u << 2) && MR32(PL_BASE + 868) == 0 && !busy && !spt_dash;
    last = b;
}
/* his dodge while dashing, his laugh while R3 is held */
static void spt_pose(uint32_t p) {
    static int held;
    const char *nm = spt_dash > 0 ? "bs_spt_kawasi" : spt_taunt ? "bs_spt_laugh" : NULL;
    if (!spt_taunt) held = 0;
    int g = nm ? mot_find(nm) : -1; if (g < 0) return;
    float l = mot_len(g), t = spt_dash > 0 ? l * (float)(SPT_DASH - spt_dash) / SPT_DASH : (float)(held++ % (int)(l + 1));
    uint32_t v; memcpy(&v, &t, 4);
    MW32(p + 1348, (uint32_t)g); MW32(p + 1316, v);
    MW32(p + 1352, (uint32_t)g); MW32(p + 1312, v);
}
/* the gadgets sit in his hand as in Spike's: his right hand's turn from Spike's when
 * both stand (Spike's pl_std_shand1, his bs_spt_wait), measured once through the
 * player's own draw before its pose is built */
static void spt_measure(Ctx *c, uint32_t d) {
    static uint32_t tab;
    if (tab == MR32(MOT_TAB)) return;
    int ms = mot_find("pl_std_shand1"), mt = mot_find("bs_spt_wait");
    if (ms < 0 || mt < 0) return;
    tab = MR32(MOT_TAB);
    uint32_t ls = MR32(tab + (uint32_t)ms * 64 + 20), lt = MR32(tab + (uint32_t)mt * 64 + 20);
    int js = RAMOK(ls) ? joint_index(ls, "jnt_rhand") : -1, jt = RAMOK(lt) ? joint_index(lt, "jnt_handr") : -1;
    if (js < 0 || jt < 0) return;
    static const float I3[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } }; static const float Z[3] = { 0, 0, 0 };
    Xf S, M; float mi[3][3];
    pose_rest(c, d, ms); spad_joint(&S, NULL, SPAD + 64 * js, I3, Z);
    pose_rest(c, d, mt); spad_joint(&M, NULL, SPAD + 64 * jt, I3, Z);
    if (!inv3(mi, M.r)) return;
    mul3(spt_C, S.r, mi);
    INFO("[spt] hand turn %.3f %.3f %.3f / %.3f %.3f %.3f / %.3f %.3f %.3f\n", spt_C[0][0], spt_C[0][1], spt_C[0][2], spt_C[1][0], spt_C[1][1], spt_C[1][2], spt_C[2][0], spt_C[2][1], spt_C[2][2]);
}
