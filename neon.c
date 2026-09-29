/*
 * neon.c - ARMv7 Advanced SIMD (NEON), reference implementation.
 *
 * Works on the ARM encodings: data processing 1111 001U ..., element and
 * structure loads/stores 1111 0100 xxx0 .... Thumb-2 encodings are turned
 * into these by thumb.c. Operands are copied out of the register file before
 * computing, so any overlap of Vd/Vn/Vm behaves as on hardware.
 *
 * Float arithmetic uses the "standard FPSCR" NEON always runs with:
 * flush-to-zero, default NaN, round to nearest.
 */
#include "emu.h"

typedef __int128 s128;

#define VD(i) ((((i) >> 18) & 0x10) | (((i) >> 12) & 0xF))
#define VN(i) ((((i) >> 3) & 0x10) | (((i) >> 16) & 0xF))
#define VM(i) ((((i) >> 1) & 0x10) | ((i) & 0xF))
#define QC 0x08000000u

static void nundef(cpu_t *c, u32 insn)
{
    emu_trap(c, "NEON undefined %08x near pc %08x", insn, c->r[15]);
}

/* ---- lanes on a little-endian doubleword array ---- */
static inline u64 ge_(const u64 *v, int es, int i)
{
    if (es == 64) return v[i];
    int per = 64 / es;
    return (v[i / per] >> ((i % per) * es)) & ((1ULL << es) - 1);
}
static inline void se_(u64 *v, int es, int i, u64 x)
{
    if (es == 64) { v[i] = x; return; }
    int per = 64 / es, sh = (i % per) * es;
    u64 m = ((1ULL << es) - 1) << sh;
    v[i / per] = (v[i / per] & ~m) | ((x << sh) & m);
}
static inline s64 sx(u64 x, int es) { return es == 64 ? (s64)x : (s64)(x << (64 - es)) >> (64 - es); }
static inline u64 mask_es(int es) { return es == 64 ? ~0ULL : (1ULL << es) - 1; }
/* element as a 128-bit value, signed or unsigned interpretation */
static inline s128 ext(u64 x, int es, bool uns) { return uns ? (s128)(x & mask_es(es)) : (s128)sx(x, es); }

static inline void ldv(cpu_t *c, u32 reg, int regs, u64 *out) { for (int i = 0; i < regs; i++) out[i] = c->v.q[(reg + (u32)i) & 31]; }
static inline void stv(cpu_t *c, u32 reg, int regs, const u64 *in) { for (int i = 0; i < regs; i++) c->v.q[(reg + (u32)i) & 31] = in[i]; }

/* saturate a wide value to es bits; sets FPSCR.QC */
static u64 satv(cpu_t *c, s128 v, int es, bool uns)
{
    s128 hi, lo;
    if (uns) { hi = (s128)mask_es(es); lo = 0; }
    else { hi = es == 64 ? (s128)INT64_MAX : ((s128)1 << (es - 1)) - 1; lo = -hi - 1; }
    if (v > hi) { c->fpscr |= QC; return (u64)hi & mask_es(es); }
    if (v < lo) { c->fpscr |= QC; return (u64)lo & mask_es(es); }
    return (u64)v & mask_es(es);
}

/* ---- float helpers (standard FPSCR) ---- */
static inline f32 fin(u32 b)
{
    if (!(b & 0x7F800000u) && (b & 0x7FFFFFu)) b &= 0x80000000u;   /* input denormal -> zero */
    f32 f; memcpy(&f, &b, 4); return f;
}
static inline u32 fout(f32 f)
{
    u32 b; memcpy(&b, &f, 4);
    if ((b & 0x7F800000u) == 0x7F800000u && (b & 0x7FFFFFu)) return 0x7FC00000u;   /* default NaN */
    if (!(b & 0x7F800000u) && (b & 0x7FFFFFu)) b &= 0x80000000u;                   /* flush result */
    return b;
}
static inline bool fnan(u32 b) { return (b & 0x7F800000u) == 0x7F800000u && (b & 0x7FFFFFu); }

static u32 fmaxmin(u32 a, u32 b, bool is_max)
{
    if (fnan(a) || fnan(b)) return 0x7FC00000u;
    f32 x = fin(a), y = fin(b);
    if (x == 0 && y == 0) {                                  /* +0 vs -0 */
        u32 ab = fout(x), bb = fout(y);
        return is_max ? (ab & bb) : (ab | bb);
    }
    return fout(is_max ? (x > y ? x : y) : (x < y ? x : y));
}

