#include "Buffer.h"

#include <QByteArray>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QStringList>
#include <QWidget>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <vector>

#include "Config.h"          // Phase 5X — smartHighlightEnabled / bracketAutoPairEnabled

#include "Backup.h"            // Phase 5AA — clearBuffer on save
#include "Languages.h"
#include "MacroManager.h"      // Phase 5Z — suppress auto-pair sends during recording
#include "Scintilla.h"         // SC_CP_UTF8, SC_MARGIN_NUMBER, STYLE_DEFAULT
#include "ILexer.h"            // Scintilla::ILexer5 — must precede Lexilla.h
#include "Lexilla.h"           // CreateLexer
#include "ScintillaEditBase.h"
#include "ScintillaMessages.h"
#include "Theme.h"
#include "UserDefineLang.h"    // Phase 5U.2 — UDL applyToLexer push

using Scintilla::Message;

namespace {
constexpr int kMarginNumberWidth   = 36;
constexpr int kMarginBookmark      = 1;       // margin index used for bookmarks
constexpr int kMarginBookmarkWidth = 16;
constexpr int kBookmarkMarker      = 24;      // marker number used for bookmarks
// Phase 9m.2 — smart-highlight match marker. Shares the bookmark margin
// (same gutter, smaller glyph). Slot 23 sits below kBookmarkMarker; both
// flow into the bookmark margin's mask.
constexpr int kSmartHighlightMarker = 23;

// Phase 5MK — Mark indicator base. Slots 10..14 are Mark 1..5. Slot 9 is
// the smart-highlight indicator; 0..7 reserved for built-in/lexer use; 8
// is the find-mark. INDIC_CONTAINER_MIN is 8 in Scintilla 5.x so 10..14
// are safe.
constexpr int kMarkIndicBase = 10;
constexpr int kBookmarkMask        =
    (1 << kBookmarkMarker) | (1 << kSmartHighlightMarker);
// Phase 9e — fold margin (index 2). Uses SC_MASK_FOLDERS; the seven
// SC_MARKNUM_FOLDER* slots (25-31) live in this mask. Margin width 14
// is the de-facto standard from upstream NPP; SetMarginSensitiveN(2,1)
// makes margin clicks fire SCN_MARGINCLICK so we can toggle the fold.
constexpr int kMarginFold      = 2;
constexpr int kMarginFoldWidth = 14;

// Phase 9b.4 — convert "#RRGGBB" to Scintilla packed colour
// (red | green<<8 | blue<<16). Mirrors Theme.cpp's sciColor helper.
// Returns light grey (0xC0C0C0) if the input is malformed.
int sciColorFromHex(const QString& hex)
{
    auto toComponent = [](QChar a, QChar b) -> int {
        bool oka = false, okb = false;
        const int hi = QString(a).toInt(&oka, 16);
        const int lo = QString(b).toInt(&okb, 16);
        if (!oka || !okb) return -1;
        return (hi << 4) | lo;
    };
    if (hex.size() != 7 || hex.at(0) != QChar('#')) {
        return 0xC0C0C0;   // r=g=b=0xC0, already in Scintilla's packed order
    }
    const int r = toComponent(hex.at(1), hex.at(2));
    const int g = toComponent(hex.at(3), hex.at(4));
    const int b = toComponent(hex.at(5), hex.at(6));
    if (r < 0 || g < 0 || b < 0) return 0xC0C0C0;
    return r | (g << 8) | (b << 16);
}

int sciEolValue(Buffer::EolMode m)
{
    // SC_EOL_CRLF/CR/LF are 0/1/2 — NOT sequential w.r.t. our enum order.
    switch (m) {
        case Buffer::EolMode::Crlf: return SC_EOL_CRLF;
        case Buffer::EolMode::Cr:   return SC_EOL_CR;
        case Buffer::EolMode::Lf:   return SC_EOL_LF;
    }
    return SC_EOL_LF;
}

Buffer::EolMode detectEolMode(const QString& text)
{
    if (text.contains(QStringLiteral("\r\n"))) return Buffer::EolMode::Crlf;
    if (text.contains(QChar('\n')))            return Buffer::EolMode::Lf;
    if (text.contains(QChar('\r')))            return Buffer::EolMode::Cr;
    return Buffer::EolMode::Lf;
}

// Phase 9i — collect every >=3-char identifier in the buffer for the
// autocomplete candidate list. Mirrors MainWindow's collectBufferWords
// helper; duplicated in Buffer's anon namespace so the trigger path
// doesn't have to reach across modules.
QStringList collectBufferWords(ScintillaEditBase* ed)
{
    const Scintilla::sptr_t length =
        ed->send(static_cast<unsigned int>(Scintilla::Message::GetTextLength));
    if (length <= 0) return {};
    std::vector<char> buf(static_cast<std::size_t>(length) + 1, '\0');
    ed->send(static_cast<unsigned int>(Scintilla::Message::GetText),
             static_cast<Scintilla::uptr_t>(buf.size()),
             reinterpret_cast<Scintilla::sptr_t>(buf.data()));
    const QString text = QString::fromUtf8(buf.data(), static_cast<int>(length));

    QSet<QString> set;
    QString token;
    token.reserve(32);
    auto flush = [&]{
        if (token.size() >= 3) set.insert(token);
        token.clear();
    };
    for (QChar c : text) {
        if (c.isLetterOrNumber() || c == QChar('_')) token.append(c);
        else flush();
    }
    flush();
    QStringList out = set.values();
    out.sort(Qt::CaseInsensitive);
    return out;
}
}

