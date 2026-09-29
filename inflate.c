/*
 * inflate.c - raw DEFLATE decoder (RFC 1951) for unpacking the APK / OBB zip,
 * so no zlib library is needed on any host. Whole-buffer: the caller knows the
 * uncompressed size from the zip directory. Huffman codes up to 10 bits decode
 * with one table lookup; longer ones fall back to the canonical walk.
 */
#include "emu.h"

#define FAST_BITS 10

typedef struct {
    u16 count[16];              /* codes per length */
    u16 sym[288];               /* symbols ordered by code */
    u16 fast[1 << FAST_BITS];   /* sym | len << 9, 0 = longer than FAST_BITS */
} huff_t;

typedef struct {
    const u8 *in, *end;
    u64 bits;
    int nbits, over;            /* over: bytes read past the end (zero padding) */
    u8 *out, *out_end, *out0;
} infl_t;

static inline void refill(infl_t *s)
{
    while (s->nbits <= 56) {
        u64 b = 0;
        if (s->in < s->end) b = *s->in++; else s->over++;
        s->bits |= b << s->nbits;
        s->nbits += 8;
    }
}

static inline u32 getbits(infl_t *s, int n)
{
    if (s->nbits < n) refill(s);
    u32 v = (u32)(s->bits & ((1ull << n) - 1));
    s->bits >>= n;
    s->nbits -= n;
    return v;
}

static bool huff_build(huff_t *h, const u8 *lens, int n)
{
    u16 offs[16];
    memset(h->count, 0, sizeof(h->count));
    memset(h->fast, 0, sizeof(h->fast));
    for (int i = 0; i < n; i++) h->count[lens[i]]++;
    h->count[0] = 0;
    offs[1] = 0;
    for (int l = 1; l < 15; l++) offs[l + 1] = offs[l] + h->count[l];
    for (int i = 0; i < n; i++) if (lens[i]) h->sym[offs[lens[i]]++] = (u16)i;
    /* fast table: canonical codes, bit-reversed because DEFLATE sends them MSB first */
    u32 code = 0, k = 0;
    for (int l = 1; l <= 15; l++) {
        for (u32 j = 0; j < h->count[l]; j++, code++, k++) {
            if (l > FAST_BITS) continue;
            u32 rev = 0;
            for (int b = 0; b < l; b++) rev |= ((code >> b) & 1) << (l - 1 - b);
            for (u32 e = rev; e < (1u << FAST_BITS); e += 1u << l) h->fast[e] = (u16)(h->sym[k] | l << 9);
        }
        code <<= 1;
    }
    return true;
}

static int huff_decode(infl_t *s, const huff_t *h)
{
    if (s->nbits < 15) refill(s);
    u16 e = h->fast[s->bits & ((1u << FAST_BITS) - 1)];
    if (e) {
        int l = e >> 9;
        s->bits >>= l;
        s->nbits -= l;
        return e & 0x1FF;
    }
    int code = 0, first = 0, index = 0;
    for (int l = 1; l <= 15; l++) {
        code |= (int)getbits(s, 1);
        int cnt = h->count[l];
        if (code - first < cnt) return h->sym[index + code - first];
        index += cnt;
        first = (first + cnt) << 1;
        code <<= 1;
    }
    return -1;
}

static const u16 len_base[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                  35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const u8 len_extra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const u16 dist_base[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769,
                                   1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const u8 dist_extra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

static bool inflate_block(infl_t *s, const huff_t *lit, const huff_t *dist)
{
    for (;;) {
        int sym = huff_decode(s, lit);
        if (sym < 0 || s->over > 8) return false;
        if (sym < 256) {
            if (s->out >= s->out_end) return false;
            *s->out++ = (u8)sym;
            continue;
        }
        if (sym == 256) return true;
        sym -= 257;
        if (sym >= 29) return false;
        u32 len = len_base[sym] + getbits(s, len_extra[sym]);
        int ds = huff_decode(s, dist);
        if (ds < 0 || ds >= 30) return false;
        u32 d = dist_base[ds] + getbits(s, dist_extra[ds]);
        if (d > (u32)(s->out - s->out0) || len > (u32)(s->out_end - s->out)) return false;
        const u8 *from = s->out - d;
        for (u32 i = 0; i < len; i++) s->out[i] = from[i];      /* overlapping copies are intended */
        s->out += len;
    }
}

/* 0 on success, -1 on corrupt data or an output size mismatch */
int inflate_raw(const u8 *in, size_t n, u8 *out, size_t outn)
{
    static huff_t fixed_lit, fixed_dist;
    static bool fixed_ready;
    if (!fixed_ready) {
        u8 l[288];
        for (int i = 0; i < 288; i++) l[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
        huff_build(&fixed_lit, l, 288);
        for (int i = 0; i < 30; i++) l[i] = 5;
        huff_build(&fixed_dist, l, 30);
        fixed_ready = true;
    }
    infl_t s = { in, in + n, 0, 0, 0, out, out + outn, out };
    huff_t *lit = malloc(2 * sizeof(huff_t)), *dist = lit + 1;
    bool ok = lit != NULL, last = false;
    while (ok && !last) {
        last = getbits(&s, 1);
        u32 type = getbits(&s, 2);
        if (type == 0) {
            s.bits >>= s.nbits & 7;                              /* to a byte boundary */
            s.nbits -= s.nbits & 7;
            u32 len = getbits(&s, 16), nlen = getbits(&s, 16);
            if ((len ^ 0xFFFF) != nlen || len > (size_t)(s.out_end - s.out)) { ok = false; break; }
            while (len && s.nbits >= 8) { *s.out++ = (u8)getbits(&s, 8); len--; }
            if (len > (size_t)(s.end - s.in)) { ok = false; break; }
            memcpy(s.out, s.in, len);
            s.out += len;
            s.in += len;
        } else if (type == 1) {
            ok = inflate_block(&s, &fixed_lit, &fixed_dist);
        } else if (type == 2) {
            u32 hlit = getbits(&s, 5) + 257, hdist = getbits(&s, 5) + 1, hclen = getbits(&s, 4) + 4;
            static const u8 order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
            u8 cl[19] = { 0 }, lens[320] = { 0 };
            for (u32 i = 0; i < hclen; i++) cl[order[i]] = (u8)getbits(&s, 3);
            huff_t clh;
            huff_build(&clh, cl, 19);
            for (u32 i = 0; i < hlit + hdist && ok; ) {
                int sym = huff_decode(&s, &clh);
                if (sym < 16 && sym >= 0) { lens[i++] = (u8)sym; continue; }
                u32 rep = 0; u8 v = 0;
                if (sym == 16 && i > 0) { v = lens[i - 1]; rep = 3 + getbits(&s, 2); }
                else if (sym == 17) rep = 3 + getbits(&s, 3);
                else if (sym == 18) rep = 11 + getbits(&s, 7);
                else { ok = false; break; }
                if (i + rep > hlit + hdist) { ok = false; break; }
                while (rep--) lens[i++] = v;
            }
            if (!ok) break;
            huff_build(lit, lens, (int)hlit);
            huff_build(dist, lens + hlit, (int)hdist);
            ok = inflate_block(&s, lit, dist);
        } else ok = false;
        if (s.over > 8) ok = false;
    }
    free(lit);
    return ok && s.out == s.out_end ? 0 : -1;
}
