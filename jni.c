/*
 * jni.c - fake JavaVM/JNIEnv.
 *
 * Tables live in guest memory and hold HLE slot addresses. References are
 * opaque handles (JREF_BASE + 4*idx) into a host object table; method and
 * field IDs are handles into host tables bound at Get*ID time to a host
 * implementation, so a Call*Method is one table lookup + one indirect call.
 * Java behaviour the game depends on is implemented in jni_java.c-style
 * entries in `java_impls` below; everything else returns a typed default and
 * logs once.
 */
#include "emu.h"
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#define JREF_BASE  0xE0000000u
#define JMID_BASE  0xE8000000u
#define JFID_BASE  0xEC000000u
#define MAX_JOBJ   65536
#define MAX_JMETH  4096
#define MAX_JFIELD 2048
#define MAX_NATIVES 1024

enum { K_FREE, K_CLASS, K_OBJ, K_STR, K_ARR, K_OBJARR };

typedef struct {
    u8    kind;
    u8    elem;       /* K_ARR element size */
    u16   refs;
    u32   cls;        /* class handle (K_OBJ / arrays) */
    char *str;        /* K_CLASS name, K_STR utf8 */
    gptr  data;       /* K_ARR guest storage, direct buffers */
    u32   len;
    u32  *objs;       /* K_OBJARR elements (handles) */
    FILE *fp;         /* InputStream */
    gptr  utf;        /* cached GetStringUTFChars copy */
    int   fd;         /* AssetFileDescriptor: host fd + 1 (0 = none) */
    struct jfv { u32 fid; u64 v; } *fv;   /* instance (or static, for classes) field values */
    void *native; void (*native_free)(void *);   /* host state of Java classes we implement natively */
    u16   nfv, capfv;
} jobj_t;

struct jargs;
typedef u64 (*jimpl_t)(cpu_t *c, u32 self, struct jargs *a);

struct jimpl_ent;
typedef struct { u32 cls; const char *name; const char *sig; jimpl_t fn; const struct jimpl_ent *ent; } jmeth_t;
typedef struct { u32 cls; const char *name; const char *sig; } jfield_t;

static jobj_t   objs[MAX_JOBJ];
static u32      obj_free_hint = 1;
static jmeth_t  meths[MAX_JMETH];
static int      nmeths;
static jfield_t fields[MAX_JFIELD];
static int      nfields;
static pthread_mutex_t jlock = PTHREAD_MUTEX_INITIALIZER;
static __thread u32 pending_exc;

static gptr g_env, g_vm;
static u32  h_activity, h_assets;

/* ------------------------------------------------------------------ */
/* object table                                                       */
/* ------------------------------------------------------------------ */

static inline jobj_t *jo(u32 h)
{
    u32 i = (h - JREF_BASE) >> 2;
    return (h >= JREF_BASE && i < MAX_JOBJ && objs[i].kind) ? &objs[i] : NULL;
}

/* local references: objects created while a host->guest native call is running are released
 * when it returns, like the JVM does; DeleteLocalRef takes them out early */
typedef struct { u32 *h; int n, cap; int base[64]; int depth; } lframe_t;
static __thread lframe_t lf;
static void lf_push_handle(u32 h)
{
    if (!lf.depth) return;
    if (lf.n == lf.cap) { lf.cap = lf.cap ? lf.cap * 2 : 256; lf.h = realloc(lf.h, (size_t)lf.cap * 4); }
    lf.h[lf.n++] = h;
}
static bool lf_take(u32 h)                   /* remove one local entry for h in the current frame */
{
    int base = lf.depth ? lf.base[lf.depth - 1] : 0;
    for (int i = lf.n - 1; i >= base; i--) if (lf.h[i] == h) { lf.h[i] = 0; return true; }
    return false;
}
static void jo_unref(u32 h);
void jni_push_frame(void) { if (lf.depth < 64) lf.base[lf.depth++] = lf.n; }
void jni_pop_frame(void)
{
    if (!lf.depth) return;
    int base = lf.base[--lf.depth];
    for (int i = lf.n - 1; i >= base; i--) if (lf.h[i]) jo_unref(lf.h[i]);
    lf.n = base;
}
/* keep an object we created ourselves (e.g. stored in a field) out of the caller's frame */
static u32 jo_keep(u32 h) { lf_take(h); return h; }

static u32 jo_new(int kind)
{
    pthread_mutex_lock(&jlock);
    for (u32 n = 0; n < MAX_JOBJ; n++) {
        u32 i = obj_free_hint;
        obj_free_hint = obj_free_hint + 1 < MAX_JOBJ ? obj_free_hint + 1 : 1;
        if (!objs[i].kind) {
            memset(&objs[i], 0, sizeof(objs[i]));
            objs[i].kind = (u8)kind;
            objs[i].refs = 1;
            pthread_mutex_unlock(&jlock);
            if (kind != K_CLASS) lf_push_handle(JREF_BASE + 4 * i);
            return JREF_BASE + 4 * i;
        }
    }
    pthread_mutex_unlock(&jlock);
    fatal("JNI object table full");
}

static void jo_unref(u32 h)
{
    jobj_t *o = jo(h);
    if (!o || o->kind == K_CLASS) return;
    pthread_mutex_lock(&jlock);
    if (o->refs && --o->refs == 0) {
        free(o->str);
        free(o->objs);
        if (o->fp) fclose(o->fp);
        if (o->utf) guest_free(o->utf);
        if (o->fd) close(o->fd - 1);
        if (o->kind == K_ARR) guest_free(o->data);
        if (o->native) { o->native_free(o->native); o->native = NULL; }
        struct jfv *fv = o->fv; u16 nfv = o->nfv;
        o->fv = NULL; o->nfv = o->capfv = 0;
        o->kind = K_FREE;
        pthread_mutex_unlock(&jlock);
        for (u16 k = 0; k < nfv; k++)                   /* object fields held references */
            if (fv[k].v >= JREF_BASE && fv[k].v < JREF_BASE + 4 * MAX_JOBJ && (fields[(fv[k].fid - JFID_BASE) >> 2].sig[0] == 'L' ||
                                                                                  fields[(fv[k].fid - JFID_BASE) >> 2].sig[0] == '['))
                jo_unref((u32)fv[k].v);
        free(fv);
        return;
    }
    pthread_mutex_unlock(&jlock);
}

#define MAX_JCLASS 1024
static u32 classes[MAX_JCLASS];
static int nclasses;

static u32 jclass(const char *name)
{
    pthread_mutex_lock(&jlock);
    for (int i = 0; i < nclasses; i++)
        if (!strcmp(objs[(classes[i] - JREF_BASE) >> 2].str, name)) {
            pthread_mutex_unlock(&jlock);
            return classes[i];
        }
    pthread_mutex_unlock(&jlock);
    u32 h = jo_new(K_CLASS);
    jo(h)->str = strdup(name);
    pthread_mutex_lock(&jlock);
    if (nclasses >= MAX_JCLASS) fatal("JNI class table full");
    classes[nclasses++] = h;
    pthread_mutex_unlock(&jlock);
    return h;
}

