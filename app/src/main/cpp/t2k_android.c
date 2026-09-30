/* Tempest 2000 for Android - native host for the Virtual Jaguar libretro core (linked in statically).
 *
 * Runs the original t2000.abs (an APK asset).  Same design as the Apple TV app:
 *   - exactly one emulated frame per screen refresh, driven by Java's Choreographer (tick());  the Jaguar core runs
 *     at 60.05 Hz, the game is stepped at a steady 60 fps (time-based, so 90 / 120 Hz displays run every 2nd / 1.5th refresh);
 *   - audio through AAudio; the core's samples go into a lock-free ring and one sample per frame is added or dropped when
 *     the ring drifts from its target, so the audio and display clocks never fight;
 *   - the picture is written straight into an RGB565 window buffer, pre-scaled SCALE x with nearest neighbour so the
 *     compositor's smooth scaling to the screen keeps the pixels sharp;
 *   - the core's fast blitter and idle-loop skip are switched on (as on the Apple TV).
 * Input arrives as a libretro joypad bit mask from Java (touch controls, keyboard or game controller).
 */
#include <jni.h>
#include <android/log.h>
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <aaudio/AAudio.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include "libretro.h"

#define TAG "T2K"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#define SCALE        4                  /* nearest-neighbour pre-scale of the picture */
#define AUDIO_RATE   48000
#define RING_FRAMES  (1 << 15)          /* stereo frames */
#define AUDIO_TARGET_MIN 2400           /* about 3 game frames of sound queued... */
#define AUDIO_TARGET_MAX 9600           /* ...growing up to 12 frames if the device keeps stalling */
#define ASSET_NAME   "t2000.abs"
#define MIN_PRESS    3                  /* frames: even a very quick tap lasts long enough for the game to notice it */

static struct {
    /* core / video */
    enum retro_pixel_format pixfmt;
    unsigned w, h; double aspect, fps;
    const void *frame; size_t pitch; int ready;
    _Atomic uint32_t pad;               /* buttons held right now (from Java) */
    _Atomic uint32_t pad_latch;         /* buttons pressed at any moment since the last frame: a tap shorter than a frame still counts */
    uint32_t frame_pad;                 /* what the game sees this frame */
    int hold[16];                       /* frames each button still has to be held (minimum press length) */
    int loaded;
    uint8_t *rom; size_t rom_len;
    char save_dir[512], eeprom_path[600];
    uint8_t last_eeprom[4096]; size_t last_eeprom_len;
    /* frame hand-over to the drawing thread: three buffers, the emulation thread fills one, the drawing thread shows the newest */
    pthread_mutex_t pm; pthread_cond_t pc; pthread_t pthr; int pthr_on, pquit;
    struct { uint8_t *data; size_t cap, bytes; unsigned w, h, pitch; int fmt; } fb[3];
    int latest, busy; int dropped;
    /* window */
    pthread_mutex_t wlock; ANativeWindow *win; int win_w, win_h;
    /* run control */
    pthread_mutex_t run;                /* serialises tick() / start() / stop() */
    int running; double acc; int64_t last_ns;
    /* audio */
    AAudioStream *stream; volatile int audio_err; int audio_rate; int audio_wanted; int audio_target; uint32_t seen_underruns;
    /* statistics */
    int frames_run; double ms_sum, ms_max, gap_max, run_sum, present_sum, run_max, present_max, tick_gap_max; int missed_ticks; int64_t last_frame_ns; int late; uint32_t last_underruns;
} g = { .pixfmt = RETRO_PIXEL_FORMAT_RGB565, .w = 320, .h = 240, .aspect = 4.0 / 3.0, .fps = 60.0,
        .wlock = PTHREAD_MUTEX_INITIALIZER, .run = PTHREAD_MUTEX_INITIALIZER,
        .pm = PTHREAD_MUTEX_INITIALIZER, .pc = PTHREAD_COND_INITIALIZER, .latest = -1, .busy = -1, .audio_target = AUDIO_TARGET_MIN };

static int64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec; }

/* ------------------------------------------------------------------ audio ring */
static int16_t ring[RING_FRAMES * 2];
static _Atomic uint32_t rd, wr, underruns;
static uint32_t ring_fill(void) { return atomic_load(&wr) - atomic_load(&rd); }

