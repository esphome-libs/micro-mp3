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

    # Suppress warnings for old OpenCore C++ code
    target_compile_options(${TARGET} PRIVATE
        -Wno-unused-variable
        -Wno-unused-but-set-variable
        -Wno-sign-compare
    )

    # Host builds use -O2 (ESP-IDF builds use -O3 for decoder performance)
    target_compile_options(${TARGET} PRIVATE -O2 -ffunction-sections -fdata-sections)

    # Require C++14 for the OpenCore MP3 C++ sources
    target_compile_features(${TARGET} PRIVATE cxx_std_14)

    message(STATUS "micro-mp3: Building for host platform")
endfunction()
