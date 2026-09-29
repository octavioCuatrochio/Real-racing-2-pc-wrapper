/*
 * elf.c - ELF32 (ARM) shared-object loader, several libraries.
 *
 * The main library is mapped at GUEST_LIB_BASE; its DT_NEEDED libraries that
 * exist next to it (libc++_shared.so, libNimble.so, ...) are loaded after it,
 * recursively; everything else (libc, libm, GLES, ...) is the HLE "system".
 *
 * Symbol binding follows the Android linker's global-then-local order:
 * names the HLE implements win (they stand in for the system libraries that
 * sit in the global group), except the C++ runtime pieces libc++_shared
 * provides itself (exceptions, unwinder, RTTI); otherwise the first guest
 * library in load order that defines the symbol; otherwise an HLE stub.
 * REL relocations only (RELATIVE/ABS32/GLOB_DAT/JUMP_SLOT), bound eagerly.
 */
#include "emu.h"
#include <libgen.h>

typedef struct {
    u32 ident[4]; u16 type, machine; u32 version, entry, phoff, shoff, flags;
    u16 ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} Elf32_Ehdr;
typedef struct { u32 type, offset, vaddr, paddr, filesz, memsz, flags, align; } Elf32_Phdr;
typedef struct { s32 tag; u32 val; } Elf32_Dyn;
typedef struct { u32 name, value, size; u8 info, other; u16 shndx; } Elf32_Sym;
typedef struct { u32 offset, info; } Elf32_Rel;

#define PT_LOAD 1
#define PT_DYNAMIC 2
#define PT_ARM_EXIDX 0x70000001u

#define DT_NEEDED 1
#define DT_PLTRELSZ 2
#define DT_HASH 4
#define DT_STRTAB 5
#define DT_SYMTAB 6
#define DT_INIT 12
#define DT_FINI 13
#define DT_REL 17
#define DT_RELSZ 18
#define DT_JMPREL 23
#define DT_INIT_ARRAY 25
#define DT_FINI_ARRAY 26
#define DT_INIT_ARRAYSZ 27
#define DT_FINI_ARRAYSZ 28

#define R_ARM_ABS32 2
#define R_ARM_GLOB_DAT 21
#define R_ARM_JUMP_SLOT 22
#define R_ARM_RELATIVE 23

#define MAX_LIBS 16

typedef struct {
    char name[64];
    char path[1024];
    u8  *file;
    u32  base, lo, hi;              /* load bias; mapped guest range */
    u32  text_lo, text_hi;
    u32  exidx, nexidx;
    u32  symtab, strtab, nsym, hash; /* file offsets (vaddr == offset in these libs) */
    u32  rel, relsz, jmprel, pltrelsz;
    u32  init, fini, init_array, init_arraysz, fini_array, fini_arraysz;
    int  needed[MAX_LIBS], nneeded; /* indices of guest deps */
    bool inited;
} lib_t;

static lib_t libs[MAX_LIBS];
static int nlibs;

gptr hle_data_object(const char *name);    /* hle_libc.c */
bool hle_has(const char *name);            /* hle.c: a real (non-stub) implementation is registered */

static inline Elf32_Sym symat(const lib_t *l, u32 i)
{
    Elf32_Sym s; memcpy(&s, l->file + l->symtab + i * sizeof(s), sizeof(s)); return s;
}
static inline const char *strat(const lib_t *l, u32 off) { return (const char *)l->file + l->strtab + off; }

static u32 elf_hash(const char *n)
{
    u32 h = 0, g;
    while (*n) { h = (h << 4) + (u8)*n++; g = h & 0xF0000000u; if (g) h ^= g >> 24; h &= ~g; }
    return h;
}

