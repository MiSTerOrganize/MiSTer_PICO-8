//
//  lol_shim — Lolengine replacement for MiSTer FPGA port
//
//  <lol/engine.h> — Engine stub
//
//  vm.h includes this for lol::net::http (async download/SPLORE).
//  vm.cpp uses lol::net::http::client and lol::net::http::status.
//
//  We do NOT define __NX__ because that breaks path handling in
//  private.cpp (makes it use Nintendo Switch "save:/" paths).
//  Instead we provide a full lol::net::http::client stub that
//  compiles and works as a member variable but always fails
//  downloads gracefully.
//

#pragma once

// Pull in sub-headers that engine.h normally provides
#include "vector"
#include "math"
#include "msg"
#include "file"
#include "utils"
#include "sys/init.h"

namespace lol
{

// ── net::http stub ────────────────────────────────────────────────────
// Full client stub so it works as a member variable in vm.h
// (the #if !__NX__ guard is NOT triggered — client member exists)
// All downloads fail immediately — SPLORE is not supported on MiSTer.

namespace net { namespace http {

enum class status
{
    pending,
    success,
    error
};

struct client
{
    void get(std::string const &) { m_status = status::error; }
    status get_status() const { return m_status; }
    std::string get_result() const { return {}; }

private:
    status m_status = status::error;
};

}} // namespace net::http

// ── Minimal stubs ─────────────────────────────────────────────────────

namespace app
{
    struct handle { void run() {} };
    inline handle *init(char const *, ivec2, float) { return nullptr; }
}

namespace input
{
    enum class key
    {
        SC_Left, SC_Right, SC_Up, SC_Down,
        SC_Z, SC_X, SC_C, SC_V, SC_N, SC_M,
        SC_P, SC_Return, SC_Backspace, SC_Delete, SC_Insert,
        SC_S, SC_F, SC_E, SC_D, SC_A, SC_Q,
        SC_LShift, SC_Tab, SC_B
    };
}

} // namespace lol