static const char *jclass_name(u32 h)
{
    jobj_t *o = jo(h);
    if (!o) return "?";
    if (o->kind == K_CLASS) return o->str;
    jobj_t *k = jo(o->cls);
    return k ? k->str : "?";
}

static u32 jnew_obj(const char *cls)
{
    u32 h = jo_new(K_OBJ);
    jo(h)->cls = jclass(cls);
    return h;
}

static u32 jstr_new(const char *s)
{
    u32 h = jo_new(K_STR);
    jobj_t *o = jo(h);
    o->str = strdup(s);
    o->len = (u32)strlen(s);
    o->cls = jclass("java/lang/String");
    return h;
}

static const char *jstr(u32 h)
{
    jobj_t *o = jo(h);
    return o && o->kind == K_STR ? o->str : "";
}

static u32 jarr_new(u32 n, u32 elem)
{
    u32 h = jo_new(K_ARR);
    jobj_t *o = jo(h);
    o->elem = (u8)elem;
    o->len = n;
    o->data = guest_malloc(n * elem + 1);
    memset(g2h(o->data), 0, n * elem);
    return h;
}

static u32 jobjarr_new(u32 n, u32 cls)
{
    u32 h = jo_new(K_OBJARR);
    jobj_t *o = jo(h);
    o->len = n;
    o->cls = cls;
    o->objs = calloc(n ? n : 1, 4);
    return h;
}

/* ------------------------------------------------------------------ */
/* argument walking for Call*Method / NewObject                       */
/* ------------------------------------------------------------------ */

enum { M_DOTS, M_V, M_A };

typedef struct jargs { int mode; argwalk_t w; gptr p; } jargs_t;

static u32 ja_i32(jargs_t *a)
{
    u32 v;
    switch (a->mode) {
    case M_DOTS: return argwalk_u32(&a->w);
    case M_V:    v = ld32(a->p); a->p += 4; return v;
    default:     v = ld32(a->p); a->p += 8; return v;
    }
}
static u64 ja_i64(jargs_t *a)
{
    u64 v;
    switch (a->mode) {
    case M_DOTS: return argwalk_u64(&a->w);
    case M_V:    a->p = (a->p + 7) & ~7u; break;
    default:     break;
    }
    v = ld32(a->p) | ((u64)ld32(a->p + 4) << 32);
    a->p += 8;
    return v;
}
static f32 ja_f32(jargs_t *a)
{
    if (a->mode == M_A) { f32 f = ldf32(a->p); a->p += 8; return f; }
    u64 b = ja_i64(a);       /* varargs promote float to double */
    f64 d; memcpy(&d, &b, 8); return (f32)d;
}

/* ------------------------------------------------------------------ */
/* Java-side implementations                                          */
/* ------------------------------------------------------------------ */

static char g_apk_assets[512] = "./apk_assets";
static char g_files_dir[512] = "/data/data/com.ea.game.realracing2_OTD_row/files";

static FILE *asset_open(const char *path)
{
    char host[1024];
    snprintf(host, sizeof(host), "%s/%s", g_apk_assets, path);
    return fopen(host, "rb");
}

static void jthrow(const char *cls)
{
    pending_exc = jnew_obj(cls);
}

static u64 j_get_activity(cpu_t *c, u32 self, jargs_t *a) { return h_activity; }
static u64 j_get_assets(cpu_t *c, u32 self, jargs_t *a)   { return h_assets; }

static u64 j_asset_open(cpu_t *c, u32 self, jargs_t *a)
{
    const char *path = jstr(ja_i32(a));
    FILE *f = asset_open(path);
    VLOG(1, "[jni] AssetManager.open(%s) -> %s\n", path, f ? "ok" : "missing");
    if (!f) { jthrow("java/io/FileNotFoundException"); return 0; }
    u32 h = jnew_obj("java/io/InputStream");
    jo(h)->fp = f;
    return h;
}

static u64 j_asset_list(cpu_t *c, u32 self, jargs_t *a)
{
    char host[1024];
    snprintf(host, sizeof(host), "%s/%s", g_apk_assets, jstr(ja_i32(a)));
    DIR *d = opendir(host);
    u32 n = 0, cap = 64;
    char **names = malloc(cap * sizeof(char *));
    for (struct dirent *e; d && (e = readdir(d)); ) {
        if (e->d_name[0] == '.') continue;
        if (n == cap) names = realloc(names, (cap *= 2) * sizeof(char *));
        names[n++] = strdup(e->d_name);
    }
    if (d) closedir(d);
    u32 h = jobjarr_new(n, jclass("java/lang/String"));
    for (u32 i = 0; i < n; i++) { jo(h)->objs[i] = jstr_new(names[i]); free(names[i]); }
    free(names);
    return h;
}

static u32 field_new(u32 cls, const char *name, const char *sig);
static u64 *fslot(u32 obj, u32 fid, bool create);
#include <fcntl.h>
#include <unistd.h>

/* AssetManager.openFd: the game only asks for the length; a host fd is opened only if
 * getFileDescriptor() is called (exposed as FileDescriptor.descriptor) */
static u64 j_asset_openfd(cpu_t *c, u32 self, jargs_t *a)
{
    const char *path = jstr(ja_i32(a));
    char host[1024];
    snprintf(host, sizeof(host), "%s/%s", g_apk_assets, path);
    struct stat st;
    if (stat(host, &st) != 0) { jthrow("java/io/FileNotFoundException"); return 0; }
    u32 afd = jnew_obj("android/content/res/AssetFileDescriptor");
    jo(afd)->len = (u32)st.st_size;
    jo(afd)->str = strdup(host);
    return afd;
}
static u64 j_afd_length(cpu_t *c, u32 self, jargs_t *a) { jobj_t *o = jo(self); return o ? o->len : (u64)-1; }
static u64 j_afd_offset(cpu_t *c, u32 self, jargs_t *a) { return 0; }
static u64 j_afd_fd(cpu_t *c, u32 self, jargs_t *a)
{
    jobj_t *o = jo(self);
    if (!o || !o->str) return 0;
    if (!o->data) {
        int fd = open(o->str, O_RDONLY | O_BINARY);
        u32 fdo = jnew_obj("java/io/FileDescriptor");
        u32 fid = field_new(jclass("java/io/FileDescriptor"), "descriptor", "I");
        pthread_mutex_lock(&jlock);
        *fslot(fdo, fid, true) = (u32)fd;
        pthread_mutex_unlock(&jlock);
        o->data = fdo;
        o->fd = fd + 1;
    }
    return o->data;
}
static u64 j_afd_close(cpu_t *c, u32 self, jargs_t *a)
{
    jobj_t *o = jo(self);
    if (o && o->fd) { close(o->fd - 1); o->fd = 0; }
    return 0;
}

