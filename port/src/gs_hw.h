/* GPU renderer: the GS drawn with OpenGL 3.3, at 1x..8x internal resolution.
 *
 * The GS front end (GIF parsing, registers, primitive assembly, texture decoding with
 * CLUTs) is shared with the software renderer; only rasterisation moves to the GPU.
 *   - Render targets live in GPU textures at the chosen scale, keyed by frame-buffer
 *     address/width. Depth buffers likewise (32-bit float depth = Z / 2^32, exact for
 *     the range this game uses).
 *   - Textures are decoded by the shared CPU path (tex_lookup) and uploaded once per
 *     decode; a texture whose address is a render target samples that target directly
 *     (render-to-texture effects keep their resolution).
 *   - GS colour maths is done in the fragment shader in 0..255 units with the GS's
 *     integer truncation. Blending uses dual-source fixed-function blending where the
 *     GS equation allows; the rest (destination-dependent forms, factors above 1.0,
 *     destination alpha test) read the destination in the shader, through a texture
 *     barrier when available or a copy of the target otherwise.
 *   - VRAM stays the authority for everything the CPU reads: when a texture, CLUT,
 *     transfer or the display reads a region that only exists on the GPU, the target is
 *     read back (downscaled) first; uploads into a target region are pushed to the GPU.
 *
 * Included from gs.c after the software renderer (uses its statics). All GL calls happen
 * on the thread that runs the GS front end, in a context shared with the presenter. */
#include "gl.h"
#include "host_gl.h"
#include <SDL2/SDL.h>

extern int g_hw_request;            /* 0 software, 1 GPU (set by main / menu) */
int g_hw_scale = 2;                 /* requested internal resolution multiplier */
static int hw_on, hw_failed, hw_inited, hw_scale = 1;
static int hw_barrier;              /* glTextureBarrier available */
int g_hw_active;
const char *g_hw_fail;              /* why the GPU renderer could not start (shown by the menu) */                    /* reported to the menu */

/* ------------------------------------------------------------------ GL objects */
static GLuint hw_prog, hw_vao, hw_vbo, hw_prog_copy;
static GLint u_tex, u_dst, u_vp, u_mode, u_atst, u_blend, u_misc, u_dstoff, u_fix, u_fog;
static GLint uc_tex, uc_rect;
static GLuint dst_copy_tex, dst_copy_fbo; static int dst_copy_w, dst_copy_h;
static GLuint disp_tex[3], disp_fbo[3]; static int disp_w[3], disp_h[3], disp_cur;
static GLsync disp_fence[3];

typedef struct {
    float x, y, z, f;       /* native pixels (offset applied), Z as float (exact below 2^24), fog */
    float r, g, b, a;       /* 0..255 */
    float s, t, q, pad;     /* texture coordinates, normalised (s/q, t/q in 0..1 over the texture) */
} HwV;
#define HW_MAXV 65536
static HwV hw_v[HW_MAXV]; static int hw_nv;

/* ------------------------------------------------------------------ render targets */
typedef struct {
    int used;
    uint32_t fbp, fbw; int is16;
    int w, h;                   /* native size of the GL texture */
    int vh;                     /* rows in use (whole VRAM pages): the VRAM the target claims */
    GLuint tex, fbo;
    uint32_t zbp; GLuint ztex;  /* depth attached to fbo (0 = none) */
    int gpu_dirty;              /* holds drawing not yet in VRAM ... */
    int dx0, dy0, dx1, dy1;     /* ... in this rectangle (native pixels) */
    uint64_t lru;
} HwRT;
/* VRAM blocks (256 bytes) spanned by rows y0..y1 of a target: whole page rows */
static void hw_rt_rows_blocks(const HwRT *o, int y0, int y1, uint32_t *b0, uint32_t *b1) {
    int ph = o->is16 ? 64 : 32;
    *b0 = o->fbp + (uint32_t)(y0 / ph) * o->fbw * 32;
    *b1 = o->fbp + (uint32_t)(y1 / ph + 1) * o->fbw * 32;
}
static int hw_rt_overlaps(const HwRT *o, uint32_t bp, uint32_t nblocks) {
    uint32_t a0, a1; hw_rt_rows_blocks(o, 0, o->vh - 1, &a0, &a1);
    return a0 < bp + nblocks && bp < a1;
}
static int hw_rt_dirty_overlaps(const HwRT *o, uint32_t bp, uint32_t nblocks) {
    if (!o->gpu_dirty) return 0;
    uint32_t a0, a1; hw_rt_rows_blocks(o, o->dy0, o->dy1, &a0, &a1);
    return a0 < bp + nblocks && bp < a1;
}
static void hw_rt_mark(HwRT *o, int x0, int y0, int x1, int y1) {
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 >= o->w) x1 = o->w - 1; if (y1 >= o->h) y1 = o->h - 1;
    if (x0 > x1 || y0 > y1) return;
    if (!o->gpu_dirty) { o->dx0 = x0; o->dy0 = y0; o->dx1 = x1; o->dy1 = y1; o->gpu_dirty = 1; return; }
    if (x0 < o->dx0) o->dx0 = x0; if (y0 < o->dy0) o->dy0 = y0; if (x1 > o->dx1) o->dx1 = x1; if (y1 > o->dy1) o->dy1 = y1;
}
typedef struct { int used; uint32_t zbp; int w, h; GLuint tex; int gpu_dirty; uint64_t lru; } HwZ;
#define HW_NRT 16
static HwRT hw_rt[HW_NRT];
static HwZ hw_z[HW_NRT];
static uint64_t hw_tick;
/* GL textures for decoded (CPU) textures, parallel to the software texture cache */
static GLuint hw_ctex[TC_N]; static uint64_t hw_cseq[TC_N], hw_cepoch[TC_N]; static int hw_cw[TC_N], hw_ch[TC_N];
static int tc_last_idx;

/* ------------------------------------------------------------------ batch state */
typedef struct {
    HwRT *rt; HwZ *zb;
    GLuint tex; int tex_rt; int tw, th; int bilinear, wms, wmt;
    int prim;                   /* GL primitive (always triangles here) */
    int mode[4], atst[4], blend[4], misc[4];
    int fog[4];                 /* fge, FOGCOL r, g, b */
    float fix;
    int depth_func, depth_write, depth_test;
    int cmask[4];
    int bl_on, bl_eq, bl_sf, bl_df;
    int need_dst;
    int scx0, scy0, scx1, scy1;
    int pass2;                  /* alpha test with AFAIL: second pass for the failing pixels */
} HwState;
static HwState hw_cur, hw_pending; static int hw_have_cur;
static uint64_t hw_draws, hw_batches, hw_dstreads, hw_downloads, hw_uploads;

static const char *HW_VS =
    "#version 330 core\n"
    "in vec4 a0; in vec4 a1; in vec4 a2;\n"
    "uniform vec4 u_vp;\n"                          /* 2/W, 2/H, zscale */
    "out vec4 v_col; flat out vec4 v_colf; noperspective out vec3 v_stq; noperspective out float v_z; noperspective out float v_f;\n"
    "void main() {\n"
    "  vec2 p = (a0.xy + 0.5) * u_vp.xy - 1.0;\n"
    "  v_z = a0.z * u_vp.z;\n"
    "  gl_Position = vec4(p, 0.0, 1.0);\n"
    "  v_col = a1; v_colf = a1; v_stq = a2.xyz; v_f = a0.w;\n"
    "}\n";
/* u_mode: tme, tfx, tcc, iip     u_atst: atst(8=off), aref, afail-mode, date(0/1/2)
 * u_blend: shader blend (0 off, 1 on), A, B, C (A/B/D packed as a*16+b*4+d in .y) .. see setup
 * u_misc: fba, PABE, factor source (0 As, 1 FIX, 2 As as colour, 3 As multiplied into the colour), colclamp
 * u_fog: fge, FOGCOL r, g, b */
