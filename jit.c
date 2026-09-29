/*
 * jit.c - ARM -> x86-64 block translator.
 *
 * A block is straight-line guest code up to a branch (or JIT_MAX_INSNS). Hot
 * forms are emitted natively with guest registers kept in cpu_t (rbx) and
 * guest memory addressed off r15 (g_mem). Anything else calls the same
 * handler the interpreter uses (fast op or generic reference), so coverage
 * never affects correctness. Block exits jump through 8-byte link slots:
 * unlinked slots point at the exit stub, and the dispatcher patches a slot to
 * the target block once it exists (an aligned 8-byte store, safe while other
 * threads run). Indirect branches look the target up inline.
 */
#include "fastops.h"
#include <stddef.h>
#include <sys/mman.h>
#include <xmmintrin.h>

#define JIT_CACHE      (64u << 20)
#define JIT_MAX_INSNS  64

typedef void (JITCALL *jit_entry_t)(cpu_t *c, u8 *mem, void *code);

static u8  *jc_base, *jc_ptr, *jc_end;
static void **jit_table;                 /* per text halfword: block code or NULL */
static u32   jt_lo, jt_words;
static jit_entry_t jit_enter;
static u8   *jit_exit_stub;              /* rax = link slot (or 0) */
static u8   *jit_slow_ind;               /* indirect miss: HLE call in place, else exit */
static void emit_slow_indirect_stub(void);
static pthread_mutex_t jit_lock = PTHREAD_MUTEX_INITIALIZER;
static bool  jit_full;
int g_jit = 1;

JITCALL void d_generic(cpu_t *c, const di_t *d);

/* ------------------------------------------------------------------ */
/* x86-64 emitter                                                      */
/* ------------------------------------------------------------------ */

enum { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15 };

static u8 *p;
static inline void e8(u32 v)  { *p++ = (u8)v; }
static inline void e32(u32 v) { memcpy(p, &v, 4); p += 4; }
static inline void e64(u64 v) { memcpy(p, &v, 8); p += 8; }

static inline void rex(int w, int r, int x, int b, bool force)
{
    u8 v = 0x40 | (w << 3) | ((r >> 3) << 2) | ((x >> 3) << 1) | (b >> 3);
    if (v != 0x40 || force) e8(v);
}
static void vc_drop_off(s32 disp, int bytes);
/* op reg, [rbx + disp32] */
static void mrm_rbx(int w, const u8 *op, int nop, int reg, s32 disp)
{
    if (nop == 1 && (op[0] == 0x88 || op[0] == 0x89)) vc_drop_off(disp, w ? 8 : 4);
    if (nop == 2 && op[0] == 0x0F && (op[1] == 0x11 || op[1] == 0x29 || op[1] == 0xD6)) vc_drop_off(disp, 16);
    rex(w, reg, 0, RBX, false);
    for (int i = 0; i < nop; i++) e8(op[i]);
    e8(0x80 | ((reg & 7) << 3) | (RBX & 7));
    e32((u32)disp);
}
/* op reg, reg2 (mod=11) */
static void mrm_rr(int w, const u8 *op, int nop, int reg, int rm, bool force)
{
    rex(w, reg, 0, rm, force);
    for (int i = 0; i < nop; i++) e8(op[i]);
    e8(0xC0 | ((reg & 7) << 3) | (rm & 7));
}
/* op reg, [r15 + idx] (guest memory) */
static void mrm_mem(int w, const u8 *op, int nop, int reg, int idx, bool force, bool pfx66)
{
    if (pfx66) e8(0x66);
    rex(w, reg, idx, R15, force);
    for (int i = 0; i < nop; i++) e8(op[i]);
    e8(0x04 | ((reg & 7) << 3));
    e8(((idx & 7) << 3) | (R15 & 7));
}

#define OFF_R(n)   ((s32)offsetof(cpu_t, r) + 4 * (n))
#define OFF_CPSR   ((s32)offsetof(cpu_t, cpsr))
#define OFF_FPSCR  ((s32)offsetof(cpu_t, fpscr))
#define OFF_S(n)   ((s32)offsetof(cpu_t, v) + 4 * (n))
#define OFF_D(n)   ((s32)offsetof(cpu_t, v) + 8 * (n))
#define OFF_ICNT   ((s32)offsetof(cpu_t, insn_count))
#define OFF_LINK   ((s32)offsetof(cpu_t, jit_link))

static void mov_ri(int x, u32 v)  { rex(0, 0, 0, x, false); e8(0xB8 + (x & 7)); e32(v); }
static void mov_ri64(int x, u64 v) { rex(1, 0, 0, x, false); e8(0xB8 + (x & 7)); e64(v); }
static void mov_rr(int d, int s)  { static const u8 o[] = { 0x89 }; mrm_rr(0, o, 1, s, d, false); }

/* Guest register cache, per block and write-through: r0-r14 stay in host registers once loaded or
 * written, while every store still reaches cpu_t, so exits and handlers see memory as is. Dropped at
 * block start, after any call (handlers may write registers) and at every forward-branch join. */
#define RC_N 4
static const int rc_host[RC_N] = { R12, R13, R14, RBP };
static int rc_greg[RC_N] = { -1, -1, -1, -1 };
static unsigned rc_stamp[RC_N], rc_clock;
static void vc_reset(void);
static void rc_reset(void) { for (int i = 0; i < RC_N; i++) rc_greg[i] = -1; vc_reset(); }
static int rc_greg_of(s32 off)
{
    s32 d = off - (s32)offsetof(cpu_t, r);
    return d >= 0 && d < 15 * 4 && !(d & 3) ? d / 4 : -1;
}
static int rc_find(int g)
{
    for (int i = 0; i < RC_N; i++) if (rc_greg[i] == g) { rc_stamp[i] = ++rc_clock; return i; }
    return -1;
}
static int rc_take(int g)
{
    int i = rc_find(g);
    if (i >= 0) return i;
    i = 0;
    for (int k = 1; k < RC_N; k++) if (rc_greg[k] < 0 || (rc_greg[i] >= 0 && rc_stamp[k] < rc_stamp[i])) i = k;
    rc_greg[i] = g; rc_stamp[i] = ++rc_clock;
    return i;
}
/* NEON q registers the same way, in xmm8-xmm15. Every store into cpu_t.v other than st_vec's
 * drops the entries it overlaps (hooked in mrm_rbx / st_imm). */
#define VC_N 8
static int vc_q[VC_N] = { -1, -1, -1, -1, -1, -1, -1, -1 };
static unsigned vc_stamp[VC_N], vc_clock;
static void vc_reset(void) { for (int i = 0; i < VC_N; i++) vc_q[i] = -1; }
static void vc_drop_off(s32 disp, int bytes)
{
    s32 lo = disp - (s32)offsetof(cpu_t, v), hi = lo + bytes - 1;
    if (hi < 0 || lo >= 256) return;
    for (int i = 0; i < VC_N; i++) if (vc_q[i] >= 0 && vc_q[i] * 16 <= hi && vc_q[i] * 16 + 15 >= lo) vc_q[i] = -1;
}
static int vc_find(int q)
{
    for (int i = 0; i < VC_N; i++) if (vc_q[i] == q) { vc_stamp[i] = ++vc_clock; return i; }
    return -1;
}
static int vc_take(int q)
{
    int i = vc_find(q);
    if (i >= 0) return i;
    i = 0;
    for (int k = 1; k < VC_N; k++) if (vc_q[k] < 0 || (vc_q[i] >= 0 && vc_stamp[k] < vc_stamp[i])) i = k;
    vc_q[i] = q; vc_stamp[i] = ++vc_clock;
    return i;
}

typedef struct { int g[RC_N]; unsigned st[RC_N]; int q[VC_N]; unsigned qs[VC_N]; } rc_snap_t;   /* emitted, then rolled back */
static rc_snap_t rc_save(void)
{
    rc_snap_t v;
    memcpy(v.g, rc_greg, sizeof(v.g)); memcpy(v.st, rc_stamp, sizeof(v.st));
    memcpy(v.q, vc_q, sizeof(v.q)); memcpy(v.qs, vc_stamp, sizeof(v.qs));
    return v;
}
static void rc_restore(const rc_snap_t *v)
{
    memcpy(rc_greg, v->g, sizeof(v->g)); memcpy(rc_stamp, v->st, sizeof(v->st));
    memcpy(vc_q, v->q, sizeof(v->q)); memcpy(vc_stamp, v->qs, sizeof(v->qs));
}
static void rc_drop(int g) { for (int i = 0; i < RC_N; i++) if (rc_greg[i] == g) rc_greg[i] = -1; }

static void ld_r(int x, s32 off)                                    /* mov x32,[rbx+off] */
{
    int g = rc_greg_of(off), i = g >= 0 ? rc_find(g) : -1;
    if (i >= 0) { mov_rr(x, rc_host[i]); return; }
    static const u8 o[] = { 0x8B }; mrm_rbx(0, o, 1, x, off);
    if (g >= 0) mov_rr(rc_host[rc_take(g)], x);
}
static void st_r(int x, s32 off)                                    /* mov [rbx+off],x32 */
{
    static const u8 o[] = { 0x89 }; mrm_rbx(0, o, 1, x, off);
    int g = rc_greg_of(off);
    if (g >= 0) mov_rr(rc_host[rc_take(g)], x);
}
static void st_imm(s32 off, u32 v)
{
    rex(0, 0, 0, RBX, false); e8(0xC7); e8(0x83); e32((u32)off); e32(v);
    vc_drop_off(off, 4);
    int g = rc_greg_of(off);
    if (g >= 0) mov_ri(rc_host[rc_take(g)], v);
}
/* alu d, s: 0 add 1 or 2 adc 3 sbb 4 and 5 sub 6 xor 7 cmp */
static void alu_rr(int k, int d, int s) { u8 o = (u8)(k * 8 + 1); mrm_rr(0, &o, 1, s, d, false); }
static void alu_ri(int k, int d, u32 imm) { rex(0, 0, 0, d, false); e8(0x81); e8(0xC0 | (k << 3) | (d & 7)); e32(imm); }
static void test_rr(int a, int b) { static const u8 o[] = { 0x85 }; mrm_rr(0, o, 1, b, a, false); }
/* shift by imm: 0 rol 1 ror 4 shl 5 shr 7 sar */
static void sh_ri(int k, int d, u8 n) { rex(0, 0, 0, d, false); e8(0xC1); e8(0xC0 | (k << 3) | (d & 7)); e8(n); }
static void not_r(int d) { rex(0, 0, 0, d, false); e8(0xF7); e8(0xD0 | (d & 7)); }
static void imul_rr(int d, int s) { static const u8 o[] = { 0x0F, 0xAF }; mrm_rr(0, o, 2, d, s, false); }
static void setcc(int cc, int d) { static u8 o[2] = { 0x0F, 0 }; o[1] = (u8)(0x90 + cc); mrm_rr(0, o, 2, 0, d, d >= 4); }
static void movzx8(int d, int s) { static const u8 o[] = { 0x0F, 0xB6 }; mrm_rr(0, o, 2, d, s, s >= 4); }
static void bt_rr(int base, int bit) { static const u8 o[] = { 0x0F, 0xA3 }; mrm_rr(0, o, 2, bit, base, false); }
static u8 *jcc32(int cc) { e8(0x0F); e8(0x80 + cc); u8 *at = p; e32(0); return at; }
static u8 *jmp32(void) { e8(0xE9); u8 *at = p; e32(0); return at; }
static void patch32(u8 *at, u8 *target)
{
    s32 rel = (s32)(target - (at + 4)); memcpy(at, &rel, 4);
    if (target == p) rc_reset();                /* a join: the other path never filled the cache */
}
static void call_abs(void *fn) { mov_ri64(RAX, (u64)(uintptr_t)fn); e8(0xFF); e8(0xD0); rc_reset(); }
/* guest memory access, address in idx register (32-bit, zero-extended) */
static void gld32(int d, int idx) { static const u8 o[] = { 0x8B }; mrm_mem(0, o, 1, d, idx, false, false); }
static void gld64(int d, int idx) { static const u8 o[] = { 0x8B }; mrm_mem(1, o, 1, d, idx, false, false); }
static void gldu8(int d, int idx) { static const u8 o[] = { 0x0F, 0xB6 }; mrm_mem(0, o, 2, d, idx, false, false); }
static void gldu16(int d, int idx) { static const u8 o[] = { 0x0F, 0xB7 }; mrm_mem(0, o, 2, d, idx, false, false); }
static void glds8(int d, int idx) { static const u8 o[] = { 0x0F, 0xBE }; mrm_mem(0, o, 2, d, idx, false, false); }
static void glds16(int d, int idx) { static const u8 o[] = { 0x0F, 0xBF }; mrm_mem(0, o, 2, d, idx, false, false); }
static void gst32(int s, int idx) { static const u8 o[] = { 0x89 }; mrm_mem(0, o, 1, s, idx, false, false); }
static void gst64(int s, int idx) { static const u8 o[] = { 0x89 }; mrm_mem(1, o, 1, s, idx, false, false); }
static void gst8(int s, int idx)  { static const u8 o[] = { 0x88 }; mrm_mem(0, o, 1, s, idx, s >= 4, false); }
static void gst16(int s, int idx) { static const u8 o[] = { 0x89 }; mrm_mem(0, o, 1, s, idx, false, true); }
static void ld64_rbx(int x, s32 off) { static const u8 o[] = { 0x8B }; mrm_rbx(1, o, 1, x, off); }
static void st64_rbx(int x, s32 off)
{
    static const u8 o[] = { 0x89 }; mrm_rbx(1, o, 1, x, off);
    int g = rc_greg_of(off);
    if (g >= 0) { rc_drop(g); rc_drop(g + 1); }
}
/* SSE scalar single: op xmm, [rbx+off] (0x10 movss load, 0x58 add, 0x59 mul, 0x5C sub, 0x5E div) */
static void sse_ss(u8 op, int x, s32 off) { e8(0xF3); u8 o[2] = { 0x0F, op }; mrm_rbx(0, o, 2, x, off); }
static void sse_st(int x, s32 off) { e8(0xF3); static const u8 o[] = { 0x0F, 0x11 }; mrm_rbx(0, o, 2, x, off); }
static void sse_rr(u8 op, int d, int s) { e8(0xF3); u8 o[2] = { 0x0F, op }; mrm_rr(0, o, 2, d, s, false); }

