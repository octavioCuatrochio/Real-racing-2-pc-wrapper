/*
 * download.c - resumable HTTPS download to a file, for the launcher's game-file fetcher.
 * Linux: libcurl through dlopen (no build dependency). Windows: WinHTTP.
 * Data goes to <path>.part and is renamed to <path> once complete, so an interrupted
 * download continues where it stopped the next time.
 */
#include "emu.h"
#include "download.h"
#include <sys/stat.h>
#include <time.h>

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

typedef struct {
    FILE *f;
    u64 base, got, total;
    dl_progress_fn cb;
    void *ud;
    double t0, last;
    u64 rate_bytes; double rate_t; double rate;
    bool cancelled;
} xfer_t;

/* progress at most ~10 times a second; speed averaged over the last few seconds */
static bool xfer_tick(xfer_t *x, bool force)
{
    double t = now_s();
    if (!force && t - x->last < 0.1) return !x->cancelled;
    x->last = t;
    if (t - x->rate_t >= 2.0) {
        x->rate = (x->got - x->rate_bytes) / (t - x->rate_t);
        x->rate_bytes = x->got; x->rate_t = t;
    } else if (x->rate == 0 && t - x->t0 > 0.5) x->rate = x->got / (t - x->t0);
    if (x->cb && !x->cb(x->base + x->got, x->total, x->rate, x->ud)) x->cancelled = true;
    return !x->cancelled;
}

static u64 file_size(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 ? (u64)st.st_size : 0;
}

#ifndef _WIN32
#include <dlfcn.h>

/* the few libcurl entry points and option numbers used (stable ABI since 7.x) */
#define CURLOPT_WRITEDATA          10001
#define CURLOPT_URL                10002
#define CURLOPT_USERAGENT          10018
#define CURLOPT_WRITEFUNCTION      20011
#define CURLOPT_XFERINFOFUNCTION   20219
#define CURLOPT_XFERINFODATA       10057
#define CURLOPT_NOPROGRESS         43
#define CURLOPT_FAILONERROR        45
#define CURLOPT_FOLLOWLOCATION     52
#define CURLOPT_CONNECTTIMEOUT     78
#define CURLOPT_LOW_SPEED_LIMIT    19
#define CURLOPT_LOW_SPEED_TIME     20
#define CURLOPT_RESUME_FROM_LARGE  30116
#define CURLINFO_RESPONSE_CODE     0x200002
#define CURLE_ABORTED_BY_CALLBACK  42

static struct {
    void *lib;
    int   (*global_init)(long);
    void *(*easy_init)(void);
    int   (*easy_setopt)(void *, int, ...);
    int   (*easy_perform)(void *);
    int   (*easy_getinfo)(void *, int, ...);
    void  (*easy_cleanup)(void *);
    const char *(*easy_strerror)(int);
} cu;

static bool curl_load(void)
{
    if (cu.lib) return true;
    static const char *const names[] = { "libcurl.so.4", "libcurl-gnutls.so.4", "libcurl.so" };
    for (unsigned i = 0; i < 3 && !cu.lib; i++) cu.lib = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
    if (!cu.lib) return false;
    *(void **)&cu.global_init = dlsym(cu.lib, "curl_global_init");
    *(void **)&cu.easy_init = dlsym(cu.lib, "curl_easy_init");
    *(void **)&cu.easy_setopt = dlsym(cu.lib, "curl_easy_setopt");
    *(void **)&cu.easy_perform = dlsym(cu.lib, "curl_easy_perform");
    *(void **)&cu.easy_getinfo = dlsym(cu.lib, "curl_easy_getinfo");
    *(void **)&cu.easy_cleanup = dlsym(cu.lib, "curl_easy_cleanup");
    *(void **)&cu.easy_strerror = dlsym(cu.lib, "curl_easy_strerror");
    if (!cu.global_init || !cu.easy_init || !cu.easy_setopt || !cu.easy_perform || !cu.easy_getinfo ||
        !cu.easy_cleanup || !cu.easy_strerror) { dlclose(cu.lib); cu.lib = NULL; return false; }
    cu.global_init(3 /* CURL_GLOBAL_DEFAULT */);
    return true;
}

static size_t on_data(char *p, size_t sz, size_t n, void *ud)
{
    xfer_t *x = ud;
    size_t len = sz * n;
    if (fwrite(p, 1, len, x->f) != len) return 0;
    x->got += len;
    return xfer_tick(x, false) ? len : 0;
}
static int on_progress(void *ud, long long dt, long long dn, long long ut, long long un)
{
    (void)dt; (void)dn; (void)ut; (void)un;
    return xfer_tick(ud, false) ? 0 : 1;
}

