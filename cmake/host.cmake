# cmake/host.cmake
# Host platform build configuration for micro-mp3

# Guard against multiple inclusion
if(__mp3_host_defined)
    return()
endif()
set(__mp3_host_defined TRUE)

# Capture path at include-time (CMAKE_CURRENT_LIST_DIR in functions resolves to caller)
get_filename_component(_MP3_OPENCORE_DIR "${CMAKE_CURRENT_LIST_DIR}/../src/opencore-mp3dec" ABSOLUTE)

# ==============================================================================
# mp3_configure_host
# ==============================================================================
# Main configuration function for host builds (Linux, macOS, Windows).
# Call this after creating the library target to set up all host-specific
# configuration.
#
# Requires MP3_LIB_SOURCES to be populated in the calling scope (via
# mp3_get_sources) before this is called: the -w warning suppression below
# reads it. CMake's dynamic scoping makes the function see the caller's
# variable, but the coupling is implicit -- keep mp3_get_sources first.
#
# Arguments:
#   TARGET         - The library target name
#   SOURCE_DIR     - The source directory path (CMAKE_CURRENT_SOURCE_DIR)
# ==============================================================================
function(mp3_configure_host TARGET SOURCE_DIR)
    set(MP3_SOURCE_DIR "${_MP3_OPENCORE_DIR}")

    # Private include directories (internal headers)
    target_include_directories(${TARGET} PRIVATE
        "${MP3_SOURCE_DIR}"
        "${MP3_SOURCE_DIR}/oscl"
    )

    # Public include directories (API headers)
    target_include_directories(${TARGET} PUBLIC
        "${MP3_SOURCE_DIR}"
        "${SOURCE_DIR}/include"
    )

    # Optimization flags. Drop to -O1 under sanitizers so ASan/UBSan reports
    # map cleanly back to source lines; -O2 inlining/reordering otherwise
    # muddies the diagnostics. Host builds otherwise use -O2 (ESP-IDF uses -O3
    # for decoder performance). ENABLE_SANITIZERS is unset on the ESP-IDF path.
    target_compile_options(${TARGET} PRIVATE -ffunction-sections -fdata-sections)
    if(ENABLE_SANITIZERS)
        target_compile_options(${TARGET} PRIVATE -O1)
    else()
        target_compile_options(${TARGET} PRIVATE -O2)
    endif()

    # Require C++14 for the OpenCore MP3 C++ sources
    target_compile_features(${TARGET} PRIVATE cxx_std_14)

    # Strict warnings for our first-party wrapper sources. The OpenCore sources
    # below override these with -w, so warnings only surface from our own code.
    target_compile_options(${TARGET} PRIVATE
        -Wall
        -Wextra
        -Wshadow
        -Wimplicit-fallthrough
        $<$<BOOL:${ENABLE_WERROR}>:-Werror>
    )

    # Suppress warnings from the upstream OpenCore MP3 sources (third-party
    # code we don't control); the strict flags above stay scoped to our wrapper.
    set_source_files_properties(
        ${MP3_LIB_SOURCES}
        PROPERTIES
        COMPILE_FLAGS "-w"
    )

    message(STATUS "micro-mp3: Building for host platform")
endfunction()
