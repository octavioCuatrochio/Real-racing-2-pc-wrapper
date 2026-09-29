/*
 * hle_libc2.inc - the rest of Bionic libc/libm/libdl + zlib + EGL queries that
 * RR3's libraries import. Included by hle_libc.c (shares its file table, the
 * printf/scanf engines and the VFS path translation).
 */
#include <dlfcn.h>

/* ---- AEABI memory helpers (note __aeabi_memset's (dest, n, c) order) ---- */
static void hle_aeabi_memcpy(cpu_t *c)  { memcpy(g2h(harg(c,0)), g2h(harg(c,1)), harg(c,2)); }
static void hle_aeabi_memmove(cpu_t *c) { memmove(g2h(harg(c,0)), g2h(harg(c,1)), harg(c,2)); }
static void hle_aeabi_memset(cpu_t *c)  { memset(g2h(harg(c,0)), (int)harg(c,2), harg(c,1)); }
static void hle_aeabi_memclr(cpu_t *c)  { memset(g2h(harg(c,0)), 0, harg(c,1)); }

/* ---- strings ---- */
static void hle_strdup(cpu_t *c)
{
    const char *s = GSTR(harg(c,0));
    size_t n = strlen(s) + 1;
    gptr p = guest_malloc((u32)n);
    if (p) memcpy(g2h(p), s, n);
    hret(c, p);
}
static void hle_memchr(cpu_t *c)
{
    const u8 *s = g2h(harg(c,0));
    const u8 *r = memchr(s, (int)(harg(c,1) & 0xFF), harg(c,2));
    hret(c, r ? harg(c,0) + (u32)(r - s) : 0);
}
static void hle_memrchr(cpu_t *c)
{
    const u8 *s = g2h(harg(c,0));
    u32 n = harg(c,2); u8 ch = (u8)harg(c,1);
    while (n--) if (s[n] == ch) { hret(c, harg(c,0) + n); return; }
    hret(c, 0);
}
static void hle_memmem(cpu_t *c)
{
    const u8 *h = g2h(harg(c,0)), *nd = g2h(harg(c,2));
    u32 hn = harg(c,1), nn = harg(c,3);
    if (!nn) { hret(c, harg(c,0)); return; }
    for (u32 i = 0; i + nn <= hn; i++) if (h[i] == nd[0] && !memcmp(h + i, nd, nn)) { hret(c, harg(c,0) + i); return; }
    hret(c, 0);
}
static void hle_strnlen(cpu_t *c) { hret(c, (u32)strnlen(GSTR(harg(c,0)), harg(c,1))); }
static void hle_strspn(cpu_t *c)  { hret(c, (u32)strspn(GSTR(harg(c,0)), GSTR(harg(c,1)))); }
static void hle_strcspn2(cpu_t *c) { hret(c, (u32)strcspn(GSTR(harg(c,0)), GSTR(harg(c,1)))); }
static void hle_strpbrk(cpu_t *c)
{
    const char *s = GSTR(harg(c,0));
    const char *r = strpbrk(s, GSTR(harg(c,1)));
    hret(c, r ? harg(c,0) + (u32)(r - s) : 0);
}
static void hle_strtok_r(cpu_t *c)
{
    gptr s = harg(c,0), delim = harg(c,1), save = harg(c,2);
    if (!s) s = ld32(save);
    if (!s) { hret(c, 0); return; }
    const char *d = GSTR(delim);
    s += (u32)strspn(GSTR(s), d);
    if (!ld8(s)) { st32(save, s); hret(c, 0); return; }
    gptr e = s + (u32)strcspn(GSTR(s), d);
    if (ld8(e)) { st8(e, 0); st32(save, e + 1); } else st32(save, e);
    hret(c, s);
}
static void hle_strncasecmp(cpu_t *c) { hret(c, (u32)strncasecmp(GSTR(harg(c,0)), GSTR(harg(c,1)), harg(c,2))); }
static void hle_strcoll(cpu_t *c) { hret(c, (u32)strcmp(GSTR(harg(c,0)), GSTR(harg(c,1)))); }
static void hle_strxfrm(cpu_t *c)
{
    const char *s = GSTR(harg(c,1));
    u32 n = (u32)strlen(s);
    if (harg(c,2) > n) memcpy(g2h(harg(c,0)), s, n + 1);
    hret(c, n);
}
static void hle_wcscoll(cpu_t *c)                   /* Android wchar_t is 32-bit */
{
    gptr a = harg(c,0), b = harg(c,1);
    for (;; a += 4, b += 4) {
        u32 x = ld32(a), y = ld32(b);
        if (x != y || !x) { hret(c, x < y ? (u32)-1 : x > y); return; }
    }
}
static void hle_wcsxfrm(cpu_t *c)
{
    gptr d = harg(c,0), s = harg(c,1); u32 n = harg(c,2), len = 0;
    while (ld32(s + 4 * len)) len++;
    if (n > len) memcpy(g2h(d), g2h(s), 4 * (len + 1));
    hret(c, len);
}
static gptr g_strbuf;
static gptr strbuf(void) { if (!g_strbuf) g_strbuf = hle_data_alloc(4096, 16); return g_strbuf; }
static void hle_strerror(cpu_t *c)
{
    gptr b = strbuf() + 2048;
    snprintf(g2h(b), 256, "%s", strerror((int)harg(c,0)));
    hret(c, b);
}
static void hle_strerror_r(cpu_t *c)
{
    if (harg(c,2)) snprintf(g2h(harg(c,1)), harg(c,2), "%s", strerror((int)harg(c,0)));
    hret(c, 0);
}
static void hle_basename(cpu_t *c)
{
    const char *s = harg(c,0) ? GSTR(harg(c,0)) : "";
    const char *e = s + strlen(s);
    while (e > s + 1 && e[-1] == '/') e--;
    const char *b = e;
    while (b > s && b[-1] != '/') b--;
    gptr out = strbuf() + 1024;
    if (e == s) strcpy(g2h(out), ".");
    else snprintf(g2h(out), 1024, "%.*s", (int)(e - b), b);
    hret(c, out);
}
static void hle_setlocale(cpu_t *c)
{
    gptr b = strbuf() + 3072;
    strcpy(g2h(b), "C");
    hret(c, b);
}