/* VideoPlayer: no FMV decoding; play() reports completion on the next frame */
static volatile u32 g_video_done_obj;
static u64 j_video_play(cpu_t *c, u32 self, jargs_t *a)
{
    VLOG(1, "[jni] video play -> completing immediately\n");
    g_video_done_obj = self;
    return 0;
}
u32 jni_take_video_completion(void) { return __atomic_exchange_n(&g_video_done_obj, 0, __ATOMIC_ACQ_REL); }

static u64 j_stream_read(cpu_t *c, u32 self, jargs_t *a)
{
    u32 arr = ja_i32(a), off = ja_i32(a), len = ja_i32(a);
    jobj_t *s = jo(self), *b = jo(arr);
    if (!s || !s->fp || !b || b->kind != K_ARR || off + len > b->len) return (u64)-1;
    emu_prefault(g2h(b->data + off), len);
    size_t n = fread(g2h(b->data + off), 1, len, s->fp);
    return n ? (u32)n : (u32)-1;
}
static u64 j_stream_read_all(cpu_t *c, u32 self, jargs_t *a)
{
    u32 arr = ja_i32(a);
    jobj_t *s = jo(self), *b = jo(arr);
    if (!s || !s->fp || !b || b->kind != K_ARR) return (u32)-1;
    emu_prefault(g2h(b->data), b->len);
    size_t n = fread(g2h(b->data), 1, b->len, s->fp);
    return n ? (u32)n : (u32)-1;
}
static u64 j_stream_available(cpu_t *c, u32 self, jargs_t *a)
{
    jobj_t *s = jo(self);
    if (!s || !s->fp) return 0;
    long cur = ftell(s->fp);
    struct stat st;
    if (fstat(fileno(s->fp), &st)) return 0;
    return (u32)(st.st_size - cur);
}
static u64 j_stream_skip(cpu_t *c, u32 self, jargs_t *a)
{
    s64 n = (s64)ja_i64(a);
    jobj_t *s = jo(self);
    if (!s || !s->fp) return 0;
    long cur = ftell(s->fp);
    fseek(s->fp, (long)n, SEEK_CUR);
    return (u64)(ftell(s->fp) - cur);
}
static u64 j_stream_close(cpu_t *c, u32 self, jargs_t *a)
{
    jobj_t *s = jo(self);
    if (s && s->fp) { fclose(s->fp); s->fp = NULL; }
    return 0;
}

static char g_ext_files_dir[512] = "/data/data/com.ea.game.realracing2_OTD_row/files";
static u64 j_package_name(cpu_t *c, u32 self, jargs_t *a)
{
    return jstr_new(G.game == 3 ? "com.ea.games.r3_row" : "com.ea.game.realracing2_OTD_row");
}
static u64 j_ext_files_dir(cpu_t *c, u32 self, jargs_t *a)
{
    u32 h = jnew_obj("java/io/File");
    jo(h)->str = strdup(g_ext_files_dir);
    return h;
}
void jni_rr3_setup(void)
{
    snprintf(g_files_dir, sizeof(g_files_dir), "/data/data/com.ea.games.r3_row/files");
    snprintf(g_ext_files_dir, sizeof(g_ext_files_dir), "/sdcard/Android/data/com.ea.games.r3_row/files");
}
static u64 j_files_dir(cpu_t *c, u32 self, jargs_t *a)
{
    u32 h = jnew_obj("java/io/File");
    jo(h)->str = strdup(g_files_dir);
    return h;
}
static u64 j_file_path(cpu_t *c, u32 self, jargs_t *a)
{
    jobj_t *o = jo(self);
    return jstr_new(o && o->str ? o->str : "");
}
static u64 j_true(cpu_t *c, u32 self, jargs_t *a) { return 1; }

/* the game's Quit: it has already saved, and on Android the process ends here */
static u64 j_exit_app(cpu_t *c, u32 self, jargs_t *a)
{
    LOG("[jni] exitApp: quitting\n");
    host_shutdown();
    fflush(NULL);
    _exit(0);
}

/* EAAudioCore: Java's Startup() builds an AudioTrack and calls native Init(track, rate, ch, buf) */
#define AUDIO_RATE 44100
#define AUDIO_BUF  16384
static u64 j_audio_startup(cpu_t *c, u32 self, jargs_t *a)
{
    static bool done;
    if (done) return 0;
    done = true;
    u32 init = elf_lookup("Java_com_ea_EAAudioCore_AndroidEAAudioCore_Init");
    if (!init) return 0;
    const char *r = getenv("RR2_AUDIO_RATE");       /* the game mixes in software at this rate */
    u32 rate = r ? (u32)atoi(r) : AUDIO_RATE;
    if (rate < 8000 || rate > 48000) rate = AUDIO_RATE;
    host_audio_open((int)rate, 1);                  /* Java builds a CHANNEL_CONFIGURATION_MONO track */
    u32 track = jnew_obj("android/media/AudioTrack");
    jo(track)->refs = 0xFFFF;                     /* lives for the whole run */
    u32 args[6] = { g_env, jclass("com/ea/EAAudioCore/AndroidEAAudioCore"), track, rate, 2, AUDIO_BUF };
    emu_call(c, init, 6, args);
    return 0;
}
static u64 j_track_write(cpu_t *c, u32 self, jargs_t *a)
{
    u32 arr = ja_i32(a), off = ja_i32(a), len = ja_i32(a);
    jobj_t *b = jo(arr);
    if (!b || b->kind != K_ARR || off + len > b->len) return (u32)-3;   /* ERROR_BAD_VALUE */
    host_audio_write(g2h(b->data + off * b->elem), len * b->elem);
    return len;
}
static u64 j_ext_storage(cpu_t *c, u32 self, jargs_t *a)
{
    u32 h = jnew_obj("java/io/File");
    jo(h)->str = strdup("/sdcard");
    return h;
}
static u64 j_storage_state(cpu_t *c, u32 self, jargs_t *a) { return jstr_new("mounted"); }

typedef struct jimpl_ent { const char *cls, *name, *sig; jimpl_t fn; const char *sv; u32 iv; } jimpl_ent;

static __thread const jmeth_t *cur_meth;
static u64 j_str(cpu_t *c, u32 self, jargs_t *a)  { return jstr_new(cur_meth->ent->sv); }
static u64 j_int(cpu_t *c, u32 self, jargs_t *a)  { return cur_meth->ent->iv; }
static u64 j_dpi(cpu_t *c, u32 self, jargs_t *a)  { f32 f = G.dpi; u32 b; memcpy(&b, &f, 4); return b; }
static u64 j_width(cpu_t *c, u32 self, jargs_t *a)  { return (u32)G.width; }
static u64 j_height(cpu_t *c, u32 self, jargs_t *a) { return (u32)G.height; }

#include "jni_rr3.inc"

#define SYSD "com/ea/blast/SystemAndroidDelegate"
#define PKG  "com.ea.game.realracing2_OTD_row"

