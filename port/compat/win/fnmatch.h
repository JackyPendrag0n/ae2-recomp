/* Windows: the shell-style matching the memory card directory listing needs (* and ?). */
#pragma once
static inline int fnmatch(const char *p, const char *s, int flags) {
    (void)flags;
    const char *star = 0, *ss = 0;
    while (*s) {
        if (*p == '?' || *p == *s) { p++; s++; continue; }
        if (*p == '*') { star = p++; ss = s; continue; }
        if (star) { p = star + 1; s = ++ss; continue; }
        return 1;
    }
    while (*p == '*') p++;
    return *p ? 1 : 0;
}
