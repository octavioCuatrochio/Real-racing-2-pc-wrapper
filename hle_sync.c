/*
 * hle_sync.c - pthreads/semaphores/atomics/C++ guards/setjmp for the guest.
 *
 * Guest sync objects (pthread_mutex_t etc.) are 4-byte words in old Bionic;
 * we keep host objects in side tables keyed by guest address, lazily created
 * when the guest word is zero (covers PTHREAD_*_INITIALIZER and zeroed
 * memcpy'd objects; a memcpy of an initialized object yields a fresh one).
 *
 * Each guest thread is a real host thread with its own cpu_t and guest stack.
 */
#include "emu.h"
#include <unistd.h>
#include <semaphore.h>
#include <time.h>

/* ================================================================== */
/* guest threads                                                      */
/* ================================================================== */

#define GUEST_TSTACK_SIZE (1u << 20)

static pthread_mutex_t tstack_lock = PTHREAD_MUTEX_INITIALIZER;
static gptr tstack_next = GUEST_TSTACK_BASE;

static gptr tstack_alloc(u32 size)
{
    pthread_mutex_lock(&tstack_lock);
    if (tstack_next - size < 0x60000000u) {
        pthread_mutex_unlock(&tstack_lock);
        return 0;
    }
    tstack_next -= size;
    gptr top = tstack_next + size;
    pthread_mutex_unlock(&tstack_lock);
    return top;
}

typedef struct { cpu_t *cpu; u32 start; u32 arg; } gthread_boot;

static void tls_run_destructors(cpu_t *c);

static void *gthread_main(void *p)
{
    gthread_boot *b = p;
    cpu_t *c = b->cpu;
    u32 start = b->start, arg = b->arg;
    free(b);

    c->r[0] = arg;
    c->r[14] = HLE_SLOT_BASE;      /* magic return: start_routine return ends thread */
    c->r[15] = start;
    cpu_run(c);

    c->thread_ret = c->r[0];
    tls_run_destructors(c);
    VLOG(1, "[thread] guest thread %d (%s) exited\n", c->tid, c->name);
    emu_free_cpu(c);
    return NULL;
}

static void hle_pthread_create(cpu_t *c)
{
    gptr out = harg(c, 0);
    gptr attr = harg(c, 1);
    u32 start = harg(c, 2);
    u32 arg = harg(c, 3);

    /* r7 bionic pthread_attr_t: flags@0, stack_base@4, stack_size@8 */
    gptr stack_base = 0;
    u32 stack_size = GUEST_TSTACK_SIZE;
    if (attr) {
        u32 sb = ld32(attr + 4), ss = ld32(attr + 8);
        if (sb) { stack_base = sb; stack_size = ss ? ss : GUEST_TSTACK_SIZE; }
    }
    gptr stack_top;
    if (stack_base) {
        stack_top = stack_base + stack_size;
    } else {
        stack_top = tstack_alloc(stack_size);
        if (!stack_top) { hret(c, 12 /* ENOMEM */); return; }
    }

    cpu_t *nc = emu_new_cpu();
    nc->r[13] = stack_top & ~7u;
    nc->stack_lo = stack_top - stack_size;
    nc->stack_hi = stack_top;
    snprintf(nc->name, sizeof(nc->name), "guest%d", nc->tid);

    gthread_boot *b = malloc(sizeof(*b));
    b->cpu = nc; b->start = start; b->arg = arg;
    pthread_t th;
    if (pthread_create(&th, NULL, gthread_main, b) != 0) {
        free(b);
        emu_free_cpu(nc);
        hret(c, 11 /* EAGAIN */);
        return;
    }
    nc->host_thread = (void *)th;
    pthread_detach(th);
    if (out) st32(out, (u32)nc->tid + 1);
    VLOG(1, "[thread] created guest thread %d (start %08x, stack %08x)\n",
         nc->tid, start, stack_top);
    hret(c, 0);
}