/* ---- ctype (C locale) ---- */
#define CTYPE_FN(n) static void hle_##n(cpu_t *c) { int v = (int)harg(c,0); hret(c, (v >= -1 && v < 256 && v != -1) ? (n(v) != 0) : 0); }
CTYPE_FN(isalpha) CTYPE_FN(isalnum) CTYPE_FN(isprint) CTYPE_FN(isupper) CTYPE_FN(isspace) CTYPE_FN(isxdigit)
static void hle_isblank(cpu_t *c) { int v = (int)harg(c,0); hret(c, v == ' ' || v == '\t'); }
static void hle_toupper(cpu_t *c) { int v = (int)harg(c,0); hret(c, (u32)(v >= 'a' && v <= 'z' ? v - 32 : v)); }
static void hle_tolower(cpu_t *c) { int v = (int)harg(c,0); hret(c, (u32)(v >= 'A' && v <= 'Z' ? v + 32 : v)); }

/* ---- number parsing ---- */
static void strto_end(cpu_t *c, gptr endp, const char *s, const char *e) { if (endp) st32(endp, harg(c,0) + (u32)(e - s)); }
static void hle_atol(cpu_t *c) { hret(c, (u32)atol(GSTR(harg(c,0)))); }
static void hle_strtol(cpu_t *c)
{
    const char *s = GSTR(harg(c,0)); char *e;
    long long v = strtoll(s, &e, (int)harg(c,2));
    if (v > 0x7FFFFFFFLL) v = 0x7FFFFFFFLL; else if (v < -0x80000000LL) v = -0x80000000LL;
    strto_end(c, harg(c,1), s, e);
    hret(c, (u32)v);
}
static void hle_strtoll(cpu_t *c)
{
    const char *s = GSTR(harg(c,0)); char *e;
    long long v = strtoll(s, &e, (int)harg(c,2));
    strto_end(c, harg(c,1), s, e);
    hret64(c, (u64)v);
}
static void hle_strtoull(cpu_t *c)
{
    const char *s = GSTR(harg(c,0)); char *e;
    unsigned long long v = strtoull(s, &e, (int)harg(c,2));
    strto_end(c, harg(c,1), s, e);
    hret64(c, (u64)v);
}

/* ---- stdio extras ---- */
static void hle_puts(cpu_t *c) { hret(c, (u32)puts(GSTR(harg(c,0)))); }
static void hle_fseeko(cpu_t *c) { FILE *f = file_for(harg(c,0)); hret(c, f ? (u32)fseeko(f, (off_t)(s32)harg(c,1), (int)harg(c,2)) : (u32)-1); }
static void hle_ftello(cpu_t *c) { FILE *f = file_for(harg(c,0)); hret(c, f ? (u32)ftello(f) : (u32)-1); }
static void hle_feof(cpu_t *c)   { FILE *f = file_for(harg(c,0)); hret(c, f ? (u32)feof(f) : 1); }
static void hle_ferror(cpu_t *c) { FILE *f = file_for(harg(c,0)); hret(c, f ? (u32)ferror(f) : 1); }
static void hle_rewind(cpu_t *c) { FILE *f = file_for(harg(c,0)); if (f) rewind(f); }
static void hle_getc(cpu_t *c)   { FILE *f = file_for(harg(c,0)); hret(c, f ? (u32)fgetc(f) : (u32)EOF); }
static void hle_fileno(cpu_t *c) { FILE *f = file_for(harg(c,0)); hret(c, f ? (u32)fileno(f) : (u32)-1); }
static void hle_setvbuf(cpu_t *c) { hret(c, 0); }
static void hle_fputwc(cpu_t *c) { FILE *f = file_for(harg(c,1)); if (f && harg(c,0) < 128) fputc((int)harg(c,0), f); hret(c, harg(c,0)); }
static void hle_fdopen(cpu_t *c)
{
    const char *mode = GSTR(harg(c,1));
    char hmode[8];
    snprintf(hmode, sizeof(hmode), "%.6s%s", mode, strchr(mode, 'b') ? "" : "b");
    FILE *f = fdopen((int)harg(c,0), hmode);
    hret(c, f ? file_register(f) : 0);
}
static void hle_vasprintf(cpu_t *c)
{
    static __thread char buf[65536];
    gva_t w = { .c = c, .vlist = harg(c,2) };
    size_t n = gfmt(buf, sizeof(buf), harg(c,1), &w);
    if (n >= sizeof(buf)) n = sizeof(buf) - 1;
    gptr p = guest_malloc((u32)n + 1);
    if (p) { memcpy(g2h(p), buf, n); st8(p + (u32)n, 0); }
    st32(harg(c,0), p);
    hret(c, p ? (u32)n : (u32)-1);
}
static void hle_vsscanf(cpu_t *c)
{
    scan_src src = { .c = c, .p = GSTR(harg(c,0)), .f = NULL };
    gva_t w = { .c = c, .vlist = harg(c,2) };
    hret(c, (u32)gscan(c, &src, harg(c,1), &w));
}
static void hle_access(cpu_t *c)
{
    const char *host = vfs_xlate(GSTR(harg(c,0)), 0);
    int r = access(host, F_OK);
    VLOG(1, "[vfs] access(%s) -> %s = %d\n", GSTR(harg(c,0)), host, r);
    free((void *)host);
    if (r) guest_errno_set(c, ENOENT);
    hret(c, (u32)r);
}

