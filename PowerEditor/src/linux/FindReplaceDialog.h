// FindReplaceDialog.h — Phase 3b: dockable Find/Replace dialog.
//
// QDockWidget so it can dock to bottom (default), top, left, or right, or
// be torn off as a floating window. The Find and Replace tabs are
// feature-complete (Match Case, Whole Word, Wrap Around, Normal/Extended/
// Regex search modes, Up/Down direction, status line). The remaining
// tabs (Find in Files, Find in Open Files, Mark) are scaffolded with a
// "deferred to Phase 5" stub — see PORTING_NOTES.md.
//
// Search runs through Scintilla's SCI_SETTARGETSTART/END +
// SCI_SETSEARCHFLAGS + SCI_SEARCHINTARGET + SCI_REPLACETARGET[RE].
// SCFIND_REGEXP delegates to Scintilla's Boost.Regex backend, so no
// extra regex library is needed here.

#pragma once

#include <QByteArray>
#include <QDockWidget>
#include <QString>

class MainWindow;
class ScintillaEditBase;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QRadioButton;
class QTabWidget;
class QTreeWidget;
class QTreeWidgetItem;
class QWidget;

class FindReplaceDialog : public QDockWidget {
    Q_OBJECT
public:
    // Phase 3d — takes MainWindow* (was EditorTabs*) so cross-pane iteration
    // (Find in Open Files, Replace All in Open Documents) can walk both
    // panes via mw->allBuffers().
    explicit FindReplaceDialog(MainWindow* mw, QWidget* parent = nullptr);

    // Switch which editor the next search will target. Call from MainWindow
    // when the active tab changes.
    void setEditor(ScintillaEditBase* editor);

    // Open the dialog on the named tab. If 'prefill' is non-empty, populate
    // the find input with it. Idempotent — calling while open just refocuses.
    void showFindTab(const QString& prefill = {});
    void showReplaceTab(const QString& prefill = {});
    // Phase 9r — open on the Find in Files tab (Search → Find in Files
    // menu). Pre-fills the search input the same way showFindTab does.
    void showFindInFilesTab(const QString& prefill = {});
    // Phase 9r — open on the Mark tab (Search → Mark menu). Pre-fills
    // the mark text input.
    void showMarkTab(const QString& prefill = {});

    // Standalone API used by F3 / Shift+F3 when the dialog is closed.
    // Returns true if a match was found.
    bool findNextStandalone(bool forwards);
    bool hasLastSearch() const { return !m_lastOptions.findWhat.isEmpty(); }

signals:
    // MainWindow forwards these to its status bar (5-second timeout).
    void statusMessage(const QString& msg);

private slots:
    // Find tab buttons
    void onFindNext();
    void onFindPrevious();
    void onFindCount();
    void onFindCloseClicked();

    // Replace tab buttons
    void onReplaceFindNext();
    void onReplaceCurrent();
    void onReplaceAll();
    void onReplaceAllInOpenDocs();
    void onReplaceCloseClicked();

    // Phase 5R — Find in Files / Find in Open Files
    void onFindAllInFiles();
    void onFindAllInOpenFiles();
    void onFifBrowseDirectory();
    void onResultActivated(QTreeWidgetItem* item, int column);

    // Phase 5MK — Mark feature. Five Mark N buttons + Clear all.
    void onMarkN(int n);
    void onClearMarks();

private:
    enum class Mode { Normal, Extended, Regex };

    struct SearchOptions {
        QString findWhat;
        QString replaceWith;
        bool matchCase  = false;
        bool wholeWord  = false;
        bool wrapAround = true;
        bool dirDown    = true;
        Mode mode       = Mode::Normal;
    };

    QWidget* buildFindTab();
    QWidget* buildReplaceTab();
    QWidget* buildFindInFilesTab();      // Phase 5R
    QWidget* buildFindInOpenFilesTab();  // Phase 5R
    QWidget* buildMarkTab();             // Phase 5MK — Mark 1..5
    QWidget* buildStubTab(const QString& message);

    // Phase 5R — runs the search and populates the given results tree.
    // bytes is what to search for; opts.matchCase / opts.wholeWord /
    // opts.mode honoured as in the in-buffer Find. Returns total
    // match count.
    int searchFiles(const QStringList& paths, const SearchOptions& opts,
                    QTreeWidget* into);

    SearchOptions readFindOptions() const;
    SearchOptions readReplaceOptions() const;