static void imul64_rr(int d, int s) { static const u8 o[] = { 0x0F, 0xAF }; mrm_rr(1, o, 2, d, s, false); }
static void movsxd_rbx(int d, s32 off) { static const u8 o[] = { 0x63 }; mrm_rbx(1, o, 1, d, off); }
static void add64_rr(int d, int s) { static const u8 o[] = { 0x01 }; mrm_rr(1, o, 1, s, d, false); }
static void or64_rr(int d, int s) { static const u8 o[] = { 0x09 }; mrm_rr(1, o, 1, s, d, false); }
static void shl64_ri(int d, u8 n) { rex(1, 0, 0, d, false); e8(0xC1); e8(0xE0 | (d & 7)); e8(n); }
static void shr64_ri(int d, u8 n) { rex(1, 0, 0, d, false); e8(0xC1); e8(0xE8 | (d & 7)); e8(n); }
static void movd_x_r(int x, int r) { e8(0x66); static const u8 o[] = { 0x0F, 0x6E }; mrm_rr(0, o, 2, x, r, false); }  /* movd xmm, r32 */
static void movd_r_x(int r, int x) { e8(0x66); static const u8 o[] = { 0x0F, 0x7E }; mrm_rr(0, o, 2, x, r, false); }  /* movd r32, xmm */
static void ucomiss_rr(int a, int b) { static const u8 o[] = { 0x0F, 0x2E }; mrm_rr(0, o, 2, a, b, false); }
static void cvttss2si_r(int r, int x) { e8(0xF3); static const u8 o[] = { 0x0F, 0x2C }; mrm_rr(0, o, 2, r, x, false); }
static void cvtsi2ss_x(int x, int r, int w) { e8(0xF3); static const u8 o[] = { 0x0F, 0x2A }; mrm_rr(w, o, 2, x, r, false); }
static void xorps_rr(int d, int s) { static const u8 o[] = { 0x0F, 0x57 }; mrm_rr(0, o, 2, d, s, false); }

/* ------------------------------------------------------------------ */
/* helpers emitted into blocks                                         */
/* ------------------------------------------------------------------ */

/* guest register -> x86 register (r15 reads fold to the constant pc+8) */
static void get_reg(int x, u32 r, u32 pc) { if (r == 15) mov_ri(x, pc + 8); else ld_r(x, OFF_R(r)); }

/* x86 cc codes */
enum { CC_O = 0, CC_NO, CC_B, CC_AE, CC_E, CC_NE, CC_BE, CC_A, CC_S, CC_NS };

/* cond fail -> jump to returned patch site. cond_tab[cond] is a compile-time constant */
static u8 *emit_cond_skip(u32 cond)
{
    ld_r(RAX, OFF_CPSR);
    sh_ri(5, RAX, 28);
    mov_ri(RCX, cond_tab[cond]);
    bt_rr(RCX, RAX);
    return jcc32(CC_AE);                       /* CF=0: condition false */
}

/* NZ (and optional C, V) from x86 flags of the op just executed; result in eax.
 * cmode: 0 keep C, 1 C=CF, 2 C=!CF, 3 C=constant(cval). vmode: 0 keep V, 1 V=OF */
static void emit_flags(int cmode, int vmode, u32 cval)
{
    if (cmode == 1) setcc(CC_B, R10); else if (cmode == 2) setcc(CC_AE, R10);
    if (vmode) setcc(CC_O, R11);
    test_rr(RAX, RAX);
    setcc(CC_S, R8);
    setcc(CC_E, R9);
    movzx8(R8, R8); sh_ri(4, R8, 31);
    movzx8(R9, R9); sh_ri(4, R9, 30); alu_rr(1, R8, R9);
    u32 keep = 0x3FFFFFFFu;
    if (cmode) {
        keep &= ~FLAG_C;
        if (cmode == 3) { if (cval) alu_ri(1, R8, FLAG_C); }
        else { movzx8(R10, R10); sh_ri(4, R10, 29); alu_rr(1, R8, R10); }
    }
    if (vmode) { keep &= ~FLAG_V; movzx8(R11, R11); sh_ri(4, R11, 28); alu_rr(1, R8, R11); }
    ld_r(RCX, OFF_CPSR);
    alu_ri(4, RCX, keep);
    alu_rr(1, RCX, R8);
    st_r(RCX, OFF_CPSR);
}

/* ARM C -> x86 CF (inv: CF = !C, for sbb borrow semantics) */
static void emit_load_carry(bool inv)
{
    ld_r(RCX, OFF_CPSR);
    sh_ri(5, RCX, 30);                          /* C (bit 29) -> CF */
    if (inv) e8(0xF5);                          /* cmc */
}

/* exits. Slots live in the code cache next to the block */
static u8 *slot_alloc(void)
{
    u8 *s = (u8 *)(((uintptr_t)p + 7) & ~(uintptr_t)7);
    while (p < s) e8(0xCC);
    memcpy(s, &jit_exit_stub, 8);
    p = s + 8;
    return s;
}

typedef struct { u8 *jmp_at; } pending_exit;
static u8 *pend_slot_jmp[4 * JIT_MAX_INSNS + 16];
static int npend;

/* store pc, then jmp [rip+slot] with rax = &slot (the stub records it for linking) */
static void emit_exit_direct(u32 target)
{
    st_imm(OFF_R(15), target);
    e8(0x48); e8(0x8D); e8(0x05); u8 *lea_at = p; e32(0);          /* lea rax,[rip+slot] */
    e8(0xFF); e8(0x25); u8 *jmp_at = p; e32(0);                    /* jmp [rip+slot] */
    pend_slot_jmp[npend++] = lea_at;
    pend_slot_jmp[npend++] = jmp_at;
}
/* side exits: a taken conditional branch leaves mid-block; it takes back the instructions it
 * skipped from insn_count once the block's length is known */
static u8 *side_imm[JIT_MAX_INSNS + 8];
static int side_done[JIT_MAX_INSNS + 8], nside;
static void emit_side_exit(u32 target, int done)
{
    if (nside < JIT_MAX_INSNS + 8) {
        e8(0x48); e8(0x81); e8(0x83); e32((u32)OFF_ICNT); side_imm[nside] = p; side_done[nside++] = done; e32(0);
    }
    emit_exit_direct(target);
}
static void patch_side_exits(int total)
{
    for (int i = 0; i < nside; i++) { s32 v = side_done[i] - total; memcpy(side_imm[i], &v, 4); }
    nside = 0;
}
/* a forward jump over code that always leaves the block: nothing merges, keep the cache */
static void patch32_over_exit(u8 *at) { s32 rel = (s32)(p - (at + 4)); memcpy(at, &rel, 4); }
/* same, for skipped code that leaves both caches as they were */
#define patch32_keep patch32_over_exit

static void flush_slots(void)
{
    for (int i = 0; i < npend; i += 2) {
        u8 *slot = slot_alloc();
        patch32(pend_slot_jmp[i], slot);
        patch32(pend_slot_jmp[i + 1], slot);
    }
    npend = 0;
}

/* eax = target (Thumb state already in cpsr): store, inline table lookup; miss -> slow stub */
static void emit_exit_indirect(void)
{
    st_r(RAX, OFF_R(15));
    alu_ri(5, RAX, jt_lo);
    alu_ri(7, RAX, jt_words * 2);
    u8 *miss2 = jcc32(CC_AE);
    mov_ri64(RDX, (u64)(uintptr_t)jit_table);
    /* mov rcx, [rdx + rax*4]  (rax = byte offset; table has 8-byte entries per 2-byte slot) */
    e8(0x48); e8(0x8B); e8(0x0C); e8(0x82);
    e8(0x48); e8(0x85); e8(0xC9);               /* test rcx, rcx */
    u8 *miss3 = jcc32(CC_E);
    e8(0xFF); e8(0xE1);                         /* jmp rcx */
    u8 *miss = p;
    patch32(miss2, miss); patch32(miss3, miss);
    u8 *j = jmp32(); patch32(j, jit_slow_ind ? jit_slow_ind : jit_exit_stub);
}
/* interworking branch (BX, LoadWritePC, ARM ALUWritePC): eax bit0 selects Thumb */
static void emit_exit_indirect_iw(void)
{
    e8(0xA8); e8(1);                            /* test al, 1 */
    u8 *arm = jcc32(CC_E);
    alu_ri(4, RAX, ~1u);
    ld_r(RCX, OFF_CPSR); alu_ri(1, RCX, FLAG_T); st_r(RCX, OFF_CPSR);
    u8 *go = jmp32();
    patch32(arm, p);
    ld_r(RCX, OFF_CPSR); alu_ri(4, RCX, ~FLAG_T); st_r(RCX, OFF_CPSR);
    patch32(go, p);
    emit_exit_indirect();
}

/* Thumb blocks: fall-through address of the instruction being emitted, and the exact
 * Thumb-decoded entry that handler fallbacks must run (NULL: decode the ARM word) */
static u32 g_next;
static bool g_tmode;
static const di_t *g_fb_di;
/* data-processing immediate the ARM word cannot encode (Thumb modified immediates) */
static bool g_ovr; static u32 g_ovr_v; static int g_ovr_rot;

/* RR2_JIT_STATS=1: count handler calls from JIT code per (handler, op, insn class) */
#include <dlfcn.h>
static struct { void *fn; int op; u32 key; u64 n; } g_js[4096];
static int g_js_on = -1, g_njs;
static void js_dump(void)
{
    u64 tot = 0;
    for (int i = 0; i < g_njs; i++) tot += g_js[i].n;
    fprintf(stderr, "[jit] %llu handler calls from JIT code\n", (unsigned long long)tot);
    for (int r = 0; r < 40; r++) {
        int b = -1;
        for (int i = 0; i < g_njs; i++) if (g_js[i].n && (b < 0 || g_js[i].n > g_js[b].n)) b = i;
        if (b < 0) break;
        Dl_info di;
        const char *nm = dladdr(g_js[b].fn, &di) && di.dli_sname ? di.dli_sname : "?";
        fprintf(stderr, "  %6.2f%%  %-22s op %4d  %08x\n", 100.0 * g_js[b].n / (tot + !tot), nm, g_js[b].op, g_js[b].key);
        g_js[b].n = 0;
    }
}
static void js_count(void *fn, int op, u32 key)
{
    if (g_js_on < 0) { g_js_on = getenv("RR2_JIT_STATS") != NULL; if (g_js_on) atexit(js_dump); }
    if (!g_js_on) return;
    int i;
    for (i = 0; i < g_njs; i++) if (g_js[i].fn == fn && g_js[i].op == op && g_js[i].key == key) break;
    if (i == g_njs) { if (g_njs == 4096) return; g_js[g_njs].fn = fn; g_js[g_njs].op = op; g_js[g_njs].key = key; g_njs++; }
    mov_ri64(RAX, (u64)(uintptr_t)&g_js[i].n);
    e8(0x48); e8(0xFF); e8(0x00);               /* inc qword [rax] */
}

/* fallback: run the interpreter's handler for this instruction */
static void emit_call_handler(cpu_t *dummy, u32 pc, const di_t *src)
{
    (void)dummy;
    di_t *d = malloc(sizeof(*d));               /* lives as long as the code */
    *d = *src;
    js_count((void *)(d->op == OP_generic ? (dfn_t)d_generic : d->h), d->op, ((d->insn >> 25) & 7) >= 6 ? d->insn & 0x0FFF0FF0u : d->insn & 0x0FF000F0u);
    st_imm(OFF_R(15), g_next);
    e8(0x48); e8(0x89); e8(0xDF);               /* mov rdi, rbx */
    mov_ri64(RSI, (u64)(uintptr_t)d);
    dfn_t fn = d->op == OP_generic ? d_generic : d->h;
    call_abs((void *)fn);
}

/* ------------------------------------------------------------------ */
/* native translations. Return false to fall back to a handler call   */
/* ------------------------------------------------------------------ */

/* operand2 of data processing -> RDX. sets *cmode/cval for logical S carry */
static bool dp_operand(u32 insn, u32 pc, int *cmode, u32 *cval)
{
    if (g_ovr && (insn & (1u << 25))) {
        mov_ri(RDX, g_ovr_v);
        if (g_ovr_rot) { *cmode = 3; *cval = g_ovr_v >> 31; } else *cmode = 0;
        return true;
    }
    if (insn & (1u << 25)) {
        u32 imm = insn & 0xFF, rot = ((insn >> 8) & 0xF) * 2;
        u32 v = rot ? (imm >> rot) | (imm << (32 - rot)) : imm;
        mov_ri(RDX, v);
        if (rot) { *cmode = 3; *cval = v >> 31; } else *cmode = 0;
        return true;
    }
    if (insn & (1u << 4)) return false;         /* register-specified shift */
    u32 rm = insn & 0xF, type = (insn >> 5) & 3, sh = (insn >> 7) & 0x1F;
    if (rm == 15 && (type || sh)) return false;
    get_reg(RDX, rm, pc);                       /* rm == 15: constant pc+8 */
    if (type == 0 && sh == 0) { *cmode = 0; return true; }
    if (sh == 0) return false;                  /* LSR/ASR #32, RRX */
    static const int k[4] = { 4, 5, 7, 1 };     /* shl shr sar ror */
    sh_ri(k[type], RDX, (u8)sh);
    *cmode = 1;                                 /* CF = last bit shifted out (ror: bit31) */
    return true;
}