/* ---- time ---- */
static void unmarshal_tm(gptr in, struct tm *tm)
{
    memset(tm, 0, sizeof(*tm));
    tm->tm_sec = (int)ld32(in); tm->tm_min = (int)ld32(in + 4); tm->tm_hour = (int)ld32(in + 8);
    tm->tm_mday = (int)ld32(in + 12); tm->tm_mon = (int)ld32(in + 16); tm->tm_year = (int)ld32(in + 20);
    tm->tm_wday = (int)ld32(in + 24); tm->tm_yday = (int)ld32(in + 28); tm->tm_isdst = (int)ld32(in + 32);
}
static void hle_mktime(cpu_t *c)
{
    struct tm tm;
    unmarshal_tm(harg(c,0), &tm);
    time_t t = mktime(&tm);
    marshal_tm(harg(c,0), &tm);
    hret(c, (u32)t);
}
static void hle_localtime_r(cpu_t *c)
{
    time_t t = (time_t)(s32)ld32(harg(c,0));
    struct tm tm;
#ifdef _WIN32
    struct tm *p = localtime(&t); if (p) tm = *p; else memset(&tm, 0, sizeof(tm));
#else
    localtime_r(&t, &tm);
#endif
    marshal_tm(harg(c,1), &tm);
    hret(c, harg(c,1));
}
static void hle_gmtime_r(cpu_t *c)
{
    time_t t = (time_t)(s32)ld32(harg(c,0));
    struct tm tm;
#ifdef _WIN32
    struct tm *p = gmtime(&t); if (p) tm = *p; else memset(&tm, 0, sizeof(tm));
#else
    gmtime_r(&t, &tm);
#endif
    marshal_tm(harg(c,1), &tm);
    hret(c, harg(c,1));
}
static void hle_difftime(cpu_t *c)
{
    double r = (double)(s32)harg(c,0) - (double)(s32)harg(c,1);
    u64 b; memcpy(&b, &r, 8); hret64(c, b);
}
static void hle_usleep(cpu_t *c) { if (!G.fake_clock) usleep(harg(c,0)); hret(c, 0); }

/* ---- math ---- */
MATH_D1(cosh, cosh) MATH_D1(sinh, sinh) MATH_D1(tanh, tanh) MATH_D1(acosh, acosh) MATH_D1(asinh, asinh)
MATH_D1(atanh, atanh) MATH_D1(cbrt, cbrt) MATH_D1(erf, erf) MATH_D1(erfc, erfc) MATH_D1(expm1, expm1)
MATH_D1(lgamma, lgamma) MATH_D1(log1p, log1p) MATH_D1(logb, logb) MATH_D1(nearbyint, nearbyint)
MATH_D1(rint, rint) MATH_D1(tgamma, tgamma) MATH_D1(exp2, exp2) MATH_D1(round, round) MATH_D1(trunc, trunc)
MATH_D1(log2, log2) MATH_F1(roundf, roundf) MATH_F1(exp2f, exp2f) MATH_F1(truncf, truncf) MATH_F1(log2f, log2f)
MATH_F1(cbrtf, cbrtf) MATH_F1(tanhf, tanhf) MATH_F1(coshf, coshf) MATH_F1(sinhf, sinhf) MATH_F1(fabsf, fabsf)
MATH_F1(rintf, rintf) MATH_F1(nearbyintf, nearbyintf) MATH_F1(log1pf, log1pf) MATH_F1(expm1f, expm1f)
MATH_D2(hypot, hypot) MATH_D2(remainder, remainder) MATH_D2(nextafter, nextafter) MATH_D2(fmax, fmax) MATH_D2(fmin, fmin)
MATH_F2(nextafterf, nextafterf) MATH_F2(hypotf, hypotf) MATH_F2(fmaxf, fmaxf) MATH_F2(fminf, fminf) MATH_F2(remainderf, remainderf)
MATH_D1(fabs, fabs)
static void hle_modff(cpu_t *c)
{
    f32 x; memcpy(&x, &c->r[0], 4);
    f32 ip, r = modff(x, &ip);
    if (harg(c,1)) { u32 b; memcpy(&b, &ip, 4); st32(harg(c,1), b); }
    u32 o; memcpy(&o, &r, 4); hret(c, o);
}
static void hle_lrintf(cpu_t *c) { f32 x; memcpy(&x, &c->r[0], 4); hret(c, (u32)(s32)lrintf(x)); }
static void hle_lrint(cpu_t *c)  { hret(c, (u32)(s32)lrint(harg_d(c))); }
static void hle_llrint(cpu_t *c) { hret64(c, (u64)llrint(harg_d(c))); }
static void hle_scalbn(cpu_t *c)
{
    double r = scalbn(harg_d(c), (int)harg(c,2));
    u64 b; memcpy(&b, &r, 8); hret64(c, b);
}
static void hle_remquo(cpu_t *c)
{
    argwalk_t w; argwalk_init(&w, c);
    u64 b1 = argwalk_u64(&w), b2 = argwalk_u64(&w);
    gptr qp = argwalk_u32(&w);
    double x, y; memcpy(&x, &b1, 8); memcpy(&y, &b2, 8);
    int q; double r = remquo(x, y, &q);
    if (qp) st32(qp, (u32)q);
    u64 b; memcpy(&b, &r, 8); hret64(c, b);
}
static void hle_fpclassifyd(cpu_t *c)
{
    switch (fpclassify(harg_d(c))) {                  /* Bionic's FP_* values */
    case FP_INFINITE: hret(c, 0x01); break; case FP_NAN: hret(c, 0x02); break;
    case FP_NORMAL: hret(c, 0x04); break; case FP_SUBNORMAL: hret(c, 0x08); break; default: hret(c, 0x10);
    }
}
static void hle_signbitd(cpu_t *c) { hret(c, (c->r[1] >> 31) & 1); }
static void hle_isfinited(cpu_t *c) { hret(c, isfinite(harg_d(c)) ? 1 : 0); }
static void hle_isnand(cpu_t *c) { hret(c, isnan(harg_d(c)) ? 1 : 0); }

