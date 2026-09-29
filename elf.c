/*
 * elf.c - ELF32 (ARM, shared object) loader.
 *
 * Maps PT_LOAD segments at GUEST_LIB_BASE + p_vaddr, walks .dynamic,
 * applies REL relocations (RELATIVE/ABS32/GLOB_DAT/JUMP_SLOT), binds
 * imports to HLE slots (functions) or HLE data objects (like __sF).
 * No lazy PLT resolution: JUMP_SLOT slots are bound eagerly.
 */
#include "emu.h"

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

#define DT_HASH 4
#define DT_STRTAB 5
#define DT_SYMTAB 6
#define DT_STRSZ 10
#define DT_SYMENT 11
#define DT_REL 17
#define DT_RELSZ 18
#define DT_JMPREL 23
#define DT_PLTRELSZ 2
#define DT_INIT_ARRAY 25
#define DT_INIT_ARRAYSZ 27
#define DT_FINI_ARRAY 26
#define DT_FINI_ARRAYSZ 28

#define R_ARM_ABS32 2
#define R_ARM_GLOB_DAT 21
#define R_ARM_JUMP_SLOT 22
#define R_ARM_RELATIVE 23

#define STT_OBJECT 1
#define STT_FUNC 2

static u8 *elf_file;        /* file image in host memory */
static u32 dynsym_off, dynstr_off, dynsym_count;
static gptr init_array, fini_array;
static u32 init_array_sz, fini_array_sz;

gptr hle_data_object(const char *name);   /* hle_libc.c */


/* find dynamic symbol; returns st_value (unbased) or -1, sets *defined */
static long find_dynsym(const char *name, bool *defined, int *stt)
{
    /* linear scan (406 symbols: cheap) */
    for (u32 i = 0; i < dynsym_count; i++) {
        Elf32_Sym s;
        memcpy(&s, elf_file + dynsym_off + i * sizeof(s), sizeof(s));
        const char *n = (const char *)elf_file + dynstr_off + s.name;
        if (!strcmp(n, name)) {
            *defined = (s.shndx != 0);
            *stt = s.info & 0xF;
            return s.value;
        }
    }
    return -1;
}

/* address of a defined symbol in guest space (for relocation addends) */
static u32 sym_guest_value(Elf32_Sym *s)
{
    return G.lib_base + s->value;
}

int elf_load(emu_t *e, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return -1; }
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    fseek(f, 0, SEEK_SET);
    elf_file = malloc(fsz);
    if (fread(elf_file, 1, fsz, f) != (size_t)fsz) fatal("read %s", path);
    fclose(f);

    Elf32_Ehdr eh;
    memcpy(&eh, elf_file, sizeof(eh));
    if (memcmp(elf_file, "\x7f""ELF", 4) || eh.machine != 40)
        fatal("%s: not an ARM ELF32", path);

    e->lib_base = GUEST_LIB_BASE;

    /* map segments */
    u32 lo = ~0u, hi = 0;
    for (int i = 0; i < eh.phnum; i++) {
        Elf32_Phdr ph;
        memcpy(&ph, elf_file + eh.phoff + i * sizeof(ph), sizeof(ph));
        if (ph.type == PT_ARM_EXIDX) {
            e->exidx_base = e->lib_base + ph.vaddr;
            e->exidx_count = ph.memsz / 8;
            continue;
        }
        if (ph.type != PT_LOAD) continue;
        u32 base = e->lib_base + ph.vaddr;
        memcpy(g2h(base), elf_file + ph.offset, ph.filesz);
        memset(g2h(base + ph.filesz), 0, ph.memsz - ph.filesz);
        if (ph.flags & 4 /* PF_R? use X check below */) { }
        if (ph.flags & 1 /* PF_X */) {
            e->text_lo = base;
            e->text_span = ph.memsz;
        }
        if (base < lo) lo = base;
        if (base + ph.memsz > hi) hi = base + ph.memsz;
        VLOG(1, "[elf] LOAD %08x..%08x filesz=%x memsz=%x flags=%x\n",
             base, base + ph.memsz, ph.filesz, ph.memsz, ph.flags);
    }
    e->lib_size = hi - lo;
    LOG("[elf] mapped %08x..%08x (text %08x+%08x, exidx %d entries)\n",
        lo, hi, e->text_lo, e->text_span, e->exidx_count);

    /* dynamic section */
    u32 dyn_off = 0, dyn_filesz = 0;
    for (int i = 0; i < eh.phnum; i++) {
        Elf32_Phdr ph;
        memcpy(&ph, elf_file + eh.phoff + i * sizeof(ph), sizeof(ph));
        if (ph.type == PT_DYNAMIC) { dyn_off = ph.offset; dyn_filesz = ph.filesz; }
    }
    if (!dyn_off) fatal("no PT_DYNAMIC");

    u32 rel_off = 0, rel_sz = 0, jmprel_off = 0, pltrel_sz = 0;
    u32 symtab_off = 0, strtab_off = 0, strsz = 0, hash_off = 0;
    for (u32 p = dyn_off; p < dyn_off + dyn_filesz; p += sizeof(Elf32_Dyn)) {
        Elf32_Dyn d;
        memcpy(&d, elf_file + p, sizeof(d));
        switch (d.tag) {
        case 0: goto dyn_done;
        case DT_SYMTAB: symtab_off = d.val; break;      /* vaddr == offset here (vaddr 0 base) */
        case DT_STRTAB: strtab_off = d.val; break;
        case DT_STRSZ: strsz = d.val; break;
        case DT_HASH: hash_off = d.val; break;
        case DT_REL: rel_off = d.val; break;
        case DT_RELSZ: rel_sz = d.val; break;
        case DT_JMPREL: jmprel_off = d.val; break;
        case DT_PLTRELSZ: pltrel_sz = d.val; break;
        case DT_INIT_ARRAY: init_array = e->lib_base + d.val; break;
        case DT_INIT_ARRAYSZ: init_array_sz = d.val; break;
        case DT_FINI_ARRAY: fini_array = e->lib_base + d.val; break;
        case DT_FINI_ARRAYSZ: fini_array_sz = d.val; break;
        }
    }
