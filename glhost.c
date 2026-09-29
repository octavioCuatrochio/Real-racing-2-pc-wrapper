/*
 * glhost.c - guest GLES2 -> host GLES2 passthrough.
 *
 * Plain calls go straight through (softfp float bits reinterpreted, guest
 * pointers translated once). Hand-written thunks cover what differs across
 * the 32/64-bit boundary: client-side vertex/index arrays (resolved against
 * the tracked buffer bindings), GLsizeiptr widening, string arrays, strings
 * returned to the guest, mapped buffers (guest shadow copy), and ATC
 * textures, which desktop GPUs lack and are decoded to RGBA8 on upload.
 */
#include "emu.h"
#include <stddef.h>

typedef unsigned GLenum, GLuint, GLbitfield;
typedef int GLint, GLsizei;
typedef float GLfloat;
typedef unsigned char GLboolean;
typedef char GLchar;
typedef ptrdiff_t GLsizeiptr, GLintptr;

#define COMMA ,
#define A(i) harg(c, i)
#define I(i) ((GLint)harg(c, i))
#define F(i) u2f(harg(c, i))
#define P(i) gp(harg(c, i))
#define RET(x) hret(c, (u32)(x))

static inline f32 u2f(u32 v) { f32 f; memcpy(&f, &v, 4); return f; }
static inline void *gp(gptr p) { return p ? g2h(p) : NULL; }