static const char *HW_FS =
    "#version 330 core\n"
    "in vec4 v_col; flat in vec4 v_colf; noperspective in vec3 v_stq; noperspective in float v_z; noperspective in float v_f;\n"
    "uniform sampler2D u_tex; uniform sampler2D u_dst;\n"
    "uniform ivec4 u_mode; uniform ivec4 u_atst; uniform ivec4 u_blend; uniform ivec4 u_misc; uniform ivec4 u_fog;\n"
    "uniform vec2 u_dstoff; uniform float u_fix; uniform int u_dbg;\n"
    "layout(location = 0, index = 0) out vec4 o_col;\n"
    "layout(location = 0, index = 1) out vec4 o_fac;\n"
    "void main() {\n"
    "  vec4 c = u_mode.w != 0 ? v_col : v_colf;\n"
    "  c = floor(c + 0.0001);\n"
    "  vec3 rgb = c.rgb; float a = c.a;\n"
    "  if (u_mode.x != 0) {\n"
    "    vec4 t = floor(texture(u_tex, v_stq.xy / v_stq.z) * 255.0 + 0.5);\n"
    "    int tfx = u_mode.y;\n"
    "    if (tfx == 0) { rgb = floor(t.rgb * c.rgb / 128.0); a = u_mode.z != 0 ? floor(t.a * c.a / 128.0) : c.a; }\n"
    "    else if (tfx == 1) { rgb = t.rgb; a = u_mode.z != 0 ? t.a : c.a; }\n"
    "    else if (tfx == 2) { rgb = floor(t.rgb * c.rgb / 128.0) + c.a; a = u_mode.z != 0 ? t.a + c.a : c.a; }\n"
    "    else { rgb = floor(t.rgb * c.rgb / 128.0) + c.a; a = u_mode.z != 0 ? t.a : c.a; }\n"
    "    rgb = min(rgb, 255.0); a = min(a, 255.0);\n"
    "  }\n"
    /* fog (FGE): after texturing, before the alpha test; F is interpolated even when flat shaded */
    "  if (u_fog.x != 0) {\n"
    "    float f = clamp(floor(v_f + 0.0001), 0.0, 255.0);\n"
    "    rgb = floor((f * rgb + (255.0 - f) * vec3(u_fog.yzw)) / 256.0);\n"
    "  }\n"
    /* alpha test: 0 never 1 always 2 less 3 lequal 4 equal 5 gequal 6 greater 7 notequal; 8 off */
    "  bool pass = true; float ar = float(u_atst.y);\n"
    "  switch (u_atst.x) {\n"
    "    case 0: pass = false; break; case 2: pass = a < ar; break; case 3: pass = a <= ar; break;\n"
    "    case 4: pass = a == ar; break; case 5: pass = a >= ar; break; case 6: pass = a > ar; break;\n"
    "    case 7: pass = a != ar; break; default: pass = true; break;\n"
    "  }\n"
    "  if (u_atst.z == 0 && !pass) discard;\n"         /* KEEP, or first pass of a split test */
    "  if (u_atst.z == 1 && pass) discard;\n"          /* second pass: only the failing pixels */
    "  vec4 d = vec4(0.0);\n"
    "  if (u_blend.x != 0 || u_atst.w != 0) d = floor(texelFetch(u_dst, ivec2(gl_FragCoord.xy - u_dstoff), 0) * 255.0 + 0.5);\n"
    "  if (u_atst.w == 1 && d.a >= 128.0) discard;\n"  /* destination alpha test */
    "  if (u_atst.w == 2 && d.a < 128.0) discard;\n"
    "  float asrc = a;\n"
    "  if (u_blend.x != 0 && !(u_misc.y != 0 && asrc < 128.0)) {\n"   /* PABE: As < 0x80 is written unblended */
    "    int ab = u_blend.y; int A = ab / 16, B = (ab / 4) & 3, D = ab & 3;\n"
    "    vec3 cA = A == 0 ? rgb : A == 1 ? d.rgb : vec3(0.0);\n"
    "    vec3 cB = B == 0 ? rgb : B == 1 ? d.rgb : vec3(0.0);\n"
    "    vec3 cD = D == 0 ? rgb : D == 1 ? d.rgb : vec3(0.0);\n"
    "    float C = u_blend.z == 0 ? asrc : u_blend.z == 1 ? d.a : u_fix;\n"
    "    rgb = floor((cA - cB) * C / 128.0) + cD;\n"
    "    rgb = u_misc.w != 0 ? clamp(rgb, 0.0, 255.0) : mod(rgb, 256.0);\n"
    "  }\n"
    "  float aout = (u_misc.x != 0 && a < 128.0) ? a + 128.0 : a;\n"
    "  o_col = vec4(rgb / 255.0, aout / 255.0);\n"
    "  if (u_dbg == 1) o_col = vec4(texture(u_tex, v_stq.xy / v_stq.z).rgb, 1.0);\n"
    "  if (u_dbg == 2) o_col = vec4(c.rgb / 255.0, 1.0);\n"
    "  if (u_dbg == 3) o_col = vec4(fract(v_stq.xy / v_stq.z), 0.0, 1.0);\n"
    "  if (u_dbg == 4) o_col = vec4(texelFetch(u_tex, ivec2(gl_FragCoord.xy) & 63, 0).rgb, 1.0);\n"
    "  if (u_dbg == 5) o_col = vec4(vec3(textureSize(u_tex, 0).x) / 256.0, 1.0);\n"
    "  float fac = u_misc.z == 1 ? u_fix / 128.0 : asrc / 128.0;\n"
    "  o_fac = vec4(fac);\n"
    "  if (u_misc.z == 2) o_col.rgb = vec3(min(asrc / 128.0, 1.0));\n"   /* Cd*(1+As): blended as src*Cd + Cd */
    "  if (u_misc.z == 3) o_col.rgb = min(floor(rgb * asrc / 128.0), 255.0) / 255.0;\n"   /* Cs*As, with As up to 2.0 */
    "  gl_FragDepth = v_z;\n"
    "}\n";
/* copy / scale a texture rectangle (uploads into targets, display, downscaled readback) */
static const char *HW_VS_COPY =
    "#version 330 core\nin vec4 a0; uniform vec4 u_rect; out vec2 uv;\n"
    "void main() { uv = a0.zw; gl_Position = vec4(a0.xy, 0.0, 1.0); }\n";
static const char *HW_FS_COPY =
    "#version 330 core\nin vec2 uv; uniform sampler2D u_tex; out vec4 o;\n"
    "void main() { o = texture(u_tex, uv); }\n";

/* ------------------------------------------------------------------ helpers */
static void hw_tex_params(GLuint t, int filter, int wrap_s, int wrap_t) {
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap_s);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap_t);
}
static GLuint hw_new_tex(int w, int h, GLenum ifmt, GLenum fmt, GLenum type) {
    GLuint t; glGenTextures(1, &t); glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, (GLint)ifmt, w, h, 0, fmt, type, NULL);
    hw_tex_params(t, GL_NEAREST, GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE);
    return t;
}
/* draw a textured quad: dst rect in NDC of the bound framebuffer, src in normalised uv */
static void hw_quad(GLuint src, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, int linear) {
    float q[16] = {x0, y0, u0, v0, x1, y0, u1, v0, x0, y1, u0, v1, x1, y1, u1, v1};
    glUseProgram(hw_prog_copy); glUniform1i(uc_tex, 0);
    glActiveTexture(GL_TEXTURE0); hw_tex_params(src, linear ? GL_LINEAR : GL_NEAREST, GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE);
    glBindBuffer(GL_ARRAY_BUFFER, hw_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof q, q, GL_STREAM_DRAW);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 16, (void *)0);
    glDisableVertexAttribArray(1); glDisableVertexAttribArray(2);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    hw_have_cur = 0;   /* GL state changed behind the batch cache */
}
static void hw_plain_state(void) {
    glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_SCISSOR_TEST); glDepthMask(GL_FALSE);
    glColorMask(1, 1, 1, 1);
}

