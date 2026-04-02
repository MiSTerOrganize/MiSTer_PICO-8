cmake_minimum_required(VERSION 3.13)
project(MiSTer_PICO-8 C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)

# Default to Release build
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ── Compiler flags ─────────────────────────────────────────────────────
# ARM flags are set via CMAKE_C_FLAGS / CMAKE_CXX_FLAGS from the build script
# or toolchain file. For local x86 testing, none are needed.

set(CMAKE_CXX_FLAGS_RELEASE "-Ofast -DNDEBUG")
set(CMAKE_C_FLAGS_RELEASE "-Ofast -DNDEBUG")

# Suppress known upstream warnings
add_compile_options(-Wno-unused-parameter -Wno-sign-compare)

# Global defines
add_definitions(-DHAVE_CONFIG_H=0 -DPACKAGE_VERSION="0.0.0" -D__MISTER__=1)

# ── Include paths ──────────────────────────────────────────────────────
# lol_shim MUST come first to intercept <lol/...> includes
include_directories(BEFORE
    ${CMAKE_SOURCE_DIR}/src/lol_shim
)
include_directories(
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/3rdparty/z8lua
    ${CMAKE_SOURCE_DIR}/src/3rdparty/lodepng
)

# ── z8lua (compiled as C++) ────────────────────────────────────────────
# z8lua .c files must be compiled as C++ because fix32.h uses C++ features
# Explicit file list matches upstream Makefile.am (excludes lmathlib, loslib, liolib, loadlib)
set(Z8LUA_SOURCES
    src/3rdparty/z8lua/lapi.c    src/3rdparty/z8lua/lcode.c   src/3rdparty/z8lua/ldebug.c
    src/3rdparty/z8lua/ldo.c     src/3rdparty/z8lua/ldump.c   src/3rdparty/z8lua/lfunc.c
    src/3rdparty/z8lua/lgc.c     src/3rdparty/z8lua/llex.c    src/3rdparty/z8lua/lmem.c
    src/3rdparty/z8lua/lobject.c src/3rdparty/z8lua/lopcodes.c src/3rdparty/z8lua/lparser.c
    src/3rdparty/z8lua/lstate.c  src/3rdparty/z8lua/lstring.c src/3rdparty/z8lua/ltable.c
    src/3rdparty/z8lua/ltm.c     src/3rdparty/z8lua/lundump.c src/3rdparty/z8lua/lvm.c
    src/3rdparty/z8lua/lzio.c    src/3rdparty/z8lua/lctype.c  src/3rdparty/z8lua/lauxlib.c
    src/3rdparty/z8lua/lbaselib.c src/3rdparty/z8lua/lcorolib.c src/3rdparty/z8lua/ltablib.c
    src/3rdparty/z8lua/lstrlib.c src/3rdparty/z8lua/lpico8lib.c src/3rdparty/z8lua/linit.c
    src/3rdparty/z8lua/ldblib.c  src/3rdparty/z8lua/eris.c
)
set_source_files_properties(${Z8LUA_SOURCES} PROPERTIES LANGUAGE CXX)
add_library(z8lua STATIC ${Z8LUA_SOURCES})
target_compile_definitions(z8lua PRIVATE LUA_USE_POSIX LUA_USE_STRTODHEX)
target_include_directories(z8lua PRIVATE src/3rdparty/z8lua)

# ── lodepng ────────────────────────────────────────────────────────────
add_library(lodepng STATIC src/3rdparty/lodepng/lodepng.cpp)
target_include_directories(lodepng PRIVATE src/3rdparty/lodepng)

# ── QuickJS (compiled as C) ───────────────────────────────────────────
set(QUICKJS_SOURCES
    src/3rdparty/quickjs/quickjs.c
    src/3rdparty/quickjs/libbf.c
    src/3rdparty/quickjs/libregexp.c
    src/3rdparty/quickjs/libunicode.c
    src/3rdparty/quickjs/cutils.c
)
add_library(quickjs STATIC ${QUICKJS_SOURCES})
target_include_directories(quickjs PRIVATE src/3rdparty/quickjs)
target_compile_definitions(quickjs PRIVATE
    _GNU_SOURCE
    CONFIG_VERSION="2019-08-18"
)
target_compile_options(quickjs PRIVATE
    -Wno-unused-parameter -Wno-sign-compare
)

# ── libzepto8 core ─────────────────────────────────────────────────────
set(ZEPTO8_CORE_SOURCES
    src/vm.cpp
    src/bios.cpp
    src/synth.cpp
    src/filter.cpp
    src/textfile.cpp
    src/pico8/vm.cpp
    src/pico8/cart.cpp
    src/pico8/gfx.cpp
    src/pico8/sfx.cpp
    src/pico8/render.cpp
    src/pico8/code.cpp
    src/pico8/ast.cpp
    src/pico8/parser.cpp
    src/pico8/private.cpp
    src/pico8/api.cpp
)
add_library(zepto8core STATIC ${ZEPTO8_CORE_SOURCES})
target_link_libraries(zepto8core z8lua lodepng quickjs)

# ── MiSTer frontend ───────────────────────────────────────────────────
# SDL 1.2.15 is built as a static library in the CI pipeline.
# SDL_STATIC_PREFIX is set by the workflow cmake invocation.
set(SDL_STATIC_PREFIX "" CACHE STRING "Path to static SDL 1.2 install prefix")

if(SDL_STATIC_PREFIX AND EXISTS "${SDL_STATIC_PREFIX}/lib/libSDL.a")
    # Static SDL from our custom build
    message(STATUS "Using static SDL from: ${SDL_STATIC_PREFIX}")
    set(SDL_INCLUDE_DIR "${SDL_STATIC_PREFIX}/include")
    set(SDL_LIBRARY "${SDL_STATIC_PREFIX}/lib/libSDL.a")
else()
    # Fallback: try system SDL (for local x86 testing)
    find_package(SDL QUIET)
    if(NOT SDL_FOUND)
        message(FATAL_ERROR "SDL not found. Set -DSDL_STATIC_PREFIX=/path/to/sdl")
    endif()
endif()

add_executable(PICO-8 src/mister_main.cpp src/native_video_writer.c)
target_include_directories(PICO-8 PRIVATE ${SDL_INCLUDE_DIR})
target_link_libraries(PICO-8 zepto8core ${SDL_LIBRARY} -lm -ldl -lpthread -lrt)

# Static link C++ runtime so binary works on MiSTer's Buildroot Linux
target_link_options(PICO-8 PRIVATE -static-libstdc++ -static-libgcc)

# ── Install ────────────────────────────────────────────────────────────
install(TARGETS PICO-8 RUNTIME DESTINATION bin)
install(FILES src/pico8/bios.p8 DESTINATION bin/pico8)
