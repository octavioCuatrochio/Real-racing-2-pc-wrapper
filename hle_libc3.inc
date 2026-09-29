/*
 * hle_libc3.inc - Bionic entry points newer NDK builds import (Real Racing 3 14.x):
 * FORTIFY variants, 64-bit file calls, auxv/properties, wide characters and the
 * locale calls libc++ makes. Included at the end of hle_libc.c.
 */
#include <wctype.h>

void hle_mmap_entry(cpu_t *c);                              /* hle_malloc.c */
bool hle_alias(const char *name, const char *existing);    /* hle.c */

/* ---- FORTIFY: same leading arguments as the plain call; the extra size is advisory ---- */
static void hle_vsnprintf_chk(cpu_t *c)                     /* (buf, maxlen, flags, slen, fmt, va) */
{
    c->r[2] = harg(c, 4); c->r[3] = harg(c, 5);
    hle_vsnprintf(c);
}
static void hle_vsprintf_chk(cpu_t *c)                      /* (buf, flags, slen, fmt, va) */
{
    c->r[1] = c->r[3]; c->r[2] = harg(c, 4);
    hle_vsprintf(c);
}
static void hle_fd_set_chk(cpu_t *c)   { u32 fd = harg(c, 0); gptr s = harg(c, 1) + fd / 32 * 4; st32(s, ld32(s) | 1u << (fd & 31)); hret(c, 0); }
static void hle_fd_isset_chk(cpu_t *c) { u32 fd = harg(c, 0); hret(c, (ld32(harg(c, 1) + fd / 32 * 4) >> (fd & 31)) & 1); }
static void hle_assert2(cpu_t *c)
{
    LOG("[hle] assertion failed: %s:%u %s: %s\n", GSTR(harg(c, 0)), harg(c, 1), GSTR(harg(c, 2)), GSTR(harg(c, 3)));
    emu_trap(c, "__assert2");
}
static void hle_printf(cpu_t *c)
{
    char buf[4096];
    gva_t w = { .c = c, .reg = 1, .stk = 0, .vlist = 0 };
    size_t n = gfmt(buf, sizeof(buf), harg(c, 0), &w);
    fwrite(buf, 1, n, stdout);
    hret(c, (u32)n);
}

/* ---- files ---- */
static void hle_lseek64(cpu_t *c)                           /* (fd, off64 in r2:r3, whence on the stack) */
{
    s64 off = (s64)((u64)c->r[2] | (u64)c->r[3] << 32);
    long long r = lseek((int)c->r[0], (off_t)off, (int)harg(c, 4));
    if (r < 0) guest_errno_set(c, errno);
    hret64(c, (u64)r);
}
static void hle_ftruncate64(cpu_t *c)
{
    s64 len = (s64)((u64)c->r[2] | (u64)c->r[3] << 32);
    int r = ftruncate((int)c->r[0], (off_t)len);
    if (r < 0) guest_errno_set(c, errno);
    hret(c, (u32)r);
}
static void hle_mmap2(cpu_t *c)                             /* offset in pages: rewrite the stack argument */
{
    st32(c->r[13] + 4, harg(c, 5) * 4096);
    hle_mmap_entry(c);
}
static void hle_realpath(cpu_t *c)                          /* guest paths are already absolute here */
{
    const char *p = GSTR(harg(c, 0));
    gptr out = harg(c, 1) ? harg(c, 1) : guest_malloc(4096);
    snprintf(g2h(out), 4096, "%s", p);
    hret(c, out);
}
static void hle_readlink(cpu_t *c)  { guest_errno_set(c, 22 /* EINVAL: not a link */); hret(c, (u32)-1); }
static void hle_eperm(cpu_t *c)     { guest_errno_set(c, 1); hret(c, (u32)-1); }
static void hle_enosys(cpu_t *c)    { guest_errno_set(c, 38); hret(c, (u32)-1); }
static void hle_pathconf(cpu_t *c)  { hret(c, 255); }
static void hle_clearerr(cpu_t *c)  { FILE *f = file_for(harg(c, 0)); if (f) clearerr(f); hret(c, 0); }
static void hle_nothing(cpu_t *c)   { hret(c, 0); }
static void hle_getentropy(cpu_t *c)
{
    u8 *p = g2h(harg(c, 0));
    for (u32 i = 0; i < harg(c, 1); i++) p[i] = (u8)(rand() >> 7);
    hret(c, 0);
}
static void hle_mremap(cpu_t *c)    { guest_errno_set(c, 12 /* ENOMEM */); hret(c, (u32)-1); }