static void ring_push(const int16_t *d, size_t n)
{
    uint32_t w = atomic_load(&wr), r = atomic_load(&rd);
    for (size_t i = 0; i < n && w - r < RING_FRAMES; i++) {
        ring[(w & (RING_FRAMES - 1)) * 2] = d[i * 2]; ring[(w & (RING_FRAMES - 1)) * 2 + 1] = d[i * 2 + 1]; w++;
    }
    atomic_store(&wr, w);
}

static size_t audio_batch_cb(const int16_t *d, size_t frames)
{
    if (!g.audio_wanted || !frames) return frames;
    int fill = (int)ring_fill();
    if (!g.stream && fill > g.audio_target) {         /* nobody is playing yet: keep only the newest frames */
        atomic_store(&rd, atomic_load(&wr) - g.audio_target); fill = g.audio_target;
    }
    /* Drift control: at most one sample added or dropped per frame keeps the ring near its target. */
    if (fill > g.audio_target + 200) frames--;                      /* too much queued: drop one */
    ring_push(d, frames);
    if (fill < g.audio_target - 200) ring_push(d + (frames - 1) * 2, 1);   /* too little: repeat the last one */
    return frames;
}
static void audio_one_cb(int16_t l, int16_t r) { int16_t s[2] = { l, r }; audio_batch_cb(s, 1); }

static aaudio_data_callback_result_t audio_data_cb(AAudioStream *s, void *ud, void *out, int32_t n)
{
    int16_t *o = out; uint32_t r = atomic_load(&rd), avail = atomic_load(&wr) - r;
    for (int32_t i = 0; i < n; i++) {
        if ((uint32_t)i < avail) { o[i * 2] = ring[(r & (RING_FRAMES - 1)) * 2]; o[i * 2 + 1] = ring[(r & (RING_FRAMES - 1)) * 2 + 1]; r++; }
        else { o[i * 2] = o[i * 2 + 1] = 0; }
    }
    if (avail < (uint32_t)n) atomic_fetch_add(&underruns, 1);
    atomic_store(&rd, r);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}
static void audio_error_cb(AAudioStream *s, void *ud, aaudio_result_t e) { g.audio_err = 1; }

static void audio_close(void)
{
    if (!g.stream) return;
    AAudioStream_requestStop(g.stream); AAudioStream_close(g.stream); g.stream = NULL;
}

static void audio_open(void)
{
    AAudioStreamBuilder *b; aaudio_result_t r;
    if (g.stream) return;
    g.audio_err = 0;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) { LOGW("AAudio unavailable: running without sound"); return; }
    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setSampleRate(b, AUDIO_RATE);
    AAudioStreamBuilder_setChannelCount(b, 2);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setDataCallback(b, audio_data_cb, NULL);
    AAudioStreamBuilder_setErrorCallback(b, audio_error_cb, NULL);
    r = AAudioStreamBuilder_openStream(b, &g.stream);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK) { LOGW("cannot open audio stream: %s (running without sound)", AAudio_convertResultToText(r)); g.stream = NULL; return; }
    g.audio_rate = AAudioStream_getSampleRate(g.stream);
    AAudioStream_setBufferSizeInFrames(g.stream, AAudioStream_getFramesPerBurst(g.stream) * 2);
    { uint32_t w = atomic_load(&wr); if (w - atomic_load(&rd) > (uint32_t)g.audio_target) atomic_store(&rd, w - g.audio_target); }   /* start with exactly the target queued */
    r = AAudioStream_requestStart(g.stream);
    if (r != AAUDIO_OK) { LOGW("cannot start audio: %s", AAudio_convertResultToText(r)); audio_close(); return; }
    LOGI("audio: %d Hz, burst %d frames, buffer %d frames%s", g.audio_rate, AAudioStream_getFramesPerBurst(g.stream),
         AAudioStream_getBufferSizeInFrames(g.stream), g.audio_rate == AUDIO_RATE ? "" : "  (device rate differs: pitch will be off)");
}

/* ------------------------------------------------------------------ libretro callbacks */
static void RETRO_CALLCONV log_cb(enum retro_log_level level, const char *fmt, ...)
{
    if (level < RETRO_LOG_WARN) return;
    va_list ap; va_start(ap, fmt); __android_log_vprint(ANDROID_LOG_WARN, "T2K.core", fmt, ap); va_end(ap);
}

