// ShortcutMapperDialog.h — Phase 5Q: rebind any menu shortcut.
//
// A modal QDialog with a QTreeWidget listing every menu action that has
// or could have a shortcut. Click a row to load it into the edit panel;
// QKeySequenceEdit captures the new keys; Apply pushes onto the QAction;
// Save persists to config.xml as ShortcutOverride entries that re-apply
// at the next launch.
//
// "Default" shortcuts are the ones built into MainWindow::buildMenus —
// captured into a hash before user overrides land at startup, exposed
// through the Shortcuts:: helpers in this file.

#pragma once

#include <QDialog>
#include <QHash>
#include <QKeySequence>
#include <QString>

class QKeySequenceEdit;
class QLabel;
class QLineEdit;
class QMenuBar;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace Shortcuts {
// Walk the menu bar once at startup BEFORE applying user overrides;
// stash every action's shortcut in a path-keyed hash. The dialog
// reads from this hash to show "Default".
void captureDefaults(QMenuBar* mb);

// Walk again, applying any persisted overrides from Config. Called
// at end of MainWindow::ctor after captureDefaults.
void applyOverrides(QMenuBar* mb);

// Map of menu-path -> QKeySequence captured by captureDefaults.
const QHash<QString, QKeySequence>& defaults();
} // namespace Shortcuts

class ShortcutMapperDialog : public QDialog {
    Q_OBJECT
public:
    explicit ShortcutMapperDialog(QMenuBar* menuBar, QWidget* parent = nullptr);

private slots:
    void onItemSelected();
    void onSetShortcut();
    void onClearShortcut();
    void onResetToDefault();
    void onResetAllToDefaults();
    void onApply();
    void onSave();
    void onFilterChanged(const QString& text);

private:
    void populateTree();
    void updateRow(QTreeWidgetItem* item);

    QMenuBar*    m_menuBar = nullptr;
    QTreeWidget* m_tree    = nullptr;
    QLabel*      m_selectedLabel = nullptr;
    QKeySequenceEdit* m_edit = nullptr;
    QLineEdit*   m_filter  = nullptr;

    // Pending edits — a path -> new sequence map. Apply / Save flush
    // these onto the QActions and (for Save) into Config.
    QHash<QString, QKeySequence> m_pending;
};