/* ---- process / system ---- */
static void hle_sleep(cpu_t *c)       { sleep(harg(c, 0)); hret(c, 0); }
static void hle_getpagesize(cpu_t *c) { hret(c, 4096); }
static void hle_getauxval(cpu_t *c)
{
    switch (harg(c, 0)) {
    case 6: hret(c, 4096); return;                                           /* AT_PAGESZ */
    case 16: hret(c, 0x2 | 0x4 | 0x10 | 0x40 | 0x80 | 0x1000 | 0x2000 | 0x8000 |   /* AT_HWCAP: half thumb fastmult vfp */
                     0x10000 | 0x20000 | 0x40000 | 0x80000); return;           /* edsp neon vfpv3 tls vfpv4 idiva idivt vfpd32 */
    default: hret(c, 0); return;                                            /* AT_HWCAP2 0: no AES/SHA insns emulated */
    }
}
static void hle_syscall(cpu_t *c)
{
    if (harg(c, 0) == 224) { hret(c, (u32)(1000 + c->tid)); return; }     /* __NR_gettid */
    guest_errno_set(c, 38);
    hret(c, (u32)-1);
}
static void hle_system_property_get(cpu_t *c)
{
    static const char *const props[][2] = {
        { "ro.build.version.sdk", "25" }, { "ro.build.version.release", "7.1.2" },
        { "ro.product.model", "Pixel 2" }, { "ro.product.manufacturer", "Google" },
        { "ro.product.brand", "google" }, { "ro.product.device", "walleye" },
        { "ro.hardware", "walleye" }, { "ro.board.platform", "msm8998" },
        { "ro.product.cpu.abi", "armeabi-v7a" },
    };
    const char *n = GSTR(harg(c, 0));
    char *out = g2h(harg(c, 1));
    out[0] = 0;
    for (unsigned i = 0; i < sizeof(props) / sizeof(props[0]); i++)
        if (!strcmp(n, props[i][0])) { strcpy(out, props[i][1]); break; }
    hret(c, (u32)strlen(out));
}
static void hle_dl_unwind_find_exidx(cpu_t *c)
{
    u32 n = 0, a = elf_exidx_for(harg(c, 0), &n);
    if (harg(c, 1)) st32(harg(c, 1), n);
    hret(c, a);
}
static void hle_sched_getparam(cpu_t *c) { if (harg(c, 1)) st32(harg(c, 1), 0); hret(c, 0); }

/* ---- math ---- */
static float argf(cpu_t *c, int i) { u32 b = harg(c, i); float f; memcpy(&f, &b, 4); return f; }
static void retf(cpu_t *c, float f) { u32 b; memcpy(&b, &f, 4); hret(c, b); }
static void retd(cpu_t *c, double d) { u64 b; memcpy(&b, &d, 8); hret64(c, b); }
static void hle_atof(cpu_t *c)     { retd(c, strtod(GSTR(harg(c, 0)), NULL)); }
static void hle_strtof(cpu_t *c)
{
    const char *s = GSTR(harg(c, 0));
    char *end;
    float f = strtof(s, &end);
    if (harg(c, 1)) st32(harg(c, 1), harg(c, 0) + (u32)(end - s));
    retf(c, f);
}
static void hle_sincosf(cpu_t *c)
{
    float x = argf(c, 0), s = sinf(x), co = cosf(x);
    u32 b;
    memcpy(&b, &s, 4); st32(harg(c, 1), b);
    memcpy(&b, &co, 4); st32(harg(c, 2), b);
    hret(c, 0);
}
static void hle_llround(cpu_t *c)  { hret64(c, (u64)llround(harg_d(c))); }
static void hle_llroundf(cpu_t *c) { hret64(c, (u64)llroundf(argf(c, 0))); }