static bool emit_dp(u32 insn, u32 pc)
{
    u32 opc = (insn >> 21) & 0xF, S = (insn >> 20) & 1;
    u32 rn = (insn >> 16) & 0xF, rd = (insn >> 12) & 0xF;
    if (rd == 15) return false;
    int cm = 0; u32 cv = 0;
    if (!dp_operand(insn, pc, &cm, &cv)) return false;
    bool logic = opc <= 1 || opc == 8 || opc == 9 || opc >= 0xC;
    /* the shifter's CF must survive until emit_flags: capture it now */
    if (S && logic && cm == 1) setcc(CC_B, R10);
    if (opc != 0xD && opc != 0xF) get_reg(RAX, rn, pc);
    switch (opc) {
    case 0x0: case 0x8: alu_rr(4, RAX, RDX); break;
    case 0x1: case 0x9: alu_rr(6, RAX, RDX); break;
    case 0xC: alu_rr(1, RAX, RDX); break;
    case 0xD: mov_rr(RAX, RDX); break;
    case 0xE: not_r(RDX); alu_rr(4, RAX, RDX); break;
    case 0xF: not_r(RDX); mov_rr(RAX, RDX); break;
    case 0x2: case 0xA: alu_rr(5, RAX, RDX); break;
    case 0x3: alu_rr(5, RDX, RAX); mov_rr(RAX, RDX); break;
    case 0x4: case 0xB: alu_rr(0, RAX, RDX); break;
    case 0x5: emit_load_carry(false); alu_rr(2, RAX, RDX); break;
    case 0x6: emit_load_carry(true); alu_rr(3, RAX, RDX); break;
    case 0x7: emit_load_carry(true); alu_rr(3, RDX, RAX); mov_rr(RAX, RDX); break;
    }
    if (S || (opc >= 8 && opc <= 11)) {
        if (logic) {
            if (cm == 1) {                      /* C captured in r10b above */
                test_rr(RAX, RAX);
                setcc(CC_S, R8); setcc(CC_E, R9);
                movzx8(R8, R8); sh_ri(4, R8, 31);
                movzx8(R9, R9); sh_ri(4, R9, 30); alu_rr(1, R8, R9);
                movzx8(R10, R10); sh_ri(4, R10, 29); alu_rr(1, R8, R10);
                ld_r(RCX, OFF_CPSR); alu_ri(4, RCX, 0x1FFFFFFFu); alu_rr(1, RCX, R8); st_r(RCX, OFF_CPSR);
            } else emit_flags(cm == 3 ? 3 : 0, 0, cv);
        } else {
            bool sub = opc == 2 || opc == 3 || opc == 6 || opc == 7 || opc == 0xA;
            emit_flags(sub ? 2 : 1, 1, 0);
        }
    }
    if (!(opc >= 8 && opc <= 11)) st_r(RAX, OFF_R(rd));
    return true;
}

static bool emit_mul(u32 insn)
{
    u32 A = (insn >> 21) & 1, S = (insn >> 20) & 1;
    u32 rd = (insn >> 16) & 0xF, rn = (insn >> 12) & 0xF, rs = (insn >> 8) & 0xF, rm = insn & 0xF;
    if (S || rd == 15 || rm == 15 || rs == 15 || (A && rn == 15)) return false;
    ld_r(RAX, OFF_R(rm)); ld_r(RDX, OFF_R(rs)); imul_rr(RAX, RDX);
    if (A) { ld_r(RDX, OFF_R(rn)); alu_rr(0, RAX, RDX); }
    st_r(RAX, OFF_R(rd));
    return true;
}

static bool emit_mull(u32 insn)
{
    u32 U = (insn >> 22) & 1, A = (insn >> 21) & 1, S = (insn >> 20) & 1;
    u32 hi = (insn >> 16) & 0xF, lo = (insn >> 12) & 0xF, rs = (insn >> 8) & 0xF, rm = insn & 0xF;
    if (S || hi == 15 || lo == 15 || hi == lo || rs == 15 || rm == 15) return false;
    if (U) { movsxd_rbx(RAX, OFF_R(rm)); movsxd_rbx(RCX, OFF_R(rs)); }
    else { ld_r(RAX, OFF_R(rm)); ld_r(RCX, OFF_R(rs)); }
    imul64_rr(RAX, RCX);                        /* exact 64-bit product for both signednesses */
    if (A) { ld_r(RDX, OFF_R(lo)); ld_r(RCX, OFF_R(hi)); shl64_ri(RCX, 32); or64_rr(RDX, RCX); add64_rr(RAX, RDX); }
    st_r(RAX, OFF_R(lo));
    shr64_ri(RAX, 32);
    st_r(RAX, OFF_R(hi));
    return true;
}

/* vldm/vstm/vpush/vpop, unrolled. imm8*4 bytes of base adjustment (FSTMX-compatible) */
static bool emit_vldm(u32 insn)
{
    u32 P = (insn >> 24) & 1, U = (insn >> 23) & 1, D = (insn >> 22) & 1, W = (insn >> 21) & 1;
    u32 L = (insn >> 20) & 1, rn = (insn >> 16) & 0xF, vd = (insn >> 12) & 0xF, imm8 = insn & 0xFF;
    bool dbl = ((insn >> 8) & 0xF) == 11;
    if (P == U || rn == 15 || !imm8) return false;
    u32 first = dbl ? (D << 4) | vd : (vd << 1) | D, n = dbl ? imm8 / 2 : imm8;
    if (!n || first + n > 32) return false;
    ld_r(RCX, OFF_R(rn));
    if (P) alu_ri(5, RCX, imm8 * 4);            /* DB: start below the base */
    for (u32 i = 0; i < n; i++) {
        if (i) alu_ri(0, RCX, dbl ? 8 : 4);
        if (dbl) { if (L) { gld64(RAX, RCX); st64_rbx(RAX, OFF_D(first + i)); } else { ld64_rbx(RAX, OFF_D(first + i)); gst64(RAX, RCX); } }
        else { if (L) { gld32(RAX, RCX); st_r(RAX, OFF_S(first + i)); } else { ld_r(RAX, OFF_S(first + i)); gst32(RAX, RCX); } }
    }
    if (W) { ld_r(RAX, OFF_R(rn)); alu_ri(P ? 5 : 0, RAX, imm8 * 4); st_r(RAX, OFF_R(rn)); }
    return true;
}

/* float guards: DN/FZ mode or a NaN result -> the handler computes ARM semantics */
static u8 *emit_fp_mode_check(void)
{
    rex(0, 0, 0, RBX, false); e8(0xF7); e8(0x83); e32((u32)OFF_FPSCR); e32(0x03000000u);   /* test [rbx+fpscr], DN|FZ */
    return jcc32(CC_NE);
}
static u8 *emit_nan_check(int x) { ucomiss_rr(x, x); return jcc32(0xA); }                    /* jp: unordered */
static void emit_fp_slow(u8 *a, u8 *b, u8 *done_jmp, u32 pc, u32 insn)
{
    patch32(a, p); if (b) patch32(b, p);
    di_t d; cpu_decode_one(&d, pc, insn);
    emit_call_handler(NULL, pc, &d);
    patch32(done_jmp, p);
}

/* VFP extension group (op 7) + vnmla/vnmls. returns false for anything unusual */
static bool emit_vfp_ext(u32 insn, u32 pc, const di_t *fallback_d)
{
    (void)pc; (void)fallback_d;
    u32 D = (insn >> 22) & 1, N = (insn >> 7) & 1, M = (insn >> 5) & 1, L = (insn >> 6) & 1;
    u32 vn = (insn >> 16) & 0xF, vd = (insn >> 12) & 0xF, vm = insn & 0xF;
    u32 sd = (vd << 1) | D, sn = (vn << 1) | N, sm = (vm << 1) | M;
    u32 vop = (((insn >> 23) & 1) << 2) | ((insn >> 20) & 3), b76 = (N << 1) | L;
    if (vop == 1) {                              /* vnmls: n*m - d ; vnmla: -(n*m) - d */
        u8 *slow1 = emit_fp_mode_check();
        sse_ss(0x10, 0, OFF_S(sn)); sse_ss(0x59, 0, OFF_S(sm));
        if (L) { movd_r_x(RAX, 0); alu_ri(6, RAX, 0x80000000u); movd_x_r(0, RAX); }
        sse_ss(0x5C, 0, OFF_S(sd));
        u8 *slow2 = emit_nan_check(0);
        sse_st(0, OFF_S(sd));
        u8 *done = jmp32();
        emit_fp_slow(slow1, slow2, done, pc, insn);
        return true;
    }
    if (vop != 7 || !(b76 & 1)) return false;
    switch (vn) {
    case 0x4: case 0x5: {                        /* vcmp(e) / vcmp(e) #0 -> FPSCR NZCV */
        u8 *slow1 = emit_fp_mode_check();
        sse_ss(0x10, 0, OFF_S(sd));
        if (vn == 4) sse_ss(0x10, 1, OFF_S(sm)); else xorps_rr(1, 1);
        ucomiss_rr(0, 1);
        setcc(0xA, RAX);                         /* setp: unordered */
        setcc(CC_E, RDX);
        setcc(CC_B, RCX);
        movzx8(RAX, RAX); movzx8(RDX, RDX); movzx8(RCX, RCX);
        sh_ri(4, RAX, 2); sh_ri(4, RDX, 1); alu_rr(1, RAX, RDX); alu_rr(1, RAX, RCX);   /* idx = P*4+Z*2+C */
        sh_ri(4, RAX, 2); mov_rr(RCX, RAX);      /* cl = idx*4 */
        mov_ri(RDX, 0x30000682u);                /* nibble[idx]: gt=2 lt=8 eq=6 unord=3 */
        rex(0, 0, 0, RDX, false); e8(0xD3); e8(0xE8 | (RDX & 7));   /* shr edx, cl */
        alu_ri(4, RDX, 0xF); sh_ri(4, RDX, 28);
        ld_r(RAX, OFF_FPSCR); alu_ri(4, RAX, 0x0FFFFFFFu); alu_rr(1, RAX, RDX); st_r(RAX, OFF_FPSCR);
        u8 *done = jmp32();
        emit_fp_slow(slow1, NULL, done, pc, insn);
        return true; }
    case 0x8:                                    /* vcvt.f32.{s32,u32} */
        ld_r(RAX, OFF_S(sm));
        if (b76 & 2) cvtsi2ss_x(0, RAX, 0);      /* signed */
        else cvtsi2ss_x(0, RAX, 1);              /* unsigned: 64-bit source of the zero-extended value */
        sse_st(0, OFF_S(sd));
        return true;
    default:
        return false;
    }
}

/* vcvt.s32.f32 (round to zero): native, saturating cases via the handler */
static bool emit_vcvt_s32(u32 insn, u32 pc, const di_t *d)
{
    u32 D = (insn >> 22) & 1, M = (insn >> 5) & 1, N = (insn >> 7) & 1;
    u32 sd = (((insn >> 12) & 0xF) << 1) | D, sm = ((insn & 0xF) << 1) | M;
    if (((insn >> 16) & 0xF) != 0xD || !N || ((insn >> 8) & 0xF) != 10) return false;
    sse_ss(0x10, 0, OFF_S(sm));
    cvttss2si_r(RAX, 0);
    alu_ri(7, RAX, 0x80000000u);
    u8 *slow = jcc32(CC_E);
    st_r(RAX, OFF_S(sd));
    u8 *done = jmp32();
    patch32(slow, p);
    emit_call_handler(NULL, pc, d);              /* overflow/NaN: ARM saturates */
    patch32(done, p);
    return true;
}

/* word/byte load/store. ea -> RCX */
static bool emit_ls(u32 insn, u32 pc)
{
    u32 P = (insn >> 24) & 1, U = (insn >> 23) & 1, B = (insn >> 22) & 1, W = (insn >> 21) & 1;
    u32 L = (insn >> 20) & 1, rn = (insn >> 16) & 0xF, rd = (insn >> 12) & 0xF;
    if (rd == 15) return false;
    bool wb = !P || W;
    if (wb && (rn == 15 || rn == rd)) return false;
    if (insn & (1u << 25)) {                    /* register offset, LSL #n only */
        u32 rm = insn & 0xF, type = (insn >> 5) & 3, sh = (insn >> 7) & 0x1F;
        if ((insn & (1u << 4)) || type || rm == 15) return false;
        get_reg(RCX, rn, pc);
        ld_r(RDX, OFF_R(rm));
        if (sh) sh_ri(4, RDX, (u8)sh);
        if (P) alu_rr(U ? 0 : 5, RCX, RDX);
        else { mov_rr(R8, RCX); alu_rr(U ? 0 : 5, R8, RDX); }
    } else {
        u32 off = insn & 0xFFF;
        if (rn == 15 && P && !W && L && !B) {   /* literal from read-only text: fold */
            u32 a = pc + 8 + (U ? off : (u32)-off);
            if (a - G.text_lo < G.text_span - 4) { mov_ri(RAX, ld32(a)); st_r(RAX, OFF_R(rd)); return true; }
        }
        get_reg(RCX, rn, pc);
        if (P) { if (off) alu_ri(U ? 0 : 5, RCX, off); }
        else { mov_rr(R8, RCX); if (off) alu_ri(U ? 0 : 5, R8, off); }
    }
    if (P && W) st_r(RCX, OFF_R(rn));
    if (L) {
        if (B) gldu8(RAX, RCX); else gld32(RAX, RCX);
        if (!P) st_r(R8, OFF_R(rn));
        st_r(RAX, OFF_R(rd));
    } else {
        ld_r(RAX, OFF_R(rd));
        if (!P) st_r(R8, OFF_R(rn));
        if (B) gst8(RAX, RCX); else gst32(RAX, RCX);
    }
    return true;
}

/* halfword / signed / doubleword */
static bool emit_ls_extra(u32 insn, u32 pc)
{
    u32 P = (insn >> 24) & 1, U = (insn >> 23) & 1, I = (insn >> 22) & 1, W = (insn >> 21) & 1;
    u32 L = (insn >> 20) & 1, rn = (insn >> 16) & 0xF, rd = (insn >> 12) & 0xF, SH = (insn >> 5) & 3;
    bool dbl = !L && SH != 1;
    if (rd == 15 || rn == 15 || (dbl && ((rd & 1) || rd == 14))) return false;
    bool wb = !P || W;
    if (wb && (rn == rd || (dbl && rn == rd + 1))) return false;
    if (!P && W) return false;
    get_reg(RCX, rn, pc);
    if (I) {
        u32 off = ((insn >> 4) & 0xF0) | (insn & 0xF);
        if (P) { if (off) alu_ri(U ? 0 : 5, RCX, off); }
        else { mov_rr(R8, RCX); if (off) alu_ri(U ? 0 : 5, R8, off); }
    } else {
        u32 rm = insn & 0xF;
        if (rm == 15) return false;
        ld_r(RDX, OFF_R(rm));
        if (P) alu_rr(U ? 0 : 5, RCX, RDX);
        else { mov_rr(R8, RCX); alu_rr(U ? 0 : 5, R8, RDX); }
    }
    if (P && W) st_r(RCX, OFF_R(rn));
    if (dbl) {
        if (SH == 2) {                          /* ldrd */
            gld64(RAX, RCX);
            if (!P) st_r(R8, OFF_R(rn));
            st64_rbx(RAX, OFF_R(rd));
        } else {                                /* strd */
            ld64_rbx(RAX, OFF_R(rd));
            if (!P) st_r(R8, OFF_R(rn));
            gst64(RAX, RCX);
        }
        return true;
    }
    if (L) {
        if (SH == 1) gldu16(RAX, RCX); else if (SH == 2) glds8(RAX, RCX); else glds16(RAX, RCX);
        if (!P) st_r(R8, OFF_R(rn));
        st_r(RAX, OFF_R(rd));
    } else {
        ld_r(RAX, OFF_R(rd));
        if (!P) st_r(R8, OFF_R(rn));
        gst16(RAX, RCX);
    }
    return true;
}