/* ------------------------------------------------------------------ init */
static int gpu_init(void) {
    if (hw_inited) return !hw_failed;
    hw_inited = 1;
    void *ctx = hostgl_gs_context();
    if (!ctx || !hostgl_window()) { WARN("[gpu] no OpenGL context for the GS thread; using the software renderer\n"); g_hw_fail = "no shared OpenGL context"; hw_failed = 1; return 0; }
    /* the GS context's own hidden surface first; the main window as a fallback (fine on X11/GLX) */
    int mc = SDL_GL_MakeCurrent(hostgl_gs_window(), (SDL_GLContext)ctx);
    if (mc != 0) { WARN("[gpu] GS context on its own surface: %s; trying the main window\n", SDL_GetError());
                   mc = SDL_GL_MakeCurrent(hostgl_window(), (SDL_GLContext)ctx); }
    if (mc != 0) { { static char why[160]; snprintf(why, sizeof why, "GL context error: %s", SDL_GetError()); g_hw_fail = why; }
        WARN("[gpu] cannot use the GS context: %s\n", SDL_GetError()); hw_failed = 1; return 0; }
    hw_prog = gl_compile(HW_VS, HW_FS, "gs");
    hw_prog_copy = gl_compile(HW_VS_COPY, HW_FS_COPY, "gs-copy");
    if (!hw_prog || !hw_prog_copy) { WARN("[gpu] shader setup failed; using the software renderer\n"); g_hw_fail = "shader compile failed (see terminal)"; hw_failed = 1; return 0; }
    u_tex = glGetUniformLocation(hw_prog, "u_tex"); u_dst = glGetUniformLocation(hw_prog, "u_dst");
    u_vp = glGetUniformLocation(hw_prog, "u_vp"); u_mode = glGetUniformLocation(hw_prog, "u_mode");
    u_atst = glGetUniformLocation(hw_prog, "u_atst"); u_blend = glGetUniformLocation(hw_prog, "u_blend");
    u_misc = glGetUniformLocation(hw_prog, "u_misc"); u_dstoff = glGetUniformLocation(hw_prog, "u_dstoff");
    u_fix = glGetUniformLocation(hw_prog, "u_fix"); u_fog = glGetUniformLocation(hw_prog, "u_fog");
    uc_tex = glGetUniformLocation(hw_prog_copy, "u_tex"); uc_rect = glGetUniformLocation(hw_prog_copy, "u_rect");
    glGenVertexArrays(1, &hw_vao); glBindVertexArray(hw_vao);
    glGenBuffers(1, &hw_vbo);
    hw_barrier = glTextureBarrier != NULL && (gl_has_ext("GL_ARB_texture_barrier") || gl_has_ext("GL_NV_texture_barrier"));
    GLint maxtex = 0; glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxtex);
    hw_scale = g_hw_scale < 1 ? 1 : g_hw_scale > 8 ? 8 : g_hw_scale;
    while (hw_scale > 1 && 1024 * hw_scale > maxtex) hw_scale--;
    INFO("[gpu] GPU renderer on %s, %dx internal resolution%s\n", glGetString(GL_RENDERER), hw_scale, hw_barrier ? ", texture barriers" : "");
    hw_on = 1;
    return 1;
}