/* ---- process / identity / signals (single-process fiction) ---- */
static void hle_ret0(cpu_t *c) { hret(c, 0); }
static void hle_retm1(cpu_t *c) { guest_errno_set(c, 38 /* ENOSYS */); hret(c, (u32)-1); }
static void hle_getpid(cpu_t *c) { hret(c, 4242); }
static void hle_getuid(cpu_t *c) { hret(c, 10127); }
static void hle_getpwuid_r(cpu_t *c) { if (harg(c,4)) st32(harg(c,4), 0); hret(c, 2 /* ENOENT */); }
static void hle_dup2(cpu_t *c) { hret(c, (u32)dup2((int)harg(c,0), (int)harg(c,1))); }
/* only ever reached for sockets, which never exist offline: wait out the timeout (at most 1 s), nothing ready.
 * Returning at once made fmRUDP's network thread spin a whole core. */
static void hle_select(cpu_t *c)
{
    for (int i = 1; i <= 3; i++) if (harg(c,i)) memset(g2h(harg(c,i)), 0, 128);
    gptr tv = harg(c,4);
    u64 us = tv ? (u64)ld32(tv) * 1000000 + ld32(tv + 4) : 1000000;
    if (us > 1000000) us = 1000000;
    if (us) usleep((useconds_t)us);
    hret(c, 0);
}

/* ---- named semaphores: in-process only ---- */
static struct { char name[64]; gptr sem; } g_named_sems[16];
static void hle_sem_open(cpu_t *c)
{
    const char *n = GSTR(harg(c,0));
    for (int i = 0; i < 16; i++) if (g_named_sems[i].sem && !strcmp(g_named_sems[i].name, n)) { hret(c, g_named_sems[i].sem); return; }
    u32 val = (harg(c,1) & 0x40 /* O_CREAT */) ? harg(c,3) : 0;
    for (int i = 0; i < 16; i++)
        if (!g_named_sems[i].sem) {
            gptr s = hle_data_alloc(16, 4);
            snprintf(g_named_sems[i].name, sizeof(g_named_sems[i].name), "%s", n);
            g_named_sems[i].sem = s;
            extern void hle_sem_init_at(gptr addr, u32 value);
            hle_sem_init_at(s, val);
            hret(c, s);
            return;
        }
    hret(c, 0);
}
static void hle_sem_unlink(cpu_t *c)
{
    const char *n = GSTR(harg(c,0));
    for (int i = 0; i < 16; i++) if (g_named_sems[i].sem && !strcmp(g_named_sems[i].name, n)) g_named_sems[i].name[0] = 1;
    hret(c, 0);
}

/* ---- network helpers (offline) ---- */
static void hle_getaddrinfo(cpu_t *c) { if (harg(c,3)) st32(harg(c,3), 0); hret(c, 8 /* EAI_NONAME */); }
static void hle_inet_pton(cpu_t *c)                     /* IPv4 only (offline anyway) */
{
    unsigned v[4]; char tail;
    if ((int)harg(c,0) != 2 || sscanf(GSTR(harg(c,1)), "%u.%u.%u.%u%c", &v[0], &v[1], &v[2], &v[3], &tail) != 4 ||
        v[0] > 255 || v[1] > 255 || v[2] > 255 || v[3] > 255) { hret(c, 0); return; }
    for (int i = 0; i < 4; i++) st8(harg(c,2) + (u32)i, (u8)v[i]);
    hret(c, 1);
}
static void hle_inet_ntop(cpu_t *c)
{
    if ((int)harg(c,0) != 2 || harg(c,3) < 16) { hret(c, 0); return; }
    const u8 *a = g2h(harg(c,1));
    snprintf(g2h(harg(c,2)), 16, "%u.%u.%u.%u", a[0], a[1], a[2], a[3]);
    hret(c, harg(c,2));
}

