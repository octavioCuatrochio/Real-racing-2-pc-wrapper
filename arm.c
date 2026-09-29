/*
 * arm.c - ARM-mode instruction handlers + decode table classifier.
 *
 * Decode key: instr bits 27-20 || 7-4 (4096 entries). Condition codes are
 * evaluated by the dispatcher; handlers see only condition-passed instrs
 * (except classes where cond=0xF extends the encoding, checked inside).
 *
 * r15 semantics (ARMv5): reads = pc+8; +12 when used as Rm/Rs/Rn in
 * register-controlled shifts; stored value = pc+12 for STR/STM of r15.
 * (Dispatcher pre-increments r15 to pc+4, so reads add 4, stores add 8.)
 */
#include "emu.h"

#define COND(insn) ((insn) >> 28)

/* read rn/rm with r15 = pc+8 */
static inline u32 rr(cpu_t *c, u32 n) { return n == 15 ? c->r[15] + 4 : c->r[n]; }

static void op_undef(cpu_t *c, u32 insn)
{
    emu_trap(c, "undefined instruction %08x at %08x (key %03x)",
             insn, c->r[15] - 4, DEC_KEY(insn));
}

/* ------------------------------------------------------------------ */
/* shifter operand                                                     */
/* ------------------------------------------------------------------ */

static u32 shifter(cpu_t *c, u32 insn, u32 *cout)
{
    u32 c_in = c->cpsr & FLAG_C;
    if (insn & (1u << 25)) {
        u32 imm = insn & 0xFF, rot = ((insn >> 8) & 0xF) * 2;
        if (!rot) { *cout = c_in; return imm; }
        u32 v = (imm >> rot) | (imm << (32 - rot));
        *cout = (v & 0x80000000u) ? FLAG_C : 0;
        return v;
    }
    u32 rm = insn & 0xF, type = (insn >> 5) & 3;
    u32 v = rr(c, rm);
    if (insn & (1u << 4)) {                    /* register-specified shift */
        u32 rs = (insn >> 8) & 0xF;
        if (rs == 15) emu_trap(c, "shifter rs=r15");
        if (rm == 15) v = c->r[15] + 8;        /* +12 pipeline quirk */
        u32 sh = c->r[rs] & 0xFF;
        *cout = c_in;
        if (sh == 0) return v;
        switch (type) {
        case 0:
            if (sh < 32) { *cout = (v >> (32 - sh)) & 1 ? FLAG_C : 0; return v << sh; }
            if (sh == 32) { *cout = v & 1 ? FLAG_C : 0; return 0; }
            *cout = 0; return 0;
        case 1:
            if (sh < 32) { *cout = (v >> (sh - 1)) & 1 ? FLAG_C : 0; return v >> sh; }
            if (sh == 32) { *cout = (v >> 31) & 1 ? FLAG_C : 0; return 0; }
            *cout = 0; return 0;
        case 2:
            if (sh < 32) { *cout = (v >> (sh - 1)) & 1 ? FLAG_C : 0; return (u32)((s32)v >> sh); }
            *cout = (v >> 31) & 1 ? FLAG_C : 0;
            return (v & 0x80000000u) ? 0xFFFFFFFFu : 0;
        default:
            sh &= 31;
            if (sh == 0) { *cout = (v >> 31) & 1 ? FLAG_C : 0; return v; }
            *cout = (v >> (sh - 1)) & 1 ? FLAG_C : 0;
            return (v >> sh) | (v << (32 - sh));
        }
    }
    u32 sh = (insn >> 7) & 0x1F;               /* immediate shift */
    switch (type) {
    case 0:
        if (!sh) { *cout = c_in; return v; }
        *cout = (v >> (32 - sh)) & 1 ? FLAG_C : 0; return v << sh;
    case 1:
        if (!sh) { *cout = (v >> 31) & 1 ? FLAG_C : 0; return 0; }
        *cout = (v >> (sh - 1)) & 1 ? FLAG_C : 0; return v >> sh;
    case 2:
        if (!sh) { *cout = (v >> 31) & 1 ? FLAG_C : 0;
                   return (v & 0x80000000u) ? 0xFFFFFFFFu : 0; }
        *cout = (v >> (sh - 1)) & 1 ? FLAG_C : 0; return (u32)((s32)v >> sh);
    default:
        if (!sh) {                              /* RRX */
            u32 co = v & 1;
            v = (v >> 1) | (c_in ? 0x80000000u : 0);
            *cout = co ? FLAG_C : 0; return v;
        }
        *cout = (v >> (sh - 1)) & 1 ? FLAG_C : 0;
        return (v >> sh) | (v << (32 - sh));
    }
}