/* run guest fn on a fresh guest thread (e.g. Android's UI thread); *done set on return */
typedef struct { cpu_t *cpu; u32 fn; int argc; u32 args[8]; volatile int *done; } async_call;
static void *async_main(void *p)
{
    async_call *a = p;
    emu_call(a->cpu, a->fn, a->argc, a->args);
    __atomic_store_n(a->done, 1, __ATOMIC_RELEASE);
    emu_free_cpu(a->cpu);
    free(a);
    return NULL;
}
bool guest_async_call(u32 fn, int argc, const u32 *args, volatile int *done)
{
    gptr top = tstack_alloc(GUEST_TSTACK_SIZE);
    if (!top || argc > 8) return false;
    async_call *a = calloc(1, sizeof(*a));
    a->cpu = emu_new_cpu();
    a->cpu->r[13] = top & ~7u;
    a->cpu->stack_lo = top - GUEST_TSTACK_SIZE;
    a->cpu->stack_hi = top;
    snprintf(a->cpu->name, sizeof(a->cpu->name), "ui");
    a->fn = fn; a->argc = argc; a->done = done;
    memcpy(a->args, args, (size_t)argc * 4);
    pthread_t th;
    if (pthread_create(&th, NULL, async_main, a)) { emu_free_cpu(a->cpu); free(a); return false; }
    pthread_detach(th);
    return true;
}

static void hle_pthread_exit(cpu_t *c)
{
    c->thread_ret = harg(c, 0);
    c->exit_loop = 1;
    /* never returns to guest code */
}
static void hle_pthread_self(cpu_t *c) { hret(c, (u32)c->tid + 1); }

static void hle_pthread_attr_init(cpu_t *c)
{
    gptr a = harg(c, 0);
    memset(g2h(a), 0, 32);
    hret(c, 0);
}
static void hle_pthread_attr_destroy(cpu_t *c) { hret(c, 0); }
static void hle_pthread_attr_setstacksize(cpu_t *c)
{
    gptr a = harg(c, 0);
    st32(a + 8, harg(c, 1));
    hret(c, 0);
}
static void hle_pthread_attr_setstack(cpu_t *c)
{
    gptr a = harg(c, 0);
    st32(a + 4, harg(c, 1));
    st32(a + 8, harg(c, 2));
    hret(c, 0);
}
static void hle_pthread_attr_getstack(cpu_t *c)
{
    gptr a = harg(c, 0), base = harg(c, 1), size = harg(c, 2);
    st32(base, ld32(a + 4));
    st32(size, ld32(a + 8));
    hret(c, 0);
}
static void hle_pthread_getattr_np(cpu_t *c)
{
    gptr a = harg(c, 1);
    memset(g2h(a), 0, 32);
    st32(a + 4, c->stack_lo);
    st32(a + 8, c->stack_hi - c->stack_lo);
    hret(c, 0);
}
static void hle_pthread_setschedparam(cpu_t *c) { hret(c, 0); }
static void hle_pthread_getschedparam(cpu_t *c)
{
    gptr p = harg(c, 2);
    if (p) st32(p, 0);
    hret(c, 0);
}

/* ================================================================== */
/* mutexes / conds / sems (keyed by guest address, lazy init)         */
/* ================================================================== */

#define MAX_SYNC 1024

typedef struct { gptr addr; pthread_mutex_t m; int used; } gmutex;
typedef struct { gptr addr; pthread_cond_t cv; int used; } gcond;
typedef struct { gptr addr; sem_t s; int used; } gsem;

static gmutex g_mutexes[MAX_SYNC];
static gcond  g_conds[MAX_SYNC];
static gsem   g_sems[MAX_SYNC];
static pthread_mutex_t sync_lock = PTHREAD_MUTEX_INITIALIZER;

static pthread_mutex_t *mutex_for(gptr addr)
{
    pthread_mutex_lock(&sync_lock);
    for (int i = 0; i < MAX_SYNC; i++)
        if (g_mutexes[i].used && g_mutexes[i].addr == addr) {
            pthread_mutex_unlock(&sync_lock);
            return &g_mutexes[i].m;
        }
    for (int i = 0; i < MAX_SYNC; i++)
        if (!g_mutexes[i].used) {
            g_mutexes[i].used = 1;
            g_mutexes[i].addr = addr;
            pthread_mutex_init(&g_mutexes[i].m, NULL);
            pthread_mutex_unlock(&sync_lock);
            return &g_mutexes[i].m;
        }
    pthread_mutex_unlock(&sync_lock);
    fatal("mutex table full");
}

