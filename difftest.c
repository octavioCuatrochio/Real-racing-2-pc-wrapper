/*
 * difftest.c - differential CPU fuzzing against Unicorn (QEMU's ARM core).
 *
 *   RR2_UNICORN=/path/libunicorn.so.2 rr2emu --difftest <class> [count] [seed]
 *   classes: t16 t32 tit tneon arm vfp neon all
 *
 * Each trial writes one random instruction (plus an IT prefix for "tit"),
 * random registers (pointers into a scratch area half the time) and random
 * memory into both CPUs, runs it, and compares core registers, flags, FPSCR,
 * D0-D31 and the scratch memory. Encodings Unicorn rejects are skipped.
 * Development tool only; nothing links against Unicorn.
 */
#include "emu.h"
#include <setjmp.h>
#include <dlfcn.h>
#include <sys/mman.h>

#define DT_CODE 0x50000000u
#define DT_DATA 0x60000000u
#define DT_DSZ  0x4000u

typedef void uc_engine;
static int (*uc_open)(int, int, uc_engine **);
static int (*uc_mem_map)(uc_engine *, u64, size_t, u32);
static int (*uc_mem_write)(uc_engine *, u64, const void *, size_t);
static int (*uc_mem_read)(uc_engine *, u64, void *, size_t);
static int (*uc_reg_write)(uc_engine *, int, const void *);
static int (*uc_reg_read)(uc_engine *, int, void *);
static int (*uc_emu_start)(uc_engine *, u64, u64, u64, size_t);
static int (*uc_ctl)(uc_engine *, u32, ...);
static const char *(*uc_strerror)(int);
static int (*uc_close)(uc_engine *);

enum { R_APSR = 1, R_CPSR = 3, R_FPEXC = 4, R_FPSCR = 6, R_LR = 10, R_PC = 11, R_SP = 12, R_D0 = 14, R_R0 = 66 };
static int rid(int i) { return i < 13 ? R_R0 + i : i == 13 ? R_SP : i == 14 ? R_LR : R_PC; }

extern jmp_buf *g_trap_jmp;
extern char g_trap_msg[256];
void thumb_it_clear(void);
void thumb_it_reset(void);

static u64 rs;
static u32 rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (u32)(rs >> 11); }

typedef struct { u32 r[16], cpsr, fpscr; u64 d[32]; u8 mem[DT_DSZ]; } state_t;

static u32 rnd_float(void)
{
    u32 k = rnd() % 10;
    if (k < 6) return (rnd() & 0x807FFFFFu) | ((110 + rnd() % 36) << 23);
    if (k < 8) return rnd();
    static const u32 sp[] = { 0, 0x80000000u, 0x3F800000u, 0xBF800000u, 0x7F800000u, 0x7FC00000u, 1, 0x00400000u, 0x4B000000u, 0x4F000000u };
    return k == 8 ? sp[rnd() % 10] : rnd() % 300;
}

static void make_state(state_t *s, bool thumb)
{
    bool ptrs = rnd() % 10 < 7;
    for (int i = 0; i < 15; i++) {
        if (ptrs) s->r[i] = DT_DATA + 0x1000 + (rnd() % 0x800) * (rnd() % 4 ? 4 : 1);
        else { u32 k = rnd() % 4; s->r[i] = k == 0 ? rnd() % 64 : k == 1 ? (u32)-(s32)(rnd() % 64) : rnd(); }
    }
    s->r[13] = DT_DATA + 0x2000 + (rnd() % 64) * 4;
    if (rnd() % 2) s->r[14] = DT_CODE + 0x800 + (rnd() % 64) * 4 + (thumb ? 1 : 0);
    s->cpsr = (rnd() & 0xF80F0000u) | (thumb ? FLAG_T : 0);
    s->fpscr = rnd() % 4 ? 0 : (rnd() & 0xF8000000u);
    for (int i = 0; i < 32; i++) s->d[i] = rnd_float() | (u64)rnd_float() << 32;
    for (u32 i = 0; i < DT_DSZ; i += 4) { u32 v = rnd(); memcpy(s->mem + i, &v, 4); }
}