/* ------------------------------------------------------------------ */
/* data processing                                                     */
/* ------------------------------------------------------------------ */

static inline void dp_flags_logic(cpu_t *c, u32 res, u32 cout)
{
    c->cpsr = (c->cpsr & ~(FLAG_N | FLAG_Z | FLAG_C)) |
              (res & 0x80000000u ? FLAG_N : 0) | (res == 0 ? FLAG_Z : 0) | cout;
}

static inline void dp_flags_add(cpu_t *c, u32 a, u32 b, u64 res)
{
    u32 r = (u32)res;
    c->cpsr = (c->cpsr & ~(FLAG_N | FLAG_Z | FLAG_C | FLAG_V)) |
              (r & 0x80000000u ? FLAG_N : 0) | (r == 0 ? FLAG_Z : 0) |
              (res >> 32 ? FLAG_C : 0) |
              ((~(a ^ b) & (a ^ r) & 0x80000000u) ? FLAG_V : 0);
}

static inline void dp_flags_sub(cpu_t *c, u32 a, u32 b, u32 r)
{
    c->cpsr = (c->cpsr & ~(FLAG_N | FLAG_Z | FLAG_C | FLAG_V)) |
              (r & 0x80000000u ? FLAG_N : 0) | (r == 0 ? FLAG_Z : 0) |
              (a >= b ? FLAG_C : 0) |
              (((a ^ b) & (a ^ r) & 0x80000000u) ? FLAG_V : 0);
}

