/*
 * hle_libc.c - libc/libm/liblog HLE for the guest.
 *
 * Includes: string/memory, stdio with VFS path remapping, Bionic struct
 * marshalling (stat/dirent/tm), time, math, environment, qsort with guest
 * comparator, and the gprintf/gscanf engines (AAPCS soft-float varargs).
 *
 * Pointer discipline: every value from the guest is gptr; translation to
 * host happens exactly once at the boundary (g2h / gchk).
 */
#include "emu.h"
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <poll.h>
#include <stdarg.h>

void malloc_hle_init(void);

/* ================================================================== */
/* guest errno                                                        */
/* ================================================================== */

static void guest_errno_set(cpu_t *c, int e)
{
    if (c && c->errno_addr) st32(c->errno_addr, (u32)e);
}
static int guest_errno_get(cpu_t *c)
{
    return (c && c->errno_addr) ? (int)ld32(c->errno_addr) : 0;
}

/* ================================================================== */
/* VFS: guest path -> host path                                       */
/* ================================================================== */

static char g_cwd[512] = "/";
static pthread_mutex_t vfs_lock = PTHREAD_MUTEX_INITIALIZER;

/* read-only location of a translated tail (RR3 may keep apk/ and sdcard/ elsewhere) */
static void vfs_data_path(char *out, size_t n, const char *tail)
{
    if (G.game == 3) {
        if (G.rr3_base_apk && !strcmp(tail, "apk/base.apk")) { snprintf(out, n, "%s", G.rr3_base_apk); return; }
        if (G.rr3_apk_dir && !strncmp(tail, "apk/", 4)) { snprintf(out, n, "%s/%s", G.rr3_apk_dir, tail + 4); return; }
        if (G.rr3_sdcard_dir && !strncmp(tail, "sdcard", 6) && (!tail[6] || tail[6] == '/')) {
            snprintf(out, n, "%s%s", G.rr3_sdcard_dir, tail + 6);
            return;
        }
    }
    snprintf(out, n, "%s/%s", G.assets_dir, tail);
}

/* translate; returns static buffer (call under vfs_lock or copy out).
 * Writes land in ./save; reads prefer ./save (so saves read back) then the data root. */
static const char *vfs_xlate_locked(const char *gpath, int for_write)
{
    static char out[1024];
    char path[1024];
    const char *tail;
    char t3[1024];
    const char *save = G.save_dir ? G.save_dir : "./save";

    if (gpath[0] != '/') {
        snprintf(path, sizeof(path), "%s/%s", g_cwd, gpath);
        gpath = path;
    }
    if (G.game == 3) {
        /* RR3: external app data -> sdcard/, internal app data -> internal/, anything else -> other/ */
        const char *pkg = "com.ea.games.r3_row";
        const char *e = strstr(gpath, "Android/data/com.ea.games.r3_row");
        const char *i = strstr(gpath, "/data/data/com.ea.games.r3_row");
        if (!i) i = strstr(gpath, "/data/user/0/com.ea.games.r3_row");
        const char *ap = strstr(gpath, "/data/app/com.ea.games.r3_row");
        if (ap) ap = strchr(ap + 10, '/');                /* after "/data/app/<pkg>-N" */
        if (e) snprintf(t3, sizeof(t3), "sdcard%s", e + strlen("Android/data/") + strlen(pkg));
        else if (ap) snprintf(t3, sizeof(t3), "apk%s", ap);
        else if (i) snprintf(t3, sizeof(t3), "internal%s", strstr(i, pkg) + strlen(pkg));
        else snprintf(t3, sizeof(t3), "other%s", gpath);
        tail = t3;
    } else {
        const char *anchor = strstr(gpath, "com.ea.game.realracing2");
        if (anchor) tail = anchor;
        else if (!strncmp(gpath, "/sdcard/", 8)) tail = gpath + 8;
        else if (!strncmp(gpath, "/mnt/sdcard/", 12)) tail = gpath + 12;
        else if (!strncmp(gpath, "/data/data/", 11)) tail = gpath + 11;
        else tail = gpath + 1;
    }

    if (strstr(tail, "..")) {
        snprintf(out, sizeof(out), "%s/_invalid_", save);
        return out;
    }
    snprintf(out, sizeof(out), "%s/%s", save, tail);
    if (!for_write && access(out, F_OK) != 0)
        vfs_data_path(out, sizeof(out), tail);
    return out;
}

/* read-only base location of a guest path (directory merging), or NULL */
const char *vfs_base_path(const char *gpath, char *buf, size_t n);
const char *vfs_base_path(const char *gpath, char *buf, size_t n)
{
    pthread_mutex_lock(&vfs_lock);
    const char *o = vfs_xlate_locked(gpath, 1);
    const char *save = G.save_dir ? G.save_dir : "./save";
    size_t sl = strlen(save);
    int ok = !strncmp(o, save, sl) && o[sl] == '/';
    if (ok) vfs_data_path(buf, n, o + sl + 1);
    pthread_mutex_unlock(&vfs_lock);
    return ok ? buf : NULL;
}

/* host path for a guest path, read access (malloc'd) */
char *vfs_host_path(const char *gpath)
{
    pthread_mutex_lock(&vfs_lock);
    char *r = strdup(vfs_xlate_locked(gpath, 0));
    pthread_mutex_unlock(&vfs_lock);
    return r;
}

volatile int g_hide_next_tex;

/* HUD images the user doesn't want drawn (default: touch pedals), RR2_HIDE=a,b,... */
static void vfs_check_hidden(const char *gpath)
{
    static char list[512];
    static int init;
    if (!init) {
        const char *e = getenv("RR2_HIDE");
        snprintf(list, sizeof(list), "%s", e ? e : "hud_accel_icon_gray,hud_brake_icon_gray");
        init = 1;
    }
    const char *base = strrchr(gpath, '/');
    base = base ? base + 1 : gpath;
    if (!strncmp(base, "hud_speed_icon", 14)) { g_hide_next_tex = 2; return; }   /* race HUD marker */
    if (!list[0]) return;
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", list);
    for (char *t = strtok(tmp, ","); t; t = strtok(NULL, ","))
        if (*t && !strncmp(base, t, strlen(t))) { g_hide_next_tex = 1; return; }
}

static const char *vfs_xlate(const char *gpath, int for_write)
{
    if (!for_write) vfs_check_hidden(gpath);
    pthread_mutex_lock(&vfs_lock);
    const char *r = strdup(vfs_xlate_locked(gpath, for_write));
    pthread_mutex_unlock(&vfs_lock);
    return r;   /* caller frees */
}

/* make parent dirs of path (save/ writes) */
static void vfs_mkdirs(const char *path)
{
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            emu_mkdir(tmp, 0755);
            *p = '/';
        }
    }
}

/* ================================================================== */
/* FILE* / fd tables                                                  */
/* ================================================================== */

#define MAX_GFILES 256

typedef struct { gptr gfile; FILE *host; } gfile_ent;
static gfile_ent g_files[MAX_GFILES];
static int g_nfiles;
static pthread_mutex_t files_lock = PTHREAD_MUTEX_INITIALIZER;

static gptr g_sf[3];   /* guest __sF entries: stdin/stdout/stderr */

static FILE *file_for(gptr gf)
{
    if (gf == g_sf[0]) return stdin;
    if (gf == g_sf[1]) return stdout;
    if (gf == g_sf[2]) return stderr;
    pthread_mutex_lock(&files_lock);
    for (int i = 0; i < g_nfiles; i++)
        if (g_files[i].gfile == gf) {
            FILE *f = g_files[i].host;
            pthread_mutex_unlock(&files_lock);
            return f;
        }
    pthread_mutex_unlock(&files_lock);
    return NULL;
}

static gptr file_register(FILE *host)
{
    gptr gf = hle_data_alloc(256, 16);   /* fake guest FILE */
    pthread_mutex_lock(&files_lock);
    if (g_nfiles < MAX_GFILES) {
        g_files[g_nfiles].gfile = gf;
        g_files[g_nfiles].host = host;
        g_nfiles++;
    }
    pthread_mutex_unlock(&files_lock);
    return gf;
}

static void file_unregister(gptr gf)
{
    pthread_mutex_lock(&files_lock);
    for (int i = 0; i < g_nfiles; i++)
        if (g_files[i].gfile == gf) {
            g_files[i] = g_files[--g_nfiles];
            break;
        }
    pthread_mutex_unlock(&files_lock);
}

/* ================================================================== */
/* gprintf engine: format + AAPCS varargs walker -> host buffer       */
/* ================================================================== */

typedef struct {
    cpu_t *c;
    int reg;            /* next core reg (r0-r3 consumed so far) */
    u32 stk;            /* stack offset for args beyond r3 */
    gptr vlist;         /* if nonzero: guest va_list pointer (v* calls) */
} gva_t;

static u32 gva_u32(gva_t *w)
{
    if (w->vlist) { u32 v = ld32(w->vlist); w->vlist += 4; return v; }
    if (w->reg < 4) return w->c->r[w->reg++];
    u32 v = ld32(w->c->r[13] + w->stk); w->stk += 4; return v;
}

