//
//  ZEPTO-8 — Fantasy console emulator
//
//  Copyright © 2016–2024 Sam Hocevar <sam@hocevar.net>
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

#include <lol/engine.h> // lol::net
#include <lol/msg>      // lol::msg
#include <lol/file>     // lol::file
#include <lol/utils> // lol::split

#include <algorithm>  // std::min
#include <filesystem>
#include "compat_format.h"
#include <cstring>    // memset, memcpy (MiSTer shim patch)
#include <cmath>      // std::abs (MiSTer shim patch)
#include <unistd.h>   // _exit (MiSTer Reset Cart pause-menu handler)
#include <chrono>
#include <ctime>
#include <cassert>
#include <vector>     // savestate_load — staging keys-to-delete during sandbox mutation
#include <sstream> // std::stringstream

#include "pico8/pico8.h"
#include "pico8/vm.h"
#include "bindings/lua.h"
#include "bios.h"
#include "eris.h"  // Lua state persistence — used by savestate (Phase 1B)

// FIXME: activate this one day, when we use Lua 5.3 maybe?
#define HAVE_LUA_GETEXTRASPACE 0

// Binding specialisations specific to PICO-8
template<> void z8::bindings::lua_get(lua_State *l, int n,
                                      z8::pico8::rich_string &arg)
{
    if (lua_isnone(l, n))
        arg.assign("[no value]");
    else if (lua_isnil(l, n))
        arg.assign("[nil]");
    else if (lua_type(l, n) == LUA_TSTRING)
    {
        size_t len;
        char const *s = lua_tolstring(l, n, &len);
        arg.assign(s, len);
    }
    else if (lua_isnumber(l, n))
    {
        char buffer[20];
        fix32 x = lua_tonumber(l, n);
        int i = sprintf(buffer, "%.4f", (double)x);
        // Remove trailing zeroes and comma
        while (i > 2 && buffer[i - 1] == '0' && ::isdigit(buffer[i - 2]))
            buffer[--i] = '\0';
        if (i > 2 && buffer[i - 1] == '0' && buffer[i - 2] == '.')
            buffer[i -= 2] = '\0';
        arg.assign(buffer);
    }
    else if (lua_istable(l, n))
        arg.assign("[table]");
    else if (lua_isthread(l, n))
        arg.assign("[thread]");
    else if (lua_isfunction(l, n))
        arg.assign("[function]");
    else
        arg.assign(lua_toboolean(l, n) ? "true" : "false");
}

