/* OpenGL presenter (host_gl.c) */
#pragma once
#include <stdint.h>
struct SDL_Window;
int hostgl_init(struct SDL_Window *w, int want_gs_context);
void *hostgl_gs_context(void);
struct SDL_Window *hostgl_gs_window(void);
struct SDL_Window *hostgl_window(void);
void hostgl_present_sw(const uint32_t *px, int w, int h);
void hostgl_present_hw(unsigned tex, float umax, float vmax, void *fence);
void hostgl_present_none(void);
unsigned gl_compile(const char *vs, const char *fs, const char *what);
