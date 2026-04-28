//
//  cart_browser.h — Visual cart browser for zepto8-mister
//
//  Controls:
//    D-pad Up/Down  — scroll through items
//    D-pad Right    — enter folder
//    D-pad Left     — go back to parent folder
//    A (0) or X (2) — launch cart / enter folder
//    B (1)          — go back
//    Back (6)       — quit
//    Guide (8)      — quit
//

#pragma once

#include <SDL/SDL.h>
#include <string>
#include <vector>
#include <algorithm>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <linux/joystick.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>

// PICO-8 palette (RGB565)
static const uint16_t P8_BLACK      = 0x0000;
static const uint16_t P8_DARK_BLUE  = 0x1968;
static const uint16_t P8_DARK_PURPLE= 0x7928;
static const uint16_t P8_DARK_GREEN = 0x0428;
static const uint16_t P8_LIGHT_GRAY = 0xC618;
static const uint16_t P8_WHITE      = 0xFFBD;
static const uint16_t P8_YELLOW     = 0xFF64;
static const uint16_t P8_ORANGE     = 0xFD00;
static const uint16_t P8_PINK       = 0xFBB5;
static const uint16_t P8_INDIGO     = 0x8393;

// ── Directory scanning ────────────────────────────────────────────────

struct BrowserEntry {
    std::string name;
    std::string full_path;
    bool is_folder;
};

static bool entry_sort(const BrowserEntry &a, const BrowserEntry &b) {
    if (a.is_folder != b.is_folder) return a.is_folder;
    return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
}

static std::vector<BrowserEntry> scan_directory(const std::string &path)
{
    std::vector<BrowserEntry> entries;
    DIR *dir = opendir(path.c_str());
    if (!dir) return entries;

    struct dirent *ent;
    while ((ent = readdir(dir)) != nullptr)
    {
        std::string name = ent->d_name;
        if (name == "." || name == "..") continue;

        std::string full = path;
        if (full.back() != '/') full += '/';
        full += name;

        struct stat st;
        if (stat(full.c_str(), &st) != 0) continue;

        BrowserEntry e;
        e.name = name;
        e.full_path = full;
        e.is_folder = S_ISDIR(st.st_mode);

        if (e.is_folder) {
            entries.push_back(e);
        } else {
            std::string lower = name;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            if (lower.size() >= 3 && lower.substr(lower.size()-3) == ".p8")
                entries.push_back(e);
            else if (lower.size() >= 7 && lower.substr(lower.size()-7) == ".p8.png")
                entries.push_back(e);
        }
    }
    closedir(dir);
    std::sort(entries.begin(), entries.end(), entry_sort);
    return entries;
}

// ── Tiny 4×6 text renderer ───────────────────────────────────────────

static void draw_char_4x6(uint16_t *pixels, int pitch16, int x, int y,
                           char ch, uint16_t color, int sw, int sh)
{
    static const uint32_t font_data[96] = {
        0x000000,0x444040,0xAA0000,0xAEAEA0,0x4E8E40,0xA2484A,
        0x4A4CA6,0x440000,0x248420,0x842480,0x0A4A00,0x04E400,
        0x000048,0x00E000,0x000040,0x224880,
        0x4AA4A4,0x4C4444,0xE2E8E0,0xE2E2E0,0xAAE220,0xE8E2E0,
        0xE8EAE0,0xE24480,0xEAEAE0,0xEAE2E0,0x040400,0x040480,
        0x248420,0x0E0E00,0x842480,0xE24040,0x4A6C84,
        0x4AEAA0,0xCACAC0,0x688860,0xCAAAAC,0xE8C8E0,0xE8C880,
        0x68EA60,0xAAEAA0,0xE44440,0x222A40,0xAACCA0,0x8888E0,
        0xAEEAA0,0xAEEEA0,0x4AAA40,0xEAE880,0x4AAA60,0xEAECA0,
        0x684260,0xE44440,0xAAAA60,0xAAAA40,0xAAEEA0,0xAA4AA0,
        0xAA4440,0xE248E0,0x644460,0x884220,0x622260,0x4A0000,
        0x0000E0,0x840000,
        0x4AEAA0,0xCACAC0,0x688860,0xCAAAAC,0xE8C8E0,0xE8C880,
        0x68EA60,0xAAEAA0,0xE44440,0x222A40,0xAACCA0,0x8888E0,
        0xAEEAA0,0xAEEEA0,0x4AAA40,0xEAE880,0x4AAA60,0xEAECA0,
        0x684260,0xE44440,0xAAAA60,0xAAAA40,0xAAEEA0,0xAA4AA0,
        0xAA4440,0xE248E0,0x644460,0x444440,0xC22260,0x060000,
        0xEEEEE0,
    };
    int idx = (int)(unsigned char)ch - 32;
    if (idx < 0 || idx >= 96) return;
    uint32_t g = font_data[idx];
    for (int row = 0; row < 6; row++) {
        int nibble = (g >> (20 - row * 4)) & 0xF;
        for (int col = 0; col < 4; col++) {
            if (nibble & (0x8 >> col)) {
                int px = x + col, py = y + row;
                if (px >= 0 && px < sw && py >= 0 && py < sh)
                    pixels[py * pitch16 + px] = color;
            }
        }
    }
}

