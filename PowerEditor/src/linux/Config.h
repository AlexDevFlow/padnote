// Config.h — preferences persisted to
// $XDG_CONFIG_HOME/padnote/padnote--/config.xml.
//
// Schema reuses common conventions for recent-files History and
// GUIConfig elements. Unknown elements are not preserved on write —
// padnote only owns the subset it understands.

#pragma once

#include <QRect>
#include <QString>
#include <QStringList>

namespace Config {

// Load config.xml. Idempotent; safe to call before main_qt.cpp's other
// inits since it doesn't depend on Theme/Languages.
void load();

// Persist current values to config.xml. Creates the directory if needed.
// Called on theme switch, on add-recent-file, and on app close.
void save();

// Cloud sync via config-path redirect. padnote doesn't integrate
// cloud APIs; users redirect the config dir to a path inside their
// Dropbox/Nextcloud/Syncthing folder and the existing sync client
// handles propagation.
//
// Sentinel file: $XDG_CONFIG_HOME/padnote/padnote--/cloud.path
// containing a single line with the absolute target directory.
// The sentinel itself stays local (it does NOT live in the override
// dir) so cross-machine sync doesn't force the override on machines
// that wanted local config.
//
// configFilePath() honours the override; Session.cpp + UserDefineLang.cpp
// route through the same helper so all three xml files redirect
// together.
//
// Empty/missing sentinel = local default (current behaviour;
// backward-compat with all pre-12 installs).
QString cloudConfigDir();
void    setCloudConfigDir(const QString& dir);    // writes the sentinel
void    clearCloudConfigDir();                    // removes the sentinel
QString configFilePath();                         // resolves through override

// ---- Theme ---------------------------------------------------------------
// "Light" or "Dark". Default "Light" if unset.
QString theme();
void    setTheme(const QString& mode);

// ---- Window geometry ----------------------------------------------------
// Returns an empty QRect if not set yet.
QRect   windowGeometry();
void    setWindowGeometry(const QRect& geom);
bool    windowMaximized();
void    setWindowMaximized(bool max);

// ---- Recent files -------------------------------------------------------
QStringList recentFiles();
void        addRecentFile(const QString& path);   // bumps to top, dedups, caps
void        clearRecentFiles();
int         recentFilesMax();                     // default 10
void        setRecentFilesMax(int n);             // clamped 0..50; 0 = disable

// ---- Style overrides (Phase 5P) ----------------------------------------
// Persisted as <GUIConfig name="StyleOverride" key="..." value="..."/>
// children. Loaded on startup BEFORE buffers are constructed so the
// override is in place before the first applyEditorBaseStyles call.
QString styleOverride(const QString& key);            // empty if unset
void    setStyleOverride(const QString& key, const QString& value);
void    clearStyleOverrides();
QStringList styleOverrideKeys();                       // for iteration

// Phase 8a — typed wrappers for per-language style overrides. Keys are
// encoded as `lang:<internalName>:<styleID>:<attr>` in the same flat
// hash, where `<attr>` ∈ {"fg", "bg", "fontStyle", "fontSize"}. Returns
// empty if the override isn't set; setter writes through to the hash
// (caller batches Config::save() after the dialog session).
QString perLanguageStyleOverride(const QString& internalName, int styleID,
                                 const QString& attr);
void    setPerLanguageStyleOverride(const QString& internalName, int styleID,
                                    const QString& attr, const QString& value);
// Removes every `lang:<internalName>:*` row; used by "Reset this
// language" in the Style Configurator.
void    clearPerLanguageOverridesFor(const QString& internalName);

// ---- Shortcut overrides (Phase 5Q) -------------------------------------
// Same flat <GUIConfig name="ShortcutOverride"...> schema as Style
// overrides; key is the menu path ("File / New"), value is the QKeySequence
// in PortableText form.
QString shortcutOverride(const QString& path);         // empty if unset
void    setShortcutOverride(const QString& path, const QString& value);
void    clearShortcutOverrides();

// ---- Smart-edit toggles (Phase 5X / extended in 9c.2) -----------------
// Persisted as <GUIConfig name="SmartEdit" smartHighlight="..."
// bracketAutoPair="..." smartHighlightMatchCase="..."
// smartHighlightWholeWord="..."/>. The two new attributes (Phase 9c.2)
// fine-tune the highlight-all-matches search; both default true so the
// pre-9c behaviour is preserved.
bool smartHighlightEnabled();
void setSmartHighlightEnabled(bool v);
bool bracketAutoPairEnabled();
void setBracketAutoPairEnabled(bool v);
bool smartHighlightMatchCase();
void setSmartHighlightMatchCase(bool v);
bool smartHighlightWholeWord();
void setSmartHighlightWholeWord(bool v);

// ---- Auto-completion auto-trigger (Phase 9i) -------------------------
// Persisted on the existing <GUIConfig name="SmartEdit"/> element with
// two new attributes. autocompleteAutoTrigger=yes makes the popup
// appear automatically once the user has typed N consecutive
// identifier characters; autocompleteTriggerChars sets N (clamped 2..6,
// default 3). Default disabled — opt-in via Preferences. Manual
// Ctrl+Space (Edit → Function and word completion) always works
// regardless of this toggle.
bool autocompleteAutoTrigger();
void setAutocompleteAutoTrigger(bool v);
int  autocompleteTriggerChars();
void setAutocompleteTriggerChars(int n);

// ---- UI language (Phase 7d) --------------------------------------------
// Stored as <GUIConfig name="UiLanguage" value="Italiano"/>. Empty value
// or missing element = English (no overlay).
QString uiLanguage();
void    setUiLanguage(const QString& displayName);

// ---- Document Map dock (Phase 3c.1) ------------------------------------
// Persists whether the View → Show Document Map dock was visible at last
// shutdown. Stored as <GUIConfig name="DocumentMap" visible="yes|no"/>.
bool documentMapVisible();
void setDocumentMapVisible(bool v);

// ---- Function List dock (Phase 3c.2) ----------------------------------
// <GUIConfig name="FunctionList" visible="yes|no"/>. Same shape as Document
// Map; default hidden, omitted from config.xml when false.
bool functionListVisible();
void setFunctionListVisible(bool v);

// ---- File Browser dock (Phase 3c.3) ----------------------------------
// <GUIConfig name="FileBrowser" visible="yes|no" root="/abs/path"/>.
// `root` is empty by default → $HOME at first show. Both fields persist
// only when set.
bool fileBrowserVisible();
void setFileBrowserVisible(bool v);
QString fileBrowserRoot();
void    setFileBrowserRoot(const QString& path);

// ---- Project Panel dock (Phase 3c.4 / 9q) ----------------------------
// Phase 9q — three panels (1/2/3) per upstream parity. Panel 1 reads /
// writes `<GUIConfig name="ProjectPanel" .../>` (the legacy 3c.4 key,
// kept untouched for backward compat); panels 2/3 use `ProjectPanel2`
// / `ProjectPanel3`. `lastWorkspace<N>` is loaded on dock first-show.
//
// The single-arg accessors below are kept as panel-1 wrappers so any
// caller that hasn't migrated to the indexed form still works.
bool projectPanelVisible(int n);            // n in {1, 2, 3}
void setProjectPanelVisible(int n, bool v);
QString lastWorkspacePath(int n);
void    setLastWorkspacePath(int n, const QString& path);

// Legacy aliases — equivalent to passing n=1.
bool projectPanelVisible();
void setProjectPanelVisible(bool v);
QString lastWorkspacePath();
void    setLastWorkspacePath(const QString& path);

// ---- UI visibility (Phase 5N.2) ----------------------------------------
// <GUIConfig name="UiVisibility" toolbar="yes|no" statusbar="yes|no"
//   tabsClosable="yes|no"/>. All default to true; element written when any
// is non-default so first-run config.xml stays tight.
bool showToolBar();
void setShowToolBar(bool v);
bool showStatusBar();
void setShowStatusBar(bool v);
bool tabsClosable();
void setTabsClosable(bool v);

// ---- Editing prefs (Phase 5N.3) ----------------------------------------
// <GUIConfig name="Editing" caretWidth="N" caretBlink="N"/>.
// caretWidth: 1 (default) / 2 / 3 — clamped to 1..3.
// caretBlink: ms (default 500). 0 = no blink. Clamped to 0..2000.
int  caretWidth();
void setCaretWidth(int px);
int  caretBlinkMs();
void setCaretBlinkMs(int ms);

// ---- New Document defaults (Phase 5N.4) -------------------------------
// <GUIConfig name="NewDocument" encoding="UTF-8" hasBom="no" eol="2"
//   language="cpp"/>.
// `encoding` is a Qt codec name (UTF-8, UTF-16BE, windows-1252, ...).
// `hasBom` defaults to no.
// `eol`: 0=Crlf, 1=Cr, 2=Lf (matches Buffer::EolMode enum order).
// `language`: LanguageDef::internalName ("cpp", "python", "normal", ...);
// empty = "Plain text".
QString defaultEncodingName();
bool    defaultEncodingHasBom();
int     defaultEolMode();         // returns 0/1/2 matching Buffer::EolMode
QString defaultLanguage();        // empty = plain text
void    setDefaultEncoding(const QString& name, bool hasBom);
void    setDefaultEolMode(int mode);
void    setDefaultLanguage(const QString& internalName);

// ---- Default open directory (Phase 5N.5) -----------------------------
// <GUIConfig name="DefaultDirectory" path="/abs/path"/>. Empty = Qt's
// "remember last used directory" default (the standard QFileDialog
// behaviour). Non-empty = MainWindow::onFileOpen seeds the dialog with
// this path on every invocation.
QString defaultOpenDirectory();
void    setDefaultOpenDirectory(const QString& path);

// ---- Language indent defaults (Phase 5N.8) ---------------------------
// <GUIConfig name="LanguageIndent" tabWidth="4" useTabs="no"/>.
// tabWidth: 1..16 (default 4). useTabs: false (default — modern editor
// default). Buffer's ctor reads them; Phase 5AB's auto-detect on file
// load may override per-buffer when the sample is conclusive.
int  defaultTabWidth();
void setDefaultTabWidth(int n);
bool defaultUseTabs();
void setDefaultUseTabs(bool v);

// ---- Print header/footer (Phase 5N.10) + syntax highlighting (Phase 5T) -
// + magnification (Phase 5T-polish) ------------------------------------
// <GUIConfig name="Print" header="…" footer="…"
//            syntaxHighlight="yes|no" magnification="-10..+10"/>.
// header/footer default empty; syntaxHighlight defaults true;
// magnification defaults 0 (use the editor's font size as-is).
// Macro substitution at print time:
//   $(FULL_CURRENT_PATH) / $(FILE_NAME) / $(NAME_PART) / $(EXT_PART)
//   $(CURRENT_DATE) / $(CURRENT_TIME)
//   $(PAGE_NUMBER) / $(NB_PAGES)   — Phase 9o (per-page pagination)
QString printHeader();
void    setPrintHeader(const QString& s);
QString printFooter();
void    setPrintFooter(const QString& s);
bool    syntaxHighlightedPrint();
void    setSyntaxHighlightedPrint(bool v);
int     printMagnification();
void    setPrintMagnification(int n);

// ---- Backup snapshot interval (Phase 5N.11) --------------------------
// <GUIConfig name="Backup" intervalSec="N" enabled="yes|no"/>.
// intervalSec: 1..300 (default 10). enabled: true (default).
int  backupIntervalSec();
void setBackupIntervalSec(int n);
bool backupEnabled();
void setBackupEnabled(bool v);

// ---- File watcher (Phase 5N.11) --------------------------------------
// <GUIConfig name="FileWatcher" enabled="yes|no"/>. Default true.
bool fileWatcherEnabled();
void setFileWatcherEnabled(bool v);

// ---- Find in Files exclusions (Phase 9g) -----------------------------
// Stored as <GUIConfig name="FindInFiles" excludeDirs="..."
//   excludeFiles="..."/>. Both space-separated glob lists.
// `excludeDirs` matches against the directory NAME (any path component,
// e.g. ".git" excludes ".git" and "src/.git/"). Default skips common
// VCS / build / dependency dirs. `excludeFiles` matches against the
// file basename. Default empty.
QString findInFilesExcludeDirs();
void    setFindInFilesExcludeDirs(const QString& space_separated_globs);
QString findInFilesExcludeFiles();
void    setFindInFilesExcludeFiles(const QString& space_separated_globs);

// ---- Find / Replace history (Phase 9b.3) -----------------------------
// Persisted as <GUIConfig name="FindHistory"  entries="e1\ne2\n..."/> and
// <GUIConfig name="ReplaceHistory" entries="..."/> — each combo's last
// 10 unique strings (newest first). `\n` separator: the find dialog's
// multi-line search is regex-driven, so plain-text entries don't carry
// embedded newlines in practice.
QStringList findHistory();
void        setFindHistory(const QStringList& entries);
QStringList replaceHistory();
void        setReplaceHistory(const QStringList& entries);

// ---- Per-language indent override (Phase 9f) -------------------------
// <GUIConfig name="LanguageIndentOverride" lang="cpp" tabWidth="2"
//   useTabs="no"/>. Multiple instances allowed (one per language).
// When set, takes precedence over Config::defaultTabWidth /
// defaultUseTabs AND over Phase 5AB's auto-detect on file load —
// "I always want this language at this indent, regardless of how the
// existing file is laid out."
struct LanguageIndent {
    int  tabWidth;
    bool useTabs;
};
bool             hasLanguageIndentOverride(const QString& internalName);
LanguageIndent   languageIndentOverride(const QString& internalName);
void             setLanguageIndentOverride(const QString& internalName,
                                           int tabWidth, bool useTabs);
void             clearLanguageIndentOverride(const QString& internalName);
QStringList      languageIndentOverrideKeys();   // for iteration

// ---- Vertical-edge marker (Phase 9b.4) -------------------------------
// <GUIConfig name="VerticalEdge" enabled="yes|no" column="80"
//   color="#C0C0C0"/>. Defaults: off, column 80, light grey. Buffer's
// ctor reads them; MainWindow's applyEditingPrefsFromConfig pushes
// updates to every open buffer on Preferences Apply.
bool    verticalEdgeEnabled();
void    setVerticalEdgeEnabled(bool v);
int     verticalEdgeColumn();
void    setVerticalEdgeColumn(int n);
QString verticalEdgeColor();             // "#RRGGBB"
void    setVerticalEdgeColor(const QString& hex);

} // namespace Config
