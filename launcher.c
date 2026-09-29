/*
 * launcher.c - settings screen shown before the game boots: game files and video
 * options, drawn with GLES2 in the game window. Settings persist in
 * ~/.config/rr2emu.cfg; an APK or OBB/zip is unpacked once into ~/.cache/rr2emu.
 */
#include "emu.h"
#ifdef _WIN32
#include <windows.h>
#endif
#include <dlfcn.h>
#include <dirent.h>
#include <strings.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

#define W 800
#define H LAUNCHER_H
#define SO_REL "lib/armeabi/libEAMRealRacing2.so"
#define SO3_REL "lib/armeabi-v7a/libRealRacing3.so"

static const u8 font5x8[95][8] = {
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    { 0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04, 0x00 },
    { 0x0e, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04, 0x00 },
    { 0x0e, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04, 0x00 },
    { 0x0e, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04, 0x00 },
    { 0x0e, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04, 0x00 },
    { 0x0e, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04, 0x00 },
    { 0x04, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    { 0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02, 0x00 },
    { 0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08, 0x00 },
    { 0x0e, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04, 0x00 },
    { 0x00, 0x04, 0x04, 0x1f, 0x04, 0x04, 0x00, 0x00 },
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x04, 0x08 },
    { 0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x0c, 0x00 },
    { 0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x00, 0x00 },
    { 0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e, 0x00 },
    { 0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e, 0x00 },
    { 0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f, 0x00 },
    { 0x1f, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0e, 0x00 },
    { 0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02, 0x00 },
    { 0x1f, 0x10, 0x1e, 0x01, 0x01, 0x11, 0x0e, 0x00 },
    { 0x06, 0x08, 0x10, 0x1e, 0x11, 0x11, 0x0e, 0x00 },
    { 0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08, 0x00 },
    { 0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e, 0x00 },
    { 0x0e, 0x11, 0x11, 0x0f, 0x01, 0x02, 0x0c, 0x00 },
    { 0x00, 0x04, 0x04, 0x00, 0x04, 0x04, 0x00, 0x00 },
    { 0x0e, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04, 0x00 },
    { 0x02, 0x04, 0x08, 0x10, 0x08, 0x04, 0x02, 0x00 },
    { 0x00, 0x00, 0x1f, 0x00, 0x1f, 0x00, 0x00, 0x00 },
    { 0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08, 0x00 },
    { 0x0e, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04, 0x00 },
    { 0x0e, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04, 0x00 },
    { 0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11, 0x00 },
    { 0x1e, 0x11, 0x11, 0x1e, 0x11, 0x11, 0x1e, 0x00 },
    { 0x0e, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0e, 0x00 },
    { 0x1e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1e, 0x00 },
    { 0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f, 0x00 },
    { 0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x10, 0x00 },
    { 0x0e, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0f, 0x00 },
    { 0x11, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11, 0x00 },
    { 0x0e, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0e, 0x00 },
    { 0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0c, 0x00 },
    { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11, 0x00 },
    { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f, 0x00 },
    { 0x11, 0x1b, 0x15, 0x15, 0x11, 0x11, 0x11, 0x00 },
    { 0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x00 },
    { 0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e, 0x00 },
    { 0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10, 0x00 },
    { 0x0e, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0d, 0x00 },
    { 0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11, 0x00 },
    { 0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e, 0x00 },
    { 0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x00 },
    { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e, 0x00 },
    { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0a, 0x04, 0x00 },
    { 0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0a, 0x00 },
    { 0x11, 0x11, 0x0a, 0x04, 0x0a, 0x11, 0x11, 0x00 },
    { 0x11, 0x11, 0x0a, 0x04, 0x04, 0x04, 0x04, 0x00 },
    { 0x1f, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1f, 0x00 },
    { 0x0e, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04, 0x00 },
    { 0x0e, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04, 0x00 },
    { 0x0e, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04, 0x00 },
    { 0x0e, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04, 0x00 },
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1f },
    { 0x0e, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04, 0x00 },
    { 0x00, 0x00, 0x0e, 0x01, 0x0f, 0x11, 0x0f, 0x00 },
    { 0x10, 0x10, 0x1e, 0x11, 0x11, 0x11, 0x1e, 0x00 },
    { 0x00, 0x00, 0x0e, 0x10, 0x10, 0x10, 0x0e, 0x00 },
    { 0x01, 0x01, 0x0f, 0x11, 0x11, 0x11, 0x0f, 0x00 },
    { 0x00, 0x00, 0x0e, 0x11, 0x1f, 0x10, 0x0e, 0x00 },
    { 0x06, 0x08, 0x1e, 0x08, 0x08, 0x08, 0x08, 0x00 },
    { 0x00, 0x00, 0x0f, 0x11, 0x11, 0x0f, 0x01, 0x0e },
    { 0x10, 0x10, 0x1e, 0x11, 0x11, 0x11, 0x11, 0x00 },
    { 0x04, 0x00, 0x0c, 0x04, 0x04, 0x04, 0x0e, 0x00 },
    { 0x02, 0x00, 0x06, 0x02, 0x02, 0x02, 0x12, 0x0c },
    { 0x10, 0x10, 0x12, 0x14, 0x18, 0x14, 0x12, 0x00 },
    { 0x0c, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0e, 0x00 },
    { 0x00, 0x00, 0x1a, 0x15, 0x15, 0x15, 0x15, 0x00 },
    { 0x00, 0x00, 0x1e, 0x11, 0x11, 0x11, 0x11, 0x00 },
    { 0x00, 0x00, 0x0e, 0x11, 0x11, 0x11, 0x0e, 0x00 },
    { 0x00, 0x00, 0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10 },
    { 0x00, 0x00, 0x0f, 0x11, 0x11, 0x0f, 0x01, 0x01 },
    { 0x00, 0x00, 0x16, 0x19, 0x10, 0x10, 0x10, 0x00 },
    { 0x00, 0x00, 0x0f, 0x10, 0x0e, 0x01, 0x1e, 0x00 },
    { 0x08, 0x08, 0x1e, 0x08, 0x08, 0x09, 0x06, 0x00 },
    { 0x00, 0x00, 0x11, 0x11, 0x11, 0x13, 0x0d, 0x00 },
    { 0x00, 0x00, 0x11, 0x11, 0x11, 0x0a, 0x04, 0x00 },
    { 0x00, 0x00, 0x11, 0x11, 0x15, 0x15, 0x0a, 0x00 },
    { 0x00, 0x00, 0x11, 0x0a, 0x04, 0x0a, 0x11, 0x00 },
    { 0x00, 0x00, 0x11, 0x11, 0x11, 0x0f, 0x01, 0x0e },
    { 0x00, 0x00, 0x1f, 0x02, 0x04, 0x08, 0x1f, 0x00 },
    { 0x0e, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04, 0x00 },
    { 0x0e, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04, 0x00 },
    { 0x0e, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04, 0x00 },
    { 0x00, 0x00, 0x08, 0x15, 0x02, 0x00, 0x00, 0x00 },
};

