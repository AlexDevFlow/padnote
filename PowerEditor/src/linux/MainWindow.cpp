#include "MainWindow.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QDate>
#include <QDesktopServices>
#include <QDir>
#include <QTime>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QHash>
#include <QKeySequence>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPrintDialog>
#include <QPrintPreviewDialog>
#include <QPrinter>
#include <QProcess>
#include <QPushButton>
#include <QSet>
#include <QSplitter>
#include <QTextDocument>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QToolBar>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>
#include <vector>

#include <QActionGroup>
#include <QInputDialog>
#include <QPalette>
#include <QRect>
#include <QString>
#include <QStringList>

#include "Buffer.h"
#include "Config.h"
#include "DocumentMapDock.h"
#include "DocumentSwitcherDialog.h"
#include "EditorTabs.h"
#include "Encoding.h"
#include "FileBrowserDock.h"
#include "FindReplaceDialog.h"
#include "FunctionListDock.h"
#include "PluginLoader.h"     // Phase 10a/10b — plugin runtime
#include "../../../plugins/linuxplugin.h"   // LinuxFuncItem
#include "PreferencesDialog.h"
#include "ProjectPanelDock.h"
#include "HashDialog.h"
#include "Languages.h"
#include "Backup.h"
#include "ColumnEditorDialog.h"
#include "Localization.h"
#include "MacroManager.h"
#include "RunDialog.h"
#include "ShortcutMapperDialog.h"
#include "StyleConfigDialog.h"
#include "Scintilla.h"   // EDGE_NONE / EDGE_LINE (Phase 9b.4)
#include "ScintillaEditBase.h"
#include "ScintillaMessages.h"
#include "Session.h"
#include "Theme.h"
#include "UserDefineDialog.h"   // Phase 5U.3

using Scintilla::Message;

namespace {
// Forward decls — definitions live near the View → Theme submenu code.
void applyDarkChromePalette();
void applyLightChromePalette();

// Bookmark margin/marker — must match the values used by Buffer.cpp's ctor.
constexpr int kBookmarkMarker = 24;
constexpr int kBookmarkMask   = 1 << kBookmarkMarker;

// Phase 9b.4 — convert "#RRGGBB" to Scintilla packed colour
// (red | green<<8 | blue<<16). Falls back to light grey on bad input.
// Mirrors Buffer.cpp's anon-namespace helper; duplicated here so
// MainWindow doesn't have to reach into Buffer.cpp.
int sciColorFromHex(const QString& hex)
{
    auto toComponent = [](QChar a, QChar b) -> int {
        bool oka = false, okb = false;
        const int hi = QString(a).toInt(&oka, 16);
        const int lo = QString(b).toInt(&okb, 16);
        if (!oka || !okb) return -1;
        return (hi << 4) | lo;
    };
    if (hex.size() != 7 || hex.at(0) != QChar('#')) return 0xC0C0C0;
    const int r = toComponent(hex.at(1), hex.at(2));
    const int g = toComponent(hex.at(3), hex.at(4));
    const int b = toComponent(hex.at(5), hex.at(6));
    if (r < 0 || g < 0 || b < 0) return 0xC0C0C0;
    return r | (g << 8) | (b << 16);
}
}

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent),
      m_statusCaret(nullptr),
      m_statusLanguage(nullptr),
      m_statusEncoding(nullptr),
      m_statusEol(nullptr)
{
    setWindowTitle(tr("padnote--"));

    // Restore window geometry from config.xml if present, else default size.
    const QRect geom = Config::windowGeometry();
    if (geom.isValid()) {
        setGeometry(geom);
    } else {
        resize(1100, 760);
    }
    if (Config::windowMaximized()) {
        showMaximized();
    }

    // Apply the persisted theme to the Qt chrome (Theme::setMode for editor
    // colours was already called in main_qt.cpp). The editor styling was
    // applied to Buffers as they were constructed; the chrome palette only
    // needs to be set once here.
    if (Theme::mode() == Theme::Mode::Dark) {
        applyDarkChromePalette();
    }

    // Phase 3d — central widget is a horizontal QSplitter holding two
    // EditorTabs. Right pane is hidden by default; View → Split View
    // toggles it. Both panes connect to the same currentBufferChanged
    // and closeRequested slots; the active-pane logic uses sender() to
    // distinguish.
    m_panes[0] = new EditorTabs(this);
    m_panes[1] = new EditorTabs(this);
    m_splitter = new QSplitter(Qt::Horizontal, this);
    m_splitter->addWidget(m_panes[0]);
    m_splitter->addWidget(m_panes[1]);
    m_splitter->setChildrenCollapsible(false);
    m_panes[1]->hide();
    setCentralWidget(m_splitter);

    for (EditorTabs* p : m_panes) {
        connect(p, &EditorTabs::currentBufferChanged,
                this, &MainWindow::onCurrentBufferChanged);
        connect(p, &EditorTabs::closeRequested,
                this, &MainWindow::onTabCloseRequest);
        connect(p, &EditorTabs::tabContextMenuRequested,
                this, &MainWindow::onTabContextMenu);
        // Phase 9b.2 — track closed-with-a-path buffers in an MRU.
        connect(p, &EditorTabs::bufferAboutToClose,
                this, &MainWindow::onBufferAboutToClose);
        // Phase 9l — drag-tab-between-panes (release-outside-bar path).
        connect(p, &EditorTabs::tabDraggedOutside,
                this, &MainWindow::onTabDraggedOutside);
    }

    // Phase 3d — focus listener promotes the pane the user clicks into.
    connect(qApp, &QApplication::focusChanged,
            this, &MainWindow::onAppFocusChanged);

    // Phase 5T — file watcher. Stays alive for the MainWindow's lifetime;
    // syncFileWatcher() is called whenever a Buffer's filePath changes
    // (load / saveAs / rename) or the tab list changes (open / close).
    m_fileWatcher = new QFileSystemWatcher(this);
    connect(m_fileWatcher, &QFileSystemWatcher::fileChanged,
            this, &MainWindow::onWatchedFileChanged);

    // Phase 5Z — wire the singleton MacroManager BEFORE buildMenus so
    // the saved-macros submenu can populate from already-loaded config.
    m_macros = &MacroManager::instance();
    connect(m_macros, &MacroManager::recordingStateChanged,
            this, &MainWindow::onMacroRecordingStateChanged);
    connect(m_macros, &MacroManager::lastMacroChanged,
            this, &MainWindow::onMacroLastChanged);
    connect(m_macros, &MacroManager::savedMacrosChanged,
            this, &MainWindow::onMacroSavedListChanged);

    // Phase 7d — load bundled language registry and overlay the
    // persisted active language onto the menu bar after construction.
    Localization::init();

    // Phase 10b — load plugins BEFORE buildMenus() so the Plugins
    // menu builder sees them. Loader calls back via the MainWindow's
    // status bar (which doesn't exist yet); guard against that by
    // doing the status nudge lazily — for now status messages are
    // suppressed during the load (no-op statusBar()->showMessage on
    // a not-yet-built status bar is safe — Qt creates one on demand).
    PluginLoader::loadAll(this);

    buildMenus();
    buildToolbar();
    buildStatusBar();
    // Phase 5N.2 — apply persisted UI visibility (toolbar / statusbar /
    // tabsClosable) AFTER the chrome is built. Defaults are all-on so
    // first-run users see the same UI as before.
    applyUiVisibilityFromConfig();
    // Phase 5N.3 — apply caret width / blink to the initial buffer (and
    // any future buffers go through Buffer's ctor with theme/encoding;
    // those live-prefs are pushed here on each Apply/OK from Preferences,
    // and on startup for the initial buffer). Buffer-ctor doesn't read
    // Config for these today — runtime push is the canonical path.

    // Initial enabled-state sweep — Start enabled iff a buffer exists,
    // Stop / PlayLast / RunMulti / Save disabled until there's content.
    onMacroRecordingStateChanged(false);

    // Phase 5Q — capture every menu action's shortcut as the "default",
    // then overlay any persisted user overrides from config.xml. Order
    // matters: captureDefaults BEFORE applyOverrides, otherwise we'd
    // record overridden values as defaults.
    Shortcuts::captureDefaults(menuBar());
    Shortcuts::applyOverrides(menuBar());

    // Phase 7d — overlay translated top-level menu titles AFTER the
    // Shortcut Mapper has captured defaults, so the captured paths
    // remain English-keyed (Shortcut overrides persist by Qt-built
    // path, which is constructed from the original tr() text). The
    // localization layer only rewrites the visible label.
    Localization::applyToMenuBar(menuBar());

    // Open one untitled buffer at startup so the user has somewhere to type.
    m_panes[0]->newUntitled();

    // Phase 5N.3 — push persisted caret width / blink to the initial
    // buffer. New buffers created later via newUntitled / openFile inherit
    // Buffer.cpp's defaults (caret width 1, default blink); a future polish
    // phase could move these reads INTO Buffer's ctor for consistency.
    applyEditingPrefsFromConfig();

    // Phase 3c.1 — restore Document Map visibility from config.xml. Done
    // AFTER the initial buffer exists so the map binds to it on show.
    if (Config::documentMapVisible() && m_actDocumentMap) {
        m_actDocumentMap->setChecked(true);
        onViewDocumentMap();
    }
    // Phase 3c.2 — same for Function List.
    if (Config::functionListVisible() && m_actFunctionList) {
        m_actFunctionList->setChecked(true);
        onViewFunctionList();
    }
    // Phase 3c.3 — same for File Browser.
    if (Config::fileBrowserVisible() && m_actFileBrowser) {
        m_actFileBrowser->setChecked(true);
        onViewFileBrowser();
    }
    // Phase 3c.4 / 9q — auto-show every Project Panel that was visible
    // at last shutdown. Each panel auto-loads its own last workspace.
    for (int n = 1; n <= 3; ++n) {
        if (Config::projectPanelVisible(n) && m_actProjectPanels[n - 1]) {
            m_actProjectPanels[n - 1]->setChecked(true);
            // Dispatch via the action's existing trigger handler so the
            // lazy-construct + initial-load path runs uniformly.
            m_actProjectPanels[n - 1]->trigger();
        }
    }

    // Phase 5AA — periodic backup snapshot of every dirty buffer. Phase
    // 3d walks both panes via allBuffers(). Phase 5N.11 makes the interval
    // user-configurable + adds a master enable; m_backupTimer is held so
    // Apply from Preferences can re-tune via applyBackupPrefsFromConfig.
    m_backupTimer = new QTimer(this);
    m_backupTimer->setInterval(Config::backupIntervalSec() * 1000);
    connect(m_backupTimer, &QTimer::timeout, this, [this]{
        for (Buffer* b : allBuffers()) {
            if (b && b->isDirty()) Backup::writeBuffer(b);
            else if (b && !b->isDirty() && !b->backupUuid().isEmpty()) {
                // Buffer became clean externally (e.g. via Reload from
                // Disk); drop its stale backup.
                Backup::clearBuffer(b);
            }
        }
    });
    if (Config::backupEnabled()) m_backupTimer->start();
}

// -----------------------------------------------------------------------------
// Menu construction.
//
// This mirrors the top-level menu structure from menuCmdID.h (File, Edit,
// Search, View, Encoding, Language, Run, Tools, Settings, Window, ?).
// Phase 9r — all stubs burned down. Each menu item is either wired to
// a real slot or removed; no "not yet implemented" placeholders remain.
// -----------------------------------------------------------------------------

