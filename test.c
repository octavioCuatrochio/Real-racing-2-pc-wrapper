/*
 * test.c - CPU/VFP self-tests with hand-encoded instruction words.
 * Run via --selftest. The .so's own __aeabi_* functions become differential
 * vectors once the loader exists (test_aeabi, called from main).
 */
#include "emu.h"
#include <time.h>
#include <sys/stat.h>

#define TBASE 0x50000000u
#define TDATA 0x60000000u

static int passes, fails;

#define CHECK(cond) do { if (cond) passes++; else { fails++; \
    LOG("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static void run_words(cpu_t *c, const u32 *code, int n)
{
    memset(g2h(TBASE), 0, 4096);
    memcpy(g2h(TBASE), code, n * 4);
    memset(c->r, 0, sizeof(c->r));
    c->cpsr = 0; c->fpscr = 0;
    memset(&c->v, 0, sizeof(c->v));
    c->r[13] = 0x70800000u;
    c->r[14] = HLE_SLOT_BASE;
    c->r[15] = TBASE;
    c->exit_loop = 0;
    cpu_icache_reset(); cpu_run(c);
}

#define AL 0xE0000000u
#define MOV_IMM(rd, imm)      (AL | 0x3A00000 | ((rd) << 12) | (imm))
#define MVN_IMM(rd, imm)      (AL | 0x3E00000 | ((rd) << 12) | (imm))
#define ADD_IMM(rd, rn, imm)  (AL | 0x2800000 | ((rn) << 16) | ((rd) << 12) | (imm))
#define SUB_IMM(rd, rn, imm)  (AL | 0x2400000 | ((rn) << 16) | ((rd) << 12) | (imm))
#define SUBS_IMM(rd, rn, imm) (AL | 0x2500000 | ((rn) << 16) | ((rd) << 12) | (imm))
#define ADDS_IMM(rd, rn, imm) (AL | 0x2900000 | ((rn) << 16) | ((rd) << 12) | (imm))
#define ADCS_IMM(rd, rn, imm) (AL | 0x2B00000 | ((rn) << 16) | ((rd) << 12) | (imm))
#define SBCS_IMM(rd, rn, imm) (AL | 0x2D00000 | ((rn) << 16) | ((rd) << 12) | (imm))
#define CMP_IMM(rn, imm)      (AL | 0x3500000 | ((rn) << 16) | (imm))
#define CMP_REG(rn, rm)       (AL | 0x1500000 | ((rn) << 16) | (rm))
#define ADD_REG(rd, rn, rm)   (AL | 0x0800000 | ((rn) << 16) | ((rd) << 12) | (rm))
#define SUB_REG(rd, rn, rm)   (AL | 0x0400000 | ((rn) << 16) | ((rd) << 12) | (rm))
#define MOV_REG(rd, rm, sh)   (AL | 0x1A00000 | ((rd) << 12) | (sh) | (rm))
#define MOVS_REG(rd, rm, sh)  (AL | 0x1B00000 | ((rd) << 12) | (sh) | (rm))
#define LSL(n)  (((n) & 31) << 7)
#define LSR(n)  ((((n) & 31) << 7) | 0x20)
#define ASR(n)  ((((n) & 31) << 7) | 0x40)
#define ROR(n)  ((((n) & 31) << 7) | 0x60)
#define BXLR                  0xE12FFF1Eu
#define B(off)                (AL | 0xA000000 | (((off) / 4 - 2) & 0xFFFFFF))
#define BL(off)               (AL | 0xB000000 | (((off) / 4 - 2) & 0xFFFFFF))
#define LDR_IMM(rd, rn, imm)  (AL | 0x5900000 | ((rn) << 16) | ((rd) << 12) | (imm))
#define STR_IMM(rd, rn, imm)  (AL | 0x5800000 | ((rn) << 16) | ((rd) << 12) | (imm))
#define LDRB_IMM(rd, rn, imm) (AL | 0x5D00000 | ((rn) << 16) | ((rd) << 12) | (imm))
#define STRB_IMM(rd, rn, imm) (AL | 0x5C00000 | ((rn) << 16) | ((rd) << 12) | (imm))

static void test_dp(cpu_t *c)
{
    u32 code[] = {
        MOV_IMM(0, 5), MOV_IMM(1, 3),
        ADD_IMM(2, 0, 1),          /* r2 = 6 */
        SUB_IMM(3, 2, 10),         /* r3 = -4 */
        SUBS_IMM(4, 1, 3),         /* r4 = 0, Z=1, C=1 */
        BXLR,
    };
    run_words(c, code, 6);
    CHECK(c->r[2] == 6 && c->r[3] == (u32)-4 && c->r[4] == 0);
    CHECK(c->cpsr & FLAG_Z); CHECK(c->cpsr & FLAG_C); CHECK(!(c->cpsr & FLAG_N));
}

static void test_flags(cpu_t *c)
{
    u32 code[] = {
        MOV_IMM(0, 0),
        SUBS_IMM(1, 0, 1),         /* 0-1 = FFFFFFFF: N=1, C=0 */
        ADDS_IMM(2, 1, 1),         /* FFFFFFFF+1 = 0: Z=1, C=1 */
        BXLR,
    };
    run_words(c, code, 4);
    CHECK(c->r[1] == 0xFFFFFFFF && c->r[2] == 0);
    CHECK((c->cpsr & (FLAG_Z | FLAG_C)) == (FLAG_Z | FLAG_C));
    /* 64-bit add chain */
    u32 code2[] = {
        MVN_IMM(0, 0),             /* r0 = FFFFFFFF */
        MOV_IMM(1, 0),
        MOV_IMM(2, 1),
        MOV_IMM(3, 0),
        ADD_REG(4, 0, 2),          /* sets no flags... use adds */
        ADDS_IMM(4, 0, 1),         /* r4 = 0, C=1 */
        ADCS_IMM(5, 1, 0),         /* r5 = 0+0+C = 1 */
        BXLR,
    };
    run_words(c, code2, 8);
    CHECK(c->r[4] == 0 && c->r[5] == 1);
    /* sbc: 0 - 1 with C=0 after subs */
    u32 code3[] = {
        MOV_IMM(0, 0),
        CMP_IMM(0, 1),             /* 0-1: C=0 (borrow) */
        SBCS_IMM(1, 0, 0),         /* 0 - 0 - !C = -1 */
        BXLR,
    };
    run_words(c, code3, 4);
    CHECK(c->r[1] == 0xFFFFFFFF);
}

static void test_shifter(cpu_t *c)
{
    u32 code[] = {
        MOV_IMM(0, 0xFF),
        MOV_REG(1, 0, LSL(4)),     /* FF0 */
        MOV_REG(2, 1, LSR(4)),     /* FF */
        MVN_IMM(3, 0x7F),          /* FFFFFF80 */
        MOV_REG(4, 3, ASR(7)),     /* FFFFFFFF */
        MOV_REG(5, 3, ROR(8)),     /* 80FFFFFF */
        BXLR,
    };
    run_words(c, code, 7);
    CHECK(c->r[1] == 0xFF0 && c->r[2] == 0xFF);
    CHECK(c->r[4] == 0xFFFFFFFF && c->r[5] == 0x80FFFFFF);
    /* lsr #32 and asr #32 and rrx */
    u32 code2[] = {
        MOV_IMM(0, 0x80),
        MOVS_REG(1, 0, LSL(24)),   /* 80000000 */
        MOV_REG(2, 1, LSR(0)),     /* lsr #32 -> 0 */
        MOV_REG(3, 1, ASR(0)),     /* asr #32 -> FFFFFFFF */
        CMP_IMM(0, 0),             /* C=1 */
        MOV_REG(4, 0, ROR(0)),     /* RRX: (C<<31)|(80>>1) = 80000040 */
        BXLR,
    };
    run_words(c, code2, 7);
    CHECK(c->r[2] == 0 && c->r[3] == 0xFFFFFFFF);
    CHECK(c->r[4] == 0x80000040);
    /* mov imm with rotation: 0xFF ror 8 = 0xFF000000 */
    u32 code3[] = { 0xE3A004FFu, MOV_IMM(1, 0x4F), BXLR };
    run_words(c, code3, 3);
    CHECK(c->r[0] == 0xFF000000 && c->r[1] == 0x4F);
}

static void test_mul(cpu_t *c)
{
    u32 code[] = {
        MOV_IMM(0, 7), MOV_IMM(1, 6),
        0xE0000290u | (0 << 16) | (1 << 8) | (2 << 0),   /* placeholder, replaced below */
        BXLR,
    };
    /* mul r2, r0, r1 = E002 0091? encoding: 000000 A S Rd 0000 Rs 1001 Rm
       mul r2, r0, r1: E 000000 0 0 0010 0000 0001 1001 0000 = 0xE0020190 */
    u32 mul_r2_r0_r1 = 0xE0020190u, mla_r5 = 0xE0253291u; /* mla r5, r1, r2, r3 */
    ((u32 *)code)[2] = mul_r2_r0_r1;
    run_words(c, code, 4);
    CHECK(c->r[2] == 42);
    /* umull r2:r3 = r0 * r1 (7*6) ; smull with -1 */
    u32 code2[] = {
        MOV_IMM(0, 7), MOV_IMM(1, 6),
        0xE0832190u,               /* umull r3:r2, r0, r1 -> E 00001 0 0 0 0011 0010 0001 1001 0000 */
        MVN_IMM(0, 0),             /* -1 */
        MVN_IMM(1, 0),             /* -1 */
        0xE0C76190u,               /* smull r7:r6, r0, r1 = E 00001 1 0 0 0111 0110 0001 1001 0000 */
        BXLR,
    };
    run_words(c, code2, 7);
    CHECK(c->r[2] == 42 && c->r[3] == 0);
    CHECK(c->r[6] == 1 && c->r[7] == 0);
    (void)mla_r5;
}

/* run with r1 preset to TDATA */
static void run_words_r1(cpu_t *c, const u32 *code, int n)
{
    memset(g2h(TBASE), 0, 4096);
    memcpy(g2h(TBASE), code, n * 4);
    memset(c->r, 0, sizeof(c->r));
    c->cpsr = 0; c->fpscr = 0;
    memset(&c->v, 0, sizeof(c->v));
    c->r[1] = TDATA;
    c->r[13] = 0x70800000u;
    c->r[14] = HLE_SLOT_BASE;
    c->r[15] = TBASE;
    c->exit_loop = 0;
    cpu_icache_reset(); cpu_run(c);
}

static void test_ls(cpu_t *c)
{
    /* data buffer at TDATA: pattern written by host first */
    st32(TDATA + 0, 0x11223344);
    st32(TDATA + 4, 0xAABBCCDD);
    st16(TDATA + 8, 0xFF80);
    st8(TDATA + 10, 0xFE);

    u32 code2[] = {
        LDR_IMM(0, 1, 0),          /* r0 = 11223344 */
        LDRB_IMM(2, 1, 5),         /* CC */
        LDRB_IMM(3, 1, 4),         /* DD */
        BXLR,
    };
    run_words_r1(c, code2, 4);
    CHECK(c->r[0] == 0x11223344 && c->r[2] == 0xCC && c->r[3] == 0xDD);

    /* stores + writeback addressing */
    u32 code3[] = {
        LDR_IMM(0, 1, 0),          /* r0 = 11223344 */
        STR_IMM(0, 1, 8),          /* [TDATA+8] = r0 */
        STRB_IMM(0, 1, 15),        /* byte */
        LDR_IMM(2, 1, 8),
        0xE4913004u,               /* post: ldr r3, [r1], #4 */
        0xE5B14004u,               /* pre wb: ldr r4, [r1, #4]! */
        BXLR,
    };
    run_words_r1(c, code3, 7);
    CHECK(c->r[2] == 0x11223344);
    CHECK(ld8(TDATA + 15) == 0x44);
    CHECK(c->r[3] == 0x11223344 && c->r[1] == TDATA + 8); /* post(+4) then pre wb(+4) */
    CHECK(c->r[4] == 0x11223344);   /* [TDATA+8]: stored earlier */

    /* halfword signed */
    u32 code5[] = {
        0xE1D160D4u,               /* ldrsb r6, [r1, #4]  (DD -> FFFFFFDD) */
        0xE1D170F6u,               /* ldrsh r7, [r1, #6]  (CCDD -> FFFFCCDD) */
        0xE1C160B6u,               /* strh r6, [r1, #6] */
        BXLR,
    };
    run_words_r1(c, code5, 4);
    CHECK(c->r[6] == 0xFFFFFFDD && c->r[7] == 0xFFFFAABB);
    CHECK(ld16(TDATA + 6) == 0xFFDD);

    /* ldrd / strd */
    st32(TDATA + 16, 0x11111111); st32(TDATA + 20, 0x22222222);
    u32 code6[] = {
        0xE1C181D0u,               /* ldrd r8, [r1, #16]  (SH=10, L=0, immH=1) */
        0xE1C182F0u,               /* strd r8, [r1, #32]  (SH=11, L=0, immH=2) */
        0xE1C1A2D0u,               /* ldrd r10, [r1, #32] */
        BXLR,
    };
    run_words_r1(c, code6, 4);
    CHECK(c->r[8] == 0x11111111 && c->r[9] == 0x22222222);
    CHECK(c->r[10] == 0x11111111 && c->r[11] == 0x22222222);
}

static void test_ldmstm(cpu_t *c)
{
    u32 code[] = {
        MOV_IMM(4, 11), MOV_IMM(5, 22), MOV_IMM(6, 33),
        0xE92D0070u,               /* stmfd sp!, {r4,r5,r6} */
        MOV_IMM(4, 0), MOV_IMM(5, 0), MOV_IMM(6, 0),
        0xE8BD0070u,               /* ldmfd sp!, {r4,r5,r6} */
        BXLR,
    };
    run_words(c, code, 9);
    CHECK(c->r[4] == 11 && c->r[5] == 22 && c->r[6] == 33);
    CHECK(c->r[13] == 0x70800000u);
    /* stmdb/ldmda variations: store descending */
    u32 code2[] = {
        MOV_IMM(0, 1), MOV_IMM(1, 2), MOV_IMM(2, 3),
        0xE92D0007u,               /* stmdb sp!, {r0,r1,r2} */
        0xE8BD0007u,               /* ldmia sp!, {r0,r1,r2} (W=1) */
        BXLR,
    };
    run_words(c, code2, 6);
    CHECK(c->r[0] == 1 && c->r[1] == 2 && c->r[2] == 3 && c->r[13] == 0x70800000u);
}

static void test_branch(cpu_t *c)
{
    /* save lr, bl to a leaf fn, then return to the magic address */
    u32 code[] = {
        0xE92D4000u,               /* word 0: push {lr} */
        BL(16),                    /* word 1: call word 5 */
        MOV_IMM(1, 99),            /* word 2: after return */
        B(16),                     /* word 3: jump to word 7 */
        MOV_IMM(1, 55),            /* word 4: skipped */
        MOV_IMM(0, 77),            /* word 5: leaf */
        BXLR,                      /* word 6 */
        0xE8BD8000u,               /* word 7: pop {pc} -> magic return */
    };
    run_words(c, code, 8);
    CHECK(c->r[0] == 77 && c->r[1] == 99);
}

static void test_cond(cpu_t *c)
{
    u32 code[] = {
        MOV_IMM(0, 1),
        CMP_IMM(0, 2),             /* 1-2: N=1,V=0 -> LT */
        0x03A01001u,               /* moveq r1, #1 */
        0x13A01002u,               /* movne r1, #2 */
        0xB3A02003u,               /* movlt r2, #3 */
        0xC3A03004u,               /* movgt r3, #4 */
        BXLR,
    };
    run_words(c, code, 7);
    CHECK(c->r[1] == 2 && c->r[2] == 3 && c->r[3] == 0);
}

static void test_misc(cpu_t *c)
{
    u32 code[] = {
        MOV_IMM(0, 0x10),
        0xE16F1F10u,               /* clz r1, r0 */
        BXLR,
    };
    run_words(c, code, 3);
    CHECK(c->r[1] == 27);
    /* qadd saturation */
    u32 code2[] = {
        0xE3E00102u,               /* mvn r0, #0x80000000 -> 7FFFFFFF */
        MOV_IMM(1, 1),
        0xE1012050u,               /* qadd r2, r0, r1 */
        BXLR,
    };
    run_words(c, code2, 4);
    CHECK(c->r[2] == 0x7FFFFFFF && (c->cpsr & FLAG_Q));
}

static void test_vfp(cpu_t *c)
{
    /* s16 = 1.0f, s18 = s16+s16 = 2.0 */
    u32 code[] = {
        MOV_IMM(0, 0x3F80),        /* low 16 bits only; fix with rotation trick */
        BXLR,
    };
    (void)code;
    u32 code2[] = {
        0xE3A00E7Fu,               /* mov r0, #0x7F000000? -> wrong; use literal below */
        BXLR,
    };
    (void)code2;
    /* 1.0f = 0x3F800000: mov r0,#0xFE; mov r0, r0, ror#1 -> 7F000000... simpler:
       0x3F800000 = imm8 0xFE ror 2? 0xFE rotated: use MVN: ~0xC07FFFFF no.
       imm8=0x7F, rot=1 (ror 2): 0x7F ror 2 = 0x3FC00000 no.
       Use two-step: mov r0, #0x3F80 (0x3F80), lsl #16 -> 0x3F800000. */
    u32 code3[] = {
        MOV_IMM(0, 0x3F8),         /* too big for imm8! 0x3F8 > 255 */
        BXLR,
    };
    (void)code3;
    /* 0x3F8 needs rotation: imm8=0xFE,rot=15 (ror 30 = lsl 2): 0xFE<<2 = 0x3F8 ✓ */
    u32 code4[] = {
        MOV_IMM(0, 0xFE),          /* 0xFE */
        MOV_REG(0, 0, LSL(22)),    /* 0x3F800000 (1.0f) */
        0xEE080A10u,               /* vmov s16, r0 */
        0xEE381A08u,               /* vadd.f32 s2? -> compute: vadd s2,s16,s16:
                                      1110 0011 vn=1000 vd=0001 1010 N=0 0 M=0 vm=1000 = 0xEE381A08 */
        0xEE111A10u,               /* vmov r1, s2: mrc L=1: 1110 0001 vn=0001 rt=0001 1010 N=1 ...
                                      = 1110 0001 0001 0001 1010 1001 0000 -> 0xEE111A90 */
        BXLR,
    };
    run_words(c, code4, 6);
    CHECK(c->r[1] == 0x40000000);   /* 2.0f */

    /* int conversions */
    u32 code5[] = {
        MOV_IMM(0, 42),
        0xEE000A10u,               /* vmov s0, r0 */
        0xEEB81A40u,               /* vcvt.f32.s32 s2, s0 = 1110 1011 1000 0000 0001 1010 0100 0000 */
        0xEE111A10u,               /* vmov r1, s2 (vn=1, rt=1, N=0) */
        0xEEBD3AC1u,               /* vcvt.s32.f32 s6, s2 (vm=1) */
        0xEE133A10u,               /* vmov r3, s6: vn=3 rt=3 = 0xEE133A10 */
        BXLR,
    };
    run_words(c, code5, 7);
    CHECK(c->r[1] == 0x42280000);   /* 42.0f */
    CHECK(c->r[3] == 42);

    /* f64 via mcrr/mrrc: d9 = 2.0, d8 = d9+d9 = 4.0 */
    u32 code6[] = {
        MOV_IMM(0, 0),
        0xE3A01E01u | (1 << 8),    /* mov r1, #0x40000000 (imm8=1 ror 4? 1 ror 4 = 0x40000000: rot=2) */
        0xEC410B19u,               /* vmov d9, r0, r1 */
        0xEE398B09u,               /* vadd.f64 d8, d9, d9 */
        0xEC532B18u,               /* vmov r2, r3, d8 */
        BXLR,
    };
    /* fix r1 encoding: imm8=0x10 ror 8? 0x40000000 = imm8 1 ror 4 -> rot field=2 */
    code6[1] = 0xE3A01101u;
    run_words(c, code6, 6);
    CHECK(c->r[3] == 0x40100000 && c->r[2] == 0);  /* 4.0 */

    /* vcmp + vmrs */
    u32 code7[] = {
        MOV_IMM(0, 0xFE),
        MOV_REG(0, 0, LSL(22)),    /* 0x3F800000 (1.0f) */
        0xEE080A10u,               /* vmov s16, r0 */
        0xEE381A08u,               /* vadd.f32 s2, s16, s16 -> 2.0 */
        0xEEB40A48u,               /* vcmp.f32 s16, s2: 1110 1011 0100 0000 0000 1010 0100 1000 */
        0xEEF1FA10u,               /* vmrs apsr_nzcv, fpscr */
        BXLR,
    };
    run_words(c, code7, 7);
    CHECK(c->cpsr & FLAG_N);        /* 1.0 < 2.0 */
    CHECK(!(c->cpsr & FLAG_Z));

    /* vldr/vstr */
    st32(TDATA + 32, 0x40490FDB);   /* pi */
    u32 code8[] = {
        0xED918A08u,               /* vldr s17, [r1, #32] */
        0xED818A09u,               /* vstr s16, [r1, #36] (s16 should be 1.0... zero here) */
        0xEE182A10u,               /* vmov r2, s16 (vn=8, N=0) */
        BXLR,
    };
    run_words_r1(c, code8, 4);
    CHECK(c->r[2] == 0x40490FDB);
    CHECK(ld32(TDATA + 36) == 0x40490FDB);   /* vstr s16 stored pi */
}

/* saturation edge cases for vcvt */
static void test_vfp_sat(cpu_t *c)
{
    /* s0 = NaN (0x7FC00000); vcvt.s32.f32 -> 0 ; s2 = 1e30f -> INT_MAX */
    u32 code[] = {
        0xE3E00E7Fu | (7 << 8),    /* mvn r0, ... easier: host pokes */
        BXLR,
    };
    (void)code;
    u32 code2[] = {
        0xEE000A10u,               /* vmov s0, r0 */
        0xEEBD1A40u,               /* vcvt.s32.f32 s2, s0 */
        0xEE113A10u,               /* vmov r3? no: vmov r1, s2: vn=1,rt=1: 0xEE112A10 */
        BXLR,
    };
    code2[2] = 0xEE111A10u;   /* vmov r1, s2 */
    /* NaN */
    memcpy(g2h(TBASE), code2, sizeof(code2));
    memset(c->r, 0, sizeof(c->r));
    c->r[0] = 0x7FC00000u;
    c->r[13] = 0x70800000; c->r[14] = HLE_SLOT_BASE; c->r[15] = TBASE; c->exit_loop = 0;
    memset(&c->v, 0, sizeof(c->v)); c->cpsr = 0; c->fpscr = 0;
    cpu_icache_reset(); cpu_run(c);
    CHECK(c->r[1] == 0);
    /* +inf -> INT_MAX */
    memset(c->r, 0, sizeof(c->r)); memset(&c->v, 0, sizeof(c->v));
    c->r[0] = 0x7F800000u;
    c->r[13] = 0x70800000; c->r[14] = HLE_SLOT_BASE; c->r[15] = TBASE; c->exit_loop = 0;
    cpu_icache_reset(); cpu_run(c);
    CHECK(c->r[1] == 0x7FFFFFFF);
    /* -inf -> INT_MIN */
    memset(c->r, 0, sizeof(c->r)); memset(&c->v, 0, sizeof(c->v));
    c->r[0] = 0xFF800000u;
    c->r[13] = 0x70800000; c->r[14] = HLE_SLOT_BASE; c->r[15] = TBASE; c->exit_loop = 0;
    cpu_icache_reset(); cpu_run(c);
    CHECK(c->r[1] == 0x80000000);
}

/* regressions for decode-table gaps found booting the game */
static void test_decode_regress(cpu_t *c)
{
    st32(TDATA + 0x14, 0xCAFEF00D);
    u32 ldr[] = { MOV_IMM(0, 0x206), LDR_IMM(1, 0, 0x14), BXLR };   /* offset bit4 set */
    run_words(c, ldr, 3);
    CHECK(c->r[1] == 0xCAFEF00D);

    u32 vp[] = {
        MOV_IMM(0, 0x55), MOV_IMM(1, 0x66),
        0xEC410B18u,               /* vmov d8, r0, r1 */
        0xED2D8B02u,               /* vpush {d8} */
        0xEC432B18u,               /* vmov d8, r2, r3 (zero) */
        0xECBD8B02u,               /* vpop {d8} */
        0xEC554B18u,               /* vmov r4, r5, d8 */
        BXLR,
    };
    run_words(c, vp, 8);
    CHECK(c->r[4] == 0x55 && c->r[5] == 0x66 && c->r[13] == 0x70800000u);
    CHECK(ld32(0x70800000u - 8) == 0x55);

    u32 dsp[] = {
        MOV_IMM(1, 3), MOV_IMM(2, 4), MOV_IMM(3, 10),
        0xE1003281u,               /* smlabb r0, r1, r2, r3 */
        MOV_IMM(1, 0x805),         /* r1 = 0x00050000 */
        MOV_IMM(2, 7),
        0xE16602A1u,               /* smultb r6, r1, r2 */
        MOV_IMM(0, 10), MOV_IMM(1, 3),
        0xE1212050u,               /* qsub r2, r0, r1 */
        BXLR,
    };
    run_words(c, dsp, 11);
    CHECK(c->r[6] == 35);
    CHECK(c->r[2] == 7);
    u32 dsp2[] = { MOV_IMM(1, 3), MOV_IMM(2, 4), MOV_IMM(3, 10), 0xE1003281u, BXLR };
    run_words(c, dsp2, 5);
    CHECK(c->r[0] == 22 && c->r[3] == 10);
}

static void test_heap(void)
{
    enum { N = 4096 };
    static gptr ptr[N];
    static u32 len[N];
    u32 seed = 12345, bad = 0;
    for (int it = 0; it < 200000; it++) {
        seed = seed * 1103515245u + 12345u;
        int i = (seed >> 8) % N;
        if (ptr[i]) {
            for (u32 k = 0; k < len[i]; k += 61)
                if (ld8(ptr[i] + k) != (u8)(i + k)) bad++;
            if (seed & 0x10000) { guest_free(ptr[i]); ptr[i] = 0; continue; }
            u32 nl = (seed >> 20) % 3000 + 1;
            gptr np = guest_realloc(ptr[i], nl);
            for (u32 k = 0; k < (nl < len[i] ? nl : len[i]); k += 61)
                if (ld8(np + k) != (u8)(i + k)) bad++;
            ptr[i] = np; len[i] = nl;
        } else {
            u32 l = (seed & 0x3000) == 0x3000 ? (seed >> 12) % 200000 + 1 : (seed >> 16) % 500 + 1;
            ptr[i] = guest_malloc(l);
            len[i] = l;
            if (!ptr[i] || (ptr[i] & 7) || guest_usable_size(ptr[i]) < l) bad++;
        }
        if (ptr[i]) for (u32 k = 0; k < len[i]; k += 61) st8(ptr[i] + k, (u8)(i + k));
    }
    CHECK(bad == 0);
    for (int i = 0; i < N; i++) { guest_free(ptr[i]); ptr[i] = 0; }
    gptr a = guest_malloc(1 << 20), b = guest_malloc(1 << 20);   /* space got coalesced back */
    CHECK(a && b && b > a);
    guest_free(a); guest_free(b);

    gptr al[64];
    bool ok = true;
    for (int i = 0; i < 64; i++) {
        u32 A = 16u << (i % 9);
        al[i] = guest_memalign(A, 40 + i * 13);
        ok &= al[i] && !(al[i] & (A - 1));
        memset(g2h(al[i]), 0xAB, 40 + i * 13);
        if (i & 1) { gptr t = guest_malloc(24); ok &= t != 0; guest_free(t); }
    }
    for (int i = 0; i < 64; i += 2) guest_free(al[i]);
    for (int i = 1; i < 64; i += 2) guest_free(al[i]);
    CHECK(ok);
}

void hle_sprintf(cpu_t *c);

/* printf engine through the real HLE entry: soft-float doubles, 64-bit, stack args */
static void test_printf(cpu_t *c)
{
    gptr buf = TDATA + 0x800, fmt = TDATA + 0x700;
    strcpy(g2h(fmt), "%d|%5.2f|%s|%lld|%x|%c");
    gptr str = TDATA + 0x780;
    strcpy(g2h(str), "ok");
    f64 d = 3.14159; u64 db; memcpy(&db, &d, 8);
    c->r[13] = 0x707ff000u;
    c->r[0] = buf; c->r[1] = fmt; c->r[2] = (u32)-7;      /* %d in r2, %f needs r4:r5 -> stack */
    st32(c->r[13] + 0, (u32)db); st32(c->r[13] + 4, (u32)(db >> 32));
    st32(c->r[13] + 8, str);
    st32(c->r[13] + 16, 0x9ABCDEF0u); st32(c->r[13] + 20, 0x12);   /* lld: 8-aligned */
    st32(c->r[13] + 24, 0xbeef); st32(c->r[13] + 28, 'Z');
    hle_sprintf(c);
    CHECK(!strcmp(g2h(buf), "-7| 3.14|ok|79905480432|beef|Z"));
    if (strcmp(g2h(buf), "-7| 3.14|ok|79905480432|beef|Z")) LOG("  got '%s'\n", (char *)g2h(buf));
}

void hle_sscanf(cpu_t *c);

static void test_scanf(cpu_t *c)
{
    gptr in = TDATA + 0x900, fmt = TDATA + 0x980, o = TDATA + 0xA00;
    strcpy(g2h(in), "FFT_X,5466 0x1f -12 abcz");
    strcpy(g2h(fmt), "%[^,],%hd %i %hhd %[a-c]%n");
    memset(g2h(o), 0xEE, 0x100);
    c->r[13] = 0x707ff000u;
    c->r[0] = in; c->r[1] = fmt; c->r[2] = o; c->r[3] = o + 0x40;
    st32(c->r[13], o + 0x50); st32(c->r[13] + 4, o + 0x60);
    st32(c->r[13] + 8, o + 0x70); st32(c->r[13] + 12, o + 0x80);
    hle_sscanf(c);
    CHECK(c->r[0] == 5);
    CHECK(!strcmp(g2h(o), "FFT_X"));
    CHECK(ld16(o + 0x40) == 5466 && ld16(o + 0x42) == 0xEEEE);   /* %hd writes 2 bytes only */
    CHECK(ld32(o + 0x50) == 0x1f);
    CHECK(ld8(o + 0x60) == (u8)-12 && ld8(o + 0x61) == 0xEE);
    CHECK(!strcmp(g2h(o + 0x70), "abc"));
    CHECK(ld32(o + 0x80) == 23);

    strcpy(g2h(in), "sm12");                              /* lightmap names: "%2c%d" */
    strcpy(g2h(fmt), "%2c%d");
    memset(g2h(o), 0, 16);
    c->r[0] = in; c->r[1] = fmt; c->r[2] = o; c->r[3] = o + 8;
    hle_sscanf(c);
    CHECK(c->r[0] == 2 && ld8(o) == 's' && ld8(o + 1) == 'm' && ld32(o + 8) == 12);
}

/* differential fuzz: fast pre-decoded handlers vs the generic reference */
static u32 fz_seed = 0x1234567;
static u32 fz(void) { fz_seed ^= fz_seed << 13; fz_seed ^= fz_seed >> 17; fz_seed ^= fz_seed << 5; return fz_seed; }

static void test_fuzz_fast(cpu_t *c)
{
    enum { MEM = 0x3000 };
    static u8 mem0[MEM], mem1[MEM];
    const u32 pc = TBASE + 0x100;
    int tested = 0, bad = 0, jtested = 0, jbad = 0;
    for (int it = 0; it < 600000 && bad < 8 && jbad < 8; it++) {
        u32 kind = fz() % 5, cond = (fz() & 3) ? 0xE : fz() % 15;
        u32 insn = cond << 28;
        if (kind == 0) insn |= fz() & 0x03FFFFFFu;                         /* data processing */
        else if (kind == 1) insn |= 0x04000000u | (fz() & 0x03FFFFFFu);    /* ldr/str */
        else if (kind == 2) insn |= 0x08000000u | (fz() & 0x01FF7FFFu);    /* ldm/stm, no pc */
        else if (kind == 3) insn |= 0x90u | (fz() & 0x003FFF0Fu);          /* mul/mla */
        else insn |= ((fz() & 1) ? 0x0C000000u : 0x0E000000u) | (fz() & 0x01FFF0FFu) | ((10u + (fz() & 1)) << 8);  /* vfp */
        di_t d;
        if (!cpu_decode_one(&d, pc, insn)) continue;
        if ((insn & 0x0FFFFFF0u) == 0x012FFF10u) continue;               /* bx: needs even target */
        if (kind < 2 && ((insn >> 12) & 0xF) == 15) continue;            /* writes pc: random target */
        bool memop = kind == 1 || kind == 2 || (kind == 4 && ((insn >> 25) & 7) == 6) ||
                     (kind == 0 && !(insn & (1u << 25)) && (insn & 0x90) == 0x90 && (insn & 0x60));
        u32 r0[16], cpsr0 = (fz() & 0xF0000000u);
        for (int i = 0; i < 15; i++)
            r0[i] = !memop ? ((fz() & 1) ? fz() : fz() & 0xFF) : TDATA + 0x1000 + (fz() & 0x7FC);
        if (kind == 1 && (insn & (1u << 25))) {
            if (((insn >> 7) & 0x1F) > 4) continue;
            r0[insn & 0xF] = fz() & 0xF;
        }
        if (kind == 1 && (insn & (1u << 25)) && (insn & 0xF) == ((insn >> 16) & 0xF)) continue;
        for (int i = 0; i < MEM; i++) mem0[i] = (u8)fz();
        static u32 v0[32], vg[32];
        for (int i = 0; i < 32; i++) {                       /* mix of sane floats and raw bits */
            f32 f = (f32)((s32)fz() % 20000) / 64.0f;
            memcpy(&v0[i], &f, 4);
            if (!(fz() & 7)) v0[i] = fz();
        }
        u32 fpscr0 = fz() & 0xF0000000u;

        u32 rg[16], fg, rf[16], ff, fpg;
        memcpy(c->r, r0, sizeof(r0)); c->cpsr = cpsr0; c->r[15] = pc + 4;
        memcpy(c->v.w, v0, sizeof(v0)); c->fpscr = fpscr0;
        memcpy(g2h(TDATA), mem0, MEM);
        if (cond_ok(insn >> 28, c->cpsr)) dec_table[DEC_KEY(insn)](c, insn);
        memcpy(rg, c->r, sizeof(rg)); fg = c->cpsr; memcpy(mem1, g2h(TDATA), MEM);
        memcpy(vg, c->v.w, sizeof(vg)); fpg = c->fpscr;

        memcpy(c->r, r0, sizeof(r0)); c->cpsr = cpsr0; c->r[15] = pc + 4;
        memcpy(c->v.w, v0, sizeof(v0)); c->fpscr = fpscr0;
        memcpy(g2h(TDATA), mem0, MEM);
        if (d.cond == 0xE || ((cond_tab[d.cond] >> (c->cpsr >> 28)) & 1)) d.h(c, &d);
        memcpy(rf, c->r, sizeof(rf)); ff = c->cpsr;

        tested++;
        bool fast_bad = memcmp(rg, rf, sizeof(rg)) || fg != ff || memcmp(mem1, g2h(TDATA), MEM) ||
                        memcmp(vg, c->v.w, sizeof(vg)) || fpg != c->fpscr;

        /* third run: the JIT, one instruction compiled as its own block */
        memcpy(c->r, r0, sizeof(r0)); c->cpsr = cpsr0;
        memcpy(c->v.w, v0, sizeof(v0)); c->fpscr = fpscr0;
        memcpy(g2h(TDATA), mem0, MEM);
        if (jit_test_one(c, pc, insn)) {
            jtested++;
            u32 rj[16]; memcpy(rj, c->r, sizeof(rj));
            rj[15] = rg[15];                       /* JIT leaves pc at the next insn, not pc+4 */
            if (memcmp(rg, rj, 15 * 4) || fg != c->cpsr || memcmp(mem1, g2h(TDATA), MEM) ||
                memcmp(vg, c->v.w, sizeof(vg)) || fpg != c->fpscr) {
                jbad++;
                if (jbad <= 8) {
                    LOG("JIT mismatch insn %08x: cpsr %08x vs %08x", insn, fg, c->cpsr);
                    for (int i = 0; i < 15; i++) if (rg[i] != rj[i]) LOG(" r%d %08x vs %08x", i, rg[i], rj[i]);
                    for (int i = 0; i < 32; i++) if (vg[i] != c->v.w[i]) LOG(" s%d %08x vs %08x", i, vg[i], c->v.w[i]);
                    LOG("%s\n", memcmp(mem1, g2h(TDATA), MEM) ? " (memory differs)" : "");
                }
            }
        }
        if (fast_bad) {
            bad++;
            LOG("FUZZ mismatch insn %08x: cpsr %08x vs %08x", insn, fg, ff);
            for (int i = 0; i < 16; i++) if (rg[i] != rf[i]) LOG(" r%d %08x vs %08x", i, rg[i], rf[i]);
            for (int i = 0; i < 32; i++) if (vg[i] != c->v.w[i]) LOG(" s%d %08x vs %08x", i, vg[i], c->v.w[i]);
            if (fpg != c->fpscr) LOG(" fpscr %08x vs %08x", fpg, c->fpscr);
            LOG("%s\n", memcmp(mem1, g2h(TDATA), MEM) ? " (memory differs)" : "");
        }
    }
    LOG("[selftest] fuzz: %d fast-path, %d JIT instructions compared\n", tested, jtested);
    CHECK(bad == 0 && tested > 100000);
    CHECK(jbad == 0 && jtested > 100000);
}

/* interpreter throughput on a fixed mixed loop: alu, cmp, ldr/str, bl/push/pop */
int bench_main(void)
{
    cpu_t *c = emu_new_cpu();
    G.text_lo = TBASE; G.text_span = 0x1000;
    u32 code[] = {
        0xE52DE004u,                              /* push {lr} */
        MOV_IMM(4, 0), 0xE3A05801u,               /* r4 = 0; r5 = 0x10000 iterations */
        0xE3A06206u,                              /* r6 = 0x60000000 (TDATA) */
        /* loop: */
        ADD_IMM(4, 4, 3),
        0xE0847005u,                              /* add r7, r4, r5 */
        0xE1A08127u,                              /* mov r8, r7, lsr #2 */
        0xE5868000u,                              /* str r8, [r6] */
        0xE5969004u,                              /* ldr r9, [r6, #4] */
        0xE0899008u,                              /* add r9, r9, r8 */
        0xE5869004u,                              /* str r9, [r6, #4] */
        0xE3540C01u,                              /* cmp r4, #256 */
        0x23A04000u,                              /* movcs r4, #0 */
        0xEB000002u,                              /* bl leaf */
        SUBS_IMM(5, 5, 1),
        0x1AFFFFF3u,                              /* bne loop */
        0xE49DF004u,                              /* pop {pc} */
        /* leaf: push {r4, lr}; eor r4, r4, r9; pop {r4, pc} */
        0xE92D4010u, 0xE0244009u, 0xE8BD8010u,
    };
    memcpy(g2h(TBASE), code, sizeof(code));
    struct timespec t0, t1;
    u64 n = 0;
    double best = 0;
    for (int rep = 0; rep < 5; rep++) {
        memset(c->r, 0, sizeof(c->r));
        c->r[13] = 0x70800000u; c->r[14] = HLE_SLOT_BASE; c->r[15] = TBASE; c->exit_loop = 0; c->cpsr = 0;
        u64 i0 = c->insn_count;
        cpu_icache_reset();
        jit_reset();
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (int k = 0; k < 40; k++) {
            c->r[15] = TBASE; c->r[14] = HLE_SLOT_BASE; c->exit_loop = 0;
            cpu_run(c);
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        n = c->insn_count - i0;
        double dt = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
        if (n / dt > best) best = n / dt;
    }
    LOG("[bench] %llu insns/run, best %.1f MIPS\n", (unsigned long long)n, best / 1e6);
    return 0;
}

void marshal_stat(gptr out, const struct stat *st);

static void test_stat_layout(void)
{
    struct stat st = { 0 };
    st.st_mode = 0100644; st.st_size = 0x123456789LL; st.st_ino = 77;
#ifndef _WIN32
    st.st_blksize = 4096;
#endif
    gptr o = TDATA + 0xC00;
    memset(g2h(o), 0xEE, 128);
    marshal_stat(o, &st);
    CHECK(ld32(o + 16) == 0100644);
    CHECK(ld32(o + 48) == 0x23456789 && ld32(o + 52) == 1);       /* long long st_size @48 */
    CHECK(ld32(o + 56) == 4096 && ld32(o + 96) == 77);
    CHECK(ld32(o + 104) == 0xEEEEEEEE);                          /* nothing past 104 bytes */
}

/* fused compare+branch: dp S-op then conditional b/bl, vs the reference */
static void test_fuzz_fused(cpu_t *c)
{
    const u32 pc = TBASE + 0x200;
    int tested = 0, bad = 0;
    for (int it = 0; it < 200000 && bad < 8; it++) {
        static const u32 opcs[] = { 0x0, 0x1, 0x2, 0x4, 0x8, 0x9, 0xA, 0xB, 0xC };
        u32 opc = opcs[fz() % 9];
        u32 i1 = 0xE0000000u | (fz() & 0x020FFFFFu) | (opc << 21) | (1u << 20);
        if (!(i1 & (1u << 25))) i1 &= ~0x10u;                  /* immediate shifts only */
        if (((i1 >> 12) & 0xF) == 15 || ((i1 >> 16) & 0xF) == 15 || (!(i1 & (1u << 25)) && (i1 & 0xF) == 15))
            continue;                                          /* pc operands: not in this test */
        u32 cond = fz() % 14;
        u32 i2 = (cond << 28) | 0x0A000000u | ((fz() & 1) << 24) | (fz() & 0x3F);
        u32 r0[16], cpsr0 = fz() & 0xF0000000u;
        for (int i = 0; i < 15; i++) r0[i] = (fz() & 1) ? fz() : fz() & 0xFF;
        /* reference */
        memcpy(c->r, r0, sizeof(r0)); c->cpsr = cpsr0;
        c->r[15] = pc + 4;
        dec_table[DEC_KEY(i1)](c, i1);
        u32 rp = pc + 8;
        if (cond_ok(i2 >> 28, c->cpsr)) {
            if (i2 & (1u << 24)) c->r[14] = pc + 8;
            rp = pc + 12 + (u32)((s32)(i2 << 8) >> 6);
        }
        u32 rg[16]; memcpy(rg, c->r, sizeof(rg)); u32 fg = c->cpsr;
        /* JIT */
        memcpy(c->r, r0, sizeof(r0)); c->cpsr = cpsr0;
        if (!jit_test_pair(c, pc, i1, i2)) continue;
        tested++;
        if (memcmp(rg, c->r, 15 * 4) || fg != c->cpsr || c->r[15] != rp) {
            bad++;
            LOG("FUSED mismatch %08x %08x: cpsr %08x vs %08x pc %08x vs %08x\n", i1, i2, fg, c->cpsr, rp, c->r[15]);
        }
    }
    LOG("[selftest] fused fuzz: %d pairs compared\n", tested);
    CHECK(bad == 0 && tested > 50000);
}

int selftest_main(void)
{
    cpu_t *c = emu_new_cpu();
    G.text_lo = TBASE; G.text_span = 0x1000;

    test_dp(c);
    test_flags(c);
    test_shifter(c);
    test_mul(c);
    test_ls(c);
    test_ldmstm(c);
    test_branch(c);
    test_cond(c);
    test_misc(c);
    test_vfp(c);
    test_vfp_sat(c);
    test_decode_regress(c);
    test_heap();
    test_printf(c);
    test_scanf(c);
    test_stat_layout();
    cpu_icache_reset();
    test_fuzz_fast(c);
    test_fuzz_fused(c);

    LOG("[selftest] %d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
