/*
 * jni_rr3_text.inc - RR3's Java text rendering (com.firemint.realracing.Font and
 * GlyphVector), done natively with stb_truetype. Included at the end of jni.c.
 *
 * Mirrors the Android calls the Java code makes: Paint.getFontMetrics /
 * getTextBounds / getTextWidths, Canvas.drawText into ALPHA_8 / ARGB_4444
 * bitmaps, and StaticLayout / BoringLayout for paragraphs (word wrap,
 * alignment, clipping, tail ellipsis).
 */
#include "stb_truetype.h"

char *vfs_host_path(const char *gpath);                /* hle_libc.c */
u32 glhost_text_texture(u32 tex, int w, int h, const u8 *px, u32 fmt);

#define FONT_CLS "com/firemint/realracing/Font"
#define GV_CLS   "com/firemint/realracing/GlyphVector"

typedef struct ttf { char path[512]; u8 *data; stbtt_fontinfo fi; struct ttf *next; } ttf_t;
typedef struct {
    ttf_t *t;
    float size, wscale, sy, sx;          /* sy = px per font unit, sx = sy * widthScale */
    float ascent, descent;               /* positive, Android Paint.FontMetrics style */
    bool bold;
} font_t;

static ttf_t *ttf_cache;
static pthread_mutex_t text_lock = PTHREAD_MUTEX_INITIALIZER;

static ttf_t *ttf_load(const char *gpath)
{
    for (ttf_t *t = ttf_cache; t; t = t->next) if (!strcmp(t->path, gpath)) return t;
    char *host = vfs_host_path(gpath);
    FILE *f = host ? fopen(host, "rb") : NULL;
    free(host);
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    ttf_t *t = calloc(1, sizeof(*t));
    t->data = malloc((size_t)n);
    if (fread(t->data, 1, (size_t)n, f) != (size_t)n || !stbtt_InitFont(&t->fi, t->data, stbtt_GetFontOffsetForIndex(t->data, 0))) {
        fclose(f); free(t->data); free(t);
        return NULL;
    }
    fclose(f);
    snprintf(t->path, sizeof(t->path), "%s", gpath);
    t->next = ttf_cache; ttf_cache = t;
    return t;
}

/* native state hung off the Java object */
static void native_free(void *p) { free(p); }
static void *jo_native(u32 h) { jobj_t *o = jo(h); return o ? o->native : NULL; }
static void jo_set_native(u32 h, void *p)
{
    jobj_t *o = jo(h);
    if (!o) { free(p); return; }
    if (o->native) o->native_free(o->native);
    o->native = p; o->native_free = native_free;
}

static void set_f(u32 obj, const char *cls, const char *name, float v) { u32 b; memcpy(&b, &v, 4); jfield_set(obj, cls, name, "F", b); }
static void set_i(u32 obj, const char *cls, const char *name, s32 v) { jfield_set(obj, cls, name, "I", (u32)v); }

/* UTF-8 -> code points */
static int utf8_decode(const char *s, u32 *out, int max)
{
    int n = 0;
    const u8 *p = (const u8 *)s;
    while (*p && n < max) {
        u32 cp = *p++;
        if (cp >= 0xF0 && p[0] && p[1] && p[2]) { cp = ((cp & 7) << 18) | ((p[0] & 63u) << 12) | ((p[1] & 63u) << 6) | (p[2] & 63u); p += 3; }
        else if (cp >= 0xE0 && p[0] && p[1]) { cp = ((cp & 15) << 12) | ((p[0] & 63u) << 6) | (p[1] & 63u); p += 2; }
        else if (cp >= 0xC0 && p[0]) { cp = ((cp & 31) << 6) | (p[0] & 63u); p += 1; }
        if (cp == 0x2060) continue;                           /* word joiner: sanitize() */
        out[n++] = cp;
    }
    return n;
}

