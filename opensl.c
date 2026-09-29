/*
 * opensl.c - the OpenSL ES subset FMOD Ex uses on Android: an engine, an
 * output mix and one PCM audio player fed through an Android simple buffer
 * queue. Interfaces are guest vtables of HLE entry points; libOpenSLES.so is
 * "dlopen"ed and its symbols dlsym'd through here. A host thread plays each
 * enqueued buffer (host_audio_write paces it) and then runs the guest's
 * buffer-queue callback, which is where FMOD mixes and enqueues the next one.
 */
#include "emu.h"

cpu_t *guest_cpu_for_host_thread(const char *name);

#define SL_OK 0
#define SL_FEATURE_UNSUPPORTED 12
#define SL_PLAYSTATE_STOPPED 1
#define SL_PLAYSTATE_PAUSED  2
#define SL_PLAYSTATE_PLAYING 3

enum { IID_OBJECT, IID_ENGINE, IID_PLAY, IID_BUFQ, IID_ANDROIDCFG, IID_RECORD, IID_VOLUME, IID_COUNT };
static const char *const iid_names[IID_COUNT] = {
    "SL_IID_OBJECT", "SL_IID_ENGINE", "SL_IID_PLAY", "SL_IID_ANDROIDSIMPLEBUFFERQUEUE",
    "SL_IID_ANDROIDCONFIGURATION", "SL_IID_RECORD", "SL_IID_VOLUME" };
static gptr iid_var[IID_COUNT], iid_guid[IID_COUNT];     /* the exported pointer variables and their GUIDs */
static gptr vt[IID_COUNT];                                /* vtables */

typedef struct slobj {
    int kind;                                             /* 0 engine, 1 output mix, 2 player */
    gptr obj, itf[IID_COUNT];                             /* guest object / interface addresses */
    /* player */
    int rate, channels, state;
    gptr cb, cb_ctx;
    struct { gptr p; u32 n; } q[32];
    int qh, qt;
    pthread_t th; bool running;
    pthread_mutex_t lock; pthread_cond_t cv;
} slobj;
static slobj *objs[32];
static int nobjs;

static slobj *by_addr(gptr a)
{
    for (int i = 0; i < nobjs; i++) {
        if (objs[i]->obj == a) return objs[i];
        for (int k = 0; k < IID_COUNT; k++) if (objs[i]->itf[k] == a) return objs[i];
    }
    return NULL;
}

static gptr new_itf(int iid)                              /* struct { vtable* } */
{
    gptr p = hle_data_alloc(16, 8);
    st32(p, vt[iid]);
    return p;
}
static slobj *new_obj(int kind)
{
    if (nobjs >= 32) fatal("opensl: too many objects");
    slobj *o = calloc(1, sizeof(*o));
    o->kind = kind;
    o->obj = new_itf(IID_OBJECT);
    o->itf[IID_OBJECT] = o->obj;
    if (kind == 0) o->itf[IID_ENGINE] = new_itf(IID_ENGINE);
    if (kind == 2) {
        o->itf[IID_PLAY] = new_itf(IID_PLAY);
        o->itf[IID_BUFQ] = new_itf(IID_BUFQ);
        o->itf[IID_ANDROIDCFG] = new_itf(IID_ANDROIDCFG);
        o->itf[IID_VOLUME] = new_itf(IID_VOLUME);
        o->state = SL_PLAYSTATE_STOPPED;
        pthread_mutex_init(&o->lock, NULL);
        pthread_cond_init(&o->cv, NULL);
    }
    objs[nobjs++] = o;
    return o;
}
static int iid_of(gptr guid)
{
    for (int k = 0; k < IID_COUNT; k++) if (guid == iid_guid[k] || !memcmp(g2h(guid), g2h(iid_guid[k]), 16)) return k;
    return -1;
}