static bool RETRO_CALLCONV env_cb(unsigned cmd, void *data)
{
    switch (cmd & 0xffff) {
    case RETRO_ENVIRONMENT_GET_CAN_DUPE: *(bool *)data = true; return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
        enum retro_pixel_format f = *(enum retro_pixel_format *)data;
        if (f == RETRO_PIXEL_FORMAT_XRGB8888 || f == RETRO_PIXEL_FORMAT_RGB565) { g.pixfmt = f; return true; }
        return false; }
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY: *(const char **)data = g.save_dir; return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: ((struct retro_log_callback *)data)->log = log_cb; return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {          /* the core's "Fast" blitter and RISC idle-loop skip: ~2x faster overall */
        struct retro_variable *v = data;
        if (v->key && !strcmp(v->key, "virtualjaguar_usefastblitter")) { v->value = "enabled"; return true; }
        if (v->key && !strcmp(v->key, "virtualjaguar_risc_idle_skip")) { v->value = "enabled"; return true; }
        return false; }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE: *(bool *)data = false; return true;
    case RETRO_ENVIRONMENT_SET_VARIABLES:
    case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
    case RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS:
    case RETRO_ENVIRONMENT_SET_ROTATION:
    case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS: return true;
    case RETRO_ENVIRONMENT_GET_LANGUAGE: *(unsigned *)data = RETRO_LANGUAGE_ENGLISH; return true;
    case RETRO_ENVIRONMENT_SET_GEOMETRY: {
        const struct retro_game_geometry *geo = data;
        g.aspect = geo->aspect_ratio > 0 ? geo->aspect_ratio : (double)geo->base_width / geo->base_height; return true; }
    default: return false;
    }
}

static void RETRO_CALLCONV video_cb(const void *d, unsigned w, unsigned h, size_t pitch)
{
    if (!d) return;                                 /* duplicate frame */
    g.frame = d; g.pitch = pitch; g.w = w; g.h = h; g.ready = 1;
}
static void RETRO_CALLCONV poll_cb(void) { }
static int16_t RETRO_CALLCONV input_cb(unsigned port, unsigned dev, unsigned idx, unsigned id)
{
    if (port || (dev & RETRO_DEVICE_MASK) != RETRO_DEVICE_JOYPAD) return 0;
    uint32_t m = g.frame_pad;
    if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return (int16_t)(m & 0xffff);
    return (m >> id) & 1;
}

/* ------------------------------------------------------------------ high scores (cartridge EEPROM) */
static void eeprom_load(void)
{
    size_t n = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM); uint8_t *p = retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
    FILE *f = fopen(g.eeprom_path, "rb"); if (!f || !n || !p) { if (f) fclose(f); return; }
    size_t got = fread(p, 1, n, f); fclose(f); (void)got;
    g.last_eeprom_len = n < sizeof g.last_eeprom ? n : sizeof g.last_eeprom; memcpy(g.last_eeprom, p, g.last_eeprom_len);
}
static void eeprom_save(void)
{
    size_t n = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM); uint8_t *p = retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
    if (!n || !p) return;
    size_t k = n < sizeof g.last_eeprom ? n : sizeof g.last_eeprom;
    if (k == g.last_eeprom_len && !memcmp(g.last_eeprom, p, k)) return;         /* unchanged */
    FILE *f = fopen(g.eeprom_path, "wb"); if (!f) return;
    fwrite(p, 1, n, f); fclose(f);
    g.last_eeprom_len = k; memcpy(g.last_eeprom, p, k);
}

/* ------------------------------------------------------------------ video */
static void set_geometry_locked(unsigned fw, unsigned fh)
{
    if (!g.win) return;
    g.win_w = (int)fw * SCALE; g.win_h = (int)fh * SCALE;
    ANativeWindow_setBuffersGeometry(g.win, g.win_w, g.win_h, WINDOW_FORMAT_RGBA_8888);
}

/* core pixel -> window pixel (RGBA_8888 in memory = R,G,B,A bytes) */
static inline uint32_t px_from_xrgb8888(uint32_t p) { return 0xff000000u | ((p & 0xffu) << 16) | (p & 0xff00u) | ((p >> 16) & 0xffu); }
static inline uint32_t px_from_rgb565(uint16_t p)
{
    uint32_t r = (p >> 11) & 31, gg = (p >> 5) & 63, bl = p & 31;
    return 0xff000000u | (((bl << 3) | (bl >> 2)) << 16) | (((gg << 2) | (gg >> 4)) << 8) | ((r << 3) | (r >> 2));
}

