/*
 * fastops.h - specialised handlers for the pre-decoded interpreter.
 * Included by dec.c (function table for decode + selftest fuzz) and cpu.c
 * (computed-goto labels, so each handler gets its own dispatch branch).
 * All handlers assume no r15 operand except where noted.
 */
#ifndef FASTOPS_H
#define FASTOPS_H
#include "emu.h"

static inline __attribute__((always_inline))
void dp_exec(cpu_t *c, const int opc, const int S, u32 a, u32 b, u32 co, u32 rd)
{
    u32 r;
    u32 f = c->cpsr;
#define NZ(x) (((x) & 0x80000000u) | ((x) == 0 ? FLAG_Z : 0))
    switch (opc) {
    case 0x0: r = a & b;  if (S) c->cpsr = (f & 0x1FFFFFFFu) | NZ(r) | co; c->r[rd] = r; return;
    case 0x1: r = a ^ b;  if (S) c->cpsr = (f & 0x1FFFFFFFu) | NZ(r) | co; c->r[rd] = r; return;
    case 0xC: r = a | b;  if (S) c->cpsr = (f & 0x1FFFFFFFu) | NZ(r) | co; c->r[rd] = r; return;
    case 0xD: r = b;      if (S) c->cpsr = (f & 0x1FFFFFFFu) | NZ(r) | co; c->r[rd] = r; return;
    case 0xE: r = a & ~b; if (S) c->cpsr = (f & 0x1FFFFFFFu) | NZ(r) | co; c->r[rd] = r; return;
    case 0xF: r = ~b;     if (S) c->cpsr = (f & 0x1FFFFFFFu) | NZ(r) | co; c->r[rd] = r; return;
    case 0x8: r = a & b;  c->cpsr = (f & 0x1FFFFFFFu) | NZ(r) | co; return;
    case 0x9: r = a ^ b;  c->cpsr = (f & 0x1FFFFFFFu) | NZ(r) | co; return;
    case 0x2: case 0xA: {                                      /* SUB / CMP */
        r = a - b;
        if (S || opc == 0xA)
            c->cpsr = (f & 0x0FFFFFFFu) | NZ(r) | (a >= b ? FLAG_C : 0) |
                      (((a ^ b) & (a ^ r) & 0x80000000u) ? FLAG_V : 0);
        if (opc == 0x2) c->r[rd] = r;
        return; }
    case 0x3:                                                  /* RSB */
        r = b - a;
        if (S) c->cpsr = (f & 0x0FFFFFFFu) | NZ(r) | (b >= a ? FLAG_C : 0) |
                         (((b ^ a) & (b ^ r) & 0x80000000u) ? FLAG_V : 0);
        c->r[rd] = r;
        return;
    case 0x4: case 0xB: {                                      /* ADD / CMN */
        u64 w = (u64)a + b; r = (u32)w;
        if (S || opc == 0xB)
            c->cpsr = (f & 0x0FFFFFFFu) | NZ(r) | ((w >> 32) ? FLAG_C : 0) |
                      ((~(a ^ b) & (a ^ r) & 0x80000000u) ? FLAG_V : 0);
        if (opc == 0x4) c->r[rd] = r;
        return; }
    case 0x5: {                                                /* ADC */
        u64 w = (u64)a + b + ((f >> 29) & 1); r = (u32)w;
        if (S) c->cpsr = (f & 0x0FFFFFFFu) | NZ(r) | ((w >> 32) ? FLAG_C : 0) |
                         ((~(a ^ b) & (a ^ r) & 0x80000000u) ? FLAG_V : 0);
        c->r[rd] = r;
        return; }
    case 0x6: {                                                /* SBC */
        u64 sub = (u64)b + (((f >> 29) & 1) ^ 1);
        r = (u32)((u64)a - sub);
        if (S) c->cpsr = (f & 0x0FFFFFFFu) | NZ(r) | ((u64)a >= sub ? FLAG_C : 0) |
                         (((a ^ b) & (a ^ r) & 0x80000000u) ? FLAG_V : 0);
        c->r[rd] = r;
        return; }
    default: {                                                 /* RSC */
        u64 sub = (u64)a + (((f >> 29) & 1) ^ 1);
        r = (u32)((u64)b - sub);
        if (S) c->cpsr = (f & 0x0FFFFFFFu) | NZ(r) | ((u64)b >= sub ? FLAG_C : 0) |
                         (((b ^ a) & (b ^ r) & 0x80000000u) ? FLAG_V : 0);
        c->r[rd] = r;
        return; }
    }
#undef NZ
}

