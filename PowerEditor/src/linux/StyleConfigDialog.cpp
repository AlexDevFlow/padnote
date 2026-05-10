#include "StyleConfigDialog.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QFont>
#include <QFontDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QStatusBar>
#include <QTabWidget>
#include <QVBoxLayout>

#include "Buffer.h"
#include "Config.h"
#include "EditorTabs.h"
#include "Languages.h"   // Phase 8a — LanguageDef::internalName
#include "Localization.h"   // Phase 8b-polish-2 — applyToDialog
#include "MainWindow.h"
#include "Scintilla.h"
#include "ScintillaEditBase.h"
#include "ScintillaMessages.h"
#include "Theme.h"

using Scintilla::Message;

namespace {

// Convert Scintilla's BGR-packed colour (red | green<<8 | blue<<16) to QColor.
QColor sciColorToQ(int packed)
{
    return QColor(packed & 0xFF, (packed >> 8) & 0xFF, (packed >> 16) & 0xFF);
}

// And the reverse — convert QColor to Scintilla's wire format.
int qToSciColor(const QColor& c)
{
    return (c.red() & 0xFF)
         | ((c.green() & 0xFF) << 8)
         | ((c.blue() & 0xFF) << 16);
}

void setSwatch(QPushButton* btn, const QColor& c)
{
    btn->setText(c.name(QColor::HexRgb).toUpper());
    btn->setStyleSheet(QStringLiteral(
        "background-color: %1; color: %2; padding: 4px 12px; "
        "border: 1px solid #888;")
        .arg(c.name(),
             c.lightness() > 128 ? QStringLiteral("#000") : QStringLiteral("#fff")));
}

} // namespace

// =============================================================================
// Construction
// =============================================================================

StyleConfigDialog::StyleConfigDialog(MainWindow* mw, QWidget* parent)
    : QDialog(parent), m_mw(mw)
{
    setWindowTitle(tr("Style Configurator"));
    resize(640, 540);

    auto* root = new QVBoxLayout(this);

    m_tabs = new QTabWidget(this);
    m_tabs->addTab(buildGlobalTab(),       tr("&Global"));
    m_tabs->addTab(buildPerLanguageTab(),  tr("&Per-language"));
    root->addWidget(m_tabs, 1);

    // Bottom button row — shared across both tabs.
    auto* row = new QHBoxLayout;
    auto* aResetGlobal = new QPushButton(tr("Reset to theme defaults"), this);
    auto* aApply       = new QPushButton(tr("&Apply"), this);
    auto* aSave        = new QPushButton(tr("&Save"),  this);
    auto* aClose       = new QPushButton(tr("Close"),  this);
    aSave->setDefault(true);
    row->addWidget(aResetGlobal);
    row->addStretch(1);
    row->addWidget(aApply);
    row->addWidget(aSave);
    row->addWidget(aClose);
    root->addLayout(row);

    connect(aResetGlobal, &QPushButton::clicked, this, &StyleConfigDialog::onResetToTheme);
    connect(aApply,       &QPushButton::clicked, this, &StyleConfigDialog::onApply);
    connect(aSave,        &QPushButton::clicked, this, &StyleConfigDialog::onSave);
    connect(aClose,       &QPushButton::clicked, this, &QDialog::accept);

    // Phase 8b-polish-2 — apply active language overlay.
    Localization::applyToDialog(this, "StyleConfig");
}