static void op_dp(cpu_t *c, u32 insn)
{
    if (COND(insn) == 0xF) op_undef(c, insn);
    u32 opc = (insn >> 21) & 0xF, S = (insn >> 20) & 1;
    u32 rn = (insn >> 16) & 0xF, rd = (insn >> 12) & 0xF;
    u32 cout, sh = shifter(c, insn, &cout);
    u32 a = rr(c, rn), res = 0;
    bool write = true;

    switch (opc) {
    case 0x0: res = a & sh;  if (S) dp_flags_logic(c, res, cout); break;
    case 0x1: res = a ^ sh;  if (S) dp_flags_logic(c, res, cout); break;
    case 0x2: res = a - sh;  if (S) dp_flags_sub(c, a, sh, res); break;
    case 0x3: res = sh - a;  if (S) dp_flags_sub(c, sh, a, res); break;
    case 0x4: res = (u32)((u64)a + sh); if (S) dp_flags_add(c, a, sh, (u64)a + sh); break;
    case 0x5: {              /* ADC */
        u32 ci = (c->cpsr & FLAG_C) ? 1 : 0;
        u64 r = (u64)a + sh + ci; res = (u32)r;
        if (S) dp_flags_add(c, a, sh, r);    /* V from original operands */
        break; }
    case 0x6: {              /* SBC */
        u32 bi = (c->cpsr & FLAG_C) ? 0 : 1;
        u64 sub = (u64)sh + bi;
        res = (u32)((u64)a - sub);
        if (S) c->cpsr = (c->cpsr & ~(FLAG_N | FLAG_Z | FLAG_C | FLAG_V)) |
            (res & 0x80000000u ? FLAG_N : 0) | (res == 0 ? FLAG_Z : 0) |
            ((u64)a >= sub ? FLAG_C : 0) |
            (((a ^ sh) & (a ^ res) & 0x80000000u) ? FLAG_V : 0);
        break; }
    case 0x7: {              /* RSC */
        u32 bi = (c->cpsr & FLAG_C) ? 0 : 1;
        u64 sub = (u64)a + bi;
        res = (u32)((u64)sh - sub);
        if (S) c->cpsr = (c->cpsr & ~(FLAG_N | FLAG_Z | FLAG_C | FLAG_V)) |
            (res & 0x80000000u ? FLAG_N : 0) | (res == 0 ? FLAG_Z : 0) |
            ((u64)sh >= sub ? FLAG_C : 0) |
            (((sh ^ a) & (sh ^ res) & 0x80000000u) ? FLAG_V : 0);
        break; }
    case 0x8: res = a & sh;  dp_flags_logic(c, res, cout); write = false; break; /* TST */
    case 0x9: res = a ^ sh;  dp_flags_logic(c, res, cout); write = false; break; /* TEQ */
    case 0xA: res = a - sh;  dp_flags_sub(c, a, sh, res);  write = false; break; /* CMP */
    case 0xB: res = (u32)((u64)a + sh); dp_flags_add(c, a, sh, (u64)a + sh); write = false; break; /* CMN */
    case 0xC: res = a | sh;  if (S) dp_flags_logic(c, res, cout); break;
    case 0xD: res = sh;      if (S) dp_flags_logic(c, res, cout); break;
    case 0xE: res = a & ~sh; if (S) dp_flags_logic(c, res, cout); break;
    case 0xF: res = ~sh;     if (S) dp_flags_logic(c, res, cout); break;
    }
    if (!write) return;
    if (rd == 15) {
        if (S) LOG_ONCE("[cpu] dp with rd=15,S=1: SPSR restore ignored\n");
        cpu_branch(c, res);
    } else {
        c->r[rd] = res;
    }
}

/* ------------------------------------------------------------------ */
/* multiply                                                            */
/* ------------------------------------------------------------------ */

static void op_mul(cpu_t *c, u32 insn)
{
    u32 A = (insn >> 21) & 1, S = (insn >> 20) & 1;
    u32 rd = (insn >> 16) & 0xF, rn = (insn >> 12) & 0xF;
    u32 rs = (insn >> 8) & 0xF, rm = insn & 0xF;
    if (rd == 15 || rm == 15 || rs == 15) emu_trap(c, "mul with r15");
    u32 res = c->r[rm] * c->r[rs];
    if (A) res += c->r[rn];
    c->r[rd] = res;
    if (S) c->cpsr = (c->cpsr & ~(FLAG_N | FLAG_Z)) |
                     (res & 0x80000000u ? FLAG_N : 0) | (res == 0 ? FLAG_Z : 0);
}

static void op_mullong(cpu_t *c, u32 insn)
{
    u32 U = (insn >> 22) & 1, A = (insn >> 21) & 1, S = (insn >> 20) & 1;
    u32 rdhi = (insn >> 16) & 0xF, rdlo = (insn >> 12) & 0xF;
    u32 rs = (insn >> 8) & 0xF, rm = insn & 0xF;
    if (rdhi == 15 || rdlo == 15 || rdhi == rdlo) emu_trap(c, "mullong bad regs");
    u64 res;
    if (U) res = (u64)(s64)((s32)c->r[rm]) * (s64)((s32)c->r[rs]);
    else   res = (u64)c->r[rm] * (u64)c->r[rs];
    if (A) res += ((u64)c->r[rdhi] << 32) | c->r[rdlo];
    c->r[rdlo] = (u32)res;
    c->r[rdhi] = (u32)(res >> 32);
    if (S) c->cpsr = (c->cpsr & ~(FLAG_N | FLAG_Z)) |
                     (res & 0x8000000000000000ULL ? FLAG_N : 0) | (res == 0 ? FLAG_Z : 0);
}