typedef unsigned GLenum, GLuint;
typedef int GLint;
typedef float GLfloat;
static struct {
    GLuint (*CreateShader)(GLenum);
    void (*ShaderSource)(GLuint, int, const char *const *, const GLint *);
    void (*CompileShader)(GLuint);
    GLuint (*CreateProgram)(void);
    void (*AttachShader)(GLuint, GLuint);
    void (*BindAttribLocation)(GLuint, GLuint, const char *);
    void (*LinkProgram)(GLuint);
    void (*UseProgram)(GLuint);
    void (*VertexAttribPointer)(GLuint, GLint, GLenum, unsigned char, int, const void *);
    void (*EnableVertexAttribArray)(GLuint);
    void (*DisableVertexAttribArray)(GLuint);
    void (*DrawArrays)(GLenum, GLint, int);
    void (*ClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
    void (*Clear)(unsigned);
    void (*Viewport)(GLint, GLint, int, int);
    void (*DeleteProgram)(GLuint);
    void (*DeleteShader)(GLuint);
    void (*BindBuffer)(GLenum, GLuint);
    void (*GenTextures)(int, GLuint *);
    void (*BindTexture)(GLenum, GLuint);
    void (*TexImage2D)(GLenum, GLint, GLint, int, int, GLint, GLenum, GLenum, const void *);
    void (*TexParameteri)(GLenum, GLenum, GLint);
    void (*DeleteTextures)(int, const GLuint *);
    void (*PixelStorei)(GLenum, GLint);
    void (*Enable)(GLenum);
    void (*Disable)(GLenum);
    void (*BlendFunc)(GLenum, GLenum);
} gl;
static const char *const gl_names[] = {
    "glCreateShader", "glShaderSource", "glCompileShader", "glCreateProgram", "glAttachShader",
    "glBindAttribLocation", "glLinkProgram", "glUseProgram", "glVertexAttribPointer",
    "glEnableVertexAttribArray", "glDisableVertexAttribArray", "glDrawArrays", "glClearColor", "glClear",
    "glViewport", "glDeleteProgram", "glDeleteShader", "glBindBuffer", "glGenTextures", "glBindTexture",
    "glTexImage2D", "glTexParameteri", "glDeleteTextures", "glPixelStorei", "glEnable", "glDisable", "glBlendFunc",
};

/* one batch per frame: position, atlas uv, rgba */
#define MAXV 60000
static GLfloat vb[MAXV * 8];
static int nv;
static GLuint prog, shaders[2], atlas_tex;

#define AT 1024
static u8 atlas[AT * AT];
static int ax = 8, ay = 0, arow;

typedef struct { short x, y, w, h, left, top, adv; } glyph_t;
typedef struct { glyph_t g[95]; int asc, height; } font_t;
enum { F_SMALL, F_BODY, F_BOLD, F_BTN, F_HEAD, F_LOGO, NFONTS };
static font_t fonts[NFONTS];

static void vtx(GLfloat x, GLfloat y, GLfloat u, GLfloat v, u32 c)
{
    if (nv >= MAXV) return;
    GLfloat *p = vb + nv++ * 8;
    p[0] = x; p[1] = y; p[2] = u / AT; p[3] = v / AT;
    p[4] = (c >> 24) / 255.0f; p[5] = (c >> 16 & 255) / 255.0f; p[6] = (c >> 8 & 255) / 255.0f; p[7] = (c & 255) / 255.0f;
}

static u32 mix(u32 a, u32 b, float t)
{
    u32 r = 0;
    for (int s = 0; s < 32; s += 8) {
        float x = (a >> s & 255) * (1 - t) + (b >> s & 255) * t;
        r |= (u32)(x + 0.5f) << s;
    }
    return r;
}

static void tri(GLfloat ax_, GLfloat ay_, GLfloat bx, GLfloat by, GLfloat cx, GLfloat cy, u32 c)
{
    vtx(ax_, ay_, 1, 1, c); vtx(bx, by, 1, 1, c); vtx(cx, cy, 1, 1, c);
}

/* vertical gradient quad; skew shifts the top edge right (GT-style slanted bands) */
static void grad(GLfloat x, GLfloat y, GLfloat w, GLfloat h, u32 top, u32 bot, GLfloat skew)
{
    vtx(x + skew, y, 1, 1, top); vtx(x + w + skew, y, 1, 1, top); vtx(x, y + h, 1, 1, bot);
    vtx(x + w + skew, y, 1, 1, top); vtx(x + w, y + h, 1, 1, bot); vtx(x, y + h, 1, 1, bot);
}

static void rrect(GLfloat x, GLfloat y, GLfloat w, GLfloat h, GLfloat r, u32 top, u32 bot)
{
    grad(x, y + r, w, h - 2 * r, mix(top, bot, r / h), mix(top, bot, 1 - r / h), 0);
    grad(x + r, y, w - 2 * r, r, top, mix(top, bot, r / h), 0);
    grad(x + r, y + h - r, w - 2 * r, r, mix(top, bot, 1 - r / h), bot, 0);
    static const float cs[7] = { 1, 0.9659f, 0.8660f, 0.7071f, 0.5f, 0.2588f, 0 };
    for (int q = 0; q < 4; q++) {
        GLfloat cx = q & 1 ? x + w - r : x + r, cy = q & 2 ? y + h - r : y + r;
        GLfloat sx = q & 1 ? 1 : -1, sy = q & 2 ? 1 : -1;
        for (int i = 0; i < 6; i++) {
            GLfloat x0 = cx + sx * r * cs[i], y0 = cy + sy * r * cs[6 - i];
            GLfloat x1 = cx + sx * r * cs[i + 1], y1 = cy + sy * r * cs[5 - i];
            u32 cc = mix(top, bot, (cy - y) / h), c0 = mix(top, bot, (y0 - y) / h), c1 = mix(top, bot, (y1 - y) / h);
            vtx(cx, cy, 1, 1, cc); vtx(x0, y0, 1, 1, c0); vtx(x1, y1, 1, 1, c1);
        }
    }
}

static void frame(GLfloat x, GLfloat y, GLfloat w, GLfloat h, u32 c)
{
    grad(x, y, w, 1, c, c, 0); grad(x, y + h - 1, w, 1, c, c, 0);
    grad(x, y, 1, h, c, c, 0); grad(x + w - 1, y, 1, h, c, c, 0);
}

static int text_w(int f, const char *s)
{
    int w = 0;
    for (; *s; s++) { unsigned ch = (u8)*s; w += fonts[f].g[(ch < 32 || ch > 126 ? '?' : ch) - 32].adv; }
    return w;
}

/* y is the top of the line box; slant > 0 gives an italic lean */
static void text(int f, GLfloat x, GLfloat y, u32 c, const char *s, GLfloat slant)
{
    const font_t *ft = &fonts[f];
    for (; *s; s++) {
        unsigned ch = (u8)*s;
        const glyph_t *g = &ft->g[(ch < 32 || ch > 126 ? '?' : ch) - 32];
        if (g->w) {
            GLfloat x0 = x + g->left, y0 = y + ft->asc - g->top, x1 = x0 + g->w, y1 = y0 + g->h;
            GLfloat s0 = slant * (ft->asc - (y0 - y)), s1 = slant * (ft->asc - (y1 - y));
            vtx(x0 + s0, y0, g->x, g->y, c); vtx(x1 + s0, y0, g->x + g->w, g->y, c); vtx(x0 + s1, y1, g->x, g->y + g->h, c);
            vtx(x1 + s0, y0, g->x + g->w, g->y, c); vtx(x1 + s1, y1, g->x + g->w, g->y + g->h, c); vtx(x0 + s1, y1, g->x, g->y + g->h, c);
        }
        x += g->adv;
    }
}

static bool atlas_put(int w, int h, int *ox, int *oy)
{
    if (ax + w + 1 > AT) { ax = 0; ay += arow + 1; arow = 0; }
    if (ay + h > AT) return false;
    *ox = ax; *oy = ay;
    ax += w + 1;
    if (h > arow) arow = h;
    return true;
}

/* FreeType through dlopen; offsets are the stable x86-64 FT_FaceRec/FT_GlyphSlotRec layout */
static void *ft_lib;
static int (*ft_new_face)(void *, const char *, long, void **);
static int (*ft_set_size)(void *, unsigned, unsigned);
static int (*ft_load_char)(void *, unsigned long, int);

static bool font_ft(font_t *ft, const char *const *files, int px)
{
    if (!ft_lib) {
        void *l = dlopen("libfreetype.so.6", RTLD_NOW | RTLD_LOCAL);
        int (*init)(void **) = l ? dlsym(l, "FT_Init_FreeType") : NULL;
        *(void **)&ft_new_face = l ? dlsym(l, "FT_New_Face") : NULL;
        *(void **)&ft_set_size = l ? dlsym(l, "FT_Set_Pixel_Sizes") : NULL;
        *(void **)&ft_load_char = l ? dlsym(l, "FT_Load_Char") : NULL;
        if (!init || !ft_new_face || !ft_set_size || !ft_load_char || init(&ft_lib)) { ft_lib = NULL; return false; }
    }
    void *face = NULL;
    for (; *files && !face; files++) if (ft_new_face(ft_lib, *files, 0, &face)) face = NULL;
    if (!face || ft_set_size(face, 0, px)) return false;
    const char *size = *(char **)((char *)face + 160);
    ft->asc = (int)(*(long *)(size + 48) >> 6);
    ft->height = (int)(*(long *)(size + 64) >> 6);
    for (int ch = 32; ch < 127; ch++) {
        glyph_t *g = &ft->g[ch - 32];
        memset(g, 0, sizeof(*g));
        if (ft_load_char(face, ch, 4 /* FT_LOAD_RENDER */)) continue;
        const char *slot = *(char **)((char *)face + 152);
        unsigned rows = *(unsigned *)(slot + 152), width = *(unsigned *)(slot + 156);
        int pitch = *(int *)(slot + 160);
        const u8 *buf = *(u8 **)(slot + 168);
        g->adv = (short)(*(long *)(slot + 128) >> 6);
        g->left = (short)*(int *)(slot + 192);
        g->top = (short)*(int *)(slot + 196);
        int ox, oy;
        if (!width || !rows || !atlas_put(width, rows, &ox, &oy)) continue;
        for (unsigned r = 0; r < rows; r++) memcpy(atlas + (oy + r) * AT + ox, buf + r * pitch, width);
        g->x = ox; g->y = oy; g->w = width; g->h = rows;
    }
    return true;
}

static void font_bitmap(font_t *ft, int px)
{
    int sc = px / 8 > 0 ? px / 8 : 1;
    ft->asc = 7 * sc;
    ft->height = 9 * sc;
    for (int ch = 32; ch < 127; ch++) {
        glyph_t *g = &ft->g[ch - 32];
        memset(g, 0, sizeof(*g));
        g->adv = 6 * sc;
        g->top = 7 * sc;
        int ox, oy;
        if (ch == ' ' || !atlas_put(5 * sc, 8 * sc, &ox, &oy)) continue;
        for (int y = 0; y < 8 * sc; y++)
            for (int x = 0; x < 5 * sc; x++)
                atlas[(oy + y) * AT + ox + x] = font5x8[ch - 32][y / sc] >> (4 - x / sc) & 1 ? 255 : 0;
        g->x = ox; g->y = oy; g->w = 5 * sc; g->h = 8 * sc;
    }
}

#define FONT_DIR "/usr/share/fonts/"
static const char *const f_regular[] = { FONT_DIR "opentype/urw-base35/NimbusSans-Regular.otf",
    FONT_DIR "truetype/liberation/LiberationSans-Regular.ttf", FONT_DIR "truetype/dejavu/DejaVuSans.ttf", NULL };
static const char *const f_bold[] = { FONT_DIR "opentype/urw-base35/NimbusSans-Bold.otf",
    FONT_DIR "truetype/liberation/LiberationSans-Bold.ttf", FONT_DIR "truetype/dejavu/DejaVuSans-Bold.ttf", NULL };
static const char *const f_bolditalic[] = { FONT_DIR "opentype/urw-base35/NimbusSans-BoldItalic.otf",
    FONT_DIR "truetype/liberation/LiberationSans-BoldItalic.ttf", NULL };
static float logo_slant;

#ifdef _WIN32
/* Windows: GDI rasterizes the system's Arial, so no FreeType DLL is needed */
static bool font_gdi(font_t *ft, int px, int bold, int italic)
{
    HDC dc = CreateCompatibleDC(NULL);
    HFONT f = CreateFontA(-px, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL, italic, 0, 0, ANSI_CHARSET, OUT_TT_PRECIS,
                          CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, "Arial");
    if (!dc || !f) return false;
    HGDIOBJ old = SelectObject(dc, f);
    TEXTMETRICA tm;
    GetTextMetricsA(dc, &tm);
    ft->asc = tm.tmAscent;
    ft->height = tm.tmHeight + tm.tmExternalLeading;
    static const MAT2 m = { { 0, 1 }, { 0, 0 }, { 0, 0 }, { 0, 1 } };
    static u8 buf[256 * 256];
    for (int ch = 32; ch < 127; ch++) {
        glyph_t *g = &ft->g[ch - 32];
        memset(g, 0, sizeof(*g));
        GLYPHMETRICS gm;
        DWORD n = GetGlyphOutlineA(dc, (UINT)ch, GGO_GRAY8_BITMAP, &gm, sizeof(buf), buf, &m);
        INT adv = 0;
        g->adv = GetCharWidth32A(dc, (UINT)ch, (UINT)ch, &adv) ? (short)adv : (short)gm.gmCellIncX;
        if (n == GDI_ERROR || !n || !gm.gmBlackBoxX) continue;
        int w = (int)gm.gmBlackBoxX, h = (int)gm.gmBlackBoxY, pitch = (w + 3) & ~3, ox, oy;
        if (!atlas_put(w, h, &ox, &oy)) continue;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                int v = buf[y * pitch + x] * 255 / 64;           /* GGO_GRAY8: 0..64 */
                atlas[(oy + y) * AT + ox + x] = (u8)(v > 255 ? 255 : v);
            }
        g->x = ox; g->y = oy; g->w = w; g->h = h;
        g->left = (short)gm.gmptGlyphOrigin.x;
        g->top = (short)gm.gmptGlyphOrigin.y;
    }
    SelectObject(dc, old);
    DeleteObject(f);
    DeleteDC(dc);
    return true;
}
#endif

