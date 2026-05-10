#include "PreferencesDialog.h"

#include <QCheckBox>
#include <QColor>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QProcess>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStringList>
#include <QVBoxLayout>

#include <algorithm>

#include "Config.h"
#include "Languages.h"
#include "Localization.h"   // Phase 8b-polish-2 — applyToDialog

namespace {

// =============================================================================
// General tab — visibility toggles (Phase 5N.2).
//
// Three checkbox controls bound to Config::showToolBar / showStatusBar /
// tabsClosable. MainWindow re-applies these to the live UI on
// PreferencesDialog::settingsApplied (Apply / OK).
//
// bracketAutoPair was here in 5N.1 as a pattern-validation control;
// it moves to the Editing tab in 5N.3 where it belongs by upstream
// organisation.
// =============================================================================

class GeneralTab : public PreferencesTab {
public:
    explicit GeneralTab(QWidget* parent = nullptr) : PreferencesTab(parent)
    {
        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(12, 12, 12, 12);
        root->setSpacing(8);

        auto* form = new QFormLayout;
        m_showToolBar    = new QCheckBox(tr("Show toolbar"), this);
        m_showStatusBar  = new QCheckBox(tr("Show status bar"), this);
        m_tabsClosable   = new QCheckBox(tr("Show close (×) button on tabs"), this);
        m_showToolBar->setToolTip(tr(
            "When OFF, the main toolbar is hidden until you re-enable it here."));
        m_showStatusBar->setToolTip(tr(
            "When OFF, the bottom status bar is hidden. Status messages "
            "still appear briefly even when the bar is hidden."));
        m_tabsClosable->setToolTip(tr(
            "When OFF, the per-tab close button is hidden in both panes. "
            "Use Ctrl+W or the File menu to close tabs."));
        form->addRow(QString(), m_showToolBar);
        form->addRow(QString(), m_showStatusBar);
        form->addRow(QString(), m_tabsClosable);
        root->addLayout(form);

        auto* note = new QLabel(
            tr("More General options (multi-instance, document switcher) "
               "will land in future 5N sub-phases."), this);
        note->setWordWrap(true);
        note->setStyleSheet(QStringLiteral("color: gray; font-style: italic;"));
        root->addWidget(note);
        root->addStretch(1);
    }

    QString displayName() const override { return tr("General"); }

    void loadFromConfig() override
    {
        m_showToolBar->setChecked(Config::showToolBar());
        m_showStatusBar->setChecked(Config::showStatusBar());
        m_tabsClosable->setChecked(Config::tabsClosable());
    }

