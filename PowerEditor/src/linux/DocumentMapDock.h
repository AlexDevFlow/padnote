// DocumentMapDock.h — Phase 3c.1: dockable miniature view of the active
// buffer with a viewport-rectangle indicator.
//
// The dock owns a second ScintillaEditBase that shares the active buffer's
// underlying document via SCI_SETDOCPOINTER — edits in the main editor
// reflect immediately in the map without us having to push text. The map
// runs at a very small zoom (kMapZoom) with no margins / no scrollbars and
// is read-only.
//
// Two integrations with the main editor:
//   1. Visible-range indicator. We listen for the main editor's updateUi
//      signal and redraw a translucent box (Scintilla indicator slot
//      kViewportIndicator) over the byte range corresponding to the lines
//      currently visible in the main editor.
//   2. Click-to-jump. The map's textAreaClicked signal carries the doc
//      line under the cursor; we send SCI_GOTOLINE to the main editor and
//      SCI_SCROLLCARET so the user lands at that line.
//
// setBuffer(b) is idempotent — calling it with the currently-bound buffer
// is a no-op. Calling it with nullptr detaches and clears.

#pragma once

#include <QDockWidget>
#include <QPointer>

#include "ScintillaTypes.h"   // Scintilla::Position + Scintilla::Update

class Buffer;
class ScintillaEditBase;

class DocumentMapDock : public QDockWidget {
    Q_OBJECT
public:
    explicit DocumentMapDock(QWidget* parent = nullptr);

    // Bind the dock to a specific buffer. The dock shares the buffer's
    // underlying Scintilla document and listens for its updateUi to keep
    // the viewport indicator in sync. Call with nullptr to detach.
    void setBuffer(Buffer* buffer);

    // Phase 9m.1 — re-push the theme-driven viewport indicator colour.
    // Called by MainWindow on light/dark switch so the map indicator
    // doesn't keep its old-theme colour after the active theme changes.
    void reapplyTheme();

protected:
    // Phase 9h — drag-to-scroll. Watches the map editor's mouse events;
    // a press-and-hold drag continuously scrolls the main editor to
    // follow the line under the cursor. Plain click still goes through
    // the existing textAreaClicked path.
    bool eventFilter(QObject* watched, QEvent* event) override;

private slots:
    void onMainUpdateUi(Scintilla::Update updated);
    void onMapTextAreaClicked(Scintilla::Position line, int modifiers);

private:
    void refreshViewportIndicator();
    void detachFromBuffer();
    // Phase 9h — convert a y-coordinate inside m_map's viewport to a
    // doc line in the underlying buffer, then scroll the main editor
    // to that line (centered, like the click-to-jump path).
    void scrollMainToMapY(int viewportY);

    ScintillaEditBase* m_map = nullptr;
    QPointer<Buffer>   m_target;
    bool               m_dragging = false;   // Phase 9h
};
