/*
 * arm7.c - ARMv6 / ARMv7-A additions to the ARM reference core: media
 * instructions (extend, reverse, saturate, bitfield, SIMD add/sub, v6
 * multiplies, divide), MOVW/MOVT, exclusives, hints, barriers and the
 * unconditional space (BLX imm, PLD, CLREX, NEON). Thumb-2 reuses these
 * through ARM-equivalent encodings (thumb.c).
 */
#include "emu.h"

void neon_dp(cpu_t *c, u32 insn);
void neon_ls(cpu_t *c, u32 insn);

static inline u32 rr7(cpu_t *c, u32 n) { return n == 15 ? c->r[15] + 4 : c->r[n]; }
static inline u32 ror32(u32 v, u32 s) { s &= 31; return s ? (v >> s) | (v << (32 - s)) : v; }

static void undef7(cpu_t *c, u32 insn)
{
    emu_trap(c, "undefined instruction %08x at %08x (v7 media/unconditional)", insn, c->r[15] - 4);
}

/* ---- saturation ---- */
static u32 ssat_q(cpu_t *c, s64 v, u32 bits)      /* signed saturate to `bits` (1..32) */
{
    s64 hi = (1LL << (bits - 1)) - 1, lo = -(1LL << (bits - 1));
    if (v > hi) { c->cpsr |= FLAG_Q; return (u32)hi; }
    if (v < lo) { c->cpsr |= FLAG_Q; return (u32)lo; }
    return (u32)v;
}
static u32 usat_q(cpu_t *c, s64 v, u32 bits)      /* unsigned saturate to `bits` (0..31) */
{
    s64 hi = bits >= 32 ? 0xFFFFFFFFLL : (1LL << bits) - 1;
    if (v > hi) { c->cpsr |= FLAG_Q; return (u32)hi; }
    if (v < 0) { c->cpsr |= FLAG_Q; return 0; }
    return (u32)v;
}

/* ---- parallel add/subtract (0110 0xxx): op1 = bits 22-20, op2 = bits 7-5 ---- */
static s32 sat_s(s32 v, int bits) { s32 hi = (1 << (bits - 1)) - 1, lo = -(1 << (bits - 1)); return v > hi ? hi : v < lo ? lo : v; }
static u32 sat_u(s32 v, int bits) { s32 hi = (1 << bits) - 1; return (u32)(v > hi ? hi : v < 0 ? 0 : v); }

void arm7_parallel(cpu_t *c, u32 insn)
{
    u32 op1 = (insn >> 20) & 7, op2 = (insn >> 5) & 7;
    u32 rn = (insn >> 16) & 0xF, rd = (insn >> 12) & 0xF, rm = insn & 0xF;
    bool u = insn & (1u << 22);
    u32 kind = op1 & 3;              /* 1 = plain (sets GE), 2 = saturating, 3 = halving */
    if (!kind || op2 == 5 || op2 == 6) undef7(c, insn);
    u32 a = c->r[rn], b = c->r[rm], res = 0, ge = 0;
    bool b8 = op2 >= 4;              /* 100 add8, 111 sub8; 000 add16 001 asx 010 sax 011 sub16 */
    if (b8) {
        bool sub = op2 == 7;
        for (int i = 0; i < 4; i++) {
            s32 x = u ? (s32)((a >> (8 * i)) & 0xFF) : (s32)(s8)(a >> (8 * i));
            s32 y = u ? (s32)((b >> (8 * i)) & 0xFF) : (s32)(s8)(b >> (8 * i));
            s32 r = sub ? x - y : x + y;
            u32 e;
            if (kind == 1) { e = (u32)r & 0xFF; if (u ? (sub ? r >= 0 : r >= 0x100) : r >= 0) ge |= 1u << i; }
            else if (kind == 2) e = u ? sat_u(r, 8) : (u32)sat_s(r, 8) & 0xFF;
            else e = (u32)(r >> 1) & 0xFF;
            res |= (e & 0xFF) << (8 * i);
        }
    } else {
        for (int i = 0; i < 2; i++) {
            /* lane i of the result: asx: lo = a.lo - b.hi, hi = a.hi + b.lo; sax: lo = a.lo + b.hi, hi = a.hi - b.lo */
            int bl = (op2 == 1 || op2 == 2) ? 1 - i : i;
            s32 x = u ? (s32)((a >> (16 * i)) & 0xFFFF) : (s32)(s16)(a >> (16 * i));
            s32 y = u ? (s32)((b >> (16 * bl)) & 0xFFFF) : (s32)(s16)(b >> (16 * bl));
            bool sub = op2 == 3 || (op2 == 1 && i == 0) || (op2 == 2 && i == 1);
            s32 r = sub ? x - y : x + y;
            u32 e;
            if (kind == 1) { e = (u32)r & 0xFFFF; if (u ? (sub ? r >= 0 : r >= 0x10000) : r >= 0) ge |= 3u << (2 * i); }
            else if (kind == 2) e = u ? sat_u(r, 16) : (u32)sat_s(r, 16) & 0xFFFF;
            else e = (u32)(r >> 1) & 0xFFFF;
            res |= (e & 0xFFFF) << (16 * i);
        }
    }
    if (kind == 1) c->cpsr = (c->cpsr & ~FLAG_GE) | (ge << 16);
    c->r[rd] = res;
}

