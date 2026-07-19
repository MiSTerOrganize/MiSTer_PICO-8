//
//  zepto8-mister — MiSTer FPGA frontend for the zepto8 PICO-8 emulator
//
//  Copyright © 2024-2026 MiSTer Organize
//
//  Built on zepto8 by Sam Hocevar (WTFPL license)
//  MiSTer frontend patterns for the zepto8 PICO-8 emulator on MiSTer
//

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <csignal>
#include <cmath>
#include <string>
#include <memory>
#include <thread>
#include <atomic>

#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <linux/joystick.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>

#include <SDL/SDL.h>

#include "zepto8.h"
#include "pico8/vm.h"
#include <lol/sys/init.h>
#include "cart_browser.h"
#include "native_video_writer.h"
#include "test_trace.h"

// ── Configuration ─────────────────────────────────────────────────────

static const int PICO8_W        = 128;
static const int PICO8_H        = 128;
static const int SCREEN_W       = 320;  // Set by vmode in pico-8.sh
static const int SCREEN_H       = 240;
static const int SCREEN_BPP     = 16;   // rgb16
static const int AUDIO_RATE     = 22050;
static const int AUDIO_CHANNELS = 1;    // mono
static const int AUDIO_BUF_SAMPLES = 512;
static const int DEFAULT_FPS    = 60;   // PICO-8 BIOS expects 60 ticks/sec

// ── Globals ───────────────────────────────────────────────────────────

static volatile bool g_running = true;
static volatile bool g_return_to_browser = false;

// ── Golden-master hash trace (-test flag) ─────────────────────────────
// When enabled, each gameplay frame emits FRAME:VIDEOCRC:AUDIOCRC to the
// trace file (see src/test_trace.h — same format + hash points as the x86
// z8headless --test oracle, so traces diff directly). Default-off; inert
// unless -test is passed. In test mode the free-running audio thread is
// NOT started — the main loop pulls the engine audio deterministically.
static FILE *g_test_trace = NULL;
static long long g_test_frame = 0;         // frames traced so far
static long long g_test_frames_limit = 0;  // -testframes N: exit(0) after N (0 = unlimited)
static std::unique_ptr<z8::pico8::vm> g_vm;
static bool g_joystick_connected = false;
static SDL_Joystick* g_sdl_joystick = NULL;  // SDL joystick for polling hat/axis state

// ── Timing ────────────────────────────────────────────────────────────

static uint64_t get_time_ns()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

// ══════════════════════════════════════════════════════════════════════
//  FPGA Native Audio — DDR3 ring buffer
//  ARM writes 48KHz stereo PCM to DDR3, FPGA reads at 48KHz.
//  Same audio path as NES, SNES, Genesis — I2S + SPDIF + DAC.
//  No ALSA, no Linux kernel, no dlopen.
// ══════════════════════════════════════════════════════════════════════

static const int SRC_RATE = 22050;   // zepto8 native sample rate
static const int DST_RATE = 48000;   // FPGA audio output rate

// ── Signal handler ────────────────────────────────────────────────────
static void signal_handler(int sig)
{
    (void)sig;
    g_running = false;
    // No ALSA cleanup needed — audio goes through DDR3 to FPGA.
    // When the process dies, the FPGA ring buffer drains to silence.
}

// Save-state request flags. Set by the FPGA save-state UI dispatch path
// (DDR3 control word, polled between frames). Main loop dispatches
// the actual save/load when one of these is set.
static volatile int g_savestate_save_request = -1;  // slot 0..3, or -1
static volatile int g_savestate_load_request = -1;

// ── Audio thread — DDR3 ring buffer writer ───────────────────────────
// Renders 22050Hz mono from zepto8, upsamples to 48KHz stereo,
// writes to DDR3 ring buffer. FPGA reads at 48KHz.
// No ALSA. No kernel. Same path as every MiSTer core.

static pthread_t g_audio_thread;
static volatile bool g_audio_running = false;

// Upsample 22050 Hz mono → 48000 Hz stereo using LINEAR INTERPOLATION.
//
// Engine-source-driven choice per the NON-NEGOTIABLE rule in
// feedback_audio_type_from_engine_source.md: zepto8's
// src/pico8/sfx.cpp::get_audio produces 22050 Hz output via linear
// interpolation at the engine level (PCM streaming linear-interp at
// line 446: s0 + (s1 - s0) * frac). The wrapper resampler matches
// the engine kernel character (linear).
//
// Per output sample: 1 multiply + 1 add. Cheap, matches engine
// character exactly. Mono input is duplicated to stereo on write.
//
// Returns number of stereo output samples written.