/* ---- straight passthrough: name, return, host params, call ---- */
#define GL_PLAIN(X) \
    X(glActiveTexture, void, (GLenum), p(A(0))) \
    X(glAttachShader, void, (GLuint, GLuint), p(A(0), A(1))) \
    X(glBindAttribLocation, void, (GLuint, GLuint, const GLchar *), p(A(0), A(1), P(2))) \
    X(glBindFramebuffer, void, (GLenum, GLuint), p(A(0), A(1))) \
    X(glBindRenderbuffer, void, (GLenum, GLuint), p(A(0), A(1))) \
    X(glBindTexture, void, (GLenum, GLuint), p(A(0), A(1))) \
    X(glBlendColor, void, (GLfloat, GLfloat, GLfloat, GLfloat), p(F(0), F(1), F(2), F(3))) \
    X(glBlendEquation, void, (GLenum), p(A(0))) \
    X(glBlendEquationSeparate, void, (GLenum, GLenum), p(A(0), A(1))) \
    X(glBlendFunc, void, (GLenum, GLenum), p(A(0), A(1))) \
    X(glBlendFuncSeparate, void, (GLenum, GLenum, GLenum, GLenum), p(A(0), A(1), A(2), A(3))) \
    X(glCheckFramebufferStatus, GLenum, (GLenum), RET(p(A(0)))) \
    X(glClear, void, (GLbitfield), p(A(0))) \
    X(glClearColor, void, (GLfloat, GLfloat, GLfloat, GLfloat), p(F(0), F(1), F(2), F(3))) \
    X(glClearDepthf, void, (GLfloat), p(F(0))) \
    X(glClearStencil, void, (GLint), p(I(0))) \
    X(glColorMask, void, (GLboolean, GLboolean, GLboolean, GLboolean), p(A(0), A(1), A(2), A(3))) \
    X(glCompileShader, void, (GLuint), p(A(0))) \
    X(glCompressedTexSubImage2D, void, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLsizei, const void *), \
      p(A(0), I(1), I(2), I(3), I(4), I(5), A(6), I(7), P(8))) \
    X(glCopyTexImage2D, void, (GLenum, GLint, GLenum, GLint, GLint, GLsizei, GLsizei, GLint), \
      p(A(0), I(1), A(2), I(3), I(4), I(5), I(6), I(7))) \
    X(glCopyTexSubImage2D, void, (GLenum, GLint, GLint, GLint, GLint, GLint, GLsizei, GLsizei), \
      p(A(0), I(1), I(2), I(3), I(4), I(5), I(6), I(7))) \
    X(glCreateProgram, GLuint, (void), RET(p())) \
    X(glCreateShader, GLuint, (GLenum), RET(p(A(0)))) \
    X(glCullFace, void, (GLenum), p(A(0))) \
    X(glDeleteFramebuffers, void, (GLsizei, const GLuint *), p(I(0), P(1))) \
    X(glDeleteProgram, void, (GLuint), p(A(0))) \
    X(glDeleteRenderbuffers, void, (GLsizei, const GLuint *), p(I(0), P(1))) \
    X(glDeleteShader, void, (GLuint), p(A(0))) \
    X(glDeleteTextures, void, (GLsizei, const GLuint *), p(I(0), P(1))) \
    X(glDepthFunc, void, (GLenum), p(A(0))) \
    X(glDepthMask, void, (GLboolean), p(A(0))) \
    X(glDepthRangef, void, (GLfloat, GLfloat), p(F(0), F(1))) \
    X(glDetachShader, void, (GLuint, GLuint), p(A(0), A(1))) \
    X(glDisable, void, (GLenum), p(A(0))) \
    X(glDisableVertexAttribArray, void, (GLuint), p(A(0))) \
    X(glDrawArrays, void, (GLenum, GLint, GLsizei), p(A(0), I(1), I(2))) \
    X(glEnable, void, (GLenum), p(A(0))) \
    X(glEnableVertexAttribArray, void, (GLuint), p(A(0))) \
    X(glFinish, void, (void), p()) \
    X(glFlush, void, (void), p()) \
    X(glFramebufferRenderbuffer, void, (GLenum, GLenum, GLenum, GLuint), p(A(0), A(1), A(2), A(3))) \
    X(glFramebufferTexture2D, void, (GLenum, GLenum, GLenum, GLuint, GLint), p(A(0), A(1), A(2), A(3), I(4))) \
    X(glFrontFace, void, (GLenum), p(A(0))) \
    X(glGenBuffers, void, (GLsizei, GLuint *), p(I(0), P(1))) \
    X(glGenFramebuffers, void, (GLsizei, GLuint *), p(I(0), P(1))) \
    X(glGenRenderbuffers, void, (GLsizei, GLuint *), p(I(0), P(1))) \
    X(glGenTextures, void, (GLsizei, GLuint *), p(I(0), P(1))) \
    X(glGenerateMipmap, void, (GLenum), p(A(0))) \
    X(glGetActiveAttrib, void, (GLuint, GLuint, GLsizei, GLsizei *, GLint *, GLenum *, GLchar *), \
      p(A(0), A(1), I(2), P(3), P(4), P(5), P(6))) \
    X(glGetActiveUniform, void, (GLuint, GLuint, GLsizei, GLsizei *, GLint *, GLenum *, GLchar *), \
      p(A(0), A(1), I(2), P(3), P(4), P(5), P(6))) \
    X(glGetAttachedShaders, void, (GLuint, GLsizei, GLsizei *, GLuint *), p(A(0), I(1), P(2), P(3))) \
    X(glGetAttribLocation, GLint, (GLuint, const GLchar *), RET(p(A(0), P(1)))) \
    X(glGetBooleanv, void, (GLenum, GLboolean *), p(A(0), P(1))) \
    X(glGetBufferParameteriv, void, (GLenum, GLenum, GLint *), p(A(0), A(1), P(2))) \
    X(glGetError, GLenum, (void), RET(p())) \
    X(glGetFloatv, void, (GLenum, GLfloat *), p(A(0), P(1))) \
    X(glGetFramebufferAttachmentParameteriv, void, (GLenum, GLenum, GLenum, GLint *), p(A(0), A(1), A(2), P(3))) \
    X(glGetProgramiv, void, (GLuint, GLenum, GLint *), p(A(0), A(1), P(2))) \
    X(glGetProgramInfoLog, void, (GLuint, GLsizei, GLsizei *, GLchar *), p(A(0), I(1), P(2), P(3))) \
    X(glGetRenderbufferParameteriv, void, (GLenum, GLenum, GLint *), p(A(0), A(1), P(2))) \
    X(glGetShaderiv, void, (GLuint, GLenum, GLint *), p(A(0), A(1), P(2))) \
    X(glGetShaderInfoLog, void, (GLuint, GLsizei, GLsizei *, GLchar *), p(A(0), I(1), P(2), P(3))) \
    X(glGetShaderPrecisionFormat, void, (GLenum, GLenum, GLint *, GLint *), p(A(0), A(1), P(2), P(3))) \
    X(glGetShaderSource, void, (GLuint, GLsizei, GLsizei *, GLchar *), p(A(0), I(1), P(2), P(3))) \
    X(glGetTexParameterfv, void, (GLenum, GLenum, GLfloat *), p(A(0), A(1), P(2))) \
    X(glGetTexParameteriv, void, (GLenum, GLenum, GLint *), p(A(0), A(1), P(2))) \
    X(glGetUniformfv, void, (GLuint, GLint, GLfloat *), p(A(0), I(1), P(2))) \
    X(glGetUniformiv, void, (GLuint, GLint, GLint *), p(A(0), I(1), P(2))) \
    X(glGetUniformLocation, GLint, (GLuint, const GLchar *), RET(p(A(0), P(1)))) \
    X(glGetVertexAttribfv, void, (GLuint, GLenum, GLfloat *), p(A(0), A(1), P(2))) \
    X(glGetVertexAttribiv, void, (GLuint, GLenum, GLint *), p(A(0), A(1), P(2))) \
    X(glHint, void, (GLenum, GLenum), p(A(0), A(1))) \
    X(glIsBuffer, GLboolean, (GLuint), RET(p(A(0)))) \
    X(glIsEnabled, GLboolean, (GLenum), RET(p(A(0)))) \
    X(glIsFramebuffer, GLboolean, (GLuint), RET(p(A(0)))) \
    X(glIsProgram, GLboolean, (GLuint), RET(p(A(0)))) \
    X(glIsRenderbuffer, GLboolean, (GLuint), RET(p(A(0)))) \
    X(glIsShader, GLboolean, (GLuint), RET(p(A(0)))) \
    X(glIsTexture, GLboolean, (GLuint), RET(p(A(0)))) \
    X(glLineWidth, void, (GLfloat), p(F(0))) \
    X(glLinkProgram, void, (GLuint), p(A(0))) \
    X(glPixelStorei, void, (GLenum, GLint), p(A(0), I(1))) \
    X(glPolygonOffset, void, (GLfloat, GLfloat), p(F(0), F(1))) \
    X(glReadPixels, void, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *), \
      p(I(0), I(1), I(2), I(3), A(4), A(5), P(6))) \
    X(glReleaseShaderCompiler, void, (void), p()) \
    X(glRenderbufferStorage, void, (GLenum, GLenum, GLsizei, GLsizei), p(A(0), A(1), I(2), I(3))) \
    X(glSampleCoverage, void, (GLfloat, GLboolean), p(F(0), A(1))) \
    X(glScissor, void, (GLint, GLint, GLsizei, GLsizei), p(I(0), I(1), I(2), I(3))) \
    X(glStencilFunc, void, (GLenum, GLint, GLuint), p(A(0), I(1), A(2))) \
    X(glStencilFuncSeparate, void, (GLenum, GLenum, GLint, GLuint), p(A(0), A(1), I(2), A(3))) \
    X(glStencilMask, void, (GLuint), p(A(0))) \
    X(glStencilMaskSeparate, void, (GLenum, GLuint), p(A(0), A(1))) \
    X(glStencilOp, void, (GLenum, GLenum, GLenum), p(A(0), A(1), A(2))) \
    X(glStencilOpSeparate, void, (GLenum, GLenum, GLenum, GLenum), p(A(0), A(1), A(2), A(3))) \
    X(glTexImage2D, void, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *), \
      p(A(0), I(1), I(2), I(3), I(4), I(5), A(6), A(7), P(8))) \
    X(glTexParameterf, void, (GLenum, GLenum, GLfloat), p(A(0), A(1), F(2))) \
    X(glTexParameterfv, void, (GLenum, GLenum, const GLfloat *), p(A(0), A(1), P(2))) \
    X(glTexParameteri, void, (GLenum, GLenum, GLint), p(A(0), A(1), I(2))) \
    X(glTexParameteriv, void, (GLenum, GLenum, const GLint *), p(A(0), A(1), P(2))) \
    X(glTexSubImage2D, void, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *), \
      p(A(0), I(1), I(2), I(3), I(4), I(5), A(6), A(7), P(8))) \
    X(glUniform1f, void, (GLint, GLfloat), p(I(0), F(1))) \
    X(glUniform2f, void, (GLint, GLfloat, GLfloat), p(I(0), F(1), F(2))) \
    X(glUniform3f, void, (GLint, GLfloat, GLfloat, GLfloat), p(I(0), F(1), F(2), F(3))) \
    X(glUniform4f, void, (GLint, GLfloat, GLfloat, GLfloat, GLfloat), p(I(0), F(1), F(2), F(3), F(4))) \
    X(glUniform1i, void, (GLint, GLint), p(I(0), I(1))) \
    X(glUniform2i, void, (GLint, GLint, GLint), p(I(0), I(1), I(2))) \
    X(glUniform3i, void, (GLint, GLint, GLint, GLint), p(I(0), I(1), I(2), I(3))) \
    X(glUniform4i, void, (GLint, GLint, GLint, GLint, GLint), p(I(0), I(1), I(2), I(3), I(4))) \
    X(glUniform1fv, void, (GLint, GLsizei, const GLfloat *), p(I(0), I(1), P(2))) \
    X(glUniform2fv, void, (GLint, GLsizei, const GLfloat *), p(I(0), I(1), P(2))) \
    X(glUniform3fv, void, (GLint, GLsizei, const GLfloat *), p(I(0), I(1), P(2))) \
    X(glUniform4fv, void, (GLint, GLsizei, const GLfloat *), p(I(0), I(1), P(2))) \
    X(glUniform1iv, void, (GLint, GLsizei, const GLint *), p(I(0), I(1), P(2))) \
    X(glUniform2iv, void, (GLint, GLsizei, const GLint *), p(I(0), I(1), P(2))) \
    X(glUniform3iv, void, (GLint, GLsizei, const GLint *), p(I(0), I(1), P(2))) \
    X(glUniform4iv, void, (GLint, GLsizei, const GLint *), p(I(0), I(1), P(2))) \
    X(glUniformMatrix2fv, void, (GLint, GLsizei, GLboolean, const GLfloat *), p(I(0), I(1), A(2), P(3))) \
    X(glUniformMatrix3fv, void, (GLint, GLsizei, GLboolean, const GLfloat *), p(I(0), I(1), A(2), P(3))) \
    X(glUniformMatrix4fv, void, (GLint, GLsizei, GLboolean, const GLfloat *), p(I(0), I(1), A(2), P(3))) \
    X(glUseProgram, void, (GLuint), p(A(0))) \
    X(glValidateProgram, void, (GLuint), p(A(0))) \
    X(glVertexAttrib1f, void, (GLuint, GLfloat), p(A(0), F(1))) \
    X(glVertexAttrib2f, void, (GLuint, GLfloat, GLfloat), p(A(0), F(1), F(2))) \
    X(glVertexAttrib3f, void, (GLuint, GLfloat, GLfloat, GLfloat), p(A(0), F(1), F(2), F(3))) \
    X(glVertexAttrib4f, void, (GLuint, GLfloat, GLfloat, GLfloat, GLfloat), p(A(0), F(1), F(2), F(3), F(4))) \
    X(glVertexAttrib1fv, void, (GLuint, const GLfloat *), p(A(0), P(1))) \
    X(glVertexAttrib2fv, void, (GLuint, const GLfloat *), p(A(0), P(1))) \
    X(glVertexAttrib3fv, void, (GLuint, const GLfloat *), p(A(0), P(1))) \
    X(glVertexAttrib4fv, void, (GLuint, const GLfloat *), p(A(0), P(1))) \
    X(glViewport, void, (GLint, GLint, GLsizei, GLsizei), p(I(0), I(1), I(2), I(3)))