/* ldm/stm without pc/base-in-list. returns false otherwise */
static bool emit_ldm(u32 insn)
{
    u32 P = (insn >> 24) & 1, U = (insn >> 23) & 1, S = (insn >> 22) & 1, W = (insn >> 21) & 1;
    u32 L = (insn >> 20) & 1, rn = (insn >> 16) & 0xF, list = insn & 0xFFFF;
    if (S || rn == 15 || !list || (list & 0x8000) || (list & (1u << rn))) return false;
    int n = __builtin_popcount(list);
    ld_r(RCX, OFF_R(rn));
    s32 start = U ? (P ? 4 : 0) : (P ? -4 * n : -4 * n + 4);
    if (start) alu_ri(0, RCX, (u32)start);
    for (int i = 0, k = 0; i < 16; i++) {
        if (!(list & (1u << i))) continue;
        if (k) alu_ri(0, RCX, 4);
        if (L) { gld32(RAX, RCX); st_r(RAX, OFF_R(i)); }
        else { ld_r(RAX, OFF_R(i)); gst32(RAX, RCX); }
        k++;
    }
    if (W) { ld_r(RAX, OFF_R(rn)); alu_ri(U ? 0 : 5, RAX, 4u * (u32)n); st_r(RAX, OFF_R(rn)); }
    return true;
}

/* single-precision VFP arithmetic + loads/stores + vmov */
static bool emit_vfp(u32 insn, u32 pc)
{
    u32 cls = (insn >> 25) & 7, cp = (insn >> 8) & 0xF;
    if (cp != 10 && cp != 11) return false;
    u32 D = (insn >> 22) & 1, N = (insn >> 7) & 1, M = (insn >> 5) & 1;
    u32 vn = (insn >> 16) & 0xF, vd = (insn >> 12) & 0xF, vm = insn & 0xF;
    u32 sd = (vd << 1) | D, sn = (vn << 1) | N, sm = (vm << 1) | M, dd = (D << 4) | vd;
    if (cls == 6) {                             /* vldr / vstr */
        u32 P = (insn >> 24) & 1, U = (insn >> 23) & 1, W = (insn >> 21) & 1, L = (insn >> 20) & 1;
        if (!P || W) return false;
        u32 off = (insn & 0xFF) * 4, rn = vn;
        get_reg(RCX, rn, pc);
        if (off) alu_ri(U ? 0 : 5, RCX, off);
        if (cp == 10) {
            if (L) { gld32(RAX, RCX); st_r(RAX, OFF_S(sd)); }
            else { ld_r(RAX, OFF_S(sd)); gst32(RAX, RCX); }
        } else {
            if (L) { gld64(RAX, RCX); st64_rbx(RAX, OFF_D(dd)); }
            else { ld64_rbx(RAX, OFF_D(dd)); gst64(RAX, RCX); }
        }
        return true;
    }
    if (insn & (1u << 24)) return false;
    if (insn & (1u << 4)) {                     /* vmov core<->s, vmrs APSR */
        u32 op1 = (insn >> 21) & 7, L = (insn >> 20) & 1, rt = vd;
        if (cp != 10) return false;
        if (op1 == 0 && rt != 15) {
            if (L) { ld_r(RAX, OFF_S(sn)); st_r(RAX, OFF_R(rt)); }
            else { ld_r(RAX, OFF_R(rt)); st_r(RAX, OFF_S(sn)); }
            return true;
        }
        if (op1 == 7 && L && vn == 1 && rt == 15) {
            ld_r(RAX, OFF_FPSCR); alu_ri(4, RAX, 0xF0000000u);
            ld_r(RCX, OFF_CPSR); alu_ri(4, RCX, 0x0FFFFFFFu); alu_rr(1, RCX, RAX); st_r(RCX, OFF_CPSR);
            return true;
        }
        return false;
    }
    if (cp != 10) return false;
    u32 vop = (((insn >> 23) & 1) << 2) | ((insn >> 20) & 3), L = (insn >> 6) & 1;
    if (vop == 1 || vop > 4 || (vop == 4 && L)) return false;
    u8 *slow1 = emit_fp_mode_check();
    switch (vop) {
    case 3: sse_ss(0x10, 0, OFF_S(sn)); sse_ss(L ? 0x5C : 0x58, 0, OFF_S(sm)); break;   /* vsub/vadd */
    case 2: sse_ss(0x10, 0, OFF_S(sn)); sse_ss(0x59, 0, OFF_S(sm)); break;              /* vmul/vnmul */
    case 4: sse_ss(0x10, 0, OFF_S(sn)); sse_ss(0x5E, 0, OFF_S(sm)); break;              /* vdiv */
    default:                                                                            /* vmla/vmls */
        sse_ss(0x10, 0, OFF_S(sn)); sse_ss(0x59, 0, OFF_S(sm));
        if (!L) sse_ss(0x58, 0, OFF_S(sd));
        else { sse_ss(0x10, 1, OFF_S(sd)); sse_rr(0x5C, 1, 0); sse_rr(0x10, 0, 1); }
        break;
    }
    u8 *slow2 = emit_nan_check(0);
    if (vop == 2 && L) { movd_r_x(RAX, 0); alu_ri(6, RAX, 0x80000000u); st_r(RAX, OFF_S(sd)); }
    else sse_st(0, OFF_S(sd));
    u8 *done = jmp32();
    emit_fp_slow(slow1, slow2, done, pc, insn);
    return true;
}

/* ------------------------------------------------------------------ */
/* compare + branch fusion                                             */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* NEON: vld1/vst1, f32 arithmetic, bitwise, immediates               */
/* ------------------------------------------------------------------ */

u64 neon_expand_imm(u32 op, u32 cmode, u32 imm8, bool *ok);

#define NVD(i) ((((i) >> 18) & 0x10) | (((i) >> 12) & 0xF))
#define NVN(i) ((((i) >> 3) & 0x10) | (((i) >> 16) & 0xF))
#define NVM(i) ((((i) >> 1) & 0x10) | ((i) & 0xF))

/* NEON float runs with FZ+DN: MXCSR FTZ|DAZ while a run of them executes */
static u32 g_mx[2];
static bool g_mx_on;
static void mx_emit(int on) { mov_ri64(RAX, (u64)(uintptr_t)&g_mx[on]); e8(0x0F); e8(0xAE); e8(0x10); g_mx_on = on; }

static void x_rbx(u8 pfx, u8 op, int x, s32 off) { if (pfx) e8(pfx); u8 o[2] = { 0x0F, op }; mrm_rbx(0, o, 2, x, off); }
static void x_rr(u8 pfx, u8 op, int d, int s) { if (pfx) e8(pfx); u8 o[2] = { 0x0F, op }; mrm_rr(0, o, 2, d, s, false); }
static void x_mem(u8 pfx, u8 op, int x, int idx)
{
    if (pfx) e8(pfx);
    rex(0, x, idx, R15, false);
    e8(0x0F); e8(op); e8(0x04 | ((x & 7) << 3)); e8(((idx & 7) << 3) | (R15 & 7));
}
static void x_shi(u8 ext, int x, u8 n) { e8(0x66); e8(0x0F); e8(0x72); e8(0xC0 | (ext << 3) | (x & 7)); e8(n); }   /* psrld/pslld */
static void ld_vec(int x, u32 reg, bool q)
{
    if (!q) { x_rbx(0xF3, 0x7E, x, OFF_D(reg)); return; }
    int i = vc_find((int)reg >> 1);
    if (i >= 0) { x_rr(0, 0x28, x, 8 + i); return; }               /* movaps x, cached */
    x_rbx(0, 0x10, x, OFF_D(reg));
    x_rr(0, 0x28, 8 + vc_take((int)reg >> 1), x);
}
static void st_vec(int x, u32 reg, bool q)
{
    if (!q) { x_rbx(0x66, 0xD6, x, OFF_D(reg)); return; }
    x_rbx(0, 0x11, x, OFF_D(reg));
    x_rr(0, 0x28, 8 + vc_take((int)reg >> 1), x);
}
static void and64_rr(int d, int s) { static const u8 o[] = { 0x21 }; mrm_rr(1, o, 1, s, d, false); }

enum { NF_NONE, NF_ADD, NF_SUB, NF_MUL, NF_MLA, NF_MLS };
/* f32 forms emitted natively (and only those need the MXCSR switch) */
static int neon_fop(u32 insn, bool *scalar)
{
    *scalar = false;
    if ((insn & 0xFE000000u) != 0xF2000000u) return NF_NONE;
    u32 U = (insn >> 24) & 1, A = (insn >> 8) & 0xF, B = (insn >> 4) & 1, C = (insn >> 20) & 3, Q = (insn >> 6) & 1;
    if (!(insn & (1u << 23))) {                                    /* three same */
        if (A != 13 || (C & 1)) return NF_NONE;
        if (Q && ((NVD(insn) | NVN(insn) | NVM(insn)) & 1)) return NF_NONE;
        bool op = C & 2;
        if (!U && !B) return op ? NF_SUB : NF_ADD;
        if (!U && B) return op ? NF_MLS : NF_MLA;
        if (U && B && !op) return NF_MUL;
        return NF_NONE;
    }
    /* two registers and a scalar: 1111 001Q 1Dsz nnnn dddd AAAA N1M0 mmmm */
    if (((insn >> 20) & 3) != 2 || (insn & 0x50) != 0x40 || (((insn >> 19) & 0x16) == 0x16)) return NF_NONE;
    if (U && ((NVD(insn) | NVN(insn)) & 1)) return NF_NONE;
    *scalar = true;
    return A == 9 ? NF_MUL : A == 1 ? NF_MLA : A == 5 ? NF_MLS : NF_NONE;
}
static bool neon_needs_mx(u32 insn) { bool s; return neon_fop(insn, &s) != NF_NONE; }

static bool emit_neon_float(u32 insn)
{
    bool scalar;
    int f = neon_fop(insn, &scalar);
    if (f == NF_NONE) return false;
    bool q = scalar ? (insn >> 24) & 1 : (insn >> 6) & 1;
    u32 d = NVD(insn), n = NVN(insn), m = NVM(insn);
    ld_vec(0, n, q);
    if (scalar) {
        u32 mreg = insn & 0xF, idx = (insn >> 5) & 1;
        sse_ss(0x10, 1, OFF_S(mreg * 2 + idx));
        x_rr(0, 0xC6, 1, 1); e8(0);                                 /* shufps xmm1, xmm1, 0 */
    } else ld_vec(1, m, q);
    int r = 0;
    switch (f) {
    case NF_ADD: x_rr(0, 0x58, 0, 1); break;
    case NF_SUB: x_rr(0, 0x5C, 0, 1); break;
    case NF_MUL: x_rr(0, 0x59, 0, 1); break;
    default:
        x_rr(0, 0x59, 0, 1);
        ld_vec(1, d, q);
        x_rr(0, f == NF_MLA ? 0x58 : 0x5C, 1, 0);
        r = 1;
        break;
    }
    /* NaN lanes -> default NaN 0x7FC00000 (rare: branch around the fix) */
    x_rr(0, 0x28, 2, r); x_rr(0, 0xC2, 2, 2); e8(3);               /* xmm2 = unord(r, r) */
    x_rr(0, 0x50, RAX, 2); test_rr(RAX, RAX);                      /* movmskps eax, xmm2 */
    u8 *clean = jcc32(CC_E);
    x_rr(0x66, 0x76, 3, 3); x_shi(2, 3, 23); x_shi(6, 3, 22);       /* xmm3 = 0x7FC00000 */
    x_rr(0, 0x54, 3, 2);                                           /* andps 3, 2 */
    x_rr(0, 0x55, 2, r);                                           /* andnps 2, r */
    x_rr(0, 0x56, 2, 3);                                           /* orps 2, 3 */
    x_rr(0, 0x28, r, 2);
    patch32_keep(clean);
    st_vec(r, d, q);
    return true;
}

static bool emit_neon_bitwise(u32 insn)
{
    /* 1111 001U 0DCC nnnn dddd 0001 NQM1 mmmm */
    if ((insn & 0xFE800F10u) != 0xF2000110u) return false;
    u32 U = (insn >> 24) & 1, C = (insn >> 20) & 3, q = (insn >> 6) & 1;
    u32 d = NVD(insn), n = NVN(insn), m = NVM(insn);
    if (q && ((d | n | m) & 1)) return false;
    ld_vec(0, n, q); ld_vec(1, m, q);
    if (!U) {
        switch (C) {
        case 0: x_rr(0, 0x54, 0, 1); break;                        /* vand */
        case 1: x_rr(0, 0x55, 1, 0); x_rr(0, 0x28, 0, 1); break;   /* vbic: ~m & n */
        case 2: x_rr(0, 0x56, 0, 1); break;                        /* vorr */
        default: x_rr(0x66, 0x76, 2, 2); x_rr(0, 0x57, 1, 2); x_rr(0, 0x56, 0, 1); break;   /* vorn */
        }
    } else if (C == 0) x_rr(0, 0x57, 0, 1);                        /* veor */
    else {
        ld_vec(2, d, q);
        /* vbsl: m ^ ((n ^ m) & d)   vbit: d ^ ((n ^ d) & m)   vbif: n ^ ((d ^ n) & m) */
        int base = C == 1 ? 1 : C == 2 ? 2 : 0, other = C == 1 ? 0 : C == 2 ? 0 : 2, sel = C == 1 ? 2 : 1;
        x_rr(0, 0x28, 3, other); x_rr(0, 0x57, 3, base); x_rr(0, 0x54, 3, sel); x_rr(0, 0x57, 3, base);
        x_rr(0, 0x28, 0, 3);
    }
    st_vec(0, d, q);
    return true;
}

