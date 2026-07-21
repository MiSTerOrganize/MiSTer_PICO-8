// z8headless — headless PICO-8 frame dumper for the differential bug-finding
// harness. Links only zepto8core (engine), no SDL / no MiSTer hardware. Loads a
// cart, runs N frames (optionally with a held button mask or a per-frame input
// script), and dumps selected frames as PNG. Used to diff our engine fork
// against stock zepto8 / a reference, and to reproduce cart bugs off-device.
//
// Build: cmake -B build -DBUILD_HEADLESS=ON -DSKIP_SDL_FRONTEND=ON && cmake --build build
//
// Usage:
//   z8headless --cart C.p8 --frames N --dump all|F1,F2,.. --out DIR
//              [--hold MASK]          constant P1 button mask every frame
//              [--input SCRIPT]       per-frame input: "FRAME MASK" lines
//              [--test TRACEFILE]     golden-master hash trace: one
//                                     FRAME:VIDEOCRC:AUDIOCRC line per frame
//                                     (see src/test_trace.h for the format)
//   Button mask bits: 0=left 1=right 2=up 3=down 4=O(z/btn4) 5=X(x/btn5)

#include "pico8/vm.h"
#include "test_trace.h"
#include "lol/sys/init.h"   // lol::sys::set_data_path (BIOS/cart path prefix)
#include "lodepng.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <csignal>
#include <execinfo.h>
#include <unistd.h>

using namespace z8::pico8;

// Crash triage: on a fatal signal, dump a backtrace (offsets) so a segfaulting
// cart can be localized. On a fully-static non-PIE binary the offsets are
// absolute addresses -> resolve with addr2line -e z8headless <addr>.
static void crash_handler(int sig)
{
    void *bt[40];
    int n = backtrace(bt, 40);
    if (sig == SIGALRM)
        fprintf(stderr, "\n[HANG] wall-clock alarm fired (likely C++ engine "
                "infinite loop) — backtrace (%d frames):\n", n);
    else
        fprintf(stderr, "\n[CRASH] fatal signal %d — backtrace (%d frames):\n", sig, n);
    fflush(stderr);
    backtrace_symbols_fd(bt, n, 2);
    _exit(sig == SIGALRM ? 98 : 139);
}

