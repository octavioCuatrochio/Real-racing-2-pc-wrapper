/*
 * gles.c - phase-1 GLES2/EGL stubs: no-op everything, but with plausible
 * return values so game logic proceeds headless. Phase 2 replaces this with
 * real translation to desktop GL. Unknown gl* imports still hit the generic
 * log-once stub via hle_bind, so this file only registers the functions
 * whose return values (or side effects) matter.
 */
#include "emu.h"

static u32 g_next_id = 100;

/* buffer shadows for glMapBufferOES */
#define MAX_GLBUF 512
static struct { u32 id; u32 size; gptr shadow; } g_buffers[MAX_GLBUF];
static int g_nbuffers;

static gptr buffer_shadow(u32 id, u32 size)
{
    for (int i = 0; i < g_nbuffers; i++)
        if (g_buffers[i].id == id) {
            if (g_buffers[i].size < size) g_buffers[i].size = size;
            if (!g_buffers[i].shadow) g_buffers[i].shadow = hle_data_alloc(g_buffers[i].size + 4096, 16);
            return g_buffers[i].shadow;
        }
    if (g_nbuffers >= MAX_GLBUF) return 0;
    g_buffers[g_nbuffers].id = id;
    g_buffers[g_nbuffers].size = size;
    g_buffers[g_nbuffers].shadow = hle_data_alloc(size + 4096, 16);
    return g_buffers[g_nbuffers++].shadow;
}

static gptr gl_string(const char *s)
{
    static gptr ptrs[8];
    static int used[8];
    int slot = -1;
    for (int i = 0; i < 8; i++)
        if (used[i] && !strcmp(gstr(ptrs[i]), s)) return ptrs[i];
    for (int i = 0; i < 8; i++)
        if (!used[i]) { slot = i; break; }
    if (slot < 0) slot = 0;
    if (!used[slot]) {
        ptrs[slot] = hle_data_alloc(256, 4);
        used[slot] = 1;
    }
    snprintf(g2h(ptrs[slot]), 256, "%s", s);
    return ptrs[slot];
}

static void hle_glGetString(cpu_t *c)
{
    if (G.game == 3) {                          /* RR3 picks its texture set (ATC) from an Adreno renderer */
        switch (harg(c, 0)) {
        case 0x1F00: hret(c, gl_string("Qualcomm")); return;
        case 0x1F01: hret(c, gl_string("Adreno (TM) 540")); return;
        case 0x1F03: hret(c, gl_string("GL_AMD_compressed_ATC_texture GL_OES_vertex_array_object GL_OES_depth24 "
                                       "GL_OES_packed_depth_stencil GL_OES_rgb8_rgba8 GL_OES_texture_npot "
                                       "GL_EXT_texture_filter_anisotropic GL_OES_element_index_uint")); return;
        }
    }
    switch (harg(c, 0)) {
    case 0x1F00: hret(c, gl_string("rr2emu")); break;                    /* VENDOR */
    case 0x1F01: hret(c, gl_string("headless-gpu")); break;              /* RENDERER */
    case 0x1F02: hret(c, gl_string("OpenGL ES 2.0 (rr2emu)")); break;    /* VERSION */
    case 0x1F03: hret(c, gl_string("GL_OES_mapbuffer GL_OES_texture_3D GL_OES_compressed_paletted_texture")); break; /* EXTENSIONS */
    case 0x8B8C: hret(c, gl_string("OpenGL ES GLSL 1.00")); break;       /* SHADING_LANGUAGE_VERSION */
    default: hret(c, gl_string("")); break;
    }
}

static void hle_glGetError(cpu_t *c) { hret(c, 0); }

