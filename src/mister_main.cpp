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

// ── Audio thread — DDR3 ring buffer writer ───────────────────────────
// Renders 22050Hz mono from zepto8, upsamples to 48KHz stereo,
// writes to DDR3 ring buffer. FPGA reads at 48KHz.
// No ALSA. No kernel. Same path as every MiSTer core.

static pthread_t g_audio_thread;
static volatile bool g_audio_running = false;

// Upsample 22050Hz mono → 48000Hz stereo using nearest-neighbor
// Returns number of stereo output samples written
static int upsample_mono_to_stereo(const int16_t *mono_in, int in_samples,
                                    int16_t *stereo_out, int max_out)
{
    // Fixed-point step: (22050 << 16) / 48000 = 30146
    const uint32_t step = (uint32_t)(((uint64_t)SRC_RATE << 16) / DST_RATE);
    uint32_t accum = 0;
    int out_count = 0;

    while (out_count < max_out) {
        uint32_t src_idx = accum >> 16;
        if (src_idx >= (uint32_t)in_samples) break;
        int16_t s = mono_in[src_idx];
        stereo_out[out_count * 2 + 0] = s;  // Left
        stereo_out[out_count * 2 + 1] = s;  // Right (mono duplicate)
        out_count++;
        accum += step;
    }
    return out_count;
}

static void *audio_thread_func(void *arg)
{
    (void)arg;

    // Pin audio thread to CPU core 1 (main thread runs on core 0)
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(1, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);

    int16_t mono_buf[AUDIO_BUF_SAMPLES];
    // Max output: 512 * 48000/22050 + 2 ≈ 1117 stereo samples
    int16_t stereo_buf[2400];

    fprintf(stderr, "Audio: DDR3 ring buffer, %dHz mono → %dHz stereo\n", SRC_RATE, DST_RATE);
    fflush(stderr);

    int total_calls = 0;
    int nonzero_calls = 0;
    int16_t global_max = 0;

    while (g_audio_running)
    {
        if (!g_vm) break;

        // Render mono audio from zepto8 at 22050Hz
        g_vm->get_audio(mono_buf, AUDIO_BUF_SAMPLES * sizeof(int16_t));
        total_calls++;

        int16_t max_val = 0;
        for (int i = 0; i < AUDIO_BUF_SAMPLES; i++) {
            int16_t av = mono_buf[i] < 0 ? -mono_buf[i] : mono_buf[i];
            if (av > max_val) max_val = av;
        }
        if (max_val > 0) nonzero_calls++;
        if (max_val > global_max) global_max = max_val;

        // Log after each of the first 5 calls (before buffer can fill)
        if (total_calls <= 5) {
            fprintf(stderr, "Audio call %d: max=%d s[0]=%d s[1]=%d s[2]=%d s[3]=%d\n",
                    total_calls, max_val, mono_buf[0], mono_buf[1], mono_buf[2], mono_buf[3]);
            fflush(stderr);
        }

        // Upsample to 48KHz stereo
        int out_samples = upsample_mono_to_stereo(mono_buf, AUDIO_BUF_SAMPLES,
                                                   stereo_buf, 1200);

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
    fprintf(stderr, "  -h          Show this help\n");
}

int main(int argc, char **argv)
{
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
        else if (arg == "-h" || arg == "--help") { print_usage(argv[0]); return 0; }
        else if (arg[0] != '-')              { cart_path = arg; }
        else { fprintf(stderr, "Unknown option: %s\n", arg.c_str()); return 1; }
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

    // Pin main thread to CPU core 0 (audio thread will use core 1)
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
                // SDL cart browser (normal fbcon mode)
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
        // sets flag to return to browser instead of quitting
        g_vm->add_extcmd("z8_cart_browser", [](std::string const &) {
            g_return_to_browser = true;
        });

        g_vm->load(cart_path);
        g_vm->run();
        fprintf(stderr, "=== Game started: %s (PID=%d) ===\n", cart_path.c_str(), getpid());

        // Start audio thread
        bool audio_started = false;
        if (have_audio) {
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
        // The boot.rom pause-menu uses __z8_anybtnp() helpers so that once
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

        // Step the VM
        g_vm->step(1.0f / target_fps);

        // Render video
        g_vm->render(rgba_buf);
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
        if (!hot_swap_pending) {
            cart_path.clear();
            unlink("/media/fat/config/PICO-8.s0");
            fprintf(stderr, "Quit: cleared .s0, returning to OSD picker\n");
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
    NativeVideoWriter_Shutdown();

    SDL_CloseAudio();
    SDL_Quit();

    return 0;
}