static int upsample_mono_to_stereo(const int16_t *mono_in, int in_samples,
                                    int16_t *stereo_out, int max_out)
{
    // Fixed-point step: (22050 << 16) / 48000 = 30106. uint64 intermediate
    // to avoid the int32 overflow trap (signed-shift UB on >= 32768).
    const uint32_t step = (uint32_t)(((uint64_t)SRC_RATE << 16) / DST_RATE);
    uint32_t accum = 0;
    int out_count = 0;

    while (out_count < max_out) {
        uint32_t src_idx = accum >> 16;
        if (src_idx >= (uint32_t)in_samples) break;

        uint32_t fr = accum & 0xFFFF;

        // Linear interpolation: s0 + (s1 - s0) * frac, with frac in 16.16.
        int32_t s0 = (int32_t)mono_in[src_idx];
        int32_t s1 = (src_idx + 1 < (uint32_t)in_samples)
                     ? (int32_t)mono_in[src_idx + 1]
                     : s0;  // Boundary: hold last sample
        int32_t sum = s0 + (((s1 - s0) * (int32_t)fr) >> 16);
        if (sum > 32767)  sum = 32767;
        if (sum < -32768) sum = -32768;
        int16_t s16 = (int16_t)sum;

        stereo_out[out_count * 2 + 0] = s16;  // Left
        stereo_out[out_count * 2 + 1] = s16;  // Right (mono duplicate)
        out_count++;
        accum += step;
    }
    return out_count;
}

static void *audio_thread_func(void *arg)
{
    (void)arg;

    // Pin audio thread to CPU core 1 (render/main thread runs on the memory-fast core 0).
    // 2026-06-13 affinity rule INVERTED (render->core0, audio->core1): mem_bench shows
    // core 0 has ~1.85x core 1's DDR3 read bandwidth; sprite render is memory-bound, so
    // render takes core 0 and audio (light) takes core 1. Validated +81% on OpenBOR_7533
    // He-Man. PICO-8 is vsync-locked (not memory-bound) so neutral here -- switched for
    // the uniform cross-core rule. See feedback_affinity_render_core0_audio_core1.md.
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(1, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);

    int16_t mono_buf[AUDIO_BUF_SAMPLES];
    // Max output: 512 * 48000/22050 + 2 ≈ 1117 stereo samples
    int16_t stereo_buf[2400];

    fprintf(stderr, "Audio: DDR3 ring buffer, %dHz mono → %dHz stereo\n", SRC_RATE, DST_RATE);
    fflush(stderr);

    while (g_audio_running)
    {
        if (!g_vm) break;

        // Render mono audio from zepto8 at 22050Hz
        g_vm->get_audio(mono_buf, AUDIO_BUF_SAMPLES * sizeof(int16_t));

        // Upsample to 48KHz stereo
        int out_samples = upsample_mono_to_stereo(mono_buf, AUDIO_BUF_SAMPLES,
                                                   stereo_buf, 1200);
        (void)out_samples;

        // Wait for space in the DDR3 ring buffer, then write
        while (g_audio_running) {
            uint32_t space = NativeVideoWriter_AudioSpace();
            if (space >= (uint32_t)out_samples) break;
            usleep(500);  // ~0.5ms — ring buffer provides 85ms of slack
        }
        if (!g_audio_running) break;

        NativeVideoWriter_WriteAudio(stereo_buf, out_samples);
    }

    return nullptr;
}

static bool audio_thread_start()
{
    g_audio_running = true;
    int err = pthread_create(&g_audio_thread, nullptr, audio_thread_func, nullptr);
    if (err != 0) {
        fprintf(stderr, "Audio thread creation failed\n");
        g_audio_running = false;
        return false;
    }
    fprintf(stderr, "Audio thread launched (DDR3 ring buffer)\n");
    return true;
}

static void audio_thread_stop()
{
    g_audio_running = false;
    pthread_join(g_audio_thread, nullptr);
}

// ── SDL dummy audio callback ──────────────────────────────────────────
// SDL_OpenAudio with a real callback is REQUIRED for SDL's internal
// timer/event system. Without it, video rendering has severe flicker.
// Leave SDL audio open with dummy callback — never use it for output.

static void DummyAudioCallback(void *userdata, Uint8 *stream, int len)
{
    (void)userdata;
    memset(stream, 0, len);
}

// ── Video helpers ─────────────────────────────────────────────────────

static inline uint16_t rgba_to_565(uint8_t r, uint8_t g, uint8_t b)
{
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}

// Precomputed scale lookup tables (initialized once)
static int scale_lut_x[320]; // maps dst_x → src_x
static int scale_lut_y[240]; // maps dst_y → src_y
static int scale_off_x = 0;
static int scale_w = 0;
static bool scale_lut_ready = false;

static void init_scale_luts()
{
    if (scale_lut_ready) return;
    scale_w = SCREEN_H; // 240×240 square
    scale_off_x = (SCREEN_W - scale_w) / 2;
    for (int dx = 0; dx < scale_w; ++dx)
        scale_lut_x[dx] = dx * PICO8_W / scale_w;
    for (int dy = 0; dy < SCREEN_H; ++dy)
        scale_lut_y[dy] = dy * PICO8_H / SCREEN_H;
    scale_lut_ready = true;
}

// Clear the border strips (call once after SDL_SetVideoMode)
static void clear_borders(SDL_Surface *surface)
{
    if (SDL_MUSTLOCK(surface)) SDL_LockSurface(surface);
    memset(surface->pixels, 0, surface->pitch * surface->h);
    if (SDL_MUSTLOCK(surface)) SDL_UnlockSurface(surface);
}