/* v5TE DSP multiplies + saturating add/sub (rare in this binary; insurance) */
static u32 sat32(s64 v, bool *sat)
{
    if (v > 0x7FFFFFFFLL) { *sat = true; return 0x7FFFFFFFu; }
    if (v < -0x80000000LL) { *sat = true; return 0x80000000u; }
    return (u32)v;
}

static void op_dsp_mul(cpu_t *c, u32 insn)
{
    u32 sub = (insn >> 21) & 3;   /* 0=SMLAxy 1=SMLAWy/SMULWy 2=SMLALxy 3=SMULxy */
    u32 b74 = (insn >> 4) & 0xF;
    u32 x = (b74 >> 1) & 1, y = (b74 >> 2) & 1;  /* bits 5,6: operand halfword selects */
    u32 rd = (insn >> 16) & 0xF, rn = (insn >> 12) & 0xF;
    u32 rs = (insn >> 8) & 0xF, rm = insn & 0xF;
    s32 a = (s32)(x ? c->r[rm] >> 16 : c->r[rm] & 0xFFFF);
    a = (s32)(s16)a;
    s64 prod;
    bool sat = false;
    switch (sub) {
    case 0: { /* SMLAxy */
        s32 b = (s32)(s16)(y ? c->r[rs] >> 16 : c->r[rs] & 0xFFFF);
        prod = (s64)a * b;
        u32 r = (u32)prod + c->r[rn];
        s64 chk = (s64)(s32)prod + (s32)c->r[rn];
        if (chk != (s32)chk) c->cpsr |= FLAG_Q;
        c->r[rd] = r;
        break; }
    case 1: { /* SMLAWy (b74 bit0=0) / SMULWy */
        s32 b = (s32)(s16)(y ? c->r[rs] >> 16 : c->r[rs] & 0xFFFF);
        prod = ((s64)(s32)c->r[rm] * b) >> 16;
        if (x) { /* SMULWy */
            c->r[rd] = (u32)(s32)prod;
        } else {
            s64 chk = prod + (s32)c->r[rn];
            c->r[rd] = sat32(chk, &sat);
            if (sat) c->cpsr |= FLAG_Q;
        }
        break; }
    case 2: { /* SMLALxy */
        s32 b = (s32)(s16)(y ? c->r[rs] >> 16 : c->r[rs] & 0xFFFF);
        prod = (s64)a * b;
        u64 acc = ((u64)c->r[rd] << 32) | c->r[rn];
        acc += (u64)prod;
        c->r[rn] = (u32)acc; c->r[rd] = (u32)(acc >> 32);
        break; }
    default: { /* SMULxy */
        s32 b = (s32)(s16)(y ? c->r[rs] >> 16 : c->r[rs] & 0xFFFF);
        c->r[rd] = (u32)(a * b);
        break; }
    }
}

static void op_qxx(cpu_t *c, u32 insn)
{
    u32 sub = (insn >> 21) & 3;
    u32 rn = insn & 0xF, rd = (insn >> 12) & 0xF, rm = (insn >> 16) & 0xF;
    bool sat = false;
    s64 a = (s32)c->r[rm], b = (s32)c->r[rn], r;   /* a = Rn(19-16), b = Rm(3-0) */
    if (sub >= 2) a = (s32)sat32(2 * a, &sat);     /* QDADD/QDSUB saturate the doubling */
    switch (sub) {
    case 0: case 2: r = b + a; break;             /* QADD / QDADD */
    default:        r = b - a; break;             /* QSUB / QDSUB */
    }
    u32 res = sat32(r, &sat);
    if (sat) c->cpsr |= FLAG_Q;
    c->r[rd] = res;
}