    void applyToConfig() override
    {
        Config::setShowToolBar(m_showToolBar->isChecked());
        Config::setShowStatusBar(m_showStatusBar->isChecked());
        Config::setTabsClosable(m_tabsClosable->isChecked());
    }

private:
    QCheckBox* m_showToolBar   = nullptr;
    QCheckBox* m_showStatusBar = nullptr;
    QCheckBox* m_tabsClosable  = nullptr;
};

// =============================================================================
// Editing tab (Phase 5N.3).
//
// Smart highlight (also wired to View → Highlight all matches; both writes
// share the same Config key so the menu and dialog stay in sync), bracket
// auto-pair (moved here from General per upstream organisation), caret
// width (1-3 px), caret blink rate (ms; 0 = never blink).
// =============================================================================

class EditingTab : public PreferencesTab {
public:
    explicit EditingTab(QWidget* parent = nullptr) : PreferencesTab(parent)
    {
        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(12, 12, 12, 12);
        root->setSpacing(8);

        auto* form = new QFormLayout;

        m_smartHighlight = new QCheckBox(tr("Highlight all matches"), this);
        m_smartHighlight->setToolTip(tr(
            "When ON, double-clicking a word (or making a 2-200 char "
            "selection) highlights every matching occurrence in the buffer. "
            "Also toggleable from View → Highlight all matches."));
        form->addRow(QString(), m_smartHighlight);

        // Phase 9c.2 — fine-tune the highlight-all-matches search.
        m_smartHighlightMatchCase = new QCheckBox(
            tr("    Match case (highlight)"), this);
        m_smartHighlightMatchCase->setToolTip(tr(
            "When ON, the highlight-all-matches search is case-sensitive: "
            "selecting `foo` highlights `foo` but not `Foo`. Default ON."));
        form->addRow(QString(), m_smartHighlightMatchCase);

        m_smartHighlightWholeWord = new QCheckBox(
            tr("    Whole word only (highlight)"), this);
        m_smartHighlightWholeWord->setToolTip(tr(
            "When ON, only standalone words match. When OFF, selecting "
            "`foo` also highlights `foo` inside `foobar`. Default ON."));
        form->addRow(QString(), m_smartHighlightWholeWord);

        // Both fine-tunes only matter when the master toggle is ON; reflect
        // that in the UI by graying them out when it's OFF.
        auto refreshHighlightSubControls = [this]{
            const bool on = m_smartHighlight->isChecked();
            m_smartHighlightMatchCase->setEnabled(on);
            m_smartHighlightWholeWord->setEnabled(on);
        };
        connect(m_smartHighlight, &QCheckBox::toggled,
                this, refreshHighlightSubControls);

        m_bracketAutoPair = new QCheckBox(tr("Auto-pair brackets and quotes"), this);
        m_bracketAutoPair->setToolTip(tr(
            "When ON, typing (, [, {, \", or ' inserts the matching closer "
            "and positions the caret between them."));
        form->addRow(QString(), m_bracketAutoPair);

        // Phase 9i — auto-trigger autocomplete after N identifier chars.
        m_autocompleteAutoTrigger = new QCheckBox(
            tr("Auto-trigger word completion"), this);
        m_autocompleteAutoTrigger->setToolTip(tr(
            "When ON, the autocomplete popup appears automatically after "
            "you've typed N identifier characters. Default OFF — manual "
            "Ctrl+Space (Edit → Function and word completion) always works."));
        form->addRow(QString(), m_autocompleteAutoTrigger);

        m_autocompleteTriggerChars = new QSpinBox(this);
        m_autocompleteTriggerChars->setRange(2, 6);
        m_autocompleteTriggerChars->setSuffix(tr(" chars"));
        m_autocompleteTriggerChars->setToolTip(tr(
            "Number of consecutive identifier characters that trigger the "
            "autocomplete popup. Lower = more aggressive (more popups, "
            "smaller prefixes); higher = less intrusive."));
        form->addRow(tr("    Trigger after:"), m_autocompleteTriggerChars);

        // Disable the threshold spinbox when the master toggle is off.
        auto refreshAutocompleteSubControls = [this]{
            m_autocompleteTriggerChars->setEnabled(
                m_autocompleteAutoTrigger->isChecked());
        };
        connect(m_autocompleteAutoTrigger, &QCheckBox::toggled,
                this, refreshAutocompleteSubControls);

        m_caretWidth = new QSpinBox(this);
        m_caretWidth->setRange(1, 3);
        m_caretWidth->setSuffix(tr(" px"));
        m_caretWidth->setToolTip(tr("Width of the editor caret in pixels (1–3)."));
        form->addRow(tr("Caret width:"), m_caretWidth);

        m_caretBlink = new QSpinBox(this);
        m_caretBlink->setRange(0, 2000);
        m_caretBlink->setSuffix(tr(" ms"));
        m_caretBlink->setSingleStep(50);
        m_caretBlink->setSpecialValueText(tr("(no blink)"));
        m_caretBlink->setToolTip(tr(
            "Blink interval in milliseconds. 0 disables blinking; "
            "default 500 ms."));
        form->addRow(tr("Caret blink rate:"), m_caretBlink);

        // Phase 9b.4 — vertical-edge marker controls.
        m_edgeEnabled = new QCheckBox(tr("Show vertical edge"), this);
        m_edgeEnabled->setToolTip(tr(
            "When ON, draw a thin vertical line at the configured column "
            "in every buffer (Scintilla EDGE_LINE)."));
        form->addRow(QString(), m_edgeEnabled);

        m_edgeColumn = new QSpinBox(this);
        m_edgeColumn->setRange(1, 200);
        m_edgeColumn->setSuffix(tr(" cols"));
        m_edgeColumn->setToolTip(tr(
            "Column at which the vertical edge is drawn (1–200; default 80)."));
        form->addRow(tr("Vertical edge column:"), m_edgeColumn);

        // The colour picker is a small button showing the current swatch;
        // clicking opens QColorDialog. Storage is "#RRGGBB" in Config.
        auto* colorRow = new QHBoxLayout;
        m_edgeColorBtn = new QPushButton(tr("Pick…"), this);
        m_edgeColorBtn->setToolTip(tr(
            "Colour of the vertical edge line. Click to choose."));
        m_edgeColorSwatch = new QLabel(this);
        m_edgeColorSwatch->setMinimumWidth(48);
        m_edgeColorSwatch->setMinimumHeight(20);
        m_edgeColorSwatch->setFrameShape(QFrame::StyledPanel);
        colorRow->addWidget(m_edgeColorSwatch);
        colorRow->addWidget(m_edgeColorBtn);
        colorRow->addStretch(1);
        connect(m_edgeColorBtn, &QPushButton::clicked, this, [this]{
            const QColor initial(m_edgeColorHex);
            const QColor picked = QColorDialog::getColor(
                initial.isValid() ? initial : QColor(QStringLiteral("#C0C0C0")),
                this, tr("Vertical edge colour"));
            if (picked.isValid()) {
                m_edgeColorHex = picked.name();
                refreshEdgeSwatch();
            }
        });
        form->addRow(tr("Vertical edge colour:"), colorRow);

        root->addLayout(form);
        root->addStretch(1);
    }

    QString displayName() const override { return tr("Editing"); }

    void loadFromConfig() override
    {
        m_smartHighlight->setChecked(Config::smartHighlightEnabled());
        m_smartHighlightMatchCase->setChecked(Config::smartHighlightMatchCase());
        m_smartHighlightWholeWord->setChecked(Config::smartHighlightWholeWord());
        const bool shOn = m_smartHighlight->isChecked();
        m_smartHighlightMatchCase->setEnabled(shOn);
        m_smartHighlightWholeWord->setEnabled(shOn);
        m_bracketAutoPair->setChecked(Config::bracketAutoPairEnabled());
        m_autocompleteAutoTrigger->setChecked(Config::autocompleteAutoTrigger());
        m_autocompleteTriggerChars->setValue(Config::autocompleteTriggerChars());
        m_autocompleteTriggerChars->setEnabled(m_autocompleteAutoTrigger->isChecked());
        m_caretWidth->setValue(Config::caretWidth());
        m_caretBlink->setValue(Config::caretBlinkMs());
        m_edgeEnabled->setChecked(Config::verticalEdgeEnabled());
        m_edgeColumn->setValue(Config::verticalEdgeColumn());
        m_edgeColorHex = Config::verticalEdgeColor();
        refreshEdgeSwatch();
    }

    void applyToConfig() override
    {
        Config::setSmartHighlightEnabled(m_smartHighlight->isChecked());
        Config::setSmartHighlightMatchCase(m_smartHighlightMatchCase->isChecked());
        Config::setSmartHighlightWholeWord(m_smartHighlightWholeWord->isChecked());
        Config::setBracketAutoPairEnabled(m_bracketAutoPair->isChecked());
        Config::setAutocompleteAutoTrigger(m_autocompleteAutoTrigger->isChecked());
        Config::setAutocompleteTriggerChars(m_autocompleteTriggerChars->value());
        Config::setCaretWidth(m_caretWidth->value());
        Config::setCaretBlinkMs(m_caretBlink->value());
        Config::setVerticalEdgeEnabled(m_edgeEnabled->isChecked());
        Config::setVerticalEdgeColumn(m_edgeColumn->value());
        Config::setVerticalEdgeColor(m_edgeColorHex);
    }

private:
    void refreshEdgeSwatch()
    {
        m_edgeColorSwatch->setStyleSheet(QStringLiteral(
            "background-color: %1;").arg(m_edgeColorHex));
        m_edgeColorSwatch->setText(m_edgeColorHex);
    }