void MainWindow::buildMenus()
{
    auto* mb = menuBar();

    // ---- File ----
    {
        auto* m = mb->addMenu(tr("&File"));
        m->setProperty("nlMenuId", "file");   // Phase 7d
        auto* aNew    = m->addAction(tr("&New"));
        aNew->setShortcut(QKeySequence::New);
        connect(aNew, &QAction::triggered, this, &MainWindow::onFileNew);

        auto* aOpen   = m->addAction(tr("&Open..."));
        aOpen->setShortcut(QKeySequence::Open);
        connect(aOpen, &QAction::triggered, this, &MainWindow::onFileOpen);

        // Phase 9r — Open Folder as Workspace: prompt for a directory,
        // open the File Browser dock at that root. Upstream's variant
        // also creates a project; in our port the File Browser dock
        // covers the "browse + open files in a directory" use case
        // and Project Panel covers the persistent-workspace use case.
        // Routing here to File Browser matches the more frequent flow.
        auto* aOpenFolder = m->addAction(tr("Open &Folder as Workspace..."));
        connect(aOpenFolder, &QAction::triggered, this, [this]() {
            const QString dir = QFileDialog::getExistingDirectory(this,
                tr("Open Folder as Workspace"), QDir::homePath());
            if (dir.isEmpty()) return;
            if (m_actFileBrowser && !m_actFileBrowser->isChecked()) {
                m_actFileBrowser->setChecked(true);
                onViewFileBrowser();
            }
            if (m_fileBrowser) m_fileBrowser->setRoot(dir);
            statusBar()->showMessage(
                tr("Opened folder: %1").arg(dir), 4000);
        });
        auto* aReload = m->addAction(tr("&Reload from Disk"));
        aReload->setShortcut(tr("Ctrl+R"));
        connect(aReload, &QAction::triggered, this, &MainWindow::onFileReloadFromDisk);

        // Open Recent submenu — populated lazily via aboutToShow.
        m_recentMenu = m->addMenu(tr("Open &Recent"));
        m_recentMenu->setProperty("nlSubMenuId", "file-recentFiles");   // Phase 8b
        connect(m_recentMenu, &QMenu::aboutToShow,
                this, &MainWindow::refreshRecentFilesMenu);

        // Phase 9b.2 — Reopen the most-recently-closed file (Ctrl+Shift+T)
        // and a lazy submenu listing the last 10. The MRU lives in
        // MainWindow only (not persisted); resets on app close.
        auto* aReopenLast = m->addAction(tr("Reopen Recently Closed File"));
        aReopenLast->setShortcut(tr("Ctrl+Shift+T"));
        connect(aReopenLast, &QAction::triggered,
                this, &MainWindow::onFileReopenRecentlyClosed);
        m_recentlyClosedMenu = m->addMenu(tr("Recently Closed Files"));
        connect(m_recentlyClosedMenu, &QMenu::aboutToShow,
                this, &MainWindow::refreshRecentlyClosedMenu);

        m->addSeparator();
        auto* aSave   = m->addAction(tr("&Save"));
        aSave->setShortcut(QKeySequence::Save);
        connect(aSave, &QAction::triggered, this, [this]{ onFileSave(); });

        auto* aSaveAs = m->addAction(tr("Save &As..."));
        aSaveAs->setShortcut(QKeySequence::SaveAs);
        connect(aSaveAs, &QAction::triggered, this, [this]{ onFileSaveAs(); });

        auto* aSaveAll = m->addAction(tr("Save A&ll"));
        aSaveAll->setShortcut(tr("Ctrl+Shift+S"));
        connect(aSaveAll, &QAction::triggered, this, [this]{ onFileSaveAll(); });

        auto* aSaveCopyAs = m->addAction(tr("Save a &Copy As..."));
        connect(aSaveCopyAs, &QAction::triggered,
                this, &MainWindow::onFileSaveCopyAs);

        auto* aRename = m->addAction(tr("&Rename..."));
        connect(aRename, &QAction::triggered, this, &MainWindow::onFileRename);

        m->addSeparator();
        auto* aClose = m->addAction(tr("&Close"));
        aClose->setShortcut(QKeySequence::Close);
        connect(aClose, &QAction::triggered, this, &MainWindow::onFileCloseTab);

        auto* aCloseAll = m->addAction(tr("Close A&ll"));
        connect(aCloseAll, &QAction::triggered, this, &MainWindow::onFileCloseAll);

        auto* aCloseBut = m->addAction(tr("Close All BUT Current"));
        connect(aCloseBut, &QAction::triggered, this, &MainWindow::onFileCloseAllBut);

        auto* aTrash = m->addAction(tr("&Move to Recycle Bin"));
        connect(aTrash, &QAction::triggered, this, &MainWindow::onFileMoveToTrash);

        m->addSeparator();
        // Phase 9c.1 — Read-Only toggle + Clear flag. Toggle is checkable
        // and reflects the active buffer's m_readOnly; Clear is a one-shot
        // that turns the flag off (handy when "Read-Only" was set
        // accidentally and the user wants the explicit reset path).
        m_actReadOnly = m->addAction(tr("Read-&Only"));
        m_actReadOnly->setCheckable(true);
        connect(m_actReadOnly, &QAction::triggered,
                this, &MainWindow::onFileToggleReadOnly);
        auto* aClearRO = m->addAction(tr("&Clear Read-Only Flag"));
        connect(aClearRO, &QAction::triggered,
                this, &MainWindow::onFileClearReadOnly);

        m->addSeparator();
        auto* aPrint = m->addAction(tr("&Print..."));
        aPrint->setShortcut(QKeySequence::Print);
        connect(aPrint, &QAction::triggered, this, &MainWindow::onFilePrint);
        auto* aPrintNow = m->addAction(tr("Print Now"));
        connect(aPrintNow, &QAction::triggered, this, &MainWindow::onFilePrintNow);
        auto* aPrintPrev = m->addAction(tr("Print Pre&view..."));
        connect(aPrintPrev, &QAction::triggered, this, &MainWindow::onFilePrintPreview);

        m->addSeparator();
        auto* aLoadSess = m->addAction(tr("&Load Session..."));
        connect(aLoadSess, &QAction::triggered, this, &MainWindow::onFileLoadSession);
        auto* aSaveSess = m->addAction(tr("Sa&ve Session..."));
        connect(aSaveSess, &QAction::triggered, this, &MainWindow::onFileSaveSessionAs);

        m->addSeparator();
        auto* aExit = m->addAction(tr("E&xit"));
        aExit->setShortcut(QKeySequence::Quit);
        connect(aExit, &QAction::triggered, this, &QMainWindow::close);
    }

    // ---- Edit ----
    {
        auto* m = mb->addMenu(tr("&Edit"));
        m->setProperty("nlMenuId", "edit");
        auto* aUndo = m->addAction(tr("&Undo"));
        aUndo->setShortcut(QKeySequence::Undo);
        connect(aUndo, &QAction::triggered, this, &MainWindow::onEditUndo);

        auto* aRedo = m->addAction(tr("&Redo"));
        aRedo->setShortcut(QKeySequence::Redo);
        connect(aRedo, &QAction::triggered, this, &MainWindow::onEditRedo);

        m->addSeparator();
        auto* aCut = m->addAction(tr("Cu&t"));
        aCut->setShortcut(QKeySequence::Cut);
        connect(aCut, &QAction::triggered, this, &MainWindow::onEditCut);

        auto* aCopy = m->addAction(tr("&Copy"));
        aCopy->setShortcut(QKeySequence::Copy);
        connect(aCopy, &QAction::triggered, this, &MainWindow::onEditCopy);

        auto* aPaste = m->addAction(tr("&Paste"));
        aPaste->setShortcut(QKeySequence::Paste);
        connect(aPaste, &QAction::triggered, this, &MainWindow::onEditPaste);

        auto* aDel = m->addAction(tr("&Delete"));
        aDel->setShortcut(QKeySequence::Delete);
        connect(aDel, &QAction::triggered, this, &MainWindow::onEditDelete);

        auto* aSelectAll = m->addAction(tr("Select &All"));
        aSelectAll->setShortcut(QKeySequence::SelectAll);
        connect(aSelectAll, &QAction::triggered, this, &MainWindow::onEditSelectAll);

        m->addSeparator();
        // Phase 5W — Begin / End Select + Column Mode + Column Editor.
        auto* aBegin = m->addAction(tr("Begin Select"));
        connect(aBegin, &QAction::triggered, this, &MainWindow::onEditBeginSelect);
        auto* aEnd = m->addAction(tr("End Select (Column)"));
        connect(aEnd, &QAction::triggered, this, &MainWindow::onEditEndSelect);
        auto* aColMode = m->addAction(tr("Column Mode..."));
        aColMode->setShortcut(tr("Alt+C"));
        connect(aColMode, &QAction::triggered, this, &MainWindow::onEditColumnMode);
        auto* aColEd = m->addAction(tr("Column Editor..."));
        connect(aColEd, &QAction::triggered, this, &MainWindow::onEditColumnEditor);
        m->addSeparator();
        // Phase 9r — Convert Case To submenu (was flat UPPER/lower
        // items; expanded with Proper / Invert / Random per upstream).
        auto* mCase = m->addMenu(tr("Convert &Case to"));
        mCase->setProperty("nlSubMenuId", "edit-convertCaseTo");
        auto* aUpper = mCase->addAction(tr("&UPPERCASE"));
        aUpper->setShortcut(tr("Ctrl+Shift+U"));
        connect(aUpper, &QAction::triggered, this, &MainWindow::onEditUpperCase);
        auto* aLower = mCase->addAction(tr("&lowercase"));
        aLower->setShortcut(tr("Ctrl+U"));
        connect(aLower, &QAction::triggered, this, &MainWindow::onEditLowerCase);
        auto* aProper = mCase->addAction(tr("&Proper Case"));
        connect(aProper, &QAction::triggered, this, &MainWindow::onEditProperCase);
        auto* aInvert = mCase->addAction(tr("i&NVERT cASE"));
        connect(aInvert, &QAction::triggered, this, &MainWindow::onEditInvertCase);
        auto* aRandom = mCase->addAction(tr("&rANdoM CASE"));
        connect(aRandom, &QAction::triggered, this, &MainWindow::onEditRandomCase);
        m->addSeparator();
        auto* aBlock = m->addAction(tr("Block Comment / Uncomment"));
        aBlock->setShortcut(tr("Ctrl+Q"));
        connect(aBlock, &QAction::triggered, this, &MainWindow::onEditBlockComment);
        auto* aStream = m->addAction(tr("Stream Comment"));
        aStream->setShortcut(tr("Ctrl+Shift+Q"));
        connect(aStream, &QAction::triggered, this, &MainWindow::onEditStreamComment);
        m->addSeparator();
        auto* aAutoc = m->addAction(tr("&Auto-Complete"));
        aAutoc->setShortcut(tr("Ctrl+Space"));
        connect(aAutoc, &QAction::triggered, this, &MainWindow::onEditAutocomplete);
        // Phase 5Y — add a cursor at the next occurrence of the current
        // selection (or word under caret).
        auto* aAddNext = m->addAction(tr("Add cursor at next match"));
        aAddNext->setShortcut(tr("Ctrl+D"));
        connect(aAddNext, &QAction::triggered,
                this, &MainWindow::onEditAddNextOccurrence);
        // Phase 9c.3 — add a cursor at every occurrence of the current
        // selection (or word under caret) in one go.
        auto* aAddAll = m->addAction(tr("Add cursors at all matches"));
        aAddAll->setShortcut(tr("Ctrl+Alt+Return"));
        connect(aAddAll, &QAction::triggered,
                this, &MainWindow::onEditAddAllOccurrences);
        m->addSeparator();
        // Phase 9a — Insert sub-menu (Date/Time).
        auto* mInsert = m->addMenu(tr("&Insert"));
        mInsert->setProperty("nlSubMenuId", "edit-insert");
        auto* aDateShort = mInsert->addAction(tr("Date (Short)"));
        connect(aDateShort, &QAction::triggered,
                this, &MainWindow::onEditInsertDateShort);
        auto* aDateLong = mInsert->addAction(tr("Date (Long)"));
        connect(aDateLong, &QAction::triggered,
                this, &MainWindow::onEditInsertDateLong);
        auto* aTimeIns = mInsert->addAction(tr("Time"));
        connect(aTimeIns, &QAction::triggered,
                this, &MainWindow::onEditInsertTime);

        // Phase 9a — Blank Operations sub-menu (Trim* + Convert tabs↔spaces).
        // The previous flat "Trim Trailing Spaces" entry now lives here.
        auto* mBlank = m->addMenu(tr("&Blank Operations"));
        mBlank->setProperty("nlSubMenuId", "edit-blankOperations");
        auto* aTrimT = mBlank->addAction(tr("Trim Trailing Space"));
        connect(aTrimT, &QAction::triggered,
                this, &MainWindow::onEditTrimTrailing);
        auto* aTrimL = mBlank->addAction(tr("Trim Leading Space"));
        connect(aTrimL, &QAction::triggered,
                this, &MainWindow::onEditTrimLeading);
        auto* aTrimB = mBlank->addAction(tr("Trim Leading and Trailing Space"));
        connect(aTrimB, &QAction::triggered,
                this, &MainWindow::onEditTrimBoth);
        auto* aTabsToSp = mBlank->addAction(tr("Convert Tabs to Spaces"));
        connect(aTabsToSp, &QAction::triggered,
                this, &MainWindow::onEditConvertTabsToSpaces);
        auto* aSpToTabs = mBlank->addAction(tr("Convert Spaces to Tabs"));
        connect(aSpToTabs, &QAction::triggered,
                this, &MainWindow::onEditConvertSpacesToTabs);

        // Phase 9a — Sort Lines sub-menu. Previous flat
        // "Sort Lines (Ascending)" becomes "As Text Ascending" here.
        auto* mSort = m->addMenu(tr("&Sort Lines"));
        mSort->setProperty("nlSubMenuId", "edit-lineOperations");
        auto* aSortAscText = mSort->addAction(tr("As Text Ascending"));
        connect(aSortAscText, &QAction::triggered,
                this, &MainWindow::onEditSortLinesAscending);
        auto* aSortDescText = mSort->addAction(tr("As Text Descending"));
        connect(aSortDescText, &QAction::triggered,
                this, &MainWindow::onEditSortLinesDescending);
        auto* aSortAscInt = mSort->addAction(tr("As Integer Ascending"));
        connect(aSortAscInt, &QAction::triggered,
                this, &MainWindow::onEditSortLinesAscIntegers);
        auto* aSortDescInt = mSort->addAction(tr("As Integer Descending"));
        connect(aSortDescInt, &QAction::triggered,
                this, &MainWindow::onEditSortLinesDescIntegers);
        auto* aSortAscDec = mSort->addAction(tr("As Decimal Ascending"));
        connect(aSortAscDec, &QAction::triggered,
                this, &MainWindow::onEditSortLinesAscDecimals);
        auto* aSortDescDec = mSort->addAction(tr("As Decimal Descending"));
        connect(aSortDescDec, &QAction::triggered,
                this, &MainWindow::onEditSortLinesDescDecimals);

        // Phase 9d.2 — Hide Lines / Show All Lines. Scintilla
        // SCI_HIDELINES collapses a range without using fold markers;
        // SCI_SHOWLINES restores it. Show-All sweeps the whole document.
        m->addSeparator();
        auto* aHideLines = m->addAction(tr("Hide Lines"));
        connect(aHideLines, &QAction::triggered,
                this, &MainWindow::onEditHideLines);
        auto* aShowAllLines = m->addAction(tr("Show All Lines"));
        connect(aShowAllLines, &QAction::triggered,
                this, &MainWindow::onEditShowAllLines);
    }

    // ---- Search ----
    {
        auto* m = mb->addMenu(tr("&Search"));
        m->setProperty("nlMenuId", "search");
        auto* aFind = m->addAction(tr("&Find..."));
        aFind->setShortcut(QKeySequence::Find);
        connect(aFind, &QAction::triggered, this, &MainWindow::onSearchFind);

        auto* aFindNext = m->addAction(tr("Find &Next"));
        aFindNext->setShortcut(QKeySequence::FindNext);
        connect(aFindNext, &QAction::triggered, this, &MainWindow::onSearchFindNext);

        auto* aFindPrev = m->addAction(tr("Find &Previous"));
        aFindPrev->setShortcut(QKeySequence::FindPrevious);
        connect(aFindPrev, &QAction::triggered, this, &MainWindow::onSearchFindPrevious);

        auto* aReplace = m->addAction(tr("&Replace..."));
        aReplace->setShortcut(QKeySequence::Replace);
        connect(aReplace, &QAction::triggered, this, &MainWindow::onSearchReplace);

        // Phase 9r — Find in Files used to be a stub; the dock has
        // been live since Phase 5R/9g. This menu item now routes to
        // FindReplaceDialog::showFindInFilesTab.
        auto* aFiF = m->addAction(tr("Find in F&iles..."));
        aFiF->setShortcut(tr("Ctrl+Shift+F"));
        connect(aFiF, &QAction::triggered,
                this, &MainWindow::onSearchFindInFiles);
        // Phase 9r — Search → Mark surfaces the Mark feature
        // (Phase 5MK). Pre-fills with the current selection so users
        // can mark a word the same way they'd Find it.
        auto* aMark = m->addAction(tr("Mar&k..."));
        connect(aMark, &QAction::triggered,
                this, &MainWindow::onSearchMark);
        auto* aGoTo = m->addAction(tr("&Go to Line..."));
        aGoTo->setShortcut(tr("Ctrl+G"));
        connect(aGoTo, &QAction::triggered, this, &MainWindow::onSearchGoToLine);
        m->addSeparator();
        auto* aBkTog = m->addAction(tr("Toggle &Bookmark"));
        aBkTog->setShortcut(tr("Ctrl+F2"));
        connect(aBkTog, &QAction::triggered, this, &MainWindow::onSearchToggleBookmark);
        auto* aBkNext = m->addAction(tr("&Next Bookmark"));
        aBkNext->setShortcut(tr("F2"));
        connect(aBkNext, &QAction::triggered, this, &MainWindow::onSearchNextBookmark);
        auto* aBkPrev = m->addAction(tr("Pre&vious Bookmark"));
        aBkPrev->setShortcut(tr("Shift+F2"));
        connect(aBkPrev, &QAction::triggered, this, &MainWindow::onSearchPreviousBookmark);
        auto* aBkClear = m->addAction(tr("Clear All Bookmarks"));
        connect(aBkClear, &QAction::triggered, this, &MainWindow::onSearchClearBookmarks);
    }

    // ---- View ----
    {
        auto* m = mb->addMenu(tr("&View"));
        m->setProperty("nlMenuId", "view");
        m_actAlwaysOnTop = m->addAction(tr("Always on Top"));
        m_actAlwaysOnTop->setCheckable(true);
        connect(m_actAlwaysOnTop, &QAction::triggered,
                this, &MainWindow::onViewAlwaysOnTop);
        // Phase 3d — Split View toggle. Checkable; show=second pane visible.
        m_actSplitView = m->addAction(tr("&Split View"));
        m_actSplitView->setCheckable(true);
        connect(m_actSplitView, &QAction::triggered,
                this, &MainWindow::onViewSplitView);
        m_actFullScreen = m->addAction(tr("&Toggle Full Screen Mode"));
        m_actFullScreen->setShortcut(tr("F11"));
        m_actFullScreen->setCheckable(true);
        connect(m_actFullScreen, &QAction::triggered,
                this, &MainWindow::onViewToggleFullScreen);
        m_actDistractionFree = m->addAction(tr("&Distraction Free Mode"));
        m_actDistractionFree->setCheckable(true);
        connect(m_actDistractionFree, &QAction::triggered,
                this, &MainWindow::onViewDistractionFree);
        m_actPostIt = m->addAction(tr("&Post-It"));
        m_actPostIt->setCheckable(true);
        connect(m_actPostIt, &QAction::triggered,
                this, &MainWindow::onViewPostIt);
        m->addSeparator();
        auto* aWhite = m->addAction(tr("Show &Symbol → All Characters"));
        aWhite->setCheckable(true);
        connect(aWhite, &QAction::triggered, this, &MainWindow::onViewToggleWhitespace);
        auto* aIndent = m->addAction(tr("Show Symbol → Indent Guide"));
        aIndent->setCheckable(true);
        connect(aIndent, &QAction::triggered, this, &MainWindow::onViewToggleIndentGuides);
        // Phase 5X — smart highlight toggle.
        m_actSmartHighlight = m->addAction(tr("&Highlight all matches"));
        m_actSmartHighlight->setCheckable(true);
        m_actSmartHighlight->setChecked(Config::smartHighlightEnabled());
        connect(m_actSmartHighlight, &QAction::triggered,
                this, &MainWindow::onViewToggleSmartHighlight);
        m->addSeparator();
        auto* aWrap = m->addAction(tr("Word &Wrap"));
        aWrap->setCheckable(true);
        connect(aWrap, &QAction::triggered, this, &MainWindow::onViewToggleWordWrap);

        auto* aLN = m->addAction(tr("&Line Numbers"));
        aLN->setCheckable(true);
        aLN->setChecked(true);
        connect(aLN, &QAction::triggered, this, &MainWindow::onViewToggleLineNumbers);

        m->addSeparator();
        auto* aFoldAll = m->addAction(tr("Fold All"));
        aFoldAll->setShortcut(tr("Alt+0"));
        connect(aFoldAll, &QAction::triggered, this, &MainWindow::onViewFoldAll);
        auto* aUnfoldAll = m->addAction(tr("Unfold All"));
        aUnfoldAll->setShortcut(tr("Alt+Shift+0"));
        connect(aUnfoldAll, &QAction::triggered, this, &MainWindow::onViewUnfoldAll);
        m->addSeparator();

        auto* aZIn  = m->addAction(tr("Zoom &In"));
        aZIn->setShortcut(QKeySequence::ZoomIn);
        connect(aZIn, &QAction::triggered, this, &MainWindow::onViewZoomIn);

        auto* aZOut = m->addAction(tr("Zoom &Out"));
        aZOut->setShortcut(QKeySequence::ZoomOut);
        connect(aZOut, &QAction::triggered, this, &MainWindow::onViewZoomOut);

        auto* aZRes = m->addAction(tr("&Restore Zoom"));
        aZRes->setShortcut(tr("Ctrl+0"));
        connect(aZRes, &QAction::triggered, this, &MainWindow::onViewZoomReset);

        m->addSeparator();
        // Phase 3c.1 — Document Map (real). Other side panels still stubs.
        m_actDocumentMap = m->addAction(tr("Show &Document Map"));
        m_actDocumentMap->setCheckable(true);
        connect(m_actDocumentMap, &QAction::triggered,
                this, &MainWindow::onViewDocumentMap);
        // Phase 3c.2 — Function List (real). File Browser / Project Panel still stubs.
        m_actFunctionList = m->addAction(tr("Show &Function List"));
        m_actFunctionList->setCheckable(true);
        connect(m_actFunctionList, &QAction::triggered,
                this, &MainWindow::onViewFunctionList);
        // Phase 3c.3 — File Browser (real).
        m_actFileBrowser = m->addAction(tr("Show File &Browser"));
        m_actFileBrowser->setCheckable(true);
        connect(m_actFileBrowser, &QAction::triggered,
                this, &MainWindow::onViewFileBrowser);
        // Phase 3c.4 / 9q — three independent Project Panels per upstream
        // parity. Panel 1 keeps the Alt+J accelerator on the leading "j"
        // for back-compat with users' muscle memory; 2/3 use plain text.
        // Each action carries its 1-based panel index in data() so the
        // shared slot can dispatch.
        for (int n = 1; n <= 3; ++n) {
            const QString label = (n == 1)
                ? tr("Show Pro&ject Panel 1")
                : tr("Show Project Panel %1").arg(n);
            QAction* a = m->addAction(label);
            a->setCheckable(true);
            a->setData(n);
            connect(a, &QAction::triggered,
                    this, &MainWindow::onViewProjectPanel);
            m_actProjectPanels[n - 1] = a;
        }

        m->addSeparator();
        // Phase 9k — Document Switcher (Ctrl+Tab). Modal popup with
        // open buffers in MRU order; user picks with arrow keys + Enter.
        auto* aSwitcher = m->addAction(tr("Document &Switcher..."));
        aSwitcher->setShortcut(tr("Ctrl+Tab"));
        connect(aSwitcher, &QAction::triggered,
                this, &MainWindow::onViewDocumentSwitcher);

        m->addSeparator();
        // Theme submenu — Phase 4c added Light/Dark; Phase 7f opens the
        // gates to all 22 bundled themes from upstream's
        // installer/themes/. Each entry is an exclusive radio.
        auto* mTheme = m->addMenu(tr("&Theme"));
        auto* themeGroup = new QActionGroup(this);
        themeGroup->setExclusive(true);
        const QString currentName = Theme::currentName();
        for (const QString& name : Theme::availableThemes()) {
            auto* a = mTheme->addAction(name);
            a->setCheckable(true);
            a->setChecked(name == currentName);
            themeGroup->addAction(a);
            connect(a, &QAction::triggered, this, [this, name]{
                Theme::setThemeByName(name);
                applyCurrentThemeToChrome();
                for (Buffer* b : allBuffers()) {
                    if (b) b->reapplyTheme();
                }
                if (m_documentMap) m_documentMap->reapplyTheme();   // Phase 9m.1
                Config::setTheme(name);
                statusBar()->showMessage(tr("Theme: %1").arg(name), 2500);
            });
        }
    }

    // ---- Encoding ----
    {
        auto* m = mb->addMenu(tr("E&ncoding"));
        m->setProperty("nlMenuId", "encoding");
        // Single QActionGroup owns every radio in this menu — top group
        // and char-sets submenu — so picking any one un-ticks the rest.
        auto* group = new QActionGroup(this);
        group->setExclusive(true);

        auto addRadio = [&](QMenu* sub, const QString& label,
                            const EncodingInfo& enc) -> QAction* {
            auto* a = sub->addAction(label);
            a->setCheckable(true);
            group->addAction(a);
            m_encodingRadios.emplace_back(a, enc);
            connect(a, &QAction::triggered, this,
                    [this, enc]{ reloadCurrentBufferAs(enc); });
            return a;
        };

        // Top group — the most common encodings users hit by name.
        addRadio(m, tr("ANSI"),          {QStringLiteral("windows-1252"), false});
        addRadio(m, tr("UTF-8"),         {QStringLiteral("UTF-8"),        false});
        addRadio(m, tr("UTF-8-BOM"),     {QStringLiteral("UTF-8"),        true});
        addRadio(m, tr("UTF-16 BE BOM"), {QStringLiteral("UTF-16BE"),     true});
        addRadio(m, tr("UTF-16 LE BOM"), {QStringLiteral("UTF-16LE"),     true});
        m->addSeparator();

        // Character sets submenu — region groupings. All entries are
        // radios in the same group as the top entries.
        auto* mc = m->addMenu(tr("Character sets"));
        mc->setProperty("nlSubMenuId", "encoding-characterSets");   // Phase 8b

        auto addRegion = [&](const QString& title,
                             std::initializer_list<std::pair<QString, QString>> entries) {
            auto* sub = mc->addMenu(title);
            for (const auto& [label, codec] : entries) {
                addRadio(sub, label, {codec, false});
            }
        };

        addRegion(tr("Western European"), {
            {tr("ISO-8859-1 (Latin-1)"),  QStringLiteral("ISO-8859-1")},
            {tr("ISO-8859-15 (Latin-9)"), QStringLiteral("ISO-8859-15")},
            {tr("Windows-1252"),          QStringLiteral("windows-1252")},
            {tr("ISO-8859-3 (Latin-3)"),  QStringLiteral("ISO-8859-3")},
            {tr("OEM 850"),               QStringLiteral("IBM 850")},
        });
        addRegion(tr("Eastern European"), {
            {tr("ISO-8859-2 (Latin-2)"),  QStringLiteral("ISO-8859-2")},
            {tr("Windows-1250"),          QStringLiteral("windows-1250")},
            {tr("ISO-8859-4 (Latin-4)"),  QStringLiteral("ISO-8859-4")},
            {tr("ISO-8859-13 (Latin-7)"), QStringLiteral("ISO-8859-13")},
            {tr("ISO-8859-16 (Latin-10)"),QStringLiteral("ISO-8859-16")},
        });
        addRegion(tr("East Asian"), {
            {tr("Shift_JIS"),             QStringLiteral("Shift_JIS")},
            {tr("EUC-JP"),                QStringLiteral("EUC-JP")},
            {tr("ISO-2022-JP"),           QStringLiteral("ISO-2022-JP")},
            {tr("EUC-KR"),                QStringLiteral("EUC-KR")},
            {tr("GB18030"),               QStringLiteral("GB18030")},
            {tr("GB2312"),                QStringLiteral("GB2312")},
            {tr("Big5"),                  QStringLiteral("Big5")},
        });
        addRegion(tr("Cyrillic"), {
            {tr("ISO-8859-5"),            QStringLiteral("ISO-8859-5")},
            {tr("KOI8-R"),                QStringLiteral("KOI8-R")},
            {tr("KOI8-U"),                QStringLiteral("KOI8-U")},
            {tr("Windows-1251"),          QStringLiteral("windows-1251")},
            {tr("IBM 866"),               QStringLiteral("IBM 866")},
        });
        addRegion(tr("Greek"), {
            {tr("ISO-8859-7"),            QStringLiteral("ISO-8859-7")},
            {tr("Windows-1253"),          QStringLiteral("windows-1253")},
        });
        addRegion(tr("Hebrew"), {
            {tr("ISO-8859-8"),            QStringLiteral("ISO-8859-8")},
            {tr("Windows-1255"),          QStringLiteral("windows-1255")},
        });
        addRegion(tr("Arabic"), {
            {tr("ISO-8859-6"),            QStringLiteral("ISO-8859-6")},
            {tr("Windows-1256"),          QStringLiteral("windows-1256")},
        });

        m->addSeparator();
        // Bottom group: "Convert to X — re-encode this content as X."
        auto addConv = [&](const QString& label, void (MainWindow::*slot)()) {
            auto* a = m->addAction(label);
            connect(a, &QAction::triggered, this, slot);
            return a;
        };
        addConv(tr("Convert to ANSI"),          &MainWindow::onEncodingConvertAnsi);
        addConv(tr("Convert to UTF-8"),         &MainWindow::onEncodingConvertUtf8);
        addConv(tr("Convert to UTF-8-BOM"),     &MainWindow::onEncodingConvertUtf8Bom);
        addConv(tr("Convert to UTF-16 BE BOM"), &MainWindow::onEncodingConvertUtf16Be);
        addConv(tr("Convert to UTF-16 LE BOM"), &MainWindow::onEncodingConvertUtf16Le);
    }

    // ---- Format (EOL) ----
    {
        auto* m = mb->addMenu(tr("F&ormat"));
        // Top group: "this buffer's EOL mode is X" — sets what newly-typed
        // lines use, doesn't touch existing bytes.
        auto* group = new QActionGroup(this);
        group->setExclusive(true);
        auto addRadio = [&](const QString& label, void (MainWindow::*slot)()) {
            auto* a = m->addAction(label);
            a->setCheckable(true);
            group->addAction(a);
            connect(a, &QAction::triggered, this, slot);
            return a;
        };
        m_eolWindows = addRadio(tr("&Windows (CR LF)"),  &MainWindow::onEolSetWindows);
        m_eolUnix    = addRadio(tr("&Unix (LF)"),        &MainWindow::onEolSetUnix);
        m_eolMac     = addRadio(tr("&Macintosh (CR)"),   &MainWindow::onEolSetMac);
        m->addSeparator();
        // Bottom group: "Convert all line endings in this buffer to X."
        auto addConv = [&](const QString& label, void (MainWindow::*slot)()) {
            auto* a = m->addAction(label);
            connect(a, &QAction::triggered, this, slot);
            return a;
        };
        addConv(tr("Convert to Windows (CR LF)"), &MainWindow::onEolConvertWindows);
        addConv(tr("Convert to Unix (LF)"),       &MainWindow::onEolConvertUnix);
        addConv(tr("Convert to Macintosh (CR)"),  &MainWindow::onEolConvertMac);
    }

    // ---- Language ----
    {
        m_languageMenu = mb->addMenu(tr("&Language"));
        m_languageMenu->setProperty("nlMenuId", "language");
        // Phase 5U.3 — body lives in rebuildLanguageMenu() so the dialog's
        // Save flow can tear it down and repopulate from a refreshed
        // Languages::all() without restarting the app.
        rebuildLanguageMenu();
    }

    // ---- Run ----
    {
        auto* m = mb->addMenu(tr("&Run"));
        m->setProperty("nlMenuId", "run");
        auto* aRun = m->addAction(tr("&Run..."));
        aRun->setShortcut(tr("F5"));
        connect(aRun, &QAction::triggered, this, &MainWindow::onRunCommand);
        // Saved Run commands persist via config.xml's RunCommands
        // entries; users wanting to rename/delete a saved command
        // can edit ~/.config/padnote/padnote--/config.xml directly.
        // A real management UI is a future polish phase.
    }

    // ---- Macro (Phase 5Z) ----
    // Start and Stop share Ctrl+Shift+R; mutual disable in
    // onMacroRecordingStateChanged means Qt routes the keystroke
    // unambiguously (disabled QActions don't fire shortcuts).
    {
        auto* m = mb->addMenu(tr("&Macro"));
        m->setProperty("nlMenuId", "macro");
        m_actMacroStart = m->addAction(tr("Start &Recording"));
        m_actMacroStart->setShortcut(tr("Ctrl+Shift+R"));
        connect(m_actMacroStart, &QAction::triggered,
                this, &MainWindow::onMacroStartRecording);

        m_actMacroStop = m->addAction(tr("Sto&p Recording"));
        m_actMacroStop->setShortcut(tr("Ctrl+Shift+R"));
        connect(m_actMacroStop, &QAction::triggered,
                this, &MainWindow::onMacroStopRecording);

        m_actMacroPlayLast = m->addAction(tr("&Play Last Macro"));
        m_actMacroPlayLast->setShortcut(tr("Ctrl+Shift+P"));
        connect(m_actMacroPlayLast, &QAction::triggered,
                this, &MainWindow::onMacroPlayLast);

        m_actMacroRunMulti = m->addAction(tr("Run a Macro &Multiple Times..."));
        connect(m_actMacroRunMulti, &QAction::triggered,
                this, &MainWindow::onMacroRunMultipleTimes);

        m->addSeparator();
        m_actMacroSave = m->addAction(tr("&Save Currently Recorded Macro..."));
        connect(m_actMacroSave, &QAction::triggered,
                this, &MainWindow::onMacroSaveCurrentRecorded);

        m->addSeparator();
        m_savedMacrosMenu = m->addMenu(tr("Sa&ved"));
        rebuildSavedMacrosMenu();
    }

    // ---- Tools ----
    {
        auto* m = mb->addMenu(tr("&Tools"));
        m->setProperty("nlMenuId", "tools");
        auto* mhash = m->addMenu(tr("&Hash"));
        auto* aMd5 = mhash->addAction(tr("&MD5"));
        connect(aMd5, &QAction::triggered, this, &MainWindow::onToolsHashMd5);
        auto* aSha1 = mhash->addAction(tr("&SHA-1"));
        connect(aSha1, &QAction::triggered, this, &MainWindow::onToolsHashSha1);
        auto* aSha256 = mhash->addAction(tr("SHA-&256"));
        connect(aSha256, &QAction::triggered, this, &MainWindow::onToolsHashSha256);
        auto* aSha512 = mhash->addAction(tr("SHA-&512"));
        connect(aSha512, &QAction::triggered, this, &MainWindow::onToolsHashSha512);
        m->addSeparator();
        // Phase 5AB — Word Count modal.
        auto* aWc = m->addAction(tr("&Word Count..."));
        connect(aWc, &QAction::triggered, this, &MainWindow::onToolsWordCount);
        // Phase 9d.1 — Summary dialog. Read-only modal showing file size,
        // mtime, encoding, EOL, language, plus the Word Count totals.
        auto* aSum = m->addAction(tr("&Summary..."));
        connect(aSum, &QAction::triggered, this, &MainWindow::onToolsSummary);
    }

    // ---- Settings ----
    {
        auto* m = mb->addMenu(tr("&Settings"));
        m->setProperty("nlMenuId", "settings");
        // Phase 5N.1+ — Preferences dialog. settingsApplied fires on every
        // Apply / OK; MainWindow re-pulls live UI state from Config.
        auto* aPrefs = m->addAction(tr("&Preferences..."));
        connect(aPrefs, &QAction::triggered, this, [this]{
            PreferencesDialog dlg(this);
            connect(&dlg, &PreferencesDialog::settingsApplied,
                    this, &MainWindow::applyUiVisibilityFromConfig);
            connect(&dlg, &PreferencesDialog::settingsApplied,
                    this, &MainWindow::applyEditingPrefsFromConfig);
            connect(&dlg, &PreferencesDialog::settingsApplied,
                    this, &MainWindow::applyBackupPrefsFromConfig);
            connect(&dlg, &PreferencesDialog::settingsApplied,
                    this, &MainWindow::applyFileWatcherPrefsFromConfig);
            dlg.exec();
        });
        auto* aStyle = m->addAction(tr("&Style Configurator..."));
        connect(aStyle, &QAction::triggered,
                this, &MainWindow::onSettingsStyleConfigurator);
        auto* aMapper = m->addAction(tr("Shortcut &Mapper..."));
        connect(aMapper, &QAction::triggered,
                this, &MainWindow::onSettingsShortcutMapper);
        // Theme XMLs auto-load from ~/.config/padnote/padnote--/themes/.
        // Edit Popup Context Menu (data-driven right-click) is out of
        // scope until the menu is made config-driven.

        // Phase 7d — UI Language submenu. Populated from the bundled
        // nativeLang XMLs; "Default (English)" is always first and
        // selecting it clears the overlay. Live-applies translations
        // without restart by re-walking the menu bar after setActive.
        m->addSeparator();
        {
            auto* langMenu = m->addMenu(tr("UI &Language"));
            auto* group = new QActionGroup(this);
            group->setExclusive(true);

            auto addLangEntry = [&](const QString& display,
                                    const QString& payload) {
                QAction* a = langMenu->addAction(display);
                a->setCheckable(true);
                a->setData(payload);     // "" = default English
                if (Localization::active() == payload) a->setChecked(true);
                else if (payload.isEmpty() && Localization::active().isEmpty())
                    a->setChecked(true);
                group->addAction(a);
                connect(a, &QAction::triggered,
                        this, &MainWindow::onUiLanguageSelected);
            };

            addLangEntry(tr("Default (English)"), QString());
            langMenu->addSeparator();
            for (const Localization::Lang& lang : Localization::available()) {
                addLangEntry(lang.displayName, lang.displayName);
            }
        }

        m->addSeparator();
        // Phase 5S — system tray. Available only when the desktop has a
        // tray (KDE / XFCE / Cinnamon / MATE / GNOME-with-extension).
        // Hidden on stock GNOME — Settings → Tray submenu disables
        // the toggle when QSystemTrayIcon::isSystemTrayAvailable is
        // false (no addStub fallback; just a disabled QAction).
        m_actMinimizeToTray = m->addAction(tr("Minimize to system &tray"));
        m_actMinimizeToTray->setCheckable(true);
        connect(m_actMinimizeToTray, &QAction::triggered,
                this, &MainWindow::onSettingsMinimizeToTray);
        if (!QSystemTrayIcon::isSystemTrayAvailable()) {
            m_actMinimizeToTray->setEnabled(false);
            m_actMinimizeToTray->setToolTip(
                tr("No system tray detected on this desktop."));
        }
    }

    // ---- Window ----
    {
        auto* m = mb->addMenu(tr("&Window"));
        m->setProperty("nlMenuId", "Window");
        auto* aList = m->addAction(tr("&Window list..."));
        connect(aList, &QAction::triggered, this, &MainWindow::onWindowList);

        auto* aByName = m->addAction(tr("Sort by &Name"));
        connect(aByName, &QAction::triggered, this, &MainWindow::onWindowSortByName);

        auto* aByPath = m->addAction(tr("Sort by &Path"));
        connect(aByPath, &QAction::triggered, this, &MainWindow::onWindowSortByPath);

        // Phase 9r — dynamic buffer list. On every open, append one
        // entry per open buffer (across both panes); clicking activates.
        // The header separator + entries are removed before each
        // re-populate so toggling Window doesn't accumulate stale rows.
        m->addSeparator();
        connect(m, &QMenu::aboutToShow, this, [this, m]() {
            // Strip everything below the first separator (the static
            // List/Sort entries live above it).
            const auto actions = m->actions();
            int sepIdx = -1;
            for (int i = 0; i < actions.size(); ++i) {
                if (actions[i]->isSeparator()) { sepIdx = i; break; }
            }
            for (int i = actions.size() - 1; i > sepIdx; --i) {
                m->removeAction(actions[i]);
            }
            // Rebuild from current buffer set.
            int n = 1;
            for (Buffer* b : allBuffers()) {
                if (!b) continue;
                const QString label = QStringLiteral("&%1  %2")
                    .arg(n)
                    .arg(b->displayName());
                auto* a = m->addAction(label);
                connect(a, &QAction::triggered, this, [this, b]() {
                    if (!b) return;
                    // Find which pane hosts the buffer; switch to it.
                    for (int p = 0; p < 2; ++p) {
                        EditorTabs* tabs = (p == 0) ? leftPane() : rightPane();
                        if (!tabs) continue;
                        const int idx = tabs->indexOfBuffer(b);
                        if (idx >= 0) {
                            if (p == 1 && rightPane() && !rightPane()->isVisible())
                                showSplitPane(true);
                            tabs->setCurrentIndex(idx);
                            tabs->setFocus();
                            return;
                        }
                    }
                });
                ++n;
                if (n > 99) break;   // sanity cap
            }
        });
    }

    // ---- Plugins (Phase 10b) ----
    // Built once at startup with whatever PluginLoader::loadAll() found.
    // Each loaded plugin gets its own submenu of FuncItem actions; the
    // submenu's children dispatch back to the plugin's `func()` via
    // QAction::data(int = LinuxFuncItem.cmdID).
    {
        m_pluginsMenu = mb->addMenu(tr("&Plugins"));
        m_pluginsMenu->setProperty("nlMenuId", "plugins");
        rebuildPluginsMenu();
    }

    // ---- ? (Help) ----
    {
        auto* m = mb->addMenu(tr("&?"));
        auto* aAbout = m->addAction(tr("&About padnote--..."));
        connect(aAbout, &QAction::triggered, this, &MainWindow::onHelpAbout);

        auto* aDocs = m->addAction(tr("&Documentation"));
        connect(aDocs, &QAction::triggered, this, &MainWindow::onHelpOnlineDocs);

        auto* aForum = m->addAction(tr("&Forum"));
        connect(aForum, &QAction::triggered, this, &MainWindow::onHelpForum);

        auto* aDebug = m->addAction(tr("&Debug Info..."));
        connect(aDebug, &QAction::triggered, this, &MainWindow::onHelpDebugInfo);
    }
}

void MainWindow::buildToolbar()
{
    auto* tb = addToolBar(tr("Main toolbar"));
    tb->setObjectName(QStringLiteral("mainToolBar"));
    tb->setMovable(false);
    QStyle* s = style();

    auto add = [&](QStyle::StandardPixmap icon, const QString& tip, auto slot){
        auto* a = tb->addAction(s->standardIcon(icon), tip);
        a->setToolTip(tip);
        connect(a, &QAction::triggered, this, slot);
        return a;
    };

    add(QStyle::SP_FileIcon,        tr("New"),    [this]{ onFileNew(); });
    add(QStyle::SP_DirOpenIcon,     tr("Open..."), [this]{ onFileOpen(); });
    add(QStyle::SP_DialogSaveButton, tr("Save"),   [this]{ onFileSave(); });
    add(QStyle::SP_DialogSaveButton, tr("Save All"), [this]{ onFileSaveAll(); });
    tb->addSeparator();
    add(QStyle::SP_ArrowBack,       tr("Undo"),   [this]{ onEditUndo(); });
    add(QStyle::SP_ArrowForward,    tr("Redo"),   [this]{ onEditRedo(); });
    tb->addSeparator();
    add(QStyle::SP_FileDialogContentsView, tr("Find..."),
        [this]{ onSearchFind(); });
    add(QStyle::SP_BrowserReload,   tr("Reload from disk"),
        [this]{ onFileReloadFromDisk(); });
}

void MainWindow::buildStatusBar()
{
    m_statusCaret    = new QLabel(tr("Ln 1, Col 1"), this);
    m_statusLanguage = new QLabel(tr("Plain text"), this);
    m_statusEncoding = new QLabel(tr("UTF-8"), this);
    m_statusEol      = new QLabel(tr("Unix (LF)"), this);

    statusBar()->addPermanentWidget(m_statusLanguage);
    statusBar()->addPermanentWidget(m_statusEol);
    statusBar()->addPermanentWidget(m_statusEncoding);
    statusBar()->addPermanentWidget(m_statusCaret);
    statusBar()->showMessage(tr("Ready"));
}

void MainWindow::wireScintillaStatusUpdates(Buffer* buffer)
{
    if (!buffer) return;
    static QTimer* refreshTimer = nullptr;
    if (!refreshTimer) {
        refreshTimer = new QTimer(this);
        refreshTimer->setInterval(200);
        connect(refreshTimer, &QTimer::timeout, this,
                &MainWindow::updateCaretStatus);
        refreshTimer->start();
    }
}

void MainWindow::updateCaretStatus()
{
    Buffer* b = activePane()->currentBuffer();
    if (!b) {
        m_statusCaret->setText(tr("—"));
        return;
    }
    auto* ed = b->editor();
    const Scintilla::sptr_t pos =
        ed->send(static_cast<unsigned int>(Message::GetCurrentPos));
    const Scintilla::sptr_t line =
        ed->send(static_cast<unsigned int>(Message::LineFromPosition), pos) + 1;
    const Scintilla::sptr_t col =
        ed->send(static_cast<unsigned int>(Message::GetColumn), pos) + 1;
    const Scintilla::sptr_t selBytes =
        ed->send(static_cast<unsigned int>(Message::GetSelectionEnd))
        - ed->send(static_cast<unsigned int>(Message::GetSelectionStart));
    // Phase 5Y — surface "N selections" when multi-cursor is active.
    const Scintilla::sptr_t nSel =
        ed->send(static_cast<unsigned int>(Message::GetSelections));
    QString text;
    if (nSel > 1) {
        text = tr("Ln %1, Col %2  %3 selections").arg(line).arg(col).arg(nSel);
    } else if (selBytes > 0) {
        text = tr("Ln %1, Col %2  Sel %3").arg(line).arg(col).arg(selBytes);
    } else {
        text = tr("Ln %1, Col %2").arg(line).arg(col);
    }
    // Phase 9c.1 — show "[RO]" in the caret pane when the user has flipped
    // the active buffer to read-only.
    if (b->isReadOnly()) text += tr("  [RO]");
    m_statusCaret->setText(text);
}