/* ---- libdl ---- */
#define DL_HANDLE(i) (0xD1000000u + (u32)(i))
static const char *const dl_names[] = { "libRealRacing3.so", "libNimble.so", "libfmodex.so", "libc++_shared.so",
                                        "libc.so", "libm.so", "libGLESv2.so", "libGLESv3.so", "libEGL.so", "libdl.so", "liblog.so",
                                        "libOpenSLES.so" };
#define DL_OPENSL DL_HANDLE(11)
u32 opensl_dlsym(const char *name);
static __thread char dl_err[256];
static void hle_dlopen(cpu_t *c)
{
    const char *n = harg(c,0) ? GSTR(harg(c,0)) : "";
    const char *b = strrchr(n, '/'); b = b ? b + 1 : n;
    if (!*b) { hret(c, DL_HANDLE(0)); return; }
    for (unsigned i = 0; i < sizeof(dl_names) / sizeof(dl_names[0]); i++)
        if (!strcmp(b, dl_names[i])) { hret(c, DL_HANDLE(i)); return; }
    snprintf(dl_err, sizeof(dl_err), "dlopen failed: library \"%s\" not found", n);
    LOG("[dl] dlopen(%s) -> NULL\n", n);
    hret(c, 0);
}
static void hle_dlsym(cpu_t *c)
{
    const char *s = GSTR(harg(c,1));
    u32 h = harg(c,0), a = 0;
    if (h == DL_OPENSL) { hret(c, opensl_dlsym(s)); return; }
    if (h >= DL_HANDLE(0) && h < DL_HANDLE(4)) a = elf_lookup_in(dl_names[h - DL_HANDLE(0)], s);
    if (!a) a = elf_lookup(s);
    if (!a && hle_has(s)) a = hle_bind(s);
    if (!a) { gptr d = hle_data_object(s); a = d; }
    if (!a) { snprintf(dl_err, sizeof(dl_err), "undefined symbol: %s", s); VLOG(1, "[dl] dlsym(%s) -> NULL\n", s); }
    hret(c, a);
}
static void hle_dlerror(cpu_t *c)
{
    if (!dl_err[0]) { hret(c, 0); return; }
    gptr b = strbuf() + 3328;
    snprintf(g2h(b), 256, "%s", dl_err);
    dl_err[0] = 0;
    hret(c, b);
}
static void hle_dladdr(cpu_t *c)
{
    u32 base; const char *n = elf_lib_of(harg(c,0), &base);
    gptr info = harg(c,1);
    if (!n) { hret(c, 0); return; }
    gptr b = strbuf() + 3584;
    snprintf(g2h(b), 256, "/data/app/com.ea.games.r3_row-1/lib/arm/%s", n);
    st32(info, b); st32(info + 4, base); st32(info + 8, 0); st32(info + 12, 0);
    hret(c, 1);
}

/* ---- EGL queries (the context itself is created by the Java side / host) ---- */
static void hle_eglGetDisplay(cpu_t *c) { hret(c, 1); }
static void hle_eglQueryString(cpu_t *c)
{
    gptr b = strbuf() + 3840;
    const char *v = harg(c,1) == 0x3054 ? "1.4 Android META-EGL" : harg(c,1) == 0x3053 ? "Android" : "";
    snprintf(g2h(b), 64, "%s", v);
    hret(c, b);
}
static void hle_eglGetProcAddress(cpu_t *c)
{
    const char *s = GSTR(harg(c,0));                   /* like Android: every gl* name gets an entry point */
    hret(c, hle_has(s) || !strncmp(s, "gl", 2) ? hle_bind(s) : 0);
}

/* ---- zlib: the host's libz, z_stream marshalled field by field ---- */
typedef struct { u8 *next_in; unsigned avail_in; unsigned long total_in; u8 *next_out; unsigned avail_out;
                 unsigned long total_out; const char *msg; void *state; void *zalloc, *zfree, *opaque;
                 int data_type; unsigned long adler, reserved; } hz_stream;
