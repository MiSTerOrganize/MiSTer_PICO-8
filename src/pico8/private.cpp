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

// I know codecvt_utf8 is deprecated, but let’s hope C++ comes with a
// replacement before they actually remove the feature.
#define _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING 1

#include <lol/msg>   // lol::msg
#include <lol/utils> // lol::ends_with
#include "compat_format.h"

#include <locale>
#include <string>
#include <codecvt>
#include <cstring>

#include "pico8/pico8.h"
#include "pico8/vm.h"

namespace z8::pico8
{

std::string_view charset::to_utf8[256];
std::u32string_view charset::to_utf32[256];

static uint8_t multibyte_start[256];
static std::map<std::string, uint8_t> to_pico8;
std::regex charset::utf8_regex = charset::static_init();

std::regex charset::static_init()
{
    std::wstring_convert<std::codecvt_utf8<char32_t>, char32_t> cvt;

    // The complete PICO-8 charmap, from 0 to 255. We cannot just store
    // codepoints because some emoji glyphs are combinations of several
    // codepoints, e.g. ⬇️ is U+2B07 (down arrow) + U+FE0F (variation
    // selector-16).
    static char const utf8_chars[] =
        "\0¹²³⁴⁵⁶⁷⁸\t\nᵇᶜ\rᵉᶠ▮■□⁙⁘‖◀▶「」¥•、。゛゜"
        " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNO"
        "PQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~○"
        "█▒🐱⬇️░✽●♥☉웃⌂⬅️😐♪🅾️◆…➡️★⧗⬆️ˇ∧❎▤▥あいうえおか"
        "きくけこさしすせそたちつてとなにぬねのはひふへほまみむめもやゆよ"
        "らりるれろわをんっゃゅょアイウエオカキクケコサシスセソタチツテト"
        "ナニヌネノハヒフヘホマミムメモヤユヨラリルレロワヲンッャュョ◜◝";
    static auto utf32_chars = cvt.from_bytes(utf8_chars, &utf8_chars[sizeof(utf8_chars)]);

    // Create all sorts of lookup tables for PICO-8 character conversions
    char const *p8 = utf8_chars;
    auto const *p32 = (char32_t const *)utf32_chars.data();
    std::string regex("(");
    for (int i = 0; i < 256; ++i)
    {
        size_t len32 = p32[1] == 0xfe0f ? 2 : 1;
        size_t len8 = ((0xe5000000 >> ((*p8 >> 3) & 0x1e)) & 3) + len32 * len32;
        to_utf8[i] = std::string_view(p8, len8);
        to_utf32[i] = std::u32string_view(p32, len32);
        to_pico8[std::string(p8, len8)] = i;

        // Build a regex that lets us do faster (maybe?) UTF-8 conversions
        if (len8 > 1)
        {
            multibyte_start[(uint8_t)*p8] = 1;
            regex += std::string(p8, len8) + '|';
        }

        p8 += len8;
        p32 += len32;
    }
    regex += ')'; // Fall back to an empty match on purpose

    return std::regex(regex);
}

std::string charset::utf8_to_pico8(std::string const &str)
{
    std::string ret;
    std::smatch sm;

    for (auto p = str.begin(); p != str.end(); )
    {
        // Only pass known start characters through the expensive regex
        if (multibyte_start[(uint8_t)*p]
             && std::regex_search(p, str.end(), sm, utf8_regex)
             && sm.length() > 1)
        {
            ret += to_pico8[sm.str()];
            p += sm.length();
        }
        else
        {
            ret += *p++;
        }
    }

    return ret;
}

std::string charset::pico8_to_utf8(std::string const &str)
{
    std::string ret;
    for (uint8_t ch : str)
        ret += std::string(to_utf8[ch]);
    return ret;
}

void vm::private_stub(std::string str)
{
    lol::msg::info("z8:stub:%s\n", str.c_str());
}

bool vm::private_is_api(std::string str)
{
    // Find str in function list
    if (api::functions.find(str) != api::functions.end())
        return true;

    // Find str in special glyphs
    if (str.size() == 1 && uint8_t(str[0]) >= 0x80 && uint8_t(str[0]) < 0x80 + 26)
        return true;

    return false;
}

opt<bool> vm::private_cartdata(opt<std::string> str)
{
    // No argument given: we return whether there is data
    if (!str)
        return m_cartdata.size() > 0;

    if (!str->size())
    {
        // Empty argument given: get rid of cart data
        m_cartdata = "";
        return std::nullopt;
    }

    m_cartdata = *str;
    private_stub(std::format("cartdata(\"{}\")", m_cartdata));

    return load_cartdata();
}

std::string vm::get_path_config()
{
#if defined(__MISTER__)
    return "/media/fat/config/zepto8.cfg";
#elif __NX__
    std::string file_path = "save:/";
#else
    #if _WIN32
        std::string base_dir = lol::sys::getenv("APPDATA");
    #else
        std::string base_dir = lol::sys::getenv("HOME") + "/.lexaloffle";
    #endif
    std::string file_path = base_dir + "/" + m_path_config_dir + "/";
#endif
#if !defined(__MISTER__)
    return file_path + "config.txt";
#endif
}

std::string vm::get_path_cstore(std::string cart_name)
{
    // extract name from path
    size_t found;
    found = cart_name.find_last_of("/\\");
    cart_name = cart_name.substr(found);

    // save .p8.png as .p8 (pico 8 do that, not sure why)
    if (lol::ends_with(lol::tolower(cart_name), ".p8.png"))
    {
        cart_name = cart_name.substr(0, cart_name.length() - 4);
    }

    std::string save_directory_name = "cstore";
    if (m_save_slot > 0)
    {
        save_directory_name = save_directory_name + "_" + std::to_string(m_save_slot);
    }

#if defined(__MISTER__)
    std::string file_path = "/media/fat/saves/PICO-8/";
#elif __NX__
    std::string file_path = "save:/";
#else
    #if _WIN32
        std::string base_dir = lol::sys::getenv("APPDATA");
    #else
        std::string base_dir = lol::sys::getenv("HOME") + "/.lexaloffle";
    #endif
    std::string file_path = base_dir + "/" + m_path_config_dir + "/" + save_directory_name + "/";
#endif

#if !__NX__ && !__SCE__
    std::error_code code;
    std::filesystem::create_directories(file_path, code);
#endif
    return file_path + cart_name;
}

std::string vm::get_path_save(std::string cart_name)
{
    std::string save_directory_name = "cdata";
    if (m_save_slot > 0)
    {
        save_directory_name = save_directory_name + "_" + std::to_string(m_save_slot);
    }
#if defined(__MISTER__)
    std::string file_path = "/media/fat/saves/PICO-8/";
#elif __NX__
    std::string file_path = "save:/";
#else
    #if _WIN32
        std::string base_dir = lol::sys::getenv("APPDATA");
    #else
        std::string base_dir = lol::sys::getenv("HOME") + "/.lexaloffle";
    #endif
    std::string file_path = base_dir + "/" + m_path_config_dir + "/" + save_directory_name + "/";
#endif

    cart_name += ".p8d.txt";

#if !__NX__ && !__SCE__
    std::error_code code;
    std::filesystem::create_directories(file_path, code);
#endif
    return file_path + cart_name;
}

std::string vm::get_default_carts_dir()
{
#if defined(__MISTER__)
    std::string base_dir = lol::sys::getenv("ZEPTO8_BASE_DIR");
    if (base_dir.empty()) base_dir = ".";
    std::string file_path = base_dir + "/Carts/";
#else
    #if _WIN32
        std::string base_dir = lol::sys::getenv("APPDATA");
    #else
        std::string base_dir = lol::sys::getenv("HOME") + "/.lexaloffle";
    #endif
        std::string file_path = base_dir + "/" + m_path_config_dir + "/carts/";
#endif

    #if !__NX__ && !__SCE__
        std::error_code code;
        std::filesystem::create_directories(file_path, code);
    #endif
        return file_path;
}

std::string vm::get_path_active_dir()
{
    if (m_path_active_dir.size() == 0)
        m_path_active_dir = get_default_carts_dir();
    return m_path_active_dir;
}

void vm::set_path_active_dir(std::string filename)
{
    size_t found;
    found = filename.find_last_of("/\\");
    m_path_active_dir = filename.substr(0, found);
}

std::vector<std::string> vm::private_dir(opt<std::string> target_dir)
{
    std::vector<std::string> files;
    std::string path = get_path_active_dir();
    if (target_dir) path = path + *target_dir;
    if (std::filesystem::exists(path))
    {
        // add directories
        for (const auto& entry : std::filesystem::directory_iterator(path))
            if (entry.is_directory())
                files.push_back(entry.path().filename().string() + "/");
        // add files
        for (const auto& entry : std::filesystem::directory_iterator(path))
            if (!entry.is_directory())
                files.push_back(entry.path().filename().string());
    }
    // FIXME: LUA doesn't seems to support returning tupple of more than 32 strings
    if (files.size() > 32)
    {
        files.resize(32);
    }
    return files;
}

void vm::private_set_pause(bool pause)
{
    m_in_pause = pause;
}

} // namespace z8::pico8