void MainWindow::updateLanguageStatus()
{
    Buffer* b = activePane()->currentBuffer();
    if (!b) { m_statusLanguage->setText(tr("—")); return; }
    const LanguageDef* L = b->language();
    m_statusLanguage->setText(L ? L->displayName : tr("Plain text"));

    // Sync the Language menu's checked entry.
    for (std::size_t i = 0;
         i < m_languageActions.size() && static_cast<int>(i) < Languages::count();
         ++i) {
        const LanguageDef* candidate = &Languages::all()[i];
        m_languageActions[i]->setChecked(candidate == L);
    }
}

void MainWindow::updateEncodingStatus()
{
    Buffer* b = activePane()->currentBuffer();
    if (!b) {
        m_statusEncoding->setText(tr("—"));
        return;
    }
    const EncodingInfo enc = b->encoding();
    m_statusEncoding->setText(Encoding::displayName(enc));

    // Walk every radio in the menu; tick the one whose EncodingInfo matches
    // the active buffer (or none if it's outside the menu's coverage —
    // updateEncodingStatus only runs after a real buffer change).
    for (auto& [action, radioEnc] : m_encodingRadios) {
        const bool match =
            enc.name.compare(radioEnc.name, Qt::CaseInsensitive) == 0
            && enc.hasBom == radioEnc.hasBom;
        action->setChecked(match);
    }
}

void MainWindow::updateEolStatus()
{
    Buffer* b = activePane()->currentBuffer();
    if (!b) {
        m_statusEol->setText(tr("—"));
        return;
    }
    const Buffer::EolMode mode = b->eolMode();
    QString label;
    switch (mode) {
        case Buffer::EolMode::Crlf: label = tr("Windows (CR LF)"); break;
        case Buffer::EolMode::Lf:   label = tr("Unix (LF)");       break;
        case Buffer::EolMode::Cr:   label = tr("Macintosh (CR)");  break;
    }
    m_statusEol->setText(label);

    if (m_eolWindows) m_eolWindows->setChecked(mode == Buffer::EolMode::Crlf);
    if (m_eolUnix)    m_eolUnix   ->setChecked(mode == Buffer::EolMode::Lf);
    if (m_eolMac)     m_eolMac    ->setChecked(mode == Buffer::EolMode::Cr);
}

// -----------------------------------------------------------------------------
// Slots
// -----------------------------------------------------------------------------

void MainWindow::onCurrentBufferChanged(Buffer* buffer)
{
    // Phase 3d — both panes connect here. Wire per-buffer signals on the
    // sender's new buffer regardless of which pane sent the signal. Then,
    // if the sender pane is the active one (or this is a tab switch in a
    // pane that should now BE active), refresh the chrome.
    EditorTabs* sender_pane = qobject_cast<EditorTabs*>(sender());
    if (buffer) {
        // Idempotent: Qt::UniqueConnection makes repeated calls harmless.
        connect(buffer, &Buffer::languageChanged,
                this, &MainWindow::onBufferLanguageChanged,
                Qt::UniqueConnection);
        connect(buffer, &Buffer::encodingChanged,
                this, &MainWindow::onBufferEncodingChanged,
                Qt::UniqueConnection);
        connect(buffer, &Buffer::eolModeChanged,
                this, &MainWindow::onBufferEolModeChanged,
                Qt::UniqueConnection);
        // Phase 5T — when a buffer's path changes (load / saveAs / rename),
        // re-sync the file watcher.
        connect(buffer, &Buffer::displayNameChanged,
                this, &MainWindow::onBufferDisplayNameChanged,
                Qt::UniqueConnection);
        // Phase 9c.1 — read-only toggle radio mirror.
        connect(buffer, &Buffer::readOnlyChanged,
                this, &MainWindow::onBufferReadOnlyChanged,
                Qt::UniqueConnection);
    }

    // If a tab switch (or open/close) happened in a pane that wasn't active,
    // promote that pane to active so the user's interaction drives the
    // chrome. setActivePane will recursively re-enter onCurrentBufferChanged
    // — guarded against an infinite loop because m_activePane only changes
    // once.
    if (sender_pane && sender_pane != activePane() && buffer) {
        const int idx = paneIndexOf(sender_pane);
        if (idx >= 0) {
            setActivePane(idx);
            return;
        }
    }

    // Tab opens / closes don't carry per-buffer signals, but
    // currentBufferChanged fires on those too — re-sync the watcher each
    // time the active tab changes.
    syncFileWatcher();

    Buffer* active = activePane()->currentBuffer();
    updateCaretStatus();
    updateLanguageStatus();
    updateEncodingStatus();
    updateEolStatus();
    // Phase 9c.1 — sync the File → Read-Only toggle to the new active buffer.
    if (m_actReadOnly) {
        m_actReadOnly->setChecked(active ? active->isReadOnly() : false);
    }
    // Phase 9k — keep the MRU front-of-list = currently active buffer.
    // QPointer auto-nulls on Buffer destruction so we sweep dead entries
    // alongside the move-to-front. Cap the list at 50 to bound the
    // sweep cost on long-lived sessions.
    if (active) {
        for (auto it = m_mruBuffers.begin(); it != m_mruBuffers.end(); ) {
            if (it->isNull() || it->data() == active) it = m_mruBuffers.erase(it);
            else ++it;
        }
        m_mruBuffers.prepend(QPointer<Buffer>(active));
        while (m_mruBuffers.size() > 50) m_mruBuffers.removeLast();
    }
    if (m_findReplace) {
        m_findReplace->setEditor(active ? active->editor() : nullptr);
    }
    // Phase 3c.1 — re-target the document map at the new active buffer.
    if (m_documentMap && m_documentMap->isVisible()) {
        m_documentMap->setBuffer(active);
    }
    // Phase 3c.2 — re-target the function list at the new active buffer.
    if (m_functionList && m_functionList->isVisible()) {
        m_functionList->setBuffer(active);
    }
    if (active) {
        setWindowTitle(tr("%1 — padnote--").arg(active->displayName()));
        wireScintillaStatusUpdates(active);
        active->editor()->setFocus();
    } else {
        setWindowTitle(tr("padnote--"));
    }
}

void MainWindow::onFileNew()
{
    activePane()->newUntitled();
}

void MainWindow::onFileOpen()
{
    // Phase 5N.5 — seed the file dialog with Config::defaultOpenDirectory
    // when set. Empty (the default) → QFileDialog uses its standard
    // remember-last-used behaviour.
    const QString seed = Config::defaultOpenDirectory();
    const QStringList paths = QFileDialog::getOpenFileNames(this,
        tr("Open file(s)"), seed);
    for (const QString& p : paths) activePane()->openFile(p);
}

bool MainWindow::onFileSave()
{
    Buffer* b = activePane()->currentBuffer();
    if (!b) return false;
    if (!b->hasFile()) return onFileSaveAs();

    QString err;
    if (!b->saveToFile(b->filePath(), &err)) {
        QMessageBox::warning(this, tr("Save failed"),
            tr("Cannot write %1:\n%2").arg(b->filePath(), err));
        return false;
    }
    statusBar()->showMessage(tr("Saved %1").arg(b->displayName()), 3000);
    return true;
}

bool MainWindow::onFileSaveAs()
{
    Buffer* b = activePane()->currentBuffer();
    if (!b) return false;
    const QString path = QFileDialog::getSaveFileName(this, tr("Save as"),
        b->hasFile() ? b->filePath() : b->displayName());
    if (path.isEmpty()) return false;
    QString err;
    if (!b->saveToFile(path, &err)) {
        QMessageBox::warning(this, tr("Save failed"),
            tr("Cannot write %1:\n%2").arg(path, err));
        return false;
    }
    statusBar()->showMessage(tr("Saved %1").arg(b->displayName()), 3000);
    return true;
}

bool MainWindow::onFileSaveAll()
{
    // Phase 3d — walk both panes. For an untitled dirty buffer in a non-
    // active pane we promote that pane to active first (onFileSaveAs reads
    // activePane()->currentBuffer()).
    bool allOk = true;
    for (int p = 0; p < 2; ++p) {
        EditorTabs* pane = m_panes[p];
        if (!pane) continue;
        for (int i = 0; i < pane->bufferCount(); ++i) {
            Buffer* b = pane->bufferAt(i);
            if (!b || !b->isDirty()) continue;
            if (!b->hasFile()) {
                setActivePane(p);
                pane->setCurrentIndex(i);
                allOk = onFileSaveAs() && allOk;
            } else {
                QString err;
                allOk = b->saveToFile(b->filePath(), &err) && allOk;
            }
        }
    }
    return allOk;
}

void MainWindow::onFileCloseTab()
{
    EditorTabs* pane = activePane();
    const int idx = pane->currentIndex();
    Buffer* b = pane->bufferAt(idx);
    // Phase 9n — refuse to close pinned tabs. The user has to right-click
    // → Unpin first; this is the price of pinning. Status nudge so the
    // Ctrl+W keystroke isn't a silent no-op.
    if (b && b->isPinned()) {
        statusBar()->showMessage(
            tr("Tab is pinned; unpin first to close."), 3000);
        return;
    }
    if (confirmCloseAt(pane, idx)) {
        pane->dropBufferAt(idx);
        if (pane->bufferCount() == 0) pane->newUntitled();
    }
}

void MainWindow::onFileCloseAll()
{
    // Phase 9n — skip pinned tabs in both panes. Window-close still uses
    // closeAllBuffers() directly which is unconditional — pinning is a
    // session-scoped affordance, not a "refuse to quit" mechanism.
    int skippedPinned = 0;
    for (int p = 0; p < 2; ++p) {
        EditorTabs* pane = m_panes[p];
        if (!pane) continue;
        bool advancing = true;
        while (advancing) {
            advancing = false;
            for (int i = 0; i < pane->bufferCount(); ++i) {
                Buffer* b = pane->bufferAt(i);
                if (b && b->isPinned()) continue;
                if (!confirmCloseAt(pane, i)) return;
                pane->dropBufferAt(i);
                advancing = true;
                break;
            }
        }
    }
    for (int p = 0; p < 2; ++p) {
        EditorTabs* pane = m_panes[p];
        if (!pane) continue;
        for (int i = 0; i < pane->bufferCount(); ++i) {
            Buffer* b = pane->bufferAt(i);
            if (b && b->isPinned()) ++skippedPinned;
        }
        if (pane->bufferCount() == 0) pane->newUntitled();
    }
    if (skippedPinned > 0) {
        statusBar()->showMessage(
            tr("Kept %n pinned tab(s) open.", nullptr, skippedPinned), 3000);
    }
}

// ---- Phase 5j — File menu small wins ----

void MainWindow::onFileSaveCopyAs()
{
    Buffer* b = activePane()->currentBuffer();
    if (!b) return;
    const QString path = QFileDialog::getSaveFileName(this,
        tr("Save a Copy As"),
        b->hasFile() ? b->filePath() : b->displayName());
    if (path.isEmpty()) return;
    QString err;
    if (!b->writeCopyTo(path, &err)) {
        QMessageBox::warning(this, tr("Save a Copy failed"),
            tr("Cannot write %1:\n%2").arg(path, err));
        return;
    }
    statusBar()->showMessage(tr("Wrote copy to %1").arg(path), 4000);
}

void MainWindow::onFileRename()
{
    Buffer* b = activePane()->currentBuffer();
    if (!b) return;
    if (!b->hasFile()) {
        statusBar()->showMessage(
            tr("Rename: this is an untitled buffer — use Save As instead."), 4000);
        return;
    }
    const QString oldPath = b->filePath();
    const QString newPath = QFileDialog::getSaveFileName(this,
        tr("Rename"), oldPath);
    if (newPath.isEmpty() || newPath == oldPath) return;

    // Flush a dirty buffer to the OLD path first so the rename moves
    // consistent bytes. Otherwise QFile::rename moves stale on-disk
    // bytes and the buffer would still claim ownership of the new
    // name with newer in-memory content.
    if (b->isDirty()) {
        QString err;
        if (!b->saveToFile(oldPath, &err)) {
            QMessageBox::warning(this, tr("Rename failed"),
                tr("Could not save buffer to %1 before rename:\n%2")
                    .arg(oldPath, err));
            return;
        }
    }

    if (!QFile::rename(oldPath, newPath)) {
        QMessageBox::warning(this, tr("Rename failed"),
            tr("Could not rename %1 to %2.").arg(oldPath, newPath));
        return;
    }
    b->setFilePath(newPath);
    statusBar()->showMessage(tr("Renamed to %1").arg(newPath), 4000);
}

void MainWindow::onFileCloseAllBut()
{
    EditorTabs* pane = activePane();
    Buffer* keep = pane->currentBuffer();
    if (!keep) return;
    // Walk forward, closing every non-`keep`, non-pinned tab. dropBufferAt
    // mutates indices, so we re-scan each iteration. confirmCloseAt may
    // abort the loop on user Cancel. Phase 3d note: scoped to the active
    // pane (closing tabs in the other view from here would surprise users).
    // Phase 9n — pinned tabs survive the bulk close silently (no per-tab
    // status nudge would be useful). The targeted tab is the first that's
    // neither `keep` nor pinned.
    int skippedPinned = 0;
    while (pane->bufferCount() > 1) {
        int target = -1;
        for (int i = 0; i < pane->bufferCount(); ++i) {
            Buffer* b = pane->bufferAt(i);
            if (b == keep) continue;
            if (b && b->isPinned()) continue;
            target = i;
            break;
        }
        if (target < 0) break;
        if (!confirmCloseAt(pane, target)) return;
        pane->dropBufferAt(target);
    }
    // Surface a single roll-up nudge if any pinned tabs were spared so the
    // user knows the bulk close didn't silently miss work.
    for (int i = 0; i < pane->bufferCount(); ++i) {
        Buffer* b = pane->bufferAt(i);
        if (b && b != keep && b->isPinned()) ++skippedPinned;
    }
    if (skippedPinned > 0) {
        statusBar()->showMessage(
            tr("Kept %n pinned tab(s) open.", nullptr, skippedPinned), 3000);
    }
}

