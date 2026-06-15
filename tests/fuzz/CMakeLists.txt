cmake_minimum_required(VERSION 3.16)
project(micro_mp3_fuzz CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# libFuzzer is the default. Apple clang ships without libclang_rt.fuzzer, so
# point CMAKE_CXX_COMPILER at Homebrew LLVM (e.g. /opt/homebrew/opt/llvm/bin/clang++).
# Pass -DFUZZ_USE_LIBFUZZER=OFF for the standalone harness instead.
option(FUZZ_USE_LIBFUZZER "Build as a libFuzzer target" ON)

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "Fuzzing requires Clang. Set CMAKE_CXX_COMPILER to clang++.")
endif()

# ASan + UBSan, with two checks disabled for the OpenCore fixed-point DSP:
#
#   shift-base              The DSP intentionally left-shifts negative values in
#                           ~100 places (documented in
#                           src/opencore-mp3dec/CHANGES.md). Disabled here too,
#                           matching tests/CMakeLists.txt.
#   signed-integer-overflow The Q-format butterflies (e.g. the adds/subtracts in
#                           pvmp3_dct_6 / pvmp3_dct_9 and the fxp_mac32 chains)
#                           rely on modular two's-complement int32 wraparound,
#                           UB per the C standard but the intended behavior on
#                           every real target. The standalone battery trips it
#                           on the first noise frame, so it would bury real
#                           findings; same rationale and treatment as Tremor's
#                           MDCT/residue math in micro-vorbis.
#
# Both are benign-by-design value computations, not memory access, so disabling
# them does not weaken the memory-safety coverage that matters (OOB indexing,
# shift-by-width, use-after-free); ASan and the rest of UBSan stay active. Add a
# further suppression only when a clean run proves it noisy, and justify it here.
set(FUZZ_FLAGS "-g -fno-omit-frame-pointer -fsanitize=address,undefined")
set(FUZZ_FLAGS "${FUZZ_FLAGS} -fno-sanitize=shift-base,signed-integer-overflow")
set(FUZZ_FLAGS "${FUZZ_FLAGS} -fno-sanitize-recover=all")

if(FUZZ_USE_LIBFUZZER)
    set(FUZZ_FLAGS "${FUZZ_FLAGS} -fsanitize=fuzzer-no-link")
endif()

set(CMAKE_C_FLAGS          "${CMAKE_C_FLAGS} ${FUZZ_FLAGS}")
set(CMAKE_CXX_FLAGS        "${CMAKE_CXX_FLAGS} ${FUZZ_FLAGS}")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${FUZZ_FLAGS}")

# Convention borrowed from Chromium/OpenSSL: a build flag for code to disable
# per-error logging that malformed inputs would otherwise flood. The wrapper and
# the OpenCore source carry no such logging today, so this is purely a guard for
# any future addition; clang does not define it automatically. Set globally so
# the static library TUs see it too.
add_compile_definitions(FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION=1)

# On macOS with Homebrew LLVM, link against LLVM's libc++ so the sanitizer
# runtime and the C++ library agree. The clang++ driver already adds -lc++
# implicitly; the -L/-rpath just steers it to LLVM's copy over the system one.
# (An explicit -lc++ here would collide with the sanitizer-injected one and
# draw a harmless "ignoring duplicate libraries: '-lc++'" warning from ld.)
if(APPLE)
    get_filename_component(LLVM_BIN_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
    get_filename_component(LLVM_PREFIX  "${LLVM_BIN_DIR}" DIRECTORY)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -L${LLVM_PREFIX}/lib/c++ -Wl,-rpath,${LLVM_PREFIX}/lib/c++")
endif()

# Build micro-mp3 as a host library (same pattern as tests/CMakeLists.txt).
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../.. micro-mp3-build)

# --- MP3 streaming fuzzer ----------------------------------------------------
add_executable(fuzz_mp3_decode fuzz_mp3_decode.cpp)
target_link_libraries(fuzz_mp3_decode PRIVATE micro_mp3)

if(FUZZ_USE_LIBFUZZER)
    target_compile_options(fuzz_mp3_decode PRIVATE -fsanitize=fuzzer)
    target_link_options(fuzz_mp3_decode    PRIVATE -fsanitize=fuzzer)
else()
    target_compile_definitions(fuzz_mp3_decode PRIVATE FUZZ_STANDALONE=1)
endif()