/* immediate shift of rm; d->b = type | amount << 2 (amount 0 keeps ARM's special cases) */
static inline __attribute__((always_inline)) u32 shift_imm(cpu_t *c, u32 v, u32 tb, u32 *co)
{
    u32 type = tb & 3, sh = tb >> 2, cin = c->cpsr & FLAG_C;
    switch (type) {
    case 0:
        if (!sh) { *co = cin; return v; }
        *co = (v >> (32 - sh)) & 1 ? FLAG_C : 0; return v << sh;
    case 1:
        if (!sh) { *co = v >> 31 ? FLAG_C : 0; return 0; }
        *co = (v >> (sh - 1)) & 1 ? FLAG_C : 0; return v >> sh;
    case 2:
        if (!sh) { *co = v >> 31 ? FLAG_C : 0; return (u32)((s32)v >> 31); }
        *co = (v >> (sh - 1)) & 1 ? FLAG_C : 0; return (u32)((s32)v >> sh);
    default:
        if (!sh) { *co = v & 1 ? FLAG_C : 0; return (v >> 1) | (cin << 2); }   /* RRX: C is bit 29 */
        *co = (v >> (sh - 1)) & 1 ? FLAG_C : 0; return (v >> sh) | (v << (32 - sh));
    }
}

#define DP_FNS(opc) \
    static inline void dp_##opc##_0i(cpu_t *c, const di_t *d) { dp_exec(c, opc, 0, c->r[d->rn], d->a, 0, d->rd); } \
    static inline void dp_##opc##_1i(cpu_t *c, const di_t *d) { dp_exec(c, opc, 1, c->r[d->rn], d->a, d->b ? (d->a & 0x80000000u) >> 2 : c->cpsr & FLAG_C, d->rd); } \
    static inline void dp_##opc##_0r(cpu_t *c, const di_t *d) { dp_exec(c, opc, 0, c->r[d->rn], c->r[d->rm], 0, d->rd); } \
    static inline void dp_##opc##_1r(cpu_t *c, const di_t *d) { dp_exec(c, opc, 1, c->r[d->rn], c->r[d->rm], c->cpsr & FLAG_C, d->rd); } \
    static inline void dp_##opc##_0s(cpu_t *c, const di_t *d) { u32 co; u32 b = shift_imm(c, c->r[d->rm], d->b, &co); dp_exec(c, opc, 0, c->r[d->rn], b, 0, d->rd); } \
    static inline void dp_##opc##_1s(cpu_t *c, const di_t *d) { u32 co; u32 b = shift_imm(c, c->r[d->rm], d->b, &co); dp_exec(c, opc, 1, c->r[d->rn], b, co, d->rd); }
DP_FNS(0x0) DP_FNS(0x1) DP_FNS(0x2) DP_FNS(0x3) DP_FNS(0x4) DP_FNS(0x5) DP_FNS(0x6) DP_FNS(0x7)
DP_FNS(0x8) DP_FNS(0x9) DP_FNS(0xA) DP_FNS(0xB) DP_FNS(0xC) DP_FNS(0xD) DP_FNS(0xE) DP_FNS(0xF)


/* ------------------------------------------------------------------ */
/* load / store                                                        */
/* ------------------------------------------------------------------ */