Buffer::Buffer(QWidget* parent)
    : QObject(parent),
      m_editor(new ScintillaEditBase(parent)),
      m_dirty(false),
      m_untitledIndex(0),
      m_language(Languages::plainText()),
      m_encoding(Encoding::defaultEncoding()),
      m_eolMode(EolMode::Lf)
{
    // Phase 5N.4 — apply New Document defaults from Config. These override
    // the initializer-list defaults above. Done before SetEOLMode below so
    // SetEOLMode picks up the right value.
    {
        const QString lang = Config::defaultLanguage();
        if (!lang.isEmpty()) {
            const QByteArray langBytes = lang.toUtf8();
            if (auto* L = Languages::findByInternalName(langBytes.constData())) {
                m_language = L;
            }
        }
        const QString encName = Config::defaultEncodingName();
        if (!encName.isEmpty()) {
            m_encoding.name   = encName;
            m_encoding.hasBom = Config::defaultEncodingHasBom();
        }
        const int eol = Config::defaultEolMode();
        if (eol == 0) m_eolMode = EolMode::Crlf;
        else if (eol == 1) m_eolMode = EolMode::Cr;
        else               m_eolMode = EolMode::Lf;
    }
    // Editor defaults: UTF-8 codepage, line-number margin. Font/colours come
    // from Theme::applyEditorBaseStyles below.
    m_editor->send(static_cast<unsigned int>(Message::SetCodePage), SC_CP_UTF8, 0);
    m_editor->send(static_cast<unsigned int>(Message::SetEOLMode),
                   sciEolValue(m_eolMode), 0);
    m_editor->send(static_cast<unsigned int>(Message::SetMarginWidthN), 0,
                   kMarginNumberWidth);
    m_editor->send(static_cast<unsigned int>(Message::SetMarginTypeN), 0,
                   SC_MARGIN_NUMBER);

    // Bookmark margin (index 1). Symbol margin sized for one marker; only
    // bookmark markers (kBookmarkMarker) appear here.
    m_editor->send(static_cast<unsigned int>(Message::SetMarginTypeN),
                   kMarginBookmark, SC_MARGIN_SYMBOL);
    m_editor->send(static_cast<unsigned int>(Message::SetMarginWidthN),
                   kMarginBookmark, kMarginBookmarkWidth);
    m_editor->send(static_cast<unsigned int>(Message::SetMarginMaskN),
                   kMarginBookmark, kBookmarkMask);
    m_editor->send(static_cast<unsigned int>(Message::SetMarginSensitiveN),
                   kMarginBookmark, 1);
    m_editor->send(static_cast<unsigned int>(Message::MarkerDefine),
                   kBookmarkMarker, SC_MARK_BOOKMARK);
    // Phase 7e — bookmark marker colour comes from the active theme's
    // "Bookmark margin" / "Mark Style 1" globals. Falls back to red if
    // unset. Re-applied in reapplyTheme on theme switch.
    m_editor->send(static_cast<unsigned int>(Message::MarkerSetFore),
                   kBookmarkMarker, Theme::bookmarkMarkerFore());
    m_editor->send(static_cast<unsigned int>(Message::MarkerSetBack),
                   kBookmarkMarker, Theme::bookmarkMarkerBack());

    // Phase 9m.2 — smart-highlight match marker. SC_MARK_SMALLRECT is a
    // narrow filled square that sits unobtrusively next to a bookmark
    // when both share a line. The colour follows the active theme's
    // "Smart Highlighting" global so the gutter glyph matches the
    // in-text indicator visually.
    m_editor->send(static_cast<unsigned int>(Message::MarkerDefine),
                   kSmartHighlightMarker, SC_MARK_SMALLRECT);
    m_editor->send(static_cast<unsigned int>(Message::MarkerSetFore),
                   kSmartHighlightMarker, Theme::smartHighlightFore());
    m_editor->send(static_cast<unsigned int>(Message::MarkerSetBack),
                   kSmartHighlightMarker, Theme::smartHighlightFore());

    // Phase 9e — fold margin (index 2). SC_MARGIN_SYMBOL with mask
    // SC_MASK_FOLDERS reserves the 25..31 marker slots for the seven
    // fold markers (FOLDER, FOLDEROPEN, FOLDEREND, FOLDEROPENMID,
    // FOLDERMIDTAIL, FOLDERTAIL, FOLDERSUB). Sensitive=1 routes margin
    // clicks to the marginClicked signal so the user can toggle folds.
    m_editor->send(static_cast<unsigned int>(Message::SetMarginTypeN),
                   kMarginFold, SC_MARGIN_SYMBOL);
    m_editor->send(static_cast<unsigned int>(Message::SetMarginWidthN),
                   kMarginFold, kMarginFoldWidth);
    m_editor->send(static_cast<unsigned int>(Message::SetMarginMaskN),
                   kMarginFold, SC_MASK_FOLDERS);
    m_editor->send(static_cast<unsigned int>(Message::SetMarginSensitiveN),
                   kMarginFold, 1);
    // Marker shapes — upstream's Box-style choice (also matches modern
    // editors like VSCode). The seven slots: 30=collapsed-header,
    // 31=expanded-header, 25=end-of-fold, 26=expanded-mid, 27=mid-tail,
    // 28=tail, 29=sub-line continuation.
    m_editor->send(static_cast<unsigned int>(Message::MarkerDefine),
                   SC_MARKNUM_FOLDER,         SC_MARK_BOXPLUS);
    m_editor->send(static_cast<unsigned int>(Message::MarkerDefine),
                   SC_MARKNUM_FOLDEROPEN,     SC_MARK_BOXMINUS);
    m_editor->send(static_cast<unsigned int>(Message::MarkerDefine),
                   SC_MARKNUM_FOLDEREND,      SC_MARK_BOXPLUSCONNECTED);
    m_editor->send(static_cast<unsigned int>(Message::MarkerDefine),
                   SC_MARKNUM_FOLDEROPENMID,  SC_MARK_BOXMINUSCONNECTED);
    m_editor->send(static_cast<unsigned int>(Message::MarkerDefine),
                   SC_MARKNUM_FOLDERMIDTAIL,  SC_MARK_TCORNER);
    m_editor->send(static_cast<unsigned int>(Message::MarkerDefine),
                   SC_MARKNUM_FOLDERTAIL,     SC_MARK_LCORNER);
    m_editor->send(static_cast<unsigned int>(Message::MarkerDefine),
                   SC_MARKNUM_FOLDERSUB,      SC_MARK_VLINE);
    // Theme-driven marker colours. Theme::foldMarkerFore/Back fall back
    // to sensible defaults when the active theme doesn't define them.
    for (int n : {SC_MARKNUM_FOLDER, SC_MARKNUM_FOLDEROPEN,
                  SC_MARKNUM_FOLDEREND, SC_MARKNUM_FOLDEROPENMID,
                  SC_MARKNUM_FOLDERMIDTAIL, SC_MARKNUM_FOLDERTAIL,
                  SC_MARKNUM_FOLDERSUB}) {
        m_editor->send(static_cast<unsigned int>(Message::MarkerSetFore),
                       n, Theme::foldMarkerFore());
        m_editor->send(static_cast<unsigned int>(Message::MarkerSetBack),
                       n, Theme::foldMarkerBack());
    }
    // Show contracted line as highlighted; line-after-fold gets a
    // subtle line. SC_FOLDFLAG_LINEAFTER_CONTRACTED makes the fold
    // boundary visible without needing a separator margin.
    m_editor->send(static_cast<unsigned int>(Message::SetFoldFlags),
                   SC_FOLDFLAG_LINEAFTER_CONTRACTED, 0);
    // Wire margin-click → toggle fold. Connection is per-Buffer so each
    // editor has its own slot binding; lambda captures `this`.
    connect(m_editor, &ScintillaEditBase::marginClicked,
            this, &Buffer::onMarginClicked);

    Theme::applyEditorBaseStyles(m_editor);

    // Phase 5Y — enable multi-selection. Ctrl+click adds a caret;
    // typing replicates across every caret; paste pastes once per
    // caret (SC_MULTIPASTE_EACH). Single-cursor users see no change.
    m_editor->send(static_cast<unsigned int>(Message::SetMultipleSelection), 1, 0);
    m_editor->send(static_cast<unsigned int>(Message::SetAdditionalSelectionTyping), 1, 0);
    m_editor->send(static_cast<unsigned int>(Message::SetMultiPaste),
                   SC_MULTIPASTE_EACH, 0);

    connect(m_editor, &ScintillaEditBase::savePointChanged,
            this, &Buffer::onSavePointChanged);

    // Phase 5X — smart-edit signal hooks.
    connect(m_editor, &ScintillaEditBase::charAdded,
            this, &Buffer::onCharAdded);
    connect(m_editor, &ScintillaEditBase::updateUi,
            this, &Buffer::onUpdateUi);

    // Phase 5X — smart-highlight indicator (slot #9, INDIC_ROUNDBOX).
    // Slots 0-7 are reserved for built-in / lexer use; 8 is conventionally
    // the find-mark indicator; 9 is safe for our highlight-all-instances.
    // Phase 9m.2 — colour now from theme so it matches the new margin
    // marker; reapplyTheme re-pushes on theme switch.
    constexpr int kSmartIndic = 9;
    m_editor->send(static_cast<unsigned int>(Message::IndicSetStyle),
                   kSmartIndic, INDIC_ROUNDBOX);
    m_editor->send(static_cast<unsigned int>(Message::IndicSetFore),
                   kSmartIndic, Theme::smartHighlightFore());
    m_editor->send(static_cast<unsigned int>(Message::IndicSetAlpha),
                   kSmartIndic, 80);
    m_editor->send(static_cast<unsigned int>(Message::IndicSetUnder),
                   kSmartIndic, 1);

    // Phase 5MK — Mark indicator slots 10..14 (kMarkIndicBase + 0..4).
    // Each Mark N (1..5) in Find/Replace's Mark tab fills its own slot
    // with a distinct theme-aware colour from Theme::markStyleFore(N).
    // INDIC_ROUNDBOX with alpha + Under so the marks render behind the
    // text without obscuring the lexer's own highlight indicators.
    for (int i = 0; i < 5; ++i) {
        const int slot = kMarkIndicBase + i;
        m_editor->send(static_cast<unsigned int>(Message::IndicSetStyle),
                       slot, INDIC_ROUNDBOX);
        m_editor->send(static_cast<unsigned int>(Message::IndicSetFore),
                       slot, Theme::markStyleFore(i + 1));
        m_editor->send(static_cast<unsigned int>(Message::IndicSetAlpha),
                       slot, 80);
        m_editor->send(static_cast<unsigned int>(Message::IndicSetUnder),
                       slot, 1);
    }

    // Phase 5N.3 — caret width / blink rate read from Config so new buffers
    // (newUntitled / openFile / detached-then-adopted) inherit the user's
    // Preferences-set caret. Defaults match Scintilla's defaults (width 1,
    // 500 ms blink) so first-run users see no change.
    m_editor->send(static_cast<unsigned int>(Message::SetCaretWidth),
                   static_cast<Scintilla::uptr_t>(Config::caretWidth()));
    m_editor->send(static_cast<unsigned int>(Message::SetCaretPeriod),
                   static_cast<Scintilla::uptr_t>(Config::caretBlinkMs()));

    // Phase 9b.4 — vertical-edge marker. EDGE_LINE renders a thin vertical
    // bar at the configured column; EDGE_NONE hides it. Apply once per
    // buffer at construction; MainWindow::applyEditingPrefsFromConfig
    // pushes updates to every open buffer on Preferences Apply.
    m_editor->send(static_cast<unsigned int>(Message::SetEdgeMode),
                   Config::verticalEdgeEnabled() ? EDGE_LINE : EDGE_NONE, 0);
    m_editor->send(static_cast<unsigned int>(Message::SetEdgeColumn),
                   static_cast<Scintilla::uptr_t>(Config::verticalEdgeColumn()), 0);
    m_editor->send(static_cast<unsigned int>(Message::SetEdgeColour),
                   static_cast<Scintilla::uptr_t>(
                       sciColorFromHex(Config::verticalEdgeColor())),
                   0);

    // Phase 5N.4 — push the resolved default language to Scintilla. The
    // initializer list set m_language; setLanguage installs the Lexilla
    // lexer + applies the theme's per-language styles. Always call this
    // (even when the default IS plain text) so the path is exercised
    // consistently.
    if (m_language) setLanguage(m_language);

    // Phase 5N.8 / 9f — initial tab width / indent / use-tabs. Phase 9f's
    // applyResolvedIndent honours any per-language override before
    // falling back to Config::defaultTabWidth / defaultUseTabs. Called
    // unconditionally (setLanguage's early-return when the language
    // didn't change would otherwise skip the indent push for the
    // default-language case where m_language was pre-set in the
    // initializer list).
    applyResolvedIndent();
}

