# plugins/CMakeLists.txt — first-party plugin build glue.
#
# Each subdirectory under plugins/ that ships a plugin contributes a
# `add_padnote_plugin(<name>)` call here. The macro builds a SHARED
# library named `<name>.so`, installs it under
# `${CMAKE_INSTALL_LIBDIR}/padnote/plugins/<name>/<name>.so`, and (for
# development) drops a copy under `${CMAKE_BINARY_DIR}/plugins/<name>/`
# so a freshly-built binary can find it without `make install`.
#
# Plugin source layout: plugins/<name>/<name>.c (or .cpp). Single-file
# plugins are the norm; multi-file plugins should bring their own
# CMakeLists.txt and skip add_padnote_plugin.

include(GNUInstallDirs)

function(add_padnote_plugin name)
    set(src "${CMAKE_CURRENT_SOURCE_DIR}/${name}/${name}.c")
    if(NOT EXISTS "${src}")
        set(src "${CMAKE_CURRENT_SOURCE_DIR}/${name}/${name}.cpp")
    endif()
    if(NOT EXISTS "${src}")
        message(FATAL_ERROR "add_padnote_plugin(${name}): no ${name}.c or ${name}.cpp")
    endif()

    add_library(padnote-plugin-${name} MODULE "${src}")

    target_include_directories(padnote-plugin-${name} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"           # for linuxplugin.h
    )

    set_target_properties(padnote-plugin-${name} PROPERTIES
        OUTPUT_NAME "${name}"
        PREFIX ""                               # no `lib` prefix
        LIBRARY_OUTPUT_DIRECTORY
            "${CMAKE_BINARY_DIR}/plugins/${name}"
        C_VISIBILITY_PRESET "default"
        CXX_VISIBILITY_PRESET "default"
    )

    install(TARGETS padnote-plugin-${name}
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/padnote/plugins/${name}"
    )
endfunction()

# ----------------------------------------------------------------------------
# First-party plugins.
# ----------------------------------------------------------------------------

# Minimal proof-of-bundling plugin.
add_padnote_plugin(hello)
