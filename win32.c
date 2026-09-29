/*
 * win32.c - Windows host layer: the POSIX pieces the emulator uses (anonymous
 * mmap with lazy commit, dlopen, pread, rename, poll) and the fault handler.
 *
 * Linux reserves the 4 GB guest space with MAP_NORESERVE and the kernel hands
 * out pages on first touch. Here every mapping is reserved and a vectored
 * exception handler commits pages as they are touched, so the same memory
 * layout works without charging 4 GB of commit up front.
 */
#ifdef _WIN32
#include "emu.h"
#include <windows.h>
#include <direct.h>
#include <io.h>
#include <sys/mman.h>
#include <dlfcn.h>
#include <poll.h>

/* ---- anonymous mappings ---- */

typedef struct { u8 *base; size_t len; DWORD prot; } region_t;
typedef struct { u8 *lo, *hi; } guard_t;
#define MAX_REGIONS 64
#define MAX_GUARDS  16
static region_t regions[MAX_REGIONS];
static guard_t guards[MAX_GUARDS];      /* PROT_NONE ranges that must keep faulting */
static SRWLOCK map_lock = SRWLOCK_INIT;

static DWORD win_prot(int prot)
{
    if (prot & PROT_EXEC) return prot & PROT_WRITE ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ;
    if (prot & PROT_WRITE) return PAGE_READWRITE;
    if (prot & PROT_READ) return PAGE_READONLY;
    return PAGE_NOACCESS;
}

void *mmap(void *addr, size_t len, int prot, int flags, int fd, long long off)
{
    (void)addr; (void)flags; (void)off;
    if (fd >= 0 || !len) return MAP_FAILED;
    u8 *p = VirtualAlloc(NULL, len, MEM_RESERVE, PAGE_NOACCESS);
    if (!p) return MAP_FAILED;
    AcquireSRWLockExclusive(&map_lock);
    int i;
    for (i = 0; i < MAX_REGIONS && regions[i].base; i++) {}
    if (i < MAX_REGIONS) regions[i] = (region_t){ p, len, win_prot(prot) };
    ReleaseSRWLockExclusive(&map_lock);
    if (i == MAX_REGIONS) { VirtualFree(p, 0, MEM_RELEASE); return MAP_FAILED; }
    return p;
}

int munmap(void *addr, size_t len)
{
    (void)len;
    AcquireSRWLockExclusive(&map_lock);
    for (int i = 0; i < MAX_REGIONS; i++) if (regions[i].base == addr) regions[i].base = NULL;
    ReleaseSRWLockExclusive(&map_lock);
    return VirtualFree(addr, 0, MEM_RELEASE) ? 0 : -1;
}

int mprotect(void *addr, size_t len, int prot)
{
    u8 *lo = addr, *hi = lo + len;
    AcquireSRWLockExclusive(&map_lock);
    int g, spare = -1;
    for (g = 0; g < MAX_GUARDS; g++) {
        if (guards[g].lo == lo && guards[g].hi == hi) break;
        if (!guards[g].lo && spare < 0) spare = g;
    }
    if (prot == PROT_NONE && g == MAX_GUARDS && spare >= 0) guards[spare] = (guard_t){ lo, hi };
    if (prot != PROT_NONE && g < MAX_GUARDS) guards[g] = (guard_t){ NULL, NULL };
    ReleaseSRWLockExclusive(&map_lock);
    MEMORY_BASIC_INFORMATION mi;
    if (VirtualQuery(addr, &mi, sizeof(mi)) && mi.State != MEM_COMMIT) {
        if (prot == PROT_NONE) return 0;                     /* reserved already faults */
        if (!VirtualAlloc(addr, len, MEM_COMMIT, win_prot(prot))) return -1;
        return 0;
    }
    DWORD old;
    return VirtualProtect(addr, len, win_prot(prot), &old) ? 0 : -1;
}

int madvise(void *addr, size_t len, int advice)
{
    if (advice == MADV_DONTNEED) VirtualFree(addr, len, MEM_DECOMMIT);   /* reads back as zero, like Linux */
    return 0;
}

int mincore(void *addr, size_t len, unsigned char *vec)
{
    for (size_t off = 0, k = 0; off < len; off += 4096, k++) {
        MEMORY_BASIC_INFORMATION mi;
        vec[k] = VirtualQuery((u8 *)addr + off, &mi, sizeof(mi)) && mi.State == MEM_COMMIT;
    }
    return 0;
}