namespace z8::pico8
{

vm::vm()
{
    load_config();

    m_bios = std::make_unique<bios>();

    m_lua = luaL_newstate();
    lua_atpanic(m_lua, &vm::panic_hook);
    lua_setpico8memory(m_lua, (uint8_t *)&m_ram);
    luaL_openlibs(m_lua);

    bindings::lua::init(m_lua, this);

    // Automatically yield every 1000 instructions
    lua_sethook(m_lua, &vm::instruction_hook, LUA_MASKCOUNT, 1000);

    // Clear memory
    private_init_ram();

    // Initialise VM state (TODO: check what else to init)
    ::memset(m_state.buttons, 0, sizeof(m_state.buttons));
    ::memset(&m_state.mouse, 0, sizeof(m_state.mouse));

    // Initialize Zepto8 runtime
    int status = luaL_dostring(m_lua, m_bios->get_code().c_str());
    if (status != LUA_OK)
    {
        char const *message = lua_tostring(m_lua, -1);
        lol::msg::error("error %d loading bios.p8: %s\n", status, message);
        lua_pop(m_lua, 1);
        assert(false);
    }
}

vm::~vm()
{
    save(true);
    lua_close(m_lua);
}

std::string const &vm::get_code() const
{
    return m_cart.get_code();
}

u4mat2<128, 128> const &vm::get_front_screen() const
{
    return m_front_buffer;
}

u4mat2<128, 128> const& vm::get_current_screen() const
{
    if (m_in_pause) return m_front_buffer;
    if (m_ram.draw_state.misc_features.multi_screen)
    {
        if (m_multiscreen_current > 0 && m_multiscreen_current <= m_multiscreens.size())
        {
            return *m_multiscreens[m_multiscreen_current - 1];
        }
    }
    return m_ram.hw_state.mapping_screen == 0 ? m_ram.gfx : m_ram.screen;
}

u4mat2<128, 128>& vm::get_current_screen()
{
    return const_cast<u4mat2<128, 128> &>(std::as_const(*this).get_current_screen());
}

lol::ivec2 vm::get_screen_resolution() const
{
    return lol::ivec2(128 * m_multiscreens_x, 128 * m_multiscreens_y);
}

std::tuple<uint8_t *, size_t> vm::ram()
{
    return std::make_tuple(&m_ram[0], sizeof(m_ram));
}

std::tuple<uint8_t *, size_t> vm::rom()
{
    auto rom = m_cart.get_rom();
    return std::make_tuple(&rom[0], sizeof(rom));
}

void vm::runtime_error(std::string str)
{
    // This function never returns
    luaL_error(m_sandbox_lua, str.c_str());
}

int vm::panic_hook(lua_State* l)
{
    char const *message = lua_tostring(l, -1);
    lol::msg::error("Lua panic: %s\n", message);
    assert(false);
    return 0;
}

void vm::instruction_hook(lua_State *l, lua_Debug *)
{
#if HAVE_LUA_GETEXTRASPACE
    vm *that = *static_cast<vm**>(lua_getextraspace(l));
#else
    lua_getglobal(l, "\x01");
    vm *that = (vm *)lua_touserdata(l, -1);
    lua_remove(l, -1);
#endif

    // The value 135000 was found using trial and error, but it causes
    // side effects in lots of cases. Use 300000 instead.
    // FIXME: this is because we do not consider system costs, see
    // https://pico-8.fandom.com/wiki/CPU for more information
    that->m_instructions += 1000;
    if (that->m_instructions >= that->m_max_instructions)
    {
        // Heavy-bytecode carts (e.g. Another World, which interprets its
        // own VM inside Lua) trip this hook from the main thread while
        // BIOS code is running between coresume calls. lua_yield on the
        // main thread throws "attempt to yield from outside a coroutine"
        // and kills the cart. Check before yielding — addresses the
        // pre-existing zepto8 FIXME on this hook.
        // lua_pushthread returns 1 if the current thread IS the main
        // thread (can't yield from it). We pop the pushed thread reference
        // to avoid leaking it on every hook fire (every 1000 instructions).
        int is_main_thread = lua_pushthread(l);
        lua_pop(l, 1);

        lua_getglobal(l, "__z8_is_inside_main_loop");
        bool is_inside_loop = lua_toboolean(l, -1);
        lua_pop(l, 1);

        if (!is_main_thread)
            lua_yield(l, 0);
        if (!is_inside_loop)
        {
            // should only handle buttons if we are outside _draw or _update
            that->private_buttons();
        }
    }
}

tup<bool, bool, std::string> vm::private_download(opt<std::string> str)
{
#if !__NX__ && !__SCE__
    // Load cart info from a URL
    //std::string host = "https://www.lexaloffle.com";
    std::string host = "http://sam.hocevar.net/zepto8";

    if (str)
        download_state.step = 0;

    switch (download_state.step)
    {
    case 0:
        // Step 0: start downloading cart info
        download_state.client.get(host + "/bbs/cpost_lister3.php?nfo=1&version=000112bw&lid=" + str->substr(1));
        download_state.step = 1;
        break;
    case 1:
        // Step 1: wait while downloading
        if (download_state.client.get_status() != lol::net::http::status::pending)
            download_state.step = 2;
        break;
    case 2: {
        // Step 2: bail out if errors, save cart info, start downloading cart
        if (download_state.client.get_status() != lol::net::http::status::success)
            return std::make_tuple(true, false, std::string("error downloading info"));

        std::map<std::string, std::string> data;
        for (auto& line : lol::split(download_state.client.get_result(), '\n'))
        {
            size_t delim = line.find(':');
            if (delim != std::string::npos)
                data[line.substr(0, delim)] = line.substr(delim + 1);
        }

        auto const& lid = data["lid"];
        auto const& mid = data["mid"];
        if (lid.size() == 0 || mid.size() == 0)
            return std::make_tuple(true, false, std::string("no cart info found"));

        // Write cart info
#if _WIN32
        std::string cache_dir = lol::sys::getenv("APPDATA");
#else
        std::string cache_dir = lol::sys::getenv("HOME") + "/.lexaloffle";
#endif
        cache_dir += "/pico-8/bbs/";
        cache_dir += (mid[0] >= '1' && mid[0] <= '9') ? mid.substr(0, 1) : "carts";
        std::error_code code;
        if (!std::filesystem::create_directories(cache_dir, code))
            if (code.value() != 0) // XXX: workaround for Windows weird behaviour
                return std::make_tuple(true, false, "can't create cache dir");
        std::string nfo_path = cache_dir + "/" + mid + ".nfo";
        download_state.cart_path = cache_dir + "/" + lid + ".p8.png";

        if (!lol::file::write(nfo_path, download_state.client.get_result()))
            return std::make_tuple(true, false, "can't save nfo file");

        // Download cart
        download_state.client.get(host + "/bbs/get_cart.php?cat=7&lid=" + lid);
        download_state.step = 3;
        break;
    }
    case 3:
        // Step 3: wait while downloading
        if (download_state.client.get_status() != lol::net::http::status::pending)
            download_state.step = 4;
        break;
    case 4:
        // Step 4: bail out if errors, load cart otherwise
        if (download_state.client.get_status() != lol::net::http::status::success)
            return std::make_tuple(true, false, std::string("error downloading cart"));

        // Save cart
        if (!lol::file::write(download_state.cart_path, download_state.client.get_result()))
            return std::make_tuple(true, false, "can't save cart file");

        // Load cart
        load(download_state.cart_path);
        return std::make_tuple(true, true, std::string());
    default:
        break;
    }
#endif

    return std::make_tuple(false, false, std::string());
}

bool file_exists(std::string filepath)
{
    if (std::filesystem::exists(filepath))
    {
        return true;
    }
    return false;
}

void save_commit()
{
}

void handle_exit_request()
{
}

void vm::private_init_ram()
{
    ::memset(&m_ram, 0, sizeof(m_ram));

    // init mapping default values:
    m_ram.hw_state.mapping_screen = 0x60;
    m_ram.hw_state.mapping_map = 0x20;
    m_ram.hw_state.mapping_map_width = 0x80;

    // Initialise the PRNG with the current time
    auto now = std::chrono::high_resolution_clock::now();
    api_srand(fix32::frombits((int32_t)now.time_since_epoch().count()));

    // also reset timer, maybe should be done in a separate function?
    m_time = 0;
    m_timer_last = std::chrono::steady_clock::now();
}

bool vm::private_load(std::string name, opt<std::string> breadcrumb, opt<std::string> params)
{
    save(true);

    std::string previous_cart = m_cart.get_filename();

    std::string requested = name;
    name = get_path_active_dir() + "/" + name;
    // tmp fix: if extension is not .p8 or .png, set it to .p8
    if (!lol::ends_with(lol::tolower(name), ".p8") && !lol::ends_with(lol::tolower(name), ".png"))
        name += ".p8";
    fprintf(stderr, "[multicart] load() requested='%s' resolved='%s' params='%s'\n",
            requested.c_str(), name.c_str(),
            params.has_value() ? (*params).c_str() : "(none)");
    // Load cart from a file
    if (!load_cart(m_cart, name)) {
        fprintf(stderr, "[multicart] load() FAILED for %s\n", name.c_str());
        return false;
    }

    // Stash params so stat(6) can return them even when no breadcrumb
    // was supplied (PICO-8 spec).
    m_load_params = params.has_value() ? *params : "";

    if (breadcrumb.has_value() && (*breadcrumb).length() > 1)
    {
        breadcrumb_path new_breadcrumb;
        new_breadcrumb.cart_path = previous_cart;
        new_breadcrumb.title = *breadcrumb;
        new_breadcrumb.params = params.has_value() ? *params : "";
        breadcrumbs.push_back(new_breadcrumb);
    }

    run();
    return true;
}

void vm::load(std::string const &name)
{
    save(true);

    set_path_active_dir(name);
    load_cart(m_cart, name);
    // Remember this as the entry cart so Reset Cart returns here even if
    // the game later swaps to a sub-cart via load() / private_load().
    m_entry_cart = m_cart.get_filename();
    breadcrumbs.clear();
    m_load_params.clear();
}

bool vm::load_cart(cart &target_cart, std::string const& filename)
{
    bool has_loaded = target_cart.load(filename);
    if (has_loaded)
    {
        // TODO: in pico 8, cstore is saved using file hash as a filename
        // goal is that if you make a new version of your cart, it will change hash and so have a separate save
        // also works if you have several cartridge with same file name
        std::string name_cstore = get_path_cstore(filename);
        if (file_exists(name_cstore))
        {
            // Load cart from the save file
            auto reload_cart = std::make_shared<cart>();
            reload_cart->load(name_cstore);
            // copy save cart rom to loaded cart rom
            target_cart.set_from_ram(reload_cart->get_rom(), 0, 0, offsetof(memory, code));
        }
    }
    return has_loaded;
}

bool vm::save_cart(cart& target_cart, std::string const& filename)
{
    // save in cstore
    std::string name_cstore = get_path_cstore(filename);
    bool success = target_cart.save(name_cstore);
    save_commit();
    return success;
}

void vm::reset()
{
    // Prefer the entry cart (the one MiSTer mounted via .s0) so a reset on
    // a multicart game returns to the title screen rather than restarting
    // the current sub-cart.
    std::string target = !m_entry_cart.empty() ? m_entry_cart : m_cart.get_filename();
    load(target);
    run();
}

void vm::run()
{
    // Start the cartridge!
    int status = luaL_dostring(m_lua, "run()");
    if (status != LUA_OK)
    {
        char const *message = lua_tostring(m_lua, -1);
        lol::msg::error("error %d running cartridge: %s\n", status, message);
        lua_pop(m_lua, 1);
    }
}

bool vm::step(float /* seconds */)
{
    auto time_now = std::chrono::steady_clock::now();
    if (!m_in_pause)
    {
        m_time += std::chrono::duration_cast<std::chrono::duration<double>>(time_now - m_timer_last).count();
    }
    m_timer_last = time_now;

    if (m_exit_requested)
    {
        save(true);
        m_is_running = false;
        handle_exit_request();
        return false;
    }

    bool ret = false;
    lua_getglobal(m_lua, "__z8_tick");
    int status = lua_pcall(m_lua, 0, 1, 0);
    if (status != LUA_OK)
    {
        char const *message = lua_tostring(m_lua, -1);
        lol::msg::error("error %d in main loop: %s\n", status, message);
        // Mirror to stderr (pico8.log) so cart-side Lua errors are visible
        // alongside other debug output instead of being lost in lol::msg.
        fprintf(stderr, "[lua-error] main loop status=%d msg=%s\n",
                status, message ? message : "(null)");
        fflush(stderr);
    }
    else
    {
        ret = (int)lua_tonumber(m_lua, -1) >= 0;
    }
    lua_pop(m_lua, 1);

    m_instructions = 0;

    save(false);

    if (m_watch_file_change && m_cart.has_file_changed())
    {
        if (load_cart(m_cart, m_cart.get_filename())) {
            run();
        }
    }

    return ret;
}

void vm::button(int player, int index, int state)
{
    // Edge-trigger diagnostic for press-X loop investigation.
    // Logs only on state transitions, not per-frame holds.
    static int last_state[4][8] = {{0}};
    if (player < 4 && index < 8 && state != last_state[player][index]) {
        fprintf(stderr, "[input] p%d btn%d -> %d (was %d)\n",
                player, index, state, last_state[player][index]);
        fflush(stderr);
        last_state[player][index] = state;
    }
    m_state.buttons[1][player * 8 + index] += state;
}

void vm::mouse(lol::ivec2 coords, lol::ivec2 relative, int buttons, int scroll)
{
    m_state.mouse.x = (double)coords.x;
    m_state.mouse.y = (double)coords.y;
    m_state.mouse.b = (double)buttons;
    m_state.mouse.s[0] = (double)scroll;

    bool has_button_pressed = buttons > 0 && m_state.mouse.lb != m_state.mouse.b;
    if (has_button_pressed)
        m_state.mouse.ac = 4;
    else
        m_state.mouse.ac = int(std::min(std::abs(double(m_state.mouse.rx) - relative.x) + std::abs(double(m_state.mouse.ry) - relative.y), 3.0));
    
    m_state.mouse.rx = (double)relative.x;
    m_state.mouse.ry = (double)relative.y;
    
    if (m_ram.draw_state.mouse_flags.buttons)
    {
        m_state.buttons[1][5] += (buttons & 0x1) ? 1 : 0;
        m_state.buttons[1][4] += (buttons & 0x2) ? 1 : 0;
        m_state.buttons[1][6] += (buttons & 0x4) ? 1 : 0;
    }
}

void vm::text(char ch)
{
    // Convert uppercase characters to special glyphs
    if (ch >= 'A' && ch <= 'Z')
        ch = '\x80' + (ch - 'A');
    m_state.kbd.chars[m_state.kbd.stop] = ch;
    m_state.kbd.stop = (m_state.kbd.stop + 1) % (int)sizeof(m_state.kbd.chars);
}

void vm::sixaxis(lol::vec3 angle)
{
    m_state.rotation.x = angle.x;
    m_state.rotation.y = angle.y;
    m_state.rotation.z = angle.z;
}

void vm::axis(int player, float valueX, float valueY)
{
    m_state.axes[player][0] = valueX;
    m_state.axes[player][1] = valueY;
}

//
// System
//

void vm::api_run()
{
    save(true);

    // Initialise VM state (TODO: check what else to init)
    ::memset(m_state.buttons, 0, sizeof(m_state.buttons));
    ::memset(&m_state.mouse, 0, sizeof(m_state.mouse));

    // reset multiscreen
    m_ram.draw_state.misc_features.multi_screen = false;
    m_multiscreens_x = 1;
    m_multiscreens_y = 1;

    // Load cartridge code and call __z8_run_cart() on it
    lua_getglobal(m_sandbox_lua, "__z8_run_cart");
    lua_pushstring(m_sandbox_lua, m_cart.preprocess_code().c_str());
    lua_pcall(m_sandbox_lua, 1, 0, 0);
}

void vm::api_reload(int16_t in_dst, int16_t in_src, opt<int16_t> in_size, opt<std::string> filename)
{
    using std::min;

    int dst = 0, src = 0, size = offsetof(memory, code);

    if (in_size && *in_size <= 0)
        return;

    // PICO-8 fully reloads the cartridge if not all arguments are passed
    if (in_size)
    {
        dst = in_dst & 0xffff;
        src = in_src & 0xffff;
        size = *in_size & 0xffff;
    }

    // Attempting to write outside the memory area raises an error. Everything
    // else seems legal, especially reading from anywhere.
    // FIXME: may not be true anymore with extended memory, as full 0xffff are accessible
    if (dst + size > (int)sizeof(m_ram))
    {
        runtime_error("bad memory access");
        return;
    }

    // If reading from after the cart, fill that part with zeroes
    if (src > (int)offsetof(memory, code))
    {
        int amount = min(size, (int)sizeof(m_ram) - src);
        ::memset(&m_ram[dst], 0, amount);
        dst += amount;
        src = (src + amount) & 0xffff;
        size -= amount;
    }

    // Now copy possibly legal data
    int amount = min(size, (int)offsetof(memory, code) - src);

    if (filename.has_value())
    {
        std::string name = get_path_active_dir() + "/" + filename.value();
        // tmp fix: if extension is not .p8 or .png, set it to .p8
        if (!lol::ends_with(lol::tolower(name), ".p8") && !lol::ends_with(lol::tolower(name), ".png"))
            name += ".p8";
        // Load cart from a file
        auto reload_cart = std::make_shared<cart>();
        bool ok = load_cart(*reload_cart, name);
        if (!ok) {
            fprintf(stderr, "[reload] FAILED to load '%s' (dst=0x%x src=0x%x size=0x%x) -- destination will be zero-filled\n",
                    name.c_str(), dst, src, amount);
        }
        // copy rom from loaded cart
        ::memcpy(&m_ram[dst], &reload_cart->get_rom()[src], amount);
    }
    else
    {
        // from same cart
        ::memcpy(&m_ram[dst], &m_cart.get_rom()[src], amount);
    }

    dst += amount;
    size -= amount;

    // If there is anything left to copy, it’s zeroes again
    ::memset(&m_ram[dst], 0, size);

    update_registers();
}

void vm::api_cstore(int16_t in_dst, int16_t in_src, opt<int16_t> in_size, opt<std::string> filename)
{
    int dst = 0, src = 0, size = offsetof(memory, code);

    if (in_size && *in_size <= 0)
        return;

    // PICO-8 fully reloads the cartridge if not all arguments are passed
    if (in_size)
    {
        dst = in_dst & 0xffff;
        src = in_src & 0xffff;
        size = *in_size & 0xffff;
    }

    if (filename.has_value())
    {
        std::string name = get_path_active_dir() + "/" + filename.value();
        // tmp fix: if extension is not .p8 or .png, set it to .p8
        if (!lol::ends_with(lol::tolower(name), ".p8") && !lol::ends_with(lol::tolower(name), ".png"))
            name += ".p8";
        // Load cart from a file
        auto reload_cart = std::make_shared<cart>();
        load_cart(*reload_cart, name);
        reload_cart->set_from_ram(m_ram, dst, src, size);
        save_cart(*reload_cart, reload_cart->get_filename());
    }
    else
    {
        // from same cart
        m_cart.set_from_ram(m_ram, dst, src, size);
        save_cart(m_cart, m_cart.get_filename());
    }

    update_registers();
}

fix32 vm::api_dget(int16_t n)
{
    // FIXME: cannot be used before cartdata()
    return n >= 0 && n < 64 ? api_peek4(0x5e00 + 4 * n, 1)[0] : fix32(0);
}

void vm::api_dset(int16_t n, fix32 x)
{
    // FIXME: cannot be used before cartdata()
    if (n >= 0 && n < 64)
        api_poke4(0x5e00 + 4 * n, std::vector<fix32> { x });
}

uint8_t vm::raw_peek(int16_t addr)
{
    addr = address_translate(addr);
    return m_ram[addr];
}

// Modern PICO-8 (v0.2.4+) supports peek(addr, n) for large n — carts pack
// data tables (map state, packed sprites, audio buffers) into the 64K RAM
// and read them out as long byte strings via chr(peek(addr, N)). Old cap
// of 8192 truncated reads silently; carts saw nil for indices > 8192.
// PICO-8's documented maximum is the full 64K RAM. Bump cap to 32768 for
// peek/peek2 and 8192 for peek4 (peek4 returns 4 bytes per entry, so 8192
// entries = 32K bytes — same effective byte cap).
static constexpr size_t PEEK_BYTE_MAX = 32768;
static constexpr size_t PEEK4_ENTRY_MAX = 8192;

std::vector<int16_t> vm::api_peek(int16_t addr, opt<int16_t> count)
{
    std::vector<int16_t> ret;
    // Note: peek() is the same as peek(0)
    size_t n = count ? std::max(0, std::min(int(*count), int(PEEK_BYTE_MAX))) : 1;

    for ( ; ret.size() < n; ++addr)
    {
        int16_t bits = 0;
        bits = raw_peek(addr);
        ret.push_back(bits);
    }

    return ret;
}

std::vector<int16_t> vm::api_peek2(int16_t addr, opt<int16_t> count)
{
    std::vector<int16_t> ret;
    size_t n = count ? std::max(0, std::min(int(*count), int(PEEK_BYTE_MAX / 2))) : 1;

    for ( ; ret.size() < n; addr += 2)
    {
        int16_t bits = 0;
        for (int i = 0; i < 2; ++i)
        {
            bits |= raw_peek(addr + i) << (8 * i);
        }
        ret.push_back(bits);
    }

    return ret;
}

std::vector<fix32> vm::api_peek4(int16_t addr, opt<int16_t> count)
{
    std::vector<fix32> ret;
    size_t n = count ? std::max(0, std::min(int(*count), int(PEEK4_ENTRY_MAX))) : 1;

    for ( ; ret.size() < n; addr += 4)
    {
        int32_t bits = 0;
        for (int i = 0; i < 4; ++i)
        {
            bits |= raw_peek(addr + i) << (8 * i);
        }
        ret.push_back(fix32::frombits(bits));
    }

    return ret;
}

int16_t vm::address_translate(int16_t addr)
{
    if (addr >= 0x0000 && addr < 0x2000 && m_ram.hw_state.mapping_spritesheet == 0x60) addr = addr + 0x6000;
    if (addr >= 0x6000 && m_ram.hw_state.mapping_screen == 0) addr = addr - 0x6000;
    return addr;
}

void vm::raw_poke(int16_t addr, uint8_t val)
{
    if (addr >= 0x5e00 && addr < 0x5f00) m_savefile.set_dirty();
    addr = address_translate(addr);
    m_ram[addr] = (uint8_t)val;
}

void vm::api_poke(int16_t addr, std::vector<int16_t> args)
{
    // Note: poke() is the same as poke(0, 0)
    if (args.empty())
        args.push_back(0);

    for (auto val : args)
        raw_poke(addr++, (uint8_t)val);

    update_registers();
}

void vm::api_poke2(int16_t addr, std::vector<int16_t> args)
{
    // Note: poke2() is the same as poke2(0, 0)
    if (args.empty())
        args.push_back(0);

    for (auto val : args)
    {
        raw_poke(addr++, (uint8_t)val);
        raw_poke(addr++, (uint8_t)((uint16_t)val >> 8));
    }

    update_registers();
}

void vm::api_poke4(int16_t addr, std::vector<fix32> args)
{
    // Note: poke4() is the same as poke4(0, 0)
    if (args.empty())
        args.push_back(fix32(0));

    // 1Hz summary of poke4 traffic for AW diagnostic — counts calls hitting
    // screen memory (0x6000-0x8000) vs extended memory (0x8000+). Rare-event
    // logging: fires at most once per second.
    static int screen_pokes_this_sec = 0;
    static int ext_pokes_this_sec = 0;
    static int big_writes_this_sec = 0;
    static auto last_log = std::chrono::steady_clock::now();
    uint16_t uaddr = (uint16_t)addr;
    if (uaddr >= 0x6000 && uaddr < 0x8000) screen_pokes_this_sec++;
    else if (uaddr >= 0x8000) ext_pokes_this_sec++;
    if (args.size() > 100) big_writes_this_sec++;
    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_log).count() >= 1000) {
        fprintf(stderr, "[poke4-1s] screen=%d ext=%d big(>100)=%d\n",
                screen_pokes_this_sec, ext_pokes_this_sec, big_writes_this_sec);
        fflush(stderr);
        screen_pokes_this_sec = ext_pokes_this_sec = big_writes_this_sec = 0;
        last_log = now;
    }

    for (auto val : args)
    {
        uint32_t x = (uint32_t)val.bits();
        raw_poke(addr++, (uint8_t)x);
        raw_poke(addr++, (uint8_t)(x >> 8));
        raw_poke(addr++, (uint8_t)(x >> 16));
        raw_poke(addr++, (uint8_t)(x >> 24));
    }

    update_registers();
}

