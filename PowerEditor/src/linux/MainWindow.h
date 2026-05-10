// MainWindow.h — Phase 3b main window: tabs, full menu skeleton, toolbar,
//                status bar, dockable Find/Replace dialog.
//
// Phase 3d: split view. The central widget is now a QSplitter holding two
// EditorTabs (left + right). The right pane is hidden by default; View →
// Split View toggles it. activePane() follows focus; status bar / menu
// state reflects the active pane only. allBuffers() walks both panes for
// cross-pane bookkeeping (file watcher, backup timer, Find in Open Files).

#pragma once

#include <QMainWindow>
#include <QPointer>
#include <QString>
#include <QSystemTrayIcon>
#include <QVector>
#include <utility>
#include <vector>

#include "Encoding.h"

class Buffer;
class DocumentMapDock;
class EditorTabs;
class FileBrowserDock;
class FindReplaceDialog;
class FunctionListDock;
class MacroManager;
class ProjectPanelDock;
class QAction;
class QLabel;
class QMenu;
class QSplitter;
struct LanguageDef;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

    // Phase 3d — public pane API. activePane() is the pane the user last
    // interacted with (clicked into / switched a tab in). leftPane() and
    // rightPane() are stable for the MainWindow's lifetime; rightPane() is
    // hidden by default until View → Split View toggles it on. allBuffers()
    // returns every Buffer across both panes (left first, then right) — used
    // by Find in Open Files, the file watcher, the backup timer, and Style
    // Configurator's apply.
    EditorTabs* leftPane()  const { return m_panes[0]; }
    EditorTabs* rightPane() const { return m_panes[1]; }
    EditorTabs* activePane() const { return m_panes[m_activePane]; }
    bool        rightPaneVisible() const;
    QVector<Buffer*> allBuffers() const;

    // Phase 3d — open files in the active pane. Used by SingleInstance's
    // D-Bus dispatch and the CLI-args path in main_qt.cpp. Returns the
    // number of files actually opened (zero-length list returns 0).
    int openFilesInActivePane(const QStringList& files);

    // Phase 3d — adopt a fresh Untitled buffer with the given UTF-8 bytes
    // into the active pane. Used by the crash-recovery path in main_qt.cpp.
    Buffer* adoptCrashRecoveredBuffer(const QByteArray& utf8Bytes);

    // Phase 3d — show or hide the right pane. Used by main_qt.cpp's session
    // restore to reapply the saved splitVisible state.
    void setSplitVisible(bool on);

protected:
    void closeEvent(QCloseEvent* e) override;