static int fetch(const char *url, xfer_t *x, char *err, size_t errn)
{
    if (!curl_load()) { snprintf(err, errn, "libcurl not found (install libcurl4)"); return DL_ERROR; }
    void *h = cu.easy_init();
    if (!h) { snprintf(err, errn, "curl init failed"); return DL_ERROR; }
    cu.easy_setopt(h, CURLOPT_URL, url);
    cu.easy_setopt(h, CURLOPT_USERAGENT, "rr2emu");
    cu.easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    cu.easy_setopt(h, CURLOPT_FAILONERROR, 1L);
    cu.easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 30L);
    cu.easy_setopt(h, CURLOPT_LOW_SPEED_LIMIT, 1024L);      /* under 1 KB/s for a minute: give up (resumable) */
    cu.easy_setopt(h, CURLOPT_LOW_SPEED_TIME, 60L);
    cu.easy_setopt(h, CURLOPT_WRITEFUNCTION, on_data);
    cu.easy_setopt(h, CURLOPT_WRITEDATA, x);
    cu.easy_setopt(h, CURLOPT_NOPROGRESS, 0L);
    cu.easy_setopt(h, CURLOPT_XFERINFOFUNCTION, on_progress);
    cu.easy_setopt(h, CURLOPT_XFERINFODATA, x);
    if (x->base) cu.easy_setopt(h, CURLOPT_RESUME_FROM_LARGE, (long long)x->base);
    int rc = cu.easy_perform(h);
    long code = 0;
    cu.easy_getinfo(h, CURLINFO_RESPONSE_CODE, &code);
    cu.easy_cleanup(h);
    if (x->cancelled || rc == CURLE_ABORTED_BY_CALLBACK) return DL_CANCELLED;
    if (rc) { snprintf(err, errn, "%s", cu.easy_strerror(rc)); return DL_ERROR; }
    if (x->base && code == 200) { snprintf(err, errn, "server ignored the resume request"); return DL_RESTART; }
    return DL_OK;
}

#else
#include <windows.h>
#include <winhttp.h>

static int fetch(const char *url, xfer_t *x, char *err, size_t errn)
{
    wchar_t wurl[2048], host[256], path[2048], hdr[64];
    MultiByteToWideChar(CP_UTF8, 0, url, -1, wurl, 2048);
    URL_COMPONENTS uc = { .dwStructSize = sizeof(uc), .lpszHostName = host, .dwHostNameLength = 256,
                          .lpszUrlPath = path, .dwUrlPathLength = 2048 };
    if (!WinHttpCrackUrl(wurl, 0, 0, &uc)) { snprintf(err, errn, "bad URL"); return DL_ERROR; }
    HINTERNET s = WinHttpOpen(L"rr2emu", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    HINTERNET c = s ? WinHttpConnect(s, host, uc.nPort, 0) : NULL;
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path, NULL, NULL, NULL,
                                         uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : NULL;
    int ret = DL_ERROR;
    if (!r) { snprintf(err, errn, "cannot connect (error %lu)", GetLastError()); goto out; }
    if (x->base) {
        swprintf(hdr, 64, L"Range: bytes=%llu-", (unsigned long long)x->base);
        WinHttpAddRequestHeaders(r, hdr, (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
    }
    if (!WinHttpSendRequest(r, NULL, 0, NULL, 0, 0, 0) || !WinHttpReceiveResponse(r, NULL)) {
        snprintf(err, errn, "request failed (error %lu)", GetLastError()); goto out;
    }
    DWORD code = 0, cl = sizeof(code);
    WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &code, &cl, NULL);
    if (code >= 400) { snprintf(err, errn, "HTTP %lu", code); goto out; }
    if (x->base && code == 200) { snprintf(err, errn, "server ignored the resume request"); ret = DL_RESTART; goto out; }
    static char buf[1 << 16];
    for (;;) {
        DWORD n = 0;
        if (!WinHttpReadData(r, buf, sizeof(buf), &n)) { snprintf(err, errn, "read failed (error %lu)", GetLastError()); goto out; }
        if (!n) break;
        if (fwrite(buf, 1, n, x->f) != n) { snprintf(err, errn, "cannot write the file (disk full?)"); goto out; }
        x->got += n;
        if (!xfer_tick(x, false)) { ret = DL_CANCELLED; goto out; }
    }
    ret = DL_OK;
out:
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    if (s) WinHttpCloseHandle(s);
    return ret;
}
#endif

int http_download(const char *url, const char *path, u64 size, dl_progress_fn cb, void *ud, char *err, size_t errn)
{
    char part[1200];
    snprintf(part, sizeof(part), "%s.part", path);
    *err = 0;
    for (int attempt = 0; attempt < 2; attempt++) {
        u64 have = file_size(part);
        if (size && have > size) { remove(part); have = 0; }
        xfer_t x = { .base = have, .total = size, .cb = cb, .ud = ud, .t0 = now_s() };
        x.rate_t = x.t0;
        if (!size || have < size) {
            x.f = fopen(part, have ? "ab" : "wb");
            if (!x.f) { snprintf(err, errn, "cannot write %s", part); return DL_ERROR; }
            int rc = fetch(url, &x, err, errn);
            if (fclose(x.f) != 0 && rc == DL_OK) { snprintf(err, errn, "cannot write the file (disk full?)"); rc = DL_ERROR; }
            if (rc == DL_RESTART) { remove(part); continue; }
            if (rc != DL_OK) return rc;
        }
        xfer_tick(&x, true);
        u64 got = file_size(part);
        if (size && got != size) { snprintf(err, errn, "incomplete: %llu of %llu bytes", (unsigned long long)got, (unsigned long long)size); return DL_ERROR; }
        remove(path);
        if (rename(part, path) != 0) { snprintf(err, errn, "cannot rename %s", part); return DL_ERROR; }
        return DL_OK;
    }
    return DL_ERROR;
}