// Blit 128x128 RGBA8 buffer to 320x240 16bpp SDL surface with StretchToFit
// Uses precomputed lookup tables — no division in the inner loop
static void blit_stretched(SDL_Surface *surface, const lol::u8vec4 *src)
{
    init_scale_luts();

    if (SDL_MUSTLOCK(surface)) SDL_LockSurface(surface);

    uint16_t *dst = (uint16_t *)surface->pixels;
    int pitch16 = surface->pitch / 2;

    for (int dy = 0; dy < SCREEN_H; ++dy) {
        int sy = scale_lut_y[dy];
        const lol::u8vec4 *src_row = src + sy * PICO8_W;
        uint16_t *dst_row = dst + dy * pitch16 + scale_off_x;
        for (int dx = 0; dx < scale_w; ++dx) {
            const lol::u8vec4 &p = src_row[scale_lut_x[dx]];
            dst_row[dx] = rgba_to_565(p.r, p.g, p.b);
        }
    }

    if (SDL_MUSTLOCK(surface)) SDL_UnlockSurface(surface);
}

// ── Joystick input ────────────────────────────────────────────────────
// Reads Linux joystick events from /dev/input/js0
// Button mapping (verified on hardware):
//
//   Xbox SDL#  PICO-8
//   A    0     O button (jump, in-game only — no menu function)
//   B    1     Nothing
//   X    2     X button (confirm menu, shoot in-game)
//   Y    3     Nothing
//   Back 6     Quit
//   Start 7    Pause
//   Guide 8    Quit

// ── Path resolution ───────────────────────────────────────────────────

static std::string get_exe_dir()
{
    char buf[4096];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len <= 0) return "./";
    buf[len] = '\0';
    char *last = strrchr(buf, '/');
    if (last) { *(last + 1) = '\0'; return std::string(buf); }
    return "./";
}

// ── Main ──────────────────────────────────────────────────────────────

static void print_usage(const char *prog)
{
    fprintf(stderr, "zepto8 (PICO-8 emulator) for MiSTer FPGA\n\n");
    fprintf(stderr, "Usage: %s [options] <cart.p8|cart.p8.png>\n\n", prog);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -fps <N>    Target frame rate (default: %d)\n", DEFAULT_FPS);
    fprintf(stderr, "  -nosound    Disable audio\n");
    fprintf(stderr, "  -nojoy      Disable joystick\n");
    fprintf(stderr, "  -nativevideo Write video to DDR3 for FPGA native output (CRT)\n");
    fprintf(stderr, "  -data <dir> Set data directory (for pico8/bios.p8)\n");
    fprintf(stderr, "  -test <file>      Write golden-master hash trace (frame:videocrc:audiocrc)\n");
    fprintf(stderr, "  -testframes <N>   With -test: exit(0) after tracing N frames\n");
    fprintf(stderr, "  -h          Show this help\n");
}