static void fonts_load(void)
{
    static const struct { const char *const *files; int px; } spec[NFONTS] = {
        { f_regular, 14 }, { f_regular, 18 }, { f_bold, 18 }, { f_bold, 21 }, { f_bolditalic, 17 }, { f_bolditalic, 38 } };
    memset(atlas, 255, 4 * AT);
    for (int i = 0; i < NFONTS; i++)
#ifdef _WIN32
        if (!font_gdi(&fonts[i], spec[i].px, spec[i].files != f_regular, spec[i].files == f_bolditalic)) {
#else
        if (!font_ft(&fonts[i], spec[i].files, spec[i].px)) {
#endif
            font_bitmap(&fonts[i], spec[i].px);
            if (i >= F_HEAD) logo_slant = 0.2f;
        }
#ifndef _WIN32
    if (!ft_new_face) LOG("[launcher] FreeType not found, using the built-in pixel font\n");
#endif
}

static void flush(void)
{
    if (!nv) return;
    gl.VertexAttribPointer(0, 2, 0x1406, 0, 32, vb);
    gl.VertexAttribPointer(1, 2, 0x1406, 0, 32, vb + 2);
    gl.VertexAttribPointer(2, 4, 0x1406, 0, 32, vb + 4);
    gl.DrawArrays(4, 0, nv);
    nv = 0;
}

static bool gl_setup(void)
{
    void **fp = (void **)&gl;
    for (unsigned i = 0; i < sizeof(gl_names) / sizeof(gl_names[0]); i++)
        if (!(fp[i] = host_gl_proc(gl_names[i]))) return false;
    static const char *vs =
        "attribute vec2 p; attribute vec2 t; attribute vec4 c; varying vec2 vt; varying vec4 vc;"
        "void main() { gl_Position = vec4(p.x / 400.0 - 1.0, 1.0 - p.y / 320.0, 0.0, 1.0); vt = t; vc = c; }";
    static const char *fs =
        "#ifdef GL_ES\nprecision mediump float;\n#endif\nuniform sampler2D s; varying vec2 vt; varying vec4 vc;"
        "void main() { gl_FragColor = vec4(vc.rgb, vc.a * texture2D(s, vt).a); }";
    shaders[0] = gl.CreateShader(0x8B31);
    shaders[1] = gl.CreateShader(0x8B30);
    gl.ShaderSource(shaders[0], 1, &vs, NULL);
    gl.ShaderSource(shaders[1], 1, &fs, NULL);
    prog = gl.CreateProgram();
    for (int i = 0; i < 2; i++) { gl.CompileShader(shaders[i]); gl.AttachShader(prog, shaders[i]); }
    gl.BindAttribLocation(prog, 0, "p");
    gl.BindAttribLocation(prog, 1, "t");
    gl.BindAttribLocation(prog, 2, "c");
    gl.LinkProgram(prog);
    gl.UseProgram(prog);
    fonts_load();
    gl.GenTextures(1, &atlas_tex);
    gl.BindTexture(0x0DE1, atlas_tex);
    gl.PixelStorei(0x0CF5, 1);
    gl.TexImage2D(0x0DE1, 0, 0x1906, AT, AT, 0, 0x1906, 0x1401, atlas);
    gl.TexParameteri(0x0DE1, 0x2801, 0x2600);
    gl.TexParameteri(0x0DE1, 0x2800, 0x2600);
    gl.BindBuffer(0x8892, 0);
    for (int i = 0; i < 3; i++) gl.EnableVertexAttribArray(i);
    gl.Enable(0x0BE2);
    gl.BlendFunc(0x0302, 0x0303);
    return true;
}

static void gl_teardown(void)
{
    for (int i = 0; i < 3; i++) gl.DisableVertexAttribArray(i);
    gl.Disable(0x0BE2);
    gl.PixelStorei(0x0CF5, 4);
    gl.BindTexture(0x0DE1, 0);
    gl.DeleteTextures(1, &atlas_tex);
    gl.UseProgram(0);
    gl.DeleteProgram(prog);
    gl.DeleteShader(shaders[0]);
    gl.DeleteShader(shaders[1]);
    gl.ClearColor(0, 0, 0, 1);
    gl.Clear(0x4000);
    host_swap();
}

/* ---- settings ---- */

enum { R_GAME, R_APK, R_DATA, R_RES, R_FULL, R_ANISO, R_VSYNC, R_ASSIST, R_TILT, R_FOV, R_PLAY, R_CTRL, R_QUIT, NROWS };
#define R_LAST_OPT R_FOV
static const char *const labels[] = { "Game", "Game APK", "Game data (OBB)", "Resolution", "Fullscreen", "Anisotropic", "VSync",
                                      "Disable assists", "Horizon tilt", "Cockpit FOV" };
/* apk/data: paths for the selected game; other_*: the other game's, swapped in on a switch */
static char apk[1024], data[1024], other_apk[1024], other_data[1024], edit[1024], status[256];
static int game = 2;
static u32 status_rgb;
static struct { int w, h; } res[16];
static int nres, ires, native_w = 1920, native_h = 1080;
static int aniso_opts[5] = { 0, 2, 4, 8, 16 }, naniso = 1, ianiso;
static int sel = R_PLAY, editing, page;
static int csel_r, csel_c, capturing;   /* controls page: cursor row/column, waiting for an input */
#define FOV_MIN (-10)
#define FOV_MAX 40
#define FOV_STEP 5

/* config: $XDG_CONFIG_HOME or ~/.config (Windows: %APPDATA%\rr2emu); cache: ~/.cache (%LOCALAPPDATA%) */
static void home_path(char *out, size_t n, const char *xdg, const char *fallback, const char *leaf)
{
#ifdef _WIN32
    int config = !strcmp(xdg, "XDG_CONFIG_HOME");
    const char *b = getenv(config ? "APPDATA" : "LOCALAPPDATA");
    snprintf(out, n, config ? "%s/rr2emu/%s" : "%s/%s", b ? b : ".", leaf);
    (void)fallback;
#else
    const char *x = getenv(xdg), *h = getenv("HOME");
    if (x && *x) snprintf(out, n, "%s/%s", x, leaf);
    else snprintf(out, n, "%s/%s/%s", h ? h : ".", fallback, leaf);
#endif
}

#define rr2_apk (game == 2 ? apk : other_apk)
#define rr2_data (game == 2 ? data : other_data)
#define rr3_apk (game == 3 ? apk : other_apk)
#define rr3_data (game == 3 ? data : other_data)

static void cfg_load(void)
{
    char path[1024], line[1200];
    home_path(path, sizeof(path), "XDG_CONFIG_HOME", ".config", "rr2emu.cfg");
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *v = strchr(line, '=');
        if (!v) continue;
        *v++ = 0;
        int a;
        if (!strcmp(line, "apk")) snprintf(rr2_apk, sizeof(apk), "%s", v);
        else if (!strcmp(line, "data")) snprintf(rr2_data, sizeof(apk), "%s", v);
        else if (!strcmp(line, "rr3_apk")) snprintf(rr3_apk, sizeof(apk), "%s", v);
        else if (!strcmp(line, "rr3_data")) snprintf(rr3_data, sizeof(apk), "%s", v);
        else if (!strcmp(line, "game")) game = atoi(v) == 3 ? 3 : 2;
        else if (!strcmp(line, "width")) G.width = atoi(v);
        else if (!strcmp(line, "height")) G.height = atoi(v);
        else if (!strcmp(line, "fullscreen")) G.fullscreen = atoi(v);
        else if (!strcmp(line, "aniso")) G.aniso = atoi(v);
        else if (!strcmp(line, "vsync")) G.vsync = atoi(v);
        else if (!strcmp(line, "no_assists")) G.no_assists = atoi(v);
        else if (!strcmp(line, "no_tilt")) G.no_tilt = atoi(v);
        else if (!strcmp(line, "cockpit_fov")) G.cockpit_fov = atoi(v);
        else if (sscanf(line, "bind%d", &a) == 1 && a >= 0 && a < ACT_COUNT)
            sscanf(v, "%d,%d,%d,%d", &g_binds[a][0], &g_binds[a][1], &g_binds[a][2], &g_binds[a][3]);
    }
    fclose(f);
}