/* ------------------------------------------------------------------ VRAM <-> GPU */
/* native RGBA of a VRAM rectangle in a colour format */
static void hw_vram_read_rgba(uint32_t bp, uint32_t bw, int psm, int x0, int y0, int w, int h, uint32_t *out) {
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        uint32_t c = vread(psm, bp, bw, (x0 + x) & 2047, (y0 + y) & 2047);
        if (psm_is16(psm)) c = ((c & 0x1f) << 3) | (((c >> 5) & 0x1f) << 11) | (((c >> 10) & 0x1f) << 19) | ((c & 0x8000) ? 0x80000000u : 0);
        else if (psm == PSMCT24) c |= 0x80000000u;
        out[y * w + x] = c;
    }
}
/* push a native-resolution RGBA image into a scaled target rectangle */
static void hw_upload_rect(HwRT *rt, int x0, int y0, int w, int h, const uint32_t *px) {
    static GLuint up; static int upw, uph;
    if (!up || upw < w || uph < h) {
        if (up) glDeleteTextures(1, &up);
        upw = w > upw ? w : upw; uph = h > uph ? h : uph;
        up = hw_new_tex(upw, uph, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE);
    }
    glBindTexture(GL_TEXTURE_2D, up);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glBindFramebuffer(GL_FRAMEBUFFER, rt->fbo);
    glViewport(0, 0, rt->w * hw_scale, rt->h * hw_scale);
    hw_plain_state();
    float X0 = 2.0f * x0 / rt->w - 1, X1 = 2.0f * (x0 + w) / rt->w - 1, Y0 = 2.0f * y0 / rt->h - 1, Y1 = 2.0f * (y0 + h) / rt->h - 1;
    hw_quad(up, X0, Y0, X1, Y1, 0, 0, (float)w / upw, (float)h / uph, 0);
    hw_uploads++;
}
/* read the drawn part of a target back into VRAM (downscaled, point sampled) */
static void hw_download(HwRT *rt) {
    if (!rt->gpu_dirty) return;
    static GLuint dn, dnfbo; static int dnw, dnh;
    int w = rt->w, h = rt->h;
    if (!dn || dnw != w || dnh != h) {
        if (dn) { glDeleteTextures(1, &dn); glDeleteFramebuffers(1, &dnfbo); }
        dn = hw_new_tex(w, h, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE); dnw = w; dnh = h;
        glGenFramebuffers(1, &dnfbo); glBindFramebuffer(GL_FRAMEBUFFER, dnfbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dn, 0);
    }
    int x0 = rt->dx0, y0 = rt->dy0, rw = rt->dx1 - rt->dx0 + 1, rh = rt->dy1 - rt->dy0 + 1;
    glBindFramebuffer(GL_FRAMEBUFFER, dnfbo);
    glViewport(0, 0, w, h);
    hw_plain_state();
    hw_quad(rt->tex, -1, -1, 1, 1, 0, 0, 1, 1, 0);
    uint32_t *px = malloc((size_t)rw * rh * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(x0, y0, rw, rh, GL_RGBA, GL_UNSIGNED_BYTE, px);
    int psm = rt->is16 ? PSMCT16 : PSMCT32;
    global_seq++;   /* before the writes: cached texture decodes older than this must be redone */
    for (int y = 0; y < rh; y++) for (int x = 0; x < rw; x++) {
        uint32_t c = px[y * rw + x];
        if (rt->is16) c = ((c >> 3) & 0x1f) | (((c >> 11) & 0x1f) << 5) | (((c >> 19) & 0x1f) << 10) | ((c >> 31) << 15);
        vwrite(psm, rt->fbp, rt->fbw, x0 + x, y0 + y, c);
    }
    free(px);
    rt->gpu_dirty = 0;
    global_seq++;
    hw_downloads++;
}

/* ------------------------------------------------------------------ targets */
static void hw_flush(void);
static void hw_rt_attach_z(HwRT *rt, HwZ *z) {
    GLuint zt = z ? z->tex : 0;
    if (rt->ztex == zt) return;
    glBindFramebuffer(GL_FRAMEBUFFER, rt->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, zt, 0);
    rt->ztex = zt; rt->zbp = z ? z->zbp : 0;
}
static HwRT *hw_rt_find(uint32_t fbp, uint32_t fbw, int is16) {
    for (int i = 0; i < HW_NRT; i++) if (hw_rt[i].used && hw_rt[i].fbp == fbp && hw_rt[i].fbw == fbw && hw_rt[i].is16 == is16) return &hw_rt[i];
    return NULL;
}
/* forget a target (its drawing goes back to VRAM first) */
static void hw_rt_drop(HwRT *o) {
    if (hw_have_cur && hw_cur.rt == o) { hw_flush(); hw_have_cur = 0; }
    hw_download(o);
    glDeleteTextures(1, &o->tex); glDeleteFramebuffers(1, &o->fbo);
    o->used = 0;
}
/* rows y0..y1 of a target take their content from VRAM; older targets that claim that
 * VRAM are retired first (the game reuses texture memory for short-lived targets, e.g.
 * the water reflections, at a different address every frame) */
static void hw_rt_claim_rows(HwRT *rt, int y0, int y1) {
    uint32_t b0, b1; hw_rt_rows_blocks(rt, y0, y1, &b0, &b1);
    for (int i = 0; i < HW_NRT; i++) if (hw_rt[i].used && &hw_rt[i] != rt && hw_rt_overlaps(&hw_rt[i], b0, b1 - b0)) hw_rt_drop(&hw_rt[i]);
    int h = y1 - y0 + 1;
    uint32_t *px = malloc((size_t)rt->w * h * 4);
    hw_vram_read_rgba(rt->fbp, rt->fbw, rt->is16 ? PSMCT16 : PSMCT32, 0, y0, rt->w, h, px);
    hw_upload_rect(rt, 0, y0, rt->w, h, px);
    free(px);
}
static HwRT *hw_rt_get(uint32_t fbp, uint32_t fbw, int is16, int need_h) {
    int ph = is16 ? 64 : 32;
    if (need_h > 1024) need_h = 1024;
    need_h = (need_h + ph - 1) / ph * ph; if (need_h < ph) need_h = ph;
    HwRT *rt = hw_rt_find(fbp, fbw, is16);
    if (rt && rt->h >= need_h) {
        rt->lru = ++hw_tick;
        if (need_h > rt->vh) {   /* drawing reaches further down: claim those rows */
            hw_flush();
            int y0 = rt->vh; rt->vh = need_h;
            hw_rt_claim_rows(rt, y0, need_h - 1);
        }
        return rt;
    }
    hw_flush();
    int w = (int)(fbw ? fbw : 1) * 64, h = need_h <= 512 ? 512 : 1024;   /* VRAM holds at most 1024 rows of a 512-wide target */
    if (rt) { int keep = rt->vh; hw_rt_drop(rt); if (keep > need_h) need_h = keep; }   /* growing: the content goes through VRAM */
    else {
        rt = NULL;
        for (int i = 0; i < HW_NRT && !rt; i++) if (!hw_rt[i].used) rt = &hw_rt[i];
        if (!rt) {   /* evict the least recently used */
            HwRT *v = &hw_rt[0];
            for (int i = 1; i < HW_NRT; i++) if (hw_rt[i].lru < v->lru) v = &hw_rt[i];
            hw_rt_drop(v);
        }
    }
    for (int i = 0; i < HW_NRT && (!rt || rt->used); i++) if (!hw_rt[i].used) rt = &hw_rt[i];
    memset(rt, 0, sizeof *rt);
    rt->used = 1; rt->fbp = fbp; rt->fbw = fbw; rt->is16 = is16; rt->w = w; rt->h = h; rt->lru = ++hw_tick;
    rt->vh = need_h;
    rt->tex = hw_new_tex(w * hw_scale, h * hw_scale, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE);
    glGenFramebuffers(1, &rt->fbo); glBindFramebuffer(GL_FRAMEBUFFER, rt->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt->tex, 0);
    hw_rt_claim_rows(rt, 0, need_h - 1);
    return rt;
}
static HwZ *hw_z_get(uint32_t zbp, int w, int h) {
    for (int i = 0; i < HW_NRT; i++) if (hw_z[i].used && hw_z[i].zbp == zbp) {
        HwZ *z = &hw_z[i];
        if (z->w == w && z->h == h) { z->lru = ++hw_tick; return z; }
        /* size change: recreate (depth content is not carried over) */
        for (int k = 0; k < HW_NRT; k++) if (hw_rt[k].used && hw_rt[k].ztex == z->tex) hw_rt_attach_z(&hw_rt[k], NULL);
        glDeleteTextures(1, &z->tex); z->used = 0;
        break;
    }
    HwZ *z = NULL;
    for (int i = 0; i < HW_NRT && !z; i++) if (!hw_z[i].used) z = &hw_z[i];
    if (!z) { z = &hw_z[0]; for (int i = 1; i < HW_NRT; i++) if (hw_z[i].lru < z->lru) z = &hw_z[i];
              for (int k = 0; k < HW_NRT; k++) if (hw_rt[k].used && hw_rt[k].ztex == z->tex) hw_rt_attach_z(&hw_rt[k], NULL);
              glDeleteTextures(1, &z->tex); }
    memset(z, 0, sizeof *z);
    z->used = 1; z->zbp = zbp; z->w = w; z->h = h; z->lru = ++hw_tick;
    z->tex = hw_new_tex(w * hw_scale, h * hw_scale, GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT, GL_FLOAT);
    return z;
}
/* does [bp, bp + span) overlap a target with GPU-only content? read those back */
static const char *hw_sync_why = "";
static void hw_sync_vram_range(uint32_t bp, uint32_t nblocks) {
    for (int i = 0; i < HW_NRT; i++) if (hw_rt[i].used && hw_rt_dirty_overlaps(&hw_rt[i], bp, nblocks)) {
        { static int chk = -1, nl; if (chk < 0) chk = getenv("AE2_HWCHECK") != NULL;
          if (chk && nl++ < 30) INFO("[gpu] read-back of target %x (dirty rows %d-%d) for %s at %x+%x\n", hw_rt[i].fbp, hw_rt[i].dy0, hw_rt[i].dy1, hw_sync_why, bp, nblocks); }
        hw_flush(); hw_download(&hw_rt[i]);
    }
}
static int hw_clut_src_dirty(uint32_t cbp) {
    for (int i = 0; i < HW_NRT; i++) if (hw_rt[i].used && hw_rt_dirty_overlaps(&hw_rt[i], cbp, 8)) return 1;
    return 0;
}
static uint32_t hw_tex_blocks(const TexState *t) {
    int bpp = t->psm == PSMT4 ? 4 : t->psm == PSMT8 ? 8 : psm_is16(t->psm) ? 16 : 32;
    int w = 1 << t->tw, h = 1 << t->th;
    int tbw = t->tbw ? (int)t->tbw * 64 : w;
    uint32_t bytes = (uint32_t)tbw * (uint32_t)h * (uint32_t)bpp / 8;
    return bytes / 256 + 32;   /* blocks of 256 bytes, plus a page of slack for swizzling */
}
/* a texture that is a render target (same address, a colour format the target holds) */
static HwRT *hw_tex_rt(const TexState *t) {
    for (int i = 0; i < HW_NRT; i++) if (hw_rt[i].used) {
        HwRT *o = &hw_rt[i];
        if (o->fbp != t->tbp) continue;
        int is16 = psm_is16(t->psm);
        if ((t->psm == PSMCT32 || t->psm == PSMCT24 || is16) && is16 == o->is16 && (t->tbw == o->fbw || (1u << t->tw) <= o->fbw * 64u)) return o;
    }
    return NULL;
}
/* called from setup_draw before the CPU texture decode */
static int hw_texture_prepare(TexState *t) {
    if (hw_tex_rt(t)) return 1;                 /* sampled straight from the GPU */
    hw_sync_why = "texture";
    hw_sync_vram_range(t->tbp, hw_tex_blocks(t));
    return 0;
}

/* ------------------------------------------------------------------ batches */
static void hw_bind_dst(HwState *s, int x0, int y0, int x1, int y1) {
    /* the destination for shader blending / DATE: the target itself behind a texture
     * barrier, or a copy of the touched rectangle */
    HwRT *rt = s->rt;
    glActiveTexture(GL_TEXTURE1);
    if (hw_barrier) {
        glBindTexture(GL_TEXTURE_2D, rt->tex);
        glTextureBarrier();
        glUniform2f(u_dstoff, 0, 0);
    } else {
        int S = hw_scale;
        int w = (x1 - x0 + 1) * S, h = (y1 - y0 + 1) * S;
        if (w < 1 || h < 1) w = h = 1;
        if (!dst_copy_tex || dst_copy_w < w || dst_copy_h < h) {
            if (dst_copy_tex) { glDeleteTextures(1, &dst_copy_tex); glDeleteFramebuffers(1, &dst_copy_fbo); }
            dst_copy_w = w > dst_copy_w ? w : dst_copy_w; dst_copy_h = h > dst_copy_h ? h : dst_copy_h;
            dst_copy_tex = hw_new_tex(dst_copy_w, dst_copy_h, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE);
            glGenFramebuffers(1, &dst_copy_fbo); glBindFramebuffer(GL_FRAMEBUFFER, dst_copy_fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dst_copy_tex, 0);
        }
        glBindFramebuffer(GL_READ_FRAMEBUFFER, rt->fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dst_copy_fbo);
        glDisable(GL_SCISSOR_TEST);
        glBlitFramebuffer(x0 * S, y0 * S, x0 * S + w, y0 * S + h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, rt->fbo);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, dst_copy_tex);
        glUniform2f(u_dstoff, (float)(x0 * S), (float)(y0 * S));
    }
    glActiveTexture(GL_TEXTURE0);
    hw_dstreads++;
}
static int hw_bx0, hw_by0, hw_bx1, hw_by1;   /* bbox of the pending batch (native pixels) */
#define HW_MAXDB 256
static int hw_db[HW_MAXDB][4], hw_ndb;      /* bboxes of a pending batch that reads the destination */
static void hw_apply_and_draw(HwState *s) {
    HwRT *rt = s->rt;
    glBindFramebuffer(GL_FRAMEBUFFER, rt->fbo);
    hw_rt_attach_z(rt, s->zb);
    int S = hw_scale;
    glViewport(0, 0, rt->w * S, rt->h * S);
    glUseProgram(hw_prog);
    glUniform1i(u_tex, 0); glUniform1i(u_dst, 1);
    glUniform4f(u_vp, 2.0f / rt->w, 2.0f / rt->h, 1.0f / 4294967296.0f, 0);
    glUniform4i(u_mode, s->mode[0], s->mode[1], s->mode[2], s->mode[3]);
    glUniform4i(u_atst, s->atst[0], s->atst[1], s->atst[2], s->atst[3]);
    glUniform4i(u_blend, s->blend[0], s->blend[1], s->blend[2], s->blend[3]);
    glUniform4i(u_misc, s->misc[0], s->misc[1], s->misc[2], s->misc[3]);
    glUniform1f(u_fix, s->fix);
    glUniform4i(u_fog, s->fog[0], s->fog[1], s->fog[2], s->fog[3]);
    { static int dbg = -1; static GLint loc; if (dbg < 0) { const char *e = getenv("AE2_HWDBG"); dbg = e ? atoi(e) : 0; loc = glGetUniformLocation(hw_prog, "u_dbg"); } glUniform1i(loc, dbg); }
    glEnable(GL_SCISSOR_TEST);
    glScissor(s->scx0 * S, s->scy0 * S, (s->scx1 - s->scx0 + 1) * S, (s->scy1 - s->scy0 + 1) * S);
    if (s->depth_test) { glEnable(GL_DEPTH_TEST); glDepthFunc((GLenum)s->depth_func); } else glDisable(GL_DEPTH_TEST);
    glDepthMask(s->depth_write ? GL_TRUE : GL_FALSE);
    glColorMask(s->cmask[0], s->cmask[1], s->cmask[2], s->cmask[3]);
    if (s->bl_on) { glEnable(GL_BLEND); glBlendEquationSeparate((GLenum)s->bl_eq, GL_FUNC_ADD); glBlendFuncSeparate((GLenum)s->bl_sf, (GLenum)s->bl_df, GL_ONE, GL_ZERO); }
    else glDisable(GL_BLEND);
    glActiveTexture(GL_TEXTURE0);
    if (s->mode[0]) {
        glBindTexture(GL_TEXTURE_2D, s->tex);
        GLint f = s->bilinear ? GL_LINEAR : GL_NEAREST;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, s->wms == 0 ? GL_REPEAT : GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, s->wmt == 0 ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    }
    if (s->need_dst) hw_bind_dst(s, hw_bx0 < 0 ? 0 : hw_bx0, hw_by0 < 0 ? 0 : hw_by0, hw_bx1 >= rt->w ? rt->w - 1 : hw_bx1, hw_by1 >= rt->h ? rt->h - 1 : hw_by1);
    glBindBuffer(GL_ARRAY_BUFFER, hw_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)hw_nv * sizeof(HwV), hw_v, GL_STREAM_DRAW);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(HwV), (void *)0);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(HwV), (void *)16);
    glEnableVertexAttribArray(2); glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(HwV), (void *)32);
    glDrawArrays(GL_TRIANGLES, 0, hw_nv);
    { static int chk = -1; if (chk < 0) chk = getenv("AE2_HWCHECK") != NULL;
      if (chk) { static int nl; GLenum e = glGetError(); GLenum fs = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if ((e || fs != GL_FRAMEBUFFER_COMPLETE) && nl++ < 20) WARN("[gpu] batch: GL error %x, framebuffer %x (rt %x z %x, %d verts)\n", e, fs, rt->fbp, s->zb ? s->zb->zbp : 0, hw_nv);
        static int nb; static long lf = -2; if (lf == -2) lf = getenv("AE2_HWLOGFRAME") ? atol(getenv("AE2_HWLOGFRAME")) : -1;
        if (nb++ < 12 || g_frame == (uint32_t)lf) INFO("[gpu] batch %d: rt %x %dx%d z %s tex %u(rt%d) mode %d%d%d%d atst %d/%d/%d/%d blend on%d eq%x sf%x df%x shader%d depth t%d f%x w%d mask %d%d%d%d sc %d,%d-%d,%d v0 %.1f,%.1f z%.6g\n", nb, rt->fbp, rt->w, rt->h,
            s->zb ? "yes" : "no", s->tex, s->tex_rt, s->mode[0], s->mode[1], s->mode[2], s->mode[3], s->atst[0], s->atst[1], s->atst[2], s->atst[3], s->bl_on, s->bl_eq, s->bl_sf, s->bl_df, s->blend[0],
            s->depth_test, s->depth_func, s->depth_write, s->cmask[0], s->cmask[1], s->cmask[2], s->cmask[3], s->scx0, s->scy0, s->scx1, s->scy1, hw_v[0].x, hw_v[0].y, hw_v[0].z); } }
    if (s->pass2) {   /* AFAIL: draw the pixels that failed the alpha test with the reduced write mask */
        glUniform4i(u_atst, s->atst[0], s->atst[1], 1, s->atst[3]);
        int af = s->pass2;   /* 1 FB_ONLY, 2 ZB_ONLY, 3 RGB_ONLY */
        glColorMask(af != 2 && s->cmask[0], af != 2 && s->cmask[1], af != 2 && s->cmask[2], af == 1 && s->cmask[3]);
        glDepthMask(af == 2 && s->depth_write ? GL_TRUE : GL_FALSE);
        glDrawArrays(GL_TRIANGLES, 0, hw_nv);
    }
    {   int x0 = hw_bx0 > s->scx0 ? hw_bx0 : s->scx0, y0 = hw_by0 > s->scy0 ? hw_by0 : s->scy0;
        int x1 = hw_bx1 < s->scx1 ? hw_bx1 : s->scx1, y1 = hw_by1 < s->scy1 ? hw_by1 : s->scy1;
        if (s->cmask[0] || s->cmask[1] || s->cmask[2] || s->cmask[3]) hw_rt_mark(rt, x0, y0, x1, y1); }
    hw_batches++;
}
static void hw_flush(void) {
    hw_ndb = 0;
    if (!hw_nv) return;
    hw_apply_and_draw(&hw_cur);
    hw_nv = 0;
    hw_bx0 = hw_by0 = 1 << 20; hw_bx1 = hw_by1 = -(1 << 20);
}