    QCheckBox* m_smartHighlight  = nullptr;
    QCheckBox* m_smartHighlightMatchCase = nullptr;   // Phase 9c.2
    QCheckBox* m_smartHighlightWholeWord = nullptr;   // Phase 9c.2
    QCheckBox* m_bracketAutoPair = nullptr;
    QCheckBox* m_autocompleteAutoTrigger = nullptr;   // Phase 9i
    QSpinBox*  m_autocompleteTriggerChars = nullptr;  // Phase 9i
    QSpinBox*  m_caretWidth      = nullptr;
    QSpinBox*  m_caretBlink      = nullptr;
    QCheckBox* m_edgeEnabled     = nullptr;
    QSpinBox*  m_edgeColumn      = nullptr;
    QPushButton* m_edgeColorBtn  = nullptr;
    QLabel*    m_edgeColorSwatch = nullptr;
    QString    m_edgeColorHex;   // "#RRGGBB" — staged value before Apply
};

// =============================================================================
// New Document tab (Phase 5N.4).
//
// Defaults applied to every new buffer (newUntitled / openFile after fresh
// load — the file's actual encoding wins on load via uchardet, but EOL +
// language fall back to these). Buffer's ctor reads these directly.
// =============================================================================

class NewDocumentTab : public PreferencesTab {
public:
    explicit NewDocumentTab(QWidget* parent = nullptr) : PreferencesTab(parent)
    {
        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(12, 12, 12, 12);
        root->setSpacing(8);

        auto* form = new QFormLayout;

        // ---- Encoding combo ----------------------------------------
        // UserData stores a pair-encoded int: codec name index in a
        // parallel vector. Simpler to store the pair as a QPoint? No —
        // just stash name + hasBom in two separate roles.
        m_encoding = new QComboBox(this);
        struct EncEntry { QString label; QString codec; bool bom; };
        const std::initializer_list<EncEntry> encs = {
            { tr("UTF-8"),         QStringLiteral("UTF-8"),       false },
            { tr("UTF-8 with BOM"),QStringLiteral("UTF-8"),       true  },
            { tr("UTF-16 BE BOM"), QStringLiteral("UTF-16BE"),    true  },
            { tr("UTF-16 LE BOM"), QStringLiteral("UTF-16LE"),    true  },
            { tr("ANSI (Windows-1252)"),
              QStringLiteral("windows-1252"), false },
        };
        for (const auto& e : encs) {
            QStringList payload; payload << e.codec
                                          << (e.bom ? QStringLiteral("yes")
                                                    : QStringLiteral("no"));
            m_encoding->addItem(e.label, payload);
        }
        m_encoding->setToolTip(tr(
            "Encoding applied when creating a new (untitled) buffer. "
            "Files opened from disk are decoded by uchardet auto-detection; "
            "this default only governs fresh buffers."));
        form->addRow(tr("Default encoding:"), m_encoding);

        // ---- EOL combo ---------------------------------------------
        // 0=Crlf, 1=Cr, 2=Lf — matches Buffer::EolMode enum order +
        // the int Config stores.
        m_eol = new QComboBox(this);
        m_eol->addItem(tr("Windows (CR LF)"), 0);
        m_eol->addItem(tr("Macintosh (CR)"),  1);
        m_eol->addItem(tr("Unix (LF)"),       2);
        m_eol->setToolTip(tr(
            "End-of-line style for new buffers. Files opened from disk "
            "preserve their existing EOL; this default applies to "
            "untitled buffers."));
        form->addRow(tr("Default EOL:"), m_eol);

        // ---- Language combo ----------------------------------------
        m_language = new QComboBox(this);
        m_language->addItem(tr("Plain text"), QString());   // empty payload
        // Languages::all() returns a stable, registry-sorted array.
        for (int i = 0; i < Languages::count(); ++i) {
            const LanguageDef* L = &Languages::all()[i];
            if (!L) continue;
            // Skip the "normal" / plain-text entry — already at index 0.
            if (L->internalName == "normal") continue;
            m_language->addItem(L->displayName,
                QString::fromUtf8(L->internalName.c_str()));
        }
        m_language->setToolTip(tr(
            "Default syntax-highlighting language for new buffers. "
            "Files opened from disk override this via extension matching."));
        form->addRow(tr("Default language:"), m_language);

        root->addLayout(form);

        auto* note = new QLabel(
            tr("These defaults take effect for new buffers. "
               "Existing buffers are unaffected."), this);
        note->setWordWrap(true);
        note->setStyleSheet(QStringLiteral("color: gray; font-style: italic;"));
        root->addWidget(note);
        root->addStretch(1);
    }

    QString displayName() const override { return tr("New Document"); }

    void loadFromConfig() override
    {
        // Encoding: walk items, match (codec, bom) pair.
        const QString name   = Config::defaultEncodingName();
        const bool    hasBom = Config::defaultEncodingHasBom();
        for (int i = 0; i < m_encoding->count(); ++i) {
            const QStringList payload = m_encoding->itemData(i).toStringList();
            if (payload.size() == 2
             && payload[0].compare(name, Qt::CaseInsensitive) == 0
             && (payload[1] == QStringLiteral("yes")) == hasBom) {
                m_encoding->setCurrentIndex(i);
                break;
            }
        }
        // EOL: match by int payload.
        const int eol = Config::defaultEolMode();
        for (int i = 0; i < m_eol->count(); ++i) {
            if (m_eol->itemData(i).toInt() == eol) {
                m_eol->setCurrentIndex(i);
                break;
            }
        }
        // Language: match by internalName payload (empty = plain text = idx 0).
        const QString lang = Config::defaultLanguage();
        for (int i = 0; i < m_language->count(); ++i) {
            if (m_language->itemData(i).toString() == lang) {
                m_language->setCurrentIndex(i);
                break;
            }
        }
    }