static u64 gva_u64(gva_t *w)
{
    if (w->vlist) {
        if (w->vlist & 7) w->vlist += 4;
        u64 v = ld32(w->vlist) | ((u64)ld32(w->vlist + 4) << 32);
        w->vlist += 8; return v;
    }
    if (w->reg & 1) w->reg++;
    if (w->reg + 1 < 4) {
        u32 lo = w->c->r[w->reg], hi = w->c->r[w->reg + 1];
        w->reg += 2; return lo | ((u64)hi << 32);
    }
    if (w->reg < 4) w->reg = 4;
    if (w->stk & 7) w->stk += 4;
    u64 v = ld32(w->c->r[13] + w->stk) | ((u64)ld32(w->c->r[13] + w->stk + 4) << 32);
    w->stk += 8; return v;
}

/* render one conversion into out (returns chars written) */
static size_t gfmt_one(char *out, size_t outsz, const char *spec, int speclen,
                       gva_t *w, char conv)
{
    char fmt[32];
    fmt[0] = '%';
    memcpy(fmt + 1, spec, speclen);
    fmt[speclen + 1] = conv;
    fmt[speclen + 2] = 0;
    switch (conv) {
    case 'd': case 'i': {
        if (strstr(spec, "ll")) return snprintf(out, outsz, fmt, (long long)(s64)gva_u64(w));
        if (strchr(spec, 'l'))  return snprintf(out, outsz, fmt, (long)(s32)gva_u32(w));
        return snprintf(out, outsz, fmt, (int)(s32)gva_u32(w)); }
    case 'u': case 'x': case 'X': case 'o': {
        if (strstr(spec, "ll")) return snprintf(out, outsz, fmt, (unsigned long long)gva_u64(w));
        if (strchr(spec, 'l'))  return snprintf(out, outsz, fmt, (unsigned long)gva_u32(w));
        return snprintf(out, outsz, fmt, (unsigned)gva_u32(w)); }
    case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
        u64 bits = gva_u64(w);       /* soft-float: doubles travel in reg pairs */
        double d; memcpy(&d, &bits, 8);
        return snprintf(out, outsz, fmt, d); }
    case 'c': {
        int ch = (int)gva_u32(w);
        return snprintf(out, outsz, fmt, ch); }
    case 's': {
        gptr sp = gva_u32(w);
        const char *s = sp ? (const char *)g2h(sp) : "(null)";
        return snprintf(out, outsz, fmt, s); }
    case 'p': {
        return snprintf(out, outsz, fmt, (void *)(uintptr_t)gva_u32(w)); }
    case 'n': {
        gptr np = gva_u32(w);
        if (np) st32(np, 0);   /* patched by caller count; simplified */
        return 0; }
    default:
        if (outsz) { out[0] = conv; if (outsz > 1) out[1] = 0; }
        return 1;
    }
}

/* core formatter: guest format string -> host buffer (bounded) */
static size_t gfmt(char *out, size_t outsz, gptr fmt_g, gva_t *w)
{
    const char *f = (const char *)g2h(fmt_g);
    size_t o = 0;
    while (*f && o + 1 < outsz) {
        if (*f != '%') { out[o++] = *f++; continue; }
        f++;
        if (*f == '%') { out[o++] = '%'; f++; continue; }
        char spec[24];
        int sl = 0;
        /* flags, width, precision, length modifiers */
        while (*f && sl < 20 && !strchr("diuxXofeEgGacspn", *f))
            spec[sl++] = *f++;
        if (!*f) break;
        spec[sl] = 0;
        char conv = *f++;
        /* dynamic width/precision: substitute from args */
        char sub[24];
        int sl2 = 0;
        for (int i = 0; i < sl; i++) {
            if (spec[i] == '*') {
                int v = (int)gva_u32(w);
                sl2 += snprintf(sub + sl2, sizeof(sub) - sl2, "%d", v);
            } else sub[sl2++] = spec[i];
        }
        sub[sl2] = 0;
        if (conv == 'n') {
            gptr np = gva_u32(w);
            if (np) st32(np, (u32)o);
            continue;
        }
        char tmp[1024];
        size_t n = gfmt_one(tmp, sizeof(tmp), sub, sl2, w, conv);
        for (size_t i = 0; i < n && o + 1 < outsz; i++) out[o++] = tmp[i];
    }
    out[o] = 0;
    return o;
}

/* ================================================================== */
/* gscanf engine (minimal: %d %u %x %o %i %f %g %e %s %c %%)          */
/* ================================================================== */

typedef struct { cpu_t *c; const char *p; FILE *f; u32 n; } scan_src;
static int scan_getc(scan_src *s)
{
    int ch = s->f ? fgetc(s->f) : (*s->p ? (unsigned char)*s->p++ : EOF);
    if (ch != EOF) s->n++;
    return ch;
}
static void scan_ungetc(scan_src *s, int ch)
{
    if (ch == EOF) return;
    s->n--;
    if (s->f) ungetc(ch, s->f);
    else s->p--;
}
static void scan_store_int(gptr p, u64 v, int size)
{
    if (!p) return;
    switch (size) {
    case 1: st8(p, (u8)v); break;
    case 2: st16(p, (u16)v); break;
    case 8: st32(p, (u32)v); st32(p + 4, (u32)(v >> 32)); break;
    default: st32(p, (u32)v);
    }
}

static int gscan(cpu_t *c, scan_src *src, gptr fmt_g, gva_t *w)
{
    const char *f = (const char *)g2h(fmt_g);
    int nconv = 0;
    while (*f) {
        if (isspace(*f)) {
            int ch;
            do { ch = scan_getc(src); } while (ch != EOF && isspace(ch));
            scan_ungetc(src, ch);
            f++;
            continue;
        }
        if (*f != '%') {
            int ch = scan_getc(src);
            if (ch != *f) { scan_ungetc(src, ch); return nconv; }
            f++;
            continue;
        }
        f++;
        int suppress = 0, width = 0, islong = 0, isll = 0, isize = 4;
        if (*f == '*') { suppress = 1; f++; }
        while (isdigit(*f)) width = width * 10 + (*f++ - '0');
        if (*f == 'l') { islong = 1; f++; if (*f == 'l') { isll = 1; isize = 8; f++; } }
        else if (*f == 'h') { isize = 2; f++; if (*f == 'h') { isize = 1; f++; } }
        else if (*f == 'j' || *f == 'q' || *f == 'L') { isll = 1; isize = 8; f++; }
        else if (*f == 'z' || *f == 't') f++;
        char conv = *f++;
        if (!width) width = (conv == 'c' || conv == 'n') ? 1 : 256;

        if (conv == '%') {
            int ch = scan_getc(src);
            if (ch != '%') { scan_ungetc(src, ch); return nconv; }
            continue;
        }
        if (conv == 'n') {
            if (!suppress) scan_store_int(gva_u32(w), src->n, isize);   /* %n does not count */
            continue;
        }
        /* skip leading space for numeric/string conversions */
        int ch;
        if (conv != 'c' && conv != '[') {
            do { ch = scan_getc(src); } while (ch != EOF && isspace(ch));
            scan_ungetc(src, ch);
            if (ch == EOF) return nconv ? nconv : EOF;
        }
        char buf[320];
        int bi = 0;
        switch (conv) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': {
            int base = conv == 'x' || conv == 'X' ? 16 : conv == 'o' ? 8 : conv == 'i' ? 0 : 10;
            if (bi < width && (ch = scan_getc(src)) != EOF) {   /* sign */
                if (ch == '+' || ch == '-') buf[bi++] = ch; else scan_ungetc(src, ch);
            }
            if ((base == 16 || base == 0) && bi < width && (ch = scan_getc(src)) != EOF) {
                if (ch == '0') {
                    buf[bi++] = '0';
                    int x = bi < width ? scan_getc(src) : EOF;
                    if (x == 'x' || x == 'X') { buf[bi++] = (char)x; base = 16; }
                    else { scan_ungetc(src, x); if (base == 0) base = 8; }
                } else scan_ungetc(src, ch);
            }
            if (base == 0) base = 10;
            while (bi < width && bi < 300 && (ch = scan_getc(src)) != EOF) {
                int ok = base == 16 ? isxdigit(ch) : base == 8 ? (ch >= '0' && ch <= '7') : isdigit(ch);
                if (!ok) { scan_ungetc(src, ch); break; }
                buf[bi++] = ch;
            }
            if (!bi || (bi == 1 && (buf[0] == '+' || buf[0] == '-'))) return nconv;
            buf[bi] = 0;
            if (suppress) break;
            u64 v = (conv == 'd' || conv == 'i') ? (u64)strtoll(buf, NULL, base) : strtoull(buf, NULL, base);
            scan_store_int(gva_u32(w), v, isize);
            nconv++;
            break; }
        case 'f': case 'g': case 'G': case 'e': case 'E': {
            while (bi < width && (ch = scan_getc(src)) != EOF &&
                   (isdigit(ch) || ch == '+' || ch == '-' || ch == '.' ||
                    ch == 'e' || ch == 'E' || ch == 'n' || ch == 'a' || ch == 'i' || ch == 'f' || ch == 'N' || ch == 'A' || ch == 'I' || ch == 'F'))
                buf[bi++] = ch;
            scan_ungetc(src, ch);
            if (!bi) return nconv;
            buf[bi] = 0;
            if (suppress) break;
            double v = strtod(buf, NULL);
            gptr p = gva_u32(w);
            if (p) {
                if (islong || isll) { u64 b; memcpy(&b, &v, 8); st32(p, (u32)b); st32(p + 4, (u32)(b >> 32)); }
                else { float fv = (float)v; u32 b; memcpy(&b, &fv, 4); st32(p, b); }
            }
            nconv++;
            break; }
        case 's': {
            while (bi < width && (ch = scan_getc(src)) != EOF && !isspace(ch))
                buf[bi++] = ch;
            scan_ungetc(src, ch);
            if (!bi) return nconv;
            buf[bi] = 0;
            if (!suppress) { gptr p = gva_u32(w); if (p) memcpy(g2h(p), buf, bi + 1); nconv++; }
            break; }
        case 'c': {                                   /* %Nc: exactly N chars, no NUL, no skip */
            gptr p = suppress ? 0 : gva_u32(w);
            for (int k = 0; k < width; k++) {
                ch = scan_getc(src);
                if (ch == EOF) { if (!k) return nconv ? nconv : EOF; break; }
                if (p) st8(p + (u32)k, (u8)ch);
            }
            if (!suppress) nconv++;
            break; }
        case '[': {
            /* scanset: [^...] or [...] */
            int negate = 0;
            if (*f == '^') { negate = 1; f++; }
            char set[256]; int si = 0;
            if (*f == ']') set[si++] = *f++;
            while (*f && *f != ']' && si < 250) set[si++] = *f++;
            if (*f == ']') f++;
            set[si] = 0;
            u8 in[256] = { 0 };
            for (int k = 0; k < si; k++) {
                if (k + 2 < si && set[k + 1] == '-') {
                    for (int r = (u8)set[k]; r <= (u8)set[k + 2]; r++) in[r] = 1;
                    k += 2;
                } else in[(u8)set[k]] = 1;
            }
            while (bi < width && bi < 300 && (ch = scan_getc(src)) != EOF) {
                int inset = in[(u8)ch];
                if (negate) inset = !inset;
                if (!inset) break;
                buf[bi++] = ch;
            }
            scan_ungetc(src, ch);
            if (!bi) return nconv;
            buf[bi] = 0;
            if (!suppress) { gptr p = gva_u32(w); if (p) memcpy(g2h(p), buf, bi + 1); nconv++; }
            break; }
        default:
            return nconv;
        }
    }
    return nconv;
}

