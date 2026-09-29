/*
 * vfpops.h - VFP scalar arithmetic with ARM semantics, on bit patterns.
 *
 * Fast path: host float math when FPSCR.DN/FZ are clear and the result is
 * not a NaN (host and ARM agree on every other result). Otherwise the ARM
 * rules: NaN operands propagate in order (first signalling, then first
 * quiet, quieted), invalid operations give the positive default NaN,
 * DN forces the default NaN, FZ flushes denormal inputs and results.
 * Shared by vfp.c (reference), fastops.h and the JIT's slow paths.
 */
#ifndef VFPOPS_H
#define VFPOPS_H
#include "emu.h"

#define FPSCR_DN 0x02000000u
#define FPSCR_FZ 0x01000000u

static inline u32 f2b(f32 f) { u32 b; memcpy(&b, &f, 4); return b; }
static inline f32 b2f(u32 b) { f32 f; memcpy(&f, &b, 4); return f; }
static inline u64 d2b(f64 f) { u64 b; memcpy(&b, &f, 8); return b; }
static inline f64 b2d(u64 b) { f64 f; memcpy(&f, &b, 8); return f; }

static inline bool nan32(u32 b) { return (b & 0x7F800000u) == 0x7F800000u && (b & 0x7FFFFFu); }
static inline bool snan32(u32 b) { return nan32(b) && !(b & 0x400000u); }
static inline bool nan64(u64 b) { return (b & 0x7FF0000000000000ULL) == 0x7FF0000000000000ULL && (b & 0xFFFFFFFFFFFFFULL); }
static inline bool snan64(u64 b) { return nan64(b) && !(b & 0x8000000000000ULL); }
static inline u32 ftz32(u32 b) { return (b & 0x7F800000u) ? b : b & 0x80000000u; }
static inline u64 ftz64(u64 b) { return (b & 0x7FF0000000000000ULL) ? b : b & 0x8000000000000000ULL; }

enum { FOP_ADD, FOP_SUB, FOP_MUL, FOP_DIV };

static inline u32 vfp_pick_nan32(u32 fpscr, u32 a, u32 b, u32 c3, int n)
{
    u32 r;
    if (snan32(a)) r = a; else if (n > 1 && snan32(b)) r = b; else if (n > 2 && snan32(c3)) r = c3;
    else if (nan32(a)) r = a; else if (n > 1 && nan32(b)) r = b; else r = c3;
    return (fpscr & FPSCR_DN) ? 0x7FC00000u : r | 0x400000u;
}
static inline u64 vfp_pick_nan64(u32 fpscr, u64 a, u64 b, u64 c3, int n)
{
    u64 r;
    if (snan64(a)) r = a; else if (n > 1 && snan64(b)) r = b; else if (n > 2 && snan64(c3)) r = c3;
    else if (nan64(a)) r = a; else if (n > 1 && nan64(b)) r = b; else r = c3;
    return (fpscr & FPSCR_DN) ? 0x7FF8000000000000ULL : r | 0x8000000000000ULL;
}
static inline u32 vfp_res32(u32 fpscr, f32 r)
{
    u32 b = f2b(r);
    if (nan32(b)) return 0x7FC00000u;                 /* invalid operation: default NaN */
    return (fpscr & FPSCR_FZ) ? ftz32(b) : b;
}
static inline u64 vfp_res64(u32 fpscr, f64 r)
{
    u64 b = d2b(r);
    if (nan64(b)) return 0x7FF8000000000000ULL;
    return (fpscr & FPSCR_FZ) ? ftz64(b) : b;
}

static inline f32 fop32(int op, f32 x, f32 y) { return op == FOP_ADD ? x + y : op == FOP_SUB ? x - y : op == FOP_MUL ? x * y : x / y; }
static inline f64 fop64(int op, f64 x, f64 y) { return op == FOP_ADD ? x + y : op == FOP_SUB ? x - y : op == FOP_MUL ? x * y : x / y; }