static void hle_glGetIntegerv(cpu_t *c)
{
    u32 pname = harg(c, 0), out = harg(c, 1);
    s32 v = 0;
    switch (pname) {
    case 0x0D33: v = 4096; break;     /* MAX_TEXTURE_SIZE */
    case 0x0D3A: v = 64; break;       /* MAX_VIEWPORT_DIMS (first) */
    case 0x8869: v = 16; break;       /* MAX_VERTEX_ATTRIBS */
    case 0x8872: v = 8; break;        /* MAX_TEXTURE_IMAGE_UNITS */
    case 0x8B4D: v = 8; break;        /* MAX_COMBINED_TEXTURE_IMAGE_UNITS */
    case 0x8B4C: v = 8; break;        /* MAX_VERTEX_TEXTURE_IMAGE_UNITS */
    case 0x8DFB: v = 8; break;        /* MAX_VARYING_VECTORS */
    case 0x826E: v = 8; break;        /* MAX_FRAGMENT_UNIFORM_VECTORS-ish */
    case 0x84E8: v = 1; break;        /* MAX_TEXTURE_UNITS */
    case 0x0D50: v = 2; break;        /* SAMPLES */
    case 0x80A9: v = 1; break;        /* SAMPLE_BUFFERS */
    case 0x0B71: v = 24; break;       /* DEPTH_BITS */
    case 0x0D55: v = 8; break;        /* STENCIL_BITS */
    case 0x84E2: v = 1280; break;     /* just in case */
    default: v = 0; break;
    }
    if (out) st32(out, (u32)v);
    /* array-valued queries get a second element */
    if (pname == 0x0D3A) { st32(out + 4, 720); }
    if (pname == 0x846E || pname == 0x846F) { st32(out, 0); st32(out + 4, 0x3F800000); }
}
static void hle_glGetFloatv(cpu_t *c)
{
    u32 out = harg(c, 1);
    if (out) { stf32(out, 1.0f); stf32(out + 4, 1.0f); }
}
static void hle_glGetBooleanv(cpu_t *c)
{
    u32 out = harg(c, 1);
    if (out) st8(out, 1);
}

static void hle_glCreateShader(cpu_t *c) { hret(c, g_next_id++); }
static void hle_glCreateProgram(cpu_t *c) { hret(c, g_next_id++); }

#define GENIDS(name) \
    static void hle_##name(cpu_t *c) { \
        u32 n = harg(c, 0), out = harg(c, 1); \
        for (u32 i = 0; i < n; i++) st32(out + 4 * i, g_next_id++); \
    }
GENIDS(glGenTextures)
GENIDS(glGenBuffers)
GENIDS(glGenFramebuffers)
GENIDS(glGenRenderbuffers)
GENIDS(glGenFencesNV)

static void hle_glGenPerfMonitorsAMD(cpu_t *c)
{
    u32 n = harg(c, 0), out = harg(c, 1);
    for (u32 i = 0; i < n; i++) st32(out + 4 * i, g_next_id++);
}

static void hle_glIsTexture(cpu_t *c) { hret(c, 1); }
static void hle_glIsBuffer(cpu_t *c) { hret(c, 1); }
static void hle_glIsFramebuffer(cpu_t *c) { hret(c, 1); }
static void hle_glIsRenderbuffer(cpu_t *c) { hret(c, 1); }
static void hle_glIsShader(cpu_t *c) { hret(c, 1); }
static void hle_glIsProgram(cpu_t *c) { hret(c, 1); }
static void hle_glIsEnabled(cpu_t *c) { hret(c, 0); }
static void hle_glIsFenceNV(cpu_t *c) { hret(c, 1); }

static u32 g_next_loc = 1;
static void hle_glGetAttribLocation(cpu_t *c) { hret(c, g_next_loc++ % 15); }
static void hle_glGetUniformLocation(cpu_t *c) { hret(c, g_next_loc++); }