    void applyToConfig() override
    {
        const QStringList encPayload =
            m_encoding->currentData().toStringList();
        if (encPayload.size() == 2) {
            Config::setDefaultEncoding(encPayload[0],
                encPayload[1] == QStringLiteral("yes"));
        }
        Config::setDefaultEolMode(m_eol->currentData().toInt());
        Config::setDefaultLanguage(m_language->currentData().toString());
    }

private:
    QComboBox* m_encoding = nullptr;
    QComboBox* m_eol      = nullptr;
    QComboBox* m_language = nullptr;
};

// =============================================================================
// Default Directory tab (Phase 5N.5).
//
// One QLineEdit + Browse button for the seed path used by File → Open's
// dialog. Empty = Qt's remember-last-used default.
// =============================================================================

class DefaultDirectoryTab : public PreferencesTab {
public:
    explicit DefaultDirectoryTab(QWidget* parent = nullptr)
        : PreferencesTab(parent)
    {
        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(12, 12, 12, 12);
        root->setSpacing(8);

        auto* hint = new QLabel(
            tr("Directory used as the starting point for File → Open. "
               "Leave empty to let the file dialog remember the last-used "
               "location (Qt default)."), this);
        hint->setWordWrap(true);
        root->addWidget(hint);

        auto* row = new QHBoxLayout;
        row->setSpacing(4);
        m_path = new QLineEdit(this);
        m_path->setPlaceholderText(tr("(remember last-used directory)"));
        auto* browse = new QPushButton(tr("Browse..."), this);
        row->addWidget(m_path, 1);
        row->addWidget(browse);
        root->addLayout(row);
        root->addStretch(1);

        connect(browse, &QPushButton::clicked, this, [this]{
            const QString seed = m_path->text().isEmpty()
                ? QDir::homePath() : m_path->text();
            const QString picked = QFileDialog::getExistingDirectory(this,
                tr("Choose default directory"), seed);
            if (!picked.isEmpty()) m_path->setText(picked);
        });
    }

    QString displayName() const override { return tr("Default Directory"); }

    void loadFromConfig() override
    {
        m_path->setText(Config::defaultOpenDirectory());
    }

    void applyToConfig() override
    {
        Config::setDefaultOpenDirectory(m_path->text().trimmed());
    }

private:
    QLineEdit* m_path = nullptr;
};

// =============================================================================
// Recent Files History tab (Phase 5N.6).
//
// Just one control today: the cap for the File → Open Recent submenu.
// Future polish could add "Sort by name" / "Show full path" toggles.
// =============================================================================

class RecentFilesTab : public PreferencesTab {
public:
    explicit RecentFilesTab(QWidget* parent = nullptr) : PreferencesTab(parent)
    {
        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(12, 12, 12, 12);
        root->setSpacing(8);

        auto* form = new QFormLayout;
        m_max = new QSpinBox(this);
        m_max->setRange(0, 50);
        m_max->setSuffix(tr(" entries"));
        m_max->setSpecialValueText(tr("(disabled)"));
        m_max->setToolTip(tr(
            "Maximum number of files retained in File → Open Recent. "
            "0 disables the recent-files feature entirely; the menu still "
            "exists but stays empty."));
        form->addRow(tr("Recent files cap:"), m_max);
        root->addLayout(form);

        auto* note = new QLabel(
            tr("Lowering the cap immediately trims the saved list. "
               "Raising it doesn't recover previously-trimmed entries."), this);
        note->setWordWrap(true);
        note->setStyleSheet(QStringLiteral("color: gray; font-style: italic;"));
        root->addWidget(note);
        root->addStretch(1);
    }

    QString displayName() const override { return tr("Recent Files"); }

    void loadFromConfig() override
    {
        m_max->setValue(Config::recentFilesMax());
    }

    void applyToConfig() override
    {
        Config::setRecentFilesMax(m_max->value());
    }

private:
    QSpinBox* m_max = nullptr;
};

// =============================================================================
// File Association tab (Phase 5N.7).
//
// Linux file association lives in ~/.config/mimeapps.list and is keyed off
// the system-installed .desktop entry (Phase 6a). Editing mimeapps.list
// directly from a text editor that may not have been installed via the
// system package manager is fragile, so this tab is informational +
// surfaces a button that opens the desktop's standard Default Apps panel
// (which knows how to write mimeapps.list correctly).
// =============================================================================

class FileAssociationTab : public PreferencesTab {
public:
    explicit FileAssociationTab(QWidget* parent = nullptr)
        : PreferencesTab(parent)
    {
        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(12, 12, 12, 12);
        root->setSpacing(8);

        auto* note = new QLabel(
            tr("On Linux, file associations live in <code>~/.config/mimeapps.list</code> "
               "and are managed by the desktop environment, not by the editor."), this);
        note->setWordWrap(true);
        note->setTextFormat(Qt::RichText);
        root->addWidget(note);

        auto* explain = new QLabel(
            tr("padnote ships a <code>.desktop</code> entry plus MIME-type "
               "definitions for common code extensions. To set padnote as "
               "the default for a specific file type, use your desktop's "
               "Default Applications panel, or right-click a file in your "
               "file manager and choose Open With."), this);
        explain->setWordWrap(true);
        explain->setTextFormat(Qt::RichText);
        root->addWidget(explain);

        auto* row = new QHBoxLayout;
        row->setSpacing(4);
        auto* btnDefaults = new QPushButton(
            tr("Open system Default Applications settings"), this);
        btnDefaults->setToolTip(tr(
            "Tries gnome-control-center, then plasma-systemmonitor, then "
            "xdg-open. If your desktop doesn't expose a standard Default "
            "Apps panel, edit ~/.config/mimeapps.list by hand."));
        connect(btnDefaults, &QPushButton::clicked, this, [this]{
            // Try the most common DE-specific helpers in order.
            const QStringList candidates = {
                QStringLiteral("gnome-control-center default-apps"),
                QStringLiteral("kcmshell5 kcm_componentchooser"),
                QStringLiteral("xfce4-mime-settings"),
                // Final fallback: xdg-open the mimeapps.list itself.
                QStringLiteral("xdg-open ~/.config/mimeapps.list"),
            };
            for (const QString& cmd : candidates) {
                if (QProcess::startDetached(QStringLiteral("/bin/sh"),
                        {QStringLiteral("-c"), cmd}))
                    return;
            }
        });
        row->addWidget(btnDefaults);
        row->addStretch(1);
        root->addLayout(row);

        auto* tail = new QLabel(
            tr("Future polish: read-only display of currently-claimed "
               "extensions and a one-click \"register all my extensions\" "
               "shortcut. The MVP keeps association strictly OS-side to "
               "avoid pinning users to a config-file write that may not "
               "survive a reinstall."), this);
        tail->setWordWrap(true);
        tail->setStyleSheet(QStringLiteral("color: gray; font-style: italic;"));
        root->addWidget(tail);
        root->addStretch(1);
    }