#define X(n, r, params, call) static r (*p_##n) params;
GL_PLAIN(X)
#undef X
enum {
#define X(n, r, params, call) GLC_##n,
    GL_PLAIN(X)
#undef X
    GLC_COUNT
};
static const char *const glc_names[GLC_COUNT] = {
#define X(n, r, params, call) #n,
    GL_PLAIN(X)
#undef X
};
u64 g_glc[GLC_COUNT + 16];
u64 g_tex_bytes, g_tex_uploads;
#define X(n, r, params, call) static void h_##n(cpu_t *c) { r (*p) params = p_##n; g_glc[GLC_##n]++; call; }
GL_PLAIN(X)
#undef X

/* ---- hand-written thunks ---- */

static void (*p_glBindBuffer)(GLenum, GLuint);
static void (*p_glBufferData)(GLenum, GLsizeiptr, const void *, GLenum);
static void (*p_glBufferSubData)(GLenum, GLintptr, GLsizeiptr, const void *);
static void (*p_glDeleteBuffers)(GLsizei, const GLuint *);
static void (*p_glDrawElements)(GLenum, GLsizei, GLenum, const void *);
static void (*p_glVertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *);
static void (*p_glGetVertexAttribPointerv)(GLuint, GLenum, void **);
static void (*p_glShaderSource)(GLuint, GLsizei, const GLchar *const *, const GLint *);
static const GLchar *(*p_glGetString)(GLenum);
static void (*p_glGetIntegerv)(GLenum, GLint *);
static void (*p_glTexImage2Dx)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
static void (*p_glCompressedTexImage2D)(GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void *);

static GLuint bound_array, bound_elem;

/* buffer shadows: size (from glBufferData) + guest copy for glMapBufferOES */
#define MAX_BUFS 8192
static struct { u32 size; gptr shadow; } bufs[MAX_BUFS];

static void h_glBindBuffer(cpu_t *c)
{
    GLenum t = A(0); GLuint b = A(1);
    if (t == 0x8892) bound_array = b; else if (t == 0x8893) bound_elem = b;
    p_glBindBuffer(t, b);
}
static GLuint *buf_slot_for(GLenum t) { static GLuint none; return t == 0x8892 ? &bound_array : t == 0x8893 ? &bound_elem : &none; }
static void h_glBufferData(cpu_t *c)
{
    GLenum t = A(0);
    u32 size = A(1);
    GLuint b = *buf_slot_for(t);
    if (b < MAX_BUFS) {
        if (bufs[b].shadow && bufs[b].size < size) { guest_free(bufs[b].shadow); bufs[b].shadow = 0; }
        bufs[b].size = size;
    }
    p_glBufferData(t, (GLsizeiptr)size, P(2), A(3));
}
static void h_glBufferSubData(cpu_t *c) { p_glBufferSubData(A(0), (GLintptr)A(1), (GLsizeiptr)A(2), P(3)); }
static void h_glDeleteBuffers(cpu_t *c)
{
    GLsizei n = I(0);
    gptr ids = A(1);
    for (GLsizei i = 0; i < n; i++) {
        u32 b = ld32(ids + 4 * i);
        if (b < MAX_BUFS) { guest_free(bufs[b].shadow); bufs[b].shadow = 0; bufs[b].size = 0; }
        if (b == bound_array) bound_array = 0;
        if (b == bound_elem) bound_elem = 0;
    }
    p_glDeleteBuffers(n, P(1));
}
/* client arrays are guest pointers; with a buffer bound the "pointer" is an offset */
static inline bool draw_hidden(void);
static void h_glDrawElements(cpu_t *c)
{
    gptr ix = A(3);
    if (draw_hidden()) return;
    p_glDrawElements(A(0), I(1), A(2), bound_elem ? (const void *)(uintptr_t)ix : gp(ix));
}
static void h_glVertexAttribPointer(cpu_t *c)
{
    gptr ptr = A(5);
    p_glVertexAttribPointer(A(0), I(1), A(2), (GLboolean)A(3), I(4),
                            bound_array ? (const void *)(uintptr_t)ptr : gp(ptr));
}
static void h_glGetVertexAttribPointerv(cpu_t *c)
{
    void *hp = NULL;
    p_glGetVertexAttribPointerv(A(0), A(1), &hp);
    u8 *h = hp;
    st32(A(2), (h >= g_mem && h < g_mem + 0x100000000ULL) ? (u32)(h - g_mem) : (u32)(uintptr_t)hp);
}
/* desktop GL: GLSL ES 1.00 -> GLSL 1.20 (precision qualifiers compiled out, ES-only lines dropped) */
static char *gles_to_desktop(const GLchar *const *src, const GLint *lens, int n)
{
    size_t total = 128;
    for (int i = 0; i < n; i++) total += (lens && lens[i] >= 0) ? (size_t)lens[i] : strlen(src[i]);
    char *in = malloc(total), *out = malloc(total + 128), *q = in;
    for (int i = 0; i < n; i++) {
        size_t l = (lens && lens[i] >= 0) ? (size_t)lens[i] : strlen(src[i]);
        memcpy(q, src[i], l);
        q += l;
    }
    *q = 0;
    char *o = out + sprintf(out, "#version 120\n#define lowp\n#define mediump\n#define highp\n");
    for (char *line = in; *line; ) {
        char *eol = strchr(line, '\n');
        size_t len = eol ? (size_t)(eol - line + 1) : strlen(line);
        const char *t = line;
        while (*t == ' ' || *t == '\t') t++;
        bool drop = !strncmp(t, "precision ", 10) || !strncmp(t, "#version", 8) ||
                    (!strncmp(t, "#extension", 10) && (strstr(t, "GL_OES_") || strstr(t, "GL_EXT_shader_texture_lod")));
        if (drop) *o++ = '\n';                             /* keep line numbers for compile logs */
        else { memcpy(o, line, len); o += len; }
        line += len;
    }
    *o = 0;
    free(in);
    return out;
}

