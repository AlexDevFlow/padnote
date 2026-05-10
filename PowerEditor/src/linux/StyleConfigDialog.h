// StyleConfigDialog.h — Phase 5P (Global tab) + Phase 8a (Per-language tab).
//
// Two-tab dialog:
//   • Global  — Default fg/bg, line-number fg/bg, caret colour, selection bg
//               (the original 5P MVP — unchanged behaviour).
//   • Per-language — language list on the left, scrollable form on the
//               right with one row per `<WordsStyle>` (style name + fg/bg
//               swatch + Bold/Italic/Underline checkboxes). Edits persist
//               as `lang:<internalName>:<styleID>:<attr>` rows in the
//               flat StyleOverride hash.
//
// Apply: pushes the relevant Scintilla messages to every open buffer.
// Save: persists pending edits via Config::setStyleOverride + Config::save.

#pragma once

#include <QColor>
#include <QDialog>
#include <QHash>
#include <QString>

class MainWindow;
class QCheckBox;
class QFormLayout;
class QListWidget;
class QPushButton;
class QScrollArea;
class QTabWidget;
class QWidget;

class StyleConfigDialog : public QDialog {
    Q_OBJECT
public:
    // Phase 3d — takes MainWindow* (was EditorTabs*) so Apply / Save can
    // walk both panes via mw->allBuffers().
    explicit StyleConfigDialog(MainWindow* mw, QWidget* parent = nullptr);

private slots:
    void onPickFg();
    void onPickBg();
    void onPickLineNumberFg();
    void onPickLineNumberBg();
    void onPickCaret();
    void onPickSelectionBg();
    void onApply();
    void onSave();
    void onResetToTheme();

    // Phase 8a — per-language tab.
    void onLanguageRowChanged(int row);
    void onResetThisLanguage();

private:
    QColor pickColor(const QColor& current, const QString& title);
    void   refreshSwatches();
    void   applyToAllBuffers();

    // Phase 8a helpers.
    QWidget* buildGlobalTab();
    QWidget* buildPerLanguageTab();
    void     populateLanguageList();
    void     loadStylesForCurrentLanguage();
    QString  pendingValue(const QString& key) const;   // pending hash overrides Config
    void     setPendingValue(const QString& key, const QString& value);

    MainWindow* m_mw = nullptr;
    QTabWidget* m_tabs = nullptr;

    // ---- Global tab state ----
    QColor m_fg, m_bg;
    QColor m_lnFg, m_lnBg;
    QColor m_caret;
    QColor m_selBg;

    QPushButton* m_swFg     = nullptr;
    QPushButton* m_swBg     = nullptr;
    QPushButton* m_swLnFg   = nullptr;
    QPushButton* m_swLnBg   = nullptr;
    QPushButton* m_swCaret  = nullptr;
    QPushButton* m_swSelBg  = nullptr;

    // ---- Per-language tab state (Phase 8a) ----
    QListWidget* m_langList     = nullptr;
    QScrollArea* m_styleScroll  = nullptr;
    QWidget*     m_styleHost    = nullptr;   // live in the scroll area;
                                              // recreated on language change
    QFormLayout* m_styleForm    = nullptr;

    // Pending per-language overrides not yet flushed to Config. Keyed
    // identically to Config's flat `lang:<internalName>:<styleID>:<attr>`
    // schema. An empty value means "delete the override" (ie. revert to
    // theme default).
    QHash<QString, QString> m_pending;

    // Internal name of the currently-displayed language, e.g. "cpp".
    QString m_currentLangInternal;
};