static pthread_cond_t *cond_for(gptr addr)
{
    pthread_mutex_lock(&sync_lock);
    for (int i = 0; i < MAX_SYNC; i++)
        if (g_conds[i].used && g_conds[i].addr == addr) {
            pthread_mutex_unlock(&sync_lock);
            return &g_conds[i].cv;
        }
    for (int i = 0; i < MAX_SYNC; i++)
        if (!g_conds[i].used) {
            g_conds[i].used = 1;
            g_conds[i].addr = addr;
            pthread_cond_init(&g_conds[i].cv, NULL);
            pthread_mutex_unlock(&sync_lock);
            return &g_conds[i].cv;
        }
    pthread_mutex_unlock(&sync_lock);
    fatal("cond table full");
}

static sem_t *sem_for(gptr addr)
{
    pthread_mutex_lock(&sync_lock);
    for (int i = 0; i < MAX_SYNC; i++)
        if (g_sems[i].used && g_sems[i].addr == addr) {
            pthread_mutex_unlock(&sync_lock);
            return &g_sems[i].s;
        }
    pthread_mutex_unlock(&sync_lock);
    return NULL;   /* sems must be sem_init'd first */
}

static void hle_pthread_mutex_init(cpu_t *c)
{
    pthread_mutexattr_t ma, *map = NULL;
    gptr attr = harg(c, 1);
    if (attr) {
        pthread_mutexattr_init(&ma);
        if ((int)ld32(attr) == 1) pthread_mutexattr_settype(&ma, PTHREAD_MUTEX_RECURSIVE);
        map = &ma;
    }
    pthread_mutex_t *m = mutex_for(harg(c, 0));
    /* re-init in place if attr given (already default-inited by mutex_for) */
    if (attr) pthread_mutex_init(m, map);
    hret(c, 0);
}
static void hle_pthread_mutex_destroy(cpu_t *c) { hret(c, 0); }
static void hle_pthread_mutex_lock(cpu_t *c)    { pthread_mutex_lock(mutex_for(harg(c, 0))); hret(c, 0); }
static void hle_pthread_mutex_trylock(cpu_t *c) { hret(c, (u32)pthread_mutex_trylock(mutex_for(harg(c, 0)))); }
static void hle_pthread_mutex_unlock(cpu_t *c)  { pthread_mutex_unlock(mutex_for(harg(c, 0))); hret(c, 0); }
static void hle_pthread_mutexattr_init(cpu_t *c) { st32(harg(c, 0), 0); hret(c, 0); }
static void hle_pthread_mutexattr_settype(cpu_t *c) { st32(harg(c, 0), harg(c, 1)); hret(c, 0); }
static void hle_pthread_mutexattr_destroy(cpu_t *c) { hret(c, 0); }

static void hle_pthread_cond_init(cpu_t *c)    { (void)cond_for(harg(c, 0)); hret(c, 0); }
static void hle_pthread_cond_destroy(cpu_t *c) { hret(c, 0); }
static void hle_pthread_cond_signal(cpu_t *c)  { pthread_cond_signal(cond_for(harg(c, 0))); hret(c, 0); }
static void hle_pthread_cond_broadcast(cpu_t *c) { pthread_cond_broadcast(cond_for(harg(c, 0))); hret(c, 0); }
static void hle_pthread_cond_wait(cpu_t *c)
{
    pthread_cond_wait(cond_for(harg(c, 0)), mutex_for(harg(c, 1)));
    hret(c, 0);
}
static void hle_pthread_cond_timedwait(cpu_t *c)
{
    gptr ts = harg(c, 2);
    struct timespec t = { .tv_sec = ld32(ts), .tv_nsec = ld32(ts + 4) };
    hret(c, (u32)pthread_cond_timedwait(cond_for(harg(c, 0)), mutex_for(harg(c, 1)), &t));
}