/* ================================================================== */
/* HLE helpers shared with other files                                */
/* ================================================================== */

void libc_register_stdio_for(gptr gf, FILE *f);   /* (unused export) */

/* ================================================================== */
/* string / memory                                                    */
/* ================================================================== */

#define GSTR(p) ((const char *)g2h(p))

static void hle_memcpy(cpu_t *c)  { memcpy(g2h(harg(c,0)), g2h(harg(c,1)), harg(c,2)); hret(c, harg(c,0)); }
static void hle_memmove(cpu_t *c) { memmove(g2h(harg(c,0)), g2h(harg(c,1)), harg(c,2)); hret(c, harg(c,0)); }
static void hle_memset(cpu_t *c)  { memset(g2h(harg(c,0)), (int)harg(c,1), harg(c,2)); hret(c, harg(c,0)); }
static void hle_memcmp(cpu_t *c)  { hret(c, (u32)memcmp(g2h(harg(c,0)), g2h(harg(c,1)), harg(c,2))); }
static void hle_bcopy(cpu_t *c)   { memmove(g2h(harg(c,1)), g2h(harg(c,0)), harg(c,2)); }
static void hle_strlen(cpu_t *c)  { hret(c, (u32)strlen(GSTR(harg(c,0)))); }
static void hle_strcmp(cpu_t *c)  { hret(c, (u32)strcmp(GSTR(harg(c,0)), GSTR(harg(c,1)))); }
static void hle_strncmp(cpu_t *c) { hret(c, (u32)strncmp(GSTR(harg(c,0)), GSTR(harg(c,1)), harg(c,2))); }
static void hle_strcasecmp(cpu_t *c) { hret(c, (u32)strcasecmp(GSTR(harg(c,0)), GSTR(harg(c,1)))); }
static void hle_strcpy(cpu_t *c)  { strcpy(g2h(harg(c,0)), GSTR(harg(c,1))); hret(c, harg(c,0)); }
static void hle_strncpy(cpu_t *c) { strncpy(g2h(harg(c,0)), GSTR(harg(c,1)), harg(c,2)); hret(c, harg(c,0)); }
static void hle_strcat(cpu_t *c)  { strcat(g2h(harg(c,0)), GSTR(harg(c,1))); hret(c, harg(c,0)); }
static void hle_strchr(cpu_t *c)
{
    const char *s = GSTR(harg(c,0));
    const char *r = strchr(s, (int)harg(c,1));
    hret(c, r ? h2g(r) : 0);
}
static void hle_strrchr(cpu_t *c)
{
    const char *s = GSTR(harg(c,0));
    const char *r = strrchr(s, (int)harg(c,1));
    hret(c, r ? h2g(r) : 0);
}
static void hle_strstr(cpu_t *c)
{
    const char *r = strstr(GSTR(harg(c,0)), GSTR(harg(c,1)));
    hret(c, r ? h2g(r) : 0);
}
static void hle_strtok(cpu_t *c)
{
    /* per-thread strtok state in guest memory (after errno slot) */
    gptr save = c->errno_addr + 8;
    gptr str = harg(c,0), delim = harg(c,1);
    char *sp = str ? g2h(str) : (char *)g2h(ld32(save));
    if (!sp || (uintptr_t)sp == (uintptr_t)g_mem) { hret(c, 0); return; }
    char *r = strtok(sp, GSTR(delim));
    st32(save, r ? h2g(r + strlen(r) + 1) : 0);
    hret(c, r ? h2g(r) : 0);
}
static void hle_atoi(cpu_t *c)    { hret(c, (u32)atoi(GSTR(harg(c,0)))); }
static void hle_strtoul(cpu_t *c)
{
    const char *s = GSTR(harg(c,0));
    char *end = NULL;
    unsigned long v = strtoul(s, &end, (int)harg(c,2));
    gptr ep = harg(c,1);
    if (ep) st32(ep, end ? h2g(end) : h2g(s));
    hret(c, (u32)v);
}
static void hle_strtod(cpu_t *c)
{
    const char *s = GSTR(harg(c,0));
    char *end = NULL;
    double v = strtod(s, &end);
    gptr ep = harg(c,1);
    if (ep) st32(ep, end ? h2g(end) : h2g(s));
    hret64(c, *(u64 *)&v);
}
static void hle_qsort(cpu_t *c)
{
    gptr base = harg(c,0);
    u32 nmemb = harg(c,1), size = harg(c,2), cmp = harg(c,3);
    /* merge sort with guest comparator via emu_call */
    u8 *tmp = malloc(nmemb * size);
    u8 *b = g2h(base);
    for (u32 width = 1; width < nmemb; width *= 2) {
        for (u32 i = 0; i < nmemb; i += 2 * width) {
            u32 l = i, m = i + width < nmemb ? i + width : nmemb;
            u32 r = i + 2 * width < nmemb ? i + 2 * width : nmemb;
            u32 p = l, q = m, t = l;
            while (p < m && q < r) {
                u32 args[2] = { h2g(b) + p * size, h2g(b) + q * size };
                s32 cmpres = (s32)emu_call(c, cmp, 2, args);
                memcpy(tmp + (size_t)t++ * size, b + (cmpres <= 0 ? p++ : q++) * (size_t)size, size);
            }
            while (p < m) { memcpy(tmp + (size_t)t++ * size, b + (size_t)p++ * size, size); }
            while (q < r) { memcpy(tmp + (size_t)t++ * size, b + (size_t)q++ * size, size); }
        }
        memcpy(b, tmp, nmemb * size);
    }
    free(tmp);
}

/* ================================================================== */
/* stdio                                                              */
/* ================================================================== */

