/*
 * mem.c - guest address space: one flat 4GB host reservation, commit-on-touch.
 * Guest pointer = u32 index into g_mem; no bounds checks on the CPU hot path.
 */
#include "emu.h"
#include <sys/mman.h>
#include <stdarg.h>

u8 *g_mem;
int g_verbose;
emu_t G;
__thread cpu_t *tls_cpu;

[[noreturn]] void fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "\n*** fatal: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    abort();
}

void mem_init(void)
{
    void *p = mmap(NULL, 0x100000000ULL, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED)
        fatal("cannot reserve 4GB guest space: %m");
    g_mem = p;
    /* low 1MB: null-pointer traps */
    if (mprotect(g_mem, GUEST_NULL_LIMIT, PROT_NONE) != 0)
        fatal("cannot protect null page: %m");
    LOG("[mem] 4GB guest space reserved at %p\n", (void *)g_mem);
}

/* bounds-checked translation for HLE paths that walk guest structures */
void *gchk(gptr p, u32 len)
{
    if (p < GUEST_NULL_LIMIT || p + len < p)
        fatal("HLE: wild guest pointer %08x len %u", p, len);
    return g2h(p);
}

/* ---- guest mmap arena: simple page bump allocator with free list ---- */

#define MMAP_ARENA_END 0x40000000u

static pthread_mutex_t mmap_lock = PTHREAD_MUTEX_INITIALIZER;
static gptr mmap_brk = GUEST_MMAP_BASE;

typedef struct freeblk { gptr addr; u32 npages; struct freeblk *next; } freeblk_t;
static freeblk_t *mmap_free;
static u64 mmap_live_pages;
u64 guest_mmap_live(void) { return mmap_live_pages * 4096; }

gptr guest_mmap_pages(u32 npages)
{
    pthread_mutex_lock(&mmap_lock);
    for (freeblk_t **pp = &mmap_free; *pp; pp = &(*pp)->next) {
        if ((*pp)->npages >= npages) {
            freeblk_t *b = *pp;
            gptr a = b->addr;
            if (b->npages == npages) { *pp = b->next; free(b); }
            else { b->addr += npages * 4096; b->npages -= npages; }
            mmap_live_pages += npages;
            pthread_mutex_unlock(&mmap_lock);
            return a;                         /* released with MADV_DONTNEED: already zero */
        }
    }
    gptr a = mmap_brk;
    if (a + npages * 4096 > MMAP_ARENA_END) {
        pthread_mutex_unlock(&mmap_lock);
        return 0;
    }
    mmap_brk += npages * 4096;
    mmap_live_pages += npages;
    pthread_mutex_unlock(&mmap_lock);
    return a;
}

void guest_munmap_pages(gptr addr, u32 npages)
{
    madvise(g2h(addr), (size_t)npages * 4096, MADV_DONTNEED);   /* give the RAM back */
    pthread_mutex_lock(&mmap_lock);
    mmap_live_pages -= npages;
    freeblk_t *b = malloc(sizeof(*b));
    b->addr = addr; b->npages = npages; b->next = mmap_free; mmap_free = b;
    pthread_mutex_unlock(&mmap_lock);
}

/* ---- HLE data zone allocator (guest objects: __sF, ctype tables, JNI tables) ---- */

static pthread_mutex_t hledata_lock = PTHREAD_MUTEX_INITIALIZER;
static gptr hledata_off = GUEST_HLE_DATA;

gptr hle_data_alloc(u32 size, u32 align)
{
    pthread_mutex_lock(&hledata_lock);
    gptr a = (hledata_off + align - 1) & ~(align - 1);
    if (a + size >= GUEST_MMAP_BASE) {
        pthread_mutex_unlock(&hledata_lock);
        fatal("HLE data zone exhausted");
    }
    hledata_off = a + size;
    pthread_mutex_unlock(&hledata_lock);
    memset(g2h(a), 0, size);
    return a;
}

/* ---- cpu lifecycle ---- */

cpu_t *emu_new_cpu(void)
{
    cpu_t *c = calloc(1, sizeof(*c));
    c->emu = &G;
    c->fpscr = 0;                       /* round-to-nearest, exceptions masked */
    /* per-thread guest scratch block (errno at [0]) */
    c->errno_addr = hle_data_alloc(256, 16);
    c->tls_base = c->errno_addr;        /* mrc p15,c13 insurance */

    pthread_mutex_lock(&G.threads_lock);
    if (G.nthreads < MAX_THREADS) {
        c->tid = G.nthreads;
        G.threads[G.nthreads++] = c;
    }
    pthread_mutex_unlock(&G.threads_lock);
    return c;
}

void emu_free_cpu(cpu_t *c)
{
    pthread_mutex_lock(&G.threads_lock);
    for (int i = 0; i < G.nthreads; i++)
        if (G.threads[i] == c) {
            G.threads[i] = G.threads[--G.nthreads];
            break;
        }
    pthread_mutex_unlock(&G.threads_lock);
    free(c);
}
