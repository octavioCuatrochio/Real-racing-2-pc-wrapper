/*
 * patches.c - optional game patches (--patch NAME, or RR2_PATCHES=a,b).
 * Each patch lists (offset in the .so, expected original word, replacement).
 * All words are verified first: on a different game build nothing is written.
 */
#include "emu.h"

typedef struct { u32 off, orig, repl; } patch_word;
typedef struct { const char *name, *desc; const patch_word *w; int n; } patch_t;

static const patch_t patches[] = { { NULL, NULL, NULL, 0 } };

static bool apply_one(const char *name)
{
    for (unsigned i = 0; i < sizeof(patches) / sizeof(patches[0]); i++) {
        const patch_t *pt = &patches[i];
        if (!pt->name || strcmp(pt->name, name)) continue;
        for (int k = 0; k < pt->n; k++)
            if (ld32(G.lib_base + pt->w[k].off) != pt->w[k].orig) {
                LOG("[patch] %s: game code differs at +%x, not applied\n", name, pt->w[k].off);
                return false;
            }
        for (int k = 0; k < pt->n; k++) st32(G.lib_base + pt->w[k].off, pt->w[k].repl);
        LOG("[patch] %s applied (%s)\n", name, pt->desc);
        return true;
    }
    LOG("[patch] unknown patch '%s'. available:", name);
    for (unsigned i = 0; i < sizeof(patches) / sizeof(patches[0]); i++) if (patches[i].name) LOG(" %s", patches[i].name);
    LOG("\n");
    return false;
}

/* host patch points: the game word is replaced by svc #0x52nnnn, and patch_svc emulates it */
#define SVC(id) (0xEF520000u | (id))
enum { SVC_PROFILE = 1, SVC_FOV };

static void svc_install(u32 off, u32 orig, u32 id, const char *what)
{
    if (ld32(G.lib_base + off) != orig) { LOG("[patch] %s: game code differs at +%x, not applied\n", what, off); return; }
    st32(G.lib_base + off, SVC(id));
    LOG("[patch] %s\n", what);
}

void patch_svc(cpu_t *c, u32 id)
{
    u32 *r = c->r;
    switch (id) {
    case SVC_PROFILE:                      /* was mov r3, #0x18400, after the profile is loaded or defaulted */
        r[3] = 0x18400;
        if (G.no_tilt) st8(r[4] + 0x1871d, 0);                          /* horizon tilt */
        if (G.no_assists) {
            st8(r[4] + 0x1871e, 0);                                      /* steering assist */
            st8(r[4] + 0x18756, 0);                                      /* anti-skid */
            st32(r[4] + 0x18738, 0);                                     /* brake assist (float 0) */
        }
        break;
    case SVC_FOV: {                        /* was ldr r1, [r4, r2]: the race camera's FOV for SetFov */
        static u32 saved_near;             /* racecam+0x1844 near plane (20), +0x1848 far */
        u32 v = ld32(r[4] + r[2]);
        if (ld32(r[4] + 0x1840) == 1) {    /* interior camera: wider view, near plane pulled in so the cabin isn't cut */
            f32 f; memcpy(&f, &v, 4);
            f += (f32)G.cockpit_fov;
            memcpy(&v, &f, 4);
            u32 n = ld32(r[4] + 0x1844), near4 = 0x40800000u;
            if (n != near4) { saved_near = n; st32(r[4] + 0x1844, near4); }
        } else if (saved_near) {
            st32(r[4] + 0x1844, saved_near);
            saved_near = 0;
        }
        r[1] = v;
        break; }
    }
}

void patches_setup(void)
{
    if (G.no_assists || G.no_tilt) svc_install(0xe8c70, 0xE3A03B61u, SVC_PROFILE, "profile: assists/horizon tilt overridden");
    if (G.cockpit_fov) svc_install(0x106924, 0xE7941002u, SVC_FOV, "cockpit FOV offset");
}

/* list is comma separated; call after the .so is loaded and before any code runs */
void patches_apply(const char *list)
{
    if (!list || !*list) return;
    char buf[512];
    snprintf(buf, sizeof(buf), "%s", list);
    for (char *t = strtok(buf, ","); t; t = strtok(NULL, ",")) apply_one(t);
}
