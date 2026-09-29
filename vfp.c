/*
 * vfp.c - VFPv3-D32 / VFPv4 coprocessor (cp10=single, cp11=double), plus the
 * NEON core<->scalar transfers that live in the same encoding space.
 *
 * Encoding rules validated against the binary's ground truth:
 *   f32 reg indices: Fd=(Vd<<1)|bit22, Fn=(Vn<<1)|bit7, Fm=(Vm<<1)|bit5
 *   f64 reg indices: Fd=(bit22<<4)|Vd, Fn=(bit7<<4)|Vn, Fm=(bit5<<4)|Vm  (d0-15)
 *   3-reg op = bits{23,21,20}: 0=vmla/s 1=vnmls/a 2=vmul/vnmul 3=vadd/vsub 4=vdiv
 *   extension group (op=7): sub in bits19-16, aux in bits7-6
 * Host float/double arithmetic matches VFP (round-nearest, no fused MLA).
 */
#include "emu.h"
#include "vfpops.h"

static inline u32 rr(cpu_t *c, u32 n) { return n == 15 ? c->r[15] + 4 : c->r[n]; }

static void vfp_trap(cpu_t *c, u32 insn, const char *what)
{
    emu_trap(c, "VFP unimplemented %s (insn %08x)", what, insn);
}

/* FPSCR flag bits (same positions as CPSR) */
static void vfp_set_cmp_flags(cpu_t *c, f64 a, f64 b)
{
    u32 f;
    if (isunordered(a, b)) f = 0x30000000u;          /* C=1,V=1 */
    else if (a == b)         f = FLAG_Z | FLAG_C;
    else if (a < b)          f = FLAG_N;
    else                     f = FLAG_C;
    c->fpscr = (c->fpscr & 0x0FFFFFFFu) | f;
}

/* float -> int conversion with ARM saturation semantics */
static u32 vfp_f2i(cpu_t *c, f64 v, bool to_unsigned, bool use_fpcr_rm)
{
    if (isnan(v)) return 0;
    u32 rm = use_fpcr_rm ? ((c->fpscr >> 22) & 3) : 3;   /* 3 = round-to-zero */
    f64 r;
    switch (rm) {
    case 0: r = nearbyint(v); break;      /* to nearest, ties even */
    case 1: r = ceil(v); break;
    case 2: r = floor(v); break;
    default: r = trunc(v); break;
    }
    if (to_unsigned) {
        if (r <= 0.0) return 0;
        if (r >= 4294967295.0) return 0xFFFFFFFFu;
        return (u32)r;
    }
    if (r >= 2147483647.0) return 0x7FFFFFFFu;
    if (r <= -2147483648.0) return 0x80000000u;
    return (u32)(s32)r;
}

/* VFPExpandImm: imm8 = abcdefgh -> a : NOT(b) : b..b : cd : efgh : 0..0 */
u32 vfp_imm_bits32(u32 imm8)
{
    u32 a = (imm8 >> 7) & 1, b = (imm8 >> 6) & 1, cd = (imm8 >> 4) & 3, efgh = imm8 & 15;
    return a << 31 | ((b ? 0x7Cu : 0x80u) | cd) << 23 | efgh << 19;
}
static f32 vfp_imm_f32(u32 imm8) { u32 bits = vfp_imm_bits32(imm8); f32 v; memcpy(&v, &bits, 4); return v; }
static f64 vfp_imm_f64(u32 imm8)
{
    u64 a = (imm8 >> 7) & 1, b = (imm8 >> 6) & 1, cd = (imm8 >> 4) & 3, efgh = imm8 & 15;
    u64 bits = a << 63 | ((b ? 0x3FCull : 0x400ull) | cd) << 52 | efgh << 48;
    f64 v; memcpy(&v, &bits, 8); return v;
}

/* ------------------------------------------------------------------ */

