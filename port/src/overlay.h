/* On-screen overlay (debug menu, FPS counter): a list of filled rectangles and glyphs
 * in a logical 1280x720 space, drawn on top of the game picture by the presenter. */
#pragma once
#include <stdint.h>

#define OV_W 1280
#define OV_H 720
typedef struct { int16_t x, y, w, h; uint32_t rgba; int16_t glyph; } OvItem;   /* glyph < 0: filled rect */
typedef struct { OvItem it[4096]; int n; } OvList;

static inline void ov_rect(OvList *l, int x, int y, int w, int h, uint32_t rgba) {
    if (l->n < 4096) l->it[l->n++] = (OvItem){(int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h, rgba, -1};
}
int ov_text(OvList *l, int x, int y, const char *s, uint32_t rgba);   /* returns the end x */

/* main thread: menu input and drawing */
int debug_menu_is_open(void);
void debug_menu_key(int key);                 /* DMK_* below */
void debug_menu_draw(OvList *l);
enum { DMK_TOGGLE = 1, DMK_UP, DMK_DOWN, DMK_LEFT, DMK_RIGHT, DMK_OK, DMK_BACK, DMK_FREECAM, DMK_NOCLIP, DMK_CONTROL };
extern int g_control_mode;   /* debug_menu.c: 0 one player per device, 1 all input to player 1, 2 all input to player 2 */
extern volatile int g_freecam, g_noclip;   /* debug: free camera (Hikaru stays put) / no clip (Hikaru flies through walls) */

/* settings the presenter reads */
extern volatile int g_show_fps;
extern volatile int g_widescreen;      /* 16:9 output (and the game's own 16:9 mode) */
extern volatile float g_fps_game, g_fps_draw;
extern int g_hw_request, g_hw_scale, g_hw_active;
extern const char *g_hw_fail;   /* renderer: 0 software, 1 GPU; GPU internal scale */
int host_use_gl(void);