/* defined symbol in one library (DT_HASH lookup): guest address (Thumb functions keep bit0), 0 if none */
static u32 lib_sym(const lib_t *l, const char *name)
{
    u32 nbucket, h = elf_hash(name);
    memcpy(&nbucket, l->file + l->hash, 4);
    if (!nbucket) return 0;
    u32 i;
    memcpy(&i, l->file + l->hash + 8 + 4 * (h % nbucket), 4);
    while (i && i < l->nsym) {
        Elf32_Sym s = symat(l, i);
        if (s.shndx && !strcmp(strat(l, s.name), name)) return l->base + s.value;
        memcpy(&i, l->file + l->hash + 8 + 4 * nbucket + 4 * i, 4);
    }
    return 0;
}

/* C++ runtime symbols that libc++_shared implements itself (the HLE stubs some of them for RR2) */
static bool cxx_runtime_name(const char *n)
{
    if (!strcmp(n, "__cxa_atexit") || !strcmp(n, "__cxa_finalize")) return false;   /* bionic owns these */
    return !strncmp(n, "__cxa_", 6) || !strncmp(n, "_Unwind_", 8) || !strncmp(n, "__gxx_", 6) ||
           !strncmp(n, "__aeabi_unwind", 14) || !strncmp(n, "_ZTI", 4) || !strncmp(n, "_ZTS", 4) ||
           !strncmp(n, "_ZTV", 4) || !strncmp(n, "_ZSt", 4) || !strncmp(n, "_ZNSt", 5) || !strncmp(n, "_ZNKSt", 6);
}
static bool hle_wins(const lib_t *def, const char *name)
{
    (void)def;
    return hle_has(name) && !cxx_runtime_name(name);
}

/* resolve an undefined reference */
static u32 resolve(const char *name, bool *is_hle)
{
    *is_hle = false;
    for (int i = 0; i < nlibs; i++) {
        u32 a = lib_sym(&libs[i], name);
        if (a) { if (!hle_wins(&libs[i], name)) return a; break; }
    }
    gptr d = hle_data_object(name);
    if (d) return d;
    *is_hle = true;
    return hle_bind(name);
}

static int find_lib(const char *name)
{
    for (int i = 0; i < nlibs; i++) if (!strcmp(libs[i].name, name)) return i;
    return -1;
}