static bool emit_neon_imm(u32 insn)
{
    /* 1111 001i 1D00 0iii dddd cmode 0Qo1 iiii */
    if ((insn & 0xFEB80090u) != 0xF2800010u) return false;
    u32 op = (insn >> 5) & 1, cmode = (insn >> 8) & 0xF, q = (insn >> 6) & 1, d = NVD(insn);
    u32 imm8 = ((insn >> 17) & 0x80) | ((insn >> 12) & 0x70) | (insn & 0xF);
    if (q && (d & 1)) return false;
    bool ok;
    u64 imm = neon_expand_imm(op, cmode, imm8, &ok);
    if (!ok) return false;
    bool orr_bic = (cmode & 1) && cmode < 12;
    u64 v = !op ? imm : cmode == 14 ? imm : ~imm;
    if (orr_bic) mov_ri64(RDX, op ? ~imm : imm); else mov_ri64(RDX, v);
    for (u32 i = 0; i <= q; i++) {
        if (orr_bic) { ld64_rbx(RAX, OFF_D(d + i)); if (op) and64_rr(RAX, RDX); else or64_rr(RAX, RDX); st64_rbx(RAX, OFF_D(d + i)); }
        else st64_rbx(RDX, OFF_D(d + i));
    }
    return true;
}

static void neon_writeback(u32 rn, u32 rm, u32 bytes)
{
    if (rm == 15) return;
    ld_r(RAX, OFF_R(rn));
    if (rm == 13) alu_ri(0, RAX, bytes);
    else { ld_r(RDX, OFF_R(rm)); alu_rr(0, RAX, RDX); }
    st_r(RAX, OFF_R(rn));
}

static bool emit_neon_ls(u32 insn)
{
    if ((insn & 0xFF100000u) != 0xF4000000u) return false;
    u32 A = (insn >> 23) & 1, L = (insn >> 21) & 1, rn = (insn >> 16) & 0xF, rm = insn & 0xF, d = NVD(insn);
    if (rn == 15) return false;
    if (!A) {                                                       /* vld1/vst1 multiple, contiguous */
        u32 type = (insn >> 8) & 0xF, regs;
        switch (type) { case 7: regs = 1; break; case 10: regs = 2; break; case 6: regs = 3; break; case 2: regs = 4; break; default: return false; }
        if (d + regs > 32) return false;
        ld_r(RCX, OFF_R(rn));
        u32 off = 0;
        for (u32 i = 0; i < regs; ) {
            if (off) alu_ri(0, RCX, off), off = 0;
            if (regs - i >= 2) {
                if (L) { x_mem(0, 0x10, 0, RCX); if (!((d + i) & 1)) st_vec(0, d + i, true); else x_rbx(0, 0x11, 0, OFF_D(d + i)); }
                else { if (!((d + i) & 1)) ld_vec(0, d + i, true); else x_rbx(0, 0x10, 0, OFF_D(d + i)); x_mem(0, 0x11, 0, RCX); }
                i += 2; off = 16;
            } else {
                if (L) { gld64(RAX, RCX); st64_rbx(RAX, OFF_D(d + i)); }
                else { ld64_rbx(RAX, OFF_D(d + i)); gst64(RAX, RCX); }
                i += 1; off = 8;
            }
        }
        neon_writeback(rn, rm, regs * 8);
        return true;
    }
    if ((insn >> 8) & 3) return false;                              /* only one-element structures */
    u32 sz = (insn >> 10) & 3;
    if (sz == 3) {                                                  /* vld1 to all lanes */
        u32 s2 = (insn >> 6) & 3, T = (insn >> 5) & 1;
        if (!L || s2 == 3 || d + T >= 32) return false;
        ld_r(RCX, OFF_R(rn));
        if (s2 == 2 && T && !(d & 1)) {                             /* vld1.32 {dN[], dN+1[]}: straight into q */
            gld32(RAX, RCX); movd_x_r(0, RAX);
            e8(0x66); e8(0x0F); e8(0x70); e8(0xC0); e8(0x00);       /* pshufd xmm0, xmm0, 0 */
            st_vec(0, d, true);
            neon_writeback(rn, rm, 4);
            return true;
        }
        if (s2 == 0) { gldu8(RAX, RCX); rex(0, RAX, 0, RAX, false); e8(0x69); e8(0xC0); e32(0x01010101u); }
        else if (s2 == 1) { gldu16(RAX, RCX); rex(0, RAX, 0, RAX, false); e8(0x69); e8(0xC0); e32(0x00010001u); }
        else gld32(RAX, RCX);
        mov_rr(RDX, RAX); shl64_ri(RDX, 32); or64_rr(RAX, RDX);
        st64_rbx(RAX, OFF_D(d));
        if (T) st64_rbx(RAX, OFF_D(d + 1));
        neon_writeback(rn, rm, 1u << s2);
        return true;
    }
    ld_r(RCX, OFF_R(rn));
    u32 ia = (insn >> 4) & 0xF, idx = sz == 0 ? ia >> 1 : sz == 1 ? ia >> 2 : ia >> 3;
    s32 lane = OFF_D(d) + (s32)(idx << sz);
    if (L) {
        if (sz == 0) { gldu8(RAX, RCX); static const u8 o[] = { 0x88 }; mrm_rbx(0, o, 1, RAX, lane); }
        else if (sz == 1) { gldu16(RAX, RCX); e8(0x66); static const u8 o[] = { 0x89 }; mrm_rbx(0, o, 1, RAX, lane); }
        else { gld32(RAX, RCX); st_r(RAX, lane); }
    } else {
        ld_r(RAX, lane);
        if (sz == 0) gst8(RAX, RCX); else if (sz == 1) gst16(RAX, RCX); else gst32(RAX, RCX);
    }
    neon_writeback(rn, rm, 1u << sz);
    return true;
}

static bool emit_neon(u32 insn)
{
    if ((insn & 0xFF100000u) == 0xF4000000u) return emit_neon_ls(insn);
    if ((insn & 0xFE000000u) != 0xF2000000u) return false;
    return emit_neon_float(insn) || emit_neon_bitwise(insn) || emit_neon_imm(insn);
}

/* ------------------------------------------------------------------ */
/* ARMv6/v7 media, movw/movt, more VFP (D32, f64, moves)              */
/* ------------------------------------------------------------------ */

static void movsx8(int d, int s) { static const u8 o[] = { 0x0F, 0xBE }; mrm_rr(0, o, 2, d, s, s >= 4); }
static void movzx16(int d, int s) { static const u8 o[] = { 0x0F, 0xB7 }; mrm_rr(0, o, 2, d, s, false); }
static void movsx16(int d, int s) { static const u8 o[] = { 0x0F, 0xBF }; mrm_rr(0, o, 2, d, s, false); }

static bool emit_arm7(u32 insn)
{
    u32 rd = (insn >> 12) & 0xF;
    if ((insn & 0x0FB00000u) == 0x03000000u) {                     /* movw / movt */
        if (rd == 15) return false;
        u32 imm = ((insn >> 4) & 0xF000) | (insn & 0xFFF);
        if (!(insn & (1u << 22))) { st_imm(OFF_R(rd), imm); return true; }
        ld_r(RAX, OFF_R(rd)); alu_ri(4, RAX, 0xFFFF); alu_ri(1, RAX, imm << 16); st_r(RAX, OFF_R(rd));
        return true;
    }
    if ((insn & 0x0F8003F0u) == 0x06800070u) {                     /* sxt/uxt{a}{b,h} */
        u32 op = (insn >> 20) & 7, rn = (insn >> 16) & 0xF, rm = insn & 0xF, rot = (insn >> 10) & 3;
        if (op != 2 && op != 3 && op != 6 && op != 7) return false;   /* no *xtb16 */
        if (rd == 15 || rm == 15) return false;
        ld_r(RAX, OFF_R(rm));
        if (rot) sh_ri(1, RAX, (u8)(rot * 8));
        if (op == 2) movsx8(RAX, RAX); else if (op == 3) movsx16(RAX, RAX);
        else if (op == 6) movzx8(RAX, RAX); else movzx16(RAX, RAX);
        if (rn != 15) { ld_r(RDX, OFF_R(rn)); alu_rr(0, RAX, RDX); }
        st_r(RAX, OFF_R(rd));
        return true;
    }
    if ((insn & 0x0FA00070u) == 0x07A00050u) {                     /* ubfx / sbfx */
        u32 w = ((insn >> 16) & 0x1F) + 1, lsb = (insn >> 7) & 0x1F, rn = insn & 0xF;
        if (rd == 15 || rn == 15 || lsb + w > 32) return false;
        ld_r(RAX, OFF_R(rn));
        if (insn & (1u << 22)) {
            if (lsb) sh_ri(5, RAX, (u8)lsb);
            if (w < 32) alu_ri(4, RAX, (1u << w) - 1);
        } else {
            if (32 - lsb - w) sh_ri(4, RAX, (u8)(32 - lsb - w));
            if (w < 32) sh_ri(7, RAX, (u8)(32 - w));
        }
        st_r(RAX, OFF_R(rd));
        return true;
    }
    if ((insn & 0x0FE00070u) == 0x07C00010u) {                     /* bfi / bfc */
        u32 msb = (insn >> 16) & 0x1F, lsb = (insn >> 7) & 0x1F, rn = insn & 0xF;
        if (rd == 15 || msb < lsb) return false;
        u32 mask = (msb - lsb == 31 ? ~0u : ((1u << (msb - lsb + 1)) - 1)) << lsb;
        ld_r(RAX, OFF_R(rd)); alu_ri(4, RAX, ~mask);
        if (rn != 15) {
            ld_r(RDX, OFF_R(rn));
            if (lsb) sh_ri(4, RDX, (u8)lsb);
            alu_ri(4, RDX, mask); alu_rr(1, RAX, RDX);
        }
        st_r(RAX, OFF_R(rd));
        return true;
    }
    return false;
}

static u32 vfp_imm32(u32 imm8)
{
    return (imm8 & 0x80) << 24 | ((imm8 & 0x40) ? 0x3E000000u : 0x40000000u) | (imm8 & 0x3F) << 19;
}
static u64 vfp_imm64(u32 imm8)
{
    return (u64)(imm8 & 0x80) << 56 | ((imm8 & 0x40) ? 0x3FC0000000000000ULL : 0x4000000000000000ULL) | (u64)(imm8 & 0x3F) << 48;
}
static void sd_rbx(u8 op, int x, s32 off) { e8(0xF2); u8 o[2] = { 0x0F, op }; mrm_rbx(0, o, 2, x, off); }
static void ucomisd_rr(int a, int b) { e8(0x66); static const u8 o[] = { 0x0F, 0x2E }; mrm_rr(0, o, 2, a, b, false); }

/* vcmp result (flags from ucomis*) -> FPSCR NZCV, same table as the f32 path */
static void emit_cmp_to_fpscr(void)
{
    setcc(0xA, RAX); setcc(CC_E, RDX); setcc(CC_B, RCX);
    movzx8(RAX, RAX); movzx8(RDX, RDX); movzx8(RCX, RCX);
    sh_ri(4, RAX, 2); sh_ri(4, RDX, 1); alu_rr(1, RAX, RDX); alu_rr(1, RAX, RCX);
    sh_ri(4, RAX, 2); mov_rr(RCX, RAX);
    mov_ri(RDX, 0x30000682u);
    rex(0, 0, 0, RDX, false); e8(0xD3); e8(0xE8 | (RDX & 7));
    alu_ri(4, RDX, 0xF); sh_ri(4, RDX, 28);
    ld_r(RAX, OFF_FPSCR); alu_ri(4, RAX, 0x0FFFFFFFu); alu_rr(1, RAX, RDX); st_r(RAX, OFF_FPSCR);
}