static void hle_fopen(cpu_t *c)
{
    const char *mode = GSTR(harg(c,1));
    int for_write = strchr(mode, 'w') || strchr(mode, 'a') || strchr(mode, '+');
    const char *host = vfs_xlate(GSTR(harg(c,0)), for_write);
    if (for_write) vfs_mkdirs(host);
    char hmode[8];                                    /* Android stdio is always binary */
    snprintf(hmode, sizeof(hmode), "%.6s%s", mode, strchr(mode, 'b') ? "" : "b");
    FILE *f = fopen(host, hmode);
    VLOG(1, "[vfs] fopen(%s [%s]) -> %s = %p\n", GSTR(harg(c,0)), mode, host, (void *)f);
    free((void *)host);
    if (!f) { guest_errno_set(c, ENOENT); hret(c, 0); return; }
    hret(c, file_register(f));
}
static void hle_fclose(cpu_t *c)
{
    FILE *f = file_for(harg(c,0));
    if (!f || f == stdin || f == stdout || f == stderr) { hret(c, 0); return; }
    file_unregister(harg(c,0));
    hret(c, (u32)fclose(f));
}
static void hle_fread(cpu_t *c)
{
    FILE *f = file_for(harg(c,3));
    if (!f) { hret(c, 0); return; }
    emu_prefault(g2h(harg(c,0)), (size_t)harg(c,1) * harg(c,2));
    hret(c, (u32)fread(g2h(harg(c,0)), harg(c,1), harg(c,2), f));
}
static void hle_fwrite(cpu_t *c)
{
    FILE *f = file_for(harg(c,3));
    if (!f) { hret(c, 0); return; }
    emu_prefault(g2h(harg(c,0)), (size_t)harg(c,1) * harg(c,2));
    hret(c, (u32)fwrite(g2h(harg(c,0)), harg(c,1), harg(c,2), f));
}
static void hle_fseek(cpu_t *c)
{
    FILE *f = file_for(harg(c,0));
    hret(c, f ? (u32)fseek(f, (long)(s32)harg(c,1), (int)harg(c,2)) : (u32)-1);
}
static void hle_ftell(cpu_t *c)
{
    FILE *f = file_for(harg(c,0));
    hret(c, f ? (u32)ftell(f) : (u32)-1);
}
static void hle_fflush(cpu_t *c)
{
    FILE *f = file_for(harg(c,0));
    hret(c, f ? (u32)fflush(f) : 0);
}
static void hle_fgetc(cpu_t *c)
{
    FILE *f = file_for(harg(c,0));
    hret(c, f ? (u32)fgetc(f) : (u32)EOF);
}
static void hle_fgets(cpu_t *c)
{
    FILE *f = file_for(harg(c,2));
    if (!f) { hret(c, 0); return; }
    emu_prefault(g2h(harg(c,0)), harg(c,1));
    char *r = fgets(g2h(harg(c,0)), (int)harg(c,1), f);
    hret(c, r ? harg(c,0) : 0);
}
static void hle_fputc(cpu_t *c)
{
    FILE *f = file_for(harg(c,1));
    hret(c, f ? (u32)fputc((int)harg(c,0), f) : (u32)EOF);
}
static void hle_fputs(cpu_t *c)
{
    FILE *f = file_for(harg(c,1));
    hret(c, f ? (u32)fputs(GSTR(harg(c,0)), f) : (u32)EOF);
}
static void hle_ungetc(cpu_t *c)
{
    FILE *f = file_for(harg(c,1));
    hret(c, f ? (u32)ungetc((int)harg(c,0), f) : (u32)EOF);
}
static void hle_putchar(cpu_t *c) { hret(c, (u32)putchar((int)harg(c,0))); }
static void hle_perror(cpu_t *c)
{
    fprintf(stderr, "%s: %s\n", GSTR(harg(c,0)), strerror(guest_errno_get(c)));
}
static void hle_fwide(cpu_t *c) { hret(c, 0); }
#ifdef _WIN32
#define fsync _commit
#endif
static void hle_fsync(cpu_t *c)  { if ((int)harg(c,0) >= 0) fsync((int)harg(c,0)); hret(c, 0); }
static void hle_ftruncate(cpu_t *c) { hret(c, (u32)ftruncate((int)harg(c,0), (off_t)(s32)harg(c,1))); }

/* printf family via gfmt */
void hle_sprintf(cpu_t *c)
{
    gva_t w = { .c = c, .reg = 2, .stk = 0, .vlist = 0 };
    gfmt(g2h(harg(c,0)), 1 << 20, harg(c,1), &w);
    hret(c, (u32)strlen(GSTR(harg(c,0))));
}
static void hle_snprintf(cpu_t *c)
{
    gva_t w = { .c = c, .reg = 3, .stk = 0, .vlist = 0 };
    size_t n = gfmt(g2h(harg(c,0)), harg(c,1), harg(c,2), &w);
    hret(c, (u32)n);
}
static void hle_vsprintf(cpu_t *c)
{
    gva_t w = { .c = c, .vlist = harg(c,2) };
    gfmt(g2h(harg(c,0)), 1 << 20, harg(c,1), &w);
    hret(c, (u32)strlen(GSTR(harg(c,0))));
}
static void hle_vsnprintf(cpu_t *c)
{
    gva_t w = { .c = c, .vlist = harg(c,3) };
    size_t n = gfmt(g2h(harg(c,0)), harg(c,1), harg(c,2), &w);
    hret(c, (u32)n);
}
static void hle_fprintf(cpu_t *c)
{
    FILE *f = file_for(harg(c,0));
    if (!f) { hret(c, -1); return; }
    char buf[4096];
    gva_t w = { .c = c, .reg = 2, .stk = 0, .vlist = 0 };
    size_t n = gfmt(buf, sizeof(buf), harg(c,1), &w);
    fwrite(buf, 1, n, f);
    hret(c, (u32)n);
}
static void hle_vfprintf(cpu_t *c)
{
    FILE *f = file_for(harg(c,0));
    if (!f) { hret(c, -1); return; }
    char buf[4096];
    gva_t w = { .c = c, .vlist = harg(c,2) };
    size_t n = gfmt(buf, sizeof(buf), harg(c,1), &w);
    fwrite(buf, 1, n, f);
    hret(c, (u32)n);
}
static void hle_vprintf(cpu_t *c)
{
    char buf[4096];
    gva_t w = { .c = c, .vlist = harg(c,1) };
    size_t n = gfmt(buf, sizeof(buf), harg(c,0), &w);
    fwrite(buf, 1, n, stdout);
    hret(c, (u32)n);
}
void hle_sscanf(cpu_t *c)
{
    scan_src src = { .c = c, .p = GSTR(harg(c,0)), .f = NULL };
    gva_t w = { .c = c, .reg = 2, .stk = 0, .vlist = 0 };
    hret(c, (u32)gscan(c, &src, harg(c,1), &w));
}
static void hle_fscanf(cpu_t *c)
{
    FILE *f = file_for(harg(c,0));
    if (!f) { hret(c, (u32)EOF); return; }
    scan_src src = { .c = c, .f = f };
    gva_t w = { .c = c, .reg = 2, .stk = 0, .vlist = 0 };
    hret(c, (u32)gscan(c, &src, harg(c,1), &w));
}

/* ================================================================== */
/* low-level fd io                                                    */
/* ================================================================== */

static void hle_open(cpu_t *c)
{
    u32 gf = harg(c,1);                               /* ARM Linux O_* bits, rebuilt for the host */
    int flags = (gf & 3) == 1 ? O_WRONLY : (gf & 3) == 2 ? O_RDWR : O_RDONLY;
    if (gf & 0100) flags |= O_CREAT;
    if (gf & 0200) flags |= O_EXCL;
    if (gf & 01000) flags |= O_TRUNC;
    if (gf & 02000) flags |= O_APPEND;
    if (gf & 0040000) flags |= O_DIRECTORY;
    if (gf & 0100000) flags |= O_NOFOLLOW;
    flags |= O_BINARY;
    int for_write = (flags & 3) != O_RDONLY || (flags & O_CREAT);
    const char *host = vfs_xlate(GSTR(harg(c,0)), for_write);
    if (for_write) vfs_mkdirs(host);
    int fd = open(host, flags, 0644);
    VLOG(1, "[vfs] open(%s, %x) -> %s = %d\n", GSTR(harg(c,0)), flags, host, fd);
    free((void *)host);
    if (fd < 0) { guest_errno_set(c, errno); hret(c, (u32)-1); return; }
    hret(c, (u32)fd);
}
static void hle_close(cpu_t *c)  { hret(c, (u32)close((int)harg(c,0))); }
static void hle_read(cpu_t *c)   { emu_prefault(g2h(harg(c,1)), harg(c,2)); hret(c, (u32)read((int)harg(c,0), g2h(harg(c,1)), harg(c,2))); }
static void hle_write(cpu_t *c)  { emu_prefault(g2h(harg(c,1)), harg(c,2)); hret(c, (u32)write((int)harg(c,0), g2h(harg(c,1)), harg(c,2))); }
static void hle_lseek(cpu_t *c)  { hret(c, (u32)lseek((int)harg(c,0), (off_t)(s32)harg(c,1), (int)harg(c,2))); }
static void hle_fcntl(cpu_t *c)  { LOG_ONCE("[hle] fcntl stub\n"); hret(c, 0); }
static void hle_ioctl(cpu_t *c)  { LOG_ONCE("[hle] ioctl stub\n"); hret(c, (u32)-1); }
static void hle_poll(cpu_t *c)
{
    gptr fds_g = harg(c,0);
    u32 nfds = harg(c,1);
    int timeout = (int)harg(c,2);
    struct pollfd pf[16];
    if (nfds > 16) nfds = 16;
    for (u32 i = 0; i < nfds; i++) {
        pf[i].fd = (int)ld32(fds_g + i * 8);
        pf[i].events = (short)ld16(fds_g + i * 8 + 4);
        pf[i].revents = 0;
    }
    int r = poll(pf, nfds, timeout);
    for (u32 i = 0; i < nfds; i++)
        st16(fds_g + i * 8 + 6, (u16)pf[i].revents);
    hret(c, (u32)r);
}