static float glyph_adv(const font_t *f, u32 cp, u32 next)
{
    int adv, lsb;
    stbtt_GetCodepointHMetrics(&f->t->fi, (int)cp, &adv, &lsb);
    float a = adv * f->sx;
    if (next) a += stbtt_GetCodepointKernAdvance(&f->t->fi, (int)cp, (int)next) * f->sx;
    return a;
}
/* ink box of one glyph drawn with its origin at (x, 0) */
static void glyph_box(const font_t *f, u32 cp, float x, int *x0, int *y0, int *x1, int *y1)
{
    float fx = x - floorf(x);
    stbtt_GetCodepointBitmapBoxSubpixel(&f->t->fi, (int)cp, f->sx, f->sy, fx, 0, x0, y0, x1, y1);
    int ix = (int)floorf(x);
    *x0 += ix; *x1 += ix;
    if (f->bold) { (*x1)++; }
}
typedef struct { int l, t, r, b; bool any; } rect_t;
static void rect_add(rect_t *r, int l, int t, int rr, int b)
{
    if (rr <= l || b <= t) return;
    if (!r->any) { r->l = l; r->t = t; r->r = rr; r->b = b; r->any = true; return; }
    if (l < r->l) r->l = l; if (t < r->t) r->t = t; if (rr > r->r) r->r = rr; if (b > r->b) r->b = b;
}
static float run_width(const font_t *f, const u32 *cp, int n)
{
    float w = 0;
    for (int i = 0; i < n; i++) w += glyph_adv(f, cp[i], i + 1 < n ? cp[i + 1] : 0);
    return w;
}
static rect_t run_bounds(const font_t *f, const u32 *cp, int n)       /* Paint.getTextBounds */
{
    rect_t r = { 0 };
    float x = 0;
    for (int i = 0; i < n; i++) {
        int x0, y0, x1, y1;
        glyph_box(f, cp[i], x, &x0, &y0, &x1, &y1);
        rect_add(&r, x0, y0, x1, y1);
        x += glyph_adv(f, cp[i], i + 1 < n ? cp[i + 1] : 0);
    }
    if (!r.any) { r.l = 0; r.t = 0; r.r = 0; r.b = 0; }
    return r;
}
/* draw a run into an 8-bit coverage buffer with its baseline origin at (ox, oy); flip = rows bottom-up */
static void run_draw(const font_t *f, const u32 *cp, int n, float ox, float oy, float scale, u8 *buf, int w, int h, bool flip)
{
    float x = ox;
    float sx = f->sx * scale, sy = f->sy * scale;
    for (int i = 0; i < n; i++) {
        u32 c = cp[i];
        if (c != ' ' && c != '\n') {
            float fx = x - floorf(x), fy = oy - floorf(oy);
            int x0, y0, x1, y1;
            stbtt_GetCodepointBitmapBoxSubpixel(&f->t->fi, (int)c, sx, sy, fx, fy, &x0, &y0, &x1, &y1);
            int gw = x1 - x0, gh = y1 - y0;
            if (gw > 0 && gh > 0 && gw < 4096 && gh < 4096) {
                u8 *g = calloc((size_t)gw * gh, 1);
                stbtt_MakeCodepointBitmapSubpixel(&f->t->fi, g, gw, gh, gw, sx, sy, fx, fy, (int)c);
                int bx = (int)floorf(x) + x0, by = (int)floorf(oy) + y0;
                for (int yy = 0; yy < gh; yy++) {
                    int dy = by + yy;
                    if (dy < 0 || dy >= h) continue;
                    u8 *row = buf + (size_t)(flip ? h - 1 - dy : dy) * w;
                    for (int xx = 0; xx < gw; xx++) {
                        int dx = bx + xx;
                        if (dx < 0 || dx >= w) continue;
                        u32 s = g[(size_t)yy * gw + xx];
                        if (f->bold && xx + 1 < gw) { u32 s2 = g[(size_t)yy * gw + xx + 1]; if (s2 > s) s = s2; }
                        u32 d = row[dx];
                        row[dx] = (u8)(s + d - s * d / 255);        /* coverage "over" */
                    }
                }
                free(g);
            }
        }
        x += glyph_adv(f, c, i + 1 < n ? cp[i + 1] : 0) * scale;
    }
}
/* max-filter dilation (outline strokes) */
static void dilate(u8 *buf, int w, int h, int r)
{
    if (r <= 0) return;
    u8 *src = malloc((size_t)w * h);
    memcpy(src, buf, (size_t)w * h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            u8 m = 0;
            for (int dy = -r; dy <= r; dy++) {
                int yy = y + dy; if (yy < 0 || yy >= h) continue;
                for (int dx = -r; dx <= r; dx++) {
                    int xx = x + dx; if (xx < 0 || xx >= w || dx * dx + dy * dy > r * r + r) continue;
                    if (src[(size_t)yy * w + xx] > m) m = src[(size_t)yy * w + xx];
                }
            }
            buf[(size_t)y * w + x] = m;
        }
    free(src);
}