static void h_glShaderSource(cpu_t *c)
{
    GLsizei n = I(1);
    gptr strs = A(2), lens = A(3);
    const GLchar *hs[64];
    if (n > 64) n = 64;
    for (GLsizei i = 0; i < n; i++) hs[i] = g2h(ld32(strs + 4 * i));
    const char *dump = getenv("RR2_DUMP_SHADERS");         /* debug: write plaintext shaders */
    if (dump) {
        char path[512];
        snprintf(path, sizeof(path), "%s/shader_%u.glsl", dump, A(0));
        FILE *f = fopen(path, "w");
        for (GLsizei i = 0; f && i < n; i++) {
            s32 len = lens ? (s32)ld32(lens + 4 * i) : -1;
            fwrite(hs[i], 1, len < 0 ? strlen(hs[i]) : (size_t)len, f);
        }
        if (f) fclose(f);
    }
    const char *dbg = getenv("RR2_DBG_ROAD");               /* debug: show one road shader input */
    if (dbg && n == 1 && strstr(hs[0], "v_Position") && strstr(hs[0], "gl_FragColor")) {
        static const char *const outs[] = {
            "gl_FragColor = vec4(texture2D(s_Texture0, v_TexCoord0).rgb, 1.0);",
            "gl_FragColor = vec4(texture2D(s_Texture1, v_TexCoord1).rgb, 1.0);",
            "gl_FragColor = vec4(v_Colour.rgb, 1.0);",
            "gl_FragColor = vec4(texture2D(s_Texture2, v_Position).ggg, 1.0);",
            "gl_FragColor = vec4(fract(v_TexCoord1), 0.0, 1.0);",
            "gl_FragColor = vec4(texture2D(s_Texture1, fract(v_TexCoord1)).rgb, 1.0);",
            "gl_FragColor = vec4(abs(v_TexCoord1) * 0.1, 0.0, 1.0);",
        };
        const char *at = strstr(hs[0], "gl_FragColor = color;");
        if (at) {
            size_t pre = (size_t)(at - hs[0]);
            char *patched = malloc(strlen(hs[0]) + 128);
            sprintf(patched, "%.*s%s}\n", (int)pre, hs[0], outs[atoi(dbg) % 7]);
            p_glShaderSource(A(0), 1, (const GLchar *const *)&patched, NULL);
            free(patched);
            return;
        }
    }
    if (g_gl_desktop) {
        GLint hl[64];
        for (GLsizei i = 0; i < n && lens; i++) hl[i] = (GLint)ld32(lens + 4 * i);
        char *t = gles_to_desktop(hs, lens ? hl : NULL, n);
        p_glShaderSource(A(0), 1, (const GLchar *const *)&t, NULL);
        free(t);
        return;
    }
    p_glShaderSource(A(0), n, hs, lens ? g2h(lens) : NULL);
}

static gptr guest_str(const char *s)
{
    static struct { const char *h; gptr g; } cache[16];
    for (int i = 0; i < 16; i++) if (cache[i].h == s) return cache[i].g;
    for (int i = 0; i < 16; i++)
        if (!cache[i].h) {
            size_t n = strlen(s) + 1;
            cache[i].h = s;
            cache[i].g = hle_data_alloc((u32)n, 4);
            memcpy(g2h(cache[i].g), s, n);
            return cache[i].g;
        }
    return 0;
}
static void h_glGetString(cpu_t *c)
{
    GLenum name = A(0);
    const GLchar *s = p_glGetString(name);
    if (g_gl_desktop && name == 0x1F02) s = "OpenGL ES 2.0 rr2emu (desktop GL)";   /* the game expects ES strings */
    if (g_gl_desktop && name == 0x8B8C) s = "OpenGL ES GLSL ES 1.00";
    if (name == 0x1F03 && s) {                     /* advertise the ATC we emulate */
        static char *ext;
        if (!ext) {
            ext = malloc(strlen(s) + 64);
            sprintf(ext, "%s GL_AMD_compressed_ATC_texture", s);
        }
        s = ext;
    }
    RET(s ? guest_str(s) : 0);
}

static const GLenum atc_formats[3] = { 0x8C92, 0x8C93, 0x87EE };
static void h_glGetIntegerv(cpu_t *c)
{
    GLenum pn = A(0);
    gptr out = A(1);
    GLint n = 0;
    if (pn == 0x86A2) {                            /* NUM_COMPRESSED_TEXTURE_FORMATS */
        p_glGetIntegerv(pn, &n);
        st32(out, (u32)(n + 3));
        return;
    }
    if (pn == 0x86A3) {                            /* COMPRESSED_TEXTURE_FORMATS */
        p_glGetIntegerv(0x86A2, &n);
        GLint *tmp = calloc((size_t)n + 1, sizeof(GLint));
        p_glGetIntegerv(pn, tmp);
        for (GLint i = 0; i < n; i++) st32(out + 4 * i, (u32)tmp[i]);
        for (int i = 0; i < 3; i++) st32(out + 4 * (n + i), atc_formats[i]);
        free(tmp);
        return;
    }
    p_glGetIntegerv(pn, P(1));
    /* report 2011-phone limits: the game sizes per-unit state from these */
    static const struct { GLenum pn; s32 max; } caps[] = {
        { 0x8B4D, 8 },      /* MAX_COMBINED_TEXTURE_IMAGE_UNITS */
        { 0x8872, 8 },      /* MAX_TEXTURE_IMAGE_UNITS */
        { 0x8B4C, 4 },      /* MAX_VERTEX_TEXTURE_IMAGE_UNITS */
        { 0x8869, 16 },     /* MAX_VERTEX_ATTRIBS */
        { 0x0D33, 4096 },   /* MAX_TEXTURE_SIZE */
    };
    for (unsigned i = 0; i < sizeof(caps) / sizeof(caps[0]); i++)
        if (pn == caps[i].pn && (s32)ld32(out) > caps[i].max) st32(out, (u32)caps[i].max);
    VLOG(1, "[gl] glGetIntegerv(%04x) = %d\n", pn, (s32)ld32(out));
}

/* mapped buffers: hand the guest a shadow, upload it on unmap */
static void h_glMapBufferOES(cpu_t *c)
{
    GLuint b = *buf_slot_for(A(0));
    if (b >= MAX_BUFS || !bufs[b].size) { RET(0); return; }
    if (!bufs[b].shadow) bufs[b].shadow = guest_malloc(bufs[b].size);
    RET(bufs[b].shadow);
}
static void h_glUnmapBufferOES(cpu_t *c)
{
    GLenum t = A(0);
    GLuint b = *buf_slot_for(t);
    if (b < MAX_BUFS && bufs[b].shadow)
        p_glBufferSubData(t, 0, (GLsizeiptr)bufs[b].size, g2h(bufs[b].shadow));
    RET(1);
}
static void h_glGetBufferPointervOES(cpu_t *c)
{
    GLuint b = *buf_slot_for(A(0));
    st32(A(2), b < MAX_BUFS ? bufs[b].shadow : 0);
}

/* ---- texture binding tracking: lets us hide chosen HUD images (touch pedals) ---- */

#define MAX_TEX 16384
static u8 tex_hidden[MAX_TEX];
static GLuint tex_bound[32];
static u32 tex_unit;