/* ---- instruction generators ---- */
static u32 gen_t16(void)
{
    for (;;) {
        u32 h = rnd() & 0xFFFF;
        if ((h >> 11) >= 0x1D) continue;
        if ((h & 0xFF00) == 0xBF00 && (h & 0xF)) continue;       /* IT: only in "tit" */
        return h;
    }
}
/* most of the time, keep r13/r15 out of the usual register fields: those forms are mostly UNPREDICTABLE */
static u32 clean_regs(u32 w, const int *pos, int n)
{
    for (int i = 0; i < n; i++) {
        u32 r = (w >> pos[i]) & 15;
        if ((r == 13 || r == 15) && rnd() % 32) w = (w & ~(15u << pos[i])) | ((rnd() % 13) << pos[i]);
    }
    return w;
}
static u32 gen_t32(void)
{
    u32 h1 = (rnd() & 0x1FFF) | 0xE000;
    if ((h1 >> 11) < 0x1D) h1 |= 0x0800 | (rnd() % 2 ? 0x1000 : 0);
    static const int pos[] = { 16, 12, 8, 0 };
    u32 w = h1 << 16 | (rnd() & 0xFFFF);
    if ((h1 & 0xFE40) == 0xE800) {                     /* ldm/stm: list, not register fields */
        w &= ~0x2000u;                                   /* sp in list: unpredictable */
        if (!(h1 & 0x10)) w &= ~0x8000u;                 /* stm with pc: unpredictable */
        if ((h1 & 0x10) && (w & 0x8000)) w &= ~0x4000u;  /* ldm with pc and lr: unpredictable */
        if ((h1 & 0x20) && (w >> ((h1 & 15))) & 1) w &= ~(1u << (h1 & 15));   /* writeback base in list */
        return w;
    }
    w = clean_regs(w, pos, 4);
    u32 rn = (w >> 16) & 15, rt = (w >> 12) & 15, rt2 = (w >> 8) & 15;
    if ((h1 & 0xFE40) == 0xE840 && ((h1 & 0x20) || !(h1 & 0x100)))          /* ldrd/strd with writeback */
        while (rn == 15 || rt == rn || rt2 == rn) { rn = rnd() % 13; w = (w & ~0xF0000u) | rn << 16; }
    if ((h1 & 0xFE00) == 0xF800 && !(h1 & 0x80) && (w & 0x800) && (w & 0x500) != 0x400 && rt == rn)   /* ld/st imm8 writeback */
        w = (w & ~0xF000u) | (((rn + 1) % 13) << 12);
    if ((h1 & 0xFFF0) == 0xFB80 || (h1 & 0xFFF0) == 0xFBA0 || (h1 & 0xFFF0) == 0xFBC0 || (h1 & 0xFFF0) == 0xFBE0)   /* long mul: RdLo != RdHi */
        if (rt == rt2) w = (w & ~0xF00u) | (((rt + 1) % 13) << 8);
    return w;
}
/* ARM-encoded neon element/structure access: the highest D register it touches (for filtering overflow past d31) */
static bool neon_ls_ok(u32 a)
{
    u32 d = ((a >> 18) & 0x10) | ((a >> 12) & 15), type = (a >> 8) & 15, span;
    if (!(a & (1u << 23))) {
        static const u8 sp[16] = { 4, 7, 4, 4, 3, 5, 3, 1, 2, 3, 2, 0, 0, 0, 0, 0 };
        span = sp[type];
    } else {
        u32 n = ((a >> 8) & 3) + 1, inc = 1;
        if (((a >> 10) & 3) == 3) inc = (a & 0x20) ? 2 : 1;
        else if (((a >> 10) & 3) == 1) inc = (a & 0x20) ? 2 : 1;
        else if (((a >> 10) & 3) == 2) inc = (a & 0x40) ? 2 : 1;
        span = n == 1 && ((a >> 10) & 3) == 3 && (a & 0x20) ? 2 : 1 + (n - 1) * inc;
    }
    return span && d + span <= 32 && ((a >> 16) & 15) != 15;
}
static u32 gen_tneon(void)
{
    u32 w = rnd();
    switch (rnd() % 3) {
    case 0: return (w & 0x10FFFFFFu) | 0xEF000000u;              /* 111U 1111 */
    case 1: for (;;) { u32 v = (rnd() & 0x00EFFFFFu) | 0xF9000000u; if (neon_ls_ok(0xF4000000u | (v & 0x00FFFFFFu))) return v; }
    default: return (w & 0x03FFF0FFu) | 0xEC000000u | ((rnd() % 2 ? 0xAu : 0xBu) << 8);   /* vfp in thumb */
    }
}
/* ARM: steer away from UNPREDICTABLE register overlaps in loads/stores */
static u32 arm_fix(u32 w)
{
    u32 cls = (w >> 25) & 7, rn = (w >> 16) & 15, rt = (w >> 12) & 15;
    bool P = w & (1u << 24), W = w & (1u << 21);
    bool extra = cls == 0 && (w & 0x90) == 0x90 && (w & 0x60);
    if (extra && !(w & 0x100000) && (w & 0x40) && (rt & 1 || rt == 14)) {   /* ldrd/strd: even rt, not r14 */
        rt &= 0xC; w = (w & ~0xF000u) | rt << 12;
    }
    if ((cls == 2 || cls == 3 || extra) && (!P || W))
        while (rn == rt || rn == (rt | 1) || rn == 15) { rn = rnd() % 13; w = (w & ~0xF0000u) | rn << 16; }
    if (cls == 4 && W && (w & (1u << rn))) w &= ~(1u << rn);          /* ldm/stm writeback base in list */
    if (cls == 4 && (w & 0xFFFF) == 0) w |= 1;
    return w;
}
static u32 gen_arm(void)
{
    static const int pos[] = { 16, 12, 8, 0 };
    u32 w = clean_regs(rnd(), pos, 4);
    w = arm_fix(w);
    switch (rnd() % 4) {
    case 0: return arm_fix((w & 0x0FFFFFFFu) | 0xE0000000u);
    case 1: return arm_fix((w & 0x01FFFFFFu) | 0xE6000010u);     /* media */
    case 2: return arm_fix((w & 0x00FFFFFFu) | 0xE1000090u);     /* extra ld/st, exclusives, mul */
    default: return arm_fix((w & 0x0FFFFFFFu) | ((rnd() % 15) << 28));    /* conditional */
    }
}
static u32 gen_vfp(void)
{
    u32 w = rnd();
    u32 cp = rnd() % 2 ? 0xA00 : 0xB00;
    switch (rnd() % 4) {
    case 0: return (w & 0x00FFF0FFu) | 0xEE000000u | cp;                    /* cdp / mcr / mrc */
    case 1: return (w & 0x00FFF0EFu) | 0xEE000A00u | cp;                    /* data processing */
    case 2: return (w & 0x01FFF0FFu) | 0xEC000000u | cp | 0x00800000u;      /* ldc/stc (vldr/vldm) */
    default: return (w & 0x00FFF0FFu) | 0xEE000010u | cp;                    /* vmov core <-> s / scalar, vdup, vmrs */
    }
}
static u32 gen_neon(void)
{
    u32 w = rnd();
    if (rnd() % 4) return (w & 0x01FFFFFFu) | 0xF2000000u;
    for (;;) { u32 v = (rnd() & 0x00EFFFFFu) | 0xF4000000u; if (neon_ls_ok(v)) return v; }
}