/* ---- wide characters (wchar_t is 32-bit on Android) and the C/UTF-8 locale ---- */
#define WFN(name) static void hle_##name(cpu_t *c) { hret(c, (u32)name((wint_t)harg(c, 0))); }
WFN(iswlower) WFN(iswupper) WFN(iswspace) WFN(iswprint) WFN(iswcntrl) WFN(iswalpha)
WFN(iswdigit) WFN(iswpunct) WFN(iswxdigit) WFN(towupper) WFN(towlower)
#undef WFN
static void hle_btowc(cpu_t *c) { u32 ch = harg(c, 0); hret(c, ch < 0x80 ? ch : 0xFFFFFFFFu); }
static void hle_wctob(cpu_t *c) { u32 ch = harg(c, 0); hret(c, ch < 0x80 ? ch : 0xFFFFFFFFu); }
static void hle_wcslen(cpu_t *c) { gptr p = harg(c, 0); u32 n = 0; while (ld32(p + 4 * n)) n++; hret(c, n); }
static void hle_wmemcpy(cpu_t *c)  { memmove(g2h(harg(c, 0)), g2h(harg(c, 1)), (size_t)harg(c, 2) * 4); hret(c, harg(c, 0)); }
static void hle_wmemset(cpu_t *c)  { for (u32 i = 0; i < harg(c, 2); i++) st32(harg(c, 0) + 4 * i, harg(c, 1)); hret(c, harg(c, 0)); }