void MainWindow::onFileMoveToTrash()
{
    Buffer* b = activePane()->currentBuffer();
    if (!b) return;
    if (!b->hasFile()) {
        statusBar()->showMessage(
            tr("Move to Trash: this buffer has no file on disk."), 4000);
        return;
    }
    const QString path = b->filePath();

    const auto reply = QMessageBox::question(this, tr("Move to Trash"),
        tr("Move '%1' to the system trash and close this tab?").arg(path),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (reply != QMessageBox::Yes) return;

    // Prefer `gio trash` (GLib/GVFS — covers GNOME / KDE / XFCE on every
    // modern desktop distro). Fall back to QFile::moveToTrash (Qt's own
    // implementation, also Linux-aware). Final fallback: confirm a
    // permanent delete so the user is never stuck.
    QProcess gio;
    gio.start(QStringLiteral("gio"),
              {QStringLiteral("trash"), path});
    const bool gioOk = gio.waitForStarted(2000)
                    && gio.waitForFinished(5000)
                    && gio.exitStatus() == QProcess::NormalExit
                    && gio.exitCode() == 0;
    if (!gioOk) {
        if (!QFile::moveToTrash(path)) {
            const auto fallback = QMessageBox::question(this,
                tr("Move to Trash"),
                tr("Trash is unavailable. Permanently delete '%1' instead?")
                    .arg(path),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (fallback != QMessageBox::Yes) return;
            if (!QFile::remove(path)) {
                QMessageBox::warning(this, tr("Move to Trash failed"),
                    tr("Could not delete %1.").arg(path));
                return;
            }
        }
    }

    // The user authorised discarding the file's bytes; skip the Q11
    // dirty-prompt and drop the tab directly.
    EditorTabs* pane = activePane();
    const int idx = pane->indexOfBuffer(b);
    if (idx >= 0) {
        pane->dropBufferAt(idx);
        if (pane->bufferCount() == 0) pane->newUntitled();
    }
    statusBar()->showMessage(tr("Moved %1 to trash").arg(path), 4000);
}

// =============================================================================
// Phase 9c.1 — File → Read-Only / Clear Read-Only Flag.
//
// Per-buffer transient state. The toggle action mirrors the active buffer's
// m_readOnly; flipping it pushes SCI_SETREADONLY through Buffer::setReadOnly
// and updates every chrome pointer (status bar, menu radio). Clearing is
// the explicit "off" path — handy when the user toggled by mistake.
//
// NOT persisted: when the user reopens the file, it loads writable. Distinct
// from the file-on-disk read-only bit (we don't auto-set the flag on a
// chmod -w'd file today; future polish could).
// =============================================================================

void MainWindow::onFileToggleReadOnly()
{
    Buffer* b = activePane()->currentBuffer();
    if (!b) {
        if (m_actReadOnly) m_actReadOnly->setChecked(false);
        return;
    }
    const bool wantRO = !b->isReadOnly();
    b->setReadOnly(wantRO);
    statusBar()->showMessage(
        wantRO ? tr("Buffer is now read-only.")
               : tr("Buffer is now writable."),
        2500);
}

void MainWindow::onFileClearReadOnly()
{
    Buffer* b = activePane()->currentBuffer();
    if (!b) return;
    if (!b->isReadOnly()) {
        statusBar()->showMessage(
            tr("Buffer was already writable."), 2500);
        return;
    }
    b->setReadOnly(false);
    statusBar()->showMessage(tr("Read-Only flag cleared."), 2500);
}

void MainWindow::onBufferReadOnlyChanged(Buffer* buffer)
{
    if (buffer != activePane()->currentBuffer()) return;
    if (m_actReadOnly) m_actReadOnly->setChecked(buffer->isReadOnly());
    updateCaretStatus();   // refresh "[RO]" suffix
}

void MainWindow::onFileLoadSession()
{
    const QString path = QFileDialog::getOpenFileName(this,
        tr("Load Session"), QString{},
        tr("Session XML (*.xml);;All files (*)"));
    if (path.isEmpty()) return;
    bool wasSplit = false;
    int  activeView = 0;
    const int n = Session::restoreFromFile(m_panes[0], m_panes[1],
                                           &wasSplit, &activeView, path);
    if (wasSplit) showSplitPane(true);
    setActivePane(activeView == 1 && rightPaneVisible() ? 1 : 0);
    statusBar()->showMessage(
        n > 0 ? tr("Loaded %1 file(s) from session %2").arg(n).arg(path)
              : tr("No openable files in %1").arg(path),
        5000);
}

void MainWindow::onFileSaveSessionAs()
{
    const QString path = QFileDialog::getSaveFileName(this,
        tr("Save Session As"), QStringLiteral("session.xml"),
        tr("Session XML (*.xml);;All files (*)"));
    if (path.isEmpty()) return;
    if (!Session::saveToFile(m_panes[0], m_panes[1],
                              rightPaneVisible(), m_activePane, path)) {
        QMessageBox::warning(this, tr("Save Session failed"),
            tr("Could not write session to %1").arg(path));
        return;
    }
    statusBar()->showMessage(tr("Saved session to %1").arg(path), 4000);
}

namespace {

// Phase 5N.10 — substitute print-header/footer macros against the active
// buffer's path-derived placeholders + the current date/time. Phase 9o
// added per-page $(PAGE_NUMBER) / $(NB_PAGES) substitution; pass page=-1
// when those aren't known (the manual-pagination path computes total
// pages first, then re-substitutes per page).
QString substitutePrintMacros(const QString& tmpl, Buffer* b,
                              int pageNumber = -1, int totalPages = -1)
{
    if (tmpl.isEmpty()) return tmpl;
    const QString full = (b && b->hasFile()) ? b->filePath() : QString{};
    const QFileInfo fi(full);
    QString out = tmpl;
    out.replace(QStringLiteral("$(FULL_CURRENT_PATH)"), full);
    out.replace(QStringLiteral("$(FILE_NAME)"),
                b ? b->displayName() : QString{});
    out.replace(QStringLiteral("$(NAME_PART)"), fi.completeBaseName());
    out.replace(QStringLiteral("$(EXT_PART)"),  fi.suffix());
    out.replace(QStringLiteral("$(CURRENT_DATE)"),
                QDate::currentDate().toString(Qt::ISODate));
    out.replace(QStringLiteral("$(CURRENT_TIME)"),
                QTime::currentTime().toString(QStringLiteral("HH:mm:ss")));
    if (pageNumber >= 0)
        out.replace(QStringLiteral("$(PAGE_NUMBER)"),
                    QString::number(pageNumber));
    if (totalPages >= 0)
        out.replace(QStringLiteral("$(NB_PAGES)"),
                    QString::number(totalPages));
    return out;
}

// Phase 5T — syntax-highlighted print via SCI_FORMATRANGEFULL.
// Two-pass: a "measure" pass walks the doc with FORMATRANGEFULL in
// no-draw mode to enumerate page boundaries (gives us the total page
// count for $(NB_PAGES) and lets the draw pass supply pre-computed
// chrg ranges per page), then a "draw" pass renders header → body
// slice → footer per page.
//
// Risk noted in ROADMAP_v3: Scintilla's Qt FORMATRANGE port is tested
// via the ScintillaEdit template binding but no real Qt apps we know
// of exercise it. The pattern below relies on Qt SurfaceImpl reusing
// our active QPainter via `device->paintingActive()` (PlatQt.cpp:786) —
// we keep ONE painter session over the whole print so the surface
// doesn't churn.
//
// Fall-back: a Preferences toggle (Config::syntaxHighlightedPrint)
// lets the user opt back to the plain-text path if FORMATRANGEFULL
// crashes or paints wrong on their printer.
void printSyntaxHighlighted(QPrinter* printer, Buffer* b,
                            const QString& headerTmpl,
                            const QString& footerTmpl)
{
    auto* ed = b->editor();
    const Scintilla::sptr_t docLen =
        ed->send(static_cast<unsigned int>(Message::GetLength));
    if (docLen <= 0) return;

    // Save/restore Scintilla's print colour mode. Default Scintilla
    // print colour mode is 0 (NORMAL — paints the editor's bg colour
    // including dark themes). We force SC_PRINT_COLOURONWHITEDEFAULTBG
    // (=4) so a dark-theme buffer prints with its dark text colour
    // mapped onto white paper — saves toner and matches user
    // expectations for paper output.
    const auto prevColour = ed->send(
        static_cast<unsigned int>(Message::GetPrintColourMode));
    ed->send(static_cast<unsigned int>(Message::SetPrintColourMode), 4);

    // Word-wrap on overflow. Without this, long lines get cropped at
    // the right margin.
    const auto prevWrap = ed->send(
        static_cast<unsigned int>(Message::GetPrintWrapMode));
    ed->send(static_cast<unsigned int>(Message::SetPrintWrapMode), 1);

    // Phase 5T-polish — print font-size offset relative to the
    // editor's. 0 = no change; +N enlarges by N points; -N shrinks.
    // Editor's screen-tuned font size (~10pt) is often unreadable on
    // paper, so users can bump it up here without changing the
    // on-screen size.
    const auto prevMag = ed->send(
        static_cast<unsigned int>(Message::GetPrintMagnification));
    ed->send(static_cast<unsigned int>(Message::SetPrintMagnification),
             Config::printMagnification());

    QPainter painter;
    if (!painter.begin(printer)) {
        ed->send(static_cast<unsigned int>(Message::SetPrintColourMode),
                 prevColour);
        ed->send(static_cast<unsigned int>(Message::SetPrintWrapMode),
                 prevWrap);
        ed->send(static_cast<unsigned int>(Message::SetPrintMagnification),
                 prevMag);
        return;
    }

    // Header/footer painting helpers reuse a small body of code from
    // printWithHeaderFooter; declared inline so we don't have to factor
    // out shared utilities (the bodies are short enough).
    const QFont chromeFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    const QFontMetricsF fm(chromeFont, painter.device());
    const qreal lineH    = fm.lineSpacing();
    const qreal headerH  = headerTmpl.isEmpty() ? 0.0 : (lineH + 6.0);
    const qreal footerH  = footerTmpl.isEmpty() ? 0.0 : (lineH + 6.0);
    const QRectF pageRectF = printer->pageRect(QPrinter::DevicePixel);
    const QRect  pageRect  = pageRectF.toRect();
    const qreal  pageH     = pageRectF.height();
    const QRect  bodyRect(
        pageRect.left(),
        pageRect.top() + static_cast<int>(headerH),
        pageRect.width(),
        pageRect.height() - static_cast<int>(headerH + footerH));

    if (bodyRect.height() <= 0) {
        painter.end();
        ed->send(static_cast<unsigned int>(Message::SetPrintColourMode),
                 prevColour);
        ed->send(static_cast<unsigned int>(Message::SetPrintWrapMode),
                 prevWrap);
        return;
    }

    // Measure pass: walk the doc with FORMATRANGEFULL in no-draw mode.
    // The Qt SurfaceImpl will reuse our active painter via
    // device->paintingActive() (PlatQt.cpp:786) — no painter churn.
    QVector<Scintilla::sptr_t> pageStarts;
    pageStarts.append(0);
    {
        Scintilla::sptr_t cpMin = 0;
        // Safety cap: 4096 pages = ~250 000 lines at a fairly dense
        // 60 lines/page. Beyond that we abort and fall back to the
        // plain-text path; the user can split the file or disable
        // syntax-highlighted print.
        for (int safety = 0; cpMin < docLen && safety < 4096; ++safety) {
            Sci_RangeToFormatFull fr{};
            fr.hdc        = painter.device();
            fr.hdcTarget  = painter.device();
            fr.rc.left    = bodyRect.left();
            fr.rc.top     = bodyRect.top();
            fr.rc.right   = bodyRect.right();
            fr.rc.bottom  = bodyRect.bottom();
            fr.rcPage     = {pageRect.left(), pageRect.top(),
                             pageRect.right(), pageRect.bottom()};
            fr.chrg.cpMin = cpMin;
            fr.chrg.cpMax = docLen;

            const Scintilla::sptr_t next = ed->send(
                static_cast<unsigned int>(Message::FormatRangeFull),
                0,                              // measure only (no draw)
                reinterpret_cast<Scintilla::sptr_t>(&fr));
            if (next <= cpMin) break;           // no progress — bail
            cpMin = next;
            if (cpMin < docLen) pageStarts.append(cpMin);
        }
    }
    const int totalPages = pageStarts.size();

    auto paintBand = [&](const QString& tmpl, qreal yTop, int page) {
        if (tmpl.isEmpty()) return;
        const QString line = substitutePrintMacros(
            tmpl, b, page + 1, totalPages);
        painter.save();
        painter.setFont(chromeFont);
        painter.setPen(QColor(70, 70, 70));
        const QRectF rect(pageRect.left(), pageRect.top() + yTop,
                          pageRect.width(), lineH);
        painter.drawText(rect,
            Qt::AlignVCenter | Qt::AlignLeft | Qt::TextSingleLine, line);
        painter.setPen(QPen(QColor(140, 140, 140), 0.5));
        const qreal sepY = (yTop < pageH / 2.0)
            ? rect.bottom() + 2.0
            : rect.top()    - 2.0;
        painter.drawLine(QPointF(pageRect.left(),  pageRect.top() + sepY),
                         QPointF(pageRect.right(), pageRect.top() + sepY));
        painter.restore();
    };

    // Draw pass: per page, paint header → FORMATRANGEFULL body slice → footer.
    for (int p = 0; p < totalPages; ++p) {
        if (p > 0) printer->newPage();

        paintBand(headerTmpl, 0.0,                p);
        paintBand(footerTmpl, pageH - footerH,    p);

        Sci_RangeToFormatFull fr{};
        fr.hdc        = painter.device();
        fr.hdcTarget  = painter.device();
        fr.rc.left    = bodyRect.left();
        fr.rc.top     = bodyRect.top();
        fr.rc.right   = bodyRect.right();
        fr.rc.bottom  = bodyRect.bottom();
        fr.rcPage     = {pageRect.left(), pageRect.top(),
                         pageRect.right(), pageRect.bottom()};
        fr.chrg.cpMin = pageStarts[p];
        fr.chrg.cpMax = (p + 1 < totalPages) ? pageStarts[p + 1] : docLen;

        // Save/restore painter state — Scintilla's surface impl mutates
        // pen / brush / font during rendering and doesn't promise to
        // restore them.
        painter.save();
        ed->send(static_cast<unsigned int>(Message::FormatRangeFull),
                 1,                                 // draw mode
                 reinterpret_cast<Scintilla::sptr_t>(&fr));
        painter.restore();
    }

    painter.end();

    ed->send(static_cast<unsigned int>(Message::SetPrintColourMode),    prevColour);
    ed->send(static_cast<unsigned int>(Message::SetPrintWrapMode),      prevWrap);
    ed->send(static_cast<unsigned int>(Message::SetPrintMagnification), prevMag);
}

// Phase 9o — manual-pagination print path. Used when the user has
// configured a header or footer; lays out the body once into a
// QTextDocument sized to the printer's body region (page minus header
// and footer reservations), then walks pages drawing:
//   1. header (drawText, with PAGE_NUMBER / NB_PAGES substituted live)
//   2. a vertically-translated slice of the body text
//   3. footer (drawText, same substitution)
// PAGE_NUMBER / NB_PAGES use 1-based numbering (page 1 of N).
//
// Why not QTextDocument::print(QPagedPaintDevice*)? It paginates
// automatically but doesn't expose per-page hooks for header/footer
// rendering. Manual pagination via QPainter::drawText for header/footer
// + QTextDocument::drawContents for the body slice is the conventional
// Qt 6 idiom (see "Document Printing" in QTextDocument docs).
void printWithHeaderFooter(QPrinter* printer, Buffer* b, const QString& text,
                           const QString& headerTmpl, const QString& footerTmpl)
{
    QPainter painter;
    if (!painter.begin(printer)) return;

    const QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    painter.setFont(font);
    const QFontMetricsF fm(font, painter.device());

    // Reserve a single line of header / footer height plus a small
    // padding gap so they don't crash into the body text.
    const qreal lineH    = fm.lineSpacing();
    const qreal headerH  = headerTmpl.isEmpty() ? 0.0 : (lineH + 6.0);
    const qreal footerH  = footerTmpl.isEmpty() ? 0.0 : (lineH + 6.0);
    const QRectF pageRect = printer->pageRect(QPrinter::DevicePixel);
    const qreal pageW    = pageRect.width();
    const qreal pageH    = pageRect.height();
    const qreal bodyTop  = headerH;
    const qreal bodyH    = pageH - headerH - footerH;
    if (bodyH <= 0.0) {                                 // pathological margins
        painter.end();
        return;
    }

    QTextDocument doc;
    doc.setDefaultFont(font);
    doc.setPageSize(QSizeF(pageW, bodyH));
    doc.setPlainText(text);

    const int totalPages = qMax(1, doc.pageCount());

    // Helpers for header/footer painting on the current page. drawText
    // anchors at the top-left of the supplied rect; we right-align the
    // page indicator if no explicit anchor token exists. For the MVP we
    // render headers + footers as a single line of text, no formatting.
    auto paintBand = [&](const QString& tmpl, qreal yTop, int page) {
        if (tmpl.isEmpty()) return;
        const QString line = substitutePrintMacros(
            tmpl, b, page + 1, totalPages);
        painter.save();
        painter.setPen(QColor(70, 70, 70));
        const QRectF rect(pageRect.left(), pageRect.top() + yTop,
                          pageW, lineH);
        painter.drawText(rect,
            Qt::AlignVCenter | Qt::AlignLeft | Qt::TextSingleLine, line);
        // Thin separator. Hairline above footer / below header.
        painter.setPen(QPen(QColor(140, 140, 140), 0.5));
        const qreal sepY = (yTop < pageH / 2.0)
            ? rect.bottom() + 2.0
            : rect.top()    - 2.0;
        painter.drawLine(QPointF(pageRect.left(),  pageRect.top() + sepY),
                         QPointF(pageRect.right(), pageRect.top() + sepY));
        painter.restore();
    };

    for (int p = 0; p < totalPages; ++p) {
        if (p > 0) printer->newPage();

        paintBand(headerTmpl, 0.0,                p);
        paintBand(footerTmpl, pageH - footerH,    p);

        painter.save();
        painter.translate(pageRect.left(), pageRect.top() + bodyTop);
        // drawContents takes a clip rect IN doc coords; offset the
        // viewport by `p * bodyH` to render that page's slice.
        const QRectF viewport(0, p * bodyH, pageW, bodyH);
        painter.translate(0, -p * bodyH);
        doc.drawContents(&painter, viewport);
        painter.restore();
    }

    painter.end();
}

// Pull the active buffer's text + render to the QPrinter. Three paths:
//   1. Phase 5T syntax-highlighted (Config::syntaxHighlightedPrint == true) —
//      routes through SCI_FORMATRANGEFULL via printSyntaxHighlighted.
//      Always handles header/footer + page macros internally.
//   2. Phase 9o plain-text with chrome (header or footer set) — manual
//      pagination via QPainter + QTextDocument::drawContents per slice.
//   3. Phase 5l fast plain-text path (chrome-less) — QTextDocument::print
//      single-pass auto-pagination.
//
// The Phase 5T path subsumes (2) when colour print is on; (2) remains for
// users who turn syntax highlighting OFF but still want a header/footer.
void printActiveBuffer(MainWindow* w, EditorTabs* tabs, QPrinter* printer)
{
    Buffer* b = tabs->currentBuffer();
    if (!b || !printer) return;

    printer->setDocName(b->displayName());

    const QString headerTmpl = Config::printHeader();
    const QString footerTmpl = Config::printFooter();

    if (Config::syntaxHighlightedPrint()) {
        // Phase 5T — syntax-highlighted via FORMATRANGEFULL. Handles its
        // own header/footer pagination using the same macro substitution
        // as the plain-text path.
        printSyntaxHighlighted(printer, b, headerTmpl, footerTmpl);
    } else {
        // Plain-text fall-back. Pull the text once and route to the
        // appropriate plain path.
        auto* ed = b->editor();
        const Scintilla::sptr_t length =
            ed->send(static_cast<unsigned int>(Message::GetTextLength));
        QString text;
        if (length > 0) {
            std::vector<char> buf(static_cast<std::size_t>(length) + 1, '\0');
            ed->send(static_cast<unsigned int>(Message::GetText),
                     static_cast<Scintilla::uptr_t>(buf.size()),
                     reinterpret_cast<Scintilla::sptr_t>(buf.data()));
            text = QString::fromUtf8(buf.data(), static_cast<int>(length));
        }

        if (headerTmpl.isEmpty() && footerTmpl.isEmpty()) {
            // Fast path — no header/footer; the simplest
            // QTextDocument::print path. Battle-tested since Phase 5l.
            QTextDocument doc;
            doc.setDefaultFont(
                QFontDatabase::systemFont(QFontDatabase::FixedFont));
            doc.setPlainText(text);
            doc.print(printer);
        } else {
            // Phase 9o — manual pagination so per-page header/footer +
            // PAGE_NUMBER / NB_PAGES macros work.
            printWithHeaderFooter(printer, b, text, headerTmpl, footerTmpl);
        }
    }

    w->statusBar()->showMessage(QObject::tr("Printed %1").arg(b->displayName()),
                                3000);
}

} // namespace

void MainWindow::onFilePrint()
{
    Buffer* b = activePane()->currentBuffer();
    if (!b) return;
    if (!m_lastPrinter) m_lastPrinter = new QPrinter(QPrinter::HighResolution);
    QPrintDialog dlg(m_lastPrinter, this);
    dlg.setWindowTitle(tr("Print %1").arg(b->displayName()));
    if (dlg.exec() != QDialog::Accepted) return;
    printActiveBuffer(this, activePane(),m_lastPrinter);
}

void MainWindow::onFilePrintNow()
{
    if (!activePane()->currentBuffer()) return;
    if (!m_lastPrinter) {
        // First print of the session — Print Now without prior settings
        // falls back to the dialog so the user picks at least once.
        onFilePrint();
        return;
    }
    printActiveBuffer(this, activePane(),m_lastPrinter);
}

// Phase 5AB — print preview wraps the same printActiveBuffer flow.
void MainWindow::onFilePrintPreview()
{
    Buffer* b = activePane()->currentBuffer();
    if (!b) return;
    if (!m_lastPrinter) m_lastPrinter = new QPrinter(QPrinter::HighResolution);
    QPrintPreviewDialog dlg(m_lastPrinter, this);
    dlg.setWindowTitle(tr("Print preview — %1").arg(b->displayName()));
    connect(&dlg, &QPrintPreviewDialog::paintRequested, this,
            [this](QPrinter* p) { printActiveBuffer(this, activePane(),p); });
    dlg.exec();
}

// Phase 5AB — Word count modal. Counts on selection if non-empty,
// else on the whole document. UTF-8 byte stream from Scintilla; we
// decode and count Unicode code points / grapheme-ish words.
void MainWindow::onToolsWordCount()
{
    Buffer* b = activePane()->currentBuffer();
    if (!b) return;
    auto* ed = b->editor();

    const Scintilla::sptr_t selStart =
        ed->send(static_cast<unsigned int>(Message::GetSelectionStart));
    const Scintilla::sptr_t selEnd =
        ed->send(static_cast<unsigned int>(Message::GetSelectionEnd));
    const bool useSelection = (selEnd > selStart);

    QString text;
    if (useSelection) {
        const Scintilla::sptr_t bytes = selEnd - selStart;
        std::vector<char> buf(static_cast<std::size_t>(bytes) + 1, '\0');
        ed->send(static_cast<unsigned int>(Message::GetSelText), 0,
                 reinterpret_cast<Scintilla::sptr_t>(buf.data()));
        text = QString::fromUtf8(buf.data(), static_cast<int>(bytes));
    } else {
        const Scintilla::sptr_t length =
            ed->send(static_cast<unsigned int>(Message::GetTextLength));
        std::vector<char> buf(static_cast<std::size_t>(length) + 1, '\0');
        ed->send(static_cast<unsigned int>(Message::GetText),
                 static_cast<Scintilla::uptr_t>(buf.size()),
                 reinterpret_cast<Scintilla::sptr_t>(buf.data()));
        text = QString::fromUtf8(buf.data(), static_cast<int>(length));
    }

    int chars = static_cast<int>(text.size());
    int charsNoSpace = 0;
    int lines = text.isEmpty() ? 0 : 1;
    int words = 0;
    bool inWord = false;
    for (QChar c : text) {
        if (c == QChar('\n')) ++lines;
        if (!c.isSpace()) ++charsNoSpace;
        const bool wordChar = c.isLetterOrNumber() || c == QChar('_');
        if (wordChar && !inWord) { ++words; inWord = true; }
        else if (!wordChar)        { inWord = false; }
    }

    QMessageBox::information(this, tr("Word Count"),
        tr("<table>"
           "<tr><th align=left>Scope</th><td>%1</td></tr>"
           "<tr><th align=left>Characters</th><td>%2</td></tr>"
           "<tr><th align=left>Characters (no spaces)</th><td>%3</td></tr>"
           "<tr><th align=left>Words</th><td>%4</td></tr>"
           "<tr><th align=left>Lines</th><td>%5</td></tr>"
           "</table>")
        .arg(useSelection ? tr("Selection") : tr("Whole document"))
        .arg(chars).arg(charsNoSpace).arg(words).arg(lines));
}

// Phase 9d.1 — Tools → Summary. Read-only modal extending Word Count
// with file metadata. Always operates on the whole document — selection
// scoping wouldn't change file size / mtime / encoding / EOL / language.
void MainWindow::onToolsSummary()
{
    Buffer* b = activePane()->currentBuffer();
    if (!b) return;
    auto* ed = b->editor();

    // ---- Whole-document text → counts (mirrors onToolsWordCount) ----
    const Scintilla::sptr_t length =
        ed->send(static_cast<unsigned int>(Message::GetTextLength));
    std::vector<char> buf(static_cast<std::size_t>(length) + 1, '\0');
    ed->send(static_cast<unsigned int>(Message::GetText),
             static_cast<Scintilla::uptr_t>(buf.size()),
             reinterpret_cast<Scintilla::sptr_t>(buf.data()));
    const QString text = QString::fromUtf8(buf.data(), static_cast<int>(length));

    int chars = static_cast<int>(text.size());
    int charsNoSpace = 0;
    int lines = text.isEmpty() ? 0 : 1;
    int words = 0;
    bool inWord = false;
    for (QChar c : text) {
        if (c == QChar('\n')) ++lines;
        if (!c.isSpace()) ++charsNoSpace;
        const bool wordChar = c.isLetterOrNumber() || c == QChar('_');
        if (wordChar && !inWord) { ++words; inWord = true; }
        else if (!wordChar)        { inWord = false; }
    }

    // ---- File metadata --------------------------------------------------
    QString fileSize = tr("(not yet on disk)");
    QString modified = tr("(not yet on disk)");
    QString fullPath = tr("(untitled)");
    if (b->hasFile()) {
        fullPath = b->filePath();
        const QFileInfo fi(b->filePath());
        if (fi.exists()) {
            const qint64 sz = fi.size();
            // Show both the byte count and a human-readable form for files
            // bigger than 1 KiB.
            if (sz < 1024) {
                fileSize = tr("%1 bytes").arg(sz);
            } else if (sz < 1024 * 1024) {
                fileSize = tr("%1 bytes (%2 KiB)")
                    .arg(sz).arg(QString::number(sz / 1024.0, 'f', 1));
            } else {
                fileSize = tr("%1 bytes (%2 MiB)")
                    .arg(sz).arg(QString::number(sz / 1048576.0, 'f', 2));
            }
            modified = fi.lastModified()
                         .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
        }
    }

    const EncodingInfo enc = b->encoding();
    QString eolName;
    switch (b->eolMode()) {
        case Buffer::EolMode::Crlf: eolName = tr("Windows (CR LF)"); break;
        case Buffer::EolMode::Lf:   eolName = tr("Unix (LF)");       break;
        case Buffer::EolMode::Cr:   eolName = tr("Macintosh (CR)");  break;
    }
    const LanguageDef* lang = b->language();
    const QString langName = lang ? lang->displayName : tr("Plain text");

    QMessageBox::information(this, tr("Summary"),
        tr("<table>"
           "<tr><th align=left>Path</th><td>%1</td></tr>"
           "<tr><th align=left>Size</th><td>%2</td></tr>"
           "<tr><th align=left>Modified</th><td>%3</td></tr>"
           "<tr><th align=left>Encoding</th><td>%4</td></tr>"
           "<tr><th align=left>EOL</th><td>%5</td></tr>"
           "<tr><th align=left>Language</th><td>%6</td></tr>"
           "<tr><th align=left>Read-only</th><td>%7</td></tr>"
           "<tr><td>&nbsp;</td><td>&nbsp;</td></tr>"
           "<tr><th align=left>Characters</th><td>%8</td></tr>"
           "<tr><th align=left>Characters (no spaces)</th><td>%9</td></tr>"
           "<tr><th align=left>Words</th><td>%10</td></tr>"
           "<tr><th align=left>Lines</th><td>%11</td></tr>"
           "</table>")
        .arg(fullPath, fileSize, modified,
             Encoding::displayName(enc), eolName, langName,
             b->isReadOnly() ? tr("yes") : tr("no"))
        .arg(chars).arg(charsNoSpace).arg(words).arg(lines));
}

// ---- Edit delegates ----
namespace { Scintilla::sptr_t sciSend(Buffer* b, Message m) {
    return b ? b->editor()->send(static_cast<unsigned int>(m)) : 0;
}}
void MainWindow::onEditUndo()       { sciSend(activePane()->currentBuffer(), Message::Undo); }
void MainWindow::onEditRedo()       { sciSend(activePane()->currentBuffer(), Message::Redo); }
void MainWindow::onEditCut()        { sciSend(activePane()->currentBuffer(), Message::Cut); }
void MainWindow::onEditCopy()       { sciSend(activePane()->currentBuffer(), Message::Copy); }
void MainWindow::onEditPaste()      { sciSend(activePane()->currentBuffer(), Message::Paste); }
void MainWindow::onEditDelete()     { sciSend(activePane()->currentBuffer(), Message::Clear); }
void MainWindow::onEditSelectAll()  { sciSend(activePane()->currentBuffer(), Message::SelectAll); }
void MainWindow::onEditUpperCase()  { sciSend(activePane()->currentBuffer(), Message::UpperCase); }
void MainWindow::onEditLowerCase()  { sciSend(activePane()->currentBuffer(), Message::LowerCase); }

// Phase 9r — Convert Case To submenu fillers. Scintilla doesn't have
// built-in messages for Proper / Invert / Random; we pull the
// selection, transform, and SCI_REPLACESEL the result. Selection-less
// invocations no-op (matching Upper/Lower's behaviour).
namespace {
void replaceSelectionTransformed(Buffer* b,
    QString (*xform)(const QString&))
{
    if (!b) return;
    auto* ed = b->editor();
    const Scintilla::sptr_t s = ed->send(static_cast<unsigned int>(Message::GetSelectionStart));
    const Scintilla::sptr_t e = ed->send(static_cast<unsigned int>(Message::GetSelectionEnd));
    if (s == e) return;
    const Scintilla::sptr_t len = e - s;
    std::vector<char> buf(static_cast<std::size_t>(len) + 1, '\0');
    Sci_TextRangeFull tr;
    tr.chrg.cpMin = s;
    tr.chrg.cpMax = e;
    tr.lpstrText  = buf.data();
    ed->send(static_cast<unsigned int>(Message::GetTextRangeFull),
             0, reinterpret_cast<Scintilla::sptr_t>(&tr));
    const QString src = QString::fromUtf8(buf.data(), static_cast<int>(len));
    const QByteArray dst = xform(src).toUtf8();
    ed->sends(static_cast<unsigned int>(Message::ReplaceSel),
              static_cast<Scintilla::uptr_t>(dst.size()), dst.constData());
}
} // namespace

void MainWindow::onEditProperCase()
{
    replaceSelectionTransformed(activePane()->currentBuffer(),
        [](const QString& src) {
            QString out;
            out.reserve(src.size());
            bool wordStart = true;
            for (const QChar c : src) {
                out.append(wordStart ? c.toUpper() : c.toLower());
                wordStart = !c.isLetter();
            }
            return out;
        });
}

void MainWindow::onEditInvertCase()
{
    replaceSelectionTransformed(activePane()->currentBuffer(),
        [](const QString& src) {
            QString out;
            out.reserve(src.size());
            for (const QChar c : src) {
                out.append(c.isUpper() ? c.toLower()
                          : c.isLower() ? c.toUpper() : c);
            }
            return out;
        });
}

void MainWindow::onEditRandomCase()
{
    replaceSelectionTransformed(activePane()->currentBuffer(),
        [](const QString& src) {
            QString out;
            out.reserve(src.size());
            unsigned int seed = static_cast<unsigned int>(
                std::hash<std::string>{}(src.toStdString()));
            for (const QChar c : src) {
                seed = seed * 1103515245u + 12345u;
                if (!c.isLetter())          out.append(c);
                else if ((seed >> 16) & 1)  out.append(c.toUpper());
                else                        out.append(c.toLower());
            }
            return out;
        });
}

void MainWindow::onSearchFindInFiles()
{
    findReplace()->showFindInFilesTab(currentSelectionText());
}

void MainWindow::onSearchMark()
{
    findReplace()->showMarkTab(currentSelectionText());
}

// ---- Search ----
void MainWindow::onSearchFind()
{
    findReplace()->showFindTab(currentSelectionText());
}

void MainWindow::onSearchReplace()
{
    findReplace()->showReplaceTab(currentSelectionText());
}

void MainWindow::onSearchFindNext()
{
    auto* fr = findReplace();
    if (!fr->hasLastSearch()) {
        // No prior search — open the dialog and let the user enter one.
        fr->showFindTab(currentSelectionText());
        return;
    }
    fr->findNextStandalone(true);
}

void MainWindow::onSearchFindPrevious()
{
    auto* fr = findReplace();
    if (!fr->hasLastSearch()) {
        fr->showFindTab(currentSelectionText());
        return;
    }
    fr->findNextStandalone(false);
}

// ---- View ----
void MainWindow::onViewToggleWordWrap()
{
    Buffer* b = activePane()->currentBuffer(); if (!b) return;
    auto* ed = b->editor();
    const Scintilla::sptr_t cur =
        ed->send(static_cast<unsigned int>(Message::GetWrapMode));
    ed->send(static_cast<unsigned int>(Message::SetWrapMode),
             cur == SC_WRAP_NONE ? SC_WRAP_WORD : SC_WRAP_NONE);
}

void MainWindow::onViewToggleLineNumbers()
{
    Buffer* b = activePane()->currentBuffer(); if (!b) return;
    auto* ed = b->editor();
    const Scintilla::sptr_t cur =
        ed->send(static_cast<unsigned int>(Message::GetMarginWidthN), 0);
    ed->send(static_cast<unsigned int>(Message::SetMarginWidthN), 0,
             cur == 0 ? 36 : 0);
}

void MainWindow::onViewZoomIn()
{
    if (auto* b = activePane()->currentBuffer())
        b->editor()->send(static_cast<unsigned int>(Message::ZoomIn));
}

void MainWindow::onViewZoomOut()
{
    if (auto* b = activePane()->currentBuffer())
        b->editor()->send(static_cast<unsigned int>(Message::ZoomOut));
}

void MainWindow::onViewZoomReset()
{
    if (auto* b = activePane()->currentBuffer())
        b->editor()->send(static_cast<unsigned int>(Message::SetZoom), 0, 0);
}

// ---- Help ----
void MainWindow::onHelpAbout()
{
    QMessageBox::about(this, tr("About padnote--"),
        tr("<h3>padnote--</h3>"
           "<p>Version %1</p>"
           "<p>Qt 6 text editor for Linux.</p>"
           "<p>See <code>LICENSE</code> and <code>LICENSE_PORT.md</code> "
           "for licensing.</p>")
            .arg(QStringLiteral(PADNOTE_VERSION_STRING)));
}

void MainWindow::onHelpOnlineDocs()
{
    // No online docs site yet — open the in-tree README instead.
    QDesktopServices::openUrl(QUrl::fromLocalFile(
        QCoreApplication::applicationDirPath() + QStringLiteral("/../README.md")));
}

void MainWindow::onHelpForum()
{
    // No forum yet — placeholder; route to local README.
    QDesktopServices::openUrl(QUrl::fromLocalFile(
        QCoreApplication::applicationDirPath() + QStringLiteral("/../README.md")));
}

void MainWindow::onHelpDebugInfo()
{
    const QString compiler =
#if defined(__clang__)
        QStringLiteral("Clang ") + QStringLiteral(__clang_version__);
#elif defined(__GNUC__)
        QStringLiteral("GCC %1.%2.%3")
            .arg(__GNUC__).arg(__GNUC_MINOR__).arg(__GNUC_PATCHLEVEL__);
#else
        QStringLiteral("(unknown compiler)");
#endif

    const QString info = QStringLiteral(
        "padnote-- %1\n"
        "Qt runtime:       %2\n"
        "Qt compile-time:  %3\n"
        "Compiler:         %4\n"
        "Build date:       %5 %6\n"
        "Config dir:       %7\n"
        "Languages:        %8\n"
        "Theme:            %9\n"
        "Qt platform:      %10\n")
        .arg(QStringLiteral(PADNOTE_VERSION_STRING))
        .arg(QString::fromLatin1(qVersion()))
        .arg(QStringLiteral(QT_VERSION_STR))
        .arg(compiler)
        .arg(QStringLiteral(__DATE__))
        .arg(QStringLiteral(__TIME__))
        .arg(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation))
        .arg(Languages::count())
        .arg(Theme::mode() == Theme::Mode::Dark ? tr("Dark") : tr("Light"))
        .arg(QGuiApplication::platformName());

    QDialog dlg(this);
    dlg.setWindowTitle(tr("Debug Info"));
    dlg.resize(640, 360);
    auto* root = new QVBoxLayout(&dlg);
    auto* edit = new QPlainTextEdit(&dlg);
    edit->setReadOnly(true);
    edit->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    edit->setPlainText(info);
    root->addWidget(edit, 1);

    auto* row = new QHBoxLayout;
    auto* aCopy  = new QPushButton(tr("&Copy to Clipboard"), &dlg);
    auto* aClose = new QPushButton(tr("Close"), &dlg);
    aClose->setDefault(true);
    row->addStretch(1);
    row->addWidget(aCopy);
    row->addWidget(aClose);
    root->addLayout(row);

    // info outlives the lambda — dlg.exec() blocks until close.
    connect(aCopy, &QPushButton::clicked, &dlg, [this, &info]{
        QGuiApplication::clipboard()->setText(info);
        statusBar()->showMessage(tr("Debug info copied to clipboard."), 3000);
    });
    connect(aClose, &QPushButton::clicked, &dlg, &QDialog::accept);

    dlg.exec();
}

// -----------------------------------------------------------------------------
// Phase 5P — Style Configurator
// -----------------------------------------------------------------------------

void MainWindow::onSettingsStyleConfigurator()
{
    StyleConfigDialog dlg(this, this);
    dlg.exec();
}

// -----------------------------------------------------------------------------
// Phase 5Q — Shortcut Mapper
// -----------------------------------------------------------------------------

void MainWindow::onSettingsShortcutMapper()
{
    ShortcutMapperDialog dlg(menuBar(), this);
    dlg.exec();
}

// -----------------------------------------------------------------------------
// Phase 5S — System tray
// -----------------------------------------------------------------------------

void MainWindow::onSettingsMinimizeToTray()
{
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        statusBar()->showMessage(
            tr("System tray unavailable on this desktop."), 4000);
        if (m_actMinimizeToTray) m_actMinimizeToTray->setChecked(false);
        return;
    }

    if (!m_tray) {
        // Lazy-construct on first toggle so we don't add an icon to trays
        // the user doesn't actually want it in.
        m_tray = new QSystemTrayIcon(
            style()->standardIcon(QStyle::SP_FileIcon), this);
        m_tray->setToolTip(QStringLiteral("padnote--"));

        auto* menu = new QMenu(this);
        auto* aShow = menu->addAction(tr("Show / Hide"));
        connect(aShow, &QAction::triggered, this, [this]{
            if (isVisible()) {
                hide();
            } else {
                show();
                raise();
                activateWindow();
            }
        });
        menu->addSeparator();
        auto* aQuit = menu->addAction(tr("&Quit"));
        connect(aQuit, &QAction::triggered, this, &QMainWindow::close);
        m_tray->setContextMenu(menu);

        connect(m_tray, &QSystemTrayIcon::activated,
                this, &MainWindow::onTrayActivated);
    }

    if (m_actMinimizeToTray && m_actMinimizeToTray->isChecked()) {
        m_tray->show();
        statusBar()->showMessage(
            tr("Minimize-to-tray ON — closing the window will hide to tray."),
            4000);
    } else {
        m_tray->hide();
        statusBar()->showMessage(tr("Minimize-to-tray OFF."), 3000);
    }
}

void MainWindow::onTrayActivated(QSystemTrayIcon::ActivationReason reason)
{
    // Single-click / double-click toggles visibility. Right-click shows
    // the context menu Qt installed automatically.
    if (reason == QSystemTrayIcon::Trigger
     || reason == QSystemTrayIcon::DoubleClick) {
        if (isVisible()) {
            hide();
        } else {
            show();
            raise();
            activateWindow();
        }
    }
}

// -----------------------------------------------------------------------------
// Phase 5T — File watcher
// -----------------------------------------------------------------------------

void MainWindow::onBufferDisplayNameChanged(Buffer* /*buffer*/)
{
    syncFileWatcher();
}

void MainWindow::syncFileWatcher()
{
    if (!m_fileWatcher) return;
    // Phase 5N.11 — when the user disabled the watcher in Preferences,
    // skip the add/remove dance entirely. applyFileWatcherPrefsFromConfig
    // already cleared the path set on the off-toggle.
    if (!Config::fileWatcherEnabled()) return;

    // Build the desired set of paths from buffers in BOTH panes (Phase 3d).
    QStringList desired;
    for (Buffer* b : allBuffers()) {
        if (b && b->hasFile()) desired << b->filePath();
    }

    const QStringList current = m_fileWatcher->files();
    QStringList toRemove;
    for (const QString& p : current) {
        if (!desired.contains(p)) toRemove << p;
    }
    QStringList toAdd;
    for (const QString& p : desired) {
        if (!current.contains(p)) toAdd << p;
    }
    if (!toRemove.isEmpty()) m_fileWatcher->removePaths(toRemove);
    if (!toAdd.isEmpty())    m_fileWatcher->addPaths(toAdd);
}