int main(int argc, char **argv)
{
    setvbuf(stderr, NULL, _IOLBF, 0);   // line-buffered so crash output isn't lost
    signal(SIGSEGV, crash_handler);
    signal(SIGABRT, crash_handler);
    signal(SIGFPE,  crash_handler);
    signal(SIGALRM, crash_handler);   // wall-clock hang catcher (C++ engine loops)

    std::string cart, outdir = ".", inputfile, datadir, dumpcode, tracefile;
    int frames = 120, hold = 0, alarm_secs = 0;
    uint64_t watchdog = 0;   // 0 = off; abort+dump stuck stack if a step exceeds N instr
    std::set<int> dump;
    bool dump_all = false, verbose = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : std::string(); };
        if      (a == "--cart")    cart    = next();
        else if (a == "--frames")  frames  = atoi(next().c_str());
        else if (a == "--out")     outdir  = next();
        else if (a == "--hold")    hold    = atoi(next().c_str());
        else if (a == "--input")   inputfile = next();
        else if (a == "--datadir") datadir = next();   // dir containing bios.p8
        else if (a == "--verbose") verbose = true;     // print load/run/frame markers
        else if (a == "--dumpcode") dumpcode = next();  // write decompressed cart code, then exit
        else if (a == "--test")     tracefile = next(); // golden-master hash trace output
        else if (a == "--watchdog") watchdog = strtoull(next().c_str(), nullptr, 10); // Lua runaway-loop guard
        else if (a == "--alarm")    alarm_secs = atoi(next().c_str()); // wall-clock hang backtrace (C++ loops)
        else if (a == "--dump") {
            std::string s = next();
            if (s == "all") dump_all = true;
            else {
                size_t p = 0;
                while (p <= s.size()) {
                    size_t c = s.find(',', p);
                    std::string t = s.substr(p, c == std::string::npos ? std::string::npos : c - p);
                    if (!t.empty()) dump.insert(atoi(t.c_str()));
                    if (c == std::string::npos) break;
                    p = c + 1;
                }
            }
        }
    }
    if (cart.empty()) {
        fprintf(stderr, "usage: z8headless --cart C --frames N --dump all|list --out DIR [--hold MASK] [--input SCRIPT]\n");
        return 2;
    }

    // Optional per-frame input script: lines of "FRAME MASK".
    std::map<int,int> script;
    if (!inputfile.empty()) {
        FILE *f = fopen(inputfile.c_str(), "r");
        if (f) { int fr, mk; while (fscanf(f, "%d %d", &fr, &mk) == 2) script[fr] = mk; fclose(f); }
        else fprintf(stderr, "warn: cannot open input script %s\n", inputfile.c_str());
    }

    // BIOS (bios.p8) + relative cart paths resolve via this prefix. Must be set
    // before vm creation (the BIOS loads in the vm constructor).
    if (!datadir.empty()) lol::sys::set_data_path(datadir);

    // Trace mode: force the deterministic PRNG seed BEFORE vm creation
    // (private_init_ram seeds in the constructor). Without this, the
    // per-boot wall-clock rnd() seed makes traces nondeterministic for
    // any cart that calls rnd() during boot.
    if (!tracefile.empty()) setenv("Z8_TEST_SEED", "1", 0);

    auto vm = new z8::pico8::vm();
    if (watchdog) { vm->set_watchdog(watchdog); fprintf(stderr, "[z8headless] watchdog=%llu instr/step\n", (unsigned long long)watchdog); }
    // No-op stubs for desktop callbacks (default std::function throws on call).
    vm->registerPointerLockCallback([](bool){});
    vm->registerSetFullscreenCallback([](int){});
    vm->registerGetFullscreenCallback([]() -> std::string { return ""; });
    vm->registerSetFilterCallback([](int v){ return v; });
    vm->registerGetFilterNameCallback([](int) -> std::string { return ""; });

    fprintf(stderr, "[z8headless] loading %s\n", cart.c_str());
    vm->load(cart);
    if (verbose) fprintf(stderr, "[z8headless] loaded OK\n");

    // --dumpcode: write the cart code AFTER #include expansion (preprocess_code,
    // what the engine actually runs) for diffing against PICO-8 ground truth.
    // Covers both the decompressor (.p8.png/ROM) AND the #include preprocessor
    // (.p8 text) stages. Exit before run().
    if (!dumpcode.empty()) {
        std::string code = vm->get_preprocessed_code();
        FILE *cf = fopen(dumpcode.c_str(), "wb");
        if (cf) { fwrite(code.data(), 1, code.size(), cf); fclose(cf); }
        fprintf(stderr, "[z8headless] dumped %zu bytes of preprocessed code -> %s\n", code.size(), dumpcode.c_str());
        return 0;
    }

    vm->run();
    if (verbose) fprintf(stderr, "[z8headless] run() OK\n");

    // --test: golden-master hash trace (frame:videocrc:audiocrc per frame).
    // Fully buffered — flushed once at fclose, not per line.
    FILE *tracef = nullptr;
    if (!tracefile.empty()) {
        tracef = fopen(tracefile.c_str(), "w");
        if (!tracef) { fprintf(stderr, "err: cannot open trace file %s\n", tracefile.c_str()); return 2; }
        setvbuf(tracef, NULL, _IOFBF, 65536);
    }

    std::vector<lol::u8vec4> fb(128 * 128);
    int held = hold;
    for (int fr = 0; fr < frames; ++fr) {
        auto it = script.find(fr);
        if (it != script.end()) held = it->second;
        for (int b = 0; b < 6; ++b) vm->button(0, b, (held >> b) & 1);

        if (verbose && fr < 8) fprintf(stderr, "[z8headless] frame %d\n", fr);
        if (alarm_secs) alarm(alarm_secs); // re-arm each frame; fires if one step() hangs
        vm->step(1.0f / 60.0f);
        vm->render(fb.data(), fb.size());

        if (tracef) {
            // Video: CRC32 over R,G,B of the native 128x128 render output
            // (alpha excluded). Member access keeps it layout-agnostic.
            uint32_t vh = 0;
            for (int i = 0; i < 128 * 128; ++i) {
                uint8_t px[3] = { fb[i].r, fb[i].g, fb[i].b };
                vh = tt_crc32(vh, px, 3);
            }
            // Audio: this frame's share of 22050 Hz mono engine output
            // (367/368 alternating — long-run exact), hashed pre-upsample.
            int ns = tt_audio_samples_for_frame(fr, 22050, 60);
            int16_t amono[512];
            vm->get_audio(amono, (size_t)ns * sizeof(int16_t));
            uint32_t ah = tt_crc32(0, amono, (size_t)ns * sizeof(int16_t));
            tt_emit(tracef, fr, vh, ah);
        }

        if (dump_all || dump.count(fr)) {
            std::vector<unsigned char> img(128 * 128 * 4);
            for (int i = 0; i < 128 * 128; ++i) {
                img[i*4+0] = fb[i].r; img[i*4+1] = fb[i].g;
                img[i*4+2] = fb[i].b; img[i*4+3] = 255;
            }
            char path[1024];
            snprintf(path, sizeof(path), "%s/frame_%05d.png", outdir.c_str(), fr);
            unsigned err = lodepng::encode(path, img, 128, 128);
            if (err) fprintf(stderr, "png err %u: %s\n", err, lodepng_error_text(err));
            else     printf("wrote %s\n", path);
        }
    }
    if (alarm_secs) alarm(0);   // disarm before clean exit
    if (tracef) {
        fclose(tracef);
        fprintf(stderr, "[z8headless] trace written -> %s\n", tracefile.c_str());
    }
    fprintf(stderr, "[z8headless] done (%d frames)\n", frames);
    return 0;
}
