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
//   Button mask bits: 0=left 1=right 2=up 3=down 4=O(z/btn4) 5=X(x/btn5)

#include "pico8/vm.h"
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
    fprintf(stderr, "\n[CRASH] fatal signal %d — backtrace (%d frames):\n", sig, n);
    fflush(stderr);
    backtrace_symbols_fd(bt, n, 2);
    _exit(139);
}

int main(int argc, char **argv)
{
    setvbuf(stderr, NULL, _IOLBF, 0);   // line-buffered so crash output isn't lost
    signal(SIGSEGV, crash_handler);
    signal(SIGABRT, crash_handler);
    signal(SIGFPE,  crash_handler);

    std::string cart, outdir = ".", inputfile, datadir;
    int frames = 120, hold = 0;
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

    auto vm = new z8::pico8::vm();
    // No-op stubs for desktop callbacks (default std::function throws on call).
    vm->registerPointerLockCallback([](bool){});
    vm->registerSetFullscreenCallback([](int){});
    vm->registerGetFullscreenCallback([]() -> std::string { return ""; });
    vm->registerSetFilterCallback([](int v){ return v; });
    vm->registerGetFilterNameCallback([](int) -> std::string { return ""; });

    fprintf(stderr, "[z8headless] loading %s\n", cart.c_str());
    vm->load(cart);
    if (verbose) fprintf(stderr, "[z8headless] loaded OK\n");
    vm->run();
    if (verbose) fprintf(stderr, "[z8headless] run() OK\n");

    std::vector<lol::u8vec4> fb(128 * 128);
    int held = hold;
    for (int fr = 0; fr < frames; ++fr) {
        auto it = script.find(fr);
        if (it != script.end()) held = it->second;
        for (int b = 0; b < 6; ++b) vm->button(0, b, (held >> b) & 1);

        if (verbose && fr < 8) fprintf(stderr, "[z8headless] frame %d\n", fr);
        vm->step(1.0f / 60.0f);
        vm->render(fb.data());

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
    fprintf(stderr, "[z8headless] done (%d frames)\n", frames);
    return 0;
}
