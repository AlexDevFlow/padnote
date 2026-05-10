#include "DocumentMapDock.h"

#include <QEvent>
#include <QMouseEvent>

#include "Buffer.h"
#include "Scintilla.h"
#include "ScintillaEditBase.h"
#include "ScintillaMessages.h"
#include "ScintillaTypes.h"
#include "Theme.h"          // Phase 9m.1 — viewport indicator colour

using Scintilla::Message;
using Scintilla::Update;

namespace {

// Indicator slot used to paint the viewport-rectangle on the map. Slot 9
// is owned by Phase 5X's smart-highlight; 10 is free.
constexpr int kViewportIndicator = 10;

// Map editor zoom. Scintilla zoom is delta-from-default-points; deeply
// negative makes the font small enough that a typical 1-2k-line file
// fits in the dock without scrolling.
constexpr int kMapZoom = -8;

// Default dock width. The user can drag-resize at runtime.
constexpr int kDockDefaultWidth = 130;

void configureMapEditor(ScintillaEditBase* ed)
{
    if (!ed) return;
    // UTF-8 codepage so SCI_SETDOCPOINTER content renders correctly.
    ed->send(static_cast<unsigned int>(Message::SetCodePage), SC_CP_UTF8, 0);
    // Read-only — the map is for navigation, not editing.
    ed->send(static_cast<unsigned int>(Message::SetReadOnly), 1, 0);
    // No line-number / bookmark / fold margins. The map is content-only.
    for (int margin = 0; margin < 5; ++margin) {
        ed->send(static_cast<unsigned int>(Message::SetMarginWidthN),
                 margin, 0);
    }
    // Hide the caret line (otherwise it shows where the LAST click landed
    // on the map even after focus moves back to the main editor).
    ed->send(static_cast<unsigned int>(Message::SetCaretLineVisible), 0, 0);
    // Tiny font.
    ed->send(static_cast<unsigned int>(Message::SetZoom), kMapZoom, 0);
    // No horizontal scrollbar — wrap visually so long lines don't blow
    // out the dock width. SC_WRAP_WORD = 1 (matches View → Word Wrap).
    ed->send(static_cast<unsigned int>(Message::SetWrapMode), SC_WRAP_WORD, 0);
    // Hide the vertical scrollbar — the map shows the whole document at
    // tiny zoom; scroll-by-mouse on the dock itself is sufficient.
    ed->send(static_cast<unsigned int>(Message::SetVScrollBar), 0, 0);
    ed->send(static_cast<unsigned int>(Message::SetHScrollBar), 0, 0);

    // Configure the viewport indicator (slot 10). Phase 9m.1 reads the
    // colour from the active theme so dark themes don't get the same
    // mid-grey light themes use.
    ed->send(static_cast<unsigned int>(Message::IndicSetStyle),
             kViewportIndicator, INDIC_STRAIGHTBOX);
    ed->send(static_cast<unsigned int>(Message::IndicSetFore),
             kViewportIndicator, Theme::documentMapIndicatorFore());
    ed->send(static_cast<unsigned int>(Message::IndicSetAlpha),
             kViewportIndicator, 60);
    ed->send(static_cast<unsigned int>(Message::IndicSetOutlineAlpha),
             kViewportIndicator, 120);
    ed->send(static_cast<unsigned int>(Message::IndicSetUnder),
             kViewportIndicator, 1);
}

} // namespace

DocumentMapDock::DocumentMapDock(QWidget* parent)
    : QDockWidget(tr("Document Map"), parent)
{
    setObjectName(QStringLiteral("documentMapDock"));
    setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    setFeatures(QDockWidget::DockWidgetClosable
              | QDockWidget::DockWidgetMovable
              | QDockWidget::DockWidgetFloatable);

    m_map = new ScintillaEditBase(this);
    m_map->setMinimumWidth(60);
    m_map->resize(kDockDefaultWidth, m_map->height());
    setWidget(m_map);

    configureMapEditor(m_map);

    // Click in the map → jump the main editor to that line.
    connect(m_map, &ScintillaEditBase::textAreaClicked,
            this, &DocumentMapDock::onMapTextAreaClicked);

    // Phase 9h — drag-to-scroll. The event filter watches the map
    // editor's viewport for mouse press / move / release and
    // continuously scrolls the main editor while the user holds.
    // Scintilla's QAbstractScrollArea routes events to its viewport
    // child, so the filter targets that.
    if (m_map->viewport()) {
        m_map->viewport()->installEventFilter(this);
    }
}