/* ARMv7 reciprocal estimate helpers (recip_estimate / recip_sqrt_estimate reference code) */
static u32 recip_est(u32 q)          /* q = a*512 rounded down, 256..511 -> s = estimate*256, 256..511 */
{
    double r = 1.0 / (((double)q + 0.5) / 512.0);
    return (u32)(int)(256.0 * r + 0.5);
}
static u32 rsqrt_est(u32 q, bool quarter)   /* quarter: 0.25 <= a < 0.5, q = a*512; else q = a*256 */
{
    double r = quarter ? 1.0 / sqrt(((double)q + 0.5) / 512.0) : 1.0 / sqrt(((double)q + 0.5) / 256.0);
    return (u32)(int)(256.0 * r + 0.5);
}
static u32 frecpe(u32 op)
{
    if (fnan(op)) return 0x7FC00000u;
    u32 sign = op & 0x80000000u, exp = (op >> 23) & 0xFF;
    if ((op & 0x7FFFFFFFu) == 0x7F800000u) return sign;                       /* inf -> 0 */
    if (!exp) return sign | 0x7F800000u;                                       /* zero/denormal (FZ) -> inf */
    if (exp >= 253) return sign;                                               /* result underflows (FZ) */
    u32 est = recip_est(256 + ((op & 0x7FFFFFu) >> 15));
    return sign | ((253 - exp) << 23) | ((est & 0xFF) << 15);
}
static u32 frsqrte(u32 op)
{
    if (fnan(op)) return 0x7FC00000u;
    u32 sign = op & 0x80000000u, exp = (op >> 23) & 0xFF;
    if (!exp) return sign | 0x7F800000u;                                       /* zero/denormal -> inf */
    if (sign) return 0x7FC00000u;                                              /* negative -> NaN */
    if ((op & 0x7FFFFFFFu) == 0x7F800000u) return 0;                          /* +inf -> +0 */
    u32 est = rsqrt_est(128 + ((op & 0x7FFFFFu) >> 16), exp & 1);
    return (((380 - exp) / 2) << 23) | ((est & 0xFF) << 15);
}
static u32 urecpe(u32 op)
{
    if (!(op & 0x80000000u)) return 0xFFFFFFFFu;
    return recip_est(op >> 23) << 23;
}
static u32 ursqrte(u32 op)
{
    if (!(op & 0xC0000000u)) return 0xFFFFFFFFu;
    return ((op & 0x80000000u) ? rsqrt_est(op >> 24, false) : rsqrt_est(op >> 23, true)) << 23;
}
static u32 frecps(u32 a, u32 b)
{
    if (fnan(a) || fnan(b)) return 0x7FC00000u;
    f32 x = fin(a), y = fin(b);
    if ((isinf(x) && y == 0) || (x == 0 && isinf(y))) return fout(2.0f);
    return fout(2.0f - fin(fout(x * y)));
}
static u32 frsqrts(u32 a, u32 b)
{
    if (fnan(a) || fnan(b)) return 0x7FC00000u;
    f32 x = fin(a), y = fin(b);
    if ((isinf(x) && y == 0) || (x == 0 && isinf(y))) return fout(1.5f);
    return fout((3.0f - fin(fout(x * y))) / 2.0f);
}
static u32 f2i(u32 b, bool uns, int fbits)       /* round toward zero, saturate */
{
    if (fnan(b)) return 0;
    double v = trunc((double)fin(b) * ldexp(1.0, fbits));
    if (uns) { if (v <= 0) return 0; if (v >= 4294967295.0) return 0xFFFFFFFFu; return (u32)v; }
    if (v >= 2147483647.0) return 0x7FFFFFFFu;
    if (v <= -2147483648.0) return 0x80000000u;
    return (u32)(s32)v;
}
static u32 i2f(u32 v, bool uns, int fbits)
{
    double d = uns ? (double)v : (double)(s32)v;     /* exact; one rounding below */
    return fout((f32)(d / ldexp(1.0, fbits)));
}

/* IEEE half <-> single */
u16 f32_to_f16(u32 b, int rmode)                 /* rmode: FPSCR RMode (0 nearest, 1 +inf, 2 -inf, 3 zero) */
{
    u32 sign = (b >> 16) & 0x8000, exp = (b >> 23) & 0xFF, frac = b & 0x7FFFFF;
    if (exp == 0xFF) return frac ? (u16)(sign | 0x7E00 | (frac >> 13)) : (u16)(sign | 0x7C00);
    if (!exp && !frac) return (u16)sign;
    u32 m = exp ? (frac | 0x800000u) : frac;
    int e = exp ? (int)exp - 150 : -149;                /* value = m * 2^e */
    int ue = (31 - __builtin_clz(m)) + e;               /* unbiased exponent */
    int q = ue - 10;                                     /* weight of the half's lsb */
    if (q < -24) q = -24;
    int shift = q - e;                                   /* always >= 1 */
    u32 r, rem;
    bool half_exact, above;
    if (shift >= 32) { r = 0; rem = m; half_exact = false; above = false; }
    else {
        r = m >> shift; rem = m & ((1u << shift) - 1);
        u32 h = 1u << (shift - 1);
        half_exact = rem == h; above = rem > h;
    }
    bool inc;
    switch (rmode) {
    case 0: inc = above || (half_exact && (r & 1)); break;
    case 1: inc = !sign && rem; break;
    case 2: inc = sign && rem; break;
    default: inc = false; break;
    }
    if (inc) r++;
    if (r >= 0x800) { r >>= 1; q++; }
    if (r >= 0x400) {
        int he = q + 25;
        if (he >= 31) {
            bool to_inf = rmode == 0 || (rmode == 1 && !sign) || (rmode == 2 && sign);
            return (u16)(sign | (to_inf ? 0x7C00 : 0x7BFF));
        }
        return (u16)(sign | ((u32)he << 10) | (r & 0x3FF));
    }
    return (u16)(sign | r);
}
u32 f16_to_f32(u16 h)
{
    u32 sign = (u32)(h & 0x8000) << 16, exp = (h >> 10) & 0x1F, frac = h & 0x3FF;
    if (exp == 0x1F) return frac ? (sign | 0x7FC00000u | (frac << 13)) : sign | 0x7F800000u;
    if (!exp) {
        if (!frac) return sign;
        int e = -1;
        do { frac <<= 1; e++; } while (!(frac & 0x400));
        return sign | ((u32)(127 - 15 - e) << 23) | ((frac & 0x3FF) << 13);
    }
    return sign | ((exp + 112) << 23) | (frac << 13);
}

/* shift of a (sign/zero-extended) element by a signed amount, optional rounding */
static s128 vshift(s128 x, int sh, bool round)
{
    if (sh >= 0) return sh >= 100 ? 0 : x << sh;
    int r = -sh;
    if (r >= 100) return round ? 0 : (x < 0 ? -1 : 0);
    s128 rc = round ? ((s128)1 << (r - 1)) : 0;
    return (x + rc) >> r;
}

static u64 pmul8(u64 a, u64 b)       /* carryless 8x8 -> 16 */
{
    u64 r = 0;
    for (int i = 0; i < 8; i++) if ((b >> i) & 1) r ^= (a & 0xFF) << i;
    return r;
}