    int        scintillaSearchFlags(const SearchOptions& o) const;
    QByteArray expandToBytes(const QString& s, Mode m) const;

    // Returns true on match, false on miss. Selects the match in the editor
    // and emits a status message.
    bool doFindNext(const SearchOptions& o, bool forwards);

    // Replaces the current selection (if it is the most recent match) and
    // advances to the next match.
    void doReplaceCurrent(const SearchOptions& o);

    // Returns count of replacements in the given editor, wrapped in a single
    // undo group. If editor is null, no-op returning 0.
    int doReplaceAllInEditor(ScintillaEditBase* editor, const SearchOptions& o);

    void setStatus(const QString& msg);
    void rememberLastSearch(const SearchOptions& o) { m_lastOptions = o; }

    // Phase 9b.3 — push the current find/replace strings to the combo
    // history (move-to-front, dedupe, cap at 10) and persist via
    // Config::setFindHistory / setReplaceHistory + Config::save().
    // Both find combos (Find tab and Replace tab) stay in sync so the
    // user sees the same history regardless of which tab they opened.
    void pushAndPersistHistory(const SearchOptions& o);

    MainWindow*        m_mw     = nullptr;     // not owned (Phase 3d)
    ScintillaEditBase* m_editor = nullptr;     // current editor; not owned

    QTabWidget* m_innerTabs = nullptr;
    QLabel*     m_statusLabel = nullptr;

    // Find tab
    QComboBox*    m_findCombo       = nullptr;
    QCheckBox*    m_findMatchCase   = nullptr;
    QCheckBox*    m_findWholeWord   = nullptr;
    QCheckBox*    m_findWrapAround  = nullptr;
    QRadioButton* m_findModeNormal  = nullptr;
    QRadioButton* m_findModeExt     = nullptr;
    QRadioButton* m_findModeRegex   = nullptr;
    QRadioButton* m_findDirUp       = nullptr;
    QRadioButton* m_findDirDown     = nullptr;

    // Replace tab
    QComboBox*    m_repFindCombo    = nullptr;
    QComboBox*    m_repWithCombo    = nullptr;
    QCheckBox*    m_repMatchCase    = nullptr;
    QCheckBox*    m_repWholeWord    = nullptr;
    QCheckBox*    m_repWrapAround   = nullptr;
    QRadioButton* m_repModeNormal   = nullptr;
    QRadioButton* m_repModeExt      = nullptr;
    QRadioButton* m_repModeRegex    = nullptr;
    QRadioButton* m_repDirUp        = nullptr;
    QRadioButton* m_repDirDown      = nullptr;

    SearchOptions m_lastOptions;   // for F3 / Shift+F3 with dialog closed

    // Find in Files tab (Phase 5R; exclusions Phase 9g)
    QComboBox*    m_fifFindCombo     = nullptr;
    QLineEdit*    m_fifDirectory     = nullptr;
    QLineEdit*    m_fifFilters       = nullptr;
    QLineEdit*    m_fifExcludeDirs   = nullptr;   // Phase 9g
    QLineEdit*    m_fifExcludeFiles  = nullptr;   // Phase 9g
    QCheckBox*    m_fifRecursive     = nullptr;
    QCheckBox*    m_fifMatchCase     = nullptr;
    QCheckBox*    m_fifWholeWord     = nullptr;
    QRadioButton* m_fifModeNormal    = nullptr;
    QRadioButton* m_fifModeExt       = nullptr;
    QRadioButton* m_fifModeRegex     = nullptr;
    QTreeWidget*  m_fifResults       = nullptr;

    // Find in Open Files tab (Phase 5R)
    QComboBox*    m_fioFindCombo     = nullptr;
    QCheckBox*    m_fioMatchCase     = nullptr;
    QCheckBox*    m_fioWholeWord     = nullptr;
    QRadioButton* m_fioModeNormal    = nullptr;
    QRadioButton* m_fioModeExt       = nullptr;
    QRadioButton* m_fioModeRegex     = nullptr;
    QTreeWidget*  m_fioResults       = nullptr;

    // Mark tab (Phase 5MK).
    QLineEdit*    m_markText         = nullptr;
    QCheckBox*    m_markMatchCase    = nullptr;
    QCheckBox*    m_markWholeWord    = nullptr;
    QPushButton*  m_markButtons[5]   = {nullptr};
    QPushButton*  m_markClearAll     = nullptr;
};
