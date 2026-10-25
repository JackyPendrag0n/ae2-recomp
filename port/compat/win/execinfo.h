/* Windows: no glibc backtrace(); AE2_BADTRACE prints nothing there. */
#pragma once
static inline int backtrace(void **bt, int n) { (void)bt; (void)n; return 0; }
static inline void backtrace_symbols_fd(void *const *bt, int n, int fd) { (void)bt; (void)n; (void)fd; }
