/*
 * host.c - window, GLES2 context and events through SDL2, loaded with dlopen
 * so the build needs no SDL/GL development packages. Only the handful of SDL
 * entry points and struct offsets used here are declared.
 */
#include "emu.h"
#include <dlfcn.h>
#include <unistd.h>
#include <time.h>

#define SDL_INIT_AUDIO          0x00000010u
#define SDL_INIT_VIDEO          0x00000020u
#define SDL_INIT_EVENTS         0x00004000u
#define SDL_INIT_GAMECONTROLLER 0x00002000u
#define SDL_WINDOW_FULLSCREEN   0x00000001u
#define SDL_WINDOW_FULLSCREEN_DESKTOP 0x00001001u
#define SDL_WINDOW_OPENGL       0x00000002u
#define SDL_WINDOW_SHOWN        0x00000004u
#define SDL_WINDOWPOS_CENTERED  0x2FFF0000
#define SDL_GL_RED_SIZE 0
#define SDL_GL_GREEN_SIZE 1
#define SDL_GL_BLUE_SIZE 2
#define SDL_GL_ALPHA_SIZE 3
#define SDL_GL_DOUBLEBUFFER 5
#define SDL_GL_DEPTH_SIZE 6
#define SDL_GL_STENCIL_SIZE 7
#define SDL_GL_CONTEXT_MAJOR_VERSION 17
#define SDL_GL_CONTEXT_MINOR_VERSION 18
#define SDL_GL_CONTEXT_PROFILE_MASK 21
#define SDL_GL_CONTEXT_PROFILE_ES 0x0004
#define SDL_QUIT            0x100
#define SDL_KEYDOWN         0x300
#define SDL_KEYUP           0x301
#define SDL_TEXTINPUT       0x303
#define SDL_DROPFILE        0x1000
#define KMOD_CTRL           0x00C0
#define SDL_MOUSEMOTION     0x400
#define SDL_MOUSEBUTTONDOWN 0x401
#define SDL_MOUSEBUTTONUP   0x402
#define SDL_CONTROLLERAXISMOTION   0x650
#define SDL_CONTROLLERBUTTONDOWN   0x651
#define SDL_CONTROLLERBUTTONUP     0x652
#define SDL_CONTROLLERDEVICEADDED  0x653
#define SDLK_ESCAPE    27
#define SDLK_RETURN    13
#define SDLK_SPACE     32
#define SDLK_BACKSPACE 8
#define SDLK_TAB       9
#define SDLK_KP_ENTER  0x40000058
#define SDLK_RIGHT     0x4000004F
#define SDLK_LEFT      0x40000050
#define SDLK_DOWN      0x40000051
#define SDLK_UP        0x40000052
/* SDL_GameController layout */
#define PAD_A 0
#define PAD_B 1
#define PAD_X 2
#define PAD_Y 3
#define PAD_BACK 4
#define PAD_START 6
#define PAD_DPAD_UP 11
#define PAD_DPAD_DOWN 12
#define PAD_DPAD_LEFT 13
#define PAD_DPAD_RIGHT 14
#define AXIS_LEFTX 0
#define AXIS_TRIGGERLEFT 4
#define AXIS_TRIGGERRIGHT 5
#define PAD_AXIS(a, neg) (0x100 + (a) * 2 + (neg))

typedef union { u32 type; u8 pad[64]; } sdl_event;
typedef struct {
    s32 freq; u16 format; u8 channels, silence; u16 samples, padding; u32 size;
    void *callback, *userdata;
} sdl_audio_spec;
#define AUDIO_S16LSB 0x8010