/* Draws one hand-over buffer into the window (drawing thread). */
static void draw_frame(const uint8_t *frame, unsigned fw, unsigned fh, size_t pitch, int fmt)
{
    ANativeWindow_Buffer b;
    pthread_mutex_lock(&g.wlock);
    if (g.win) {
        if ((int)fw * SCALE != g.win_w || (int)fh * SCALE != g.win_h) set_geometry_locked(fw, fh);
        if (ANativeWindow_lock(g.win, &b, NULL) == 0) {
            const int w = (int)fw, h = (int)fh;
            if ((b.format == WINDOW_FORMAT_RGBA_8888 || b.format == WINDOW_FORMAT_RGBX_8888) && b.width >= w * SCALE && b.height >= h * SCALE) {
                for (int y = 0; y < h; y++) {
                    uint32_t *row0 = (uint32_t *)b.bits + (size_t)y * SCALE * b.stride, *o = row0;
                    if (fmt == RETRO_PIXEL_FORMAT_XRGB8888) {
                        const uint32_t *s = (const uint32_t *)(frame + (size_t)y * pitch);
                        for (int x = 0; x < w; x++) { uint32_t p = px_from_xrgb8888(s[x]); o[0] = o[1] = o[2] = o[3] = p; o += 4; }
                    } else {
                        const uint16_t *s = (const uint16_t *)(frame + (size_t)y * pitch);
                        for (int x = 0; x < w; x++) { uint32_t p = px_from_rgb565(s[x]); o[0] = o[1] = o[2] = o[3] = p; o += 4; }
                    }
                    for (int k = 1; k < SCALE; k++) memcpy(row0 + (size_t)k * b.stride, row0, (size_t)w * SCALE * 4);
                }
            }
            ANativeWindow_unlockAndPost(g.win);
        }
    }
    pthread_mutex_unlock(&g.wlock);
}

static volatile double draw_sum, draw_max; static volatile int draw_count;

static void *draw_thread(void *arg)
{
    for (;;) {
        pthread_mutex_lock(&g.pm);
        while (g.latest < 0 && !g.pquit) pthread_cond_wait(&g.pc, &g.pm);
        if (g.pquit) { pthread_mutex_unlock(&g.pm); break; }
        int k = g.latest; g.latest = -1; g.busy = k;
        pthread_mutex_unlock(&g.pm);
        int64_t t0 = now_ns();
        draw_frame(g.fb[k].data, g.fb[k].w, g.fb[k].h, g.fb[k].pitch, g.fb[k].fmt);
        double ms = (double)(now_ns() - t0) * 1e-6; draw_sum += ms; if (ms > draw_max) draw_max = ms; draw_count++;
        pthread_mutex_lock(&g.pm); g.busy = -1; pthread_mutex_unlock(&g.pm);
    }
    return NULL;
}

/* Emulation thread: hand the frame just produced to the drawing thread (a copy, ~300 KB). Never blocks on the display. */
static void queue_frame(void)
{
    if (!g.ready) return;
    pthread_mutex_lock(&g.pm);
    int k = 0; while (k == g.latest || k == g.busy) k++;                 /* three buffers: one is always free */
    if (g.latest >= 0) g.dropped++;                                      /* the drawing thread never got to show the previous one */
    pthread_mutex_unlock(&g.pm);
    size_t bytes = (size_t)g.pitch * g.h;
    if (bytes > g.fb[k].cap) { free(g.fb[k].data); g.fb[k].data = malloc(bytes); g.fb[k].cap = bytes; }
    memcpy(g.fb[k].data, g.frame, bytes); g.fb[k].bytes = bytes; g.fb[k].w = g.w; g.fb[k].h = g.h; g.fb[k].pitch = (unsigned)g.pitch; g.fb[k].fmt = g.pixfmt;
    pthread_mutex_lock(&g.pm); g.latest = k; pthread_cond_signal(&g.pc); pthread_mutex_unlock(&g.pm);
}

static void draw_thread_start(void)
{
    if (g.pthr_on) return;
    g.pquit = 0; g.latest = -1; g.busy = -1;
    if (pthread_create(&g.pthr, NULL, draw_thread, NULL) == 0) g.pthr_on = 1;
}
static void draw_thread_stop(void)
{
    if (!g.pthr_on) return;
    pthread_mutex_lock(&g.pm); g.pquit = 1; pthread_cond_signal(&g.pc); pthread_mutex_unlock(&g.pm);
    pthread_join(g.pthr, NULL); g.pthr_on = 0;
}

/* ------------------------------------------------------------------ JNI */
static double rss_mb(void)
{
    long pages = 0, res = 0; FILE *f = fopen("/proc/self/statm", "r");
    if (f) { if (fscanf(f, "%ld %ld", &pages, &res) != 2) res = 0; fclose(f); }
    return res * 4096.0 / 1048576.0;
}

