/* Memory card server (mcserv, sid 0x80000400, original 2001-2002 command set).
 * Card contents live as ordinary files under <savedir>/mc<port>/ (mc<port>-2p/ in the
 * title menu's 2-player mode).
 */
#include "rt.h"
#include <sys/stat.h>
#include <dirent.h>
#include <fnmatch.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>

const char *g_save_dir = "memcard";
static char curdir[2][1024] = {"/", "/"};
static FILE *fds[32];
static int fd_port[32];
static uint32_t res_area;
#define R32(o) (*(uint32_t *)IOP(res_area + (o)))

extern int g_p2_mode;
/* the title menu's 2-player mode has cards of its own (mc0-2p), so its saves stay apart */
static void card_root(int port, char *out, size_t n) {
    snprintf(out, n, "%s/mc%d%s", g_save_dir, port & 1, g_p2_mode ? "-2p" : "");
    mkdir(g_save_dir, 0755);
    mkdir(out, 0755);
}

static void host_path(int port, const char *name, char *out, size_t n) {
    char root[512], full[1024];
    card_root(port, root, sizeof root);
    if (name[0] == '/') snprintf(full, sizeof full, "%s", name);
    else snprintf(full, sizeof full, "%s%s%s", curdir[port & 1], curdir[port & 1][strlen(curdir[port & 1]) - 1] == '/' ? "" : "/", name);
    /* forbid escaping the card root */
    for (char *p = full; *p; p++) if (p[0] == '.' && p[1] == '.') p[0] = p[1] = '_';
    snprintf(out, n, "%s%s", root, full);
}

static void put_datetime(uint8_t *d, time_t t) {
    struct tm tm; localtime_r(&t, &tm);
    d[0] = 0; d[1] = tm.tm_sec; d[2] = tm.tm_min; d[3] = tm.tm_hour; d[4] = tm.tm_mday; d[5] = tm.tm_mon + 1;
    uint16_t y = tm.tm_year + 1900; memcpy(d + 6, &y, 2);
}