    QString displayName() const override { return tr("File Association"); }
    void    loadFromConfig() override {}     // no Config state
    void    applyToConfig()  override {}     // no Config state
};

// =============================================================================
// Language tab (Phase 5N.8).
//
// MVP scope: global indent defaults (tab width 1-16, use tabs vs spaces).
// Buffer's ctor reads these. Phase 5AB's auto-detect on file load may
// override per-buffer when the sample is conclusive — these defaults
// govern new untitled buffers and tiny/unclear file loads.
//
// Per-language overrides ("C++ uses 2-space indent, Python uses 4")
// + UDL editing are deferred to Phase 5U + future Style Configurator
// polish.
// =============================================================================

class LanguageTab : public PreferencesTab {
public:
    explicit LanguageTab(QWidget* parent = nullptr) : PreferencesTab(parent)
    {
        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(12, 12, 12, 12);
        root->setSpacing(8);

        auto* form = new QFormLayout;

        m_tabWidth = new QSpinBox(this);
        m_tabWidth->setRange(1, 16);
        m_tabWidth->setSuffix(tr(" columns"));
        m_tabWidth->setToolTip(tr(
            "Default display width of a tab character AND the size of one "
            "indent step. Phase 9f's per-language overrides take "
            "precedence; Phase 5AB's auto-detect on file load applies "
            "only when no override is set for the language."));
        form->addRow(tr("Default tab width:"), m_tabWidth);

        m_useTabs = new QCheckBox(tr("Use tab characters for indentation"), this);
        m_useTabs->setToolTip(tr(
            "Default: when OFF, pressing Tab inserts spaces (count = tab "
            "width). Per-language overrides (below) take precedence. "
            "Modern-editor default is OFF (spaces)."));
        form->addRow(QString(), m_useTabs);

        root->addLayout(form);

        // Phase 9f — per-language override editor.
        auto* sep = new QFrame(this);
        sep->setFrameShape(QFrame::HLine);
        sep->setFrameShadow(QFrame::Sunken);
        root->addWidget(sep);

        auto* heading = new QLabel(
            tr("<b>Per-language indent overrides</b>"), this);
        heading->setTextFormat(Qt::RichText);
        root->addWidget(heading);

        auto* helpLabel = new QLabel(
            tr("Override the default tab width / use-tabs setting for a "
               "specific language. Buffers using that language always pick "
               "up the override, even when the file's existing indent "
               "style would normally win via auto-detect."), this);
        helpLabel->setWordWrap(true);
        helpLabel->setStyleSheet(QStringLiteral("color: gray;"));
        root->addWidget(helpLabel);

        auto* editorRow = new QHBoxLayout;
        m_overrideLang = new QComboBox(this);
        m_overrideLang->setToolTip(tr("Language to override."));
        // Populate with every language from the registry.
        for (int i = 0; i < Languages::count(); ++i) {
            const LanguageDef* L = &Languages::all()[i];
            const QString internalName =
                QString::fromUtf8(L->internalName.c_str());
            // displayName for the user-visible label; userData = internalName
            // so we can persist by internalName.
            m_overrideLang->addItem(L->displayName, internalName);
        }
        editorRow->addWidget(m_overrideLang);

        m_overrideTabWidth = new QSpinBox(this);
        m_overrideTabWidth->setRange(1, 16);
        m_overrideTabWidth->setSuffix(tr(" cols"));
        editorRow->addWidget(m_overrideTabWidth);

        m_overrideUseTabs = new QCheckBox(tr("Use tabs"), this);
        editorRow->addWidget(m_overrideUseTabs);

        auto* btnSet = new QPushButton(tr("Set"), this);
        btnSet->setToolTip(tr(
            "Add or update an override for the selected language."));
        editorRow->addWidget(btnSet);

        auto* btnRemove = new QPushButton(tr("Remove"), this);
        btnRemove->setToolTip(tr(
            "Remove the currently-selected override from the list below."));
        editorRow->addWidget(btnRemove);
        root->addLayout(editorRow);

        m_overrideList = new QListWidget(this);
        m_overrideList->setToolTip(tr(
            "Existing overrides. Click an entry to load it back into the "
            "editor row above; click Remove to delete it."));
        root->addWidget(m_overrideList, 1);

        connect(btnSet, &QPushButton::clicked, this, [this]{
            const QString internal = m_overrideLang->currentData().toString();
            if (internal.isEmpty()) return;
            const int tw   = m_overrideTabWidth->value();
            const bool ut  = m_overrideUseTabs->isChecked();
            m_pendingOverrides[internal] = qMakePair(tw, ut);
            refreshList();
        });
        connect(btnRemove, &QPushButton::clicked, this, [this]{
            QListWidgetItem* it = m_overrideList->currentItem();
            if (!it) return;
            const QString internal = it->data(Qt::UserRole).toString();
            if (internal.isEmpty()) return;
            m_pendingOverrides.remove(internal);
            refreshList();
        });
        connect(m_overrideList, &QListWidget::currentItemChanged, this,
                [this](QListWidgetItem* cur, QListWidgetItem*){
            if (!cur) return;
            const QString internal = cur->data(Qt::UserRole).toString();
            if (internal.isEmpty()) return;
            // Sync the editor row to the clicked entry.
            const int idx = m_overrideLang->findData(internal);
            if (idx >= 0) m_overrideLang->setCurrentIndex(idx);
            const auto& v = m_pendingOverrides.value(internal);
            m_overrideTabWidth->setValue(v.first);
            m_overrideUseTabs->setChecked(v.second);
        });

        root->addStretch(1);
    }

    QString displayName() const override { return tr("Language"); }