/* GS state -> batch state */
static void hw_state_from(const DrawState *d, HwState *s, HwRT *rt, HwZ *zb, GLuint tex, int tex_rt, int tw, int th) {
    memset(s, 0, sizeof *s);
    s->rt = rt; s->zb = zb;
    s->scx0 = d->scx0; s->scy0 = d->scy0; s->scx1 = d->scx1; s->scy1 = d->scy1;
    if (s->scx1 >= rt->w) s->scx1 = rt->w - 1; if (s->scy1 >= rt->h) s->scy1 = rt->h - 1;
    s->mode[0] = d->tme; s->mode[1] = d->tme ? (int)d->tex.tfx : 0; s->mode[2] = d->tme ? (int)d->tex.tcc : 0; s->mode[3] = d->iip;
    if (d->tme) { s->tex = tex; s->tex_rt = tex_rt; s->tw = tw; s->th = th; s->bilinear = d->tex.bilinear; s->wms = d->tex.wms & 1; s->wmt = d->tex.wmt & 1; }
    /* depth */
    s->depth_test = zb && d->zte && d->ztst >= 2;
    s->depth_func = d->ztst == 3 ? GL_GREATER : GL_GEQUAL;
    s->depth_write = zb && d->zwrite;
    if (zb && d->zte && d->ztst == 1) { s->depth_test = 1; s->depth_func = GL_ALWAYS; }
    /* colour write mask (byte granularity of FBMSK) */
    uint32_t m = d->fbmsk;
    if (rt->is16) { s->cmask[0] = !(m & 0xf8); s->cmask[1] = !(m & 0xf800); s->cmask[2] = !(m & 0xf80000); s->cmask[3] = !(m & 0x80000000u); }
    else { s->cmask[0] = (m & 0xff) != 0xff; s->cmask[1] = (m & 0xff00) != 0xff00; s->cmask[2] = (m & 0xff0000) != 0xff0000; s->cmask[3] = (m & 0xff000000u) != 0xff000000u; }
    if (d->fpsm == PSMCT24) s->cmask[3] = 0;
    /* alpha test */
    s->atst[0] = d->ate ? d->atst : 8; s->atst[1] = d->aref; s->atst[2] = 0;
    if (d->ate && d->afail != 0) {
        if (d->atst == 0) {   /* NEVER + AFAIL = a pure write mask */
            s->atst[0] = 8;
            if (d->afail == 1) s->depth_write = 0;
            else if (d->afail == 2) { s->cmask[0] = s->cmask[1] = s->cmask[2] = s->cmask[3] = 0; }
            else { s->cmask[3] = 0; s->depth_write = 0; }
        } else if (d->atst != 1) { s->atst[2] = 2; s->pass2 = d->afail; }   /* first pass: passing pixels, full write */
        else s->atst[0] = 8;
    }
    if (s->atst[2] == 2) s->atst[2] = 0;
    s->atst[3] = d->date ? 1 + d->datm : 0;
    /* blending: (A - B) * C / 128 + D */
    s->misc[0] = d->fba; s->misc[3] = d->colclamp;
    if (d->fge) { s->fog[0] = 1; s->fog[1] = d->fogcol & 255; s->fog[2] = (d->fogcol >> 8) & 255; s->fog[3] = (d->fogcol >> 16) & 255; }
    s->fix = (float)d->fix;
    int A = d->a_a, B = d->a_b, C = d->a_c, D = d->a_d;
    int shader = 0;
    if (d->abe) {
        if (A == B) {            /* result is D */
            if (D == 0) { /* Cs: no blending */ }
            else if (D == 1) { s->bl_on = 1; s->bl_eq = GL_FUNC_ADD; s->bl_sf = GL_ZERO; s->bl_df = GL_ONE; }
            else { s->bl_on = 1; s->bl_eq = GL_FUNC_ADD; s->bl_sf = GL_ZERO; s->bl_df = GL_ZERO; }
        } else if (C == 1 || (C == 2 && d->fix > 128)) shader = 1;      /* destination alpha / factor above 1 */
        else {
            int fsrc = C == 2 ? 1 : 0;   /* factor: As or FIX (both / 128, at most 1 here) */
            s->misc[2] = fsrc;
            GLenum F = GL_SRC1_ALPHA, IF = GL_ONE_MINUS_SRC1_ALPHA;
            s->bl_on = 1;
            if (A == 0 && B == 1 && D == 1) { s->bl_eq = GL_FUNC_ADD; s->bl_sf = F; s->bl_df = IF; }            /* Cs*C + Cd*(1-C) */
            else if (A == 0 && B == 1 && D == 2) { s->bl_eq = GL_FUNC_SUBTRACT; s->bl_sf = F; s->bl_df = F; }   /* (Cs-Cd)*C */
            /* the factor applies to Cs only: As is multiplied in by the shader, where it may exceed 1.0 (a blend factor can't) */
            else if (A == 0 && B == 2 && D == 1) { s->bl_eq = GL_FUNC_ADD; s->bl_sf = fsrc ? F : GL_ONE; s->bl_df = GL_ONE; if (!fsrc) s->misc[2] = 3; }   /* Cs*C + Cd */
            else if (A == 0 && B == 2 && D == 2) { s->bl_eq = GL_FUNC_ADD; s->bl_sf = fsrc ? F : GL_ONE; s->bl_df = GL_ZERO; if (!fsrc) s->misc[2] = 3; }  /* Cs*C */
            else if (A == 1 && B == 0 && D == 0) { s->bl_eq = GL_FUNC_ADD; s->bl_sf = IF; s->bl_df = F; }       /* Cs*(1-C) + Cd*C */
            else if (A == 1 && B == 0 && D == 2) { s->bl_eq = GL_FUNC_REVERSE_SUBTRACT; s->bl_sf = F; s->bl_df = F; } /* (Cd-Cs)*C */
            else if (A == 2 && B == 0 && D == 1) { s->bl_eq = GL_FUNC_REVERSE_SUBTRACT; s->bl_sf = fsrc ? F : GL_ONE; s->bl_df = GL_ONE; if (!fsrc) s->misc[2] = 3; } /* Cd - Cs*C */
            else if (A == 2 && B == 1 && D == 0) { s->bl_eq = GL_FUNC_SUBTRACT; s->bl_sf = GL_ONE; s->bl_df = F; }  /* Cs - Cd*C */
            else if (A == 1 && B == 2 && D == 0) { s->bl_eq = GL_FUNC_ADD; s->bl_sf = GL_ONE; s->bl_df = F; }       /* Cs + Cd*C */
            else if (A == 1 && B == 2 && D == 2) { s->bl_eq = GL_FUNC_ADD; s->bl_sf = GL_ZERO; s->bl_df = F; }      /* Cd*C */
            else if (A == 2 && B == 1 && D == 1) { s->bl_eq = GL_FUNC_ADD; s->bl_sf = GL_ZERO; s->bl_df = IF; }     /* Cd*(1-C) */
            else if (A == 1 && B == 2 && D == 1 && C == 0) { s->bl_eq = GL_FUNC_ADD; s->bl_sf = GL_DST_COLOR; s->bl_df = GL_ONE; s->misc[2] = 2; } /* Cd*(1+As) */
            else if (A == 2 && B == 0 && D == 2) { s->bl_eq = GL_FUNC_ADD; s->bl_sf = GL_ZERO; s->bl_df = GL_ZERO; } /* -Cs*C clamps to 0 */
            else { s->bl_on = 0; shader = 1; }
        }
    }
    if (d->abe && d->pabe && !(A == B && D == 0)) shader = 1;   /* per-pixel blend enable: pixels with As < 0x80 are written unblended */
    if (shader) { s->bl_on = 0; s->blend[0] = 1; s->blend[1] = A * 16 + B * 4 + D; s->blend[2] = C; s->misc[1] = d->pabe; s->misc[2] = 0; }
    s->need_dst = shader || d->date;
    s->prim = GL_TRIANGLES;
}
static int hw_state_eq(const HwState *a, const HwState *b) { return !memcmp(a, b, sizeof *a); }

