/* Minimal OpenGL 3.3 core loader (no external dependency): the functions this port
 * uses, resolved with SDL_GL_GetProcAddress. Define GL_IMPL in exactly one file. */
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef unsigned int GLenum, GLuint, GLbitfield;
typedef int GLint, GLsizei;
typedef unsigned char GLboolean, GLubyte;
typedef float GLfloat;
typedef char GLchar;
typedef ptrdiff_t GLsizeiptr, GLintptr;
typedef struct __GLsync *GLsync;
typedef uint64_t GLuint64;

#define GL_FALSE 0
#define GL_TRUE 1
#define GL_NO_ERROR 0
#define GL_POINTS 0x0000
#define GL_LINES 0x0001
#define GL_TRIANGLES 0x0004
#define GL_TRIANGLE_STRIP 0x0005
#define GL_ZERO 0
#define GL_ONE 1
#define GL_SRC_COLOR 0x0300
#define GL_SRC_ALPHA 0x0302
#define GL_DST_COLOR 0x0306
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_DST_ALPHA 0x0304
#define GL_ONE_MINUS_DST_ALPHA 0x0305
#define GL_CONSTANT_COLOR 0x8001
#define GL_CONSTANT_ALPHA 0x8003
#define GL_ONE_MINUS_CONSTANT_ALPHA 0x8004
#define GL_SRC1_ALPHA 0x8589
#define GL_ONE_MINUS_SRC1_ALPHA 0x88FB
#define GL_FUNC_ADD 0x8006
#define GL_FUNC_SUBTRACT 0x800A
#define GL_FUNC_REVERSE_SUBTRACT 0x800B
#define GL_NEVER 0x0200
#define GL_LESS 0x0201
#define GL_EQUAL 0x0202
#define GL_LEQUAL 0x0203
#define GL_GREATER 0x0204
#define GL_NOTEQUAL 0x0205
#define GL_GEQUAL 0x0206
#define GL_ALWAYS 0x0207
#define GL_DEPTH_TEST 0x0B71
#define GL_BLEND 0x0BE2
#define GL_SCISSOR_TEST 0x0C11
#define GL_CULL_FACE 0x0B44
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_PACK_ALIGNMENT 0x0D05
#define GL_UNPACK_ROW_LENGTH 0x0CF2
#define GL_TEXTURE_2D 0x0DE1
#define GL_UNSIGNED_BYTE 0x1401
#define GL_UNSIGNED_SHORT 0x1403
#define GL_INT 0x1404
#define GL_UNSIGNED_INT 0x1405
#define GL_FLOAT 0x1406
#define GL_RED 0x1903
#define GL_RGBA 0x1908
#define GL_RED_INTEGER 0x8D94
#define GL_RGBA_INTEGER 0x8D99
#define GL_R8 0x8229
#define GL_R32UI 0x8236
#define GL_RGBA8 0x8058
#define GL_RGBA8UI 0x8D7C
#define GL_DEPTH_COMPONENT 0x1902
#define GL_DEPTH_COMPONENT32F 0x8CAC
#define GL_NEAREST 0x2600
#define GL_LINEAR 0x2601
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_REPEAT 0x2901
#define GL_TEXTURE0 0x84C0
#define GL_TEXTURE1 0x84C1
#define GL_ARRAY_BUFFER 0x8892
#define GL_STREAM_DRAW 0x88E0
#define GL_DYNAMIC_DRAW 0x88E8
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_INFO_LOG_LENGTH 0x8B84
#define GL_FRAMEBUFFER 0x8D40
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_DEPTH_BUFFER_BIT 0x0100
#define GL_VERSION 0x1F02
#define GL_RENDERER 0x1F01
#define GL_EXTENSIONS 0x1F03
#define GL_NUM_EXTENSIONS 0x821D
#define GL_SYNC_GPU_COMMANDS_COMPLETE 0x9117
#define GL_TIMEOUT_IGNORED 0xFFFFFFFFFFFFFFFFull
#define GL_SYNC_FLUSH_COMMANDS_BIT 0x00000001
#define GL_MAX_TEXTURE_SIZE 0x0D33
#define GL_MAP_WRITE_BIT 0x0002
#define GL_MAP_INVALIDATE_BUFFER_BIT 0x0008
#define GL_MAP_UNSYNCHRONIZED_BIT 0x0020
#define GL_FRAMEBUFFER_SRGB 0x8DB9