static void hle_glGetShaderiv(cpu_t *c)
{
    u32 pname = harg(c, 1), out = harg(c, 2);
    s32 v = 0;
    if (pname == 0x8B81) v = 1;       /* COMPILE_STATUS */
    else if (pname == 0x8B84) v = 0;  /* INFO_LOG_LENGTH */
    else if (pname == 0x8B4F) v = 0x8B30; /* SHADER_TYPE: pretend FRAGMENT */
    if (out) st32(out, (u32)v);
}
static void hle_glGetProgramiv(cpu_t *c)
{
    u32 pname = harg(c, 1), out = harg(c, 2);
    s32 v = 0;
    if (pname == 0x8B82) v = 1;       /* LINK_STATUS */
    else if (pname == 0x8B83) v = 1;  /* VALIDATE_STATUS */
    else if (pname == 0x8B84) v = 0;  /* INFO_LOG_LENGTH */
    else if (pname == 0x8B89) v = 0;  /* ACTIVE_ATTRIBUTES */
    else if (pname == 0x8B86) v = 0;  /* ACTIVE_UNIFORMS */
    if (out) st32(out, (u32)v);
}
static void hle_glGetShaderInfoLog(cpu_t *c)
{
    u32 out = harg(c, 2);
    if (out) st8(out, 0);
    gptr len = harg(c, 1);
    (void)len;
}
static void hle_glGetProgramInfoLog(cpu_t *c)
{
    u32 out = harg(c, 2);
    if (out) st8(out, 0);
}
static void hle_glGetShaderSource(cpu_t *c)
{
    u32 out = harg(c, 3);
    if (out) st8(out, 0);
}
static void hle_glGetActiveAttrib(cpu_t *c)
{
    u32 name = harg(c, 6);
    if (name) st8(name, 0);
}
static void hle_glGetActiveUniform(cpu_t *c)
{
    u32 name = harg(c, 6);
    if (name) st8(name, 0);
}
static void hle_glGetAttachedShaders(cpu_t *c)
{
    gptr count = harg(c, 2);
    if (count) st32(count, 0);
}

static void hle_glCheckFramebufferStatus(cpu_t *c) { hret(c, 0x8CD5); } /* COMPLETE */

static void hle_glBufferData(cpu_t *c)
{
    /* note the size for shadow allocation; track the bound id loosely */
    static u32 bound[2];
    u32 target = harg(c, 0);
    u32 size = harg(c, 1);
    bound[target == 0x8893 ? 1 : 0] = g_next_id ? bound[target == 0x8893 ? 1 : 0] : 0;
    (void)bound;
    (void)size;
}
static void hle_glBindBuffer(cpu_t *c)
{
    u32 target = harg(c, 0), id = harg(c, 1);
    static u32 bound_arr, bound_vbo;
    if (target == 0x8892) bound_arr = id; else bound_vbo = id;
    g_buffers[0].id = 0;   /* shadow for id fetched lazily by map */
    (void)bound_arr; (void)bound_vbo;
    /* remember the most recent bind for map sizing */
    extern u32 g_last_bound_vbo;
    g_last_bound_vbo = id;
}
u32 g_last_bound_vbo;

static void hle_glMapBufferOES(cpu_t *c)
{
    u32 id = g_last_bound_vbo;
    u32 size = 1 << 20;
    for (int i = 0; i < g_nbuffers; i++)
        if (g_buffers[i].id == id && g_buffers[i].size)
            size = g_buffers[i].size;
    hret(c, buffer_shadow(id ? id : 1, size));
}
static void hle_glUnmapBufferOES(cpu_t *c) { hret(c, 1); }
static void hle_glGetBufferParameteriv(cpu_t *c)
{
    u32 out = harg(c, 2);
    if (out) st32(out, 1 << 20);
}
static void hle_glGetBufferPointervOES(cpu_t *c)
{
    gptr out = harg(c, 2);
    if (out) st32(out, buffer_shadow(g_last_bound_vbo ? g_last_bound_vbo : 1, 1 << 20));
}

