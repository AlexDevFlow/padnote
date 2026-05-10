// FunctionListDock.h — Phase 3c.2: dockable Function List panel.
//
// Renders the active buffer's symbols (functions, classes, headings,
// sections — see FunctionList.h) in a flat QTreeWidget. Double-click a
// row to jump the main editor to that line. The list rebuilds when:
//   1. The bound buffer changes (setBuffer).
//   2. The buffer's content changes (Scintilla `modified` signal,
//      debounced through a 250 ms QTimer so typing bursts don't thrash
//      the regex engine).

#pragma once

#include <QDockWidget>
#include <QPointer>

class Buffer;
class QTimer;
class QTreeWidget;
class ScintillaEditBase;

class FunctionListDock : public QDockWidget {
    Q_OBJECT
public:
    explicit FunctionListDock(QWidget* parent = nullptr);

    // Re-target the dock at a different buffer. nullptr clears the tree.
    void setBuffer(Buffer* buffer);

private slots:
    void onItemActivated(class QTreeWidgetItem* item, int column);
    void onSourceModified();
    void onRebuildTimerFired();

private:
    void rebuildTree();
    void detachFromBuffer();

    QTreeWidget*     m_tree         = nullptr;
    QTimer*          m_rebuildTimer = nullptr;
    QPointer<Buffer> m_target;
};