    void loadFromConfig() override
    {
        m_tabWidth->setValue(Config::defaultTabWidth());
        m_useTabs->setChecked(Config::defaultUseTabs());
        // Snapshot the current overrides into m_pendingOverrides; the
        // editor row mutates this map and Apply diffs against it.
        m_pendingOverrides.clear();
        for (const QString& key : Config::languageIndentOverrideKeys()) {
            if (Config::hasLanguageIndentOverride(key)) {
                const Config::LanguageIndent ind =
                    Config::languageIndentOverride(key);
                m_pendingOverrides[key] = qMakePair(ind.tabWidth, ind.useTabs);
            }
        }
        refreshList();
    }

    void applyToConfig() override
    {
        Config::setDefaultTabWidth(m_tabWidth->value());
        Config::setDefaultUseTabs(m_useTabs->isChecked());
        // Reconcile: drop overrides that disappeared, set the rest.
        const QStringList existing = Config::languageIndentOverrideKeys();
        for (const QString& key : existing) {
            if (!m_pendingOverrides.contains(key)) {
                Config::clearLanguageIndentOverride(key);
            }
        }
        for (auto it = m_pendingOverrides.constBegin();
             it != m_pendingOverrides.constEnd(); ++it) {
            Config::setLanguageIndentOverride(it.key(),
                it.value().first, it.value().second);
        }
    }

private:
    void refreshList()
    {
        m_overrideList->clear();
        // Build a sorted list for stable display.
        QStringList keys = m_pendingOverrides.keys();
        keys.sort();
        for (const QString& key : keys) {
            const auto& v = m_pendingOverrides.value(key);
            // Resolve display name from the language registry.
            const LanguageDef* L =
                Languages::findByInternalName(key.toUtf8().constData());
            const QString display = L ? L->displayName : key;
            const QString useTabsLabel = v.second
                ? tr("tabs")
                : tr("spaces");
            const QString label = QStringLiteral("%1 — %2 %3")
                .arg(display).arg(v.first).arg(useTabsLabel);
            auto* item = new QListWidgetItem(label, m_overrideList);
            item->setData(Qt::UserRole, key);
        }
    }

    QSpinBox*    m_tabWidth         = nullptr;
    QCheckBox*   m_useTabs          = nullptr;
    QComboBox*   m_overrideLang     = nullptr;
    QSpinBox*    m_overrideTabWidth = nullptr;
    QCheckBox*   m_overrideUseTabs  = nullptr;
    QListWidget* m_overrideList     = nullptr;
    // Staged overrides: edits go here first; applyToConfig diffs.
    QHash<QString, QPair<int, bool>> m_pendingOverrides;
};

// =============================================================================
// Highlighting tab (Phase 5N.9).
//
// Pure pointer to Settings → Style Configurator. Per-language style
// editing is deferred to a Style Configurator polish phase; the existing
// Style Configurator (Phase 5P MVP) covers global theme overrides.
// =============================================================================

class HighlightingTab : public PreferencesTab {
public:
    explicit HighlightingTab(QWidget* parent = nullptr) : PreferencesTab(parent)
    {
        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(12, 12, 12, 12);
        root->setSpacing(8);

        auto* note = new QLabel(
            tr("Syntax-highlight colour editing lives in <b>Settings → "
               "Style Configurator</b>. Theme switching is in <b>View → "
               "Theme</b>."), this);
        note->setWordWrap(true);
        note->setTextFormat(Qt::RichText);
        root->addWidget(note);

        auto* explain = new QLabel(
            tr("The MVP Style Configurator (Phase 5P) exposes global "
               "fg/bg/caret/selection colours. A future polish phase "
               "will add per-language style editing (Comment, Keyword, "
               "Number, etc.) — that requires Lexilla style-name "
               "introspection per lexer, which is its own piece of "
               "work."), this);
        explain->setWordWrap(true);
        explain->setStyleSheet(QStringLiteral("color: gray; font-style: italic;"));
        root->addWidget(explain);

        root->addStretch(1);
    }

    QString displayName() const override { return tr("Highlighting"); }
    void    loadFromConfig() override {}
    void    applyToConfig()  override {}
};

// =============================================================================
// Print tab (Phase 5N.10 + Phase 9o + Phase 5T).
//
// Header / footer template strings with macro substitution. Macros
// resolved at print time against the active buffer's path-derived
// placeholders + the current date/time + per-page numbering (Phase 9o).
// Phase 5T adds a syntax-highlight toggle that routes the print path
// through SCI_FORMATRANGEFULL instead of QTextDocument::print.
// =============================================================================

class PrintTab : public PreferencesTab {
public:
    explicit PrintTab(QWidget* parent = nullptr) : PreferencesTab(parent)
    {
        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(12, 12, 12, 12);
        root->setSpacing(8);

        m_syntaxHighlight = new QCheckBox(
            tr("Print with syntax highlighting "
               "(uses editor colours and font)"), this);
        m_syntaxHighlight->setToolTip(tr(
            "When ticked, the printer renders coloured tokens using "
            "Scintilla's SCI_FORMATRANGEFULL path. When unticked, the "
            "printer falls back to a plain-text monospaced render via "
            "QTextDocument."));
        root->addWidget(m_syntaxHighlight);

        auto* form = new QFormLayout;

        m_magnification = new QSpinBox(this);
        m_magnification->setRange(-10, 10);
        m_magnification->setSuffix(tr(" pt"));
        m_magnification->setToolTip(tr(
            "Adjust the print font size relative to the editor's. "
            "0 = same as on-screen; +2 makes paper output larger; "
            "-2 makes it smaller. Mirrors Scintilla's "
            "SCI_SETPRINTMAGNIFICATION setting. Only applies when "
            "syntax highlighting is on."));
        form->addRow(tr("Print font size adjustment:"), m_magnification);

        m_header = new QLineEdit(this);
        m_header->setPlaceholderText(tr("(no header)"));
        m_footer = new QLineEdit(this);
        m_footer->setPlaceholderText(tr("(no footer)"));
        form->addRow(tr("Header text:"), m_header);
        form->addRow(tr("Footer text:"), m_footer);
        root->addLayout(form);

        auto* macros = new QLabel(
            tr("<b>Available macros:</b><br>"
               "<code>$(FULL_CURRENT_PATH)</code> — absolute path of the active file<br>"
               "<code>$(FILE_NAME)</code> — file name (basename) or display name<br>"
               "<code>$(NAME_PART)</code> — filename without extension<br>"
               "<code>$(EXT_PART)</code> — extension only<br>"
               "<code>$(CURRENT_DATE)</code> — today's date (ISO 8601)<br>"
               "<code>$(CURRENT_TIME)</code> — current time (HH:mm:ss)<br>"
               "<code>$(PAGE_NUMBER)</code> — 1-based page index<br>"
               "<code>$(NB_PAGES)</code> — total page count"), this);
        macros->setTextFormat(Qt::RichText);
        macros->setWordWrap(true);
        root->addWidget(macros);

        root->addStretch(1);
    }