void MainWindow::onWatchedFileChanged(const QString& path)
{
    // Locate the Buffer that owns this path. Phase 3d — search both panes.
    Buffer* target = nullptr;
    for (Buffer* b : allBuffers()) {
        if (b && b->filePath() == path) { target = b; break; }
    }
    if (!target) return;

    const QFileInfo fi(path);
    const qint64 nowMtime = fi.exists()
        ? fi.lastModified().toMSecsSinceEpoch()
        : 0;

    // Filter out our own writes: saveToFile / loadFromFile call refreshMtime
    // synchronously, so by the time this slot fires the buffer's last-known
    // mtime already matches what's on disk.
    if (nowMtime == target->lastKnownMtime()) return;

    if (!fi.exists()) {
        statusBar()->showMessage(
            tr("'%1' was deleted on disk.").arg(target->displayName()), 6000);
        target->refreshMtime();   // accept the new state (mtime = 0)
        return;
    }

    if (target->isDirty()) {
        // Local edits AND external write — don't silently clobber either.
        // Just warn; the user decides whether to save (overwrites) or
        // reload (loses local edits) themselves via the menus.
        statusBar()->showMessage(
            tr("'%1' changed on disk; you have unsaved edits — Save will overwrite, Reload will discard.")
                .arg(target->displayName()),
            8000);
        target->refreshMtime();   // suppress repeat nags on the same change
        return;
    }

    // Clean buffer + external change → offer to reload.
    const auto reply = QMessageBox::question(this, tr("File changed"),
        tr("'%1' changed on disk. Reload?").arg(target->displayName()),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
    if (reply == QMessageBox::Yes) {
        // Preserve cursor across the reload (matches the manual Reload
        // from Disk flow from Phase 5d).
        auto* ed = target->editor();
        const Scintilla::sptr_t pos = ed->send(
            static_cast<unsigned int>(Message::GetCurrentPos));
        const Scintilla::sptr_t firstVis = ed->send(
            static_cast<unsigned int>(Message::GetFirstVisibleLine));
        QString err;
        if (target->loadFromFile(path, &err)) {
            const Scintilla::sptr_t length = ed->send(
                static_cast<unsigned int>(Message::GetLength));
            const Scintilla::sptr_t safePos = pos < length ? pos : length;
            ed->send(static_cast<unsigned int>(Message::GotoPos),
                     static_cast<Scintilla::uptr_t>(safePos));
            ed->send(static_cast<unsigned int>(Message::SetFirstVisibleLine),
                     static_cast<Scintilla::uptr_t>(firstVis));
            statusBar()->showMessage(
                tr("Reloaded %1 (external change).").arg(target->displayName()),
                4000);
        } else {
            QMessageBox::warning(this, tr("Reload failed"),
                tr("Could not re-read %1:\n%2").arg(path, err));
        }
    } else {
        // User declined — accept the new on-disk state so we don't keep
        // re-prompting on the same change.
        target->refreshMtime();
    }

    // Some editors (vim with `:set backupcopy=no`) replace the file on
    // save instead of writing in place. inotify in that case removes
    // the watch as soon as the original inode goes. Re-add to be safe.
    if (fi.exists() && !m_fileWatcher->files().contains(path)) {
        m_fileWatcher->addPath(path);
    }
}

// -----------------------------------------------------------------------------
// Phase 5W — Edit polish (Begin/End Select + Column Mode + Column Editor)
// -----------------------------------------------------------------------------

namespace {

// Map menu actions to a small per-MainWindow stash so Begin Select's
// anchor pos persists between user clicks. Keyed by MainWindow pointer.
QHash<MainWindow*, Scintilla::sptr_t>& beginSelectAnchors()
{
    static QHash<MainWindow*, Scintilla::sptr_t> g;
    return g;
}

} // namespace

void MainWindow::onEditBeginSelect()
{
    Buffer* b = activePane()->currentBuffer();
    if (!b) return;
    auto* ed = b->editor();
    const Scintilla::sptr_t pos =
        ed->send(static_cast<unsigned int>(Message::GetCurrentPos));
    beginSelectAnchors()[this] = pos;
    statusBar()->showMessage(
        tr("Begin Select set at line %1. Move caret then choose End Select.")
            .arg(ed->send(static_cast<unsigned int>(Message::LineFromPosition),
                 static_cast<Scintilla::uptr_t>(pos)) + 1),
        4000);
}

void MainWindow::onEditEndSelect()
{
    Buffer* b = activePane()->currentBuffer();
    if (!b) return;
    if (!beginSelectAnchors().contains(this)) {
        statusBar()->showMessage(
            tr("End Select: no Begin Select recorded. Choose Begin Select first."),
            4000);
        return;
    }
    auto* ed = b->editor();
    const Scintilla::sptr_t anchor = beginSelectAnchors().value(this);
    const Scintilla::sptr_t caret =
        ed->send(static_cast<unsigned int>(Message::GetCurrentPos));

    // Switch to rectangular selection mode and set the anchor + caret.
    ed->send(static_cast<unsigned int>(Message::SetSelectionMode),
             SC_SEL_RECTANGLE, 0);
    ed->send(static_cast<unsigned int>(Message::SetSel),
             static_cast<Scintilla::uptr_t>(anchor),
             static_cast<Scintilla::sptr_t>(caret));
    beginSelectAnchors().remove(this);
}

void MainWindow::onEditColumnMode()
{
    Buffer* b = activePane()->currentBuffer();
    if (!b) return;
    auto* ed = b->editor();
    const Scintilla::sptr_t mode =
        ed->send(static_cast<unsigned int>(Message::GetSelectionMode));
    const bool wasRect = (mode == SC_SEL_RECTANGLE);
    ed->send(static_cast<unsigned int>(Message::SetSelectionMode),
             wasRect ? SC_SEL_STREAM : SC_SEL_RECTANGLE, 0);
    statusBar()->showMessage(
        wasRect ? tr("Column Mode: OFF (stream selection)")
                : tr("Column Mode: ON (rectangular selection — Alt+drag also works)"),
        3500);
}

void MainWindow::onEditColumnEditor()
{
    ColumnEditorDialog dlg(activePane(), this);
    dlg.exec();
}

// Phase 5Y — Sublime/VSCode-style Ctrl+D behaviour: take the current
// selection (or the word under caret if no selection), find the next
// occurrence in the buffer, add it as an additional selection.
void MainWindow::onEditAddNextOccurrence()
{
    Buffer* b = activePane()->currentBuffer();
    if (!b) return;
    auto* ed = b->editor();

    Scintilla::sptr_t selStart =
        ed->send(static_cast<unsigned int>(Message::GetSelectionStart));
    Scintilla::sptr_t selEnd =
        ed->send(static_cast<unsigned int>(Message::GetSelectionEnd));
    if (selStart == selEnd) {
        // No selection — auto-select the word under caret first.
        const Scintilla::sptr_t pos =
            ed->send(static_cast<unsigned int>(Message::GetCurrentPos));
        selStart = ed->send(static_cast<unsigned int>(Message::WordStartPosition),
                            static_cast<Scintilla::uptr_t>(pos), true);
        selEnd   = ed->send(static_cast<unsigned int>(Message::WordEndPosition),
                            static_cast<Scintilla::uptr_t>(pos), true);
        if (selStart == selEnd) {
            statusBar()->showMessage(
                tr("Add cursor: no word at caret."), 3000);
            return;
        }
        ed->send(static_cast<unsigned int>(Message::SetSel),
                 static_cast<Scintilla::uptr_t>(selStart),
                 static_cast<Scintilla::sptr_t>(selEnd));
        return;   // first invocation = "select the word"; user invokes again
                  // to actually add the second cursor. Matches VSCode.
    }

    const Scintilla::sptr_t bytes = selEnd - selStart;
    std::vector<char> buf(static_cast<std::size_t>(bytes) + 1, '\0');
    ed->send(static_cast<unsigned int>(Message::GetSelText), 0,
             reinterpret_cast<Scintilla::sptr_t>(buf.data()));

    // Search for next occurrence after the current selection. Wrap-
    // around to start of doc on miss.
    const Scintilla::sptr_t docEnd =
        ed->send(static_cast<unsigned int>(Message::GetLength));
    ed->send(static_cast<unsigned int>(Message::SetSearchFlags),
             SCFIND_MATCHCASE, 0);

    auto findNextFrom = [&](Scintilla::sptr_t fromPos) -> Scintilla::sptr_t {
        ed->send(static_cast<unsigned int>(Message::SetTargetRange),
                 static_cast<Scintilla::uptr_t>(fromPos),
                 static_cast<Scintilla::sptr_t>(docEnd));
        return ed->sends(
            static_cast<unsigned int>(Message::SearchInTarget),
            static_cast<Scintilla::uptr_t>(bytes),
            buf.data());
    };

    Scintilla::sptr_t found = findNextFrom(selEnd);
    if (found < 0) found = findNextFrom(0);   // wrap
    if (found < 0 || found == selStart) {
        statusBar()->showMessage(
            tr("Add cursor: no further occurrences."), 3000);
        return;
    }
    const Scintilla::sptr_t targetEnd = ed->send(
        static_cast<unsigned int>(Message::GetTargetEnd));
    ed->send(static_cast<unsigned int>(Message::AddSelection),
             static_cast<Scintilla::uptr_t>(found),
             static_cast<Scintilla::sptr_t>(targetEnd));
    ed->send(static_cast<unsigned int>(Message::ScrollCaret));
}

// Phase 9c.3 — Sublime/VSCode Ctrl+Alt+Enter: sweep the whole buffer for
// the current selection (or word under caret) and add a caret at every
// occurrence in one go. Each match becomes an additional selection on
// top of the original, leaving the user with N cursors ready to type at.
void MainWindow::onEditAddAllOccurrences()
{
    Buffer* b = activePane()->currentBuffer();
    if (!b) return;
    auto* ed = b->editor();

    Scintilla::sptr_t selStart =
        ed->send(static_cast<unsigned int>(Message::GetSelectionStart));
    Scintilla::sptr_t selEnd =
        ed->send(static_cast<unsigned int>(Message::GetSelectionEnd));
    if (selStart == selEnd) {
        // No selection — auto-pick the word under caret as the needle.
        const Scintilla::sptr_t pos =
            ed->send(static_cast<unsigned int>(Message::GetCurrentPos));
        selStart = ed->send(static_cast<unsigned int>(Message::WordStartPosition),
                            static_cast<Scintilla::uptr_t>(pos), true);
        selEnd   = ed->send(static_cast<unsigned int>(Message::WordEndPosition),
                            static_cast<Scintilla::uptr_t>(pos), true);
        if (selStart == selEnd) {
            statusBar()->showMessage(
                tr("Add cursors at all matches: no word at caret."), 3000);
            return;
        }
    }

    const Scintilla::sptr_t bytes = selEnd - selStart;
    std::vector<char> buf(static_cast<std::size_t>(bytes) + 1, '\0');
    ed->send(static_cast<unsigned int>(Message::GetSelText), 0,
             reinterpret_cast<Scintilla::sptr_t>(buf.data()));

    const Scintilla::sptr_t docEnd =
        ed->send(static_cast<unsigned int>(Message::GetLength));
    ed->send(static_cast<unsigned int>(Message::SetSearchFlags),
             SCFIND_MATCHCASE, 0);

    // Reset to a single primary selection at the original range; subsequent
    // AddSelection calls layer on top. Walking the document forward
    // accumulates every match.
    ed->send(static_cast<unsigned int>(Message::SetSelection),
             static_cast<Scintilla::uptr_t>(selStart),
             static_cast<Scintilla::sptr_t>(selEnd));

    Scintilla::sptr_t cursor = 0;
    int matches = 0;
    constexpr int kCap = 5000;   // safety: huge buffers w/ common needles
    while (cursor < docEnd && matches < kCap) {
        ed->send(static_cast<unsigned int>(Message::SetTargetRange),
                 static_cast<Scintilla::uptr_t>(cursor),
                 static_cast<Scintilla::sptr_t>(docEnd));
        const Scintilla::sptr_t found = ed->sends(
            static_cast<unsigned int>(Message::SearchInTarget),
            static_cast<Scintilla::uptr_t>(bytes),
            buf.data());
        if (found < 0) break;
        const Scintilla::sptr_t mEnd = ed->send(
            static_cast<unsigned int>(Message::GetTargetEnd));
        // Skip the original selection — it's already the primary cursor.
        if (found != selStart) {
            ed->send(static_cast<unsigned int>(Message::AddSelection),
                     static_cast<Scintilla::uptr_t>(found),
                     static_cast<Scintilla::sptr_t>(mEnd));
        }
        ++matches;
        cursor = (mEnd > found) ? mEnd : found + 1;
    }
    ed->send(static_cast<unsigned int>(Message::ScrollCaret));
    statusBar()->showMessage(
        tr("Added cursors at %1 match(es).").arg(matches), 3000);
}

// Phase 9d.2 — Hide Lines / Show All Lines.
//
// SCI_HIDELINES(start, end) collapses a line range in the editor without
// using fold markers — useful when the user wants to focus on a region
// without the language having a fold structure (or to hide noise lines).
// The hidden lines aren't deleted; SCI_SHOWLINES restores them.
//
// We hide whatever the current selection covers (line-aligned). With no
// selection, the current line only is hidden. Show-All sweeps every
// line in the document.
void MainWindow::onEditHideLines()
{
    Buffer* b = activePane()->currentBuffer();
    if (!b) return;
    auto* ed = b->editor();
    const Scintilla::sptr_t selStart =
        ed->send(static_cast<unsigned int>(Message::GetSelectionStart));
    const Scintilla::sptr_t selEnd =
        ed->send(static_cast<unsigned int>(Message::GetSelectionEnd));
    Scintilla::sptr_t lineStart =
        ed->send(static_cast<unsigned int>(Message::LineFromPosition), selStart);
    Scintilla::sptr_t lineEnd =
        ed->send(static_cast<unsigned int>(Message::LineFromPosition), selEnd);
    // Empty selection that ends at column 0 of line N would otherwise
    // extend the hidden range one line past the visible selection. Trim.
    if (selEnd > selStart && lineEnd > lineStart) {
        const Scintilla::sptr_t lineEndStart =
            ed->send(static_cast<unsigned int>(Message::PositionFromLine), lineEnd);
        if (selEnd == lineEndStart) --lineEnd;
    }
    // Don't hide line 0 — Scintilla won't render the buffer at all if every
    // line is hidden, and the user has no way to recover without keyboard.
    if (lineStart == 0) lineStart = 1;
    if (lineEnd < lineStart) {
        statusBar()->showMessage(
            tr("Hide Lines: cannot hide the first line."), 3000);
        return;
    }
    ed->send(static_cast<unsigned int>(Message::HideLines),
             static_cast<Scintilla::uptr_t>(lineStart),
             static_cast<Scintilla::sptr_t>(lineEnd));
    const int n = static_cast<int>(lineEnd - lineStart + 1);
    statusBar()->showMessage(
        tr("Hid %1 line(s). Use Edit → Show All Lines to restore.").arg(n),
        4000);
}

void MainWindow::onEditShowAllLines()
{
    Buffer* b = activePane()->currentBuffer();
    if (!b) return;
    auto* ed = b->editor();
    const Scintilla::sptr_t lineCount =
        ed->send(static_cast<unsigned int>(Message::GetLineCount));
    if (lineCount <= 1) return;
    ed->send(static_cast<unsigned int>(Message::ShowLines),
             0,
             static_cast<Scintilla::sptr_t>(lineCount - 1));
    statusBar()->showMessage(tr("All lines shown."), 2500);
}

// -----------------------------------------------------------------------------
// Phase 5X — Smart-edit pack (autocomplete + smart-highlight toggle)
// -----------------------------------------------------------------------------

void MainWindow::onEditAutocomplete()
{
    // Phase 9i — the heavy lifting (collect words + lexer keywords +
    // SCI_AUTOCSHOW) lives in Buffer::triggerAutocomplete now so the
    // auto-trigger path in Buffer::onCharAdded shares it. This thin
    // wrapper just translates the bool result into a status message
    // for the manual Ctrl+Space path.
    Buffer* b = activePane()->currentBuffer();
    if (!b) return;
    auto* ed = b->editor();
    const Scintilla::sptr_t pos =
        ed->send(static_cast<unsigned int>(Message::GetCurrentPos));
    const Scintilla::sptr_t wordStart =
        ed->send(static_cast<unsigned int>(Message::WordStartPosition),
                 static_cast<Scintilla::uptr_t>(pos), true);
    if (pos == wordStart) {
        statusBar()->showMessage(
            tr("Autocomplete: type at least one character first."), 3000);
        return;
    }
    if (!b->triggerAutocomplete(/*manualTrigger=*/true)) {
        statusBar()->showMessage(tr("Autocomplete: no candidates."), 3000);
    }
}

void MainWindow::onViewToggleSmartHighlight()
{
    const bool on = m_actSmartHighlight && m_actSmartHighlight->isChecked();
    Config::setSmartHighlightEnabled(on);
    Config::save();
    // If turning OFF, clear the indicator on every buffer immediately
    // across both panes (Phase 3d).
    if (!on) {
        for (Buffer* b : allBuffers()) {
            if (!b) continue;
            auto* ed = b->editor();
            const Scintilla::sptr_t end =
                ed->send(static_cast<unsigned int>(Message::GetLength));
            ed->send(static_cast<unsigned int>(Message::SetIndicatorCurrent), 9, 0);
            ed->send(static_cast<unsigned int>(Message::IndicatorClearRange), 0, end);
        }
    }
    statusBar()->showMessage(
        on ? tr("Highlight all matches: ON") : tr("Highlight all matches: OFF"),
        2500);
}

// -----------------------------------------------------------------------------
// Phase 5V — View polish (Always on Top / Full Screen / Distraction Free / Post-It)
// -----------------------------------------------------------------------------

void MainWindow::onViewAlwaysOnTop()
{
    m_alwaysOnTop = !m_alwaysOnTop;
    setWindowFlag(Qt::WindowStaysOnTopHint, m_alwaysOnTop);
    if (m_actAlwaysOnTop) m_actAlwaysOnTop->setChecked(m_alwaysOnTop);
    show();   // setWindowFlag hides the widget on X11 — bring it back
    statusBar()->showMessage(
        m_alwaysOnTop ? tr("Always on Top: ON") : tr("Always on Top: OFF"),
        2500);
}

void MainWindow::onViewToggleFullScreen()
{
    if (isFullScreen()) {
        showNormal();
        if (m_actFullScreen) m_actFullScreen->setChecked(false);
    } else {
        showFullScreen();
        if (m_actFullScreen) m_actFullScreen->setChecked(true);
    }
}

void MainWindow::onViewDistractionFree()
{
    if (!m_distractionFree) {
        // Snapshot current chrome visibility so toggling back restores it
        // even if the user had already hidden the toolbar via a future
        // Preferences-pane toggle (v2 placeholder).
        m_preDistractGeom         = geometry();
        m_preDistractMenuVis      = menuBar()->isVisible();
        m_preDistractStatusVis    = statusBar()->isVisible();
        QToolBar* tb = findChild<QToolBar*>();
        m_preDistractToolbarVis   = tb && tb->isVisible();
        menuBar()->setVisible(false);
        statusBar()->setVisible(false);
        for (auto* t : findChildren<QToolBar*>()) t->setVisible(false);
        showFullScreen();
        m_distractionFree = true;
    } else {
        menuBar()->setVisible(m_preDistractMenuVis);
        statusBar()->setVisible(m_preDistractStatusVis);
        for (auto* t : findChildren<QToolBar*>())
            t->setVisible(m_preDistractToolbarVis);
        showNormal();
        if (m_preDistractGeom.isValid()) setGeometry(m_preDistractGeom);
        m_distractionFree = false;
    }
    if (m_actDistractionFree) m_actDistractionFree->setChecked(m_distractionFree);
}

void MainWindow::onViewPostIt()
{
    if (!m_postIt) {
        // Save geometry, hide chrome, shrink to a small floating editor on
        // top of everything else. Frame stays so the user can drag it.
        m_prePostItGeom = geometry();
        menuBar()->setVisible(false);
        for (auto* t : findChildren<QToolBar*>()) t->setVisible(false);
        statusBar()->setVisible(false);
        setWindowFlag(Qt::WindowStaysOnTopHint, true);
        show();
        resize(560, 400);
        m_postIt = true;
    } else {
        menuBar()->setVisible(true);
        for (auto* t : findChildren<QToolBar*>()) t->setVisible(true);
        statusBar()->setVisible(true);
        setWindowFlag(Qt::WindowStaysOnTopHint, m_alwaysOnTop);
        show();
        if (m_prePostItGeom.isValid()) setGeometry(m_prePostItGeom);
        m_postIt = false;
    }
    if (m_actPostIt) m_actPostIt->setChecked(m_postIt);
}

// -----------------------------------------------------------------------------
// Phase 5i — Window menu
// -----------------------------------------------------------------------------

void MainWindow::onWindowList()
{
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Window list"));
    dlg.resize(560, 360);
    auto* root = new QVBoxLayout(&dlg);

    auto* list = new QListWidget(&dlg);
    list->setSelectionMode(QAbstractItemView::SingleSelection);

    // Re-populate from current state across BOTH panes. Each item stores
    // its (paneIdx, indexInPane) in two Qt::UserRole slots. When the right
    // pane is shown, labels carry an "(L) " / "(R) " prefix so users with
    // the same file in both panes can disambiguate. (Phase 3d.)
    auto refresh = [this, list]{
        list->clear();
        const bool split = rightPaneVisible();
        for (int p = 0; p < 2; ++p) {
            EditorTabs* pane = m_panes[p];
            if (!pane) continue;
            if (p == 1 && !split) break;
            for (int i = 0; i < pane->bufferCount(); ++i) {
                Buffer* b = pane->bufferAt(i);
                if (!b) continue;
                QString label = b->isDirty()
                    ? QStringLiteral("● ") + b->displayName()
                    : b->displayName();
                if (split) {
                    label = (p == 0 ? QStringLiteral("(L) ")
                                    : QStringLiteral("(R) ")) + label;
                }
                const QString tip = b->hasFile() ? b->filePath() : b->displayName();
                auto* it = new QListWidgetItem(label, list);
                it->setToolTip(tip);
                it->setData(Qt::UserRole,     i);
                it->setData(Qt::UserRole + 1, p);
            }
        }
        // Highlight the active pane's current tab.
        const int activeIdx = activePane()->currentIndex();
        for (int row = 0; row < list->count(); ++row) {
            auto* it = list->item(row);
            if (it->data(Qt::UserRole).toInt() == activeIdx
             && it->data(Qt::UserRole + 1).toInt() == m_activePane) {
                list->setCurrentRow(row);
                break;
            }
        }
    };
    refresh();
    root->addWidget(list, 1);

    auto* row = new QHBoxLayout;
    auto* aSwitch = new QPushButton(tr("&Switch to"), &dlg);
    auto* aSave   = new QPushButton(tr("Sa&ve"),      &dlg);
    auto* aClose  = new QPushButton(tr("&Close"),     &dlg);
    auto* aDone   = new QPushButton(tr("Done"),       &dlg);
    aDone->setDefault(true);
    row->addWidget(aSwitch);
    row->addWidget(aSave);
    row->addWidget(aClose);
    row->addStretch(1);
    row->addWidget(aDone);
    root->addLayout(row);

    auto selected = [list]() -> std::pair<int,int> {
        QListWidgetItem* it = list->currentItem();
        if (!it) return {-1, -1};
        return {it->data(Qt::UserRole + 1).toInt(),
                it->data(Qt::UserRole).toInt()};
    };

    connect(list, &QListWidget::itemDoubleClicked, &dlg,
            [this, selected, &dlg](QListWidgetItem*){
        const auto [paneIdx, idx] = selected();
        if (paneIdx >= 0 && idx >= 0) {
            setActivePane(paneIdx);
            m_panes[paneIdx]->setCurrentIndex(idx);
        }
        dlg.accept();
    });
    connect(aSwitch, &QPushButton::clicked, &dlg, [this, selected, &dlg]{
        const auto [paneIdx, idx] = selected();
        if (paneIdx >= 0 && idx >= 0) {
            setActivePane(paneIdx);
            m_panes[paneIdx]->setCurrentIndex(idx);
        }
        dlg.accept();
    });
    connect(aSave, &QPushButton::clicked, &dlg, [this, selected, refresh]{
        const auto [paneIdx, idx] = selected();
        if (paneIdx < 0 || idx < 0) return;
        setActivePane(paneIdx);
        m_panes[paneIdx]->setCurrentIndex(idx);   // onFileSave reads activePane
        onFileSave();                              // pops Save-As if untitled
        refresh();
    });
    connect(aClose, &QPushButton::clicked, &dlg, [this, selected, refresh]{
        const auto [paneIdx, idx] = selected();
        if (paneIdx < 0 || idx < 0) return;
        EditorTabs* pane = m_panes[paneIdx];
        if (!confirmCloseAt(pane, idx)) return;
        pane->dropBufferAt(idx);
        if (pane->bufferCount() == 0) pane->newUntitled();
        refresh();
    });
    connect(aDone, &QPushButton::clicked, &dlg, &QDialog::accept);

    dlg.exec();
}

namespace {

template <typename KeyFn>
void sortTabsBy(EditorTabs* tabs, KeyFn key)
{
    const int n = tabs->bufferCount();
    if (n < 2) return;
    Buffer* current = tabs->currentBuffer();

    std::vector<std::pair<QString, Buffer*>> pairs;
    pairs.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        Buffer* b = tabs->bufferAt(i);
        if (b) pairs.emplace_back(key(b), b);
    }
    std::stable_sort(pairs.begin(), pairs.end(),
        [](const auto& a, const auto& b) {
            return a.first.localeAwareCompare(b.first) < 0;
        });

    // For each desired position d, locate the current index of the desired
    // buffer and move it. After d settles, positions [0..d-1] are stable.
    // O(n^2) due to indexOfBuffer's linear search; fine for typical tab
    // counts (≤30).
    for (int d = 0; d < static_cast<int>(pairs.size()); ++d) {
        const int src = tabs->indexOfBuffer(pairs[d].second);
        if (src >= 0 && src != d) tabs->moveTab(src, d);
    }
    if (current) tabs->setCurrentIndex(tabs->indexOfBuffer(current));
}

} // namespace

void MainWindow::onWindowSortByName()
{
    sortTabsBy(activePane(),[](Buffer* b){ return b->displayName(); });
}

void MainWindow::onWindowSortByPath()
{
    sortTabsBy(activePane(),[](Buffer* b) -> QString {
        // Untitled buffers (no file path) sort to the very end via the
        // 0xFFFF-prefix trick — sorts after every printable filename.
        if (b->hasFile()) return b->filePath();
        return QString(QChar(0xFFFF)) + QChar(0xFFFF) + b->displayName();
    });
}

// -----------------------------------------------------------------------------
// Tab close orchestration (Q11 fix).
//
// EditorTabs emits closeRequested(int) when QTabWidget asks to close a tab,
// and closeAllBuffers() / onFileCloseTab() also funnel through here. The
// flow drives Save/Discard/Cancel, runs the Save-As dialog when needed
// (so untitled-and-dirty buffers don't lose content silently), then asks
// EditorTabs to drop the buffer.
// -----------------------------------------------------------------------------

bool MainWindow::confirmCloseAt(EditorTabs* pane, int index)
{
    if (!pane) return true;
    Buffer* b = pane->bufferAt(index);
    if (!b) return true;
    if (!b->isDirty()) return true;

    // Make the tab in question visible (and focus its pane) so the prompt
    // isn't ambiguous when both panes are showing.
    setActivePane(paneIndexOf(pane));
    pane->setCurrentIndex(index);

    const auto reply = QMessageBox::question(this, tr("Unsaved changes"),
        tr("'%1' has unsaved changes. Save before closing?")
            .arg(b->displayName()),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
        QMessageBox::Save);

    switch (reply) {
        case QMessageBox::Save:
            return onFileSave();          // pops Save-As if untitled
        case QMessageBox::Discard:
            return true;
        default:
            return false;                  // cancelled
    }
}

bool MainWindow::closeAllBuffers()
{
    // Phase 3d — walk both panes, in order: left first, then right.
    for (int p = 0; p < 2; ++p) {
        EditorTabs* pane = m_panes[p];
        if (!pane) continue;
        while (pane->bufferCount() > 0) {
            if (!confirmCloseAt(pane, 0)) return false;
            pane->dropBufferAt(0);
        }
    }
    return true;
}

void MainWindow::onTabCloseRequest(int index)
{
    EditorTabs* pane = qobject_cast<EditorTabs*>(sender());
    if (!pane) pane = activePane();
    Buffer* b = pane->bufferAt(index);
    // Phase 9n — pin gate at the tab's ✕ button. Mirrors onFileCloseTab.
    if (b && b->isPinned()) {
        statusBar()->showMessage(
            tr("Tab is pinned; unpin first to close."), 3000);
        return;
    }
    if (!confirmCloseAt(pane, index)) return;
    pane->dropBufferAt(index);
    if (pane->bufferCount() == 0) pane->newUntitled();
}