void vm::api_memcpy(int16_t in_dst, int16_t in_src, int16_t in_size)
{
    using std::min;

    if (in_size <= 0)
        return;

    // TODO: see if we can stick to int16_t instead of promoting these variables
    int src = in_src & 0xffff;
    int dst = in_dst & 0xffff;
    int size = in_size & 0xffff;

    if (src < dst) // copy from the end
    {
        int16_t addr_dst = dst + size;
        int16_t addr_src = src + size;
        for (int16_t i = 0; i < size; ++i)
            raw_poke(--addr_dst, raw_peek(--addr_src));
    }
    else
    {
        int16_t addr_dst = dst;
        int16_t addr_src = src;
        for (int16_t i = 0; i < size; ++i)
            raw_poke(addr_dst++, raw_peek(addr_src++));
    }

    update_registers();
}

void vm::api_memset(int16_t dst, uint8_t val, int16_t size)
{
    if (size <= 0)
        return;

    for (int16_t i = 0; i < size; ++i)
        raw_poke(dst++, val);

    update_registers();
}

void vm::update_registers()
{
    // PICO-8 appears to update this after a poke(). No bits are set to 1 though.
    for (uint8_t &btn : m_ram.hw_state.btn_state)
        btn &= 0x3f;
}

void vm::update_prng()
{
    auto &prng = m_ram.hw_state.prng;
    prng.a = ((prng.a >> 16) | (prng.a << 16)) + prng.b;
    prng.b += prng.a;
}