static void hle_glReadPixels(cpu_t *c)
{
    u32 w = harg(c, 2), h = harg(c, 3);
    u32 data = harg(c, 6);
    if (data) memset(g2h(data), 0, w * h * 4);
}
static void hle_glGetUniformfv(cpu_t *c)
{
    u32 out = harg(c, 2);
    if (out) stf32(out, 0.0f);
}
static void hle_glGetUniformiv(cpu_t *c)
{
    u32 out = harg(c, 2);
    if (out) st32(out, 0);
}
static void hle_glGetVertexAttribfv(cpu_t *c)
{
    u32 out = harg(c, 2);
    if (out) stf32(out, 0.0f);
}
static void hle_glGetVertexAttribiv(cpu_t *c)
{
    u32 out = harg(c, 2);
    if (out) st32(out, 0);
}
static void hle_glGetTexParameterfv(cpu_t *c)
{
    u32 out = harg(c, 2);
    if (out) stf32(out, 0.0f);
}
static void hle_glGetTexParameteriv(cpu_t *c)
{
    u32 out = harg(c, 2);
    if (out) st32(out, 0x0DE1);   /* TEXTURE_2D-ish */
}
static void hle_glGetFramebufferAttachmentParameteriv(cpu_t *c)
{
    u32 out = harg(c, 4);
    if (out) st32(out, 0);
}
static void hle_glGetRenderbufferParameteriv(cpu_t *c)
{
    u32 out = harg(c, 2);
    if (out) st32(out, 0);
}
static void hle_glGetFenceivNV(cpu_t *c)
{
    u32 out = harg(c, 2);
    if (out) st32(out, 1);
}
static void hle_glTestFenceNV(cpu_t *c) { hret(c, 1); }
static void hle_glFinishFenceNV(cpu_t *c) { }
static void hle_glSetFenceNV(cpu_t *c) { }
static void hle_glDeleteFencesNV(cpu_t *c) { }
static void hle_glGetShaderPrecisionFormat(cpu_t *c)
{
    u32 range = harg(c, 2), prec = harg(c, 3);
    if (range) { st32(range, 127); st32(range + 4, 127); }
    if (prec) st32(prec, 24);
}
static void hle_glGetProgramBinaryOES(cpu_t *c)
{
    gptr len = harg(c, 2);
    if (len) st32(len, 0);
}
static void hle_glGetDriverControlsQCOM(cpu_t *c)
{
    gptr count = harg(c, 1);
    if (count) st32(count, 0);
}
static void hle_glGetPerfMonitorGroupsAMD(cpu_t *c)
{
    gptr count = harg(c, 0);
    if (count) st32(count, 0);
}
static void hle_glGetPerfMonitorCountersAMD(cpu_t *c)
{
    gptr count = harg(c, 1);
    if (count) st32(count, 0);
}
static void hle_glGetPerfMonitorGroupStringAMD(cpu_t *c)
{
    u32 out = harg(c, 3);
    if (out) st8(out, 0);
}
static void hle_glGetPerfMonitorCounterStringAMD(cpu_t *c)
{
    u32 out = harg(c, 4);
    if (out) st8(out, 0);
}
static void hle_glGetPerfMonitorCounterInfoAMD(cpu_t *c) { }
static void hle_glGetPerfMonitorCounterDataAMD(cpu_t *c)
{
    u32 out = harg(c, 2);
    if (out) st32(out, 0);
}
static void hle_glSelectPerfMonitorCountersAMD(cpu_t *c) { }
static void hle_glBeginPerfMonitorAMD(cpu_t *c) { }
static void hle_glEndPerfMonitorAMD(cpu_t *c) { }
static void hle_glGenPerfMonitorsAMD2(cpu_t *c) { }
static void hle_glDeletePerfMonitorsAMD(cpu_t *c) { }
static void hle_glReleaseShaderCompiler(cpu_t *c) { }