private slots:
    // File menu
    void onFileNew();
    void onFileOpen();
    bool onFileSave();
    bool onFileSaveAs();
    bool onFileSaveAll();
    void onFileCloseTab();
    void onFileCloseAll();
    void onFileReloadFromDisk();
    // Phase 5j — File menu small wins
    void onFileSaveCopyAs();
    void onFileRename();
    void onFileCloseAllBut();
    void onFileMoveToTrash();
    void onFileLoadSession();
    void onFileSaveSessionAs();
    void onFilePrint();
    void onFilePrintNow();
    void onFilePrintPreview();   // Phase 5AB

    // Edit menu (delegate to Scintilla)
    void onEditUndo();
    void onEditRedo();
    void onEditCut();
    void onEditCopy();
    void onEditPaste();
    void onEditDelete();
    void onEditSelectAll();
    void onEditUpperCase();
    void onEditLowerCase();
    // Phase 9r — Convert Case To submenu fillers (upstream set has more
    // variants but Proper / Invert / Random cover the daily-driver case).
    void onEditProperCase();
    void onEditInvertCase();
    void onEditRandomCase();
    void onEditTrimTrailing();
    void onEditSortLinesAscending();
    // Phase 9a — Edit menu polish (P2 batch).
    void onEditInsertDateShort();
    void onEditInsertDateLong();
    void onEditInsertTime();
    void onEditTrimLeading();
    void onEditTrimBoth();
    void onEditConvertTabsToSpaces();
    void onEditConvertSpacesToTabs();
    void onEditSortLinesDescending();
    void onEditSortLinesAscIntegers();
    void onEditSortLinesDescIntegers();
    void onEditSortLinesAscDecimals();
    void onEditSortLinesDescDecimals();
    void onEditBlockComment();
    void onEditStreamComment();
    // Phase 9d.2 — Scintilla SCI_HIDELINES / SCI_SHOWLINES wrappers.
    void onEditHideLines();
    void onEditShowAllLines();
    // Phase 5X
    void onEditAutocomplete();
    void onViewToggleSmartHighlight();

    // Phase 5W — Column mode / Begin-End select / Column Editor
    void onEditBeginSelect();
    void onEditEndSelect();
    void onEditColumnMode();
    void onEditColumnEditor();

    // Phase 5Y — multi-cursor add-cursor-at-next-match
    void onEditAddNextOccurrence();
    // Phase 9c.3 — multi-cursor add-cursor-at-every-match (Sublime/VSCode
    // Ctrl+Alt+Enter): sweep the whole buffer for the current selection
    // (or word under caret) and add a caret at every occurrence.
    void onEditAddAllOccurrences();

    // Search menu
    void onSearchFind();
    void onSearchReplace();
    // Phase 9r — Search → Find in Files / Mark menu items, route to
    // the FindReplaceDialog dock's tabs.
    void onSearchFindInFiles();
    void onSearchMark();
    void onSearchFindNext();
    void onSearchFindPrevious();
    void onSearchGoToLine();
    void onSearchToggleBookmark();
    void onSearchNextBookmark();
    void onSearchPreviousBookmark();
    void onSearchClearBookmarks();

    // View menu
    void onViewAlwaysOnTop();
    void onViewToggleFullScreen();
    void onViewDistractionFree();
    void onViewPostIt();
    void onViewSplitView();   // Phase 3d
    void onViewDocumentMap(); // Phase 3c.1
    void onViewFunctionList();// Phase 3c.2
    void onViewFileBrowser(); // Phase 3c.3
    // Phase 3c.4 → 9q. Three independent panels (1, 2, 3) per upstream
    // parity. The slot dispatches on the QAction's data() payload
    // (1-based panel index) so a single body services all three.
    void onViewProjectPanel();
    void onViewDocumentSwitcher();   // Phase 9k — Ctrl+Tab popup
    void onViewToggleWordWrap();
    void onViewToggleLineNumbers();
    void onViewZoomIn();
    void onViewZoomOut();
    void onViewZoomReset();
    void onViewFoldAll();
    void onViewUnfoldAll();
    void onViewToggleWhitespace();
    void onViewToggleIndentGuides();

    // Help menu
    void onHelpAbout();
    void onHelpOnlineDocs();
    void onHelpForum();
    void onHelpDebugInfo();

    // Tab change — both panes connect here. The slot identifies the sender
    // pane via qobject_cast<EditorTabs*>(sender()); switching tabs in a
    // pane promotes that pane to active.
    void onCurrentBufferChanged(Buffer* buffer);

    // EditorTabs::closeRequested → drives Save/Discard/Cancel + Save-As dialog,
    // then asks EditorTabs to drop the buffer. (Q11 fix.)
    void onTabCloseRequest(int index);

    // Phase 3d — right-click on a tab bar. The sender pane fires this with
    // the tab index under the cursor and the global click position. Pops a
    // Move/Clone-to-other-view menu.
    void onTabContextMenu(int idx, const QPoint& globalPos);

    // Phase 9l — user dragged a tab out of its bar and released LMB. If
    // the release point falls within the OTHER pane's geometry, perform
    // the cross-pane move (detach + adopt). Otherwise no-op.
    void onTabDraggedOutside(int idx, const QPoint& globalPos);

    // Phase 3d — focus changed listener. Promotes whichever pane the new
    // focus widget is a descendant of to the active pane.
    void onAppFocusChanged(QWidget* old, QWidget* now);

    // Language menu
    void onLanguageSelected(const LanguageDef* lang);
    void onBufferLanguageChanged(Buffer* buffer);

    // Phase 5U.3 — UDL editor wiring.
    void onDefineYourLanguage();        // opens UserDefineDialog
    void onUDLsSaved();                 // dialog Save → reload + rebuild menu
    void refreshUserDefinedMenu();      // populate "User Defined Language" submenu
    void rebuildLanguageMenu();         // tear down + repopulate from Languages::all()
    // Phase 10b — Plugins menu. Built once at startup from
    // PluginLoader::all(); each plugin gets its own submenu of
    // FuncItem actions. Empty plugin set produces a "(no plugins
    // installed)" placeholder submenu for discoverability.
    void rebuildPluginsMenu();

    // View → Theme submenu (Phase 4c)
    void onThemeLight();
    void onThemeDark();

    // Tools → Hash submenu (Phase 5b)
    void onToolsHashMd5();
    void onToolsHashSha1();
    void onToolsHashSha256();
    void onToolsHashSha512();

    // Tools → Word Count (Phase 5AB)
    void onToolsWordCount();

    // Tools → Summary (Phase 9d.1) — extends Word Count with file
    // size / mtime / encoding / EOL / language metadata in a read-only
    // modal.
    void onToolsSummary();

    // Run menu (Phase 5e)
    void onRunCommand();

    // Macro menu (Phase 5Z)
    void onMacroStartRecording();
    void onMacroStopRecording();
    void onMacroPlayLast();
    void onMacroRunMultipleTimes();
    void onMacroSaveCurrentRecorded();
    void onMacroPlaySaved();
    void onMacroRecordingStateChanged(bool recording);
    void onMacroLastChanged();
    void onMacroSavedListChanged();
    // Phase 9d.3 — right-click on a saved-macros entry → Rename / Delete.
    void onMacroSavedContextMenu(const QPoint& localPos);

    // Settings -> Language submenu (Phase 7d)
    void onUiLanguageSelected();

    // Phase 5T — file watcher
    void onWatchedFileChanged(const QString& path);
    void onBufferDisplayNameChanged(Buffer* buffer);   // re-syncs watcher

    // Phase 5S — system tray
    void onTrayActivated(QSystemTrayIcon::ActivationReason reason);
    void onSettingsMinimizeToTray();

    // Phase 5P — Style Configurator
    void onSettingsStyleConfigurator();

    // Phase 5Q — Shortcut Mapper
    void onSettingsShortcutMapper();

    // Window menu (Phase 5i)
    void onWindowList();
    void onWindowSortByName();
    void onWindowSortByPath();

    // Encoding menu (Phase 5c, expanded in 5k) — Set entries are wired
    // directly in buildMenus via lambdas calling reloadCurrentBufferAs.
    // Convert entries keep named slots (5 buttons, no expansion).
    void onEncodingConvertAnsi();
    void onEncodingConvertUtf8();
    void onEncodingConvertUtf8Bom();
    void onEncodingConvertUtf16Be();
    void onEncodingConvertUtf16Le();
    void onBufferEncodingChanged(Buffer* buffer);

    // Format menu (Phase 5d) — EOL mode
    void onEolSetWindows();
    void onEolSetUnix();
    void onEolSetMac();
    void onEolConvertWindows();
    void onEolConvertUnix();
    void onEolConvertMac();
    void onBufferEolModeChanged(Buffer* buffer);

    // File → Open Recent submenu (Phase 4d)
    void refreshRecentFilesMenu();
    void onClearRecentFiles();

    // Phase 9c.1 — File → Read-Only / Clear Read-Only Flag. Per-buffer
    // transient state; SCI_SETREADONLY blocks edits. Status bar caret
    // label gets a "[RO]" suffix when on.
    void onFileToggleReadOnly();
    void onFileClearReadOnly();
    void onBufferReadOnlyChanged(Buffer* buffer);

    // Phase 9b.2 — File → Reopen Recently Closed File (Ctrl+Shift+T) +
    // File → Recently Closed Files submenu. In-memory MRU only, capped
    // at 20; populated by EditorTabs::bufferAboutToClose. Untitled
    // buffers are not tracked.
    void onFileReopenRecentlyClosed();
    void refreshRecentlyClosedMenu();
    void onBufferAboutToClose(const QString& path);

