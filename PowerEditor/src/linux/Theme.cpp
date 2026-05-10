#include "Theme.h"

#include <QByteArray>
#include <QColor>
#include <QFile>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "Config.h"        // Phase 5P — read user style overrides
#include "Languages.h"     // Phase 8a — display-name lookup for languagesWithStyles
#include "Scintilla.h"     // STYLE_DEFAULT, STYLE_LINENUMBER, STYLE_INDENTGUIDE...
#include "ScintillaEditBase.h"
#include "ScintillaMessages.h"

#include "pugixml.hpp"

using Scintilla::Message;
using Scintilla::sptr_t;
using Scintilla::uptr_t;

namespace {

// Scintilla packs colours as red | (green << 8) | (blue << 16) (Geometry.h).
// The bundled theme XMLs use CSS-style hex (#RRGGBB without the hash),
// so swap R↔B.
constexpr int sciColor(unsigned int rgb)
{
    return static_cast<int>(((rgb & 0xFF0000u) >> 16)
                          |  (rgb & 0x00FF00u)
                          | ((rgb & 0x0000FFu) << 16));
}

bool parseHexColor(const char* s, int* outSciColor)
{
    if (!s) return false;
    std::size_t len = std::strlen(s);
    if (len != 6) return false;
    unsigned int v = 0;
    for (std::size_t i = 0; i < 6; ++i) {
        char c = s[i];
        unsigned int d;
        if      (c >= '0' && c <= '9') d = static_cast<unsigned int>(c - '0');
        else if (c >= 'a' && c <= 'f') d = static_cast<unsigned int>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = static_cast<unsigned int>(c - 'A' + 10);
        else return false;
        v = (v << 4) | d;
    }
    *outSciColor = sciColor(v);
    return true;
}

// One <WordsStyle> entry. fg/bg/fontSize are -1 / "" / 0 when unset.
// Phase 8a — `name` carries the upstream stylers.xml name="..." attribute
// ("COMMENT", "KEYWORD", "NUMBER", ...) so the Style Configurator can
// label each row.
struct WordsStyle {
    int  styleID  = -1;
    int  fg       = -1;
    int  bg       = -1;
    bool bold     = false;
    bool italic   = false;
    bool underline= false;
    int  fontSize = 0;
    std::string fontName;
    std::string name;       // upstream `name=` attribute (Phase 8a)
};

struct ThemeData {
    QString name;
    bool isDark = false;
    // perLanguage[ "cpp" ] = vector of styles for the cpp <LexerType>
    std::unordered_map<std::string, std::vector<WordsStyle>> perLanguage;
    // globals[ "Default Style" ] = WidgetStyle for STYLE_DEFAULT, etc.
    std::unordered_map<std::string, WordsStyle> globals;
};

// All themes loaded once at init. Index 0 = Default (Light), 1 = Default
// (Dark), then 22 bundled themes alphabetically (Phase 7f).
std::vector<ThemeData>  g_themes;
const ThemeData*        g_current   = nullptr;
bool                    g_initialised = false;

void parseStyleAttrs(pugi::xml_node node, WordsStyle& out)
{
    out.styleID = node.attribute("styleID").as_int(-1);
    parseHexColor(node.attribute("fgColor").value(), &out.fg);
    parseHexColor(node.attribute("bgColor").value(), &out.bg);

    const int fs = node.attribute("fontStyle").as_int(0);
    out.bold      = (fs & 1) != 0;
    out.italic    = (fs & 2) != 0;
    out.underline = (fs & 4) != 0;

    const char* sz = node.attribute("fontSize").value();
    out.fontSize = (sz && *sz) ? std::atoi(sz) : 0;

    const char* fn = node.attribute("fontName").value();
    if (fn && *fn) out.fontName = fn;

    // Phase 8a — preserve the upstream name= attribute so the Style
    // Configurator can label each style row.
    const char* nm = node.attribute("name").value();
    if (nm && *nm) out.name = nm;
}

bool parseTheme(const char* qrcPath, const QString& displayName, bool isDark,
                ThemeData* out)
{
    QFile f(QString::fromLatin1(qrcPath));
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray data = f.readAll();
    f.close();

    pugi::xml_document doc;
    const pugi::xml_parse_result r = doc.load_buffer(data.constData(),
        static_cast<size_t>(data.size()));
    if (!r) return false;

    out->name   = displayName;
    out->isDark = isDark;

    auto root = doc.child("NotepadPlus");

    // <LexerStyles><LexerType name="X">…<WordsStyle …/></LexerType></LexerStyles>
    for (pugi::xml_node lex : root.child("LexerStyles").children("LexerType")) {
        const char* lname = lex.attribute("name").value();
        if (!lname || !*lname) continue;
        std::vector<WordsStyle> styles;
        styles.reserve(16);
        for (pugi::xml_node ws : lex.children("WordsStyle")) {
            WordsStyle s;
            parseStyleAttrs(ws, s);
            if (s.styleID >= 0) styles.push_back(std::move(s));
        }
        out->perLanguage.emplace(std::string(lname), std::move(styles));
    }

    // <GlobalStyles><WidgetStyle name="…" styleID="…" …/></GlobalStyles>
    // The "name" attribute is the lookup key (e.g. "Default Style",
    // "Caret colour"). styleID matters for some entries (32, 33, 34, 35, 37
    // are real Scintilla style slots) but is informational for others.
    for (pugi::xml_node ws : root.child("GlobalStyles").children("WidgetStyle")) {
        const char* nm = ws.attribute("name").value();
        if (!nm || !*nm) continue;
        WordsStyle s;
        parseStyleAttrs(ws, s);
        out->globals.emplace(std::string(nm), std::move(s));
    }
    return true;
}

inline sptr_t sciSend(ScintillaEditBase* ed, Message m, uptr_t w = 0, sptr_t l = 0)
{
    return ed->send(static_cast<unsigned int>(m), w, l);
}

void applyOneStyle(ScintillaEditBase* ed, int styleID, const WordsStyle& s)
{
    if (s.fg != -1)
        sciSend(ed, Message::StyleSetFore, static_cast<uptr_t>(styleID), s.fg);
    if (s.bg != -1)
        sciSend(ed, Message::StyleSetBack, static_cast<uptr_t>(styleID), s.bg);
    sciSend(ed, Message::StyleSetBold,      static_cast<uptr_t>(styleID), s.bold      ? 1 : 0);
    sciSend(ed, Message::StyleSetItalic,    static_cast<uptr_t>(styleID), s.italic    ? 1 : 0);
    sciSend(ed, Message::StyleSetUnderline, static_cast<uptr_t>(styleID), s.underline ? 1 : 0);
    if (s.fontSize > 0)
        sciSend(ed, Message::StyleSetSize, static_cast<uptr_t>(styleID), s.fontSize);
    if (!s.fontName.empty())
        ed->sends(static_cast<unsigned int>(Message::StyleSetFont),
                  static_cast<uptr_t>(styleID), s.fontName.c_str());
}

const WordsStyle* findGlobal(const ThemeData& t, const char* name)
{
    auto it = t.globals.find(name);
    if (it == t.globals.end()) return nullptr;
    return &it->second;
}

void applyBaseStylesInternal(ScintillaEditBase* editor, const ThemeData& t)
{
    // 1. STYLE_DEFAULT — fg/bg + font (use globals "Default Style" if present,
    //    else fall back to a sensible monospace 11).
    {
        WordsStyle base;
        base.fg = sciColor(t.isDark ? 0xDCDCCCu : 0x000000u);
        base.bg = sciColor(t.isDark ? 0x3F3F3Fu : 0xFFFFFFu);
        base.fontSize = 11;
        base.fontName = "Monospace";
        if (const WordsStyle* g = findGlobal(t, "Default Style")) {
            // Prefer XML values where set.
            if (g->fg != -1) base.fg = g->fg;
            if (g->bg != -1) base.bg = g->bg;
            if (g->fontSize > 0) base.fontSize = g->fontSize;
            // Don't honour upstream fontName — it's typically "Courier New" /
            // "Consolas", which may not be installed on Linux. Stick with
            // "Monospace" so Qt picks the system mono font. This also avoids
            // the otherwise-undefined behaviour of an XML-less fontName="".
        }
        applyOneStyle(editor, STYLE_DEFAULT, base);
    }

    // 2. Propagate STYLE_DEFAULT to every style slot — gives every unstyled
    //    style ID the right background/font. After this, per-style colours
    //    can override what they care about.
    sciSend(editor, Message::StyleClearAll);

    // 3. Line-number margin (STYLE_LINENUMBER, slot 33).
    if (const WordsStyle* g = findGlobal(t, "Line number margin")) {
        applyOneStyle(editor, STYLE_LINENUMBER, *g);
    } else {
        WordsStyle ln;
        ln.fg = sciColor(t.isDark ? 0x808080u : 0x808080u);
        ln.bg = sciColor(t.isDark ? 0x303030u : 0xF0F0F0u);
        applyOneStyle(editor, STYLE_LINENUMBER, ln);
    }

    // 4. Caret + selection (these are SCI_SETCARETFORE / SCI_SETSELBACK,
    //    not StyleSet*).
    if (const WordsStyle* g = findGlobal(t, "Caret colour")) {
        if (g->fg != -1) sciSend(editor, Message::SetCaretFore,
                                 static_cast<uptr_t>(g->fg));
    }
    if (const WordsStyle* g = findGlobal(t, "Selected text colour")) {
        if (g->bg != -1) sciSend(editor, Message::SetSelBack, 1, g->bg);
        if (g->fg != -1) sciSend(editor, Message::SetSelFore, 1, g->fg);
    }
    if (const WordsStyle* g = findGlobal(t, "Current line background colour")) {
        if (g->bg != -1) {
            sciSend(editor, Message::SetCaretLineVisible, 1);
            sciSend(editor, Message::SetCaretLineBack,
                    static_cast<uptr_t>(g->bg));
        }
    }

    // 5. Indent guide style (slot 37 = STYLE_INDENTGUIDE).
    if (const WordsStyle* g = findGlobal(t, "Indent guideline style")) {
        applyOneStyle(editor, STYLE_INDENTGUIDE, *g);
    }
    // 6. Brace highlight (slot 34 = STYLE_BRACELIGHT) and bad brace (35).
    if (const WordsStyle* g = findGlobal(t, "Brace highlight style")) {
        applyOneStyle(editor, STYLE_BRACELIGHT, *g);
    }
    if (const WordsStyle* g = findGlobal(t, "Bad brace colour")) {
        applyOneStyle(editor, STYLE_BRACEBAD, *g);
    }
}

// Phase 5P — apply user-saved colour overrides on top of the base theme.
// Called from Theme::applyEditorBaseStyles after the theme XML pass.
void applyUserOverrides(ScintillaEditBase* editor)
{
    auto setIf = [&](const QString& key, auto sciCall) {
        const QString hex = Config::styleOverride(key);
        if (hex.isEmpty()) return;
        const QColor c(hex);
        if (!c.isValid()) return;
        const int packed = (c.red() & 0xFF)
                         | ((c.green() & 0xFF) << 8)
                         | ((c.blue() & 0xFF) << 16);
        sciCall(packed);
    };

    setIf(QStringLiteral("default.fg"), [&](int p) {
        sciSend(editor, Message::StyleSetFore, STYLE_DEFAULT, p);
    });
    setIf(QStringLiteral("default.bg"), [&](int p) {
        sciSend(editor, Message::StyleSetBack, STYLE_DEFAULT, p);
    });
    setIf(QStringLiteral("linenumber.fg"), [&](int p) {
        sciSend(editor, Message::StyleSetFore, STYLE_LINENUMBER, p);
    });
    setIf(QStringLiteral("linenumber.bg"), [&](int p) {
        sciSend(editor, Message::StyleSetBack, STYLE_LINENUMBER, p);
    });
    setIf(QStringLiteral("caret.fg"), [&](int p) {
        sciSend(editor, Message::SetCaretFore, p, 0);
    });
    setIf(QStringLiteral("selection.bg"), [&](int p) {
        sciSend(editor, Message::SetSelBack, 1, p);
    });
}

} // namespace