static void op_swp(cpu_t *c, u32 insn)
{
    u32 B = (insn >> 22) & 1;
    u32 rn = (insn >> 16) & 0xF, rd = (insn >> 12) & 0xF, rm = insn & 0xF;
    u32 addr = rr(c, rn);
    if (B) { u8 t = ld8(addr); st8(addr, (u8)c->r[rm]); c->r[rd] = t; }
    else   { u32 t = ld32(addr); st32(addr, c->r[rm]); c->r[rd] = t; }
    LOG_ONCE("[cpu] swp executed\n");
}

/* ------------------------------------------------------------------ */
/* load / store word+byte                                              */
/* ------------------------------------------------------------------ */

static void op_ls(cpu_t *c, u32 insn)
{
    u32 b2720 = (insn >> 20) & 0xFF;
    u32 P = (insn >> 24) & 1, U = (insn >> 23) & 1, B = (insn >> 22) & 1;
    u32 W = (insn >> 21) & 1, L = (insn >> 20) & 1;
    u32 rn = (insn >> 16) & 0xF, rd = (insn >> 12) & 0xF;

    if (COND(insn) == 0xF) {   /* pld = 1111 0101 U101 Rn 1111 imm12 */
        if ((b2720 & 0xD7) == 0x55 && rd == 15) return;
        op_undef(c, insn);
    }

    u32 off;
    if (insn & (1u << 25)) {
        /* scaled register offset: shifter with immediate shift (bit4=0) */
        u32 rm = insn & 0xF, type = (insn >> 5) & 3, sh = (insn >> 7) & 0x1F;
        u32 v = rr(c, rm);
        switch (type) {
        case 0: off = v << sh; break;
        case 1: off = sh ? v >> sh : 0; break;
        case 2: off = sh ? (u32)((s32)v >> sh) : (v & 0x80000000u ? 0xFFFFFFFFu : 0); break;
        default: off = sh ? (v >> sh) | (v << (32 - sh))
                          : ((c->cpsr & FLAG_C ? 0x80000000u : 0) | (v >> 1)); break;
        }
    } else {
        off = insn & 0xFFF;
    }
    u32 base = rr(c, rn);
    u32 ea;
    if (P) {
        ea = U ? base + off : base - off;
        if (W) c->r[rn] = ea;
    } else {
        ea = base;
        c->r[rn] = U ? base + off : base - off;      /* post-index always writes back */
    }

    if (L) {
        u32 v = B ? ld8(ea) : ld32(ea);
        if (rd == 15) cpu_branch(c, v);
        else c->r[rd] = v;
    } else {
        u32 v = (rd == 15) ? c->r[15] + 8 : c->r[rd]; /* store of r15 = pc+12 */
        if (B) st8(ea, (u8)v); else st32(ea, v);
    }
}

/* halfword / signed / doubleword transfers: ... 1SH1 */
static void op_ls_extra(cpu_t *c, u32 insn)
{
    u32 P = (insn >> 24) & 1, U = (insn >> 23) & 1, I = (insn >> 22) & 1;
    u32 W = (insn >> 21) & 1, L = (insn >> 20) & 1;
    u32 rn = (insn >> 16) & 0xF, rd = (insn >> 12) & 0xF;
    u32 SH = (insn >> 5) & 3;

    u32 off;
    if (I) off = (((insn >> 8) & 0xF) << 4) | (insn & 0xF);
    else   off = c->r[insn & 0xF];

    u32 base = rr(c, rn);
    u32 ea;
    if (P) {
        ea = U ? base + off : base - off;
        if (W) c->r[rn] = ea;
    } else {
        ea = base;
        c->r[rn] = U ? base + off : base - off;
    }

    switch (SH) {
    case 1:                     /* unsigned halfword */
        if (L) c->r[rd] = ld16(ea);
        else   st16(ea, (u16)c->r[rd]);
        break;
    case 2:                     /* LDRD (L=0) / LDRSB (L=1) */
        if (L) c->r[rd] = (u32)(s32)(s8)ld8(ea);
        else {
            if (rd & 1) emu_trap(c, "ldrd odd rd");
            c->r[rd] = ld32(ea);
            c->r[rd + 1] = ld32(ea + 4);
        }
        break;
    case 3:                     /* STRD (L=0) / LDRSH (L=1) */
        if (L) c->r[rd] = (u32)(s32)(s16)ld16(ea);
        else {
            if (rd & 1) emu_trap(c, "strd odd rd");
            st32(ea, c->r[rd]);
            st32(ea + 4, c->r[rd + 1]);
        }
        break;
    }
}