/* ------------------------------------------------------------------ primitives */
/* texture coordinate scale for the batch: FST texels -> normalised, and for a render
 * target sampled as a texture, texture-normalised -> target-normalised */
static float hw_fu, hw_fv, hw_su, hw_sv;
static inline void hw_vert(const Vtx *v, const Vtx *colsrc, const DrawState *d, int tw, int th, float zf) {
    (void)tw; (void)th;
    HwV *o = &hw_v[hw_nv++];
    o->x = v->x / 16.0f; o->y = v->y / 16.0f;
    o->z = zf; o->f = v->f;
    o->r = colsrc->r; o->g = colsrc->g; o->b = colsrc->b; o->a = colsrc->a;
    if (d->tme) {
        if (d->fst) { o->s = v->u * hw_fu; o->t = v->v * hw_fv; o->q = 1.0f; }
        else { o->s = v->s * hw_su; o->t = v->t * hw_sv; o->q = v->q; }
    } else { o->s = o->t = 0; o->q = 1; }
}
static void hw_prim(const DrawState *d, int type, const Vtx *a, const Vtx *b, const Vtx *c, int bx0, int by0, int bx1, int by1) {
    if (!d->tme && 0) return;
    /* target and depth */
    int is16 = psm_is16(d->fpsm);
    HwRT *rt = hw_rt_get(d->fbp, d->fbw, is16, by1 + 1);
    HwZ *zb = NULL;
    if (d->zte && (d->zread || d->zwrite || d->ztst == 1)) zb = hw_z_get(d->zbp, rt->w, rt->h);
    /* texture */
    GLuint tex = 0; int tex_rt = 0, tw = 1, th = 1;
    if (d->tme) {
        const TexState *t = &d->tex;
        tw = 1 << t->tw; th = 1 << t->th;
        hw_fu = 1.0f / (16.0f * tw); hw_fv = 1.0f / (16.0f * th); hw_su = hw_sv = 1.0f;
        HwRT *src = hw_tex_rt(t);
        if (src) {
            tex = src->tex; tex_rt = 1;
            hw_fu = 1.0f / (16.0f * src->w); hw_fv = 1.0f / (16.0f * src->h);
            hw_su = (float)tw / src->w; hw_sv = (float)th / src->h;
        } else if (t->cache) {
            int idx = tc_last_idx;
            if (idx < 0 || tcache[idx].data != t->cache) {
                for (idx = 0; idx < TC_N && tcache[idx].data != t->cache; idx++);
            }
            if (idx < TC_N) {
                TexCacheEnt *e = &tcache[idx];
                if (!hw_ctex[idx] || hw_cseq[idx] != e->seq || hw_cepoch[idx] != e->epoch || hw_cw[idx] != tw || hw_ch[idx] != th) {
                    if (!hw_ctex[idx]) glGenTextures(1, &hw_ctex[idx]);
                    glBindTexture(GL_TEXTURE_2D, hw_ctex[idx]);
                    if (hw_cw[idx] != tw || hw_ch[idx] != th) glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
                    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, tw, th, GL_RGBA, GL_UNSIGNED_BYTE, e->data);
                    hw_cseq[idx] = e->seq; hw_cepoch[idx] = e->epoch; hw_cw[idx] = tw; hw_ch[idx] = th;
                }
                tex = hw_ctex[idx];
            }
        }
    }
    HwState s;
    hw_state_from(d, &s, rt, zb, tex, tex_rt, tw, th);
    if (d->tme && !tex) s.mode[0] = 0;
    /* RT sampled from itself (feedback): copy semantics via the destination path */
    if (s.tex_rt && tex == rt->tex) {
        /* rare in this game; draw from a snapshot of the target */
        static GLuint snap, snapfbo; static int sw_, sh_;
        int W = rt->w * hw_scale, H = rt->h * hw_scale;
        hw_flush();
        if (!snap || sw_ != W || sh_ != H) {
            if (snap) { glDeleteTextures(1, &snap); glDeleteFramebuffers(1, &snapfbo); }
            snap = hw_new_tex(W, H, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE); sw_ = W; sh_ = H;
            glGenFramebuffers(1, &snapfbo); glBindFramebuffer(GL_FRAMEBUFFER, snapfbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, snap, 0);
        }
        glBindFramebuffer(GL_READ_FRAMEBUFFER, rt->fbo); glBindFramebuffer(GL_DRAW_FRAMEBUFFER, snapfbo);
        glDisable(GL_SCISSOR_TEST);
        glBlitFramebuffer(0, 0, W, H, 0, 0, W, H, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        s.tex = snap;
    }
    /* primitives that read the destination can share a draw only if they don't overlap */
    int dst_conflict = 0;
    if (s.need_dst && hw_nv) {
        if (hw_ndb >= HW_MAXDB) dst_conflict = 1;
        else for (int i = 0; i < hw_ndb; i++)
            if (bx0 <= hw_db[i][2] && hw_db[i][0] <= bx1 && by0 <= hw_db[i][3] && hw_db[i][1] <= by1) { dst_conflict = 1; break; }
    }
    if (!hw_have_cur || !hw_state_eq(&s, &hw_cur) || hw_nv + 6 > HW_MAXV || dst_conflict) {
        hw_flush();
        hw_cur = s; hw_have_cur = 1;
    }
    if (s.need_dst) { hw_db[hw_ndb][0] = bx0; hw_db[hw_ndb][1] = by0; hw_db[hw_ndb][2] = bx1; hw_db[hw_ndb][3] = by1; hw_ndb++; }
    if (bx0 < hw_bx0) hw_bx0 = bx0; if (by0 < hw_by0) hw_by0 = by0;
    if (bx1 > hw_bx1) hw_bx1 = bx1; if (by1 > hw_by1) hw_by1 = by1;
    /* normalise coordinates for a target sampled as a texture: s,t over the texture size */
    if (type == 3) {
        const Vtx *cs = d->iip ? NULL : c;
        hw_vert(a, cs ? cs : a, d, tw, th, (float)(a->z > d->zmax ? d->zmax : a->z));
        hw_vert(b, cs ? cs : b, d, tw, th, (float)(b->z > d->zmax ? d->zmax : b->z));
        hw_vert(c, c, d, tw, th, (float)(c->z > d->zmax ? d->zmax : c->z));
    } else if (type == 6) {
        /* sprite: corners from the two vertices, colour/Z/Q of the second */
        Vtx v[4]; v[0] = *a; v[3] = *b;
        v[1] = *b; v[1].y = a->y; v[1].v = a->v; v[1].t = a->t;
        v[2] = *a; v[2].y = b->y; v[2].v = b->v; v[2].t = b->t;
        v[2].x = a->x; v[2].u = a->u; v[2].s = a->s;
        v[1].x = b->x; v[1].u = b->u; v[1].s = b->s;
        for (int i = 0; i < 4; i++) { v[i].q = b->q; v[i].f = b->f; }   /* fog too, like the colour */
        float zf = (float)(b->z > d->zmax ? d->zmax : b->z);
        static const int idx[6] = {0, 1, 2, 1, 3, 2};
        for (int i = 0; i < 6; i++) hw_vert(&v[idx[i]], b, d, tw, th, zf);
    } else if (type == 1) {
        /* line: a thin quad one native pixel wide */
        float dx = (float)(b->x - a->x), dy = (float)(b->y - a->y);
        Vtx v[4]; v[0] = *a; v[1] = *a; v[2] = *b; v[3] = *b;
        v[0].f = v[1].f = b->f;   /* fog of the second vertex, as the software renderer does */
        int horiz = fabsf(dx) >= fabsf(dy);
        if (horiz) { v[1].y += 16; v[3].y += 16; } else { v[1].x += 16; v[3].x += 16; }
        static const int idx[6] = {0, 2, 1, 1, 2, 3};
        for (int i = 0; i < 6; i++) hw_vert(&v[idx[i]], d->iip ? &v[idx[i]] : b, d, tw, th, (float)v[idx[i]].z);
    } else {
        Vtx v[4]; for (int i = 0; i < 4; i++) v[i] = *a;
        v[1].x += 16; v[2].y += 16; v[3].x += 16; v[3].y += 16;
        static const int idx[6] = {0, 1, 2, 1, 3, 2};
        for (int i = 0; i < 6; i++) hw_vert(&v[idx[i]], a, d, tw, th, (float)a->z);
    }
    hw_draws++;
}