void MainWindow::closeEvent(QCloseEvent* e)
{
    // Phase 5S — if "Minimize to tray" is on AND the tray icon is visible,
    // hide the window instead of quitting. The tray icon's Quit menu item
    // bypasses this (it calls close() with the tray hidden, which falls
    // through). closing with Ctrl+Q similarly bypasses.
    if (m_tray && m_tray->isVisible()
     && m_actMinimizeToTray && m_actMinimizeToTray->isChecked()) {
        hide();
        e->ignore();
        return;
    }

    // Snapshot the session BEFORE dropping buffers, so the next launch
    // can reopen every file we currently have open.
    Session::save(m_panes[0], m_panes[1], rightPaneVisible(), m_activePane);

    // Hot exit: every dirty buffer (file-bound or Untitled) gets a
    // backup snapshot persisted to ~/.config/.../backup/, then a
    // `hot-exit-pending` marker is dropped. Next launch sees that
    // marker and silently overlays the snapshots — matching VSCode's
    // "Hot Exit" behaviour. No save prompt, no lost work.
    for (int p = 0; p < 2; ++p) {
        EditorTabs* pane = m_panes[p];
        if (!pane) continue;
        for (int i = 0; i < pane->bufferCount(); ++i) {
            Buffer* b = pane->bufferAt(i);
            if (b && b->isDirty()) Backup::writeBuffer(b);
        }
    }
    Backup::markHotExit();

    // Drop every buffer without prompting. Dirty content survives in
    // the backup snapshots; saved-and-clean buffers had their backups
    // cleared by Buffer::saveToFile.
    for (int p = 0; p < 2; ++p) {
        EditorTabs* pane = m_panes[p];
        if (!pane) continue;
        while (pane->bufferCount() > 0) {
            pane->dropBufferAt(0);
        }
    }
    // NB: deliberately NOT calling Backup::clearAll() here — that
    // would wipe the snapshots we just wrote. clearAll runs on next
    // launch after the overlay (or crash-recovery dismiss) consumes
    // them.
    // Phase 10b — unload plugins on clean exit. dlclose runs each
    // plugin's destructors (if any); they get one last chance to
    // persist state.
    PluginLoader::unloadAll();
    // Persist window geometry on a clean exit. If the window is maximized,
    // store the underlying restored geometry so the next launch can un-max
    // back to a sensible size.
    Config::setWindowMaximized(isMaximized());
    Config::setWindowGeometry(isMaximized() ? normalGeometry() : geometry());
    Config::save();
    e->accept();
}

// -----------------------------------------------------------------------------
// Find/Replace helpers
// -----------------------------------------------------------------------------

FindReplaceDialog* MainWindow::findReplace()
{
    if (m_findReplace) return m_findReplace;

    m_findReplace = new FindReplaceDialog(this, this);
    addDockWidget(Qt::BottomDockWidgetArea, m_findReplace);
    m_findReplace->hide();

    Buffer* b = activePane()->currentBuffer();
    m_findReplace->setEditor(b ? b->editor() : nullptr);

    connect(m_findReplace, &FindReplaceDialog::statusMessage,
            this, [this](const QString& msg){
        statusBar()->showMessage(msg, 5000);
    });

    return m_findReplace;
}

// -----------------------------------------------------------------------------
// Language menu
// -----------------------------------------------------------------------------

void MainWindow::onLanguageSelected(const LanguageDef* lang)
{
    Buffer* b = activePane()->currentBuffer();
    if (!b || !lang) return;
    b->setLanguage(lang);
    statusBar()->showMessage(
        tr("Language set to %1.").arg(lang->displayName),
        3000);
}

void MainWindow::onBufferLanguageChanged(Buffer* buffer)
{
    if (buffer == activePane()->currentBuffer()) {
        updateLanguageStatus();
    }
}

// Phase 5U.3 — Language menu (re)build. Splits the body out of
// buildMenus() so dialog Save can repopulate without restart.
void MainWindow::rebuildPluginsMenu()
{
    if (!m_pluginsMenu) return;
    m_pluginsMenu->clear();

    const auto& plugins = PluginLoader::all();
    if (plugins.isEmpty()) {
        // Empty placeholder so users can see the menu exists. The
        // "Open Plugins Folder" entry is the discoverability fix —
        // a user wondering "where do I drop the .so?" gets a
        // direct answer.
        auto* aEmpty = m_pluginsMenu->addAction(
            tr("(no plugins installed)"));
        aEmpty->setEnabled(false);
        m_pluginsMenu->addSeparator();
        auto* aOpen = m_pluginsMenu->addAction(
            tr("Open Plugins Folder..."));
        connect(aOpen, &QAction::triggered, this, [this]() {
            const QString dir = PluginLoader::pluginsDir();
            QDir().mkpath(dir);
            QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
        });
        return;
    }

    // One submenu per plugin, in load order (which loadAll() sorts
    // alphabetically by directory name).
    for (PluginLoader::Plugin* p : plugins) {
        if (!p) continue;
        auto* sub = m_pluginsMenu->addMenu(p->name);
        for (LinuxFuncItem* fn : p->functions) {
            if (!fn) continue;
            const QString itemName = QString::fromUtf8(fn->itemName);
            if (itemName.isEmpty()) {
                sub->addSeparator();
                continue;
            }
            auto* a = sub->addAction(itemName);
            a->setData(QVariant::fromValue<qulonglong>(
                reinterpret_cast<qulonglong>(fn)));
            if (fn->init2Check) {
                a->setCheckable(true);
                a->setChecked(true);
            }
            // Optional shortcut — Phase 10b ignores Ctrl/Alt/Shift
            // for the MVP and just maps the bare key. Plugin authors
            // who care about modifier shortcuts can call back via
            // messageProc to register a real QShortcut.
            if (fn->pShKey && fn->pShKey->key) {
                a->setShortcut(QKeySequence(fn->pShKey->key));
            }
            connect(a, &QAction::triggered, this, [a]() {
                auto packed = a->data().toULongLong();
                if (!packed) return;
                auto* fi = reinterpret_cast<LinuxFuncItem*>(packed);
                if (fi && fi->func) fi->func();
            });
        }
    }

    m_pluginsMenu->addSeparator();
    auto* aOpen = m_pluginsMenu->addAction(tr("Open Plugins Folder..."));
    connect(aOpen, &QAction::triggered, this, [this]() {
        const QString dir = PluginLoader::pluginsDir();
        QDir().mkpath(dir);
        QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
    });
}

void MainWindow::rebuildLanguageMenu()
{
    if (!m_languageMenu) return;
    m_languageMenu->clear();
    m_languageActions.clear();

    auto* group = new QActionGroup(this);
    group->setExclusive(true);
    m_languageActions.reserve(static_cast<std::size_t>(Languages::count()));
    for (int i = 0; i < Languages::count(); ++i) {
        const LanguageDef* L = &Languages::all()[i];
        auto* a = m_languageMenu->addAction(L->displayName);
        a->setCheckable(true);
        group->addAction(a);
        connect(a, &QAction::triggered, this,
                [this, L]{ onLanguageSelected(L); });
        m_languageActions.push_back(a);
    }
    m_languageMenu->addSeparator();

    // Phase 5U.3 — User Defined Language submenu, lazy aboutToShow refresh
    // (mirrors Phase 4d's recent-files pattern). Lists every UDL currently
    // in the registry; each entry routes to setLanguage on the active
    // buffer.
    m_userDefinedMenu = m_languageMenu->addMenu(tr("&User Defined Language..."));
    connect(m_userDefinedMenu, &QMenu::aboutToShow,
            this, &MainWindow::refreshUserDefinedMenu);

    m_actDefineYourLang = m_languageMenu->addAction(tr("Define your &language..."));
    connect(m_actDefineYourLang, &QAction::triggered,
            this, &MainWindow::onDefineYourLanguage);

    // Sync the radio with the active buffer's language. Skipped when the
    // status bar isn't built yet (initial buildMenus() runs before
    // buildStatusBar()) — the per-buffer signal flow hits it later.
    if (m_statusLanguage) updateLanguageStatus();
}

void MainWindow::refreshUserDefinedMenu()
{
    if (!m_userDefinedMenu) return;
    m_userDefinedMenu->clear();

    bool any = false;
    for (int i = 0; i < Languages::count(); ++i) {
        const LanguageDef* L = &Languages::all()[i];
        if (!L->userDefined) continue;
        auto* a = m_userDefinedMenu->addAction(L->displayName);
        connect(a, &QAction::triggered, this,
                [this, L]{ onLanguageSelected(L); });
        any = true;
    }
    if (!any) {
        auto* empty = m_userDefinedMenu->addAction(tr("(none defined)"));
        empty->setEnabled(false);
    }
    m_userDefinedMenu->addSeparator();
    auto* aOpen = m_userDefinedMenu->addAction(tr("&Edit defined languages..."));
    connect(aOpen, &QAction::triggered, this, &MainWindow::onDefineYourLanguage);
}

void MainWindow::onDefineYourLanguage()
{
    UserDefineDialog dlg(this);
    connect(&dlg, &UserDefineDialog::udlsSaved,
            this, &MainWindow::onUDLsSaved);
    dlg.exec();
}

void MainWindow::onUDLsSaved()
{
    // Snapshot every buffer's UDL internalName before we tear the registry
    // down. Built-in pointers stay valid; UDL pointers will be invalidated
    // by reloadUDLs() and need re-resolution.
    QVector<Buffer*> udlBuffers;
    QVector<QString> udlNames;       // by internalName ("udl:<name>")
    for (Buffer* b : allBuffers()) {
        const LanguageDef* L = b->language();
        if (L && L->userDefined) {
            udlBuffers.push_back(b);
            udlNames.push_back(QString::fromUtf8(L->internalName.c_str()));
        }
    }

    Languages::reloadUDLs();

    // Re-resolve UDL languages by internalName. If the UDL was deleted,
    // setLanguage(nullptr) falls back to plain text.
    for (int i = 0; i < udlBuffers.size(); ++i) {
        const QByteArray name = udlNames[i].toUtf8();
        const LanguageDef* L = Languages::findByInternalName(name.constData());
        udlBuffers[i]->setLanguage(L);   // nullptr → plainText() in setLanguage
    }

    rebuildLanguageMenu();
    statusBar()->showMessage(tr("User-defined languages saved."), 3000);
}

// -----------------------------------------------------------------------------
// File → Open Recent submenu (Phase 4d)
// -----------------------------------------------------------------------------

void MainWindow::refreshRecentFilesMenu()
{
    if (!m_recentMenu) return;
    m_recentMenu->clear();

    const QStringList recents = Config::recentFiles();
    if (recents.isEmpty()) {
        auto* empty = m_recentMenu->addAction(tr("(empty)"));
        empty->setEnabled(false);
        return;
    }

    for (const QString& path : recents) {
        const QString label = QFileInfo(path).fileName()
                            + QStringLiteral("    ")
                            + QFileInfo(path).absolutePath();
        auto* a = m_recentMenu->addAction(label);
        a->setToolTip(path);
        connect(a, &QAction::triggered, this, [this, path]{
            activePane()->openFile(path);
        });
    }
    m_recentMenu->addSeparator();
    auto* aClear = m_recentMenu->addAction(tr("&Clear Recent List"));
    connect(aClear, &QAction::triggered, this, &MainWindow::onClearRecentFiles);
}

void MainWindow::onClearRecentFiles()
{
    Config::clearRecentFiles();
    statusBar()->showMessage(tr("Recent files list cleared."), 3000);
}

// -----------------------------------------------------------------------------
// File → Reopen Recently Closed File (Phase 9b.2)
//
// EditorTabs::bufferAboutToClose pipes the closing buffer's path here from
// either pane. The MRU is in-memory only (capped at 20); resets on app
// close. Untitled buffers don't surface the signal — there's no path to
// reopen by.
// -----------------------------------------------------------------------------

namespace {
constexpr int kRecentlyClosedCap = 20;   // 9b.2 — internal storage cap
constexpr int kRecentlyClosedMenuMax = 10;   // 9b.2 — submenu display cap
} // namespace

void MainWindow::onBufferAboutToClose(const QString& path)
{
    if (path.isEmpty()) return;
    // Move-to-front: drop any prior occurrence then prepend.
    m_recentlyClosed.removeAll(path);
    m_recentlyClosed.prepend(path);
    while (m_recentlyClosed.size() > kRecentlyClosedCap) {
        m_recentlyClosed.removeLast();
    }
}

void MainWindow::onFileReopenRecentlyClosed()
{
    if (m_recentlyClosed.isEmpty()) {
        statusBar()->showMessage(
            tr("No recently closed files to reopen."), 3000);
        return;
    }
    const QString path = m_recentlyClosed.takeFirst();
    activePane()->openFile(path);
}

void MainWindow::refreshRecentlyClosedMenu()
{
    if (!m_recentlyClosedMenu) return;
    m_recentlyClosedMenu->clear();

    if (m_recentlyClosed.isEmpty()) {
        auto* empty = m_recentlyClosedMenu->addAction(tr("(none)"));
        empty->setEnabled(false);
        return;
    }

    const int shown = qMin<int>(m_recentlyClosed.size(),
                                kRecentlyClosedMenuMax);
    for (int i = 0; i < shown; ++i) {
        const QString path = m_recentlyClosed.at(i);
        const QString label = QFileInfo(path).fileName()
                            + QStringLiteral("    ")
                            + QFileInfo(path).absolutePath();
        auto* a = m_recentlyClosedMenu->addAction(label);
        a->setToolTip(path);
        connect(a, &QAction::triggered, this, [this, path]{
            // Pull the chosen entry to the front and remove it from the
            // MRU before opening (so reopening doesn't double-list it on
            // the next close-then-reopen cycle).
            m_recentlyClosed.removeAll(path);
            activePane()->openFile(path);
        });
    }
}

// -----------------------------------------------------------------------------
// View → Theme submenu (Phase 4c)
// -----------------------------------------------------------------------------

namespace {

void applyDarkChromePalette()
{
    // Dark Qt palette for the surrounding chrome (menus, toolbar, status bar,
    // tab strip, dialogs). Approximates GNOME Adwaita-Dark / KDE Breeze-Dark.
    QPalette p;
    const QColor base(0x2A, 0x2A, 0x2A);
    const QColor altBase(0x35, 0x35, 0x35);
    const QColor text(0xDC, 0xDC, 0xCC);
    const QColor brightText(0xFF, 0xFF, 0xFF);
    const QColor disabledText(0x80, 0x80, 0x80);
    const QColor highlight(0x2A, 0x82, 0xDA);
    p.setColor(QPalette::Window,          QColor(0x35, 0x35, 0x35));
    p.setColor(QPalette::WindowText,      text);
    p.setColor(QPalette::Base,            base);
    p.setColor(QPalette::AlternateBase,   altBase);
    p.setColor(QPalette::ToolTipBase,     QColor(0x40, 0x40, 0x40));
    p.setColor(QPalette::ToolTipText,     text);
    p.setColor(QPalette::Text,            text);
    p.setColor(QPalette::Button,          QColor(0x40, 0x40, 0x40));
    p.setColor(QPalette::ButtonText,      text);
    p.setColor(QPalette::BrightText,      brightText);
    p.setColor(QPalette::Highlight,       highlight);
    p.setColor(QPalette::HighlightedText, brightText);
    p.setColor(QPalette::Link,            QColor(0x6F, 0xB7, 0xFF));
    p.setColor(QPalette::Disabled, QPalette::Text,       disabledText);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, disabledText);
    p.setColor(QPalette::Disabled, QPalette::WindowText, disabledText);
    qApp->setStyle(QStringLiteral("Fusion"));   // Fusion respects custom palettes
    qApp->setPalette(p);
}

void applyLightChromePalette()
{
    // Restore the system default palette/style.
    qApp->setStyle(QString{});
    qApp->setPalette(QPalette{});
}

} // namespace

void MainWindow::onThemeLight()
{
    Theme::setMode(Theme::Mode::Light);
    applyLightChromePalette();
    for (Buffer* b : allBuffers()) {
        if (b) b->reapplyTheme();
    }
    if (m_documentMap) m_documentMap->reapplyTheme();   // Phase 9m.1
    Config::setTheme(QStringLiteral("Light"));
    statusBar()->showMessage(tr("Theme: Light"), 2000);
}

void MainWindow::onThemeDark()
{
    Theme::setMode(Theme::Mode::Dark);
    applyDarkChromePalette();
    for (Buffer* b : allBuffers()) {
        if (b) b->reapplyTheme();
    }
    if (m_documentMap) m_documentMap->reapplyTheme();   // Phase 9m.1
    Config::setTheme(QStringLiteral("Dark"));
    statusBar()->showMessage(tr("Theme: Dark"), 2000);
}

void MainWindow::applyUiVisibilityFromConfig()
{
    // Phase 5N.2 — toolbar / statusbar / per-tab close button. Called on
    // startup (after buildToolbar/buildStatusBar) and on every Apply / OK
    // from PreferencesDialog. Distraction Free / Post-It modes (Phase 5V)
    // explicitly hide chrome and persist their own snapshots; they restore
    // visibility based on those snapshots, not from Config — leaving
    // applyUiVisibilityFromConfig safely idempotent for the user-driven
    // path.
    const bool wantTb  = Config::showToolBar();
    const bool wantSb  = Config::showStatusBar();
    const bool wantTc  = Config::tabsClosable();
    for (QToolBar* tb : findChildren<QToolBar*>()) {
        tb->setVisible(wantTb);
    }
    if (statusBar()) statusBar()->setVisible(wantSb);
    for (EditorTabs* p : m_panes) {
        if (p) p->setTabsClosable(wantTc);
    }
}

void MainWindow::applyEditingPrefsFromConfig()
{
    // Phase 5N.3 — caret width + blink period applied to every editor in
    // both panes. Smart-highlight + bracket-auto-pair are read at
    // notification time by Buffer.cpp so they need no explicit push;
    // however we DO clear the smart-highlight indicator on every editor
    // when the toggle is off, mirroring onViewToggleSmartHighlight's
    // behaviour (so a Preferences-driven toggle-off has the same effect
    // as the View menu one).
    //
    // Phase 9b.4 — vertical-edge marker. The mode (EDGE_NONE / EDGE_LINE),
    // column, and colour are pushed to every editor so a Preferences-
    // driven toggle takes effect without restart. New buffers pick up
    // the same values from Buffer's ctor.
    const int caretWidth = Config::caretWidth();
    const int caretBlink = Config::caretBlinkMs();
    const bool smartOn   = Config::smartHighlightEnabled();
    const bool edgeOn    = Config::verticalEdgeEnabled();
    const int  edgeCol   = Config::verticalEdgeColumn();
    const int  edgeRgb   = sciColorFromHex(Config::verticalEdgeColor());
    if (m_actSmartHighlight) m_actSmartHighlight->setChecked(smartOn);
    for (Buffer* b : allBuffers()) {
        if (!b) continue;
        auto* ed = b->editor();
        ed->send(static_cast<unsigned int>(Message::SetCaretWidth),
                 static_cast<Scintilla::uptr_t>(caretWidth));
        ed->send(static_cast<unsigned int>(Message::SetCaretPeriod),
                 static_cast<Scintilla::uptr_t>(caretBlink));
        ed->send(static_cast<unsigned int>(Message::SetEdgeMode),
                 edgeOn ? EDGE_LINE : EDGE_NONE, 0);
        ed->send(static_cast<unsigned int>(Message::SetEdgeColumn),
                 static_cast<Scintilla::uptr_t>(edgeCol), 0);
        ed->send(static_cast<unsigned int>(Message::SetEdgeColour),
                 static_cast<Scintilla::uptr_t>(edgeRgb), 0);
        // Phase 9c.2 — always invalidate the smart-highlight cache on
        // Apply so a match-case / whole-word change takes effect on the
        // user's next selection. When the master toggle is OFF this
        // also wipes any leftover indicator (matches onViewToggleSmartHighlight).
        b->clearSmartHighlightCache();
        // Phase 9f — push the resolved per-language indent so global
        // default OR per-language override changes flow through to
        // every open buffer without needing a tab switch.
        b->applyResolvedIndent();
    }
}

void MainWindow::applyBackupPrefsFromConfig()
{
    // Phase 5N.11 — backup interval + master enable. Re-tune the live
    // QTimer; toggling enable starts/stops it.
    if (!m_backupTimer) return;
    const int sec = Config::backupIntervalSec();
    m_backupTimer->setInterval(sec * 1000);
    if (Config::backupEnabled()) {
        if (!m_backupTimer->isActive()) m_backupTimer->start();
    } else {
        if (m_backupTimer->isActive()) m_backupTimer->stop();
    }
}

void MainWindow::applyFileWatcherPrefsFromConfig()
{
    // Phase 5N.11 — file watcher master enable. When disabled, drop every
    // watched path and stop reacting to fileChanged. Re-enabling re-adds
    // every open buffer's path via syncFileWatcher.
    if (!m_fileWatcher) return;
    if (Config::fileWatcherEnabled()) {
        syncFileWatcher();
    } else {
        const QStringList paths = m_fileWatcher->files();
        if (!paths.isEmpty()) m_fileWatcher->removePaths(paths);
    }
}

void MainWindow::applyCurrentThemeToChrome()
{
    if (Theme::mode() == Theme::Mode::Dark) {
        applyDarkChromePalette();
    } else {
        applyLightChromePalette();
    }
}

QString MainWindow::currentSelectionText() const
{
    Buffer* b = activePane()->currentBuffer();
    if (!b) return {};
    auto* ed = b->editor();
    const Scintilla::sptr_t selStart =
        ed->send(static_cast<unsigned int>(Message::GetSelectionStart));
    const Scintilla::sptr_t selEnd =
        ed->send(static_cast<unsigned int>(Message::GetSelectionEnd));
    if (selEnd <= selStart) return {};
    // Reasonable cap: don't prefill the find combo with a giant selection.
    const Scintilla::sptr_t bytes = selEnd - selStart;
    if (bytes > 4096) return {};
    std::vector<char> buf(static_cast<std::size_t>(bytes) + 1, '\0');
    ed->send(static_cast<unsigned int>(Message::GetSelText),
             0, reinterpret_cast<Scintilla::sptr_t>(buf.data()));
    QString s = QString::fromUtf8(buf.data(), static_cast<int>(bytes));
    // Multi-line selections shouldn't be a search prefill — pick the first
    // line only. Mimics most editors' behaviour.
    const int nl = s.indexOf(QChar('\n'));
    if (nl >= 0) s.truncate(nl);
    return s.trimmed();
}

// -----------------------------------------------------------------------------
// Phase 5a — Edit / Search / View burndown
// -----------------------------------------------------------------------------

namespace {

ScintillaEditBase* currentEditor(EditorTabs* tabs)
{
    Buffer* b = tabs->currentBuffer();
    return b ? b->editor() : nullptr;
}

inline Scintilla::sptr_t sci(ScintillaEditBase* ed, Message m,
                             Scintilla::uptr_t w = 0, Scintilla::sptr_t l = 0)
{
    return ed->send(static_cast<unsigned int>(m), w, l);
}

inline Scintilla::sptr_t scis(ScintillaEditBase* ed, Message m,
                              Scintilla::uptr_t w, const char* s)
{
    return ed->sends(static_cast<unsigned int>(m), w, s);
}

} // namespace

void MainWindow::onEditTrimTrailing()
{
    auto* ed = currentEditor(activePane());
    if (!ed) return;
    sci(ed, Message::BeginUndoAction);
    const Scintilla::sptr_t lineCount = sci(ed, Message::GetLineCount);
    for (Scintilla::sptr_t line = 0; line < lineCount; ++line) {
        const Scintilla::sptr_t lineStart = sci(ed, Message::PositionFromLine,
                                                static_cast<Scintilla::uptr_t>(line));
        const Scintilla::sptr_t lineEnd   = sci(ed, Message::GetLineEndPosition,
                                                static_cast<Scintilla::uptr_t>(line));
        Scintilla::sptr_t p = lineEnd;
        while (p > lineStart) {
            const char ch = static_cast<char>(sci(ed, Message::GetCharAt,
                static_cast<Scintilla::uptr_t>(p - 1)));
            if (ch != ' ' && ch != '\t' && ch != '\v' && ch != '\f') break;
            --p;
        }
        if (p < lineEnd) {
            sci(ed, Message::DeleteRange, static_cast<Scintilla::uptr_t>(p),
                lineEnd - p);
        }
    }
    sci(ed, Message::EndUndoAction);
    statusBar()->showMessage(tr("Trimmed trailing whitespace."), 3000);
}

void MainWindow::onEditSortLinesAscending()
{
    auto* ed = currentEditor(activePane());
    if (!ed) return;
    const Scintilla::sptr_t length = sci(ed, Message::GetTextLength);
    if (length <= 0) return;
    std::vector<char> buf(static_cast<std::size_t>(length) + 1, '\0');
    sci(ed, Message::GetText, static_cast<Scintilla::uptr_t>(buf.size()),
        reinterpret_cast<Scintilla::sptr_t>(buf.data()));
    QString text = QString::fromUtf8(buf.data(), static_cast<int>(length));

    // Detect EOL style: prefer \r\n if present, else \n. Sort by line, rejoin.
    const bool crlf = text.contains(QStringLiteral("\r\n"));
    const QString sep = crlf ? QStringLiteral("\r\n") : QStringLiteral("\n");
    QStringList lines = text.split(sep);
    // Preserve a trailing empty line if the document ended with EOL.
    bool trailingEmpty = !lines.isEmpty() && lines.last().isEmpty();
    if (trailingEmpty) lines.removeLast();
    std::sort(lines.begin(), lines.end(),
              [](const QString& a, const QString& b) {
                  return a.localeAwareCompare(b) < 0;
              });
    if (trailingEmpty) lines.append(QString{});
    const QByteArray out = lines.join(sep).toUtf8();

    sci(ed, Message::BeginUndoAction);
    sci(ed, Message::ClearAll);
    scis(ed, Message::AddText, static_cast<Scintilla::uptr_t>(out.size()),
         out.constData());
    sci(ed, Message::EndUndoAction);
    statusBar()->showMessage(
        tr("Sorted %1 lines (ascending).").arg(lines.size()), 3000);
}

// =============================================================================
// Phase 9a — Edit menu polish (P2 batch).
// =============================================================================

namespace {

// Insert `text` at the caret (or replace the active selection) as a single
// undo step. Used by Insert Date / Time and any future Insert items.
void insertAtCaret(ScintillaEditBase* ed, const QString& text)
{
    if (!ed) return;
    const QByteArray bytes = text.toUtf8();
    sci(ed, Message::BeginUndoAction);
    scis(ed, Message::ReplaceSel, 0, bytes.constData());
    sci(ed, Message::EndUndoAction);
}

// Read the entire buffer, return as QString (UTF-8 decoded). Caller can
// transform and feed back via writeWholeBuffer().
QString readWholeBuffer(ScintillaEditBase* ed)
{
    const Scintilla::sptr_t length = sci(ed, Message::GetTextLength);
    if (length <= 0) return {};
    std::vector<char> buf(static_cast<std::size_t>(length) + 1, '\0');
    sci(ed, Message::GetText, static_cast<Scintilla::uptr_t>(buf.size()),
        reinterpret_cast<Scintilla::sptr_t>(buf.data()));
    return QString::fromUtf8(buf.data(), static_cast<int>(length));
}

// Replace the buffer's contents with `text` as a single undo step. Used
// by every whole-buffer transform (sort, convert tabs/spaces).
void writeWholeBuffer(ScintillaEditBase* ed, const QString& text)
{
    const QByteArray out = text.toUtf8();
    sci(ed, Message::BeginUndoAction);
    sci(ed, Message::ClearAll);
    scis(ed, Message::AddText, static_cast<Scintilla::uptr_t>(out.size()),
         out.constData());
    sci(ed, Message::EndUndoAction);
}

// Detect EOL convention from `text`: prefer CRLF if present, else LF.
QString detectEol(const QString& text)
{
    return text.contains(QStringLiteral("\r\n"))
        ? QStringLiteral("\r\n") : QStringLiteral("\n");
}

} // namespace

// ---- Insert Date / Time ----

void MainWindow::onEditInsertDateShort()
{
    auto* ed = currentEditor(activePane());
    if (!ed) return;
    insertAtCaret(ed, QLocale().toString(QDate::currentDate(),
                                         QLocale::ShortFormat));
    statusBar()->showMessage(tr("Inserted date (short)."), 2000);
}

void MainWindow::onEditInsertDateLong()
{
    auto* ed = currentEditor(activePane());
    if (!ed) return;
    insertAtCaret(ed, QLocale().toString(QDate::currentDate(),
                                         QLocale::LongFormat));
    statusBar()->showMessage(tr("Inserted date (long)."), 2000);
}

void MainWindow::onEditInsertTime()
{
    auto* ed = currentEditor(activePane());
    if (!ed) return;
    insertAtCaret(ed, QLocale().toString(QTime::currentTime(),
                                         QLocale::ShortFormat));
    statusBar()->showMessage(tr("Inserted time."), 2000);
}

// ---- Trim Leading / Both ----

namespace {

// Walk `lineStart` forward, count leading whitespace bytes. Mirrors the
// trailing-walk in onEditTrimTrailing but in the opposite direction.
Scintilla::sptr_t countLeadingWs(ScintillaEditBase* ed,
                                 Scintilla::sptr_t lineStart,
                                 Scintilla::sptr_t lineEnd)
{
    Scintilla::sptr_t p = lineStart;
    while (p < lineEnd) {
        const char ch = static_cast<char>(sci(ed, Message::GetCharAt,
            static_cast<Scintilla::uptr_t>(p)));
        if (ch != ' ' && ch != '\t' && ch != '\v' && ch != '\f') break;
        ++p;
    }
    return p - lineStart;
}

} // namespace

