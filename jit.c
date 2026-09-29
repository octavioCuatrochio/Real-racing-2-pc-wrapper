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

#define JIT_CACHE      (64u << 20)
#define JIT_MAX_INSNS  64

typedef void (JITCALL *jit_entry_t)(cpu_t *c, u8 *mem, void *code);

static u8  *jc_base, *jc_ptr, *jc_end;
static void **jit_table;                 /* per text word: block code or NULL */
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
/* op reg, [rbx + disp32] */
static void mrm_rbx(int w, const u8 *op, int nop, int reg, s32 disp)
{
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

static void ld_r(int x, s32 off)  { static const u8 o[] = { 0x8B }; mrm_rbx(0, o, 1, x, off); }  /* mov x32,[rbx+off] */
static void st_r(int x, s32 off)  { static const u8 o[] = { 0x89 }; mrm_rbx(0, o, 1, x, off); }  /* mov [rbx+off],x32 */
static void st_imm(s32 off, u32 v) { rex(0, 0, 0, RBX, false); e8(0xC7); e8(0x83); e32((u32)off); e32(v); }
static void mov_ri(int x, u32 v)  { rex(0, 0, 0, x, false); e8(0xB8 + (x & 7)); e32(v); }
static void mov_ri64(int x, u64 v) { rex(1, 0, 0, x, false); e8(0xB8 + (x & 7)); e64(v); }
static void mov_rr(int d, int s)  { static const u8 o[] = { 0x89 }; mrm_rr(0, o, 1, s, d, false); }
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
static void patch32(u8 *at, u8 *target) { s32 rel = (s32)(target - (at + 4)); memcpy(at, &rel, 4); }
static void call_abs(void *fn) { mov_ri64(RAX, (u64)(uintptr_t)fn); e8(0xFF); e8(0xD0); }
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
static void st64_rbx(int x, s32 off) { static const u8 o[] = { 0x89 }; mrm_rbx(1, o, 1, x, off); }
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
static u8 *pend_slot_jmp[2 * JIT_MAX_INSNS + 8];
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
static void flush_slots(void)
{
    for (int i = 0; i < npend; i += 2) {
        u8 *slot = slot_alloc();
        patch32(pend_slot_jmp[i], slot);
        patch32(pend_slot_jmp[i + 1], slot);
    }
    npend = 0;
}

/* eax = target: store, then inline table lookup; miss -> stub with rax = 0 */
static void emit_exit_indirect(void)
{
    st_r(RAX, OFF_R(15));
    e8(0xA8); e8(3);                            /* test al, 3: misaligned/thumb -> C side traps */
    u8 *miss1 = jcc32(CC_NE);
    alu_ri(5, RAX, jt_lo);
    alu_ri(7, RAX, jt_words * 4);
    u8 *miss2 = jcc32(CC_AE);
    mov_ri64(RDX, (u64)(uintptr_t)jit_table);
    /* mov rcx, [rdx + rax*2]  (rax = byte offset; table has 8-byte entries per 4-byte word) */
    e8(0x48); e8(0x8B); e8(0x0C); e8(0x42);
    e8(0x48); e8(0x85); e8(0xC9);               /* test rcx, rcx */
    u8 *miss3 = jcc32(CC_E);
    e8(0xFF); e8(0xE1);                         /* jmp rcx */
    u8 *miss = p;
    patch32(miss1, miss); patch32(miss2, miss); patch32(miss3, miss);
    u8 *j = jmp32(); patch32(j, jit_slow_ind ? jit_slow_ind : jit_exit_stub);
}

/* fallback: run the interpreter's handler for this instruction */
static void emit_call_handler(cpu_t *dummy, u32 pc, const di_t *src)
{
    (void)dummy;
    di_t *d = malloc(sizeof(*d));               /* lives as long as the code */
    *d = *src;
    st_imm(OFF_R(15), pc + 4);
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
    if (!n || (dbl ? first + n > 16 : first + n > 32)) return false;
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

/* VFP extension group (op 7) + vnmla/vnmls. returns false for anything unusual */
static bool emit_vfp_ext(u32 insn, u32 pc, const di_t *fallback_d)
{
    (void)pc; (void)fallback_d;
    u32 D = (insn >> 22) & 1, N = (insn >> 7) & 1, M = (insn >> 5) & 1, L = (insn >> 6) & 1;
    u32 vn = (insn >> 16) & 0xF, vd = (insn >> 12) & 0xF, vm = insn & 0xF;
    u32 sd = (vd << 1) | D, sn = (vn << 1) | N, sm = (vm << 1) | M;
    u32 vop = (((insn >> 23) & 1) << 2) | ((insn >> 20) & 3), b76 = (N << 1) | L;
    if (vop == 1) {                              /* vnmls: n*m - d ; vnmla: -(n*m) - d */
        sse_ss(0x10, 0, OFF_S(sn)); sse_ss(0x59, 0, OFF_S(sm));
        if (L) { movd_r_x(RAX, 0); alu_ri(6, RAX, 0x80000000u); movd_x_r(0, RAX); }
        sse_ss(0x5C, 0, OFF_S(sd));
        sse_st(0, OFF_S(sd));
        return true;
    }
    if (vop != 7 || b76 == 0) return false;
    switch (vn) {
    case 0x4: case 0x5: {                        /* vcmp(e) / vcmp(e) #0 -> FPSCR NZCV */
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
        if (cp == 11 && dd >= 16) return false;
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
    switch (vop) {
    case 3: sse_ss(0x10, 0, OFF_S(sn)); sse_ss(L ? 0x5C : 0x58, 0, OFF_S(sm)); break;   /* vsub/vadd */
    case 2: sse_ss(0x10, 0, OFF_S(sn)); sse_ss(0x59, 0, OFF_S(sm));                     /* vmul/vnmul */
            if (L) { e8(0x66); e8(0x0F); e8(0x7E); e8(0xC0); alu_ri(6, RAX, 0x80000000u);  /* movd eax,xmm0 */
                     st_r(RAX, OFF_S(sd)); return true; }
            break;
    case 4: if (L) return false; sse_ss(0x10, 0, OFF_S(sn)); sse_ss(0x5E, 0, OFF_S(sm)); break; /* vdiv */
    case 0:                                                                             /* vmla/vmls */
        sse_ss(0x10, 0, OFF_S(sn)); sse_ss(0x59, 0, OFF_S(sm));
        if (!L) sse_ss(0x58, 0, OFF_S(sd));
        else { sse_ss(0x10, 1, OFF_S(sd)); sse_rr(0x5C, 1, 0); sse_st(1, OFF_S(sd)); return true; }
        break;
    default: return false;
    }
    sse_st(0, OFF_S(sd));
    return true;
}

/* ------------------------------------------------------------------ */
/* compare + branch fusion                                             */
/* ------------------------------------------------------------------ */

static int g_flag_ignore_abi = -1;
static bool g_fuse_force_live;               /* selftest: always materialize flags */
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
    if (!dp_operand(insn, pc, &cm, &cv)) { p = save; return false; }
    if (kind == 2 && cm == 1) { p = save; return false; }            /* shifter carry: plain path */
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
        di_t d; cpu_decode_one(&d, pc, insn);
        if (d.op != OP_nop) emit_call_handler(NULL, pc, &d);
        return false;
    }
    if (cond == 0xE && cls <= 1 && try_fused(pc, insn, ended)) return true;
    u8 *skip = cond != 0xE ? emit_cond_skip(cond) : NULL;

    /* branches end the block */
    if (cls == 5) {
        s32 off = (s32)(insn << 8) >> 6;
        u32 target = pc + 8 + (u32)off;
        if (insn & (1u << 24)) st_imm(OFF_R(14), pc + 4);
        emit_exit_direct(target);
        if (skip) patch32(skip, p);
        *ended = true;
        emit_exit_direct(pc + 4);               /* condition failed: fall through */
        return true;
    }
    if ((insn & 0x0FFFFFF0u) == 0x012FFF10u || (insn & 0x0FFFFFF0u) == 0x012FFF30u) {   /* bx / blx */
        u32 rm = insn & 0xF;
        if (rm == 15) goto fallback;
        ld_r(RAX, OFF_R(rm));
        if (insn & 0x20) st_imm(OFF_R(14), pc + 4);
        emit_exit_indirect();
        if (skip) patch32(skip, p);
        *ended = true;
        emit_exit_direct(pc + 4);
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
            emit_exit_indirect();
            if (skip) patch32(skip, p);
            *ended = true;
            emit_exit_direct(pc + 4);
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
        emit_exit_indirect();
        if (skip) patch32(skip, p);
        *ended = true;
        emit_exit_direct(pc + 4);
        return true;
    }
    /* mov pc, rm */
    if ((insn & 0x0FEFFFF0u) == 0x01A0F000u && (insn & 0xF) != 15) {
        ld_r(RAX, OFF_R(insn & 0xF));
        emit_exit_indirect();
        if (skip) patch32(skip, p);
        *ended = true;
        emit_exit_direct(pc + 4);
        return true;
    }

    {
        bool ok = false;
        u32 b74 = (insn >> 4) & 0xF;
        if (cls <= 1) {
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
        if (d.op == OP_nop) { if (skip) patch32(skip, p); return false; }
        emit_call_handler(NULL, pc, &d);
        /* the handler may have branched: continue only if pc is still pc+4 */
        ld_r(RAX, OFF_R(15));
        alu_ri(7, RAX, pc + 4);
        u8 *same = jcc32(CC_E);
        emit_exit_indirect();
        patch32(same, p);
        if (skip) patch32(skip, p);
        return false;
    }
}

static void *compile_block(u32 pc0, int max_insns)
{
    if (jit_full || (u32)(jc_end - jc_ptr) < 64 * 1024) { jit_full = true; return NULL; }
    p = jc_ptr;
    npend = 0;
    u8 *code = p;
    /* add qword [rbx+icnt], N (patched once N is known) */
    e8(0x48); e8(0x81); e8(0x83); e32((u32)OFF_ICNT); u8 *cnt_at = p; e32(0);
    u32 pc = pc0;
    int n = 0;
    bool ended = false;
    while (n < max_insns) {
        if (pc - G.text_lo >= G.text_span) break;
        u32 insn = ld32(pc);
        n++;
        if (max_insns == 1 && ((insn >> 25) & 7) <= 1 && (insn >> 28) == 0xE) {
            /* selftest single-insn mode: no fusion with a neighbour */
            u32 save_span = G.text_span; G.text_span = 4;
            bool e = emit_insn(pc, insn, &ended);
            G.text_span = save_span;
            if (e) break;
        } else if (emit_insn(pc, insn, &ended)) break;
        pc += 4;
    }
    if (!ended) emit_exit_direct(pc);            /* ran off the end: continue at the next pc */
    flush_slots();
    n += g_fused_extra;
    g_fused_extra = 0;
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
    jc_base = mmap(NULL, JIT_CACHE, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (jc_base == MAP_FAILED) { jc_base = NULL; return false; }
    jc_end = jc_base + JIT_CACHE;
    p = jc_base;
    /* entry(cpu=rdi, mem=rsi, code=rdx) */
    jit_enter = (jit_entry_t)(void *)p;
    e8(0x53); e8(0x41); e8(0x54); e8(0x41); e8(0x55); e8(0x41); e8(0x56); e8(0x41); e8(0x57);  /* push rbx,r12-r15 */
    e8(0x48); e8(0x89); e8(0xFB);               /* mov rbx, rdi */
    e8(0x49); e8(0x89); e8(0xF7);               /* mov r15, rsi */
    e8(0xFF); e8(0xE2);                         /* jmp rdx */
    p = (u8 *)(((uintptr_t)p + 15) & ~(uintptr_t)15);
    /* exit stub: record link slot, restore, return */
    jit_exit_stub = p;
    st64_rbx(RAX, OFF_LINK);
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
    jt_words = G.text_span / 4;
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
    e8(0xA8); e8(3);
    u8 *m1 = jcc32(CC_NE);
    alu_ri(5, RAX, jt_lo);
    alu_ri(7, RAX, jt_words * 4);
    u8 *again = jcc32(CC_AE);                   /* e.g. HLE tail-called into another HLE slot */
    mov_ri64(RDX, (u64)(uintptr_t)jit_table);
    e8(0x48); e8(0x8B); e8(0x0C); e8(0x42);     /* mov rcx, [rdx + rax*2] */
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

static void *block_for(u32 pc)
{
    u32 i = (pc - jt_lo) >> 2;
    void *b = __atomic_load_n(&jit_table[i], __ATOMIC_ACQUIRE);
    if (b) return b;
    pthread_mutex_lock(&jit_lock);
    b = jit_table[i];
    if (!b && (b = compile_block(pc, JIT_MAX_INSNS))) {
        if (!blk_map) blk_map = calloc(MAX_BLOCKS, sizeof(*blk_map));
        if (nblk < MAX_BLOCKS) { blk_map[nblk].code = b; blk_map[nblk].pc = pc; nblk++; }
        __atomic_store_n(&jit_table[i], b, __ATOMIC_RELEASE);
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
        if (__builtin_expect(pc & 3, 0) && pc - jt_lo < jt_words * 4)
            emu_trap(c, "branch to %08x: Thumb/misaligned target", pc);
        if (pc - jt_lo >= jt_words * 4 || (pc & 3) || c->exit_loop) {
            if (slow(c, pc)) return true;
            continue;
        }
        void *b = block_for(pc);
        if (!b) return false;                   /* cache full: interpreter takes over */
        c->jit_link = NULL;
        jit_enter(c, g_mem, b);
        /* link the exit we left through to its (now compiled) target */
        void **slot = c->jit_link;
        u32 t = c->r[15];
        if (slot && t - jt_lo < jt_words * 4 && !(t & 3) && !c->exit_loop) {
            void *nb = block_for(t);
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