QWidget* StyleConfigDialog::buildGlobalTab()
{
    auto* w = new QWidget(this);
    auto* root = new QVBoxLayout(w);

    auto* note = new QLabel(
        tr("Adjust the editor's global style colours. Apply re-styles every open buffer; "
           "Save persists across launches via config.xml.\n"
           "For per-language colours (Comment / Keyword / Number / …), use the "
           "Per-language tab."),
        w);
    note->setWordWrap(true);
    root->addWidget(note);

    // Snapshot the active editor's current colours so the dialog opens
    // showing what's there.
    Buffer* activeBuf = m_mw && m_mw->activePane()
                        ? m_mw->activePane()->currentBuffer() : nullptr;
    if (activeBuf) {
        auto* ed = activeBuf->editor();
        m_fg    = sciColorToQ(static_cast<int>(ed->send(
            static_cast<unsigned int>(Message::StyleGetFore), STYLE_DEFAULT)));
        m_bg    = sciColorToQ(static_cast<int>(ed->send(
            static_cast<unsigned int>(Scintilla::Message::StyleGetBack), STYLE_DEFAULT)));
        m_lnFg  = sciColorToQ(static_cast<int>(ed->send(
            static_cast<unsigned int>(Scintilla::Message::StyleGetFore), STYLE_LINENUMBER)));
        m_lnBg  = sciColorToQ(static_cast<int>(ed->send(
            static_cast<unsigned int>(Scintilla::Message::StyleGetBack), STYLE_LINENUMBER)));
        m_caret = sciColorToQ(static_cast<int>(ed->send(
            static_cast<unsigned int>(Message::GetCaretFore))));
        m_selBg = QColor(0x80, 0x80, 0xFF);
    } else {
        m_fg     = QColor(0x00, 0x00, 0x00);
        m_bg     = QColor(0xFF, 0xFF, 0xFF);
        m_lnFg   = QColor(0x80, 0x80, 0x80);
        m_lnBg   = QColor(0xF0, 0xF0, 0xF0);
        m_caret  = QColor(0x00, 0x00, 0x00);
        m_selBg  = QColor(0x80, 0x80, 0xFF);
    }

    auto* form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight);
    auto buildSwatch = [&](const QString& label, QColor& target,
                           QPushButton*& storage, void (StyleConfigDialog::*slot)()) {
        storage = new QPushButton(w);
        setSwatch(storage, target);
        form->addRow(label, storage);
        connect(storage, &QPushButton::clicked, this, slot);
    };
    buildSwatch(tr("Default foreground:"),    m_fg,    m_swFg,
                &StyleConfigDialog::onPickFg);
    buildSwatch(tr("Default background:"),    m_bg,    m_swBg,
                &StyleConfigDialog::onPickBg);
    buildSwatch(tr("Line-number foreground:"),m_lnFg,  m_swLnFg,
                &StyleConfigDialog::onPickLineNumberFg);
    buildSwatch(tr("Line-number background:"),m_lnBg,  m_swLnBg,
                &StyleConfigDialog::onPickLineNumberBg);
    buildSwatch(tr("Caret colour:"),          m_caret, m_swCaret,
                &StyleConfigDialog::onPickCaret);
    buildSwatch(tr("Selection background:"),  m_selBg, m_swSelBg,
                &StyleConfigDialog::onPickSelectionBg);
    root->addLayout(form);
    root->addStretch(1);
    return w;
}

QWidget* StyleConfigDialog::buildPerLanguageTab()
{
    auto* w = new QWidget(this);
    auto* root = new QVBoxLayout(w);

    auto* note = new QLabel(
        tr("Per-language style colours. Pick a language; edit the fg/bg/font-style "
           "of each token style. Edits apply on top of the active theme — Reset "
           "this language reverts back to theme defaults."),
        w);
    note->setWordWrap(true);
    root->addWidget(note);

    auto* row = new QHBoxLayout;
    m_langList = new QListWidget(w);
    m_langList->setMinimumWidth(180);
    m_langList->setMaximumWidth(220);
    connect(m_langList, &QListWidget::currentRowChanged,
            this, &StyleConfigDialog::onLanguageRowChanged);
    row->addWidget(m_langList);

    m_styleScroll = new QScrollArea(w);
    m_styleScroll->setWidgetResizable(true);
    row->addWidget(m_styleScroll, 1);

    root->addLayout(row, 1);

    auto* btnRow = new QHBoxLayout;
    auto* aResetThis = new QPushButton(tr("Reset this language to theme defaults"), w);
    btnRow->addWidget(aResetThis);
    btnRow->addStretch(1);
    connect(aResetThis, &QPushButton::clicked, this, &StyleConfigDialog::onResetThisLanguage);
    root->addLayout(btnRow);

    populateLanguageList();
    return w;
}

void StyleConfigDialog::populateLanguageList()
{
    if (!m_langList) return;
    m_langList->clear();
    for (const Theme::LanguageWithStyles& lang : Theme::languagesWithStyles()) {
        auto* item = new QListWidgetItem(lang.displayName, m_langList);
        item->setData(Qt::UserRole, lang.internalName);
    }
    if (m_langList->count() > 0) m_langList->setCurrentRow(0);
}