Buffer::~Buffer() = default;

QString Buffer::displayName() const
{
    if (!m_filePath.isEmpty()) return QFileInfo(m_filePath).fileName();
    if (m_untitledIndex > 0)
        return QObject::tr("Untitled %1").arg(m_untitledIndex);
    return QObject::tr("Untitled");
}

bool Buffer::loadFromFile(const QString& path, QString* errorOut)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (errorOut) *errorOut = f.errorString();
        return false;
    }
    QByteArray data = f.readAll();
    f.close();

    // Phase 5c: detect encoding (BOM first, else uchardet, else UTF-8).
    // detect() may strip a BOM from 'data' as a side effect.
    m_encoding = Encoding::detect(data);
    const QString text = Encoding::decode(data, m_encoding);
    const QByteArray utf8 = text.toUtf8();

    m_editor->sends(static_cast<unsigned int>(Message::ClearAll), 0, "");
    m_editor->sends(static_cast<unsigned int>(Message::AddText),
                    static_cast<Scintilla::uptr_t>(utf8.size()), utf8.constData());
    m_editor->send(static_cast<unsigned int>(Message::EmptyUndoBuffer));
    m_editor->send(static_cast<unsigned int>(Message::SetSavePoint));
    m_editor->send(static_cast<unsigned int>(Message::GotoPos), 0, 0);

    // Detect EOL convention from the loaded content. Newly-typed lines will
    // match the file's existing convention.
    m_eolMode = detectEolMode(text);
    m_editor->send(static_cast<unsigned int>(Message::SetEOLMode),
                   sciEolValue(m_eolMode), 0);

    setFilePath(path);
    emit encodingChanged(this);
    emit eolModeChanged(this);
    refreshMtime();

    // Auto-detect language from the path's extension and re-style.
    setLanguage(Languages::findByExtension(path));

    // Phase 5AB — auto-detect indent style. Sample the first 100 lines;
    // if the majority lead with tabs, use tabs; otherwise, use spaces
    // and infer the indent width as the smallest non-zero leading-space
    // count observed.
    //
    // Phase 9f — when the active language has a per-language indent
    // override, the user has said "always use my override regardless of
    // the file's existing style." Skip the auto-detect entirely so
    // setLanguage's pushed values stick.
    if (m_language && Config::hasLanguageIndentOverride(
            QString::fromUtf8(m_language->internalName.c_str()))) {
        return true;
    }
    {
        int tabLines = 0;
        int spaceLines = 0;
        int minSpaceRun = 0;     // smallest non-zero leading-space count
        int sampledLines = 0;
        int linesEnumerated = 0;
        const QStringList sampleLines = text.left(20000)
            .split(QChar('\n'), Qt::KeepEmptyParts);
        for (const QString& ln : sampleLines) {
            if (++linesEnumerated > 100) break;
            if (ln.isEmpty()) continue;
            QChar first = ln.at(0);
            if (first == QChar('\t')) {
                ++tabLines;
                ++sampledLines;
            } else if (first == QChar(' ')) {
                int run = 0;
                while (run < ln.size() && ln.at(run) == QChar(' ')) ++run;
                ++spaceLines;
                ++sampledLines;
                if (run > 0 && (minSpaceRun == 0 || run < minSpaceRun))
                    minSpaceRun = run;
            }
        }
        if (sampledLines >= 4) {
            const bool useTabs = tabLines > spaceLines;
            m_editor->send(static_cast<unsigned int>(Message::SetUseTabs),
                           useTabs ? 1 : 0, 0);
            if (!useTabs && minSpaceRun > 0 && minSpaceRun <= 8) {
                m_editor->send(static_cast<unsigned int>(Message::SetIndent),
                               minSpaceRun, 0);
                m_editor->send(static_cast<unsigned int>(Message::SetTabWidth),
                               minSpaceRun, 0);
            }
        }
    }
    return true;
}