static void mkdirs(const char *path)
{
    char p[1100];
    snprintf(p, sizeof(p), "%s", path);
    for (char *s = p + 1; *s; s++)
        if (strchr(PATH_SEP_CHARS, *s)) { char k = *s; *s = 0; emu_mkdir(p, 0755); *s = k; }
    emu_mkdir(p, 0755);
}

/* every change is saved right away */
static void cfg_save(void)
{
    G.width = res[ires].w;
    G.height = res[ires].h;
    G.aniso = aniso_opts[ianiso];
    char path[1024];
    home_path(path, sizeof(path), "XDG_CONFIG_HOME", ".config", "");
    mkdirs(path);
    strcat(path, "rr2emu.cfg");
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "game=%d\napk=%s\ndata=%s\nrr3_apk=%s\nrr3_data=%s\nwidth=%d\nheight=%d\nfullscreen=%d\naniso=%d\nvsync=%d\n"
               "no_assists=%d\nno_tilt=%d\ncockpit_fov=%d\n",
            game, rr2_apk, rr2_data, rr3_apk, rr3_data, G.width, G.height, G.fullscreen, G.aniso, G.vsync, G.no_assists, G.no_tilt, G.cockpit_fov);
    for (int a = 0; a < ACT_COUNT; a++)
        fprintf(f, "bind%d=%d,%d,%d,%d\n", a, g_binds[a][0], g_binds[a][1], g_binds[a][2], g_binds[a][3]);
    fclose(f);
}

static void build_lists(void)
{
    static const int base[][2] = { { 800, 480 }, { 1024, 600 }, { 1280, 720 }, { 1280, 768 }, { 1366, 768 },
                                   { 1600, 900 }, { 1920, 1080 }, { 2560, 1440 }, { 3840, 2160 } };
    host_desktop_size(&native_w, &native_h);
    nres = 0;
    for (unsigned i = 0; i < sizeof(base) / sizeof(base[0]); i++) {
        int w = base[i][0], h = base[i][1];
        if (w > native_w || h > native_h) continue;
        if (nres && res[nres - 1].w * res[nres - 1].h < native_w * native_h && w * h > native_w * native_h)
            res[nres].w = native_w, res[nres++].h = native_h;
        if (w == native_w && h == native_h) continue;
        res[nres].w = w, res[nres++].h = h;
    }
    res[nres].w = native_w, res[nres++].h = native_h;
    for (int i = 0; i + 1 < nres; i++)
        if (res[i].w * res[i].h > res[nres - 1].w * res[nres - 1].h) {
            int w = res[nres - 1].w, h = res[nres - 1].h;
            memmove(&res[i + 1], &res[i], (nres - 1 - i) * sizeof(res[0]));
            res[i].w = w, res[i].h = h;
            break;
        }
    ires = -1;
    for (int i = 0; i < nres; i++) if (res[i].w == G.width && res[i].h == G.height) ires = i;
    if (ires < 0 && nres < 16 && G.width > 0 && G.height > 0) { res[nres].w = G.width, res[nres].h = G.height; ires = nres++; }
    if (ires < 0) ires = 0;

    float amax = glhost_max_aniso();
    naniso = 1;
    for (int i = 1; i < 5; i++) if (aniso_opts[i] <= amax) naniso = i + 1;
    ianiso = 0;
    for (int i = 0; i < naniso; i++) if (aniso_opts[i] <= G.aniso) ianiso = i;
    if (G.cockpit_fov < FOV_MIN) G.cockpit_fov = FOV_MIN;
    if (G.cockpit_fov > FOV_MAX) G.cockpit_fov = FOV_MAX;
}

static void set_status(u32 rgb, const char *fmt, const char *arg)
{
    snprintf(status, sizeof(status), fmt, arg);
    status_rgb = rgb;
}

/* ---- drawing (Gran Turismo 4 flavoured: navy header, silver-blue body, glossy blue selection) ---- */

#define ROW_H 34
#define PX 40
#define PW (W - 2 * PX)
#define VX (PX + 250)
#define VR (PX + PW - 16)
static const int row_top[] = { 112, 146, 180, 244, 278, 312, 346, 410, 444, 478 };
#define BTN_Y 524
#define BTN_W 200
#define BTN_H 40
static const int btn_x[3] = { W / 2 - 316, W / 2 - 100, W / 2 + 116 };
#define FOOT_Y 596
#define C_NAVY 0x14263dffu
#define C_BLUE 0x1d5fc0ffu
#define CT_Y 124                 /* controls page table */
#define CT_ROW 40
#define CT_X0 (PX + 190)
#define CT_CW 128

static void sel_bar(GLfloat x, GLfloat y, GLfloat w, GLfloat h)
{
    rrect(x, y, w, h, 5, 0x4aa3f5ffu, 0x1450b4ffu);
    rrect(x + 2, y + 1, w - 4, h / 2 - 1, 4, 0xffffff60u, 0xffffff18u);
}

static void section(int y, const char *title)
{
    grad(PX, y + 3, 6, 14, 0x3d8ff0ffu, 0x1450b4ffu, 0);
    text(F_HEAD, PX + 14, y, C_NAVY, title, logo_slant);
    grad(PX + 20 + text_w(F_HEAD, title), y + 11, PW - 20 - text_w(F_HEAD, title), 1, 0x8ea3bcffu, 0x8ea3bcffu, 0);
}

static void fit_left(char *out, size_t n, int f, const char *s, int maxw)
{
    snprintf(out, n, "%s", s);
    if (text_w(f, out) <= maxw) return;
    size_t l = strlen(s);
    for (size_t k = 1; k < l; k++) {
        snprintf(out, n, "...%s", s + k);
        if (text_w(f, out) <= maxw) return;
    }
}