void StyleConfigDialog::onLanguageRowChanged(int /*row*/)
{
    loadStylesForCurrentLanguage();
}

void StyleConfigDialog::loadStylesForCurrentLanguage()
{
    if (!m_styleScroll) return;
    auto* item = m_langList ? m_langList->currentItem() : nullptr;
    if (!item) {
        m_currentLangInternal.clear();
        m_styleScroll->setWidget(new QWidget);  // empty placeholder
        return;
    }
    m_currentLangInternal = item->data(Qt::UserRole).toString();

    // New host widget per language switch — letting us swap the form
    // wholesale instead of mutating an existing layout in place.
    m_styleHost = new QWidget;
    m_styleForm = new QFormLayout(m_styleHost);
    m_styleForm->setLabelAlignment(Qt::AlignLeft);

    const QByteArray nameUtf8 = m_currentLangInternal.toUtf8();
    const QVector<Theme::LanguageStyleInfo> styles =
        Theme::stylesForLanguage(nameUtf8.constData());

    if (styles.isEmpty()) {
        auto* empty = new QLabel(
            tr("No styles defined for this language in the active theme."));
        empty->setStyleSheet(QStringLiteral("color: gray; font-style: italic;"));
        m_styleForm->addRow(empty);
        m_styleScroll->setWidget(m_styleHost);
        return;
    }

    // Apply pending edits on top of theme+saved values so the form opens
    // showing the user's most-recent edits during this dialog session.
    for (const Theme::LanguageStyleInfo& info : styles) {
        const int     styleID    = info.styleID;
        const QString styleLabel = info.name.isEmpty()
            ? tr("Style %1").arg(styleID)
            : info.name;

        const QString kFg        = QStringLiteral("lang:%1:%2:fg").arg(m_currentLangInternal).arg(styleID);
        const QString kBg        = QStringLiteral("lang:%1:%2:bg").arg(m_currentLangInternal).arg(styleID);
        const QString kFontStyle = QStringLiteral("lang:%1:%2:fontStyle").arg(m_currentLangInternal).arg(styleID);
        const QString kFontName  = QStringLiteral("lang:%1:%2:fontName").arg(m_currentLangInternal).arg(styleID);
        const QString kFontSize  = QStringLiteral("lang:%1:%2:fontSize").arg(m_currentLangInternal).arg(styleID);

        const QColor fgColor = sciColorToQ(info.fg < 0 ? 0 : info.fg);
        const QColor bgColor = sciColorToQ(info.bg < 0 ? 0xFFFFFF : info.bg);
        int fontStyle = info.fontStyle;
        // Apply pending overrides if any.
        const QString pendingFg = pendingValue(kFg);
        const QString pendingBg = pendingValue(kBg);
        const QString pendingFs = pendingValue(kFontStyle);
        const QString pendingFn = pendingValue(kFontName);
        const QString pendingSz = pendingValue(kFontSize);
        QColor fgEff = pendingFg.isEmpty() ? fgColor : QColor(pendingFg);
        QColor bgEff = pendingBg.isEmpty() ? bgColor : QColor(pendingBg);
        if (!pendingFs.isEmpty()) fontStyle = pendingFs.toInt();
        // Phase 8a-polish — fontName / fontSize per style. Empty / 0
        // means "inherit STYLE_DEFAULT" — Scintilla's default behaviour.
        QString fontNameEff = pendingFn.isEmpty() ? info.fontName : pendingFn;
        int     fontSizeEff = pendingSz.isEmpty() ? info.fontSize  : pendingSz.toInt();

        // Build the row: [fg-swatch] [bg-swatch] [B] [I] [U]
        auto* rowW = new QWidget(m_styleHost);
        auto* rowL = new QHBoxLayout(rowW);
        rowL->setContentsMargins(0, 0, 0, 0);

        auto* swFg = new QPushButton(rowW);
        swFg->setToolTip(tr("Foreground colour"));
        if (fgEff.isValid()) setSwatch(swFg, fgEff);
        else                  swFg->setText(tr("(default)"));
        connect(swFg, &QPushButton::clicked, this, [this, swFg, kFg, fgEff]() mutable {
            QColor c = pickColor(fgEff.isValid() ? fgEff : QColor(0,0,0),
                                 tr("Foreground"));
            if (!c.isValid()) return;
            fgEff = c;
            setSwatch(swFg, c);
            setPendingValue(kFg, c.name());
        });
        rowL->addWidget(swFg);

        auto* swBg = new QPushButton(rowW);
        swBg->setToolTip(tr("Background colour"));
        if (bgEff.isValid()) setSwatch(swBg, bgEff);
        else                  swBg->setText(tr("(default)"));
        connect(swBg, &QPushButton::clicked, this, [this, swBg, kBg, bgEff]() mutable {
            QColor c = pickColor(bgEff.isValid() ? bgEff : QColor(0xFF,0xFF,0xFF),
                                 tr("Background"));
            if (!c.isValid()) return;
            bgEff = c;
            setSwatch(swBg, c);
            setPendingValue(kBg, c.name());
        });
        rowL->addWidget(swBg);

        auto* cbBold = new QCheckBox(QStringLiteral("B"), rowW);
        cbBold->setToolTip(tr("Bold"));
        cbBold->setChecked((fontStyle & 1) != 0);
        rowL->addWidget(cbBold);

        auto* cbItalic = new QCheckBox(QStringLiteral("I"), rowW);
        cbItalic->setToolTip(tr("Italic"));
        cbItalic->setChecked((fontStyle & 2) != 0);
        rowL->addWidget(cbItalic);

        auto* cbUnder = new QCheckBox(QStringLiteral("U"), rowW);
        cbUnder->setToolTip(tr("Underline"));
        cbUnder->setChecked((fontStyle & 4) != 0);
        rowL->addWidget(cbUnder);

        auto fontStyleChanged = [this, kFontStyle, cbBold, cbItalic, cbUnder]() {
            const int v = (cbBold->isChecked()   ? 1 : 0)
                        | (cbItalic->isChecked() ? 2 : 0)
                        | (cbUnder->isChecked()  ? 4 : 0);
            setPendingValue(kFontStyle, QString::number(v));
        };
        connect(cbBold,   &QCheckBox::toggled, this, fontStyleChanged);
        connect(cbItalic, &QCheckBox::toggled, this, fontStyleChanged);
        connect(cbUnder,  &QCheckBox::toggled, this, fontStyleChanged);

        // Phase 8a-polish — Font… button. Opens QFontDialog; on accept,
        // captures family + pointSize back into pending overrides.
        // B/I/U stay on the checkboxes (we ignore the dialog's
        // weight/italic flags so users don't accidentally toggle B/I
        // through the font picker).
        auto* btnFont = new QPushButton(rowW);
        btnFont->setToolTip(tr(
            "Font name + size. B / I / U are edited via the checkboxes; "
            "this button only changes family and size. Empty defaults "
            "fall back to the editor's main font."));
        auto formatFontLabel = [](const QString& name, int size) -> QString {
            if (name.isEmpty() && size <= 0) return tr("(default)");
            if (name.isEmpty()) return QStringLiteral("— %1pt").arg(size);
            if (size <= 0)      return name;
            return QStringLiteral("%1 %2pt").arg(name).arg(size);
        };
        btnFont->setText(formatFontLabel(fontNameEff, fontSizeEff));
        connect(btnFont, &QPushButton::clicked, this,
            [this, btnFont, kFontName, kFontSize, formatFontLabel,
             fontNameEff, fontSizeEff]() mutable {
            QFont seed;
            if (!fontNameEff.isEmpty()) seed.setFamily(fontNameEff);
            if (fontSizeEff > 0)        seed.setPointSize(fontSizeEff);
            else                         seed.setPointSize(11);
            bool ok = false;
            const QFont chosen = QFontDialog::getFont(
                &ok, seed, this, tr("Pick font"),
                QFontDialog::DontUseNativeDialog | QFontDialog::MonospacedFonts);
            if (!ok) return;
            fontNameEff = chosen.family();
            fontSizeEff = chosen.pointSize();
            btnFont->setText(formatFontLabel(fontNameEff, fontSizeEff));
            setPendingValue(kFontName, fontNameEff);
            setPendingValue(kFontSize, QString::number(fontSizeEff));
        });
        rowL->addWidget(btnFont);

        rowL->addStretch(1);
        m_styleForm->addRow(styleLabel, rowW);
    }

    m_styleScroll->setWidget(m_styleHost);
}