bool Buffer::saveToFile(const QString& path, QString* errorOut)
{
    const Scintilla::sptr_t length =
        m_editor->send(static_cast<unsigned int>(Message::GetTextLength));

    std::vector<char> buf(static_cast<std::size_t>(length) + 1, '\0');
    m_editor->send(static_cast<unsigned int>(Message::GetText),
                   static_cast<Scintilla::uptr_t>(buf.size()),
                   reinterpret_cast<Scintilla::sptr_t>(buf.data()));

    // Phase 5c: re-encode UTF-8 → target charset (+ BOM if applicable).
    const QString text = QString::fromUtf8(buf.data(), static_cast<int>(length));
    const QByteArray bytes = Encoding::encode(text, m_encoding);

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorOut) *errorOut = f.errorString();
        return false;
    }
    const qint64 written = f.write(bytes);
    f.close();
    if (written != bytes.size()) {
        if (errorOut)
            *errorOut = QObject::tr("Short write: %1 / %2 bytes").arg(written).arg(bytes.size());
        return false;
    }

    m_editor->send(static_cast<unsigned int>(Message::SetSavePoint));
    setFilePath(path);
    refreshMtime();

    // Phase 5AA — clean save invalidates the crash-recovery snapshot.
    Backup::clearBuffer(this);
    return true;
}

bool Buffer::writeCopyTo(const QString& path, QString* errorOut)
{
    // Same encode-and-write as saveToFile, but no buffer-state mutation
    // (no setFilePath, no SetSavePoint). Lets File -> Save a Copy As
    // emit a snapshot to disk while leaving the live buffer untouched.
    const Scintilla::sptr_t length =
        m_editor->send(static_cast<unsigned int>(Message::GetTextLength));

    std::vector<char> buf(static_cast<std::size_t>(length) + 1, '\0');
    m_editor->send(static_cast<unsigned int>(Message::GetText),
                   static_cast<Scintilla::uptr_t>(buf.size()),
                   reinterpret_cast<Scintilla::sptr_t>(buf.data()));

    const QString text = QString::fromUtf8(buf.data(), static_cast<int>(length));
    const QByteArray bytes = Encoding::encode(text, m_encoding);

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorOut) *errorOut = f.errorString();
        return false;
    }
    const qint64 written = f.write(bytes);
    f.close();
    if (written != bytes.size()) {
        if (errorOut)
            *errorOut = QObject::tr("Short write: %1 / %2 bytes")
                .arg(written).arg(bytes.size());
        return false;
    }
    return true;
}

void Buffer::setFilePath(const QString& path)
{
    if (m_filePath == path) return;
    m_filePath = path;
    emit displayNameChanged(this);
}

void Buffer::refreshMtime()
{
    if (m_filePath.isEmpty()) {
        m_lastKnownMtime = 0;
        return;
    }
    const QFileInfo fi(m_filePath);
    m_lastKnownMtime = fi.exists() ? fi.lastModified().toMSecsSinceEpoch() : 0;
}

void Buffer::setLanguage(const LanguageDef* lang)
{
    if (lang == nullptr) lang = Languages::plainText();
    if (lang == m_language) return;   // base styles already set in ctor
    m_language = lang;

    // Detach the previous lexer, if any, before installing the new one.
    m_editor->send(static_cast<unsigned int>(Message::SetILexer), 0, 0);

    // "null" lexilla = plain text. Skip CreateLexer entirely.
    if (!lang->lexilla.empty() && lang->lexilla != "null") {
        Scintilla::ILexer5* lex = ::CreateLexer(lang->lexilla.c_str());
        if (lex) {
            m_editor->send(static_cast<unsigned int>(Message::SetILexer), 0,
                           reinterpret_cast<Scintilla::sptr_t>(lex));
            // Phase 9e — enable lexer fold-level computation. Without
            // `fold=1` the lexer leaves SC_FOLDLEVELHEADERFLAG unset
            // and the fold margin shows no markers. Each lexer that
            // supports folding honours this property.
            m_editor->send(static_cast<unsigned int>(Message::SetProperty),
                           reinterpret_cast<Scintilla::uptr_t>("fold"),
                           reinterpret_cast<Scintilla::sptr_t>("1"));
            // Phase 5U.2 — UDL branch. When lexilla == "user", the
            // LanguageDef's own keyword vector is empty; the UDL's
            // keyword tables + properties are pushed via applyToLexer.
            if (lang->userDefined != nullptr) {
                UserDefineLang::applyToLexer(m_editor, *lang->userDefined);
            } else {
                for (std::size_t i = 0; i < lang->keywords.size() && i < 8; ++i) {
                    if (lang->keywords[i].empty()) continue;
                    m_editor->sends(static_cast<unsigned int>(Message::SetKeyWords),
                                    static_cast<Scintilla::uptr_t>(i),
                                    lang->keywords[i].c_str());
                }
            }
        }
    }

    Theme::applyForLanguage(m_editor, lang->internalName.c_str());

    // Phase 9f — re-apply the resolved per-language indent on every
    // language switch so flipping A → B → A lands at A's override (not
    // whatever Phase 5AB's auto-detect last set).
    applyResolvedIndent();

    // Re-style the whole buffer immediately so the user sees the change.
    const Scintilla::sptr_t end =
        m_editor->send(static_cast<unsigned int>(Message::GetLength));
    m_editor->send(static_cast<unsigned int>(Message::Colourise), 0, end);

    emit languageChanged(this);
}