/* ------------------------------------------------------------------ */
/* ldm / stm                                                           */
/* ------------------------------------------------------------------ */

static void op_ldmstm(cpu_t *c, u32 insn)
{
    u32 P = (insn >> 24) & 1, U = (insn >> 23) & 1, S = (insn >> 22) & 1;
    u32 W = (insn >> 21) & 1, L = (insn >> 20) & 1;
    u32 rn = (insn >> 16) & 0xF;
    u32 list = insn & 0xFFFF;
    if (!list) emu_trap(c, "ldm/stm empty list");
    if (S) LOG_ONCE("[cpu] ldm/stm S-bit ignored (user mode)\n");
    if (COND(insn) == 0xF) op_undef(c, insn);

    int n = __builtin_popcount(list);
    u32 base = c->r[rn];
    u32 start;
    if (U) start = P ? base + 4 : base;                 /* IB / IA */
    else   start = P ? base - 4 * n : base - 4 * n + 4; /* DB / DA */

    u32 addr = start;
    if (L) {
        u32 vals[16];
        int k = 0;
        for (int i = 0; i < 16; i++)
            if (list & (1 << i)) { vals[k++] = ld32(addr); addr += 4; }
        if (W) c->r[rn] = U ? base + 4 * n : base - 4 * n;
        k = 0;
        for (int i = 0; i < 16; i++)
            if (list & (1 << i)) {
                if (i == 15) cpu_branch(c, vals[k]);
                else c->r[i] = vals[k];
                k++;
            }
    } else {
        u32 wb = U ? base + 4 * n : base - 4 * n;
        for (int i = 0; i < 16; i++)
            if (list & (1 << i)) {
                u32 v = (i == 15) ? c->r[15] + 8 : c->r[i];  /* r15 stores pc+12 */
                if (i == (int)rn && (list & ((1 << rn) - 1)))
                    v = c->r[rn];     /* base not lowest in list: UNPREDICTABLE, use old */
                st32(addr, v);
                addr += 4;
            }
        if (W) c->r[rn] = wb;
    }
}

/* ------------------------------------------------------------------ */
/* branch                                                              */
/* ------------------------------------------------------------------ */

static void op_branch(cpu_t *c, u32 insn)
{
    u32 L = (insn >> 24) & 1;
    s32 off = (s32)(insn & 0xFFFFFF);
    if (off & 0x800000) off |= ~0xFFFFFF;
    if (COND(insn) == 0xF)           /* BLX immediate: switches to Thumb */
        emu_trap(c, "BLX imm (Thumb) not supported");
    if (L) c->r[14] = c->r[15];      /* pc+4 */
    c->r[15] = c->r[15] + 4 + ((u32)off << 2);
}

/* ------------------------------------------------------------------ */
/* misc group: mrs/msr/bx/blx/clz/bkpt                                 */
/* ------------------------------------------------------------------ */