/* commit the reserved pages around a faulting address (64 KB at a time) */
static bool lazy_commit(u8 *a)
{
    bool ok = false;
    AcquireSRWLockShared(&map_lock);
    for (int g = 0; g < MAX_GUARDS; g++) if (a >= guards[g].lo && a < guards[g].hi) goto out;
    for (int i = 0; i < MAX_REGIONS; i++) {
        region_t *r = &regions[i];
        if (!r->base || a < r->base || a >= r->base + r->len) continue;
        u8 *lo = r->base + (((size_t)(a - r->base)) & ~(size_t)0xFFFF), *hi = lo + 0x10000;
        if (hi > r->base + r->len) hi = r->base + r->len;
        for (u8 *p = lo; p < hi; ) {             /* only the still-reserved runs: keep watch pages' protection */
            MEMORY_BASIC_INFORMATION mi;
            if (!VirtualQuery(p, &mi, sizeof(mi))) break;
            u8 *end = (u8 *)mi.BaseAddress + mi.RegionSize;
            if (end > hi) end = hi;
            bool guarded = false;
            for (int g = 0; g < MAX_GUARDS; g++) if (guards[g].lo < end && guards[g].hi > p) guarded = true;
            if (mi.State == MEM_RESERVE && !guarded) VirtualAlloc(p, end - p, MEM_COMMIT, r->prot);
            p = end;
        }
        MEMORY_BASIC_INFORMATION mi;
        ok = VirtualQuery(a, &mi, sizeof(mi)) && mi.State == MEM_COMMIT && mi.Protect != PAGE_NOACCESS;
        break;
    }
out:
    ReleaseSRWLockShared(&map_lock);
    return ok;
}

/* the kernel fails I/O into uncommitted pages instead of faulting them in (Linux does) */
void emu_prefault(void *p, size_t n)
{
    u8 *a = p, *end = a + n;
    while (a < end) {
        lazy_commit(a);
        a = (u8 *)(((uintptr_t)a + 0x10000) & ~(uintptr_t)0xFFFF);
    }
}

int emu_fault(void *addr);                     /* main.c: watchpoints, else crash report */

static LONG CALLBACK on_exception(EXCEPTION_POINTERS *ep)
{
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    u8 *a = (u8 *)ep->ExceptionRecord->ExceptionInformation[1];
    if (lazy_commit(a)) return EXCEPTION_CONTINUE_EXECUTION;
    if (emu_fault(a)) return EXCEPTION_CONTINUE_EXECUTION;
    fprintf(stderr, "  host rip=%p\n", (void *)ep->ContextRecord->Rip);
    fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}

/* ---- libraries ---- */

static char dl_err[256];
void *dlopen(const char *name, int flags)
{
    (void)flags;
    HMODULE h = LoadLibraryA(name);
    if (!h) snprintf(dl_err, sizeof(dl_err), "%s: error %lu", name, GetLastError());
    return h;
}
void *dlsym(void *lib, const char *sym) { return (void *)GetProcAddress((HMODULE)lib, sym); }
const char *dlerror(void) { return dl_err; }

/* ---- files ---- */

int emu_mkdir(const char *path, int mode) { (void)mode; return _mkdir(path); }

char *emu_realpath(const char *path)
{
    char *r = _fullpath(NULL, path, 0);
    if (r && GetFileAttributesA(r) == INVALID_FILE_ATTRIBUTES) { free(r); r = NULL; }
    return r;
}

long emu_pread(int fd, void *buf, size_t n, long long off)
{
    long long keep = _lseeki64(fd, 0, SEEK_CUR);
    if (_lseeki64(fd, off, SEEK_SET) < 0) return -1;
    long got = _read(fd, buf, (unsigned)n);
    _lseeki64(fd, keep, SEEK_SET);
    return got;
}

int emu_rename(const char *from, const char *to)
{
    if (MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) return 0;
    errno = GetLastError() == ERROR_FILE_NOT_FOUND ? ENOENT : EACCES;
    return -1;
}

int poll(struct pollfd *fds, unsigned long nfds, int timeout)
{
    for (unsigned long i = 0; i < nfds; i++) fds[i].revents = 0;
    if (timeout > 0) Sleep((DWORD)timeout);
    return 0;
}

/* ---- process setup ---- */

void win_init(void)
{
    AddVectoredExceptionHandler(1, on_exception);
    timeBeginPeriod(1);                          /* the game's frame limiter sleeps in 1 ms steps */
    /* GUI binary: log to the console that started us, else to rr2emu.log next to the exe */
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
    } else if (GetStdHandle(STD_ERROR_HANDLE) == NULL || GetFileType(GetStdHandle(STD_ERROR_HANDLE)) == FILE_TYPE_UNKNOWN) {
        char path[MAX_PATH];
        DWORD n = GetModuleFileNameA(NULL, path, sizeof(path));
        while (n && path[n - 1] != '\\' && path[n - 1] != '/') n--;
        snprintf(path + n, sizeof(path) - n, "rr2emu.log");
        freopen(path, "w", stderr);
        freopen(path, "a", stdout);
    }
    setvbuf(stderr, NULL, _IONBF, 0);
}
#endif