bool vm::save(bool force)
{
    save_cartdata(force);
    save_config(force);
    return true;
}

bool vm::load_cartdata()
{
    if (m_cartdata.size() == 0) return false;
    
    return m_savefile.read_save(get_path_save(m_cartdata), m_ram.persistent);
}

bool vm::save_cartdata(bool force)
{
    if (m_cartdata.size() == 0) return false;

    if (!m_savefile.tick(force)) return true;
    bool saved = m_savefile.write_save(get_path_save(m_cartdata), m_ram.persistent);
    save_commit();
    return saved;
}

// ─────────────────────────────────────────────────────────────────────
// Save states (MiSTer Frontier — same UX as NES core's 4-slot save)
//
// File format (version 3 — current):
//   magic[4]         "ZS01"
//   version[4]       uint32_t = 3
//   ram_size[4]      uint32_t = sizeof(memory)
//   ram[ram_size]    m_ram (PICO-8 memory map: gfx, sfx, map, persistent)
//   cart_path_len[4] uint32_t
//   cart_path[len]   active cart filename (for multicart restore)
//   lua_blob_len[4]  uint32_t
//   lua_blob[len]    eris-persisted __z8_sandbox (cart _ENV table)
//   state_size[4]    uint32_t = sizeof(state)
//   state[state_size]  m_state (audio sequencer, draw mirror, input)
//   menu_blob_len[4] uint32_t
//   menu_blob[len]   eris-persisted __z8_menu (pause-menu state + items)
//
// Restore strategy (KEY): the cart sandbox table object identity must be
// preserved. The cart's compiled functions hold _ENV upvalues bound to
// the original __z8_sandbox object. If we replace the global with a new
// table, those upvalues still point at the orphan original. So load
// IN-PLACE-MUTATES the existing __z8_sandbox: clear its keys, then
// copy from the eris-restored temp table. Function _ENVs continue to
// point at the same object, now with restored content. Same applies to
// __z8_menu (BIOS code accesses it via global lookup, but we still
// in-place mutate for consistency and to avoid orphaning the old menu
// items table).
//
// Coroutine call stack is NOT restored. The cart's existing __z8_loop
// coroutine continues from wherever it currently is. Cart globals are
// restored, so the next _update60 / _draw cycle reads saved state and
// renders the saved scene. This is the same approach v2/v3 used and
// what works in practice — full coroutine persist (attempted in v4)
// hit eris-related _G identity issues that proved intractable.
//
// Older formats are still loadable:
//   v1 (Phase 1A): ram only.
//   v2 (Phase 1B): + cart_path + sandbox-only lua_blob.
//   v3 (this version): + m_state + menu_blob.
// ─────────────────────────────────────────────────────────────────────

static constexpr char SAVESTATE_MAGIC[4] = {'Z','S','0','1'};
static constexpr uint32_t SAVESTATE_VERSION_MIN = 1;
// v1: ram only.
// v2: + cart filename + Lua sandbox via eris.
// v3: + m_state (audio sequencer / draw / input mirror) + __z8_menu via eris.
static constexpr uint32_t SAVESTATE_VERSION = 3;

// eris perms-table builder. eris uses a "permanent" table to map values
// (functions, userdata) that should be stored as references rather than
// serialized. We populate it from the BIOS api function set so eris
// references the live C/Lua functions on restore instead of trying to
// serialize C closures (which it can't).
//
// IMPORTANT: iteration order must be stable across save and load. We use
// the function name string as the perm-table key to avoid relying on
// unordered_set iteration order.
static void savestate_build_perms(lua_State* L, bool for_save)
{
    lua_newtable(L);  // perms table at top
    for (auto const& name : api::functions) {
        lua_getglobal(L, name.c_str());
        if (lua_isnil(L, -1)) { lua_pop(L, 1); continue; }
        if (for_save) {
            // perms[function] = name_string  (so eris finds the id when serializing)
            lua_pushstring(L, name.c_str());
            lua_settable(L, -3);
        } else {
            // perms[name_string] = function  (so eris finds the value when restoring)
            // value is at -1, perms at -2
            lua_setfield(L, -2, name.c_str());
        }
    }
}

std::string vm::savestate_path(int slot) const
{
    // Per-cart filename, like NES: /media/fat/savestates/PICO-8/<basename>_<slot>.ss
    std::string cart_path = m_entry_cart.empty() ? m_cart.get_filename() : m_entry_cart;
    auto last_slash = cart_path.find_last_of('/');
    std::string basename = (last_slash == std::string::npos) ? cart_path : cart_path.substr(last_slash + 1);
    // Strip .p8.png or .p8 extension
    if (basename.size() > 7 && basename.compare(basename.size() - 7, 7, ".p8.png") == 0)
        basename.resize(basename.size() - 7);
    else if (basename.size() > 3 && basename.compare(basename.size() - 3, 3, ".p8") == 0)
        basename.resize(basename.size() - 3);
    return "/media/fat/savestates/PICO-8/" + basename + "_" + std::to_string(slot) + ".ss";
}

bool vm::savestate_save(int slot)
{
    if (slot < 0 || slot > 3) {
        fprintf(stderr, "[savestate] save: invalid slot %d (must be 0-3)\n", slot);
        return false;
    }

    system("mkdir -p /media/fat/savestates/PICO-8");

    std::string path = savestate_path(slot);
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) {
        fprintf(stderr, "[savestate] save slot %d: cannot open %s for write\n", slot, path.c_str());
        return false;
    }

    // Header
    fwrite(SAVESTATE_MAGIC, 1, 4, f);
    uint32_t version = SAVESTATE_VERSION;
    fwrite(&version, 4, 1, f);

    // m_ram section
    uint32_t ram_size = sizeof(memory);
    fwrite(&ram_size, 4, 1, f);
    fwrite(&m_ram, 1, sizeof(memory), f);

    // Cart path section — what cart was active at save time. Used by load
    // to handle multicarts: if the active cart at load time differs, we
    // private_load() the saved one before restoring memory + Lua state.
    std::string cart_path = m_cart.get_filename();
    uint32_t cart_path_len = (uint32_t)cart_path.size();
    fwrite(&cart_path_len, 4, 1, f);
    if (cart_path_len) fwrite(cart_path.data(), 1, cart_path_len, f);

    // Lua sandbox state via eris. Persist __z8_sandbox (the cart's _ENV)
    // with a perms table mapping all api::functions to their name strings.
    std::string lua_blob;
    int top = lua_gettop(m_lua);
    lua_getglobal(m_lua, "__z8_sandbox");
    if (lua_istable(m_lua, -1)) {
        int sandbox_idx = lua_gettop(m_lua);
        savestate_build_perms(m_lua, /*for_save=*/true);
        int perms_idx = lua_gettop(m_lua);
        eris_persist(m_lua, perms_idx, sandbox_idx);
        size_t blob_len = 0;
        const char* blob_data = lua_tolstring(m_lua, -1, &blob_len);
        if (blob_data && blob_len > 0) {
            lua_blob.assign(blob_data, blob_len);
        }
    } else {
        fprintf(stderr, "[savestate] save slot %d: __z8_sandbox not available, skipping Lua state\n", slot);
    }
    lua_settop(m_lua, top);

    uint32_t lua_blob_len = (uint32_t)lua_blob.size();
    fwrite(&lua_blob_len, 4, 1, f);
    if (lua_blob_len) fwrite(lua_blob.data(), 1, lua_blob_len, f);

    // m_state — audio sequencer / draw mirror / input. Pure POD.
    uint32_t state_size = (uint32_t)sizeof(state);
    fwrite(&state_size, 4, 1, f);
    fwrite(&m_state, 1, sizeof(state), f);

    // __z8_menu — pause-menu state + cart-defined menuitem callbacks.
    std::string menu_blob;
    {
        int top2 = lua_gettop(m_lua);
        lua_getglobal(m_lua, "__z8_menu");
        if (lua_istable(m_lua, -1)) {
            int menu_idx = lua_gettop(m_lua);
            savestate_build_perms(m_lua, /*for_save=*/true);
            int perms_idx = lua_gettop(m_lua);
            eris_persist(m_lua, perms_idx, menu_idx);
            size_t blob_len = 0;
            const char* blob_data = lua_tolstring(m_lua, -1, &blob_len);
            if (blob_data && blob_len > 0) {
                menu_blob.assign(blob_data, blob_len);
            }
        }
        lua_settop(m_lua, top2);
    }
    uint32_t menu_blob_len = (uint32_t)menu_blob.size();
    fwrite(&menu_blob_len, 4, 1, f);
    if (menu_blob_len) fwrite(menu_blob.data(), 1, menu_blob_len, f);

    long pos = ftell(f);
    fclose(f);
    fprintf(stderr, "[savestate] saved slot %d: %ld bytes (ram=%u, cart=%u, lua=%u, state=%u, menu=%u) -> %s\n",
            slot, pos, ram_size, cart_path_len, lua_blob_len, state_size, menu_blob_len, path.c_str());
    return true;
}