/* ================================================================== */
/* three registers of the same length                                  */
/* ================================================================== */
static void three_same(cpu_t *c, u32 insn)
{
    u32 U = (insn >> 24) & 1, A = (insn >> 8) & 0xF, B = (insn >> 4) & 1, C = (insn >> 20) & 3, Q = (insn >> 6) & 1;
    u32 d = VD(insn), n = VN(insn), m = VM(insn);
    if (Q && ((d | n | m) & 1)) nundef(c, insn);
    int regs = Q ? 2 : 1, es = 8 << C, ne = regs * 64 / es;
    u64 vn[2], vm[2], vd[2], r[2] = { 0, 0 };
    ldv(c, n, regs, vn); ldv(c, m, regs, vm); ldv(c, d, regs, vd);
    bool uns = U;

    if (A == 1 && B) {                                              /* bitwise */
        for (int i = 0; i < regs; i++) {
            u64 x = vn[i], y = vm[i], z = vd[i];
            if (!U) r[i] = C == 0 ? x & y : C == 1 ? x & ~y : C == 2 ? x | y : x | ~y;
            else r[i] = C == 0 ? x ^ y : C == 1 ? (z & x) | (~z & y) : C == 2 ? (x & y) | (z & ~y) : (z & y) | (x & ~y);
        }
        stv(c, d, regs, r);
        return;
    }
    if (A >= 12) {                                                  /* float (A=12: vfma/vfms) */
        if (C & 1) nundef(c, insn);
        bool op = C & 2;
        ne = regs * 2;
        if (A == 13 && !B && U && !op) {                            /* vpadd.f32 */
            if (Q) nundef(c, insn);
            r[0] = fout(fin((u32)vn[0]) + fin((u32)(vn[0] >> 32))) | (u64)fout(fin((u32)vm[0]) + fin((u32)(vm[0] >> 32))) << 32;
            stv(c, d, 1, r);
            return;
        }
        if (A == 15 && !B && U) {                                   /* vpmax / vpmin .f32 */
            if (Q) nundef(c, insn);
            r[0] = fmaxmin((u32)vn[0], (u32)(vn[0] >> 32), !op) | (u64)fmaxmin((u32)vm[0], (u32)(vm[0] >> 32), !op) << 32;
            stv(c, d, 1, r);
            return;
        }
        for (int i = 0; i < ne; i++) {
            u32 a = (u32)ge_(vn, 32, i), b = (u32)ge_(vm, 32, i), z = (u32)ge_(vd, 32, i), o = 0;
            f32 x = fin(a), y = fin(b), w = fin(z);
            switch (A) {
            case 12:
                if (!B || U) nundef(c, insn);
                o = fout(fmaf(op ? -x : x, y, w));
                break;
            case 13:
                if (!B) {
                    if (!U) o = fout(op ? x - y : x + y);
                    else if (op) o = fout(fabsf(x - y)) & 0x7FFFFFFFu;
                    else nundef(c, insn);
                } else {
                    if (!U) { f32 p = fin(fout(x * y)); o = fout(op ? w - p : w + p); }
                    else if (!op) o = fout(x * y);
                    else nundef(c, insn);
                }
                break;
            case 14: {
                bool res;
                if (!B) {
                    if (!U && op) nundef(c, insn);
                    res = !(fnan(a) || fnan(b)) && (!U ? x == y : op ? x > y : x >= y);
                } else {
                    if (!U) nundef(c, insn);
                    res = !(fnan(a) || fnan(b)) && (op ? fabsf(x) > fabsf(y) : fabsf(x) >= fabsf(y));
                }
                o = res ? 0xFFFFFFFFu : 0;
                break; }
            default:
                if (!B) o = fmaxmin(a, b, !op);
                else if (!U) o = op ? frsqrts(a, b) : frecps(a, b);
                else nundef(c, insn);
                break;
            }
            se_(r, 32, i, o);
        }
        stv(c, d, regs, r);
        return;
    }
    if (A == 10 || (A == 11 && B)) {                                /* pairwise integer ops: D only */
        if (Q || (A == 11 && U) || es == 64) nundef(c, insn);
        int h = ne / 2;
        for (int i = 0; i < ne; i++) {
            const u64 *src = i < h ? vn : vm;
            int k = (i % h) * 2;
            s128 x = ext(ge_(src, es, k), es, uns), y = ext(ge_(src, es, k + 1), es, uns), o;
            if (A == 11) o = x + y;
            else if (!B) o = x > y ? x : y;
            else o = x < y ? x : y;
            se_(r, es, i, (u64)o);
        }
        stv(c, d, 1, r);
        return;
    }
    for (int i = 0; i < ne; i++) {
        u64 a = ge_(vn, es, i), b = ge_(vm, es, i), z = ge_(vd, es, i), o = 0;
        s128 x = ext(a, es, uns), y = ext(b, es, uns);
        switch (A) {
        case 0: o = B ? satv(c, x + y, es, uns) : (u64)((x + y) >> 1); break;          /* vhadd / vqadd */
        case 1: o = (u64)((x + y + 1) >> 1); break;                                     /* vrhadd */
        case 2: o = B ? satv(c, x - y, es, uns) : (u64)((x - y) >> 1); break;          /* vhsub / vqsub */
        case 3: o = (B ? x >= y : x > y) ? ~0ULL : 0; break;                            /* vcgt / vcge */
        case 4: case 5: {                                                               /* vshl vqshl vrshl vqrshl */
            s128 v = y;                                                                 /* data: Vm */
            int sh = (int)(s8)(u8)a;                                                    /* shift: low byte of Vn */
            bool rnd = A == 5;
            if (B) {
                if (sh >= es) o = v == 0 ? 0 : satv(c, v < 0 ? -((s128)1 << 100) : ((s128)1 << 100), es, uns);
                else o = satv(c, vshift(v, sh, rnd), es, uns);
            } else o = (u64)vshift(v, sh >= es ? 100 : sh, rnd);
            break; }
        case 6: o = (u64)(B ? (x < y ? x : y) : (x > y ? x : y)); break;                /* vmax / vmin */
        case 7: { s128 df = x - y; if (df < 0) df = -df; o = B ? z + (u64)df : (u64)df; break; }   /* vabd / vaba */
        case 8:
            if (!B) o = U ? a - b : a + b;                                              /* vadd / vsub */
            else o = (U ? a == b : (a & b) != 0) ? ~0ULL : 0;                           /* vceq / vtst */
            break;
        case 9:
            if (!B) o = U ? z - a * b : z + a * b;                                      /* vmla / vmls */
            else if (!U) o = a * b;                                                     /* vmul */
            else { if (es != 8) nundef(c, insn); o = pmul8(a, b); }                     /* vmul.p8 */
            break;
        case 11: {                                                                      /* vqdmulh / vqrdmulh */
            if (es != 16 && es != 32) nundef(c, insn);
            s128 p = 2 * (s128)sx(a, es) * (s128)sx(b, es);
            if (U) p += (s128)1 << (es - 1);
            o = satv(c, p >> es, es, false);
            break; }
        default: nundef(c, insn);
        }
        se_(r, es, i, o);
    }
    stv(c, d, regs, r);
}