static bool emit_vfp2(u32 insn, u32 pc)
{
    u32 cls = (insn >> 25) & 7, cp = (insn >> 8) & 0xF;
    if (cp != 10 && cp != 11) return false;
    bool dbl = cp == 11;
    u32 D = (insn >> 22) & 1, N = (insn >> 7) & 1, M = (insn >> 5) & 1;
    u32 vn = (insn >> 16) & 0xF, vd = (insn >> 12) & 0xF, vm = insn & 0xF;
    u32 sd = (vd << 1) | D, sm = (vm << 1) | M, dd = (D << 4) | vd, dn = (N << 4) | vn, dm = (M << 4) | vm;
    if (cls == 6) {                                                 /* vmov two core regs <-> d / s pair */
        if ((insn & 0x0FE000D0u) != 0x0C400010u) return false;
        u32 L = (insn >> 20) & 1, rt = vd, rt2 = vn;
        if (rt == 15 || rt2 == 15 || (L && rt == rt2)) return false;
        s32 lo = dbl ? OFF_D(dm) : OFF_S(sm), hi = lo + 4;
        if (!dbl && sm == 31) return false;
        if (L) { ld_r(RAX, lo); ld_r(RDX, hi); st_r(RAX, OFF_R(rt)); st_r(RDX, OFF_R(rt2)); }
        else { ld_r(RAX, OFF_R(rt)); ld_r(RDX, OFF_R(rt2)); st_r(RAX, lo); st_r(RDX, hi); }
        return true;
    }
    if (cls != 7 || (insn & (1u << 24)) || (insn & 0x10)) return false;
    u32 vop = (((insn >> 23) & 1) << 2) | ((insn >> 20) & 3), b6 = (insn >> 6) & 1;
    if (vop == 7) {
        if (!b6) {                                                  /* vmov immediate */
            u32 imm8 = (vn << 4) | vm;
            if (dbl) { mov_ri64(RAX, vfp_imm64(imm8)); st64_rbx(RAX, OFF_D(dd)); }
            else st_imm(OFF_S(sd), vfp_imm32(imm8));
            return true;
        }
        u32 N7 = (insn >> 7) & 1;
        if (vn == 0 || vn == 1) {                                   /* vmov reg / vabs / vneg */
            if (vn == 1 && N7) return false;                        /* vsqrt */
            if (dbl) {
                ld64_rbx(RAX, OFF_D(dm));
                if (vn == 1 || N7) {
                    mov_ri64(RDX, vn == 1 ? 0x8000000000000000ULL : 0x7FFFFFFFFFFFFFFFULL);
                    if (vn == 1) { static const u8 o[] = { 0x31 }; mrm_rr(1, o, 1, RDX, RAX, false); }
                    else and64_rr(RAX, RDX);
                }
                st64_rbx(RAX, OFF_D(dd));
            } else {
                ld_r(RAX, OFF_S(sm));
                if (vn == 1) alu_ri(6, RAX, 0x80000000u);
                else if (N7) alu_ri(4, RAX, 0x7FFFFFFFu);
                st_r(RAX, OFF_S(sd));
            }
            return true;
        }
        if ((vn == 4 || vn == 5) && dbl) {                          /* vcmp(e).f64 */
            if (vn == 5 && (insn & 0x2F)) return false;
            u8 *slow1 = emit_fp_mode_check();
            sd_rbx(0x10, 0, OFF_D(dd));
            if (vn == 4) sd_rbx(0x10, 1, OFF_D(dm)); else xorps_rr(1, 1);
            ucomisd_rr(0, 1);
            emit_cmp_to_fpscr();
            u8 *done = jmp32();
            emit_fp_slow(slow1, NULL, done, pc, insn);
            return true;
        }
        return false;
    }
    if (!dbl) return false;                                         /* f32 arithmetic: emit_vfp */
    u32 L = b6;
    if (vop == 1 || vop > 4 || (vop == 4 && L)) return false;
    u8 *slow1 = emit_fp_mode_check();
    sd_rbx(0x10, 0, OFF_D(dn));
    switch (vop) {
    case 3: sd_rbx(L ? 0x5C : 0x58, 0, OFF_D(dm)); break;
    case 2: sd_rbx(0x59, 0, OFF_D(dm)); break;
    case 4: sd_rbx(0x5E, 0, OFF_D(dm)); break;
    default:
        sd_rbx(0x59, 0, OFF_D(dm));
        if (!L) sd_rbx(0x58, 0, OFF_D(dd));
        else { sd_rbx(0x10, 1, OFF_D(dd)); e8(0xF2); e8(0x0F); e8(0x5C); e8(0xC8); e8(0xF2); e8(0x0F); e8(0x10); e8(0xC1); }
        break;
    }
    ucomisd_rr(0, 0);
    u8 *slow2 = jcc32(0xA);
    if (vop == 2 && L) {
        e8(0x66); e8(0x48); e8(0x0F); e8(0x7E); e8(0xC0);           /* movq rax, xmm0 */
        mov_ri64(RDX, 0x8000000000000000ULL);
        { static const u8 o[] = { 0x31 }; mrm_rr(1, o, 1, RDX, RAX, false); }
        st64_rbx(RAX, OFF_D(dd));
    } else sd_rbx(0x11, 0, OFF_D(dd));
    u8 *done = jmp32();
    emit_fp_slow(slow1, slow2, done, pc, insn);
    return true;
}

/* selftest: would emit_arm7 / emit_vfp2 translate this (generic-decoded) word natively? */
#define TBASE_DUMMY 0x50000000u
bool jit_arm7_covers(u32 insn)
{
    static u8 scratch[4096];
    u8 *save = p;
    rc_snap_t rcs = rc_save();
    p = scratch;
    u32 cls = (insn >> 25) & 7;
    bool ok = emit_arm7(insn) || ((cls == 6 || cls == 7) && emit_vfp2(insn, TBASE_DUMMY));
    p = save;
    rc_restore(&rcs);
    return ok;
}

/* selftest: would emit_neon translate this word natively? */
bool jit_neon_covers(u32 insn)
{
    static u8 scratch[4096];
    u8 *save = p;
    rc_snap_t rcs = rc_save();
    p = scratch;
    bool ok = emit_neon(insn);
    p = save;
    rc_restore(&rcs);
    return ok;
}

static int g_flag_ignore_abi = -1;
static bool g_fuse_force_live;               /* selftest: always materialize flags */
static int g_icount;                         /* instructions emitted so far in this block, excluding the current one */
static bool g_skip_next;                     /* try_fused consumed the next word and the block goes on */
static int g_fused_extra;                    /* instructions consumed beyond the current one */           /* RR2_JIT_SAFEFLAGS=1: never assume calls/returns kill flags */

/* true if every path from pc overwrites NZCV before reading any of it (bounded scan) */
static bool flags_dead(u32 pc, int budget)
{
    if (g_flag_ignore_abi < 0) g_flag_ignore_abi = getenv("RR2_JIT_SAFEFLAGS") != NULL;
    while (budget-- > 0) {
        if (pc - G.text_lo >= G.text_span) return false;
        u32 insn = ld32(pc), cond = insn >> 28, cls = (insn >> 25) & 7;
        if (cond != 0xE) return false;                        /* conditional: reads flags */
        if (cls == 5) {
            if (insn & (1u << 24)) return !g_flag_ignore_abi; /* bl: AAPCS doesn't keep flags */
            s32 off = (s32)(insn << 8) >> 6;
            pc = pc + 8 + (u32)off;                           /* follow b */
            continue;
        }
        if ((insn & 0x0FFFFFD0u) == 0x012FFF10u) return !g_flag_ignore_abi;       /* bx / blx */
        if (cls == 4 && (insn & 0x8000) && (insn & (1u << 20))) return !g_flag_ignore_abi; /* pop {pc} */
        if (cls <= 1) {
            u32 b74 = (insn >> 4) & 0xF, opc = (insn >> 21) & 0xF, S = (insn >> 20) & 1;
            bool misc = !(insn & (1u << 25)) && ((b74 & 9) == 9 || ((insn >> 20) & 0x19) == 0x10);
            if (cls == 0 && b74 == 9 && ((insn >> 22) & 0x3F) == 0) { if (S) return false; pc += 4; continue; }
            if (cls == 0 && (b74 & 9) == 9 && (b74 & 6)) { pc += 4; continue; }  /* ldrh etc. */
            if (misc) return false;                                               /* mrs, msr, ... */
            if (opc == 5 || opc == 6 || opc == 7) return false;                   /* adc/sbc/rsc read C */
            if (!(insn & (1u << 25)) && (insn & 0x10)) return false;              /* reg shift: be safe */
            if (!(insn & (1u << 25)) && ((insn >> 5) & 3) == 3 && !((insn >> 7) & 0x1F)) return false; /* rrx */
            if (((insn >> 12) & 0xF) == 15) return false;
            if (S || (opc >= 8 && opc <= 11)) {
                bool arith = opc == 2 || opc == 3 || opc == 4 || opc == 0xA || opc == 0xB;
                return arith;                                /* sets all of NZCV; logical keeps V/C */
            }
            pc += 4;
            continue;
        }
        if (cls == 2 || cls == 3) { if (((insn >> 12) & 0xF) == 15) return false; pc += 4; continue; }
        if (cls == 4) { pc += 4; continue; }
        if (cls == 6) { pc += 4; continue; }                 /* vldr/vstr/vldm */
        if (cls == 7) {
            if ((insn & 0x0FFFFFFFu) == 0x0EF1FA10u) return true;              /* vmrs APSR_nzcv, fpscr */
            if (insn & (1u << 24)) return false;                                /* svc */
            pc += 4;
            continue;
        }
        return false;
    }
    return false;
}

/* ARM cond -> x86 cc on the flags of the fused op. kind: 0 sub-like, 1 add-like, 2 logical */
static int cond_to_cc(u32 cond, int kind)
{
    switch (cond) {
    case 0x0: return CC_E;
    case 0x1: return CC_NE;
    case 0x4: return CC_S;
    case 0x5: return CC_NS;
    }
    if (kind == 2) return -1;
    switch (cond) {
    case 0x2: return kind == 0 ? CC_AE : CC_B;
    case 0x3: return kind == 0 ? CC_B : CC_AE;
    case 0x6: return CC_O;
    case 0x7: return CC_NO;
    case 0x8: return kind == 0 ? CC_A : -1;
    case 0x9: return kind == 0 ? CC_BE : -1;
    case 0xA: return 0xD;                       /* ge */
    case 0xB: return 0xC;                       /* lt */
    case 0xC: return 0xF;                       /* gt */
    case 0xD: return 0xE;                       /* le */
    }
    return -1;
}

/* cmp/cmn/tst/teq/subs/adds/ands/orrs/eors + conditional b: one x86 op + jcc */
static bool try_fused(u32 pc, u32 insn, bool *ended)
{
    if (pc + 4 - G.text_lo >= G.text_span) return false;
    u32 next = ld32(pc + 4);
    u32 ncond = next >> 28;
    if ((insn >> 28) != 0xE || ((insn >> 25) & 7) > 1 || ((next >> 25) & 7) != 5 || ncond >= 0xE) return false;
    u32 opc = (insn >> 21) & 0xF, S = (insn >> 20) & 1, rd = (insn >> 12) & 0xF, rn = (insn >> 16) & 0xF;
    if (!S) return false;
    if ((insn & 0x0E000090u) == 0x00000090u) return false;           /* mul / extra ls */
    if (!(insn & (1u << 25)) && (insn & 0x10)) return false;          /* register shift */
    int kind;
    switch (opc) {
    case 0x2: case 0xA: kind = 0; break;
    case 0x4: case 0xB: kind = 1; break;
    case 0x0: case 0x8: case 0x1: case 0x9: case 0xC: kind = 2; break;
    default: return false;
    }
    if (rd == 15 && !(opc >= 8 && opc <= 11)) return false;
    int cc = cond_to_cc(ncond, kind);
    if (cc < 0) return false;
    int cm = 0; u32 cv = 0;
    u8 *save = p;
    rc_snap_t rcs = rc_save();
    if (!dp_operand(insn, pc, &cm, &cv)) { p = save; rc_restore(&rcs); return false; }
    if (kind == 2 && cm == 1) { p = save; rc_restore(&rcs); return false; }   /* shifter carry: plain path */
    get_reg(RAX, rn, pc);
    switch (opc) {
    case 0x0: case 0x8: alu_rr(4, RAX, RDX); break;
    case 0x1: case 0x9: alu_rr(6, RAX, RDX); break;
    case 0xC: alu_rr(1, RAX, RDX); break;
    case 0x2: case 0xA: alu_rr(5, RAX, RDX); break;
    default: alu_rr(0, RAX, RDX); break;
    }
    if (!(opc >= 8 && opc <= 11)) st_r(RAX, OFF_R(rd));              /* mov: flags survive */
    s32 off = (s32)(next << 8) >> 6;
    u32 target = pc + 12 + (u32)off, fall = pc + 8;
    bool link = (next >> 24) & 1;
    bool dead = !g_fuse_force_live && flags_dead(target, 8) && flags_dead(fall, 8);
    if (!link && npend < 3 * JIT_MAX_INSNS) {                      /* taken: side exit; not taken: continue */
        u8 *nt = jcc32(cc ^ 1);
        if (!dead) { if (kind == 2) emit_flags(cm == 3 ? 3 : 0, 0, cv); else emit_flags(kind == 0 ? 2 : 1, 1, 0); }
        emit_side_exit(target, g_icount + 2);
        patch32_over_exit(nt);
        if (!dead) { if (kind == 2) emit_flags(cm == 3 ? 3 : 0, 0, cv); else emit_flags(kind == 0 ? 2 : 1, 1, 0); }
        g_fused_extra++;
        g_skip_next = true;
        return true;
    }
    u8 *taken = jcc32(cc);
    /* not taken */
    if (!dead) { if (kind == 2) emit_flags(cm == 3 ? 3 : 0, 0, cv); else emit_flags(kind == 0 ? 2 : 1, 1, 0); }
    emit_exit_direct(fall);
    patch32(taken, p);
    if (!dead) { if (kind == 2) emit_flags(cm == 3 ? 3 : 0, 0, cv); else emit_flags(kind == 0 ? 2 : 1, 1, 0); }
    if (link) st_imm(OFF_R(14), pc + 8);
    emit_exit_direct(target);
    *ended = true;
    g_fused_extra++;
    return true;
}

/* ------------------------------------------------------------------ */
/* block compiler                                                      */
/* ------------------------------------------------------------------ */