/* ---- run one trial on both cpus ---- */
static uc_engine *uc;

static u32 g_until;                     /* nonzero: run to this address instead of counting (IT blocks) */
static int run_uc(state_t *s, u32 pc, bool thumb, int n)
{
    uc_mem_write(uc, DT_DATA, s->mem, DT_DSZ);
    u32 cpsr = (s->cpsr & ~0x1Fu & ~FLAG_T) | 0x10;
    uc_reg_write(uc, R_CPSR, &cpsr);
    u32 fpexc = 0x40000000u; uc_reg_write(uc, R_FPEXC, &fpexc);
    uc_reg_write(uc, R_FPSCR, &s->fpscr);
    for (int i = 0; i < 15; i++) uc_reg_write(uc, rid(i), &s->r[i]);
    for (int i = 0; i < 32; i++) uc_reg_write(uc, R_D0 + i, &s->d[i]);
    int err = g_until ? uc_emu_start(uc, pc | 1, g_until, 1000000, 0)
                      : uc_emu_start(uc, pc | (thumb ? 1 : 0), DT_CODE + 0xFFF0, 0, (size_t)n);
    if (err) return err;
    for (int i = 0; i < 16; i++) uc_reg_read(uc, rid(i), &s->r[i]);
    uc_reg_read(uc, R_CPSR, &s->cpsr);
    uc_reg_read(uc, R_FPSCR, &s->fpscr);
    for (int i = 0; i < 32; i++) uc_reg_read(uc, R_D0 + i, &s->d[i]);
    uc_mem_read(uc, DT_DATA, s->mem, DT_DSZ);
    return 0;
}

