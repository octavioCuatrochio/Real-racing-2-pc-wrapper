/*
 * jni_rr3.inc - the Java side of Real Racing 3 (com.firemint.realracing.*,
 * cloudcell, Nimble) as far as the native code calls it. Included by jni.c;
 * consulted before the generic table when G.game == 3.
 *
 * The emulated device: an Adreno phone on Android 7.1.2 (API 25), landscape,
 * offline (every network query answers "no connection").
 */

#define RR3_PKG   "com.ea.games.r3_row"
#define RR3_EXT   "/sdcard/Android/data/" RR3_PKG "/files"
#define RR3_APK   "/data/app/" RR3_PKG "-1/base.apk"
#define PLAT      "com/firemint/realracing/Platform"

static u64 j_null(cpu_t *c, u32 self, jargs_t *a)  { return 0; }
static u64 j_false(cpu_t *c, u32 self, jargs_t *a) { return 0; }
static u64 j_long(cpu_t *c, u32 self, jargs_t *a)  { return ((u64)cur_meth->ent->iv) << 20; }   /* iv in MB */
static u64 j_float1(cpu_t *c, u32 self, jargs_t *a) { return 0x3F800000u; }
static u64 j_dpi_i(cpu_t *c, u32 self, jargs_t *a) { return (u32)(G.dpi > 0 ? G.dpi : 320); }
static u64 j_install_time(cpu_t *c, u32 self, jargs_t *a) { return 1600000000000ULL; }
static u64 j_rotation(cpu_t *c, u32 self, jargs_t *a) { return 1; }                 /* ROTATION_90: landscape */
static u64 j_log_message(cpu_t *c, u32 self, jargs_t *a)
{
    u32 t = ja_i32(a), m = ja_i32(a);
    LOG("[rr3] message box: %s / %s\n", t ? jstr(t) : "", m ? jstr(m) : "");
    return 0;
}

/* jni_rr3_text.inc */
static u64 j_font_init(cpu_t *c, u32 self, jargs_t *a);
static u64 j_font_load_glyph(cpu_t *c, u32 self, jargs_t *a);
static u64 j_font_load_bitmap(cpu_t *c, u32 self, jargs_t *a);
static u64 j_gv_init(cpu_t *c, u32 self, jargs_t *a);
static u64 j_gv_init_para(cpu_t *c, u32 self, jargs_t *a);
static u64 j_gv_create_texture(cpu_t *c, u32 self, jargs_t *a);
static u64 j_gv_render_to_texture(cpu_t *c, u32 self, jargs_t *a);

static const jimpl_ent rr3_impls[] = {
    { "com/firemint/realracing/Font", "init", NULL, j_font_init },
    { "com/firemint/realracing/Font", "loadGlyph", NULL, j_font_load_glyph },
    { "com/firemint/realracing/Font", "loadBitmap", NULL, j_font_load_bitmap },
    { "com/firemint/realracing/GlyphVector", "init", NULL, j_gv_init },
    { "com/firemint/realracing/GlyphVector", "initWithParagraph", NULL, j_gv_init_para },
    { "com/firemint/realracing/GlyphVector", "createTexture", NULL, j_gv_create_texture },
    { "com/firemint/realracing/GlyphVector", "renderToTexture", NULL, j_gv_render_to_texture },
    { PLAT, "getDeviceUID",          NULL, j_str, "0123456789abcdef" },
    { PLAT, "getModelID",            NULL, j_str, "Pixel 2" },
    { PLAT, "getModelName",          NULL, j_str, "Pixel 2" },
    { PLAT, "GetDeviceHardware",     NULL, j_str, "walleye" },
    { PLAT, "getBuildDevice",        NULL, j_str, "walleye" },
    { PLAT, "getBuildManufacturer",  NULL, j_str, "Google" },
    { PLAT, "getScreenDensityName",  NULL, j_str, "xhdpi" },
    { PLAT, "getScreenSizeName",     NULL, j_str, "normal" },
    { PLAT, "getSupportedAbis",      NULL, j_str, "armeabi-v7a,armeabi" },
    { PLAT, "getCpuCount",           NULL, j_int, NULL, 4 },
    { PLAT, "getCpuSpeed",           NULL, j_int, NULL, 2350000 },
    { PLAT, "getCpuName",            NULL, j_str, "Qualcomm Technologies, Inc MSM8998" },
    { PLAT, "getAppName",            NULL, j_str, "Real Racing 3" },
    { PLAT, "getAppVersion",         NULL, j_str, "7.6.0" },
    { PLAT, "getAppPath",            NULL, j_str, RR3_APK },
    { PLAT, "getAppInstallTime",     NULL, j_install_time },
    { PLAT, "getApiLevel",           NULL, j_int, NULL, 25 },
    { PLAT, "getSystemMediaVolume",  NULL, j_float1 },
    { PLAT, "getOsVersion",          NULL, j_str, "7.1.2" },
    { PLAT, "getStringExtra",        NULL, j_null },
    { PLAT, "getExternalStorageState", NULL, j_int, NULL, 2 },
    { PLAT, "getExternalStorageDir", NULL, j_str, RR3_EXT },
    { PLAT, "getLegacyExternalStorageDirPath", NULL, j_str, "/sdcard/Android/data/" RR3_PKG },
    { PLAT, "getScreenRotation",     NULL, j_rotation },
    { PLAT, "getScreenWidth",        NULL, j_width },
    { PLAT, "getScreenHeight",       NULL, j_height },
    { PLAT, "getScreenDPI",          NULL, j_dpi_i },
    { PLAT, "getTotalMemory",        NULL, j_int, NULL, 3900000 },          /* kB, like /proc/meminfo */
    { PLAT, "getAppMemoryUsage",     NULL, j_long, NULL, 300 },
    { PLAT, "getAppMemoryFree",      NULL, j_long, NULL, 1500 },
    { PLAT, "getLocale",             NULL, j_str, "en_US" },
    { PLAT, "getCountry",            NULL, j_str, "US" },
    { PLAT, "isNetworkAvailable",    NULL, j_false },
    { PLAT, "isMobileDataAvailable", NULL, j_false },
    { PLAT, "GetWifiSSID",           NULL, j_str, "" },
    { PLAT, "getMobileDataType",     NULL, j_str, "" },
    { PLAT, "getCarrier",            NULL, j_str, "" },
    { PLAT, "getCarrierName",        NULL, j_str, "" },
    { PLAT, "GetAudioFramesPerBuffer", NULL, j_int, NULL, 256 },
    { PLAT, "GetAudioSampleRate",    NULL, j_int, NULL, 44100 },
    { PLAT, "IsInForeground",        NULL, j_true },
    { PLAT, "deleteDirectory",       NULL, j_false },
    { PLAT, "loadTextureFromMemory", NULL, j_null },
    { PLAT, "showMessage",           NULL, j_log_message },
    { PLAT, "getCloudcellEnvironment", NULL, j_str, "LIVE" },
    { "com/firemint/realracing/MainActivity", "getApiLevel", NULL, j_int, NULL, 25 },
    { "com/firemint/realracing/MainActivity", "getScreenRotation", NULL, j_rotation },
    { "com/firemint/realracing/MainActivity", "getScreenOrientation", NULL, j_int, NULL, 2 },
    { NULL, "getPackageName",        NULL, j_str, RR3_PKG },
};
