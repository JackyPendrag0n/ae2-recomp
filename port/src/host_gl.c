/* OpenGL presentation (main thread): draws the game picture - either a software frame
 * uploaded as a texture, or a texture rendered by the GPU renderer on the GS thread in a
 * shared context - into the window with the right aspect ratio, then the overlay. */
#define GL_IMPL
#include "rt.h"
#include "gl.h"
#include "overlay.h"
#include "font_data.h"
#include "host_gl.h"
#include <SDL2/SDL.h>

int gl_load(void *(*getproc)(const char *)) {
#define GLX_LOAD(ret, name, args) name = (ret (*) args)getproc(#name);
    GL_FUNCS(GLX_LOAD)
#undef GLX_LOAD
    /* required set */
    if (!glClear || !glCreateShader || !glGenFramebuffers || !glGenVertexArrays || !glFenceSync || !glBlendFuncSeparate || !glVertexAttribIPointer) return -1;
    return 0;
}
int gl_has_ext(const char *name) {
    GLint n = 0; glGetIntegerv(GL_NUM_EXTENSIONS, &n);
    for (GLint i = 0; i < n; i++) { const char *e = (const char *)glGetStringi(GL_EXTENSIONS, (GLuint)i); if (e && !strcmp(e, name)) return 1; }
    return 0;
}

GLuint gl_compile(const char *vs, const char *fs, const char *what) {
    GLuint p = glCreateProgram();
    const char *src[2] = {vs, fs}; GLenum ty[2] = {GL_VERTEX_SHADER, GL_FRAGMENT_SHADER};
    for (int i = 0; i < 2; i++) {
        GLuint s = glCreateShader(ty[i]);
        glShaderSource(s, 1, &src[i], NULL); glCompileShader(s);
        GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) { char log[4096]; glGetShaderInfoLog(s, sizeof log, NULL, log); WARN("[gl] %s %s shader: %s\n", what, i ? "fragment" : "vertex", log); }
        glAttachShader(p, s); glDeleteShader(s);
    }
    glBindAttribLocation(p, 0, "a0"); glBindAttribLocation(p, 1, "a1"); glBindAttribLocation(p, 2, "a2");
    glBindAttribLocation(p, 3, "a3"); glBindAttribLocation(p, 4, "a4");
    glLinkProgram(p);
    GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { char log[4096]; glGetProgramInfoLog(p, sizeof log, NULL, log); WARN("[gl] %s link: %s\n", what, log); return 0; }
    return p;
}

static SDL_Window *gwin;
static SDL_GLContext ctx_main, ctx_gs;
static SDL_Window *gs_win;   /* hidden 1x1 window: the GS thread's own surface (EGL/Wayland allow a surface
                              * to be current on one thread only; the GS renders into its own FBOs anyway) */
static GLuint prog_pic, prog_ov, vao, vbo, sw_tex, font_tex;
static int sw_w, sw_h;
static GLint u_pic_tex, u_ov_tex, u_pic_gam;
extern float g_gfx_gamma;

static const char *VS_PIC =
    "#version 330 core\nin vec2 a0; in vec2 a1; out vec2 uv;\n"
    "void main() { uv = a1; gl_Position = vec4(a0, 0.0, 1.0); }\n";
static const char *FS_PIC =   /* gam: brightness (Options > Graphics), 1 = as the game */
    "#version 330 core\nin vec2 uv; uniform sampler2D tex; uniform float gam; out vec4 o;\n"
    "void main() { o = vec4(pow(texture(tex, uv).rgb, vec3(gam)), 1.0); }\n";
static const char *VS_OV =
    "#version 330 core\nin vec2 a0; in vec2 a1; in vec4 a2; out vec2 uv; out vec4 col;\n"
    "void main() { uv = a1; col = a2; gl_Position = vec4(a0, 0.0, 1.0); }\n";
static const char *FS_OV =
    "#version 330 core\nin vec2 uv; in vec4 col; uniform sampler2D tex; out vec4 o;\n"
    "void main() { o = vec4(col.rgb, col.a * texture(tex, uv).r); }\n";