static struct {
    int   (*Init)(u32);
    const char *(*GetError)(void);
    int   (*GL_SetAttribute)(int, int);
    void *(*CreateWindow)(const char *, int, int, int, int, u32);
    void *(*GL_CreateContext)(void *);
    int   (*GL_MakeCurrent)(void *, void *);
    int   (*GL_SetSwapInterval)(int);
    void  (*GL_SwapWindow)(void *);
    void *(*GL_GetProcAddress)(const char *);
    int   (*PollEvent)(sdl_event *);
    void  (*Quit)(void);
    int   (*InitSubSystem)(u32);
    u32   (*OpenAudioDevice)(const char *, int, const sdl_audio_spec *, sdl_audio_spec *, int);
    int   (*QueueAudio)(u32, const void *, u32);
    u32   (*GetQueuedAudioSize)(u32);
    void  (*PauseAudioDevice)(u32, int);
    int   (*NumJoysticks)(void);
    int   (*IsGameController)(int);
    void *(*GameControllerOpen)(int);
    int   (*WaitEventTimeout)(sdl_event *, int);
    void  (*SetWindowSize)(void *, int, int);
    void  (*SetWindowPosition)(void *, int, int);
    int   (*SetWindowFullscreen)(void *, u32);
    int   (*SetWindowDisplayMode)(void *, const void *);
    int   (*GetDesktopDisplayMode)(int, void *);
    void  (*GL_GetDrawableSize)(void *, int *, int *);
    char *(*GetClipboardText)(void);
    void  (*free)(void *);
    void  (*StartTextInput)(void);
    void  (*StopTextInput)(void);
    int   (*EventState)(u32, int);
    const char *(*GetKeyName)(s32);
    void  (*DestroyWindow)(void *);
} sdl;
typedef struct { u32 format; int w, h, refresh_rate; void *driverdata; } sdl_display_mode;
static void *sdl_lib;

static void *g_win, *g_ctx;
int g_gl_desktop;
static host_input_fn g_input_cb;

bool host_video_init(int w, int h, int vsync)
{
    void *lib = sdl_lib = dlopen(SDL_LIBNAME, RTLD_NOW | RTLD_LOCAL);
    if (!lib) { LOG("[host] SDL2 not available (%s), running headless\n", dlerror()); return false; }
#define SYM(field, name) if (!(*(void **)&sdl.field = dlsym(lib, name))) { LOG("[host] missing %s\n", name); return false; }
    SYM(Init, "SDL_Init") SYM(GetError, "SDL_GetError") SYM(GL_SetAttribute, "SDL_GL_SetAttribute")
    SYM(CreateWindow, "SDL_CreateWindow") SYM(GL_CreateContext, "SDL_GL_CreateContext")
    SYM(GL_MakeCurrent, "SDL_GL_MakeCurrent") SYM(GL_SetSwapInterval, "SDL_GL_SetSwapInterval")
    SYM(GL_SwapWindow, "SDL_GL_SwapWindow") SYM(GL_GetProcAddress, "SDL_GL_GetProcAddress")
    SYM(PollEvent, "SDL_PollEvent") SYM(Quit, "SDL_Quit")
    SYM(InitSubSystem, "SDL_InitSubSystem") SYM(OpenAudioDevice, "SDL_OpenAudioDevice")
    SYM(QueueAudio, "SDL_QueueAudio") SYM(GetQueuedAudioSize, "SDL_GetQueuedAudioSize")
    SYM(PauseAudioDevice, "SDL_PauseAudioDevice")
    SYM(NumJoysticks, "SDL_NumJoysticks") SYM(IsGameController, "SDL_IsGameController")
    SYM(GameControllerOpen, "SDL_GameControllerOpen")
    SYM(WaitEventTimeout, "SDL_WaitEventTimeout") SYM(SetWindowSize, "SDL_SetWindowSize")
    SYM(SetWindowPosition, "SDL_SetWindowPosition") SYM(SetWindowFullscreen, "SDL_SetWindowFullscreen")
    SYM(SetWindowDisplayMode, "SDL_SetWindowDisplayMode") SYM(GetDesktopDisplayMode, "SDL_GetDesktopDisplayMode")
    SYM(GL_GetDrawableSize, "SDL_GL_GetDrawableSize") SYM(GetClipboardText, "SDL_GetClipboardText")
    SYM(free, "SDL_free") SYM(StartTextInput, "SDL_StartTextInput") SYM(StopTextInput, "SDL_StopTextInput")
    SYM(EventState, "SDL_EventState") SYM(GetKeyName, "SDL_GetKeyName")
    SYM(DestroyWindow, "SDL_DestroyWindow")
#undef SYM
    if (sdl.Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER) != 0) {
        LOG("[host] SDL_Init failed: %s\n", sdl.GetError());
        return false;
    }
    /* OpenGL ES 2 where the driver offers it, else desktop GL 2.1 with the shaders rewritten (glhost.c) */
    const char *want = getenv("RR2_GL");
    for (int attempt = want && !strcmp(want, "desktop"); attempt < 2 && !g_ctx; attempt++) {
        g_gl_desktop = attempt;
        sdl.GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, attempt ? 0 : SDL_GL_CONTEXT_PROFILE_ES);
        sdl.GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
        sdl.GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, attempt ? 1 : 0);
        sdl.GL_SetAttribute(SDL_GL_RED_SIZE, 8);
        sdl.GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
        sdl.GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
        sdl.GL_SetAttribute(SDL_GL_ALPHA_SIZE, 0);
        sdl.GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
        sdl.GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
        sdl.GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        g_win = sdl.CreateWindow("Real Racing 2 (rr2emu)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                 w, h, SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN);
        if (g_win) g_ctx = sdl.GL_CreateContext(g_win);
        if (!g_ctx) {
            LOG("[host] %s context: %s\n", attempt ? "desktop GL" : "GLES2", sdl.GetError());
            if (g_win) sdl.DestroyWindow(g_win);
            g_win = NULL;
            if (want && !strcmp(want, "es")) break;
        }
    }
    if (!g_ctx) return false;
    if (g_gl_desktop) LOG("[host] using desktop OpenGL (GLES2 shaders translated)\n");
    sdl.GL_MakeCurrent(g_win, g_ctx);
    sdl.GL_SetSwapInterval(vsync ? 1 : 0);
    for (int i = 0; i < sdl.NumJoysticks(); i++)
        if (sdl.IsGameController(i) && sdl.GameControllerOpen(i)) LOG("[host] game controller %d opened\n", i);
    sdl.EventState(SDL_DROPFILE, 1);
    sdl.StopTextInput();
    LOG("[host] %dx%d GLES2 window up\n", w, h);
    return true;
}