static void hle_sem_init(cpu_t *c)
{
    gptr addr = harg(c, 0);
    u32 val = harg(c, 2);
    pthread_mutex_lock(&sync_lock);
    for (int i = 0; i < MAX_SYNC; i++)
        if (!g_sems[i].used) {
            g_sems[i].used = 1;
            g_sems[i].addr = addr;
            sem_init(&g_sems[i].s, 0, val);
            break;
        }
    pthread_mutex_unlock(&sync_lock);
    hret(c, 0);
}
static void hle_sem_destroy(cpu_t *c) { hret(c, 0); }
static void hle_sem_wait(cpu_t *c)    { sem_wait(sem_for(harg(c, 0))); hret(c, 0); }
static void hle_sem_trywait(cpu_t *c) { hret(c, (u32)sem_trywait(sem_for(harg(c, 0)))); }
static void hle_sem_post(cpu_t *c)    { sem_post(sem_for(harg(c, 0))); hret(c, 0); }
static void hle_sem_getvalue(cpu_t *c)
{
    int v = 0;
    sem_getvalue(sem_for(harg(c, 0)), &v);
    gptr out = harg(c, 1);
    if (out) st32(out, (u32)v);
    hret(c, 0);
}
static void hle_sem_timedwait(cpu_t *c)
{
    gptr ts = harg(c, 1);
    struct timespec t = { .tv_sec = ld32(ts), .tv_nsec = ld32(ts + 4) };
    hret(c, (u32)sem_timedwait(sem_for(harg(c, 0)), &t));
}

/* ================================================================== */
/* pthread_once / TLS keys                                            */
/* ================================================================== */

static pthread_mutex_t once_lock = PTHREAD_MUTEX_INITIALIZER;

static void hle_pthread_once(cpu_t *c)
{
    gptr ctrl = harg(c, 0);
    u32 init = harg(c, 1);
    pthread_mutex_lock(&once_lock);
    if (ld32(ctrl) == 0) {
        st32(ctrl, 2);          /* in progress */
        pthread_mutex_unlock(&once_lock);
        emu_call(c, init, 0, NULL);
        pthread_mutex_lock(&once_lock);
        st32(ctrl, 1);
    } else {
        while (ld32(ctrl) == 2) {   /* someone else is running it */
            pthread_mutex_unlock(&once_lock);
            sched_yield();
            pthread_mutex_lock(&once_lock);
        }
    }
    pthread_mutex_unlock(&once_lock);
    hret(c, 0);
}

#define MAX_TLS_KEYS 128
static void (*g_tls_dtor[MAX_TLS_KEYS])(void *);
static int g_tls_nkeys;
static pthread_mutex_t tls_lock = PTHREAD_MUTEX_INITIALIZER;
/* dtors stored as guest fn ptrs */
static u32 g_tls_dtor_fn[MAX_TLS_KEYS];

static void hle_pthread_key_create(cpu_t *c)
{
    pthread_mutex_lock(&tls_lock);
    int k = g_tls_nkeys < MAX_TLS_KEYS ? g_tls_nkeys++ : -1;
    if (k >= 0) g_tls_dtor_fn[k] = harg(c, 1);
    pthread_mutex_unlock(&tls_lock);
    if (k < 0) { hret(c, 11); return; }
    st32(harg(c, 0), (u32)k);
    hret(c, 0);
}
static void hle_pthread_key_delete(cpu_t *c) { hret(c, 0); }
static void hle_pthread_getspecific(cpu_t *c)
{
    u32 k = harg(c, 0);
    hret(c, k < MAX_TLS_KEYS ? c->tls_values[k] : 0);
}
static void hle_pthread_setspecific(cpu_t *c)
{
    u32 k = harg(c, 0);
    if (k < MAX_TLS_KEYS) c->tls_values[k] = harg(c, 1);
    hret(c, 0);
}

static void tls_run_destructors(cpu_t *c)
{
    for (int round = 0; round < 4; round++) {
        bool any = false;
        for (int k = 0; k < g_tls_nkeys; k++) {
            if (c->tls_values[k] && g_tls_dtor_fn[k]) {
                u32 args[1] = { c->tls_values[k] };
                c->tls_values[k] = 0;
                emu_call(c, g_tls_dtor_fn[k], 1, args);
                any = true;
            }
        }
        if (!any) break;
    }
}

/* ================================================================== */
/* atomics (Bionic __atomic_*)                                        */
/* ================================================================== */