/* returns true if the instruction ends the block (branch-type) */
static bool emit_insn(u32 pc, u32 insn, bool *ended)
{
    *ended = false;
    u32 cond = insn >> 28, cls = (insn >> 25) & 7;
    if (cond == 0xF) {
        if (emit_neon(insn)) return false;
        di_t d; cpu_decode_one(&d, pc, insn);
        if (g_fb_di) d = *g_fb_di;
        if (d.op == OP_nop) return false;
        emit_call_handler(NULL, pc, &d);
        if ((insn & 0xFE000000u) == 0xFA000000u) {          /* blx imm: always leaves the block */
            ld_r(RAX, OFF_R(15));
            emit_exit_indirect();
            *ended = true;
            return true;
        }
        return false;
    }
    if (!g_tmode && cond == 0xE && cls <= 1 && try_fused(pc, insn, ended)) return *ended;
    u8 *skip = cond != 0xE ? emit_cond_skip(cond) : NULL;

    /* branches end the block */
    if (cls == 5) {
        s32 off = (s32)(insn << 8) >> 6;
        u32 target = pc + 8 + (u32)off;
        if (skip && !(insn & (1u << 24)) && npend < 3 * JIT_MAX_INSNS) {   /* b<cond>: side exit, keep going */
            emit_side_exit(target, g_icount + 1);
            patch32_over_exit(skip);
            return false;
        }
        if (insn & (1u << 24)) st_imm(OFF_R(14), g_next);
        emit_exit_direct(target);
        if (skip) patch32(skip, p);
        *ended = true;
        emit_exit_direct(g_next);               /* condition failed: fall through */
        return true;
    }
    if ((insn & 0x0FFFFFF0u) == 0x012FFF10u || (insn & 0x0FFFFFF0u) == 0x012FFF30u) {   /* bx / blx */
        u32 rm = insn & 0xF;
        if (rm == 15) goto fallback;
        ld_r(RAX, OFF_R(rm));
        if (insn & 0x20) st_imm(OFF_R(14), g_next);
        emit_exit_indirect_iw();
        if (skip) patch32(skip, p);
        *ended = true;
        emit_exit_direct(g_next);
        return true;
    }
    /* pop {..., pc} / ldmia rn!, {..., pc} */
    if (cls == 4 && (insn & 0x8000) && ((insn >> 20) & 1) && !((insn >> 22) & 1)) {
        u32 P = (insn >> 24) & 1, U = (insn >> 23) & 1, W = (insn >> 21) & 1, rn = (insn >> 16) & 0xF;
        u32 list = insn & 0x7FFF;
        if (!P && U && rn != 15 && !(list & (1u << rn))) {
            ld_r(RCX, OFF_R(rn));
            for (int i = 0; i < 15; i++)
                if (list & (1u << i)) { gld32(RAX, RCX); st_r(RAX, OFF_R(i)); alu_ri(0, RCX, 4); }
            gld32(RAX, RCX);
            if (W) { alu_ri(0, RCX, 4); st_r(RCX, OFF_R(rn)); }
            emit_exit_indirect_iw();
            if (skip) patch32(skip, p);
            *ended = true;
            emit_exit_direct(g_next);
            return true;
        }
        goto fallback;
    }
    /* ldr pc, [rn, #imm]{!} (vtable calls, plt) */
    if (cls == 2 && ((insn >> 12) & 0xF) == 15 && ((insn >> 20) & 1) && !((insn >> 22) & 1) && ((insn >> 24) & 1)) {
        u32 rn = (insn >> 16) & 0xF, off = insn & 0xFFF, U = (insn >> 23) & 1, W = (insn >> 21) & 1;
        if (rn == 15) goto fallback;
        ld_r(RCX, OFF_R(rn));
        if (off) alu_ri(U ? 0 : 5, RCX, off);
        if (W) st_r(RCX, OFF_R(rn));
        gld32(RAX, RCX);
        emit_exit_indirect_iw();
        if (skip) patch32(skip, p);
        *ended = true;
        emit_exit_direct(g_next);
        return true;
    }
    /* mov pc, rm */
    if ((insn & 0x0FEFFFF0u) == 0x01A0F000u && (insn & 0xF) != 15) {
        ld_r(RAX, OFF_R(insn & 0xF));
        emit_exit_indirect_iw();
        if (skip) patch32(skip, p);
        *ended = true;
        emit_exit_direct(g_next);
        return true;
    }

    {
        bool ok = emit_arm7(insn) || ((cls == 6 || cls == 7) && emit_vfp2(insn, pc));
        u32 b74 = (insn >> 4) & 0xF;
        if (ok) {
        } else if (cls <= 1) {
            bool misc = !(insn & (1u << 25)) && ((b74 & 9) == 9 || ((insn >> 20) & 0x19) == 0x10);
            if (cls == 0 && b74 == 9 && ((insn >> 22) & 0x3F) == 0) ok = emit_mul(insn);
            else if (cls == 0 && b74 == 9 && ((insn >> 23) & 0x1F) == 1) ok = emit_mull(insn);
            else if (cls == 0 && (b74 & 9) == 9 && (b74 & 6)) ok = emit_ls_extra(insn, pc);
            else if (!misc && !(cls == 1 && ((insn >> 20) & 0x1B) == 0x12) &&
                     !(!((insn >> 20) & 1) && ((insn >> 21) & 0xF) >= 8 && ((insn >> 21) & 0xF) <= 11))
                ok = emit_dp(insn, pc);
        } else if (cls == 2 || cls == 3) {
            if (!(cls == 3 && (insn & 0x10))) ok = emit_ls(insn, pc);
        } else if (cls == 4) {
            ok = emit_ldm(insn);
        } else if (cls == 6 || cls == 7) {
            ok = emit_vfp(insn, pc);
            if (!ok && cls == 6 && ((insn >> 8) & 0xE) == 0xA && !(((insn >> 24) & 1) && !((insn >> 21) & 1)))
                ok = emit_vldm(insn);
            if (!ok && cls == 7 && !(insn & (1u << 24)) && !(insn & 0x10) && ((insn >> 8) & 0xF) == 10)
                ok = emit_vfp_ext(insn, pc, NULL);
            if (!ok && cls == 7 && !(insn & (1u << 24)) && !(insn & 0x10)) {
                di_t dd; cpu_decode_one(&dd, pc, insn);
                if (dd.op == OP_vcvt_s32_s_rz) ok = emit_vcvt_s32(insn, pc, &dd);
            }
        }
        if (ok) { if (skip) patch32(skip, p); return false; }
    }
fallback: {
        di_t d;
        cpu_decode_one(&d, pc, insn);
        if (g_fb_di) d = *g_fb_di;
        if (d.op == OP_nop) { if (skip) patch32(skip, p); return false; }
        emit_call_handler(NULL, pc, &d);
        /* the handler may have branched: continue only if pc is still pc+4 */
        ld_r(RAX, OFF_R(15));
        alu_ri(7, RAX, g_next);
        u8 *same = jcc32(CC_E);
        emit_exit_indirect();
        patch32(same, p);
        if (skip) patch32(skip, p);
        return false;
    }
}


/* ------------------------------------------------------------------ */
/* Thumb blocks                                                        */
/* ------------------------------------------------------------------ */

static bool arm_imm_enc(u32 v, u32 *enc)
{
    for (u32 r = 0; r < 16; r++) {
        u32 x = r ? (v << (2 * r)) | (v >> (32 - 2 * r)) : v;   /* rotate left undoes ROR 2r */
        if (x < 256) { *enc = r << 8 | x; return true; }
    }
    return false;
}

/* ARM word with the same semantics as a Thumb fast-op entry (0: none); may set the dp override */
static u32 arm_from_di(const di_t *d)
{
    int op = d->op;
    u32 rd = d->rd, rn = d->rn, rm = d->rm;
    s32 off = (s32)d->a;
    u32 U = off >= 0, mag = U ? (u32)off : (u32)-off;
    static const struct { int op; u8 L, B, P, W; } wb[] = {
        { OP_ls_ldr, 1, 0, 1, 0 }, { OP_ls_str, 0, 0, 1, 0 }, { OP_ls_ldrb, 1, 1, 1, 0 }, { OP_ls_strb, 0, 1, 1, 0 },
        { OP_ls_ldr_pre, 1, 0, 1, 1 }, { OP_ls_str_pre, 0, 0, 1, 1 }, { OP_ls_ldrb_pre, 1, 1, 1, 1 }, { OP_ls_strb_pre, 0, 1, 1, 1 },
        { OP_ls_ldr_post, 1, 0, 0, 0 }, { OP_ls_str_post, 0, 0, 0, 0 }, { OP_ls_ldrb_post, 1, 1, 0, 0 }, { OP_ls_strb_post, 0, 1, 0, 0 },
    };
    for (unsigned i = 0; i < sizeof(wb) / sizeof(wb[0]); i++)
        if (wb[i].op == op)
            return mag < 4096 ? 0xE4000000u | (u32)wb[i].P << 24 | U << 23 | (u32)wb[i].B << 22 | (u32)wb[i].W << 21 |
                                (u32)wb[i].L << 20 | rn << 16 | rd << 12 | mag : 0;
    static const struct { int op; u8 L, SH, P, W; } hb[] = {
        { OP_ls_ldrh, 1, 1, 1, 0 }, { OP_ls_strh, 0, 1, 1, 0 }, { OP_ls_ldrsb, 1, 2, 1, 0 }, { OP_ls_ldrsh, 1, 3, 1, 0 },
        { OP_ls_ldrh_pre, 1, 1, 1, 1 }, { OP_ls_strh_pre, 0, 1, 1, 1 }, { OP_ls_ldrsb_pre, 1, 2, 1, 1 }, { OP_ls_ldrsh_pre, 1, 3, 1, 1 },
        { OP_ls_ldrh_post, 1, 1, 0, 0 }, { OP_ls_strh_post, 0, 1, 0, 0 }, { OP_ls_ldrsb_post, 1, 2, 0, 0 }, { OP_ls_ldrsh_post, 1, 3, 0, 0 },
    };
    for (unsigned i = 0; i < sizeof(hb) / sizeof(hb[0]); i++)
        if (hb[i].op == op)
            return mag < 256 ? 0xE0400090u | (u32)hb[i].P << 24 | U << 23 | (u32)hb[i].W << 21 | (u32)hb[i].L << 20 |
                               rn << 16 | rd << 12 | (mag >> 4) << 8 | (u32)hb[i].SH << 5 | (mag & 15) : 0;
    static const struct { int op; u8 L, B; } rb[] = {
        { OP_ls_ldr_reg, 1, 0 }, { OP_ls_str_reg, 0, 0 }, { OP_ls_ldrb_reg, 1, 1 }, { OP_ls_strb_reg, 0, 1 } };
    for (unsigned i = 0; i < 4; i++)
        if (rb[i].op == op)
            return 0xE7000000u | 1u << 24 | (u32)!d->a << 23 | (u32)rb[i].B << 22 | (u32)rb[i].L << 20 |
                   rn << 16 | rd << 12 | (d->b & 31) << 7 | rm;
    static const struct { int op; u8 L, SH; } hr[] = {
        { OP_ls_ldrh_rr, 1, 1 }, { OP_ls_strh_rr, 0, 1 }, { OP_ls_ldrsb_rr, 1, 2 }, { OP_ls_ldrsh_rr, 1, 3 } };
    for (unsigned i = 0; i < 4; i++)
        if (hr[i].op == op) return 0xE1800090u | (u32)hr[i].L << 20 | rn << 16 | rd << 12 | (u32)hr[i].SH << 5 | rm;
    if (op >= OP_dp_0x0_0i && op < OP_dp_0x0_0i + 16 * 6 && (op - OP_dp_0x0_0i) % 6 < 2) {
        u32 idx = (u32)(op - OP_dp_0x0_0i), opc = idx / 6, S = idx % 6, enc;
        u32 w = 0xE2000000u | opc << 21 | S << 20 | rn << 16 | rd << 12;
        bool ok = arm_imm_enc(d->a, &enc);
        if (ok && (d->b != 0) == (enc >= 0x100)) return w | enc;   /* same carry rule */
        g_ovr = true; g_ovr_v = d->a; g_ovr_rot = d->b != 0;
        return w;
    }
    return 0;
}

static bool thumb_is_branchy(int op)
{
    return op == OP_t_tbb || op == OP_t_bwpc || op == OP_t_add_pc || op == OP_t_bx_pc || op == OP_generic ||
           op == OP_ldm_ia_wb_pc || op == OP_ldr_pc || op == OP_ldr_pc_pre || op == OP_hook;
}

static void *compile_block_thumb(u32 pc0, int max_insns)
{
    if (jit_full || (u32)(jc_end - jc_ptr) < 64 * 1024) { jit_full = true; return NULL; }
    p = jc_ptr;
    npend = 0;
    u8 *code = p;
    rc_reset();
    e8(0x48); e8(0x81); e8(0x83); e32((u32)OFF_ICNT); u8 *cnt_at = p; e32(0);
    u32 pc = pc0;
    int n = 0;
    bool ended = false;
    g_tmode = true;
    while (n < max_insns && !ended) {
        if (pc - G.text_lo >= G.text_span) break;
        di_t d;
        if (npend >= 3 * JIT_MAX_INSNS) break;
        thumb_decode(&d, pc);
        bool mx = d.insn && neon_needs_mx(d.insn);
        if (mx != g_mx_on) mx_emit(mx);
        u32 next = pc + d.len;
        g_next = next; g_fb_di = &d; g_ovr = false;
        g_icount = n;
        n++;
        int op = d.op;
        u8 *skip = NULL;
        bool conditional = d.cond != 0xE;
        switch (op) {
        case OP_nop:
            break;
        case OP_br_b:
            if (conditional) {                                    /* side exit, keep going */
                skip = emit_cond_skip(d.cond);
                emit_side_exit(d.a, n);
                patch32_over_exit(skip);
                break;
            }
            emit_exit_direct(d.a);
            ended = true;
            break;
        case OP_br_bl: case OP_t_blxi:
            if (conditional) skip = emit_cond_skip(d.cond);
            st_imm(OFF_R(14), d.b);
            if (op == OP_t_blxi) { ld_r(RCX, OFF_CPSR); alu_ri(4, RCX, ~FLAG_T); st_r(RCX, OFF_CPSR); }
            emit_exit_direct(d.a);
            if (skip) { patch32(skip, p); emit_exit_direct(next); }
            ended = true;
            break;
        case OP_br_bx: case OP_br_blx:
            if (conditional) skip = emit_cond_skip(d.cond);
            ld_r(RAX, OFF_R(d.rm));
            if (op == OP_br_blx) st_imm(OFF_R(14), d.b);
            emit_exit_indirect_iw();
            if (skip) { patch32(skip, p); emit_exit_direct(next); }
            ended = true;
            break;
        case OP_t_cbz: case OP_t_cbnz: {
            ld_r(RAX, OFF_R(d.rn));
            test_rr(RAX, RAX);
            u8 *nt = jcc32(op == OP_t_cbz ? CC_NE : CC_E);
            emit_side_exit(d.a, n);
            patch32_over_exit(nt);
            break; }
        case OP_mov_const:
            if (conditional) skip = emit_cond_skip(d.cond);
            st_imm(OFF_R(d.rd), d.a);
            if (skip) patch32(skip, p);
            break;
        case OP_ls_ldr_lit:
            if (conditional) skip = emit_cond_skip(d.cond);
            if (d.a - G.text_lo < G.text_span - 4) st_imm(OFF_R(d.rd), ld32(d.a));
            else { mov_ri(RCX, d.a); gld32(RAX, RCX); st_r(RAX, OFF_R(d.rd)); }
            if (skip) patch32(skip, p);
            break;
        case OP_add_pc_reg:
            if (conditional) skip = emit_cond_skip(d.cond);
            ld_r(RAX, OFF_R(d.rm)); alu_ri(0, RAX, d.a); st_r(RAX, OFF_R(d.rd));
            if (skip) patch32(skip, p);
            break;
        case OP_t_bwpc: case OP_t_add_pc:
            if (conditional) skip = emit_cond_skip(d.cond);
            ld_r(RAX, OFF_R(d.rm));
            if (op == OP_t_add_pc) alu_ri(0, RAX, d.a);
            alu_ri(4, RAX, ~1u);
            emit_exit_indirect();                                 /* stays in Thumb */
            if (skip) { patch32(skip, p); emit_exit_direct(next); }
            ended = true;
            break;
        default: {
            u32 w = d.insn ? d.insn : arm_from_di(&d);
            if (w && (w >> 28) == 0xE) {
                w = (w & 0x0FFFFFFFu) | (u32)d.cond << 28;       /* IT condition */
                if (emit_insn(pc, w, &ended)) ended = true;
            } else {
                if (conditional) skip = emit_cond_skip(d.cond);
                if ((w >> 28) != 0xF || !emit_neon(w)) {
                    emit_call_handler(NULL, pc, &d);
                    if (thumb_is_branchy(op) || (w >> 28) == 0xF) {
                        ld_r(RAX, OFF_R(15));
                        alu_ri(7, RAX, next);
                        u8 *same = jcc32(CC_E);
                        emit_exit_indirect();
                        patch32(same, p);
                    }
                }
                if (skip) patch32(skip, p);
            }
            g_ovr = false;
            break; }
        }
        pc = next;
    }
    if (g_mx_on) mx_emit(0);
    if (!ended) emit_exit_direct(pc);
    g_tmode = false; g_fb_di = NULL;
    flush_slots();
    patch_side_exits(n);
    memcpy(cnt_at, &n, 4);
    jc_ptr = (u8 *)(((uintptr_t)p + 15) & ~(uintptr_t)15);
    return code;
}