bool vm::savestate_load(int slot)
{
    if (slot < 0 || slot > 3) {
        fprintf(stderr, "[savestate] load: invalid slot %d (must be 0-3)\n", slot);
        return false;
    }

    std::string path = savestate_path(slot);
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        fprintf(stderr, "[savestate] load slot %d: cannot open %s (no save in this slot?)\n", slot, path.c_str());
        return false;
    }

    char magic[4];
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, SAVESTATE_MAGIC, 4) != 0) {
        fprintf(stderr, "[savestate] load slot %d: bad magic in %s\n", slot, path.c_str());
        fclose(f);
        return false;
    }

    uint32_t version, ram_size;
    if (fread(&version, 4, 1, f) != 1 || fread(&ram_size, 4, 1, f) != 1) {
        fprintf(stderr, "[savestate] load slot %d: short read of header\n", slot);
        fclose(f);
        return false;
    }

    if (version < SAVESTATE_VERSION_MIN || version > SAVESTATE_VERSION) {
        fprintf(stderr, "[savestate] load slot %d: unsupported version %u (this build supports %u..%u)\n",
                slot, version, SAVESTATE_VERSION_MIN, SAVESTATE_VERSION);
        fclose(f);
        return false;
    }

    if (ram_size != sizeof(memory)) {
        fprintf(stderr, "[savestate] load slot %d: ram_size mismatch (file=%u, expected=%zu)\n",
                slot, ram_size, sizeof(memory));
        fclose(f);
        return false;
    }

    // Read m_ram into a staging buffer first — we may need to defer applying
    // it until after a cart-reload (which resets m_ram).
    memory staged_ram;
    if (fread(&staged_ram, 1, sizeof(memory), f) != sizeof(memory)) {
        fprintf(stderr, "[savestate] load slot %d: short read of m_ram\n", slot);
        fclose(f);
        return false;
    }

    // V2+ sections: cart path
    std::string saved_cart_path;
    if (version >= 2) {
        uint32_t cart_path_len = 0;
        if (fread(&cart_path_len, 4, 1, f) != 1) {
            fprintf(stderr, "[savestate] load slot %d: short read of cart_path_len\n", slot);
            fclose(f); return false;
        }
        if (cart_path_len) {
            saved_cart_path.resize(cart_path_len);
            if (fread(&saved_cart_path[0], 1, cart_path_len, f) != cart_path_len) {
                fprintf(stderr, "[savestate] load slot %d: short read of cart_path\n", slot);
                fclose(f); return false;
            }
        }
    }

    // V2+: lua_blob (sandbox-only persist via eris).
    std::string lua_blob;
    if (version >= 2) {
        uint32_t lua_blob_len = 0;
        if (fread(&lua_blob_len, 4, 1, f) != 1) {
            fprintf(stderr, "[savestate] load slot %d: short read of lua_blob_len\n", slot);
            fclose(f); return false;
        }
        if (lua_blob_len) {
            lua_blob.resize(lua_blob_len);
            if (fread(&lua_blob[0], 1, lua_blob_len, f) != lua_blob_len) {
                fprintf(stderr, "[savestate] load slot %d: short read of lua_blob\n", slot);
                fclose(f); return false;
            }
        }
    }

    // V3+ sections: m_state
    bool have_state = false;
    state staged_state;  // zero-initialized via default constructor
    if (version >= 3) {
        uint32_t state_size = 0;
        if (fread(&state_size, 4, 1, f) != 1) {
            fprintf(stderr, "[savestate] load slot %d: short read of state_size\n", slot);
            fclose(f); return false;
        }
        if (state_size != sizeof(state)) {
            fprintf(stderr, "[savestate] load slot %d: state size mismatch (file=%u, expected=%zu) — skipping state restore\n",
                    slot, state_size, sizeof(state));
            std::string skip(state_size, '\0');
            fread(&skip[0], 1, state_size, f);
        } else {
            if (fread(&staged_state, 1, sizeof(state), f) != sizeof(state)) {
                fprintf(stderr, "[savestate] load slot %d: short read of state\n", slot);
                fclose(f); return false;
            }
            have_state = true;
        }
    }

    // V3+: menu_blob (separate eris persist of __z8_menu).
    std::string menu_blob;
    if (version >= 3) {
        uint32_t menu_blob_len = 0;
        if (fread(&menu_blob_len, 4, 1, f) != 1) {
            fprintf(stderr, "[savestate] load slot %d: short read of menu_blob_len\n", slot);
            fclose(f); return false;
        }
        if (menu_blob_len) {
            menu_blob.resize(menu_blob_len);
            if (fread(&menu_blob[0], 1, menu_blob_len, f) != menu_blob_len) {
                fprintf(stderr, "[savestate] load slot %d: short read of menu_blob\n", slot);
                fclose(f); return false;
            }
        }
    }

    fclose(f);

    // Multicart: if the saved cart differs from the currently active cart,
    // load the saved cart and let it execute on subsequent __z8_tick.
    //
    // KNOWN UX WART: cart-mismatch loads require two Load State presses
    // for the saved sub-cart's state to fully restore. First load reloads
    // the cart and applies our memcpy+mutate, but on the next __z8_tick
    // the new coroutine's setup runs (top-level + _init) which clobbers
    // sandbox values with cart defaults. Second load now hits the
    // matching-cart path and the in-place mutation sticks.
    //
    // We previously tried forcing one lua_resume here to drive the cart
    // through its setup before applying restore. That broke multicart
    // sub-carts that depend on inter-cart load() parameters: e.g.,
    // vracing_main.p8 expects state set up by vracing_title.p8's
    // load("vracing_main.p8", breadcrumb, params) call, and crashes at
    // "attempt to index local 'b' (a nil value)" when run standalone.
    // Reverted 2026-05-03 — the two-load UX is the lesser evil.
    if (!saved_cart_path.empty() && saved_cart_path != m_cart.get_filename()) {
        fprintf(stderr, "[savestate] load slot %d: cart mismatch — restarting with %s\n",
                slot, saved_cart_path.c_str());
        if (!load_cart(m_cart, saved_cart_path)) {
            fprintf(stderr, "[savestate] load slot %d: failed to load saved cart %s\n",
                    slot, saved_cart_path.c_str());
            return false;
        }
        run();  // creates the __z8_loop coroutine (suspended at start)
    }

    // Apply m_ram (overwrites cart-init memory state).
    memcpy(&m_ram, &staged_ram, sizeof(memory));

    // Apply Lua state via eris. We mutate __z8_sandbox in place rather
    // than replacing the global, so the cart's main-loop coroutine
    // (which holds a reference to the table object) sees the new values.
    if (!lua_blob.empty()) {
        int top = lua_gettop(m_lua);
        lua_getglobal(m_lua, "__z8_sandbox");
        if (!lua_istable(m_lua, -1)) {
            fprintf(stderr, "[savestate] load slot %d: __z8_sandbox not a table, skipping Lua restore\n", slot);
            lua_settop(m_lua, top);
        } else {
            int sandbox_idx = lua_gettop(m_lua);  // absolute index of the sandbox

            savestate_build_perms(m_lua, /*for_save=*/false);
            int perms_idx = lua_gettop(m_lua);

            // Push the blob as a Lua string and call eris_unpersist on it
            lua_pushlstring(m_lua, lua_blob.data(), lua_blob.size());
            int blob_idx = lua_gettop(m_lua);
            eris_unpersist(m_lua, perms_idx, blob_idx);
            // Stack now: ..., sandbox, perms, blob_string, restored_value

            if (!lua_istable(m_lua, -1)) {
                fprintf(stderr, "[savestate] load slot %d: eris restored value is not a table\n", slot);
                lua_settop(m_lua, top);
                return false;
            }

            int restored_idx = lua_gettop(m_lua);

            // IN-PLACE MUTATION (the key to making this work).
            //
            // The cart's compiled code holds _ENV upvalues bound to the
            // ORIGINAL __z8_sandbox table object. Replacing the global
            // with a different table would orphan those upvalues — cart
            // would see no state changes. Instead we mutate the existing
            // sandbox in place: clear stale keys, then copy from the
            // eris-restored temp table. Object identity preserved, so
            // the cart's _ENV continues to point at the same table — now
            // with restored content.
            //
            // Step 1: clear keys in current sandbox that aren't in the
            // restored table (cart may have ADDED globals between save and
            // load — those need to go away for a true "back to save" load).
            // We collect keys-to-delete first so we don't mutate the table
            // mid-traversal.
            std::vector<std::string> keys_to_delete;
            lua_pushnil(m_lua);
            while (lua_next(m_lua, sandbox_idx) != 0) {
                // stack: ..., sandbox, perms, restored, key, value
                if (lua_type(m_lua, -2) == LUA_TSTRING) {
                    std::string k = lua_tostring(m_lua, -2);
                    // Check if this key exists in the restored table
                    lua_pushvalue(m_lua, -2);   // copy key
                    lua_gettable(m_lua, restored_idx);
                    if (lua_isnil(m_lua, -1)) {
                        keys_to_delete.push_back(k);
                    }
                    lua_pop(m_lua, 1);  // pop the lookup result
                }
                lua_pop(m_lua, 1);  // pop value, keep key for next iteration
            }
            for (auto const& k : keys_to_delete) {
                lua_pushstring(m_lua, k.c_str());
                lua_pushnil(m_lua);
                lua_settable(m_lua, sandbox_idx);
            }

            // Step 2: copy keys from restored table to sandbox.
            lua_pushnil(m_lua);
            while (lua_next(m_lua, restored_idx) != 0) {
                lua_pushvalue(m_lua, -2);  // copy key
                lua_pushvalue(m_lua, -2);  // copy value
                lua_settable(m_lua, sandbox_idx);
                lua_pop(m_lua, 1);  // pop value, keep key
            }

            lua_settop(m_lua, top);
            fprintf(stderr, "[savestate] load slot %d: sandbox restored (%zu bytes blob, %zu stale keys cleared)\n",
                    slot, lua_blob.size(), keys_to_delete.size());
        }
    }

    // Apply m_state. Brief audio click is possible if the audio thread
    // is mid-read of channels[].reverb_2/4 during this memcpy — acceptable
    // (~1 frame). State was captured at a coherent point on save.
    if (have_state) {
        memcpy(&m_state, &staged_state, sizeof(state));
    }

    // Restore __z8_menu via in-place mutation (same reasoning as sandbox:
    // any code that captured a local reference to __z8_menu would otherwise
    // be orphaned). The pause-menu items table is small so this is cheap.
    if (!menu_blob.empty()) {
        int top = lua_gettop(m_lua);
        lua_getglobal(m_lua, "__z8_menu");
        if (!lua_istable(m_lua, -1)) {
            // Fall back to setglobal if __z8_menu isn't currently a table.
            lua_pop(m_lua, 1);
            savestate_build_perms(m_lua, /*for_save=*/false);
            int perms_idx = lua_gettop(m_lua);
            lua_pushlstring(m_lua, menu_blob.data(), menu_blob.size());
            int blob_idx = lua_gettop(m_lua);
            eris_unpersist(m_lua, perms_idx, blob_idx);
            if (lua_istable(m_lua, -1)) {
                lua_setglobal(m_lua, "__z8_menu");
            }
        } else {
            int menu_idx = lua_gettop(m_lua);
            savestate_build_perms(m_lua, /*for_save=*/false);
            int perms_idx = lua_gettop(m_lua);
            lua_pushlstring(m_lua, menu_blob.data(), menu_blob.size());
            int blob_idx = lua_gettop(m_lua);
            eris_unpersist(m_lua, perms_idx, blob_idx);
            if (lua_istable(m_lua, -1)) {
                int restored_idx = lua_gettop(m_lua);
                // Clear and copy
                std::vector<std::string> keys_to_delete;
                lua_pushnil(m_lua);
                while (lua_next(m_lua, menu_idx) != 0) {
                    if (lua_type(m_lua, -2) == LUA_TSTRING) {
                        keys_to_delete.push_back(lua_tostring(m_lua, -2));
                    }
                    lua_pop(m_lua, 1);
                }
                for (auto const& k : keys_to_delete) {
                    lua_pushstring(m_lua, k.c_str());
                    lua_pushnil(m_lua);
                    lua_settable(m_lua, menu_idx);
                }
                lua_pushnil(m_lua);
                while (lua_next(m_lua, restored_idx) != 0) {
                    lua_pushvalue(m_lua, -2);
                    lua_pushvalue(m_lua, -2);
                    lua_settable(m_lua, menu_idx);
                    lua_pop(m_lua, 1);
                }
            }
        }
        lua_settop(m_lua, top);
        fprintf(stderr, "[savestate] load slot %d: menu restored (%zu bytes)\n", slot, menu_blob.size());
    }

    fprintf(stderr, "[savestate] loaded slot %d from %s\n", slot, path.c_str());
    return true;
}

