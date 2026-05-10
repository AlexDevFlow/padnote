#-------------------------------------------------------------------------------
# padnote-- — main executable target.
#-------------------------------------------------------------------------------

set(NPP_LINUX_SOURCES
    main_qt.cpp
    MainWindow.cpp
    MainWindow.h
    Buffer.cpp
    Buffer.h
    DocumentMapDock.cpp
    DocumentMapDock.h
    DocumentSwitcherDialog.cpp
    DocumentSwitcherDialog.h
    EditorTabs.cpp
    EditorTabs.h
    FileBrowserDock.cpp
    FileBrowserDock.h
    FindReplaceDialog.cpp
    FindReplaceDialog.h
    FunctionList.cpp
    FunctionList.h
    FunctionListXml.cpp
    FunctionListXml.h
    FunctionListDock.cpp
    FunctionListDock.h
    PluginLoader.cpp
    PluginLoader.h
    PreferencesDialog.cpp
    PreferencesDialog.h
    ProjectPanelDock.cpp
    ProjectPanelDock.h
    Languages.cpp
    Languages.h
    Theme.cpp
    Theme.h
    Config.cpp
    Config.h
    Session.cpp
    Session.h
    HashDialog.cpp
    HashDialog.h
    RunDialog.cpp
    RunDialog.h
    SingleInstance.cpp
    SingleInstance.h
    StyleConfigDialog.cpp
    StyleConfigDialog.h
    ShortcutMapperDialog.cpp
    ShortcutMapperDialog.h
    ColumnEditorDialog.cpp
    ColumnEditorDialog.h
    Backup.cpp
    Backup.h
    Localization.cpp
    Localization.h
    MacroManager.cpp
    MacroManager.h
    Encoding.cpp
    Encoding.h
    UserDefineDialog.cpp
    UserDefineDialog.h
    UserDefineLang.cpp
    UserDefineLang.h
    win32_compat.h
    resources.qrc
    # PugiXML (vendored) — used by Languages.cpp to parse langs.model.xml,
    # stylers.xml, config.xml, session.xml.
    ${CMAKE_SOURCE_DIR}/PowerEditor/src/pugixml/pugixml.cpp
    ${CMAKE_SOURCE_DIR}/PowerEditor/src/pugixml/pugixml.hpp
    ${CMAKE_SOURCE_DIR}/PowerEditor/src/pugixml/pugiconfig.hpp

    # uchardet (vendored) — charset detection at file load.
    ${CMAKE_SOURCE_DIR}/PowerEditor/src/uchardet/CharDistribution.cpp
    ${CMAKE_SOURCE_DIR}/PowerEditor/src/uchardet/JpCntx.cpp
    ${CMAKE_SOURCE_DIR}/PowerEditor/src/uchardet/LangBulgarianModel.cpp
    ${CMAKE_SOURCE_DIR}/PowerEditor/src/uchardet/LangCyrillicModel.cpp
    ${CMAKE_SOURCE_DIR}/PowerEditor/src/uchardet/LangGreekModel.cpp
    ${CMAKE_SOURCE_DIR}/PowerEditor/src/uchardet/LangHebrewModel.cpp
    ${CMAKE_SOURCE_DIR}/PowerEditor/src/uchardet/LangHungarianModel.cpp
    ${CMAKE_SOURCE_DIR}/PowerEditor/src/uchardet/LangThaiModel.cpp
    ${CMAKE_SOURCE_DIR}/PowerEditor/src/uchardet/nsBig5Prober.cpp
    ${CMAKE_SOURCE_DIR}/PowerEditor/src/uchardet/nsCharSetProber.cpp
    ${CMAKE_SOURCE_DIR}/PowerEditor/src/uchardet/nsEscCharsetProber.cpp
    ${CMAKE_SOURCE_DIR}/PowerEditor/src/uchardet/nsEscSM.cpp
    ${CMAKE_SOURCE_DIR}/PowerEditor/src/uchardet/nsEUCJPProber.cpp
    ${CMAKE_SOURCE_DIR}/PowerEditor/src/uchardet/nsEUCKRProber.cpp
    ${CMAKE_SOURCE_DIR}/PowerEditor/src/uchardet/nsEUCTWProber.cpp
    ${CMAKE_SOURCE_DIR}/PowerEditor/src/uchardet/nsGB2312Prober.cpp
    ${CMAKE_SOURCE_DIR}/PowerEditor/src/uchardet/nsHebrewProber.cpp
    ${CMAKE_SOURCE_DIR}/PowerEditor/src/uchardet/nsLatin1Prober.cpp
    ${CMAKE_SOURCE_DIR}/PowerEditor/src/uchardet/nsMBCSGroupProber.cpp
    ${CMAKE_SOURCE_DIR}/PowerEditor/src/uchardet/nsMBCSSM.cpp
    ${CMAKE_SOURCE_DIR}/PowerEditor/src/uchardet/nsSBCharSetProber.cpp
    ${CMAKE_SOURCE_DIR}/PowerEditor/src/uchardet/nsSBCSGroupProber.cpp
    ${CMAKE_SOURCE_DIR}/PowerEditor/src/uchardet/nsSJISProber.cpp
    ${CMAKE_SOURCE_DIR}/PowerEditor/src/uchardet/nsUniversalDetector.cpp
    ${CMAKE_SOURCE_DIR}/PowerEditor/src/uchardet/nsUTF8Prober.cpp
    ${CMAKE_SOURCE_DIR}/PowerEditor/src/uchardet/uchardet.cpp
)

