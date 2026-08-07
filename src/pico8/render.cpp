//
//  ZEPTO-8 — Fantasy console emulator
//
//  Copyright © 2016—2020 Sam Hocevar <sam@hocevar.net>
//
//  This program is free software. It comes without any warranty, to
//  the extent permitted by applicable law. You can redistribute it
//  and/or modify it under the terms of the Do What the Fuck You Want
//  to Public License, Version 2, as published by the WTFPL Task Force.
//  See http://www.wtfpl.net/ for more details.
//

#if HAVE_CONFIG_H
#   include "config.h"
#endif

#include "pico8/vm.h"
#include "pico8/pico8.h"

#include <lol/vector> // lol::u8vec4
#include <cstring>    // memcpy (MiSTer shim patch)

namespace z8::pico8
{

static uint8_t normalize_palette_color(uint8_t color)
{
    return color & 0x8f;
}

void vm::private_end_render()
{
    if (m_in_pause) return;

    memcpy(&m_front_buffer, &get_current_screen(), sizeof(m_front_buffer));
    m_front_draw_state = m_ram.draw_state;
    m_front_hw_state = m_ram.hw_state;
}

/* Set the screen mode for BOTH the live and the front draw state.
 *
 * The pause menu needs mode 0 while it is open: it draws at fixed coordinates
 * (x=24..103, y~36..92) and a cart in a stretch mode -- e.g. Alex Kidd in Pico
 * World, whose _init() does poke(0x5f2c,3) -- only displays coords 0..63, which
 * clips the menu to its bottom-right corner.
 *
 * A plain poke(0x5f2c, 0) from the bios CANNOT do this. render() reads
 * m_front_draw_state (see below), and private_end_render() early-returns while
 * m_in_pause, so the front state stays frozen at whatever the cart last had.
 * The live state the poke touches is never consulted. Hence this pair-setter.
 *
 * Restoring on unpause also sets both: the next private_end_render() would
 * refresh the front state anyway, but setting it here closes the one-frame
 * window before that happens.
 */
void vm::private_pause_screen_mode(int16_t mode)
{
    m_ram.draw_state.screen_mode = (uint8_t)mode;
    m_front_draw_state.screen_mode = (uint8_t)mode;
}

void vm::render(lol::u8vec4 *screen) const
{
    render(screen, SIZE_MAX);
}

void vm::render(lol::u8vec4 *screen, size_t max_pixels) const
{
    // Cannot use a 256-value LUT because data access will be
    // very random due to rotation, flip, stretch etc.
    lol::u8vec4 lut[128 + 16];
    for (int c = 0; c < 16; ++c)
    {
        lut[c] = palette::get8(c);
        lut[128 + c] = palette::get8(16 + c);
    }

    // Multiscreen carts (_map_display) interleave extra 128x128 screens
    // into the output rows, emitting 128*msx x 128*msy pixels total. If
    // the caller's buffer only holds a single screen, fall back to
    // rendering screen 0 alone — the old unbounded write overflowed the
    // caller's heap buffer on EVERY multiscreen cart (both the MiSTer
    // present path and z8headless allocate exactly 128x128; ASan-pinned
    // 2026-07-21 via "Oust (Demo)", which crashed SIGBUS when a heap
    // layout shift put the buffer against an unmapped page).
    int msx = m_multiscreens_x, msy = m_multiscreens_y;
    if ((size_t)(128 * msx) * (size_t)(128 * msy) > max_pixels)
        msx = msy = 1;

    for (int y = 0; y < 128; ++y)
    {
        for (int x = 0; x < 128; ++x)
            // _composited: screen 0 is the one the pause menu draws on. The
            // extra multiscreen surfaces below keep plain pixel().
            *screen++ = lut[pixel_composited(x, y, get_front_screen())];
        if (msx > 1)
        {
            for (int sx = 1; sx < msx; ++sx)
                for (int x = 0; x < 128; ++x)
                    *screen++ = lut[pixel(x, y, *m_multiscreens[sx - 1])];
        }
    }
    if (msy > 1)
    {
        for (int sy = 1; sy < msy; ++sy)
            for (int y = 0; y < 128; ++y)
            {
                for (int sx = 0; sx < msx; ++sx)
                    for (int x = 0; x < 128; ++x)
                        *screen++ = lut[pixel(x, y, *m_multiscreens[sx + sy * msx - 1])];
            }
    }
}


// Hardware pixel accessor
/* Screen-mode transform: display coords in, source coords out.
 *
 * Split out of pixel() so the A9 display-space pause overlay can shade a
 * pixel WITHOUT it, while the game behind the menu still gets it. Byte-for-
 * byte the same arithmetic as before -- moving it did not change it. */
void vm::screen_xform(int &x, int &y) const
{
    // Get screen mode
    uint8_t const& mode = m_front_draw_state.screen_mode;

    // Apply screen mode (rotation, mirror, flip…)
    if ((mode & 0xbc) == 0x84)
    {
        // Rotation modes (0x84 to 0x87)
        if (mode & 1)
            std::swap(x, y);
        x = mode & 2 ? 127 - x : x;
        y = ((mode + 1) & 2) ? 127 - y : y;
    }
    else
    {
        // Other modes
        x = (mode & 0xbd) == 0x05 ? std::min(x, 127 - x) // mirror
            : (mode & 0xbd) == 0x01 ? x / 2                // stretch
            : (mode & 0xbd) == 0x81 ? 127 - x : x;         // flip
        y = (mode & 0xbe) == 0x06 ? std::min(y, 127 - y) // mirror
            : (mode & 0xbe) == 0x02 ? y / 2                // stretch
            : (mode & 0xbe) == 0x82 ? 127 - y : y;         // flip
    }
}

/* Raster mode + screen palette, i.e. everything pixel() does AFTER the fetch.
 *
 * y is the row the colour is being shaded FOR: the transformed source row for
 * cart pixels, the display row for overlay pixels. Raster effects are per-row,
 * so passing the wrong one would tilt a gradient against the menu. */
uint8_t vm::pixel_shade(int c, int y) const
{
    auto &hw_state = m_front_hw_state;

    // Apply raster mode
    if (hw_state.raster.mode == 0x10)
    {
        // Raster mode: alternate palette
        if (hw_state.raster.bits[y])
            return normalize_palette_color(hw_state.raster.palette[c]);
    }
    else if ((hw_state.raster.mode & 0x30) == 0x30)
    {
        // Raster mode: gradient
        if ((hw_state.raster.mode & 0x0f) == c)
        {
            int c2 = (y / 8 + (hw_state.raster.bits[y] ? 1 : 0)) % 16;
            return normalize_palette_color(hw_state.raster.palette[c2]);
        }
    }

    return normalize_palette_color(m_front_draw_state.screen_palette[c]);
}

uint8_t vm::pixel(int x, int y, u4mat2<128, 128> const& screen) const
{
    // TODO: cache all state
    screen_xform(x, y);
    return pixel_shade(screen.get(x, y), y);
}

/* A9: the game, then the pause menu on top of it in display space.
 *
 * Inside the darken box the overlay owns the pixel outright -- the box was
 * filled with the dimmed backdrop before the menu drew over it, so there is
 * nothing to blend here. Outside it, and whenever no menu is up, this is
 * exactly pixel(): one predictable branch on a bool that is false during
 * normal play. */
uint8_t vm::pixel_composited(int x, int y, u4mat2<128, 128> const& screen) const
{
    // m_in_pause as well as the flag: private_set_pause(false) clears the flag
    // on the normal exit, but gating on the menu ACTUALLY being up means no
    // bios path can leave a stale overlay painted over live gameplay. The
    // invariant is then local to this function instead of spread across the
    // bios, which is the difference between a bug being impossible and a bug
    // being merely unlikely.
    if (m_in_pause && m_pause_overlay_on
         && x >= m_pause_box[0] && x <= m_pause_box[2]
         && y >= m_pause_box[1] && y <= m_pause_box[3])
        return pixel_shade(m_pause_overlay.get(x, y), y);

    return pixel(x, y, screen);
}

int vm::get_ansi_color(uint8_t c) const
{
    static int const ansi_palette[] =
    {
         16, // 000000 → 000000
         17, // 1d2b53 → 00005f
         89, // 7e2553 → 87005f
         29, // 008751 → 00875f
        131, // ab5236 → ab5236
        240, // 5f574f → 5f5f5f
        251, // c2c3c7 → c6c6c6
        230, // fff1e8 → ffffdf
        197, // ff004d → ff005f
        214, // ffa300 → ffaf00
        220, // ffec27 → ffdf00
         47, // 00e436 → 00ff5f
         39, // 29adff → 00afff
        103, // 83769c → 8787af
        211, // ff77a8 → f787af
        223, // ffccaa → ffdfaf
    };

    return ansi_palette[normalize_palette_color(m_front_draw_state.screen_palette[c & 0xf]) & 0xf];
}

} // namespace z8::pico8