bool config_parse_256(std::string line, std::string name, float& value)
{
    if (line.rfind(name, 0) != 0) return false;
    std::string rest = line.substr(name.length());
    int rawvalue = std::stoi(rest);
    value = std::clamp(rawvalue / 256.0f, 0.0f, 1.0f);
    return true;
}

std::string config_make_256(std::string name, float value)
{
    int intvalue = std::clamp(int(value * 256.0f), 0, 256);
    return name + " " + std::to_string(intvalue) + "\n";
}

bool config_parse_int(std::string line, std::string name, int& value)
{
    if (line.rfind(name, 0) != 0) return false;
    std::string rest = line.substr(name.length());
    value = std::stoi(rest);
    return true;
}

std::string config_make_int(std::string name, int value)
{
    return name + " " + std::to_string(value) + "\n";
}

bool vm::load_config()
{
    std::string s;
    if (!lol::file::read(get_path_config(), s))
        return false;

    auto ss = std::stringstream(s);

    for (std::string line; std::getline(ss, line, '\n');)
    {
        config_parse_256(line, "sound_volume", m_state.music.volume_sfx);
        config_parse_256(line, "music_volume", m_state.music.volume_music);
        config_parse_int(line, "filter_index", m_filter_index);
        config_parse_int(line, "fullscreen_method", m_fullscreen);
        config_parse_int(line, "save_slot", m_save_slot);
    }

    return true;
}

bool vm::save_config(bool force)
{
    if (!m_configfile.tick(force)) return true;

    std::string content;
    content += "// :: Audio Settings\n";
    content += config_make_256("sound_volume", m_state.music.volume_sfx);
    content += config_make_256("music_volume", m_state.music.volume_music);
    content += config_make_int("filter_index", m_filter_index);
    content += config_make_int("fullscreen_method", m_fullscreen);
    content += config_make_int("save_slot", m_save_slot);

    if (!lol::file::write(get_path_config(), content))
        return false;

    save_commit();

    return true;
}

fix32 vm::api_private_rnd(opt<fix32> in_range)
{
    if (in_range && in_range->bits() == 0) return fix32(0);
    update_prng();
    uint32_t a = m_ram.hw_state.prng.a;
    uint32_t range = in_range ? uint32_t(in_range->bits()) : 0x10000;
    return fix32::frombits(range > 0 ? a % range : 0);
}

void vm::api_srand(fix32 seed)
{
    // PICO-8 removes the seed’s MSB
    seed &= fix32::frombits(0x7fffffff);
    auto &prng = m_ram.hw_state.prng;
    prng.b = seed ? seed.bits() : 0xdeadbeef;
    prng.a = prng.b ^ 0xbead29ba;
    for (int i = 0; i < 32; ++i)
        update_prng();
}

template<class... T>
static auto any_to_variant(std::any a) -> var<T...>
{
    if (!a.has_value())
        return nullptr;

    opt<var<T...>> v = std::nullopt;

    if (!((a.type() == typeid(T) && (v = std::any_cast<T>(std::move(a)), true)) || ...))
        return nullptr;

    return std::move(*v);
}

