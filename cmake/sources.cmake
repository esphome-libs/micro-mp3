# cmake/sources.cmake
# Source file definitions for micro-mp3
# Separated from main CMakeLists.txt for maintainability
#
# OpenCore MP3 decoder sources are in src/opencore-mp3dec/

# Guard against multiple inclusion
if(__mp3_sources_defined)
    return()
endif()
set(__mp3_sources_defined TRUE)

# Capture path at include-time (CMAKE_CURRENT_LIST_DIR in functions resolves to caller)
get_filename_component(_MP3_OPENCORE_DIR "${CMAKE_CURRENT_LIST_DIR}/../src/opencore-mp3dec" ABSOLUTE)

# ==============================================================================
# mp3_get_sources
# ==============================================================================
# Populates source file lists for the OpenCore MP3 decoder.
# Sources are located in src/opencore-mp3dec/ relative to the project root.
# ==============================================================================
function(mp3_get_sources)
    set(MP3_DIR "${_MP3_OPENCORE_DIR}")
    # --------------------------------------------------------------------------
    # OpenCore MP3 decoder sources
    # (pvmp3_decoder.cpp excluded: depends on missing OSCL headers)
    # (asm/*.s excluded: ARM assembly not needed for generic builds)
    # --------------------------------------------------------------------------
    set(MP3_LIB_SOURCES
        ${MP3_DIR}/pvmp3_alias_reduction.cpp
        ${MP3_DIR}/pvmp3_crc.cpp
        ${MP3_DIR}/pvmp3_dct_16.cpp
        ${MP3_DIR}/pvmp3_dct_6.cpp
        ${MP3_DIR}/pvmp3_dct_9.cpp
        ${MP3_DIR}/pvmp3_decode_header.cpp
        ${MP3_DIR}/pvmp3_decode_huff_cw.cpp
        ${MP3_DIR}/pvmp3_dequantize_sample.cpp
        ${MP3_DIR}/pvmp3_equalizer.cpp
        ${MP3_DIR}/pvmp3_framedecoder.cpp
        ${MP3_DIR}/pvmp3_get_main_data_size.cpp
        ${MP3_DIR}/pvmp3_get_scale_factors.cpp
        ${MP3_DIR}/pvmp3_get_side_info.cpp
        ${MP3_DIR}/pvmp3_getbits.cpp
        ${MP3_DIR}/pvmp3_huffman_decoding.cpp
        ${MP3_DIR}/pvmp3_huffman_parsing.cpp
        ${MP3_DIR}/pvmp3_imdct_synth.cpp
        ${MP3_DIR}/pvmp3_mdct_18.cpp
        ${MP3_DIR}/pvmp3_mdct_6.cpp
        ${MP3_DIR}/pvmp3_mpeg2_get_scale_data.cpp
        ${MP3_DIR}/pvmp3_mpeg2_get_scale_factors.cpp
        ${MP3_DIR}/pvmp3_mpeg2_stereo_proc.cpp
        ${MP3_DIR}/pvmp3_normalize.cpp
        ${MP3_DIR}/pvmp3_poly_phase_synthesis.cpp
        ${MP3_DIR}/pvmp3_polyphase_filter_window.cpp
        ${MP3_DIR}/pvmp3_reorder.cpp
        ${MP3_DIR}/pvmp3_seek_synch.cpp
        ${MP3_DIR}/pvmp3_stereo_proc.cpp
        ${MP3_DIR}/pvmp3_tables.cpp
        PARENT_SCOPE
    )
endfunction()

# ==============================================================================
# Non-library sources (wrapper, in our src/ directory)
# ==============================================================================

# MP3 decoder C++ wrapper
set(MP3_WRAPPER_SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/../src/mp3_decoder.cpp
)