/* d->a: signed offset (as u32 add), or absolute address for literals */
static inline void ls_ldr_lit(cpu_t *c, const di_t *d)  { c->r[d->rd] = ld32(d->a); }
static inline void ls_ldr(cpu_t *c, const di_t *d)      { c->r[d->rd] = ld32(c->r[d->rn] + d->a); }
static inline void ls_str(cpu_t *c, const di_t *d)      { st32(c->r[d->rn] + d->a, c->r[d->rd]); }
static inline void ls_ldrb(cpu_t *c, const di_t *d)     { c->r[d->rd] = ld8(c->r[d->rn] + d->a); }
static inline void ls_strb(cpu_t *c, const di_t *d)     { st8(c->r[d->rn] + d->a, (u8)c->r[d->rd]); }
static inline void ls_ldrh(cpu_t *c, const di_t *d)     { c->r[d->rd] = ld16(c->r[d->rn] + d->a); }
static inline void ls_strh(cpu_t *c, const di_t *d)     { st16(c->r[d->rn] + d->a, (u16)c->r[d->rd]); }
static inline void ls_ldrsb(cpu_t *c, const di_t *d)    { c->r[d->rd] = (u32)(s32)(s8)ld8(c->r[d->rn] + d->a); }
static inline void ls_ldrsh(cpu_t *c, const di_t *d)    { c->r[d->rd] = (u32)(s32)(s16)ld16(c->r[d->rn] + d->a); }
/* pre-index writeback (e.g. str rX, [sp, #-4]!) and post-index (ldr rX, [rY], #4) */
static inline void ls_ldr_pre(cpu_t *c, const di_t *d)  { u32 ea = c->r[d->rn] + d->a; c->r[d->rn] = ea; c->r[d->rd] = ld32(ea); }
static inline void ls_str_pre(cpu_t *c, const di_t *d)  { u32 ea = c->r[d->rn] + d->a; u32 v = c->r[d->rd]; c->r[d->rn] = ea; st32(ea, v); }
static inline void ls_ldr_post(cpu_t *c, const di_t *d) { u32 ea = c->r[d->rn]; c->r[d->rn] = ea + d->a; c->r[d->rd] = ld32(ea); }
static inline void ls_str_post(cpu_t *c, const di_t *d) { u32 ea = c->r[d->rn]; u32 v = c->r[d->rd]; c->r[d->rn] = ea + d->a; st32(ea, v); }
static inline void ls_ldrb_post(cpu_t *c, const di_t *d) { u32 ea = c->r[d->rn]; c->r[d->rn] = ea + d->a; c->r[d->rd] = ld8(ea); }
static inline void ls_strb_post(cpu_t *c, const di_t *d) { u32 ea = c->r[d->rn]; u32 v = c->r[d->rd]; c->r[d->rn] = ea + d->a; st8(ea, (u8)v); }
/* register offset, LSL #n, no writeback; d->b = shift, d->a = 0 add / 1 subtract */
static inline void ls_ldr_reg(cpu_t *c, const di_t *d)  { u32 o = c->r[d->rm] << d->b; c->r[d->rd] = ld32(c->r[d->rn] + (d->a ? -o : o)); }
static inline void ls_str_reg(cpu_t *c, const di_t *d)  { u32 o = c->r[d->rm] << d->b; st32(c->r[d->rn] + (d->a ? -o : o), c->r[d->rd]); }
static inline void ls_ldrb_reg(cpu_t *c, const di_t *d) { u32 o = c->r[d->rm] << d->b; c->r[d->rd] = ld8(c->r[d->rn] + (d->a ? -o : o)); }
static inline void ls_strb_reg(cpu_t *c, const di_t *d) { u32 o = c->r[d->rm] << d->b; st8(c->r[d->rn] + (d->a ? -o : o), (u8)c->r[d->rd]); }

/* ------------------------------------------------------------------ */
/* branches / ldm-stm                                                  */
/* ------------------------------------------------------------------ */

static inline void ring_push(cpu_t *c, u32 target)
{
    c->ring[c->ring_pos = (c->ring_pos + 1) & 63] = target;
}
static inline void br_b(cpu_t *c, const di_t *d)  { ring_push(c, d->a); c->r[15] = d->a; }
static inline void br_bl(cpu_t *c, const di_t *d) { ring_push(c, d->a); c->r[14] = d->b; c->r[15] = d->a; }
static inline void br_bx(cpu_t *c, const di_t *d) { cpu_branch(c, c->r[d->rm]); }

/* push/pop fast paths: stmdb sp!, {list} / ldmia sp!, {list} without r15 / base in list */
static inline void ldm_ia_wb(cpu_t *c, const di_t *d)
{
    u32 a = c->r[d->rn], list = d->a;
    while (list) { int i = __builtin_ctz(list); list &= list - 1; c->r[i] = ld32(a); a += 4; }
    c->r[d->rn] = a;
}
static inline void ldm_ia_wb_pc(cpu_t *c, const di_t *d)
{
    u32 a = c->r[d->rn], list = d->a;
    while (list) { int i = __builtin_ctz(list); list &= list - 1; c->r[i] = ld32(a); a += 4; }
    u32 t = ld32(a);
    c->r[d->rn] = a + 4;
    cpu_branch(c, t);
}
static inline void stm_db_wb(cpu_t *c, const di_t *d)
{
    u32 a = c->r[d->rn] - d->b, list = d->a;
    c->r[d->rn] = a;
    while (list) { int i = __builtin_ctz(list); list &= list - 1; st32(a, c->r[i]); a += 4; }
}