var<bool, int16_t, fix32, std::string, std::nullptr_t> vm::api_stat(int16_t id)
{
    // Documented PICO-8 stat() arguments:
    //  0       Memory usage (0..2048)
    //  1       CPU used since last flip (1.0 == 100% CPU at 30fps)
    //  2       CPU used (system)
    //  3       Current Display
    //  4       Clipboard contents (after user has pressed CTRL-V)
    //  5       Pico-8 version
    //  6       Parameter string
    //  7       Current framerate
    //  8       Target framerate (30 or 60)
    //  9       Pico 8 framerate (system)
    // 
    //  11      Number of displays (1-4)
    //  12..15  Pause menu location
    //  16..19  Index of currently playing SFX on channels 0..3 (DEPRECATED, use 46)
    //  20..23  Note number (0..31) on channel 0..3             (DEPRECATED, use 46)
    //  24      Currently playing pattern index                 (DEPRECATED, use 46)
    //  25      Total patterns played                           (DEPRECATED, use 46)
    //  26      Ticks played on current pattern                 (DEPRECATED, use 46)
    //        
    //  28      Raw keyboard
    // 
    //  30..39  Mouse and keyboard
    //
    //  46..49  Index of currently playing SFX on channels 0..3
    //  50..53  Note number (0..31) on channel 0..3
    //  54      Currently playing pattern index
    //  55      Total patterns played
    //  56      Ticks played on current pattern
    //  57      is music playing
    //  80..85  UTC time: year, month, day, hour, minute, second
    //  90..95  Local time
    // 
    //  99      Garbage collection (raw)
    //
    //  100     Current breadcrumb label, or nil”
    //  101     current BBS ID
    //  102     website address
    //
    //  108..109 PCM Audio
    //  110      frame-by-frame mode
    //  120..121 Bytestream Availability
    //  124      Current path
    //
    //  130      ZEPTO metadata title
    //  131      ZEPTO metadata author
    
    //  140      ZEPTO Audio Volume Music
    //  141      ZEPTO Audio Volume Sfx
    //  142      ZEPTO Filter Name
    //  143      ZEPTO Fullscreen status
    //  144      ZEPTO Language
    //  145      ZEPTO Platform
    //  146      ZEPTO Player Number
    //  147      ZEPTO Save Slot
    //  149      ZEPTO Want quit confirmation message
    
    //  150..153 Gamepads axes values
    //  160..169 Physical sensors - gyro, magneto,etc.

    //  200..250 ZEPTO UI texts

    // Registered user functions have priority
    if (auto it = m_stats.find(id); it != m_stats.end())
    {
        auto ret = it->second();
        return any_to_variant<bool, int16_t, fix32, std::string, std::nullptr_t>(ret);
    }

    if (id == 0)
    {
        // Perform a GC to avoid accounting for short lifespan objects.
        // Not sure about the performance cost of this.
        lua_gc(m_sandbox_lua, LUA_GCCOLLECT, 0);

        // From the PICO-8 documentation:
        int32_t bits = ((int)lua_gc(m_sandbox_lua, LUA_GCCOUNT, 0) << 16)
                     + ((int)lua_gc(m_sandbox_lua, LUA_GCCOUNTB, 0) << 6);
        return fix32::frombits(bits);
    }

    if (id == 1 || id == 2)
        return fix32(m_instructions / float(m_max_instructions));

    if (id == 3)
        return int16_t(m_ram.draw_state.misc_features.multi_screen ? m_multiscreen_current : 0);

    if (id == 4)
        return std::string(); // TODO (clipboard)

    if (id == 5)
    {
        // Undocumented (see http://pico-8.wikia.com/wiki/Stat)
        return int16_t(PICO8_VERSION);
    }

    if (id == 6)
    {
        // PICO-8 spec: stat(6) returns the param string passed to the
        // most recent load() call, regardless of whether a breadcrumb
        // was supplied. Prefer the breadcrumb's params if one exists
        // (preserves return-from-breadcrumb behaviour); otherwise fall
        // back to the standalone params we stashed in private_load.
        if (breadcrumbs.size() > 0) return breadcrumbs.back().params;
        return m_load_params;
    }

    if (id == 7 || id == 8 || id == 9)
        return int16_t(0); // TODO (frame rate))

    if (id == 11)
        return int16_t(m_ram.draw_state.misc_features.multi_screen ? m_multiscreens_x * m_multiscreens_y : 1);

    if (id >= 12 && id <= 15)
        return int16_t(0); // TODO (pause menu)

    // two audio stats blocks
    if ((id >= 16 && id <= 26) || (id >= 46 && id <= 56))
    {
        int16_t audio_id = (id <= 26) ? id : id - 30;
        if (audio_id >= 16 && audio_id <= 19) 
            return m_state.channels[audio_id & 3].main_sfx.sfx;

        if (audio_id >= 20 && audio_id <= 23)
            return m_state.channels[audio_id & 3].main_sfx.sfx == -1 ? fix32(-1)
            : fix32((int)m_state.channels[audio_id & 3].main_sfx.offset);

        if (audio_id == 24)
            return int16_t(m_state.music.pattern);

        if (audio_id == 25)
            return int16_t(m_state.music.count);

        if (audio_id == 26)
            return int16_t(m_state.music.offset);
    }
    if (id == 57)
        return m_state.music.pattern != -1;

    if (id == 29)
    {
        // Undocumented, but PICO-8 calls codo_count_joysticks() here
        return int16_t(0);
    }

    if (id >= 30 && id <= 39)
    {
        bool devkit_mode = m_ram.draw_state.mouse_flags.enabled;
        bool devkit_pointer = devkit_mode && m_ram.draw_state.mouse_flags.locked;
        bool has_text = devkit_mode && m_state.kbd.start != m_state.kbd.stop;

        // Undocumented (see http://pico-8.wikia.com/wiki/Stat)
        switch (id)
        {
            case 30: return has_text;
            case 31:
                if (!has_text)
                    return std::string();

                if (m_state.kbd.stop > m_state.kbd.start)
                {
                    std::string ret(&m_state.kbd.chars[m_state.kbd.start],
                                    m_state.kbd.stop - m_state.kbd.start);
                    m_state.kbd.start = m_state.kbd.stop = 0;
                    return ret;
                }

                /* if (m_state.kbd.stop < m_state.kbd.start) */
                {
                    std::string ret(&m_state.kbd.chars[m_state.kbd.start],
                                    (int)sizeof(m_state.kbd.chars) - m_state.kbd.start);
                    m_state.kbd.start = 0;
                }
                return (int16_t)0;
            case 32: return devkit_mode ? m_state.mouse.x : fix32(0); break;
            case 33: return devkit_mode ? m_state.mouse.y : fix32(0); break;
            case 34: return devkit_mode ? m_state.mouse.b : fix32(0); break;
            case 35: return (int16_t)0; break; // FIXME horizontal mouse wheel / two-finger-swipe
            case 36: return devkit_mode ? m_state.mouse.s[2] : fix32(0); break;
            case 37: return devkit_mode ? m_state.mouse.ac : fix32(0); break;
            case 38: return devkit_pointer ? m_state.mouse.rx : fix32(0); break;
            case 39: return devkit_pointer ? m_state.mouse.ry : fix32(0); break;
        }
    }

    if ((id >= 80 && id <= 85) || (id >= 90 && id <= 95))
    {
        time_t t;
        time(&t);
        auto const *tm = (id <= 85 ? std::gmtime : std::localtime)(&t);
        switch (id % 10)
        {
            case 0: return int16_t(tm->tm_year + 1900);
            case 1: return int16_t(tm->tm_mon + 1);
            case 2: return int16_t(tm->tm_mday);
            case 3: return int16_t(tm->tm_hour);
            case 4: return int16_t(tm->tm_min);
            case 5: return int16_t(tm->tm_sec);
        }
    }

    // TODO: everything below this is unimplemented

    if (id >= 48 && id < 72)
    {
        if (id == 49 || (id >= 58 && id <= 63))
            return std::string();
        return nullptr;
    }

    if (id >= 72 && id < 100)
        return (int16_t)0;

    if (id == 100)
    {
        if (breadcrumbs.size() > 0) return breadcrumbs.back().title;
        return nullptr;
    }

    if (id == 101)
        return nullptr;

    if (id == 120) return false; // TODO: implement serial
    if (id == 121) return false; // TODO: implement serial
    if (id == 120) return false; // TODO: implement serial
    if (id == 124) return "/";

    if (id == 130) return m_metadata_title;
    if (id == 131) return m_metadata_author;

    if (id == 140) return fix32(m_state.music.volume_music);
    if (id == 141) return fix32(m_state.music.volume_sfx);
    if (id == 142) return getfiltername_callback ? getfiltername_callback(m_filter_index) : "none";
    if (id == 143) return m_fullscreen;
    if (id == 147) return m_save_slot;
    if (id == 149) return m_quit_confirmation;

    // Gamepads axes values
    if (id == 150) return fix32(m_state.axes[0][0]); // P1 X
    if (id == 151) return fix32(m_state.axes[0][1]); // P1 Y
    if (id == 152) return fix32(m_state.axes[1][0]); // P2 X
    if (id == 153) return fix32(m_state.axes[1][1]); // P2 Y

    if (id >= 160 && id < 170)
    {
        switch (id)
        {
        case 160: // get rotation
            return std::format("{} {} {}", (float)m_state.rotation.x, (float)m_state.rotation.y, (float)m_state.rotation.z); ;
        }
    }

    if (id >= 200 && id < 200 + m_ui_texts.size())
    {
        return m_ui_texts[id - 200];
    }

    return (int16_t)0;
}

void vm::add_stat(int16_t id, std::function<std::any()> fn)
{
    m_stats[id] = fn;
}

void vm::api_printh(rich_string str, opt<std::string> filename, opt<bool> overwrite)
{
    std::string decoded;
    for (uint8_t ch : str)
        decoded += charset::to_utf8[ch];

    // TODO: if filename is "@clip" the message should replaces the contents of the system clipboard instead of writing to a file
    if (filename.has_value())
    {
        FILE* file = fopen(filename.value().c_str(), overwrite.value_or(false) ? "w" : "a");
        if (file)
        {
            decoded += "\n"; // end of line
            fwrite(decoded.data(), sizeof(char), decoded.size(), file);
            fclose(file);
        }
        else
        {
            lol::msg::info("printh cannot open file %s to write to\n", filename.value().c_str());
        }
    }
    else
    {
        fprintf(stdout, "%s\n", decoded.c_str());
        fflush(stdout);
        // MiSTer Frontier: mirror printh output to stderr (pico8.log) too,
        // so cart-side and BIOS-side debug prints are visible in the log
        // we tail. Upstream zepto8 only writes to stdout.
        fprintf(stderr, "%s\n", decoded.c_str());
        fflush(stderr);
    }
}

void vm::fill_metadata(cart &metadata_cart)
{
    m_metadata_title = metadata_cart.get_title();
    m_metadata_author = metadata_cart.get_author();
    std::vector<uint8_t>& label = metadata_cart.get_label();
    if (label.size() >= LABEL_WIDTH * LABEL_HEIGHT)
    {
        // Direct copy to sprite memory
        for (int y = 0; y < LABEL_HEIGHT; ++y)
            for (int x = 0; x < LABEL_WIDTH; x+=2)
            {
                uint8_t col = label[y * LABEL_WIDTH + x] & 0x1f;
                col |= (label[y * LABEL_WIDTH + x + 1] & 0x1f) << 4;
                m_ram[(x/2) + y*64] = col;
            }
    }
}