dyn_done:
    dynsym_off = symtab_off;
    dynstr_off = strtab_off;
    /* symbol count via hash nchain */
    {
        u32 nchain = 0;
        memcpy(&nchain, elf_file + hash_off + 4, 4);
        dynsym_count = nchain;
    }
    VLOG(1, "[elf] %u dynsyms, strsz %u, rel %uB, jmprel %uB\n",
         dynsym_count, strsz, rel_sz, pltrel_sz);

    /* ---- relocations ---- */
    int nrel[4] = { 0, 0, 0, 0 };
    int unbound = 0;

    /* .rel.dyn (data + text) */
    for (u32 p = 0; p < rel_sz; p += sizeof(Elf32_Rel)) {
        Elf32_Rel r;
        memcpy(&r, elf_file + rel_off + p, sizeof(r));
        u32 type = r.info & 0xFF, symidx = r.info >> 8;
        gptr loc = e->lib_base + r.offset;
        switch (type) {
        case R_ARM_RELATIVE:
            st32(loc, e->lib_base + ld32(loc));
            nrel[0]++;
            break;
        case R_ARM_ABS32: {
            Elf32_Sym s;
            memcpy(&s, elf_file + dynsym_off + symidx * sizeof(s), sizeof(s));
            const char *name = (const char *)elf_file + dynstr_off + s.name;
            u32 S;
            if (s.shndx != 0) S = sym_guest_value(&s);
            else {
                gptr d = hle_data_object(name);
                S = d ? d : hle_bind(name);
            }
            st32(loc, S + ld32(loc));
            nrel[1]++;
            break; }
        case R_ARM_GLOB_DAT: {
            Elf32_Sym s;
            memcpy(&s, elf_file + dynsym_off + symidx * sizeof(s), sizeof(s));
            const char *name = (const char *)elf_file + dynstr_off + s.name;
            u32 S;
            if (s.shndx != 0) S = sym_guest_value(&s);
            else {
                gptr d = hle_data_object(name);
                S = d ? d : hle_bind(name);
            }
            st32(loc, S);
            nrel[2]++;
            break; }
        default:
            VLOG(1, "[elf] unhandled rel type %u at %08x\n", type, loc);
            nrel[3]++;
        }
    }
    /* .rel.plt (jump slots) */
    for (u32 p = 0; p < pltrel_sz; p += sizeof(Elf32_Rel)) {
        Elf32_Rel r;
        memcpy(&r, elf_file + jmprel_off + p, sizeof(r));
        u32 type = r.info & 0xFF, symidx = r.info >> 8;
        gptr loc = e->lib_base + r.offset;
        if (type != R_ARM_JUMP_SLOT) { VLOG(1, "[elf] plt rel type %u?\n", type); continue; }
        Elf32_Sym s;
        memcpy(&s, elf_file + dynsym_off + symidx * sizeof(s), sizeof(s));
        const char *name = (const char *)elf_file + dynstr_off + s.name;
        u32 S;
        if (s.shndx != 0) S = sym_guest_value(&s);
        else S = hle_bind(name);
        if (!S) unbound++;
        st32(loc, S);
        nrel[2]++;
    }
    LOG("[elf] relocations: %d relative, %d abs32, %d glob/jump, %d other\n",
        nrel[0], nrel[1], nrel[2], nrel[3]);
    if (unbound) LOG("[elf] WARNING: %d unbound imports\n", unbound);
    return 0;
}

u32 elf_lookup(const char *name)
{
    bool defined; int stt;
    long v = find_dynsym(name, &defined, &stt);
    if (v < 0 || !defined) return 0;
    return G.lib_base + (u32)v;
}

void elf_run_init_array(cpu_t *c)
{
    for (u32 p = 0; p < init_array_sz; p += 4) {
        u32 fn = ld32(init_array + p);
        if (fn && fn < HLE_SLOT_BASE)
            emu_call(c, fn, 0, NULL);
    }
}

void elf_run_fini_array(cpu_t *c)
{
    for (u32 p = 0; p < fini_array_sz; p += 4) {
        u32 fn = ld32(fini_array + p);
        if (fn && fn < HLE_SLOT_BASE)
            emu_call(c, fn, 0, NULL);
    }
}
