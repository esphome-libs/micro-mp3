# cmake/esp-idf.cmake
# ESP-IDF specific build configuration for micro-mp3

# Guard against multiple inclusion
if(__mp3_esp_idf_defined)
    return()
endif()
set(__mp3_esp_idf_defined TRUE)

# Capture path at include-time (CMAKE_CURRENT_LIST_DIR in functions resolves to caller)
get_filename_component(_MP3_OPENCORE_DIR "${CMAKE_CURRENT_LIST_DIR}/../src/opencore-mp3dec" ABSOLUTE)

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
        "${MP3_SOURCE_DIR}/oscl"
    )

    # Memory placement via Kconfig
    if(CONFIG_MP3_DECODER_PREFER_PSRAM)
        target_compile_definitions(${COMPONENT_LIB} PRIVATE MP3_DECODER_PREFER_PSRAM)
        message(STATUS "micro-mp3: Decoder memory preference: PSRAM (fall back to internal)")
    elseif(CONFIG_MP3_DECODER_PREFER_INTERNAL)
        target_compile_definitions(${COMPONENT_LIB} PRIVATE MP3_DECODER_PREFER_INTERNAL)
        message(STATUS "micro-mp3: Decoder memory preference: internal (fall back to PSRAM)")
    elseif(CONFIG_MP3_DECODER_PSRAM_ONLY)
        target_compile_definitions(${COMPONENT_LIB} PRIVATE MP3_DECODER_PSRAM_ONLY)
        message(STATUS "micro-mp3: Decoder memory preference: PSRAM only")
    elseif(CONFIG_MP3_DECODER_INTERNAL_ONLY)
        target_compile_definitions(${COMPONENT_LIB} PRIVATE MP3_DECODER_INTERNAL_ONLY)
        message(STATUS "micro-mp3: Decoder memory preference: internal only")
    endif()

    # Suppress warnings for old OpenCore C++ code
    target_compile_options(${COMPONENT_LIB} PRIVATE
        -Wno-unused-variable
        -Wno-unused-but-set-variable
        -Wno-sign-compare
    )

    # Set optimization flags
    mp3_set_optimization_flags(${COMPONENT_LIB})
endfunction()