/* ================================================================== */
/* one register and a modified immediate                               */
/* ================================================================== */
u64 neon_expand_imm(u32 op, u32 cmode, u32 imm8, bool *ok)
{
    u64 imm = 0, i8 = imm8;
    *ok = true;
    switch (cmode >> 1) {
    case 0: imm = i8 | i8 << 32; break;
    case 1: imm = (i8 << 8) | (i8 << 40); break;
    case 2: imm = (i8 << 16) | (i8 << 48); break;
    case 3: imm = (i8 << 24) | (i8 << 56); break;
    case 4: imm = i8 * 0x0001000100010001ULL; break;
    case 5: imm = (i8 << 8) * 0x0001000100010001ULL; break;
    case 6: imm = (cmode & 1) ? ((i8 << 16) | 0xFFFF) * 0x100000001ULL : ((i8 << 8) | 0xFF) * 0x100000001ULL; break;
    default:
        if (!(cmode & 1)) {
            if (!op) imm = i8 * 0x0101010101010101ULL;
            else for (int i = 0; i < 8; i++) if ((imm8 >> i) & 1) imm |= 0xFFULL << (8 * i);
        } else {
            if (op) *ok = false;
            u32 f = ((imm8 & 0x80) << 24) | ((imm8 & 0x40) ? 0x3E000000u : 0x40000000u) | ((imm8 & 0x3F) << 19);
            imm = (u64)f | ((u64)f << 32);
        }
    }
    return imm;
}

static void one_reg_imm(cpu_t *c, u32 insn)
{
    u32 op = (insn >> 5) & 1, cmode = (insn >> 8) & 0xF, Q = (insn >> 6) & 1, d = VD(insn);
    u32 imm8 = ((insn >> 17) & 0x80) | ((insn >> 12) & 0x70) | (insn & 0xF);
    if (Q && (d & 1)) nundef(c, insn);
    bool ok;
    u64 imm = neon_expand_imm(op, cmode, imm8, &ok);
    if (!ok) nundef(c, insn);
    int regs = Q ? 2 : 1;
    u64 v[2]; ldv(c, d, regs, v);
    bool orr_bic = (cmode & 1) && cmode < 12;
    for (int i = 0; i < regs; i++) {
        if (!op) v[i] = orr_bic ? v[i] | imm : imm;                 /* vorr / vmov */
        else if (cmode == 14) v[i] = imm;                           /* vmov.i64 */
        else v[i] = orr_bic ? v[i] & ~imm : ~imm;                   /* vbic / vmvn */
    }
    stv(c, d, regs, v);
}

/* ================================================================== */
/* two registers and a shift amount                                    */
/* ================================================================== */
static void two_shift(cpu_t *c, u32 insn)
{
    u32 U = (insn >> 24) & 1, A = (insn >> 8) & 0xF, L = (insn >> 7) & 1, Q = (insn >> 6) & 1;
    u32 imm6 = (insn >> 16) & 0x3F, d = VD(insn), m = VM(insn);
    int es = L ? 64 : (imm6 & 0x20) ? 32 : (imm6 & 0x10) ? 16 : 8;
    int v = (int)((L << 6) | imm6), right = 2 * es - v, left = v - es;
    u64 vm[2], vd[2], r[2] = { 0, 0 };

    if (A >= 8 && A <= 10) {
        if (L) nundef(c, insn);
        if (A == 10) {                                              /* vshll / vmovl: D -> Q */
            if (Q || (d & 1)) nundef(c, insn);
            ldv(c, m, 1, vm);
            for (int i = 0; i < 64 / es; i++) se_(r, 2 * es, i, (u64)(ext(ge_(vm, es, i), es, U) << left));
            stv(c, d, 2, r);
            return;
        }
        if (m & 1) nundef(c, insn);                                 /* narrowing: Q (2*es) -> D (es) */
        ldv(c, m, 2, vm);
        bool rnd = Q;                                               /* bit6 = rounding here */
        for (int i = 0; i < 64 / es; i++) {
            u64 e = ge_(vm, 2 * es, i), o;
            if (A == 8 && !U) o = (u64)vshift(ext(e, 2 * es, true), -right, rnd);                 /* vshrn / vrshrn */
            else if (A == 8) o = satv(c, vshift(ext(e, 2 * es, false), -right, rnd), es, true);   /* vqshrun / vqrshrun */
            else o = satv(c, vshift(ext(e, 2 * es, U), -right, rnd), es, U);                     /* vqshrn / vqrshrn */
            se_(r, es, i, o);
        }
        stv(c, d, 1, r);
        return;
    }
    if (Q && ((d | m) & 1)) nundef(c, insn);
    int regs = Q ? 2 : 1, ne = regs * 64 / es;
    ldv(c, m, regs, vm); ldv(c, d, regs, vd);
    if (A >= 14) {                                                  /* vcvt fixed <-> float */
        if (!(imm6 & 0x20) || L) nundef(c, insn);
        int fbits = 64 - (int)imm6;
        for (int i = 0; i < regs * 2; i++) {
            u32 x = (u32)ge_(vm, 32, i);
            se_(r, 32, i, A == 14 ? i2f(x, U, fbits) : f2i(x, U, fbits));
        }
        stv(c, d, regs, r);
        return;
    }
    if (A >= 11) nundef(c, insn);
    for (int i = 0; i < ne; i++) {
        u64 e = ge_(vm, es, i), z = ge_(vd, es, i), o;
        switch (A) {
        case 0: case 1: case 2: case 3: {                          /* vshr vsra vrshr vrsra */
            u64 s = (u64)vshift(ext(e, es, U), -right, A & 2);
            o = (A & 1) ? z + s : s;
            break; }
        case 4: {                                                   /* vsri */
            if (!U) nundef(c, insn);
            u64 mk = right >= es ? 0 : mask_es(es) >> right;
            o = (z & ~mk) | ((u64)vshift(ext(e, es, true), -right, false) & mk);
            break; }
        case 5: {                                                   /* vshl imm / vsli */
            u64 s = (u64)((s128)(e & mask_es(es)) << left);
            if (!U) o = s;
            else { u64 mk = (mask_es(es) << left) & mask_es(es); o = (z & ~mk) | (s & mk); }
            break; }
        default:                                                    /* 6 vqshlu, 7 vqshl */
            if (A == 6 && !U) nundef(c, insn);
            if (A == 6) o = satv(c, ext(e, es, false) << left, es, true);
            else o = satv(c, ext(e, es, U) << left, es, U);
            break;
        }
        se_(r, es, i, o);
    }
    stv(c, d, regs, r);
}