/* UTF-8 decode of up to n bytes: length consumed, 0 for NUL, -1 invalid, -2 incomplete */
static int utf8_dec(const u8 *s, u32 n, u32 *out)
{
    if (!n) return -2;
    u32 b = s[0], len = b < 0x80 ? 1 : (b & 0xE0) == 0xC0 ? 2 : (b & 0xF0) == 0xE0 ? 3 : (b & 0xF8) == 0xF0 ? 4 : 0;
    if (!len) return -1;
    if (n < len) return -2;
    u32 v = len == 1 ? b : b & (0x7F >> len);
    for (u32 i = 1; i < len; i++) { if ((s[i] & 0xC0) != 0x80) return -1; v = v << 6 | (s[i] & 0x3F); }
    *out = v;
    return v ? (int)len : 0;
}
static u32 utf8_enc(u32 v, u8 *o)
{
    if (v < 0x80) { o[0] = (u8)v; return 1; }
    if (v < 0x800) { o[0] = (u8)(0xC0 | v >> 6); o[1] = (u8)(0x80 | (v & 0x3F)); return 2; }
    if (v < 0x10000) { o[0] = (u8)(0xE0 | v >> 12); o[1] = (u8)(0x80 | (v >> 6 & 0x3F)); o[2] = (u8)(0x80 | (v & 0x3F)); return 3; }
    o[0] = (u8)(0xF0 | v >> 18); o[1] = (u8)(0x80 | (v >> 12 & 0x3F)); o[2] = (u8)(0x80 | (v >> 6 & 0x3F)); o[3] = (u8)(0x80 | (v & 0x3F));
    return 4;
}
static void hle_mbrtowc(cpu_t *c)                           /* (pwc, s, n, ps) */
{
    if (!harg(c, 1)) { hret(c, 0); return; }
    u32 v = 0;
    int r = utf8_dec(g2h(harg(c, 1)), harg(c, 2), &v);
    if (r == -1) { guest_errno_set(c, 84 /* EILSEQ */); hret(c, (u32)-1); return; }
    if (r == -2) { hret(c, (u32)-2); return; }
    if (harg(c, 0)) st32(harg(c, 0), v);
    hret(c, (u32)r);
}
static void hle_mbrlen(cpu_t *c) { c->r[3] = c->r[2]; c->r[2] = c->r[1]; c->r[1] = c->r[0]; c->r[0] = 0; hle_mbrtowc(c); }
static void hle_mbtowc(cpu_t *c) { c->r[3] = 0; hle_mbrtowc(c); if ((s32)c->r[0] == -2) hret(c, (u32)-1); }
static void hle_wcrtomb(cpu_t *c)                           /* (s, wc, ps) */
{
    if (!harg(c, 0)) { hret(c, 1); return; }
    hret(c, utf8_enc(harg(c, 1), g2h(harg(c, 0))));
}
static void hle_mbsnrtowcs(cpu_t *c)                        /* (dst, *src, nms, len, ps); mbsrtowcs has no nms */
{
    gptr dst = harg(c, 0), srcp = harg(c, 1);
    u32 nms = harg(c, 2), len = harg(c, 3), n = 0;
    gptr s = ld32(srcp);
    while (dst ? n < len : true) {
        u32 v = 0;
        int r = utf8_dec(g2h(s), nms, &v);
        if (r < 0) { if (r == -1) { guest_errno_set(c, 84); hret(c, (u32)-1); return; } break; }
        if (r == 0) { if (dst) { st32(dst + 4 * n, 0); st32(srcp, 0); } break; }
        if (dst) st32(dst + 4 * n, v);
        s += (u32)r; nms -= (u32)r; n++;
    }
    if (dst && ld32(srcp)) st32(srcp, s);
    hret(c, n);
}
static void hle_mbsrtowcs(cpu_t *c) { c->r[3] = c->r[2]; c->r[2] = 0xFFFFFFFFu; hle_mbsnrtowcs(c); }   /* (dst, *src, len, ps) */
static void hle_wcsnrtombs(cpu_t *c)                        /* (dst, *src, nwc, len, ps) */
{
    gptr dst = harg(c, 0), srcp = harg(c, 1);
    u32 nwc = harg(c, 2), len = harg(c, 3), n = 0;
    gptr s = ld32(srcp);
    for (; nwc; nwc--, s += 4) {
        u32 v = ld32(s);
        u8 tmp[4];
        u32 k = utf8_enc(v, tmp);
        if (!v) { if (dst) { if (n < len) ((u8 *)g2h(dst))[n] = 0; st32(srcp, 0); } hret(c, n); return; }
        if (dst && n + k > len) break;
        if (dst) memcpy((u8 *)g2h(dst) + n, tmp, k);
        n += k;
    }
    if (dst) st32(srcp, s);
    hret(c, n);
}
/* locales: one C/UTF-8 locale, handed out as a non-null token */
static void hle_newlocale(cpu_t *c) { hret(c, 0x10); }
static void hle_uselocale(cpu_t *c) { hret(c, 0x10); }

/* ---- network (offline) ---- */
static void hle_getnameinfo(cpu_t *c) { hret(c, (u32)-4 /* EAI_FAIL */); }
static void hle_gai_strerror(cpu_t *c)
{
    static gptr msg;
    if (!msg) { msg = hle_data_alloc(32, 4); strcpy(g2h(msg), "network unavailable"); }
    hret(c, msg);
}
static void hle_get_h_errno(cpu_t *c) { static gptr e; if (!e) e = hle_data_alloc(4, 4); hret(c, e); }