QString StyleConfigDialog::pendingValue(const QString& key) const
{
    const auto it = m_pending.constFind(key);
    if (it != m_pending.constEnd()) return it.value();
    return Config::styleOverride(key);
}

void StyleConfigDialog::setPendingValue(const QString& key, const QString& value)
{
    m_pending.insert(key, value);
}

QColor StyleConfigDialog::pickColor(const QColor& current, const QString& title)
{
    return QColorDialog::getColor(current, this, title);
}

void StyleConfigDialog::refreshSwatches()
{
    setSwatch(m_swFg,    m_fg);
    setSwatch(m_swBg,    m_bg);
    setSwatch(m_swLnFg,  m_lnFg);
    setSwatch(m_swLnBg,  m_lnBg);
    setSwatch(m_swCaret, m_caret);
    setSwatch(m_swSelBg, m_selBg);
}

// =============================================================================
// Global tab — colour pickers
// =============================================================================

void StyleConfigDialog::onPickFg()
{
    QColor c = pickColor(m_fg, tr("Default foreground"));
    if (c.isValid()) { m_fg = c; setSwatch(m_swFg, c); }
}
void StyleConfigDialog::onPickBg()
{
    QColor c = pickColor(m_bg, tr("Default background"));
    if (c.isValid()) { m_bg = c; setSwatch(m_swBg, c); }
}
void StyleConfigDialog::onPickLineNumberFg()
{
    QColor c = pickColor(m_lnFg, tr("Line-number foreground"));
    if (c.isValid()) { m_lnFg = c; setSwatch(m_swLnFg, c); }
}
void StyleConfigDialog::onPickLineNumberBg()
{
    QColor c = pickColor(m_lnBg, tr("Line-number background"));
    if (c.isValid()) { m_lnBg = c; setSwatch(m_swLnBg, c); }
}
void StyleConfigDialog::onPickCaret()
{
    QColor c = pickColor(m_caret, tr("Caret colour"));
    if (c.isValid()) { m_caret = c; setSwatch(m_swCaret, c); }
}
void StyleConfigDialog::onPickSelectionBg()
{
    QColor c = pickColor(m_selBg, tr("Selection background"));
    if (c.isValid()) { m_selBg = c; setSwatch(m_swSelBg, c); }
}