static void *zl;
static void *zsym(const char *n)
{
    if (!zl) zl = dlopen("libz.so.1", RTLD_NOW);
    if (!zl) zl = dlopen("libz.so", RTLD_NOW);
    void *f = zl ? dlsym(zl, n) : NULL;
    if (!f) fatal("zlib: %s unavailable (install zlib)", n);
    return f;
}
#define ZMAX 64
static struct { gptr g; hz_stream *h; } zs[ZMAX];
static pthread_mutex_t zs_lock = PTHREAD_MUTEX_INITIALIZER;
static hz_stream *zs_get(gptr g, bool create)
{
    pthread_mutex_lock(&zs_lock);
    hz_stream *r = NULL;
    for (int i = 0; i < ZMAX; i++) if (zs[i].g == g) { r = zs[i].h; break; }
    if (!r && create)
        for (int i = 0; i < ZMAX; i++) if (!zs[i].g) { zs[i].g = g; zs[i].h = r = calloc(1, sizeof(hz_stream)); break; }
    pthread_mutex_unlock(&zs_lock);
    return r;
}
static void zs_free(gptr g)
{
    pthread_mutex_lock(&zs_lock);
    for (int i = 0; i < ZMAX; i++) if (zs[i].g == g) { free(zs[i].h); zs[i].g = 0; zs[i].h = NULL; }
    pthread_mutex_unlock(&zs_lock);
}
static void z_in(gptr g, hz_stream *h)
{
    h->next_in = ld32(g) ? g2h(ld32(g)) : NULL; h->avail_in = ld32(g + 4);
    h->next_out = ld32(g + 12) ? g2h(ld32(g + 12)) : NULL; h->avail_out = ld32(g + 16);
    if (h->next_out) emu_prefault(h->next_out, h->avail_out);
}
static void z_out(gptr g, hz_stream *h)
{
    if (h->next_in) st32(g, h2g(h->next_in));
    st32(g + 4, h->avail_in); st32(g + 8, (u32)h->total_in);
    if (h->next_out) st32(g + 12, h2g(h->next_out));
    st32(g + 16, h->avail_out); st32(g + 20, (u32)h->total_out);
    st32(g + 24, 0); st32(g + 28, 0x5A000000u); st32(g + 44, (u32)h->data_type); st32(g + 48, (u32)h->adler);
}
static void hle_inflateInit2_(cpu_t *c)
{
    gptr g = harg(c,0);
    hz_stream *h = zs_get(g, true);
    if (!h) { hret(c, (u32)-4); return; }
    memset(h, 0, sizeof(*h));
    z_in(g, h);
    int (*f)(hz_stream *, int, const char *, int) = zsym("inflateInit2_");
    int r = f(h, (int)harg(c,1), "1.2.11", (int)sizeof(hz_stream));
    z_out(g, h);
    hret(c, (u32)r);
}
static void hle_inflateInit_(cpu_t *c) { c->r[3] = 0; c->r[2] = c->r[1]; c->r[1] = 15; hle_inflateInit2_(c); }
static void hle_inflate(cpu_t *c)
{
    gptr g = harg(c,0);
    hz_stream *h = zs_get(g, false);
    if (!h) { hret(c, (u32)-2); return; }
    z_in(g, h);
    int (*f)(hz_stream *, int) = zsym("inflate");
    int r = f(h, (int)harg(c,1));
    z_out(g, h);
    hret(c, (u32)r);
}
static void hle_inflateEnd(cpu_t *c)
{
    gptr g = harg(c,0);
    hz_stream *h = zs_get(g, false);
    int r = -2;
    if (h) { int (*f)(hz_stream *) = zsym("inflateEnd"); r = f(h); zs_free(g); st32(g + 28, 0); }
    hret(c, (u32)r);
}
static void hle_deflateInit2_(cpu_t *c)
{
    gptr g = harg(c,0);
    hz_stream *h = zs_get(g, true);
    if (!h) { hret(c, (u32)-4); return; }
    memset(h, 0, sizeof(*h));
    z_in(g, h);
    int (*f)(hz_stream *, int, int, int, int, int, const char *, int) = zsym("deflateInit2_");
    int r = f(h, (int)harg(c,1), (int)harg(c,2), (int)harg(c,3), (int)harg(c,4), (int)harg(c,5), "1.2.11", (int)sizeof(hz_stream));
    z_out(g, h);
    hret(c, (u32)r);
}
static void hle_deflateInit_(cpu_t *c)
{
    gptr g = harg(c,0);
    hz_stream *h = zs_get(g, true);
    if (!h) { hret(c, (u32)-4); return; }
    memset(h, 0, sizeof(*h));
    z_in(g, h);
    int (*f)(hz_stream *, int, const char *, int) = zsym("deflateInit_");
    int r = f(h, (int)harg(c,1), "1.2.11", (int)sizeof(hz_stream));
    z_out(g, h);
    hret(c, (u32)r);
}
static void hle_deflate(cpu_t *c)
{
    gptr g = harg(c,0);
    hz_stream *h = zs_get(g, false);
    if (!h) { hret(c, (u32)-2); return; }
    z_in(g, h);
    int (*f)(hz_stream *, int) = zsym("deflate");
    int r = f(h, (int)harg(c,1));
    z_out(g, h);
    hret(c, (u32)r);
}
static void hle_deflateEnd(cpu_t *c)
{
    gptr g = harg(c,0);
    hz_stream *h = zs_get(g, false);
    int r = -2;
    if (h) { int (*f)(hz_stream *) = zsym("deflateEnd"); r = f(h); zs_free(g); st32(g + 28, 0); }
    hret(c, (u32)r);
}
static void hle_uncompress(cpu_t *c)
{
    unsigned long dl = ld32(harg(c,1));
    emu_prefault(g2h(harg(c,0)), dl);
    int (*f)(u8 *, unsigned long *, const u8 *, unsigned long) = zsym("uncompress");
    int r = f(g2h(harg(c,0)), &dl, g2h(harg(c,2)), harg(c,3));
    st32(harg(c,1), (u32)dl);
    hret(c, (u32)r);
}
static void hle_compress2(cpu_t *c)
{
    unsigned long dl = ld32(harg(c,1));
    int lvl = (int)harg(c,4);
    emu_prefault(g2h(harg(c,0)), dl);
    int (*f)(u8 *, unsigned long *, const u8 *, unsigned long, int) = zsym("compress2");
    int r = f(g2h(harg(c,0)), &dl, g2h(harg(c,2)), harg(c,3), lvl);
    st32(harg(c,1), (u32)dl);
    hret(c, (u32)r);
}
static void hle_compress(cpu_t *c)
{
    unsigned long dl = ld32(harg(c,1));
    emu_prefault(g2h(harg(c,0)), dl);
    int (*f)(u8 *, unsigned long *, const u8 *, unsigned long, int) = zsym("compress2");
    int r = f(g2h(harg(c,0)), &dl, g2h(harg(c,2)), harg(c,3), -1);
    st32(harg(c,1), (u32)dl);
    hret(c, (u32)r);
}
static void hle_compressBound(cpu_t *c) { u32 n = harg(c,0); hret(c, n + (n >> 12) + (n >> 14) + (n >> 25) + 13); }
static void hle_crc32(cpu_t *c)
{
    unsigned long (*f)(unsigned long, const u8 *, unsigned) = zsym("crc32");
    hret(c, (u32)f(harg(c,0), harg(c,1) ? g2h(harg(c,1)) : NULL, harg(c,2)));
}
static void hle_zlibVersion(cpu_t *c)
{
    gptr b = strbuf() + 3904;
    strcpy(g2h(b), "1.2.11");
    hret(c, b);
}
static void hle_zError(cpu_t *c)
{
    gptr b = strbuf() + 3968;
    snprintf(g2h(b), 32, "zlib error %d", (int)harg(c,0));
    hret(c, b);
}
/* gz*: read-only, through the VFS */
static void hle_gzopen(cpu_t *c)
{
    const char *host = vfs_xlate(GSTR(harg(c,0)), strchr(GSTR(harg(c,1)), 'w') != NULL);
    void *(*f)(const char *, const char *) = zsym("gzopen");
    void *z = f(host, GSTR(harg(c,1)));
    free((void *)host);
    hret(c, z ? file_register((FILE *)z) : 0);                  /* handle table reused: pointer only */
}
static void hle_gzgets(cpu_t *c)
{
    void *z = file_for(harg(c,0));
    char *(*f)(void *, char *, int) = zsym("gzgets");
    char *r = z ? f(z, g2h(harg(c,1)), (int)harg(c,2)) : NULL;
    hret(c, r ? harg(c,1) : 0);
}
static void hle_gzclose(cpu_t *c)
{
    void *z = file_for(harg(c,0));
    int (*f)(void *) = zsym("gzclose");
    if (z) { file_unregister(harg(c,0)); hret(c, (u32)f(z)); } else hret(c, (u32)-2);
}