static void hle_atomic_cmpxchg(cpu_t *c)
{
    s32 oldv = (s32)harg(c, 0), newv = (s32)harg(c, 1);
    volatile s32 *p = (volatile s32 *)g2h(harg(c, 2));
    /* bionic: returns 0 on success, 1 on failure */
    hret(c, __sync_bool_compare_and_swap(p, oldv, newv) ? 0 : 1);
}
static void hle_atomic_inc(cpu_t *c)
{
    volatile s32 *p = (volatile s32 *)g2h(harg(c, 0));
    hret(c, (u32)__sync_fetch_and_add(p, 1));   /* bionic returns the old value */
}
static void hle_atomic_dec(cpu_t *c)
{
    volatile s32 *p = (volatile s32 *)g2h(harg(c, 0));
    hret(c, (u32)__sync_fetch_and_sub(p, 1));   /* old value */
}
static void hle_atomic_swap(cpu_t *c)
{
    s32 newv = (s32)harg(c, 0);
    volatile s32 *p = (volatile s32 *)g2h(harg(c, 1));
    hret(c, (u32)__sync_lock_test_and_set(p, newv));   /* old value */
}

/* ================================================================== */
/* C++ runtime bits                                                   */
/* ================================================================== */

static pthread_mutex_t guard_lock = PTHREAD_MUTEX_INITIALIZER;

static void hle_cxa_guard_acquire(cpu_t *c)
{
    gptr g = harg(c, 0);
    /* byte 0: done flag. byte 1: pending (we approximate with the global lock) */
    if (ld8(g)) { hret(c, 0); return; }
    pthread_mutex_lock(&guard_lock);
    if (ld8(g)) {
        pthread_mutex_unlock(&guard_lock);
        hret(c, 0);
        return;
    }
    hret(c, 1);   /* we hold guard_lock until release/abort */
}
static void hle_cxa_guard_release(cpu_t *c)
{
    gptr g = harg(c, 0);
    st8(g, 1);
    pthread_mutex_unlock(&guard_lock);
}
static void hle_cxa_guard_abort(cpu_t *c)
{
    pthread_mutex_unlock(&guard_lock);
}
static void hle_cxa_pure_virtual(cpu_t *c)
{
    emu_trap(c, "__cxa_pure_virtual");
}
static void hle_stack_chk_fail(cpu_t *c)
{
    emu_trap(c, "__stack_chk_fail (stack canary corrupted)");
}
static void hle_cxa_finalize(cpu_t *c) { hret(c, 0); }

#define MAX_ATEXIT 256
static u32 g_atexit_fns[MAX_ATEXIT];
static u32 g_atexit_args[MAX_ATEXIT];
static int g_natexit;

static void hle_aeabi_atexit(cpu_t *c)
{
    if (g_natexit < MAX_ATEXIT) {
        g_atexit_fns[g_natexit] = harg(c, 1);
        g_atexit_args[g_natexit] = harg(c, 0);
        g_natexit++;
    }
    hret(c, 0);
}

void sync_run_atexit(cpu_t *c)
{
    for (int i = g_natexit - 1; i >= 0; i--)
        emu_call(c, g_atexit_fns[i], 1, &g_atexit_args[i]);
}

static void hle_gnu_unwind_find_exidx(cpu_t *c)
{
    gptr pcount = harg(c, 1);
    if (pcount) st32(pcount, (u32)G.exidx_count);
    hret(c, G.exidx_base);
}
static void hle_cxa_begin_cleanup(cpu_t *c)  { LOG_ONCE("[hle] __cxa_begin_cleanup stub\n"); hret(c, 0); }
static void hle_cxa_call_unexpected(cpu_t *c) { emu_trap(c, "__cxa_call_unexpected"); }
static void hle_cxa_type_match(cpu_t *c)     { LOG_ONCE("[hle] __cxa_type_match stub\n"); hret(c, 0); }

/* ================================================================== */
/* setjmp / longjmp (own layout inside bionic's 32-word buffer)       */
/* ================================================================== */

static void hle_setjmp(cpu_t *c)
{
    gptr jb = harg(c, 0);
    for (int i = 0; i < 11; i++)          /* r4..r14 */
        st32(jb + 4 * i, c->r[4 + i]);
    for (int i = 0; i < 8; i++) {         /* d8..d15 */
        st32(jb + 4 * (16 + 2 * i), (u32)c->v.q[8 + i]);
        st32(jb + 4 * (17 + 2 * i), (u32)(c->v.q[8 + i] >> 32));
    }
    hret(c, 0);
}
static void hle_longjmp(cpu_t *c)
{
    gptr jb = harg(c, 0);
    u32 val = harg(c, 1);
    for (int i = 0; i < 11; i++)
        c->r[4 + i] = ld32(jb + 4 * i);
    for (int i = 0; i < 8; i++)
        c->v.q[8 + i] = (u64)ld32(jb + 4 * (16 + 2 * i)) |
                        ((u64)ld32(jb + 4 * (17 + 2 * i)) << 32);
    c->r[0] = val ? val : 1;
    c->r[15] = c->r[14];   /* resume at the setjmp call's return address */
}