void Buffer::reapplyTheme()
{
    if (!m_language) return;
    Theme::applyForLanguage(m_editor, m_language->internalName.c_str());
    // Phase 7e — bookmark marker colour follows theme too.
    m_editor->send(static_cast<unsigned int>(Message::MarkerSetFore),
                   kBookmarkMarker, Theme::bookmarkMarkerFore());
    m_editor->send(static_cast<unsigned int>(Message::MarkerSetBack),
                   kBookmarkMarker, Theme::bookmarkMarkerBack());
    // Phase 9m.2 — smart-highlight marker + in-text indicator follow
    // theme on every switch (so the in-text yellow doesn't linger when
    // a green-themed dark mode is selected).
    m_editor->send(static_cast<unsigned int>(Message::MarkerSetFore),
                   kSmartHighlightMarker, Theme::smartHighlightFore());
    m_editor->send(static_cast<unsigned int>(Message::MarkerSetBack),
                   kSmartHighlightMarker, Theme::smartHighlightFore());
    m_editor->send(static_cast<unsigned int>(Message::IndicSetFore),
                   9 /* kSmartIndic */, Theme::smartHighlightFore());
    // Phase 5MK — re-push Mark 1..5 indicator colours from the active
    // theme. Existing marks stay intact; only the colour swaps.
    for (int i = 0; i < 5; ++i) {
        m_editor->send(static_cast<unsigned int>(Message::IndicSetFore),
                       kMarkIndicBase + i, Theme::markStyleFore(i + 1));
    }
    // Phase 9e — fold marker colours follow theme on every theme switch.
    for (int n : {SC_MARKNUM_FOLDER, SC_MARKNUM_FOLDEROPEN,
                  SC_MARKNUM_FOLDEREND, SC_MARKNUM_FOLDEROPENMID,
                  SC_MARKNUM_FOLDERMIDTAIL, SC_MARKNUM_FOLDERTAIL,
                  SC_MARKNUM_FOLDERSUB}) {
        m_editor->send(static_cast<unsigned int>(Message::MarkerSetFore),
                       n, Theme::foldMarkerFore());
        m_editor->send(static_cast<unsigned int>(Message::MarkerSetBack),
                       n, Theme::foldMarkerBack());
    }
    const Scintilla::sptr_t end =
        m_editor->send(static_cast<unsigned int>(Message::GetLength));
    m_editor->send(static_cast<unsigned int>(Message::Colourise), 0, end);
}

bool Buffer::reloadAsEncoding(const EncodingInfo& enc, QString* errorOut)
{
    if (m_filePath.isEmpty()) {
        // No file on disk yet. Just record the new encoding so the next
        // saveAs writes with it.
        m_encoding = enc;
        emit encodingChanged(this);
        return true;
    }
    QFile f(m_filePath);
    if (!f.open(QIODevice::ReadOnly)) {
        if (errorOut) *errorOut = f.errorString();
        return false;
    }
    QByteArray data = f.readAll();
    f.close();

    // For an explicit re-decode we honour the user's choice — skip auto
    // BOM detection and use the encoding they picked. But still strip a
    // matching BOM if it's there so the visible content stays clean.
    EncodingInfo applied = enc;
    QByteArray view = data;
    EncodingInfo bomFound;
    QByteArray probe = view;
    Encoding::detect(probe);   // mutates probe to BOM-stripped if present
    if (probe.size() != view.size()) {
        // BOM was stripped. Prefer the user's chosen encoding name; only the
        // hasBom flag rolls in from the actual file.
        view = std::move(probe);
        applied.hasBom = true;
    }

    const QString text = Encoding::decode(view, applied);
    const QByteArray utf8 = text.toUtf8();

    m_editor->send(static_cast<unsigned int>(Message::BeginUndoAction));
    m_editor->sends(static_cast<unsigned int>(Message::ClearAll), 0, "");
    m_editor->sends(static_cast<unsigned int>(Message::AddText),
                    static_cast<Scintilla::uptr_t>(utf8.size()), utf8.constData());
    m_editor->send(static_cast<unsigned int>(Message::EndUndoAction));
    m_editor->send(static_cast<unsigned int>(Message::SetSavePoint));

    m_encoding = applied;
    emit encodingChanged(this);
    return true;
}

void Buffer::convertToEncoding(const EncodingInfo& enc)
{
    if (enc == m_encoding) return;
    m_encoding = enc;
    emit encodingChanged(this);
    // The visible text doesn't change but the bytes that will be written to
    // disk do — mark the buffer dirty so the user knows there's something to
    // save. Scintilla won't know we changed encoding so we drive the dirty
    // bit directly.
    if (!m_dirty) {
        m_dirty = true;
        emit dirtyChanged(this, true);
    }
}

void Buffer::setEolMode(EolMode mode)
{
    if (mode == m_eolMode) return;
    m_eolMode = mode;
    m_editor->send(static_cast<unsigned int>(Message::SetEOLMode),
                   sciEolValue(m_eolMode), 0);
    emit eolModeChanged(this);
}

void Buffer::convertEol(EolMode mode)
{
    // Always update Scintilla's EOL setting so newly-typed lines match what
    // the user just chose, even when the existing bytes are already in that
    // mode (no-op convert is fine).
    m_editor->send(static_cast<unsigned int>(Message::SetEOLMode),
                   sciEolValue(mode), 0);
    m_editor->send(static_cast<unsigned int>(Message::BeginUndoAction));
    m_editor->send(static_cast<unsigned int>(Message::ConvertEOLs),
                   sciEolValue(mode), 0);
    m_editor->send(static_cast<unsigned int>(Message::EndUndoAction));
    if (mode != m_eolMode) {
        m_eolMode = mode;
        emit eolModeChanged(this);
    }
}

void Buffer::setReadOnly(bool ro)
{
    if (ro == m_readOnly) return;
    m_readOnly = ro;
    m_editor->send(static_cast<unsigned int>(Message::SetReadOnly),
                   ro ? 1 : 0, 0);
    emit readOnlyChanged(this);
}