JNIEXPORT jboolean JNICALL Java_com_jnaina_tempest2000_Native_init(JNIEnv *env, jclass cls, jobject am, jstring saveDir)
{
    if (g.loaded) return JNI_TRUE;
    const char *sd = (*env)->GetStringUTFChars(env, saveDir, NULL);
    snprintf(g.save_dir, sizeof g.save_dir, "%s", sd);
    snprintf(g.eeprom_path, sizeof g.eeprom_path, "%s/t2000.eeprom", sd);
    (*env)->ReleaseStringUTFChars(env, saveDir, sd);
    mkdir(g.save_dir, 0755);

    AAssetManager *mgr = AAssetManager_fromJava(env, am);
    AAsset *a = AAssetManager_open(mgr, ASSET_NAME, AASSET_MODE_BUFFER);
    if (!a) { LOGE("asset %s missing from the APK (run android/setup.sh, then build again)", ASSET_NAME); return JNI_FALSE; }
    g.rom_len = (size_t)AAsset_getLength(a); g.rom = malloc(g.rom_len);
    AAsset_read(a, g.rom, g.rom_len); AAsset_close(a);

    retro_set_environment(env_cb); retro_init();
    retro_set_video_refresh(video_cb); retro_set_audio_sample(audio_one_cb); retro_set_audio_sample_batch(audio_batch_cb);
    retro_set_input_poll(poll_cb); retro_set_input_state(input_cb);
    struct retro_game_info gi = { ASSET_NAME, g.rom, g.rom_len, NULL };
    if (!retro_load_game(&gi)) { LOGE("the emulator core rejected the game"); return JNI_FALSE; }
    struct retro_system_av_info av; retro_get_system_av_info(&av);
    g.w = av.geometry.base_width; g.h = av.geometry.base_height;
    g.aspect = av.geometry.aspect_ratio > 0 ? av.geometry.aspect_ratio : (double)g.w / g.h;
    g.fps = av.timing.fps > 1 ? av.timing.fps : 60.0;
    eeprom_load();
    g.loaded = 1;
    LOGI("core loaded: %ux%u aspect %.3f, core %.2f fps, %.0f Hz audio; game %zu bytes", g.w, g.h, g.aspect, g.fps, av.timing.sample_rate, g.rom_len);
    return JNI_TRUE;
}

JNIEXPORT void JNICALL Java_com_jnaina_tempest2000_Native_setSurface(JNIEnv *env, jclass cls, jobject surface)
{
    pthread_mutex_lock(&g.wlock);
    if (g.win) { ANativeWindow_release(g.win); g.win = NULL; }
    if (surface) { g.win = ANativeWindow_fromSurface(env, surface); set_geometry_locked(g.w, g.h); }
    pthread_mutex_unlock(&g.wlock);
}

JNIEXPORT void JNICALL Java_com_jnaina_tempest2000_Native_setInput(JNIEnv *env, jclass cls, jint mask)
{
    atomic_fetch_or(&g.pad_latch, (uint32_t)mask);      /* remember even a press that ends before the next frame */
    atomic_store(&g.pad, (uint32_t)mask);
}
JNIEXPORT jfloat JNICALL Java_com_jnaina_tempest2000_Native_aspect(JNIEnv *env, jclass cls) { return (jfloat)g.aspect; }

JNIEXPORT void JNICALL Java_com_jnaina_tempest2000_Native_start(JNIEnv *env, jclass cls)
{
    pthread_mutex_lock(&g.run);
    if (g.loaded) { draw_thread_start(); atomic_store(&rd, 0); atomic_store(&wr, 0); g.audio_wanted = 1; g.running = 1; g.acc = 1.0; g.last_ns = 0; g.last_frame_ns = 0; g.frames_run = 0; }
    pthread_mutex_unlock(&g.run);
}

JNIEXPORT void JNICALL Java_com_jnaina_tempest2000_Native_stop(JNIEnv *env, jclass cls)
{
    pthread_mutex_lock(&g.run);
    g.running = 0; g.audio_wanted = 0; audio_close(); draw_thread_stop();
    if (g.loaded) eeprom_save();
    pthread_mutex_unlock(&g.run);
}