/* ================================================================== */
/* three registers of different lengths                                */
/* ================================================================== */
static void three_diff(cpu_t *c, u32 insn)
{
    u32 U = (insn >> 24) & 1, A = (insn >> 8) & 0xF, size = (insn >> 20) & 3;
    u32 d = VD(insn), n = VN(insn), m = VM(insn);
    int es = 8 << size;
    u64 vn[2], vm[2], vd[2], r[2] = { 0, 0 };
    if (A == 4 || A == 6) {                                         /* vaddhn vraddhn vsubhn vrsubhn: Q,Q -> D */
        if ((n | m) & 1) nundef(c, insn);
        ldv(c, n, 2, vn); ldv(c, m, 2, vm);
        for (int i = 0; i < 64 / es; i++) {
            u64 x = ge_(vn, 2 * es, i), y = ge_(vm, 2 * es, i);
            u64 s = A == 4 ? x + y : x - y;
            if (U) s += 1ULL << (es - 1);
            se_(r, es, i, (s & mask_es(2 * es)) >> es);
        }
        stv(c, d, 1, r);
        return;
    }
    if (A == 15) nundef(c, insn);
    if (d & 1) nundef(c, insn);
    bool wide = A == 1 || A == 3;
    if (wide && (n & 1)) nundef(c, insn);
    ldv(c, n, wide ? 2 : 1, vn); ldv(c, m, 1, vm); ldv(c, d, 2, vd);
    for (int i = 0; i < 64 / es; i++) {
        s128 x = wide ? ext(ge_(vn, 2 * es, i), 2 * es, U) : ext(ge_(vn, es, i), es, U);
        s128 y = ext(ge_(vm, es, i), es, U);
        u64 z = ge_(vd, 2 * es, i), o;
        switch (A) {
        case 0: case 1: o = (u64)(x + y); break;                                    /* vaddl vaddw */
        case 2: case 3: o = (u64)(x - y); break;                                    /* vsubl vsubw */
        case 5: case 7: { s128 df = x - y; if (df < 0) df = -df; o = A == 5 ? z + (u64)df : (u64)df; break; }   /* vabal vabdl */
        case 8: o = z + (u64)(x * y); break;                                        /* vmlal */
        case 10: o = z - (u64)(x * y); break;                                       /* vmlsl */
        case 12: o = (u64)(x * y); break;                                           /* vmull */
        case 9: case 11: case 13: {                                                 /* vqdmlal vqdmlsl vqdmull */
            if (U || es == 8) nundef(c, insn);
            u64 p = satv(c, 2 * x * y, 2 * es, false);
            if (A == 13) o = p;
            else o = satv(c, ext(z, 2 * es, false) + (A == 9 ? 1 : -1) * ext(p, 2 * es, false), 2 * es, false);
            break; }
        case 14:                                                                    /* vmull.p8 */
            if (U || es != 8) nundef(c, insn);
            o = pmul8(ge_(vn, 8, i), ge_(vm, 8, i));
            break;
        default: nundef(c, insn); o = 0;
        }
        se_(r, 2 * es, i, o);
    }
    stv(c, d, 2, r);
}

/* ================================================================== */
/* two registers and a scalar                                          */
/* ================================================================== */
static void two_scalar(cpu_t *c, u32 insn)
{
    u32 U = (insn >> 24) & 1, A = (insn >> 8) & 0xF, size = (insn >> 20) & 3;
    u32 d = VD(insn), n = VN(insn), vm4 = insn & 0xF, M = (insn >> 5) & 1;
    if (size == 0 || size == 3) nundef(c, insn);
    int es = 8 << size;
    u32 mreg, idx;
    if (es == 16) { mreg = vm4 & 7; idx = (M << 1) | (vm4 >> 3); }
    else { mreg = vm4; idx = M; }
    u64 sv = ge_(&c->v.q[mreg], es, (int)idx);
    u64 vn[2], vd[2], r[2] = { 0, 0 };
    bool lng = A == 2 || A == 3 || A == 6 || A == 7 || A == 10 || A == 11;
    if (lng) {
        if (d & 1) nundef(c, insn);
        ldv(c, n, 1, vn); ldv(c, d, 2, vd);
        for (int i = 0; i < 64 / es; i++) {
            s128 x = ext(ge_(vn, es, i), es, U), y = ext(sv, es, U);
            u64 z = ge_(vd, 2 * es, i), o;
            switch (A) {
            case 2: o = z + (u64)(x * y); break;                   /* vmlal */
            case 6: o = z - (u64)(x * y); break;                   /* vmlsl */
            case 10: o = (u64)(x * y); break;                      /* vmull */
            default: {                                              /* 3 vqdmlal 7 vqdmlsl 11 vqdmull */
                if (U) nundef(c, insn);
                u64 p = satv(c, 2 * x * y, 2 * es, false);
                o = A == 11 ? p : satv(c, ext(z, 2 * es, false) + (A == 3 ? 1 : -1) * ext(p, 2 * es, false), 2 * es, false);
                break; }
            }
            se_(r, 2 * es, i, o);
        }
        stv(c, d, 2, r);
        return;
    }
    u32 Q = U;
    if (Q && ((d | n) & 1)) nundef(c, insn);
    int regs = Q ? 2 : 1, ne = regs * 64 / es;
    ldv(c, n, regs, vn); ldv(c, d, regs, vd);
    bool fl = A == 1 || A == 5 || A == 9;
    if (fl && es != 32) nundef(c, insn);
    for (int i = 0; i < ne; i++) {
        u64 a = ge_(vn, es, i), z = ge_(vd, es, i), o;
        switch (A) {
        case 0: o = z + a * sv; break;                              /* vmla */
        case 4: o = z - a * sv; break;                              /* vmls */
        case 8: o = a * sv; break;                                  /* vmul */
        case 1: case 5: { f32 p = fin(fout(fin((u32)a) * fin((u32)sv))); o = fout(A == 1 ? fin((u32)z) + p : fin((u32)z) - p); break; }
        case 9: o = fout(fin((u32)a) * fin((u32)sv)); break;
        case 12: case 13: {                                         /* vqdmulh / vqrdmulh */
            s128 p = 2 * (s128)sx(a, es) * (s128)sx(sv, es);
            if (A == 13) p += (s128)1 << (es - 1);
            o = satv(c, p >> es, es, false);
            break; }
        default: nundef(c, insn); o = 0;
        }
        se_(r, es, i, o);
    }
    stv(c, d, regs, r);
}

