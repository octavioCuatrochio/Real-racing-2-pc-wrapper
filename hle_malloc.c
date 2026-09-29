/*
 * hle_malloc.c - guest heap (TLSF: O(1) malloc/free, immediate coalescing)
 * + sbrk window + guest mmap.
 *
 * Block layout in guest memory (8-aligned):
 *   +0 prev_phys  guest addr of the physically previous block
 *   +4 size       total block size incl. header; bit0 = free, bit1 = prev free
 *   +8 payload    (free blocks: +8 next_free, +12 prev_free)
 * The heap ends in a zero-size used sentinel; growth turns it into a free
 * block over the new range. One global lock.
 */
#include "emu.h"
#include <unistd.h>
#include <fcntl.h>

#define HDR        8u
#define MIN_BLOCK  16u
#define F_FREE     1u
#define F_PREVFREE 2u
#define SL_LOG2    5
#define SL_COUNT   (1u << SL_LOG2)
#define SMALL      256u             /* below: fl=0, one class per 8 bytes */
#define FL_SHIFT   7                /* log2(SMALL) - 1 */
#define FL_COUNT   25

static pthread_mutex_t heap_lock = PTHREAD_MUTEX_INITIALIZER;
static u64 heap_used;
u64 guest_heap_in_use(void) { return heap_used; }
static gptr heap_end;
static gptr sentinel;
static u32  fl_bitmap;
static u32  sl_bitmap[FL_COUNT];
static gptr heads[FL_COUNT][SL_COUNT];

#define B_PREV(b)  (*(u32 *)(g_mem + (b)))
#define B_SIZE(b)  (*(u32 *)(g_mem + (b) + 4))
#define B_NEXTF(b) (*(u32 *)(g_mem + (b) + 8))
#define B_PREVF(b) (*(u32 *)(g_mem + (b) + 12))

static inline u32 bsize(gptr b) { return B_SIZE(b) & ~3u; }
static inline int fls32(u32 v)  { return 31 - __builtin_clz(v); }

static inline void mapping(u32 size, u32 *fl, u32 *sl)
{
    if (size < SMALL) { *fl = 0; *sl = size >> 3; return; }
    int f = fls32(size);
    *sl = (size >> (f - SL_LOG2)) ^ SL_COUNT;
    *fl = (u32)(f - FL_SHIFT);
}

static void fl_remove(gptr b)
{
    u32 fl, sl;
    mapping(bsize(b), &fl, &sl);
    gptr n = B_NEXTF(b), p = B_PREVF(b);
    if (n) B_PREVF(n) = p;
    if (p) B_NEXTF(p) = n;
    else {
        heads[fl][sl] = n;
        if (!n && !(sl_bitmap[fl] &= ~(1u << sl)))
            fl_bitmap &= ~(1u << fl);
    }
}

static void fl_insert(gptr b)
{
    u32 fl, sl;
    mapping(bsize(b), &fl, &sl);
    gptr h = heads[fl][sl];
    B_NEXTF(b) = h;
    B_PREVF(b) = 0;
    if (h) B_PREVF(h) = b;
    heads[fl][sl] = b;
    fl_bitmap |= 1u << fl;
    sl_bitmap[fl] |= 1u << sl;
}

/* b is not in any list; merge with free neighbours and insert */
static void release(gptr b, u32 size)
{
    if (B_SIZE(b) & F_PREVFREE) {
        gptr p = B_PREV(b);
        fl_remove(p);
        size += bsize(p);
        b = p;
    }
    gptr n = b + size;
    if (B_SIZE(n) & F_FREE) {
        fl_remove(n);
        size += bsize(n);
        n = b + size;
    }
    B_SIZE(b) = size | F_FREE | (B_SIZE(b) & F_PREVFREE);
    B_PREV(n) = b;
    B_SIZE(n) |= F_PREVFREE;
    fl_insert(b);
}

/* shrink used block b to `size`, freeing the tail */
static void trim(gptr b, u32 size)
{
    u32 total = bsize(b);
    if (total - size < MIN_BLOCK) return;
    B_SIZE(b) = size | (B_SIZE(b) & F_PREVFREE);
    gptr r = b + size;
    B_PREV(r) = b;
    B_SIZE(r) = total - size;
    release(r, total - size);
}

