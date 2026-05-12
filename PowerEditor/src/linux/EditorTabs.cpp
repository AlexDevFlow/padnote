#include "EditorTabs.h"

#include <QEvent>
#include <QFileInfo>
#include <QIcon>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QTabBar>

#include "Buffer.h"
#include "Config.h"
#include "ScintillaEditBase.h"

namespace {

// Phase 9n — programmatic pin icon. We'd rather draw a tiny QPixmap than
// stuff an emoji into the tab text (CLAUDE.md rule: avoid emojis in
// files; QTabBar's tabIcon slot is the conventional vehicle anyway).
// Cached so every refreshTabLabel doesn't re-rasterise. The colour is
// theme-agnostic — Qt's icon-rendering on dark themes still reads it
// correctly because the dot is fully saturated, not a near-grey.
QIcon pinIcon()
{
    static QIcon cached;
    if (!cached.isNull()) return cached;
    QPixmap pm(12, 12);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setBrush(QColor(220, 70, 60));
    p.setPen(QPen(QColor(120, 30, 30), 1));
    p.drawEllipse(2, 2, 8, 8);
    cached = QIcon(pm);
    return cached;
}

} // namespace

EditorTabs::EditorTabs(QWidget* parent)
    : QTabWidget(parent)
{
    setTabsClosable(true);
    setMovable(true);
    setDocumentMode(true);
    setUsesScrollButtons(true);

    connect(this, &QTabWidget::currentChanged,
            this, &EditorTabs::onCurrentChanged);
    connect(this, &QTabWidget::tabCloseRequested,
            this, &EditorTabs::onTabCloseRequested);

    // Phase 3d — right-click on the tab bar emits a signal MainWindow uses
    // to pop a Move/Clone-to-other-view context menu. tabBar() is protected
    // on QTabWidget but we're a subclass, so we can reach it.
    QTabBar* bar = tabBar();
    bar->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(bar, &QWidget::customContextMenuRequested,
            this, &EditorTabs::onTabBarContextMenu);

    // Phase 9l — drag-tab-between-panes. Watch the bar for press +
    // release events; when the user releases outside the bar after
    // pressing on a tab, emit tabDraggedOutside so MainWindow can
    // route to the cross-pane move flow.
    bar->installEventFilter(this);
}

bool EditorTabs::eventFilter(QObject* watched, QEvent* event)
{
    QTabBar* bar = tabBar();
    if (watched != bar) return QTabWidget::eventFilter(watched, event);

    if (event->type() == QEvent::MouseButtonPress) {
        auto* me = static_cast<QMouseEvent*>(event);
        if (me->button() == Qt::LeftButton) {
            // Resolve the tab under the cursor at press time. -1 means the
            // user clicked an empty bar area; we don't initiate drag in
            // that case.
            m_dragTabIndex = bar->tabAt(me->position().toPoint());
        }
    } else if (event->type() == QEvent::MouseButtonRelease) {
        auto* me = static_cast<QMouseEvent*>(event);
        if (me->button() == Qt::LeftButton && m_dragTabIndex >= 0) {
            const int idx = m_dragTabIndex;
            m_dragTabIndex = -1;
            // Translate the release position into bar-local coords. If it
            // lies outside the bar's rect, the user dragged out — emit.
            const QPoint local = me->position().toPoint();
            if (!bar->rect().contains(local)) {
                emit tabDraggedOutside(idx, me->globalPosition().toPoint());
            }
            // Releases inside the bar fall through; QTabBar's setMovable
            // already handled in-bar reorder.
        }
    }
    return QTabWidget::eventFilter(watched, event);
}

Buffer* EditorTabs::currentBuffer() const
{
    return bufferAt(currentIndex());
}

Buffer* EditorTabs::bufferAt(int index) const
{
    if (index < 0 || index >= count()) return nullptr;
    QWidget* w = widget(index);
    return w ? w->property("buffer").value<Buffer*>() : nullptr;
}

int EditorTabs::indexOfBuffer(Buffer* buffer) const
{
    if (!buffer) return -1;
    for (int i = 0; i < count(); ++i) {
        if (bufferAt(i) == buffer) return i;
    }
    return -1;
}

Buffer* EditorTabs::newUntitled()
{
    auto* buffer = new Buffer(this);
    buffer->setUntitledIndex(m_nextUntitledIndex++);

    // We tag the editor widget with a back-pointer to its Buffer so we can
    // recover the buffer from the tab index without a parallel array.
    ScintillaEditBase* editor = buffer->editor();
    editor->setProperty("buffer", QVariant::fromValue<Buffer*>(buffer));

    wireBufferSignals(buffer);

    const int idx = addTab(editor, buffer->displayName());
    setCurrentIndex(idx);
    return buffer;
}

Buffer* EditorTabs::restoreUntitled(int index)
{
    auto* buffer = new Buffer(this);
    buffer->setUntitledIndex(index);
    if (index >= m_nextUntitledIndex) m_nextUntitledIndex = index + 1;

    ScintillaEditBase* editor = buffer->editor();
    editor->setProperty("buffer", QVariant::fromValue<Buffer*>(buffer));

    wireBufferSignals(buffer);

    const int idx = addTab(editor, buffer->displayName());
    setCurrentIndex(idx);
    return buffer;
}