/* ================================================================== */
/* two registers, miscellaneous                                        */
/* ================================================================== */
static u32 clz_es(u64 x, int es) { u32 n = 0; for (int b = es - 1; b >= 0 && !((x >> b) & 1); b--) n++; return n; }

static void two_misc(cpu_t *c, u32 insn)
{
    u32 A = (insn >> 16) & 3, B = (insn >> 6) & 0x1F, size = (insn >> 18) & 3, Q = (insn >> 6) & 1;
    u32 d = VD(insn), m = VM(insn);
    int es = 8 << size;
    u64 vm[2], vd[2], r[2] = { 0, 0 };
    int regs = Q ? 2 : 1;

    if (A == 2 && B >= 8) {                                         /* narrow / widen group (bit6 is not Q) */
        if (B == 12) {                                              /* vshll #esize: D -> Q */
            if ((d & 1) || es == 64) nundef(c, insn);
            ldv(c, m, 1, vm);
            for (int i = 0; i < 64 / es; i++) se_(r, 2 * es, i, ge_(vm, es, i) << es);
            stv(c, d, 2, r);
            return;
        }
        if (B == 24 || B == 28) {                                   /* vcvt f16 <-> f32 */
            if (size != 1) nundef(c, insn);
            if (B == 24) {                                          /* f32 (Qm) -> f16 (Dd) */
                if (m & 1) nundef(c, insn);
                ldv(c, m, 2, vm);
                for (int i = 0; i < 4; i++) {
                    u32 x = (u32)ge_(vm, 32, i);
                    se_(r, 16, i, fnan(x) ? 0x7E00 : f32_to_f16(fout(fin(x)), 0));
                }
                stv(c, d, 1, r);
            } else {                                                /* f16 (Dm) -> f32 (Qd) */
                if (d & 1) nundef(c, insn);
                ldv(c, m, 1, vm);
                for (int i = 0; i < 4; i++) {
                    u16 h = (u16)ge_(vm, 16, i);
                    u32 f = f16_to_f32(h);
                    se_(r, 32, i, fnan(f) ? 0x7FC00000u : fout(fin(f)));
                }
                stv(c, d, 2, r);
            }
            return;
        }
        if (B > 11 || es == 64) nundef(c, insn);
        if (m & 1) nundef(c, insn);
        ldv(c, m, 2, vm);                                           /* vmovn vqmovun vqmovn: Q (2*es) -> D (es) */
        for (int i = 0; i < 64 / es; i++) {
            u64 e = ge_(vm, 2 * es, i), o;
            if (B == 8) o = e;
            else if (B == 9) o = satv(c, ext(e, 2 * es, false), es, true);
            else o = satv(c, ext(e, 2 * es, B == 11), es, B == 11);
            se_(r, es, i, o);
        }
        stv(c, d, 1, r);
        return;
    }
    if (Q && ((d | m) & 1)) nundef(c, insn);
    ldv(c, m, regs, vm); ldv(c, d, regs, vd);
    int ne = regs * 64 / es;
    u32 op = B >> 1;                                                /* bits 10-7 */

    if (A == 2) {                                                   /* vswp vtrn vuzp vzip */
        if (op > 3) nundef(c, insn);
        u64 a[2] = { vd[0], Q ? vd[1] : 0 }, b[2] = { vm[0], Q ? vm[1] : 0 };
        if (op == 0) { stv(c, d, regs, b); stv(c, m, regs, a); return; }
        if (es == 64) nundef(c, insn);
        u64 x[2] = { 0, 0 }, y[2] = { 0, 0 };
        if (op == 1) {                                              /* vtrn */
            for (int i = 0; i < ne; i += 2) {
                se_(x, es, i, ge_(a, es, i)); se_(x, es, i + 1, ge_(b, es, i));
                se_(y, es, i, ge_(a, es, i + 1)); se_(y, es, i + 1, ge_(b, es, i + 1));
            }
        } else if (op == 2) {                                       /* vuzp: d = even elems of (d:m), m = odd */
            if (!Q && es == 32) nundef(c, insn);
            for (int i = 0; i < ne; i++) {
                int k = 2 * i;
                se_(x, es, i, k < ne ? ge_(a, es, k) : ge_(b, es, k - ne));
                se_(y, es, i, k + 1 < ne ? ge_(a, es, k + 1) : ge_(b, es, k + 1 - ne));
            }
        } else {                                                    /* vzip */
            if (!Q && es == 32) nundef(c, insn);
            for (int i = 0; i < ne; i++) {
                int k = i / 2;
                se_(x, es, i, (i & 1) ? ge_(b, es, k) : ge_(a, es, k));
                se_(y, es, i, (i & 1) ? ge_(b, es, k + ne / 2) : ge_(a, es, k + ne / 2));
            }
        }
        stv(c, d, regs, x); stv(c, m, regs, y);
        return;
    }
    if (A == 3) {
        bool fl = (insn >> 8) & 1;
        if ((B & 0x18) == 0x10) {                                   /* 10x0x vrecpe / 10x1x vrsqrte */
            if (size != 2) nundef(c, insn);
            bool rsq = (insn >> 7) & 1;
            for (int i = 0; i < ne; i++) {
                u32 x = (u32)ge_(vm, 32, i);
                se_(r, 32, i, fl ? (rsq ? frsqrte(x) : frecpe(x)) : (rsq ? ursqrte(x) : urecpe(x)));
            }
            stv(c, d, regs, r);
            return;
        }
        if ((B & 0x18) == 0x18) {                                   /* vcvt float <-> int */
            if (size != 2) nundef(c, insn);
            u32 cop = (insn >> 7) & 3;
            for (int i = 0; i < ne; i++) {
                u32 x = (u32)ge_(vm, 32, i);
                se_(r, 32, i, cop < 2 ? i2f(x, cop & 1, 0) : f2i(x, cop & 1, 0));
            }
            stv(c, d, regs, r);
            return;
        }
        nundef(c, insn);
    }
    if (A == 1) {                                                   /* compare with zero, vabs, vneg */
        bool fl = (insn >> 10) & 1;
        u32 k = op & 7;
        if (k == 5 || es == 64 || (fl && es != 32)) nundef(c, insn);
        for (int i = 0; i < ne; i++) {
            u64 e = ge_(vm, es, i), o;
            if (fl) {
                u32 b = (u32)e; f32 x = fin(b); bool nan = fnan(b), res = false;
                if (k == 6) { se_(r, 32, i, b & 0x7FFFFFFFu); continue; }   /* vabs.f32 / vneg.f32: sign bit only */
                if (k == 7) { se_(r, 32, i, b ^ 0x80000000u); continue; }
                switch (k) {
                case 0: res = !nan && x > 0; break;
                case 1: res = !nan && x >= 0; break;
                case 2: res = !nan && x == 0; break;
                case 3: res = !nan && x <= 0; break;
                default: res = !nan && x < 0; break;
                }
                o = res ? ~0ULL : 0;
            } else {
                s64 x = sx(e, es);
                switch (k) {
                case 0: o = x > 0 ? ~0ULL : 0; break;
                case 1: o = x >= 0 ? ~0ULL : 0; break;
                case 2: o = x == 0 ? ~0ULL : 0; break;
                case 3: o = x <= 0 ? ~0ULL : 0; break;
                case 4: o = x < 0 ? ~0ULL : 0; break;
                case 6: o = (u64)(x < 0 ? -x : x); break;
                default: o = (u64)-x; break;
                }
            }
            se_(r, es, i, o);
        }
        stv(c, d, regs, r);
        return;
    }
    switch (op) {
    case 0: case 1: case 2: {                                       /* vrev64 / vrev32 / vrev16 */
        int group = 64 >> op;
        if (es >= group) nundef(c, insn);
        int per = group / es;
        for (int i = 0; i < ne; i++) se_(r, es, i, ge_(vm, es, (i / per) * per + (per - 1 - i % per)));
        break; }
    case 4: case 5: case 12: case 13: {                             /* vpaddl / vpadal */
        if (es == 64) nundef(c, insn);
        bool uns = op & 1;
        for (int i = 0; i < ne / 2; i++) {
            s128 s = ext(ge_(vm, es, 2 * i), es, uns) + ext(ge_(vm, es, 2 * i + 1), es, uns);
            if (op >= 12) s += (s128)ge_(vd, 2 * es, i);
            se_(r, 2 * es, i, (u64)s);
        }
        break; }
    case 8: case 9:                                                 /* vcls / vclz */
        if (es == 64) nundef(c, insn);
        for (int i = 0; i < ne; i++) {
            u64 e = ge_(vm, es, i);
            if (op == 8) { if (sx(e, es) < 0) e = ~e & mask_es(es); se_(r, es, i, clz_es(e, es) - 1); }
            else se_(r, es, i, clz_es(e, es));
        }
        break;
    case 10: if (es != 8) nundef(c, insn); for (int i = 0; i < ne; i++) se_(r, 8, i, (u64)__builtin_popcountll(ge_(vm, 8, i))); break;   /* vcnt */
    case 11: if (es != 8) nundef(c, insn); for (int i = 0; i < regs; i++) r[i] = ~vm[i]; break;                               /* vmvn */
    case 14: case 15:                                                                                                         /* vqabs / vqneg */
        if (es == 64) nundef(c, insn);
        for (int i = 0; i < ne; i++) {
            s128 x = sx(ge_(vm, es, i), es);
            se_(r, es, i, satv(c, op == 14 ? (x < 0 ? -x : x) : -x, es, false));
        }
        break;
    default: nundef(c, insn);
    }
    stv(c, d, regs, r);
}