static void draw_text(uint16_t *px, int p, int x, int y,
                      const char *t, uint16_t c, int sw, int sh)
{
    for (int i = 0; t[i]; i++)
        draw_char_4x6(px, p, x + i * 5, y, t[i], c, sw, sh);
}

static std::string truncate_str(const std::string &s, int max_chars)
{
    if ((int)s.size() <= max_chars) return s;
    return s.substr(0, max_chars - 2) + "..";
}

static std::string strip_ext(const std::string &name)
{
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    if (lower.size() >= 7 && lower.substr(lower.size()-7) == ".p8.png")
        return name.substr(0, name.size()-7);
    if (lower.size() >= 3 && lower.substr(lower.size()-3) == ".p8")
        return name.substr(0, name.size()-3);
    return name;
}

// ── Blit 128×128 to SDL surface ───────────────────────────────────────

static void blit_browser(SDL_Surface *surface, uint16_t *buf128)
{
    if (SDL_MUSTLOCK(surface)) SDL_LockSurface(surface);
    uint16_t *dst = (uint16_t *)surface->pixels;
    int dpitch = surface->pitch / 2;
    int sw = surface->w, sh = surface->h;
    int scale_h = sh, scale_w = scale_h;
    int off_x = (sw - scale_w) / 2;
    memset(dst, 0, surface->pitch * sh);
    for (int dy = 0; dy < scale_h; dy++) {
        int sy = dy * 128 / scale_h;
        for (int dx = 0; dx < scale_w; dx++) {
            int sx = dx * 128 / scale_w;
            dst[dy * dpitch + off_x + dx] = buf128[sy * 128 + sx];
        }
    }
    if (SDL_MUSTLOCK(surface)) SDL_UnlockSurface(surface);
}

// ── Timing helper ─────────────────────────────────────────────────────

static uint64_t browser_time_ms()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL;
}

// ── Main browser ──────────────────────────────────────────────────────
// Returns full path of selected cart, or "" if user quit.