static bool run_ours(cpu_t *c, state_t *s, u32 pc, bool thumb, int n)
{
    memcpy(g2h(DT_DATA), s->mem, DT_DSZ);
    memcpy(c->r, s->r, sizeof(c->r));
    c->cpsr = (s->cpsr & ~FLAG_T) | (thumb ? FLAG_T : 0);
    c->fpscr = s->fpscr;
    memcpy(c->v.q, s->d, sizeof(s->d));
    c->r[15] = pc;
    c->excl_addr = ~0u;
    thumb_it_clear();
    jmp_buf jb;
    g_trap_jmp = &jb;
    if (setjmp(jb)) { g_trap_jmp = NULL; return false; }
    for (int i = 0; i < n; i++) {
        u32 p = c->r[15];
        di_t d;
        cpu_decode_at(&d, p, c->cpsr & FLAG_T);
        c->r[15] = p + d.len;
        if (d.cond >= 0xE || ((cond_tab[d.cond] >> (c->cpsr >> 28)) & 1)) d.h(c, &d);
    }
    g_trap_jmp = NULL;
    memcpy(s->r, c->r, sizeof(c->r));
    s->cpsr = c->cpsr;
    s->fpscr = c->fpscr;
    memcpy(s->d, c->v.q, sizeof(s->d));
    memcpy(s->mem, g2h(DT_DATA), DT_DSZ);
    return true;
}

static int diff_report(const state_t *a, const state_t *b, char *out, size_t n)
{
    int k = 0; size_t o = 0;
    for (int i = 0; i < 16; i++)
        if (a->r[i] != b->r[i]) { k++; o += (size_t)snprintf(out + o, n - o, " r%d uc=%08x us=%08x", i, a->r[i], b->r[i]); if (o >= n) return k; }
    u32 m = 0xF80F0020u;
    if ((a->cpsr & m) != (b->cpsr & m)) { k++; o += (size_t)snprintf(out + o, n - o, " cpsr uc=%08x us=%08x", a->cpsr & m, b->cpsr & m); }
    if ((a->fpscr & 0xF8000000u) != (b->fpscr & 0xF8000000u)) { k++; o += (size_t)snprintf(out + o, n - o, " fpscr uc=%08x us=%08x", a->fpscr, b->fpscr); }
    for (int i = 0; i < 32 && o < n; i++)
        if (a->d[i] != b->d[i]) { k++; o += (size_t)snprintf(out + o, n - o, " d%d uc=%016llx us=%016llx", i, (unsigned long long)a->d[i], (unsigned long long)b->d[i]); }
    for (u32 i = 0; i < DT_DSZ && o < n; i++)
        if (a->mem[i] != b->mem[i]) { k++; o += (size_t)snprintf(out + o, n - o, " mem+%x uc=%02x us=%02x", i, a->mem[i], b->mem[i]); break; }
    return k;
}