/* redundant-state filter: the host driver only sees calls that change something */
u64 g_gl_skipped;
static void t_glActiveTexture(cpu_t *c)
{
    u32 u = (A(0) - 0x84C0) & 31;
    if (u == tex_unit) { g_gl_skipped++; return; }
    tex_unit = u;
    p_glActiveTexture(A(0));
}
static void t_glBindTexture(cpu_t *c)
{
    if (A(0) == 0x0DE1) {
        if (tex_bound[tex_unit] == A(1)) { g_gl_skipped++; return; }
        tex_bound[tex_unit] = A(1);
    }
    p_glBindTexture(A(0), A(1));
}
static void t_glDeleteTextures(cpu_t *c)
{
    GLsizei n = I(0);
    for (GLsizei i = 0; i < n; i++) {          /* GL unbinds deleted textures everywhere */
        u32 id = ld32(A(1) + 4 * (u32)i);
        for (int u = 0; u < 32; u++) if (tex_bound[u] == id) tex_bound[u] = 0;
    }
    p_glDeleteTextures(n, P(1));
}

/* capability enables (cap values are small and sparse: tiny open hash) */
static struct { GLenum cap; s8 on; } caps[64];
static void cap_set(cpu_t *c, bool on)
{
    GLenum cap = A(0);
    u32 h = (cap * 2654435761u) >> 26;
    for (u32 k = 0; k < 64; k++, h = (h + 1) & 63) {
        if (caps[h].cap == cap) {
            if (caps[h].on == on) { g_gl_skipped++; return; }
            break;
        }
        if (!caps[h].cap) { caps[h].cap = cap; break; }
    }
    caps[h].on = on;
    if (on) p_glEnable(cap); else p_glDisable(cap);
}
static void t_glEnable(cpu_t *c)  { cap_set(c, true); }
static void t_glDisable(cpu_t *c) { cap_set(c, false); }

/* programs + uniform shadow: values live in the program object, keyed (program, location) */
#define MAX_PROG 4096
static GLuint cur_prog;
static u32 prog_gen[MAX_PROG];
#define UC_N 16384
static struct { u32 key, gen; u8 len; u8 val[64]; } ucache[UC_N];

static bool uni_same(GLint loc, const void *v, u32 len)
{
    if (loc < 0 || len > 64 || cur_prog >= MAX_PROG) return false;
    u32 key = (cur_prog << 16) | ((u32)loc & 0xFFFF) | 0x80000000u;
    u32 h = (key * 2654435761u) >> 18;
    for (u32 k = 0; k < 8; k++, h = (h + 1) & (UC_N - 1)) {
        if (ucache[h].key == key) {
            if (ucache[h].gen == prog_gen[cur_prog] && ucache[h].len == len && !memcmp(ucache[h].val, v, len))
                return true;
            break;
        }
        if (!ucache[h].key) break;
    }
    ucache[h].key = key; ucache[h].gen = prog_gen[cur_prog]; ucache[h].len = (u8)len;
    memcpy(ucache[h].val, v, len);
    return false;
}
static void t_glUseProgram(cpu_t *c)
{
    if (A(0) == cur_prog) { g_gl_skipped++; return; }
    cur_prog = A(0);
    p_glUseProgram(cur_prog);
}
static void t_glLinkProgram(cpu_t *c)  { if (A(0) < MAX_PROG) prog_gen[A(0)]++; p_glLinkProgram(A(0)); }
static void t_glDeleteProgram(cpu_t *c) { if (A(0) < MAX_PROG) prog_gen[A(0)]++; p_glDeleteProgram(A(0)); }
#define UNI_V(name, T, n, call) static void t_##name(cpu_t *c) { \
    u32 len = (u32)I(1) * (n) * 4; \
    if (I(1) > 0 && uni_same(I(0), P(2), len)) { g_gl_skipped++; return; } call; }
UNI_V(glUniform1fv, f, 1, p_glUniform1fv(I(0), I(1), P(2)))
UNI_V(glUniform2fv, f, 2, p_glUniform2fv(I(0), I(1), P(2)))
UNI_V(glUniform3fv, f, 3, p_glUniform3fv(I(0), I(1), P(2)))
UNI_V(glUniform4fv, f, 4, p_glUniform4fv(I(0), I(1), P(2)))
UNI_V(glUniform1iv, i, 1, p_glUniform1iv(I(0), I(1), P(2)))
UNI_V(glUniform2iv, i, 2, p_glUniform2iv(I(0), I(1), P(2)))
UNI_V(glUniform3iv, i, 3, p_glUniform3iv(I(0), I(1), P(2)))
UNI_V(glUniform4iv, i, 4, p_glUniform4iv(I(0), I(1), P(2)))
#define UNI_M(name, n) static void t_##name(cpu_t *c) { \
    u32 len = (u32)I(1) * (n) * 4; \
    if (I(1) > 0 && !A(2) && uni_same(I(0), g2h(A(3)), len)) { g_gl_skipped++; return; } \
    p_##name(I(0), I(1), A(2), P(3)); }
UNI_M(glUniformMatrix2fv, 4)
UNI_M(glUniformMatrix3fv, 9)
UNI_M(glUniformMatrix4fv, 16)
#define UNI_S(name, argexpr, nargs, call) static void t_##name(cpu_t *c) { \
    u32 v[4] = argexpr; \
    if (uni_same(I(0), v, (nargs) * 4)) { g_gl_skipped++; return; } call; }
UNI_S(glUniform1f, { A(1) }, 1, p_glUniform1f(I(0), F(1)))
UNI_S(glUniform2f, { A(1) COMMA A(2) }, 2, p_glUniform2f(I(0), F(1), F(2)))
UNI_S(glUniform3f, { A(1) COMMA A(2) COMMA A(3) }, 3, p_glUniform3f(I(0), F(1), F(2), F(3)))
UNI_S(glUniform4f, { A(1) COMMA A(2) COMMA A(3) COMMA A(4) }, 4, p_glUniform4f(I(0), F(1), F(2), F(3), F(4)))
UNI_S(glUniform1i, { A(1) }, 1, p_glUniform1i(I(0), I(1)))

/* errors: the game only polls these for debug logging */
static void t_glGetError(cpu_t *c) { RET(g_verbose ? p_glGetError() : 0); }
/* the upload right after the VFS opened a hidden image belongs to that image */
static inline void tex_note_upload(GLint level)
{
    if (level) return;
    GLuint t = tex_bound[tex_unit];
    if (t < MAX_TEX) tex_hidden[t] = (u8)g_hide_next_tex;
    g_hide_next_tex = 0;
}
volatile int g_race_hud_drawn;
static inline bool draw_hidden(void)
{
    GLuint t = tex_bound[0];
    if (t >= MAX_TEX || !tex_hidden[t]) return false;
    if (tex_hidden[t] == 2) { g_race_hud_drawn = 1; return false; }
    return true;
}
static void t_glDrawArrays(cpu_t *c) { if (!draw_hidden()) p_glDrawArrays(A(0), I(1), I(2)); }

/* ---- ATC (Adreno) texture decode ---- */