static std::string run_cart_browser(SDL_Surface *surface, const std::string &carts_dir,
                                     int joy_fd)
{
    std::vector<std::string> dir_stack;
    dir_stack.push_back(carts_dir);
    int cursor = 0;
    int scroll_offset = 0;
    const int VISIBLE_ITEMS = 14;

    uint16_t buf128[128 * 128];
    std::vector<BrowserEntry> entries = scan_directory(carts_dir);

    // Axis edge detection — track each axis separately
    int prev_axis_y1 = 0; // axis 1 (analog stick Y)
    int prev_axis_y7 = 0; // axis 7 (hat Y)
    int prev_axis_x0 = 0; // axis 0 (analog stick X)
    int prev_axis_x6 = 0; // axis 6 (hat X)

    // D-pad repeat: initial delay then repeat rate
    uint64_t last_move_time = 0;
    const uint64_t REPEAT_DELAY_MS = 300; // ms before repeat starts
    const uint64_t REPEAT_RATE_MS = 120;  // ms between repeats
    int held_dir_y = 0; // current held direction
    bool first_repeat = true;

    bool need_redraw = true;

    // Helper lambdas for folder navigation
    auto enter_folder = [&]() {
        if (!entries.empty() && entries[cursor].is_folder) {
            dir_stack.push_back(entries[cursor].full_path);
            entries = scan_directory(dir_stack.back());
            cursor = 0; scroll_offset = 0; need_redraw = true;
        } else if (!entries.empty() && !entries[cursor].is_folder) {
            // Launch cart — return path
            return entries[cursor].full_path;
        }
        return std::string();
    };

    auto go_back = [&]() {
        if (dir_stack.size() > 1) {
            dir_stack.pop_back();
            entries = scan_directory(dir_stack.back());
            cursor = 0; scroll_offset = 0; need_redraw = true;
        }
    };

    auto move_cursor = [&](int dir) {
        cursor += dir;
        if (entries.empty()) cursor = 0;
        else {
            if (cursor < 0) cursor = (int)entries.size() - 1;
            if (cursor >= (int)entries.size()) cursor = 0;
        }
        if (cursor < scroll_offset) scroll_offset = cursor;
        if (cursor >= scroll_offset + VISIBLE_ITEMS)
            scroll_offset = cursor - VISIBLE_ITEMS + 1;
        need_redraw = true;
        last_move_time = browser_time_ms();
    };

    // Track keyboard arrow key state for edge detection
    bool key_up_held = false;
    bool key_down_held = false;
    bool key_left_held = false;
    bool key_right_held = false;

    while (true)
    {
        // ── SDL events ────────────────────────────────────────────────
        // When gamepad is connected, block ALL keyboard events.
        // MiSTer remaps controller buttons to keyboard keys
        // (B→Escape, A→Enter, X→X key, etc.)
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) return "";
            if (joy_fd < 0) {
                // Keyboard-only mode (no gamepad)
                if (ev.type == SDL_KEYDOWN) {
                    switch (ev.key.keysym.sym) {
                        case SDLK_UP:
                            if (!key_up_held) { move_cursor(-1); held_dir_y = -1; first_repeat = true; }
                            key_up_held = true;
                            break;
                        case SDLK_DOWN:
                            if (!key_down_held) { move_cursor(1); held_dir_y = 1; first_repeat = true; }
                            key_down_held = true;
                            break;
                        case SDLK_RIGHT:
                            if (!key_right_held) {
                                std::string r = enter_folder();
                                if (!r.empty()) return r;
                            }
                            key_right_held = true;
                            break;
                        case SDLK_LEFT:
                            if (!key_left_held) go_back();
                            key_left_held = true;
                            break;
                        case SDLK_x:
                        case SDLK_RETURN: {
                            std::string r = enter_folder();
                            if (!r.empty()) return r;
                            break;
                        }
                        case SDLK_z:
                            go_back(); break;
                        case SDLK_ESCAPE:
                        case SDLK_F12:
                            return "";
                        default: break;
                    }
                }
                else if (ev.type == SDL_KEYUP) {
                    switch (ev.key.keysym.sym) {
                        case SDLK_UP:
                            key_up_held = false;
                            if (!key_down_held) held_dir_y = 0;
                            break;
                        case SDLK_DOWN:
                            key_down_held = false;
                            if (!key_up_held) held_dir_y = 0;
                            break;
                        case SDLK_LEFT:  key_left_held = false; break;
                        case SDLK_RIGHT: key_right_held = false; break;
                        default: break;
                    }
                }
            }
        }

        // ── Joystick events with edge detection ───────────────────────
        if (joy_fd >= 0) {
            struct js_event jev;
            while (read(joy_fd, &jev, sizeof(jev)) == sizeof(jev)) {
                jev.type &= ~JS_EVENT_INIT;

                if (jev.type == JS_EVENT_BUTTON && jev.value) {
                    switch (jev.number) {
                        case 0: { // A — enter folder / launch cart
                            std::string r = enter_folder();
                            if (!r.empty()) return r;
                            break;
                        }
                        case 1: break;                      // B — nothing
                        case 2: go_back(); break;           // X — go back
                        case 6: case 8: return "";           // Back/Guide — quit
                        default: break;
                    }
                }
                else if (jev.type == JS_EVENT_AXIS) {
                    // Y axis — track each axis number separately
                    if (jev.number == 1 || jev.number == 7) {
                        int new_y = 0;
                        if (jev.value < -16000) new_y = -1;
                        else if (jev.value > 16000) new_y = 1;

                        int *prev = (jev.number == 1) ? &prev_axis_y1 : &prev_axis_y7;
                        if (new_y != *prev) {
                            int old_combined = (prev_axis_y1 != 0) ? prev_axis_y1 : prev_axis_y7;
                            *prev = new_y;
                            int new_combined = (prev_axis_y1 != 0) ? prev_axis_y1 : prev_axis_y7;

                            // Only move if the combined direction changed
                            if (new_combined != old_combined) {
                                if (new_combined != 0) {
                                    move_cursor(new_combined);
                                    held_dir_y = new_combined;
                                    first_repeat = true;
                                } else {
                                    held_dir_y = 0;
                                }
                            }
                        }
                    }
                    // X axis — same per-axis tracking
                    else if (jev.number == 0 || jev.number == 6) {
                        int new_x = 0;
                        if (jev.value < -16000) new_x = -1;
                        else if (jev.value > 16000) new_x = 1;

                        int *prev = (jev.number == 0) ? &prev_axis_x0 : &prev_axis_x6;
                        if (new_x != *prev) {
                            int old_combined = (prev_axis_x0 != 0) ? prev_axis_x0 : prev_axis_x6;
                            *prev = new_x;
                            int new_combined = (prev_axis_x0 != 0) ? prev_axis_x0 : prev_axis_x6;

                            if (new_combined != old_combined) {
                                if (new_combined == 1) {
                                    std::string r = enter_folder();
                                    if (!r.empty()) return r;
                                } else if (new_combined == -1) {
                                    go_back();
                                }
                            }
                        }
                    }
                }
            }
        }

        // ── D-pad repeat when held ────────────────────────────────────
        if (held_dir_y != 0) {
            uint64_t now = browser_time_ms();
            uint64_t threshold = first_repeat ? REPEAT_DELAY_MS : REPEAT_RATE_MS;
            if (now - last_move_time >= threshold) {
                move_cursor(held_dir_y);
                first_repeat = false;
            }
        }

        if (!need_redraw) {
            usleep(16000); // ~60fps idle
            continue;
        }
        need_redraw = false;

        // ── Render 128×128 ────────────────────────────────────────────
        for (int i = 0; i < 128*128; i++) buf128[i] = P8_DARK_BLUE;

        // Header bar
        for (int y = 0; y < 9; y++)
            for (int x = 0; x < 128; x++)
                buf128[y * 128 + x] = P8_DARK_PURPLE;

        // Header text
        std::string header;
        if (dir_stack.size() <= 1) {
            header = "PICO-8";
        } else {
            std::string &cur = dir_stack.back();
            size_t slash = cur.find_last_of('/');
            header = (slash != std::string::npos) ? cur.substr(slash+1) : cur;
        }
        draw_text(buf128, 128, 2, 2, truncate_str(header, 25).c_str(), P8_WHITE, 128, 128);
        if (dir_stack.size() > 1)
            draw_text(buf128, 128, 110, 2, "<-", P8_YELLOW, 128, 128);

        // Item count
        char count_str[16];
        snprintf(count_str, sizeof(count_str), "%d", (int)entries.size());
        draw_text(buf128, 128, 90, 2, count_str, P8_LIGHT_GRAY, 128, 128);

        // Items
        if (entries.empty()) {
            draw_text(buf128, 128, 8, 50, "EMPTY FOLDER", P8_LIGHT_GRAY, 128, 128);
        } else {
            for (int i = 0; i < VISIBLE_ITEMS && (scroll_offset + i) < (int)entries.size(); i++)
            {
                int idx = scroll_offset + i;
                const BrowserEntry &e = entries[idx];
                int y = 11 + i * 8;
                bool selected = (idx == cursor);

                if (selected) {
                    for (int hy = y - 1; hy < y + 7 && hy < 128; hy++)
                        for (int hx = 0; hx < 128; hx++)
                            buf128[hy * 128 + hx] = P8_DARK_GREEN;
                }

                uint16_t name_color = selected ? P8_WHITE : P8_LIGHT_GRAY;

                if (e.is_folder) {
                    draw_text(buf128, 128, 2, y, ">", P8_YELLOW, 128, 128);
                    draw_text(buf128, 128, 8, y, truncate_str(e.name, 23).c_str(), name_color, 128, 128);
                } else {
                    draw_text(buf128, 128, 2, y, "*", P8_PINK, 128, 128);
                    draw_text(buf128, 128, 8, y, truncate_str(strip_ext(e.name), 23).c_str(), name_color, 128, 128);
                }
            }

            if (scroll_offset > 0)
                draw_text(buf128, 128, 120, 11, "^", P8_ORANGE, 128, 128);
            if (scroll_offset + VISIBLE_ITEMS < (int)entries.size())
                draw_text(buf128, 128, 120, 11 + (VISIBLE_ITEMS-1) * 8, "v", P8_ORANGE, 128, 128);
        }

        // Footer
        draw_text(buf128, 128, 2, 122, "A:SELECT  X:BACK", P8_INDIGO, 128, 128);

        blit_browser(surface, buf128);
        SDL_UpdateRect(surface, 0, 0, surface->w, surface->h);
    }

    return "";
}
