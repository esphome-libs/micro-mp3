cmake_minimum_required(VERSION 3.16)
project(micro_mp3_tests CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# script/clang-tidy.sh can lint the tests against this build's compile DB
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Option to enable sanitizers for debugging
option(ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

# Treat warnings as errors (also picked up by the micro_mp3 library build below)
option(ENABLE_WERROR "Treat warnings as errors" OFF)

if(ENABLE_SANITIZERS)
    message(STATUS "Building with AddressSanitizer and UndefinedBehaviorSanitizer")
    # The OpenCore fixed-point DSP intentionally left-shifts negative values in
    # ~100 places (documented in src/opencore-mp3dec/CHANGES.md); this is benign
    # on every target we support but is technically UB. Disable only the
    # shift-base check so UBSan still catches the bugs that matter (OOB indexing,
    # shift-by-width, etc.) without drowning in known, intentional noise.
    set(SANITIZER_FLAGS
        "-fsanitize=address,undefined -fno-sanitize=shift-base -fno-omit-frame-pointer -g")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${SANITIZER_FLAGS}")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${SANITIZER_FLAGS}")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${SANITIZER_FLAGS}")
endif()

# Build micro-mp3 as a standard host library
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/.. micro-mp3-build)

add_executable(test_mp3_decoder test_mp3_decoder.cpp)
target_link_libraries(test_mp3_decoder PRIVATE micro_mp3)
target_compile_options(test_mp3_decoder PRIVATE
    -Wall
    -Wextra
    -Wshadow
    -Wimplicit-fallthrough
    $<$<BOOL:${ENABLE_WERROR}>:-Werror>
)

# Test fixtures are checked in; regenerate with generate_test_data.sh if the
# fixture set needs to change (requires ffmpeg with libmp3lame).
set(TEST_DATA_DIR ${CMAKE_CURRENT_SOURCE_DIR}/data)
foreach(fixture
    sine_stereo_44100.mp3
    sine_mono_44100.mp3
    sine_stereo_22050.mp3
    sine_mono_8000.mp3
    sine_stereo_44100_vbr.mp3
    sine_stereo_44100_noxing.mp3)
    if(NOT EXISTS ${TEST_DATA_DIR}/${fixture})
        message(FATAL_ERROR "Test fixture missing: ${fixture}; run generate_test_data.sh first")
    endif()
endforeach()

enable_testing()

set(DECODER_TESTS
    stream_info_contract
    chunked_invariance
    decode_accuracy
    reset_reuse
    error_contract
    per_stream_output_buffer
    stream_info_change
    corrupt_frame_recovery
    sideinfo_overread_guard
    leading_garbage_resync
    id3v2_skip
    gapless_trim
    crc_validation
)

foreach(test_name IN LISTS DECODER_TESTS)
    add_test(NAME ${test_name}
             COMMAND test_mp3_decoder ${TEST_DATA_DIR} ${test_name})
endforeach()
