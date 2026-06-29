# cmake/esp-idf.cmake
# ESP-IDF specific build configuration for microMP3

# Guard against multiple inclusion
if(__mp3_esp_idf_defined)
    return()
endif()
set(__mp3_esp_idf_defined TRUE)

# Capture path at include-time (CMAKE_CURRENT_LIST_DIR in functions resolves to caller)
get_filename_component(_MP3_OPENCORE_DIR "${CMAKE_CURRENT_LIST_DIR}/../src/opencore-mp3dec" ABSOLUTE)

# ==============================================================================
# mp3_set_optimization_flags
# ==============================================================================
# Sets common optimization compiler flags.
#
# Arguments:
#   TARGET - The target to apply flags to
# ==============================================================================
function(mp3_set_optimization_flags TARGET)
    target_compile_options(${TARGET} PRIVATE
        -O2
        -ffunction-sections
        -fdata-sections
    )
endfunction()

# ==============================================================================
# mp3_set_per_file_optimization
# ==============================================================================
# Compiles the listed hot decoder sources at -Os instead of the -O2 baseline.
# The per-source flag is appended after the target-wide -O2, so -Os wins for
# these files only; every other file keeps -O2.
#
# These transforms and the stereo/huffman/dequant paths are bound by register
# spills and constant reloads, not by their multiplies. At -Os GCC issues fewer
# loads and stores on these files, and they decode measurably faster on hardware
# (verified on esp32-s3 and esp32-p4); -O2's extra unrolling and scheduling cost
# more than they buy here. A few equally hot files (the polyphase window and
# synthesis) regress under -Os, so they are deliberately left at -O2.
#
# set_source_files_properties needs TARGET_DIRECTORY so the property lands in the
# scope where TARGET was defined: under ESP-IDF the component library is created
# in a different scope than this helper, and a plain call there is silently
# ignored (the files would stay at -O2).
#
# Arguments:
#   TARGET       - the library target the sources are compiled into
#   OPENCORE_DIR - absolute path to src/opencore-mp3dec
# ==============================================================================
function(mp3_set_per_file_optimization TARGET OPENCORE_DIR)
    set(_mp3_size_tuned_sources
        pvmp3_alias_reduction.cpp
        pvmp3_dct_16.cpp
        pvmp3_dct_9.cpp
        pvmp3_decode_huff_cw.cpp
        pvmp3_dequantize_sample.cpp
        pvmp3_equalizer.cpp
        pvmp3_huffman_parsing.cpp
        pvmp3_imdct_synth.cpp
        pvmp3_mpeg2_stereo_proc.cpp
        pvmp3_reorder.cpp
        pvmp3_stereo_proc.cpp
    )
    foreach(_src IN LISTS _mp3_size_tuned_sources)
        set_source_files_properties(
            "${OPENCORE_DIR}/${_src}"
            TARGET_DIRECTORY ${TARGET}
            PROPERTIES COMPILE_OPTIONS "-Os"
        )
    endforeach()
endfunction()

# ==============================================================================
# mp3_configure_esp_idf
# ==============================================================================
# Main configuration function for ESP-IDF builds. Call this after
# idf_component_register() to set up all ESP-IDF specific configuration.
#
# Arguments:
#   COMPONENT_LIB   - The component library target name
#   COMPONENT_DIR   - The component directory path
# ==============================================================================
function(mp3_configure_esp_idf COMPONENT_LIB COMPONENT_DIR)
    set(MP3_SOURCE_DIR "${_MP3_OPENCORE_DIR}")

    # Private include directories (internal headers)
    target_include_directories(${COMPONENT_LIB} PRIVATE
        "${MP3_SOURCE_DIR}"
    )

    # Memory placement via Kconfig
    if(CONFIG_MICRO_MP3_PREFER_PSRAM)
        target_compile_definitions(${COMPONENT_LIB} PRIVATE MICRO_MP3_PREFER_PSRAM)
        message(STATUS "micro-mp3: Decoder memory preference: PSRAM (fall back to internal)")
    elseif(CONFIG_MICRO_MP3_PREFER_INTERNAL)
        target_compile_definitions(${COMPONENT_LIB} PRIVATE MICRO_MP3_PREFER_INTERNAL)
        message(STATUS "micro-mp3: Decoder memory preference: internal (fall back to PSRAM)")
    elseif(CONFIG_MICRO_MP3_PSRAM_ONLY)
        target_compile_definitions(${COMPONENT_LIB} PRIVATE MICRO_MP3_PSRAM_ONLY)
        message(STATUS "micro-mp3: Decoder memory preference: PSRAM only")
    elseif(CONFIG_MICRO_MP3_INTERNAL_ONLY)
        target_compile_definitions(${COMPONENT_LIB} PRIVATE MICRO_MP3_INTERNAL_ONLY)
        message(STATUS "micro-mp3: Decoder memory preference: internal only")
    endif()

    # Suppress warnings for old OpenCore C++ code
    target_compile_options(${COMPONENT_LIB} PRIVATE
        -Wno-unused-variable
        -Wno-unused-but-set-variable
        -Wno-sign-compare
    )

    # Set optimization flags (-O2 baseline, then -Os on the files that benefit)
    mp3_set_optimization_flags(${COMPONENT_LIB})
    mp3_set_per_file_optimization(${COMPONENT_LIB} "${MP3_SOURCE_DIR}")
endfunction()