int main(int argc, char **argv)
{
    // Line-buffer stderr so log output is flushed on every newline. Without
    // this, stderr (redirected to /media/fat/logs/PICO-8/pico8.log by the
    // daemon) is block-buffered (~4 KB) — diagnostic output from
    // savestate_save / savestate_load can stay buffered until process exit
    // or crash, making it impossible to debug crashes mid-restore.
    setvbuf(stderr, NULL, _IOLBF, 0);

    // ── Parse arguments ───────────────────────────────────────────────
    std::string cart_path;
    std::string data_dir;
    int target_fps = DEFAULT_FPS;
    bool enable_sound = true;
    bool enable_joy = true;
    bool enable_native_video = false;

    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        if (arg == "-fps" && i + 1 < argc)  { target_fps = atoi(argv[++i]); }
        else if (arg == "-nosound")          { enable_sound = false; }
        else if (arg == "-nojoy")            { enable_joy = false; }
        else if (arg == "-nativevideo")      { enable_native_video = true; }
        else if (arg == "-data" && i + 1 < argc) { data_dir = argv[++i]; }
        else if (arg == "-test" && i + 1 < argc) {
            const char *tp = argv[++i];
            g_test_trace = fopen(tp, "w");
            if (!g_test_trace) { fprintf(stderr, "Cannot open trace file: %s\n", tp); return 1; }
            // Fully buffered — flushed at fclose, never per line (no
            // per-frame SD I/O; see feedback_logging_hotpath_perf.md).
            setvbuf(g_test_trace, NULL, _IOFBF, 65536);
        }
        else if (arg == "-testframes" && i + 1 < argc) { g_test_frames_limit = atoll(argv[++i]); }
        else if (arg == "-h" || arg == "--help") { print_usage(argv[0]); return 0; }
        else if (arg[0] != '-')              { cart_path = arg; }
        else { fprintf(stderr, "Unknown option: %s\n", arg.c_str()); return 1; }
    }

    // Test-trace mode steps at exactly 60 fps so hardware traces are
    // directly comparable to the x86 z8headless oracle (which always
    // steps 1/60), and forces the deterministic PRNG seed (must be set
    // before the vm is constructed — private_init_ram seeds there).
    if (g_test_trace) {
        target_fps = DEFAULT_FPS;
        setenv("Z8_TEST_SEED", "1", 0);
    }

    if (cart_path.empty()) {
        // No cart specified — will show browser (SDL mode) or wait for OSD (native video)
    }
    if (target_fps < 10) target_fps = 10;
    if (target_fps > 60) target_fps = 60;

    // Data path: for bios.p8 resolution. Defaults to binary's directory.
    // In native video mode, default to MiSTer setname folder.
    if (data_dir.empty()) {
        if (enable_native_video)
            data_dir = "/media/fat/games/PICO8/";
        else
            data_dir = get_exe_dir();
    }
    lol::sys::set_data_path(data_dir);

    // Set ZEPTO8_BASE_DIR for cart path resolution.
    // With __MISTER__ defined, private.cpp uses this for:
    //   Carts:  $ZEPTO8_BASE_DIR/Carts/
    // Config: /media/fat/config/zepto8.cfg
    // Saves:  /media/fat/saves/PICO-8/
    setenv("ZEPTO8_BASE_DIR", data_dir.c_str(), 1);

    // Create standard MiSTer Organize folders
    std::string logs_dir = "/media/fat/logs/PICO-8";
    mkdir("/media/fat/logs", 0755);
    mkdir(logs_dir.c_str(), 0755);

    // Redirect stderr to log file for diagnostics
    // Captures: startup info, cart printh() output, errors, hot-swap events
    {
        std::string log_path = logs_dir + "/pico8.log";
        FILE *logf = fopen(log_path.c_str(), "w");
        if (logf) {
            dup2(fileno(logf), STDERR_FILENO);
            fclose(logf);
        }
    }

    // Pin main/render thread to CPU core 0 (the memory-fast core; audio uses core 1).
    // 2026-06-13 affinity rule INVERTED: core 0 has ~1.85x core 1's DDR3 bandwidth
    // (mem_bench) and sprite render is memory-bound -> render on core 0 beats the old
    // IRQ-avoidance layout (validated +81% on OpenBOR_7533 He-Man). PICO-8 is vsync-
    // locked so neutral; switched for the uniform cross-core rule. Matches OpenBOR.
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(0, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // ── Init SDL ──────────────────────────────────────────────────────
    // In native video mode, use SDL's dummy video driver — this gives us
    // the event system for joystick input without touching /dev/fb0
    // (which doesn't work when a custom FPGA core is loaded).
    SDL_Surface *screen = NULL;

    if (enable_native_video) {
        setenv("SDL_VIDEODRIVER", "dummy", 1);
        setenv("SDL_AUDIODRIVER", "dummy", 1);
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK) < 0) {
            fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
            return 1;
        }
        // Create a dummy surface — SDL needs this for the event pump
        screen = SDL_SetVideoMode(SCREEN_W, SCREEN_H, SCREEN_BPP, SDL_SWSURFACE);
        // screen may be NULL with dummy driver — that's okay
    } else {
        // Normal mode: full SDL init with video and audio
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK) < 0) {
            fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
            return 1;
        }

        // vmode is set by pico-8.sh launcher script (320x240 rgb16)
        // Redundant call here as fallback for direct invocation
        if (system("vmode -r 320 240 rgb16 > /dev/null 2>&1") != 0) {
            // vmode not available — script handles this, safe to ignore
        }

        screen = SDL_SetVideoMode(SCREEN_W, SCREEN_H, SCREEN_BPP, SDL_SWSURFACE);
        if (!screen) {
            fprintf(stderr, "SDL_SetVideoMode failed: %s\n", SDL_GetError());
            SDL_Quit(); return 1;
        }
        SDL_ShowCursor(SDL_DISABLE);
    }

    // Open SDL joystick for state polling (hat, axis, buttons)
    if (SDL_NumJoysticks() > 0) {
        g_sdl_joystick = SDL_JoystickOpen(0);
        if (g_sdl_joystick) {
            g_joystick_connected = true;
            fprintf(stderr, "Joystick: %s (%d buttons, %d axes, %d hats)\n",
                SDL_JoystickName(0),
                SDL_JoystickNumButtons(g_sdl_joystick),
                SDL_JoystickNumAxes(g_sdl_joystick),
                SDL_JoystickNumHats(g_sdl_joystick));
        }
    }

    // Startup screen clear (3 frames, proven pattern)
    if (screen) {
        for (int i = 0; i < 3; i++) {
            SDL_FillRect(screen, NULL, 0);
            SDL_Flip(screen);
        }
        clear_borders(screen);
    }

    // ── SDL audio init (for video stability) ───────────────────────────
    // SDL_OpenAudio with real callback required for internal timer/event
    // state. Leave open with dummy callback — audio output goes through DDR3.
    // Skip in native video mode — no SDL video means this isn't needed.
    if (screen) {
        SDL_AudioSpec desired;
        memset(&desired, 0, sizeof(desired));
        desired.freq = AUDIO_RATE;
        desired.format = AUDIO_S16LSB;
        desired.channels = AUDIO_CHANNELS;
        desired.samples = 512;
        desired.callback = DummyAudioCallback;
        if (SDL_OpenAudio(&desired, nullptr) == 0) {
            SDL_PauseAudio(0);
            // Don't close — leave open with dummy callback for SDL stability
        }
    }

    // ── Init native video DDR3 writer (for FPGA native output) ────────
    // Only when -nativevideo flag is passed (requires PICO-8 FPGA core loaded).
    bool have_native_video = false;
    if (enable_native_video) {
        have_native_video = NativeVideoWriter_Init();
        if (have_native_video)
            fprintf(stderr, "Native video: DDR3 writer active (128x128 RGB565)\n");
        else
            fprintf(stderr, "Native video: DDR3 init failed, falling back to SDL\n");
    }

    // ── Keepalive thread ──────────────────────────────────────────────
    // FPGA pico8_video_reader.sv has a 30-vblank (~500ms) staleness
    // timeout that blanks the screen when no fresh frame counter ticks
    // arrive. During cart hot-swap, save-state load, or .s0-wait loops,
    // the main thread doesn't write frames for hundreds of ms — long
    // enough to hit the timeout and produce a black flash.
    //
    // Solution: lightweight thread bumps the frame counter every 150ms
    // pointing at the LAST-written buffer. FPGA's stale detector resets
    // and keeps re-displaying the frozen previous frame instead of
    // blanking. Same pattern as OpenBOR uses (CLAUDE.md keepalive rule).
    std::thread keepalive_thread;
    std::atomic<bool> keepalive_run{true};
    if (have_native_video) {
        keepalive_thread = std::thread([&keepalive_run]() {
            while (keepalive_run.load()) {
                NativeVideoWriter_KeepaliveTick();
                usleep(150000);  /* 150ms — well under the 500ms timeout */
            }
        });
    }

    // ── Init audio ─────────────────────────────────────────────────────
    // In native video mode, audio goes through DDR3 ring buffer to FPGA.
    // No ALSA init needed — the ring buffer is part of the DDR3 writer.
    bool have_audio = false;
    if (enable_sound && have_native_video)
        have_audio = true;

    // Joystick is handled by SDL (opened after SDL_Init above)
    // Cart browser opens /dev/input/js0 directly when needed

    // ── Cart browser / game loop ──────────────────────────────────────
    // Normal mode: show visual cart browser
    // Native video mode: wait for cart from OSD file browser via DDR3

    std::string carts_dir = data_dir + "Carts";

    fprintf(stderr, "=== Process started, PID=%d ===\n", getpid());

    while (g_running)
    {
        // Get cart path
        if (cart_path.empty()) {
            if (have_native_video) {
                // SC0 mode: MiSTer writes the cart's source path to
                // /media/fat/config/PICO-8.s0 instantly when the user
                // picks from the OSD. We read the path and load the
                // cart from its real SD location — that way zepto8's
                // load("sibling.p8") for multicart games resolves to
                // the right directory (vs the old /tmp/ copy approach
                // which orphaned the cart from its siblings).
                // Clear DDR3 framebuffer so screen goes black during the
                // wait loop instead of showing the previous cart's last
                // frame (FPGA keepalive keeps reading the last-written
                // buffer). Cross-applied from OpenBOR's analogous fix per
                // `feedback_clear_framebuffer_on_wait.md`. PICO-8 native
                // is 128x128 RGBA8888 (4 bytes per pixel).
                {
                    static unsigned char _pico8_black[PICO8_W * PICO8_H * 4] = {0};
                    NativeVideoWriter_WriteFrame(_pico8_black, PICO8_W, PICO8_H);
                }

                fprintf(stderr, "Waiting for OSD cart selection (.s0)...\n");
                int poll_count = 0;
                while (g_running) {
                    char s0_path[512] = {0};
                    FILE *f = fopen("/media/fat/config/PICO-8.s0", "r");
                    if (f) {
                        if (fgets(s0_path, sizeof(s0_path), f)) {
                            char *nl = strchr(s0_path, '\n'); if (nl) *nl = 0;
                            char *cr = strchr(s0_path, '\r'); if (cr) *cr = 0;
                            // MiSTer writes .s0 without truncating, so a
                            // shorter new path can leak trailing bytes from
                            // a previous longer one. Trim at the last cart
                            // extension and strip trailing whitespace.
                            char *cut = NULL;
                            char *p;
                            for (p = strstr(s0_path, ".p8.png"); p; p = strstr(p+1, ".p8.png"))
                                cut = p + 7;
                            if (!cut) for (p = strstr(s0_path, ".p8"); p; p = strstr(p+1, ".p8"))
                                cut = p + 3;
                            if (cut) *cut = 0;
                            int pl = (int)strlen(s0_path);
                            while (pl > 0 && (s0_path[pl-1] == ' ' || s0_path[pl-1] == '\t'))
                                s0_path[--pl] = 0;
                        }
                        fclose(f);
                        if (strlen(s0_path) > 0) {
                            char full[1024];
                            // OSD picks write relative paths (games/PICO-8/...);
                            // MGL writes absolute paths (/media/fat/...).
                            // Detect and don't double-prefix.
                            if (s0_path[0] == '/')
                                snprintf(full, sizeof(full), "%s", s0_path);
                            else
                                snprintf(full, sizeof(full), "/media/fat/%s", s0_path);
                            cart_path = std::string(full);
                            fprintf(stderr, "OSD selected: %s\n", cart_path.c_str());
                            break;
                        }
                    }
                    if (++poll_count >= 30) {
                        fprintf(stderr, "Still waiting for .s0...\n");
                        poll_count = 0;
                    }
                    usleep(200000); // poll every 200ms
                }
                if (cart_path.empty()) break; // quit was requested
            } else {
                // SDL cart browser (dummy driver — no fbcon)
                if (g_sdl_joystick) {
                    SDL_JoystickClose(g_sdl_joystick);
                    g_sdl_joystick = NULL;
                }

                int browser_joy_fd = open("/dev/input/js0", O_RDONLY | O_NONBLOCK);
                cart_path = run_cart_browser(screen, carts_dir, browser_joy_fd);
                if (browser_joy_fd >= 0) close(browser_joy_fd);

                if (cart_path.empty()) {
                    g_running = false;
                    break;
                }

                if (SDL_NumJoysticks() > 0) {
                    g_sdl_joystick = SDL_JoystickOpen(0);
                }
            }
        }

        // Create VM and load cart
        g_vm = std::make_unique<z8::pico8::vm>();

        // Register no-op stubs for the optional std::function callbacks the VM
        // uses for desktop features (mouse pointer lock, fullscreen toggle,
        // CRT filter selection). Default-constructed std::function objects
        // throw std::bad_function_call when invoked, and that exception
        // propagates out through the Lua bindings as a function-typed error
        // value — silently breaking any cart that touches mouse_flags.locked,
        // sets fullscreen, or tweaks filters (POOM did this on its first
        // _update_buttons() call; symptom = black screen, daemon log shows
        // "err_type=function" on coresume). Stubs make these no-ops.
        g_vm->registerPointerLockCallback([](bool){});
        g_vm->registerSetFullscreenCallback([](int){});
        g_vm->registerGetFullscreenCallback([]() -> std::string { return ""; });
        g_vm->registerSetFilterCallback([](int v){ return v; });
        g_vm->registerGetFilterNameCallback([](int) -> std::string { return ""; });

        // Register cart browser extcmd — when selected from pause menu,
        // delete .s0 and exit(0). Master_Daemon's child-respawn logic
        // re-execs _handler.sh which starts a fresh PICO-8 binary that
        // boots into the wait-for-OSD-cart-selection loop with cleanly
        // zeroed DDR3 (Init() does the memset). Same architecture as
        // OpenBOR's pause-menu Quit. Universal hybrid core rule: every
        // pause-menu Quit must exit(0), not return-to-loop, so the
        // post-Quit state is identical to a fresh handler spawn (no
        // stale DDR3, no stale VM state, no audio thread quirks).
        g_vm->add_extcmd("z8_cart_browser", [](std::string const &) {
            unlink("/media/fat/config/PICO-8.s0");
            fprintf(stderr, "Quit: cleared .s0, exit(0) — Master_Daemon will respawn\n");
            fflush(stderr);
            _exit(0);
        });

        // PICO-8 spec: extcmd("shutdown") = quit the program. Same
        // behavior as the system pause-menu Quit (z8_cart_browser above)
        // — unlink .s0 so Master_Daemon respawns the handler into a
        // fresh wait-for-OSD state (no auto-remount), then _exit(0).
        //
        // Without this handler, vm.cpp's built-in switch routes
        // shutdown to a silent no-op. Carts using the
        // `extcmd("shutdown"); extcmd("reset")` fallback pattern (e.g.,
        // oblivion_eve title-screen Quit) then fall through to reset,
        // which reloads the entry cart = back to title screen — user
        // perceives "Quit just resets the game". Registering shutdown
        // here as a user extcmd takes priority over vm.cpp's switch
        // (m_extcmds checked first per api_extcmd:1837) and routes
        // shutdown to the same exit path as the system menu Quit.
        g_vm->add_extcmd("shutdown", [](std::string const &) {
            unlink("/media/fat/config/PICO-8.s0");
            fprintf(stderr, "Shutdown (cart-initiated): cleared .s0, _exit(0) — Master_Daemon will respawn\n");
            fflush(stderr);
            _exit(0);
        });

        g_vm->load(cart_path);
        g_vm->run();
        fprintf(stderr, "=== Game started: %s (PID=%d) ===\n", cart_path.c_str(), getpid());

        // Start audio thread. In test-trace mode the thread stays OFF —
        // it free-runs get_audio() in 512-sample chunks, which would race
        // the main loop's deterministic per-frame pulls and desync the
        // audio hash. The trace block below pulls, hashes, and (when the
        // ring has space) still plays the audio from the main loop.
        bool audio_started = false;
        if (have_audio && !g_test_trace) {
            audio_started = audio_thread_start();
        }

        // ── Game loop ─────────────────────────────────────────────────

        lol::u8vec4 rgba_buf[PICO8_W * PICO8_H];
        const uint64_t frame_ns = 1000000000ULL / target_fps;
        uint64_t next_frame = get_time_ns();

        bool game_running = true;
        bool hot_swap_pending = false;
        while (g_running && game_running)
    {
        // ── Save-state request handling ──────────────────────────────
        // Source: FPGA savestate_ui via DDR3 control word from the OSD
        // pause-menu and F1-F4 keyboard shortcuts. Dispatched here
        // between frames so the VM and audio thread are at a clean
        // state boundary.
        {
            static uint8_t ss_last_seq    = 0;
            static bool    ss_seq_seeded  = false;
            uint32_t ss_word = NativeVideoWriter_ReadSavestate();
            uint8_t  cmd  = NV_SsCmd (ss_word);
            uint8_t  slot = NV_SsSlot(ss_word) & 0x3;
            uint8_t  seq  = NV_SsSeq (ss_word);
            if (!ss_seq_seeded) {
                // Capture FPGA's current seq as the baseline so a stale
                // counter from a previous run doesn't trigger spurious
                // save/load on first frame.
                ss_last_seq   = seq;
                ss_seq_seeded = true;
            }
            else if (seq != ss_last_seq) {
                ss_last_seq = seq;
                if      (cmd == 1) g_savestate_save_request = slot;
                else if (cmd == 2) g_savestate_load_request = slot;
            }
        }
        if (g_savestate_save_request >= 0) {
            int slot = g_savestate_save_request;
            g_savestate_save_request = -1;
            if (g_vm) g_vm->savestate_save(slot);
        }
        if (g_savestate_load_request >= 0) {
            int slot = g_savestate_load_request;
            g_savestate_load_request = -1;
            if (g_vm) g_vm->savestate_load(slot);
        }

        uint64_t now = get_time_ns();

        // Frame timing: sleep for most of the wait, then busy-wait for precision.
        // usleep on MiSTer's Linux can oversleep by 1-5ms, which at 60fps
        // (16.6ms budget) would drop us to ~50fps. Sleep until 2ms before
        // target, then spin for the remaining time.
        if (now < next_frame) {
            uint64_t wait = next_frame - now;
            if (wait > 2500000) // more than 2.5ms remaining
                usleep((unsigned int)((wait - 2000000) / 1000)); // sleep to within 2ms
            while (get_time_ns() < next_frame) {} // spin-wait the rest
        }
        next_frame += frame_ns;

        // Don't fall more than 2 frames behind
        uint64_t actual = get_time_ns();
        if (actual > next_frame + frame_ns * 2)
            next_frame = actual;

        // -- Input: read joysticks from DDR3 (FPGA writes hps_io data) --
        // Main_MiSTer has exclusive access to /dev/input/js*, so we read
        // joystick state directly from DDR3 where the FPGA puts it. PICO-8
        // supports up to 8 players via btn(b,p); MiSTer's hps_io provides
        // up to 4 USB joysticks, so we feed players 0..3.
        //
        // Standard PICO-8 input semantics: each player's controller maps
        // ONLY to their own player slot. Single-player carts read btn(b)
        // (= btn(b, 0)) and only see P1; multi-player carts read btn(b, p)
        // for each p. Don't OR across controllers here — that breaks
        // single-player carts (P2 would pause / move P1's character).
        // The bios.p8 pause-menu uses __z8_anybtnp() helpers so that once
        // P1 opens the menu, any player can navigate it.
        //
        // CONF_STR: "J1,O,X,Pause;" / "jn,B,Y,Start;" (SNES: B=Xbox A, Y=Xbox X)
        // joystick_N bits: 0=R 1=L 2=D 3=U 4=Xbox A(O) 5=Xbox X(X) 6=Start(Pause)
        if (have_native_video) {
            for (int p = 0; p < 4; p++) {
                uint32_t joy = NativeVideoWriter_ReadJoystick(p);
                g_vm->button(p, 0, (joy >> 1) & 1);  // Left
                g_vm->button(p, 1, (joy >> 0) & 1);  // Right
                g_vm->button(p, 2, (joy >> 3) & 1);  // Up
                g_vm->button(p, 3, (joy >> 2) & 1);  // Down
                g_vm->button(p, 4, (joy >> 4) & 1);  // O     ← Xbox A
                g_vm->button(p, 5, (joy >> 5) & 1);  // X     ← Xbox X
                g_vm->button(p, 6, (joy >> 6) & 1);  // Pause ← Start (per-player)
            }
        }

        // Check if VM requested exit or user pressed Back — return to browser
        if (!g_vm->is_running() || g_return_to_browser)
            game_running = false;

        // Check for OSD cart-swap during gameplay — poll .s0 for a
        // path different from the currently-loaded cart. SC0 mode: the
        // user picks a new cart from MiSTer's file browser, MiSTer
        // updates .s0 instantly. Throttle to ~2 Hz so we're not
        // hammering the SD card.
        if (have_native_video && game_running) {
            static int swap_poll = 0;
            if (++swap_poll >= 30) { // ~30 frames @ 60fps = 0.5s
                swap_poll = 0;
                char s0_path[512] = {0};
                FILE *f = fopen("/media/fat/config/PICO-8.s0", "r");
                if (f) {
                    if (fgets(s0_path, sizeof(s0_path), f)) {
                        char *nl = strchr(s0_path, '\n'); if (nl) *nl = 0;
                        char *cr = strchr(s0_path, '\r'); if (cr) *cr = 0;
                        char *cut = NULL; char *p;
                        for (p = strstr(s0_path, ".p8.png"); p; p = strstr(p+1, ".p8.png")) cut = p + 7;
                        if (!cut) for (p = strstr(s0_path, ".p8"); p; p = strstr(p+1, ".p8")) cut = p + 3;
                        if (cut) *cut = 0;
                        int pl = (int)strlen(s0_path);
                        while (pl > 0 && (s0_path[pl-1] == ' ' || s0_path[pl-1] == '\t')) s0_path[--pl] = 0;
                    }
                    fclose(f);
                    if (strlen(s0_path) > 0) {
                        char full[1024];
                        // Same absolute-path detection as the startup poll —
                        // MGL hot-swaps would otherwise double-prefix /media/fat/.
                        if (s0_path[0] == '/')
                            snprintf(full, sizeof(full), "%s", s0_path);
                        else
                            snprintf(full, sizeof(full), "/media/fat/%s", s0_path);
                        if (cart_path != full) {
                            fprintf(stderr, "Hot-swap: new OSD cart %s\n", full);
                            cart_path = std::string(full);
                            game_running = false;
                            hot_swap_pending = true;
                        }
                    }
                }
            }
        }

        g_vm->step(1.0f / target_fps);

        // Render video
        g_vm->render(rgba_buf);

        // ── Golden-master hash trace (-test) ─────────────────────────
        // Hash points mirror tools/z8headless.cpp exactly: video = CRC32
        // over R,G,B of the native 128x128 render output (pre-upscale);
        // audio = CRC32 over this frame's 22050 Hz mono engine output
        // (367/368-sample pacing, hashed pre-upsample so the platform
        // upsampler/ring cannot affect the trace).
        if (g_test_trace) {
            uint32_t vh = 0;
            for (int i = 0; i < PICO8_W * PICO8_H; ++i) {
                uint8_t px[3] = { rgba_buf[i].r, rgba_buf[i].g, rgba_buf[i].b };
                vh = tt_crc32(vh, px, 3);
            }
            int ns = tt_audio_samples_for_frame(g_test_frame, SRC_RATE, DEFAULT_FPS);
            static int16_t tmono[512];
            g_vm->get_audio(tmono, (size_t)ns * sizeof(int16_t));
            uint32_t ah = tt_crc32(0, tmono, (size_t)ns * sizeof(int16_t));
            tt_emit(g_test_trace, g_test_frame, vh, ah);

            // Keep the run audible: upsample + write to the DDR3 ring
            // when there's space, drop when full — the hash was taken
            // pre-upsample, so playback can never affect the trace.
            if (have_native_video && have_audio) {
                static int16_t tstereo[2400];
                int out = upsample_mono_to_stereo(tmono, ns, tstereo, 1200);
                if (NativeVideoWriter_AudioSpace() >= (uint32_t)out)
                    NativeVideoWriter_WriteAudio(tstereo, out);
            }

            g_test_frame++;
            if (g_test_frames_limit && g_test_frame >= g_test_frames_limit) {
                fclose(g_test_trace);
                fprintf(stderr, "[test] trace complete (%lld frames) — exiting\n", g_test_frame);
                exit(0);
            }
        }

        if (have_native_video) {
            // FPGA native path: write 128×128 RGBA8 → DDR3 as RGB565
            // The FPGA reader polls DDR3 and outputs scaled native video
            NativeVideoWriter_WriteFrame(rgba_buf, PICO8_W, PICO8_H);
        } else {
            // Fallback: SDL framebuffer → Linux /dev/fb0 → HDMI scaler
            blit_stretched(screen, rgba_buf);
            SDL_UpdateRect(screen, 0, 0, SCREEN_W, SCREEN_H);
        }

        // Audio is handled by the separate thread — no audio work here
    }

        // Game ended — clean up for next cart
        fprintf(stderr, "=== Game ended (PID=%d) ===\n", getpid());
        if (audio_started) {
            audio_thread_stop();
            audio_started = false;
        }
        g_vm.reset();
        g_return_to_browser = false;

        // Hot-swap: cart_path already set to new cart, outer loop reloads it.
        // Normal exit (user picked Quit from PICO-8 pause menu): clear
        // cart_path AND delete .s0 — otherwise the next outer-loop poll
        // would see the old .s0 path and immediately reload the same cart
        // (perceived as "Quit just resets the cartridge").
        // Reachable only for cart-driven shutdowns (rare — extcmd("shutdown")
        // from a cart) or hot-swap. User-driven Quit-from-pause-menu now
        // exit(0)s directly via the z8_cart_browser extcmd handler above —
        // never reaches this cleanup path. Master_Daemon respawns _handler.sh
        // for the next cart, fresh process inits DDR3 to zero. Same architecture
        // as OpenBOR's Quit. Universal rule: pause-menu Quit must exit(0).
        if (!hot_swap_pending) {
            cart_path.clear();
            unlink("/media/fat/config/PICO-8.s0");
            fprintf(stderr, "Cart-driven shutdown: cleared .s0, will wait for OSD\n");
            // Cart-driven shutdown: explicit DDR3 clear, since binary stays
            // alive (vs Quit which exits and re-inits).
            if (have_native_video)
                NativeVideoWriter_ClearScreen();
        } else {
            fprintf(stderr, "Hot-swap: reloading %s\n", cart_path.c_str());
        }

        // Clear screen before returning to browser
        if (screen) {
            for (int i = 0; i < 3; i++) {
                SDL_FillRect(screen, NULL, 0);
                SDL_Flip(screen);
            }
        }

    } // end of browser/game while loop

    // ── Shutdown ──────────────────────────────────────────────────────
    if (g_sdl_joystick) {
        SDL_JoystickClose(g_sdl_joystick);
        g_sdl_joystick = NULL;
    }
    keepalive_run.store(false);
    if (keepalive_thread.joinable()) keepalive_thread.join();
    NativeVideoWriter_Shutdown();

    SDL_CloseAudio();
    SDL_Quit();

    return 0;
}