/* ---- player thread ---- */
static void *player_main(void *arg)
{
    slobj *o = arg;
    cpu_t *c = guest_cpu_for_host_thread("opensl");
    if (!c) return NULL;
    if (!host_audio_open(o->rate, o->channels)) LOG("[opensl] no host audio device: playing silently\n");
    for (;;) {
        pthread_mutex_lock(&o->lock);
        while (o->running && (o->state != SL_PLAYSTATE_PLAYING || o->qh == o->qt)) pthread_cond_wait(&o->cv, &o->lock);
        if (!o->running) { pthread_mutex_unlock(&o->lock); break; }
        gptr p = o->q[o->qh].p; u32 n = o->q[o->qh].n;
        pthread_mutex_unlock(&o->lock);
        host_audio_write(g2h(p), n);                      /* blocks while the host queue is full */
        pthread_mutex_lock(&o->lock);
        o->qh = (o->qh + 1) % 32;
        gptr cb = o->cb, ctx = o->cb_ctx;
        pthread_mutex_unlock(&o->lock);
        if (cb) { u32 a[2] = { o->itf[IID_BUFQ], ctx }; emu_call(c, cb, 2, a); }
    }
    return NULL;
}

/* ---- Object ---- */
static void o_Realize(cpu_t *c)  { hret(c, SL_OK); }
static void o_Resume(cpu_t *c)   { hret(c, SL_OK); }
static void o_GetState(cpu_t *c) { st32(harg(c, 1), 2 /* REALIZED */); hret(c, SL_OK); }
static void o_GetInterface(cpu_t *c)
{
    slobj *o = by_addr(harg(c, 0));
    int k = iid_of(harg(c, 1));
    if (!o || k < 0 || !o->itf[k]) { st32(harg(c, 2), 0); hret(c, SL_FEATURE_UNSUPPORTED); return; }
    st32(harg(c, 2), o->itf[k]);
    hret(c, SL_OK);
}
static void o_Destroy(cpu_t *c)
{
    slobj *o = by_addr(harg(c, 0));
    if (o && o->kind == 2 && o->running) {
        pthread_mutex_lock(&o->lock); o->running = false; pthread_cond_broadcast(&o->cv); pthread_mutex_unlock(&o->lock);
        pthread_join(o->th, NULL);
    }
}
static void sl_ok(cpu_t *c) { hret(c, SL_OK); }
static void sl_unsupported(cpu_t *c) { hret(c, SL_FEATURE_UNSUPPORTED); }

/* ---- Engine ---- */
static void e_CreateOutputMix(cpu_t *c)
{
    slobj *o = new_obj(1);
    st32(harg(c, 1), o->obj);
    hret(c, SL_OK);
}
static void e_CreateAudioPlayer(cpu_t *c)
{
    gptr src = harg(c, 2), fmt = ld32(src + 4);
    slobj *o = new_obj(2);
    o->channels = fmt ? (int)ld32(fmt + 4) : 2;
    o->rate = fmt ? (int)(ld32(fmt + 8) / 1000) : 44100;              /* samplesPerSec is in milliHz */
    u32 bits = fmt ? ld32(fmt + 12) : 16;
    LOG("[opensl] audio player: %d Hz, %d ch, %u bit\n", o->rate, o->channels, bits);
    if (bits != 16) LOG("[opensl] warning: %u-bit PCM not supported, expect noise\n", bits);
    st32(harg(c, 1), o->obj);
    hret(c, SL_OK);
}

/* ---- Play ---- */
static void p_SetPlayState(cpu_t *c)
{
    slobj *o = by_addr(harg(c, 0));
    if (!o) { hret(c, SL_OK); return; }
    pthread_mutex_lock(&o->lock);
    o->state = (int)harg(c, 1);
    if (o->state == SL_PLAYSTATE_PLAYING && !o->running) {
        o->running = true;
        pthread_create(&o->th, NULL, player_main, o);
    }
    if (o->state == SL_PLAYSTATE_STOPPED) o->qh = o->qt;
    pthread_cond_broadcast(&o->cv);
    pthread_mutex_unlock(&o->lock);
    hret(c, SL_OK);
}
static void p_GetPlayState(cpu_t *c) { slobj *o = by_addr(harg(c, 0)); st32(harg(c, 1), o ? (u32)o->state : 1); hret(c, SL_OK); }
static void p_GetPosition(cpu_t *c) { st32(harg(c, 1), 0); hret(c, SL_OK); }