static void arrow(GLfloat x, GLfloat cy, int dir, u32 c)
{
    if (dir < 0) tri(x, cy, x + 8, cy - 6, x + 8, cy + 6, c);
    else tri(x + 8, cy, x, cy - 6, x, cy + 6, c);
}

static void button(int x, const char *s, int on)
{
    rrect(x - 1, BTN_Y - 1, BTN_W + 2, BTN_H + 2, 9, on ? 0x0d3a86ffu : 0x8193aaffu, on ? 0x0d3a86ffu : 0x5d6f88ffu);
    if (on) sel_bar(x, BTN_Y, BTN_W, BTN_H);
    else {
        rrect(x, BTN_Y, BTN_W, BTN_H, 8, 0xffffffffu, 0xc5d0dcffu);
        rrect(x + 2, BTN_Y + 1, BTN_W - 4, BTN_H / 2, 7, 0xffffffb0u, 0xffffff20u);
    }
    text(F_BTN, x + (BTN_W - text_w(F_BTN, s)) / 2, BTN_Y + (BTN_H - fonts[F_BTN].height) / 2 + 1,
         on ? 0xffffffffu : C_NAVY, s, 0);
}

static int pill(int x, const char *key, const char *what)
{
    int kw = text_w(F_SMALL, key) + 14;
    rrect(x, FOOT_Y + 12, kw, 20, 4, 0xe9eef4ffu, 0xb4c1d0ffu);
    text(F_SMALL, x + 7, FOOT_Y + 13, 0x14263dffu, key, 0);
    text(F_SMALL, x + kw + 7, FOOT_Y + 13, 0xc8d4e2ffu, what, 0);
    return x + kw + 14 + text_w(F_SMALL, what) + 22;
}

static void panel_row(int x, int y, int w, int last)
{
    grad(x, y, w, ROW_H, 0xfffffff0u, 0xf4f7faf0u, 0);
    if (!last) grad(x + 10, y + ROW_H - 1, w - 20, 1, 0xd5dde7ffu, 0xd5dde7ffu, 0);
}

static void draw_value_row(int i, int y, int on)
{
    char v[64];
    if (i == R_GAME) snprintf(v, sizeof(v), "Real Racing %d", game);
    else if (game == 3 && i >= R_ASSIST) snprintf(v, sizeof(v), "Real Racing 2 only");
    else if (i == R_RES) snprintf(v, sizeof(v), "%d x %d%s", res[ires].w, res[ires].h,
                             res[ires].w == native_w && res[ires].h == native_h ? "  (native)" : "");
    else if (i == R_FULL) snprintf(v, sizeof(v), "%s", G.fullscreen ? "On" : "Off");
    else if (i == R_VSYNC) snprintf(v, sizeof(v), "%s", G.vsync ? "On" : "Off");
    else if (i == R_ASSIST) snprintf(v, sizeof(v), "%s", G.no_assists ? "On (all assists off)" : "Off");
    else if (i == R_TILT) snprintf(v, sizeof(v), "%s", G.no_tilt ? "Off (level horizon)" : "Game setting");
    else if (i == R_FOV) snprintf(v, sizeof(v), G.cockpit_fov ? "%+d deg" : "Default", G.cockpit_fov);
    else if (naniso < 2) snprintf(v, sizeof(v), "Not supported");
    else if (aniso_opts[ianiso]) snprintf(v, sizeof(v), "%dx", aniso_opts[ianiso]);
    else snprintf(v, sizeof(v), "Off");
    int f = on ? F_BOLD : F_BODY, tw = text_w(f, v), ty = y + (ROW_H - fonts[f].height) / 2;
    GLfloat cy = ty + fonts[f].asc * 0.62f;
    bool off = game == 3 && i >= R_ASSIST;
    u32 ac = on ? 0xffffffffu : 0x2f7fe0ffu, fg = on ? 0xffffffffu : off ? 0x9aa8b8ffu : C_BLUE;
    if (!off) {
        arrow(VX, cy, -1, ac);
        arrow(VR - 8, cy, 1, ac);
    }
    if (i == R_FOV && game == 2) {                       /* slider track */
        int tx = VX + 24, tw2 = 180;
        float t = (float)(G.cockpit_fov - FOV_MIN) / (FOV_MAX - FOV_MIN);
        rrect(tx, cy - 3, tw2, 6, 3, on ? 0xffffff50u : 0xc5d0dcffu, on ? 0xffffff30u : 0xdde4ecffu);
        rrect(tx, cy - 3, 6 + (tw2 - 6) * t, 6, 3, on ? 0xffffffffu : 0x4aa3f5ffu, on ? 0xdbe9ffffu : 0x1450b4ffu);
        rrect(tx + (tw2 - 12) * t, cy - 7, 12, 14, 6, 0xffffffffu, on ? 0xdbe9ffffu : 0xc5d0dcffu);
        text(f, tx + tw2 + 20, ty, fg, v, 0);
        return;
    }
    int cx = VX + (VR - 12 - VX) / 2;
    text(f, cx - tw / 2 + 4, ty, fg, v, 0);
}

static void draw_main(void)
{
    section(88, "GAME FILES");
    section(220, "DISPLAY");
    section(386, "GAMEPLAY");
    static const int groups[][2] = { { R_GAME, R_DATA }, { R_RES, R_VSYNC }, { R_ASSIST, R_FOV } };
    for (int g = 0; g < 3; g++)
        frame(PX - 1, row_top[groups[g][0]] - 1, PW + 2, (groups[g][1] - groups[g][0] + 1) * ROW_H + 2, 0x9eb0c5ffu);
    for (int i = 0; i <= R_LAST_OPT; i++) {
        int y = row_top[i], on = sel == i;
        panel_row(PX, y, PW, i == R_DATA || i == R_VSYNC || i == R_FOV);
        if (on) sel_bar(PX + 3, y + 3, PW - 6, ROW_H - 6);
        const char *label = game == 3 && i == R_DATA ? "Game data" : labels[i];
        text(on ? F_BOLD : F_BODY, PX + 16, y + (ROW_H - fonts[F_BODY].height) / 2, on ? 0xffffffffu : C_NAVY, label, 0);
        if (i != R_APK && i != R_DATA) { draw_value_row(i, y, on); continue; }
        const char *p = editing && on ? edit : i == R_APK ? apk : data;
        char buf[1100];
        int ty = y + (ROW_H - fonts[F_SMALL].height) / 2;
        if (editing && on) {
            rrect(VX - 8, y + 6, VR - VX + 12, ROW_H - 12, 3, 0xffffffffu, 0xf0f4f8ffu);
            char tmp[1100];
            snprintf(tmp, sizeof(tmp), "%s|", p);
            fit_left(buf, sizeof(buf), F_SMALL, tmp, VR - VX - 4);
            text(F_SMALL, VX, ty, C_NAVY, buf, 0);
        } else if (!*p) {
            text(F_SMALL, VX, ty, on ? 0xdbe9ffffu : 0x7d8ea3ffu,
                 game == 3 ? (i == R_APK ? "APK file or install folder; Enter to type a path"
                                         : "com.ea.games.r3_row data folder (or zip)")
                           : "Drop a file here, or press Enter to type a path", 0);
        } else {
            fit_left(buf, sizeof(buf), F_SMALL, p, VR - VX);
            text(F_SMALL, VX, ty, on ? 0xffffffffu : C_BLUE, buf, 0);
        }
    }
    button(btn_x[0], "START", sel == R_PLAY);
    button(btn_x[1], "CONTROLS", sel == R_CTRL);
    button(btn_x[2], "EXIT", sel == R_QUIT);
}

