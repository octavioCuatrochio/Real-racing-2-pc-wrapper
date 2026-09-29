/*
 * rr3.c - Real Racing 3 (v7.6.0, armeabi-v7a) boot and frame loop.
 *
 * Mirrors what the APK's Java side does (com.firemint.realracing.MainActivity):
 * System.loadLibrary(c++_shared, Nimble, fmodex, RealRacing3) -> JNI_OnLoad,
 * onCreate -> onCreateJNI, onStart -> onStartJNI, then the GLSurfaceView
 * thread: onViewCreatedJNI, onViewChangedJNI(w, h, orientation, rotation),
 * onResumeJNI, onWindowFocusChangedJNI(true) and onViewRenderJNI per frame.
 * The UI and GL threads are the same host thread here.
 *
 * Data layout (G.assets_dir): sdcard/ = /sdcard/Android/data/com.ea.games.r3_row,
 * internal/ = /data/data/com.ea.games.r3_row, apk/ = the installed APK and libs.
 */
#include "emu.h"
#include <time.h>

extern long g_frame;
void jni_rr3_setup(void);

static u32 fn(const char *name)
{
    u32 a = elf_lookup(name);
    if (!a) LOG("[rr3] missing native %s\n", name);
    return a;
}

static void jcall(cpu_t *c, u32 f, int n, const u32 *extra)
{
    if (!f) return;
    u32 a[8] = { jni_env_ptr(), jni_activity() };
    for (int i = 0; i < n && i < 6; i++) a[2 + i] = extra[i];
    jni_push_frame();
    emu_call(c, f, 2 + n, a);
    jni_pop_frame();
}

/* ---- input: mouse -> touch, keyboard / gamepad -> one virtual Android controller ---- */
static cpu_t *g_ui;
static u32 f_touch_begin, f_touch_move, f_touch_end, f_key_down, f_key_up, f_pad_btn, f_pad_axis, g_cm;

static inline u32 fbits(float f) { u32 b; memcpy(&b, &f, 4); return b; }

static void on_touch3(int action, int x, int y)
{
    if (!g_ui) return;
    u32 a[4] = { 0, fbits((float)x), fbits((float)y), 1 };
    if (action == 0) jcall(g_ui, f_touch_begin, 3, a);
    else if (action == 2) jcall(g_ui, f_touch_move, 3, a);
    else jcall(g_ui, f_touch_end, 4, a);
}

static void pad_call(cpu_t *c, u32 f, int n, const u32 *extra)
{
    if (!f) return;
    u32 a[8] = { jni_env_ptr(), g_cm };
    for (int i = 0; i < n; i++) a[2 + i] = extra[i];
    jni_push_frame();
    emu_call(c, f, 2 + n, a);
    jni_pop_frame();
}
static void pad_axis(cpu_t *c, int axis, float v)
{
    static float last[8] = { 9, 9, 9, 9, 9, 9, 9, 9 };
    if (last[axis] == v) return;
    last[axis] = v;
    u32 a[3] = { 1, fbits(v), (u32)axis };
    pad_call(c, f_pad_axis, 3, a);
}
static void pad_button(cpu_t *c, int btn, int down)
{
    u32 a[3] = { 1, (u32)down, (u32)btn };
    pad_call(c, f_pad_btn, 3, a);
}

/* SDL game-controller button index -> RR3 ControllerButtons */
static const s8 sdl_to_rr3[15] = { 0, 1, 2, 3, 12, -1, 13, -1, -1, 4, 5, 10, 11, 8, 9 };

static void rr3_input(cpu_t *c)
{
    static float steer;
    static u32 prev_btn;
    static int backs, cams;
    float d = g_input.steer_target - steer;
    steer += d > 0.12f ? 0.12f : d < -0.12f ? -0.12f : d;
    pad_axis(c, 0, steer);                                  /* AXIS_LTHUMB_X */
    pad_axis(c, 5, g_input.gasv > 1 ? 1 : g_input.gasv);   /* AXIS_RTRIGGER */
    pad_axis(c, 4, g_input.brakev > 1 ? 1 : g_input.brakev);   /* AXIS_LTRIGGER */
    u32 b = g_input.pad_btn, ch = b ^ prev_btn;
    for (int i = 0; i < 15; i++)
        if (((ch >> i) & 1) && sdl_to_rr3[i] >= 0) pad_button(c, sdl_to_rr3[i], (b >> i) & 1);
    prev_btn = b;
    while (cams < g_input.camera) { pad_button(c, 3, 1); pad_button(c, 3, 0); cams++; }   /* camera: Y */
    while (backs < g_input.back) {                          /* Android KEYCODE_BACK */
        u32 k = 4;
        jcall(c, f_key_down, 1, &k);
        jcall(c, f_key_up, 1, &k);
        backs++;
    }
}