/* ---- packing, unpacking, saturation, reversal (0110 1xxx) ---- */
void arm7_pack(cpu_t *c, u32 insn)
{
    u32 op1 = (insn >> 20) & 7, op2 = (insn >> 5) & 7;
    u32 rn = (insn >> 16) & 0xF, rd = (insn >> 12) & 0xF, rm = insn & 0xF;
    u32 rot = ((insn >> 10) & 3) * 8;
    u32 vm = c->r[rm];
    if (op1 == 0 && !(op2 & 1)) {                                  /* pkhbt / pkhtb */
        u32 sh = (insn >> 7) & 31, tb = (insn >> 6) & 1, v;
        if (tb) v = sh ? (u32)((s32)vm >> sh) : (u32)((s32)vm >> 31);
        else v = vm << sh;
        c->r[rd] = tb ? (c->r[rn] & 0xFFFF0000u) | (v & 0xFFFF) : (c->r[rn] & 0xFFFF) | (v & 0xFFFF0000u);
        return;
    }
    if ((op1 & 2) && !(op2 & 1)) {                                 /* ssat / usat */
        u32 sat = (insn >> 16) & 31, sh = (insn >> 7) & 31, n;
        s32 v = (insn & 0x40) ? (s32)vm >> (sh ? sh : 31) : (s32)(vm << sh);
        n = (op1 & 4) ? usat_q(c, v, sat) : ssat_q(c, v, sat + 1);
        c->r[rd] = n;
        return;
    }
    if (op2 == 1) {
        u32 a = rr7(c, rm);
        switch (op1) {
        case 2: {                                                  /* ssat16 */
            u32 sat = ((insn >> 16) & 15) + 1;
            u32 lo = ssat_q(c, (s16)a, sat) & 0xFFFF, hi = ssat_q(c, (s16)(a >> 16), sat) & 0xFFFF;
            c->r[rd] = lo | (hi << 16); return; }
        case 6: {                                                  /* usat16 */
            u32 sat = (insn >> 16) & 15;
            u32 lo = usat_q(c, (s16)a, sat), hi = usat_q(c, (s16)(a >> 16), sat);
            c->r[rd] = lo | (hi << 16); return; }
        case 3: c->r[rd] = __builtin_bswap32(a); return;           /* rev */
        case 7: {                                                  /* rbit */
            u32 v = a, r = 0;
            for (int i = 0; i < 32; i++) { r = (r << 1) | (v & 1); v >>= 1; }
            c->r[rd] = r; return; }
        }
    }
    if (op2 == 5) {
        u32 a = c->r[rm];
        if (op1 == 3) { c->r[rd] = ((a & 0x00FF00FFu) << 8) | ((a >> 8) & 0x00FF00FFu); return; }   /* rev16 */
        if (op1 == 7) { c->r[rd] = (u32)(s32)(s16)(((a & 0xFF) << 8) | ((a >> 8) & 0xFF)); return; } /* revsh */
    }
    if (op1 == 0 && op2 == 5) {                                    /* sel */
        u32 ge = (c->cpsr >> 16) & 15, a = c->r[rn], b = c->r[rm], r = 0;
        for (int i = 0; i < 4; i++) r |= ((ge >> i) & 1 ? a : b) & (0xFFu << (8 * i));
        c->r[rd] = r;
        return;
    }
    if (op2 == 3 && (op1 == 0 || op1 == 4)) {                      /* sxtab16 / uxtab16 (rn=15: sxtb16) */
        u32 v = ror32(vm, rot), add = rn == 15 ? 0 : c->r[rn];
        u32 lo = op1 ? (v & 0xFF) : (u32)(s32)(s8)v, hi = op1 ? ((v >> 16) & 0xFF) : (u32)(s32)(s8)(v >> 16);
        c->r[rd] = ((add + lo) & 0xFFFF) | (((add >> 16) + hi) << 16);
        return;
    }
    if (op2 == 3) {                                                /* sxtab sxtah uxtab uxtah (rn=15: plain) */
        u32 v = ror32(vm, rot), add = rn == 15 ? 0 : c->r[rn], e;
        switch (op1) {
        case 2: e = (u32)(s32)(s8)v; break;
        case 3: e = (u32)(s32)(s16)v; break;
        case 6: e = v & 0xFF; break;
        case 7: e = v & 0xFFFF; break;
        default: undef7(c, insn);
        }
        c->r[rd] = add + e;
        return;
    }
    undef7(c, insn);
}

