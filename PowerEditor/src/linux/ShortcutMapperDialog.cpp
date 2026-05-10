#include "ShortcutMapperDialog.h"

#include <functional>

#include <QAction>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include "Config.h"
#include "Localization.h"   // Phase 8b-polish-2 — applyToDialog

namespace {

QString stripMnemonic(const QString& s)
{
    // QAction text often has '&' for the keyboard mnemonic. Strip a
    // single literal '&' (but preserve "&&" as "&").
    QString out = s;
    int i = 0;
    while (i < out.size()) {
        if (out[i] == QLatin1Char('&')) {
            if (i + 1 < out.size() && out[i + 1] == QLatin1Char('&')) {
                out.remove(i, 1);
                ++i;
                continue;
            }
            out.remove(i, 1);
        } else {
            ++i;
        }
    }
    return out;
}

void walkMenuActions(QMenu* menu, const QString& parentPath,
                     const std::function<void(const QString&, QAction*)>& visit)
{
    for (QAction* a : menu->actions()) {
        if (a->isSeparator()) continue;
        if (a->menu()) {
            const QString sub = parentPath
                + stripMnemonic(a->text())
                + QStringLiteral(" / ");
            walkMenuActions(a->menu(), sub, visit);
        } else {
            const QString path = parentPath + stripMnemonic(a->text());
            visit(path, a);
        }
    }
}

void walkMenuBar(QMenuBar* mb,
                 const std::function<void(const QString&, QAction*)>& visit)
{
    for (QAction* topAct : mb->actions()) {
        if (!topAct->menu()) continue;
        const QString sub = stripMnemonic(topAct->text())
            + QStringLiteral(" / ");
        walkMenuActions(topAct->menu(), sub, visit);
    }
}

QHash<QString, QKeySequence>& defaultsMap()
{
    static QHash<QString, QKeySequence> g;
    return g;
}

} // namespace

namespace Shortcuts {

void captureDefaults(QMenuBar* mb)
{
    if (!mb) return;
    defaultsMap().clear();
    walkMenuBar(mb, [](const QString& path, QAction* a) {
        defaultsMap().insert(path, a->shortcut());
    });
}

void applyOverrides(QMenuBar* mb)
{
    if (!mb) return;
    walkMenuBar(mb, [](const QString& path, QAction* a) {
        const QString persisted = Config::shortcutOverride(path);
        if (!persisted.isEmpty()) {
            a->setShortcut(QKeySequence(persisted));
        }
    });
}

const QHash<QString, QKeySequence>& defaults() { return defaultsMap(); }

} // namespace Shortcuts

ShortcutMapperDialog::ShortcutMapperDialog(QMenuBar* menuBar, QWidget* parent)
    : QDialog(parent), m_menuBar(menuBar)
{
    setWindowTitle(tr("Shortcut Mapper"));
    resize(720, 500);

    auto* root = new QVBoxLayout(this);

    auto* filterRow = new QHBoxLayout;
    filterRow->addWidget(new QLabel(tr("Filter:"), this));
    m_filter = new QLineEdit(this);
    m_filter->setPlaceholderText(tr("Type to filter actions..."));
    filterRow->addWidget(m_filter);
    root->addLayout(filterRow);
    connect(m_filter, &QLineEdit::textChanged,
            this, &ShortcutMapperDialog::onFilterChanged);

    m_tree = new QTreeWidget(this);
    m_tree->setColumnCount(3);
    m_tree->setHeaderLabels({tr("Menu Path"), tr("Current Shortcut"),
                             tr("Default Shortcut")});
    m_tree->setRootIsDecorated(false);
    m_tree->setAlternatingRowColors(true);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->setUniformRowHeights(true);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    root->addWidget(m_tree, 1);
    connect(m_tree, &QTreeWidget::itemSelectionChanged,
            this, &ShortcutMapperDialog::onItemSelected);

    auto* editRow = new QHBoxLayout;
    m_selectedLabel = new QLabel(tr("(none selected)"), this);
    m_selectedLabel->setMinimumWidth(220);
    editRow->addWidget(m_selectedLabel);
    m_edit = new QKeySequenceEdit(this);
    editRow->addWidget(m_edit, 1);
    auto* aSet = new QPushButton(tr("Set"), this);
    auto* aClr = new QPushButton(tr("Clear"), this);
    auto* aDef = new QPushButton(tr("Default"), this);
    editRow->addWidget(aSet);
    editRow->addWidget(aClr);
    editRow->addWidget(aDef);
    root->addLayout(editRow);
    connect(aSet, &QPushButton::clicked, this, &ShortcutMapperDialog::onSetShortcut);
    connect(aClr, &QPushButton::clicked, this, &ShortcutMapperDialog::onClearShortcut);
    connect(aDef, &QPushButton::clicked, this, &ShortcutMapperDialog::onResetToDefault);

    auto* btnRow = new QHBoxLayout;
    auto* aResetAll = new QPushButton(tr("Reset all to defaults"), this);
    auto* aApply    = new QPushButton(tr("&Apply"), this);
    auto* aSave     = new QPushButton(tr("&Save"),  this);
    auto* aClose    = new QPushButton(tr("Close"),  this);
    aSave->setDefault(true);
    btnRow->addWidget(aResetAll);
    btnRow->addStretch(1);
    btnRow->addWidget(aApply);
    btnRow->addWidget(aSave);
    btnRow->addWidget(aClose);
    root->addLayout(btnRow);

    connect(aResetAll, &QPushButton::clicked,
            this, &ShortcutMapperDialog::onResetAllToDefaults);
    connect(aApply, &QPushButton::clicked, this, &ShortcutMapperDialog::onApply);
    connect(aSave,  &QPushButton::clicked, this, &ShortcutMapperDialog::onSave);
    connect(aClose, &QPushButton::clicked, this, &QDialog::accept);

    populateTree();

    // Phase 8b-polish-2 — apply active language overlay.
    Localization::applyToDialog(this, "ShortcutMapper");
}