/* ================================================================== */
/* filesystem metadata (Bionic layouts!)                              */
/* ================================================================== */

/* Bionic ARM struct stat (== stat64), 104 bytes:
 *   0 st_dev u64 | 8 pad[4] | 12 __st_ino | 16 st_mode | 20 st_nlink | 24 st_uid | 28 st_gid
 *  32 st_rdev u64 | 40 pad[4] | 48 st_size s64 | 56 st_blksize | 64 st_blocks u64
 *  72 atime,nsec | 80 mtime,nsec | 88 ctime,nsec | 96 st_ino u64 */
void marshal_stat(gptr out, const struct stat *st)
{
    memset(g2h(out), 0, 104);
    st32(out + 0,  (u32)st->st_dev);
    st32(out + 12, (u32)st->st_ino);
    st32(out + 16, (u32)st->st_mode);
    st32(out + 20, (u32)st->st_nlink);
    st32(out + 24, (u32)st->st_uid);
    st32(out + 28, (u32)st->st_gid);
    st32(out + 32, (u32)st->st_rdev);
    st32(out + 48, (u32)st->st_size);
    st32(out + 52, (u32)((u64)st->st_size >> 32));
#ifdef _WIN32
    st32(out + 56, 4096);
    st32(out + 64, (u32)((st->st_size + 511) / 512));
#else
    st32(out + 56, (u32)st->st_blksize);
    st32(out + 64, (u32)st->st_blocks);
#endif
    st32(out + 72, (u32)st->st_atime);
    st32(out + 80, (u32)st->st_mtime);
    st32(out + 88, (u32)st->st_ctime);
    st32(out + 96, (u32)st->st_ino);
}

static void hle_stat(cpu_t *c)
{
    const char *host = vfs_xlate(GSTR(harg(c,0)), 0);
    struct stat st;
    int r = stat(host, &st);
    VLOG(1, "[vfs] stat(%s) -> %s = %d\n", GSTR(harg(c,0)), host, r);
    free((void *)host);
    if (r == 0) marshal_stat(harg(c,1), &st);
    else guest_errno_set(c, errno);
    hret(c, (u32)r);
}
static void hle_fstat(cpu_t *c)
{
    struct stat st;
    int r = fstat((int)harg(c,0), &st);
    if (r == 0) marshal_stat(harg(c,1), &st);
    else guest_errno_set(c, errno);
    hret(c, (u32)r);
}
static void hle_mkdir(cpu_t *c)
{
    const char *host = vfs_xlate(GSTR(harg(c,0)), 1);
    vfs_mkdirs(host);
    int r = emu_mkdir(host, (int)harg(c,1));
    if (r) {                                       /* a directory only in the read-only data root exists too */
        char b[1200]; struct stat st;
        const char *base = vfs_base_path(GSTR(harg(c,0)), b, sizeof(b));
        int e = errno;
        if (e != EEXIST && base && !stat(base, &st)) e = EEXIST;
        guest_errno_set(c, e == EEXIST ? 17 : e == ENOENT ? 2 : e);
    }
    free((void *)host);
    hret(c, (u32)r);
}
static void hle_rmdir(cpu_t *c)
{
    const char *host = vfs_xlate(GSTR(harg(c,0)), 1);
    int r = rmdir(host);
    free((void *)host);
    hret(c, (u32)r);
}
static void hle_remove(cpu_t *c)
{
    const char *host = vfs_xlate(GSTR(harg(c,0)), 1);
    int r = remove(host);
    free((void *)host);
    hret(c, (u32)r);
}
static void hle_unlink(cpu_t *c)
{
    const char *host = vfs_xlate(GSTR(harg(c,0)), 1);
    int r = unlink(host);
    free((void *)host);
    hret(c, (u32)r);
}
static void hle_rename(cpu_t *c)
{
    const char *a = vfs_xlate(GSTR(harg(c,0)), 1);
    const char *b = vfs_xlate(GSTR(harg(c,1)), 1);
    int r = emu_rename(a, b);
    free((void *)a); free((void *)b);
    hret(c, (u32)r);
}
static void hle_chdir(cpu_t *c)
{
    pthread_mutex_lock(&vfs_lock);
    snprintf(g_cwd, sizeof(g_cwd), "%s", GSTR(harg(c,0)));
    pthread_mutex_unlock(&vfs_lock);
    hret(c, 0);
}
static void hle_getcwd(cpu_t *c)
{
    pthread_mutex_lock(&vfs_lock);
    snprintf(g2h(harg(c,0)), harg(c,1), "%s", g_cwd);
    pthread_mutex_unlock(&vfs_lock);
    hret(c, harg(c,0));
}
static void hle_chmod(cpu_t *c)  { hret(c, 0); }
static void hle_utime(cpu_t *c)  { hret(c, 0); }
static void hle_statfs(cpu_t *c)
{
    /* Bionic 32-bit struct statfs (84 bytes): type, bsize, u64 blocks/bfree/bavail/files/ffree,
     * fsid[2], namelen, frsize, flags, spare[4]. Fake a 64GB fs with 32GB free */
    gptr out = harg(c,1);
    memset(g2h(out), 0, 84);
    st32(out + 0, 0xEF53);          /* f_type */
    st32(out + 4, 4096);            /* f_bsize */
    st32(out + 8, 0x1000000);       /* f_blocks */
    st32(out + 16, 0x0800000);      /* f_bfree */
    st32(out + 24, 0x0800000);      /* f_bavail */
    st32(out + 32, 0x100000);       /* f_files */
    st32(out + 40, 0x80000);        /* f_ffree */
    st32(out + 56, 255);            /* f_namelen */
    st32(out + 60, 4096);           /* f_frsize */
    hret(c, 0);
}

/* directory reading: guest DIR* = index into table of host DIR* */
#define MAX_GDIRS 16
static DIR *g_dirs[MAX_GDIRS];
static char *g_dir_paths[MAX_GDIRS];      /* host path, for d_type where the host has none */
static int g_ndirs;

static void hle_opendir(cpu_t *c)
{
    const char *host = vfs_xlate(GSTR(harg(c,0)), 0);
    DIR *d = opendir(host);
    VLOG(1, "[vfs] opendir(%s) -> %s = %p\n", GSTR(harg(c,0)), host, (void *)d);
    if (!d) { free((void *)host); guest_errno_set(c, ENOENT); hret(c, 0); return; }
    int slot = -1;
    for (int i = 0; i < g_ndirs; i++) if (!g_dirs[i]) { slot = i; break; }
    if (slot < 0) {
        if (g_ndirs >= MAX_GDIRS) { LOG("[vfs] opendir: too many open directories\n"); free((void *)host); closedir(d); hret(c, 0); return; }
        slot = g_ndirs++;
    }
    g_dirs[slot] = d;
    g_dir_paths[slot] = (char *)host;
    hret(c, (u32)(slot + 1));   /* 1-based index */
}
static void hle_closedir(cpu_t *c)
{
    u32 id = harg(c,0);
    if (id >= 1 && id <= (u32)g_ndirs && g_dirs[id - 1]) {
        closedir(g_dirs[id - 1]);
        g_dirs[id - 1] = NULL;
        free(g_dir_paths[id - 1]);
        g_dir_paths[id - 1] = NULL;
    }
    hret(c, 0);
}
/* Bionic dirent (__DIRENT64_BODY, also on 32-bit): u64 d_ino; s64 d_off; u16 d_reclen; u8 d_type; char d_name[256] */
static u8 dirent_type(u32 id, const struct dirent *de)
{
#ifdef _WIN32
    char p[1200];
    struct stat st;
    snprintf(p, sizeof(p), "%s/%s", g_dir_paths[id - 1], de->d_name);
    if (stat(p, &st) != 0) return 0;                  /* DT_UNKNOWN */
    return S_ISDIR(st.st_mode) ? 4 : 8;               /* DT_DIR / DT_REG */
#else
    (void)id;
    return de->d_type;
#endif
}
static u32 marshal_dirent(gptr out, const struct dirent *de, u8 type)
{
    u32 reclen = 280;               /* 8 + 8 + 2 + 1 + 256, 8-aligned */
    memset(g2h(out), 0, reclen);
    st32(out + 0, (u32)de->d_ino);
    st32(out + 4, (u32)((u64)de->d_ino >> 32));
    st32(out + 8, 0); st32(out + 12, 0);          /* d_off */
    st16(out + 16, (u16)reclen);
    st8(out + 18, type);
    snprintf(g2h(out + 19), 256, "%s", de->d_name);
    return reclen;
}
static void hle_readdir(cpu_t *c)
{
    static pthread_mutex_t rd_lock = PTHREAD_MUTEX_INITIALIZER;
    static struct { gptr buf; u32 used; } ent;
    u32 id = harg(c,0);
    if (id < 1 || id > (u32)g_ndirs) { guest_errno_set(c, EBADF); hret(c, 0); return; }
    pthread_mutex_lock(&rd_lock);
    struct dirent *de = g_dirs[id - 1] ? readdir(g_dirs[id - 1]) : NULL;
    VLOG(2, "[vfs] readdir(%u) -> %s\n", id, de ? de->d_name : "(end)");
    if (!de) { pthread_mutex_unlock(&rd_lock); hret(c, 0); return; }
    if (!ent.buf) ent.buf = hle_data_alloc(512, 8);
    marshal_dirent(ent.buf, de, dirent_type(id, de));
    pthread_mutex_unlock(&rd_lock);
    hret(c, ent.buf);
}
static void hle_readdir_r(cpu_t *c)
{
    u32 id = harg(c,0);
    gptr buf = harg(c,1), out = harg(c,2);
    if (id < 1 || id > (u32)g_ndirs) { st32(out, 0); hret(c, (u32)EBADF); return; }
    static pthread_mutex_t rd2_lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&rd2_lock);
    struct dirent *de = g_dirs[id - 1] ? readdir(g_dirs[id - 1]) : NULL;
    VLOG(2, "[vfs] readdir_r(%u) -> %s\n", id, de ? de->d_name : "(end)");
    if (de) {
        marshal_dirent(buf, de, dirent_type(id, de));
        st32(out, buf);
    } else {
        st32(out, 0);
    }
    pthread_mutex_unlock(&rd2_lock);
    hret(c, 0);
}

