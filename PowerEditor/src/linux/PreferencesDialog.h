// PreferencesDialog.h — Phase 5N.1: dialog shell for the Preferences UI.
//
// Settings → Preferences... opens a modal dialog with a QListWidget on the
// left listing tab names and a QStackedWidget on the right showing the
// active tab's content. Bottom row carries OK / Apply / Cancel.
//
// Apply / Cancel semantics:
//   • Each tab is a `PreferencesTab` (or, in this MVP shell, a thin
//     `QWidget` whose `loadFromConfig()` / `applyToConfig()` are called by
//     the dialog).
//   • OK = applyToConfig() on every dirty tab → Config::save() → close.
//   • Apply = applyToConfig() on every dirty tab → Config::save() → leave
//     dialog open (allows previewing before committing).
//   • Cancel = no-op; close. (Per-tab "load from config" runs at dialog
//     construction, so no rollback is needed — pending state lives in
//     widget values only.)
//
// 5N.1 lands this shell + ONE working tab (General with bracketAutoPair)
// to validate the pattern end-to-end. 5N.2..5N.10 will fill out the
// remaining tabs (General-rest / Editing / New Document / etc.) by
// adding new `PreferencesTab` subclasses to `addTabs`.

#pragma once

#include <QDialog>
#include <QString>
#include <QVector>

class QListWidget;
class QStackedWidget;
class QPushButton;

class PreferencesTab;

class PreferencesDialog : public QDialog {
    Q_OBJECT
public:
    explicit PreferencesDialog(QWidget* parent = nullptr);

signals:
    // Fires on every Apply (and on OK before accept()). Callers (e.g.
    // MainWindow) connect this to re-pull live state from Config —
    // toolbar / statusbar visibility, tab close-button toggle, etc.
    // Phase 5N.2+.
    void settingsApplied();

private slots:
    void onApply();
    void onOk();
    void onTabChanged(int row);

private:
    void addTabs();
    void addTab(PreferencesTab* tab);
    void applyAllTabs();

    QListWidget*   m_tabList   = nullptr;
    QStackedWidget* m_stack    = nullptr;
    QPushButton*   m_btnOk     = nullptr;
    QPushButton*   m_btnApply  = nullptr;
    QPushButton*   m_btnCancel = nullptr;

    QVector<PreferencesTab*> m_tabs;
};

// -----------------------------------------------------------------------------
// Per-tab base class. Subclass for each preference tab. The dialog calls
// loadFromConfig() at construction (after the tab is parented) and
// applyToConfig() on Apply / OK. `displayName()` is shown in the left list.
// -----------------------------------------------------------------------------

class PreferencesTab : public QWidget {
    Q_OBJECT
public:
    explicit PreferencesTab(QWidget* parent = nullptr) : QWidget(parent) {}

    virtual QString displayName() const = 0;
    virtual void    loadFromConfig() = 0;
    virtual void    applyToConfig() = 0;
};