static bool grow(u32 need)
{
    if (!heap_end) {
        sentinel = GUEST_HEAP_BASE;
        B_PREV(sentinel) = 0;
        B_SIZE(sentinel) = 0;
        heap_end = sentinel + HDR;
    }
    /* slack so the new block lands in a class find() will search */
    u32 incr = (need + (need >> SL_LOG2) + HDR + 0xFFFFu) & ~0xFFFFu;
    if (heap_end + incr > GUEST_HEAP_MAX || heap_end + incr < heap_end) return false;
    gptr b = sentinel;
    heap_end += incr;
    sentinel = b + incr;
    B_SIZE(sentinel) = 0;
    B_SIZE(b) = incr | (B_SIZE(b) & F_PREVFREE);
    release(b, incr);
    return true;
}

static inline u32 req_size(u32 n)
{
    u32 s = (n + HDR + 7) & ~7u;
    return s < MIN_BLOCK ? MIN_BLOCK : s;
}

static gptr find(u32 size)
{
    u32 fl, sl;
    if (size >= SMALL) size += (1u << (fls32(size) - SL_LOG2)) - 1;
    mapping(size, &fl, &sl);
    if (fl >= FL_COUNT) return 0;
    u32 m = sl_bitmap[fl] & (~0u << sl);
    if (!m) {
        u32 fm = fl_bitmap & (~0u << (fl + 1));
        if (!fm) return 0;
        fl = (u32)__builtin_ctz(fm);
        m = sl_bitmap[fl];
    }
    return heads[fl][__builtin_ctz(m)];
}

static gptr take(u32 size)
{
    gptr b = find(size);
    if (!b) {
        if (!grow(size) || !(b = find(size))) return 0;
    }
    fl_remove(b);
    B_SIZE(b) &= ~F_FREE;
    B_SIZE(b + bsize(b)) &= ~F_PREVFREE;
    trim(b, size);
    return b;
}

gptr guest_malloc(u32 n)
{
    if (n > 0x7FFFFFF0u) return 0;
    pthread_mutex_lock(&heap_lock);
    gptr b = take(req_size(n));
    if (b) heap_used += bsize(b);
    pthread_mutex_unlock(&heap_lock);
    return b ? b + HDR : 0;
}

void guest_free(gptr p)
{
    if (!p) return;
    gptr b = p - HDR;
    pthread_mutex_lock(&heap_lock);
    if (__builtin_expect(B_SIZE(b) & F_FREE, 0)) {
        pthread_mutex_unlock(&heap_lock);
        LOG("[heap] double free of %08x ignored\n", p);
        return;
    }
    heap_used -= bsize(b);
    release(b, bsize(b));
    pthread_mutex_unlock(&heap_lock);
}

u64 guest_heap_in_use(void);
u32 guest_usable_size(gptr p) { return p ? bsize(p - HDR) - HDR : 0; }

gptr guest_realloc(gptr p, u32 n)
{
    if (!p) return guest_malloc(n);
    if (!n) { guest_free(p); return 0; }
    gptr b = p - HDR;
    u32 want = req_size(n);
    pthread_mutex_lock(&heap_lock);
    u32 have = bsize(b);
    if (want <= have) { trim(b, want); pthread_mutex_unlock(&heap_lock); return p; }
    gptr nx = b + have;
    if ((B_SIZE(nx) & F_FREE) && have + bsize(nx) >= want) {   /* grow in place */
        fl_remove(nx);
        u32 total = have + bsize(nx);
        B_SIZE(b) = total | (B_SIZE(b) & F_PREVFREE);
        B_PREV(b + total) = b;
        B_SIZE(b + total) &= ~F_PREVFREE;
        trim(b, want);
        pthread_mutex_unlock(&heap_lock);
        return p;
    }
    pthread_mutex_unlock(&heap_lock);
    gptr np = guest_malloc(n);
    if (!np) return 0;
    memcpy(g2h(np), g2h(p), have - HDR);
    guest_free(p);
    return np;
}

