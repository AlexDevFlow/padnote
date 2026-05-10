#-------------------------------------------------------------------------------
# NppCommon.cmake — shared compile settings for the Notepad++ Linux port
#-------------------------------------------------------------------------------

# Default build type when none is specified (single-config generators only).
get_property(_is_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT _is_multi_config AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING
        "Build type (Debug, Release, RelWithDebInfo, MinSizeRel)" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        Debug Release RelWithDebInfo MinSizeRel)
endif()

# C++20 across the project. Notepad++ upstream uses C++17/20 mixed; the port
# uniformizes to C++20 (g++ 11+ / clang 14+).
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Position-independent code everywhere so we can mix shared and static libs.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Export compile_commands.json so clangd / IDEs can pick up the build.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Warning flags. Treat warnings seriously but do NOT promote to errors yet —
# we'll be touching upstream code that wasn't written with -Wpedantic in mind.
function(npp_add_warning_flags target)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE
            -Wall -Wextra
            -Wno-unused-parameter   # too noisy on Win32-shaped APIs we'll port
            -Wno-missing-field-initializers
        )
    endif()
endfunction()

# Run-time hardening (Linux-specific).
function(npp_add_hardening_flags target)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE
            -fstack-protector-strong
            -D_FORTIFY_SOURCE=2
        )
        target_link_options(${target} PRIVATE
            -Wl,-z,relro,-z,now
            -Wl,-z,noexecstack
        )
    endif()
endfunction()