/* register-specified shift of rm by rs (low byte), ARM semantics; d->b = type */
static inline __attribute__((always_inline)) u32 shift_reg(cpu_t *c, u32 v, u32 type, u32 sh, u32 *co)
{
    u32 cin = c->cpsr & FLAG_C;
    *co = cin;
    if (sh == 0) return v;
    switch (type) {
    case 0:
        if (sh < 32) { *co = (v >> (32 - sh)) & 1 ? FLAG_C : 0; return v << sh; }
        *co = (sh == 32 && (v & 1)) ? FLAG_C : 0; return 0;
    case 1:
        if (sh < 32) { *co = (v >> (sh - 1)) & 1 ? FLAG_C : 0; return v >> sh; }
        *co = (sh == 32 && (v >> 31)) ? FLAG_C : 0; return 0;
    case 2:
        if (sh < 32) { *co = (v >> (sh - 1)) & 1 ? FLAG_C : 0; return (u32)((s32)v >> sh); }
        *co = (v >> 31) ? FLAG_C : 0; return (u32)((s32)v >> 31);
    default:
        sh &= 31;
        if (sh == 0) { *co = (v >> 31) ? FLAG_C : 0; return v; }
        *co = (v >> (sh - 1)) & 1 ? FLAG_C : 0; return (v >> sh) | (v << (32 - sh));
    }
}
#define DP_RS_FNS(opc) \
    static inline void dp_##opc##_0x(cpu_t *c, const di_t *d) { u32 co; u32 b = shift_reg(c, c->r[d->rm], d->b, c->r[d->a] & 0xFF, &co); dp_exec(c, opc, 0, c->r[d->rn], b, 0, d->rd); } \
    static inline void dp_##opc##_1x(cpu_t *c, const di_t *d) { u32 co; u32 b = shift_reg(c, c->r[d->rm], d->b, c->r[d->a] & 0xFF, &co); dp_exec(c, opc, 1, c->r[d->rn], b, co, d->rd); }
DP_RS_FNS(0x0) DP_RS_FNS(0x1) DP_RS_FNS(0x2) DP_RS_FNS(0x3) DP_RS_FNS(0x4) DP_RS_FNS(0x5) DP_RS_FNS(0x6) DP_RS_FNS(0x7)
DP_RS_FNS(0x8) DP_RS_FNS(0x9) DP_RS_FNS(0xA) DP_RS_FNS(0xB) DP_RS_FNS(0xC) DP_RS_FNS(0xD) DP_RS_FNS(0xE) DP_RS_FNS(0xF)

/* pc-relative arithmetic folded at decode time */
static inline void mov_const(cpu_t *c, const di_t *d)   { c->r[d->rd] = d->a; }
static inline void add_pc_reg(cpu_t *c, const di_t *d)  { c->r[d->rd] = d->a + c->r[d->rm]; }
static inline void nop(cpu_t *c, const di_t *d)         { (void)c; (void)d; }

/* loads into pc: vtable calls, PLT stubs (ldr pc, [ip, #x]!) */
static inline void ldr_pc(cpu_t *c, const di_t *d)      { cpu_branch(c, ld32(c->r[d->rn] + d->a)); }
static inline void ldr_pc_pre(cpu_t *c, const di_t *d)  { u32 ea = c->r[d->rn] + d->a; c->r[d->rn] = ea; cpu_branch(c, ld32(ea)); }