/* ================================================================== */
/* time                                                               */
/* ================================================================== */

static u64 now_ns(cpu_t *c)
{
    if (G.fake_clock) return G.fake_now_ns;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

static void hle_gettimeofday(cpu_t *c)
{
    gptr tv = harg(c,0);
    if (tv) {
        u64 ns = now_ns(c);
        st32(tv, (u32)(ns / 1000000000ull));
        st32(tv + 4, (u32)((ns / 1000ull) % 1000000ull));
    }
    hret(c, 0);
}
static void hle_clock_gettime(cpu_t *c)
{
    gptr ts = harg(c,1);
    if (ts) {
        u64 ns = now_ns(c);
        st32(ts, (u32)(ns / 1000000000ull));
        st32(ts + 4, (u32)(ns % 1000000000ull));
    }
    hret(c, 0);
}
static void hle_time(cpu_t *c)
{
    u32 t = (u32)(now_ns(c) / 1000000000ull);
    gptr out = harg(c,0);
    if (out) st32(out, t);
    hret(c, t);
}
static void hle_clock(cpu_t *c)
{
    hret(c, (u32)(now_ns(c) / 1000ull));
}
static void hle_nanosleep(cpu_t *c)
{
    gptr req = harg(c,0);
    u64 ns = (u64)ld32(req) * 1000000000ull + ld32(req + 4);
    VLOG(2, "[sleep] %llu us from %08x\n", (unsigned long long)(ns / 1000), c->r[14]);
    if (!G.fake_clock) usleep(ns / 1000);
    hret(c, 0);
}
/* Bionic struct tm = glibc layout: 9 ints + long gmtoff + char* zone */
static void marshal_tm(gptr out, const struct tm *tm)
{
    st32(out + 0, (u32)tm->tm_sec);
    st32(out + 4, (u32)tm->tm_min);
    st32(out + 8, (u32)tm->tm_hour);
    st32(out + 12, (u32)tm->tm_mday);
    st32(out + 16, (u32)tm->tm_mon);
    st32(out + 20, (u32)tm->tm_year);
    st32(out + 24, (u32)tm->tm_wday);
    st32(out + 28, (u32)tm->tm_yday);
    st32(out + 32, (u32)tm->tm_isdst);
#ifdef _WIN32
    st32(out + 36, 0);
#else
    st32(out + 36, (u32)tm->tm_gmtoff);
#endif
    static gptr zone;
    if (!zone) { zone = hle_data_alloc(8, 4); strcpy(g2h(zone), "UTC"); }
    st32(out + 40, zone);
}
/* gmtime / localtime: one static struct tm per guest thread, like bionic's */
static gptr tm_buf(cpu_t *c)
{
    static __thread gptr b;
    (void)c;
    if (!b) b = hle_data_alloc(64, 8);
    return b;
}
static void hle_gmtime(cpu_t *c)
{
    time_t t = (time_t)(s32)ld32(harg(c,0));
    struct tm tmv;
    gmtime_r(&t, &tmv);
    gptr o = tm_buf(c);
    marshal_tm(o, &tmv);
    hret(c, o);
}
static void hle_localtime(cpu_t *c)
{
    time_t t = (time_t)(s32)ld32(harg(c,0));
    struct tm tmv;
    localtime_r(&t, &tmv);
    gptr o = tm_buf(c);
    marshal_tm(o, &tmv);
    hret(c, o);
}
static void hle_strftime(cpu_t *c)
{
    struct tm tmv;
    gptr gtm = harg(c,3);
    tmv.tm_sec = ld32(gtm + 0);  tmv.tm_min = ld32(gtm + 4);
    tmv.tm_hour = ld32(gtm + 8); tmv.tm_mday = ld32(gtm + 12);
    tmv.tm_mon = ld32(gtm + 16); tmv.tm_year = ld32(gtm + 20);
    tmv.tm_wday = ld32(gtm + 24); tmv.tm_yday = ld32(gtm + 28);
    tmv.tm_isdst = ld32(gtm + 32);
    hret(c, (u32)strftime(g2h(harg(c,0)), harg(c,1), GSTR(harg(c,2)), &tmv));
}

/* ================================================================== */
/* math (soft-fp ABI: f32 in r0, f64 in r0:r1 pairs)                  */
/* ================================================================== */

#define MATH_D1(name, fn) \
    static void hle_##name(cpu_t *c) { \
        u64 b; double r = fn(harg_d(c)); memcpy(&b, &r, 8); hret64(c, b); }
static double harg_d(cpu_t *c) { u64 b = c->r[0] | ((u64)c->r[1] << 32); double d; memcpy(&d, &b, 8); return d; }
#define MATH_F1(name, fn) \
    static void hle_##name(cpu_t *c) { \
        f32 a; u32 w = c->r[0]; memcpy(&a, &w, 4); f32 r = fn(a); u32 o; memcpy(&o, &r, 4); hret(c, o); }
#define MATH_D2(name, fn) \
    static void hle_##name(cpu_t *c) { \
        argwalk_t w; argwalk_init(&w, c); \
        u64 b1 = argwalk_u64(&w), b2 = argwalk_u64(&w); \
        double x, y, r; memcpy(&x, &b1, 8); memcpy(&y, &b2, 8); \
        r = fn(x, y); u64 b; memcpy(&b, &r, 8); hret64(c, b); }
#define MATH_F2(name, fn) \
    static void hle_##name(cpu_t *c) { \
        f32 x, y; memcpy(&x, &c->r[0], 4); memcpy(&y, &c->r[1], 4); \
        f32 r = fn(x, y); u32 o; memcpy(&o, &r, 4); hret(c, o); }

MATH_D1(acos, acos) MATH_D1(asin, asin) MATH_D1(atan, atan)
MATH_D1(cos, cos) MATH_D1(sin, sin) MATH_D1(tan, tan)
MATH_D1(exp, exp) MATH_D1(log, log) MATH_D1(log10, log10)
MATH_D1(sqrt, sqrt) MATH_D1(ceil, ceil) MATH_D1(floor, floor)
MATH_F1(acosf, acosf) MATH_F1(asinf, asinf) MATH_F1(atanf, atanf)
MATH_F1(cosf, cosf) MATH_F1(sinf, sinf) MATH_F1(tanf, tanf)
MATH_F1(expf, expf) MATH_F1(logf, logf) MATH_F1(log10f, log10f)
MATH_F1(sqrtf, sqrtf) MATH_F1(ceilf, ceilf) MATH_F1(floorf, floorf)
MATH_D2(atan2, atan2) MATH_F2(atan2f, atan2f)
MATH_D2(pow, pow) MATH_F2(powf, powf)
MATH_D2(fmod, fmod) MATH_F2(fmodf, fmodf)

static void hle_frexp(cpu_t *c)
{
    int e;
    double x = harg_d(c);
    double r = frexp(x, &e);
    gptr ep = harg(c, 2);
    if (ep) st32(ep, (u32)e);
    u64 b; memcpy(&b, &r, 8); hret64(c, b);
}
static void hle_ldexp(cpu_t *c)
{
    argwalk_t w; argwalk_init(&w, c);
    u64 b1 = argwalk_u64(&w);
    int e = (int)argwalk_u32(&w);
    double x; memcpy(&x, &b1, 8);
    double r = ldexp(x, e);
    u64 b; memcpy(&b, &r, 8); hret64(c, b);
}
static void hle_modf(cpu_t *c)
{
    double x = harg_d(c);
    double ip;
    double r = modf(x, &ip);
    gptr pp = harg(c, 2);
    if (pp) { u64 b; memcpy(&b, &ip, 8); st32(pp, (u32)b); st32(pp + 4, (u32)(b >> 32)); }
    u64 b2; memcpy(&b2, &r, 8); hret64(c, b2);
}
static void hle___isinf(cpu_t *c) { double x = harg_d(c); hret(c, isinf(x) ? 1 : 0); }