namespace Theme {

namespace {

// Bundled theme catalogue. (Path-in-resource, display name, isDark
// flag.) User themes are discovered from
// ~/.config/padnote/padnote--/themes/.
struct BundledTheme { const char* path; const char* name; bool dark; };

const BundledTheme kBundledThemes[] = {
    {":/stylers.model.xml",          "Default (Light)",     false},
    {":/DarkModeDefault.xml",        "Default (Dark)",      true},
    {":/themes/Bespin.xml",          "Bespin",              true},
    {":/themes/Black board.xml",     "Black board",         true},
    {":/themes/Choco.xml",           "Choco",               true},
    {":/themes/DansLeRuSH-Dark.xml", "DansLeRuSH-Dark",     true},
    {":/themes/Deep Black.xml",      "Deep Black",          true},
    {":/themes/Hello Kitty.xml",     "Hello Kitty",         false},
    {":/themes/HotFudgeSundae.xml",  "HotFudgeSundae",      true},
    {":/themes/khaki.xml",           "khaki",               false},
    {":/themes/Mono Industrial.xml", "Mono Industrial",     true},
    {":/themes/Monokai.xml",         "Monokai",             true},
    {":/themes/MossyLawn.xml",       "MossyLawn",           false},
    {":/themes/Navajo.xml",          "Navajo",              false},
    {":/themes/Obsidian.xml",        "Obsidian",            true},
    {":/themes/Plastic Code Wrap.xml","Plastic Code Wrap",  true},
    {":/themes/Ruby Blue.xml",       "Ruby Blue",           true},
    {":/themes/Solarized-light.xml", "Solarized Light",     false},
    {":/themes/Solarized.xml",       "Solarized",           true},
    {":/themes/Twilight.xml",        "Twilight",            true},
    {":/themes/Vibrant Ink.xml",     "Vibrant Ink",         true},
    {":/themes/vim Dark Blue.xml",   "vim Dark Blue",       true},
    {":/themes/Zenburn.xml",         "Zenburn",             true},
};

} // namespace

void init()
{
    if (g_initialised) return;
    g_themes.reserve(sizeof(kBundledThemes) / sizeof(kBundledThemes[0]));
    for (const auto& t : kBundledThemes) {
        ThemeData td;
        if (parseTheme(t.path, QString::fromUtf8(t.name), t.dark, &td)) {
            g_themes.push_back(std::move(td));
        }
    }
    if (!g_themes.empty()) g_current = &g_themes.front();
    g_initialised = true;
}

Mode mode()
{
    init();
    return (g_current && g_current->isDark) ? Mode::Dark : Mode::Light;
}

void setMode(Mode m)
{
    init();
    // Find the FIRST theme matching the requested mode. Existing
    // setMode-based callers (config restore, View → Theme menu's
    // Light/Dark entries) keep their two-state semantics this way.
    const bool wantDark = (m == Mode::Dark);
    for (const auto& t : g_themes) {
        if (t.isDark == wantDark) { g_current = &t; return; }
    }
}

QStringList availableThemes()
{
    init();
    QStringList out;
    out.reserve(static_cast<int>(g_themes.size()));
    for (const auto& t : g_themes) out.append(t.name);
    return out;
}

QString currentName()
{
    init();
    return g_current ? g_current->name : QString{};
}

bool setThemeByName(const QString& name)
{
    init();
    for (const auto& t : g_themes) {
        if (t.name == name) { g_current = &t; return true; }
    }
    return false;
}

int bookmarkMarkerFore()
{
    init();
    if (!g_current) return 0x6060FF;   // default red — Scintilla BGR
    const WordsStyle* g = findGlobal(*g_current, "Bookmark margin");
    if (g && g->fg >= 0) return g->fg;
    // Some themes use "Mark Style 1" instead.
    g = findGlobal(*g_current, "Mark Style 1");
    if (g && g->fg >= 0) return g->fg;
    return 0x6060FF;
}

int bookmarkMarkerBack()
{
    init();
    if (!g_current) return 0x6060FF;
    const WordsStyle* g = findGlobal(*g_current, "Bookmark margin");
    if (g && g->bg >= 0) return g->bg;
    g = findGlobal(*g_current, "Mark Style 1");
    if (g && g->bg >= 0) return g->bg;
    return 0x6060FF;
}

// Phase 9e — fold marker fore/back. Theme key "Fold margin" carries
// both colours in upstream stylers.xml; some themes split it into
// "Fold" + "Fold active". Falls back to grey-on-line-number-bg —
// readable in both light and dark themes without screaming.
int foldMarkerFore()
{
    init();
    if (!g_current) return 0x808080;
    const WordsStyle* g = findGlobal(*g_current, "Fold margin");
    if (g && g->fg >= 0) return g->fg;
    g = findGlobal(*g_current, "Fold");
    if (g && g->fg >= 0) return g->fg;
    return g_current->isDark ? 0xC0C0C0 : 0x808080;   // grey
}

int foldMarkerBack()
{
    init();
    if (!g_current) return 0xF0F0F0;
    const WordsStyle* g = findGlobal(*g_current, "Fold margin");
    if (g && g->bg >= 0) return g->bg;
    g = findGlobal(*g_current, "Fold");
    if (g && g->bg >= 0) return g->bg;
    // Default: match Theme::applyEditorBaseStyles' line-number margin
    // background so the fold margin blends in with the gutter.
    return g_current->isDark ? 0x303030 : 0xF0F0F0;
}

// Phase 9m.1 — Document Map viewport indicator. The "you are here" box
// on the map should visually match the editor's caret-line affordance,
// since they answer the same question. Upstream's Win32 NPP uses a
// hardcoded orange (FF8000); we derive from the theme so dark themes
// get a subdued indicator instead of glowing orange.
int documentMapIndicatorFore()
{
    init();
    if (!g_current) return 0x808080;     // neutral grey
    const WordsStyle* g = findGlobal(*g_current, "Current line background colour");
    if (g && g->bg >= 0) return g->bg;
    // Fall back to selection bg — also a "this is what's relevant" colour.
    g = findGlobal(*g_current, "Selected text colour");
    if (g && g->bg >= 0) return g->bg;
    return g_current->isDark ? 0x606060 : 0x808080;
}

// Phase 9m.2 — Smart-highlight colour. Stylers.xml ships
// `<WidgetStyle name="Smart Highlighting" .../>` in both light and
// dark themes (00FF00 / 358A35); fall back to BGR yellow if missing.
int smartHighlightFore()
{
    init();
    if (!g_current) return 0x00FFFF;     // BGR yellow (legacy default)
    const WordsStyle* g = findGlobal(*g_current, "Smart Highlighting");
    if (g && g->bg >= 0) return g->bg;
    if (g && g->fg >= 0) return g->fg;
    return 0x00FFFF;
}

// Phase 5MK — Mark Style 1..5. Stylers.xml ships
// `<WidgetStyle name="Mark Style N" .../>` for N=1..5 with distinct
// background colours (cyan / orange / yellow / purple / green per
// stylers.model.xml). Themes that don't define them get the bgcolor
// from the model defaults via the embedded stylers.model.xml.
int markStyleFore(int n)
{
    static constexpr int kFallback[5] = {
        0xFFFF00,   // BGR cyan      (stylers.model.xml: 00FFFF)
        0x0080FF,   // BGR orange    (FF8000)
        0x00FFFF,   // BGR yellow    (FFFF00)
        0xFF0080,   // BGR purple    (8000FF)
        0x008000,   // BGR green     (008000)
    };
    if (n < 1 || n > 5) return kFallback[0];
    init();
    if (!g_current) return kFallback[n - 1];
    char name[16];
    std::snprintf(name, sizeof(name), "Mark Style %d", n);
    const WordsStyle* g = findGlobal(*g_current, name);
    if (g && g->bg >= 0) return g->bg;
    if (g && g->fg >= 0) return g->fg;
    return kFallback[n - 1];
}

void applyEditorBaseStyles(ScintillaEditBase* editor)
{
    if (!editor) return;
    init();
    applyBaseStylesInternal(editor, *g_current);
    applyUserOverrides(editor);
}

void clearStyles(ScintillaEditBase* editor)
{
    if (!editor) return;
    sciSend(editor, Message::StyleClearAll);
    applyEditorBaseStyles(editor);
}

// Phase 8a — apply user per-language overrides on top of the theme's
// per-language `<WordsStyle>` entries. Walks the flat StyleOverride hash
// for keys matching `lang:<internalName>:<styleID>:<attr>`, decodes them,
// and pushes via SCI_STYLESETFORE / SCI_STYLESETBACK / SCI_STYLESETBOLD
// / SCI_STYLESETITALIC / SCI_STYLESETUNDERLINE. Runs after the XML pass
// in applyForLanguage so user values win.
void applyPerLanguageUserOverrides(ScintillaEditBase* editor,
                                   const char* internalName)
{
    if (!editor || !internalName || !*internalName) return;

    const QString prefix = QStringLiteral("lang:")
        + QString::fromUtf8(internalName)
        + QStringLiteral(":");
    const QStringList keys = Config::styleOverrideKeys();

    // Group entries by styleID so we can apply the full attr set cheaply
    // even if the order in the hash is unstable. Phase 8a-polish adds
    // fontName as a per-style override.
    struct Pending {
        int       fg = -1, bg = -1;
        int       fontStyle = -1;     // -1 = unset; 0..7 = bitmask
        int       fontSize  = 0;      // 0 = leave alone
        QByteArray fontName;          // empty = leave alone (own the bytes)
    };
    std::unordered_map<int, Pending> bySlot;

    for (const QString& key : keys) {
        if (!key.startsWith(prefix)) continue;
        // tail = "<styleID>:<attr>"
        const QString tail = key.mid(prefix.size());
        const int colon = tail.indexOf(QChar(':'));
        if (colon <= 0) continue;
        bool ok = false;
        const int styleID = tail.left(colon).toInt(&ok);
        if (!ok) continue;
        const QString attr = tail.mid(colon + 1);
        const QString val  = Config::styleOverride(key);
        Pending& p = bySlot[styleID];
        if (attr == QLatin1String("fg")) {
            const QColor c(val);
            if (c.isValid()) p.fg = (c.red() & 0xFF)
                                  | ((c.green() & 0xFF) << 8)
                                  | ((c.blue()  & 0xFF) << 16);
        } else if (attr == QLatin1String("bg")) {
            const QColor c(val);
            if (c.isValid()) p.bg = (c.red() & 0xFF)
                                  | ((c.green() & 0xFF) << 8)
                                  | ((c.blue()  & 0xFF) << 16);
        } else if (attr == QLatin1String("fontStyle")) {
            p.fontStyle = val.toInt(&ok);
            if (!ok) p.fontStyle = -1;
        } else if (attr == QLatin1String("fontSize")) {
            p.fontSize = val.toInt(&ok);
            if (!ok) p.fontSize = 0;
        } else if (attr == QLatin1String("fontName")) {
            if (!val.isEmpty()) p.fontName = val.toUtf8();
        }
    }

    for (const auto& [styleID, p] : bySlot) {
        if (p.fg != -1)
            sciSend(editor, Message::StyleSetFore, static_cast<uptr_t>(styleID), p.fg);
        if (p.bg != -1)
            sciSend(editor, Message::StyleSetBack, static_cast<uptr_t>(styleID), p.bg);
        if (p.fontStyle >= 0) {
            sciSend(editor, Message::StyleSetBold,      static_cast<uptr_t>(styleID),
                    (p.fontStyle & 1) ? 1 : 0);
            sciSend(editor, Message::StyleSetItalic,    static_cast<uptr_t>(styleID),
                    (p.fontStyle & 2) ? 1 : 0);
            sciSend(editor, Message::StyleSetUnderline, static_cast<uptr_t>(styleID),
                    (p.fontStyle & 4) ? 1 : 0);
        }
        if (p.fontSize > 0)
            sciSend(editor, Message::StyleSetSize, static_cast<uptr_t>(styleID), p.fontSize);
        if (!p.fontName.isEmpty())
            editor->send(static_cast<unsigned int>(Message::StyleSetFont),
                         static_cast<uptr_t>(styleID),
                         reinterpret_cast<sptr_t>(p.fontName.constData()));
    }
}

void applyForLanguage(ScintillaEditBase* editor, const char* internalName)
{
    if (!editor) return;
    init();

    // Always re-apply base + cleared per-style table first so the previous
    // language's colours don't leak through.
    applyBaseStylesInternal(editor, *g_current);

    if (!internalName || !*internalName) return;

    auto it = g_current->perLanguage.find(internalName);
    if (it != g_current->perLanguage.end()) {
        for (const WordsStyle& s : it->second) {
            applyOneStyle(editor, s.styleID, s);
        }
    }

    // Phase 8a — per-language user overrides win over the theme defaults.
    applyPerLanguageUserOverrides(editor, internalName);
}

QVector<LanguageStyleInfo> stylesForLanguage(const char* internalName)
{
    QVector<LanguageStyleInfo> result;
    init();
    if (!g_current || !internalName || !*internalName) return result;

    auto it = g_current->perLanguage.find(internalName);
    if (it == g_current->perLanguage.end()) return result;

    // Pre-compute the override map per styleID for the requested language —
    // mirrors applyPerLanguageUserOverrides so the dialog opens with what
    // the editor would actually render.
    const QString prefix = QStringLiteral("lang:")
        + QString::fromUtf8(internalName)
        + QStringLiteral(":");
    const QStringList keys = Config::styleOverrideKeys();

    auto resolveOverride = [&](int styleID, const char* attr,
                               int defaultVal) -> int {
        const QString key = prefix
            + QString::number(styleID) + QChar(':')
            + QString::fromUtf8(attr);
        const QString hex = Config::styleOverride(key);
        if (hex.isEmpty()) return defaultVal;
        if (std::strcmp(attr, "fg") == 0 || std::strcmp(attr, "bg") == 0) {
            const QColor c(hex);
            if (!c.isValid()) return defaultVal;
            return (c.red() & 0xFF)
                 | ((c.green() & 0xFF) << 8)
                 | ((c.blue()  & 0xFF) << 16);
        }
        bool ok = false;
        const int v = hex.toInt(&ok);
        return ok ? v : defaultVal;
    };
    (void)keys;   // touched only via Config::styleOverride lookups

    // String-valued attributes (fontName) take a different resolver: the
    // theme value is a std::string; a non-empty Config override wins.
    auto resolveStringOverride = [&prefix](int styleID, const char* attr,
                                           const QString& defaultVal) -> QString {
        const QString key = QStringLiteral("%1%2:%3").arg(prefix)
                                                     .arg(styleID)
                                                     .arg(QLatin1String(attr));
        const QString val = Config::styleOverride(key);
        return val.isEmpty() ? defaultVal : val;
    };

    for (const WordsStyle& s : it->second) {
        LanguageStyleInfo info;
        info.styleID   = s.styleID;
        info.name      = QString::fromUtf8(s.name.c_str());
        info.fg        = resolveOverride(s.styleID, "fg", s.fg);
        info.bg        = resolveOverride(s.styleID, "bg", s.bg);
        info.fontStyle = resolveOverride(s.styleID, "fontStyle",
                                         (s.bold      ? 1 : 0)
                                       | (s.italic    ? 2 : 0)
                                       | (s.underline ? 4 : 0));
        info.fontSize  = resolveOverride(s.styleID, "fontSize", s.fontSize);
        info.fontName  = resolveStringOverride(s.styleID, "fontName",
                                               QString::fromUtf8(s.fontName.c_str()));
        result.push_back(std::move(info));
    }
    return result;
}

QVector<LanguageWithStyles> languagesWithStyles()
{
    QVector<LanguageWithStyles> result;
    init();
    if (!g_current) return result;

    result.reserve(static_cast<int>(g_current->perLanguage.size()));
    for (const auto& [internalName, styles] : g_current->perLanguage) {
        if (styles.empty()) continue;
        LanguageWithStyles row;
        row.internalName = QString::fromUtf8(internalName.c_str());

        // Prefer the LanguageDef's displayName so the list reads like the
        // Language menu ("C++", not "cpp"). Fall back to the internal
        // name if the language isn't in our registry (e.g. theme-only
        // entries like "globaloverride"; these still show up because
        // someone might want to edit them via the dialog).
        if (auto* L = Languages::findByInternalName(internalName.c_str())) {
            row.displayName = L->displayName;
        } else {
            row.displayName = row.internalName;
        }
        result.push_back(std::move(row));
    }

    std::sort(result.begin(), result.end(),
              [](const LanguageWithStyles& a, const LanguageWithStyles& b) {
                  return QString::localeAwareCompare(a.displayName, b.displayName) < 0;
              });
    return result;
}

} // namespace Theme