    QString displayName() const override { return tr("Print"); }

    void loadFromConfig() override
    {
        m_syntaxHighlight->setChecked(Config::syntaxHighlightedPrint());
        m_magnification->setValue(Config::printMagnification());
        m_header->setText(Config::printHeader());
        m_footer->setText(Config::printFooter());
    }

    void applyToConfig() override
    {
        Config::setSyntaxHighlightedPrint(m_syntaxHighlight->isChecked());
        Config::setPrintMagnification(m_magnification->value());
        Config::setPrintHeader(m_header->text());
        Config::setPrintFooter(m_footer->text());
    }

private:
    QCheckBox* m_syntaxHighlight = nullptr;
    QSpinBox*  m_magnification   = nullptr;
    QLineEdit* m_header = nullptr;
    QLineEdit* m_footer = nullptr;
};

// =============================================================================
// Backup & Misc tab (Phase 5N.11).
//
// Combines what upstream splits across Backup + MISC + Performance —
// the actually-plumbable settings for the Linux port. Search Engine /
// Auto-completion / Cloud / Delimiter aren't user-configurable today
// in this port, so they don't appear; future work can extend.
// =============================================================================

// =============================================================================
// Cloud tab.
//
// padnote doesn't integrate cloud APIs directly. Users redirect
// config.xml (and session.xml + userDefineLangs.xml) to a path inside
// their Dropbox/Nextcloud/Syncthing folder; the existing sync client
// handles cross-machine propagation. Sentinel file lives at
// $XDG_CONFIG_HOME/padnote/padnote--/cloud.path; its presence + content
// drive Config::configFilePath().
// =============================================================================

class CloudTab : public PreferencesTab {
public:
    explicit CloudTab(QWidget* parent = nullptr) : PreferencesTab(parent)
    {
        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(12, 12, 12, 12);
        root->setSpacing(8);

        auto* header = new QLabel(
            tr("<b>Cloud config-path redirect</b>"), this);
        header->setTextFormat(Qt::RichText);
        root->addWidget(header);

        auto* note = new QLabel(
            tr("padnote doesn't integrate Dropbox / OneDrive / Google Drive "
               "directly. Instead, point the config directory at a folder "
               "your existing sync client watches (e.g. <code>~/Dropbox/padnote/"
               "</code>) — the sync client handles cross-machine propagation.<br><br>"
               "Files redirected: <code>config.xml</code>, <code>session.xml</code>, "
               "<code>userDefineLangs.xml</code>. The sentinel file "
               "(<code>cloud.path</code>) stays in the local default config dir, "
               "so each machine can independently opt into the redirect.<br><br>"
               "Restart required for the change to apply across all open buffers."),
            this);
        note->setTextFormat(Qt::RichText);
        note->setWordWrap(true);
        root->addWidget(note);

        m_radioLocal = new QRadioButton(tr("&Local (default config directory)"), this);
        m_radioCloud = new QRadioButton(tr("&Custom path:"), this);
        root->addWidget(m_radioLocal);

        auto* row = new QHBoxLayout;
        row->addWidget(m_radioCloud);
        m_pathEdit = new QLineEdit(this);
        m_pathEdit->setPlaceholderText(tr("/home/you/Dropbox/padnote"));
        row->addWidget(m_pathEdit, 1);
        m_btnPick = new QPushButton(tr("Pick &folder..."), this);
        connect(m_btnPick, &QPushButton::clicked, this, [this]() {
            const QString d = QFileDialog::getExistingDirectory(this,
                tr("Pick cloud config directory"),
                m_pathEdit->text().isEmpty() ? QDir::homePath()
                                              : m_pathEdit->text());
            if (!d.isEmpty()) {
                m_pathEdit->setText(d);
                m_radioCloud->setChecked(true);
            }
        });
        row->addWidget(m_btnPick);
        root->addLayout(row);

        // Greying logic: path edit + pick button only enabled when the
        // Custom radio is selected.
        auto enabled = [this]() {
            const bool en = m_radioCloud->isChecked();
            m_pathEdit->setEnabled(en);
            m_btnPick->setEnabled(en);
        };
        connect(m_radioLocal, &QRadioButton::toggled, this, enabled);
        connect(m_radioCloud, &QRadioButton::toggled, this, enabled);

        root->addStretch(1);
    }

    QString displayName() const override { return tr("Cloud"); }

    void loadFromConfig() override
    {
        const QString cur = Config::cloudConfigDir();
        if (cur.isEmpty()) {
            m_radioLocal->setChecked(true);
            m_pathEdit->clear();
        } else {
            m_radioCloud->setChecked(true);
            m_pathEdit->setText(cur);
        }
        m_pathEdit->setEnabled(m_radioCloud->isChecked());
        m_btnPick->setEnabled(m_radioCloud->isChecked());
    }

