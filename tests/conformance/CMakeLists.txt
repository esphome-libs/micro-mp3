cmake_minimum_required(VERSION 3.16)
project(mp3_conformance CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(ENABLE_WERROR "Treat warnings as errors" OFF)

if(ENABLE_SANITIZERS)
    message(STATUS "Building with AddressSanitizer and UndefinedBehaviorSanitizer")
    # The OpenCore fixed-point DSP intentionally left-shifts negative values in
    # ~100 places (documented in src/opencore-mp3dec/CHANGES.md). This harness
    # runs the DSP, so disable only the shift-base check; UBSan still catches
    # the bugs that matter (OOB indexing, shift-by-width, etc.).
    set(SANITIZER_FLAGS
        "-fsanitize=address,undefined -fno-sanitize=shift-base -fno-omit-frame-pointer -g")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${SANITIZER_FLAGS}")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${SANITIZER_FLAGS}")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${SANITIZER_FLAGS}")
endif()

# Build micro_mp3 as a host library.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../.. micro-mp3-build)

add_executable(conformance conformance.cpp)
target_link_libraries(conformance PRIVATE micro_mp3)

add_executable(analyze analyze.cpp)
target_link_libraries(analyze PRIVATE micro_mp3)

target_compile_options(conformance PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wconversion
    -Wsign-conversion
    $<$<BOOL:${ENABLE_WERROR}>:-Werror>
)