static inline u32 vfp_op32(u32 fpscr, int op, u32 a, u32 b)
{
    if (__builtin_expect(!(fpscr & (FPSCR_DN | FPSCR_FZ)), 1)) {
        f32 r = fop32(op, b2f(a), b2f(b));
        if (__builtin_expect(r == r, 1)) return f2b(r);
    }
    if (fpscr & FPSCR_FZ) { a = ftz32(a); b = ftz32(b); }
    if (nan32(a) || nan32(b)) return vfp_pick_nan32(fpscr, a, b, 0, 2);
    return vfp_res32(fpscr, fop32(op, b2f(a), b2f(b)));
}
static inline u64 vfp_op64(u32 fpscr, int op, u64 a, u64 b)
{
    if (__builtin_expect(!(fpscr & (FPSCR_DN | FPSCR_FZ)), 1)) {
        f64 r = fop64(op, b2d(a), b2d(b));
        if (__builtin_expect(r == r, 1)) return d2b(r);
    }
    if (fpscr & FPSCR_FZ) { a = ftz64(a); b = ftz64(b); }
    if (nan64(a) || nan64(b)) return vfp_pick_nan64(fpscr, a, b, 0, 2);
    return vfp_res64(fpscr, fop64(op, b2d(a), b2d(b)));
}
static inline u32 vfp_neg32(u32 a) { return a ^ 0x80000000u; }
static inline u64 vfp_neg64(u64 a) { return a ^ 0x8000000000000000ULL; }

/* fused a + x*y (VFPv4 VFMA family): NaN order addend, x, y */
static inline u32 vfp_fma32(u32 fpscr, u32 a, u32 x, u32 y)
{
    if (fpscr & FPSCR_FZ) { a = ftz32(a); x = ftz32(x); y = ftz32(y); }
    if (nan32(a) || nan32(x) || nan32(y)) {
        bool inf0 = ((x & 0x7FFFFFFFu) == 0x7F800000u && !(y & 0x7FFFFFFFu)) || ((y & 0x7FFFFFFFu) == 0x7F800000u && !(x & 0x7FFFFFFFu));
        if (inf0 && !snan32(a) && nan32(a) && !nan32(x) && !nan32(y)) return 0x7FC00000u;
        return vfp_pick_nan32(fpscr, a, x, y, 3);
    }
    return vfp_res32(fpscr, fmaf(b2f(x), b2f(y), b2f(a)));
}
static inline u64 vfp_fma64(u32 fpscr, u64 a, u64 x, u64 y)
{
    if (fpscr & FPSCR_FZ) { a = ftz64(a); x = ftz64(x); y = ftz64(y); }
    if (nan64(a) || nan64(x) || nan64(y)) {
        bool inf0 = ((x << 1) == 0xFFE0000000000000ULL && !(y << 1)) || ((y << 1) == 0xFFE0000000000000ULL && !(x << 1));
        if (inf0 && !snan64(a) && nan64(a) && !nan64(x) && !nan64(y)) return 0x7FF8000000000000ULL;
        return vfp_pick_nan64(fpscr, a, x, y, 3);
    }
    return vfp_res64(fpscr, fma(b2d(x), b2d(y), b2d(a)));
}
static inline u32 vfp_sqrt32(u32 fpscr, u32 a)
{
    if (fpscr & FPSCR_FZ) a = ftz32(a);
    if (nan32(a)) return vfp_pick_nan32(fpscr, a, 0, 0, 1);
    return vfp_res32(fpscr, sqrtf(b2f(a)));
}
static inline u64 vfp_sqrt64(u32 fpscr, u64 a)
{
    if (fpscr & FPSCR_FZ) a = ftz64(a);
    if (nan64(a)) return vfp_pick_nan64(fpscr, a, 0, 0, 1);
    return vfp_res64(fpscr, sqrt(b2d(a)));
}
static inline u32 vfp_cvt_sd(u32 fpscr, u64 x)          /* f64 -> f32 */
{
    if (fpscr & FPSCR_FZ) x = ftz64(x);
    if (nan64(x)) return (fpscr & FPSCR_DN) ? 0x7FC00000u : (u32)(x >> 32 & 0x80000000u) | 0x7FC00000u | (u32)(x >> 29 & 0x3FFFFFu);
    return vfp_res32(fpscr, (f32)b2d(x));
}
static inline u64 vfp_cvt_ds(u32 fpscr, u32 x)          /* f32 -> f64 */
{
    if (fpscr & FPSCR_FZ) x = ftz32(x);
    if (nan32(x)) return (fpscr & FPSCR_DN) ? 0x7FF8000000000000ULL : ((u64)(x & 0x80000000u) << 32) | 0x7FF8000000000000ULL | ((u64)(x & 0x3FFFFFu) << 29);
    return d2b((f64)b2f(x));
}
#endif
