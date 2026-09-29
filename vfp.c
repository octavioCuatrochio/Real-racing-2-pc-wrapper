/*
 * vfp.c - VFPv3-D16 coprocessor (cp10=single, cp11=double).
 *
 * Encoding rules validated against the binary's ground truth:
 *   f32 reg indices: Fd=(Vd<<1)|bit22, Fn=(Vn<<1)|bit7, Fm=(Vm<<1)|bit5
 *   f64 reg indices: Fd=(bit22<<4)|Vd, Fn=(bit7<<4)|Vn, Fm=(bit5<<4)|Vm  (d0-15)
 *   3-reg op = bits{23,21,20}: 0=vmla/s 1=vnmls/a 2=vmul/vnmul 3=vadd/vsub 4=vdiv
 *   extension group (op=7): sub in bits19-16, aux in bits7-6
 * Host float/double arithmetic matches VFP (round-nearest, no fused MLA).
 */
#include "emu.h"

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

/* VFPv3 immediate format: s | eeee | fff -> (1+f/8) * 2^(e-7) */
static f32 vfp_imm_f32(u32 imm8)
{
    u32 s = (imm8 >> 7) & 1, e = (imm8 >> 3) & 0xF, f = imm8 & 7;
    u32 bits = (s << 31) | ((e + 120) << 23) | (f << 20);
    f32 v; memcpy(&v, &bits, 4); return v;
}
static f64 vfp_imm_f64(u32 imm8)
{
    u64 s = (imm8 >> 7) & 1, e = (imm8 >> 3) & 0xF, f = imm8 & 7;
    u64 bits = (s << 63) | ((e + 1016) << 52) | (f << 49);
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
#define D16(x) do { if (__builtin_expect((x) >= 16, 0)) vfp_trap(c, insn, "d16-d31 register"); } while (0)

    u32 op = ((insn >> 23) & 1) << 2 | ((insn >> 20) & 3);
    u32 L = (insn >> 6) & 1;

    if (op != 7) {
        /* three-register arithmetic */
        if (op > 4) vfp_trap(c, insn, "3reg op");
        if (dbl) {
            D16(dd | dn | dm);
            f64 n = c->v.d[dn], m = c->v.d[dm], r;
            switch (op) {
            case 0: r = L ? c->v.d[dd] - n * m : n * m + c->v.d[dd]; break;   /* vmla/vmls */
            case 1: r = L ? -(n * m) - c->v.d[dd] : n * m - c->v.d[dd]; break;/* vnmla/vnmls */
            case 2: r = L ? -(n * m) : n * m; break;                          /* vmul/vnmul */
            case 3: r = L ? n - m : n + m; break;                             /* vadd/vsub */
            default: r = n / m; break;                                        /* vdiv */
            }
            c->v.d[dd] = r;
        } else {
            f32 n = c->v.f[sn], m = c->v.f[sm], r;
            switch (op) {
            case 0: r = L ? c->v.f[sd] - n * m : n * m + c->v.f[sd]; break;
            case 1: r = L ? -(n * m) - c->v.f[sd] : n * m - c->v.f[sd]; break;
            case 2: r = L ? -(n * m) : n * m; break;
            case 3: r = L ? n - m : n + m; break;
            default: r = n / m; break;
            }
            c->v.f[sd] = r;
        }
        return;
    }

    /* extension group: sub-op in bits19-16, aux in bits7-6 */
    u32 sub = vn, b76 = (N << 1) | L;
    if (b76 == 0) {                       /* vmov immediate */
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
        if (b76 == 1) { if (dbl) c->v.d[dd] = c->v.d[dm]; else c->v.f[sd] = c->v.f[sm]; }
        else if (b76 == 3) { if (dbl) c->v.d[dd] = fabs(c->v.d[dm]); else c->v.f[sd] = fabsf(c->v.f[sm]); }
        else vfp_trap(c, insn, "sub0 b76=2");
        break;
    case 0x1:
        if (b76 == 1) { if (dbl) c->v.d[dd] = -c->v.d[dm]; else c->v.f[sd] = -c->v.f[sm]; }
        else if (b76 == 3) { if (dbl) c->v.d[dd] = sqrt(c->v.d[dm]); else c->v.f[sd] = sqrtf(c->v.f[sm]); }
        else vfp_trap(c, insn, "sub1 b76=2");
        break;
    case 0x4:                             /* vcmp(e) two-reg */
        if (dbl) vfp_set_cmp_flags(c, c->v.d[dd], c->v.d[dm]);
        else     vfp_set_cmp_flags(c, c->v.f[sd], c->v.f[sm]);
        break;
    case 0x5:                             /* vcmp(e) with zero */
        if (dbl) vfp_set_cmp_flags(c, c->v.d[dd], 0.0);
        else     vfp_set_cmp_flags(c, c->v.f[sd], 0.0f);
        break;
    case 0x7:                             /* vcvt f32<->f64 */
        if (dbl) c->v.f[sd] = (f32)c->v.d[dm];   /* vcvt.f32.f64 */
        else     c->v.d[dd] = (f64)c->v.f[sm];   /* vcvt.f64.f32 */
        break;
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

    if (op1 == 7) {                                   /* system registers */
        if (L) {                                      /* vmrs */
            u32 v = 0;
            if (vn == 1) v = c->fpscr;
            else if (vn == 0) v = 0x410330B3;         /* FPSID: VFPv3 */
            else if (vn == 8) v = 0x40000000;         /* FPEXC: EN */
            else LOG_ONCE("[vfp] vmrs from sysreg %u -> 0\n", vn);
            if (rt == 15) c->cpsr = (c->cpsr & 0x0FFFFFFFu) | (v & 0xF0000000u);
            else c->r[rt] = v;
        } else {                                      /* vmsr */
            if (vn == 1) c->fpscr = (rt == 15 ? 0 : c->r[rt]) & 0xF7C00000u;
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
    if (cp == 11) {
        if (crm >= 16) vfp_trap(c, insn, "mcrr d16+");
        if (L) { u64 v = c->v.q[crm]; c->r[rt] = (u32)v; c->r[rt2] = (u32)(v >> 32); }
        else   c->v.q[crm] = (u64)c->r[rt] | ((u64)c->r[rt2] << 32);
    } else {
        if (L) { c->r[rt] = c->v.w[crm]; c->r[rt2] = c->v.w[crm + 1]; }
        else   { c->v.w[crm] = c->r[rt]; c->v.w[crm + 1] = c->r[rt2]; }
        LOG_ONCE("[vfp] mcrr/mrrc cp10 (two-single vmov)\n");
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
            if (reg >= 16) vfp_trap(c, insn, "vldr/vstr d16+");
            if (L) c->v.d[reg] = ldf64(ea); else stf64(ea, c->v.d[reg]);
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
        if (reg + n > 16) vfp_trap(c, insn, "vldm/vstm d16+");
        for (u32 i = 0; i < n; i++) {
            if (L) c->v.d[reg + i] = ldf64(addr); else stf64(addr, c->v.d[reg + i]);
            addr += 8;
        }
    } else {
        u32 reg = (vd << 1) | D, n = imm8;
        for (u32 i = 0; i < n; i++) {
            if (L) c->v.w[reg + i] = ld32(addr); else st32(addr, c->v.w[reg + i]);
            addr += 4;
        }
    }
    if (W) c->r[rn] = U ? base + bytes : base - bytes;
}