static void libc3_init(void)
{
    static const char *const same[][2] = {                  /* FORTIFY / _l / 64-bit spellings of existing calls */
        { "__memcpy_chk", "memcpy" }, { "__memset_chk", "memset" }, { "__memmove_chk", "memmove" },
        { "__strcpy_chk", "strcpy" }, { "__strncpy_chk", "strncpy" }, { "__strncpy_chk2", "strncpy" },
        { "__strcat_chk", "strcat" }, { "__strchr_chk", "strchr" }, { "__strrchr_chk", "strrchr" },
        { "__fwrite_chk", "fwrite" }, { "lstat", "stat" }, { "strtoimax", "strtoll" }, { "strtoumax", "strtoull" },
        { "strtoll_l", "strtoll" }, { "strtoull_l", "strtoull" }, { "strtold_l", "strtod" }, { "wmemmove", "wmemcpy" },
    };
    hle_register("__vsnprintf_chk", hle_vsnprintf_chk);
    hle_register("__vsprintf_chk", hle_vsprintf_chk);
    hle_register("__FD_SET_chk", hle_fd_set_chk);
    hle_register("__FD_ISSET_chk", hle_fd_isset_chk);
    hle_register("__assert2", hle_assert2);
    hle_register("printf", hle_printf);
    hle_register("lseek64", hle_lseek64);
    hle_register("ftruncate64", hle_ftruncate64);
    hle_register("__mmap2", hle_mmap2);
    hle_register("realpath", hle_realpath);
    hle_register("readlink", hle_readlink);
    hle_register("symlink", hle_eperm); hle_register("link", hle_eperm);
    hle_register("sendfile", hle_enosys); hle_register("sendmmsg", hle_enosys); hle_register("recvmmsg", hle_enosys);
    hle_register("pathconf", hle_pathconf);
    hle_register("clearerr", hle_clearerr);
    static const char *const ok0[] = { "fchmod", "fchown", "fchmodat", "utimes", "utimensat", "truncate", "setbuf", "mlock",
        "sched_setaffinity", "pthread_mutexattr_setpshared", "pthread_attr_setschedpolicy", "pthread_attr_setschedparam",
        "pthread_atfork", "__register_atfork", "freelocale", "__cmsg_nxthdr" };
    for (unsigned i = 0; i < sizeof(ok0) / sizeof(ok0[0]); i++) hle_register(ok0[i], hle_nothing);
    hle_register("pthread_attr_getschedparam", hle_sched_getparam);
    hle_register("getentropy", hle_getentropy);
    hle_register("mremap", hle_mremap);
    hle_register("sleep", hle_sleep);
    hle_register("getpagesize", hle_getpagesize);
    hle_register("getauxval", hle_getauxval);
    hle_register("syscall", hle_syscall);
    hle_register("__system_property_get", hle_system_property_get);
    hle_register("dl_unwind_find_exidx", hle_dl_unwind_find_exidx);
    hle_register("atof", hle_atof); hle_register("strtof", hle_strtof); hle_register("sincosf", hle_sincosf);
    hle_register("llround", hle_llround); hle_register("llroundf", hle_llroundf);
    hle_register("iswlower", hle_iswlower); hle_register("iswupper", hle_iswupper); hle_register("iswspace", hle_iswspace);
    hle_register("iswprint", hle_iswprint); hle_register("iswcntrl", hle_iswcntrl); hle_register("iswalpha", hle_iswalpha);
    hle_register("iswdigit", hle_iswdigit); hle_register("iswpunct", hle_iswpunct); hle_register("iswxdigit", hle_iswxdigit);
    hle_register("towupper", hle_towupper); hle_register("towlower", hle_towlower);
    hle_register("btowc", hle_btowc); hle_register("wctob", hle_wctob); hle_register("wcslen", hle_wcslen);
    hle_register("wmemcpy", hle_wmemcpy); hle_register("wmemset", hle_wmemset);
    hle_register("mbrtowc", hle_mbrtowc); hle_register("mbrlen", hle_mbrlen); hle_register("mbtowc", hle_mbtowc);
    hle_register("wcrtomb", hle_wcrtomb); hle_register("mbsnrtowcs", hle_mbsnrtowcs); hle_register("mbsrtowcs", hle_mbsrtowcs);
    hle_register("wcsnrtombs", hle_wcsnrtombs);
    hle_register("newlocale", hle_newlocale); hle_register("uselocale", hle_uselocale);
    hle_register("getnameinfo", hle_getnameinfo); hle_register("gai_strerror", hle_gai_strerror);
    hle_register("__get_h_errno", hle_get_h_errno);
    for (unsigned i = 0; i < sizeof(same) / sizeof(same[0]); i++)
        if (!hle_alias(same[i][0], same[i][1])) LOG("[hle] alias %s: no %s\n", same[i][0], same[i][1]);
}
