cmake_minimum_required(VERSION 3.16)

# CMakeLists.txt for micro-mp3
# Supports both ESP-IDF component builds and standalone host builds
#
# Structure:
#   cmake/sources.cmake   - Source file definitions (references src/opencore-mp3dec/)
#   cmake/functions.cmake - Helper functions
#   cmake/esp-idf.cmake   - ESP-IDF specific configuration
#   cmake/host.cmake      - Host platform configuration

# ==============================================================================
# Build type detection
# ==============================================================================

if(DEFINED IDF_TARGET)
    set(ESP_IDF_BUILD TRUE)
else()
    set(ESP_IDF_BUILD FALSE)
endif()

# Set source directory variable for consistent use
if(ESP_IDF_BUILD)
    set(MP3_COMPONENT_DIR ${COMPONENT_DIR})
else()
    set(MP3_COMPONENT_DIR ${CMAKE_CURRENT_SOURCE_DIR})
endif()

# ==============================================================================
# Include CMake modules
# ==============================================================================

include(${MP3_COMPONENT_DIR}/cmake/sources.cmake)
include(${MP3_COMPONENT_DIR}/cmake/functions.cmake)

# ==============================================================================
# ESP-IDF Build
# ==============================================================================

if(ESP_IDF_BUILD)
    include(${MP3_COMPONENT_DIR}/cmake/esp-idf.cmake)

    # Get sources
    mp3_get_sources()

    # Collect all sources
    set(ESP_MP3_SOURCES
        ${MP3_LIB_SOURCES}
        ${MP3_WRAPPER_SOURCES}
    )

    # Register the component
    idf_component_register(
        SRCS ${ESP_MP3_SOURCES}
        INCLUDE_DIRS
            "include"
    )

    # Apply ESP-IDF configuration
    mp3_configure_esp_idf(${COMPONENT_LIB} ${COMPONENT_DIR})

# ==============================================================================
# Host Build
# ==============================================================================

else()
    project(micro_mp3 VERSION 0.1.1 LANGUAGES CXX)

    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/host.cmake)

    # Get sources
    mp3_get_sources()

    # Collect all sources
    set(HOST_MP3_SOURCES
        ${MP3_LIB_SOURCES}
        ${MP3_WRAPPER_SOURCES}
    )

    # Create the library
    add_library(micro_mp3 STATIC ${HOST_MP3_SOURCES})

    # Apply host configuration
    mp3_configure_host(micro_mp3 ${CMAKE_CURRENT_SOURCE_DIR})

endif()
