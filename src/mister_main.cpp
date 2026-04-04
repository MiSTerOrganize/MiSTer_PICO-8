//
//  zepto8-mister — MiSTer FPGA frontend for the zepto8 PICO-8 emulator
//
//  Copyright © 2024-2026 MiSTer Organize
//
//  Built on zepto8 by Sam Hocevar (WTFPL license)
//  MiSTer frontend patterns from FAKE-08 MiSTer port session summary
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
#include <dlfcn.h>
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

// ── Signal handler ────────────────────────────────────────────────────

static void signal_handler(int sig)
{
    (void)sig;
    g_running = false;
}

// ══════════════════════════════════════════════════════════════════════
//  ALSA via dlopen — Direct audio, bypassing SDL
//  Uses explicit /usr/lib/libasound.so.2 path (proven on MiSTer)
//  RTLD_NOW for immediate symbol resolution
// ══════════════════════════════════════════════════════════════════════

typedef void* snd_pcm_t;
typedef unsigned long snd_pcm_uframes_t;

static void *alsa_lib = nullptr;

static int  (*p_snd_pcm_open)(snd_pcm_t**, const char*, int, int) = nullptr;
static int  (*p_snd_pcm_set_params)(snd_pcm_t*, int, int, unsigned int,
                                     unsigned int, int, unsigned int) = nullptr;
static long (*p_snd_pcm_writei)(snd_pcm_t*, const void*, snd_pcm_uframes_t) = nullptr;
static int  (*p_snd_pcm_recover)(snd_pcm_t*, int, int) = nullptr;
static int  (*p_snd_pcm_close)(snd_pcm_t*) = nullptr;
static int  (*p_snd_pcm_prepare)(snd_pcm_t*) = nullptr;
static const char* (*p_snd_strerror)(int) = nullptr;

static snd_pcm_t *g_pcm = nullptr;

#define SND_PCM_STREAM_PLAYBACK     0
#define SND_PCM_FORMAT_S16_LE       2
#define SND_PCM_ACCESS_RW_INTERLEAVED 3

static bool alsa_init()
{
    // Explicit path — proven working on MiSTer Buildroot Linux
    alsa_lib = dlopen("/usr/lib/libasound.so.2", RTLD_NOW);
    if (!alsa_lib) {
        fprintf(stderr, "ALSA: cannot load /usr/lib/libasound.so.2: %s\n", dlerror());
        return false;
    }

    p_snd_pcm_open       = (decltype(p_snd_pcm_open))dlsym(alsa_lib, "snd_pcm_open");
    p_snd_pcm_set_params  = (decltype(p_snd_pcm_set_params))dlsym(alsa_lib, "snd_pcm_set_params");
    p_snd_pcm_writei     = (decltype(p_snd_pcm_writei))dlsym(alsa_lib, "snd_pcm_writei");
    p_snd_pcm_recover    = (decltype(p_snd_pcm_recover))dlsym(alsa_lib, "snd_pcm_recover");
    p_snd_pcm_close      = (decltype(p_snd_pcm_close))dlsym(alsa_lib, "snd_pcm_close");
    p_snd_pcm_prepare    = (decltype(p_snd_pcm_prepare))dlsym(alsa_lib, "snd_pcm_prepare");
    p_snd_strerror       = (decltype(p_snd_strerror))dlsym(alsa_lib, "snd_strerror");

    if (!p_snd_pcm_open || !p_snd_pcm_set_params || !p_snd_pcm_writei ||
        !p_snd_pcm_recover || !p_snd_pcm_close || !p_snd_pcm_prepare) {
        fprintf(stderr, "ALSA: missing symbols\n");
        dlclose(alsa_lib); alsa_lib = nullptr;
        return false;
    }

    fprintf(stderr, "ALSA: loaded /usr/lib/libasound.so.2\n");

    // Try device names in order: default, hw:0,0, plughw:0,0
    const char *devices[] = { "default", "hw:0,0", "plughw:0,0", nullptr };
    int err = -1;
    for (int i = 0; devices[i]; ++i) {
        err = p_snd_pcm_open(&g_pcm, devices[i], SND_PCM_STREAM_PLAYBACK, 0);
        if (err == 0) {
            fprintf(stderr, "ALSA: opened device '%s'\n", devices[i]);
            break;
        }
    }
    if (err < 0) {
        fprintf(stderr, "ALSA: cannot open any device\n");
        return false;
    }

    // S16_LE, mono, 22050Hz, allow resampling, 100ms latency
    err = p_snd_pcm_set_params(g_pcm,
                                SND_PCM_FORMAT_S16_LE,
                                SND_PCM_ACCESS_RW_INTERLEAVED,
                                AUDIO_CHANNELS,
                                AUDIO_RATE,
                                1,       // allow resampling
                                100000); // 100ms latency in microseconds
    if (err < 0) {
        fprintf(stderr, "ALSA: cannot set params\n");
        p_snd_pcm_close(g_pcm); g_pcm = nullptr;
        return false;
    }

    fprintf(stderr, "ALSA: mono %dHz S16LE ready\n", AUDIO_RATE);
    return true;
}

