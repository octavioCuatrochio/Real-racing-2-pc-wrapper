/*
 * dec.c - pre-decoded instruction cache + specialised fast handlers.
 *
 * Every text word gets a di_t on first execution: its handler plus operands
 * extracted (and PC-relative values resolved) once. Hot forms of data
 * processing, load/store and branches get dedicated handlers; everything
 * else, and any form touching r15, falls back to the generic arm.c handler.
 * The generic handlers stay the reference: the selftest fuzzes both paths.
 */
#include "fastops.h"
#include <sys/mman.h>

di_t *g_icache;
static u32 icache_lo, icache_words;

/* condition pass table: bit (cpsr >> 28) set when the condition passes */
u16 cond_tab[16];
u16 condx_tab[16];

JITCALL void d_decode(cpu_t *c, const di_t *d);

static const dfn_t op_fns[OP_COUNT] = {
    [OP_decode] = d_decode, [OP_generic] = NULL, [OP_hook] = NULL,   /* set in decode_into */
#define X(n) [OP_##n] = n,
    FAST_OPS(X)
#undef X
};

void cpu_icache_reset(void)
{
    if (g_icache) munmap(g_icache, (size_t)icache_words * sizeof(di_t));
    icache_lo = G.text_lo;
    icache_words = G.text_span / 4;
    /* zero-filled and never pre-touched: op 0 = OP_decode, cx 0 = always */
    g_icache = mmap(NULL, (size_t)icache_words * sizeof(di_t), PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (g_icache == MAP_FAILED) fatal("icache alloc failed");
    for (u32 cc = 0; cc < 16; cc++) {
        u16 m = 0;
        for (u32 f = 0; f < 16; f++)
            if (cond_ok(cc, f << 28)) m |= (u16)(1u << f);
        cond_tab[cc] = m;
    }
    for (u32 cc = 0; cc < 16; cc++) condx_tab[cc ^ 0xE] = cond_tab[cc];
}

/* ------------------------------------------------------------------ */
/* generic fallback                                                    */
/* ------------------------------------------------------------------ */

int g_stats;
static u64 generic_hits[4096];
static u32 generic_example[4096];

JITCALL void d_generic(cpu_t *c, const di_t *d)
{
    if (__builtin_expect(g_stats, 0)) {
        u32 k = DEC_KEY(d->insn);
        generic_hits[k]++;
        generic_example[k] = d->insn;
    }
    dec_table[DEC_KEY(d->insn)](c, d->insn);
}

void stats_dump(void)
{
    if (!g_stats) return;
    u64 total = 0;
    for (int k = 0; k < 4096; k++) total += generic_hits[k];
    LOG("[stats] generic-path executions: %llu; top decode keys:\n", (unsigned long long)total);
    for (int n = 0; n < 24; n++) {
        int best = -1;
        for (int k = 0; k < 4096; k++)
            if (generic_hits[k] && (best < 0 || generic_hits[k] > generic_hits[best])) best = k;
        if (best < 0) break;
        LOG("  key %03x  %10llu  e.g. %08x\n", best, (unsigned long long)generic_hits[best], generic_example[best]);
        generic_hits[best] = 0;
    }
}

/* ------------------------------------------------------------------ */
/* data processing: 16 opcodes x S x {imm, reg, reg-shift-imm}         */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* decoder                                                             */
/* ------------------------------------------------------------------ */

/* debug code breakpoints: the hooked pc decodes to OP_hook (no cost elsewhere) */
static u32 hook_pcs[8];
static int nhooks;
void cpu_add_hook(u32 pc) { if (nhooks < 8) hook_pcs[nhooks++] = pc; }

JITCALL void d_hook(cpu_t *c, const di_t *d)
{
    u32 pc = c->r[15] - 4;
    LOG("[hook] %08x:", pc);
    for (int i = 0; i < 16; i++) LOG(" r%d=%08x", i, i == 15 ? pc : c->r[i]);
    const char *arm = getenv("RR2_HOOK_ARM");             /* "R:A+B": arm a read (R) / write (W) watch */
    extern long g_frame;
    const char *at = getenv("RR2_HOOK_ARM_AT");
    if (arm && strlen(arm) >= 5 && (!at || g_frame >= atol(at))) {
        extern void watch_arm(u32 addr, int read);
        int ra = atoi(arm + 2); const char *p = strchr(arm, '+') + 1;
        watch_arm(c->r[ra & 15] + (*p == '#' ? (u32)strtoul(p + 1, NULL, 16) : c->r[atoi(p) & 15]), arm[0] == 'R');
    }
    const char *snap = getenv("RR2_HOOK_SNAP");           /* "reg:off:len:prefix": raw dump every 25 frames */
    if (snap) {
        extern long g_frame;
        static long last = -1;
        unsigned reg, off, len; char pre[256];
        if (sscanf(snap, "%u:%x:%x:%255s", &reg, &off, &len, pre) == 4 && g_frame % 25 == 0 && g_frame != last) {
            last = g_frame;
            char fn[300]; snprintf(fn, sizeof(fn), "%s_%ld.bin", pre, g_frame);
            FILE *f = fopen(fn, "wb");
            if (f) { fwrite(g2h(c->r[reg & 15] + off), 1, len, f); fclose(f); }
        }
    }
    const char *dump = getenv("RR2_HOOK_DUMP");           /* "reg:offset", e.g. 0:a0 */
    if (dump) {
        int reg = atoi(dump); const char *colon = strchr(dump, ':');
        u32 at = c->r[reg & 15] + (colon ? (u32)strtoul(colon + 1, NULL, 16) : 0);
        LOG("  [r%d+..] %08x:", reg, at);
        for (int i = 0; i < 8; i++) LOG(" %08x", ld32(at + 4 * i));
    }
    LOG("\n");
    dec_table[DEC_KEY(d->insn)](c, d->insn);          /* then run the instruction itself */
}

static void decode_into(di_t *d, u32 pc, u32 insn)
{
    d->insn = insn;
    d->cond = (u8)(insn >> 28);
    d->rd = (insn >> 12) & 0xF;
    d->rn = (insn >> 16) & 0xF;
    d->rm = insn & 0xF;
    d->a = d->b = 0;
    int op = OP_generic;
    u32 cls = (insn >> 25) & 7;
    bool pcreg = d->rd == 15 || d->rn == 15;

    if (d->cond == 0xF) {
        if ((insn & 0xFD70F000u) == 0xF550F000u) op = OP_nop;     /* pld */
        goto done;
    }

    if (cls <= 1) {
        u32 b74 = (insn >> 4) & 0xF, opc = (insn >> 21) & 0xF, S = (insn >> 20) & 1;
        bool misc = !(insn & (1u << 25)) && ((b74 & 9) == 9 || ((insn >> 20) & 0x19) == 0x10);
        if (cls == 1 && ((insn >> 20) & 0x1B) == 0x12) goto done;          /* msr imm */
        if (cls == 0 && (b74 & 9) == 9 && (b74 & 6) && !pcreg) {           /* ldrh/strh/ldrsb/ldrsh */
            u32 P = (insn >> 24) & 1, U = (insn >> 23) & 1, I = (insn >> 22) & 1, W = (insn >> 21) & 1;
            u32 L = (insn >> 20) & 1, SH = (b74 >> 1) & 3;
            if (!L && SH != 1) {                                           /* ldrd (SH=2) / strd (SH=3) */
                u32 offd = ((insn >> 4) & 0xF0) | (insn & 0xF);
                if ((d->rd & 1) || d->rd == 14 || !P || W) goto done;
                if (I) { d->a = U ? offd : (u32)-offd; op = SH == 2 ? OP_ldrd_i : OP_strd_i; }
                else if (U && d->rm != 15) op = SH == 2 ? OP_ldrd_r : OP_strd_r;
                goto done;
            }
            int k = !L ? 0 : SH == 1 ? 1 : SH == 2 ? 2 : 3;                /* strh ldrh ldrsb ldrsh */
            u32 off = ((insn >> 4) & 0xF0) | (insn & 0xF);
            if (I) d->a = U ? off : (u32)-off;
            if (P && !W && I) {
                static const int o[4] = { OP_ls_strh, OP_ls_ldrh, OP_ls_ldrsb, OP_ls_ldrsh };
                op = o[k];
            } else if (P && !W && !I && U && d->rm != 15) {
                static const int o[4] = { OP_ls_strh_rr, OP_ls_ldrh_rr, OP_ls_ldrsb_rr, OP_ls_ldrsh_rr };
                op = o[k];
            } else if (I && d->rn != d->rd && P && W) {
                static const int o[4] = { OP_ls_strh_pre, OP_ls_ldrh_pre, OP_ls_ldrsb_pre, OP_ls_ldrsh_pre };
                op = o[k];
            } else if (I && d->rn != d->rd && !P && !W) {
                static const int o[4] = { OP_ls_strh_post, OP_ls_ldrh_post, OP_ls_ldrsb_post, OP_ls_ldrsh_post };
                op = o[k];
            }
            goto done;
        }
        if (cls == 0 && b74 == 9 && ((insn >> 22) & 0x3F) == 0 && !S) {   /* mul / mla */
            u32 rdm = (insn >> 16) & 0xF, rs = (insn >> 8) & 0xF, A = (insn >> 21) & 1;
            if (rdm != 15 && d->rm != 15 && rs != 15 && (!A || d->rd != 15)) {
                d->b = rs;
                op = A ? OP_mla_ : OP_mul_;
            }
            goto done;
        }
        if (cls == 0 && b74 == 9 && ((insn >> 23) & 0x1F) == 1 && !S) {   /* umull/umlal/smull/smlal */
            u32 hi = (insn >> 16) & 0xF, lo = (insn >> 12) & 0xF, rs = (insn >> 8) & 0xF;
            if (hi != 15 && lo != 15 && hi != lo && rs != 15 && d->rm != 15) {
                u32 Us = (insn >> 22) & 1, A = (insn >> 21) & 1;
                d->b = rs;
                op = Us ? (A ? OP_smlal_ : OP_smull_) : (A ? OP_umlal_ : OP_umull_);
            }
            goto done;
        }
        if (cls == 0 && (insn & 0x0FFF0FF0u) == 0x016F0F10u && d->rd != 15 && d->rm != 15) {
            op = OP_clz_;                                                 /* clz */
            goto done;
        }
        if (misc) goto done;
        if (!S && opc >= 8 && opc <= 11) goto done;
        bool reg0 = !(insn & (1u << 25)) && !(insn & 0xFF0);          /* plain register, LSL #0 */
        if (d->rd != 15 && !S && d->rn == 15 && (insn & (1u << 25))) {  /* rd = pc OP imm */
            u32 imm = insn & 0xFF, rot = ((insn >> 8) & 0xF) * 2, a = pc + 8;
            u32 b = rot ? (imm >> rot) | (imm << (32 - rot)) : imm, v;
            switch (opc) {
            case 0x0: v = a & b; break;  case 0x1: v = a ^ b; break;
            case 0x2: v = a - b; break;  case 0x3: v = b - a; break;
            case 0x4: v = a + b; break;  case 0xC: v = a | b; break;
            case 0xD: v = b; break;      case 0xE: v = a & ~b; break;
            case 0xF: v = ~b; break;
            default: goto done;                                          /* adc/sbc/rsc: need C */
            }
            d->a = v;
            op = OP_mov_const;
            goto done;
        }
        if (d->rd != 15 && !S && reg0 && opc == 0x4 && d->rn == 15 && d->rm != 15) {
            d->a = pc + 8;                                               /* add rd, pc, rm */
            op = OP_add_pc_reg;
            goto done;
        }
        if (d->rd != 15 && !S && reg0 && opc == 0xD && d->rm == 15) {  /* mov rd, pc */
            d->a = pc + 8;
            op = OP_mov_const;
            goto done;
        }
        if (d->rd == 15 && !S && reg0 && opc == 0xD && d->rm != 15) {  /* mov pc, rm */
            op = OP_br_bx;
            goto done;
        }
        if (pcreg) goto done;
        if (insn & (1u << 25)) {
            u32 imm = insn & 0xFF, rot = ((insn >> 8) & 0xF) * 2;
            d->a = rot ? (imm >> rot) | (imm << (32 - rot)) : imm;
            d->b = rot != 0;                         /* carry-out = bit31 when rotated */
            op = OP_dp_0x0_0i + (int)opc * 6 + (int)S;
        } else if (insn & (1u << 4)) {
            u32 rs = (insn >> 8) & 0xF;
            if (rs == 15 || d->rm == 15) goto done;
            d->a = rs;
            d->b = (insn >> 5) & 3;
            op = OP_dp_0x0_0x + (int)opc * 2 + (int)S;
        } else {
            if (d->rm == 15) goto done;
            u32 type = (insn >> 5) & 3, sh = (insn >> 7) & 0x1F;
            if (type == 0 && sh == 0) op = OP_dp_0x0_0r + (int)opc * 6 + (int)S;
            else { d->b = type | (sh << 2); op = OP_dp_0x0_0s + (int)opc * 6 + (int)S; }
        }
        goto done;
    }
    if (cls == 2 || cls == 3) {                      /* ldr/str word/byte */
        u32 P = (insn >> 24) & 1, U = (insn >> 23) & 1, B = (insn >> 22) & 1, W = (insn >> 21) & 1;
        u32 L = (insn >> 20) & 1;
        if (d->rd == 15) {                           /* loads into pc: vtable calls, plt stubs */
            if (cls == 2 && L && !B && d->rn != 15 && P) {
                u32 off = insn & 0xFFF;
                d->a = U ? off : (u32)-off;
                op = W ? OP_ldr_pc_pre : OP_ldr_pc;
            }
            goto done;
        }
        if (cls == 2) {
            u32 off = insn & 0xFFF, so = U ? off : (u32)-off;
            if (d->rn == 15) {
                if (P && !W && L && !B) { d->a = pc + 8 + so; op = OP_ls_ldr_lit; }
                goto done;
            }
            d->a = so;
            if (P && !W) op = L ? (B ? OP_ls_ldrb : OP_ls_ldr) : (B ? OP_ls_strb : OP_ls_str);
            else if (d->rn == d->rd) goto done;     /* writeback onto the data reg: generic */
            else if (P) op = L ? (B ? OP_ls_ldrb_pre : OP_ls_ldr_pre) : (B ? OP_ls_strb_pre : OP_ls_str_pre);
            else op = L ? (B ? OP_ls_ldrb_post : OP_ls_ldr_post) : (B ? OP_ls_strb_post : OP_ls_str_post);
        } else {
            if (insn & (1u << 4)) goto done;
            u32 type = (insn >> 5) & 3, sh = (insn >> 7) & 0x1F;
            if (d->rn == 15 && d->rm != 15 && P && !W && U && type == 0 && L && !B) {
                d->a = pc + 8; d->b = sh; op = OP_ldr_pcreg;           /* ldr rd, [pc, rm] (GOT) */
                goto done;
            }
            if (d->rn == 15 || d->rm == 15 || !P || W || type != 0) goto done;
            d->a = !U;
            d->b = sh;
            op = L ? (B ? OP_ls_ldrb_reg : OP_ls_ldr_reg) : (B ? OP_ls_strb_reg : OP_ls_str_reg);
        }
        goto done;
    }
    if (cls == 4) {                                  /* ldm/stm */
        u32 P = (insn >> 24) & 1, U = (insn >> 23) & 1, S = (insn >> 22) & 1, W = (insn >> 21) & 1;
        u32 L = (insn >> 20) & 1, list = insn & 0xFFFF;
        if (S || d->rn == 15 || (list & (1u << d->rn)) || !list || (list & 0x8000)) {
            if (!(S || d->rn == 15 || (list & (1u << d->rn)) || !list) && W && L && !P && U) {
                d->a = list & 0x7FFF;
                op = OP_ldm_ia_wb_pc;                                     /* pop {..., pc} */
            }
            goto done;
        }
        if (!W) {
            if (!P && U) { d->a = list; op = L ? OP_ldm_ia : OP_stm_ia; }
            goto done;
        }
        if (!L && !P && U) { d->a = list; op = OP_stm_ia_wb; goto done; }
        if (L && !P && U) {
            d->a = list & 0x7FFF;
            op = (list & 0x8000) ? OP_ldm_ia_wb_pc : OP_ldm_ia_wb;
        } else if (!L && P && !U && !(list & 0x8000)) {
            d->a = list;
            d->b = 4 * (u32)__builtin_popcount(list);
            op = OP_stm_db_wb;
        }
        goto done;
    }
    if (cls == 5) {                                  /* b / bl */
        s32 off = (s32)(insn << 8) >> 6;
        d->a = pc + 8 + (u32)off;
        d->b = pc + 4;
        op = (insn & (1u << 24)) ? OP_br_bl : OP_br_b;
        goto done;
    }
    if (cls == 6 || cls == 7) {                      /* VFP (cp10 single, cp11 double) */
        u32 cp = (insn >> 8) & 0xF;
        if (cp != 10 && cp != 11) goto done;
        u32 D = (insn >> 22) & 1, N = (insn >> 7) & 1, M = (insn >> 5) & 1;
        u32 vn = (insn >> 16) & 0xF, vd = (insn >> 12) & 0xF, vm = insn & 0xF;
        u32 sd = (vd << 1) | D, sn = (vn << 1) | N, sm = (vm << 1) | M;
        u32 dd = (D << 4) | vd, dm = (M << 4) | vm;
        if (cls == 6) {
            u32 P = (insn >> 24) & 1, U = (insn >> 23) & 1, W = (insn >> 21) & 1, L = (insn >> 20) & 1;
            if (!(P && !W)) {                                              /* vldm/vstm/vpush/vpop */
                u32 imm8 = insn & 0xFF;
                if (P == U) goto done;                                      /* not a valid multiple */
                if (d->rn == 15 || !imm8) goto done;
                if (cp == 10) { if (sd + imm8 > 32) goto done; d->rd = (u8)sd; d->a = imm8; }
                else { if (dd + imm8 / 2 > 16 || !(imm8 / 2)) goto done; d->rd = (u8)dd; d->a = imm8 / 2; }
                d->b = W | (imm8 << 8);
                if (P) op = cp == 10 ? (L ? OP_vldm_s_db : OP_vstm_s_db) : (L ? OP_vldm_d_db : OP_vstm_d_db);
                else   op = cp == 10 ? (L ? OP_vldm_s_ia : OP_vstm_s_ia) : (L ? OP_vldm_d_ia : OP_vstm_d_ia);
                goto done;
            }
            u32 off = (insn & 0xFF) * 4, so = U ? off : (u32)-off;
            if (cp == 11 && dd >= 16) goto done;
            d->rd = (u8)(cp == 10 ? sd : dd);
            if (d->rn == 15) {
                if (!L) goto done;
                d->a = pc + 8 + so;
                op = cp == 10 ? OP_vldr_s_lit : OP_vldr_d_lit;
            } else {
                d->a = so;
                op = cp == 10 ? (L ? OP_vldr_s : OP_vstr_s) : (L ? OP_vldr_d : OP_vstr_d);
            }
            goto done;
        }
        if (insn & (1u << 24)) goto done;           /* svc */
        if (insn & (1u << 4)) {                      /* vmov core<->s, vmrs */
            u32 op1 = (insn >> 21) & 7, L = (insn >> 20) & 1, rt = d->rd;
            if (cp != 10) goto done;
            if (op1 == 0 && rt != 15) { d->rn = (u8)sn; op = L ? OP_vmov_rs : OP_vmov_sr; }
            else if (op1 == 7 && L && vn == 1 && rt == 15) op = OP_vmrs_apsr;
            goto done;
        }
        u32 vop = (((insn >> 23) & 1) << 2) | ((insn >> 20) & 3), L = (insn >> 6) & 1;
        if (vop != 7) {
            if (cp != 10) goto done;
            d->rd = (u8)sd; d->rn = (u8)sn; d->rm = (u8)sm;
            switch (vop) {
            case 0: op = L ? OP_vmls_s : OP_vmla_s; break;
            case 1: op = L ? OP_vnmla_s : OP_vnmls_s; break;
            case 2: op = L ? OP_vnmul_s : OP_vmul_s; break;
            case 3: op = L ? OP_vsub_s : OP_vadd_s; break;
            case 4: op = OP_vdiv_s; break;
            default: break;
            }
            goto done;
        }
        u32 b76 = (N << 1) | L;
        if (cp == 11) {
            if (vn == 7 && b76 && dm < 16) { d->rd = (u8)sd; d->rm = (u8)dm; op = OP_vcvt_s_d; }
            goto done;
        }
        d->rd = (u8)sd; d->rm = (u8)sm;
        if (b76 == 0) {                              /* vmov.f32 imm */
            u32 imm8 = (vn << 4) | vm;
            d->a = ((imm8 >> 7) & 1) << 31 | ((((imm8 >> 3) & 0xF) + 120) << 23) | ((imm8 & 7) << 20);
            op = OP_vmov_imm_s;
            goto done;
        }
        switch (vn) {
        case 0x0: op = b76 == 1 ? OP_vmov_ss : b76 == 3 ? OP_vabs_s : OP_generic; break;
        case 0x1: op = b76 == 1 ? OP_vneg_s : b76 == 3 ? OP_vsqrt_s : OP_generic; break;
        case 0x4: op = OP_vcmp_s; break;
        case 0x5: op = OP_vcmpz_s; break;
        case 0x7: if (dd < 16) { d->rd = (u8)dd; op = OP_vcvt_d_s; } break;
        case 0x8: op = (b76 & 2) ? OP_vcvt_s_s32 : OP_vcvt_s_u32; break;
        case 0xD: if (b76 & 2) op = OP_vcvt_s32_s_rz; break;
        default: break;
        }
        goto done;
    }
done:
    if (op == OP_generic && (insn & 0x0FFFFFF0u) == 0x012FFF10u && d->cond != 0xF)
        op = OP_br_bx;                               /* bx rm */
    if (op == OP_generic && (insn & 0x0FFFFFF0u) == 0x012FFF30u && d->cond != 0xF && d->rm != 15) {
        d->b = pc + 4;                               /* blx rm */
        op = OP_br_blx;
    }
    for (int i = 0; i < nhooks; i++)
        if (hook_pcs[i] == pc) op = OP_hook;
    d->op = (u16)op;
    d->cx = d->cond ^ 0xE;
    d->h = op == OP_generic ? d_generic : op == OP_hook ? d_hook : op_fns[op];
}

JITCALL void d_decode(cpu_t *c, const di_t *d)
{
    di_t *w = (di_t *)d;
    u32 pc = c->r[15] - 4;
    di_t tmp;
    decode_into(&tmp, pc, ld32(pc));
    dfn_t h = tmp.h;
    u16 op = tmp.op;
    tmp.h = d_decode;
    tmp.op = OP_decode;
    u8 cx = tmp.cx;
    tmp.cx = 0;                      /* published entry keeps "always, decode" until op lands */
    *w = tmp;                        /* publish operands before the handler */
    __atomic_store_n(&w->h, h, __ATOMIC_RELEASE);
    __atomic_store_n(&w->cx, cx, __ATOMIC_RELAXED);
    __atomic_store_n(&w->op, op, __ATOMIC_RELEASE);
    if (tmp.cond == 0xE || ((cond_tab[tmp.cond] >> (c->cpsr >> 28)) & 1))
        h(c, w);
}

/* for the selftest: decode one word at pc, report whether a fast handler was chosen */
bool cpu_decode_one(di_t *out, u32 pc, u32 insn)
{
    decode_into(out, pc, insn);
    return out->op != OP_generic && out->op != OP_hook;
}