static inline void atc_color_block(const u8 *s, u8 out[16][4])
{
    u32 c0 = s[0] | (s[1] << 8), c1 = s[2] | (s[3] << 8), bits;
    memcpy(&bits, s + 4, 4);
    int p[4][3];
    int r0 = ((c0 >> 10) & 31) * 255 / 31, g0 = ((c0 >> 5) & 31) * 255 / 31, b0 = (c0 & 31) * 255 / 31;
    int r1 = ((c1 >> 11) & 31) * 255 / 31, g1 = ((c1 >> 5) & 63) * 255 / 63, b1 = (c1 & 31) * 255 / 31;
    if (!(c0 & 0x8000)) {
        p[0][0] = r0; p[0][1] = g0; p[0][2] = b0;
        p[3][0] = r1; p[3][1] = g1; p[3][2] = b1;
        for (int k = 0; k < 3; k++) {
            p[1][k] = (5 * p[0][k] + 3 * p[3][k]) / 8;
            p[2][k] = (3 * p[0][k] + 5 * p[3][k]) / 8;
        }
    } else {
        p[0][0] = p[0][1] = p[0][2] = 0;
        p[2][0] = r0; p[2][1] = g0; p[2][2] = b0;
        p[3][0] = r1; p[3][1] = g1; p[3][2] = b1;
        for (int k = 0; k < 3; k++) {
            int v = p[2][k] - p[3][k] / 4;
            p[1][k] = v < 0 ? 0 : v;
        }
    }
    for (int i = 0; i < 16; i++) {
        int ix = (bits >> (2 * i)) & 3;
        out[i][0] = (u8)p[ix][0]; out[i][1] = (u8)p[ix][1]; out[i][2] = (u8)p[ix][2]; out[i][3] = 255;
    }
}

static inline void dxt5_alpha(const u8 *s, u8 out[16][4])
{
    u8 a[8];
    a[0] = s[0]; a[1] = s[1];
    if (a[0] > a[1]) for (int i = 1; i < 7; i++) a[i + 1] = (u8)(((7 - i) * a[0] + i * a[1]) / 7);
    else {
        for (int i = 1; i < 5; i++) a[i + 1] = (u8)(((5 - i) * a[0] + i * a[1]) / 5);
        a[6] = 0; a[7] = 255;
    }
    u64 bits = 0;
    for (int i = 0; i < 6; i++) bits |= (u64)s[2 + i] << (8 * i);
    for (int i = 0; i < 16; i++) out[i][3] = a[(bits >> (3 * i)) & 7];
}

static u8 *atc_decode(GLenum fmt, int w, int h, const u8 *src)
{
    int bw = (w + 3) / 4, bh = (h + 3) / 4, bsz = fmt == 0x8C92 ? 8 : 16;
    u8 *dst = malloc((size_t)w * h * 4);
    u8 px[16][4];
    for (int by = 0; by < bh; by++)
        for (int bx = 0; bx < bw; bx++) {
            const u8 *s = src + (size_t)(by * bw + bx) * bsz;
            atc_color_block(bsz == 8 ? s : s + 8, px);
            if (fmt == 0x8C93)
                for (int i = 0; i < 16; i++) px[i][3] = (u8)(((s[i / 2] >> (4 * (i & 1))) & 15) * 17);
            else if (fmt == 0x87EE)
                dxt5_alpha(s, px);
            for (int y = 0; y < 4 && by * 4 + y < h; y++)
                for (int x = 0; x < 4 && bx * 4 + x < w; x++)
                    memcpy(dst + ((size_t)(by * 4 + y) * w + bx * 4 + x) * 4, px[y * 4 + x], 4);
        }
    return dst;
}

static GLenum (*p_glGetError2)(void);
#define GLCHK(what) do { if (g_verbose) { GLenum e_ = p_glGetError2(); if (e_) \
    LOG("[gl] %s -> error %04x (fmt %04x type %04x %dx%d lvl %d)\n", what, e_, A(2), A(7), I(3), I(4), I(1)); } } while (0)
static void h_glTexImage2Dchk(cpu_t *c)
{
    { u32 bpp = A(7) == 0x1401 ? (A(6) == 0x1908 ? 4 : A(6) == 0x1907 ? 3 : A(6) == 0x190A ? 2 : 1) : 2;
      g_tex_bytes += (u64)I(3) * I(4) * bpp; g_tex_uploads++; }
    tex_note_upload(I(1));
    p_glTexImage2Dx(A(0), I(1), I(2), I(3), I(4), I(5), A(6), A(7), P(8));
    GLCHK("glTexImage2D");
}
/* ---- ATC -> S3TC transcode (same block size: 8 -> DXT1, 16 -> DXT3/DXT5) ---- */

static bool g_s3tc;

static inline u16 rgb565(int r, int g, int b) { return (u16)(((r * 31 + 127) / 255) << 11 | ((g * 63 + 127) / 255) << 5 | ((b * 31 + 127) / 255)); }

/* re-fit a 4x4 RGB block to BC1 4-colour mode (used for ATC's black-mode blocks) */
static void bc1_fit(u8 px[16][4], u8 out[8])
{
    int lo = 0, hi = 0, lum[16];
    for (int i = 0; i < 16; i++) {
        lum[i] = px[i][0] * 2 + px[i][1] * 4 + px[i][2];
        if (lum[i] < lum[lo]) lo = i;
        if (lum[i] > lum[hi]) hi = i;
    }
    u16 e0 = rgb565(px[hi][0], px[hi][1], px[hi][2]), e1 = rgb565(px[lo][0], px[lo][1], px[lo][2]);
    if (e0 == e1) { memcpy(out, &e0, 2); memcpy(out + 2, &e1, 2); memset(out + 4, 0, 4); return; }
    if (e0 < e1) { u16 t = e0; e0 = e1; e1 = t; int tl = lo; lo = hi; hi = tl; }
    int p0 = lum[hi], p1 = lum[lo], bits = 0;
    for (int i = 0; i < 16; i++) {
        /* position along the endpoint axis in thirds: 0=e0, 2=2/3e0, 3=1/3e0, 1=e1 */
        int t = p0 == p1 ? 0 : ((p0 - lum[i]) * 3 + (p0 - p1) / 2) / (p0 - p1);
        static const int map[4] = { 0, 2, 3, 1 };
        bits |= map[t < 0 ? 0 : t > 3 ? 3 : t] << (2 * i);
    }
    memcpy(out, &e0, 2); memcpy(out + 2, &e1, 2); memcpy(out + 4, &bits, 4);
}

/* ATC colour block -> BC1 colour block. four_color_forced: DXT3/5 always use 4-colour mode */
static void atc_to_bc1(const u8 *s, u8 *o, bool four_color_forced)
{
    u32 c0 = s[0] | (s[1] << 8), c1 = s[2] | (s[3] << 8), bits;
    memcpy(&bits, s + 4, 4);
    if (c0 & 0x8000) {                            /* black mode: no exact BC1 form, re-fit */
        u8 px[16][4];
        atc_color_block(s, px);
        bc1_fit(px, o);
        return;
    }
    u16 e0 = (u16)(((c0 & 0x7C00) << 1) | ((c0 & 0x03E0) << 1) | ((c0 >> 4) & 0x20) | (c0 & 0x1F));
    u16 e1 = (u16)c1;
    /* ATC 0,1,2,3 = c0, 5/8c0, 3/8c0, c1  ->  BC1 0=e0 1=e1 2=2/3e0 3=1/3e0 */
    static const u8 keep[4] = { 0, 2, 3, 1 }, swap[4] = { 1, 3, 2, 0 };
    const u8 *m = keep;
    if (!four_color_forced && e0 <= e1) {
        if (e0 == e1) { bits = 0; }
        else { u16 t = e0; e0 = e1; e1 = t; m = swap; }
    }
    u32 nb = 0;
    for (int i = 0; i < 16; i++) nb |= (u32)m[(bits >> (2 * i)) & 3] << (2 * i);
    memcpy(o, &e0, 2); memcpy(o + 2, &e1, 2); memcpy(o + 4, &nb, 4);
}