static void draw_controls(void)
{
    section(88, "CONTROLS");
    static const char *const heads[4] = { "Keyboard", "Keyboard 2", "Controller", "Controller 2" };
    for (int c = 0; c < 4; c++)
        text(F_SMALL, CT_X0 + c * CT_CW + (CT_CW - text_w(F_SMALL, heads[c])) / 2, CT_Y - 20, 0x4d6c93ffu, heads[c], 0);
    frame(PX - 1, CT_Y - 1, PW + 2, ACT_COUNT * CT_ROW + 2, 0x9eb0c5ffu);
    for (int a = 0; a < ACT_COUNT; a++) {
        int y = CT_Y + a * CT_ROW;
        grad(PX, y, PW, CT_ROW, 0xfffffff0u, 0xf4f7faf0u, 0);
        if (a + 1 < ACT_COUNT) grad(PX + 10, y + CT_ROW - 1, PW - 20, 1, 0xd5dde7ffu, 0xd5dde7ffu, 0);
        int row_on = csel_r == a;
        text(row_on ? F_BOLD : F_BODY, PX + 16, y + (CT_ROW - fonts[F_BODY].height) / 2, C_NAVY, act_names[a], 0);
        for (int c = 0; c < 4; c++) {
            int x = CT_X0 + c * CT_CW + 4, on = row_on && csel_c == c;
            const char *n = on && capturing ? (c < 2 ? "Press a key" : "Press a button") : host_bind_name(c, g_binds[a][c]);
            if (on) sel_bar(x, y + 5, CT_CW - 8, CT_ROW - 10);
            else rrect(x, y + 5, CT_CW - 8, CT_ROW - 10, 5, 0xf7f9fbffu, 0xe3e9f0ffu);
            char buf[64];
            fit_left(buf, sizeof(buf), F_SMALL, n, CT_CW - 20);
            text(F_SMALL, x + (CT_CW - 8 - text_w(F_SMALL, buf)) / 2, y + (CT_ROW - fonts[F_SMALL].height) / 2,
                 on ? 0xffffffffu : !strcmp(n, "-") ? 0x9aa8b8ffu : C_BLUE, buf, 0);
        }
    }
    text(F_SMALL, PX, CT_Y + ACT_COUNT * CT_ROW + 12, 0x4d6c93ffu,
         "Sticks and triggers bound to steering or pedals are analog. Change camera works in races.", 0);
    button(btn_x[0], "DEFAULTS", csel_r == ACT_COUNT && csel_c == 0);
    button(btn_x[2], "DONE", csel_r == ACT_COUNT && csel_c != 0);
}

static void draw(void)
{
    gl.Viewport(0, 0, W, H);
    gl.ClearColor(1, 1, 1, 1);
    gl.Clear(0x4000);

    grad(0, 0, W, H, 0xf1f5f9ffu, 0xbccbdbffu, 0);
    grad(430, 76, 160, H - 130, 0xffffff00u, 0xffffff38u, 140);
    grad(560, 76, 60, H - 130, 0xffffff00u, 0xffffff28u, 140);

    grad(0, 0, W, 74, 0x0b2252ffu, 0x1b4f9cffu, 0);
    grad(0, 0, W, 34, 0xffffff22u, 0xffffff08u, 0);
    grad(0, 74, W, 2, 0x6cc0ffffu, 0x2f7fe0ffu, 0);
    for (int i = 0; i < 3; i++) grad(W - 250 + i * 16, 18, 8, 38, 0xffffff30u, 0xffffff10u, -10);
    text(F_LOGO, 30, 14, 0xffffffffu, game == 3 ? "REAL RACING 3" : "REAL RACING 2", logo_slant);
    const char *tag = page ? "rr2emu  CONTROLS" : "rr2emu  SETUP";
    text(F_HEAD, W - 30 - text_w(F_HEAD, tag), 28, 0xbcd6f5ffu, tag, logo_slant);

    if (page) draw_controls(); else draw_main();
    if (*status) text(F_BODY, (W - text_w(F_BODY, status)) / 2, BTN_Y + BTN_H + 5, status_rgb, status, 0);

    grad(0, FOOT_Y, W, H - FOOT_Y, 0x1d2a3cffu, 0x0b121cffu, 0);
    grad(0, FOOT_Y, W, 1, 0x4d6c93ffu, 0x4d6c93ffu, 0);
    int x = 30;
    if (page && capturing) {
        x = pill(x, "Any key / button", "Bind");
        pill(x, "Esc", "Cancel");
    } else if (page) {
        x = pill(x, "Enter", "Bind");
        x = pill(x, "Backspace", "Clear");
        x = pill(x, "Arrows", "Move");
        pill(x, "Esc", "Back");
    } else if (editing) {
        x = pill(x, "Enter", "Confirm");
        x = pill(x, "Esc", "Cancel");
        pill(x, "Ctrl+V", "Paste");
    } else {
        x = pill(x, "Enter", "Select");
        x = pill(x, "Arrows", "Change");
        x = pill(x, "Esc", "Exit");
        pill(x, "Drag & drop", game == 3 ? "APK / data" : "APK / OBB");
    }
    flush();
}

/* ---- game files: folders are used as they are, archives unpacked once ---- */

static bool is_dir(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode); }
static bool is_file(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISREG(st.st_mode); }

static u16 rd16(const u8 *p) { return p[0] | p[1] << 8; }
static u32 rd32(const u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }

static bool pump_quit(void)
{
    menu_event_t ev = { 0 };
    int e;
    while ((e = host_menu_poll(0, &ev)) >= 0) if (e == MENU_QUIT) return true;
    return false;
}

static bool entry_wanted(const char *name, int apk_mode)
{
    if (!*name || name[0] == '/' || strstr(name, "..")) return false;
    if (apk_mode == 3) return !strncmp(name, "lib/armeabi-v7a/", 16);
    return !apk_mode || !strcmp(name, SO_REL) || !strncmp(name, "assets/", 7);
}

static bool unzip(const char *zip, const char *dest, int apk_mode, const char *what)
{
    int fd = open(zip, O_RDONLY | O_BINARY);
    if (fd < 0) { set_status(0xc62828ffu, "cannot open %s", zip); return false; }
    struct stat st;
    fstat(fd, &st);
    u8 tail[65557];
    off_t tl = st.st_size < (off_t)sizeof(tail) ? st.st_size : (off_t)sizeof(tail);
    const u8 *eocd = NULL;
    if (emu_pread(fd, tail, (size_t)tl, st.st_size - tl) == tl)
        for (off_t i = tl - 22; i >= 0 && !eocd; i--) if (rd32(tail + i) == 0x06054b50) eocd = tail + i;
    if (!eocd || rd32(eocd + 16) == 0xFFFFFFFFu) {
        close(fd);
        set_status(0xc62828ffu, "%s is not a zip archive", what);
        return false;
    }
    u32 count = rd16(eocd + 10), cd_size = rd32(eocd + 12), cd_off = rd32(eocd + 16);
    u8 *cd = malloc(cd_size);
    bool ok = emu_pread(fd, cd, cd_size, cd_off) == (long)cd_size;
    if (!ok) set_status(0xc62828ffu, "cannot read %s", what);
    u64 total = 0, done = 0;
    for (int pass = 0; pass < 2 && ok; pass++) {
        const u8 *e = cd;
        for (u32 k = 0; k < count && ok; k++) {
            if (e + 46 > cd + cd_size || rd32(e) != 0x02014b50) { ok = false; break; }
            u16 method = rd16(e + 10), nl = rd16(e + 28);
            u32 csize = rd32(e + 20), usize = rd32(e + 24), loff = rd32(e + 42);
            char name[1024];
            snprintf(name, sizeof(name), "%.*s", nl, (const char *)e + 46);
            e += 46 + nl + rd16(e + 30) + rd16(e + 32);
            if (!entry_wanted(name, apk_mode)) continue;
            if (!pass) { total += usize; continue; }
            char path[2100];
            snprintf(path, sizeof(path), "%s/%s", dest, name);
            if (name[strlen(name) - 1] == '/') { mkdirs(path); continue; }
            *strrchr(path, '/') = 0;             /* entry names always use '/' */
            mkdirs(path);
            path[strlen(path)] = '/';
            u8 lh[30];
            if (emu_pread(fd, lh, 30, loff) != 30 || rd32(lh) != 0x04034b50 || (method != 0 && method != 8)) { ok = false; break; }
            long long pos = (long long)loff + 30 + rd16(lh + 26) + rd16(lh + 28);
            u8 *in = malloc(csize + 1), *out = method == 8 ? malloc(usize + 1) : in;
            ok = in && out && emu_pread(fd, in, csize, pos) == (long)csize &&
                 (method == 0 || inflate_raw(in, csize, out, usize) == 0);
            FILE *f = ok ? fopen(path, "wb") : NULL;
            if (!f) ok = false;
            else if (fwrite(out, 1, method == 8 ? usize : csize, f) != (method == 8 ? usize : csize)) ok = false;
            if (out != in) free(out);
            free(in);
            done += usize;
            if (!f) { set_status(0xc62828ffu, "failed unpacking %s", name); break; }
            if (fclose(f) != 0) ok = false;
            static struct timespec last;
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            if ((now.tv_sec - last.tv_sec) * 1000 + (now.tv_nsec - last.tv_nsec) / 1000000 > 100) {
                last = now;
                char msg[128];
                snprintf(msg, sizeof(msg), "Unpacking %s... %d%%", what, total ? (int)(done * 100 / total) : 0);
                set_status(0x1d4f91ffu, "%s", msg);
                draw();
                host_swap();
                if (pump_quit()) exit(0);
            }
            if (!ok) set_status(0xc62828ffu, "failed unpacking %s", name);
        }
    }
    free(cd);
    close(fd);
    if (ok) {
        char mark[1100];
        snprintf(mark, sizeof(mark), "%s/.done", dest);
        FILE *f = fopen(mark, "w");
        if (f) fclose(f);
    }
    return ok;
}