void MainWindow::onEditTrimLeading()
{
    auto* ed = currentEditor(activePane());
    if (!ed) return;
    sci(ed, Message::BeginUndoAction);
    const Scintilla::sptr_t lineCount = sci(ed, Message::GetLineCount);
    // Walk lines back-to-front so each delete doesn't shift the indices
    // of lines we haven't processed yet.
    for (Scintilla::sptr_t line = lineCount - 1; line >= 0; --line) {
        const Scintilla::sptr_t lineStart = sci(ed, Message::PositionFromLine,
                                                static_cast<Scintilla::uptr_t>(line));
        const Scintilla::sptr_t lineEnd   = sci(ed, Message::GetLineEndPosition,
                                                static_cast<Scintilla::uptr_t>(line));
        const Scintilla::sptr_t leading = countLeadingWs(ed, lineStart, lineEnd);
        if (leading > 0) {
            sci(ed, Message::DeleteRange,
                static_cast<Scintilla::uptr_t>(lineStart), leading);
        }
    }
    sci(ed, Message::EndUndoAction);
    statusBar()->showMessage(tr("Trimmed leading whitespace."), 3000);
}

void MainWindow::onEditTrimBoth()
{
    auto* ed = currentEditor(activePane());
    if (!ed) return;
    sci(ed, Message::BeginUndoAction);
    const Scintilla::sptr_t lineCount = sci(ed, Message::GetLineCount);
    for (Scintilla::sptr_t line = lineCount - 1; line >= 0; --line) {
        const Scintilla::sptr_t lineStart = sci(ed, Message::PositionFromLine,
                                                static_cast<Scintilla::uptr_t>(line));
        const Scintilla::sptr_t lineEnd   = sci(ed, Message::GetLineEndPosition,
                                                static_cast<Scintilla::uptr_t>(line));
        // Trim trailing first (deleting at line tail doesn't shift the
        // lineStart we still need for the leading pass).
        Scintilla::sptr_t p = lineEnd;
        while (p > lineStart) {
            const char ch = static_cast<char>(sci(ed, Message::GetCharAt,
                static_cast<Scintilla::uptr_t>(p - 1)));
            if (ch != ' ' && ch != '\t' && ch != '\v' && ch != '\f') break;
            --p;
        }
        if (p < lineEnd) {
            sci(ed, Message::DeleteRange, static_cast<Scintilla::uptr_t>(p),
                lineEnd - p);
        }
        // Now trim leading.
        const Scintilla::sptr_t leading = countLeadingWs(ed, lineStart, p);
        if (leading > 0) {
            sci(ed, Message::DeleteRange,
                static_cast<Scintilla::uptr_t>(lineStart), leading);
        }
    }
    sci(ed, Message::EndUndoAction);
    statusBar()->showMessage(tr("Trimmed leading and trailing whitespace."), 3000);
}

// ---- Convert Tabs ↔ Spaces ----

void MainWindow::onEditConvertTabsToSpaces()
{
    auto* ed = currentEditor(activePane());
    if (!ed) return;
    QString text = readWholeBuffer(ed);
    if (text.isEmpty()) return;

    const int tabWidth = qBound(1, Config::defaultTabWidth(), 16);
    const QString sep  = detectEol(text);
    const QString spaces(tabWidth, QLatin1Char(' '));

    QStringList lines = text.split(sep);
    int converted = 0;
    for (QString& line : lines) {
        int i = 0;
        while (i < line.size() && line[i] == QLatin1Char('\t')) ++i;
        if (i > 0) {
            line.replace(0, i, spaces.repeated(i));
            ++converted;
        }
    }
    writeWholeBuffer(ed, lines.join(sep));
    statusBar()->showMessage(
        tr("Converted leading tabs to spaces on %1 lines.").arg(converted),
        3000);
}

void MainWindow::onEditConvertSpacesToTabs()
{
    auto* ed = currentEditor(activePane());
    if (!ed) return;
    QString text = readWholeBuffer(ed);
    if (text.isEmpty()) return;

    const int tabWidth = qBound(1, Config::defaultTabWidth(), 16);
    const QString sep  = detectEol(text);

    QStringList lines = text.split(sep);
    int converted = 0;
    for (QString& line : lines) {
        int spaceCount = 0;
        while (spaceCount < line.size()
               && line[spaceCount] == QLatin1Char(' ')) ++spaceCount;
        const int tabs = spaceCount / tabWidth;
        const int rem  = spaceCount % tabWidth;
        if (tabs > 0) {
            QString newLeading;
            newLeading.append(QString(tabs, QLatin1Char('\t')));
            newLeading.append(QString(rem, QLatin1Char(' ')));
            line.replace(0, spaceCount, newLeading);
            ++converted;
        }
    }
    writeWholeBuffer(ed, lines.join(sep));
    statusBar()->showMessage(
        tr("Converted leading spaces to tabs on %1 lines.").arg(converted),
        3000);
}

// ---- Sort Lines (Descending text + numeric) ----

namespace {

// Shared whole-buffer sort path. `cmp` receives the QString of each line
// and returns a sortable key; lines whose key is "missing" (e.g.
// non-numeric for an integer sort) all bunch up at the end. Returns
// (sorted-count, missing-count) for status-bar feedback.
struct SortStats { int sortedCount = 0; int missingCount = 0; };

template <typename Cmp>
SortStats sortBuffer(ScintillaEditBase* ed, Cmp&& cmp, bool ascending)
{
    SortStats stats;
    QString text = readWholeBuffer(ed);
    if (text.isEmpty()) return stats;

    const QString sep = detectEol(text);
    QStringList lines = text.split(sep);
    bool trailingEmpty = !lines.isEmpty() && lines.last().isEmpty();
    if (trailingEmpty) lines.removeLast();

    // Partition: comparable lines first, missing-key lines (e.g. non-
    // numeric) at the tail in their original order.
    QStringList sortable, missing;
    sortable.reserve(lines.size());
    for (const QString& l : lines) {
        if (cmp.hasKey(l)) sortable.push_back(l);
        else               missing.push_back(l);
    }
    std::sort(sortable.begin(), sortable.end(),
              [&](const QString& a, const QString& b) {
                  return ascending ? cmp.less(a, b) : cmp.less(b, a);
              });
    QStringList out;
    out.reserve(lines.size());
    out.append(sortable);
    out.append(missing);
    if (trailingEmpty) out.append(QString{});

    writeWholeBuffer(ed, out.join(sep));
    stats.sortedCount  = sortable.size();
    stats.missingCount = missing.size();
    return stats;
}

struct TextCmp {
    bool hasKey(const QString&) const { return true; }
    bool less(const QString& a, const QString& b) const {
        return a.localeAwareCompare(b) < 0;
    }
};

struct IntCmp {
    static bool parse(const QString& l, qint64* out) {
        // Accept leading whitespace, an optional sign, then digits.
        int i = 0;
        while (i < l.size() && l[i].isSpace()) ++i;
        const int start = i;
        if (i < l.size() && (l[i] == QLatin1Char('-')
                          || l[i] == QLatin1Char('+'))) ++i;
        const int digitsStart = i;
        while (i < l.size() && l[i].isDigit()) ++i;
        if (i == digitsStart) return false;
        bool ok = false;
        const qint64 v = l.mid(start, i - start).toLongLong(&ok);
        if (!ok) return false;
        *out = v;
        return true;
    }
    bool hasKey(const QString& l) const { qint64 v; return parse(l, &v); }
    bool less(const QString& a, const QString& b) const {
        qint64 va = 0, vb = 0;
        parse(a, &va);
        parse(b, &vb);
        return va < vb;
    }
};

struct DecimalCmp {
    static bool parse(const QString& l, double* out) {
        int i = 0;
        while (i < l.size() && l[i].isSpace()) ++i;
        const int start = i;
        if (i < l.size() && (l[i] == QLatin1Char('-')
                          || l[i] == QLatin1Char('+'))) ++i;
        const int numStart = i;
        bool sawDigit = false;
        while (i < l.size() && l[i].isDigit()) { ++i; sawDigit = true; }
        if (i < l.size() && l[i] == QLatin1Char('.')) {
            ++i;
            while (i < l.size() && l[i].isDigit()) { ++i; sawDigit = true; }
        }
        if (!sawDigit) return false;
        // optional exponent (e.g. 1.5e3)
        if (i < l.size() && (l[i] == QLatin1Char('e')
                          || l[i] == QLatin1Char('E'))) {
            int j = i + 1;
            if (j < l.size() && (l[j] == QLatin1Char('-')
                              || l[j] == QLatin1Char('+'))) ++j;
            const int expStart = j;
            while (j < l.size() && l[j].isDigit()) ++j;
            if (j > expStart) i = j;
        }
        bool ok = false;
        const double v = l.mid(start, i - start).toDouble(&ok);
        if (!ok) return false;
        (void)numStart;
        *out = v;
        return true;
    }
    bool hasKey(const QString& l) const { double v; return parse(l, &v); }
    bool less(const QString& a, const QString& b) const {
        double va = 0, vb = 0;
        parse(a, &va);
        parse(b, &vb);
        return va < vb;
    }
};

} // namespace

void MainWindow::onEditSortLinesDescending()
{
    auto* ed = currentEditor(activePane());
    if (!ed) return;
    const auto s = sortBuffer(ed, TextCmp{}, /*ascending=*/false);
    statusBar()->showMessage(
        tr("Sorted %1 lines as text (descending).").arg(s.sortedCount), 3000);
}

void MainWindow::onEditSortLinesAscIntegers()
{
    auto* ed = currentEditor(activePane());
    if (!ed) return;
    const auto s = sortBuffer(ed, IntCmp{}, /*ascending=*/true);
    statusBar()->showMessage(
        tr("Sorted %1 lines as integer (ascending). %2 non-numeric line(s) moved to end.")
            .arg(s.sortedCount).arg(s.missingCount),
        4000);
}

void MainWindow::onEditSortLinesDescIntegers()
{
    auto* ed = currentEditor(activePane());
    if (!ed) return;
    const auto s = sortBuffer(ed, IntCmp{}, /*ascending=*/false);
    statusBar()->showMessage(
        tr("Sorted %1 lines as integer (descending). %2 non-numeric line(s) moved to end.")
            .arg(s.sortedCount).arg(s.missingCount),
        4000);
}

void MainWindow::onEditSortLinesAscDecimals()
{
    auto* ed = currentEditor(activePane());
    if (!ed) return;
    const auto s = sortBuffer(ed, DecimalCmp{}, /*ascending=*/true);
    statusBar()->showMessage(
        tr("Sorted %1 lines as decimal (ascending). %2 non-numeric line(s) moved to end.")
            .arg(s.sortedCount).arg(s.missingCount),
        4000);
}

void MainWindow::onEditSortLinesDescDecimals()
{
    auto* ed = currentEditor(activePane());
    if (!ed) return;
    const auto s = sortBuffer(ed, DecimalCmp{}, /*ascending=*/false);
    statusBar()->showMessage(
        tr("Sorted %1 lines as decimal (descending). %2 non-numeric line(s) moved to end.")
            .arg(s.sortedCount).arg(s.missingCount),
        4000);
}

void MainWindow::onEditBlockComment()
{
    Buffer* b = activePane()->currentBuffer();
    if (!b) return;
    auto* ed = b->editor();
    const LanguageDef* lang = b->language();
    if (!lang || lang->commentLine.empty()) {
        statusBar()->showMessage(
            tr("Block Comment: no line-comment delimiter for this language."),
            4000);
        return;
    }
    const std::string prefix = lang->commentLine + " ";   // e.g. "// "

    // Determine line range. If a selection exists, span the lines it covers;
    // otherwise just the current line.
    Scintilla::sptr_t selStart = sci(ed, Message::GetSelectionStart);
    Scintilla::sptr_t selEnd   = sci(ed, Message::GetSelectionEnd);
    Scintilla::sptr_t firstLine = sci(ed, Message::LineFromPosition,
                                      static_cast<Scintilla::uptr_t>(selStart));
    Scintilla::sptr_t lastLine  = sci(ed, Message::LineFromPosition,
                                      static_cast<Scintilla::uptr_t>(selEnd));
    if (selEnd > selStart && selEnd ==
        sci(ed, Message::PositionFromLine, static_cast<Scintilla::uptr_t>(lastLine))) {
        // Selection ends at column 0 of lastLine — treat that as not
        // including lastLine (matches common editor convention).
        --lastLine;
    }

    // Decide direction: if every non-empty line already starts with the prefix
    // (after leading whitespace), uncomment; otherwise, comment.
    auto lineStartsWithPrefix = [&](Scintilla::sptr_t line) -> Scintilla::sptr_t {
        const Scintilla::sptr_t ls = sci(ed, Message::PositionFromLine,
                                         static_cast<Scintilla::uptr_t>(line));
        const Scintilla::sptr_t le = sci(ed, Message::GetLineEndPosition,
                                         static_cast<Scintilla::uptr_t>(line));
        Scintilla::sptr_t p = ls;
        while (p < le) {
            const char ch = static_cast<char>(sci(ed, Message::GetCharAt,
                static_cast<Scintilla::uptr_t>(p)));
            if (ch != ' ' && ch != '\t') break;
            ++p;
        }
        // Read prefix.size() bytes starting at p; compare.
        for (std::size_t k = 0; k < prefix.size(); ++k) {
            if (p + static_cast<Scintilla::sptr_t>(k) >= le) return -1;
            const char ch = static_cast<char>(sci(ed, Message::GetCharAt,
                static_cast<Scintilla::uptr_t>(p + static_cast<Scintilla::sptr_t>(k))));
            // Allow either "// " with the space or "//" without — be lenient
            // on the trailing space of the prefix.
            if (k == prefix.size() - 1 && prefix.back() == ' ' && ch != ' ') {
                // Treat as match (uncomment will only remove the non-space part).
                return p;
            }
            if (ch != prefix[k]) return -1;
        }
        return p;
    };

    bool allCommented = true;
    bool anyContent   = false;
    for (Scintilla::sptr_t line = firstLine; line <= lastLine; ++line) {
        const Scintilla::sptr_t ls = sci(ed, Message::PositionFromLine,
                                         static_cast<Scintilla::uptr_t>(line));
        const Scintilla::sptr_t le = sci(ed, Message::GetLineEndPosition,
                                         static_cast<Scintilla::uptr_t>(line));
        if (le > ls) anyContent = true;
        if (le > ls && lineStartsWithPrefix(line) < 0) allCommented = false;
    }
    if (!anyContent) return;
    const bool uncomment = allCommented;

    sci(ed, Message::BeginUndoAction);
    for (Scintilla::sptr_t line = firstLine; line <= lastLine; ++line) {
        if (uncomment) {
            const Scintilla::sptr_t hit = lineStartsWithPrefix(line);
            if (hit < 0) continue;
            // Remove "//" — and the following space if present.
            const std::string lineComm = lang->commentLine;
            const Scintilla::sptr_t spaceAfter = hit + static_cast<Scintilla::sptr_t>(lineComm.size());
            const char follow = static_cast<char>(sci(ed, Message::GetCharAt,
                static_cast<Scintilla::uptr_t>(spaceAfter)));
            const Scintilla::sptr_t removeLen =
                static_cast<Scintilla::sptr_t>(lineComm.size())
                + (follow == ' ' ? 1 : 0);
            sci(ed, Message::DeleteRange, static_cast<Scintilla::uptr_t>(hit),
                removeLen);
        } else {
            const Scintilla::sptr_t ls = sci(ed, Message::PositionFromLine,
                                             static_cast<Scintilla::uptr_t>(line));
            const Scintilla::sptr_t le = sci(ed, Message::GetLineEndPosition,
                                             static_cast<Scintilla::uptr_t>(line));
            if (le == ls) continue;             // skip empty lines
            scis(ed, Message::InsertText, static_cast<Scintilla::uptr_t>(ls),
                 prefix.c_str());
        }
    }
    sci(ed, Message::EndUndoAction);
}

void MainWindow::onEditStreamComment()
{
    Buffer* b = activePane()->currentBuffer();
    if (!b) return;
    auto* ed = b->editor();
    const LanguageDef* lang = b->language();
    if (!lang || lang->commentStart.empty() || lang->commentEnd.empty()) {
        statusBar()->showMessage(
            tr("Stream Comment: no block-comment delimiters for this language."),
            4000);
        return;
    }

    const Scintilla::sptr_t selStart = sci(ed, Message::GetSelectionStart);
    const Scintilla::sptr_t selEnd   = sci(ed, Message::GetSelectionEnd);
    if (selEnd <= selStart) {
        statusBar()->showMessage(
            tr("Stream Comment: select text first."), 4000);
        return;
    }

    // Insert end first so selStart isn't shifted by the start-insertion's
    // length — both inserts then land at the byte offsets the user sees.
    sci(ed, Message::BeginUndoAction);
    scis(ed, Message::InsertText,
         static_cast<Scintilla::uptr_t>(selEnd),
         lang->commentEnd.c_str());
    scis(ed, Message::InsertText,
         static_cast<Scintilla::uptr_t>(selStart),
         lang->commentStart.c_str());
    sci(ed, Message::EndUndoAction);
}

// -----------------------------------------------------------------------------
// Search menu — Go to Line + Bookmarks
// -----------------------------------------------------------------------------

void MainWindow::onSearchGoToLine()
{
    auto* ed = currentEditor(activePane());
    if (!ed) return;
    const Scintilla::sptr_t lineCount = sci(ed, Message::GetLineCount);
    const Scintilla::sptr_t cur = sci(ed, Message::LineFromPosition,
        static_cast<Scintilla::uptr_t>(sci(ed, Message::GetCurrentPos))) + 1;

    bool ok = false;
    const int target = QInputDialog::getInt(this, tr("Go to Line"),
        tr("Line number (1–%1):").arg(lineCount),
        static_cast<int>(cur), 1, static_cast<int>(lineCount), 1, &ok);
    if (!ok) return;
    sci(ed, Message::EnsureVisibleEnforcePolicy,
        static_cast<Scintilla::uptr_t>(target - 1));
    sci(ed, Message::GotoLine, static_cast<Scintilla::uptr_t>(target - 1));
    sci(ed, Message::VerticalCentreCaret);
}

void MainWindow::onSearchToggleBookmark()
{
    auto* ed = currentEditor(activePane());
    if (!ed) return;
    const Scintilla::sptr_t pos  = sci(ed, Message::GetCurrentPos);
    const Scintilla::sptr_t line = sci(ed, Message::LineFromPosition,
                                       static_cast<Scintilla::uptr_t>(pos));
    const Scintilla::sptr_t mask = sci(ed, Message::MarkerGet,
                                       static_cast<Scintilla::uptr_t>(line));
    if (mask & kBookmarkMask) {
        sci(ed, Message::MarkerDelete, static_cast<Scintilla::uptr_t>(line),
            kBookmarkMarker);
    } else {
        sci(ed, Message::MarkerAdd, static_cast<Scintilla::uptr_t>(line),
            kBookmarkMarker);
    }
}

void MainWindow::onSearchNextBookmark()
{
    auto* ed = currentEditor(activePane());
    if (!ed) return;
    const Scintilla::sptr_t cur  = sci(ed, Message::LineFromPosition,
        static_cast<Scintilla::uptr_t>(sci(ed, Message::GetCurrentPos)));
    Scintilla::sptr_t found = sci(ed, Message::MarkerNext,
        static_cast<Scintilla::uptr_t>(cur + 1), kBookmarkMask);
    if (found < 0) {
        // Wrap to top.
        found = sci(ed, Message::MarkerNext, 0, kBookmarkMask);
    }
    if (found < 0) {
        statusBar()->showMessage(tr("No bookmarks."), 2500);
        return;
    }
    sci(ed, Message::EnsureVisibleEnforcePolicy,
        static_cast<Scintilla::uptr_t>(found));
    sci(ed, Message::GotoLine, static_cast<Scintilla::uptr_t>(found));
    sci(ed, Message::VerticalCentreCaret);
}

void MainWindow::onSearchPreviousBookmark()
{
    auto* ed = currentEditor(activePane());
    if (!ed) return;
    const Scintilla::sptr_t cur  = sci(ed, Message::LineFromPosition,
        static_cast<Scintilla::uptr_t>(sci(ed, Message::GetCurrentPos)));
    Scintilla::sptr_t found = sci(ed, Message::MarkerPrevious,
        static_cast<Scintilla::uptr_t>(cur - 1), kBookmarkMask);
    if (found < 0) {
        // Wrap to bottom.
        const Scintilla::sptr_t last = sci(ed, Message::GetLineCount) - 1;
        found = sci(ed, Message::MarkerPrevious,
                    static_cast<Scintilla::uptr_t>(last), kBookmarkMask);
    }
    if (found < 0) {
        statusBar()->showMessage(tr("No bookmarks."), 2500);
        return;
    }
    sci(ed, Message::EnsureVisibleEnforcePolicy,
        static_cast<Scintilla::uptr_t>(found));
    sci(ed, Message::GotoLine, static_cast<Scintilla::uptr_t>(found));
    sci(ed, Message::VerticalCentreCaret);
}

void MainWindow::onSearchClearBookmarks()
{
    auto* ed = currentEditor(activePane());
    if (!ed) return;
    sci(ed, Message::MarkerDeleteAll, kBookmarkMarker);
    statusBar()->showMessage(tr("All bookmarks cleared."), 2500);
}

// -----------------------------------------------------------------------------
// View menu — Fold / Whitespace / Indent guides
// -----------------------------------------------------------------------------

void MainWindow::onViewFoldAll()
{
    if (auto* ed = currentEditor(activePane()))
        sci(ed, Message::FoldAll, SC_FOLDACTION_CONTRACT);
}

void MainWindow::onViewUnfoldAll()
{
    if (auto* ed = currentEditor(activePane()))
        sci(ed, Message::FoldAll, SC_FOLDACTION_EXPAND);
}

void MainWindow::onViewToggleWhitespace()
{
    auto* ed = currentEditor(activePane());
    if (!ed) return;
    const Scintilla::sptr_t cur = sci(ed, Message::GetViewWS);
    sci(ed, Message::SetViewWS, cur == 0 ? SCWS_VISIBLEALWAYS : 0);
}

void MainWindow::onViewToggleIndentGuides()
{
    auto* ed = currentEditor(activePane());
    if (!ed) return;
    const Scintilla::sptr_t cur = sci(ed, Message::GetIndentationGuides);
    sci(ed, Message::SetIndentationGuides, cur == 0 ? SC_IV_LOOKBOTH : 0);
}

// -----------------------------------------------------------------------------
// Phase 5b — Tools → Hash dialog
// -----------------------------------------------------------------------------

namespace {

void openHashDialog(MainWindow* w, EditorTabs* tabs,
                    QCryptographicHash::Algorithm algo)
{
    // Modal dialog — short-lived; freshly constructed each invocation so the
    // input always starts empty.
    HashDialog dlg(tabs, w);
    dlg.setAlgorithm(algo);
    dlg.exec();
}

} // namespace

void MainWindow::onToolsHashMd5()    { openHashDialog(this, activePane(),QCryptographicHash::Md5);    }
void MainWindow::onToolsHashSha1()   { openHashDialog(this, activePane(),QCryptographicHash::Sha1);   }
void MainWindow::onToolsHashSha256() { openHashDialog(this, activePane(),QCryptographicHash::Sha256); }
void MainWindow::onToolsHashSha512() { openHashDialog(this, activePane(),QCryptographicHash::Sha512); }

// -----------------------------------------------------------------------------
// Phase 5c — Encoding menu
// -----------------------------------------------------------------------------

namespace {

bool confirmEncodingReload(MainWindow* w, Buffer* b)
{
    if (!b->isDirty()) return true;
    const auto reply = QMessageBox::question(w,
        QObject::tr("Reload as different encoding"),
        QObject::tr("'%1' has unsaved changes that will be discarded if it's "
                    "re-read from disk in a different encoding. Continue?")
            .arg(b->displayName()),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    return reply == QMessageBox::Yes;
}

void convertTo(EditorTabs* tabs, const EncodingInfo& enc)
{
    if (Buffer* b = tabs->currentBuffer()) b->convertToEncoding(enc);
}

EncodingInfo encAnsi()    { return {QStringLiteral("windows-1252"), false}; }
EncodingInfo encUtf8()    { return {QStringLiteral("UTF-8"),        false}; }
EncodingInfo encUtf8Bom() { return {QStringLiteral("UTF-8"),        true};  }
EncodingInfo encUtf16Be() { return {QStringLiteral("UTF-16BE"),     true};  }
EncodingInfo encUtf16Le() { return {QStringLiteral("UTF-16LE"),     true};  }

} // namespace

void MainWindow::reloadCurrentBufferAs(const EncodingInfo& enc)
{
    Buffer* b = activePane()->currentBuffer();
    if (!b) return;
    if (!confirmEncodingReload(this, b)) return;
    QString err;
    if (!b->reloadAsEncoding(enc, &err)) {
        QMessageBox::warning(this, tr("Reload failed"),
            tr("Could not re-read the file: %1").arg(err));
    }
}

void MainWindow::onEncodingConvertAnsi()    { convertTo(activePane(),encAnsi());    }
void MainWindow::onEncodingConvertUtf8()    { convertTo(activePane(),encUtf8());    }
void MainWindow::onEncodingConvertUtf8Bom() { convertTo(activePane(),encUtf8Bom()); }
void MainWindow::onEncodingConvertUtf16Be() { convertTo(activePane(),encUtf16Be()); }
void MainWindow::onEncodingConvertUtf16Le() { convertTo(activePane(),encUtf16Le()); }

void MainWindow::onBufferEncodingChanged(Buffer* buffer)
{
    if (buffer == activePane()->currentBuffer()) {
        updateEncodingStatus();
    }
}

// -----------------------------------------------------------------------------
// Phase 5d — File → Reload from Disk + Format (EOL) menu
// -----------------------------------------------------------------------------

void MainWindow::onFileReloadFromDisk()
{
    Buffer* b = activePane()->currentBuffer();
    if (!b) return;
    if (!b->hasFile()) {
        statusBar()->showMessage(
            tr("Nothing to reload — this buffer has no file on disk."), 3000);
        return;
    }
    if (b->isDirty()) {
        const auto reply = QMessageBox::question(this, tr("Reload from disk"),
            tr("'%1' has unsaved changes. Reload from disk and discard them?")
                .arg(b->displayName()),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (reply != QMessageBox::Yes) return;
    }

    // Snapshot caret + scroll position so the reload feels like "refresh"
    // rather than "jump to top." loadFromFile() resets the caret to 0.
    auto* ed = b->editor();
    const Scintilla::sptr_t pos = ed->send(
        static_cast<unsigned int>(Message::GetCurrentPos));
    const Scintilla::sptr_t firstVis = ed->send(
        static_cast<unsigned int>(Message::GetFirstVisibleLine));

    QString err;
    if (!b->loadFromFile(b->filePath(), &err)) {
        QMessageBox::warning(this, tr("Reload failed"),
            tr("Could not re-read %1:\n%2").arg(b->filePath(), err));
        return;
    }

    const Scintilla::sptr_t length = ed->send(
        static_cast<unsigned int>(Message::GetLength));
    const Scintilla::sptr_t safePos = pos < length ? pos : length;
    ed->send(static_cast<unsigned int>(Message::GotoPos),
             static_cast<Scintilla::uptr_t>(safePos));
    ed->send(static_cast<unsigned int>(Message::SetFirstVisibleLine),
             static_cast<Scintilla::uptr_t>(firstVis));

    statusBar()->showMessage(tr("Reloaded %1").arg(b->displayName()), 3000);
}

void MainWindow::onEolSetWindows()
{
    if (auto* b = activePane()->currentBuffer()) b->setEolMode(Buffer::EolMode::Crlf);
}
void MainWindow::onEolSetUnix()
{
    if (auto* b = activePane()->currentBuffer()) b->setEolMode(Buffer::EolMode::Lf);
}
void MainWindow::onEolSetMac()
{
    if (auto* b = activePane()->currentBuffer()) b->setEolMode(Buffer::EolMode::Cr);
}
void MainWindow::onEolConvertWindows()
{
    if (auto* b = activePane()->currentBuffer()) b->convertEol(Buffer::EolMode::Crlf);
}
void MainWindow::onEolConvertUnix()
{
    if (auto* b = activePane()->currentBuffer()) b->convertEol(Buffer::EolMode::Lf);
}
void MainWindow::onEolConvertMac()
{
    if (auto* b = activePane()->currentBuffer()) b->convertEol(Buffer::EolMode::Cr);
}

void MainWindow::onBufferEolModeChanged(Buffer* buffer)
{
    if (buffer == activePane()->currentBuffer()) {
        updateEolStatus();
    }
}

// -----------------------------------------------------------------------------
// Phase 5e — Run command (F5)
// -----------------------------------------------------------------------------

namespace {

QString substituteRunMacros(const QString& cmd, Buffer* b)
{
    const QString full = (b && b->hasFile()) ? b->filePath() : QString{};
    const QFileInfo fi(full);
    QString out = cmd;
    out.replace(QStringLiteral("$(FULL_CURRENT_PATH)"), full);
    out.replace(QStringLiteral("$(CURRENT_DIRECTORY)"),
                full.isEmpty() ? QDir::currentPath() : fi.absolutePath());
    out.replace(QStringLiteral("$(FILE_NAME)"),
                b ? b->displayName() : QString{});
    // completeBaseName() = "foo.tar" for "foo.tar.gz" — matches upstream's
    // NAME_PART (everything up to but not including the final dot).
    out.replace(QStringLiteral("$(NAME_PART)"), fi.completeBaseName());
    out.replace(QStringLiteral("$(EXT_PART)"),  fi.suffix());
    return out;
}

} // namespace

void MainWindow::onRunCommand()
{
    RunDialog dlg(this);
    dlg.setInitialCommand(m_lastRunCommand);
    connect(&dlg, &RunDialog::saveStubMessage, this,
            [this](const QString& m){ statusBar()->showMessage(m, 4000); });
    if (dlg.exec() != QDialog::Accepted) return;

    const QString raw = dlg.command();
    if (raw.trimmed().isEmpty()) return;
    m_lastRunCommand = raw;

    Buffer* b = activePane()->currentBuffer();
    const QString cmd = substituteRunMacros(raw, b);
    const QString workdir = (b && b->hasFile())
        ? QFileInfo(b->filePath()).absolutePath()
        : QDir::currentPath();

    // Parented to MainWindow → reaped on app exit. Going through /bin/sh -c
    // so the user can pipe / redirect / chain just like a terminal command.
    auto* proc = new QProcess(this);
    proc->setWorkingDirectory(workdir);
    proc->setProcessChannelMode(QProcess::ForwardedChannels);

    connect(proc, &QProcess::errorOccurred, this,
            [this, proc](QProcess::ProcessError) {
        statusBar()->showMessage(tr("Run failed: %1").arg(proc->errorString()),
                                 6000);
        proc->deleteLater();
    });
    connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, proc](int code, QProcess::ExitStatus st) {
        const QString msg = (st == QProcess::CrashExit)
            ? tr("Run crashed (signal %1)").arg(code)
            : tr("Run completed (exit %1)").arg(code);
        statusBar()->showMessage(msg, 6000);
        proc->deleteLater();
    });