Buffer* EditorTabs::openFile(const QString& path)
{
    const QString canonical = QFileInfo(path).canonicalFilePath();
    if (!canonical.isEmpty()) {
        for (int i = 0; i < count(); ++i) {
            Buffer* b = bufferAt(i);
            if (b && QFileInfo(b->filePath()).canonicalFilePath() == canonical) {
                setCurrentIndex(i);
                return b;
            }
        }
    }

    auto* buffer = newUntitled();
    QString err;
    if (!buffer->loadFromFile(path, &err)) {
        QMessageBox::warning(this, tr("Open failed"),
            tr("Cannot read %1:\n%2").arg(path, err));
        // Roll back: remove the empty tab we just created.
        const int idx = indexOfBuffer(buffer);
        if (idx >= 0) dropBufferAt(idx);
        return nullptr;
    }
    refreshTabLabel(indexOfBuffer(buffer));
    Config::addRecentFile(path);
    return buffer;
}

void EditorTabs::dropBufferAt(int index)
{
    if (index < 0 || index >= count()) return;
    Buffer* b = bufferAt(index);
    QWidget* w = widget(index);
    // Phase 9b.2 — surface the about-to-close path so MainWindow can push
    // it onto the Reopen Recently Closed MRU. Untitled buffers (no path)
    // are silently skipped: there's nothing to reopen.
    if (b && !b->filePath().isEmpty()) {
        emit bufferAboutToClose(b->filePath());
    }
    removeTab(index);
    if (b) b->deleteLater();
    if (w) w->deleteLater();
}

void EditorTabs::moveTab(int from, int to)
{
    // QTabWidget::tabBar() is protected; expose its public moveTab through
    // EditorTabs so MainWindow's Window-menu sort can reorder tabs without
    // remove/insert dance (which would lose tab state).
    tabBar()->moveTab(from, to);
}

// -----------------------------------------------------------------------------
// Phase 3d — detach / adopt for inter-pane moves. removeTab does NOT delete
// the page widget (Qt docs: "the widget is not deleted") so we just need to
// clear the parent so the source pane's destruction can't take it down. Buffer
// is a QObject parented to EditorTabs at newUntitled time; we re-parent it on
// adopt so the destructor chain is correct.
// -----------------------------------------------------------------------------

Buffer* EditorTabs::detachBufferAt(int index)
{
    if (index < 0 || index >= count()) return nullptr;
    Buffer* b = bufferAt(index);
    QWidget* w = widget(index);
    if (b) unwireBufferSignals(b);
    removeTab(index);
    if (w) w->setParent(nullptr);
    if (b) b->setParent(nullptr);
    return b;
}

void EditorTabs::adoptBuffer(Buffer* buffer)
{
    if (!buffer) return;
    ScintillaEditBase* editor = buffer->editor();
    buffer->setParent(this);                     // Buffer's QObject parent = this pane
    // editor's parent is reset by addTab automatically.
    editor->setProperty("buffer", QVariant::fromValue<Buffer*>(buffer));
    wireBufferSignals(buffer);

    const int idx = addTab(editor, buffer->displayName());
    refreshTabLabel(idx);
    setCurrentIndex(idx);
}

void EditorTabs::wireBufferSignals(Buffer* b)
{
    if (!b) return;
    connect(b, &Buffer::dirtyChanged,
            this, &EditorTabs::onBufferDirtyChanged,
            Qt::UniqueConnection);
    connect(b, &Buffer::displayNameChanged,
            this, &EditorTabs::onBufferDisplayNameChanged,
            Qt::UniqueConnection);
    // Phase 9n — pin toggle re-runs the label refresh so the icon
    // appears / disappears immediately. Reuses the displayName slot
    // because the body just calls refreshTabLabel; the buffer ptr
    // is the meaningful payload.
    connect(b, &Buffer::pinnedChanged,
            this, &EditorTabs::onBufferDisplayNameChanged,
            Qt::UniqueConnection);
}

void EditorTabs::unwireBufferSignals(Buffer* b)
{
    if (!b) return;
    disconnect(b, &Buffer::dirtyChanged,
               this, &EditorTabs::onBufferDirtyChanged);
    disconnect(b, &Buffer::displayNameChanged,
               this, &EditorTabs::onBufferDisplayNameChanged);
    disconnect(b, &Buffer::pinnedChanged,
               this, &EditorTabs::onBufferDisplayNameChanged);
}

void EditorTabs::onCurrentChanged(int index)
{
    emit currentBufferChanged(bufferAt(index));
}

void EditorTabs::onTabCloseRequested(int index)
{
    emit closeRequested(index);
}

void EditorTabs::onBufferDirtyChanged(Buffer* buffer, bool /*dirty*/)
{
    refreshTabLabel(indexOfBuffer(buffer));
}

void EditorTabs::onBufferDisplayNameChanged(Buffer* buffer)
{
    refreshTabLabel(indexOfBuffer(buffer));
}

void EditorTabs::onTabBarContextMenu(const QPoint& localPos)
{
    QTabBar* bar = tabBar();
    const int idx = bar->tabAt(localPos);
    emit tabContextMenuRequested(idx, bar->mapToGlobal(localPos));
}

void EditorTabs::refreshTabLabel(int index)
{
    Buffer* b = bufferAt(index);
    if (!b) return;
    const QString label = (b->isDirty() ? QStringLiteral("● ") : QString{})
                          + b->displayName();
    setTabText(index, label);
    // Phase 9n — pinned tabs render with a small red-dot icon. Toggling
    // pin off clears it via the empty QIcon. The native close ✕ stays
    // available; the close gate lives in MainWindow's three close
    // entry points.
    setTabIcon(index, b->isPinned() ? pinIcon() : QIcon());
    if (b->hasFile()) setTabToolTip(index, b->filePath());
}