/* ---------------- Font ---------------- */
static u64 j_font_init(cpu_t *c, u32 self, jargs_t *a)
{
    u32 path = ja_i32(a), bold = ja_i32(a), italic = ja_i32(a);
    float size = ja_f32(a), wscale = ja_f32(a);
    (void)italic;
    pthread_mutex_lock(&text_lock);
    ttf_t *t = ttf_load(jstr(path));
    pthread_mutex_unlock(&text_lock);
    if (!t) { LOG("[font] cannot load %s\n", jstr(path)); return 0; }
    font_t *f = calloc(1, sizeof(*f));
    f->t = t; f->size = size; f->wscale = wscale > 0 ? wscale : 1.0f; f->bold = bold != 0;
    f->sy = stbtt_ScaleForMappingEmToPixels(&t->fi, size);
    f->sx = f->sy * f->wscale;
    int asc, desc, gap, bx0, by0, bx1, by1;
    stbtt_GetFontVMetrics(&t->fi, &asc, &desc, &gap);
    stbtt_GetFontBoundingBox(&t->fi, &bx0, &by0, &bx1, &by1);
    f->ascent = asc * f->sy; f->descent = -desc * f->sy;
    jo_set_native(self, f);
    set_f(self, FONT_CLS, "ascent", f->ascent);
    set_f(self, FONT_CLS, "descent", f->descent);
    set_f(self, FONT_CLS, "height", f->ascent + f->descent);
    set_f(self, FONT_CLS, "top", by1 * f->sy);
    set_f(self, FONT_CLS, "bottom", -by0 * f->sy);
    set_f(self, FONT_CLS, "leading", gap * f->sy);
    return 1;
}
static u64 j_font_load_glyph(cpu_t *c, u32 self, jargs_t *a)
{
    font_t *f = jo_native(self);
    u32 cp = ja_i32(a);
    if (!f) return 0;
    int x0, y0, x1, y1;
    glyph_box(f, cp, 0, &x0, &y0, &x1, &y1);
    if (x1 <= x0 || y1 <= y0) { x0 = x1 = y0 = y1 = 0; }
    set_f(self, FONT_CLS, "glyphOffX", (float)x0);
    set_f(self, FONT_CLS, "glyphOffY", (float)-y0);
    set_f(self, FONT_CLS, "glyphWidth", (float)(x1 - x0));
    set_f(self, FONT_CLS, "glyphHeight", (float)(y1 - y0));
    set_f(self, FONT_CLS, "glyphAdvance", glyph_adv(f, cp, 0));
    return 1;
}
static u64 j_font_load_bitmap(cpu_t *c, u32 self, jargs_t *a)
{
    font_t *f = jo_native(self);
    u32 cp = ja_i32(a);
    float offx = ja_f32(a), offy = ja_f32(a), ow = ja_f32(a);
    u32 otype = ja_i32(a);
    if (!f) return 0;
    int x0, y0, x1, y1;
    glyph_box(f, cp, 0, &x0, &y0, &x1, &y1);
    float pad = 1.0f + ((otype == 1 || otype == 3) ? ow : 0);
    float oy2 = -offy;
    float l = floorf(x0 + offx - pad), r = ceilf(x1 + offx + pad), t = floorf(y0 + oy2 - pad), b = ceilf(y1 + oy2 + pad);
    int bw = (int)(r - l), bh = (int)(b - t);
    set_i(self, FONT_CLS, "bmpLeft", (s32)l);
    set_i(self, FONT_CLS, "bmpTop", -(s32)t);
    set_i(self, FONT_CLS, "bmpWidth", bw);
    set_i(self, FONT_CLS, "bmpHeight", bh);
    if (bw <= 0 || bh <= 0 || bw > 4096 || bh > 4096) return 0;
    u8 *buf = calloc((size_t)bw * bh, 1);
    u32 one[1] = { cp };
    run_draw(f, one, 1, offx - l, oy2 - t, 1.0f, buf, bw, bh, false);
    if (otype == 1 || otype == 3) dilate(buf, bw, bh, (int)(ow + 0.5f));
    u32 arr = jarr_new((u32)(bw * bh), 1);
    memcpy(g2h(jo(arr)->data), buf, (size_t)bw * bh);
    free(buf);
    jfield_set(self, FONT_CLS, "bmpData", "[B", arr);
    jo_unref(jo_keep(arr));                                     /* the field holds it now */
    set_i(self, FONT_CLS, "bmpPitch", bw);
    return 1;
}