/* Called once per screen refresh (Choreographer frame callback), on the emulation thread. */
JNIEXPORT void JNICALL Java_com_jnaina_tempest2000_Native_tick(JNIEnv *env, jclass cls, jlong frameTimeNanos)
{
    pthread_mutex_lock(&g.run);
    if (g.running) {
        if (g.audio_err) { audio_close(); audio_open(); }
        { uint32_t u = atomic_load(&underruns);                       /* the audio ran dry: buffer a little more from now on */
          if (u != g.seen_underruns) { g.seen_underruns = u; g.audio_target += 480; if (g.audio_target > AUDIO_TARGET_MAX) g.audio_target = AUDIO_TARGET_MAX; } }
        if (!g.stream && g.audio_wanted && g.frames_run >= 20 && ring_fill() + 300 >= (uint32_t)g.audio_target) audio_open();   /* after the first-frame surface set-up, with ~3 frames queued */
        if (g.last_ns == 0) g.acc = 1.0;
        else {
            double dt = (double)(frameTimeNanos - g.last_ns) * 1e-9;
            if (dt * 1000 > g.tick_gap_max) g.tick_gap_max = dt * 1000;
            if (dt > 0.025) g.missed_ticks++;                    /* the vsync callback itself arrived late */
            if (dt < 0 || dt > 0.1) dt = 1.0 / 60.0;            /* first tick after a pause */
            g.acc += dt * 60.0;
        }
        g.last_ns = frameTimeNanos;
        if (g.acc >= 0.9) {                                       /* this refresh carries a game frame */
            g.acc -= 1.0; if (g.acc > 1.0) g.acc = 1.0;           /* never burst to catch up */
            {   /* buttons: held, or tapped since the last frame; every press lasts at least MIN_PRESS frames */
                uint32_t cur = atomic_exchange(&g.pad_latch, 0) | atomic_load(&g.pad), fp = 0;
                for (int bit = 0; bit < 16; bit++) {
                    if (cur & (1u << bit)) g.hold[bit] = MIN_PRESS;
                    if (g.hold[bit] > 0) { fp |= 1u << bit; g.hold[bit]--; }
                }
                g.frame_pad = fp;
            }
            int64_t t0 = now_ns();
            retro_run();
            int64_t t1 = now_ns();
            queue_frame();
            int64_t t2 = now_ns();
            double ms = (double)(t2 - t0) * 1e-6;
            if (g.last_frame_ns) { double gap = (double)(t0 - g.last_frame_ns) * 1e-6; if (gap > g.gap_max) g.gap_max = gap; if (gap > 25.0) g.late++; }
            g.last_frame_ns = t0;
            g.ms_sum += ms; if (ms > g.ms_max) g.ms_max = ms;
            { double r = (double)(t1 - t0) * 1e-6; g.run_sum += r; if (r > g.run_max) g.run_max = r; }
            if (++g.frames_run % 600 == 0) {
                uint32_t u = atomic_load(&underruns);
                int dc = draw_count; double da = dc ? draw_sum / dc : 0, dm = draw_max;
                draw_sum = draw_max = 0; draw_count = 0;
                LOGI("perf 600 frames | emulation avg %.2f max %.2f ms | drawing thread avg %.2f max %.2f ms, %d frames skipped | vsync callbacks: longest gap %.1f ms, %d late | audio underruns %u, queued %u (target %d) | memory %.0f MB",
                     g.run_sum / 600, g.run_max, da, dm, g.dropped, g.tick_gap_max, g.missed_ticks, u - g.last_underruns, ring_fill(), g.audio_target, rss_mb());
                if (u == g.last_underruns && g.audio_target > AUDIO_TARGET_MIN) g.audio_target -= 240;     /* a quiet 10 seconds: shrink the buffer again */
                if (g.audio_target < AUDIO_TARGET_MIN) g.audio_target = AUDIO_TARGET_MIN;
                g.dropped = 0; g.ms_sum = g.ms_max = g.gap_max = g.run_sum = g.present_sum = g.run_max = g.present_max = g.tick_gap_max = 0; g.late = 0; g.missed_ticks = 0; g.last_underruns = u;
            }
            if (g.frames_run % 300 == 0) eeprom_save();
        }
    }
    pthread_mutex_unlock(&g.run);
}

JNIEXPORT void JNICALL Java_com_jnaina_tempest2000_Native_shutdown(JNIEnv *env, jclass cls)
{
    pthread_mutex_lock(&g.run);
    g.running = 0; audio_close(); draw_thread_stop();
    if (g.loaded) { eeprom_save(); retro_unload_game(); retro_deinit(); g.loaded = 0; }
    free(g.rom); g.rom = NULL;
    pthread_mutex_unlock(&g.run);
}