void Buffer::setPinned(bool pinned)
{
    if (pinned == m_pinned) return;
    m_pinned = pinned;
    emit pinnedChanged(this);
}

void Buffer::clearSmartHighlightCache()
{
    constexpr int kSmartIndic = 9;
    const Scintilla::sptr_t end =
        m_editor->send(static_cast<unsigned int>(Message::GetLength));
    m_editor->send(static_cast<unsigned int>(Message::SetIndicatorCurrent),
                   kSmartIndic, 0);
    m_editor->send(static_cast<unsigned int>(Message::IndicatorClearRange),
                   0, end);
    // Phase 9m.2 — drop the gutter glyphs in lock-step with the in-text
    // indicator. Otherwise stale glyphs linger after a Preferences toggle.
    m_editor->send(static_cast<unsigned int>(Message::MarkerDeleteAll),
                   kSmartHighlightMarker, 0);
    m_smartHighlightWord.clear();
}

// Phase 5MK — Mark feature. Walks the document with SearchInTarget
// (same loop as smart-highlight's onUpdateUi) and fills indicator slot
// kMarkIndicBase+(markN-1) for every match. 5000-match cap matches the
// "add cursors at all matches" path (Phase 9c.3).
int Buffer::markAllOccurrences(const QString& text, int markN,
                               bool matchCase, bool wholeWord)
{
    if (text.isEmpty() || markN < 1 || markN > 5) return 0;
    const QByteArray needle = text.toUtf8();
    if (needle.isEmpty()) return 0;
    const int slot = kMarkIndicBase + (markN - 1);

    const Scintilla::sptr_t docEnd =
        m_editor->send(static_cast<unsigned int>(Message::GetLength));

    int flags = 0;
    if (matchCase) flags |= SCFIND_MATCHCASE;
    if (wholeWord) flags |= SCFIND_WHOLEWORD;
    m_editor->send(static_cast<unsigned int>(Message::SetSearchFlags), flags, 0);

    m_editor->send(static_cast<unsigned int>(Message::SetIndicatorCurrent),
                   slot, 0);

    Scintilla::sptr_t cursor = 0;
    int count = 0;
    while (cursor < docEnd && count < 5000) {
        m_editor->send(static_cast<unsigned int>(Message::SetTargetRange),
                       static_cast<Scintilla::uptr_t>(cursor),
                       static_cast<Scintilla::sptr_t>(docEnd));
        const Scintilla::sptr_t found = m_editor->sends(
            static_cast<unsigned int>(Message::SearchInTarget),
            static_cast<Scintilla::uptr_t>(needle.size()),
            needle.constData());
        if (found < 0) break;
        const Scintilla::sptr_t targetEnd = m_editor->send(
            static_cast<unsigned int>(Message::GetTargetEnd));
        m_editor->send(static_cast<unsigned int>(Message::IndicatorFillRange),
                       static_cast<Scintilla::uptr_t>(found),
                       targetEnd - found);
        ++count;
        cursor = (targetEnd > found) ? targetEnd : found + 1;
    }
    return count;
}

void Buffer::clearAllMarks()
{
    const Scintilla::sptr_t docEnd =
        m_editor->send(static_cast<unsigned int>(Message::GetLength));
    for (int i = 0; i < 5; ++i) {
        m_editor->send(static_cast<unsigned int>(Message::SetIndicatorCurrent),
                       kMarkIndicBase + i, 0);
        m_editor->send(static_cast<unsigned int>(Message::IndicatorClearRange),
                       0, docEnd);
    }
}

void Buffer::applyResolvedIndent()
{
    if (!m_language) return;
    const QString name = QString::fromUtf8(m_language->internalName.c_str());
    const Config::LanguageIndent ind =
        Config::hasLanguageIndentOverride(name)
            ? Config::languageIndentOverride(name)
            : Config::LanguageIndent{
                  Config::defaultTabWidth(), Config::defaultUseTabs() };
    m_editor->send(static_cast<unsigned int>(Message::SetTabWidth),
                   static_cast<Scintilla::uptr_t>(ind.tabWidth));
    m_editor->send(static_cast<unsigned int>(Message::SetIndent),
                   static_cast<Scintilla::uptr_t>(ind.tabWidth));
    m_editor->send(static_cast<unsigned int>(Message::SetUseTabs),
                   ind.useTabs ? 1 : 0, 0);
}

// Phase 9i — show the autocomplete popup with the current word as
// prefix. Returns true when the popup was actually shown. Called
// from MainWindow::onEditAutocomplete (manualTrigger=true) and from
// Buffer::onCharAdded's auto-trigger path (manualTrigger=false).
bool Buffer::triggerAutocomplete(bool /*manualTrigger*/)
{
    if (!m_editor) return false;
    const Scintilla::sptr_t pos =
        m_editor->send(static_cast<unsigned int>(Message::GetCurrentPos));
    const Scintilla::sptr_t wordStart =
        m_editor->send(static_cast<unsigned int>(Message::WordStartPosition),
                       static_cast<Scintilla::uptr_t>(pos), true);
    const int prefixLen = static_cast<int>(pos - wordStart);
    if (prefixLen < 1) return false;

    QSet<QString> set;
    for (const QString& w : collectBufferWords(m_editor)) set.insert(w);
    if (m_language) {
        for (const auto& kwGroup : m_language->keywords) {
            const QString flat = QString::fromUtf8(kwGroup.c_str());
            for (const QString& kw : flat.split(QChar(' '), Qt::SkipEmptyParts))
                if (kw.size() >= 1) set.insert(kw);
        }
    }

    QStringList candidates = set.values();
    std::sort(candidates.begin(), candidates.end(),
        [](const QString& a, const QString& b) {
            return a.compare(b, Qt::CaseInsensitive) < 0;
        });
    if (candidates.isEmpty()) return false;

    const QByteArray bytes = candidates.join(QChar(' ')).toUtf8();
    m_editor->sends(static_cast<unsigned int>(Message::AutoCShow),
                    static_cast<Scintilla::uptr_t>(prefixLen),
                    bytes.constData());
    return true;
}

// Phase 9e — fold-margin click. Toggles the fold of the line under the
// click. Bookmark margin clicks are handled via a different path
// (Search → Toggle Bookmark + the existing kBookmarkMarker plumbing);
// here we only react when the click lands in the fold margin.
void Buffer::onMarginClicked(Scintilla::Position position,
                             Scintilla::KeyMod   /*modifiers*/,
                             int                 margin)
{
    if (margin != kMarginFold) return;
    const Scintilla::sptr_t line =
        m_editor->send(static_cast<unsigned int>(Message::LineFromPosition),
                       static_cast<Scintilla::uptr_t>(position));
    m_editor->send(static_cast<unsigned int>(Message::ToggleFold),
                   static_cast<Scintilla::uptr_t>(line), 0);
}