static u64 j_font_get_size(cpu_t *c, u32 self, jargs_t *a)
{
    font_t *f = jo_native(self);
    float v = f ? f->size : 0; u32 b; memcpy(&b, &v, 4); return b;
}
static u64 j_font_get_wscale(cpu_t *c, u32 self, jargs_t *a)
{
    font_t *f = jo_native(self);
    float v = f ? f->wscale : 1; u32 b; memcpy(&b, &v, 4); return b;
}

/* ---------------- GlyphVector ---------------- */
#define GV_MAXCP 4096
#define GV_MAXLINES 64
typedef struct {
    font_t font;                        /* copy: the Font object may go away first */
    u32 cp[GV_MAXCP]; int n;
    int nlines, ls[GV_MAXLINES], le[GV_MAXLINES];   /* [start, end) per line */
    float lx[GV_MAXLINES];              /* line left */
    bool layout;                        /* paragraph (else single run) */
    rect_t bounds;
    float lh;                           /* line height */
} gv_t;

static void gv_publish(u32 self, gv_t *g, float offx, float offy)
{
    if (!jfield_get(self, GV_CLS, "texId", "I")) set_i(self, GV_CLS, "texId", -1);   /* Java initializer: no texture yet */
    set_f(self, GV_CLS, "offsetX", offx);
    set_f(self, GV_CLS, "offsetY", offy);
    set_f(self, GV_CLS, "boundsW", (float)(g->bounds.r - g->bounds.l));
    set_f(self, GV_CLS, "boundsH", (float)(g->bounds.b - g->bounds.t));
    set_i(self, GV_CLS, "numLines", g->nlines);
}

static u64 j_gv_init(cpu_t *c, u32 self, jargs_t *a)
{
    u32 fo = ja_i32(a), txt = ja_i32(a);
    font_t *f = jo_native(fo);
    if (!f) return 0;
    gv_t *g = calloc(1, sizeof(*g));
    g->font = *f;
    g->n = utf8_decode(jstr(txt), g->cp, GV_MAXCP);
    g->nlines = 1; g->ls[0] = 0; g->le[0] = g->n;
    g->bounds = run_bounds(f, g->cp, g->n);
    g->bounds.t--; g->bounds.b++;
    jo_set_native(self, g);
    gv_publish(self, g, (float)-g->bounds.l, (float)-g->bounds.t);
    static int dbg = -1;
    if (dbg < 0) dbg = getenv("RR3_TEXT_DEBUG") ? 40 : 0;
    if (dbg > 0) { dbg--; LOG("[text] init '%s' size %.1f -> bounds %d,%d..%d,%d\n", jstr(txt), f->size, g->bounds.l, g->bounds.t, g->bounds.r, g->bounds.b); }
    return 0;
}