void vm::api_extcmd(std::string cmdline)
{
    std::string cmd = cmdline.substr(0, cmdline.find(" "));
    std::string args = cmdline.substr(std::min(cmd.length() + 1, cmdline.length()));

    // Registered user functions have priority
    if (auto it = m_extcmds.find(cmd); it != m_extcmds.end())
    {
        it->second(args);
        return;
    }

    if (cmd == "z8_load_metadata")
    {
        if (args.length() > 0)
        {
            std::string name = get_path_active_dir() + "/" + args;
            // tmp fix: if extension is not .p8 or .png, set it to .p8
            if (!lol::ends_with(lol::tolower(name), ".p8") && !lol::ends_with(lol::tolower(name), ".png"))
                name += ".p8";
            // Load cart from a file
            auto metadata_cart = std::make_shared<cart>();
            load_cart(*metadata_cart, name);
            fill_metadata(*metadata_cart);
        }
        else
        {
            fill_metadata(m_cart);
        }
    }
    else if (cmd == "reset")
    {
        // Universal hybrid-core Reset pattern: _exit(0) and let
        // Master_Daemon respawn _handler.sh, which spawns a fresh ARM
        // binary that boots into wait-for-OSD-cart-selection. .s0 is
        // NOT deleted (unlike Quit) so the new process auto-mounts the
        // same cart from scratch, with cleanly zeroed DDR3, fresh Lua
        // VM, fresh audio thread, fresh SDL state.
        //
        // Same architecture as OpenBOR's Reset Pak (pausemenu_patch.c
        // case 2). Cost is ~1s for the respawn vs ~100ms for the old
        // in-process api_run() reload, but eliminates state-leakage
        // edge cases (audio thread persistence, lingering Lua globals,
        // etc.) and matches the universal hybrid-core architectural
        // contract — every cart-end action exits the process.
        //
        // Multicart entry-cart preservation is automatic: .s0 holds
        // the entry cart's path (whatever MiSTer mounted via OSD or
        // MGL), so respawn lands on the entry cart even if the user
        // reset while a sub-cart was active.
        fprintf(stderr, "Reset: keeping .s0, _exit(0) — Master_Daemon will respawn same cart\n");
        fflush(stderr);
        _exit(0);
    }
    else if (cmd == "pause")
    {
        luaL_dostring(m_lua, "__z8_enter_pause()");
    }
    else if (cmd == "z8_volume_music_up")
    {
        m_state.music.volume_music = std::clamp(m_state.music.volume_music + 0.125f, 0.0f, 1.0f);
        m_configfile.set_dirty();
    }
    else if (cmd == "z8_volume_music_down")
    {
        m_state.music.volume_music = std::clamp(m_state.music.volume_music - 0.125f, 0.0f, 1.0f);
        m_configfile.set_dirty();
    }
    else if (cmd == "z8_volume_sfx_up")
    {
        m_state.music.volume_sfx   = std::clamp(m_state.music.volume_sfx + 0.125f, 0.0f, 1.0f);
        m_configfile.set_dirty();
    }
    else if (cmd == "z8_volume_sfx_down")
    {
        m_state.music.volume_sfx   = std::clamp(m_state.music.volume_sfx - 0.125f, 0.0f, 1.0f);
        m_configfile.set_dirty();
    }
    else if (cmd == "z8_filter_prev" && setfilter_callback)
    {
        m_filter_index = setfilter_callback(m_filter_index - 1);
        m_configfile.set_dirty();
    }
    else if (cmd == "z8_filter_next" && setfilter_callback)
    {
        m_filter_index = setfilter_callback(m_filter_index + 1);
        m_configfile.set_dirty();
    }
    else if (cmd == "z8_window_fullscreen" && setfullscreen_callback)
    {
        if (args.length() > 0)
        {
            setfullscreen_callback(std::stoi(args));
            m_fullscreen = std::stoi(args);
            m_configfile.set_dirty();
        }
    }
    else if (cmd == "z8_quit_confirmation")
    {
        if (args.length() > 0)
        {
            m_quit_confirmation = args == "true" || args == "1";
        }
        else
        {
            m_quit_confirmation = false;
        }
    }
    else if (cmd == "z8_app_requestexit")
    {
        request_exit();
    }
    else if (cmd == "z8_cart_browser")
    {
        // Signal return to cart browser — handled by mister_main.cpp
        request_exit();
        // The frontend checks g_return_to_browser vs g_running
        // to distinguish between "go to browser" and "full quit"
    }
    else if (cmd == "z8_save_slot")
    {
        if (args.length() > 0)
        {
            save_cartdata(true);
            m_save_slot = std::max(0, std::stoi(args));
            if (!load_cartdata())
            {
                memset(m_ram.persistent, 0, 256);
            }
        }
    }
    else if (cmd == "breadcrumb" || cmd == "go_back")
    {
        if (breadcrumbs.size() == 0) return;
        breadcrumb_path breadcrumb = breadcrumbs.back();
        breadcrumbs.pop_back();
        save(true);
        m_cart.load(breadcrumb.cart_path);
        run();
    }
    else if (cmd == "z8_setuitext")
    {
        auto param1 = args.substr(0, args.find(" "));
        auto param2 = args.substr(std::min(param1.length() + 1, args.length()));

        if (param1.length() > 0 && param2.length() > 0)
        {
            try
            {
                int index = std::stoi(param1);
                if (index >= 0 && index < m_ui_texts.size())
                {
                    m_ui_texts[index] = param2;
                }
            }
            catch (std::exception const& e)
            {
                lol::msg::info("ui text parse error: %s\n", e.what());
            }
        }
    }
    else if (cmd == "z8_set_cpu_limit")
    {
        if (args.length() > 0)
        {
            m_max_instructions = std::stoi(args) * 1000;
        }
        else
        {
            m_max_instructions = m_default_max_instructions;
        }
    }
    else if (cmd == "label" || cmd == "screen" || cmd == "rec" || cmd == "video")
    {
        // Command is unimplemented
        private_stub(std::format("extcmd({})\n", cmdline));
    }
    else if (cmd == "set_title" || cmd == "audio_rec" || cmd == "audio_end"
          || cmd == "shutdown" || cmd == "go_back")
    {
        // Cosmetic / no-effect-on-MiSTer extcmds — silent no-op so carts
        // that call extcmd("set_title", "MY GAME") don't trip the
        // "unknown extcmd" error path. set_title sets the desktop-PICO-8
        // window title; meaningless on MiSTer's FPGA video output.
    }
    else
    {
        // Command is unknown
        lol::msg::error("unknown extcmd: %s\n", cmdline.c_str());
    }
}

void vm::add_extcmd(std::string const &name, std::function<void(std::string const&)> fn)
{
    m_extcmds[name] = fn;
}

void vm::api_map_display(int16_t id)
{
    if (!m_ram.draw_state.misc_features.multi_screen) return;
    m_multiscreen_current = id;

    if (m_multiscreen_current > 0)
    {
        // tmp: at least 2 horizontal screen
        m_multiscreens_x = std::max(m_multiscreens_x, 2);
        // tmp: if try to write to screen 2, we have at least 2 in height
        if (m_multiscreen_current >= 2)
        {
            m_multiscreens_y = std::max(m_multiscreens_y, 2);
        }
        // tmp: if try to write to screen 4, we have a super-wide cart 
        if (m_multiscreen_current >= 4)
        {
            m_multiscreens_x = std::max(m_multiscreens_x, 4);
        }
        int target_screen = m_multiscreen_current - 1;
        while (target_screen >= m_multiscreens.size())
        {
            m_multiscreens.push_back(std::make_shared< u4mat2<128, 128> >());
        }
    }
}

//
// I/O
//

void vm::private_buttons()
{
    uint8_t *btn_state = m_ram.hw_state.btn_state;

    // Update button state
    for (int i = 0; i < 64; ++i)
    {
        // masking system: each button is inactive after a load/reset until it has been fully released once
        if (m_state.buttons[1][i] == 0)
            m_state.buttons[2][i] = 1;
        if (m_state.buttons[2][i] == 0)
            m_state.buttons[1][i] = 0;

        if (m_state.buttons[1][i])
        {
            btn_state[i / 8] |= 1 << (i % 8);
            ++m_state.buttons[0][i];
        }
        else
        {
            btn_state[i / 8] &= ~(1 << (i % 8));
            m_state.buttons[0][i] = 0;
        }
        m_state.buttons[1][i] = 0;
    }
    m_state.mouse.s[2] = m_state.mouse.s[1] - m_state.mouse.s[0];
    m_state.mouse.s[1] = m_state.mouse.s[0];
    m_state.mouse.lb = m_state.mouse.b;

    // also handle possible pointer lock request here
    if (m_pointer_locked != m_ram.draw_state.mouse_flags.locked)
    {
        m_pointer_locked = m_ram.draw_state.mouse_flags.locked;
        pointerLock_callback(m_ram.draw_state.mouse_flags.locked);
    }
}

void vm::private_mask_buttons()
{
    for (int i = 0; i < 64; ++i)
        m_state.buttons[2][i] = 0;
}

var<bool, int16_t> vm::api_btn(opt<int16_t> n, int16_t p)
{
    if (n)
    {
        if (n < 0 || n >= 8 || p < 0 || p >= 8) return false;
        return bool(m_state.buttons[0][(*n + 8 * p) & 0x3f]);
    }   

    int16_t bits = 0;
    for (int i = 0; i < 16; ++i)
        bits |= m_state.buttons[0][i] ? 1 << i : 0;
    return bits;
}

var<bool, int16_t> vm::api_btnp(opt<int16_t> n, int16_t p)
{
    // Autorepeat behaviour is overridden by the hw state, and 255 disables it
    // (https://twitter.com/lexaloffle/status/1176688167719587841)
    int delay = m_ram.hw_state.btnp_delay ? m_ram.hw_state.btnp_delay : 15;
    int rate = m_ram.hw_state.btnp_rate ? m_ram.hw_state.btnp_rate : 4;

    auto was_pressed = [delay, rate](int i) -> bool
    {
        // “Same as btn() but only true when the button was not pressed the last frame”
        if (i == 1)
            return true;
        // “btnp() also returns true every 4 frames after the button is held for 15 frames.”
        if (delay != 255 && i > delay && (i - delay - 1) % rate == 0)
            return true;
        return false;
    };

    if (n)
    {
        if (n < 0 || n >= 8 || p < 0 || p >= 8) return false;
        return was_pressed(m_state.buttons[0][(*n + 8 * p) & 0x3f]);
    }

    int16_t bits = 0;
    for (int i = 0; i < 16; ++i)
        bits |= was_pressed(m_state.buttons[0][i]) ? 1 << i : 0;
    return bits;
}

void vm::api_serial(int16_t chan, int16_t address, int16_t len)
{
    private_stub(std::format("serial(0x{:4x}, 0x{:4x}, 0x{:4x})", chan, address, len));
}

//
// Deprecated
//

fix32 vm::api_time()
{
    return fix32(m_time);
}

} // namespace z8::pico8