private:
    void buildMenus();
    void buildToolbar();
    void buildStatusBar();
    void rebuildSavedMacrosMenu();   // Phase 5Z
    void syncFileWatcher();   // Phase 5T — keep watcher in sync with open buffers
    void applyCurrentThemeToChrome();   // Phase 7f — light/dark Qt palette
    void applyUiVisibilityFromConfig(); // Phase 5N.2 — toolbar/statusbar/tabsClosable
    void applyEditingPrefsFromConfig(); // Phase 5N.3 — caret width / blink + smart-highlight clear
    void applyBackupPrefsFromConfig();  // Phase 5N.11 — backup timer interval + master enable
    void applyFileWatcherPrefsFromConfig(); // Phase 5N.11 — watcher enable / disable

    // Phase 3d — set the active pane. Idempotent. Drives status bar / menu
    // radio refresh from the new active pane's current buffer. Index 0 = left,
    // 1 = right. Setting active to a hidden right pane is silently ignored
    // (the user can't focus a hidden widget anyway).
    void setActivePane(int idx);
    int  paneIndexOf(EditorTabs* pane) const;
    void showSplitPane(bool on);   // toggles right-pane visibility

    // Reload the active buffer with the user-chosen encoding (Phase 5c
    // helper, factored out for Phase 5k's character-sets submenu so the
    // lambda-per-radio doesn't need access to the anon-namespace helper).
    void reloadCurrentBufferAs(const EncodingInfo& enc);
    void wireScintillaStatusUpdates(Buffer* buffer);
    void updateCaretStatus();
    void updateLanguageStatus();
    void updateEncodingStatus();
    void updateEolStatus();

    // Returns true to allow the close, false to abort. Prompts the user if
    // the buffer at 'index' is dirty; on "Save" drives onFileSave (which can
    // pop a Save-As dialog for untitled buffers).
    bool confirmCloseAt(EditorTabs* pane, int index);

    // Closes every open buffer with confirmation across BOTH panes. Returns
    // false if any prompt was cancelled — caller leaves remaining tabs in
    // place.
    bool closeAllBuffers();

    // Lazily constructs the Find/Replace dialog and returns it.
    FindReplaceDialog* findReplace();

    // Returns the current selection as a QString, suitable for prefilling the
    // Find input. Empty if there's no selection or no current buffer.
    QString currentSelectionText() const;

    // Phase 3d — the two panes + splitter. m_panes[0] is left, [1] is right.
    EditorTabs*        m_panes[2] = {nullptr, nullptr};
    QSplitter*         m_splitter = nullptr;
    int                m_activePane = 0;
    QAction*           m_actSplitView = nullptr;

    QLabel*            m_statusCaret;
    QLabel*            m_statusLanguage;
    QLabel*            m_statusEncoding;
    QLabel*            m_statusEol;
    FindReplaceDialog* m_findReplace = nullptr;   // lazy
    DocumentMapDock*   m_documentMap = nullptr;   // lazy (Phase 3c.1)
    QAction*           m_actDocumentMap = nullptr;
    FunctionListDock*  m_functionList = nullptr;  // lazy (Phase 3c.2)
    QAction*           m_actFunctionList = nullptr;
    FileBrowserDock*   m_fileBrowser = nullptr;   // lazy (Phase 3c.3)
    QAction*           m_actFileBrowser = nullptr;
    // Phase 3c.4 / 9q — three lazily-constructed Project Panels.
    // Index 0 = panel 1 (legacy), 1 = panel 2, 2 = panel 3.
    ProjectPanelDock*  m_projectPanels[3]  = {nullptr, nullptr, nullptr};
    QAction*           m_actProjectPanels[3] = {nullptr, nullptr, nullptr};
    std::vector<QAction*> m_languageActions;      // parallel to Languages::all()
    QMenu*             m_languageMenu       = nullptr;  // Phase 5U.3 — for rebuildLanguageMenu
    QMenu*             m_pluginsMenu        = nullptr;  // Phase 10b — Plugins top-level
    QMenu*             m_userDefinedMenu    = nullptr;  // Phase 5U.3 — Language → User Defined Language submenu
    QAction*           m_actDefineYourLang  = nullptr;  // Phase 5U.3 — opens UserDefineDialog
    QMenu*             m_recentMenu = nullptr;    // File → Open Recent (Phase 4d)

    // Phase 9b.2 — Recently Closed in-memory MRU (file paths only) + lazy
    // submenu under File. Capped at 20 stored, last 10 shown in submenu.
    QVector<QString>   m_recentlyClosed;
    QMenu*             m_recentlyClosedMenu = nullptr;

    // Phase 9c.1 — File → Read-Only toggle. The action is checkable;
    // its checked state mirrors the active buffer's m_readOnly. Updated
    // by onBufferReadOnlyChanged + onCurrentBufferChanged.
    QAction*           m_actReadOnly = nullptr;

    // Phase 9k — Document Switcher MRU. Most-recent-first; updated on
    // every onCurrentBufferChanged. QPointer auto-nulls on Buffer
    // destruction so closed buffers fall out without explicit cleanup.
    QVector<QPointer<Buffer>> m_mruBuffers;

    // Encoding menu radios — every checkable entry the menu offers (top
    // group + character-sets submenu, ~35 total). updateEncodingStatus
    // walks this list and ticks the one whose EncodingInfo matches the
    // active buffer (or none if it falls outside the menu's coverage).
    std::vector<std::pair<QAction*, EncodingInfo>> m_encodingRadios;

    // Format menu radios (Phase 5d) — three EOL modes; one is always
    // checked since every buffer has a definite mode.
    QAction* m_eolWindows = nullptr;
    QAction* m_eolUnix    = nullptr;
    QAction* m_eolMac     = nullptr;

    // Last command typed into the Run dialog. In-memory only (Phase 5e);
    // persisting to config.xml lands with the Shortcut Mapper (Phase 5Q).
    QString  m_lastRunCommand;

    // Last printer used for File -> Print Now (Phase 5l). Constructed
    // lazily on first Print...; reused (without dialog) by Print Now.
    // Lifetime is the MainWindow's.
    class QPrinter* m_lastPrinter = nullptr;

    // Phase 5V — View polish state.
    bool   m_alwaysOnTop    = false;
    bool   m_distractionFree= false;
    bool   m_postIt         = false;
    QRect  m_preDistractGeom;        // restored when leaving distraction-free / post-it
    QRect  m_prePostItGeom;
    bool   m_preDistractMenuVis   = true;
    bool   m_preDistractStatusVis = true;
    bool   m_preDistractToolbarVis= true;

    QAction* m_actAlwaysOnTop     = nullptr;
    QAction* m_actFullScreen      = nullptr;
    QAction* m_actDistractionFree = nullptr;
    QAction* m_actPostIt          = nullptr;
    QAction* m_actSmartHighlight  = nullptr;   // Phase 5X

    // Phase 5T — watches every open buffer's filePath for external edits.
    class QFileSystemWatcher* m_fileWatcher = nullptr;
    // Phase 5AA — periodic backup snapshot timer; held so Phase 5N.11's
    // Preferences can re-tune the interval / enable on the fly.
    class QTimer*             m_backupTimer = nullptr;

    // Phase 5Z — Macro top-level menu state. m_macros points to the
    // singleton via MacroManager::instance(); we cache it for terse
    // call sites. The QMenu/QAction pointers are populated in
    // buildMenus() and toggled by onMacroRecordingStateChanged.
    MacroManager* m_macros = nullptr;
    QMenu*    m_savedMacrosMenu = nullptr;
    QAction*  m_actMacroStart   = nullptr;
    QAction*  m_actMacroStop    = nullptr;
    QAction*  m_actMacroPlayLast= nullptr;
    QAction*  m_actMacroRunMulti= nullptr;
    QAction*  m_actMacroSave    = nullptr;

    // Phase 5S — system tray. Hidden on stock GNOME (no tray); on
    // KDE/XFCE/MATE/Cinnamon/i3 tray-providing setups it shows up.
    QSystemTrayIcon* m_tray = nullptr;
    QAction*         m_actMinimizeToTray = nullptr;
};