/* extra load/store: register offset (U=1, no wb) and imm pre/post-index writeback */
static inline void ls_ldrh_rr(cpu_t *c, const di_t *d)   { c->r[d->rd] = ld16(c->r[d->rn] + c->r[d->rm]); }
static inline void ls_strh_rr(cpu_t *c, const di_t *d)   { st16(c->r[d->rn] + c->r[d->rm], (u16)c->r[d->rd]); }
static inline void ls_ldrsb_rr(cpu_t *c, const di_t *d)  { c->r[d->rd] = (u32)(s32)(s8)ld8(c->r[d->rn] + c->r[d->rm]); }
static inline void ls_ldrsh_rr(cpu_t *c, const di_t *d)  { c->r[d->rd] = (u32)(s32)(s16)ld16(c->r[d->rn] + c->r[d->rm]); }
static inline void ls_ldrh_pre(cpu_t *c, const di_t *d)  { u32 ea = c->r[d->rn] + d->a; c->r[d->rn] = ea; c->r[d->rd] = ld16(ea); }
static inline void ls_strh_pre(cpu_t *c, const di_t *d)  { u32 ea = c->r[d->rn] + d->a; u32 v = c->r[d->rd]; c->r[d->rn] = ea; st16(ea, (u16)v); }
static inline void ls_ldrsb_pre(cpu_t *c, const di_t *d) { u32 ea = c->r[d->rn] + d->a; c->r[d->rn] = ea; c->r[d->rd] = (u32)(s32)(s8)ld8(ea); }
static inline void ls_ldrsh_pre(cpu_t *c, const di_t *d) { u32 ea = c->r[d->rn] + d->a; c->r[d->rn] = ea; c->r[d->rd] = (u32)(s32)(s16)ld16(ea); }
static inline void ls_ldrh_post(cpu_t *c, const di_t *d)  { u32 ea = c->r[d->rn]; c->r[d->rn] = ea + d->a; c->r[d->rd] = ld16(ea); }
static inline void ls_strh_post(cpu_t *c, const di_t *d)  { u32 ea = c->r[d->rn]; u32 v = c->r[d->rd]; c->r[d->rn] = ea + d->a; st16(ea, (u16)v); }
static inline void ls_ldrsb_post(cpu_t *c, const di_t *d) { u32 ea = c->r[d->rn]; c->r[d->rn] = ea + d->a; c->r[d->rd] = (u32)(s32)(s8)ld8(ea); }
static inline void ls_ldrsh_post(cpu_t *c, const di_t *d) { u32 ea = c->r[d->rn]; c->r[d->rn] = ea + d->a; c->r[d->rd] = (u32)(s32)(s16)ld16(ea); }
static inline void ls_ldrb_pre(cpu_t *c, const di_t *d)  { u32 ea = c->r[d->rn] + d->a; c->r[d->rn] = ea; c->r[d->rd] = ld8(ea); }
static inline void ls_strb_pre(cpu_t *c, const di_t *d)  { u32 ea = c->r[d->rn] + d->a; u32 v = c->r[d->rd]; c->r[d->rn] = ea; st8(ea, (u8)v); }

/* multiply: d->rn = Rd (19-16), d->rd = Rn accumulator (15-12), d->b = Rs */
static inline void mul_(cpu_t *c, const di_t *d) { c->r[d->rn] = c->r[d->rm] * c->r[d->b]; }
static inline void mla_(cpu_t *c, const di_t *d) { c->r[d->rn] = c->r[d->rm] * c->r[d->b] + c->r[d->rd]; }

