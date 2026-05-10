// UserDefineDialog.h — Phase 5U.3: in-app editor for User-Defined Languages.
//
// Modal QDialog. Top row: combo to pick the active UDL + New / Rename /
// Delete / Save buttons. Body: a QTabWidget. Five tabs are wired:
//   1 Folder & Default              (5U.3 MVP)
//   2 Keywords Lists                (5U.3 MVP)
//   3 Comment & Number              (5U.3-polish)
//   4 Operators & Delimiters        (5U.3-polish)
//   5 Styles (per-style nesting)    (5U.3-polish-tail.1 dialog UI)
//
// Tab 5 is the per-`<WordsStyle>` nesting editor: a QListWidget of
// the 24 canonical style names (DEFAULT / COMMENTS / KEYWORDS1-8 /
// OPERATORS / FOLDER IN CODE1/2 / FOLDER IN COMMENT / DELIMITERS1-8)
// plus any extras the loaded UDL XML carries. The right pane shows
// 21 nesting checkboxes (8 delimiters + 2 comments + 8 keywords +
// 2 operators + 1 numbers — same set upstream's StylerDlg surfaces).
// Per upstream policy, only COMMENTS / LINE COMMENTS / DELIMITERS1-8
// styles allow nesting; checkboxes are disabled for other style rows.
// Per-style colour editing remains 8a's Style Configurator territory.
//
// Save flow: write the in-memory list back via UserDefineLang::saveAll(),
// then emit `udlsSaved()` so MainWindow can call Languages::reloadUDLs()
// + rebuildLanguageMenu() and re-resolve any open buffer that was using
// a UDL.

#pragma once

#include <QDialog>
#include <QString>
#include <QVector>

#include "UserDefineLang.h"   // UDL struct

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;
class QTabWidget;

class UserDefineDialog : public QDialog {
    Q_OBJECT
public:
    explicit UserDefineDialog(QWidget* parent = nullptr);

signals:
    // Fires after a successful Save. MainWindow connects this to its
    // `onUDLsSaved()` slot, which calls Languages::reloadUDLs() +
    // rebuildLanguageMenu() and walks open buffers to re-resolve any
    // UDL languages by internalName.
    void udlsSaved();

private slots:
    void onSelectionChanged(int row);
    void onNew();
    void onRename();
    void onDelete();
    void onSave();
    void onClose();

    // Tab 1 (Folder & Default) — apply changes to the active UDL.
    void onTab1FieldChanged();

    // Tab 2 (Keywords) — apply changes to the active UDL.
    void onTab2FieldChanged();

    // Tab 3 (Comment & Number) — apply changes to the active UDL.
    void onTab3FieldChanged();

    // Tab 4 (Operators & Delimiters) — apply changes to the active UDL.
    void onTab4FieldChanged();

    // QTableWidget cellChanged signal — only valid hook for in-place edits.
    void onDelimiterCellChanged(int row, int col);

    // Tab 5 (Styles — per-style nesting) — selection + checkbox slots.
    void onStyleListSelectionChanged(int row);
    void onNestingChanged();

private:
    void buildUi();
    QWidget* buildFolderTab();
    QWidget* buildKeywordsTab();
    QWidget* buildCommentNumberTab();
    QWidget* buildOperatorsDelimitersTab();
    QWidget* buildStylesTab();

    // Repopulate the style list for the current UDL (canonical 24 +
    // any extras the loaded XML carries) and refresh the checkbox
    // panel for the active row.
    void loadStylesIntoWidgets();

    // Refresh the nesting checkboxes from the UDLStyle whose name
    // matches the current row in m_stylesList. Disables the
    // checkboxes for styles upstream's policy doesn't permit nesting
    // on (DEFAULT / KEYWORDS / NUMBERS / OPERATORS / FOLDER ...).
    void loadStyleRowIntoWidgets();

    // Refresh the combo from m_udls. Selects `selectIdx` if valid,
    // otherwise the first entry, otherwise no selection.
    void rebuildCombo(int selectIdx);

    // Push the currently-edited UDL's widget state into m_udls[m_currentIdx].
    void writeBackCurrent();