/* cls NULL = any class; sig NULL = any signature */
static const jimpl_ent java_impls[] = {
    { NULL, "getDataFolder",   NULL, j_str, "/sdcard/Android/data/" PKG "/files/" },
    { NULL, "exitApp", "()V", j_exit_app },
    { "com/ea/EAAudioCore/AndroidEAAudioCore", "Startup", "()V", j_audio_startup },
    { "android/media/AudioTrack", "write", NULL, j_track_write },
    { "android/content/res/AssetManager", "openFd", NULL, j_asset_openfd },
    { "android/content/res/AssetFileDescriptor", "getLength", NULL, j_afd_length },
    { "android/content/res/AssetFileDescriptor", "getDeclaredLength", NULL, j_afd_length },
    { "android/content/res/AssetFileDescriptor", "getStartOffset", NULL, j_afd_offset },
    { "android/content/res/AssetFileDescriptor", "getFileDescriptor", NULL, j_afd_fd },
    { "android/content/res/AssetFileDescriptor", "close", NULL, j_afd_close },
    { "com/ea/VideoPlayer/PlayerAndroid", "play", NULL, j_video_play },
    { "android/media/AudioTrack", "play", NULL, j_true },
    { "android/media/AudioTrack", "stop", NULL, j_true },
    { "android/media/AudioTrack", "pause", NULL, j_true },
    { "android/media/AudioTrack", "flush", NULL, j_true },
    { NULL, "GetInternalStorageDirectory", NULL, j_str, "/data/data/" PKG "/files" },
    { NULL, "GetDedicatedDirectory", NULL, j_str, "Android/data/" PKG "/files/" },
    { SYSD, "GetAccelerometerCount", NULL, j_int, NULL, 1 },
    { SYSD, "GetTouchScreenCount", NULL, j_int, NULL, 1 },
    { SYSD, "GetApplicationVersionCode", NULL, j_int, NULL, 851 },
    { SYSD, "GetApplicationVersion", NULL, j_str, "0.0.851" },
    { SYSD, "GetChipset",      NULL, j_str, "" },
    { SYSD, "GetFirmware",     NULL, j_str, "GRJ22" },
    { SYSD, "GetManufacturer", NULL, j_str, "HTC" },
    { SYSD, "GetDeviceModel",  NULL, j_str, "Nexus One" },
    { SYSD, "GetDeviceName",   NULL, j_str, "passion" },
    { SYSD, "GetPhoneNumber",  NULL, j_str, "" },
    { SYSD, "GetDeviceSubscriberID", NULL, j_str, "" },
    { SYSD, "GetDeviceUniqueId", NULL, j_str, "-1" },
    { SYSD, "GetHardwareFloatingPointSupport", NULL, j_str, "true" },
    { SYSD, "GetApiLevel",     NULL, j_str, "10" },
    { SYSD, "GetPlatformVersion", NULL, j_str, "2.3.4" },
    { SYSD, "GetProcessorArchitecture", NULL, j_str, "armeabi" },
    { SYSD, "GetLanguage",     NULL, j_str, "en" },
    { SYSD, "GetLocale",       NULL, j_str, "en_US" },
    { NULL, "GetDefaultWidth", NULL, j_width },
    { NULL, "GetDefaultHeight", NULL, j_height },
    { NULL, "GetDpiX",         NULL, j_dpi },
    { NULL, "GetDpiY",         NULL, j_dpi },
    { NULL, "getAssets",       NULL, j_get_assets },
    { NULL, "getInstance",     "()Lcom/ea/blast/MainActivity;", j_get_activity },
    { NULL, NULL,              "()Lcom/ea/blast/MainActivity;", j_get_activity },
    { NULL, NULL,              "()Landroid/content/res/AssetManager;", j_get_assets },
    { "android/content/res/AssetManager", "open", "(Ljava/lang/String;)Ljava/io/InputStream;", j_asset_open },
    { "android/content/res/AssetManager", "list", NULL, j_asset_list },
    { NULL, "read",            "([BII)I", j_stream_read },
    { NULL, "read",            "([B)I", j_stream_read_all },
    { NULL, "available",       "()I", j_stream_available },
    { NULL, "skip",            "(J)J", j_stream_skip },
    { "java/io/InputStream", "close", "()V", j_stream_close },
    { NULL, "getPackageName",  NULL, j_package_name },
    { NULL, "getFilesDir",     NULL, j_files_dir },
    { NULL, "getExternalFilesDir", NULL, j_ext_files_dir },
    { NULL, "getAbsolutePath", NULL, j_file_path },
    { NULL, "getPath",         NULL, j_file_path },
    { NULL, "mkdirs",          NULL, j_true },
    { NULL, "getExternalStorageDirectory", NULL, j_ext_storage },
    { NULL, "GetPrimaryExternalStorageDirectory", NULL, j_str, "/sdcard" },
    { NULL, "getExternalStorageState", NULL, j_storage_state },
    { NULL, "GetPrimaryExternalStorageState", NULL, j_storage_state },
    { NULL, "exists",          "()Z", j_true },
};

static char sig_ret(const char *sig)
{
    const char *p = strchr(sig, ')');
    return p ? p[1] : 'V';
}

static u64 j_default(cpu_t *c, u32 self, jargs_t *a);

static const jimpl_ent *bind_impl(const char *cls, const char *name, const char *sig)
{
    if (G.game == 3)
        for (unsigned i = 0; i < sizeof(rr3_impls) / sizeof(rr3_impls[0]); i++) {
            const jimpl_ent *e = &rr3_impls[i];
            if (e->cls && strcmp(e->cls, cls)) continue;
            if (e->name && strcmp(e->name, name)) continue;
            if (e->sig && strcmp(e->sig, sig)) continue;
            return e;
        }
    for (unsigned i = 0; i < sizeof(java_impls) / sizeof(java_impls[0]); i++) {
        const jimpl_ent *e = &java_impls[i];
        if (e->cls && strcmp(e->cls, cls)) continue;
        if (e->name && strcmp(e->name, name)) continue;
        if (e->sig && strcmp(e->sig, sig)) continue;
        return e;
    }
    return NULL;
}