/* ------------------------------------------------------------------ transfers */
/* VRAM written by a host->local or local->local transfer: refresh overlapping targets */
static void hw_vram_written(uint32_t bp, uint32_t bw, int psm, int x0, int y0, int w, int h) {
    int bpp = psm == PSMT4 ? 4 : psm == PSMT8 ? 8 : psm_is16(psm) ? 16 : 32;
    uint32_t nb = (uint32_t)((bw ? bw * 64 : (uint32_t)w) * (uint32_t)(y0 + h) * bpp / 8) / 256 + 32;
    for (int i = 0; i < HW_NRT; i++) if (hw_rt[i].used) {
        HwRT *o = &hw_rt[i];
        if (!hw_rt_overlaps(o, bp, nb)) continue;
        hw_flush();
        int same = bp == o->fbp && bw == o->fbw && (psm == PSMCT32 || psm == PSMCT24 || psm_is16(psm)) && psm_is16(psm) == o->is16;
        int rx0 = x0 < 0 ? 0 : x0, ry0 = y0 < 0 ? 0 : y0, rw = w, rh = h;
        if (rx0 + rw > o->w) rw = o->w - rx0;
        if (same && rw > 0 && rh > 0 && ry0 + rh <= o->vh) {   /* an image put straight into the target */
            uint32_t *px = malloc((size_t)rw * rh * 4);
            hw_vram_read_rgba(o->fbp, o->fbw, o->is16 ? PSMCT16 : PSMCT32, rx0, ry0, rw, rh, px);
            hw_upload_rect(o, rx0, ry0, rw, rh, px);
            free(px);
        } else hw_rt_drop(o);   /* memory reused for something else (its drawing was saved before the write) */
    }
}

