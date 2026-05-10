// DocumentSwitcherDialog.h — Phase 9k: modal Ctrl+Tab popup.
//
// Shows the active panes' open buffers in MRU order. The user picks
// one with arrow keys / Tab / mouse and presses Enter to switch to it
// (Esc cancels). MainWindow keeps the MRU; this dialog is just the UI.
//
// Pre-selected row is index 1 (the second-most-recent buffer) so a
// single Ctrl+Tab + Enter cycles to "the previous tab" — matching
// VSCode / Sublime / many other editors.

#pragma once

#include <QDialog>
#include <QVector>

class Buffer;
class QListWidget;

class DocumentSwitcherDialog : public QDialog {
    Q_OBJECT
public:
    // `mru` is the buffer list in most-recent-first order. Buffers are
    // not owned; the dialog only displays their names + paths.
    explicit DocumentSwitcherDialog(const QVector<Buffer*>& mru,
                                    QWidget* parent = nullptr);

    // Returns the buffer the user picked, or nullptr if cancelled.
    // Valid only after exec() returns Accepted.
    Buffer* chosenBuffer() const { return m_chosen; }

protected:
    // Override Tab + Backtab so they navigate the list (default Tab
    // would advance the keyboard focus chain out of the list).
    void keyPressEvent(QKeyEvent* event) override;

private:
    void accept() override;

    QListWidget* m_list   = nullptr;
    Buffer*      m_chosen = nullptr;
};