void Buffer::onSavePointChanged(bool dirty)
{
    // ScintillaEditBase emits savePointChanged(true) on SavePointLeft (modified)
    // and savePointChanged(false) on SavePointReached (clean) — the bool IS the
    // dirty flag, not its inverse.
    if (dirty == m_dirty) return;
    m_dirty = dirty;
    emit dirtyChanged(this, m_dirty);
}

// Phase 5X — bracket auto-pair on SCN_CHARADDED.
//
// SCN_CHARADDED fires AFTER the char is in the buffer. Sequence for the
// "type ( then ) to confirm" flow:
//   1. User types '(' -> buffer holds "(", caret AFTER it. We insert ')'
//      at caret -> buffer is "()", caret stays where it was (between).
//      Pending state: closer=')', pos=caret (the position of the ')').
//   2. User types ')' -> SCN_CHARADDED with ch=')'. Buffer is now "())",
//      caret is one past the ')' the user just typed (at pos+1). The
//      auto-inserted ')' from step 1 is now at pos+1 too — well,
//      technically: the user's ')' was inserted at the OLD caret pos,
//      pushing our auto-')' to pos+1. The user's caret moved to pos+1.
//      We delete the auto-')' (now at caret position, the SECOND char
//      after the original) and end up with "()" with caret after.
void Buffer::onCharAdded(int ch)
{
    // Phase 9i — auto-trigger autocomplete after N identifier chars.
    // Runs after every char regardless of bracket-auto-pair state. Fires
    // only when (a) auto-trigger is enabled, (b) the just-typed char is
    // an identifier char, (c) the current word's length is at least the
    // configured threshold, (d) the popup isn't already showing. The
    // popup, once shown, is auto-updated by Scintilla as the user keeps
    // typing — we just need to fire SCI_AUTOCSHOW once.
    auto maybeTriggerAutocomplete = [this, ch]{
        if (!Config::autocompleteAutoTrigger()) return;
        const QChar qc(static_cast<char16_t>(ch));
        const bool isIdent = qc.isLetterOrNumber() || qc == QChar('_');
        if (!isIdent) return;
        if (m_editor->send(
                static_cast<unsigned int>(Message::AutoCActive)) != 0) return;
        const Scintilla::sptr_t curPos =
            m_editor->send(static_cast<unsigned int>(Message::GetCurrentPos));
        const Scintilla::sptr_t wordStart =
            m_editor->send(static_cast<unsigned int>(Message::WordStartPosition),
                           static_cast<Scintilla::uptr_t>(curPos), true);
        const int prefixLen = static_cast<int>(curPos - wordStart);
        if (prefixLen < Config::autocompleteTriggerChars()) return;
        triggerAutocomplete(false);
    };

    if (!Config::bracketAutoPairEnabled()) {
        m_pendingPairCloser = QChar();
        m_pendingPairPos = -1;
        maybeTriggerAutocomplete();
        return;
    }

    const Scintilla::sptr_t pos =
        m_editor->send(static_cast<unsigned int>(Message::GetCurrentPos));

    // Phase 5Z — auto-pair sends are programmatic, not user keystrokes.
    // While the user is recording a macro on this editor, suppress the
    // SCN_MACRORECORD that would fire for our InsertText / DeleteRange
    // by wrapping each in StopRecord/StartRecord. Without this the macro
    // would replay the closer twice (once for the user's '(' which
    // triggers our auto-insert, once for our recorded InsertText).
    const bool suppressRec =
        MacroManager::instance().isRecordingFor(m_editor);

    // "Type closer to confirm" — user's typed char matches the closer
    // we previously auto-inserted, and the auto-closer is right where
    // the caret now sits. Remove the duplicate.
    if (!m_pendingPairCloser.isNull()
     && QChar(static_cast<char16_t>(ch)) == m_pendingPairCloser
     && static_cast<int>(pos) == m_pendingPairPos + 1) {
        if (suppressRec)
            m_editor->send(static_cast<unsigned int>(Message::StopRecord), 0, 0);
        m_editor->send(static_cast<unsigned int>(Message::DeleteRange),
                       static_cast<Scintilla::uptr_t>(pos), 1);
        if (suppressRec)
            m_editor->send(static_cast<unsigned int>(Message::StartRecord), 0, 0);
        m_pendingPairCloser = QChar();
        m_pendingPairPos = -1;
        return;
    }

    // Otherwise any unrelated edit invalidates the pending state.
    m_pendingPairCloser = QChar();
    m_pendingPairPos = -1;

    // Look up the closer for the just-typed opener.
    QChar closer;
    switch (ch) {
        case '(': closer = QChar(')'); break;
        case '[': closer = QChar(']'); break;
        case '{': closer = QChar('}'); break;
        case '"': closer = QChar('"'); break;
        case '\'': closer = QChar('\''); break;
        case '`': closer = QChar('`'); break;
        // Non-bracket char (including identifier chars): fall through to
        // the autocomplete auto-trigger check before exiting.
        default:  maybeTriggerAutocomplete(); return;
    }

    // VSCode-style apostrophe heuristic: when the previous char is a
    // letter or digit, treat `'` as an apostrophe in prose (don't,
    // it's, l'amour) rather than the opener of a quoted string. Skip
    // the auto-pair. Doesn't fire for `"` because closing a paragraph
    // mid-word is far more common than typing the same letter twice
    // around quotes.
    if (ch == '\'' && pos > 0) {
        const char prevCh = static_cast<char>(
            m_editor->send(static_cast<unsigned int>(Message::GetCharAt),
                           static_cast<Scintilla::uptr_t>(pos - 1)));
        const QChar qp(prevCh);
        if (qp.isLetterOrNumber()) return;
    }

    // Don't auto-pair if the very next char is already the same closer
    // (e.g. typing inside an already-paired pair would double-up).
    const Scintilla::sptr_t length =
        m_editor->send(static_cast<unsigned int>(Message::GetLength));
    if (pos < length) {
        const char nextCh = static_cast<char>(
            m_editor->send(static_cast<unsigned int>(Message::GetCharAt),
                           static_cast<Scintilla::uptr_t>(pos)));
        if (QChar(nextCh) == closer) return;
    }

    // Insert the closer at caret. Caret stays put (Scintilla's InsertText
    // doesn't move it when pos param is the current caret position).
    const QByteArray closerBytes = QString(closer).toUtf8();
    if (suppressRec)
        m_editor->send(static_cast<unsigned int>(Message::StopRecord), 0, 0);
    m_editor->sends(static_cast<unsigned int>(Message::InsertText),
                    static_cast<Scintilla::uptr_t>(pos),
                    closerBytes.constData());
    if (suppressRec)
        m_editor->send(static_cast<unsigned int>(Message::StartRecord), 0, 0);
    m_pendingPairCloser = closer;
    m_pendingPairPos = static_cast<int>(pos);
}