/* ================================================================== */
/* misc                                                               */
/* ================================================================== */

static void hle___errno(cpu_t *c) { hret(c, c->errno_addr); }

static void hle_abort(cpu_t *c)
{
    LOG("[hle] guest called abort() (tid %d)\n", c->tid);
    emu_trap(c, "guest abort");
}
static void hle_exit(cpu_t *c)
{
    LOG("[hle] guest exit(%d) (tid %d)\n", (int)harg(c,0), c->tid);
    exit((int)harg(c,0));
}
static void hle_raise(cpu_t *c)
{
    LOG("[hle] guest raise(%d)\n", (int)harg(c,0));
    emu_trap(c, "guest raise %d", (int)harg(c,0));
}

static void hle_getenv(cpu_t *c)
{
    const char *name = GSTR(harg(c,0));
    LOG_ONCE("[hle] getenv(%s) -> NULL (log once)\n", name);
    hret(c, 0);
}
static void hle_setenv(cpu_t *c) { hret(c, 0); }
static void hle_sysconf(cpu_t *c)
{
    switch ((int)harg(c,0)) {
    case 0x0027 /* _SC_PAGESIZE */: hret(c, 4096); return;
    case 0x0061 /* _SC_NPROCESSORS_ONLN */: hret(c, 4); return;
    case 0x0002 /* _SC_CLK_TCK */: hret(c, 100); return;
    case 0x0060 /* _SC_NPROCESSORS_CONF */: hret(c, 4); return;
    case 0x0062 /* _SC_PHYS_PAGES: 512 MB */: hret(c, 131072); return;
    case 0x0063 /* _SC_AVPHYS_PAGES: 256 MB free */: hret(c, 65536); return;
    default: LOG_ONCE("[hle] sysconf(%#x) unknown -> -1\n", harg(c,0)); hret(c, (u32)-1); return;
    }
}
static void hle_gethostname(cpu_t *c)
{
    snprintf(g2h(harg(c,0)), harg(c,1), "rr2emu");
    hret(c, 0);
}
static void hle_prctl(cpu_t *c)
{
    int op = (int)harg(c,0);
    if (op == 15 /* PR_SET_NAME */) {
        snprintf(c->name, sizeof(c->name), "%.15s", GSTR(harg(c,1)));
        VLOG(2, "[hle] thread %d named '%s'\n", c->tid, c->name);
    }
    hret(c, 0);
}
static void hle_system(cpu_t *c)  { LOG_ONCE("[hle] system() stubbed\n"); hret(c, (u32)-1); }
static void hle_fork(cpu_t *c)    { LOG_ONCE("[hle] fork() stubbed\n"); hret(c, (u32)-1); }
static void hle_execv(cpu_t *c)   { LOG_ONCE("[hle] execv() stubbed\n"); hret(c, (u32)-1); }
static void hle_waitpid(cpu_t *c) { hret(c, (u32)-1); }
static void hle_sched_yield(cpu_t *c) { sched_yield(); hret(c, 0); }

/* liblog */
static void hle_android_log_print(cpu_t *c)
{
    int prio = (int)harg(c,0);
    const char *tag = GSTR(harg(c,1));
    char buf[2048];
    gva_t w = { .c = c, .reg = 3, .stk = 0, .vlist = 0 };
    gfmt(buf, sizeof(buf), harg(c,2), &w);
    const char *p = "DVWE";
    fprintf(stderr, "[log/%c] %s: %s", prio >= 2 && prio <= 5 ? p[prio - 2] : '?', tag, buf);
    if (buf[0] && buf[strlen(buf) - 1] != '\n') fputc('\n', stderr);
    hret(c, 0);
}
static void hle_android_log_vprint(cpu_t *c)
{
    const char *tag = GSTR(harg(c,1));
    char buf[2048];
    gva_t w = { .c = c, .vlist = harg(c,3) };
    gfmt(buf, sizeof(buf), harg(c,2), &w);
    fprintf(stderr, "[log] %s: %s", tag, buf);
    if (buf[0] && buf[strlen(buf) - 1] != '\n') fputc('\n', stderr);
    hret(c, 0);
}
static void hle_android_log_write(cpu_t *c)
{
    fprintf(stderr, "[log] %s", GSTR(harg(c,2)));
    hret(c, 0);
}

/* ================================================================== */
/* data objects                                                       */
/* ================================================================== */

gptr hle_data_object(const char *name)
{
    static gptr sf, guard, ctype, tolower_tab, toupper_tab, tzname_o, page_size_o, timezone_o, environ_o, stdio_o[3];
    static bool inited;
    if (!inited) {
        inited = true;
        /* __sF: three fake FILE structs */
        sf = hle_data_alloc(3 * 256, 16);
        g_sf[0] = sf; g_sf[1] = sf + 256; g_sf[2] = sf + 512;
        guard = hle_data_alloc(4, 4);
        st32(guard, 0xC0FFEE11u);
        /* Bionic: `const char *_ctype_`, `const short *_tolower_tab_/_toupper_tab_` are
         * pointer variables; tables are indexed c+1 (slot 0 = EOF), case tables are shorts */
        gptr ct_tab = hle_data_alloc(257, 4);
        u8 *ct = g2h(ct_tab);
        ct[0] = 0;
        for (int i = 0; i < 256; i++) {
            u8 fl = 0;
            if (i >= 'A' && i <= 'Z') fl |= 0x01;
            if (i >= 'a' && i <= 'z') fl |= 0x02;
            if (i >= '0' && i <= '9') fl |= 0x04;
            if (i == ' ' || (i >= 9 && i <= 13)) fl |= 0x08;
            if (i < 128 && ispunct(i)) fl |= 0x10;
            if (i < 32 || i == 127) fl |= 0x20;
            if ((i >= '0' && i <= '9') || (i >= 'a' && i <= 'f') || (i >= 'A' && i <= 'F')) fl |= 0x40;
            if (i == ' ') fl |= 0x80;
            ct[i + 1] = fl;
        }
        gptr lo_tab = hle_data_alloc(257 * 2, 4), up_tab = hle_data_alloc(257 * 2, 4);
        st16(lo_tab, (u16)-1); st16(up_tab, (u16)-1);
        for (int i = 0; i < 256; i++) {
            st16(lo_tab + 2 * (i + 1), (u16)(i < 128 ? tolower(i) : i));
            st16(up_tab + 2 * (i + 1), (u16)(i < 128 ? toupper(i) : i));
        }
        ctype = hle_data_alloc(4, 4);       st32(ctype, ct_tab);
        tolower_tab = hle_data_alloc(4, 4); st32(tolower_tab, lo_tab);
        toupper_tab = hle_data_alloc(4, 4); st32(toupper_tab, up_tab);
        gptr tz1 = hle_data_alloc(8, 4), tz2 = hle_data_alloc(8, 4);
        strcpy(g2h(tz1), "UTC"); strcpy(g2h(tz2), "UTC");
        tzname_o = hle_data_alloc(8, 4);
        st32(tzname_o, tz1); st32(tzname_o + 4, tz2);
        page_size_o = hle_data_alloc(4, 4);
        st32(page_size_o, 4096);
        timezone_o = hle_data_alloc(4, 4);
        /* `char **environ`: an empty environment, matching getenv (always NULL) */
        gptr env = hle_data_alloc(4, 4);
        environ_o = hle_data_alloc(4, 4); st32(environ_o, env);
        /* API 23+ Bionic: `FILE *stdin/stdout/stderr` variables pointing into __sF */
        for (int i = 0; i < 3; i++) { stdio_o[i] = hle_data_alloc(4, 4); st32(stdio_o[i], g_sf[i]); }
    }
    if (!strcmp(name, "environ")) return environ_o;
    if (!strcmp(name, "stdin")) return stdio_o[0];
    if (!strcmp(name, "stdout")) return stdio_o[1];
    if (!strcmp(name, "stderr")) return stdio_o[2];
    if (!strcmp(name, "timezone")) return timezone_o;
    if (!strcmp(name, "__sF")) return sf;
    if (!strcmp(name, "__stack_chk_guard")) return guard;
    if (!strcmp(name, "_ctype_")) return ctype;
    if (!strcmp(name, "_tolower_tab_")) return tolower_tab;
    if (!strcmp(name, "_toupper_tab_")) return toupper_tab;
    if (!strcmp(name, "tzname")) return tzname_o;
    if (!strcmp(name, "__page_size")) return page_size_o;
    return 0;
}