add_executable(padnote ${NPP_LINUX_SOURCES})

target_include_directories(padnote PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/PowerEditor/src           # for menuCmdID.h, keys.h
    ${CMAKE_SOURCE_DIR}/PowerEditor/src/MISC/PluginsManager  # Notepad_plus_msgs.h
    ${CMAKE_SOURCE_DIR}/PowerEditor/src/pugixml   # pugixml.hpp
    ${CMAKE_SOURCE_DIR}/PowerEditor/src/uchardet  # uchardet.h
    ${CMAKE_SOURCE_DIR}/scintilla/include          # Scintilla.h, ILexer.h
    ${CMAKE_SOURCE_DIR}/lexilla/include            # Lexilla.h, SciLexer.h
    ${CMAKE_SOURCE_DIR}/lexilla/lexlib             # LexerModule.h, Accessor.h, WordList.h
)

target_compile_definitions(padnote PRIVATE
    PADNOTE_VERSION_STRING="${CMAKE_PROJECT_VERSION}"
)

target_link_libraries(padnote PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Core5Compat
    Qt6::PrintSupport
    Qt6::DBus
    scintilla
    lexilla
    ${CMAKE_DL_LIBS}    # dlopen / dlsym / dlclose for plugin loader
)

# Export host symbols to dlopen'd plugin .so files. Future host
# helpers callable from plugin code resolve via dlsym(NULL, "...").
target_link_options(padnote PRIVATE -rdynamic)

npp_add_warning_flags(padnote)
npp_add_hardening_flags(padnote)

# uchardet's nsCharSetProber.cpp uses uintptr_t without including
# <cstdint>. Force-include it for just the uchardet TUs.
file(GLOB UCHARDET_SRC
    "${CMAKE_SOURCE_DIR}/PowerEditor/src/uchardet/*.cpp")
set_source_files_properties(${UCHARDET_SRC}
    PROPERTIES COMPILE_OPTIONS "-include;cstdint")

set_target_properties(padnote PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    AUTOMOC ON
    AUTORCC ON
)

# ----------------------------------------------------------------------------
# Desktop integration. `make install` lays out the binary, the .desktop
# entry, MIME definitions, and the icon set into XDG-compliant locations
# under CMAKE_INSTALL_PREFIX (defaults to /usr/local).
# ----------------------------------------------------------------------------
include(GNUInstallDirs)

install(TARGETS padnote
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(FILES "${CMAKE_SOURCE_DIR}/packaging/padnote.desktop"
    DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)

install(FILES "${CMAKE_SOURCE_DIR}/packaging/padnote.xml"
    DESTINATION ${CMAKE_INSTALL_DATADIR}/mime/packages)

# Icon theme: hicolor sizes 16/32/48/64/128/256.
foreach(_padnote_size IN ITEMS 16x16 32x32 48x48 64x64 128x128 256x256 512x512)
    install(FILES
        "${CMAKE_SOURCE_DIR}/packaging/icons/${_padnote_size}/padnote.png"
        DESTINATION
        ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${_padnote_size}/apps)
endforeach()