    statusBar()->showMessage(tr("Running: %1").arg(cmd), 3000);
    proc->start(QStringLiteral("/bin/sh"),
                {QStringLiteral("-c"), cmd});
}

// =============================================================================
// Phase 5Z — Macro menu slots.
// =============================================================================

void MainWindow::onMacroStartRecording()
{
    Buffer* b = activePane()->currentBuffer();
    if (!b) {
        statusBar()->showMessage(tr("No buffer to record into."), 3000);
        return;
    }
    m_macros->start(b->editor());
    statusBar()->showMessage(tr("Recording macro... (Ctrl+Shift+R to stop)"),
                             3000);
}

void MainWindow::onMacroStopRecording()
{
    if (!m_macros->isRecording()) return;
    m_macros->stop();
    const int n = static_cast<int>(m_macros->lastMacro().size());
    if (n == 0) {
        statusBar()->showMessage(
            tr("Recording stopped (empty — no ops captured)."), 4000);
    } else {
        statusBar()->showMessage(
            tr("Recording stopped — %1 op(s) captured.").arg(n), 4000);
    }
}

void MainWindow::onMacroPlayLast()
{
    Buffer* b = activePane()->currentBuffer();
    if (!b || !m_macros->hasLastMacro()) return;
    m_macros->playLast(b->editor(), 1);
}

void MainWindow::onMacroRunMultipleTimes()
{
    Buffer* b = activePane()->currentBuffer();
    if (!b || !m_macros->hasLastMacro()) return;

    bool ok = false;
    const int count = QInputDialog::getInt(
        this, tr("Run a Macro Multiple Times"),
        tr("Number of times to run the last macro:"),
        1, 1, 10000, 1, &ok);
    if (!ok || count <= 0) return;

    m_macros->playLast(b->editor(), count);
    statusBar()->showMessage(
        tr("Replayed last macro %1 time(s).").arg(count), 4000);
}

void MainWindow::onMacroSaveCurrentRecorded()
{
    if (!m_macros->hasLastMacro()) return;
    bool ok = false;
    const QString name = QInputDialog::getText(
        this, tr("Save Macro"),
        tr("Macro name:"), QLineEdit::Normal,
        QString(), &ok).trimmed();
    if (!ok) return;
    if (name.isEmpty()) {
        QMessageBox::warning(this, tr("Save Macro"),
            tr("A non-empty name is required."));
        return;
    }
    if (m_macros->hasSaved(name)) {
        const auto resp = QMessageBox::question(this, tr("Save Macro"),
            tr("A macro named \"%1\" already exists. Overwrite?").arg(name));
        if (resp != QMessageBox::Yes) return;
    }
    m_macros->saveLast(name);
    Config::save();
    statusBar()->showMessage(tr("Macro \"%1\" saved.").arg(name), 4000);
}

void MainWindow::onMacroPlaySaved()
{
    auto* a = qobject_cast<QAction*>(sender());
    if (!a) return;
    const QString name = a->data().toString();
    if (name.isEmpty()) return;
    Buffer* b = activePane()->currentBuffer();
    if (!b) return;
    m_macros->playSaved(name, b->editor(), 1);
}

void MainWindow::onMacroRecordingStateChanged(bool recording)
{
    if (m_actMacroStart)    m_actMacroStart->setEnabled(!recording);
    if (m_actMacroStop)     m_actMacroStop->setEnabled(recording);

    const bool canPlay = !recording && m_macros && m_macros->hasLastMacro();
    if (m_actMacroPlayLast) m_actMacroPlayLast->setEnabled(canPlay);
    if (m_actMacroRunMulti) m_actMacroRunMulti->setEnabled(canPlay);
    if (m_actMacroSave)     m_actMacroSave->setEnabled(canPlay);
    if (m_savedMacrosMenu)  m_savedMacrosMenu->setEnabled(!recording);

    if (recording) {
        statusBar()->showMessage(tr("Recording started."), 2000);
    }
}

void MainWindow::onMacroLastChanged()
{
    // Re-evaluate enabled state in case the user just landed a new
    // last-macro stash.
    onMacroRecordingStateChanged(m_macros && m_macros->isRecording());
}

void MainWindow::onMacroSavedListChanged()
{
    rebuildSavedMacrosMenu();
    if (m_savedMacrosMenu) {
        const bool any = m_savedMacrosMenu->actions().size() > 0;
        m_savedMacrosMenu->setEnabled(
            any && !(m_macros && m_macros->isRecording()));
    }
}

void MainWindow::rebuildSavedMacrosMenu()
{
    if (!m_savedMacrosMenu) return;
    m_savedMacrosMenu->clear();
    const QStringList names = m_macros ? m_macros->savedNames() : QStringList();
    if (names.isEmpty()) {
        auto* placeholder = m_savedMacrosMenu->addAction(tr("(none)"));
        placeholder->setEnabled(false);
        return;
    }
    for (const QString& name : names) {
        QAction* a = m_savedMacrosMenu->addAction(name);
        a->setData(name);
        connect(a, &QAction::triggered, this, &MainWindow::onMacroPlaySaved);
    }
    // Phase 9d.3 — right-click on a saved macro entry → Rename / Delete.
    // QMenu's setContextMenuPolicy(CustomContextMenu) routes right-clicks
    // to a custom signal carrying the local position; QMenu::actionAt()
    // resolves it back to the action under the cursor.
    m_savedMacrosMenu->setContextMenuPolicy(Qt::CustomContextMenu);
    // Disconnect any prior wiring so rebuilds don't stack handlers.
    disconnect(m_savedMacrosMenu, &QMenu::customContextMenuRequested,
               this, nullptr);
    connect(m_savedMacrosMenu, &QMenu::customContextMenuRequested,
            this, &MainWindow::onMacroSavedContextMenu);
}

void MainWindow::onMacroSavedContextMenu(const QPoint& localPos)
{
    if (!m_savedMacrosMenu || !m_macros) return;
    QAction* under = m_savedMacrosMenu->actionAt(localPos);
    if (!under) return;
    const QString name = under->data().toString();
    if (name.isEmpty()) return;   // (none) placeholder

    QMenu ctx(this);
    QAction* aRename = ctx.addAction(tr("Rename..."));
    QAction* aDelete = ctx.addAction(tr("Delete"));

    QAction* picked = ctx.exec(m_savedMacrosMenu->mapToGlobal(localPos));
    if (!picked) return;

    if (picked == aRename) {
        bool ok = false;
        const QString newName = QInputDialog::getText(this,
            tr("Rename Macro"),
            tr("New name for macro \"%1\":").arg(name),
            QLineEdit::Normal, name, &ok).trimmed();
        if (!ok || newName.isEmpty() || newName == name) return;
        if (m_macros->hasSaved(newName)) {
            QMessageBox::warning(this, tr("Rename Macro"),
                tr("A saved macro named \"%1\" already exists.").arg(newName));
            return;
        }
        if (m_macros->renameSaved(name, newName)) {
            Config::save();
            statusBar()->showMessage(
                tr("Renamed macro \"%1\" → \"%2\".").arg(name, newName), 4000);
        }
    } else if (picked == aDelete) {
        const auto reply = QMessageBox::question(this, tr("Delete Macro"),
            tr("Delete saved macro \"%1\"?").arg(name),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (reply != QMessageBox::Yes) return;
        m_macros->deleteSaved(name);
        Config::save();
        statusBar()->showMessage(
            tr("Deleted macro \"%1\".").arg(name), 4000);
    }
    // m_savedMacrosMenu is closed by exec() — onMacroSavedListChanged
    // will rebuild it on the savedMacrosChanged signal from MacroManager.
}

// =============================================================================
// Phase 7d — Settings -> UI Language switch.
// =============================================================================

void MainWindow::onUiLanguageSelected()
{
    auto* a = qobject_cast<QAction*>(sender());
    if (!a) return;
    const QString payload = a->data().toString();   // "" = English default
    Localization::setActive(payload);
    Localization::applyToMenuBar(menuBar());
    // Phase 8b-polish — re-apply to any already-built dialogs / docks
    // so language switches take effect without restart. New dialogs
    // pick up the active language at construction time.
    if (m_findReplace) Localization::applyToDialog(m_findReplace, "Find");
    if (payload.isEmpty()) {
        statusBar()->showMessage(
            tr("UI language reset to English."), 4000);
    } else {
        statusBar()->showMessage(
            tr("UI language switched to \"%1\".").arg(payload), 4000);
    }
}

// =============================================================================
// Phase 3d — Split view: pane plumbing, focus tracking, tab context menu.
// =============================================================================

bool MainWindow::rightPaneVisible() const
{
    return m_panes[1] && m_panes[1]->isVisible();
}

QVector<Buffer*> MainWindow::allBuffers() const
{
    QVector<Buffer*> out;
    for (int p = 0; p < 2; ++p) {
        EditorTabs* pane = m_panes[p];
        if (!pane) continue;
        out.reserve(out.size() + pane->bufferCount());
        for (int i = 0; i < pane->bufferCount(); ++i) {
            if (Buffer* b = pane->bufferAt(i)) out.append(b);
        }
    }
    return out;
}

int MainWindow::paneIndexOf(EditorTabs* pane) const
{
    if (pane == m_panes[0]) return 0;
    if (pane == m_panes[1]) return 1;
    return -1;
}

void MainWindow::setActivePane(int idx)
{
    if (idx < 0 || idx > 1) return;
    if (idx == 1 && !rightPaneVisible()) return;   // can't activate hidden pane
    if (idx == m_activePane) return;
    m_activePane = idx;

    // Refresh chrome from the new active pane's buffer. This mirrors the
    // tail of onCurrentBufferChanged but skips the sender-promotion check
    // (we already KNOW which pane is active).
    Buffer* active = activePane()->currentBuffer();
    syncFileWatcher();
    updateCaretStatus();
    updateLanguageStatus();
    updateEncodingStatus();
    updateEolStatus();
    if (m_findReplace) {
        m_findReplace->setEditor(active ? active->editor() : nullptr);
    }
    // Phase 3c.1 — re-target the document map at the new active pane's buffer.
    if (m_documentMap && m_documentMap->isVisible()) {
        m_documentMap->setBuffer(active);
    }
    // Phase 3c.2 — re-target the function list at the new active pane's buffer.
    if (m_functionList && m_functionList->isVisible()) {
        m_functionList->setBuffer(active);
    }
    if (active) {
        setWindowTitle(tr("%1 — padnote--").arg(active->displayName()));
        wireScintillaStatusUpdates(active);
    } else {
        setWindowTitle(tr("padnote--"));
    }
}

void MainWindow::showSplitPane(bool on)
{
    if (!m_panes[1]) return;
    if (on == m_panes[1]->isVisible()) return;

    if (on) {
        if (m_panes[1]->bufferCount() == 0) {
            m_panes[1]->newUntitled();
        }
        m_panes[1]->show();

        // Distribute the splitter evenly when first revealing the second
        // pane, otherwise QSplitter leaves it 0-width and the user thinks
        // toggle did nothing.
        if (m_splitter) {
            const int total = m_splitter->size().width();
            if (total > 0) m_splitter->setSizes({total / 2, total / 2});
        }
    } else {
        m_panes[1]->hide();
        // If the active pane was the right one, fall back to the left.
        if (m_activePane == 1) setActivePane(0);
    }
    if (m_actSplitView) m_actSplitView->setChecked(on);
}

void MainWindow::setSplitVisible(bool on)
{
    showSplitPane(on);
}

void MainWindow::onViewSplitView()
{
    const bool on = m_actSplitView && m_actSplitView->isChecked();
    showSplitPane(on);
    statusBar()->showMessage(
        on ? tr("Split View: ON") : tr("Split View: OFF"),
        2500);
}

void MainWindow::onAppFocusChanged(QWidget* /*old*/, QWidget* now)
{
    if (!now) return;
    // Walk up the parent chain; whichever EditorTabs we hit first is the
    // pane that owns the focused widget.
    for (QWidget* w = now; w; w = w->parentWidget()) {
        if (w == m_panes[0]) { setActivePane(0); return; }
        if (w == m_panes[1] && rightPaneVisible()) { setActivePane(1); return; }
    }
}

void MainWindow::onTabContextMenu(int idx, const QPoint& globalPos)
{
    EditorTabs* sourcePane = qobject_cast<EditorTabs*>(sender());
    if (!sourcePane || idx < 0) return;
    Buffer* b = sourcePane->bufferAt(idx);
    if (!b) return;

    const int srcIdx    = paneIndexOf(sourcePane);
    const int targetIdx = (srcIdx == 0) ? 1 : 0;
    EditorTabs* target  = m_panes[targetIdx];

    QMenu menu(this);
    // Phase 9b.1 — Copy file name / pathname / directory. No-op + status nudge
    // when the buffer has no file on disk yet (untitled).
    QAction* aCopyName = menu.addAction(tr("Copy File Name"));
    QAction* aCopyPath = menu.addAction(tr("Copy File Pathname"));
    QAction* aCopyDir  = menu.addAction(tr("Copy File Directory"));
    menu.addSeparator();
    // Phase 9n — Pin/Unpin toggle. Pinned tabs show a small red-dot icon
    // and refuse the three close paths until unpinned.
    QAction* aPin = menu.addAction(b->isPinned() ? tr("Unpin Tab")
                                                  : tr("Pin Tab"));
    menu.addSeparator();
    QAction* aMove  = menu.addAction(tr("Move to Other View"));
    QAction* aClone = menu.addAction(tr("Clone to Other View"));
    aClone->setEnabled(b->hasFile());     // clone-without-file is meaningless
    menu.addSeparator();
    QAction* aClose = menu.addAction(tr("Close"));

    QAction* picked = menu.exec(globalPos);
    if (!picked) return;

    auto copyOrNudge = [this, b](const QString& what, const QString& text){
        if (!b->hasFile()) {
            statusBar()->showMessage(
                tr("This buffer has no file on disk yet."), 3000);
            return;
        }
        QGuiApplication::clipboard()->setText(text);
        statusBar()->showMessage(
            tr("Copied %1 to clipboard.").arg(what), 3000);
    };

    if (picked == aCopyName) {
        copyOrNudge(tr("file name"),
                    QFileInfo(b->filePath()).fileName());
    } else if (picked == aCopyPath) {
        copyOrNudge(tr("file pathname"),
                    QFileInfo(b->filePath()).absoluteFilePath());
    } else if (picked == aCopyDir) {
        copyOrNudge(tr("file directory"),
                    QFileInfo(b->filePath()).absolutePath());
    } else if (picked == aMove) {
        // Moving promotes Split View on (so the user sees where the tab went).
        if (!rightPaneVisible()) showSplitPane(true);
        Buffer* moved = sourcePane->detachBufferAt(idx);
        if (moved) target->adoptBuffer(moved);
        if (sourcePane->bufferCount() == 0) sourcePane->newUntitled();
        setActivePane(targetIdx);
    } else if (picked == aClone) {
        if (!rightPaneVisible()) showSplitPane(true);
        target->openFile(b->filePath());
        setActivePane(targetIdx);
    } else if (picked == aPin) {
        b->setPinned(!b->isPinned());
        statusBar()->showMessage(
            b->isPinned()
                ? tr("Pinned: %1").arg(b->displayName())
                : tr("Unpinned: %1").arg(b->displayName()),
            2500);
    } else if (picked == aClose) {
        // Phase 9n — pin gate. Same nudge as the other two close paths.
        if (b->isPinned()) {
            statusBar()->showMessage(
                tr("Tab is pinned; unpin first to close."), 3000);
            return;
        }
        if (confirmCloseAt(sourcePane, idx)) {
            sourcePane->dropBufferAt(idx);
            if (sourcePane->bufferCount() == 0) sourcePane->newUntitled();
        }
    }
}

// Phase 9l — drag-tab-between-panes. EditorTabs emits this when the
// user pressed on a tab, dragged out of the bar, and released LMB.
// We check whether the release point lies within the OTHER pane's
// geometry; if yes, perform the cross-pane move (mirrors the existing
// Move-to-Other-View context-menu entry). If no, no-op (status nudge
// optional but kept silent — drag-and-drop with no visual feedback
// shouldn't surface noisy errors on near-misses).
void MainWindow::onTabDraggedOutside(int idx, const QPoint& globalPos)
{
    EditorTabs* sourcePane = qobject_cast<EditorTabs*>(sender());
    if (!sourcePane || idx < 0) return;
    Buffer* b = sourcePane->bufferAt(idx);
    if (!b) return;

    const int srcIdx    = paneIndexOf(sourcePane);
    const int targetIdx = (srcIdx == 0) ? 1 : 0;
    EditorTabs* target  = m_panes[targetIdx];
    if (!target) return;

    // Reveal the right pane on demand so the user sees where the tab
    // landed even when the right pane was hidden.
    if (targetIdx == 1 && !rightPaneVisible()) showSplitPane(true);

    // Map the global release point into the target pane's coords. If it
    // falls outside the pane's rect, the user released over something
    // else (toolbar, status bar, dock) — bail without moving.
    const QPoint local = target->mapFromGlobal(globalPos);
    if (!target->rect().contains(local)) return;

    Buffer* moved = sourcePane->detachBufferAt(idx);
    if (!moved) return;
    target->adoptBuffer(moved);
    if (sourcePane->bufferCount() == 0) sourcePane->newUntitled();
    setActivePane(targetIdx);
    statusBar()->showMessage(
        tr("Moved tab to %1 pane.")
            .arg(targetIdx == 0 ? tr("left") : tr("right")),
        2500);
}

int MainWindow::openFilesInActivePane(const QStringList& files)
{
    int opened = 0;
    EditorTabs* pane = activePane();
    if (!pane) return 0;
    for (const QString& f : files) {
        if (pane->openFile(f)) ++opened;
    }
    return opened;
}

Buffer* MainWindow::adoptCrashRecoveredBuffer(const QByteArray& utf8Bytes)
{
    EditorTabs* pane = activePane();
    if (!pane) return nullptr;
    Buffer* b = pane->newUntitled();
    if (!b) return nullptr;
    auto* ed = b->editor();
    ed->sends(static_cast<unsigned int>(Message::AddText),
              static_cast<Scintilla::uptr_t>(utf8Bytes.size()),
              utf8Bytes.constData());
    ed->send(static_cast<unsigned int>(Message::EmptyUndoBuffer));
    return b;
}

void MainWindow::applyHotExitOverlay(const QVector<Backup::Recovery>& recoveries)
{
    for (const auto& r : recoveries) {
        QFile bak(r.backupPath);
        if (!bak.open(QIODevice::ReadOnly)) continue;
        const QByteArray bytes = bak.readAll();
        bak.close();

        // Find the buffer Session::restore already created at the right
        // tab position. File-bound buffers match by filePath; Untitled
        // buffers match by untitledIndex (locale-independent).
        Buffer* target = nullptr;
        for (int p = 0; p < 2 && !target; ++p) {
            EditorTabs* pane = m_panes[p];
            if (!pane) continue;
            for (int i = 0; i < pane->bufferCount(); ++i) {
                Buffer* candidate = pane->bufferAt(i);
                if (!candidate) continue;
                if (!r.originalPath.isEmpty()) {
                    if (candidate->filePath() == r.originalPath) {
                        target = candidate;
                        break;
                    }
                } else if (r.untitledIndex > 0) {
                    if (!candidate->hasFile()
                        && candidate->untitledIndex() == r.untitledIndex) {
                        target = candidate;
                        break;
                    }
                }
            }
        }

        if (target) {
            // Overlay the dirty snapshot onto the placeholder buffer.
            // ClearAll + AddText replaces content; the deliberate
            // absence of a SetSavePoint call keeps the modified flag
            // set, so the tab shows as dirty.
            auto* ed = target->editor();
            ed->send(static_cast<unsigned int>(Message::ClearAll));
            ed->sends(static_cast<unsigned int>(Message::AddText),
                      static_cast<Scintilla::uptr_t>(bytes.size()),
                      bytes.constData());
            ed->send(static_cast<unsigned int>(Message::EmptyUndoBuffer));
        } else {
            // Defensive fallback: no matching tab (file vanished from
            // disk, or session/backup got out of sync). Spawn a fresh
            // Untitled tab carrying the snapshot so the work isn't lost.
            adoptCrashRecoveredBuffer(bytes);
        }
    }
}

// =============================================================================
// Phase 3c.1 — Document Map dock toggle.
// =============================================================================

void MainWindow::onViewDocumentMap()
{
    const bool wantVisible = m_actDocumentMap && m_actDocumentMap->isChecked();

    if (!m_documentMap) {
        // Lazy-construct on first show. Mirrors FindReplaceDialog's pattern.
        m_documentMap = new DocumentMapDock(this);
        addDockWidget(Qt::RightDockWidgetArea, m_documentMap);
        // When the user closes the dock via the title-bar X, keep our
        // checkable QAction in sync.
        connect(m_documentMap, &QDockWidget::visibilityChanged,
                this, [this](bool vis){
            if (m_actDocumentMap) m_actDocumentMap->setChecked(vis);
            Config::setDocumentMapVisible(vis);
            Config::save();
            // Detach when hidden so we stop painting indicators on the
            // shared document; re-attach when shown again.
            if (!vis && m_documentMap) {
                m_documentMap->setBuffer(nullptr);
            } else if (vis && m_documentMap) {
                Buffer* b = activePane() ? activePane()->currentBuffer()
                                         : nullptr;
                m_documentMap->setBuffer(b);
            }
        });
    }

    m_documentMap->setVisible(wantVisible);
    statusBar()->showMessage(
        wantVisible ? tr("Document Map: ON") : tr("Document Map: OFF"),
        2500);
}

// =============================================================================
// Phase 3c.2 — Function List dock toggle.
// =============================================================================

void MainWindow::onViewFunctionList()
{
    const bool wantVisible = m_actFunctionList && m_actFunctionList->isChecked();

    if (!m_functionList) {
        m_functionList = new FunctionListDock(this);
        addDockWidget(Qt::RightDockWidgetArea, m_functionList);
        connect(m_functionList, &QDockWidget::visibilityChanged,
                this, [this](bool vis){
            if (m_actFunctionList) m_actFunctionList->setChecked(vis);
            Config::setFunctionListVisible(vis);
            Config::save();
            // Detach when hidden so we stop debouncing rebuilds; re-bind
            // on show so the tree is fresh.
            if (!vis && m_functionList) {
                m_functionList->setBuffer(nullptr);
            } else if (vis && m_functionList) {
                Buffer* b = activePane() ? activePane()->currentBuffer()
                                         : nullptr;
                m_functionList->setBuffer(b);
            }
        });
    }

    m_functionList->setVisible(wantVisible);
    statusBar()->showMessage(
        wantVisible ? tr("Function List: ON") : tr("Function List: OFF"),
        2500);
}

// =============================================================================
// Phase 3c.3 — File Browser dock toggle.
// =============================================================================

void MainWindow::onViewFileBrowser()
{
    const bool wantVisible = m_actFileBrowser && m_actFileBrowser->isChecked();

    if (!m_fileBrowser) {
        m_fileBrowser = new FileBrowserDock(this, this);
        addDockWidget(Qt::LeftDockWidgetArea, m_fileBrowser);
        connect(m_fileBrowser, &QDockWidget::visibilityChanged,
                this, [this](bool vis){
            if (m_actFileBrowser) m_actFileBrowser->setChecked(vis);
            Config::setFileBrowserVisible(vis);
            Config::save();
        });
    }

    m_fileBrowser->setVisible(wantVisible);
    statusBar()->showMessage(
        wantVisible ? tr("File Browser: ON") : tr("File Browser: OFF"),
        2500);
}

// =============================================================================
// Phase 3c.4 — Project Panel dock toggle.
// =============================================================================

void MainWindow::onViewProjectPanel()
{
    // Phase 9q — three panels share this slot. The triggering QAction
    // carries its 1-based panel index in its data(); fall back to 1
    // if invoked from a non-action call site (defensive — there are
    // none today but the indirection costs nothing).
    auto* a = qobject_cast<QAction*>(sender());
    const int n = (a ? a->data().toInt() : 1);
    const int idx = (n >= 1 && n <= 3) ? n - 1 : 0;
    const bool wantVisible = m_actProjectPanels[idx]
                           && m_actProjectPanels[idx]->isChecked();

    if (!m_projectPanels[idx]) {
        m_projectPanels[idx] = new ProjectPanelDock(this, n, this);
        addDockWidget(Qt::LeftDockWidgetArea, m_projectPanels[idx]);
        // Auto-load this panel's last-used workspace on first show.
        const QString last = Config::lastWorkspacePath(n);
        if (!last.isEmpty()) m_projectPanels[idx]->loadWorkspace(last);
        // Capture n in the lambda so the visibilityChanged handler
        // routes to the right panel's Config slot.
        connect(m_projectPanels[idx], &QDockWidget::visibilityChanged,
                this, [this, n, idx](bool vis){
            if (m_actProjectPanels[idx])
                m_actProjectPanels[idx]->setChecked(vis);
            Config::setProjectPanelVisible(n, vis);
            Config::save();
        });
    }

    m_projectPanels[idx]->setVisible(wantVisible);
    const QString label = (n == 1)
        ? tr("Project Panel: %1").arg(wantVisible ? tr("ON") : tr("OFF"))
        : tr("Project Panel %1: %2")
              .arg(n).arg(wantVisible ? tr("ON") : tr("OFF"));
    statusBar()->showMessage(label, 2500);
}

// =============================================================================
// Phase 9k — Document Switcher (Ctrl+Tab popup).
//
// Builds the dialog from m_mruBuffers (most-recent-first). On Accept,
// looks up which pane the chosen buffer lives in (it could be either
// after an inter-pane move) and switches to it: setActivePane to that
// pane, setCurrentIndex to the buffer's tab index in that pane.
// =============================================================================

void MainWindow::onViewDocumentSwitcher()
{
    // Snapshot the MRU into a plain Buffer*-list (drop expired QPointers
    // and any dups that slipped through). Two buffers minimum or there's
    // nothing to switch between.
    QVector<Buffer*> mru;
    QSet<Buffer*> seen;
    for (const QPointer<Buffer>& p : m_mruBuffers) {
        Buffer* b = p.data();
        if (!b || seen.contains(b)) continue;
        seen.insert(b);
        mru.append(b);
    }
    // Append any open buffers we haven't seen yet (e.g. opened via CLI,
    // never focused after creation). Walk allBuffers() for completeness.
    for (Buffer* b : allBuffers()) {
        if (b && !seen.contains(b)) {
            mru.append(b);
            seen.insert(b);
        }
    }
    if (mru.size() < 2) {
        statusBar()->showMessage(
            tr("Document Switcher: only one buffer open."), 2500);
        return;
    }

    DocumentSwitcherDialog dlg(mru, this);
    if (dlg.exec() != QDialog::Accepted) return;
    Buffer* chosen = dlg.chosenBuffer();
    if (!chosen) return;

    // Find which pane owns the chosen buffer.
    for (int idx : {0, 1}) {
        EditorTabs* p = m_panes[idx];
        if (!p) continue;
        const int tabIdx = p->indexOfBuffer(chosen);
        if (tabIdx < 0) continue;
        // If the chosen buffer is in the right pane and the right pane
        // is hidden, reveal it before switching (mirrors Move-to-Other-
        // View's promotion).
        if (idx == 1 && !rightPaneVisible()) showSplitPane(true);
        p->setCurrentIndex(tabIdx);
        setActivePane(idx);
        return;
    }
}


