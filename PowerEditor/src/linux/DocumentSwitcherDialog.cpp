#include "DocumentSwitcherDialog.h"

#include <QFileInfo>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QVBoxLayout>

#include "Buffer.h"

DocumentSwitcherDialog::DocumentSwitcherDialog(const QVector<Buffer*>& mru,
                                               QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Document Switcher"));
    setModal(true);
    // Sensible default size for a list of ~20 entries; resizable.
    resize(520, 360);

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(8, 8, 8, 8);
    lay->setSpacing(6);

    auto* hint = new QLabel(
        tr("Press Enter to switch, Esc to cancel, Tab/↑/↓ to navigate."), this);
    hint->setStyleSheet(QStringLiteral("color: gray; font-style: italic;"));
    lay->addWidget(hint);

    m_list = new QListWidget(this);
    m_list->setAlternatingRowColors(true);
    lay->addWidget(m_list, 1);

    for (Buffer* b : mru) {
        if (!b) continue;
        // Display: basename — full path (or just "Untitled N" for unsaved).
        const QString display = b->displayName();
        QString label;
        if (b->hasFile()) {
            label = QStringLiteral("%1    %2").arg(display,
                QFileInfo(b->filePath()).absolutePath());
        } else {
            label = display;
        }
        if (b->isDirty()) label = QStringLiteral("● ") + label;
        auto* item = new QListWidgetItem(label, m_list);
        // Stash the Buffer* via the item's data. Matches the pattern
        // used by EditorTabs for tab→buffer back-pointers.
        item->setData(Qt::UserRole, QVariant::fromValue<void*>(b));
        if (b->hasFile()) item->setToolTip(b->filePath());
    }

    // Pre-select the second-most-recent (index 1) so Ctrl+Tab + Enter
    // = "switch to previous tab" without further navigation. Falls back
    // to index 0 if there's only one buffer.
    if (m_list->count() > 1) m_list->setCurrentRow(1);
    else if (m_list->count() == 1) m_list->setCurrentRow(0);

    // Double-click activates.
    connect(m_list, &QListWidget::itemActivated,
            this, [this](QListWidgetItem*){ accept(); });
}

void DocumentSwitcherDialog::keyPressEvent(QKeyEvent* event)
{
    // Tab / Shift+Tab navigate the list (without us, Tab would chase
    // the focus chain and leave the list).
    if (event->key() == Qt::Key_Tab) {
        const int n = m_list->count();
        if (n > 0) {
            int row = m_list->currentRow();
            row = (row + 1) % n;
            m_list->setCurrentRow(row);
        }
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Backtab
     || (event->key() == Qt::Key_Tab
         && (event->modifiers() & Qt::ShiftModifier))) {
        const int n = m_list->count();
        if (n > 0) {
            int row = m_list->currentRow();
            row = (row - 1 + n) % n;
            m_list->setCurrentRow(row);
        }
        event->accept();
        return;
    }
    QDialog::keyPressEvent(event);
}

void DocumentSwitcherDialog::accept()
{
    QListWidgetItem* it = m_list->currentItem();
    if (it) {
        m_chosen = static_cast<Buffer*>(it->data(Qt::UserRole).value<void*>());
    }
    QDialog::accept();
}