/* returns the S3TC format and fills dst (same size as src) */
static GLenum atc_transcode(GLenum fmt, int w, int h, const u8 *src, u8 *dst)
{
    int nblk = ((w + 3) / 4) * ((h + 3) / 4);
    if (fmt == 0x8C92) {
        for (int i = 0; i < nblk; i++) atc_to_bc1(src + 8 * i, dst + 8 * i, false);
        return 0x83F0;                            /* COMPRESSED_RGB_S3TC_DXT1 */
    }
    for (int i = 0; i < nblk; i++) {
        memcpy(dst + 16 * i, src + 16 * i, 8);    /* explicit alpha == DXT3, interpolated == DXT5 */
        atc_to_bc1(src + 16 * i + 8, dst + 16 * i + 8, true);
    }
    return fmt == 0x8C93 ? 0x83F2 : 0x83F3;
}

static void h_glCompressedTexImage2D(cpu_t *c)
{
    GLenum t = A(0), fmt = A(2);
    GLint lvl = I(1), w = I(3), h = I(4), border = I(5);
    tex_note_upload(lvl);
    VLOG(1, "[gl] compressed upload fmt %04x lvl %d %dx%d size %d\n", fmt, lvl, w, h, I(6));
    if ((fmt == 0x8C92 || fmt == 0x8C93 || fmt == 0x87EE) && g_s3tc) {
        size_t n = (size_t)((w + 3) / 4) * ((h + 3) / 4) * (fmt == 0x8C92 ? 8 : 16);
        u8 *bc = malloc(n);
        GLenum sf = atc_transcode(fmt, w, h, g2h(A(7)), bc);
        p_glCompressedTexImage2D(t, lvl, sf, w, h, border, (GLsizei)n, bc);
        free(bc);
        g_tex_bytes += n; g_tex_uploads++;
        GLCHK("s3tc upload");
        return;
    }
    if (fmt == 0x8C92 || fmt == 0x8C93 || fmt == 0x87EE) {
        u8 *rgba = atc_decode(fmt, w, h, g2h(A(7)));
        g_tex_bytes += (u64)w * h * 4; g_tex_uploads++;
        p_glTexImage2Dx(t, lvl, 0x1908, w, h, border, 0x1908, 0x1401, rgba);
        free(rgba);
        GLCHK("atc upload");
        return;
    }
    p_glCompressedTexImage2D(t, lvl, fmt, w, h, border, I(6), P(7));
}

/* ---- screenshot: framebuffer -> PNG (stored deflate, no zlib needed) ---- */