/* VFP single precision: d->rd/rn/rm hold s-register numbers (0-31) */
static inline void vldr_s(cpu_t *c, const di_t *d)     { c->v.w[d->rd] = ld32(c->r[d->rn] + d->a); }
static inline void vldr_s_lit(cpu_t *c, const di_t *d) { c->v.w[d->rd] = ld32(d->a); }
static inline void vstr_s(cpu_t *c, const di_t *d)     { st32(c->r[d->rn] + d->a, c->v.w[d->rd]); }
static inline void vldr_d(cpu_t *c, const di_t *d)     { c->v.d[d->rd] = ldf64(c->r[d->rn] + d->a); }
static inline void vldr_d_lit(cpu_t *c, const di_t *d) { c->v.d[d->rd] = ldf64(d->a); }
static inline void vstr_d(cpu_t *c, const di_t *d)     { stf64(c->r[d->rn] + d->a, c->v.d[d->rd]); }
static inline void vmov_rs(cpu_t *c, const di_t *d)    { c->r[d->rd] = c->v.w[d->rn]; }   /* rd = core, rn = s */
static inline void vmov_sr(cpu_t *c, const di_t *d)    { c->v.w[d->rn] = c->r[d->rd]; }
static inline void vmrs_apsr(cpu_t *c, const di_t *d)  { (void)d; c->cpsr = (c->cpsr & 0x0FFFFFFFu) | (c->fpscr & 0xF0000000u); }
static inline void vadd_s(cpu_t *c, const di_t *d)  { c->v.f[d->rd] = c->v.f[d->rn] + c->v.f[d->rm]; }
static inline void vsub_s(cpu_t *c, const di_t *d)  { c->v.f[d->rd] = c->v.f[d->rn] - c->v.f[d->rm]; }
static inline void vmul_s(cpu_t *c, const di_t *d)  { c->v.f[d->rd] = c->v.f[d->rn] * c->v.f[d->rm]; }
static inline void vnmul_s(cpu_t *c, const di_t *d) { c->v.f[d->rd] = -(c->v.f[d->rn] * c->v.f[d->rm]); }
static inline void vmla_s(cpu_t *c, const di_t *d)  { c->v.f[d->rd] = c->v.f[d->rn] * c->v.f[d->rm] + c->v.f[d->rd]; }
static inline void vmls_s(cpu_t *c, const di_t *d)  { c->v.f[d->rd] = c->v.f[d->rd] - c->v.f[d->rn] * c->v.f[d->rm]; }
static inline void vdiv_s(cpu_t *c, const di_t *d)  { c->v.f[d->rd] = c->v.f[d->rn] / c->v.f[d->rm]; }
static inline void vmov_ss(cpu_t *c, const di_t *d) { c->v.f[d->rd] = c->v.f[d->rm]; }
static inline void vneg_s(cpu_t *c, const di_t *d)  { c->v.f[d->rd] = -c->v.f[d->rm]; }
static inline void vabs_s(cpu_t *c, const di_t *d)  { c->v.f[d->rd] = fabsf(c->v.f[d->rm]); }
static inline void vsqrt_s(cpu_t *c, const di_t *d) { c->v.f[d->rd] = sqrtf(c->v.f[d->rm]); }
static inline void vmov_imm_s(cpu_t *c, const di_t *d) { c->v.w[d->rd] = d->a; }
static inline void vcmp_flags(cpu_t *c, f64 a, f64 b)
{
    u32 f;
    if (isunordered(a, b)) f = 0x30000000u;
    else if (a == b)       f = FLAG_Z | FLAG_C;
    else if (a < b)        f = FLAG_N;
    else                   f = FLAG_C;
    c->fpscr = (c->fpscr & 0x0FFFFFFFu) | f;
}
static inline void vcmp_s(cpu_t *c, const di_t *d)  { vcmp_flags(c, c->v.f[d->rd], c->v.f[d->rm]); }
static inline void vcmpz_s(cpu_t *c, const di_t *d) { vcmp_flags(c, c->v.f[d->rd], 0.0f); }
static inline void vcvt_s_s32(cpu_t *c, const di_t *d) { c->v.f[d->rd] = (f32)(s32)c->v.w[d->rm]; }
static inline void vcvt_s_u32(cpu_t *c, const di_t *d) { c->v.f[d->rd] = (f32)c->v.w[d->rm]; }
static inline void vcvt_s32_s_rz(cpu_t *c, const di_t *d)
{
    f64 v = c->v.f[d->rm], r = trunc(v);
    c->v.w[d->rd] = isnan(v) ? 0 : r >= 2147483647.0 ? 0x7FFFFFFFu : r <= -2147483648.0 ? 0x80000000u : (u32)(s32)r;
}
static inline void vcvt_d_s(cpu_t *c, const di_t *d) { c->v.d[d->rd] = (f64)c->v.f[d->rm]; }   /* rd = d-reg */
static inline void vcvt_s_d(cpu_t *c, const di_t *d) { c->v.f[d->rd] = (f32)c->v.d[d->rm]; }   /* rm = d-reg */

/* ---- race-workload additions ---- */

/* VFP load/store multiple. d->rd = first s/d reg, d->a = count, d->b = W | imm8 << 8 */
#define VLSM(name, L, dbl, db) \
static inline void name(cpu_t *c, const di_t *d) \
{ \
    u32 base = c->r[d->rn], bytes = (d->b >> 8) * 4u;     /* imm8*4, as FSTMX/FLDMX adjust */ \
    u32 addr = db ? base - bytes : base; \
    for (u32 i = 0; i < d->a; i++) { \
        if (dbl) { if (L) c->v.q[d->rd + i] = ld32(addr) | ((u64)ld32(addr + 4) << 32); \
                   else { st32(addr, (u32)c->v.q[d->rd + i]); st32(addr + 4, (u32)(c->v.q[d->rd + i] >> 32)); } \
                   addr += 8; } \
        else { if (L) c->v.w[d->rd + i] = ld32(addr); else st32(addr, c->v.w[d->rd + i]); addr += 4; } \
    } \
    if (d->b & 1) c->r[d->rn] = db ? base - bytes : base + bytes; \
}
VLSM(vldm_s_ia, 1, 0, 0) VLSM(vstm_s_ia, 0, 0, 0) VLSM(vldm_s_db, 1, 0, 1) VLSM(vstm_s_db, 0, 0, 1)
VLSM(vldm_d_ia, 1, 1, 0) VLSM(vstm_d_ia, 0, 1, 0) VLSM(vldm_d_db, 1, 1, 1) VLSM(vstm_d_db, 0, 1, 1)

