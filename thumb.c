/*
 * thumb.c - Thumb / Thumb-2 decoder.
 *
 * Every Thumb instruction becomes a di_t: where an ARM encoding with the
 * same semantics exists it is built and decoded by the ARM decoder (so the
 * fast ops and the ARM reference handlers are shared); the rest use
 * Thumb-only fast ops or fast ops with precomputed operands.
 *
 * PC reads are folded at decode time (Thumb PC = insn + 4, Align(PC, 4) for
 * literals), so no translated ARM instruction ever sees r15 as an operand.
 * During execution r15 holds the next instruction's address.
 *
 * IT blocks: decoding an IT records the condition of each following
 * instruction in a per-halfword map; those instructions then decode with
 * that condition in d->cond (and 16-bit ones as their non-flag-setting forms).
 */
#include "fastops.h"
#include <sys/mman.h>

void dec_arm(di_t *d, u32 pc, u32 insn);
void dec_finish(di_t *d, u32 pc, int op);

static u8 *itmap;                  /* per text halfword: 0, or 0x10 | cond inside an IT block */
static u32 it_lo, it_n;

void thumb_it_reset(void)
{
    if (itmap && it_lo == G.text_lo && it_n == G.text_span / 2) return;
    if (itmap) munmap(itmap, it_n ? it_n : 1);
    it_lo = G.text_lo; it_n = G.text_span / 2;
    itmap = mmap(NULL, it_n ? it_n : 1, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (itmap == MAP_FAILED) fatal("itmap alloc failed");
}

void thumb_it_clear(void) { if (itmap) memset(itmap, 0, it_n); }

static inline u32 tlen(u16 hw1) { return (hw1 >> 11) >= 0x1D ? 4 : 2; }

/* ------------------------------------------------------------------ */

typedef struct { di_t *d; u32 pc; u8 cond; u8 len; bool in_it; } ctx_t;

static void via_arm(ctx_t *x, u32 w)
{
    dec_arm(x->d, x->pc, w);
    x->d->cond = x->cond;
    x->d->cx = x->cond ^ 0xE;
    x->d->len = x->len;
}
static void via_op(ctx_t *x, int op)
{
    x->d->cond = x->cond;
    x->d->len = x->len;
    dec_finish(x->d, x->pc, op);
}
static void undef(ctx_t *x, u32 raw)
{
    (void)raw;
    via_arm(x, 0xE7F000F0u);                  /* ARM UDF: traps when executed */
}

#define AL 0xE0000000u
#define DP_OP(opc, S) (OP_dp_0x0_0i + (int)(opc) * 6 + (int)(S))

/* ThumbExpandImm_C: returns value, *rot = 1 when carry comes from bit31 */
static u32 thumb_imm(u32 imm12, int *rot)
{
    u32 b = imm12 & 0xFF;
    if (!(imm12 & 0xC00)) {
        *rot = 0;
        switch ((imm12 >> 8) & 3) {
        case 0: return b;
        case 1: return b | (b << 16);
        case 2: return (b << 8) | (b << 24);
        default: return b | (b << 8) | (b << 16) | (b << 24);
        }
    }
    *rot = 1;
    u32 v = 0x80 | (imm12 & 0x7F), r = (imm12 >> 7) & 31;
    return (v >> r) | (v << (32 - r));
}

/* ------------------------------------------------------------------ */
/* 16-bit                                                              */
/* ------------------------------------------------------------------ */

static void t16(ctx_t *x, u32 h)
{
    di_t *d = x->d;
    u32 S = x->in_it ? 0 : 1;               /* outside IT, 16-bit data processing sets flags */
    u32 pc = x->pc;
    memset(d, 0, sizeof(*d));

    switch (h >> 11) {
    case 0x00: case 0x01: case 0x02: {      /* lsl/lsr/asr imm */
        u32 type = h >> 11, imm5 = (h >> 6) & 31, rm = (h >> 3) & 7, rd = h & 7;
        via_arm(x, AL | 0x01A00000u | S << 20 | rd << 12 | imm5 << 7 | type << 5 | rm);
        return; }
    case 0x03: {                            /* add/sub reg / imm3 */
        u32 I = (h >> 10) & 1, sub = (h >> 9) & 1, rm = (h >> 6) & 7, rn = (h >> 3) & 7, rd = h & 7;
        u32 opc = sub ? 2 : 4;
        via_arm(x, AL | (I << 25) | opc << 21 | S << 20 | rn << 16 | rd << 12 | rm);
        return; }
    case 0x04: case 0x05: case 0x06: case 0x07: {   /* mov/cmp/add/sub imm8 */
        u32 op = (h >> 11) & 3, rd = (h >> 8) & 7, imm = h & 0xFF;
        static const u32 opc[4] = { 0xD, 0xA, 0x4, 0x2 };
        u32 s = op == 1 ? 1 : S;
        via_arm(x, AL | 1u << 25 | opc[op] << 21 | s << 20 | (op == 0 ? 0 : rd << 16) | (op == 1 ? 0 : rd << 12) | imm);
        return; }
    case 0x08:
        if (!(h & 0x400)) {                 /* data processing, register */
            u32 op = (h >> 6) & 15, rm = (h >> 3) & 7, rdn = h & 7;
            switch (op) {
            case 0x0: via_arm(x, AL | 0x0u << 21 | S << 20 | rdn << 16 | rdn << 12 | rm); return;   /* and */
            case 0x1: via_arm(x, AL | 0x1u << 21 | S << 20 | rdn << 16 | rdn << 12 | rm); return;   /* eor */
            case 0x2: case 0x3: case 0x4: case 0x7: {                                             /* lsl lsr asr ror (reg) */
                static const u32 ty[8] = { 0, 0, 0, 1, 2, 0, 0, 3 };
                via_arm(x, AL | 0x01A00010u | S << 20 | rdn << 12 | rm << 8 | ty[op] << 5 | rdn);
                return; }
            case 0x5: via_arm(x, AL | 0x5u << 21 | S << 20 | rdn << 16 | rdn << 12 | rm); return;   /* adc */
            case 0x6: via_arm(x, AL | 0x6u << 21 | S << 20 | rdn << 16 | rdn << 12 | rm); return;   /* sbc */
            case 0x8: via_arm(x, AL | 0x8u << 21 | 1u << 20 | rdn << 16 | rm); return;               /* tst */
            case 0x9: via_arm(x, AL | 1u << 25 | 0x3u << 21 | S << 20 | rm << 16 | rdn << 12); return; /* rsb #0 (neg) */
            case 0xA: via_arm(x, AL | 0xAu << 21 | 1u << 20 | rdn << 16 | rm); return;               /* cmp */
            case 0xB: via_arm(x, AL | 0xBu << 21 | 1u << 20 | rdn << 16 | rm); return;               /* cmn */
            case 0xC: via_arm(x, AL | 0xCu << 21 | S << 20 | rdn << 16 | rdn << 12 | rm); return;   /* orr */
            case 0xD: via_arm(x, AL | 0x00000090u | S << 20 | rdn << 16 | rdn << 8 | rm); return;    /* mul rdm = rn * rdm */
            case 0xE: via_arm(x, AL | 0xEu << 21 | S << 20 | rdn << 16 | rdn << 12 | rm); return;   /* bic */
            default:  via_arm(x, AL | 0xFu << 21 | S << 20 | rdn << 12 | rm); return;               /* mvn */
            }
        } else {                            /* special data / branch exchange */
            u32 op = (h >> 8) & 3, rm = (h >> 3) & 15, rdn = ((h >> 4) & 8) | (h & 7);
            switch (op) {
            case 0:                         /* add rdn, rm (no flags) */
                if (rdn == 15) {            /* add pc, rm: BranchWritePC */
                    if (rm == 15) { undef(x, h); return; }
                    d->rm = (u8)rm; d->a = pc + 4;
                    via_op(x, OP_t_add_pc);
                    return;
                }
                if (rm == 15) { d->rd = (u8)rdn; d->rm = (u8)rdn; d->a = pc + 4; via_op(x, OP_add_pc_reg); return; }
                via_arm(x, AL | 0x4u << 21 | rdn << 16 | rdn << 12 | rm);
                return;
            case 1:                         /* cmp hi */
                via_arm(x, AL | 0xAu << 21 | 1u << 20 | rdn << 16 | rm);
                return;
            case 2:                         /* mov hi */
                if (rdn == 15) { d->rm = (u8)rm; via_op(x, OP_t_bwpc); return; }
                if (rm == 15) { d->rd = (u8)rdn; d->a = pc + 4; via_op(x, OP_mov_const); return; }
                via_arm(x, AL | 0xDu << 21 | rdn << 12 | rm);
                return;
            default:                        /* bx / blx */
                if (rm == 15) { undef(x, h); return; }
                d->rm = (u8)rm;
                if (h & 0x80) { d->b = (pc + 2) | 1; via_op(x, OP_br_blx); }
                else via_op(x, OP_br_bx);
                return;
            }
        }
    case 0x09:                              /* ldr literal */
        d->rd = (u8)((h >> 8) & 7);
        d->a = ((pc + 4) & ~3u) + (h & 0xFF) * 4;
        via_op(x, OP_ls_ldr_lit);
        return;
    case 0x0A: case 0x0B: {                 /* load/store register offset */
        u32 op = (h >> 9) & 7, rm = (h >> 6) & 7, rn = (h >> 3) & 7, rt = h & 7;
        static const int ops[8] = { OP_ls_str_reg, OP_ls_strh_rr, OP_ls_strb_reg, OP_ls_ldrsb_rr,
                                    OP_ls_ldr_reg, OP_ls_ldrh_rr, OP_ls_ldrb_reg, OP_ls_ldrsh_rr };
        d->rd = (u8)rt; d->rn = (u8)rn; d->rm = (u8)rm;
        via_op(x, ops[op]);
        return; }
    case 0x0C: case 0x0D: case 0x0E: case 0x0F: case 0x10: case 0x11: {   /* str/ldr imm5 */
        u32 L = (h >> 11) & 1, imm5 = (h >> 6) & 31, rn = (h >> 3) & 7, rt = h & 7;
        u32 k = h >> 12;                    /* 6 word, 7 byte, 8 halfword */
        d->rd = (u8)rt; d->rn = (u8)rn;
        if (k == 6) { d->a = imm5 * 4; via_op(x, L ? OP_ls_ldr : OP_ls_str); }
        else if (k == 7) { d->a = imm5; via_op(x, L ? OP_ls_ldrb : OP_ls_strb); }
        else { d->a = imm5 * 2; via_op(x, L ? OP_ls_ldrh : OP_ls_strh); }
        return; }
    case 0x12: case 0x13:                   /* str/ldr sp-relative */
        d->rd = (u8)((h >> 8) & 7); d->rn = 13; d->a = (h & 0xFF) * 4;
        via_op(x, (h & 0x800) ? OP_ls_ldr : OP_ls_str);
        return;
    case 0x14:                              /* adr */
        d->rd = (u8)((h >> 8) & 7); d->a = ((pc + 4) & ~3u) + (h & 0xFF) * 4;
        via_op(x, OP_mov_const);
        return;
    case 0x15:                              /* add rd, sp, #imm */
        d->rd = (u8)((h >> 8) & 7); d->rn = 13; d->a = (h & 0xFF) * 4;
        via_op(x, DP_OP(4, 0));
        return;
    case 0x16: case 0x17: {                 /* miscellaneous */
        u32 op = (h >> 5) & 0x7F;
        if ((op & 0x7C) == 0x00) {          /* add/sub sp, sp, #imm7 */
            d->rd = 13; d->rn = 13; d->a = (h & 0x7F) * 4;
            via_op(x, DP_OP((h & 0x80) ? 2 : 4, 0));
            return;
        }
        if ((h & 0x500) == 0x100) {         /* cbz / cbnz */
            d->rn = (u8)(h & 7);
            d->a = pc + 4 + ((((h >> 9) & 1) << 6) | (((h >> 3) & 31) << 1));
            via_op(x, (h & 0x800) ? OP_t_cbnz : OP_t_cbz);
            return;
        }
        if ((op & 0x78) == 0x10) {          /* sxth sxtb uxth uxtb */
            u32 rm = (h >> 3) & 7, rd = h & 7;
            static const u32 o1[4] = { 0x06BF0070u, 0x06AF0070u, 0x06FF0070u, 0x06EF0070u };
            via_arm(x, AL | o1[(h >> 6) & 3] | rd << 12 | rm);
            return;
        }
        if ((op & 0x70) == 0x20) {          /* push */
            u32 list = (h & 0xFF) | ((h & 0x100) ? 0x4000 : 0);
            via_arm(x, AL | 0x092D0000u | list);
            return;
        }
        if ((op & 0x70) == 0x60) {          /* pop */
            u32 list = (h & 0xFF) | ((h & 0x100) ? 0x8000 : 0);
            via_arm(x, AL | 0x08BD0000u | list);
            return;
        }
        if ((op & 0x78) == 0x50) {          /* rev rev16 (hlt) revsh */
            u32 rm = (h >> 3) & 7, rd = h & 7, k = (h >> 6) & 3;
            static const u32 o[4] = { 0x06BF0F30u, 0x06BF0FB0u, 0, 0x06FF0FB0u };
            if (k == 2) { undef(x, h); return; }
            via_arm(x, AL | o[k] | rd << 12 | rm);
            return;
        }
        if ((op & 0x7E) == 0x32) { via_op(x, OP_nop); return; }   /* setend / cps */
        if ((op & 0x78) == 0x70) {          /* bkpt */
            via_arm(x, AL | 0x01200070u | (h & 0xF));
            return;
        }
        if ((op & 0x78) == 0x78) {          /* it / hints */
            u32 mask = h & 0xF, first = (h >> 4) & 0xF;
            if (!mask) { via_op(x, OP_nop); return; }
            u32 n = 4 - (u32)__builtin_ctz(mask), a = pc + 2;
            for (u32 i = 0; i < n; i++) {
                u32 cbit = i == 0 ? (first & 1) : (mask >> (4 - i)) & 1;
                u32 cond = first == 0xE ? 0xE : ((first & 0xE) | cbit);
                if (a - it_lo < it_n * 2) itmap[(a - it_lo) >> 1] = (u8)(0x10 | cond);
                a += tlen(ld16(a));
            }
            via_op(x, OP_nop);
            return;
        }
        undef(x, h);
        return; }
    case 0x18: case 0x19: {                 /* stm / ldm (writeback unless ldm with rn in list) */
        u32 L = (h >> 11) & 1, rn = (h >> 8) & 7, list = h & 0xFF;
        u32 W = !(L && (list & (1u << rn)));
        via_arm(x, AL | 0x08800000u | W << 21 | L << 20 | rn << 16 | list);
        return; }
    case 0x1A: case 0x1B: {                 /* b<cond> / udf / svc */
        u32 cond = (h >> 8) & 15;
        if (cond == 0xF) { via_arm(x, AL | 0x0F000000u | (h & 0xFF)); return; }
        if (cond == 0xE) { undef(x, h); return; }
        d->a = pc + 4 + (u32)((s32)(s8)(h & 0xFF) * 2);
        x->cond = (u8)cond;
        via_op(x, OP_br_b);
        return; }
    case 0x1C:                              /* b */
        d->a = pc + 4 + (u32)(((s32)(h << 21)) >> 20);
        via_op(x, OP_br_b);
        return;
    }
    undef(x, h);
}

/* ------------------------------------------------------------------ */
/* 32-bit                                                              */
/* ------------------------------------------------------------------ */

static void t32_ls_single(ctx_t *x, u32 h1, u32 h2)
{
    di_t *d = x->d;
    u32 Sg = (h1 >> 8) & 1, A = (h1 >> 7) & 1, sz = (h1 >> 5) & 3, L = (h1 >> 4) & 1, rn = h1 & 15;
    u32 rt = h2 >> 12, pc = x->pc;
    if (sz == 3 || (Sg && !L) || (Sg && sz == 2)) { undef(x, h1 << 16 | h2); return; }
    d->rd = (u8)rt; d->rn = (u8)rn;
    if (L && rt == 15 && sz != 2) { via_op(x, OP_nop); return; }   /* pld / pli */
    if (rn == 15) {                         /* literal; bit7 is U here */
        if (!L || rt == 15) { undef(x, h1 << 16 | h2); return; }
        u32 base = (pc + 4) & ~3u, off = h2 & 0xFFF;
        d->a = A ? base + off : base - off;
        if (sz == 2) { via_op(x, OP_ls_ldr_lit); return; }
        d->b = (Sg << 1) | (sz & 1);
        via_op(x, OP_t_ld_lit);
        return;
    }
    int kind;                               /* 0 w 1 b 2 h 3 sb 4 sh */
    if (sz == 2) kind = 0; else if (sz == 0) kind = Sg ? 3 : 1; else kind = Sg ? 4 : 2;
    u32 P = 1, U = 1, W = 0, off = 0;
    bool reg = false;
    if (A) off = h2 & 0xFFF;
    else if (h2 & 0x800) {
        P = (h2 >> 10) & 1; U = (h2 >> 9) & 1; W = (h2 >> 8) & 1; off = h2 & 0xFF;
        if (!P && !W) { undef(x, h1 << 16 | h2); return; }
    } else if (!(h2 & 0x7C0)) reg = true;
    else { undef(x, h1 << 16 | h2); return; }
    if (reg) {
        u32 rm = h2 & 15, sh = (h2 >> 4) & 3;
        if (kind <= 1 || rt == 15) {        /* word/byte (and ldr pc): ARM register form */
            if (rt == 15 && (kind != 0 || !L)) { undef(x, h1 << 16 | h2); return; }
            via_arm(x, AL | 0x07800000u | (kind == 1) << 22 | L << 20 | rn << 16 | rt << 12 | sh << 7 | rm);
            return;
        }
        if (!L && kind != 2) { undef(x, h1 << 16 | h2); return; }
        d->rm = (u8)rm;
        d->b = sh | (u32)(L ? kind - 1 : 0) << 4;          /* h->1 sb->2 sh->3, strh 0 */
        via_op(x, OP_t_ls_hreg);
        return;
    }
    if (rt == 15) {                         /* ldr pc, [rn, ...]: ARM form (LoadWritePC interworks) */
        if (kind != 0 || !L) { undef(x, h1 << 16 | h2); return; }
        via_arm(x, AL | 0x04100000u | P << 24 | U << 23 | W << 21 | rn << 16 | 15u << 12 | off);
        return;
    }
    bool pre = P && W, post = !P;
    if ((pre || post) && rn == rt && kind <= 1) {   /* writeback onto the data register: ARM generic */
        via_arm(x, AL | 0x04000000u | P << 24 | U << 23 | (kind == 1) << 22 | W << 21 | L << 20 | rn << 16 | rt << 12 | off);
        return;
    }
    d->a = U ? off : (u32)-off;
    int op;
    switch (kind) {
    case 0: op = L ? (pre ? OP_ls_ldr_pre : post ? OP_ls_ldr_post : OP_ls_ldr) : (pre ? OP_ls_str_pre : post ? OP_ls_str_post : OP_ls_str); break;
    case 1: op = L ? (pre ? OP_ls_ldrb_pre : post ? OP_ls_ldrb_post : OP_ls_ldrb) : (pre ? OP_ls_strb_pre : post ? OP_ls_strb_post : OP_ls_strb); break;
    case 2: op = L ? (pre ? OP_ls_ldrh_pre : post ? OP_ls_ldrh_post : OP_ls_ldrh) : (pre ? OP_ls_strh_pre : post ? OP_ls_strh_post : OP_ls_strh); break;
    case 3: op = pre ? OP_ls_ldrsb_pre : post ? OP_ls_ldrsb_post : OP_ls_ldrsb; break;
    default: op = pre ? OP_ls_ldrsh_pre : post ? OP_ls_ldrsh_post : OP_ls_ldrsh; break;
    }
    via_op(x, op);
}

static void t32_dp_modimm(ctx_t *x, u32 h1, u32 h2)
{
    di_t *d = x->d;
    u32 op = (h1 >> 5) & 15, S = (h1 >> 4) & 1, rn = h1 & 15, rd = (h2 >> 8) & 15;
    u32 imm12 = ((h1 & 0x400) << 1) | ((h2 >> 4) & 0x700) | (h2 & 0xFF);
    int rot;
    u32 v = thumb_imm(imm12, &rot);
    d->rn = (u8)rn; d->rd = (u8)rd; d->a = v; d->b = (u32)rot;
    int opc;
    switch (op) {
    case 0x0: opc = rd == 15 ? 0x8 : 0x0; break;           /* and / tst */
    case 0x1: opc = 0xE; break;                            /* bic */
    case 0x2: opc = rn == 15 ? 0xD : 0xC; break;           /* orr / mov */
    case 0x3:                                              /* orn / mvn */
        if (rn == 15) { opc = 0xF; break; }
        d->b = S | (rot ? 4 : 0);
        via_op(x, OP_t_orn);
        return;
    case 0x4: opc = rd == 15 ? 0x9 : 0x1; break;           /* eor / teq */
    case 0x8: opc = rd == 15 ? 0xB : 0x4; break;           /* add / cmn */
    case 0xA: opc = 0x5; break;
    case 0xB: opc = 0x6; break;
    case 0xD: opc = rd == 15 ? 0xA : 0x2; break;           /* sub / cmp */
    case 0xE: opc = 0x3; break;                            /* rsb */
    default: undef(x, h1 << 16 | h2); return;
    }
    if (opc >= 8 && opc <= 11) S = 1;
    via_op(x, DP_OP(opc, S));
}

static void t32_dp_plain(ctx_t *x, u32 h1, u32 h2)
{
    di_t *d = x->d;
    u32 op = (h1 >> 4) & 0x1F, rn = h1 & 15, rd = (h2 >> 8) & 15;
    u32 imm12 = ((h1 & 0x400) << 1) | ((h2 >> 4) & 0x700) | (h2 & 0xFF);
    u32 imm3_2 = ((h2 >> 10) & 0x1C) | ((h2 >> 6) & 3);    /* imm3:imm2 */
    u32 pc = x->pc;
    d->rd = (u8)rd; d->rn = (u8)rn;
    switch (op) {
    case 0x00: case 0x0A:                                  /* addw / subw (rn=15: adr) */
        if (rn == 15) { u32 b = (pc + 4) & ~3u; d->a = op ? b - imm12 : b + imm12; via_op(x, OP_mov_const); return; }
        d->a = imm12;
        via_op(x, DP_OP(op ? 2 : 4, 0));
        return;
    case 0x04:                                             /* movw */
        d->a = imm12 | ((h1 & 15) << 12);
        via_op(x, OP_mov_const);
        return;
    case 0x0C:                                             /* movt */
        via_arm(x, AL | 0x03400000u | (h1 & 15) << 16 | rd << 12 | imm12);
        return;
    case 0x10: case 0x12: {                                /* ssat (sh) / ssat16 */
        u32 sh = (h1 >> 5) & 1, sat = h2 & 31;
        if (op == 0x12 && !imm3_2) { via_arm(x, AL | 0x06A00F30u | (sat & 15) << 16 | rd << 12 | rn); return; }
        via_arm(x, AL | 0x06A00010u | sat << 16 | rd << 12 | imm3_2 << 7 | sh << 6 | rn);
        return; }
    case 0x18: case 0x1A: {                                /* usat / usat16 */
        u32 sh = (h1 >> 5) & 1, sat = h2 & 31;
        if (op == 0x1A && !imm3_2) { via_arm(x, AL | 0x06E00F30u | (sat & 15) << 16 | rd << 12 | rn); return; }
        via_arm(x, AL | 0x06E00010u | sat << 16 | rd << 12 | imm3_2 << 7 | sh << 6 | rn);
        return; }
    case 0x14:                                             /* sbfx */
        via_arm(x, AL | 0x07A00050u | (h2 & 31) << 16 | rd << 12 | imm3_2 << 7 | rn);
        return;
    case 0x1C:                                             /* ubfx */
        via_arm(x, AL | 0x07E00050u | (h2 & 31) << 16 | rd << 12 | imm3_2 << 7 | rn);
        return;
    case 0x16:                                             /* bfi / bfc (rn=15) */
        via_arm(x, AL | 0x07C00010u | (h2 & 31) << 16 | rd << 12 | imm3_2 << 7 | rn);
        return;
    }
    undef(x, h1 << 16 | h2);
}

static void t32_branch_misc(ctx_t *x, u32 h1, u32 h2)
{
    di_t *d = x->d;
    u32 op1 = (h2 >> 12) & 7, pc = x->pc;
    u32 S = (h1 >> 10) & 1, J1 = (h2 >> 13) & 1, J2 = (h2 >> 11) & 1;
    if (!(op1 & 5)) {                                      /* 0x0: conditional branch or misc */
        if (((h1 >> 7) & 7) != 7) {                        /* b<cond>.w */
            u32 cond = (h1 >> 6) & 15;
            s32 off = (s32)((S << 20) | (J2 << 19) | (J1 << 18) | ((h1 & 0x3F) << 12) | ((h2 & 0x7FF) << 1));
            off = (s32)((u32)off << 11) >> 11;
            d->a = pc + 4 + (u32)off;
            x->cond = (u8)cond;
            via_op(x, OP_br_b);
            return;
        }
        u32 op = (h1 >> 4) & 0x7F;
        if ((op & 0x7E) == 0x38) {                         /* msr (reg) */
            via_arm(x, AL | 0x0120F000u | ((h1 >> 4) & 1) << 22 | ((h2 >> 8) & 15) << 16 | (h1 & 15));
            return;
        }
        if (op == 0x3A) { via_op(x, OP_nop); return; }     /* hints, cps */
        if (op == 0x3B) { via_arm(x, 0xF57FF000u | (h2 & 0xFF)); return; }   /* clrex dsb dmb isb */
        if ((op & 0x7E) == 0x3E) {                         /* mrs */
            via_arm(x, AL | 0x010F0000u | ((h1 >> 4) & 1) << 22 | ((h2 >> 8) & 15) << 12);
            return;
        }
        undef(x, h1 << 16 | h2);
        return;
    }
    u32 I1 = !(J1 ^ S), I2 = !(J2 ^ S);
    if (op1 & 1) {                                         /* b.w / bl */
        s32 off = (s32)((S << 24) | (I1 << 23) | (I2 << 22) | ((h1 & 0x3FF) << 12) | ((h2 & 0x7FF) << 1));
        off = (s32)((u32)off << 7) >> 7;
        d->a = pc + 4 + (u32)off;
        if (op1 & 4) { d->b = (pc + 4) | 1; via_op(x, OP_br_bl); }
        else via_op(x, OP_br_b);
        return;
    }
    if (op1 & 4) {                                         /* blx imm -> ARM */
        if (h2 & 1) { undef(x, h1 << 16 | h2); return; }
        s32 off = (s32)((S << 24) | (I1 << 23) | (I2 << 22) | ((h1 & 0x3FF) << 12) | ((h2 & 0x7FE) << 1));
        off = (s32)((u32)off << 7) >> 7;
        d->a = ((pc + 4) & ~3u) + (u32)off;
        d->b = (pc + 4) | 1;
        via_op(x, OP_t_blxi);
        return;
    }
    undef(x, h1 << 16 | h2);
}

static void t32_ldst_multi_dual(ctx_t *x, u32 h1, u32 h2)
{
    di_t *d = x->d;
    u32 rn = h1 & 15, pc = x->pc;
    if (!(h1 & 0x40)) {                                    /* load/store multiple */
        u32 op = (h1 >> 7) & 3, W = (h1 >> 5) & 1, L = (h1 >> 4) & 1;
        if (op == 0 || op == 3) { undef(x, h1 << 16 | h2); return; }   /* srs / rfe */
        u32 PU = op == 1 ? 0x00800000u : 0x01000000u;       /* ia / db */
        via_arm(x, AL | 0x08000000u | PU | W << 21 | L << 20 | rn << 16 | (h2 & 0xFFFF));
        return;
    }
    u32 op1 = (h1 >> 7) & 3, op2 = (h1 >> 4) & 3, op3 = (h2 >> 4) & 15;
    u32 rt = h2 >> 12, rt2 = (h2 >> 8) & 15;
    if (op1 == 0 && op2 == 0) {                            /* strex rd, rt, [rn, #imm] */
        d->rn = (u8)rn; d->rd = (u8)rt2; d->rm = (u8)rt; d->a = (h2 & 0xFF) * 4; d->b = 0x8;
        via_op(x, OP_t_excl);
        return;
    }
    if (op1 == 0 && op2 == 1) {                            /* ldrex rt, [rn, #imm] */
        d->rn = (u8)rn; d->rd = (u8)rt; d->a = (h2 & 0xFF) * 4; d->b = 0x9;
        via_op(x, OP_t_excl);
        return;
    }
    if (op1 == 1 && op2 == 0) {                            /* strexb/h/d: status = h2[3:0] */
        d->rn = (u8)rn; d->rd = (u8)(h2 & 15); d->rm = (u8)rt;
        if (op3 == 4) d->b = 0xC; else if (op3 == 5) d->b = 0xE;
        else if (op3 == 7 && rt2 == rt + 1) d->b = 0xA;
        else { undef(x, h1 << 16 | h2); return; }
        via_op(x, OP_t_excl);
        return;
    }
    if (op1 == 1 && op2 == 1) {
        if (op3 == 0 || op3 == 1) {                        /* tbb / tbh */
            d->rn = (u8)rn; d->rm = (u8)(h2 & 15); d->a = pc + 4; d->b = op3;
            via_op(x, OP_t_tbb);
            return;
        }
        d->rn = (u8)rn; d->rd = (u8)rt;
        if (op3 == 4) d->b = 0xD; else if (op3 == 5) d->b = 0xF;
        else if (op3 == 7 && rt2 == rt + 1) d->b = 0xB;
        else { undef(x, h1 << 16 | h2); return; }
        via_op(x, OP_t_excl);
        return;
    }
    /* ldrd / strd imm (P U 1 W L), literal when rn = 15 */
    u32 P = (h1 >> 8) & 1, U = (h1 >> 7) & 1, W = (h1 >> 5) & 1, L = (h1 >> 4) & 1;
    u32 off = (h2 & 0xFF) * 4;
    d->rd = (u8)rt; d->rm = (u8)rt2; d->rn = (u8)rn;
    if (rn == 15) {
        if (!L) { undef(x, h1 << 16 | h2); return; }
        u32 b = (pc + 4) & ~3u;
        d->a = U ? b + off : b - off; d->b = 3;
        via_op(x, OP_t_ldrd);
        return;
    }
    d->a = U ? off : (u32)-off;
    d->b = !P ? 2 : W ? 1 : 0;
    via_op(x, L ? OP_t_ldrd : OP_t_strd);
}

static void t32_dp_shifted(ctx_t *x, u32 h1, u32 h2)
{
    di_t *d = x->d;
    u32 op = (h1 >> 5) & 15, S = (h1 >> 4) & 1, rn = h1 & 15, rd = (h2 >> 8) & 15, rm = h2 & 15;
    u32 imm5 = ((h2 >> 10) & 0x1C) | ((h2 >> 6) & 3), type = (h2 >> 4) & 3;
    u32 sh = imm5 << 7 | type << 5 | rm;
    int opc;
    switch (op) {
    case 0x0: opc = rd == 15 ? 0x8 : 0x0; break;
    case 0x1: opc = 0xE; break;
    case 0x2: opc = rn == 15 ? 0xD : 0xC; break;
    case 0x3:
        if (rn == 15) { opc = 0xF; break; }
        d->rn = (u8)rn; d->rd = (u8)rd; d->rm = (u8)rm;
        d->b = S | 2 | (type | imm5 << 2) << 3;
        via_op(x, OP_t_orn);
        return;
    case 0x4: opc = rd == 15 ? 0x9 : 0x1; break;
    case 0x6:                                              /* pkhbt / pkhtb */
        via_arm(x, AL | 0x06800010u | rn << 16 | rd << 12 | imm5 << 7 | (type & 2) << 5 | rm);
        return;
    case 0x8: opc = rd == 15 ? 0xB : 0x4; break;
    case 0xA: opc = 0x5; break;
    case 0xB: opc = 0x6; break;
    case 0xD: opc = rd == 15 ? 0xA : 0x2; break;
    case 0xE: opc = 0x3; break;
    default: undef(x, h1 << 16 | h2); return;
    }
    if (opc >= 8 && opc <= 11) { S = 1; rd = 0; }
    if (opc == 0xD || opc == 0xF) rn = 0;
    via_arm(x, AL | (u32)opc << 21 | S << 20 | rn << 16 | rd << 12 | sh);
}

static void t32_dp_reg(ctx_t *x, u32 h1, u32 h2)
{
    u32 op1 = (h1 >> 4) & 15, op2 = (h2 >> 4) & 15, rn = h1 & 15, rd = (h2 >> 8) & 15, rm = h2 & 15;
    if ((op1 & 8) == 0 && op2 == 0) {                      /* lsl lsr asr ror (register) */
        u32 type = (op1 >> 1) & 3, S = op1 & 1;
        via_arm(x, AL | 0x01A00010u | S << 20 | rd << 12 | rm << 8 | type << 5 | rn);
        return;
    }
    if ((op1 & 8) == 0 && (op2 & 8)) {                     /* extends (rn = 15: no add) */
        static const u32 o[6] = { 0x06B00070u, 0x06F00070u, 0x06800070u, 0x06C00070u, 0x06A00070u, 0x06E00070u };
        if (op1 > 5) { undef(x, h1 << 16 | h2); return; }
        via_arm(x, AL | o[op1] | rn << 16 | rd << 12 | ((h2 >> 4) & 3) << 10 | rm);
        return;
    }
    if ((op1 & 8) && !(op2 & 8)) {                         /* parallel add/sub */
        static const int map[8] = { 4, 0, 1, -1, 7, 3, 2, -1 };   /* thumb op1[2:0] -> arm op2 */
        int aop2 = map[op1 & 7];
        u32 U = (op2 >> 2) & 1, kind = (op2 & 3) + 1;            /* 00 plain 01 sat 10 halving */
        if (aop2 < 0 || kind > 3) { undef(x, h1 << 16 | h2); return; }
        via_arm(x, AL | 0x06000F10u | (U << 2 | kind) << 20 | rn << 16 | rd << 12 | (u32)aop2 << 5 | rm);
        return;
    }
    if ((op1 & 0xC) == 8 && (op2 & 0xC) == 8) {            /* misc */
        u32 a = op1 & 3, b = op2 & 3;
        switch (a) {
        case 0: via_arm(x, AL | 0x01000050u | b << 21 | rn << 16 | rd << 12 | rm); return;   /* qadd qdadd qsub qdsub */
        case 1: {
            static const u32 o[4] = { 0x06BF0F30u, 0x06BF0FB0u, 0x06FF0F30u, 0x06FF0FB0u };   /* rev rev16 rbit revsh */
            via_arm(x, AL | o[b] | rd << 12 | rm); return; }
        case 2: if (b) break; via_arm(x, AL | 0x06800FB0u | rn << 16 | rd << 12 | rm); return;   /* sel */
        default: if (b) break; via_arm(x, AL | 0x016F0F10u | rd << 12 | rm); return;            /* clz */
        }
    }
    undef(x, h1 << 16 | h2);
}

static void t32_mul(ctx_t *x, u32 h1, u32 h2)
{
    u32 op1 = (h1 >> 4) & 7, op2 = (h2 >> 4) & 3, rn = h1 & 15, ra = h2 >> 12, rd = (h2 >> 8) & 15, rm = h2 & 15;
    u32 N = (h2 >> 5) & 1, M = (h2 >> 4) & 1;
    switch (op1) {
    case 0:
        if (op2 == 0) { via_arm(x, AL | (ra == 15 ? 0x00000090u : 0x00200090u | ra << 12) | rd << 16 | rm << 8 | rn); return; }
        if (op2 == 1) { via_arm(x, AL | 0x00600090u | rd << 16 | ra << 12 | rm << 8 | rn); return; }
        break;
    case 1:                                                /* smla<x><y> / smul<x><y> */
        if (ra == 15) via_arm(x, AL | 0x01600080u | rd << 16 | rm << 8 | M << 6 | N << 5 | rn);
        else via_arm(x, AL | 0x01000080u | rd << 16 | ra << 12 | rm << 8 | M << 6 | N << 5 | rn);
        return;
    case 2: case 4:                                        /* smlad smuad / smlsd smusd */
        if (op2 > 1) break;
        via_arm(x, AL | 0x07000010u | rd << 16 | ra << 12 | rm << 8 | (op1 == 4) << 6 | M << 5 | rn);
        return;
    case 3:                                                /* smlaw<y> / smulw<y> */
        if (op2 > 1) break;
        via_arm(x, AL | 0x01200080u | rd << 16 | (ra == 15 ? 0 : ra << 12) | rm << 8 | M << 6 | (ra == 15) << 5 | rn);
        return;
    case 5: case 6:                                        /* smmla smmul / smmls (R = bit4) */
        if (op2 > 1) break;
        via_arm(x, AL | 0x07500010u | rd << 16 | ra << 12 | rm << 8 | (op1 == 6 ? 0xC0u : 0) | M << 5 | rn);
        return;
    default:                                               /* usad8 / usada8 */
        if (op2) break;
        via_arm(x, AL | 0x07800010u | rd << 16 | ra << 12 | rm << 8 | rn);
        return;
    }
    undef(x, h1 << 16 | h2);
}

static void t32_mull(ctx_t *x, u32 h1, u32 h2)
{
    u32 op1 = (h1 >> 4) & 7, op2 = (h2 >> 4) & 15, rn = h1 & 15, lo = h2 >> 12, hi = (h2 >> 8) & 15, rm = h2 & 15;
    switch (op1) {
    case 0: if (!op2) { via_arm(x, AL | 0x00C00090u | hi << 16 | lo << 12 | rm << 8 | rn); return; } break;   /* smull */
    case 1: if (op2 == 15) { via_arm(x, AL | 0x0710F010u | hi << 16 | rm << 8 | rn); return; } break;       /* sdiv */
    case 2: if (!op2) { via_arm(x, AL | 0x00800090u | hi << 16 | lo << 12 | rm << 8 | rn); return; } break;   /* umull */
    case 3: if (op2 == 15) { via_arm(x, AL | 0x0730F010u | hi << 16 | rm << 8 | rn); return; } break;       /* udiv */
    case 4:
        if (!op2) { via_arm(x, AL | 0x00E00090u | hi << 16 | lo << 12 | rm << 8 | rn); return; }                /* smlal */
        if ((op2 & 0xC) == 8) {                                                                                /* smlal<x><y> */
            via_arm(x, AL | 0x01400080u | hi << 16 | lo << 12 | rm << 8 | (op2 & 1) << 6 | ((op2 >> 1) & 1) << 5 | rn);
            return;
        }
        if ((op2 & 0xE) == 0xC) { via_arm(x, AL | 0x07400010u | hi << 16 | lo << 12 | rm << 8 | (op2 & 1) << 5 | rn); return; }   /* smlald */
        break;
    case 5: if ((op2 & 0xE) == 0xC) { via_arm(x, AL | 0x07400050u | hi << 16 | lo << 12 | rm << 8 | (op2 & 1) << 5 | rn); return; } break;  /* smlsld */
    case 6:
        if (!op2) { via_arm(x, AL | 0x00A00090u | hi << 16 | lo << 12 | rm << 8 | rn); return; }                /* umlal */
        if (op2 == 6) { via_arm(x, AL | 0x00400090u | hi << 16 | lo << 12 | rm << 8 | rn); return; }            /* umaal */
        break;
    }
    undef(x, h1 << 16 | h2);
}

static void t32_coproc(ctx_t *x, u32 h1, u32 h2)
{
    di_t *d = x->d;
    u32 w = h1 << 16 | h2, pc = x->pc;
    if ((h1 & 0xEF00) == 0xEF00) {                         /* neon data processing: 111U 1111 -> 1111 001U */
        via_arm(x, 0xF2000000u | ((h1 >> 12) & 1) << 24 | (w & 0x00FFFFFFu));
        return;
    }
    if (h1 & 0x1000) { undef(x, w); return; }              /* mcr2 / cdp2 etc. */
    u32 cp = (h2 >> 8) & 15;
    if ((h1 & 0xFF3F) == 0xED1F && (cp == 10 || cp == 11)) {   /* vldr literal */
        u32 U = (h1 >> 7) & 1, D = (h1 >> 6) & 1, vd = (h2 >> 12) & 15, off = (h2 & 0xFF) * 4;
        u32 b = (pc + 4) & ~3u;
        d->a = U ? b + off : b - off;
        d->rd = (u8)(cp == 10 ? (vd << 1 | D) : (D << 4 | vd));
        via_op(x, cp == 10 ? OP_vldr_s_lit : OP_vldr_d_lit);
        return;
    }
    via_arm(x, (w & 0x0FFFFFFFu) | AL);                    /* same bits as ARM with cond = AL */
}

static void t32(ctx_t *x, u32 h1, u32 h2)
{
    memset(x->d, 0, sizeof(*x->d));
    u32 op1 = (h1 >> 11) & 3, op2 = (h1 >> 4) & 0x7F, op = h2 >> 15;
    if (op1 == 1) {
        if ((op2 & 0x60) == 0x00) { t32_ldst_multi_dual(x, h1, h2); return; }
        if ((op2 & 0x60) == 0x20) { t32_dp_shifted(x, h1, h2); return; }
        t32_coproc(x, h1, h2);
        return;
    }
    if (op1 == 2) {
        if (op) { t32_branch_misc(x, h1, h2); return; }
        if (op2 & 0x20) t32_dp_plain(x, h1, h2);
        else t32_dp_modimm(x, h1, h2);
        return;
    }
    if ((op2 & 0x71) == 0x10) {                            /* neon element/structure: 1111 1001 xxx0 -> 1111 0100 xxx0 */
        via_arm(x, 0xF4000000u | ((h1 << 16 | h2) & 0x00FFFFFFu));
        return;
    }
    if ((op2 & 0x60) == 0x00) { t32_ls_single(x, h1, h2); return; }
    if ((op2 & 0x70) == 0x20) { t32_dp_reg(x, h1, h2); return; }
    if ((op2 & 0x78) == 0x30) { t32_mul(x, h1, h2); return; }
    if ((op2 & 0x78) == 0x38) { t32_mull(x, h1, h2); return; }
    if (op2 & 0x40) { t32_coproc(x, h1, h2); return; }
    undef(x, h1 << 16 | h2);
}

void thumb_decode(di_t *d, u32 pc)
{
    if (!itmap || it_lo != G.text_lo || it_n != G.text_span / 2) thumb_it_reset();
    u32 h1 = ld16(pc);
    u8 it = pc - it_lo < it_n * 2 ? itmap[(pc - it_lo) >> 1] : 0;
    ctx_t x = { d, pc, (u8)(it ? it & 15 : 0xE), (u8)tlen((u16)h1), it != 0 };
    if (x.len == 2) t16(&x, h1);
    else t32(&x, h1, ld16(pc + 2));
    d->len = x.len;
}
