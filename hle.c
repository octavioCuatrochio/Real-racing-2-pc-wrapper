/*
 * hle.c - HLE function registry + divert from the cpu run loop.
 * Slot 0 = magic return. Imported names get slots; unknown names get a
 * log-once stub returning 0 (bring-up discovers the real HLE surface).
 */
#include "emu.h"

#define HLE_MAX 4096

static hle_fn      hle_fns[HLE_MAX];
static const char *hle_names[HLE_MAX];
static int         hle_n = 1;   /* slot 0 = magic return */
static pthread_mutex_t hle_lock = PTHREAD_MUTEX_INITIALIZER;

void hle_register(const char *name, hle_fn fn)
{
    for (int i = 1; i < hle_n; i++)
        if (!strcmp(hle_names[i], name)) { hle_fns[i] = fn; return; }
    pthread_mutex_lock(&hle_lock);
    if (hle_n >= HLE_MAX) fatal("hle table full");
    hle_names[hle_n] = name;
    hle_fns[hle_n] = fn;
    hle_n++;
    pthread_mutex_unlock(&hle_lock);
}

/* register `name` with the handler already registered as `existing` */
bool hle_alias(const char *name, const char *existing)
{
    for (int i = 1; i < hle_n; i++)
        if (!strcmp(hle_names[i], existing)) { hle_register(name, hle_fns[i]); return true; }
    return false;
}

static void hle_stub(cpu_t *c)
{
    u32 idx = (c->r[15] - HLE_SLOT_BASE) / HLE_SLOT_STRIDE;
    if (idx < (u32)hle_n) {
        static char reported[HLE_MAX];
        if (!reported[idx]) {
            reported[idx] = 1;
            LOG("[hle] unimplemented: %s (from pc=%08x)\n", hle_names[idx], c->r[14] - 4);
        }
    }
    hret(c, 0);
}

/* a real implementation (not a stub) is registered under this name */
bool hle_has(const char *name)
{
    for (int i = 1; i < hle_n; i++)
        if (!strcmp(hle_names[i], name)) return hle_fns[i] != hle_stub;
    return false;
}

/* used by the ELF loader: every imported symbol gets a slot address */
u32 hle_bind(const char *name)
{
    for (int i = 1; i < hle_n; i++)
        if (!strcmp(hle_names[i], name))
            return HLE_SLOT_BASE + i * HLE_SLOT_STRIDE;
    pthread_mutex_lock(&hle_lock);
    for (int i = 1; i < hle_n; i++)   /* recheck under lock */
        if (!strcmp(hle_names[i], name)) {
            pthread_mutex_unlock(&hle_lock);
            return HLE_SLOT_BASE + i * HLE_SLOT_STRIDE;
        }
    if (hle_n >= HLE_MAX) fatal("hle table full");
    int idx = hle_n++;
    hle_names[idx] = strdup(name);
    hle_fns[idx] = hle_stub;
    pthread_mutex_unlock(&hle_lock);
    VLOG(1, "[hle] bound stub for unknown import: %s\n", name);
    return HLE_SLOT_BASE + idx * HLE_SLOT_STRIDE;
}

/* imports that are still bound to the log-and-return-0 stub */
void hle_dump_stubs(void)
{
    int n = 0;
    for (int i = 1; i < hle_n; i++)
        if (hle_fns[i] == hle_stub) { LOG("%s%s", n % 8 ? " " : "\n[hle] stubs:", hle_names[i]); n++; }
    LOG("\n[hle] %d imports have no implementation\n", n);
}

const char *hle_slot_name(u32 pc)
{
    u32 idx = (pc - HLE_SLOT_BASE) / HLE_SLOT_STRIDE;
    return idx < (u32)hle_n ? hle_names[idx] : "?";
}

void hle_init(void)
{
    hle_names[0] = "<magic-return>";
    libc_init();
    sync_init();
    gles_init();
}

/* returns true = stop the run loop (magic return slot) */
bool hle_divert(cpu_t *c, u32 pc)
{
    u32 idx = (pc - HLE_SLOT_BASE) / HLE_SLOT_STRIDE;
    if (idx == 0) { c->exit_loop = 1; return true; }
    if (__builtin_expect(idx >= (u32)hle_n || !hle_fns[idx], 0))
        emu_trap(c, "bad HLE slot %08x", pc);
    VLOG(2, "[call] %s(%08x, %08x, %08x, %08x) from %08x\n", hle_names[idx],
         c->r[0], c->r[1], c->r[2], c->r[3], c->r[14] - 4);
    hle_fns[idx](c);
    cpu_set_pc(c, c->r[14]);   /* return (lr bit0: Thumb caller) */
    return false;
}
