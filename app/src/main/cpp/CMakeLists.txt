cmake_minimum_required(VERSION 3.22.1)
project(t2k C)

# The emulator core is fetched (at a pinned commit, with our blitter patch applied) by setup.sh into ./deps.
get_filename_component(CORE "${CMAKE_CURRENT_SOURCE_DIR}/../../../../deps/virtualjaguar-libretro" REALPATH)
if(NOT EXISTS "${CORE}/libretro.c")
  message(FATAL_ERROR "Emulator core not found at ${CORE}. Run ./setup.sh first.")
endif()

# The core's blitter has hand-written SIMD paths per CPU family.
if(ANDROID_ABI STREQUAL "arm64-v8a")
  set(SIMD_SOURCE src/tom/blitter_simd_neon.c)
  set(SIMD_DEFINE BLITTER_SIMD_NEON)
elseif(ANDROID_ABI STREQUAL "x86_64")
  set(SIMD_SOURCE src/tom/blitter_simd_sse2.c)
  set(SIMD_DEFINE BLITTER_SIMD_SSE2)
else()
  message(FATAL_ERROR "Unsupported ABI ${ANDROID_ABI} (arm64-v8a and x86_64 are supported)")
endif()

# version.h: the core's own scripts generate it with sh; done here in CMake so that it works on every host.
file(STRINGS "${CORE}/Makefile" CORE_BASE_LINE REGEX "^CORE_BASE_VERSION[ \t]*:?=" LIMIT_COUNT 1)
string(REGEX REPLACE "^[^=]*=[ \t]*" "" CORE_BASE_VERSION "${CORE_BASE_LINE}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/gen/version.h"
"#ifndef VJAG_VERSION_H\n#define VJAG_VERSION_H\n#define CORE_BASE_VERSION \"${CORE_BASE_VERSION}\"\n#define GIT_VERSION \" pinned\"\n#define CORE_VERSION CORE_BASE_VERSION GIT_VERSION\n#endif\n")

include("${CMAKE_CURRENT_SOURCE_DIR}/core_sources.cmake")
list(APPEND CORE_SOURCES ${SIMD_SOURCE})
list(TRANSFORM CORE_SOURCES PREPEND "${CORE}/")
set(CHDR "${CORE}/deps/libchdr/unity.c")

add_library(t2k SHARED t2k_android.c ${CORE_SOURCES} ${CHDR})

target_include_directories(t2k PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/gen"
    "${CORE}" "${CORE}/src" "${CORE}/src/core" "${CORE}/src/tom" "${CORE}/src/jerry" "${CORE}/src/cd" "${CORE}/src/bios"
    "${CORE}/src/m68000" "${CORE}/src/debug" "${CORE}/libretro-common/include" "${CORE}/deps/libchdr/include"
    "${CORE}/deps/libchdr/deps/lzma-25.01/include" "${CORE}/deps/libchdr/deps/miniz-3.1.1" "${CORE}/deps/libchdr/deps/zstd-1.5.7")

# Always optimised, also in debug builds: the emulator is CPU bound.
target_compile_definitions(t2k PRIVATE __LIBRETRO__ ${SIMD_DEFINE} NDEBUG INLINE=inline)
target_compile_options(t2k PRIVATE -O3 -fomit-frame-pointer -fno-stack-protector -Wno-everything)
set_source_files_properties("${CHDR}" PROPERTIES
    COMPILE_FLAGS "-std=c99 -D_7ZIP_ST -DMINIZ_DEFLATE_APIS -DWANT_RAW_DATA_SECTOR=1 -DWANT_SUBCODE=1 -DVERIFY_BLOCK_CRC=1")

target_link_libraries(t2k log android aaudio m)
# 16 KB page-size compatible (required for new Android devices)
target_link_options(t2k PRIVATE -Wl,-z,max-page-size=16384)