    // Pull m_udls[m_currentIdx] into the widgets (for tab switches /
    // selection changes).
    void loadIntoWidgets();

    // Enable / disable the per-tab controls based on whether a UDL is
    // selected (combo empty after the last delete → all greyed).
    void updateControlsEnabled();

    QComboBox*    m_combo       = nullptr;
    QPushButton*  m_btnNew      = nullptr;
    QPushButton*  m_btnRename   = nullptr;
    QPushButton*  m_btnDelete   = nullptr;
    QPushButton*  m_btnSave     = nullptr;
    QPushButton*  m_btnClose    = nullptr;
    QTabWidget*   m_tabs        = nullptr;

    // Tab 1 — Folder & Default
    QLineEdit*    m_extEdit             = nullptr;
    QCheckBox*    m_caseIgnored         = nullptr;
    QCheckBox*    m_allowFoldOfComments = nullptr;
    QCheckBox*    m_foldCompact         = nullptr;
    QComboBox*    m_forcePureLC         = nullptr;
    QComboBox*    m_decimalSeparator    = nullptr;
    QLineEdit*    m_foldOpen            = nullptr;
    QLineEdit*    m_foldMiddle          = nullptr;
    QLineEdit*    m_foldClose           = nullptr;

    // Tab 2 — Keywords (8 groups). Indexed 0..7 == KEYWORDS1..8.
    QPlainTextEdit* m_kwEdit[8]   = {nullptr, nullptr, nullptr, nullptr,
                                     nullptr, nullptr, nullptr, nullptr};
    QCheckBox*      m_kwPrefix[8] = {nullptr, nullptr, nullptr, nullptr,
                                     nullptr, nullptr, nullptr, nullptr};

    // Tab 3 — Comment & Number. Five comment-marker slots (line opener /
    // continuation / closer + block open / close) packed into
    // keywords[SCE_USER_KWLIST_COMMENTS]; seven number keyword fields each
    // map to one keyword slot.
    QLineEdit*  m_commentLineOpen     = nullptr;
    QLineEdit*  m_commentLineContinue = nullptr;
    QLineEdit*  m_commentLineClose    = nullptr;
    QLineEdit*  m_commentBlockOpen    = nullptr;
    QLineEdit*  m_commentBlockClose   = nullptr;
    QLineEdit*  m_numberPrefix1       = nullptr;
    QLineEdit*  m_numberPrefix2       = nullptr;
    QLineEdit*  m_numberExtras1       = nullptr;
    QLineEdit*  m_numberExtras2       = nullptr;
    QLineEdit*  m_numberSuffix1       = nullptr;
    QLineEdit*  m_numberSuffix2       = nullptr;
    QLineEdit*  m_numberRange         = nullptr;

    // Tab 4 — Operators & Delimiters. Two operator slots map to
    // keywords[SCE_USER_KWLIST_OPERATORS1/2]. The 8x3 delimiter table packs
    // into keywords[SCE_USER_KWLIST_DELIMITERS] via encode/decodeDelimiters.
    QPlainTextEdit* m_operators1      = nullptr;
    QPlainTextEdit* m_operators2      = nullptr;
    QTableWidget*   m_delimitersTable = nullptr;

    // Tab 5 — Styles (per-style nesting). The 21 checkbox count matches
    // upstream's nestingMapper (UserDefineDialog.h:178-198): 8 delimiters,
    // 2 comments, 8 keywords, 2 operators, 1 numbers. The remaining
    // FOLDER-related bits in the SCE_USER_MASK_NESTING_* macro range are
    // not surfaced by upstream UI either, so we round-trip them via the
    // raw `int` in UDLStyle.nesting without dedicated checkboxes.
    QListWidget* m_stylesList            = nullptr;
    QCheckBox*   m_nestingChecks[21]     = {nullptr};

    QVector<UDL> m_udls;
    int          m_currentIdx = -1;

    // True while we're programmatically updating widgets — suppresses
    // the field-changed slot's writeback so we don't clobber the source.
    bool         m_loadingFromModel = false;
};