void DocumentMapDock::reapplyTheme()
{
    if (!m_map) return;
    // Phase 9m.1 — re-push only the colour; the rest of the indicator
    // config (style, alpha, under) is identical across themes.
    m_map->send(static_cast<unsigned int>(Message::IndicSetFore),
                kViewportIndicator, Theme::documentMapIndicatorFore());
}

bool DocumentMapDock::eventFilter(QObject* watched, QEvent* event)
{
    // Phase 9h — drag-to-scroll. Only react to events on the map
    // viewport; everything else passes through unchanged.
    if (!m_map || watched != m_map->viewport()) {
        return QDockWidget::eventFilter(watched, event);
    }
    switch (event->type()) {
        case QEvent::MouseButtonPress: {
            auto* me = static_cast<QMouseEvent*>(event);
            if (me->button() == Qt::LeftButton) {
                m_dragging = true;
                scrollMainToMapY(static_cast<int>(me->position().y()));
                // Don't swallow — let Scintilla update its own caret
                // position so the existing click-to-jump cue stays
                // consistent.
            }
            break;
        }
        case QEvent::MouseMove: {
            if (m_dragging) {
                auto* me = static_cast<QMouseEvent*>(event);
                if (me->buttons() & Qt::LeftButton) {
                    scrollMainToMapY(static_cast<int>(me->position().y()));
                }
            }
            break;
        }
        case QEvent::MouseButtonRelease: {
            auto* me = static_cast<QMouseEvent*>(event);
            if (me->button() == Qt::LeftButton) {
                m_dragging = false;
            }
            break;
        }
        default: break;
    }
    return QDockWidget::eventFilter(watched, event);
}

void DocumentMapDock::scrollMainToMapY(int viewportY)
{
    if (!m_target || !m_map) return;
    auto* main = m_target->editor();
    if (!main) return;
    // Convert the y-coordinate in the map's viewport to a document
    // line. Scintilla's PositionFromPoint takes (x, y) and returns
    // the document position; LineFromPosition gives the line.
    const Scintilla::sptr_t pos = m_map->send(
        static_cast<unsigned int>(Message::PositionFromPoint),
        static_cast<Scintilla::uptr_t>(0),    // x=0 → start of line
        static_cast<Scintilla::sptr_t>(viewportY));
    if (pos < 0) return;
    const Scintilla::sptr_t line = m_map->send(
        static_cast<unsigned int>(Message::LineFromPosition),
        static_cast<Scintilla::uptr_t>(pos));
    main->send(static_cast<unsigned int>(Message::EnsureVisibleEnforcePolicy),
               static_cast<Scintilla::uptr_t>(line));
    main->send(static_cast<unsigned int>(Message::GotoLine),
               static_cast<Scintilla::uptr_t>(line));
    main->send(static_cast<unsigned int>(Message::VerticalCentreCaret));
}

void DocumentMapDock::setBuffer(Buffer* buffer)
{
    if (m_target.data() == buffer) return;

    detachFromBuffer();

    m_target = buffer;
    if (!buffer) {
        // Empty document — clear the map by setting an empty doc pointer.
        // Passing 0 makes Scintilla allocate a fresh empty document for the
        // map editor.
        m_map->send(static_cast<unsigned int>(Message::SetReadOnly), 0, 0);
        m_map->send(static_cast<unsigned int>(Message::SetDocPointer), 0, 0);
        m_map->send(static_cast<unsigned int>(Message::SetReadOnly), 1, 0);
        return;
    }

    auto* main = buffer->editor();
    if (!main) return;

    // Share the main editor's document. SCI_GETDOCPOINTER returns an
    // opaque sptr_t (in practice a pointer); SCI_SETDOCPOINTER takes it
    // as lparam. Both editors now hold a reference to the same document;
    // when the map's document pointer is replaced (next setBuffer call),
    // Scintilla decrements the refcount automatically.
    const Scintilla::sptr_t doc =
        main->send(static_cast<unsigned int>(Message::GetDocPointer));
    // Clear read-only briefly so SetDocPointer is allowed (Scintilla
    // refuses to swap docs on a read-only editor in some builds).
    m_map->send(static_cast<unsigned int>(Message::SetReadOnly), 0, 0);
    m_map->send(static_cast<unsigned int>(Message::SetDocPointer),
                0, doc);
    m_map->send(static_cast<unsigned int>(Message::SetReadOnly), 1, 0);

    // Listen for visible-range changes on the main editor.
    connect(main, &ScintillaEditBase::updateUi,
            this, &DocumentMapDock::onMainUpdateUi,
            Qt::UniqueConnection);

    refreshViewportIndicator();
}