static uint32_t mc_handler(uint32_t sid, int fno, uint32_t buf, int size, int *rs) {
    (void)sid; (void)size; (void)rs;
    uint32_t *in = (uint32_t *)IOP(buf);
    memset(IOP(res_area), 0, 16);
    int32_t result = 0;
    switch (fno) {
    case 0xFE: /* init */
        R32(0) = 0; R32(4) = 0x20b; R32(8) = 0x20f;
        INFO("[mc] init send:"); for (int i = 0; i < 12; i++) INFO(" %08x", in[i]); INFO("\n");
        return res_area;
    case 0x01: { /* GetInfo: desc {fd, port, slot, type?, free?, format?, buffer, param} */
        int port = in[1];
        uint32_t param = in[7];
        uint32_t ep[16] = {0};
        if ((port & 1) == 0) { ep[0] = 2; ep[1] = 8000; result = 0; }  /* PS2 card, 8000 free clusters */
        else { ep[0] = 0; ep[1] = 0; result = -10; }                  /* no card in port 1 */
        if (param) gwrite(param, ep, 64);
        break;
    }
    case 0x02: { /* Open: name_param {port, slot, flags, maxent, ptr, name[1024]} */
        int port = in[0], flags = in[2];
        char name[1025] = {0}; memcpy(name, IOP(buf + 20), 1024);
        char hp[2048]; host_path(port, name, hp, sizeof hp);
        if (flags & 0x40) { /* mkdir */
            result = mkdir(hp, 0755) == 0 ? 0 : -4;
            if (result && errno == EEXIST) result = -4;
            INFO("[mc] mkdir %s -> %d\n", name, result);
            break;
        }
        int slot = -1;
        for (int i = 0; i < 32; i++) if (!fds[i]) { slot = i; break; }
        if (slot < 0) { result = -7; break; }
        struct stat st;
        int exists = stat(hp, &st) == 0;
        const char *mode;
        if (!exists && !(flags & 0x200)) { result = -4; INFO("[mc] open %s: not found\n", name); break; }
        if (exists && S_ISDIR(st.st_mode)) { result = -4; break; }
        if (!exists) mode = "w+b";
        else if ((flags & 3) == 1) mode = "rb";
        else mode = "r+b";
        if ((flags & 0x400) && exists) mode = "w+b";  /* trunc */
        FILE *f = fopen(hp, mode);
        if (!f) { result = -4; break; }
        fds[slot] = f; fd_port[slot] = port;
        result = slot;
        INFO("[mc] open %s flags=%x -> %d\n", name, flags, result);
        break;
    }
    case 0x03: { int fd = in[0]; if (fd >= 0 && fd < 32 && fds[fd]) { fclose(fds[fd]); fds[fd] = NULL; result = 0; } else result = -5; break; }
    case 0x04: { /* Seek */
        int fd = in[0], off = in[4], whence = in[5];
        if (fd < 0 || fd >= 32 || !fds[fd]) { result = -5; break; }
        fseek(fds[fd], off, whence == 1 ? SEEK_CUR : whence == 2 ? SEEK_END : SEEK_SET);
        result = (int32_t)ftell(fds[fd]);
        break;
    }
    case 0x05: { /* Read */
        int fd = in[0], n = in[3];
        uint32_t dst = in[6], param = in[7];
        if (fd < 0 || fd >= 32 || !fds[fd]) { result = -5; break; }
        uint8_t *tmp = malloc(n > 0 ? n : 1);
        int got = (int)fread(tmp, 1, n, fds[fd]);
        gwrite(dst, tmp, got);
        free(tmp);
        if (param) { uint32_t ep[16] = {0}; gwrite(param, ep, 64); }
        result = got;
        break;
    }
    case 0x06: { /* Write: head bytes in data[0..origin), rest at buffer */
        int fd = in[0], n = in[3], head = in[5];
        uint32_t src = in[6];
        if (fd < 0 || fd >= 32 || !fds[fd]) { result = -5; break; }
        if (head > 16) head = 16;
        fwrite(IOP(buf + 32), 1, head, fds[fd]);
        if (n > 0) { uint8_t *tmp = malloc(n); gread(tmp, src, n); fwrite(tmp, 1, n, fds[fd]); free(tmp); }
        fflush(fds[fd]);
        result = head + (n > 0 ? n : 0);
        break;
    }
    case 0x0A: result = 0; break;
    case 0x0C: { /* ChDir */
        int port = in[0];
        char name[1025] = {0}; memcpy(name, IOP(buf + 20), 1024);
        uint32_t curptr = in[4];
        if (curptr) gwrite(curptr, curdir[port & 1], (uint32_t)strlen(curdir[port & 1]) + 1);
        if (name[0] == '/') snprintf(curdir[port & 1], 1024, "%s", name);
        else if (strcmp(name, ".")) {
            size_t l = strlen(curdir[port & 1]);
            snprintf(curdir[port & 1] + l, 1024 - l, "%s%s", curdir[port & 1][l - 1] == '/' ? "" : "/", name);
        }
        result = 0;
        break;
    }
    case 0x0D: { /* GetDir: name_param {port, slot, flags, maxent, table, name} */
        int port = in[0], maxent = in[3];
        uint32_t table = in[4];
        char pat[1025] = {0}; memcpy(pat, IOP(buf + 20), 1024);
        char dirpart[1024], glob[256];
        char *sl = strrchr(pat, '/');
        if (sl) { *sl = 0; snprintf(dirpart, sizeof dirpart, "%s", pat[0] ? pat : "/"); snprintf(glob, sizeof glob, "%s", sl + 1); }
        else { snprintf(dirpart, sizeof dirpart, "%s", curdir[port & 1]); snprintf(glob, sizeof glob, "%s", pat); }
        char hp[2048]; host_path(port, dirpart, hp, sizeof hp);
        DIR *d = opendir(hp);
        int count = 0;
        if (d) {
            struct dirent *e;
            while ((e = readdir(d)) && count < maxent) {
                if (fnmatch(glob[0] ? glob : "*", e->d_name, 0)) continue;
                char ep2[3072]; snprintf(ep2, sizeof ep2, "%s/%s", hp, e->d_name);
                struct stat st; if (stat(ep2, &st)) continue;
                uint8_t ent[64] = {0};
                put_datetime(ent, st.st_mtime); put_datetime(ent + 8, st.st_mtime);
                uint32_t fsz = S_ISDIR(st.st_mode) ? 0 : (uint32_t)st.st_size;
                memcpy(ent + 16, &fsz, 4);
                uint16_t attr = S_ISDIR(st.st_mode) ? 0x8427 : 0x8497;
                memcpy(ent + 20, &attr, 2);
                strncpy((char *)ent + 32, e->d_name, 31);
                if (table) gwrite(table + count * 64, ent, 64);
                count++;
            }
            closedir(d);
        }
        result = count;
        INFO("[mc] getdir '%s' -> %d\n", (const char *)IOP(buf + 20), count);
        break;
    }
    case 0x0E: result = 0; break;  /* SetFileInfo */
    case 0x0F: { /* Delete */
        int port = in[0];
        char name[1025] = {0}; memcpy(name, IOP(buf + 20), 1024);
        char hp[2048]; host_path(port, name, hp, sizeof hp);
        result = (remove(hp) == 0) ? 0 : -4;
        break;
    }
    case 0x10: case 0x11: result = 0; break;  /* format / unformat */
    case 0x12: result = 8000; break;          /* GetEntSpace */
    default:
        WARN("[mc] unhandled fno 0x%x\n", fno);
        result = -1;
        break;
    }
    R32(0) = (uint32_t)result;
    return res_area;
}

void iop_memcard_init(void) {
    res_area = iop_alloc(0x100);
    iop_register_server(0x80000400, mc_handler, 0x500);
}