/* ---- AndroidSimpleBufferQueue ---- */
static void q_Enqueue(cpu_t *c)
{
    slobj *o = by_addr(harg(c, 0));
    if (!o) { hret(c, SL_OK); return; }
    pthread_mutex_lock(&o->lock);
    int nt = (o->qt + 1) % 32;
    if (nt == o->qh) { pthread_mutex_unlock(&o->lock); hret(c, 7 /* BUFFER_INSUFFICIENT */); return; }
    o->q[o->qt].p = harg(c, 1); o->q[o->qt].n = harg(c, 2);
    o->qt = nt;
    pthread_cond_broadcast(&o->cv);
    pthread_mutex_unlock(&o->lock);
    hret(c, SL_OK);
}
static void q_Clear(cpu_t *c)
{
    slobj *o = by_addr(harg(c, 0));
    if (o) { pthread_mutex_lock(&o->lock); o->qh = o->qt; pthread_mutex_unlock(&o->lock); }
    hret(c, SL_OK);
}
static void q_GetState(cpu_t *c)
{
    slobj *o = by_addr(harg(c, 0));
    u32 n = o ? (u32)((o->qt - o->qh + 32) % 32) : 0;
    st32(harg(c, 1), n); st32(harg(c, 1) + 4, 0);
    hret(c, SL_OK);
}
static void q_RegisterCallback(cpu_t *c)
{
    slobj *o = by_addr(harg(c, 0));
    if (o) { pthread_mutex_lock(&o->lock); o->cb = harg(c, 1); o->cb_ctx = harg(c, 2); pthread_mutex_unlock(&o->lock); }
    hret(c, SL_OK);
}

/* ---- slCreateEngine ---- */
static void sl_CreateEngine(cpu_t *c)
{
    slobj *o = new_obj(0);
    st32(harg(c, 0), o->obj);
    hret(c, SL_OK);
}

static gptr make_vtable(const char *prefix, int n, const hle_fn *fns)
{
    gptr t = hle_data_alloc((u32)n * 4, 8);
    for (int i = 0; i < n; i++) {
        char *name = malloc(64);
        snprintf(name, 64, "opensl:%s.%d", prefix, i);
        hle_register(name, fns[i] ? fns[i] : sl_ok);
        st32(t + 4 * (u32)i, hle_bind(name));
    }
    return t;
}

void opensl_init(void)
{
    static const hle_fn obj_f[10] = { o_Realize, o_Resume, o_GetState, o_GetInterface, sl_ok, sl_ok, o_Destroy, sl_ok, sl_ok, sl_ok };
    static const hle_fn eng_f[15] = { sl_unsupported, sl_unsupported, e_CreateAudioPlayer, sl_unsupported, sl_unsupported,
                                      sl_unsupported, sl_unsupported, e_CreateOutputMix, sl_unsupported, sl_unsupported };
    static const hle_fn play_f[12] = { p_SetPlayState, p_GetPlayState, sl_ok, p_GetPosition };
    static const hle_fn bq_f[4] = { q_Enqueue, q_Clear, q_GetState, q_RegisterCallback };
    static const hle_fn cfg_f[2] = { sl_ok, sl_ok };
    static const hle_fn vol_f[10] = { 0 };
    vt[IID_OBJECT] = make_vtable("Object", 10, obj_f);
    vt[IID_ENGINE] = make_vtable("Engine", 15, eng_f);
    vt[IID_PLAY] = make_vtable("Play", 12, play_f);
    vt[IID_BUFQ] = make_vtable("BufferQueue", 4, bq_f);
    vt[IID_ANDROIDCFG] = make_vtable("AndroidConfiguration", 2, cfg_f);
    vt[IID_VOLUME] = make_vtable("Volume", 10, vol_f);
    for (int k = 0; k < IID_COUNT; k++) {
        iid_guid[k] = hle_data_alloc(16, 8);
        for (int b = 0; b < 16; b++) st8(iid_guid[k] + (u32)b, (u8)(0x51 + k * 16 + b));   /* any unique bytes */
        iid_var[k] = hle_data_alloc(4, 4);
        st32(iid_var[k], iid_guid[k]);
    }
    hle_register("slCreateEngine", sl_CreateEngine);
}

/* dlsym on the libOpenSLES.so handle */
u32 opensl_dlsym(const char *name)
{
    if (!vt[IID_OBJECT]) opensl_init();
    for (int k = 0; k < IID_COUNT; k++) if (!strcmp(name, iid_names[k])) return iid_var[k];
    if (!strcmp(name, "slCreateEngine")) return hle_bind("slCreateEngine");
    return 0;
}