static u64 j_default(cpu_t *c, u32 self, jargs_t *a)
{
    const jmeth_t *m = cur_meth;
    char r = sig_ret(m->sig);
    if (r == 'L') {
        const char *cn = strchr(m->sig, ')') + 2;
        if (!strncmp(cn, "java/lang/String;", 17)) return jstr_new("");
        char buf[256];
        size_t n = strcspn(cn, ";");
        snprintf(buf, sizeof(buf), "%.*s", (int)n, cn);
        return jnew_obj(buf);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* JNIEnv: classes, methods, fields                                   */
/* ------------------------------------------------------------------ */

static u32 meth_new(u32 cls, const char *name, const char *sig)
{
    const char *cn = jclass_name(cls);
    pthread_mutex_lock(&jlock);
    for (int i = 0; i < nmeths; i++)
        if (meths[i].cls == cls && !strcmp(meths[i].name, name) && !strcmp(meths[i].sig, sig)) {
            pthread_mutex_unlock(&jlock);
            return JMID_BASE + 4 * i;
        }
    if (nmeths >= MAX_JMETH) fatal("JNI method table full");
    int i = nmeths++;
    meths[i].cls = cls;
    meths[i].name = strdup(name);
    meths[i].sig = strdup(sig);
    meths[i].ent = bind_impl(cn, name, sig);
    meths[i].fn = meths[i].ent ? meths[i].ent->fn : j_default;
    pthread_mutex_unlock(&jlock);
    if (meths[i].fn == j_default)
        LOG("[jni] unimplemented method %s.%s%s (default return)\n", cn, name, sig);
    else
        VLOG(1, "[jni] method %s.%s%s -> %08x\n", cn, name, sig, JMID_BASE + 4 * i);
    return JMID_BASE + 4 * i;
}

static void e_GetVersion(cpu_t *c)  { hret(c, 0x10006); }
static void e_FindClass(cpu_t *c)   { hret(c, jclass(gstr(harg(c, 1)))); }
static void e_GetSuperclass(cpu_t *c) { hret(c, jclass("java/lang/Object")); }
static void e_IsAssignableFrom(cpu_t *c) { hret(c, 1); }
static void e_Throw(cpu_t *c)       { pending_exc = harg(c, 1); hret(c, 0); }
static void e_ThrowNew(cpu_t *c)
{
    LOG("[jni] ThrowNew %s: %s\n", jclass_name(harg(c, 1)), gstr(harg(c, 2)));
    pending_exc = jnew_obj(jclass_name(harg(c, 1)));
    hret(c, 0);
}
static void e_ExceptionOccurred(cpu_t *c) { hret(c, pending_exc); }
static void e_ExceptionDescribe(cpu_t *c)
{
    if (pending_exc) LOG("[jni] pending exception %s\n", jclass_name(pending_exc));
}
static void e_ExceptionClear(cpu_t *c) { pending_exc = 0; }
static void e_ExceptionCheck(cpu_t *c) { hret(c, pending_exc != 0); }
static void e_FatalError(cpu_t *c)  { emu_trap(c, "JNI FatalError: %s", gstr(harg(c, 1))); }
static void e_PushLocalFrame(cpu_t *c) { hret(c, 0); }
static void e_PopLocalFrame(cpu_t *c)  { hret(c, harg(c, 1)); }
static void e_NewGlobalRef(cpu_t *c)
{
    u32 h = harg(c, 1);
    jobj_t *o = jo(h);
    if (o) { pthread_mutex_lock(&jlock); o->refs++; pthread_mutex_unlock(&jlock); }
    hret(c, h);
}
static void e_DeleteGlobalRef(cpu_t *c) { jo_unref(harg(c, 1)); }
static void e_DeleteLocalRef(cpu_t *c)  { u32 h = harg(c, 1); if (lf_take(h) || !lf.depth) jo_unref(h); }
static void e_NewLocalRef(cpu_t *c)
{
    u32 h = harg(c, 1);
    jobj_t *o = jo(h);
    if (o && o->kind != K_CLASS) { pthread_mutex_lock(&jlock); o->refs++; pthread_mutex_unlock(&jlock); lf_push_handle(h); }
    hret(c, h);
}
static void e_IsSameObject(cpu_t *c) { hret(c, harg(c, 1) == harg(c, 2)); }
static void e_EnsureLocalCapacity(cpu_t *c) { hret(c, 0); }
static void e_AllocObject(cpu_t *c)
{
    u32 h = jo_new(K_OBJ);
    jo(h)->cls = harg(c, 1);
    hret(c, h);
}
static void e_GetObjectClass(cpu_t *c)
{
    jobj_t *o = jo(harg(c, 1));
    hret(c, o ? (o->kind == K_CLASS ? jclass("java/lang/Class") : o->cls) : 0);
}
static void e_IsInstanceOf(cpu_t *c) { hret(c, 1); }
static void e_GetMethodID(cpu_t *c)  { hret(c, meth_new(harg(c, 1), gstr(harg(c, 2)), gstr(harg(c, 3)))); }

/* kind: 0 virtual (env,obj,mid,..), 1 nonvirtual (env,obj,cls,mid,..), 2 static (env,cls,mid,..) */
static void jcall(cpu_t *c, int kind, int mode)
{
    int mpos = kind == 1 ? 3 : 2;
    u32 self = harg(c, 1);
    u32 mid = harg(c, mpos);
    u32 mi = (mid - JMID_BASE) >> 2;
    if (mid < JMID_BASE || mi >= (u32)nmeths)
        emu_trap(c, "JNI call with bad methodID %08x", mid);
    jargs_t a = { .mode = mode };
    argwalk_init(&a.w, c);
    a.w.reg = mpos + 1;
    if (mode != M_DOTS) a.p = harg(c, mpos + 1);
    cur_meth = &meths[mi];
    u64 r = meths[mi].fn(c, self, &a);
    static int trace = -1;
    if (trace < 0) trace = getenv("RR2_JNI_TRACE") != NULL;
    if (trace) LOG("[jni] call %s.%s%s -> %llx%s\n", jclass_name(meths[mi].cls), meths[mi].name, meths[mi].sig,
                   (unsigned long long)r, meths[mi].fn == j_default ? " (default)" : "");
    hret64(c, r);
}

#define JCALL_FNS(K, kn) \
    static void e_call##kn##_d(cpu_t *c) { jcall(c, K, M_DOTS); } \
    static void e_call##kn##_v(cpu_t *c) { jcall(c, K, M_V); } \
    static void e_call##kn##_a(cpu_t *c) { jcall(c, K, M_A); }
JCALL_FNS(0, Virt)
JCALL_FNS(1, Nonv)
JCALL_FNS(2, Stat)

static void jnew_object(cpu_t *c, int mode)
{
    u32 cls = harg(c, 1);
    u32 h = jo_new(K_OBJ);
    jo(h)->cls = cls;
    u32 mid = harg(c, 2), mi = (mid - JMID_BASE) >> 2;
    if (mid >= JMID_BASE && mi < (u32)nmeths && meths[mi].fn != j_default) {
        jargs_t a = { .mode = mode };
        argwalk_init(&a.w, c);
        a.w.reg = 3;
        if (mode != M_DOTS) a.p = harg(c, 3);
        cur_meth = &meths[mi];
        meths[mi].fn(c, h, &a);
    }
    hret(c, h);
}
static void e_NewObject(cpu_t *c)  { jnew_object(c, M_DOTS); }
static void e_NewObjectV(cpu_t *c) { jnew_object(c, M_V); }
static void e_NewObjectA(cpu_t *c) { jnew_object(c, M_A); }

/* fields: value store keyed by (object-or-class, fieldID) */
#define FSTORE 8192
static struct { u32 obj, fid; u64 v; } fstore[FSTORE];

static u32 field_new(u32 cls, const char *name, const char *sig)
{
    pthread_mutex_lock(&jlock);
    for (int i = 0; i < nfields; i++)
        if (fields[i].cls == cls && !strcmp(fields[i].name, name)) {
            pthread_mutex_unlock(&jlock);
            return JFID_BASE + 4 * i;
        }
    if (nfields >= MAX_JFIELD) fatal("JNI field table full");
    int i = nfields++;
    fields[i].cls = cls;
    fields[i].name = strdup(name);
    fields[i].sig = strdup(sig);
    pthread_mutex_unlock(&jlock);
    VLOG(1, "[jni] field %s.%s %s\n", jclass_name(cls), name, sig);
    return JFID_BASE + 4 * i;
}

static u64 *fslot(u32 obj, u32 fid, bool create)
{
    jobj_t *o = jo(obj);
    if (o) {
        for (u16 k = 0; k < o->nfv; k++) if (o->fv[k].fid == fid) return &o->fv[k].v;
        if (!create) return NULL;
        if (o->nfv == o->capfv) { o->capfv = o->capfv ? o->capfv * 2 : 8; o->fv = realloc(o->fv, o->capfv * sizeof(*o->fv)); }
        o->fv[o->nfv].fid = fid; o->fv[o->nfv].v = 0;
        return &o->fv[o->nfv++].v;
    }
    u32 h = (obj * 2654435761u ^ fid * 40503u) & (FSTORE - 1);   /* not an object handle: global store */
    for (u32 n = 0; n < FSTORE; n++, h = (h + 1) & (FSTORE - 1)) {
        if (fstore[h].obj == obj && fstore[h].fid == fid) return &fstore[h].v;
        if (!fstore[h].obj) {
            if (!create) return NULL;
            fstore[h].obj = obj; fstore[h].fid = fid;
            return &fstore[h].v;
        }
    }
    fatal("JNI field store full");
}
static bool fid_is_obj(u32 fid)
{
    u32 i = (fid - JFID_BASE) >> 2;
    return fid >= JFID_BASE && i < (u32)nfields && (fields[i].sig[0] == 'L' || fields[i].sig[0] == '[');
}
/* host-side field access for the Java implementations (objects stored keep a reference) */
static void jfield_set(u32 obj, const char *cls, const char *name, const char *sig, u64 v)
{
    u32 fid = field_new(jclass(cls), name, sig);
    bool isobj = sig[0] == 'L' || sig[0] == '[';
    if (isobj && jo((u32)v)) { jobj_t *n = jo((u32)v); pthread_mutex_lock(&jlock); n->refs++; pthread_mutex_unlock(&jlock); }
    pthread_mutex_lock(&jlock);
    u64 *slot = fslot(obj, fid, true), old = *slot;
    *slot = v;
    pthread_mutex_unlock(&jlock);
    if (isobj && old) jo_unref((u32)old);
}
static u64 jfield_get(u32 obj, const char *cls, const char *name, const char *sig)
{
    u32 fid = field_new(jclass(cls), name, sig);
    pthread_mutex_lock(&jlock);
    u64 *slot = fslot(obj, fid, false), v = slot ? *slot : 0;
    pthread_mutex_unlock(&jlock);
    return v;
}

static void e_GetFieldID(cpu_t *c) { hret(c, field_new(harg(c, 1), gstr(harg(c, 2)), gstr(harg(c, 3)))); }
static void e_GetField(cpu_t *c)
{
    pthread_mutex_lock(&jlock);
    u64 *v = fslot(harg(c, 1), harg(c, 2), false);
    u64 r = v ? *v : 0;
    jobj_t *o = fid_is_obj(harg(c, 2)) ? jo((u32)r) : NULL;
    if (o && o->kind != K_CLASS) o->refs++;             /* GetObjectField returns a new local reference */
    pthread_mutex_unlock(&jlock);
    if (o && o->kind != K_CLASS) lf_push_handle((u32)r);
    static int ft = -1;
    if (ft < 0) ft = getenv("RR2_FIELD_TRACE") ? 400 : 0;
    if (ft > 0) {
        u32 fi = (harg(c, 2) - JFID_BASE) >> 2;
        if (fi < (u32)nfields) { ft--; LOG("[jni] get %s.%s = %llx (%s)\n", jclass_name(fields[fi].cls), fields[fi].name,
                                        (unsigned long long)r, v ? "set" : "unset"); }
    }
    hret64(c, r);
}
static void e_SetField32(cpu_t *c)
{
    u32 v = harg(c, 3), old;
    bool isobj = fid_is_obj(harg(c, 2));
    pthread_mutex_lock(&jlock);
    if (isobj && jo(v)) jo(v)->refs++;
    u64 *slot = fslot(harg(c, 1), harg(c, 2), true);
    old = (u32)*slot;
    *slot = v;
    pthread_mutex_unlock(&jlock);
    if (isobj && old) jo_unref(old);
}
static void e_SetField64(cpu_t *c)
{
    argwalk_t w; argwalk_init(&w, c); w.reg = 3;
    u64 v = argwalk_u64(&w);
    pthread_mutex_lock(&jlock);
    *fslot(harg(c, 1), harg(c, 2), true) = v;
    pthread_mutex_unlock(&jlock);
}

/* strings */
static void e_NewString(cpu_t *c)
{
    gptr u = harg(c, 1);
    u32 n = harg(c, 2);
    char *s = malloc(n + 1);
    for (u32 i = 0; i < n; i++) { u16 ch = ld16(u + 2 * i); s[i] = ch < 0x80 ? (char)ch : '?'; }
    s[n] = 0;
    hret(c, jstr_new(s));
    free(s);
}
static void e_GetStringLength(cpu_t *c) { jobj_t *o = jo(harg(c, 1)); hret(c, o ? o->len : 0); }
static void e_GetStringChars(cpu_t *c)
{
    jobj_t *o = jo(harg(c, 1));
    if (!o || o->kind != K_STR) { hret(c, 0); return; }
    gptr p = guest_malloc(o->len * 2 + 2);
    for (u32 i = 0; i <= o->len; i++) st16(p + 2 * i, (u8)o->str[i]);
    if (harg(c, 2)) st8(harg(c, 2), 1);
    hret(c, p);
}
static void e_ReleaseStringChars(cpu_t *c) { guest_free(harg(c, 2)); }
static void e_NewStringUTF(cpu_t *c) { hret(c, harg(c, 1) ? jstr_new(gstr(harg(c, 1))) : 0); }
static void e_GetStringUTFChars(cpu_t *c)
{
    jobj_t *o = jo(harg(c, 1));
    if (!o || o->kind != K_STR) { hret(c, 0); return; }
    gptr p = guest_malloc(o->len + 1);
    memcpy(g2h(p), o->str, o->len + 1);
    if (harg(c, 2)) st8(harg(c, 2), 1);
    hret(c, p);
}
static void e_ReleaseStringUTFChars(cpu_t *c) { guest_free(harg(c, 2)); }
static void e_GetStringRegion(cpu_t *c)
{
    jobj_t *o = jo(harg(c, 1));
    u32 st = harg(c, 2), n = harg(c, 3);
    gptr buf = ld32(c->r[13]);
    for (u32 i = 0; o && i < n && st + i < o->len; i++) st16(buf + 2 * i, (u8)o->str[st + i]);
}
static void e_GetStringUTFRegion(cpu_t *c)
{
    jobj_t *o = jo(harg(c, 1));
    u32 st = harg(c, 2), n = harg(c, 3);
    gptr buf = ld32(c->r[13]);
    if (o && st <= o->len) {
        if (st + n > o->len) n = o->len - st;
        memcpy(g2h(buf), o->str + st, n);
        st8(buf + n, 0);
    }
}

/* arrays */
static void e_GetArrayLength(cpu_t *c) { jobj_t *o = jo(harg(c, 1)); hret(c, o ? o->len : 0); }
static void e_NewObjectArray(cpu_t *c)
{
    u32 n = harg(c, 1), h = jobjarr_new(n, harg(c, 2)), init = harg(c, 3);
    for (u32 i = 0; i < n; i++) jo(h)->objs[i] = init;
    hret(c, h);
}
static void e_GetObjectArrayElement(cpu_t *c)
{
    jobj_t *o = jo(harg(c, 1));
    u32 i = harg(c, 2);
    hret(c, o && o->kind == K_OBJARR && i < o->len ? o->objs[i] : 0);
}
static void e_SetObjectArrayElement(cpu_t *c)
{
    jobj_t *o = jo(harg(c, 1));
    u32 i = harg(c, 2);
    if (o && o->kind == K_OBJARR && i < o->len) o->objs[i] = harg(c, 3);
}
#define NEWARR(nm, sz) static void e_New##nm##Array(cpu_t *c) { hret(c, jarr_new(harg(c, 1), sz)); }
NEWARR(Boolean, 1) NEWARR(Byte, 1) NEWARR(Char, 2) NEWARR(Short, 2)
NEWARR(Int, 4) NEWARR(Long, 8) NEWARR(Float, 4) NEWARR(Double, 8)

static void e_GetArrayElements(cpu_t *c)
{
    jobj_t *o = jo(harg(c, 1));
    if (harg(c, 2)) st8(harg(c, 2), 0);
    hret(c, o && o->kind == K_ARR ? o->data : 0);
}
static void e_ReleaseArrayElements(cpu_t *c) {}
static void e_GetArrayRegion(cpu_t *c)
{
    jobj_t *o = jo(harg(c, 1));
    u32 st = harg(c, 2), n = harg(c, 3);
    gptr buf = ld32(c->r[13]);
    if (o && o->kind == K_ARR && st + n <= o->len)
        memcpy(g2h(buf), g2h(o->data + st * o->elem), n * o->elem);
}
static void e_SetArrayRegion(cpu_t *c)
{
    jobj_t *o = jo(harg(c, 1));
    u32 st = harg(c, 2), n = harg(c, 3);
    gptr buf = ld32(c->r[13]);
    if (o && o->kind == K_ARR && st + n <= o->len)
        memcpy(g2h(o->data + st * o->elem), g2h(buf), n * o->elem);
}

/* natives registered by the guest */
static struct { char *cls, *name, *sig; u32 fn; } natives[MAX_NATIVES];
static int nnatives;

static void e_RegisterNatives(cpu_t *c)
{
    const char *cn = jclass_name(harg(c, 1));
    gptr tab = harg(c, 2);
    u32 n = harg(c, 3);
    for (u32 i = 0; i < n && nnatives < MAX_NATIVES; i++, tab += 12) {
        natives[nnatives].cls = strdup(cn);
        natives[nnatives].name = strdup(gstr(ld32(tab)));
        natives[nnatives].sig = strdup(gstr(ld32(tab + 4)));
        natives[nnatives].fn = ld32(tab + 8);
        VLOG(1, "[jni] RegisterNatives %s.%s%s -> %08x\n", cn,
             natives[nnatives].name, natives[nnatives].sig, natives[nnatives].fn);
        nnatives++;
    }
    hret(c, 0);
}
static void e_UnregisterNatives(cpu_t *c) { hret(c, 0); }
static void e_Monitor(cpu_t *c) { hret(c, 0); }
static void e_GetJavaVM(cpu_t *c) { st32(harg(c, 1), g_vm); hret(c, 0); }
static void e_NewDirectByteBuffer(cpu_t *c)
{
    u32 h = jnew_obj("java/nio/ByteBuffer");
    argwalk_t w; argwalk_init(&w, c); w.reg = 2;
    jo(h)->data = harg(c, 1);
    jo(h)->len = (u32)argwalk_u64(&w);
    hret(c, h);
}
static void e_GetDirectBufferAddress(cpu_t *c) { jobj_t *o = jo(harg(c, 1)); hret(c, o ? o->data : 0); }
static void e_GetDirectBufferCapacity(cpu_t *c) { jobj_t *o = jo(harg(c, 1)); hret64(c, o ? o->len : (u64)-1); }
static void e_GetObjectRefType(cpu_t *c) { hret(c, 1); }

/* JavaVM invoke interface */
static void vm_DestroyJavaVM(cpu_t *c) { hret(c, 0); }
static void vm_GetEnv(cpu_t *c)        { st32(harg(c, 1), g_env); hret(c, 0); }
static void vm_Detach(cpu_t *c)        { hret(c, 0); }

/* ------------------------------------------------------------------ */

static const char *const env_names[233] = {
    NULL, NULL, NULL, NULL, "GetVersion", "DefineClass", "FindClass",
    "FromReflectedMethod", "FromReflectedField", "ToReflectedMethod", "GetSuperclass",
    "IsAssignableFrom", "ToReflectedField", "Throw", "ThrowNew", "ExceptionOccurred",
    "ExceptionDescribe", "ExceptionClear", "FatalError", "PushLocalFrame", "PopLocalFrame",
    "NewGlobalRef", "DeleteGlobalRef", "DeleteLocalRef", "IsSameObject", "NewLocalRef",
    "EnsureLocalCapacity", "AllocObject", "NewObject", "NewObjectV", "NewObjectA",
    "GetObjectClass", "IsInstanceOf", "GetMethodID",
    [94] = "GetFieldID", [113] = "GetStaticMethodID", [144] = "GetStaticFieldID",
    [163] = "NewString", "GetStringLength", "GetStringChars", "ReleaseStringChars",
    "NewStringUTF", "GetStringUTFLength", "GetStringUTFChars", "ReleaseStringUTFChars",
    "GetArrayLength", "NewObjectArray", "GetObjectArrayElement", "SetObjectArrayElement",
    "NewBooleanArray", "NewByteArray", "NewCharArray", "NewShortArray", "NewIntArray",
    "NewLongArray", "NewFloatArray", "NewDoubleArray",
    [215] = "RegisterNatives", "UnregisterNatives", "MonitorEnter", "MonitorExit",
    "GetJavaVM", "GetStringRegion", "GetStringUTFRegion", "GetPrimitiveArrayCritical",
    "ReleasePrimitiveArrayCritical", "GetStringCritical", "ReleaseStringCritical",
    "NewWeakGlobalRef", "DeleteWeakGlobalRef", "ExceptionCheck", "NewDirectByteBuffer",
    "GetDirectBufferAddress", "GetDirectBufferCapacity", "GetObjectRefType",
};

static const char *const jtypes[10] = {
    "Object", "Boolean", "Byte", "Char", "Short", "Int", "Long", "Float", "Double", "Void"
};
static const char *const atypes[8] = {
    "Boolean", "Byte", "Char", "Short", "Int", "Long", "Float", "Double"
};

static u32 slot(const char *name, hle_fn fn)
{
    if (fn) hle_register(name, fn);
    return hle_bind(name);
}

void jni_init(void)
{
    static char names[233][48];
    hle_fn fns[233] = {
        [4] = e_GetVersion, [6] = e_FindClass, [10] = e_GetSuperclass,
        [11] = e_IsAssignableFrom, [13] = e_Throw, [14] = e_ThrowNew,
        [15] = e_ExceptionOccurred, [16] = e_ExceptionDescribe, [17] = e_ExceptionClear,
        [18] = e_FatalError, [19] = e_PushLocalFrame, [20] = e_PopLocalFrame,
        [21] = e_NewGlobalRef, [22] = e_DeleteGlobalRef, [23] = e_DeleteLocalRef,
        [24] = e_IsSameObject, [25] = e_NewLocalRef, [26] = e_EnsureLocalCapacity,
        [27] = e_AllocObject, [28] = e_NewObject, [29] = e_NewObjectV, [30] = e_NewObjectA,
        [31] = e_GetObjectClass, [32] = e_IsInstanceOf, [33] = e_GetMethodID,
        [94] = e_GetFieldID, [113] = e_GetMethodID, [144] = e_GetFieldID,
        [163] = e_NewString, [164] = e_GetStringLength, [165] = e_GetStringChars,
        [166] = e_ReleaseStringChars, [167] = e_NewStringUTF, [168] = e_GetStringLength,
        [169] = e_GetStringUTFChars, [170] = e_ReleaseStringUTFChars,
        [171] = e_GetArrayLength, [172] = e_NewObjectArray,
        [173] = e_GetObjectArrayElement, [174] = e_SetObjectArrayElement,
        [175] = e_NewBooleanArray, [176] = e_NewByteArray, [177] = e_NewCharArray,
        [178] = e_NewShortArray, [179] = e_NewIntArray, [180] = e_NewLongArray,
        [181] = e_NewFloatArray, [182] = e_NewDoubleArray,
        [215] = e_RegisterNatives, [216] = e_UnregisterNatives,
        [217] = e_Monitor, [218] = e_Monitor, [219] = e_GetJavaVM,
        [220] = e_GetStringRegion, [221] = e_GetStringUTFRegion,
        [222] = e_GetArrayElements, [223] = e_ReleaseArrayElements,
        [224] = e_GetStringChars, [225] = e_ReleaseStringChars,
        [226] = e_NewGlobalRef, [227] = e_DeleteGlobalRef, [228] = e_ExceptionCheck,
        [229] = e_NewDirectByteBuffer, [230] = e_GetDirectBufferAddress,
        [231] = e_GetDirectBufferCapacity, [232] = e_GetObjectRefType,
    };
    static const hle_fn callv[3][3] = {
        { e_callVirt_d, e_callVirt_v, e_callVirt_a },
        { e_callNonv_d, e_callNonv_v, e_callNonv_a },
        { e_callStat_d, e_callStat_v, e_callStat_a },
    };
    static const int callbase[3] = { 34, 64, 114 };
    static const char *const callpre[3] = { "Call", "CallNonvirtual", "CallStatic" };
    static const char *const sfx[3] = { "", "V", "A" };

    for (int i = 0; i < 233; i++)
        if (env_names[i]) snprintf(names[i], sizeof(names[i]), "JNIEnv::%s", env_names[i]);
    for (int k = 0; k < 3; k++)
        for (int t = 0; t < 10; t++)
            for (int m = 0; m < 3; m++) {
                int i = callbase[k] + t * 3 + m;
                snprintf(names[i], sizeof(names[i]), "JNIEnv::%s%sMethod%s", callpre[k], jtypes[t], sfx[m]);
                fns[i] = callv[k][m];
            }
    for (int st = 0; st < 2; st++)
        for (int t = 0; t < 9; t++) {
            int get = (st ? 145 : 95) + t, set = (st ? 154 : 104) + t;
            snprintf(names[get], sizeof(names[get]), "JNIEnv::Get%s%sField", st ? "Static" : "", jtypes[t]);
            snprintf(names[set], sizeof(names[set]), "JNIEnv::Set%s%sField", st ? "Static" : "", jtypes[t]);
            fns[get] = e_GetField;
            fns[set] = (t == 6 || t == 8) ? e_SetField64 : e_SetField32;
        }
    for (int t = 0; t < 8; t++) {
        snprintf(names[183 + t], 48, "JNIEnv::Get%sArrayElements", atypes[t]);
        snprintf(names[191 + t], 48, "JNIEnv::Release%sArrayElements", atypes[t]);
        snprintf(names[199 + t], 48, "JNIEnv::Get%sArrayRegion", atypes[t]);
        snprintf(names[207 + t], 48, "JNIEnv::Set%sArrayRegion", atypes[t]);
        fns[183 + t] = e_GetArrayElements;
        fns[191 + t] = e_ReleaseArrayElements;
        fns[199 + t] = e_GetArrayRegion;
        fns[207 + t] = e_SetArrayRegion;
    }

    gptr etab = hle_data_alloc(233 * 4, 16);
    for (int i = 0; i < 233; i++)
        st32(etab + 4 * i, names[i][0] ? slot(names[i], fns[i]) : slot("JNIEnv::reserved", NULL));
    g_env = hle_data_alloc(4, 16);
    st32(g_env, etab);

    gptr vtab = hle_data_alloc(8 * 4, 16);
    for (int i = 0; i < 3; i++) st32(vtab + 4 * i, slot("JavaVM::reserved", NULL));
    st32(vtab + 12, slot("JavaVM::DestroyJavaVM", vm_DestroyJavaVM));
    st32(vtab + 16, slot("JavaVM::AttachCurrentThread", vm_GetEnv));
    st32(vtab + 20, slot("JavaVM::DetachCurrentThread", vm_Detach));
    st32(vtab + 24, slot("JavaVM::GetEnv", vm_GetEnv));
    st32(vtab + 28, slot("JavaVM::AttachCurrentThreadAsDaemon", vm_GetEnv));
    g_vm = hle_data_alloc(4, 16);
    st32(g_vm, vtab);

    snprintf(g_apk_assets, sizeof(g_apk_assets), "%s", G.apk_assets_dir);
    h_activity = jnew_obj(G.game == 3 ? "com/firemint/realracing/MainActivity" : "com/ea/game/realracing2_OTD_row/RealRacing2Activity");
    h_assets = jnew_obj("android/content/res/AssetManager");
}

u32 jni_env_ptr(void) { return g_env; }
u32 jni_vm_ptr(void)  { return g_vm; }
u32 jni_activity(void) { return h_activity; }
u32 jni_object(const char *cls) { return jnew_obj(cls); }
u32 jni_class(const char *name) { return jclass(name); }
u32 jni_string(const char *s) { return jstr_new(s); }

#include "jni_rr3_text.inc"