static void op_misc(cpu_t *c, u32 insn)
{
    u32 b2720 = (insn >> 20) & 0xFF, b74 = (insn >> 4) & 0xF;

    if (b74 == 0x0 && b2720 == 0x10 && ((insn >> 16) & 0xF) == 0xF) {   /* MRS */
        c->r[(insn >> 12) & 0xF] = c->cpsr & 0xF8FFFFFFu;
        return;
    }
    if (b74 == 0x0 && (b2720 == 0x12 || b2720 == 0x16)) {               /* MSR reg */
        if (b2720 == 0x16) { LOG_ONCE("[cpu] MSR SPSR ignored\n"); return; }
        u32 mask = (insn >> 16) & 0xF, v = c->r[insn & 0xF];
        if (mask & 0x8) c->cpsr = (c->cpsr & 0x07FFFFFFu) | (v & 0xF8000000u);
        return;
    }
    if (b74 == 0x1 && b2720 == 0x12) {                                  /* BX */
        cpu_branch(c, c->r[insn & 0xF]);
        return;
    }
    if (b74 == 0x3 && b2720 == 0x12) {                                  /* BLX reg */
        u32 t = c->r[insn & 0xF];
        c->r[14] = c->r[15];
        cpu_branch(c, t);
        return;
    }
    if (b74 == 0x1 && b2720 == 0x16) {                                  /* CLZ */
        u32 v = c->r[insn & 0xF];
        c->r[(insn >> 12) & 0xF] = v ? (u32)__builtin_clz(v) : 32;
        return;
    }
    if (b74 == 0x7 && b2720 == 0x12)                                    /* BKPT */
        emu_trap(c, "bkpt #%u", insn & 0xFFF);
    if (b74 == 0x9 && ((insn >> 8) & 0xF) == 0 && (b2720 == 0x10 || b2720 == 0x14)) {
        op_swp(c, insn);                                                /* SWP requires SBZ in 11-8 */
        return;
    }
    if (b74 == 0x5) { op_qxx(c, insn); return; }
    if ((b74 & 0x9) == 0x8) { op_dsp_mul(c, insn); return; }
    op_undef(c, insn);
}

static void op_msr_imm(cpu_t *c, u32 insn)
{
    u32 R = (insn >> 22) & 1;
    if (R) { LOG_ONCE("[cpu] MSR SPSR imm ignored\n"); return; }
    u32 mask = (insn >> 16) & 0xF;
    u32 imm = insn & 0xFF, rot = ((insn >> 8) & 0xF) * 2;
    u32 v = rot ? (imm >> rot) | (imm << (32 - rot)) : imm;
    if (mask & 0x8) c->cpsr = (c->cpsr & 0x07FFFFFFu) | (v & 0xF8000000u);
}

/* ------------------------------------------------------------------ */
/* coprocessor (delegates VFP to vfp.c, stubs the rest)                */
/* ------------------------------------------------------------------ */

extern void vfp_cdp(cpu_t *c, u32 insn);
extern void vfp_mcr_mrc(cpu_t *c, u32 insn);
extern void vfp_ldc_stc(cpu_t *c, u32 insn);
extern void vfp_mcrr_mrrc(cpu_t *c, u32 insn);

static void op_cdp(cpu_t *c, u32 insn)
{
    u32 cp = (insn >> 8) & 0xF;
    if (COND(insn) == 0xF) op_undef(c, insn);
    if (cp == 10 || cp == 11) { vfp_cdp(c, insn); return; }
    LOG_ONCE("[cpu] cdp cp%u ignored\n", cp);
}

static void op_mcr_mrc(cpu_t *c, u32 insn)
{
    u32 cp = (insn >> 8) & 0xF;
    if (COND(insn) == 0xF) op_undef(c, insn);
    if (cp == 10 || cp == 11) { vfp_mcr_mrc(c, insn); return; }
    u32 L = (insn >> 20) & 1;
    u32 crn = (insn >> 16) & 0xF, crm = insn & 0xF, op1 = (insn >> 21) & 7, op2 = (insn >> 5) & 7;
    u32 rt = (insn >> 12) & 0xF;
    if (cp == 15 && L && crn == 13 && crm == 0) { c->r[rt] = c->tls_base; return; } /* TLS */
    if (cp == 15 && L && crn == 0 && crm == 0 && op1 == 0 && op2 == 0) {
        c->r[rt] = 0x41069265;   /* fake MIDR (ARM926EJ-S-ish) */
        return;
    }
    if (cp == 15) { LOG_ONCE("[cpu] mcr/mrc p15 ignored (%u,%u,%u,%u)\n", crn, crm, op1, op2); return; }
    LOG_ONCE("[cpu] mcr/mrc cp%u ignored (%u,%u,%u,%u)\n", cp, crn, crm, op1, op2);
}