// Phase 5X — smart-highlight on SCN_UPDATEUI.
//
// We trigger only on Update::Selection bits — typing produces
// Update::Content too which we ignore here (would be a thrash). The
// indicator slot is #9.
void Buffer::onUpdateUi(Scintilla::Update updated)
{
    using Scintilla::Update;
    constexpr int kSmartIndic = 9;

    // Bitwise check — Update is a bitmask of changes since last UI tick.
    const int bits = static_cast<int>(updated);
    if ((bits & static_cast<int>(Update::Selection)) == 0) return;

    if (!Config::smartHighlightEnabled()) {
        // Make sure any leftover indicator from a previously-on session
        // doesn't linger.
        if (!m_smartHighlightWord.isEmpty()) {
            const Scintilla::sptr_t end =
                m_editor->send(static_cast<unsigned int>(Message::GetLength));
            m_editor->send(static_cast<unsigned int>(Message::SetIndicatorCurrent),
                           kSmartIndic, 0);
            m_editor->send(static_cast<unsigned int>(Message::IndicatorClearRange),
                           0, end);
            m_editor->send(static_cast<unsigned int>(Message::MarkerDeleteAll),
                           kSmartHighlightMarker, 0);
            m_smartHighlightWord.clear();
        }
        return;
    }

    const Scintilla::sptr_t selStart =
        m_editor->send(static_cast<unsigned int>(Message::GetSelectionStart));
    const Scintilla::sptr_t selEnd =
        m_editor->send(static_cast<unsigned int>(Message::GetSelectionEnd));

    auto clearIndicator = [&]{
        const Scintilla::sptr_t end =
            m_editor->send(static_cast<unsigned int>(Message::GetLength));
        m_editor->send(static_cast<unsigned int>(Message::SetIndicatorCurrent),
                       kSmartIndic, 0);
        m_editor->send(static_cast<unsigned int>(Message::IndicatorClearRange),
                       0, end);
        // Phase 9m.2 — drop the gutter glyphs in lock-step.
        m_editor->send(static_cast<unsigned int>(Message::MarkerDeleteAll),
                       kSmartHighlightMarker, 0);
        m_smartHighlightWord.clear();
    };

    if (selEnd <= selStart) { clearIndicator(); return; }
    const Scintilla::sptr_t bytes = selEnd - selStart;
    if (bytes < 2 || bytes > 200) { clearIndicator(); return; }

    // Pull the selection text. Reject multi-line / non-identifier
    // selections so we don't highlight every space.
    std::vector<char> buf(static_cast<std::size_t>(bytes) + 1, '\0');
    m_editor->send(static_cast<unsigned int>(Message::GetSelText), 0,
                   reinterpret_cast<Scintilla::sptr_t>(buf.data()));
    const QString sel = QString::fromUtf8(buf.data(), static_cast<int>(bytes));
    auto isIdentChar = [](QChar c) {
        return c.isLetterOrNumber() || c == QChar('_');
    };
    bool allIdent = true;
    for (QChar c : sel) {
        if (!isIdentChar(c)) { allIdent = false; break; }
    }
    if (!allIdent) { clearIndicator(); return; }

    if (sel == m_smartHighlightWord) return;   // already highlighted

    // Refresh: clear, then walk the document with SearchInTarget.
    const Scintilla::sptr_t docEnd =
        m_editor->send(static_cast<unsigned int>(Message::GetLength));
    m_editor->send(static_cast<unsigned int>(Message::SetIndicatorCurrent),
                   kSmartIndic, 0);
    m_editor->send(static_cast<unsigned int>(Message::IndicatorClearRange),
                   0, docEnd);
    // Phase 9m.2 — clear stale gutter glyphs from the previous selection.
    m_editor->send(static_cast<unsigned int>(Message::MarkerDeleteAll),
                   kSmartHighlightMarker, 0);

    const QByteArray needle = sel.toUtf8();
    // Phase 9c.2 — match-case + whole-word are now Preferences-driven
    // (default both ON, matching pre-9c behaviour). The user can relax
    // either to highlight `Foo` from `foo` (match-case off) or partial
    // matches like `foo` inside `foobar` (whole-word off).
    int searchFlags = 0;
    if (Config::smartHighlightMatchCase()) searchFlags |= SCFIND_MATCHCASE;
    if (Config::smartHighlightWholeWord()) searchFlags |= SCFIND_WHOLEWORD;
    m_editor->send(static_cast<unsigned int>(Message::SetSearchFlags),
                   searchFlags, 0);

    Scintilla::sptr_t cursor = 0;
    int count = 0;
    while (cursor < docEnd && count < 1000) {
        m_editor->send(static_cast<unsigned int>(Message::SetTargetRange),
                       static_cast<Scintilla::uptr_t>(cursor),
                       static_cast<Scintilla::sptr_t>(docEnd));
        const Scintilla::sptr_t found = m_editor->sends(
            static_cast<unsigned int>(Message::SearchInTarget),
            static_cast<Scintilla::uptr_t>(needle.size()),
            needle.constData());
        if (found < 0) break;
        const Scintilla::sptr_t targetEnd = m_editor->send(
            static_cast<unsigned int>(Message::GetTargetEnd));
        // Skip the user's current selection itself.
        if (!(found == selStart && targetEnd == selEnd)) {
            m_editor->send(static_cast<unsigned int>(Message::IndicatorFillRange),
                           static_cast<Scintilla::uptr_t>(found),
                           targetEnd - found);
            // Phase 9m.2 — drop a small-rect gutter glyph on the
            // matching line. MarkerAdd doesn't dedupe, so check
            // MarkerGet first; multiple matches on the same line
            // collapse to one glyph that way.
            const Scintilla::sptr_t line = m_editor->send(
                static_cast<unsigned int>(Message::LineFromPosition),
                static_cast<Scintilla::uptr_t>(found), 0);
            const Scintilla::sptr_t lineMarkers = m_editor->send(
                static_cast<unsigned int>(Message::MarkerGet),
                static_cast<Scintilla::uptr_t>(line), 0);
            if ((lineMarkers & (1 << kSmartHighlightMarker)) == 0) {
                m_editor->send(static_cast<unsigned int>(Message::MarkerAdd),
                               static_cast<Scintilla::uptr_t>(line),
                               kSmartHighlightMarker);
            }
            ++count;
        }
        cursor = targetEnd;
        if (cursor == found) ++cursor;   // empty match guard
    }
    m_smartHighlightWord = sel;
}