/* ------------------------------------------------------------------ display */
int host_frame_dump_due(void);
void host_frame_dump(const uint32_t *px, int w, int h);
void host_present_hw(unsigned tex, float umax, float vmax, void *fence);
static void hw_vsync(int n, int w, int h, int blank) {
    hw_flush();
    uint64_t fb = gs.priv[n ? 9 : 7];
    uint32_t fbp = bits(fb, 0, 9) * 32, fbw = bits(fb, 9, 6), psm = bits(fb, 15, 5);
    uint32_t dbx = bits(fb, 32, 11), dby = bits(fb, 43, 11);
    int S = hw_scale;
    int k = disp_cur = (disp_cur + 1) % 3;
    if (disp_fence[k]) { glClientWaitSync(disp_fence[k], GL_SYNC_FLUSH_COMMANDS_BIT, 100000000ull); glDeleteSync(disp_fence[k]); disp_fence[k] = NULL; }
    int W = w * S, H = h * S;
    if (!disp_tex[k] || disp_w[k] != W || disp_h[k] != H) {
        if (disp_tex[k]) { glDeleteTextures(1, &disp_tex[k]); glDeleteFramebuffers(1, &disp_fbo[k]); }
        disp_tex[k] = hw_new_tex(W, H, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE); disp_w[k] = W; disp_h[k] = H;
        glGenFramebuffers(1, &disp_fbo[k]); glBindFramebuffer(GL_FRAMEBUFFER, disp_fbo[k]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, disp_tex[k], 0);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, disp_fbo[k]);
    glViewport(0, 0, W, H);
    hw_plain_state();
    glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
    if (!blank) {
        HwRT *rt = hw_rt_find(fbp, fbw, psm_is16(psm));
        if (rt) {
            float u0 = (float)dbx / rt->w, v0 = (float)dby / rt->h, u1 = (float)(dbx + w) / rt->w, v1 = (float)(dby + h) / rt->h;
            hw_quad(rt->tex, -1, -1, 1, 1, u0, v0, u1, v1, 0);
        } else {
            /* not drawn on the GPU (e.g. uploaded straight into VRAM): show VRAM */
            uint32_t *px = malloc((size_t)w * h * 4);
            read_display(n, px, w, h);
            static GLuint vt; static int vw, vh;
            if (!vt || vw != w || vh != h) { if (vt) glDeleteTextures(1, &vt); vt = hw_new_tex(w, h, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE); vw = w; vh = h; }
            glBindTexture(GL_TEXTURE_2D, vt);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
            free(px);
            glBindFramebuffer(GL_FRAMEBUFFER, disp_fbo[k]);
            glViewport(0, 0, W, H);
            hw_quad(vt, -1, -1, 1, 1, 0, 0, 1, 1, 1);
        }
    }
    if (host_frame_dump_due()) {
        uint32_t *px = malloc((size_t)W * H * 4);
        glBindFramebuffer(GL_FRAMEBUFFER, disp_fbo[k]);
        glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
        for (int i = 0; i < W * H; i++) px[i] |= 0xff000000u;
        host_frame_dump(px, W, H);
        free(px);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    disp_fence[k] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();
    host_present_hw(disp_tex[k], 1, 1, disp_fence[k] ? (void *)disp_fence[k] : NULL);
    /* stats */
    static int nf; if (++nf % 250 == 0) {
        INFO("[gpu] last 250 frames: %llu prims in %llu batches, %llu destination reads, %llu read-backs, %llu target uploads\n",
             (unsigned long long)hw_draws, (unsigned long long)hw_batches, (unsigned long long)hw_dstreads, (unsigned long long)hw_downloads, (unsigned long long)hw_uploads);
        hw_draws = hw_batches = hw_dstreads = hw_downloads = hw_uploads = 0;
    }
}
/* everything back to VRAM (switching to the software renderer, save states) */
static void hw_sync_all(void) {
    if (!hw_on) return;
    hw_flush();
    for (int i = 0; i < HW_NRT; i++) if (hw_rt[i].used) hw_download(&hw_rt[i]);
}
static void hw_drop_targets(void) {
    for (int i = 0; i < HW_NRT; i++) {
        if (hw_rt[i].used) { glDeleteTextures(1, &hw_rt[i].tex); glDeleteFramebuffers(1, &hw_rt[i].fbo); }
        if (hw_z[i].used) glDeleteTextures(1, &hw_z[i].tex);
        hw_rt[i].used = 0; hw_z[i].used = 0;
    }
    hw_have_cur = 0;
}

/* first primitive (GS thread): bring the GPU renderer up if it was requested */
int hw_try_start(void) {
    if (g_hw_request != 1 || hw_failed) return 0;
    if (!gpu_init()) { g_hw_request = 0; return 0; }
    hw_on = 1; hw_drop_targets();
    /* the state ring was set up for the software path: rebuild the texture binding */
    state_dirty = 1;
    return 0;   /* this primitive's state predates the switch; draw it in software */
}