/* map the file's segments at `base`, parse .dynamic, load guest deps; returns index */
static int load_one(const char *path, u32 base)
{
    if (nlibs >= MAX_LIBS) fatal("too many libraries");
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return -1; }
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    fseek(f, 0, SEEK_SET);
    u8 *file = malloc((size_t)fsz);
    if (fread(file, 1, (size_t)fsz, f) != (size_t)fsz) fatal("read %s", path);
    fclose(f);

    Elf32_Ehdr eh;
    memcpy(&eh, file, sizeof(eh));
    if (memcmp(file, "\x7f""ELF", 4) || eh.machine != 40) fatal("%s: not an ARM ELF32", path);

    int idx = nlibs++;
    lib_t *l = &libs[idx];
    memset(l, 0, sizeof(*l));
    l->file = file;
    snprintf(l->path, sizeof(l->path), "%s", path);
    { char tmp[1024]; snprintf(tmp, sizeof(tmp), "%s", path); snprintf(l->name, sizeof(l->name), "%s", basename(tmp)); }
    l->base = base;
    l->lo = ~0u;
    u32 dyn_off = 0, dyn_sz = 0;
    for (int i = 0; i < eh.phnum; i++) {
        Elf32_Phdr ph;
        memcpy(&ph, file + eh.phoff + i * sizeof(ph), sizeof(ph));
        if (ph.type == PT_ARM_EXIDX) { l->exidx = base + ph.vaddr; l->nexidx = ph.memsz / 8; }
        if (ph.type == PT_DYNAMIC) { dyn_off = ph.offset; dyn_sz = ph.filesz; }
        if (ph.type != PT_LOAD) continue;
        u32 a = base + ph.vaddr;
        emu_prefault(g2h(a), ph.memsz);
        memcpy(g2h(a), file + ph.offset, ph.filesz);
        memset(g2h(a + ph.filesz), 0, ph.memsz - ph.filesz);
        if (ph.flags & 1) { l->text_lo = a; l->text_hi = a + ph.memsz; }
        if (a < l->lo) l->lo = a;
        if (a + ph.memsz > l->hi) l->hi = a + ph.memsz;
    }
    if (!dyn_off) fatal("%s: no PT_DYNAMIC", path);

    u32 hash = 0, needed_off[MAX_LIBS]; int nneed = 0;
    for (u32 q = dyn_off; q + sizeof(Elf32_Dyn) <= dyn_off + dyn_sz; q += sizeof(Elf32_Dyn)) {
        Elf32_Dyn d;
        memcpy(&d, file + q, sizeof(d));
        if (!d.tag) break;
        switch (d.tag) {
        case DT_NEEDED: if (nneed < MAX_LIBS) needed_off[nneed++] = d.val; break;
        case DT_SYMTAB: l->symtab = d.val; break;
        case DT_STRTAB: l->strtab = d.val; break;
        case DT_HASH: hash = d.val; break;
        case DT_REL: l->rel = d.val; break;
        case DT_RELSZ: l->relsz = d.val; break;
        case DT_JMPREL: l->jmprel = d.val; break;
        case DT_PLTRELSZ: l->pltrelsz = d.val; break;
        case DT_INIT: l->init = base + d.val; break;
        case DT_FINI: l->fini = base + d.val; break;
        case DT_INIT_ARRAY: l->init_array = base + d.val; break;
        case DT_INIT_ARRAYSZ: l->init_arraysz = d.val; break;
        case DT_FINI_ARRAY: l->fini_array = base + d.val; break;
        case DT_FINI_ARRAYSZ: l->fini_arraysz = d.val; break;
        }
    }
    l->hash = hash;
    memcpy(&l->nsym, file + hash + 4, 4);                  /* nchain */
    LOG("[elf] %-22s %08x..%08x (text %08x..%08x, %u syms, exidx %u)\n",
        l->name, l->lo, l->hi, l->text_lo, l->text_hi, l->nsym, l->nexidx);

    /* guest dependencies: same directory as this library */
    char dir[1024];
    { char tmp[1024]; snprintf(tmp, sizeof(tmp), "%s", path); snprintf(dir, sizeof(dir), "%s", dirname(tmp)); }
    for (int i = 0; i < nneed; i++) {
        const char *dn = (const char *)file + libs[idx].strtab + needed_off[i];
        int j = find_lib(dn);
        if (j < 0) {
            char dp[1200];
            snprintf(dp, sizeof(dp), "%s/%s", dir, dn);
            FILE *t = fopen(dp, "rb");
            if (!t) continue;                              /* system library: HLE */
            fclose(t);
            u32 next = 0;
            for (int k = 0; k < nlibs; k++) if (libs[k].hi > next) next = libs[k].hi;
            j = load_one(dp, (next + 0xFFFFu) & ~0xFFFFu);
            if (j < 0) continue;
        }
        libs[idx].needed[libs[idx].nneeded++] = j;
    }
    return idx;
}

static void relocate(int idx)
{
    lib_t *l = &libs[idx];
    int nrel[4] = { 0 }, nhle = 0;
    for (int pass = 0; pass < 2; pass++) {
        u32 off = pass ? l->jmprel : l->rel, sz = pass ? l->pltrelsz : l->relsz;
        for (u32 q = 0; q < sz; q += sizeof(Elf32_Rel)) {
            Elf32_Rel r;
            memcpy(&r, l->file + off + q, sizeof(r));
            u32 type = r.info & 0xFF, si = r.info >> 8;
            gptr loc = l->base + r.offset;
            if (type == R_ARM_RELATIVE) { st32(loc, l->base + ld32(loc)); nrel[0]++; continue; }
            if (type != R_ARM_ABS32 && type != R_ARM_GLOB_DAT && type != R_ARM_JUMP_SLOT) {
                VLOG(1, "[elf] %s: unhandled rel type %u at %08x\n", l->name, type, loc);
                nrel[3]++;
                continue;
            }
            Elf32_Sym s = symat(l, si);
            const char *name = strat(l, s.name);
            bool h = false;
            u32 S;
            if (s.shndx && !hle_wins(l, name)) S = l->base + s.value;   /* own definition */
            else S = resolve(name, &h);
            if (h) nhle++;
            if (type == R_ARM_ABS32) { st32(loc, S + ld32(loc)); nrel[1]++; }
            else { st32(loc, S); nrel[2]++; }
        }
    }
    LOG("[elf] %s: %d relative, %d abs32, %d glob/jump (%d to HLE), %d other\n",
        l->name, nrel[0], nrel[1], nrel[2], nhle, nrel[3]);
}