/* ldrd/strd: d->a = signed imm offset (imm forms) */
static inline void ldrd_i(cpu_t *c, const di_t *d) { u32 ea = c->r[d->rn] + d->a; u32 lo = ld32(ea), hi = ld32(ea + 4); c->r[d->rd] = lo; c->r[d->rd + 1] = hi; }
static inline void strd_i(cpu_t *c, const di_t *d) { u32 ea = c->r[d->rn] + d->a; st32(ea, c->r[d->rd]); st32(ea + 4, c->r[d->rd + 1]); }
static inline void ldrd_r(cpu_t *c, const di_t *d) { u32 ea = c->r[d->rn] + c->r[d->rm]; u32 lo = ld32(ea), hi = ld32(ea + 4); c->r[d->rd] = lo; c->r[d->rd + 1] = hi; }
static inline void strd_r(cpu_t *c, const di_t *d) { u32 ea = c->r[d->rn] + c->r[d->rm]; st32(ea, c->r[d->rd]); st32(ea + 4, c->r[d->rd + 1]); }

/* long multiplies, S=0: d->rd = RdLo, d->rn = RdHi, d->b = Rs */
static inline void umull_(cpu_t *c, const di_t *d) { u64 r = (u64)c->r[d->rm] * c->r[d->b]; c->r[d->rd] = (u32)r; c->r[d->rn] = (u32)(r >> 32); }
static inline void smull_(cpu_t *c, const di_t *d) { u64 r = (u64)((s64)(s32)c->r[d->rm] * (s32)c->r[d->b]); c->r[d->rd] = (u32)r; c->r[d->rn] = (u32)(r >> 32); }
static inline void umlal_(cpu_t *c, const di_t *d) { u64 r = (u64)c->r[d->rm] * c->r[d->b] + (((u64)c->r[d->rn] << 32) | c->r[d->rd]); c->r[d->rd] = (u32)r; c->r[d->rn] = (u32)(r >> 32); }
static inline void smlal_(cpu_t *c, const di_t *d) { u64 r = (u64)((s64)(s32)c->r[d->rm] * (s32)c->r[d->b]) + (((u64)c->r[d->rn] << 32) | c->r[d->rd]); c->r[d->rd] = (u32)r; c->r[d->rn] = (u32)(r >> 32); }

static inline void ldr_pcreg(cpu_t *c, const di_t *d) { c->r[d->rd] = ld32(d->a + (c->r[d->rm] << d->b)); }  /* ldr rd, [pc, rm, lsl #n] */
static inline void vnmls_s(cpu_t *c, const di_t *d) { c->v.f[d->rd] = c->v.f[d->rn] * c->v.f[d->rm] - c->v.f[d->rd]; }
static inline void vnmla_s(cpu_t *c, const di_t *d) { c->v.f[d->rd] = -(c->v.f[d->rn] * c->v.f[d->rm]) - c->v.f[d->rd]; }
static inline void br_blx(cpu_t *c, const di_t *d) { u32 t = c->r[d->rm]; c->r[14] = d->b; cpu_branch(c, t); }
static inline void clz_(cpu_t *c, const di_t *d) { u32 v = c->r[d->rm]; c->r[d->rd] = v ? (u32)__builtin_clz(v) : 32; }

/* ldm/stm IA without writeback; stm IA with writeback. d->a = list (no pc, no base) */
static inline void ldm_ia(cpu_t *c, const di_t *d)
{ u32 a = c->r[d->rn], l = d->a; while (l) { int i = __builtin_ctz(l); l &= l - 1; c->r[i] = ld32(a); a += 4; } }
static inline void stm_ia(cpu_t *c, const di_t *d)
{ u32 a = c->r[d->rn], l = d->a; while (l) { int i = __builtin_ctz(l); l &= l - 1; st32(a, c->r[i]); a += 4; } }
static inline void stm_ia_wb(cpu_t *c, const di_t *d)
{ u32 a = c->r[d->rn], l = d->a; while (l) { int i = __builtin_ctz(l); l &= l - 1; st32(a, c->r[i]); a += 4; } c->r[d->rn] = a; }