    void applyToConfig() override
    {
        if (m_radioLocal->isChecked()) {
            Config::clearCloudConfigDir();
        } else {
            const QString p = m_pathEdit->text().trimmed();
            if (!p.isEmpty()) Config::setCloudConfigDir(p);
            else              Config::clearCloudConfigDir();
        }
    }

private:
    QRadioButton* m_radioLocal = nullptr;
    QRadioButton* m_radioCloud = nullptr;
    QLineEdit*    m_pathEdit   = nullptr;
    QPushButton*  m_btnPick    = nullptr;
};

class BackupMiscTab : public PreferencesTab {
public:
    explicit BackupMiscTab(QWidget* parent = nullptr) : PreferencesTab(parent)
    {
        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(12, 12, 12, 12);
        root->setSpacing(8);

        auto* form = new QFormLayout;

        m_backupEnabled = new QCheckBox(tr("Enable periodic backup of dirty buffers"), this);
        m_backupEnabled->setToolTip(tr(
            "When ON, every N seconds dirty buffers are snapshotted to "
            "~/.config/padnote/padnote--/backup/. Disabling stops the "
            "snapshotter; existing backups stay until clean save / close."));
        form->addRow(QString(), m_backupEnabled);

        m_backupInterval = new QSpinBox(this);
        m_backupInterval->setRange(1, 300);
        m_backupInterval->setSuffix(tr(" seconds"));
        m_backupInterval->setToolTip(tr(
            "Interval between backup snapshots. Lower = more recovery "
            "granularity but more disk churn. Default 10 s."));
        form->addRow(tr("Backup interval:"), m_backupInterval);

        m_fileWatcher = new QCheckBox(tr("Watch open files for external changes"), this);
        m_fileWatcher->setToolTip(tr(
            "When ON (Phase 5T), an external editor's write to a file you "
            "have open prompts you to reload (clean buffer) or warns you "
            "(dirty buffer). Turn off if your workflow involves heavy "
            "external file mutation that would generate prompt spam."));
        form->addRow(QString(), m_fileWatcher);

        root->addLayout(form);

        auto* note = new QLabel(
            tr("Search-engine / Delimiter / Performance settings have no "
               "user-configurable surface yet in this Linux port; future "
               "polish phases may add controls here as plumbing lands. "
               "Cloud sync moved to its own tab in Phase 12."), this);
        note->setWordWrap(true);
        note->setStyleSheet(QStringLiteral("color: gray; font-style: italic;"));
        root->addWidget(note);
        root->addStretch(1);
    }

    QString displayName() const override { return tr("Backup && Misc"); }

    void loadFromConfig() override
    {
        m_backupEnabled->setChecked(Config::backupEnabled());
        m_backupInterval->setValue(Config::backupIntervalSec());
        m_fileWatcher->setChecked(Config::fileWatcherEnabled());
    }

    void applyToConfig() override
    {
        Config::setBackupEnabled(m_backupEnabled->isChecked());
        Config::setBackupIntervalSec(m_backupInterval->value());
        Config::setFileWatcherEnabled(m_fileWatcher->isChecked());
    }

private:
    QCheckBox* m_backupEnabled  = nullptr;
    QSpinBox*  m_backupInterval = nullptr;
    QCheckBox* m_fileWatcher    = nullptr;
};

} // namespace

// =============================================================================
// PreferencesDialog
// =============================================================================

PreferencesDialog::PreferencesDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Preferences"));
    resize(720, 480);

    auto* root = new QVBoxLayout(this);

    // Body: tab list (left) + stacked content (right).
    auto* body = new QHBoxLayout;
    m_tabList = new QListWidget(this);
    m_tabList->setMaximumWidth(180);
    m_tabList->setMinimumWidth(140);
    m_stack = new QStackedWidget(this);
    body->addWidget(m_tabList);
    body->addWidget(m_stack, 1);
    root->addLayout(body, 1);

    // Buttons.
    auto* btnRow = new QHBoxLayout;
    btnRow->addStretch(1);
    m_btnOk     = new QPushButton(tr("OK"),     this);
    m_btnApply  = new QPushButton(tr("Apply"),  this);
    m_btnCancel = new QPushButton(tr("Cancel"), this);
    m_btnOk->setDefault(true);
    btnRow->addWidget(m_btnOk);
    btnRow->addWidget(m_btnApply);
    btnRow->addWidget(m_btnCancel);
    root->addLayout(btnRow);

    addTabs();
    if (m_tabList->count() > 0) m_tabList->setCurrentRow(0);

    connect(m_tabList, &QListWidget::currentRowChanged,
            this, &PreferencesDialog::onTabChanged);
    connect(m_btnOk,     &QPushButton::clicked, this, &PreferencesDialog::onOk);
    connect(m_btnApply,  &QPushButton::clicked, this, &PreferencesDialog::onApply);
    connect(m_btnCancel, &QPushButton::clicked, this, &QDialog::reject);

    // Phase 8b-polish-2 — translate window title + every QLabel /
    // QCheckBox / QPushButton / QRadioButton / QGroupBox via the
    // active language overlay. Source-text matching against
    // english.xml's <Dialog><Preference>...</Preference> block.
    Localization::applyToDialog(this, "Preference");
}

void PreferencesDialog::addTabs()
{
    addTab(new GeneralTab(this));
    addTab(new EditingTab(this));
    addTab(new NewDocumentTab(this));
    addTab(new DefaultDirectoryTab(this));
    addTab(new RecentFilesTab(this));
    addTab(new FileAssociationTab(this));
    addTab(new LanguageTab(this));
    addTab(new HighlightingTab(this));
    addTab(new PrintTab(this));
    addTab(new CloudTab(this));         // Phase 12 — cloud sync redirect
    addTab(new BackupMiscTab(this));
    // Phase 5N is complete. Future polish can extend any tab.
}

void PreferencesDialog::addTab(PreferencesTab* tab)
{
    if (!tab) return;
    tab->loadFromConfig();
    m_tabs.append(tab);
    m_tabList->addItem(tab->displayName());
    m_stack->addWidget(tab);
}

void PreferencesDialog::onTabChanged(int row)
{
    if (row >= 0 && row < m_stack->count()) {
        m_stack->setCurrentIndex(row);
    }
}

void PreferencesDialog::applyAllTabs()
{
    for (PreferencesTab* tab : m_tabs) {
        if (tab) tab->applyToConfig();
    }
    Config::save();
    emit settingsApplied();
}

void PreferencesDialog::onApply()
{
    applyAllTabs();
}

void PreferencesDialog::onOk()
{
    applyAllTabs();
    accept();
}