void vfp_cdp(cpu_t *c, u32 insn)
{
    u32 D = (insn >> 22) & 1, N = (insn >> 7) & 1, M = (insn >> 5) & 1;
    u32 vn = (insn >> 16) & 0xF, vd = (insn >> 12) & 0xF, vm = insn & 0xF;
    u32 cp = (insn >> 8) & 0xF;
    int dbl = (cp == 11);

    u32 sd = (vd << 1) | D, dd = (D << 4) | vd;
    u32 sn = (vn << 1) | N, dn = (N << 4) | vn;
    u32 sm = (vm << 1) | M, dm = (M << 4) | vm;
#define D16(x) do { (void)(x); } while (0)

    u32 op = ((insn >> 23) & 1) << 2 | ((insn >> 20) & 3);
    u32 L = (insn >> 6) & 1;

    if (op != 7) {
        /* three-register arithmetic; 5 = vfnms/vfnma, 6 = vfma/vfms (VFPv4, fused) */
        u32 f = c->fpscr;
        if (dbl) {
            u64 n = c->v.q[dn], m = c->v.q[dm], a = c->v.q[dd], p, r;
            switch (op) {
            case 0: p = vfp_op64(f, FOP_MUL, n, m); r = vfp_op64(f, FOP_ADD, a, L ? vfp_neg64(p) : p); break;          /* vmla/vmls */
            case 1: p = vfp_op64(f, FOP_MUL, n, m); r = vfp_op64(f, FOP_ADD, vfp_neg64(a), L ? vfp_neg64(p) : p); break; /* vnmls/vnmla */
            case 2: p = vfp_op64(f, FOP_MUL, n, m); r = L ? vfp_neg64(p) : p; break;                                   /* vmul/vnmul */
            case 3: r = vfp_op64(f, L ? FOP_SUB : FOP_ADD, n, m); break;                                              /* vadd/vsub */
            case 4: r = vfp_op64(f, FOP_DIV, n, m); break;                                                            /* vdiv */
            case 5: r = vfp_fma64(f, vfp_neg64(a), L ? vfp_neg64(n) : n, m); break;                                   /* vfnms / vfnma */
            default: r = vfp_fma64(f, a, L ? vfp_neg64(n) : n, m); break;                                             /* vfma / vfms */
            }
            c->v.q[dd] = r;
        } else {
            u32 n = c->v.w[sn], m = c->v.w[sm], a = c->v.w[sd], p, r;
            switch (op) {
            case 0: p = vfp_op32(f, FOP_MUL, n, m); r = vfp_op32(f, FOP_ADD, a, L ? vfp_neg32(p) : p); break;
            case 1: p = vfp_op32(f, FOP_MUL, n, m); r = vfp_op32(f, FOP_ADD, vfp_neg32(a), L ? vfp_neg32(p) : p); break;
            case 2: p = vfp_op32(f, FOP_MUL, n, m); r = L ? vfp_neg32(p) : p; break;
            case 3: r = vfp_op32(f, L ? FOP_SUB : FOP_ADD, n, m); break;
            case 4: r = vfp_op32(f, FOP_DIV, n, m); break;
            case 5: r = vfp_fma32(f, vfp_neg32(a), L ? vfp_neg32(n) : n, m); break;
            default: r = vfp_fma32(f, a, L ? vfp_neg32(n) : n, m); break;
            }
            c->v.w[sd] = r;
        }
        return;
    }

    /* extension group: sub-op in bits19-16, aux in bits7-6 */
    u32 sub = vn, b76 = (N << 1) | L;
    if (!(b76 & 1)) {                    /* vmov immediate (bit6 = 0; bit7 SBZ) */
        u32 imm8 = (vn << 4) | vm;
        if (dbl) D16(dd);
        if (dbl) c->v.d[dd] = vfp_imm_f64(imm8);
        else     c->v.f[sd] = vfp_imm_f32(imm8);
        return;
    }
    if (dbl) {                            /* only the operands each sub-op really uses */
        if (sub <= 5) D16(dd | dm);
        else if (sub == 7 || sub >= 0xC) D16(dm);
        else if (sub == 8) D16(dd);
    }
    switch (sub) {
    case 0x0:
        if (b76 == 1) { if (dbl) c->v.q[dd] = c->v.q[dm]; else c->v.w[sd] = c->v.w[sm]; }
        else if (b76 == 3) { if (dbl) c->v.q[dd] = c->v.q[dm] & ~(1ULL << 63); else c->v.w[sd] = c->v.w[sm] & 0x7FFFFFFFu; }
        else vfp_trap(c, insn, "sub0 b76=2");
        break;
    case 0x1:
        if (b76 == 1) { if (dbl) c->v.q[dd] = vfp_neg64(c->v.q[dm]); else c->v.w[sd] = vfp_neg32(c->v.w[sm]); }
        else if (b76 == 3) { if (dbl) c->v.q[dd] = vfp_sqrt64(c->fpscr, c->v.q[dm]); else c->v.w[sd] = vfp_sqrt32(c->fpscr, c->v.w[sm]); }
        else vfp_trap(c, insn, "sub1 b76=2");
        break;
    case 0x4: case 0x5: {                 /* vcmp(e) two-reg / with zero */
        bool fz = c->fpscr & FPSCR_FZ, z = sub == 5;
        if (dbl) vfp_set_cmp_flags(c, b2d(fz ? ftz64(c->v.q[dd]) : c->v.q[dd]), z ? 0.0 : b2d(fz ? ftz64(c->v.q[dm]) : c->v.q[dm]));
        else     vfp_set_cmp_flags(c, b2f(fz ? ftz32(c->v.w[sd]) : c->v.w[sd]), z ? 0.0f : b2f(fz ? ftz32(c->v.w[sm]) : c->v.w[sm]));
        break; }
    case 0x7:                             /* vcvt f32<->f64 */
        if (b76 != 3) vfp_trap(c, insn, "sub7");
        if (dbl) c->v.w[sd] = vfp_cvt_sd(c->fpscr, c->v.q[dm]);   /* vcvt.f32.f64 */
        else     c->v.q[dd] = vfp_cvt_ds(c->fpscr, c->v.w[sm]);   /* vcvt.f64.f32 */
        break;
    case 0x2: case 0x3: {                 /* vcvtb / vcvtt: half <-> single (bit7 = top half) */
        extern u16 f32_to_f16(u32 b, int rmode);
        extern u32 f16_to_f32(u16 h);
        if (dbl) vfp_trap(c, insn, "vcvt f16 double");
        int top = (insn >> 7) & 1;
        if (sub == 2) c->v.w[sd] = f16_to_f32((u16)(c->v.w[sm] >> (top * 16)));
        else {
            u16 h = f32_to_f16(c->v.w[sm], (c->fpscr >> 22) & 3);
            c->v.w[sd] = top ? (c->v.w[sd] & 0xFFFF) | ((u32)h << 16) : (c->v.w[sd] & 0xFFFF0000u) | h;
        }
        break; }
    case 0xA: case 0xB: case 0xE: case 0xF: {   /* vcvt float <-> fixed point; Vd is source and dest */
        bool to_fixed = sub & 4, uns = sub & 1;
        int size = (insn >> 7) & 1 ? 32 : 16;
        int fbits = size - (int)(((insn & 0xF) << 1) | ((insn >> 5) & 1));
        if (to_fixed) {
            f64 v = (dbl ? c->v.d[dd] : (f64)c->v.f[sd]) * ldexp(1.0, fbits), r = trunc(v);
            s64 lo = uns ? 0 : -(1LL << (size - 1)), hi = uns ? (1LL << size) - 1 : (1LL << (size - 1)) - 1;
            s64 iv = isnan(v) ? 0 : r < (f64)lo ? lo : r > (f64)hi ? hi : (s64)r;
            u32 bits = (u32)iv;
            if (size == 16) bits = uns ? bits & 0xFFFF : (u32)(s32)(s16)bits;
            if (dbl) c->v.q[dd] = uns ? (u64)bits : (u64)(s64)(s32)bits;   /* Extend(result, 64) */
            else c->v.w[sd] = bits;
        } else {
            u32 raw = dbl ? (u32)c->v.q[dd] : c->v.w[sd];
            f64 x = size == 16 ? (uns ? (f64)(u16)raw : (f64)(s16)raw) : (uns ? (f64)raw : (f64)(s32)raw);
            x /= ldexp(1.0, fbits);
            if (dbl) c->v.d[dd] = x; else c->v.f[sd] = (f32)x;
        }
        break; }
    case 0x8: {                           /* vcvt int -> float: b76 bit1: 1=s32 0=u32 */
        bool sign = b76 & 2;
        s32 si = (s32)c->v.w[sm];
        u32 ui = c->v.w[sm];
        if (dbl) c->v.d[dd] = sign ? (f64)si : (f64)ui;
        else     c->v.f[sd] = sign ? (f32)si : (f32)ui;
        break; }
    case 0xC:                             /* vcvt float -> u32 */
        c->v.w[sd] = dbl ? vfp_f2i(c, c->v.d[dm], true, !(b76 & 2))
                         : vfp_f2i(c, c->v.f[sm], true, !(b76 & 2));
        break;
    case 0xD:                             /* vcvt float -> s32 */
        c->v.w[sd] = dbl ? vfp_f2i(c, c->v.d[dm], false, !(b76 & 2))
                         : vfp_f2i(c, c->v.f[sm], false, !(b76 & 2));
        break;
    default:
        vfp_trap(c, insn, "ext sub-op");
    }
}