// =============================================================================
// Apply / Save
// =============================================================================

void StyleConfigDialog::applyToAllBuffers()
{
    if (!m_mw) return;

    // Phase 8a — flush pending per-language edits into the live Config
    // (in-memory only — Save persists to disk later). Theme::applyForLanguage
    // reads them via its override-pass.
    for (auto it = m_pending.constBegin(); it != m_pending.constEnd(); ++it) {
        Config::setStyleOverride(it.key(), it.value());
    }

    const int fg     = qToSciColor(m_fg);
    const int bg     = qToSciColor(m_bg);
    const int lnFg   = qToSciColor(m_lnFg);
    const int lnBg   = qToSciColor(m_lnBg);
    const int caret  = qToSciColor(m_caret);
    const int selBg  = qToSciColor(m_selBg);

    // Phase 3d — walk both panes via MainWindow::allBuffers().
    for (Buffer* b : m_mw->allBuffers()) {
        if (!b) continue;
        auto* ed = b->editor();
        ed->send(static_cast<unsigned int>(Message::StyleSetFore), STYLE_DEFAULT, fg);
        ed->send(static_cast<unsigned int>(Message::StyleSetBack), STYLE_DEFAULT, bg);
        ed->send(static_cast<unsigned int>(Message::StyleSetFore), STYLE_LINENUMBER, lnFg);
        ed->send(static_cast<unsigned int>(Message::StyleSetBack), STYLE_LINENUMBER, lnBg);
        ed->send(static_cast<unsigned int>(Message::SetCaretFore), caret, 0);
        ed->send(static_cast<unsigned int>(Message::SetSelBack), 1, selBg);

        // Re-style the whole buffer so per-language colours don't shadow
        // the new defaults until the next edit.
        const Scintilla::sptr_t end =
            ed->send(static_cast<unsigned int>(Message::GetLength));
        ed->send(static_cast<unsigned int>(Message::StyleClearAll), 0, 0);
        // Re-apply the buffer's language theme on top of the cleared base.
        // The override-pass at the tail of Theme::applyForLanguage picks up
        // the just-flushed pending edits.
        if (b->language()) b->reapplyTheme();
        ed->send(static_cast<unsigned int>(Message::Colourise), 0, end);
    }
}