int elf_load(emu_t *e, const char *path)
{
    nlibs = 0;
    if (load_one(path, GUEST_LIB_BASE) != 0) return -1;
    for (int i = 0; i < nlibs; i++) relocate(i);
    u32 tlo = ~0u, thi = 0, hi = 0;
    for (int i = 0; i < nlibs; i++) {
        if (libs[i].text_lo < tlo) tlo = libs[i].text_lo;
        if (libs[i].text_hi > thi) thi = libs[i].text_hi;
        if (libs[i].hi > hi) hi = libs[i].hi;
    }
    if (hi > GUEST_HEAP_BASE) fatal("libraries end at %08x, past the heap base %08x", hi, GUEST_HEAP_BASE);
    e->lib_base = libs[0].base;
    e->lib_size = libs[0].hi - libs[0].lo;
    e->text_lo = tlo;
    e->text_span = (thi - tlo + 0xFFF) & ~0xFFFu;
    e->exidx_base = libs[0].exidx;
    e->exidx_count = (int)libs[0].nexidx;
    return 0;
}

u32 elf_lookup(const char *name)
{
    for (int i = 0; i < nlibs; i++) { u32 a = lib_sym(&libs[i], name); if (a) return a; }
    return 0;
}

u32 elf_lookup_in(const char *lib, const char *name)
{
    int i = find_lib(lib);
    return i < 0 ? 0 : lib_sym(&libs[i], name);
}

/* exception index table for the library containing pc (__gnu_Unwind_Find_exidx) */
u32 elf_exidx_for(u32 pc, u32 *count)
{
    for (int i = 0; i < nlibs; i++)
        if (pc >= libs[i].lo && pc < libs[i].hi) { *count = libs[i].nexidx; return libs[i].exidx; }
    *count = 0;
    return 0;
}

/* library containing addr: name and load base (dladdr, logging) */
const char *elf_lib_of(u32 addr, u32 *base)
{
    for (int i = 0; i < nlibs; i++)
        if (addr >= libs[i].lo && addr < libs[i].hi) { if (base) *base = libs[i].base; return libs[i].name; }
    return NULL;
}

static void run_init(cpu_t *c, int i)
{
    lib_t *l = &libs[i];
    if (l->inited) return;
    l->inited = true;
    for (int k = 0; k < l->nneeded; k++) run_init(c, l->needed[k]);   /* dependencies first */
    if (l->init) emu_call(c, l->init, 0, NULL);
    for (u32 q = 0; q < l->init_arraysz; q += 4) {
        u32 fn = ld32(l->init_array + q);
        if (fn && fn != 0xFFFFFFFFu && fn < HLE_SLOT_BASE) emu_call(c, fn, 0, NULL);
    }
    VLOG(1, "[elf] %s initialized\n", l->name);
}

void elf_run_init_array(cpu_t *c)
{
    run_init(c, 0);
}

/* System.loadLibrary(name): constructors of name and its deps (once) */
bool elf_run_init_lib(cpu_t *c, const char *name)
{
    int i = find_lib(name);
    if (i < 0) return false;
    run_init(c, i);
    return true;
}

void elf_run_fini_array(cpu_t *c)
{
    for (int i = 0; i < nlibs; i++) {
        lib_t *l = &libs[i];
        for (u32 q = l->fini_arraysz; q >= 4; q -= 4) {
            u32 fn = ld32(l->fini_array + q - 4);
            if (fn && fn != 0xFFFFFFFFu && fn < HLE_SLOT_BASE) emu_call(c, fn, 0, NULL);
        }
    }
}