/* MCR/MRC: vmov core<->single, vmrs/vmsr */
void vfp_mcr_mrc(cpu_t *c, u32 insn)
{
    u32 L = (insn >> 20) & 1, op1 = (insn >> 21) & 7;
    u32 vn = (insn >> 16) & 0xF, rt = (insn >> 12) & 0xF, N = (insn >> 7) & 1;

    if (((insn >> 8) & 0xF) == 11) {                  /* NEON: core <-> scalar, vdup (core) */
        u32 dreg = (N << 4) | vn;
        if (!L && (insn & (1u << 23))) {              /* vdup.<size> Dd/Qd, Rt */
            u32 B = (insn >> 22) & 1, Q = (insn >> 21) & 1, E = (insn >> 5) & 1, v = c->r[rt];
            u64 rep = B ? (v & 0xFF) * 0x0101010101010101ULL : E ? (v & 0xFFFF) * 0x0001000100010001ULL
                        : (u64)v * 0x100000001ULL;
            if (B && E) vfp_trap(c, insn, "vdup size");
            c->v.q[dreg] = rep;
            if (Q) c->v.q[(dreg + 1) & 31] = rep;
            return;
        }
        u32 opc1 = (insn >> 21) & 3, opc2 = (insn >> 5) & 3, U = (insn >> 23) & 1;
        int es, idx;
        if (opc1 & 2) { es = 8; idx = (int)(((opc1 & 1) << 2) | opc2); }
        else if (opc2 & 1) { es = 16; idx = (int)(((opc1 & 1) << 1) | (opc2 >> 1)); }
        else if (!(opc2 & 2)) { es = 32; idx = (int)(opc1 & 1); }
        else { vfp_trap(c, insn, "vmov scalar size"); return; }
        u64 *dw = &c->v.q[dreg];
        int sh = idx * es;
        u64 mk = (es == 32 ? 0xFFFFFFFFULL : (1ULL << es) - 1) << sh;
        if (L) {
            u32 e = (u32)((*dw & mk) >> sh);
            if (!U && es < 32) e = (u32)((s32)(e << (32 - es)) >> (32 - es));
            c->r[rt] = e;
        } else *dw = (*dw & ~mk) | (((u64)c->r[rt] << sh) & mk);
        return;
    }

    if (op1 == 7) {                                   /* system registers */
        if (L) {                                      /* vmrs */
            u32 v = 0;
            if (vn == 1) v = c->fpscr;
            else if (vn == 0) v = 0x410330B3;         /* FPSID: VFPv3 */
            else if (vn == 8) v = 0x40000000;         /* FPEXC: EN */
            else if (vn == 7) v = 0x10110222;         /* MVFR0: VFPv3 D32, sqrt, div, short vectors none */
            else if (vn == 6) v = 0x11111111;         /* MVFR1: NEON int/sp/half, FTZ, DN */
            else LOG_ONCE("[vfp] vmrs from sysreg %u -> 0\n", vn);
            if (rt == 15) c->cpsr = (c->cpsr & 0x0FFFFFFFu) | (v & 0xF0000000u);
            else c->r[rt] = v;
        } else {                                      /* vmsr */
            if (vn == 1) c->fpscr = (rt == 15 ? 0 : c->r[rt]) & 0xFFC0009Fu;
            else LOG_ONCE("[vfp] vmsr to sysreg %u ignored\n", vn);
        }
        return;
    }
    if (op1 != 0) vfp_trap(c, insn, "mcr/mrc op1");
    u32 s = (vn << 1) | N;
    if (L) c->r[rt] = c->v.w[s];
    else   c->v.w[s] = c->r[rt];
}