/* ================================================================== */
/* data-processing entry                                               */
/* ================================================================== */

void neon_dp(cpu_t *c, u32 insn)
{
    u32 U = (insn >> 24) & 1, A = (insn >> 19) & 0x1F, B = (insn >> 8) & 0xF, C = (insn >> 4) & 0xF;
    if (!(A & 0x10)) { three_same(c, insn); return; }
    if ((A & 0x17) == 0x10 && (C & 9) == 1) { one_reg_imm(c, insn); return; }
    if (C & 1) { two_shift(c, insn); return; }                     /* A = 1xxxx, bit4 = 1 */
    if ((A & 0x16) != 0x16 && !(C & 5)) { three_diff(c, insn); return; }   /* A = 1x0xx / 1x10x, C = x0x0 */
    if ((A & 0x16) != 0x16 && (C & 5) == 4) { two_scalar(c, insn); return; }
    if ((A & 0x16) == 0x16) {
        u32 d = VD(insn), n = VN(insn), m = VM(insn), Q = (insn >> 6) & 1;
        if (!U) {                                                   /* vext */
            u32 imm4 = (insn >> 8) & 0xF;
            if (Q && ((d | n | m) & 1)) nundef(c, insn);
            if (!Q && imm4 >= 8) nundef(c, insn);
            int regs = Q ? 2 : 1, nb = regs * 8;
            u8 src[32], out[16];
            u64 a[2], b[2];
            ldv(c, n, regs, a); ldv(c, m, regs, b);
            memcpy(src, a, (size_t)nb); memcpy(src + nb, b, (size_t)nb);
            memcpy(out, src + imm4, (size_t)nb);
            memcpy(a, out, (size_t)nb);
            stv(c, d, regs, a);
            return;
        }
        if (!(B & 8)) { two_misc(c, insn); return; }
        if ((B & 0xC) == 8) {                                       /* vtbl / vtbx */
            u32 len = ((insn >> 8) & 3) + 1, tbx = (insn >> 6) & 1;
            u8 tab[32], idx[8], out[8];
            for (u32 i = 0; i < len; i++) memcpy(tab + 8 * i, &c->v.q[(n + i) & 31], 8);
            memcpy(idx, &c->v.q[m], 8); memcpy(out, &c->v.q[d], 8);
            for (int i = 0; i < 8; i++) {
                if (idx[i] < 8 * len) out[i] = tab[idx[i]];
                else if (!tbx) out[i] = 0;
            }
            memcpy(&c->v.q[d], out, 8);
            return;
        }
        if (B == 0xC && !(C & 8)) {                                 /* vdup scalar */
            u32 imm4 = (insn >> 16) & 0xF;
            int es, idx;
            if (imm4 & 1) { es = 8; idx = (int)(imm4 >> 1); }
            else if (imm4 & 2) { es = 16; idx = (int)(imm4 >> 2); }
            else if (imm4 & 4) { es = 32; idx = (int)(imm4 >> 3); }
            else { nundef(c, insn); return; }
            if (Q && (d & 1)) nundef(c, insn);
            u64 v = ge_(&c->v.q[m], es, idx), r[2] = { 0, 0 };
            int regs = Q ? 2 : 1;
            for (int i = 0; i < regs * 64 / es; i++) se_(r, es, i, v);
            stv(c, d, regs, r);
            return;
        }
    }
    nundef(c, insn);
}

