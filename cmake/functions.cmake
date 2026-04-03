# cmake/functions.cmake
# Helper functions for micro-mp3 build system

# Guard against multiple inclusion
if(__mp3_functions_defined)
    return()
endif()
set(__mp3_functions_defined TRUE)

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