int hostgl_init(SDL_Window *w, int want_gs_context) {
    gwin = w;
    ctx_main = SDL_GL_CreateContext(w);
    if (!ctx_main) { WARN("[gl] cannot create an OpenGL 3.3 context: %s\n", SDL_GetError()); return -1; }
    if (gl_load(SDL_GL_GetProcAddress)) { WARN("[gl] OpenGL functions missing\n"); SDL_GL_DeleteContext(ctx_main); ctx_main = NULL; return -1; }
    INFO("[gl] %s / %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
    if (want_gs_context) {
        SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
        gs_win = SDL_CreateWindow("ae2 gs", 0, 0, 1, 1, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
        if (!gs_win) WARN("[gl] no hidden window for the GS context (%s); sharing the main one\n", SDL_GetError());
        ctx_gs = SDL_GL_CreateContext(gs_win ? gs_win : w);
        if (!ctx_gs) WARN("[gl] no second (shared) context: %s\n", SDL_GetError());
        SDL_GL_MakeCurrent(w, ctx_main);
    }
    if (SDL_GL_SetSwapInterval(-1) != 0) SDL_GL_SetSwapInterval(1);   /* adaptive vsync if available */
    prog_pic = gl_compile(VS_PIC, FS_PIC, "present");
    prog_ov = gl_compile(VS_OV, FS_OV, "overlay");
    if (!prog_pic || !prog_ov) return -1;
    u_pic_tex = glGetUniformLocation(prog_pic, "tex");
    u_ov_tex = glGetUniformLocation(prog_ov, "tex");
    glGenVertexArrays(1, &vao); glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    /* font atlas: glyphs side by side, plus an opaque 2x2 block at the end for filled rects */
    int aw = (FONT_N + 1) * FONT_CW, ah = FONT_CH;
    uint8_t *px = calloc((size_t)aw * ah, 1);
    for (int g = 0; g < FONT_N; g++) for (int y = 0; y < FONT_CH; y++) for (int x = 0; x < FONT_CW; x++) px[y * aw + g * FONT_CW + x] = font_alpha[g][y * FONT_CW + x];
    for (int y = 0; y < FONT_CH; y++) for (int x = 0; x < FONT_CW; x++) px[y * aw + FONT_N * FONT_CW + x] = 255;
    glGenTextures(1, &font_tex); glBindTexture(GL_TEXTURE_2D, font_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, aw, ah, 0, GL_RED, GL_UNSIGNED_BYTE, px);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    free(px);
    glGenTextures(1, &sw_tex);
    return 0;
}
void *hostgl_gs_context(void) { return ctx_gs; }
SDL_Window *hostgl_window(void) { return gwin; }
SDL_Window *hostgl_gs_window(void) { return gs_win ? gs_win : gwin; }

/* ---- frames */
static void draw_quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1) {
    float q[16] = {x0, y0, u0, v0, x1, y0, u1, v0, x0, y1, u0, v1, x1, y1, u1, v1};
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof q, q, GL_STREAM_DRAW);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, (void *)0);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, (void *)8);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}
static void draw_overlay(int ww, int wh) {
    static OvList l; l.n = 0;
    debug_menu_draw(&l);
    if (!l.n) return;
    float sc = (float)ww / OV_W < (float)wh / OV_H ? (float)ww / OV_W : (float)wh / OV_H;
    float ox = (ww - OV_W * sc) / 2, oy = (wh - OV_H * sc) / 2;
    float aw = (float)((FONT_N + 1) * FONT_CW);
    static float v[4096 * 6 * 8];
    int n = 0;
    for (int i = 0; i < l.n; i++) {
        OvItem *it = &l.it[i];
        float x0 = (ox + it->x * sc) / ww * 2 - 1, x1 = (ox + (it->x + it->w) * sc) / ww * 2 - 1;
        float y0 = 1 - (oy + it->y * sc) / wh * 2, y1 = 1 - (oy + (it->y + it->h) * sc) / wh * 2;
        int g = it->glyph < 0 ? FONT_N : it->glyph;
        float u0 = (g * FONT_CW + (it->glyph < 0 ? 2 : 0)) / aw, u1 = (g * FONT_CW + (it->glyph < 0 ? 8 : FONT_CW)) / aw;
        float vv0 = it->glyph < 0 ? 0.25f : 0, vv1 = it->glyph < 0 ? 0.75f : 1;
        float r = (it->rgba & 255) / 255.f, gg = ((it->rgba >> 8) & 255) / 255.f, b = ((it->rgba >> 16) & 255) / 255.f, a = (it->rgba >> 24) / 255.f;
        float c[6][4] = {{x0, y0, u0, vv0}, {x1, y0, u1, vv0}, {x0, y1, u0, vv1}, {x1, y0, u1, vv0}, {x1, y1, u1, vv1}, {x0, y1, u0, vv1}};
        for (int k = 0; k < 6; k++) { float *o = &v[n * 8]; o[0] = c[k][0]; o[1] = c[k][1]; o[2] = c[k][2]; o[3] = c[k][3]; o[4] = r; o[5] = gg; o[6] = b; o[7] = a; n++; }
    }
    glUseProgram(prog_ov); glUniform1i(u_ov_tex, 0);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, font_tex);
    glEnable(GL_BLEND); glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)n * 32, v, GL_STREAM_DRAW);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 32, (void *)0);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 32, (void *)8);
    glEnableVertexAttribArray(2); glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, 32, (void *)16);
    glDrawArrays(GL_TRIANGLES, 0, n);
    glDisableVertexAttribArray(2);
    glDisable(GL_BLEND);
}
/* tex: texture to show (u/v range of the picture inside it), aspect from the game setting */
static void present_tex(GLuint tex, float umax, float vmax, int flip) {
    int ww, wh; SDL_GL_GetDrawableSize(gwin, &ww, &wh);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, ww, wh);
    glDisable(GL_SCISSOR_TEST); glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND); glDisable(GL_CULL_FACE);
    glColorMask(1, 1, 1, 1);
    glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
    int an = g_widescreen ? 16 : 4, ad = g_widescreen ? 9 : 3;
    int dw = ww, dh = ww * ad / an;
    if (dh > wh) { dh = wh; dw = wh * an / ad; }
    float x0 = (float)(ww - dw) / ww - 1 + 0, y0 = (float)(wh - dh) / wh - 1;
    if (tex) {
        glUseProgram(prog_pic); glUniform1i(u_pic_tex, 0); glUniform1f(u_pic_gam, g_gfx_gamma);
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, tex);
        if (flip) draw_quad(x0, -y0, -x0, y0, 0, 0, umax, vmax);     /* row 0 at the top */
        else draw_quad(x0, y0, -x0, -y0, 0, 0, umax, vmax);
    }
    draw_overlay(ww, wh);
    /* AE2_SHOT_EVERY=n, AE2_SHOT_DIR=dir: save every n-th presented window image (testing) */
    { static int every = -1, n; static const char *dir;
      if (every < 0) { const char *e = getenv("AE2_SHOT_EVERY"); every = e ? atoi(e) : 0; dir = getenv("AE2_SHOT_DIR"); if (!dir) dir = "."; }
      if (every && ++n % every == 0) {
          uint8_t *px = malloc((size_t)ww * wh * 4);
          glReadPixels(0, 0, ww, wh, GL_RGBA, GL_UNSIGNED_BYTE, px);
          extern uint32_t g_pad_polls;
          char path[600]; snprintf(path, sizeof path, "%s/w%05u.ppm", dir, g_pad_polls);
          FILE *f = fopen(path, "wb");
          if (f) { fprintf(f, "P6\n%d %d\n255\n", ww, wh); for (int y = wh - 1; y >= 0; y--) for (int x = 0; x < ww; x++) fwrite(px + (y * ww + x) * 4, 1, 3, f); fclose(f); }
          free(px);
      } }
    SDL_GL_SwapWindow(gwin);
}
void hostgl_present_sw(const uint32_t *px, int w, int h) {
    glBindTexture(GL_TEXTURE_2D, sw_tex);
    if (w != sw_w || h != sw_h) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        sw_w = w; sw_h = h;
    }
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    present_tex(sw_tex, 1, 1, 1);
}
/* a frame from the GPU renderer: texture in the shared GS context, ready after 'fence' */
void hostgl_present_hw(unsigned tex, float umax, float vmax, void *fence) {
    if (fence) { glWaitSync((GLsync)fence, 0, GL_TIMEOUT_IGNORED); }
    present_tex(tex, umax, vmax, 1);
}
void hostgl_present_none(void) { present_tex(0, 1, 1, 0); }