/* ================================================================== */

void sync_init(void)
{
    hle_register("pthread_create", hle_pthread_create);
    hle_register("pthread_exit", hle_pthread_exit);
    hle_register("pthread_self", hle_pthread_self);
    hle_register("pthread_attr_init", hle_pthread_attr_init);
    hle_register("pthread_attr_destroy", hle_pthread_attr_destroy);
    hle_register("pthread_attr_setstacksize", hle_pthread_attr_setstacksize);
    hle_register("pthread_attr_setstack", hle_pthread_attr_setstack);
    hle_register("pthread_attr_getstack", hle_pthread_attr_getstack);
    hle_register("pthread_getattr_np", hle_pthread_getattr_np);
    hle_register("pthread_setschedparam", hle_pthread_setschedparam);
    hle_register("pthread_getschedparam", hle_pthread_getschedparam);
    hle_register("pthread_mutex_init", hle_pthread_mutex_init);
    hle_register("pthread_mutex_destroy", hle_pthread_mutex_destroy);
    hle_register("pthread_mutex_lock", hle_pthread_mutex_lock);
    hle_register("pthread_mutex_trylock", hle_pthread_mutex_trylock);
    hle_register("pthread_mutex_unlock", hle_pthread_mutex_unlock);
    hle_register("pthread_mutexattr_init", hle_pthread_mutexattr_init);
    hle_register("pthread_mutexattr_settype", hle_pthread_mutexattr_settype);
    hle_register("pthread_mutexattr_destroy", hle_pthread_mutexattr_destroy);
    hle_register("pthread_cond_init", hle_pthread_cond_init);
    hle_register("pthread_cond_destroy", hle_pthread_cond_destroy);
    hle_register("pthread_cond_signal", hle_pthread_cond_signal);
    hle_register("pthread_cond_broadcast", hle_pthread_cond_broadcast);
    hle_register("pthread_cond_wait", hle_pthread_cond_wait);
    hle_register("pthread_cond_timedwait", hle_pthread_cond_timedwait);
    hle_register("pthread_once", hle_pthread_once);
    hle_register("pthread_key_create", hle_pthread_key_create);
    hle_register("pthread_key_delete", hle_pthread_key_delete);
    hle_register("pthread_getspecific", hle_pthread_getspecific);
    hle_register("pthread_setspecific", hle_pthread_setspecific);

    hle_register("sem_init", hle_sem_init);
    hle_register("sem_destroy", hle_sem_destroy);
    hle_register("sem_wait", hle_sem_wait);
    hle_register("sem_trywait", hle_sem_trywait);
    hle_register("sem_post", hle_sem_post);
    hle_register("sem_getvalue", hle_sem_getvalue);
    hle_register("sem_timedwait", hle_sem_timedwait);

    hle_register("__atomic_cmpxchg", hle_atomic_cmpxchg);
    hle_register("__atomic_inc", hle_atomic_inc);
    hle_register("__atomic_dec", hle_atomic_dec);
    hle_register("__atomic_swap", hle_atomic_swap);

    hle_register("__cxa_guard_acquire", hle_cxa_guard_acquire);
    hle_register("__cxa_guard_release", hle_cxa_guard_release);
    hle_register("__cxa_guard_abort", hle_cxa_guard_abort);
    hle_register("__cxa_pure_virtual", hle_cxa_pure_virtual);
    hle_register("__stack_chk_fail", hle_stack_chk_fail);
    hle_register("__cxa_finalize", hle_cxa_finalize);
    hle_register("__aeabi_atexit", hle_aeabi_atexit);
    hle_register("__gnu_Unwind_Find_exidx", hle_gnu_unwind_find_exidx);
    hle_register("__cxa_begin_cleanup", hle_cxa_begin_cleanup);
    hle_register("__cxa_call_unexpected", hle_cxa_call_unexpected);
    hle_register("__cxa_type_match", hle_cxa_type_match);

    hle_register("setjmp", hle_setjmp);
    hle_register("longjmp", hle_longjmp);
}