/* ---- signed multiplies (0111 0xxx), divide ---- */
void arm7_smul(cpu_t *c, u32 insn)
{
    u32 op1 = (insn >> 20) & 7, op2 = (insn >> 5) & 7;
    u32 rd = (insn >> 16) & 0xF, ra = (insn >> 12) & 0xF, rm = (insn >> 8) & 0xF, rn = insn & 0xF;
    u32 a = c->r[rn], b = c->r[rm];
    bool x = op2 & 1;                                   /* M: swap halves of rm */
    if (op1 == 1 || op1 == 3) {                         /* sdiv / udiv */
        if (op2 != 0) undef7(c, insn);
        if (!b) { c->r[rd] = 0; return; }
        if (op1 == 1) c->r[rd] = (a == 0x80000000u && b == 0xFFFFFFFFu) ? a : (u32)((s32)a / (s32)b);
        else c->r[rd] = a / b;
        return;
    }
    if (x && op1 != 5) b = ror32(b, 16);
    s32 p1 = (s32)(s16)a * (s32)(s16)b, p2 = (s32)(s16)(a >> 16) * (s32)(s16)(b >> 16);
    switch (op1) {
    case 0: {                                           /* smlad smuad smlsd smusd */
        bool sub = op2 & 2;
        s64 r = sub ? (s64)p1 - p2 : (s64)p1 + p2;
        if (ra != 15) r += (s32)c->r[ra];
        if (r != (s32)r) c->cpsr |= FLAG_Q;
        c->r[rd] = (u32)r;
        return; }
    case 4: {                                           /* smlald smlsld: rd = hi, ra = lo */
        bool sub = op2 & 2;
        s64 r = sub ? (s64)p1 - p2 : (s64)p1 + p2;
        u64 acc = ((u64)c->r[rd] << 32) | c->r[ra];
        acc += (u64)r;
        c->r[ra] = (u32)acc; c->r[rd] = (u32)(acc >> 32);
        return; }
    case 5: {                                           /* smmla smmul smmls (+R rounding) */
        bool round = op2 & 1, sub = op2 & 4;
        s64 prod = (s64)(s32)a * (s32)b;
        s64 acc = ra == 15 ? 0 : (s64)((u64)c->r[ra] << 32);
        s64 r = sub ? acc - prod : acc + prod;
        if (round) r += 0x80000000LL;
        c->r[rd] = (u32)((u64)r >> 32);
        return; }
    }
    undef7(c, insn);
}

