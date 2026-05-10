#-------------------------------------------------------------------------------
# Scintilla 5.61 — Qt 6 backend, built as a static library for the Linux port.
#
# Source list mirrors scintilla/qt/ScintillaEditBase/ScintillaEditBase.pro.
# Other backends (win32/, gtk/, cocoa/) are intentionally excluded from this
# Linux port build.
#-------------------------------------------------------------------------------

set(SCINTILLA_CORE_SOURCES
    src/AutoComplete.cxx
    src/CallTip.cxx
    src/CaseConvert.cxx
    src/CaseFolder.cxx
    src/CellBuffer.cxx
    src/ChangeHistory.cxx
    src/CharacterCategoryMap.cxx
    src/CharacterType.cxx
    src/CharClassify.cxx
    src/ContractionState.cxx
    src/DBCS.cxx
    src/Decoration.cxx
    src/Document.cxx
    src/EditModel.cxx
    src/Editor.cxx
    src/EditView.cxx
    src/Geometry.cxx
    src/Indicator.cxx
    src/KeyMap.cxx
    src/LineMarker.cxx
    src/MarginView.cxx
    src/PerLine.cxx
    src/PositionCache.cxx
    src/RESearch.cxx
    src/RunStyles.cxx
    src/ScintillaBase.cxx
    src/Selection.cxx
    src/Style.cxx
    src/UndoHistory.cxx
    src/UniConversion.cxx
    src/UniqueString.cxx
    src/ViewStyle.cxx
    src/XPM.cxx
)

set(SCINTILLA_QT_SOURCES
    qt/ScintillaEditBase/PlatQt.cpp
    qt/ScintillaEditBase/ScintillaQt.cpp
    qt/ScintillaEditBase/ScintillaEditBase.cpp
    qt/ScintillaEditBase/PlatQt.h
    qt/ScintillaEditBase/ScintillaQt.h
    qt/ScintillaEditBase/ScintillaEditBase.h
)

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Widgets Core5Compat)

add_library(scintilla STATIC
    ${SCINTILLA_CORE_SOURCES}
    ${SCINTILLA_QT_SOURCES}
)

# Scintilla expects these to be set when building the Qt edition as a library.
target_compile_definitions(scintilla
    PRIVATE
        MAKING_LIBRARY=1
    PUBLIC
        SCINTILLA_QT=1
)

target_include_directories(scintilla
    PUBLIC
        # Public — referenced by consumer code (MainWindow et al).
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/qt/ScintillaEditBase>
        # ScintillaEditBase.h #includes "Debugging.h", "Geometry.h", and
        # "Platform.h" from scintilla/src — those are NOT public headers per
        # upstream's intent, but the public ScintillaEditBase.h pulls them in,
        # so consumers need this path too.
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
)

target_link_libraries(scintilla
    PUBLIC
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        Qt6::Core5Compat
)

set_target_properties(scintilla PROPERTIES
    AUTOMOC ON
)

# Suppress upstream warnings we don't want to chase (Scintilla is a vendored
# tree we don't own).
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(scintilla PRIVATE
        -Wno-deprecated-declarations
        -Wno-unused-parameter
        -Wno-unused-but-set-variable
    )
endif()
