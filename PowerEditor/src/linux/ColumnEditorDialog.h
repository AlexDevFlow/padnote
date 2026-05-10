// ColumnEditorDialog.h — Phase 5W: insert text or numeric sequence into
// every line of a rectangular (or multi-line) selection.
//
// Modal QDialog. Two modes:
//   - Text: insert the same string at the start of each selected line.
//   - Numeric sequence: insert "start, start+increment, start+2*increment, …"
//     with optional leading-zero padding so columns align.
//
// Scintilla handles the per-line splitting via SCI_GETSELECTIONS — when
// rectangular-selection mode is on, each line of the rectangle is its
// own sub-selection.

#pragma once

#include <QDialog>

class QLineEdit;
class QRadioButton;
class QSpinBox;
class QCheckBox;
class EditorTabs;

class ColumnEditorDialog : public QDialog {
    Q_OBJECT
public:
    explicit ColumnEditorDialog(EditorTabs* tabs, QWidget* parent = nullptr);

private slots:
    void onApply();

private:
    EditorTabs* m_tabs = nullptr;

    QRadioButton* m_modeText   = nullptr;
    QRadioButton* m_modeNumber = nullptr;
    QLineEdit*    m_textEdit   = nullptr;
    QSpinBox*     m_startSpin  = nullptr;
    QSpinBox*     m_incrSpin   = nullptr;
    QCheckBox*    m_leadingZeros = nullptr;
};
