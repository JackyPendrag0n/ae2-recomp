/* Windows (MinGW-w64): force-included before every source file (see the Makefile).
 * Fills the few POSIX gaps the port runs into; the rest comes from MinGW and winpthreads. */
#pragma once
#ifdef _WIN32
#include <io.h>
#include <direct.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>
#define mkdir(path, mode) _mkdir(path)
static inline int setenv(const char *name, const char *value, int overwrite) {
    if (!overwrite && getenv(name)) return 0;
    return _putenv_s(name, value);
}
/* files can exceed 2 GB only in theory here, but long is 32-bit on Windows */
#define fseek(f, off, wh) _fseeki64(f, off, wh)
#define ftell(f) _ftelli64(f)
#include <malloc.h>
#include <time.h>
/* the port's aligned_alloc blocks live for the whole run and are never freed */
#define aligned_alloc(align, size) _aligned_malloc(size, align)
static inline struct tm *localtime_r(const time_t *t, struct tm *out) { return localtime_s(out, t) ? 0 : out; }
#endif