void host_swap(void) { if (g_win) sdl.GL_SwapWindow(g_win); }

void host_shutdown(void) { if (sdl_lib && sdl.Quit) { g_win = NULL; sdl.Quit(); } }

void host_desktop_size(int *w, int *h)
{
    sdl_display_mode m = { 0 };
    if (g_win && sdl.GetDesktopDisplayMode(0, &m) == 0 && m.w > 0) { *w = m.w; *h = m.h; }
}

/* resize the launcher window for the game; *w,*h come back as the real drawable size */
bool host_video_mode(int *w, int *h, int fullscreen, int vsync)
{
    if (!g_win) return false;
    int dw = 0, dh = 0;
    host_desktop_size(&dw, &dh);
    if (fullscreen) {
        sdl_display_mode m = { 0, *w, *h, 0, NULL };
        int native = *w == dw && *h == dh;
        if (!native) sdl.SetWindowDisplayMode(g_win, &m);
        sdl.SetWindowSize(g_win, *w, *h);
        if (sdl.SetWindowFullscreen(g_win, native ? SDL_WINDOW_FULLSCREEN_DESKTOP : SDL_WINDOW_FULLSCREEN) != 0)
            LOG("[host] fullscreen: %s\n", sdl.GetError());
    } else {
        sdl.SetWindowFullscreen(g_win, 0);
        sdl.SetWindowSize(g_win, *w, *h);
        sdl.SetWindowPosition(g_win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }
    sdl.GL_SetSwapInterval(vsync ? 1 : 0);
    sdl_event e;
    for (int i = 0; i < 20; i++) { while (sdl.PollEvent(&e)) {} sdl.GL_SwapWindow(g_win); }
    sdl.GL_GetDrawableSize(g_win, w, h);
    LOG("[host] video %dx%d%s\n", *w, *h, fullscreen ? " fullscreen" : "");
    return true;
}

void host_text_input(int on) { if (on) sdl.StartTextInput(); else sdl.StopTextInput(); }

/* launcher events: waits up to wait_ms; -1 when the queue is empty */
int host_menu_poll(int wait_ms, menu_event_t *ev)
{
    sdl_event e;
    if (!g_win || !(wait_ms ? sdl.WaitEventTimeout(&e, wait_ms) : sdl.PollEvent(&e))) return -1;
    switch (e.type) {
    case SDL_QUIT: return MENU_QUIT;
    case SDL_KEYDOWN: {
        s32 sym; u16 mod; memcpy(&sym, e.pad + 20, 4); memcpy(&mod, e.pad + 24, 2);
        ev->key = sym;
        if (ev->capture) return e.pad[13] ? MENU_NONE : MENU_RAW_KEY;
        switch (sym) {
        case SDLK_UP: return MENU_UP;
        case SDLK_DOWN: case SDLK_TAB: return MENU_DOWN;
        case SDLK_LEFT: return MENU_LEFT;
        case SDLK_RIGHT: return MENU_RIGHT;
        case SDLK_RETURN: case SDLK_KP_ENTER: return MENU_OK;
        case SDLK_ESCAPE: return MENU_BACK;
        case SDLK_BACKSPACE: return MENU_ERASE;
        }
        if (sym == 'v' && (mod & KMOD_CTRL)) {
            char *t = sdl.GetClipboardText();
            if (!t) return MENU_NONE;
            snprintf(ev->text, sizeof(ev->text), "%s", t);
            sdl.free(t);
            return MENU_PASTE;
        }
        return MENU_NONE; }
    case SDL_TEXTINPUT:
        snprintf(ev->text, sizeof(ev->text), "%.32s", (const char *)e.pad + 12);
        return MENU_CHAR;
    case SDL_DROPFILE: {
        char *f; memcpy(&f, e.pad + 8, sizeof(f));
        if (!f) return MENU_NONE;
        snprintf(ev->text, sizeof(ev->text), "%s", f);
        sdl.free(f);
        return MENU_DROP; }
    case SDL_MOUSEMOTION:
    case SDL_MOUSEBUTTONDOWN:
        memcpy(&ev->x, e.pad + 20, 4); memcpy(&ev->y, e.pad + 24, 4);
        return e.type == SDL_MOUSEMOTION ? MENU_MOVE : MENU_CLICK;
    case SDL_CONTROLLERBUTTONDOWN:
        ev->pad = e.pad[12];
        if (ev->capture) return MENU_RAW_PAD;
        switch (e.pad[12]) {
        case PAD_DPAD_UP: return MENU_UP;
        case PAD_DPAD_DOWN: return MENU_DOWN;
        case PAD_DPAD_LEFT: return MENU_LEFT;
        case PAD_DPAD_RIGHT: return MENU_RIGHT;
        case PAD_A: case PAD_START: return MENU_OK;
        case PAD_B: return MENU_BACK;
        }
        return MENU_NONE;
    case SDL_CONTROLLERAXISMOTION: {
        static s8 was[6];
        s16 v; memcpy(&v, e.pad + 16, 2);
        u8 axis = e.pad[12];
        if (axis >= 6) return MENU_NONE;
        int now = v > 20000 ? 1 : v < -20000 ? -1 : 0, prev = was[axis];
        was[axis] = (s8)now;
        if (!now || now == prev) return MENU_NONE;
        ev->pad = PAD_AXIS(axis, now < 0);
        if (ev->capture) return MENU_RAW_PAD;
        if (axis == 1) return now < 0 ? MENU_UP : MENU_DOWN;
        if (axis == 0) return now < 0 ? MENU_LEFT : MENU_RIGHT;
        return MENU_NONE; }
    case SDL_CONTROLLERDEVICEADDED: {
        s32 idx; memcpy(&idx, e.pad + 8, 4);
        if (sdl.IsGameController(idx)) sdl.GameControllerOpen(idx);
        return MENU_NONE; }
    }
    return MENU_NONE;
}

void *host_gl_proc(const char *name) { return sdl.GL_GetProcAddress ? sdl.GL_GetProcAddress(name) : NULL; }

void host_set_input(host_input_fn fn) { g_input_cb = fn; }

/* ---- in-game input: every action has two keyboard and two controller bindings ---- */

int g_binds[ACT_COUNT][4];
static const int bind_defaults[ACT_COUNT][4] = {
    [ACT_LEFT]   = { SDLK_LEFT, 'a', PAD_AXIS(AXIS_LEFTX, 1), PAD_DPAD_LEFT },
    [ACT_RIGHT]  = { SDLK_RIGHT, 'd', PAD_AXIS(AXIS_LEFTX, 0), PAD_DPAD_RIGHT },
    [ACT_GAS]    = { SDLK_UP, 'w', PAD_AXIS(AXIS_TRIGGERRIGHT, 0), PAD_A },
    [ACT_BRAKE]  = { SDLK_DOWN, 's', PAD_AXIS(AXIS_TRIGGERLEFT, 0), PAD_X },
    [ACT_CAMERA] = { 'c', 0, PAD_Y, -1 },
    [ACT_BACK]   = { SDLK_ESCAPE, SDLK_BACKSPACE, PAD_B, PAD_BACK },
};
const char *const act_names[ACT_COUNT] = { "Steer left", "Steer right", "Gas", "Brake", "Change camera", "Back / pause" };

void host_binds_default(void) { memcpy(g_binds, bind_defaults, sizeof(g_binds)); }

static const char *const pad_button_names[] = {
    "Cross", "Circle", "Square", "Triangle", "Share", "PS", "Options", "L3", "R3", "L1", "R1",
    "D-pad Up", "D-pad Down", "D-pad Left", "D-pad Right", "Mic", "Paddle 1", "Paddle 2", "Paddle 3", "Paddle 4", "Touchpad",
};
static const char *const pad_axis_names[6][2] = {
    { "L-stick Right", "L-stick Left" }, { "L-stick Down", "L-stick Up" },
    { "R-stick Right", "R-stick Left" }, { "R-stick Down", "R-stick Up" }, { "L2", "L2" }, { "R2", "R2" },
};

const char *host_bind_name(int slot, int code)
{
    static char buf[64];
    if (slot < 2) {
        if (!code) return "-";
        const char *n = sdl.GetKeyName ? sdl.GetKeyName(code) : NULL;
        if (n && *n) return n;
        snprintf(buf, sizeof(buf), "Key %d", code);
        return buf;
    }
    if (code < 0) return "-";
    if (code >= 0x100) return pad_axis_names[((code - 0x100) >> 1) % 6][code & 1];
    if (code < (int)(sizeof(pad_button_names) / sizeof(pad_button_names[0]))) return pad_button_names[code];
    snprintf(buf, sizeof(buf), "Button %d", code);
    return buf;
}

static u8 key_held[ACT_COUNT][2], btn_held[ACT_COUNT][2];
static u32 pad_raw, key_nav;
static float axes[6];
host_input_t g_input;

static float axis_amount(int code)
{
    if (code < 0x100) return 0;
    int a = ((code - 0x100) >> 1) % 6;
    float v = axes[a] * (code & 1 ? -1 : 1);
    return v > 0 ? v : 0;
}

static float act_amount(int act)
{
    float v = key_held[act][0] || key_held[act][1] || btn_held[act][0] || btn_held[act][1];
    for (int s = 2; s < 4; s++) { float a = axis_amount(g_binds[act][s]); if (a > v) v = a; }
    return v;
}

static void act_edge(int act)
{
    if (act == ACT_CAMERA) g_input.camera++;
    if (act == ACT_BACK) g_input.back++;
}

/* swap and pump events; false when the user closed the window */
bool host_present(void)
{
    if (!g_win) return true;
    sdl.GL_SwapWindow(g_win);
    sdl_event e;
    static int down;
    while (sdl.PollEvent(&e)) {
        switch (e.type) {
        case SDL_QUIT:
            return false;
        case SDL_KEYDOWN:
        case SDL_KEYUP: {
            s32 sym; memcpy(&sym, e.pad + 20, 4);
            int d = e.type == SDL_KEYDOWN;
            if (d && e.pad[13]) break;                          /* auto-repeat */
            {
                u32 bit = sym == SDLK_UP ? 1u << 11 : sym == SDLK_DOWN ? 1u << 12 : sym == SDLK_LEFT ? 1u << 13 :
                          sym == SDLK_RIGHT ? 1u << 14 : (sym == 13 || sym == SDLK_KP_ENTER || sym == ' ') ? 1u : sym == 8 ? 2u : 0;
                if (d) key_nav |= bit; else key_nav &= ~bit;
            }
            for (int a = 0; a < ACT_COUNT; a++)
                for (int s = 0; s < 2; s++)
                    if (g_binds[a][s] && g_binds[a][s] == sym) { if (d && !key_held[a][s]) act_edge(a); key_held[a][s] = d; }
            break; }
        case SDL_CONTROLLERAXISMOTION: {
            s16 v; memcpy(&v, e.pad + 16, 2);
            u8 axis = e.pad[12];
            if (axis >= 6) break;
            float old[ACT_COUNT];
            for (int a = 0; a < ACT_COUNT; a++) old[a] = act_amount(a);
            axes[axis] = v / 32767.0f;
            for (int a = 0; a < ACT_COUNT; a++) if (old[a] < 0.5f && act_amount(a) >= 0.5f) act_edge(a);
            break; }
        case SDL_CONTROLLERBUTTONDOWN:
        case SDL_CONTROLLERBUTTONUP: {
            int d = e.type == SDL_CONTROLLERBUTTONDOWN;
            if (e.pad[12] < 32) { if (d) pad_raw |= 1u << e.pad[12]; else pad_raw &= ~(1u << e.pad[12]); }
            for (int a = 0; a < ACT_COUNT; a++)
                for (int s = 2; s < 4; s++)
                    if (g_binds[a][s] == e.pad[12]) { if (d && !btn_held[a][s - 2]) act_edge(a); btn_held[a][s - 2] = d; }
            break; }
        case SDL_CONTROLLERDEVICEADDED: {
            s32 idx; memcpy(&idx, e.pad + 8, 4);
            if (sdl.IsGameController(idx)) sdl.GameControllerOpen(idx);
            break; }
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
        case SDL_MOUSEMOTION: {
            s32 x, y; memcpy(&x, e.pad + 20, 4); memcpy(&y, e.pad + 24, 4);
            int action = e.type == SDL_MOUSEBUTTONDOWN ? 0 : e.type == SDL_MOUSEBUTTONUP ? 1 : 2;
            if (action == 0) down = 1;
            if (action == 1) down = 0;
            if (g_input_cb && (action != 2 || down)) g_input_cb(action, x, y);
            break; }
        }
    }
    /* steering: analog bindings with a deadzone, digital ones at full lock */
    float l = act_amount(ACT_LEFT), r = act_amount(ACT_RIGHT);
    if (l < 0.12f) l = 0;
    if (r < 0.12f) r = 0;
    float st = r - l;
    g_input.steer_target = st > 1 ? 1 : st < -1 ? -1 : st;
    g_input.gasv = act_amount(ACT_GAS);
    g_input.brakev = act_amount(ACT_BRAKE);
    g_input.gas = g_input.gasv > 0.3f;
    g_input.brake = g_input.brakev > 0.3f;
    g_input.pad_btn = pad_raw | key_nav;
    return true;
}

/* ---- audio: AudioTrack.write semantics over SDL's queue (blocks when full) ---- */

static u32 g_adev;
static u32 g_arate = 44100, g_abytes_per_sec = 44100 * 4, g_alimit;

bool host_audio_open(int rate, int channels)
{
    g_arate = (u32)rate;
    g_abytes_per_sec = (u32)rate * 2 * (u32)channels;
    g_alimit = g_abytes_per_sec / 20;             /* ~50 ms queued before write blocks: low latency, no gaps */
    if (!sdl_lib || sdl.InitSubSystem(SDL_INIT_AUDIO) != 0) return false;
    sdl_audio_spec want = { .freq = rate, .format = AUDIO_S16LSB, .channels = (u8)channels, .samples = 1024 }, have;
    g_adev = sdl.OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!g_adev) { LOG("[host] audio: %s\n", sdl.GetError()); return false; }
    sdl.PauseAudioDevice(g_adev, 0);
    LOG("[host] audio %d Hz x%d\n", rate, channels);
    return true;
}