static void alsa_shutdown()
{
    if (g_pcm)   { p_snd_pcm_close(g_pcm); g_pcm = nullptr; }
    if (alsa_lib) { dlclose(alsa_lib); alsa_lib = nullptr; }
}

// ── Audio thread ─────────────────────────────────────────────────────
// snd_pcm_writei in blocking mode provides correct pacing at 22050Hz.
// Do NOT add usleep — it causes audio to play slow due to oversleep.

static pthread_t g_audio_thread;
static volatile bool g_audio_running = false;

static void *audio_thread_func(void *arg)
{
    (void)arg;

    // Pin audio thread to CPU core 1 (main thread runs on core 0)
    // This was a critical fix in the FAKE-08 project — without it,
    // both threads fight for the same core causing stuttering.
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(1, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);

    int16_t buffer[AUDIO_BUF_SAMPLES];

    while (g_audio_running)
    {
        // Null checks for quit safety
        if (!g_vm || !g_pcm) break;

        // Fill audio buffer from VM
        g_vm->get_audio(buffer, AUDIO_BUF_SAMPLES * sizeof(int16_t));

        // Write to ALSA — snd_pcm_writei blocks until ALSA accepts the samples.
        // ALSA's internal clock at 22050Hz provides correct pacing.
        // Do NOT add usleep — it causes audio to play ~8% slow.
        long written = p_snd_pcm_writei(g_pcm, buffer, (snd_pcm_uframes_t)AUDIO_BUF_SAMPLES);
        if (written < 0) {
            p_snd_pcm_recover(g_pcm, (int)written, 1);
            p_snd_pcm_prepare(g_pcm);
            p_snd_pcm_writei(g_pcm, buffer, (snd_pcm_uframes_t)AUDIO_BUF_SAMPLES);
        }
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
    fprintf(stderr, "Audio thread launched\n");
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
// We immediately close SDL audio after init so it doesn't compete
// with our direct ALSA output.

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
// Button mapping matches FAKE-08 MiSTer (verified on hardware):
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

    // Set ZEPTO8_BASE_DIR for save/config path resolution.
    // With __MISTER__ defined, private.cpp uses this for flat paths:
    //   Saves:  $ZEPTO8_BASE_DIR/Saves/cartname.p8d.txt
    //   Config: $ZEPTO8_BASE_DIR/config.txt
    //   Carts:  $ZEPTO8_BASE_DIR/Carts/
    setenv("ZEPTO8_BASE_DIR", data_dir.c_str(), 1);

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

    // ── SDL audio init (for video stability) then close ───────────────
    // SDL_OpenAudio with real callback required for internal timer/event
    // state. Close immediately so it doesn't compete with ALSA.
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
            SDL_CloseAudio(); // Close immediately — prevents competing with ALSA
        }
    }

    // ── Init direct ALSA audio ────────────────────────────────────────
    bool have_audio = false;
    if (enable_sound)
        have_audio = alsa_init();

    // ── Init native video DDR3 writer (for FPGA native output) ────────
    // Only when -nativevideo flag is passed (requires PICO-8 FPGA core loaded).
    // Without the flag, video always goes through SDL fbcon as before.
    bool have_native_video = false;
    if (enable_native_video) {
        have_native_video = NativeVideoWriter_Init();
        if (have_native_video)
            fprintf(stderr, "Native video: DDR3 writer active (128x128 RGB565)\n");
        else
            fprintf(stderr, "Native video: DDR3 init failed, falling back to SDL\n");
    }

    // Joystick is handled by SDL (opened after SDL_Init above)
    // Cart browser opens /dev/input/js0 directly when needed

    // ── Cart browser / game loop ──────────────────────────────────────
    // Normal mode: show visual cart browser
    // Native video mode: wait for cart from OSD file browser via DDR3

    std::string carts_dir = data_dir + "Carts";

    while (g_running)
    {
        // Get cart path
        if (cart_path.empty()) {
            if (have_native_video) {
                // Poll DDR3 for cart loaded via OSD file browser
                fprintf(stderr, "Waiting for cart from OSD file browser...\n");
                int poll_count = 0;
                while (g_running) {
                    uint32_t cart_size = NativeVideoWriter_CheckCart();
                    
                    // Debug: log every second
                    if (++poll_count >= 60) {
                        fprintf(stderr, "Cart poll: ctrl=0x%08X\n", cart_size);
                        poll_count = 0;
                    }
                    
                    if (cart_size > 0) {
                        // Ignore tiny sizes — startup ioctl noise from Main_MiSTer
                        if (cart_size < 64) {
                            NativeVideoWriter_AckCart();
                            continue;
                        }
                        fprintf(stderr, "Cart received: %u bytes\n", cart_size);

                        // Read cart data from DDR3
                        uint8_t *cart_buf = (uint8_t *)malloc(cart_size);
                        if (!cart_buf) {
                            fprintf(stderr, "Failed to allocate %u bytes for cart\n", cart_size);
                            NativeVideoWriter_AckCart();
                            continue;
                        }
                        uint32_t actual = NativeVideoWriter_ReadCart(cart_buf, cart_size);
                        NativeVideoWriter_AckCart();

                        // Detect format: PNG starts with 0x89504E47
                        const char *tmp_path;
                        if (actual >= 8 && cart_buf[0] == 0x89 && cart_buf[1] == 0x50 &&
                            cart_buf[2] == 0x4E && cart_buf[3] == 0x47)
                            tmp_path = "/tmp/pico8_osd_cart.p8.png";
                        else
                            tmp_path = "/tmp/pico8_osd_cart.p8";

                        // Save to temp file
                        FILE *f = fopen(tmp_path, "wb");
                        if (f) {
                            fwrite(cart_buf, 1, actual, f);
                            fclose(f);
                            cart_path = std::string(tmp_path);
                            fprintf(stderr, "Cart saved to %s\n", tmp_path);
                        } else {
                            fprintf(stderr, "Failed to write temp cart file\n");
                        }
                        free(cart_buf);
                        break;
                    }

                    // Keep rendering black frame so FPGA has valid timing
                    usleep(16000); // ~60fps polling
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

        // Register cart browser extcmd — when selected from pause menu,
        // sets flag to return to browser instead of quitting
        g_vm->add_extcmd("z8_cart_browser", [](std::string const &) {
            g_return_to_browser = true;
        });

        g_vm->load(cart_path);
        g_vm->run();

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

        // ── Input ────────────────────────────────────────────────────
        if (have_native_video) {
            // Read joystick from DDR3 — FPGA writes hps_io joystick_0 here.
            // Uses MiSTer's standard button mapping (configured via OSD).
            // Bit layout: 0=right, 1=left, 2=down, 3=up, 4=O, 5=X, 6=Pause
            uint32_t joy = NativeVideoWriter_ReadJoystick();

            g_vm->button(0, 0, (joy >> 1) & 1);  // bit 1 = left  → PICO-8 btn 0
            g_vm->button(0, 1, (joy >> 0) & 1);  // bit 0 = right → PICO-8 btn 1
            g_vm->button(0, 2, (joy >> 3) & 1);  // bit 3 = up    → PICO-8 btn 2
            g_vm->button(0, 3, (joy >> 2) & 1);  // bit 2 = down  → PICO-8 btn 3
            g_vm->button(0, 4, (joy >> 4) & 1);  // bit 4 = O     → PICO-8 btn 4 (Xbox A)
            g_vm->button(0, 5, (joy >> 5) & 1);  // bit 5 = X     → PICO-8 btn 5 (Xbox B)
            g_vm->button(0, 6, (joy >> 7) & 1);  // bit 7 = Pause → PICO-8 btn 6 (Xbox Start)
        } else {
            // SDL input path (normal fbcon mode)
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) g_running = false;
            // Button PRESSES via SDL joystick events
            if (ev.type == SDL_JOYBUTTONDOWN) {
                if (ev.jbutton.button == 0) g_vm->button(0, 4, 1);      // A → O
                else if (ev.jbutton.button == 2) g_vm->button(0, 5, 1); // X → X
                else if (ev.jbutton.button == 7) g_vm->button(0, 6, 1); // Start → Pause
                else if (ev.jbutton.button == 6 || ev.jbutton.button == 8) g_running = false; // Back/Guide
            }
            if (ev.type == SDL_JOYBUTTONUP) {
                if (ev.jbutton.button == 0) g_vm->button(0, 4, 0);
                else if (ev.jbutton.button == 2) g_vm->button(0, 5, 0);
                else if (ev.jbutton.button == 7) g_vm->button(0, 6, 0);
            }
            // Keyboard buttons (only without gamepad)
            if (!g_joystick_connected) {
                if (ev.type == SDL_KEYDOWN) {
                    SDLKey key = ev.key.keysym.sym;
                    if (key == SDLK_z || key == SDLK_SPACE)       g_vm->button(0, 4, 1);
                    else if (key == SDLK_x || key == SDLK_LALT)   g_vm->button(0, 5, 1);
                    else if (key == SDLK_RETURN)                   g_vm->button(0, 6, 1);
                    else if (key == SDLK_ESCAPE || key == SDLK_F12) g_running = false;
                }
                if (ev.type == SDL_KEYUP) {
                    SDLKey key = ev.key.keysym.sym;
                    if (key == SDLK_z || key == SDLK_SPACE)       g_vm->button(0, 4, 0);
                    else if (key == SDLK_x || key == SDLK_LALT)   g_vm->button(0, 5, 0);
                    else if (key == SDLK_RETURN)                   g_vm->button(0, 6, 0);
                }
            }
        }

        // DIRECTIONS: poll held state every frame (not events)
        // This is how FAKE-08 does it — gives continuous hold with zero latency
        int dir_left = 0, dir_right = 0, dir_up = 0, dir_down = 0;

        // Keyboard arrow state (d-pad comes as keyboard arrows on MiSTer)
        const Uint8 *keystate = SDL_GetKeyState(NULL);
        if (keystate[SDLK_LEFT])  dir_left  = 1;
        if (keystate[SDLK_RIGHT]) dir_right = 1;
        if (keystate[SDLK_UP])    dir_up    = 1;
        if (keystate[SDLK_DOWN])  dir_down  = 1;

        // Joystick hat (d-pad)
        if (g_sdl_joystick) {
            Uint8 hat = SDL_JoystickGetHat(g_sdl_joystick, 0);
            if (hat & SDL_HAT_LEFT)  dir_left  = 1;
            if (hat & SDL_HAT_RIGHT) dir_right = 1;
            if (hat & SDL_HAT_UP)    dir_up    = 1;
            if (hat & SDL_HAT_DOWN)  dir_down  = 1;

            // Analog stick
            Sint16 ax = SDL_JoystickGetAxis(g_sdl_joystick, 0);
            Sint16 ay = SDL_JoystickGetAxis(g_sdl_joystick, 1);
            if (ax < -8000) dir_left  = 1;
            if (ax >  8000) dir_right = 1;
            if (ay < -8000) dir_up    = 1;
            if (ay >  8000) dir_down  = 1;
        }

        g_vm->button(0, 0, dir_left);
        g_vm->button(0, 1, dir_right);
        g_vm->button(0, 2, dir_up);
        g_vm->button(0, 3, dir_down);
        } // end SDL input path

        // Check if VM requested exit or user pressed Back — return to browser
        if (!g_vm->is_running() || g_return_to_browser)
            game_running = false;

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
        if (audio_started)
            audio_thread_stop();
        g_vm.reset();

        // Reset flags for next game
        cart_path.clear();
        g_return_to_browser = false;

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
    alsa_shutdown();

    SDL_CloseAudio();
    SDL_Quit();

    return 0;
}