/* MCRR/MRRC: vmov two core regs <-> double (or two singles) */
void vfp_mcrr_mrrc(cpu_t *c, u32 insn)
{
    u32 L = (insn >> 20) & 1, cp = (insn >> 8) & 0xF;
    u32 rt2 = (insn >> 16) & 0xF, rt = (insn >> 12) & 0xF, crm = insn & 0xF;
    u32 M = (insn >> 5) & 1;
    if (cp == 11) {
        u32 dm = (M << 4) | crm;
        if (L) { u64 v = c->v.q[dm]; c->r[rt] = (u32)v; c->r[rt2] = (u32)(v >> 32); }
        else   c->v.q[dm] = (u64)c->r[rt] | ((u64)c->r[rt2] << 32);
    } else {
        u32 sm = (crm << 1) | M;
        if (sm == 31) vfp_trap(c, insn, "vmov s31,s32");
        if (L) { c->r[rt] = c->v.w[sm]; c->r[rt2] = c->v.w[sm + 1]; }
        else   { c->v.w[sm] = c->r[rt]; c->v.w[sm + 1] = c->r[rt2]; }
    }
}

/* LDC/STC: vldr/vstr (P=1,W=0), vldm/vstm/vpush/vpop (otherwise) */
void vfp_ldc_stc(cpu_t *c, u32 insn)
{
    u32 P = (insn >> 24) & 1, U = (insn >> 23) & 1, D = (insn >> 22) & 1;
    u32 W = (insn >> 21) & 1, L = (insn >> 20) & 1;
    u32 rn = (insn >> 16) & 0xF, vd = (insn >> 12) & 0xF;
    u32 imm8 = insn & 0xFF;
    int dbl = ((insn >> 8) & 0xF) == 11;
    u32 base = rr(c, rn);

    if (P && !W) {                              /* single register */
        u32 ea = U ? base + imm8 * 4 : base - imm8 * 4;
        if (dbl) {
            u32 reg = (D << 4) | vd;
            if (L) c->v.q[reg] = ld32(ea) | ((u64)ld32(ea + 4) << 32);
            else { st32(ea, (u32)c->v.q[reg]); st32(ea + 4, (u32)(c->v.q[reg] >> 32)); }
        } else {
            u32 reg = (vd << 1) | D;
            if (L) c->v.w[reg] = ld32(ea); else st32(ea, c->v.w[reg]);
        }
        return;
    }
    /* multiple registers: IA (P=0,U=1) or DB (P=1,U=0,W=1 — vpush), writeback if W */
    if (P == U) vfp_trap(c, insn, "vldm/vstm addressing");
    u32 bytes = imm8 * 4;
    u32 ea = U ? base : base - bytes;
    u32 addr = ea;
    if (dbl) {
        u32 reg = (D << 4) | vd, n = imm8 / 2;
        if (reg + n > 32) vfp_trap(c, insn, "vldm/vstm past d31");
        for (u32 i = 0; i < n; i++) {
            if (L) c->v.q[reg + i] = ld32(addr) | ((u64)ld32(addr + 4) << 32);
            else { st32(addr, (u32)c->v.q[reg + i]); st32(addr + 4, (u32)(c->v.q[reg + i] >> 32)); }
            addr += 8;
        }
    } else {
        u32 reg = (vd << 1) | D, n = imm8;
        if (reg + n > 32) vfp_trap(c, insn, "vldm/vstm past s31");
        for (u32 i = 0; i < n; i++) {
            if (L) c->v.w[reg + i] = ld32(addr); else st32(addr, c->v.w[reg + i]);
            addr += 4;
        }
    }
    if (W) c->r[rn] = U ? base + bytes : base - bytes;
}