/* X-macro list of every fast op. Order defines the op ids. */
#define DP_OPS(X, opc) X(dp_##opc##_0i) X(dp_##opc##_1i) X(dp_##opc##_0r) X(dp_##opc##_1r) X(dp_##opc##_0s) X(dp_##opc##_1s)
#define DP_RS_OPS(X, opc) X(dp_##opc##_0x) X(dp_##opc##_1x)
#define FAST_OPS(X) \
    DP_OPS(X, 0x0) DP_OPS(X, 0x1) DP_OPS(X, 0x2) DP_OPS(X, 0x3) DP_OPS(X, 0x4) DP_OPS(X, 0x5) \
    DP_OPS(X, 0x6) DP_OPS(X, 0x7) DP_OPS(X, 0x8) DP_OPS(X, 0x9) DP_OPS(X, 0xA) DP_OPS(X, 0xB) \
    DP_OPS(X, 0xC) DP_OPS(X, 0xD) DP_OPS(X, 0xE) DP_OPS(X, 0xF) \
    X(ls_ldr_lit) X(ls_ldr) X(ls_str) X(ls_ldrb) X(ls_strb) X(ls_ldrh) X(ls_strh) X(ls_ldrsb) X(ls_ldrsh) \
    X(ls_ldr_pre) X(ls_str_pre) X(ls_ldr_post) X(ls_str_post) X(ls_ldrb_post) X(ls_strb_post) \
    X(ls_ldr_reg) X(ls_str_reg) X(ls_ldrb_reg) X(ls_strb_reg) \
    X(br_b) X(br_bl) X(br_bx) X(ldm_ia_wb) X(ldm_ia_wb_pc) X(stm_db_wb) \
    DP_RS_OPS(X, 0x0) DP_RS_OPS(X, 0x1) DP_RS_OPS(X, 0x2) DP_RS_OPS(X, 0x3) DP_RS_OPS(X, 0x4) DP_RS_OPS(X, 0x5) \
    DP_RS_OPS(X, 0x6) DP_RS_OPS(X, 0x7) DP_RS_OPS(X, 0x8) DP_RS_OPS(X, 0x9) DP_RS_OPS(X, 0xA) DP_RS_OPS(X, 0xB) \
    DP_RS_OPS(X, 0xC) DP_RS_OPS(X, 0xD) DP_RS_OPS(X, 0xE) DP_RS_OPS(X, 0xF) \
    X(mov_const) X(add_pc_reg) X(nop) X(ldr_pc) X(ldr_pc_pre) \
    X(ls_ldrh_rr) X(ls_strh_rr) X(ls_ldrsb_rr) X(ls_ldrsh_rr) \
    X(ls_ldrh_pre) X(ls_strh_pre) X(ls_ldrsb_pre) X(ls_ldrsh_pre) \
    X(ls_ldrh_post) X(ls_strh_post) X(ls_ldrsb_post) X(ls_ldrsh_post) X(ls_ldrb_pre) X(ls_strb_pre) \
    X(mul_) X(mla_) \
    X(vldr_s) X(vldr_s_lit) X(vstr_s) X(vldr_d) X(vldr_d_lit) X(vstr_d) X(vmov_rs) X(vmov_sr) X(vmrs_apsr) \
    X(vadd_s) X(vsub_s) X(vmul_s) X(vnmul_s) X(vmla_s) X(vmls_s) X(vdiv_s) \
    X(vmov_ss) X(vneg_s) X(vabs_s) X(vsqrt_s) X(vmov_imm_s) X(vcmp_s) X(vcmpz_s) \
    X(vcvt_s_s32) X(vcvt_s_u32) X(vcvt_s32_s_rz) X(vcvt_d_s) X(vcvt_s_d) \
    X(vldm_s_ia) X(vstm_s_ia) X(vldm_s_db) X(vstm_s_db) X(vldm_d_ia) X(vstm_d_ia) X(vldm_d_db) X(vstm_d_db) \
    X(ldrd_i) X(strd_i) X(ldrd_r) X(strd_r) X(umull_) X(smull_) X(umlal_) X(smlal_) \
    X(ldr_pcreg) X(vnmls_s) X(vnmla_s) X(br_blx) X(clz_) X(ldm_ia) X(stm_ia) X(stm_ia_wb)

enum {
    OP_decode, OP_generic, OP_hook,
#define X(n) OP_##n,
    FAST_OPS(X)
#undef X
    OP_COUNT
};

#endif
