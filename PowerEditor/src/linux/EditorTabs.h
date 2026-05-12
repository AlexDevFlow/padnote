// EditorTabs.h — QTabWidget managing multiple Buffers (one per tab).
//
// Phase 3b: this class is now a low-level container. It does NOT prompt
// the user before closing dirty buffers — that flow is owned by MainWindow,
// which can drive QFileDialog::getSaveFileName when "Save" is chosen.
// EditorTabs emits closeRequested(int) when QTabWidget asks to close a tab,
// and MainWindow connects to that signal, runs the Save/Discard/Cancel
// prompt, and (on proceed) calls dropBufferAt(int) here.
//
// Phase 3d: split view. MainWindow holds TWO EditorTabs in a QSplitter.
// detachBufferAt + adoptBuffer let a tab move between panes without
// destroying its Buffer/editor. tabContextMenuRequested fires on right-
// click; MainWindow constructs the menu (it owns the pane pointers).

#pragma once

#include <QPoint>
#include <QString>
#include <QTabWidget>

class Buffer;

class EditorTabs : public QTabWidget {
    Q_OBJECT
public:
    explicit EditorTabs(QWidget* parent = nullptr);

    Buffer* currentBuffer() const;
    Buffer* bufferAt(int index) const;
    int     bufferCount() const { return count(); }
    int     indexOfBuffer(Buffer* buffer) const;

    Buffer* newUntitled();                          // creates and shows a fresh buffer
    // Like newUntitled but stamps the buffer with a specific untitled
    // index (e.g. restored from session.xml). Bumps the internal
    // counter past `index` so future newUntitled() calls don't collide.
    Buffer* restoreUntitled(int index);
    Buffer* openFile(const QString& path);          // opens or focuses existing
    void    dropBufferAt(int index);                // unconditional close (no prompt)
    void    moveTab(int from, int to);              // exposes QTabBar::moveTab (Phase 5i)

    // Phase 3d — pane move. detachBufferAt removes the tab WITHOUT deleting
    // its Buffer/editor and returns the Buffer (caller owns; should adopt
    // immediately or delete). adoptBuffer takes a Buffer detached from
    // another EditorTabs and inserts it as a fresh tab here.
    Buffer* detachBufferAt(int index);
    void    adoptBuffer(Buffer* buffer);

signals:
    void currentBufferChanged(Buffer* buffer);      // null if no tabs left
    void closeRequested(int index);                 // user clicked the tab × — caller decides

    // Phase 3d — fired on right-click of the tab bar; MainWindow builds
    // the menu (Move/Clone-to-other-view, Close). idx is the tab under
    // the cursor (or -1 for the empty area), globalPos is screen coords.
    void tabContextMenuRequested(int idx, const QPoint& globalPos);

    // Phase 9b.2 — fired from dropBufferAt before the Buffer is deleted,
    // ONLY when the buffer has a file path. MainWindow tracks the path
    // in an in-memory MRU list driving Reopen Recently Closed File.
    // Untitled buffers (no path) are intentionally not surfaced — they
    // have no identity to reopen by.
    void bufferAboutToClose(const QString& path);

    // Phase 9l — fired when the user presses on a tab, drags the cursor
    // outside the tab bar bounds, and releases LMB. MainWindow uses this
    // to support drag-tab-between-panes: if globalPos falls within the
    // OTHER pane's geometry, the tab is moved across panes (detach +
    // adopt, mirroring the existing Move-to-Other-View context-menu
    // entry). Releases inside the bar cancel via QTabBar's own
    // setMovable reorder path; this signal doesn't fire there.
    void tabDraggedOutside(int idx, const QPoint& globalPos);

public:
    // Phase 9l — installed on tabBar(); intercepts LMB press/release so
    // we can detect "user dragged a tab out of the bar." Public to
    // match QObject::eventFilter's access (Qt requires the override
    // not narrow the inherited visibility).
    bool eventFilter(QObject* watched, QEvent* event) override;

private slots:
    void onCurrentChanged(int index);
    void onTabCloseRequested(int index);
    void onBufferDirtyChanged(Buffer* buffer, bool dirty);
    void onBufferDisplayNameChanged(Buffer* buffer);
    void onTabBarContextMenu(const QPoint& localPos);

private:
    void refreshTabLabel(int index);
    void wireBufferSignals(Buffer* b);
    void unwireBufferSignals(Buffer* b);

    int m_nextUntitledIndex = 1;

    // Phase 9l — drag tracking. -1 = not pressed-on-a-tab.
    int m_dragTabIndex = -1;
};
