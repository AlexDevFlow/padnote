#-------------------------------------------------------------------------------
# Lexilla 5.48 — syntax highlighting lexers, built as a static library.
#
# Includes the catalogue (src/Lexilla.cxx), the helper library (lexlib/), and
# all 128 built-in lexers (lexers/Lex*.cxx) so all built-in languages work
# from the start.
#-------------------------------------------------------------------------------

file(GLOB LEXILLA_LEXERS CONFIGURE_DEPENDS "lexers/Lex*.cxx")
file(GLOB LEXILLA_LEXLIB CONFIGURE_DEPENDS "lexlib/*.cxx")

# LexUser.cxx is Notepad++'s UDL (User Defined Language) lexer — GPL-licensed
# NPP code that happens to live in Lexilla's lexers/ dir. The original
# `#include <windows.h>` was vestigial; removed in Phase 5U.1 of the Linux
# port so this file now compiles unchanged. lmUserDefine is exported and
# registered by Lexilla's catalogue (src/Lexilla.cxx); LexUserStub is gone.

add_library(lexilla STATIC
    src/Lexilla.cxx
    ${LEXILLA_LEXLIB}
    ${LEXILLA_LEXERS}
)

target_include_directories(lexilla
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/lexlib
        ${CMAKE_CURRENT_SOURCE_DIR}/access
        ${CMAKE_SOURCE_DIR}/scintilla/include   # for ILexer.h
)

target_compile_definitions(lexilla PRIVATE
    LEXILLA_STATIC=1
)

# Lexilla is a vendored upstream tree; suppress noisy warnings.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(lexilla PRIVATE
        # Force <cstdint> ahead of every TU. Upstream scintilla/include/Sci_Position.h
        # uses intptr_t but only includes <stddef.h>, which on g++ does not pull
        # in intptr_t. Scintilla's own .cxx files include <cstdint> before reaching
        # the header so they don't trip; Lexilla lexer files don't, so they do.
        -include cstdint
        -Wno-deprecated-declarations
        -Wno-unused-parameter
        -Wno-unused-but-set-variable
        -Wno-unused-variable
        -Wno-narrowing
    )
endif()