void host_audio_write(const void *pcm, u32 bytes)
{
    if (!g_adev) {                                 /* headless: pace like a real device would */
        usleep((useconds_t)((u64)bytes * 1000000 / g_abytes_per_sec));
        return;
    }
    static int dbg = -1;
    static u32 underruns, writes, minq = ~0u;
    static u64 bytes_sec, t_last;
    if (dbg < 0) dbg = getenv("RR2_AUDIO_DEBUG") != NULL;
    u32 q = sdl.GetQueuedAudioSize(g_adev);
    if (dbg) {
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        u64 now = (u64)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
        if (q == 0) underruns++;
        if (q < minq) minq = q;
        writes++; bytes_sec += bytes;
        if (now - t_last >= 1000) {
            LOG("[audio] %u writes/s, %.0f%% of realtime, min queue %.1f ms, underruns %u\n", writes,
                100.0 * bytes_sec / g_abytes_per_sec * 1000.0 / (double)(now - t_last),
                minq * 1000.0 / g_abytes_per_sec, underruns);
            writes = 0; bytes_sec = 0; minq = ~0u; underruns = 0; t_last = now;
        }
    }
    while (q > g_alimit) { usleep(2000); q = sdl.GetQueuedAudioSize(g_adev); }
    sdl.QueueAudio(g_adev, pcm, bytes);
}