/* ---- usad8 / bitfield (0111 1xxx) ---- */
void arm7_bits(cpu_t *c, u32 insn)
{
    u32 op1 = (insn >> 20) & 0x1F, op2 = (insn >> 5) & 7;
    u32 rd = (insn >> 12) & 0xF, rn = insn & 0xF;
    u32 lsb = (insn >> 7) & 31, w = (insn >> 16) & 31;
    if (op1 == 0x18 && op2 == 0) {                      /* usad8 / usada8 (rd = bits 19-16) */
        u32 d = (insn >> 16) & 0xF, ra = (insn >> 12) & 0xF, rm = (insn >> 8) & 0xF;
        u32 a = c->r[rn], b = c->r[rm], s = 0;
        for (int i = 0; i < 4; i++) { s32 x = (s32)((a >> (8 * i)) & 0xFF) - (s32)((b >> (8 * i)) & 0xFF); s += (u32)(x < 0 ? -x : x); }
        c->r[d] = s + (ra == 15 ? 0 : c->r[ra]);
        return;
    }
    if ((op1 & 0x1E) == 0x1A && (op2 & 3) == 2) {       /* sbfx: widthm1 = bits 20-16 */
        u32 v = c->r[rn], width = w + 1;
        if (lsb + width > 32) undef7(c, insn);
        c->r[rd] = (u32)(((s32)(v << (32 - lsb - width))) >> (32 - width));
        return;
    }
    if ((op1 & 0x1E) == 0x1E && (op2 & 3) == 2) {       /* ubfx */
        u32 v = c->r[rn], width = w + 1;
        if (lsb + width > 32) undef7(c, insn);
        c->r[rd] = width == 32 ? v : (v >> lsb) & ((1u << width) - 1);
        return;
    }
    if ((op1 & 0x1E) == 0x1C && (op2 & 3) == 0) {       /* bfc (rn=15) / bfi: msb = bits 20-16 */
        u32 msb = w;
        if (msb < lsb) undef7(c, insn);
        u32 width = msb - lsb + 1, mask = (width == 32 ? 0xFFFFFFFFu : ((1u << width) - 1)) << lsb;
        u32 src = rn == 15 ? 0 : c->r[rn] << lsb;
        c->r[rd] = (c->r[rd] & ~mask) | (src & mask);
        return;
    }
    undef7(c, insn);
}

/* ---- movw / movt ---- */
void arm7_movwt(cpu_t *c, u32 insn)
{
    u32 rd = (insn >> 12) & 0xF, imm = ((insn >> 4) & 0xF000) | (insn & 0xFFF);
    if (insn & (1u << 22)) c->r[rd] = (c->r[rd] & 0xFFFF) | (imm << 16);
    else c->r[rd] = imm;
}

/* ---- mls / umaal ---- */
void arm7_mls_umaal(cpu_t *c, u32 insn)
{
    u32 rd = (insn >> 16) & 0xF, ra = (insn >> 12) & 0xF, rm = (insn >> 8) & 0xF, rn = insn & 0xF;
    if (((insn >> 20) & 0xF) == 0x6) { c->r[rd] = c->r[ra] - c->r[rn] * c->r[rm]; return; }   /* mls */
    /* umaal: rd = hi (19-16), ra = lo (15-12) */
    u64 r = (u64)c->r[rn] * c->r[rm] + c->r[rd] + c->r[ra];
    c->r[ra] = (u32)r; c->r[rd] = (u32)(r >> 32);
}