void StyleConfigDialog::onApply()
{
    applyToAllBuffers();
    // Pending edits remain in m_pending so the dialog still shows them
    // and a subsequent Save flushes both the global + per-language sets.
}

void StyleConfigDialog::onSave()
{
    applyToAllBuffers();

    Config::setStyleOverride(QStringLiteral("default.fg"),    m_fg.name());
    Config::setStyleOverride(QStringLiteral("default.bg"),    m_bg.name());
    Config::setStyleOverride(QStringLiteral("linenumber.fg"), m_lnFg.name());
    Config::setStyleOverride(QStringLiteral("linenumber.bg"), m_lnBg.name());
    Config::setStyleOverride(QStringLiteral("caret.fg"),      m_caret.name());
    Config::setStyleOverride(QStringLiteral("selection.bg"),  m_selBg.name());

    // The per-language pending hash was already flushed into Config by
    // applyToAllBuffers; Save just persists the in-memory hash to disk.
    Config::save();
    accept();
}

void StyleConfigDialog::onResetToTheme()
{
    if (!m_mw) return;
    Config::clearStyleOverrides();
    m_pending.clear();   // discards any per-language pending edits too

    for (Buffer* b : m_mw->allBuffers()) {
        if (b) {
            Theme::applyEditorBaseStyles(b->editor());
            b->reapplyTheme();
        }
    }
    Buffer* activeBuf = m_mw->activePane()
                        ? m_mw->activePane()->currentBuffer() : nullptr;
    if (activeBuf) {
        auto* ed = activeBuf->editor();
        m_fg    = sciColorToQ(static_cast<int>(ed->send(
            static_cast<unsigned int>(Message::StyleGetFore), STYLE_DEFAULT)));
        m_bg    = sciColorToQ(static_cast<int>(ed->send(
            static_cast<unsigned int>(Message::StyleGetBack), STYLE_DEFAULT)));
        m_lnFg  = sciColorToQ(static_cast<int>(ed->send(
            static_cast<unsigned int>(Message::StyleGetFore), STYLE_LINENUMBER)));
        m_lnBg  = sciColorToQ(static_cast<int>(ed->send(
            static_cast<unsigned int>(Message::StyleGetBack), STYLE_LINENUMBER)));
        m_caret = sciColorToQ(static_cast<int>(ed->send(
            static_cast<unsigned int>(Message::GetCaretFore))));
    }
    refreshSwatches();
    if (!m_currentLangInternal.isEmpty()) loadStylesForCurrentLanguage();
}

// Phase 8a — drop only this language's overrides; keep the global tab's
// state intact.
void StyleConfigDialog::onResetThisLanguage()
{
    if (m_currentLangInternal.isEmpty() || !m_mw) return;

    // Drop pending edits scoped to this language.
    const QString prefix = QStringLiteral("lang:") + m_currentLangInternal + QChar(':');
    for (auto it = m_pending.begin(); it != m_pending.end(); ) {
        if (it.key().startsWith(prefix)) it = m_pending.erase(it);
        else                              ++it;
    }

    Config::clearPerLanguageOverridesFor(m_currentLangInternal);

    // Re-apply theme for every buffer that's on this language so the
    // reset is visible immediately. Other languages stay untouched.
    const QByteArray nameUtf8 = m_currentLangInternal.toUtf8();
    for (Buffer* b : m_mw->allBuffers()) {
        if (!b || !b->language()) continue;
        if (b->language()->internalName == nameUtf8.constData()) {
            b->reapplyTheme();
            const Scintilla::sptr_t end = b->editor()->send(
                static_cast<unsigned int>(Message::GetLength));
            b->editor()->send(static_cast<unsigned int>(Message::Colourise), 0, end);
        }
    }
    loadStylesForCurrentLanguage();   // refresh widgets to theme defaults
}