static void libc2_init(void)
{
    hle_register("__aeabi_memcpy", hle_aeabi_memcpy); hle_register("__aeabi_memcpy4", hle_aeabi_memcpy);
    hle_register("__aeabi_memcpy8", hle_aeabi_memcpy); hle_register("__aeabi_memmove", hle_aeabi_memmove);
    hle_register("__aeabi_memmove4", hle_aeabi_memmove); hle_register("__aeabi_memmove8", hle_aeabi_memmove);
    hle_register("__aeabi_memset", hle_aeabi_memset); hle_register("__aeabi_memset4", hle_aeabi_memset);
    hle_register("__aeabi_memset8", hle_aeabi_memset); hle_register("__aeabi_memclr", hle_aeabi_memclr);
    hle_register("__aeabi_memclr4", hle_aeabi_memclr); hle_register("__aeabi_memclr8", hle_aeabi_memclr);
    hle_register("strdup", hle_strdup); hle_register("memchr", hle_memchr); hle_register("memrchr", hle_memrchr);
    hle_register("memmem", hle_memmem); hle_register("strnlen", hle_strnlen); hle_register("strspn", hle_strspn);
    if (!hle_has("strcspn")) hle_register("strcspn", hle_strcspn2);
    hle_register("strpbrk", hle_strpbrk); hle_register("strtok_r", hle_strtok_r);
    hle_register("strncasecmp", hle_strncasecmp); hle_register("strcoll", hle_strcoll); hle_register("strxfrm", hle_strxfrm);
    hle_register("wcscoll", hle_wcscoll); hle_register("wcsxfrm", hle_wcsxfrm);
    hle_register("strerror", hle_strerror); hle_register("strerror_r", hle_strerror_r);
    hle_register("basename", hle_basename); hle_register("setlocale", hle_setlocale);
    hle_register("isalpha", hle_isalpha); hle_register("isalnum", hle_isalnum); hle_register("isprint", hle_isprint);
    hle_register("isupper", hle_isupper); hle_register("isspace", hle_isspace); hle_register("isxdigit", hle_isxdigit);
    hle_register("isblank", hle_isblank); hle_register("toupper", hle_toupper); hle_register("tolower", hle_tolower);
    hle_register("atol", hle_atol); hle_register("strtol", hle_strtol); hle_register("strtoll", hle_strtoll);
    hle_register("strtoull", hle_strtoull);
    hle_register("puts", hle_puts); hle_register("fseeko", hle_fseeko); hle_register("ftello", hle_ftello);
    hle_register("feof", hle_feof); hle_register("ferror", hle_ferror); hle_register("rewind", hle_rewind);
    hle_register("getc", hle_getc); hle_register("fileno", hle_fileno); hle_register("setvbuf", hle_setvbuf);
    hle_register("fputwc", hle_fputwc); hle_register("fdopen", hle_fdopen); hle_register("vasprintf", hle_vasprintf);
    hle_register("vsscanf", hle_vsscanf); hle_register("access", hle_access);
    hle_register("mktime", hle_mktime); hle_register("localtime_r", hle_localtime_r); hle_register("gmtime_r", hle_gmtime_r);
    hle_register("difftime", hle_difftime); hle_register("usleep", hle_usleep);
    hle_register("cosh", hle_cosh); hle_register("sinh", hle_sinh); hle_register("tanh", hle_tanh);
    hle_register("acosh", hle_acosh); hle_register("asinh", hle_asinh); hle_register("atanh", hle_atanh);
    hle_register("cbrt", hle_cbrt); hle_register("erf", hle_erf); hle_register("erfc", hle_erfc);
    hle_register("expm1", hle_expm1); hle_register("lgamma", hle_lgamma); hle_register("log1p", hle_log1p);
    hle_register("logb", hle_logb); hle_register("nearbyint", hle_nearbyint); hle_register("rint", hle_rint);
    hle_register("tgamma", hle_tgamma); hle_register("exp2", hle_exp2); hle_register("round", hle_round);
    hle_register("trunc", hle_trunc); hle_register("log2", hle_log2); hle_register("roundf", hle_roundf);
    hle_register("exp2f", hle_exp2f); hle_register("truncf", hle_truncf); hle_register("log2f", hle_log2f);
    hle_register("cbrtf", hle_cbrtf); hle_register("tanhf", hle_tanhf); hle_register("coshf", hle_coshf);
    hle_register("sinhf", hle_sinhf); hle_register("fabsf", hle_fabsf); hle_register("fabs", hle_fabs);
    hle_register("rintf", hle_rintf); hle_register("nearbyintf", hle_nearbyintf); hle_register("log1pf", hle_log1pf);
    hle_register("expm1f", hle_expm1f); hle_register("hypot", hle_hypot); hle_register("remainder", hle_remainder);
    hle_register("nextafter", hle_nextafter); hle_register("fmax", hle_fmax); hle_register("fmin", hle_fmin);
    hle_register("nextafterf", hle_nextafterf); hle_register("hypotf", hle_hypotf); hle_register("fmaxf", hle_fmaxf);
    hle_register("fminf", hle_fminf); hle_register("remainderf", hle_remainderf);
    hle_register("modff", hle_modff); hle_register("lrintf", hle_lrintf); hle_register("lrint", hle_lrint);
    hle_register("llrint", hle_llrint); hle_register("scalbn", hle_scalbn); hle_register("scalbnl", hle_scalbn);
    hle_register("ldexpl", hle_scalbn); hle_register("remquo", hle_remquo);
    hle_register("__fpclassifyd", hle_fpclassifyd); hle_register("__fpclassify", hle_fpclassifyd);
    hle_register("__signbit", hle_signbitd); hle_register("__isfinite", hle_isfinited); hle_register("__isnan", hle_isnand);
    hle_register("getpid", hle_getpid); hle_register("getuid", hle_getuid); hle_register("geteuid", hle_getuid);
    hle_register("getgid", hle_getuid); hle_register("getegid", hle_getuid); hle_register("getpwuid_r", hle_getpwuid_r);
    hle_register("ptrace", hle_ret0); hle_register("kill", hle_ret0); hle_register("sigaction", hle_ret0);
    hle_register("bsd_signal", hle_ret0); hle_register("pipe", hle_retm1); hle_register("socketpair", hle_retm1);
    hle_register("execl", hle_retm1); hle_register("dup2", hle_dup2); hle_register("select", hle_select);
    hle_register("sem_open", hle_sem_open); hle_register("sem_close", hle_ret0); hle_register("sem_unlink", hle_sem_unlink);
    hle_register("getaddrinfo", hle_getaddrinfo); hle_register("freeaddrinfo", hle_ret0);
    hle_register("getservbyname", hle_ret0); hle_register("if_nametoindex", hle_ret0);
    hle_register("inet_pton", hle_inet_pton); hle_register("inet_ntop", hle_inet_ntop);
    hle_register("dlopen", hle_dlopen); hle_register("dlsym", hle_dlsym); hle_register("dlclose", hle_ret0);
    hle_register("dlerror", hle_dlerror); hle_register("dladdr", hle_dladdr);
    hle_register("eglGetDisplay", hle_eglGetDisplay); hle_register("eglQueryString", hle_eglQueryString);
    hle_register("eglGetProcAddress", hle_eglGetProcAddress);
    hle_register("inflateInit_", hle_inflateInit_); hle_register("inflateInit2_", hle_inflateInit2_);
    hle_register("inflate", hle_inflate); hle_register("inflateEnd", hle_inflateEnd);
    hle_register("deflateInit_", hle_deflateInit_); hle_register("deflateInit2_", hle_deflateInit2_);
    hle_register("deflate", hle_deflate); hle_register("deflateEnd", hle_deflateEnd);
    hle_register("uncompress", hle_uncompress); hle_register("compress", hle_compress); hle_register("compress2", hle_compress2);
    hle_register("compressBound", hle_compressBound); hle_register("crc32", hle_crc32);
    hle_register("zlibVersion", hle_zlibVersion); hle_register("zError", hle_zError);
    hle_register("gzopen", hle_gzopen); hle_register("gzgets", hle_gzgets); hle_register("gzclose", hle_gzclose);
    hle_register("__cxa_thread_atexit_impl", hle_ret0);
}