void gles_init(void)
{
    hle_register("glGetString", hle_glGetString);
    hle_register("glGetError", hle_glGetError);
    hle_register("glGetIntegerv", hle_glGetIntegerv);
    hle_register("glGetFloatv", hle_glGetFloatv);
    hle_register("glGetBooleanv", hle_glGetBooleanv);
    hle_register("glCreateShader", hle_glCreateShader);
    hle_register("glCreateProgram", hle_glCreateProgram);
    hle_register("glGenTextures", hle_glGenTextures);
    hle_register("glGenBuffers", hle_glGenBuffers);
    hle_register("glGenFramebuffers", hle_glGenFramebuffers);
    hle_register("glGenRenderbuffers", hle_glGenRenderbuffers);
    hle_register("glGenFencesNV", hle_glGenFencesNV);
    hle_register("glGenPerfMonitorsAMD", hle_glGenPerfMonitorsAMD);
    hle_register("glIsTexture", hle_glIsTexture);
    hle_register("glIsBuffer", hle_glIsBuffer);
    hle_register("glIsFramebuffer", hle_glIsFramebuffer);
    hle_register("glIsRenderbuffer", hle_glIsRenderbuffer);
    hle_register("glIsShader", hle_glIsShader);
    hle_register("glIsProgram", hle_glIsProgram);
    hle_register("glIsEnabled", hle_glIsEnabled);
    hle_register("glIsFenceNV", hle_glIsFenceNV);
    hle_register("glGetAttribLocation", hle_glGetAttribLocation);
    hle_register("glGetUniformLocation", hle_glGetUniformLocation);
    hle_register("glGetShaderiv", hle_glGetShaderiv);
    hle_register("glGetProgramiv", hle_glGetProgramiv);
    hle_register("glGetShaderInfoLog", hle_glGetShaderInfoLog);
    hle_register("glGetProgramInfoLog", hle_glGetProgramInfoLog);
    hle_register("glGetShaderSource", hle_glGetShaderSource);
    hle_register("glGetActiveAttrib", hle_glGetActiveAttrib);
    hle_register("glGetActiveUniform", hle_glGetActiveUniform);
    hle_register("glGetAttachedShaders", hle_glGetAttachedShaders);
    hle_register("glCheckFramebufferStatus", hle_glCheckFramebufferStatus);
    hle_register("glBufferData", hle_glBufferData);
    hle_register("glBindBuffer", hle_glBindBuffer);
    hle_register("glMapBufferOES", hle_glMapBufferOES);
    hle_register("glUnmapBufferOES", hle_glUnmapBufferOES);
    hle_register("glGetBufferParameteriv", hle_glGetBufferParameteriv);
    hle_register("glGetBufferPointervOES", hle_glGetBufferPointervOES);
    hle_register("glReadPixels", hle_glReadPixels);
    hle_register("glGetUniformfv", hle_glGetUniformfv);
    hle_register("glGetUniformiv", hle_glGetUniformiv);
    hle_register("glGetVertexAttribfv", hle_glGetVertexAttribfv);
    hle_register("glGetVertexAttribiv", hle_glGetVertexAttribiv);
    hle_register("glGetTexParameterfv", hle_glGetTexParameterfv);
    hle_register("glGetTexParameteriv", hle_glGetTexParameteriv);
    hle_register("glGetFramebufferAttachmentParameteriv", hle_glGetFramebufferAttachmentParameteriv);
    hle_register("glGetRenderbufferParameteriv", hle_glGetRenderbufferParameteriv);
    hle_register("glGetFenceivNV", hle_glGetFenceivNV);
    hle_register("glTestFenceNV", hle_glTestFenceNV);
    hle_register("glFinishFenceNV", hle_glFinishFenceNV);
    hle_register("glSetFenceNV", hle_glSetFenceNV);
    hle_register("glDeleteFencesNV", hle_glDeleteFencesNV);
    hle_register("glGetShaderPrecisionFormat", hle_glGetShaderPrecisionFormat);
    hle_register("glGetProgramBinaryOES", hle_glGetProgramBinaryOES);
    hle_register("glGetDriverControlsQCOM", hle_glGetDriverControlsQCOM);
    hle_register("glGetPerfMonitorGroupsAMD", hle_glGetPerfMonitorGroupsAMD);
    hle_register("glGetPerfMonitorCountersAMD", hle_glGetPerfMonitorCountersAMD);
    hle_register("glGetPerfMonitorGroupStringAMD", hle_glGetPerfMonitorGroupStringAMD);
    hle_register("glGetPerfMonitorCounterStringAMD", hle_glGetPerfMonitorCounterStringAMD);
    hle_register("glGetPerfMonitorCounterInfoAMD", hle_glGetPerfMonitorCounterInfoAMD);
    hle_register("glGetPerfMonitorCounterDataAMD", hle_glGetPerfMonitorCounterDataAMD);
    hle_register("glSelectPerfMonitorCountersAMD", hle_glSelectPerfMonitorCountersAMD);
    hle_register("glBeginPerfMonitorAMD", hle_glBeginPerfMonitorAMD);
    hle_register("glEndPerfMonitorAMD", hle_glEndPerfMonitorAMD);
    hle_register("glDeletePerfMonitorsAMD", hle_glDeletePerfMonitorsAMD);
    hle_register("glReleaseShaderCompiler", hle_glReleaseShaderCompiler);
}