/* (re)create the oracle; an unpredictable PC write can leave its ARM/Thumb state stuck */
static bool uc_new(void)
{
    if (uc) uc_close(uc);
    uc = NULL;
    if (uc_open(1, 0, &uc)) return false;
    uc_ctl(uc, 7u | (1u << 26) | (1u << 30), 17);                  /* UC_CTL_CPU_MODEL = cortex-a15 */
    uc_mem_map(uc, DT_CODE, 0x10000, 7);
    uc_mem_map(uc, DT_DATA, DT_DSZ, 7);
    if (g_mem) uc_mem_write(uc, DT_CODE, g2h(DT_CODE), 0x10000);
    return true;
}

int difftest_main(int argc, char **argv)
{
    const char *cls = argc > 0 ? argv[0] : "all";
    long count = argc > 1 ? atol(argv[1]) : 100000;
    rs = argc > 2 ? strtoull(argv[2], NULL, 0) : 0x9E3779B97F4A7C15ULL;
    const char *lib = getenv("RR2_UNICORN");
    void *h = lib ? dlopen(lib, RTLD_NOW) : NULL;
    if (!h) { LOG("difftest: set RR2_UNICORN to libunicorn.so.2 (%s)\n", dlerror()); return 2; }
#define SYM(n) *(void **)&n = dlsym(h, #n)
    SYM(uc_open); SYM(uc_mem_map); SYM(uc_mem_write); SYM(uc_mem_read); SYM(uc_reg_write);
    SYM(uc_reg_read); SYM(uc_emu_start); SYM(uc_ctl); SYM(uc_strerror); SYM(uc_close);
    if (!uc_new()) { LOG("uc_open failed\n"); return 2; }

    mprotect(g_mem, GUEST_NULL_LIMIT, PROT_READ | PROT_WRITE);   /* our side must survive wild accesses too */
    G.text_lo = DT_CODE; G.text_span = 0x10000;
    cpu_icache_reset();
    thumb_it_reset();
    cpu_t *c = emu_new_cpu();
    for (u32 i = 0; i < 0x10000; i += 4) { u32 v = rnd(); memcpy(g2h(DT_CODE + i), &v, 4); }
    uc_mem_write(uc, DT_CODE, g2h(DT_CODE), 0x10000);

    long ran = 0, skipped = 0, bad = 0, traps = 0, undefs = 0;
    static state_t s0, su, so;
    for (long t = 0; t < count; t++) {
        const char *k = cls;
        if (!strcmp(cls, "all")) { static const char *all[] = { "t16", "t32", "tit", "tneon", "arm", "vfp", "neon" }; k = all[rnd() % 7]; }
        bool thumb = k[0] == 't';
        u32 code[16];
        for (int i = 0; i < 16; i++) code[i] = rnd();
        u32 insn = 0, pc = DT_CODE + 0x100 + (rnd() % 64) * 4;
        int n = 1;
        u16 *hw = (u16 *)code;
        if (!strcmp(k, "t16")) { insn = gen_t16(); hw[0] = (u16)insn; }
        else if (!strcmp(k, "t32") || !strcmp(k, "tneon")) {
            insn = !strcmp(k, "t32") ? gen_t32() : gen_tneon();
            hw[0] = (u16)(insn >> 16); hw[1] = (u16)insn;
        } else if (!strcmp(k, "tit")) {
            u32 first = rnd() % 14, mask = (rnd() % 15) + 1;
            hw[0] = (u16)(0xBF00 | first << 4 | mask);
            int cnt = 4 - __builtin_ctz(mask), at = 1;
            for (int i = 0; i < cnt; i++) {
                if (rnd() % 3) {
                    u32 h;
                    do h = gen_t16(); while (((h >> 12) >= 0xB && (h >> 12) != 0xC) || (h & 0xFF00) == 0x4700 ||
                                             ((h & 0xFC00) == 0x4400 && (h & 0x87) == 0x87));
                    hw[at++] = (u16)h;
                } else {
                    u32 w;
                    do w = gen_t32(); while (((w >> 27) == 0x1E && (w & 0x8000)) || (w >> 28) == 0xF ? ((w >> 16) & 0xFF00) != 0xF800 && (w >> 27) == 0x1E : false ||
                                             ((w >> 16 & 0xFE50) == 0xE810 && (w & 0x8000)) || ((w >> 16 & 0xFF00) == 0xF800 && (w >> 12 & 15) == 15) ||
                                             ((w >> 16 & 0xFFF0) == 0xE8D0));
                    hw[at++] = (u16)(w >> 16); hw[at++] = (u16)w;
                }
            }
            n = 1 + cnt;
            insn = (u32)hw[0] << 16 | hw[1];
            /* branches inside IT blocks are only legal last: keep it simple, skip blocks with any */
        } else {
            insn = !strcmp(k, "arm") ? gen_arm() : !strcmp(k, "vfp") ? gen_vfp() : gen_neon();
            code[0] = insn;
        }
        memcpy(g2h(DT_CODE), code, 0x100);                            /* scratch below pc too */
        memcpy(g2h(pc), code, sizeof(code));
        uc_mem_write(uc, DT_CODE, code, 0x100);
        uc_mem_write(uc, pc, code, sizeof(code));
        uc_ctl(uc, 10u | (1u << 30));                               /* UC_CTL_TB_FLUSH: code changed */
        cpu_icache_reset();

        make_state(&s0, thumb);
        su = s0; so = s0;
        g_until = 0;
        if (!strcmp(k, "tit")) { u32 len = 0; for (int i = 0; i < n; i++) len += (hw[len / 2] >> 11) >= 0x1D ? 4 : 2; g_until = pc + len; }
        int err = run_uc(&su, pc, thumb, n);
        if (err) { skipped++; uc_mem_read(uc, DT_CODE, g2h(DT_CODE), 0x10000); uc_new(); continue; }
        if (((su.cpsr & FLAG_T) != 0) != thumb) {               /* left in the other state: rebuild after this trial */
            uc_mem_read(uc, DT_CODE, g2h(DT_CODE), 0x10000); uc_new();
        }
        ran++;
        if (!run_ours(c, &so, pc, thumb, n)) {
            uc_mem_read(uc, DT_CODE, g2h(DT_CODE), 0x10000);
            if (!strncmp(g_trap_msg, "undefined", 9) || !strncmp(g_trap_msg, "bkpt", 4) || strstr(g_trap_msg, "empty list")) { undefs++; continue; }
            traps++;
            if (traps <= 40) LOG("TRAP %-5s %08x: %s\n", k, insn, g_trap_msg);
            continue;
        }
        char buf[600];
        if (diff_report(&su, &so, buf, sizeof(buf))) {
            bad++;
            uc_mem_read(uc, DT_CODE, g2h(DT_CODE), 0x10000);           /* a diverging store may have hit code */
            if (bad <= 60) {
                LOG("DIFF %-5s %08x", k, insn);
                if (!strcmp(k, "tit")) for (int i = 1; i < 6; i++) LOG(" %04x", hw[i]);
                LOG(" cpsr0=%08x:%s\n", s0.cpsr, buf);
                if (getenv("DT_VERBOSE")) { LOG("   in:"); for (int i = 0; i < 15; i++) LOG(" r%d=%08x", i, s0.r[i]); LOG(" pc=%08x\n", pc); }
            }
        }
    }
    LOG("[difftest] %s: %ld compared, %ld differ, %ld traps (unimplemented), %ld undefined here only, %ld skipped (oracle rejected)\n",
        cls, ran, bad, traps, undefs, skipped);
    return bad || traps ? 1 : 0;
}