static void op_ldc_stc(cpu_t *c, u32 insn)
{
    u32 cp = (insn >> 8) & 0xF;
    if (cp == 10 || cp == 11) { vfp_ldc_stc(c, insn); return; }
    LOG_ONCE("[cpu] ldc/stc cp%u ignored\n", cp);
    if ((insn >> 20) & 1) c->r[(insn >> 12) & 0xF] = 0;   /* load: return 0 */
}

static void op_mcrr_mrrc(cpu_t *c, u32 insn)
{
    u32 cp = (insn >> 8) & 0xF;
    if (cp == 10 || cp == 11) { vfp_mcrr_mrrc(c, insn); return; }
    LOG_ONCE("[cpu] mcrr/mrrc cp%u ignored\n", cp);
}

static void op_svc(cpu_t *c, u32 insn)
{
    u32 imm = insn & 0xFFFFFF;
    if (imm == 0) {
        /* raw Linux syscall via svc 0: r7 = nr. Only a couple make sense. */
        u32 nr = c->r[7];
        LOG_ONCE("[cpu] raw svc nr=%u: stubbed (returns 0)\n", nr);
        if (nr == 20 || nr == 24 || nr == 64 || nr == 224) c->r[0] = 1000 + (u32)c->tid; /* getpid/gettid/getuid */
        else c->r[0] = 0;
        return;
    }
    if ((imm >> 16) == 0x52) { patch_svc(c, imm & 0xFFFF); return; }   /* host patch point (patches.c) */
    LOG_ONCE("[cpu] svc #%08x ignored\n", imm);
}

/* ------------------------------------------------------------------ */
/* classifier                                                          */
/* ------------------------------------------------------------------ */

void cpu_init_decode(void)
{
    for (u32 key = 0; key < 4096; key++) {
        u32 b2720 = key >> 4;   /* instr bits 27-20 */
        u32 b74 = key & 0xF;    /* instr bits 7-4 */
        handler_t h = op_undef;
        switch (b2720 >> 5) {
        case 0: /* 000: dp / mul / extra-ls / misc */
            if (b74 == 0x9) {
                if ((b2720 & 0xFC) == 0x00) h = op_mul;
                else if ((b2720 & 0xF8) == 0x08) h = op_mullong;
                else if ((b2720 & 0xF9) == 0x10) h = op_misc;   /* swp vs smlalxy: runtime */
                else h = op_undef;                       /* ldrex/strex: unsupported */
            } else if (b74 == 0xB || b74 == 0xD || b74 == 0xF) {
                h = op_ls_extra;
            } else if ((b2720 & 0xF9) == 0x10) {
                h = op_misc;                             /* mrs/msr/bx/blx/clz/bkpt/qxx/dsp/swp */
            } else {
                h = op_dp;
            }
            break;
        case 1: /* 001: dp immediate / msr imm */
            h = ((b2720 & 0xFB) == 0x32) ? op_msr_imm : op_dp;
            break;
        case 2: /* 010: ldr/str immediate (bits 7-4 are offset) */
            h = op_ls;
            break;
        case 3: /* 011: ldr/str register (bit4=1 is media/undef) */
            h = (b74 & 1) ? op_undef : op_ls;
            break;
        case 4: h = op_ldmstm; break;
        case 5: h = op_branch; break;
        case 6: /* 110: ldc/stc or mcrr/mrrc */
            if ((b2720 & 0xFE) == 0xC4) h = op_mcrr_mrrc;
            else h = op_ldc_stc;
            break;
        case 7: /* 111: svc / cdp / mcr,mrc */
            if (b2720 & 0x10) h = op_svc;
            else if (!(b74 & 1)) h = op_cdp;
            else h = op_mcr_mrc;
            break;
        }
        dec_table[key] = h;
    }
}