int rr3_main(const char *so_path)
{
    G.game = 3;
    if (!G.save_dir) G.save_dir = "./save_rr3";
    LOG("[rr3] loading %s (data %s, saves %s)\n", so_path, G.assets_dir, G.save_dir);
    if (elf_load(&G, so_path) != 0) fatal("failed to load %s", so_path);
    if (getenv("RR2_STUBS")) { extern void hle_dump_stubs(void); hle_dump_stubs(); }
    cpu_icache_reset();
    thumb_it_reset();
    if (getenv("RR2_NOJIT")) g_jit = 0;
    jit_reset();

    cpu_t *c = emu_new_cpu();
    c->r[13] = GUEST_STACK_TOP & ~7u;
    snprintf(c->name, sizeof(c->name), "main");

    jni_rr3_setup();
    jni_init();
    /* System.loadLibrary order from MainActivity's static initializer: constructors, then JNI_OnLoad */
    static const char *const order[] = { "libc++_shared.so", "libNimble.so", "libfmodex.so", "libRealRacing3.so" };
    for (unsigned i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
        LOG("[rr3] loadLibrary %s\n", order[i]);
        elf_run_init_lib(c, order[i]);
        u32 onload = elf_lookup_in(order[i], "JNI_OnLoad");
        if (!onload) continue;
        u32 args[2] = { jni_vm_ptr(), 0 };
        u32 ver = emu_call(c, onload, 2, args);
        LOG("[rr3] %s JNI_OnLoad -> 0x%x\n", order[i], ver);
    }

    const u32 orient = 2, rot = 1;                 /* landscape */
    LOG("[rr3] onCreateJNI\n");          jcall(c, fn("Java_com_firemint_realracing_MainActivity_onCreateJNI"), 0, NULL);
    LOG("[rr3] onStartJNI\n");           jcall(c, fn("Java_com_firemint_realracing_MainActivity_onStartJNI"), 0, NULL);
    LOG("[rr3] onViewCreatedJNI\n");     jcall(c, fn("Java_com_firemint_realracing_MainActivity_onViewCreatedJNI"), 0, NULL);
    u32 vc[4] = { (u32)G.width, (u32)G.height, orient, rot };
    LOG("[rr3] onViewChangedJNI %dx%d\n", G.width, G.height);
    jcall(c, fn("Java_com_firemint_realracing_MainActivity_onViewChangedJNI"), 4, vc);
    LOG("[rr3] onResumeJNI\n");          jcall(c, fn("Java_com_firemint_realracing_MainActivity_onResumeJNI"), 0, NULL);
    u32 one = 1;
    jcall(c, fn("Java_com_firemint_realracing_MainActivity_onWindowFocusChangedJNI"), 1, &one);

    g_ui = c;
    f_touch_begin = fn("Java_com_firemint_realracing_MainActivity_onTouchBeginJNI");
    f_touch_move = fn("Java_com_firemint_realracing_MainActivity_onTouchMoveJNI");
    f_touch_end = fn("Java_com_firemint_realracing_MainActivity_onTouchEndJNI");
    f_key_down = fn("Java_com_firemint_realracing_MainActivity_onKeyPressed");
    f_key_up = fn("Java_com_firemint_realracing_MainActivity_onKeyReleased");
    f_pad_btn = fn("Java_com_firemint_realracing_ControllerManager_SetButtonValueJNI");
    f_pad_axis = fn("Java_com_firemint_realracing_ControllerManager_SetJoystickValueJNI");
    g_cm = jni_object("com/firemint/realracing/ControllerManager");
    host_set_input(on_touch3);
    {
        u32 a[4] = { jni_string("Xbox Wireless Controller"), jni_string("rr2emu-virtual-pad"), 1, 0x3FF };
        pad_call(c, fn("Java_com_firemint_realracing_ControllerManager_ControllerConnectedJNI"), 4, a);
    }

    u32 render = fn("Java_com_firemint_realracing_MainActivity_onViewRenderJNI");
    u32 ra[2] = { orient, rot };
    LOG("[rr3] render loop (max_frames=%d)\n", G.max_frames);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    u64 i0 = c->insn_count;
    long frames = 0, prof_frame = 0;
    /* timed scripting: RR2_T_PROF / RR2_T_STOP seconds, RR2_T_SHOT png at stop, RR2_TAPS "sec:x,y;..." */
    double t_prof = getenv("RR2_T_PROF") ? atof(getenv("RR2_T_PROF")) : g_do_prof ? 0 : -1;
    double t_stop = getenv("RR2_T_STOP") ? atof(getenv("RR2_T_STOP")) : 0;
    const char *taps = getenv("RR2_TAPS");
    struct timespec tb;
    clock_gettime(CLOCK_MONOTONIC, &tb);
    for (; render && (!G.max_frames || frames < G.max_frames); frames++) {
        g_frame = frames;
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double now = (t1.tv_sec - tb.tv_sec) + (t1.tv_nsec - tb.tv_nsec) / 1e9;
        if (g_do_prof && t_prof >= 0 && now >= t_prof) { prof_start(); prof_frame = frames; t_prof = -1; }
        bool stop = t_stop && now >= t_stop;
        while (taps && *taps) {
            double at; int x, y, nch = 0;
            if (sscanf(taps, "%lf:%d,%d%n", &at, &x, &y, &nch) != 3) { taps = NULL; break; }
            if (now < at) break;
            LOG("[rr3] tap %d,%d at %.1fs\n", x, y, now);
            on_touch3(0, x, y); on_touch3(1, x, y);
            taps += nch; if (*taps == ';') taps++;
        }
        rr3_input(c);
        jcall(c, render, 2, ra);
        if (stop) {
            if (getenv("RR2_T_SHOT")) glhost_screenshot(getenv("RR2_T_SHOT"), G.width, G.height);
            break;
        }
        extern void frame_screenshots(long frame);
        frame_screenshots(frames);
        if (!host_present()) break;
        if (frames % 120 == 119) {
            clock_gettime(CLOCK_MONOTONIC, &t1);
            double d = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
            LOG("[perf] frame %ld: %.1f fps, main %.1f MIPS\n", frames, 120 / d, (c->insn_count - i0) / (d * 1e6));
            t0 = t1; i0 = c->insn_count;
        }
    }
    if (g_do_prof) { prof_report(); glhost_report(frames - prof_frame); }
    LOG("[rr3] onPauseJNI / onStopJNI\n");
    jcall(c, fn("Java_com_firemint_realracing_MainActivity_onPauseJNI"), 0, NULL);
    jcall(c, fn("Java_com_firemint_realracing_MainActivity_onStopJNI"), 0, NULL);
    host_shutdown();
    return 0;
}