/* archive -> ~/.cache/rr2emu/<kind>-<size>-<mtime>, unpacked on first use */
static bool unpack_cached(const char *file, const char *kind, int apk_mode, char *out, size_t n)
{
    struct stat st;
    stat(file, &st);
    char base[1024], mark[1200];
    home_path(base, sizeof(base), "XDG_CACHE_HOME", ".cache", "rr2emu");
    snprintf(out, n, "%s/%s-%llx-%llx", base, kind, (unsigned long long)st.st_size, (unsigned long long)st.st_mtime);
    snprintf(mark, sizeof(mark), "%s/.done", out);
    if (is_file(mark)) return true;
    mkdirs(out);
    return unzip(file, out, apk_mode, kind);
}

static bool has_game_dir(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return false;
    struct dirent *e;
    bool found = false;
    while (!found && (e = readdir(d))) found = !strncmp(e->d_name, "com.ea.game.realracing2", 23);
    closedir(d);
    return found;
}

static char so_buf[1400], apk_assets_buf[1200], data_buf[1200];
static char rr3_apk_buf[1200], rr3_base_buf[1200], rr3_sd_buf[1200];

static bool path_has(const char *dir, const char *rel)
{
    char p[1400];
    snprintf(p, sizeof(p), "%s/%s", dir, rel);
    return is_file(p) || is_dir(p);
}

/* RR3: APK file (libs unpacked, base.apk read in place) or an install folder;
 * data: the com.ea.games.r3_row external data (files/.depot), an extracted root holding sdcard/, or a zip of either */
static bool prepare3(void)
{
    char root[1100];
    const char *libs;
    rr3_base_buf[0] = 0;
    if (is_file(apk)) {
        if (!unpack_cached(apk, "apk3", 3, root, sizeof(root))) return false;
        libs = "lib/armeabi-v7a";
        snprintf(rr3_base_buf, sizeof(rr3_base_buf), "%s", apk);
    } else if (is_dir(apk)) {
        snprintf(root, sizeof(root), "%s", apk);
        if (!path_has(root, "lib") && path_has(root, "apk/lib")) strcat(root, "/apk");
        libs = path_has(root, "lib/arm/libRealRacing3.so") ? "lib/arm" : "lib/armeabi-v7a";
        if (path_has(root, "base.apk")) snprintf(rr3_base_buf, sizeof(rr3_base_buf), "%s/base.apk", root);
    } else { set_status(0xc62828ffu, "%s", *apk ? "APK not found" : "Choose the Real Racing 3 APK"); sel = R_APK; return false; }
    snprintf(so_buf, sizeof(so_buf), "%s/%s/libRealRacing3.so", root, libs);
    if (!is_file(so_buf)) { set_status(0xc62828ffu, "%s", "The APK has no " SO3_REL); sel = R_APK; return false; }
    snprintf(rr3_apk_buf, sizeof(rr3_apk_buf), "%s", root);

    const char *src = *data ? data : apk;                       /* an extracted install holds both */
    if (is_file(src) && src == data) { if (!unpack_cached(data, "data3", 0, root, sizeof(root))) return false; }
    else if (is_dir(src)) snprintf(root, sizeof(root), "%s", src);
    else { set_status(0xc62828ffu, "%s", *data ? "Game data not found" : "Choose the game data folder"); sel = R_DATA; return false; }
    if (path_has(root, "com.ea.games.r3_row")) strcat(root, "/com.ea.games.r3_row");
    size_t rl = strlen(root);
    if (src == apk && rl > 4 && !strcmp(root + rl - 4, "/apk")) root[rl - 4] = 0;
    if (path_has(root, "sdcard/files/.depot")) {
        snprintf(data_buf, sizeof(data_buf), "%s", root);
        snprintf(rr3_sd_buf, sizeof(rr3_sd_buf), "%s/sdcard", root);
    } else if (path_has(root, "files/.depot")) {
        snprintf(rr3_sd_buf, sizeof(rr3_sd_buf), "%s", root);
        snprintf(data_buf, sizeof(data_buf), "%s", root);
    } else { set_status(0xc62828ffu, "%s", "No files/.depot in the game data folder"); sel = R_DATA; return false; }
    return true;
}

static bool prepare(void)
{
    if (game == 3) return prepare3();
    char root[1100], p[1300];
    if (is_dir(apk)) snprintf(root, sizeof(root), "%s", apk);
    else if (is_file(apk)) { if (!unpack_cached(apk, "apk", 1, root, sizeof(root))) return false; }
    else { set_status(0xc62828ffu, "%s", *apk ? "APK not found" : "Choose the game APK"); sel = R_APK; return false; }
    snprintf(p, sizeof(p), "%s/" SO_REL, root);
    if (!is_file(p)) { set_status(0xc62828ffu, "%s", "The APK has no " SO_REL); sel = R_APK; return false; }
    snprintf(so_buf, sizeof(so_buf), "%s", p);
    snprintf(apk_assets_buf, sizeof(apk_assets_buf), "%s/assets", root);

    if (is_dir(data)) snprintf(root, sizeof(root), "%s", data);
    else if (is_file(data)) { if (!unpack_cached(data, "data", 0, root, sizeof(root))) return false; }
    else { set_status(0xc62828ffu, "%s", *data ? "Game data not found" : "Choose the game data (OBB or zip)"); sel = R_DATA; return false; }
    if (!has_game_dir(root)) {
        char *slash = NULL;
        for (char *q = root; *q; q++) if (strchr(PATH_SEP_CHARS, *q)) slash = q;
        if (slash && !strncmp(slash + 1, "com.ea.game.realracing2", 23)) *slash = 0;
        else { set_status(0xc62828ffu, "%s", "No com.ea.game.realracing2_* folder in the game data"); sel = R_DATA; return false; }
    }
    snprintf(data_buf, sizeof(data_buf), "%s", root);
    return true;
}

/* ---- input ---- */

static void clean_path(char *dst, size_t n, const char *src)
{
    while (*src == ' ' || *src == '\'' || *src == '"') src++;
    if (!strncmp(src, "file://", 7)) src += 7;
    const char *h = getenv("HOME");
    if (src[0] == '~' && src[1] == '/' && h) snprintf(dst, n, "%s%s", h, src + 1);
    else snprintf(dst, n, "%s", src);
    size_t l = strlen(dst);
    while (l && strchr(" \t\r\n'\"", dst[l - 1])) dst[--l] = 0;
    char *r = emu_realpath(dst);
    if (r) { snprintf(dst, n, "%s", r); free(r); }
}

static int hit(int x, int y)
{
    for (int i = 0; i <= R_LAST_OPT; i++) if (x >= PX && x < PX + PW && y >= row_top[i] && y < row_top[i] + ROW_H) return i;
    if (y >= BTN_Y && y < BTN_Y + BTN_H)
        for (int b = 0; b < 3; b++) if (x >= btn_x[b] && x < btn_x[b] + BTN_W) return R_PLAY + b;
    return -1;
}

/* controls page: -1 none, else row * 4 + column (row ACT_COUNT = buttons, column 0 defaults / 1 done) */
static int hit_controls(int x, int y)
{
    for (int a = 0; a < ACT_COUNT; a++)
        for (int c = 0; c < 4; c++) {
            int cx = CT_X0 + c * CT_CW + 4, cy = CT_Y + a * CT_ROW + 5;
            if (x >= cx && x < cx + CT_CW - 8 && y >= cy && y < cy + CT_ROW - 10) return a * 4 + c;
        }
    if (y >= BTN_Y && y < BTN_Y + BTN_H) {
        if (x >= btn_x[0] && x < btn_x[0] + BTN_W) return ACT_COUNT * 4;
        if (x >= btn_x[2] && x < btn_x[2] + BTN_W) return ACT_COUNT * 4 + 1;
    }
    return -1;
}

static void change_game(void)
{
    char t[1024];
    memcpy(t, apk, sizeof(t)); memcpy(apk, other_apk, sizeof(t)); memcpy(other_apk, t, sizeof(t));
    memcpy(t, data, sizeof(t)); memcpy(data, other_data, sizeof(t)); memcpy(other_data, t, sizeof(t));
    game = game == 2 ? 3 : 2;
    *status = 0;
}

static void change(int d)
{
    if (game == 3 && sel >= R_ASSIST && sel <= R_FOV) return;
    switch (sel) {
    case R_GAME: change_game(); break;
    case R_RES: ires = (ires + d + nres) % nres; break;
    case R_FULL: G.fullscreen ^= 1; break;
    case R_ANISO: ianiso = (ianiso + d + naniso) % naniso; break;
    case R_VSYNC: G.vsync ^= 1; break;
    case R_ASSIST: G.no_assists ^= 1; break;
    case R_TILT: G.no_tilt ^= 1; break;
    case R_FOV:
        G.cockpit_fov += d * FOV_STEP;
        if (G.cockpit_fov < FOV_MIN) G.cockpit_fov = FOV_MIN;
        if (G.cockpit_fov > FOV_MAX) G.cockpit_fov = FOV_MAX;
        break;
    case R_PLAY: case R_CTRL: case R_QUIT: sel = R_PLAY + (sel - R_PLAY + d + 3) % 3; return;
    default: return;
    }
    cfg_save();
}