void ShortcutMapperDialog::populateTree()
{
    m_tree->clear();
    if (!m_menuBar) return;

    walkMenuBar(m_menuBar, [this](const QString& path, QAction* a) {
        auto* it = new QTreeWidgetItem(m_tree);
        it->setText(0, path);
        it->setText(1, a->shortcut().toString(QKeySequence::NativeText));
        const auto def = Shortcuts::defaults().value(path);
        it->setText(2, def.toString(QKeySequence::NativeText));
        it->setData(0, Qt::UserRole,
            QVariant::fromValue(reinterpret_cast<void*>(a)));
    });
    m_tree->sortItems(0, Qt::AscendingOrder);
}

void ShortcutMapperDialog::onItemSelected()
{
    const auto items = m_tree->selectedItems();
    if (items.isEmpty()) {
        m_selectedLabel->setText(tr("(none selected)"));
        m_edit->clear();
        return;
    }
    auto* item = items.first();
    m_selectedLabel->setText(item->text(0));
    m_edit->setKeySequence(QKeySequence(item->text(1)));
}

void ShortcutMapperDialog::onSetShortcut()
{
    const auto items = m_tree->selectedItems();
    if (items.isEmpty()) return;
    auto* item = items.first();
    const QString path = item->text(0);
    const QKeySequence ks = m_edit->keySequence();
    m_pending.insert(path, ks);
    item->setText(1, ks.toString(QKeySequence::NativeText));
}

void ShortcutMapperDialog::onClearShortcut()
{
    const auto items = m_tree->selectedItems();
    if (items.isEmpty()) return;
    auto* item = items.first();
    m_pending.insert(item->text(0), QKeySequence{});
    item->setText(1, QString{});
    m_edit->clear();
}

void ShortcutMapperDialog::onResetToDefault()
{
    const auto items = m_tree->selectedItems();
    if (items.isEmpty()) return;
    auto* item = items.first();
    const QString path = item->text(0);
    const QKeySequence def = Shortcuts::defaults().value(path);
    m_pending.insert(path, def);
    item->setText(1, def.toString(QKeySequence::NativeText));
    m_edit->setKeySequence(def);
}

void ShortcutMapperDialog::onResetAllToDefaults()
{
    m_pending.clear();
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        auto* item = m_tree->topLevelItem(i);
        const QString path = item->text(0);
        const QKeySequence def = Shortcuts::defaults().value(path);
        m_pending.insert(path, def);
        item->setText(1, def.toString(QKeySequence::NativeText));
    }
}

void ShortcutMapperDialog::onApply()
{
    if (!m_menuBar || m_pending.isEmpty()) return;
    walkMenuBar(m_menuBar, [this](const QString& path, QAction* a) {
        if (m_pending.contains(path)) a->setShortcut(m_pending.value(path));
    });
}

void ShortcutMapperDialog::onSave()
{
    onApply();
    // Persist every override that differs from the default.
    for (auto it = m_pending.constBegin(); it != m_pending.constEnd(); ++it) {
        const QKeySequence def = Shortcuts::defaults().value(it.key());
        if (it.value() == def) {
            // User reset to default — clear the persisted override so
            // a future default change in code is picked up.
            Config::setShortcutOverride(it.key(), QString{});
        } else {
            Config::setShortcutOverride(it.key(),
                it.value().toString(QKeySequence::PortableText));
        }
    }
    Config::save();
    accept();
}

void ShortcutMapperDialog::onFilterChanged(const QString& text)
{
    const QString needle = text.trimmed();
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        auto* item = m_tree->topLevelItem(i);
        const bool match = needle.isEmpty()
            || item->text(0).contains(needle, Qt::CaseInsensitive)
            || item->text(1).contains(needle, Qt::CaseInsensitive);
        item->setHidden(!match);
    }
}

void ShortcutMapperDialog::updateRow(QTreeWidgetItem* /*item*/)
{
    // Reserved for future per-row updates if we add columns.
}
