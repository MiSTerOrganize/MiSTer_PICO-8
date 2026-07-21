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
            *screen++ = lut[pixel(x, y, get_front_screen())];
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
uint8_t vm::pixel(int x, int y, u4mat2<128, 128> const& screen) const
{
    // TODO: cache all state
    auto &draw_state = m_front_draw_state;
    auto &hw_state = m_front_hw_state;

    // Get screen mode
    uint8_t const& mode = draw_state.screen_mode;

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

    int c = screen.get(x, y);

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

    return normalize_palette_color(draw_state.screen_palette[c]);
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

