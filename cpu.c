/*
 * cpu.c - interpreter dispatch loop, emu_call (host->guest calls), traps.
 */
#include "fastops.h"
#include <stdarg.h>

handler_t dec_table[4096];

/* hle.c */
extern bool hle_divert(cpu_t *c, u32 pc);

void cpu_branch(cpu_t *c, u32 addr)
{
    c->ring[c->ring_pos = (c->ring_pos + 1) & 63] = addr;
    if (addr & 1)
        emu_trap(c, "branch to %08x: Thumb state not supported", addr);
    if (addr & 2)
        LOG_ONCE("[cpu] warning: branch to halfword-aligned %08x\n", addr);
    c->r[15] = addr;
}

[[noreturn]] void emu_trap(cpu_t *c, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "\n*** cpu trap (tid %d): ", c ? c->tid : -1);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    if (c) {
        fprintf(stderr, "  r0=%08x r1=%08x r2=%08x r3=%08x r4=%08x r5=%08x\n",
                c->r[0], c->r[1], c->r[2], c->r[3], c->r[4], c->r[5]);
        fprintf(stderr, "  r6=%08x r7=%08x r8=%08x r9=%08x r10=%08x r11=%08x\n",
                c->r[6], c->r[7], c->r[8], c->r[9], c->r[10], c->r[11]);
        fprintf(stderr, "  r12=%08x sp=%08x lr=%08x pc=%08x cpsr=%08x fpscr=%08x\n",
                c->r[12], c->r[13], c->r[14], c->r[15], c->cpsr, c->fpscr);
        fprintf(stderr, "  insn@pc-4=%08x  recent branch targets:\n  ",
                c->r[15] >= 4 && c->r[15] - 4 < HLE_SLOT_BASE ? ld32(c->r[15] - 4) : 0);
        for (int i = 0; i < 16; i++) {
            int idx = (c->ring_pos - i) & 63;
            if (c->ring[idx]) fprintf(stderr, "%08x ", c->ring[idx]);
        }
        fprintf(stderr, "\n  (offline: llvm-objdump -d --start-address=0x%x --stop-address=0x%x the .so)\n",
                c->r[15] - 4 - G.lib_base, c->r[15] + 16 - G.lib_base);
    }
    abort();
}

/* slow path: pc outside the text window. returns true to leave cpu_run */
static bool run_slow(cpu_t *c, u32 pc)
{
    if (c->exit_loop) {
        if (c->exit_loop & EXIT_WATCH) {           /* debug watchpoint: write done, re-arm */
            c->exit_loop &= ~EXIT_WATCH;
            watch_rearm(c);
            c->span = G.text_span;
            if (!c->exit_loop) return false;
        }
        return true;
    }
    if (pc >= HLE_SLOT_BASE)
        return hle_divert(c, pc) || c->exit_loop;
    if (pc - G.text_lo < G.text_span) {            /* span was forced to 0 */
        c->span = G.text_span;
        return false;
    }
    emu_trap(c, "wild pc %08x (text %08x+%08x)", pc, G.text_lo, G.text_span);
}

static void cpu_run_trace(cpu_t *c)
{
    for (;;) {
        u32 pc = c->r[15];
        if (pc - G.text_lo >= G.text_span || c->exit_loop) {
            if (run_slow(c, pc)) return;
            continue;
        }
        u32 insn = ld32(pc);
        c->r[15] = pc + 4;
        if (cond_ok(insn >> 28, c->cpsr))
            dec_table[DEC_KEY(insn)](c, insn);
        c->insn_count++;
        LOG("[trace] %08x: %08x  r0=%08x r1=%08x r2=%08x r3=%08x sp=%08x lr=%08x\n",
            pc, insn, c->r[0], c->r[1], c->r[2], c->r[3], c->r[13], c->r[14]);
    }
}

void d_decode(cpu_t *c, const di_t *d);
void d_generic(cpu_t *c, const di_t *d);
void d_hook(cpu_t *c, const di_t *d);

/*
 * Direct-threaded dispatch: every op label ends in its own copy of DISPATCH,
 * so the host predicts each guest op's successor separately.
 */
void cpu_run(cpu_t *c)
{
    static const void *const labels[OP_COUNT] = {
        [OP_decode] = &&L_decode, [OP_generic] = &&L_generic, [OP_hook] = &&L_hook,
#define X(n) [OP_##n] = &&L_##n,
        FAST_OPS(X)
#undef X
    };
    tls_cpu = c;
    if (__builtin_expect(g_verbose >= 3, 0)) { cpu_run_trace(c); return; }
    if (g_jit) {
        c->span = G.text_span;
        if (jit_run(c, run_slow)) return;
    }
    const di_t *const cache = g_icache;
    const u32 lo = G.text_lo;
    const di_t *d;
    u32 pc;
    c->span = G.text_span;

#define DISPATCH() do {                                                        \
        pc = c->r[15];                                                         \
        if (__builtin_expect(pc - lo >= c->span, 0)) goto slow;                \
        d = &cache[(pc - lo) >> 2];                                            \
        c->r[15] = pc + 4;                                                     \
        c->insn_count++;                                                       \
        if (d->cx && !((condx_tab[d->cx] >> (c->cpsr >> 28)) & 1))             \
            goto skip;                                                         \
        goto *labels[d->op];                                                   \
    } while (0)

    DISPATCH();
slow:
    if (run_slow(c, pc)) return;
    DISPATCH();
skip:
    DISPATCH();
L_decode:
    d_decode(c, d);
    DISPATCH();
L_generic:
    d_generic(c, d);
    DISPATCH();
L_hook:
    d_hook(c, d);
    DISPATCH();
#define X(n) L_##n: n(c, d); DISPATCH();
    FAST_OPS(X)
#undef X
#undef DISPATCH
}

/*
 * Call guest function `fn` with up to 8 AAPCS int args. Saves/restores all
 * core regs so HLE functions can reenter the interpreter (qsort comparators,
 * atexit, TLS dtors...). HLE fns must set their r0 return AFTER any emu_call.
 */
u32 emu_call(cpu_t *c, u32 fn, int argc, const u32 *args)
{
    u32 saved_r[16];
    u32 saved_cpsr = c->cpsr;
    memcpy(saved_r, c->r, sizeof(saved_r));

    int nstk = argc > 4 ? argc - 4 : 0;
    u32 sp = ((c->r[13] & ~7u) - 4 * nstk) & ~7u;   /* align first: args[4] must sit at [sp] */
    for (int i = 0; i < nstk; i++) st32(sp + 4 * i, args[4 + i]);
    for (int i = 0; i < argc && i < 4; i++) c->r[i] = args[i];

    c->r[13] = sp;
    c->r[14] = HLE_SLOT_BASE;      /* magic return slot */
    if (fn & 1) fatal("emu_call to thumb fn %08x", fn);
    c->r[15] = fn;

    cpu_run(c);                    /* returns when magic-return fires */

    u32 ret = c->r[0];
    memcpy(c->r, saved_r, sizeof(saved_r));
    c->cpsr = saved_cpsr;
    c->exit_loop = 0;
    return ret;
}
