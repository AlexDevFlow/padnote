#include "FunctionListDock.h"

#include <QHeaderView>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItem>

#include "Buffer.h"
#include "FunctionList.h"
#include "Languages.h"
#include "Scintilla.h"
#include "ScintillaEditBase.h"
#include "ScintillaMessages.h"

using Scintilla::Message;

namespace {

// Debounce window for rebuilds. Typing a single character fires
// notifyChange; a 250 ms window coalesces typing bursts so the regex
// engine doesn't get hammered during fast typing.
constexpr int kRebuildDebounceMs = 250;

// Default dock width.
constexpr int kDockDefaultWidth = 220;

} // namespace

FunctionListDock::FunctionListDock(QWidget* parent)
    : QDockWidget(tr("Function List"), parent)
{
    setObjectName(QStringLiteral("functionListDock"));
    setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    setFeatures(QDockWidget::DockWidgetClosable
              | QDockWidget::DockWidgetMovable
              | QDockWidget::DockWidgetFloatable);

    m_tree = new QTreeWidget(this);
    m_tree->setHeaderLabels({tr("Symbol"), tr("Line")});
    m_tree->setRootIsDecorated(false);
    m_tree->setUniformRowHeights(true);
    m_tree->setAlternatingRowColors(true);
    m_tree->header()->setStretchLastSection(false);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_tree->setMinimumWidth(160);
    m_tree->resize(kDockDefaultWidth, m_tree->height());
    setWidget(m_tree);

    connect(m_tree, &QTreeWidget::itemActivated,
            this, &FunctionListDock::onItemActivated);

    m_rebuildTimer = new QTimer(this);
    m_rebuildTimer->setSingleShot(true);
    m_rebuildTimer->setInterval(kRebuildDebounceMs);
    connect(m_rebuildTimer, &QTimer::timeout,
            this, &FunctionListDock::onRebuildTimerFired);
}

void FunctionListDock::setBuffer(Buffer* buffer)
{
    if (m_target.data() == buffer) return;
    detachFromBuffer();
    m_target = buffer;
    if (!buffer) {
        m_tree->clear();
        return;
    }
    auto* ed = buffer->editor();
    if (ed) {
        // notifyChange fires on any document mutation. Debounce via
        // m_rebuildTimer so typing bursts coalesce into one rebuild.
        connect(ed, &ScintillaEditBase::notifyChange,
                this, &FunctionListDock::onSourceModified,
                Qt::UniqueConnection);
    }
    rebuildTree();
}

void FunctionListDock::detachFromBuffer()
{
    if (!m_target) return;
    auto* ed = m_target->editor();
    if (ed) {
        disconnect(ed, &ScintillaEditBase::notifyChange,
                   this, &FunctionListDock::onSourceModified);
    }
    m_target = nullptr;
}

void FunctionListDock::onSourceModified()
{
    // Restart the debounce window. If notifyChange fires again before the
    // timer expires, we restart again — only quiet periods of >=250 ms
    // trigger an actual rebuild.
    if (m_rebuildTimer) m_rebuildTimer->start();
}

void FunctionListDock::onRebuildTimerFired()
{
    rebuildTree();
}

void FunctionListDock::rebuildTree()
{
    m_tree->clear();
    if (!m_target) return;

    const LanguageDef* lang = m_target->language();
    const QString langKey = lang
        ? QString::fromUtf8(lang->internalName.c_str()) : QString();

    if (!FunctionList::hasParserFor(langKey)) {
        // Friendly placeholder for unsupported languages. Disabled so the
        // user can't double-click it.
        auto* item = new QTreeWidgetItem(m_tree);
        const QString langDisplay = lang ? lang->displayName : tr("Plain text");
        item->setText(0, tr("(no symbols available for %1)").arg(langDisplay));
        item->setFlags(Qt::ItemIsEnabled);
        item->setDisabled(true);
        return;
    }

    const auto symbols = FunctionList::extract(m_target);
    if (symbols.isEmpty()) {
        auto* item = new QTreeWidgetItem(m_tree);
        item->setText(0, tr("(no symbols found)"));
        item->setFlags(Qt::ItemIsEnabled);
        item->setDisabled(true);
        return;
    }

    for (const auto& s : symbols) {
        auto* item = new QTreeWidgetItem(m_tree);
        item->setText(0, s.name);
        item->setText(1, QString::number(s.line));
        item->setData(0, Qt::UserRole, s.line);
        // Hint at kind via tooltip — keeps the tree visually clean while
        // letting curious users see "function" / "class" / "section".
        item->setToolTip(0, s.kind);
    }
}

void FunctionListDock::onItemActivated(QTreeWidgetItem* item, int /*column*/)
{
    if (!item || !m_target) return;
    const int line = item->data(0, Qt::UserRole).toInt();
    if (line <= 0) return;
    auto* ed = m_target->editor();
    if (!ed) return;
    // line is 1-based; Scintilla wants 0-based.
    const Scintilla::sptr_t target = static_cast<Scintilla::sptr_t>(line - 1);
    ed->send(static_cast<unsigned int>(Message::EnsureVisibleEnforcePolicy),
             static_cast<Scintilla::uptr_t>(target));
    ed->send(static_cast<unsigned int>(Message::GotoLine),
             static_cast<Scintilla::uptr_t>(target));
    ed->send(static_cast<unsigned int>(Message::VerticalCentreCaret));
    ed->setFocus();
}