static void *compile_block(u32 pc0, int max_insns)
{
    if (jit_full || (u32)(jc_end - jc_ptr) < 64 * 1024) { jit_full = true; return NULL; }
    p = jc_ptr;
    npend = 0;
    u8 *code = p;
    rc_reset();
    /* add qword [rbx+icnt], N (patched once N is known) */
    e8(0x48); e8(0x81); e8(0x83); e32((u32)OFF_ICNT); u8 *cnt_at = p; e32(0);
    u32 pc = pc0;
    int n = 0;
    bool ended = false;
    g_tmode = false; g_fb_di = NULL; g_ovr = false;
    while (n < max_insns) {
        if (pc - G.text_lo >= G.text_span) break;
        u32 insn = ld32(pc);
        if (npend >= 3 * JIT_MAX_INSNS) break;
        if (neon_needs_mx(insn) != g_mx_on) mx_emit(!g_mx_on);
        g_next = pc + 4;
        g_icount = n + g_fused_extra;
        n++;
        if (max_insns == 1 && ((insn >> 25) & 7) <= 1 && (insn >> 28) == 0xE) {
            /* selftest single-insn mode: no fusion with a neighbour */
            u32 save_span = G.text_span; G.text_span = 4;
            bool e = emit_insn(pc, insn, &ended);
            G.text_span = save_span;
            if (e) break;
        } else if (emit_insn(pc, insn, &ended)) break;
        pc += 4;
        if (g_skip_next) { pc += 4; g_skip_next = false; }
    }
    if (g_mx_on) mx_emit(0);
    if (!ended) emit_exit_direct(pc);            /* ran off the end: continue at the next pc */
    flush_slots();
    n += g_fused_extra;
    g_fused_extra = 0;
    patch_side_exits(n);
    memcpy(cnt_at, &n, 4);
    jc_ptr = (u8 *)(((uintptr_t)p + 15) & ~(uintptr_t)15);
    return code;
}

/* ------------------------------------------------------------------ */
/* setup + dispatcher                                                  */
/* ------------------------------------------------------------------ */

static bool jit_init(void)
{
    if (jc_base) return true;
    g_mx[0] = _mm_getcsr(); g_mx[1] = g_mx[0] | 0x8040;           /* FTZ | DAZ */
    jc_base = mmap(NULL, JIT_CACHE, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (jc_base == MAP_FAILED) { jc_base = NULL; return false; }
    jc_end = jc_base + JIT_CACHE;
    p = jc_base;
    /* entry(cpu=rdi, mem=rsi, code=rdx) */
    jit_enter = (jit_entry_t)(void *)p;
    e8(0x53); e8(0x41); e8(0x54); e8(0x41); e8(0x55); e8(0x41); e8(0x56); e8(0x41); e8(0x57);  /* push rbx,r12-r15 */
    e8(0x55); e8(0x48); e8(0x83); e8(0xEC); e8(0x08);   /* push rbp; sub rsp, 8 (calls stay 16-aligned) */
    e8(0x48); e8(0x89); e8(0xFB);               /* mov rbx, rdi */
    e8(0x49); e8(0x89); e8(0xF7);               /* mov r15, rsi */
    e8(0xFF); e8(0xE2);                         /* jmp rdx */
    p = (u8 *)(((uintptr_t)p + 15) & ~(uintptr_t)15);
    /* exit stub: record link slot, restore, return */
    jit_exit_stub = p;
    st64_rbx(RAX, OFF_LINK);
    e8(0x48); e8(0x83); e8(0xC4); e8(0x08); e8(0x5D);   /* add rsp, 8; pop rbp */
    e8(0x41); e8(0x5F); e8(0x41); e8(0x5E); e8(0x41); e8(0x5D); e8(0x41); e8(0x5C); e8(0x5B);  /* pop r15..r12, rbx */
    e8(0xC3);
    jc_ptr = (u8 *)(((uintptr_t)p + 15) & ~(uintptr_t)15);
    return true;
}

void jit_reset(void)
{
    if (!g_jit || !jit_init()) { g_jit = 0; return; }
    if (jit_table) munmap(jit_table, (size_t)jt_words * sizeof(void *));
    jt_lo = G.text_lo;
    jt_words = G.text_span / 2;
    jit_table = mmap(NULL, (size_t)jt_words * sizeof(void *), PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    jc_ptr = (u8 *)(((uintptr_t)jit_exit_stub + 64) & ~(uintptr_t)15);
    jit_full = false;
    p = jc_ptr;
    emit_slow_indirect_stub();
    jc_ptr = p;
}

/* block start (host) -> guest pc, appended in allocation order (addresses ascending) */
#define MAX_BLOCKS (1u << 20)
static struct { u8 *code; u32 pc; } *blk_map;
static u32 nblk;
u32 jit_guest_pc_of(const void *host)
{
    const u8 *h = host;
    if (!blk_map || h < jc_base || h >= jc_end) return 0;
    u32 lo = 0, hi = nblk;
    while (lo + 1 < hi) { u32 m = (lo + hi) / 2; if (blk_map[m].code <= h) lo = m; else hi = m; }
    return nblk && blk_map[lo].code <= h ? blk_map[lo].pc : 0;
}
bool jit_owns(const void *host) { return jc_base && (const u8 *)host >= jc_base && (const u8 *)host < jc_end; }

/* called from JIT code for a branch into an HLE slot: 0 = keep running, 1 = leave the JIT */
bool hle_divert(cpu_t *c, u32 pc);
static JITCALL u32 jit_hle_call(cpu_t *c)
{
    if (hle_divert(c, c->r[15])) return 1;
    return c->exit_loop != 0;
}

static void emit_slow_indirect_stub(void)
{
    jit_slow_ind = p;
    u8 *top = p;
    ld_r(RAX, OFF_R(15));
    alu_ri(7, RAX, HLE_SLOT_BASE + HLE_SLOT_STRIDE);   /* slot 0 (host return) and non-HLE: exit */
    u8 *not_hle = jcc32(CC_B);
    e8(0x48); e8(0x89); e8(0xDF);               /* mov rdi, rbx */
    call_abs((void *)jit_hle_call);
    test_rr(RAX, RAX);
    u8 *leave = jcc32(CC_NE);
    /* HLE returned to lr: look it up and continue in JIT code */
    ld_r(RAX, OFF_R(15));
    e8(0xA8); e8(1);
    u8 *m1 = jcc32(CC_NE);
    alu_ri(5, RAX, jt_lo);
    alu_ri(7, RAX, jt_words * 2);
    u8 *again = jcc32(CC_AE);                   /* e.g. HLE tail-called into another HLE slot */
    mov_ri64(RDX, (u64)(uintptr_t)jit_table);
    e8(0x48); e8(0x8B); e8(0x0C); e8(0x82);     /* mov rcx, [rdx + rax*4] */
    e8(0x48); e8(0x85); e8(0xC9);
    u8 *m2 = jcc32(CC_E);
    e8(0xFF); e8(0xE1);                         /* jmp rcx */
    patch32(again, top);
    u8 *out = p;
    patch32(not_hle, out); patch32(leave, out); patch32(m1, out); patch32(m2, out);
    e8(0x31); e8(0xC0);                         /* xor eax, eax: nothing to link */
    u8 *j = jmp32(); patch32(j, jit_exit_stub);
    p = (u8 *)(((uintptr_t)p + 15) & ~(uintptr_t)15);
}

static void *block_for(u32 pc, bool thumb)
{
    u32 i = (pc - jt_lo) >> 1;
    void *b = __atomic_load_n(&jit_table[i], __ATOMIC_ACQUIRE);
    if (b) return b;
    pthread_mutex_lock(&jit_lock);
    b = jit_table[i];
    if (!b && (b = thumb ? compile_block_thumb(pc, JIT_MAX_INSNS) : compile_block(pc, JIT_MAX_INSNS))) {
        if (!blk_map) blk_map = calloc(MAX_BLOCKS, sizeof(*blk_map));
        if (nblk < MAX_BLOCKS) { blk_map[nblk].code = b; blk_map[nblk].pc = pc; nblk++; }
        __atomic_store_n(&jit_table[i], b, __ATOMIC_RELEASE);
        static long dump_pc = -2;                   /* RR2_JIT_DUMP=hexpc: host code of that block -> jit_<pc>.bin */
        if (dump_pc == -2) dump_pc = getenv("RR2_JIT_DUMP") ? strtol(getenv("RR2_JIT_DUMP"), NULL, 16) : -1;
        if ((long)pc == dump_pc) {
            char fn[64]; snprintf(fn, sizeof(fn), "jit_%08x.bin", pc);
            FILE *f = fopen(fn, "wb");
            if (f) { fwrite(b, 1, (size_t)(jc_ptr - (u8 *)b), f); fclose(f); }
        }
    }
    pthread_mutex_unlock(&jit_lock);
    return b;
}

/* run guest code via the JIT until the interpreter's slow path says stop */
bool jit_run(cpu_t *c, bool (*slow)(cpu_t *, u32))
{
    if (!g_jit || !jit_table) return false;
    for (;;) {
        u32 pc = c->r[15];
        if (__builtin_expect(pc & 1, 0)) { cpu_set_pc(c, pc); pc = c->r[15]; }   /* raw bx target from JIT code */
        bool thumb = c->cpsr & FLAG_T;
        if (__builtin_expect(!thumb && (pc & 3), 0) && pc - jt_lo < jt_words * 2)
            emu_trap(c, "ARM branch to misaligned %08x", pc);
        if (pc - jt_lo >= jt_words * 2 || c->exit_loop) {
            if (slow(c, pc)) return true;
            continue;
        }
        void *b = block_for(pc, thumb);
        if (!b) return false;                   /* cache full: interpreter takes over */
        c->jit_link = NULL;
        jit_enter(c, g_mem, b);
        /* link the exit we left through to its (now compiled) target */
        void **slot = c->jit_link;
        u32 t = c->r[15];
        bool tt = c->cpsr & FLAG_T;
        if (slot && t - jt_lo < jt_words * 2 && !(t & (tt ? 1 : 3)) && !c->exit_loop) {
            void *nb = block_for(t, tt);
            if (nb) __atomic_store_n(slot, nb, __ATOMIC_RELEASE);
        }
    }
}

/* selftest: compile one instruction at pc as its own block, run it once */
bool jit_test_one(cpu_t *c, u32 pc, u32 insn)
{
    if (!jit_init()) return false;
    pthread_mutex_lock(&jit_lock);
    u32 save_lo = G.text_lo, save_span = G.text_span;
    u32 orig = ld32(pc);
    st32(pc, insn);
    G.text_lo = pc; G.text_span = 4;            /* one word: the block stops after it */
    u8 *save_ptr = jc_ptr;
    void *b = compile_block(pc, 1);
    G.text_lo = save_lo; G.text_span = save_span;
    st32(pc, orig);                             /* leave memory as the reference runs saw it */
    pthread_mutex_unlock(&jit_lock);
    if (!b) return false;
    c->r[15] = pc;
    jit_enter(c, g_mem, b);
    jc_ptr = save_ptr;                          /* reuse the space */
    return true;
}

/* selftest: n straight-line ARM instructions compiled as one block (register cache, joins) */
bool jit_test_seq(cpu_t *c, u32 pc, const u32 *insns, int n)
{
    if (!jit_init() || n > 32) return false;
    pthread_mutex_lock(&jit_lock);
    u32 save_lo = G.text_lo, save_span = G.text_span, orig[32];
    for (int i = 0; i < n; i++) { orig[i] = ld32(pc + 4 * i); st32(pc + 4 * i, insns[i]); }
    G.text_lo = pc; G.text_span = 4 * (u32)n;
    u8 *save_ptr = jc_ptr;
    void *b = compile_block(pc, n);
    G.text_lo = save_lo; G.text_span = save_span;
    for (int i = 0; i < n; i++) st32(pc + 4 * i, orig[i]);
    pthread_mutex_unlock(&jit_lock);
    if (!b) return false;
    c->r[15] = pc;
    jit_enter(c, g_mem, b);
    jc_ptr = save_ptr;
    return true;
}

/* selftest: dp + conditional branch compiled as one (fused) block */
bool jit_test_pair(cpu_t *c, u32 pc, u32 i1, u32 i2)
{
    if (!jit_init()) return false;
    pthread_mutex_lock(&jit_lock);
    u32 save_lo = G.text_lo, save_span = G.text_span, o1 = ld32(pc), o2 = ld32(pc + 4);
    st32(pc, i1); st32(pc + 4, i2);
    G.text_lo = pc; G.text_span = 8;
    u8 *save_ptr = jc_ptr;
    g_fuse_force_live = true;
    void *b = compile_block(pc, 2);
    g_fuse_force_live = false;
    G.text_lo = save_lo; G.text_span = save_span;
    st32(pc, o1); st32(pc + 4, o2);
    pthread_mutex_unlock(&jit_lock);
    if (!b) return false;
    c->r[15] = pc;
    jit_enter(c, g_mem, b);
    jc_ptr = save_ptr;
    return true;
}
