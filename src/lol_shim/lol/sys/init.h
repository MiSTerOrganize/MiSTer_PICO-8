//
//  lol_shim — Lolengine replacement for MiSTer FPGA port
//
//  <lol/sys/init.h> — System path and environment functions
//
//  Provides: sys::get_data_path(filename), sys::getenv(name), sys::init()
//  Used by: cart.cpp (bios loading, png loading), vm.cpp (save paths)
//
//  get_data_path() resolves a filename relative to a configurable base
//  directory. On MiSTer, this will be the directory containing the binary
//  and the bios.p8 file.
//

#pragma once

#include <string>
#include <cstdlib>

namespace lol
{

namespace sys
{

// Global data path prefix. Set this from main() before creating the VM.
// Default: empty string (current working directory)
inline std::string &data_path_prefix()
{
    static std::string s_prefix;
    return s_prefix;
}

// Set the data path prefix (call from main before vm creation)
inline void set_data_path(std::string const &path)
{
    data_path_prefix() = path;
    // Ensure trailing slash
    if (!data_path_prefix().empty() && data_path_prefix().back() != '/')
        data_path_prefix() += '/';
}

// Resolve a data file path. lolengine searches multiple paths;
// we simply prepend the configured prefix.
// Usage: lol::sys::get_data_path("pico8/bios.p8")
[[nodiscard]] inline std::string get_data_path(std::string const &filename)
{
    // If the filename is already an absolute path, return as-is
    if (!filename.empty() && filename[0] == '/')
        return filename;

    return data_path_prefix() + filename;
}

// Wrap ::getenv with a safe std::string return
// Usage: lol::sys::getenv("HOME")
[[nodiscard]] inline std::string getenv(std::string const &name)
{
    char const *val = ::getenv(name.c_str());
    return val ? std::string(val) : std::string();
}

// No-op init (lolengine uses this for SDL/OpenGL/etc init)
inline void init(int, char **) {}

} // namespace sys

} // namespace lol