static float line_w(gv_t *g, int s, int e)
{
    while (e > s && (g->cp[e - 1] == ' ' || g->cp[e - 1] == '\n')) e--;
    return run_width(&g->font, g->cp + s, e - s);
}
static void gv_wrap(gv_t *g, float width)
{
    g->nlines = 0;
    int s = 0;
    while (s <= g->n && g->nlines < GV_MAXLINES) {
        int e = s, lastbreak = -1;
        float w = 0;
        while (e < g->n && g->cp[e] != '\n') {
            float aw = glyph_adv(&g->font, g->cp[e], e + 1 < g->n ? g->cp[e + 1] : 0);
            if (width > 0 && w + aw > width && g->cp[e] != ' ') {
                if (lastbreak > s) e = lastbreak;
                else if (e == s) e++;
                break;
            }
            if (g->cp[e] == ' ') lastbreak = e + 1;
            w += aw; e++;
        }
        int end = e;
        g->ls[g->nlines] = s; g->le[g->nlines] = end; g->nlines++;
        if (e < g->n && g->cp[e] == '\n') e++;
        while (e < g->n && g->cp[e] == ' ' && end == e) e++;
        if (e >= g->n) break;
        s = e;
    }
    if (!g->nlines) { g->nlines = 1; g->ls[0] = g->le[0] = 0; }
}
/* cut line l to fit width with a trailing ellipsis */
static void gv_ellipsize(gv_t *g, int l, float width)
{
    u32 ell = 0x2026;
    if (!stbtt_FindGlyphIndex(&g->font.t->fi, (int)ell)) ell = '.';
    float ew = glyph_adv(&g->font, ell, 0) * (ell == '.' ? 3 : 1);
    int s = g->ls[l], e = g->n;
    if (run_width(&g->font, g->cp + s, e - s) <= width) { g->le[l] = e; return; }
    while (e > s && run_width(&g->font, g->cp + s, e - s) + ew > width) e--;
    while (e > s && g->cp[e - 1] == ' ') e--;
    int k = e;
    if (ell == '.') { for (int i = 0; i < 3 && k < GV_MAXCP; i++) g->cp[k++] = '.'; }
    else if (k < GV_MAXCP) g->cp[k++] = ell;
    g->n = k;
    g->le[l] = k;
}