void DocumentMapDock::detachFromBuffer()
{
    if (!m_target) return;
    auto* main = m_target->editor();
    if (main) {
        disconnect(main, &ScintillaEditBase::updateUi,
                   this, &DocumentMapDock::onMainUpdateUi);
    }
    // Clear the indicator we drew on the (shared!) document. If we don't,
    // a stale viewport box stays painted when the user re-opens the dock
    // for a different buffer.
    if (m_target) {
        const Scintilla::sptr_t end =
            m_target->editor()->send(
                static_cast<unsigned int>(Message::GetLength));
        m_target->editor()->send(
            static_cast<unsigned int>(Message::SetIndicatorCurrent),
            kViewportIndicator, 0);
        m_target->editor()->send(
            static_cast<unsigned int>(Message::IndicatorClearRange),
            0, end);
    }
    m_target = nullptr;
}

void DocumentMapDock::onMainUpdateUi(Scintilla::Update /*updated*/)
{
    refreshViewportIndicator();
}

void DocumentMapDock::refreshViewportIndicator()
{
    if (!m_target) return;
    auto* main = m_target->editor();
    if (!main) return;

    // Determine the visible doc-line range in the main editor.
    const Scintilla::sptr_t firstVisVis =
        main->send(static_cast<unsigned int>(Message::GetFirstVisibleLine));
    const Scintilla::sptr_t linesOnScreen =
        main->send(static_cast<unsigned int>(Message::LinesOnScreen));
    const Scintilla::sptr_t firstDocLine =
        main->send(static_cast<unsigned int>(Message::DocLineFromVisible),
                   static_cast<Scintilla::uptr_t>(firstVisVis));
    const Scintilla::sptr_t lastDocLine =
        main->send(static_cast<unsigned int>(Message::DocLineFromVisible),
                   static_cast<Scintilla::uptr_t>(firstVisVis + linesOnScreen));

    // Clamp to document bounds.
    const Scintilla::sptr_t lineCount =
        main->send(static_cast<unsigned int>(Message::GetLineCount));
    const Scintilla::sptr_t lo = std::max<Scintilla::sptr_t>(0, firstDocLine);
    const Scintilla::sptr_t hi = std::min<Scintilla::sptr_t>(lineCount, lastDocLine);

    const Scintilla::sptr_t startPos =
        main->send(static_cast<unsigned int>(Message::PositionFromLine),
                   static_cast<Scintilla::uptr_t>(lo));
    const Scintilla::sptr_t endPos =
        main->send(static_cast<unsigned int>(Message::PositionFromLine),
                   static_cast<Scintilla::uptr_t>(hi));
    const Scintilla::sptr_t totalLen =
        main->send(static_cast<unsigned int>(Message::GetLength));

    // The map shares the same document, so painting indicators on the
    // main editor's bytes also paints them on the map. Clear the entire
    // doc range first to wipe the previous viewport, then fill the new
    // one. (Scintilla scopes indicator painting per-document, not per-
    // editor.)
    main->send(static_cast<unsigned int>(Message::SetIndicatorCurrent),
               kViewportIndicator, 0);
    main->send(static_cast<unsigned int>(Message::IndicatorClearRange),
               0, totalLen);
    if (endPos > startPos) {
        main->send(static_cast<unsigned int>(Message::IndicatorFillRange),
                   static_cast<Scintilla::uptr_t>(startPos),
                   endPos - startPos);
    }
}

void DocumentMapDock::onMapTextAreaClicked(Scintilla::Position line,
                                           int /*modifiers*/)
{
    if (!m_target) return;
    auto* main = m_target->editor();
    if (!main) return;
    main->send(static_cast<unsigned int>(Message::EnsureVisibleEnforcePolicy),
               static_cast<Scintilla::uptr_t>(line));
    main->send(static_cast<unsigned int>(Message::GotoLine),
               static_cast<Scintilla::uptr_t>(line));
    main->send(static_cast<unsigned int>(Message::VerticalCentreCaret));
    main->setFocus();
}