/* ================================================================== */
/* element and structure load/store                                    */
/* ================================================================== */
static inline u64 ldmem(u32 a, int es)
{
    switch (es) { case 8: return ld8(a); case 16: return ld16(a); case 32: return ld32(a);
                  default: return ld32(a) | ((u64)ld32(a + 4) << 32); }
}
static inline void stmem(u32 a, int es, u64 v)
{
    switch (es) { case 8: st8(a, (u8)v); break; case 16: st16(a, (u16)v); break; case 32: st32(a, (u32)v); break;
                  default: st32(a, (u32)v); st32(a + 4, (u32)(v >> 32)); break; }
}

void neon_ls(cpu_t *c, u32 insn)
{
    u32 A = (insn >> 23) & 1, L = (insn >> 21) & 1, rn = (insn >> 16) & 0xF, rm = insn & 0xF;
    u32 d = VD(insn), type = (insn >> 8) & 0xF;
    u32 addr = c->r[rn], bytes = 0;
    if (!A) {                                                       /* multiple structures */
        int es = 8 << ((insn >> 6) & 3), ne = 64 / es, regs, nst, inc;
        switch (type) {
        case 7: nst = 1; regs = 1; inc = 1; break;
        case 10: nst = 1; regs = 2; inc = 1; break;
        case 6: nst = 1; regs = 3; inc = 1; break;
        case 2: nst = 1; regs = 4; inc = 1; break;
        case 8: nst = 2; regs = 1; inc = 1; break;
        case 9: nst = 2; regs = 1; inc = 2; break;
        case 3: nst = 2; regs = 2; inc = 2; break;
        case 4: nst = 3; regs = 1; inc = 1; break;
        case 5: nst = 3; regs = 1; inc = 2; break;
        case 0: nst = 4; regs = 1; inc = 1; break;
        case 1: nst = 4; regs = 1; inc = 2; break;
        default: nundef(c, insn); return;
        }
        if (es == 64 && nst > 1) nundef(c, insn);
        u32 a = addr;
        for (int r = 0; r < regs; r++)
            for (int e = 0; e < ne; e++)
                for (int s = 0; s < nst; s++) {
                    u32 reg = (d + (u32)r + (u32)(s * inc)) & 31;
                    if (nst == 1) reg = (d + (u32)r) & 31;
                    if (L) se_(&c->v.q[reg], es, e, ldmem(a, es)); else stmem(a, es, ge_(&c->v.q[reg], es, e));
                    a += (u32)es / 8;
                }
        bytes = a - addr;
    } else {
        u32 sz = (insn >> 10) & 3, nst = ((insn >> 8) & 3) + 1;
        if (sz == 3) {                                              /* load to all lanes */
            if (!L) nundef(c, insn);
            u32 s2 = (insn >> 6) & 3, T = (insn >> 5) & 1;
            int es = s2 == 3 ? (nst == 4 ? 32 : 0) : 8 << s2;
            if (!es) { nundef(c, insn); return; }
            int inc = nst == 1 ? 1 : (T ? 2 : 1), regs = nst == 1 ? (T ? 2 : 1) : 1;
            u32 a = addr;
            for (u32 s = 0; s < nst; s++) {
                u64 v = ldmem(a, es), rep = 0;
                a += (u32)es / 8;
                for (int e = 0; e < 64 / es; e++) se_(&rep, es, e, v);
                if (nst == 1) for (int r = 0; r < regs; r++) c->v.q[(d + (u32)r) & 31] = rep;
                else c->v.q[(d + s * (u32)inc) & 31] = rep;
            }
            bytes = a - addr;
        } else {                                                    /* single lane */
            int es = 8 << sz, idx, inc = 1;
            u32 ia = (insn >> 4) & 0xF;
            if (sz == 0) idx = (int)(ia >> 1);
            else if (sz == 1) { idx = (int)(ia >> 2); if (nst > 1 && (ia & 2)) inc = 2; }
            else { idx = (int)(ia >> 3); if (nst > 1 && (ia & 4)) inc = 2; }
            u32 a = addr;
            for (u32 s = 0; s < nst; s++) {
                u32 reg = (d + s * (u32)inc) & 31;
                if (L) se_(&c->v.q[reg], es, idx, ldmem(a, es)); else stmem(a, es, ge_(&c->v.q[reg], es, idx));
                a += (u32)es / 8;
            }
            bytes = a - addr;
        }
    }
    if (rm == 15) return;
    c->r[rn] = addr + (rm == 13 ? bytes : c->r[rm]);
}