static u64 j_gv_init_para(cpu_t *c, u32 self, jargs_t *a)
{
    u32 fo = ja_i32(a), txt = ja_i32(a);
    float width = ja_f32(a), height = ja_f32(a);
    u32 brk = ja_i32(a), align = ja_i32(a);
    font_t *f = jo_native(fo);
    if (!f) return 0;
    if (width < 0) width = 0;
    if (height < 0) height = 0;
    gv_t *g = calloc(1, sizeof(*g));
    g->font = *f;
    g->n = utf8_decode(jstr(txt), g->cp, GV_MAXCP);
    g->layout = true;
    g->lh = f->ascent + f->descent;
    if (brk == 3 || brk == 4 || brk == 5) {                       /* single line, truncated (tail) */
        for (int i = 0; i < g->n; i++) if (g->cp[i] == '\n') g->cp[i] = ' ';
        g->nlines = 1; g->ls[0] = 0;
        gv_ellipsize(g, 0, width);
    } else {
        gv_wrap(g, width);
        if ((brk == 2 || brk == 6) && height > 0) {               /* clip / truncate by height */
            int keep = g->nlines;
            while (keep > 1 && keep * g->lh > height) keep--;
            if (keep < g->nlines) {
                if (brk == 6) { g->n = g->le[g->nlines - 1]; gv_ellipsize(g, keep - 1, width); }
                g->nlines = keep;
            }
        }
    }
    rect_t b = { 0 };
    for (int l = 0; l < g->nlines; l++) {
        float w = line_w(g, g->ls[l], g->le[l]);
        float x = align == 1 ? width - w : align == 2 ? (width - w) * 0.5f : 0;
        g->lx[l] = x;
        rect_add(&b, (int)floorf(x), (int)(l * g->lh), (int)ceilf(x + w), (int)((l + 1) * g->lh));
        if (!b.any) { b.l = (int)floorf(x); b.r = (int)ceilf(x + w); b.t = 0; b.b = (int)((l + 1) * g->lh); b.any = true; }
    }
    float base0 = f->ascent;
    rect_t fl = run_bounds(f, g->cp + g->ls[0], g->le[0] - g->ls[0]);
    if ((int)(base0 + fl.t) < b.t) b.t = (int)(base0 + fl.t);
    b.t--; b.b++;
    g->bounds = b;
    jo_set_native(self, g);
    gv_publish(self, g, 0, base0 - b.t);
    return 0;
}

/* draw every line into a coverage buffer (origin = bounds top-left) */
static void gv_render(gv_t *g, u8 *buf, int w, int h, float scale, float tx, float ty, bool flip)
{
    for (int l = 0; l < g->nlines; l++) {
        float bx = g->layout ? g->lx[l] : 0, by = g->layout ? g->font.ascent + l * g->lh : 0;
        run_draw(&g->font, g->cp + g->ls[l], g->le[l] - g->ls[l],
                 tx + (bx - g->bounds.l) * scale, ty + (by - g->bounds.t) * scale, scale, buf, w, h, flip);
    }
}

static u64 j_gv_create_texture(cpu_t *c, u32 self, jargs_t *a)
{
    gv_t *g = jo_native(self);
    if (!g) return 0;
    int bw = g->bounds.r - g->bounds.l, bh = g->bounds.b - g->bounds.t;
    int tw = 1, th = 1;
    while (tw < bw) tw *= 2;
    while (th < bh) th *= 2;
    if (tw > 4096 || th > 4096) return 0;
    u8 *cov = calloc((size_t)tw * th, 1);
    gv_render(g, cov, tw, th, 1.0f, 0, 0, false);
    u8 *rgba = malloc((size_t)tw * th * 4);
    for (int i = 0; i < tw * th; i++) { u8 v = cov[i]; rgba[4 * i] = rgba[4 * i + 1] = rgba[4 * i + 2] = rgba[4 * i + 3] = v; }  /* premultiplied white */
    u32 tex = glhost_text_texture(0, tw, th, rgba, 0x1908 /* GL_RGBA */);
    free(cov); free(rgba);
    set_i(self, GV_CLS, "texWidth", tw);
    set_i(self, GV_CLS, "texHeight", th);
    set_i(self, GV_CLS, "texId", (s32)tex);
    return 1;
}

static u64 j_gv_render_to_texture(cpu_t *c, u32 self, jargs_t *a)
{
    gv_t *g = jo_native(self);
    u32 tex = ja_i32(a), w = ja_i32(a), h = ja_i32(a);
    float s = ja_f32(a);
    if (!g || !w || !h || w > 4096 || h > 4096) return 0;
    u8 *cov = calloc((size_t)w * h, 1);
    float bw = (float)(g->bounds.r - g->bounds.l), bh = (float)(g->bounds.b - g->bounds.t);
    gv_render(g, cov, (int)w, (int)h, s, ((float)w - bw * s) * 0.5f, ((float)h - bh * s) * 0.5f, true);
    glhost_text_texture(tex, (int)w, (int)h, cov, 0x1909 /* GL_LUMINANCE */);
    free(cov);
    return 1;
}