/* ================================================================== */
/* registration                                                       */
/* ================================================================== */

/* ================================================================== */
/* network: offline. Every socket op fails cleanly so retry loops end */
/* ================================================================== */

static void net_fail(cpu_t *c, int e) { guest_errno_set(c, e); hret(c, (u32)-1); }
static void hle_socket(cpu_t *c)   { net_fail(c, 97 /* EAFNOSUPPORT */); }
static void hle_connect(cpu_t *c)  { net_fail(c, 101 /* ENETUNREACH */); }
static void hle_sockop(cpu_t *c)   { net_fail(c, 88 /* ENOTSOCK */); }
static void hle_gethostbyname(cpu_t *c) { hret(c, 0); }
static void hle_inet_addr(cpu_t *c)
{
    unsigned a, b, d, e;
    if (sscanf(GSTR(harg(c, 0)), "%u.%u.%u.%u", &a, &b, &d, &e) != 4 || (a | b | d | e) > 255) {
        hret(c, 0xFFFFFFFFu);
        return;
    }
    hret(c, a | (b << 8) | (d << 16) | (e << 24));   /* network byte order on little-endian */
}

/* POSIX rand48: x' = (0x5DEECE66D * x + 0xB) mod 2^48, shared state like Bionic */
static u64 rand48_x = 0x1234ABCD330EULL;
static pthread_mutex_t rand48_lock = PTHREAD_MUTEX_INITIALIZER;
static u64 rand48_next(void)
{
    pthread_mutex_lock(&rand48_lock);
    rand48_x = (0x5DEECE66DULL * rand48_x + 0xB) & 0xFFFFFFFFFFFFULL;
    u64 v = rand48_x;
    pthread_mutex_unlock(&rand48_lock);
    return v;
}
static void hle_srand48(cpu_t *c) { rand48_x = ((u64)harg(c, 0) << 16) | 0x330E; }
static void hle_lrand48(cpu_t *c) { hret(c, (u32)(rand48_next() >> 17)); }
static void hle_mrand48(cpu_t *c) { hret(c, (u32)(rand48_next() >> 16)); }
static void hle_drand48(cpu_t *c)
{
    f64 d = (f64)rand48_next() / 281474976710656.0;
    u64 b; memcpy(&b, &d, 8);
    hret64(c, b);
}

#include "hle_libc2.inc"
#include "hle_libc3.inc"

void libc_init(void)
{
    hle_register("srand48", hle_srand48);
    hle_register("lrand48", hle_lrand48);
    hle_register("mrand48", hle_mrand48);
    hle_register("drand48", hle_drand48);
    hle_register("socket", hle_socket);
    hle_register("connect", hle_connect);
    static const char *const sockops[] = { "bind", "listen", "accept", "send", "sendto", "recv",
        "recvfrom", "setsockopt", "getsockopt", "getsockname", "getpeername", "shutdown" };
    for (unsigned i = 0; i < sizeof(sockops) / sizeof(sockops[0]); i++) hle_register(sockops[i], hle_sockop);
    hle_register("gethostbyname", hle_gethostbyname);
    hle_register("inet_addr", hle_inet_addr);

    malloc_hle_init();

    hle_register("memcpy", hle_memcpy);
    hle_register("memmove", hle_memmove);
    hle_register("memset", hle_memset);
    hle_register("memcmp", hle_memcmp);
    hle_register("bcopy", hle_bcopy);
    hle_register("strlen", hle_strlen);
    hle_register("strcmp", hle_strcmp);
    hle_register("strncmp", hle_strncmp);
    hle_register("strcasecmp", hle_strcasecmp);
    hle_register("strcpy", hle_strcpy);
    hle_register("strncpy", hle_strncpy);
    hle_register("strcat", hle_strcat);
    hle_register("strchr", hle_strchr);
    hle_register("strrchr", hle_strrchr);
    hle_register("strstr", hle_strstr);
    hle_register("strtok", hle_strtok);
    hle_register("atoi", hle_atoi);
    hle_register("strtoul", hle_strtoul);
    hle_register("strtod", hle_strtod);
    hle_register("qsort", hle_qsort);

    hle_register("fopen", hle_fopen);
    hle_register("fclose", hle_fclose);
    hle_register("fread", hle_fread);
    hle_register("fwrite", hle_fwrite);
    hle_register("fseek", hle_fseek);
    hle_register("ftell", hle_ftell);
    hle_register("fflush", hle_fflush);
    hle_register("fgetc", hle_fgetc);
    hle_register("fgets", hle_fgets);
    hle_register("fputc", hle_fputc);
    hle_register("fputs", hle_fputs);
    hle_register("ungetc", hle_ungetc);
    hle_register("putchar", hle_putchar);
    hle_register("perror", hle_perror);
    hle_register("fwide", hle_fwide);
    hle_register("fsync", hle_fsync);
    hle_register("ftruncate", hle_ftruncate);
    hle_register("sprintf", hle_sprintf);
    hle_register("snprintf", hle_snprintf);
    hle_register("vsprintf", hle_vsprintf);
    hle_register("vsnprintf", hle_vsnprintf);
    hle_register("fprintf", hle_fprintf);
    hle_register("vfprintf", hle_vfprintf);
    hle_register("vprintf", hle_vprintf);
    hle_register("sscanf", hle_sscanf);
    hle_register("fscanf", hle_fscanf);

    hle_register("open", hle_open);
    hle_register("__open_2", hle_open);                  /* FORTIFY open without a mode */
    hle_register("close", hle_close);
    hle_register("read", hle_read);
    hle_register("write", hle_write);
    hle_register("lseek", hle_lseek);
    hle_register("fcntl", hle_fcntl);
    hle_register("ioctl", hle_ioctl);
    hle_register("poll", hle_poll);

    hle_register("stat", hle_stat);
    hle_register("fstat", hle_fstat);
    hle_register("mkdir", hle_mkdir);
    hle_register("rmdir", hle_rmdir);
    hle_register("remove", hle_remove);
    hle_register("unlink", hle_unlink);
    hle_register("rename", hle_rename);
    hle_register("chdir", hle_chdir);
    hle_register("getcwd", hle_getcwd);
    hle_register("chmod", hle_chmod);
    hle_register("utime", hle_utime);
    hle_register("statfs", hle_statfs);
    hle_register("opendir", hle_opendir);
    hle_register("closedir", hle_closedir);
    hle_register("readdir", hle_readdir);
    hle_register("readdir_r", hle_readdir_r);

    hle_register("gettimeofday", hle_gettimeofday);
    hle_register("clock_gettime", hle_clock_gettime);
    hle_register("time", hle_time);
    hle_register("clock", hle_clock);
    hle_register("nanosleep", hle_nanosleep);
    hle_register("gmtime", hle_gmtime);
    hle_register("localtime", hle_localtime);
    hle_register("strftime", hle_strftime);

    hle_register("acos", hle_acos); hle_register("asin", hle_asin);
    hle_register("atan", hle_atan); hle_register("cos", hle_cos);
    hle_register("sin", hle_sin); hle_register("tan", hle_tan);
    hle_register("exp", hle_exp); hle_register("log", hle_log);
    hle_register("log10", hle_log10); hle_register("sqrt", hle_sqrt);
    hle_register("ceil", hle_ceil); hle_register("floor", hle_floor);
    hle_register("acosf", hle_acosf); hle_register("asinf", hle_asinf);
    hle_register("atanf", hle_atanf); hle_register("cosf", hle_cosf);
    hle_register("sinf", hle_sinf); hle_register("tanf", hle_tanf);
    hle_register("expf", hle_expf); hle_register("logf", hle_logf);
    hle_register("log10f", hle_log10f); hle_register("sqrtf", hle_sqrtf);
    hle_register("ceilf", hle_ceilf); hle_register("floorf", hle_floorf);
    hle_register("atan2", hle_atan2); hle_register("atan2f", hle_atan2f);
    hle_register("pow", hle_pow); hle_register("powf", hle_powf);
    hle_register("fmod", hle_fmod); hle_register("fmodf", hle_fmodf);
    hle_register("frexp", hle_frexp);
    hle_register("ldexp", hle_ldexp);
    hle_register("modf", hle_modf);
    hle_register("__isinf", hle___isinf);

    hle_register("__errno", hle___errno);
    hle_register("abort", hle_abort);
    hle_register("exit", hle_exit);
    hle_register("raise", hle_raise);
    hle_register("getenv", hle_getenv);
    hle_register("setenv", hle_setenv);
    hle_register("sysconf", hle_sysconf);
    hle_register("gethostname", hle_gethostname);
    hle_register("prctl", hle_prctl);
    hle_register("system", hle_system);
    hle_register("fork", hle_fork);
    hle_register("execv", hle_execv);
    hle_register("waitpid", hle_waitpid);
    hle_register("sched_yield", hle_sched_yield);

    hle_register("__android_log_print", hle_android_log_print);
    hle_register("__android_log_vprint", hle_android_log_vprint);
    hle_register("__android_log_write", hle_android_log_write);
    libc2_init();
    libc3_init();
}