static void edit_begin(void)
{
    snprintf(edit, sizeof(edit), "%s", sel == R_APK ? apk : data);
    editing = 1;
    host_text_input(1);
}

static void edit_end(int keep)
{
    if (keep) {
        clean_path(sel == R_APK ? apk : data, sizeof(apk), edit);
        cfg_save();
    }
    editing = 0;
    host_text_input(0);
}

static void bind_set(int act, int slot, int code)
{
    for (int a = 0; a < ACT_COUNT; a++)          /* one input drives one action */
        for (int s = slot < 2 ? 0 : 2; s < (slot < 2 ? 2 : 4); s++)
            if (g_binds[a][s] == code) g_binds[a][s] = slot < 2 ? 0 : -1;
    g_binds[act][slot] = code;
    cfg_save();
}

static int handle_controls(int e, menu_event_t *ev)
{
    if (capturing) {
        if (e == MENU_RAW_KEY && ev->key == 27) capturing = 0;
        else if (e == MENU_RAW_KEY && csel_c < 2) { bind_set(csel_r, csel_c, ev->key); capturing = 0; }
        else if (e == MENU_RAW_PAD && csel_c >= 2) { bind_set(csel_r, csel_c, ev->pad); capturing = 0; }
        return 0;
    }
    int on_btn = csel_r == ACT_COUNT;
    switch (e) {
    case MENU_UP: csel_r = (csel_r + ACT_COUNT) % (ACT_COUNT + 1); if (csel_r == ACT_COUNT) csel_c = 1; break;
    case MENU_DOWN: csel_r = (csel_r + 1) % (ACT_COUNT + 1); if (csel_r == ACT_COUNT) csel_c = 1; break;
    case MENU_LEFT: csel_c = on_btn ? !csel_c : (csel_c + 3) % 4; break;
    case MENU_RIGHT: csel_c = on_btn ? !csel_c : (csel_c + 1) % 4; break;
    case MENU_ERASE: if (!on_btn) { g_binds[csel_r][csel_c] = csel_c < 2 ? 0 : -1; cfg_save(); } break;
    case MENU_BACK: page = 0; break;
    case MENU_MOVE: { int h = hit_controls(ev->x, ev->y); if (h >= 0) { csel_r = h / 4; csel_c = h % 4; } break; }
    case MENU_CLICK: {
        int h = hit_controls(ev->x, ev->y);
        if (h < 0) break;
        csel_r = h / 4; csel_c = h % 4;
        on_btn = csel_r == ACT_COUNT; }
        /* fall through */
    case MENU_OK:
        if (!on_btn) capturing = 1;
        else if (csel_c == 0) { host_binds_default(); cfg_save(); set_status(0x2e7d32ffu, "%s", "Controls reset to defaults"); }
        else page = 0;
        break;
    }
    return 0;
}

/* 1 play, -1 quit, 0 continue */
static int handle(int e, menu_event_t *ev)
{
    if (e == MENU_QUIT) return -1;
    if (page) return handle_controls(e, ev);
    if (e == MENU_DROP) {
        size_t l = strlen(ev->text);
        int is_apk = l > 4 && !strcasecmp(ev->text + l - 4, ".apk");
        if (game == 3 && !is_apk) {
            char t[1024];
            clean_path(t, sizeof(t), ev->text);
            is_apk = is_dir(t) && (path_has(t, "lib") || path_has(t, "apk/lib"));
        }
        if (editing) edit_end(0);
        clean_path(is_apk ? apk : data, sizeof(apk), ev->text);
        cfg_save();
        set_status(0x2e7d32ffu, is_apk ? "APK set" : "Game data set", NULL);
        return 0;
    }
    if (editing) {
        size_t l = strlen(edit);
        switch (e) {
        case MENU_CHAR: case MENU_PASTE:
            snprintf(edit + l, sizeof(edit) - l, "%s", ev->text);
            edit[strcspn(edit, "\r\n")] = 0;
            break;
        case MENU_ERASE:
            while (l && ((u8)edit[l - 1] & 0xC0) == 0x80) edit[--l] = 0;
            if (l) edit[--l] = 0;
            break;
        case MENU_OK: edit_end(1); break;
        case MENU_BACK: edit_end(0); break;
        case MENU_CLICK: edit_end(1); return handle(e, ev);
        }
        return 0;
    }
    switch (e) {
    case MENU_UP: sel = sel > R_PLAY ? R_LAST_OPT : (sel + NROWS - 1) % NROWS; if (sel > R_QUIT) sel = R_QUIT; break;
    case MENU_DOWN: sel = sel >= R_PLAY ? R_GAME : sel + 1; break;
    case MENU_LEFT: change(-1); break;
    case MENU_RIGHT: change(1); break;
    case MENU_BACK: return -1;
    case MENU_MOVE: {
        static int mx = -1, my = -1, moved;
        if (mx >= 0 && (ev->x != mx || ev->y != my)) moved = 1;
        mx = ev->x; my = ev->y;
        int h = moved ? hit(ev->x, ev->y) : -1;
        if (h >= 0) sel = h;
        break; }
    case MENU_CLICK: {
        int h = hit(ev->x, ev->y);
        if (h < 0) break;
        sel = h;
        if (h == R_APK || h == R_DATA) edit_begin();
        else if (h == R_PLAY) return 1;
        else if (h == R_QUIT) return -1;
        else if (h == R_CTRL) { page = 1; csel_r = csel_c = 0; }
        else change(ev->x < VX + (VR - VX) / 2 ? -1 : 1);
        break; }
    case MENU_OK:
        if (sel == R_APK || sel == R_DATA) edit_begin();
        else if (sel == R_PLAY) return 1;
        else if (sel == R_QUIT) return -1;
        else if (sel == R_CTRL) { page = 1; csel_r = csel_c = 0; }
        else change(1);
        break;
    }
    return 0;
}

/* shows the launcher; on START fills G's paths/options and *so_path */
bool launcher_run(const char **so_path)
{
    if (!gl_setup()) { LOG("[launcher] host GL lacks basics\n"); return *so_path != NULL; }
    cfg_load();
    if (*so_path) {                                  /* a path on the command line wins over the saved one */
        char tmp[1100];
        snprintf(tmp, sizeof(tmp), "%s", *so_path);
        char *lib = strstr(tmp, "/lib/");
        if (strstr(tmp, "RealRacing3") && game == 2) change_game();
        if (game == 3 && lib) { *lib = 0; clean_path(apk, sizeof(apk), tmp); }
        size_t l = strlen(tmp), r = strlen(SO_REL);
        if (game == 2 && l > r && !strcmp(tmp + l - r, SO_REL)) { tmp[l - r] = 0; clean_path(apk, sizeof(apk), tmp); }
        if (strcmp(G.assets_dir, "./assets")) clean_path(data, sizeof(data), G.assets_dir);
    }
    build_lists();
    const char *shot = getenv("RR2_LAUNCHER_SHOT");
    for (;;) {
        draw();
        if (shot) glhost_screenshot(shot, W, H);
        host_swap();
        menu_event_t ev = { 0 };
        int r = 0;
        ev.capture = capturing;
        for (int e = host_menu_poll(1000, &ev); e >= 0 && !r; ev.capture = capturing, e = host_menu_poll(0, &ev))
            r = handle(e, &ev);
        if (r < 0) { gl_teardown(); return false; }
        if (r > 0) {
            set_status(0x1d4f91ffu, "%s", "Checking game files...");
            draw();
            host_swap();
            if (!prepare()) continue;
            break;
        }
    }
    cfg_save();
    *so_path = so_buf;
    G.game = game;
    if (game == 3) {
        G.rr3_apk_dir = rr3_apk_buf;
        G.rr3_base_apk = *rr3_base_buf ? rr3_base_buf : NULL;
        G.rr3_sdcard_dir = rr3_sd_buf;
    } else G.apk_assets_dir = apk_assets_buf;
    G.assets_dir = data_buf;
    LOG("[launcher] %s | %s | %dx%d%s aniso %d vsync %d assists %s tilt %s fov %+d\n", so_buf, data_buf, G.width, G.height,
        G.fullscreen ? " fullscreen" : "", G.aniso, G.vsync, G.no_assists ? "off" : "game", G.no_tilt ? "off" : "game",
        G.cockpit_fov);
    gl_teardown();
    return true;
}