/* ---- exclusives: one global monitor address per cpu; strex succeeds if armed on the same address ---- */
static pthread_mutex_t excl_lock = PTHREAD_MUTEX_INITIALIZER;
void arm7_excl_at(cpu_t *c, u32 op, u32 a, u32 rd, u32 rt);
void arm7_excl(cpu_t *c, u32 insn)
{
    arm7_excl_at(c, (insn >> 20) & 0xF, c->r[(insn >> 16) & 0xF], (insn >> 12) & 0xF, insn & 0xF);
}

/* op: 8 strex 9 ldrex A strexd B ldrexd C strexb D ldrexb E strexh F ldrexh; rd = loaded / status reg, rt = stored */
void arm7_excl_at(cpu_t *c, u32 op, u32 a, u32 rd, u32 rt)
{
    if (op & 1) {
        c->excl_addr = a;
        switch (op) {
        case 0x9: c->r[rd] = ld32(a); break;
        case 0xB: c->r[rd] = ld32(a); c->r[rd + 1] = ld32(a + 4); break;
        case 0xD: c->r[rd] = ld8(a); break;
        default:  c->r[rd] = ld16(a); break;
        }
        return;
    }
    /* guest atomics are rare and short: serialize the check+store across threads */
    pthread_mutex_lock(&excl_lock);
    bool ok = c->excl_addr == a;
    if (ok) {
        switch (op) {
        case 0x8: st32(a, c->r[rt]); break;
        case 0xA: st32(a, c->r[rt]); st32(a + 4, c->r[rt + 1]); break;
        case 0xC: st8(a, (u8)c->r[rt]); break;
        default:  st16(a, (u16)c->r[rt]); break;
        }
    }
    pthread_mutex_unlock(&excl_lock);
    c->excl_addr = ~0u;
    c->r[rd] = ok ? 0 : 1;
}

/* ---- hints (msr imm with mask 0): nop, yield, wfe, wfi, sev, dbg ---- */
void arm7_hint(cpu_t *c, u32 insn) { (void)c; (void)insn; }

/* ---- unconditional space (cond = 1111) ---- */
void arm7_uncond(cpu_t *c, u32 insn)
{
    u32 op1 = (insn >> 20) & 0xFF;
    if ((insn & 0xFE000000u) == 0xF2000000u) { neon_dp(c, insn); return; }
    if ((insn & 0xFF100000u) == 0xF4000000u) { neon_ls(c, insn); return; }
    if ((insn & 0xFE000000u) == 0xFA000000u) {                     /* blx imm: to Thumb */
        s32 off = ((s32)(insn << 8) >> 6) | (s32)((insn >> 23) & 2);
        u32 pc = c->r[15] - 4;
        c->r[14] = pc + 4;
        cpu_branch(c, (pc + 8 + (u32)off) | 1);
        return;
    }
    if ((op1 & 0x77) == 0x55 || (op1 & 0x77) == 0x51 || (op1 & 0x77) == 0x45) return;   /* pld pldw pli (imm) */
    if ((op1 & 0x77) == 0x75 || (op1 & 0x77) == 0x71 || (op1 & 0x77) == 0x65) return;   /* pld pldw pli (reg) */
    if (op1 == 0x57) {
        u32 op2 = (insn >> 4) & 0xF;
        if (op2 == 1) { c->excl_addr = ~0u; return; }             /* clrex */
        if (op2 >= 4 && op2 <= 6) { __atomic_thread_fence(__ATOMIC_SEQ_CST); return; }   /* dsb dmb isb */
    }
    if ((insn & 0xFFF1FE20u) == 0xF1000000u) return;               /* cps: nothing in user mode */
    if ((insn & 0xFFFFFDFFu) == 0xF1010000u) {                     /* setend */
        if (insn & 0x200) emu_trap(c, "setend be");
        return;
    }
    undef7(c, insn);
}