#define GL_FUNCS(X) \
    X(void, glClear, (GLbitfield)) X(void, glClearColor, (GLfloat, GLfloat, GLfloat, GLfloat)) X(void, glClearDepth, (double)) \
    X(void, glViewport, (GLint, GLint, GLsizei, GLsizei)) X(void, glScissor, (GLint, GLint, GLsizei, GLsizei)) \
    X(void, glEnable, (GLenum)) X(void, glDisable, (GLenum)) X(GLenum, glGetError, (void)) \
    X(const GLubyte *, glGetString, (GLenum)) X(const GLubyte *, glGetStringi, (GLenum, GLuint)) X(void, glGetIntegerv, (GLenum, GLint *)) \
    X(void, glBlendFuncSeparate, (GLenum, GLenum, GLenum, GLenum)) X(void, glBlendEquationSeparate, (GLenum, GLenum)) \
    X(void, glBlendColor, (GLfloat, GLfloat, GLfloat, GLfloat)) X(void, glColorMask, (GLboolean, GLboolean, GLboolean, GLboolean)) \
    X(void, glDepthFunc, (GLenum)) X(void, glDepthMask, (GLboolean)) X(void, glPixelStorei, (GLenum, GLint)) \
    X(void, glGenTextures, (GLsizei, GLuint *)) X(void, glDeleteTextures, (GLsizei, const GLuint *)) X(void, glBindTexture, (GLenum, GLuint)) \
    X(void, glActiveTexture, (GLenum)) X(void, glTexParameteri, (GLenum, GLenum, GLint)) \
    X(void, glTexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *)) \
    X(void, glTexSubImage2D, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *)) \
    X(void, glReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *)) \
    X(void, glGenBuffers, (GLsizei, GLuint *)) X(void, glBindBuffer, (GLenum, GLuint)) X(void, glDeleteBuffers, (GLsizei, const GLuint *)) \
    X(void, glBufferData, (GLenum, GLsizeiptr, const void *, GLenum)) X(void, glBufferSubData, (GLenum, GLintptr, GLsizeiptr, const void *)) \
    X(void, glGenVertexArrays, (GLsizei, GLuint *)) X(void, glBindVertexArray, (GLuint)) \
    X(void, glEnableVertexAttribArray, (GLuint)) X(void, glDisableVertexAttribArray, (GLuint)) X(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *)) \
    X(void, glVertexAttribIPointer, (GLuint, GLint, GLenum, GLsizei, const void *)) \
    X(void, glDrawArrays, (GLenum, GLint, GLsizei)) \
    X(GLuint, glCreateShader, (GLenum)) X(void, glShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *)) \
    X(void, glCompileShader, (GLuint)) X(void, glGetShaderiv, (GLuint, GLenum, GLint *)) X(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) \
    X(GLuint, glCreateProgram, (void)) X(void, glAttachShader, (GLuint, GLuint)) X(void, glLinkProgram, (GLuint)) \
    X(void, glGetProgramiv, (GLuint, GLenum, GLint *)) X(void, glGetProgramInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) \
    X(void, glUseProgram, (GLuint)) X(void, glDeleteShader, (GLuint)) X(void, glBindAttribLocation, (GLuint, GLuint, const GLchar *)) \
    X(void, glBindFragDataLocationIndexed, (GLuint, GLuint, GLuint, const GLchar *)) \
    X(GLint, glGetUniformLocation, (GLuint, const GLchar *)) X(void, glUniform1i, (GLint, GLint)) X(void, glUniform1f, (GLint, GLfloat)) \
    X(void, glUniform2f, (GLint, GLfloat, GLfloat)) X(void, glUniform4f, (GLint, GLfloat, GLfloat, GLfloat, GLfloat)) \
    X(void, glUniform4i, (GLint, GLint, GLint, GLint, GLint)) X(void, glUniform1ui, (GLint, GLuint)) \
    X(void, glUniform4fv, (GLint, GLsizei, const GLfloat *)) X(void, glUniform4iv, (GLint, GLsizei, const GLint *)) \
    X(void, glGenFramebuffers, (GLsizei, GLuint *)) X(void, glDeleteFramebuffers, (GLsizei, const GLuint *)) X(void, glBindFramebuffer, (GLenum, GLuint)) \
    X(void, glFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint)) X(GLenum, glCheckFramebufferStatus, (GLenum)) \
    X(void, glBlitFramebuffer, (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum)) \
    X(GLsync, glFenceSync, (GLenum, GLbitfield)) X(void, glWaitSync, (GLsync, GLbitfield, GLuint64)) \
    X(GLenum, glClientWaitSync, (GLsync, GLbitfield, GLuint64)) X(void, glDeleteSync, (GLsync)) X(void, glFlush, (void)) X(void, glFinish, (void)) \
    X(void, glCopyImageSubData, (GLuint, GLenum, GLint, GLint, GLint, GLint, GLuint, GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei)) \
    X(void, glTextureBarrier, (void))

#ifdef GL_IMPL
#define GLX_DEF(ret, name, args) ret (*name) args;
#else
#define GLX_DEF(ret, name, args) extern ret (*name) args;
#endif
GL_FUNCS(GLX_DEF)
#undef GLX_DEF

/* returns 0 on success; optional functions (glCopyImageSubData, glTextureBarrier,
 * glBindFragDataLocationIndexed) may stay NULL */
int gl_load(void *(*getproc)(const char *));
int gl_has_ext(const char *name);