static u32 crc_tab[256];
static u32 png_crc(const u8 *p, size_t n, u32 crc)
{
    if (!crc_tab[1])
        for (u32 i = 0; i < 256; i++) {
            u32 v = i;
            for (int k = 0; k < 8; k++) v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            crc_tab[i] = v;
        }
    for (size_t i = 0; i < n; i++) crc = crc_tab[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}
static void be32(u8 *p, u32 v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void png_chunk(FILE *f, const char *type, const u8 *data, u32 n)
{
    u8 hdr[8]; be32(hdr, n); memcpy(hdr + 4, type, 4);
    fwrite(hdr, 1, 8, f);
    if (n) fwrite(data, 1, n, f);
    u32 crc = png_crc((const u8 *)type, 4, 0xFFFFFFFFu);
    crc = png_crc(data, n, crc) ^ 0xFFFFFFFFu;
    u8 t[4]; be32(t, crc); fwrite(t, 1, 4, f);
}

bool glhost_screenshot(const char *path, int w, int h)
{
    if (!p_glReadPixels) *(void **)&p_glReadPixels = host_gl_proc("glReadPixels");
    if (!p_glReadPixels) return false;
    u8 *px = malloc((size_t)w * h * 4);
    p_glReadPixels(0, 0, w, h, 0x1908, 0x1401, px);
    size_t row = (size_t)w * 3 + 1, raw_n = row * h;
    u8 *raw = malloc(raw_n);
    for (int y = 0; y < h; y++) {                  /* GL rows are bottom-up */
        u8 *r = raw + row * y, *s = px + (size_t)(h - 1 - y) * w * 4;
        r[0] = 0;
        for (int x = 0; x < w; x++) { r[1 + 3 * x] = s[4 * x]; r[2 + 3 * x] = s[4 * x + 1]; r[3 + 3 * x] = s[4 * x + 2]; }
    }
    size_t nblk = (raw_n + 65534) / 65535, zn = 2 + raw_n + 5 * nblk + 4;
    u8 *z = malloc(zn), *o = z;
    *o++ = 0x78; *o++ = 0x01;
    u32 a = 1, b = 0;
    for (size_t i = 0; i < raw_n; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
    for (size_t off = 0; off < raw_n; off += 65535) {
        u32 len = raw_n - off > 65535 ? 65535 : (u32)(raw_n - off);
        *o++ = off + len >= raw_n;
        *o++ = len & 0xFF; *o++ = len >> 8; *o++ = ~len & 0xFF; *o++ = (~len >> 8) & 0xFF;
        memcpy(o, raw + off, len); o += len;
    }
    be32(o, (b << 16) | a); o += 4;
    FILE *f = fopen(path, "wb");
    if (f) {
        static const u8 sig[8] = { 0x89, 'P', 'N', 'G', 13, 10, 26, 10 };
        u8 ihdr[13]; be32(ihdr, (u32)w); be32(ihdr + 4, (u32)h);
        ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = ihdr[11] = ihdr[12] = 0;
        fwrite(sig, 1, 8, f);
        png_chunk(f, "IHDR", ihdr, 13);
        png_chunk(f, "IDAT", z, (u32)(o - z));
        png_chunk(f, "IEND", NULL, 0);
        fclose(f);
    }
    free(px); free(raw); free(z);
    return f != NULL;
}

static void (*p_glGetShaderiv2)(GLuint, GLenum, GLint *);
static void (*p_glGetShaderInfoLog2)(GLuint, GLsizei, GLsizei *, GLchar *);
static void (*p_glCompileShader2)(GLuint);
static void h_glCompileShaderChk(cpu_t *c)
{
    GLuint sh = A(0); GLint ok = 1;
    p_glCompileShader2(sh);
    p_glGetShaderiv2(sh, 0x8B81, &ok);          /* COMPILE_STATUS */
    if (!ok) {
        char log[2048]; GLsizei n = 0;
        p_glGetShaderInfoLog2(sh, sizeof(log), &n, log);
        LOG("[gl] shader %u compile FAILED: %.*s\n", sh, n, log);
    }
}

void glhost_report(long frames)
{
    LOG("[gl] redundant calls filtered: %.1f /frame\n", (double)g_gl_skipped / (frames + !frames));
    LOG("[gl] textures: %llu uploads, %.1f MB as uploaded to the GPU\n",
        (unsigned long long)g_tex_uploads, g_tex_bytes / 1048576.0);
    for (int r = 0; r < 14; r++) {
        int best = -1;
        for (int i = 0; i < GLC_COUNT; i++) if (g_glc[i] && (best < 0 || g_glc[i] > g_glc[best])) best = i;
        if (best < 0) break;
        LOG("  %-28s %8.1f /frame\n", glc_names[best], (double)g_glc[best] / (frames + !frames));
        g_glc[best] = 0;
    }
}

/* anisotropic filtering: added wherever the game picks a mipmapped min filter */
static float g_aniso_level;
static void aniso_for(GLenum t, GLint f) { if (f >= 0x2700 && f <= 0x2703) p_glTexParameterf(t, 0x84FE, g_aniso_level); }
static void t_glTexParameteri(cpu_t *c)
{
    if (A(1) == 0x84FE) { p_glTexParameterf(A(0), 0x84FE, g_aniso_level); return; }
    h_glTexParameteri(c);
    if (A(1) == 0x2801) aniso_for(A(0), I(2));
}
static void t_glTexParameterf(cpu_t *c)
{
    if (A(1) == 0x84FE) { p_glTexParameterf(A(0), 0x84FE, g_aniso_level); return; }
    h_glTexParameterf(c);
    if (A(1) == 0x2801) aniso_for(A(0), (GLint)F(2));
}

float glhost_max_aniso(void)
{
    const GLchar *(*gs)(GLenum) = host_gl_proc("glGetString");
    void (*gf)(GLenum, GLfloat *) = host_gl_proc("glGetFloatv");
    const char *ext = gs ? gs(0x1F03) : NULL;
    GLfloat m = 0;
    if (ext && gf && (strstr(ext, "GL_EXT_texture_filter_anisotropic") || strstr(ext, "GL_ARB_texture_filter_anisotropic")))
        gf(0x84FF, &m);
    return m;
}

/* ---- setup ---- */

static void (*p_glClearDepth)(double);
static void (*p_glDepthRange)(double, double);
static void es_ClearDepthf(GLfloat d) { p_glClearDepth(d); }
static void es_DepthRangef(GLfloat a, GLfloat b) { p_glDepthRange(a, b); }
static void es_ReleaseShaderCompiler(void) {}
static void es_GetShaderPrecisionFormat(GLenum sh, GLenum type, GLint *range, GLint *prec)
{
    bool is_int = type >= 0x8DF3;                      /* LOW_INT.. */
    range[0] = range[1] = is_int ? 31 : 127;
    *prec = is_int ? 0 : 23;
}
/* desktop drivers without ARB_ES2_compatibility lack these; the rest of GLES2 is core GL 2.1 */
static void *es_fallback(const char *n)
{
    *(void **)&p_glClearDepth = host_gl_proc("glClearDepth");
    *(void **)&p_glDepthRange = host_gl_proc("glDepthRange");
    if (!strcmp(n, "glClearDepthf") && p_glClearDepth) return (void *)es_ClearDepthf;
    if (!strcmp(n, "glDepthRangef") && p_glDepthRange) return (void *)es_DepthRangef;
    if (!strcmp(n, "glReleaseShaderCompiler")) return (void *)es_ReleaseShaderCompiler;
    if (!strcmp(n, "glGetShaderPrecisionFormat")) return (void *)es_GetShaderPrecisionFormat;
    return NULL;
}

bool glhost_init(void)
{
#define X(n, r, params, call) \
    if (!(*(void **)&p_##n = host_gl_proc(#n)) && !(g_gl_desktop && (*(void **)&p_##n = es_fallback(#n)))) \
        { LOG("[gl] host lacks %s\n", #n); return false; }
    GL_PLAIN(X)
#undef X
#define H(n) if (!(*(void **)&p_##n = host_gl_proc(#n))) { LOG("[gl] host lacks %s\n", #n); return false; }
    H(glBindBuffer) H(glBufferData) H(glBufferSubData) H(glDeleteBuffers) H(glDrawElements)
    H(glVertexAttribPointer) H(glGetVertexAttribPointerv) H(glShaderSource) H(glGetString)
    H(glGetIntegerv) H(glCompressedTexImage2D)
#undef H
    *(void **)&p_glTexImage2Dx = host_gl_proc("glTexImage2D");

#define X(n, r, params, call) hle_register(#n, h_##n);
    GL_PLAIN(X)
#undef X
    *(void **)&p_glGetShaderiv2 = host_gl_proc("glGetShaderiv");
    *(void **)&p_glGetShaderInfoLog2 = host_gl_proc("glGetShaderInfoLog");
    *(void **)&p_glCompileShader2 = host_gl_proc("glCompileShader");
    hle_register("glCompileShader", h_glCompileShaderChk);
    *(void **)&p_glGetError2 = host_gl_proc("glGetError");
    hle_register("glTexImage2D", h_glTexImage2Dchk);
    hle_register("glActiveTexture", t_glActiveTexture);
    hle_register("glBindTexture", t_glBindTexture);
    if (!getenv("RR2_NO_GLCACHE")) {
#define R(n) hle_register(#n, t_##n);
        R(glDeleteTextures) R(glEnable) R(glDisable) R(glUseProgram) R(glLinkProgram) R(glDeleteProgram)
        R(glUniform1fv) R(glUniform2fv) R(glUniform3fv) R(glUniform4fv)
        R(glUniform1iv) R(glUniform2iv) R(glUniform3iv) R(glUniform4iv)
        R(glUniformMatrix2fv) R(glUniformMatrix3fv) R(glUniformMatrix4fv)
        R(glUniform1f) R(glUniform2f) R(glUniform3f) R(glUniform4f) R(glUniform1i) R(glGetError)
#undef R
    }
    float amax = glhost_max_aniso();
    g_aniso_level = G.aniso > amax ? amax : G.aniso;
    if (g_aniso_level > 1) {
        hle_register("glTexParameteri", t_glTexParameteri);
        hle_register("glTexParameterf", t_glTexParameterf);
        LOG("[gl] anisotropic filtering %.0fx\n", g_aniso_level);
    }
    hle_register("glDrawArrays", t_glDrawArrays);
    hle_register("glBindBuffer", h_glBindBuffer);
    hle_register("glBufferData", h_glBufferData);
    hle_register("glBufferSubData", h_glBufferSubData);
    hle_register("glDeleteBuffers", h_glDeleteBuffers);
    hle_register("glDrawElements", h_glDrawElements);
    hle_register("glVertexAttribPointer", h_glVertexAttribPointer);
    hle_register("glGetVertexAttribPointerv", h_glGetVertexAttribPointerv);
    hle_register("glShaderSource", h_glShaderSource);
    hle_register("glGetString", h_glGetString);
    hle_register("glGetIntegerv", h_glGetIntegerv);
    hle_register("glCompressedTexImage2D", h_glCompressedTexImage2D);
    hle_register("glMapBufferOES", h_glMapBufferOES);
    hle_register("glUnmapBufferOES", h_glUnmapBufferOES);
    hle_register("glGetBufferPointervOES", h_glGetBufferPointervOES);
    LOG("[gl] host: %s | %s | %s\n", p_glGetString(0x1F00), p_glGetString(0x1F01), p_glGetString(0x1F02));
    if (g_gl_desktop) {                           /* GLES2 behaviour for gl_PointSize / gl_PointCoord */
        p_glEnable(0x8642);
        p_glEnable(0x8861);
    }
    const char *ext = p_glGetString(0x1F03);
    g_s3tc = ext && strstr(ext, "GL_EXT_texture_compression_s3tc") && !getenv("RR2_NO_S3TC");
    LOG("[gl] ATC textures -> %s\n", g_s3tc ? "S3TC (transcoded, same size)" : "RGBA8 (decoded)");
    return true;
}