gptr guest_memalign(u32 align, u32 n)
{
    if (align <= 8) return guest_malloc(n);
    if (align & (align - 1)) return 0;
    u32 size = req_size(n);
    pthread_mutex_lock(&heap_lock);
    gptr b = take(size + align + MIN_BLOCK);
    if (!b) { pthread_mutex_unlock(&heap_lock); return 0; }
    gptr p = (b + HDR + align - 1) & ~(align - 1);
    if (p - HDR != b) {
        if (p - HDR - b < MIN_BLOCK) p += align;
        gptr nb = p - HDR;
        u32 lead = nb - b, total = bsize(b);
        B_SIZE(nb) = total - lead;
        B_PREV(b + total) = nb;
        B_SIZE(b) = lead | (B_SIZE(b) & F_PREVFREE);
        release(b, lead);
        b = nb;
    }
    trim(b, size);
    pthread_mutex_unlock(&heap_lock);
    return p;
}

/* ------------------------------------------------------------------ */

static void hle_malloc(cpu_t *c)  { hret(c, guest_malloc(harg(c, 0))); }
static void hle_calloc(cpu_t *c)
{
    u64 total = (u64)harg(c, 0) * harg(c, 1);
    gptr p = total > 0x7FFFFFF0u ? 0 : guest_malloc((u32)total);
    if (p) memset(g2h(p), 0, (u32)total);
    hret(c, p);
}
static void hle_realloc(cpu_t *c) { hret(c, guest_realloc(harg(c, 0), harg(c, 1))); }
static void hle_free(cpu_t *c)    { guest_free(harg(c, 0)); }
static void hle_memalign(cpu_t *c) { hret(c, guest_memalign(harg(c, 0), harg(c, 1))); }
static void hle_posix_memalign(cpu_t *c)
{
    gptr out = harg(c, 0);
    gptr p = guest_memalign(harg(c, 1), harg(c, 2));
    if (out) st32(out, p);
    hret(c, p ? 0 : 12 /* ENOMEM */);
}
static void hle_malloc_usable_size(cpu_t *c) { hret(c, guest_usable_size(harg(c, 0))); }

static gptr raw_brk = GUEST_SBRK_BASE;
static void hle_sbrk(cpu_t *c)
{
    s32 incr = (s32)harg(c, 0);
    pthread_mutex_lock(&heap_lock);
    gptr old = raw_brk;
    if ((s64)old + incr > GUEST_SBRK_MAX || (s64)old + incr < GUEST_SBRK_BASE) old = (u32)-1;
    else raw_brk += incr;
    pthread_mutex_unlock(&heap_lock);
    hret(c, old);
}
static void hle_brk(cpu_t *c) { hret(c, (u32)-1); }

static void hle_mmap(cpu_t *c)
{
    u32 len = harg(c, 1);
    int fd = (int)harg(c, 4);
    u32 offset = harg(c, 5);
    gptr a = guest_mmap_pages((len + 4095) / 4096);
    if (!a) { hret(c, (u32)-1); return; }
    if (fd >= 0) emu_prefault(g2h(a), len);
    if (fd >= 0 && emu_pread(fd, g2h(a), len, offset) < 0)
        VLOG(1, "[hle] mmap fd=%d read failed\n", fd);
    hret(c, a);
}
void hle_mmap_entry(cpu_t *c) { hle_mmap(c); }
static void hle_munmap(cpu_t *c)
{
    guest_munmap_pages(harg(c, 0), (harg(c, 1) + 4095) / 4096);
    hret(c, 0);
}
static void hle_mprotect(cpu_t *c) { hret(c, 0); }
static void hle_madvise(cpu_t *c) { hret(c, 0); }

void malloc_hle_init(void)
{
    hle_register("malloc", hle_malloc);
    hle_register("calloc", hle_calloc);
    hle_register("realloc", hle_realloc);
    hle_register("free", hle_free);
    hle_register("memalign", hle_memalign);
    hle_register("posix_memalign", hle_posix_memalign);
    hle_register("malloc_usable_size", hle_malloc_usable_size);
    hle_register("dlmalloc_usable_size", hle_malloc_usable_size);
    hle_register("sbrk", hle_sbrk);
    hle_register("brk", hle_brk);
    hle_register("mmap", hle_mmap);
    hle_register("munmap", hle_munmap);
    hle_register("mprotect", hle_mprotect);
    hle_register("madvise", hle_madvise);
}
