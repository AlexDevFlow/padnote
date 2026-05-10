// UserDefineLang.cpp — Phase 5U.2: see UserDefineLang.h for the contract.
//
// Sources of truth for the on-disk schema and the lexer wiring:
//   • PowerEditor/src/Parameters.cpp:8407 — `insertUserLang2Tree` (writer)
//   • PowerEditor/src/Parameters.cpp:3819 — `feedUserLang` (reader)
//   • PowerEditor/src/ScintillaComponent/UserDefineDialog.h:46-239 —
//     keywordIdMapper / keywordNameMapper / setLexerMapper
//   • PowerEditor/src/ScintillaComponent/ScintillaEditView.cpp:1094 —
//     `setUserLexer` (canonical SCI_SETPROPERTY / SCI_SETKEYWORDS sequence)
//   • lexilla/lexers/LexUser.cxx:1254-1290 — the lexer's `userDefine.*`
//     property reads.

#include "UserDefineLang.h"

#include <QByteArray>
#include <QChar>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QString>
#include <QStringList>

#include "Config.h"     // Phase 12 — Config::configFilePath() override

#include <cstring>

#include "Scintilla.h"
#include "ScintillaEditBase.h"
#include "ScintillaMessages.h"
#include "SciLexer.h"     // SCE_USER_KWLIST_*

#include "pugixml.hpp"

using Scintilla::Message;

namespace {

// -----------------------------------------------------------------------------
// `<Keywords name="...">` ↔ SCE_USER_KWLIST_* index mapping.
//
// Mirrors upstream's GlobalMappers::keywordIdMapper (UserDefineDialog.h:78-123)
// + the back-compat aliases (Words1-4, Folder+/-) so older UDLs still load.
//
// Writer emits the canonical "2.0+" name for each id (kIdToName).
// Reader accepts any of the listed aliases (kAliasToId).
// -----------------------------------------------------------------------------

struct KwAlias {
    const char* name;
    int         id;
};

// Reader: alias → id. First-match wins.
constexpr KwAlias kAliasToId[] = {
    // 2.0+ canonical names
    {"Comments",                       SCE_USER_KWLIST_COMMENTS},
    {"Numbers, prefix1",               SCE_USER_KWLIST_NUMBER_PREFIX1},
    {"Numbers, prefix2",               SCE_USER_KWLIST_NUMBER_PREFIX2},
    {"Numbers, extras1",               SCE_USER_KWLIST_NUMBER_EXTRAS1},
    {"Numbers, extras2",               SCE_USER_KWLIST_NUMBER_EXTRAS2},
    {"Numbers, suffix1",               SCE_USER_KWLIST_NUMBER_SUFFIX1},
    {"Numbers, suffix2",               SCE_USER_KWLIST_NUMBER_SUFFIX2},
    {"Numbers, range",                 SCE_USER_KWLIST_NUMBER_RANGE},
    {"Numbers, additional",            SCE_USER_KWLIST_NUMBER_RANGE},          // 2.0 alias
    {"Numbers, prefixes",              SCE_USER_KWLIST_NUMBER_PREFIX2},        // 2.0 alias
    {"Numbers, extras with prefixes",  SCE_USER_KWLIST_NUMBER_EXTRAS2},        // 2.0 alias
    {"Numbers, suffixes",              SCE_USER_KWLIST_NUMBER_SUFFIX2},        // 2.0 alias
    {"Operators1",                     SCE_USER_KWLIST_OPERATORS1},
    {"Operators2",                     SCE_USER_KWLIST_OPERATORS2},
    {"Operators",                      SCE_USER_KWLIST_OPERATORS1},            // pre-2.0 alias
    {"Folders in code1, open",         SCE_USER_KWLIST_FOLDERS_IN_CODE1_OPEN},
    {"Folders in code1, middle",       SCE_USER_KWLIST_FOLDERS_IN_CODE1_MIDDLE},
    {"Folders in code1, close",        SCE_USER_KWLIST_FOLDERS_IN_CODE1_CLOSE},
    {"Folders in code2, open",         SCE_USER_KWLIST_FOLDERS_IN_CODE2_OPEN},
    {"Folders in code2, middle",       SCE_USER_KWLIST_FOLDERS_IN_CODE2_MIDDLE},
    {"Folders in code2, close",        SCE_USER_KWLIST_FOLDERS_IN_CODE2_CLOSE},
    {"Folders in comment, open",       SCE_USER_KWLIST_FOLDERS_IN_COMMENT_OPEN},
    {"Folders in comment, middle",     SCE_USER_KWLIST_FOLDERS_IN_COMMENT_MIDDLE},
    {"Folders in comment, close",      SCE_USER_KWLIST_FOLDERS_IN_COMMENT_CLOSE},
    {"Folder+",                        SCE_USER_KWLIST_FOLDERS_IN_CODE1_OPEN}, // pre-2.0 alias
    {"Folder-",                        SCE_USER_KWLIST_FOLDERS_IN_CODE1_CLOSE},// pre-2.0 alias
    {"Keywords1",                      SCE_USER_KWLIST_KEYWORDS1},
    {"Keywords2",                      SCE_USER_KWLIST_KEYWORDS2},
    {"Keywords3",                      SCE_USER_KWLIST_KEYWORDS3},
    {"Keywords4",                      SCE_USER_KWLIST_KEYWORDS4},
    {"Keywords5",                      SCE_USER_KWLIST_KEYWORDS5},
    {"Keywords6",                      SCE_USER_KWLIST_KEYWORDS6},
    {"Keywords7",                      SCE_USER_KWLIST_KEYWORDS7},
    {"Keywords8",                      SCE_USER_KWLIST_KEYWORDS8},
    {"Words1",                         SCE_USER_KWLIST_KEYWORDS1},  // pre-2.0 alias
    {"Words2",                         SCE_USER_KWLIST_KEYWORDS2},
    {"Words3",                         SCE_USER_KWLIST_KEYWORDS3},
    {"Words4",                         SCE_USER_KWLIST_KEYWORDS4},
    {"Delimiters",                     SCE_USER_KWLIST_DELIMITERS},
};

// Writer: id → canonical 2.0+ name. Indexed by SCE_USER_KWLIST_* (0..27).
constexpr const char* kIdToName[28] = {
    "Comments",                                              // 0
    "Numbers, prefix1", "Numbers, prefix2",                  // 1, 2
    "Numbers, extras1", "Numbers, extras2",                  // 3, 4
    "Numbers, suffix1", "Numbers, suffix2",                  // 5, 6
    "Numbers, range",                                        // 7
    "Operators1", "Operators2",                              // 8, 9
    "Folders in code1, open",  "Folders in code1, middle",
    "Folders in code1, close",                               // 10, 11, 12
    "Folders in code2, open",  "Folders in code2, middle",
    "Folders in code2, close",                               // 13, 14, 15
    "Folders in comment, open", "Folders in comment, middle",
    "Folders in comment, close",                             // 16, 17, 18
    "Keywords1", "Keywords2", "Keywords3", "Keywords4",      // 19..22
    "Keywords5", "Keywords6", "Keywords7", "Keywords8",      // 23..26
    "Delimiters",                                            // 27
};

// `setLexerMapper` (UserDefineDialog.h:227-239) — the keyword lists that
// reach the lexer via SCI_SETPROPERTY (`userDefine.<key>`) instead of
// SCI_SETKEYWORDS. Indexed by SCE_USER_KWLIST_*. nullptr means the list
// goes through SCI_SETKEYWORDS instead.
constexpr const char* kKwlPropertyKey[28] = {
    "userDefine.comments",                                   // 0
    "userDefine.numberPrefix1", "userDefine.numberPrefix2",  // 1, 2
    "userDefine.numberExtras1", "userDefine.numberExtras2",  // 3, 4
    "userDefine.numberSuffix1", "userDefine.numberSuffix2",  // 5, 6
    "userDefine.numberRange",                                // 7
    "userDefine.operators1",                                 // 8
    nullptr,                                                 // 9 Operators2 -> SCI_SETKEYWORDS
    "userDefine.foldersInCode1Open",                         // 10
    "userDefine.foldersInCode1Middle",                       // 11
    "userDefine.foldersInCode1Close",                        // 12
    nullptr, nullptr, nullptr,                               // 13-15 FOLDERS_IN_CODE2 -> SCI_SETKEYWORDS
    nullptr, nullptr, nullptr,                               // 16-18 FOLDERS_IN_COMMENT -> SCI_SETKEYWORDS
    nullptr, nullptr, nullptr, nullptr,                      // 19-22 KEYWORDS1-4 -> SCI_SETKEYWORDS
    nullptr, nullptr, nullptr, nullptr,                      // 23-26 KEYWORDS5-8 -> SCI_SETKEYWORDS
    "userDefine.delimiters",                                 // 27
};

int idForKeywordName(const char* name)
{
    if (!name || !*name) return -1;
    for (const auto& a : kAliasToId) {
        if (std::strcmp(a.name, name) == 0) return a.id;
    }
    return -1;
}

bool xmlBoolAttr(const pugi::xml_node& node, const char* name,
                 bool defaultVal = false)
{
    const char* v = node.attribute(name).value();
    if (!v || !*v) return defaultVal;
    return std::strcmp(v, "yes") == 0
        || std::strcmp(v, "true") == 0
        || std::strcmp(v, "1")    == 0;
}

const char* xmlBoolToStr(bool v) { return v ? "yes" : "no"; }

void parseUserLang(const pugi::xml_node& node, UDL& udl)
{
    udl.name       = QString::fromUtf8(node.attribute("name").value());
    udl.ext        = QString::fromUtf8(node.attribute("ext").value());
    udl.udlVersion = QString::fromUtf8(node.attribute("udlVersion").value());
    if (udl.udlVersion.isEmpty()) udl.udlVersion = QStringLiteral("2.1");
    udl.isDarkModeTheme = xmlBoolAttr(node, "darkModeTheme");

    // Settings/Global
    if (auto settings = node.child("Settings")) {
        if (auto g = settings.child("Global")) {
            udl.isCaseIgnored       = xmlBoolAttr(g, "caseIgnored");
            udl.allowFoldOfComments = xmlBoolAttr(g, "allowFoldOfComments");
            udl.foldCompact         = xmlBoolAttr(g, "foldCompact");
            udl.forcePureLC         = g.attribute("forcePureLC").as_int(0);
            udl.decimalSeparator    = g.attribute("decimalSeparator").as_int(0);
        }
        // Settings/Prefix
        if (auto p = settings.child("Prefix")) {
            for (int i = 0; i < 8; ++i) {
                // Attribute name is "Keywords1".."Keywords8"
                char attrName[12];
                std::snprintf(attrName, sizeof(attrName), "Keywords%d", i + 1);
                udl.isPrefix[i] = xmlBoolAttr(p, attrName);
            }
        }
    }

    // KeywordLists
    if (auto kwl = node.child("KeywordLists")) {
        for (pugi::xml_node kw : kwl.children("Keywords")) {
            const char* name = kw.attribute("name").value();
            const int   id   = idForKeywordName(name);
            if (id < 0 || id >= 28) continue;

            // The textual content is the keyword string (may be empty).
            // Use child_value() so we get the whole text including spaces.
            const char* text = kw.child_value();
            if (!text) text = "";
            udl.keywords[id] = QString::fromUtf8(text);
        }
    }

    // Styles — round-trip preservation only in 5U.2; we don't apply them.
    if (auto sroot = node.child("Styles")) {
        for (pugi::xml_node s : sroot.children("WordsStyle")) {
            UDLStyle st;
            st.name       = QString::fromUtf8(s.attribute("name").value());
            st.fgColor    = QString::fromUtf8(s.attribute("fgColor").value());
            st.bgColor    = QString::fromUtf8(s.attribute("bgColor").value());
            // colorStyle / fontSize: -1 / 0 sentinel for "not present".
            const auto cs = s.attribute("colorStyle");
            st.colorStyle = cs ? cs.as_int(-1) : -1;
            st.fontName   = QString::fromUtf8(s.attribute("fontName").value());
            st.fontStyle  = s.attribute("fontStyle").as_int(0);
            // fontSize is sometimes the empty string ("") — as_int returns 0.
            st.fontSize   = s.attribute("fontSize").as_int(0);
            st.nesting    = s.attribute("nesting").as_int(0);
            udl.styles.push_back(std::move(st));
        }
    }
}

void writeUserLang(pugi::xml_node& parent, const UDL& udl)
{
    pugi::xml_node root = parent.append_child("UserLang");
    root.append_attribute("name")          = udl.name.toUtf8().constData();
    root.append_attribute("ext")           = udl.ext.toUtf8().constData();
    if (udl.isDarkModeTheme)
        root.append_attribute("darkModeTheme") = "yes";
    const QByteArray udlVer = udl.udlVersion.toUtf8();
    root.append_attribute("udlVersion") = udlVer.constData();

    // <Settings>
    {
        pugi::xml_node settings = root.append_child("Settings");
        pugi::xml_node g = settings.append_child("Global");
        g.append_attribute("caseIgnored")         = xmlBoolToStr(udl.isCaseIgnored);
        g.append_attribute("allowFoldOfComments") = xmlBoolToStr(udl.allowFoldOfComments);
        g.append_attribute("foldCompact")         = xmlBoolToStr(udl.foldCompact);
        g.append_attribute("forcePureLC")         = udl.forcePureLC;
        g.append_attribute("decimalSeparator")    = udl.decimalSeparator;

        pugi::xml_node p = settings.append_child("Prefix");
        for (int i = 0; i < 8; ++i) {
            char name[12];
            std::snprintf(name, sizeof(name), "Keywords%d", i + 1);
            p.append_attribute(name) = xmlBoolToStr(udl.isPrefix[i]);
        }
    }

    // <KeywordLists>
    {
        pugi::xml_node kwl = root.append_child("KeywordLists");
        for (int i = 0; i < 28; ++i) {
            pugi::xml_node kw = kwl.append_child("Keywords");
            kw.append_attribute("name") = kIdToName[i];
            const QByteArray text = udl.keywords[i].toUtf8();
            // pugixml doesn't track whether the text was empty; emit the
            // element with empty body when the list is empty (matches
            // upstream's writer which always emits all 28 entries).
            kw.append_child(pugi::node_pcdata).set_value(text.constData());
        }
    }

    // <Styles> — round-trip the data we loaded; if empty (newly-created UDL)
    // emit nothing here. The dialog (5U.3) doesn't yet edit per-UDL styles;
    // that's deferred to 8a's per-language Style Configurator.
    if (!udl.styles.isEmpty()) {
        pugi::xml_node sroot = root.append_child("Styles");
        for (const UDLStyle& st : udl.styles) {
            pugi::xml_node s = sroot.append_child("WordsStyle");
            s.append_attribute("name")    = st.name.toUtf8().constData();
            s.append_attribute("fgColor") = st.fgColor.toUtf8().constData();
            s.append_attribute("bgColor") = st.bgColor.toUtf8().constData();
            if (st.colorStyle >= 0)
                s.append_attribute("colorStyle") = st.colorStyle;
            if (!st.fontName.isEmpty())
                s.append_attribute("fontName") = st.fontName.toUtf8().constData();
            s.append_attribute("fontStyle") = st.fontStyle;
            // fontSize: 0 = default, written as empty string for upstream
            // round-trip parity.
            if (st.fontSize == 0)
                s.append_attribute("fontSize") = "";
            else
                s.append_attribute("fontSize") = st.fontSize;
            s.append_attribute("nesting") = st.nesting;
        }
    }
}

} // namespace

namespace UserDefineLang {

// -----------------------------------------------------------------------------
// Comment / Delimiter prefix-tag encoding (Phase 5U.3-polish).
//
// Upstream packs the comment-marker fields and the 8 delimiter pairs into a
// single space-separated string per keyword list, where each token is preceded
// by a 2-char prefix tag identifying its slot (e.g. "00//" "01\\" "03/*").
// See UserDefineDialog.cpp::convertTo / ::retrieve in the upstream tree.
//
// MVP simplification: upstream also handles `((...))` group syntax for slots
// whose value contains whitespace (rare — used for multi-word comment tokens).
// We don't surface it; if the dialog rewrites such a list, the group tokens
// drop. Power users who need it should keep hand-editing the XML.
namespace {

// Decode `encoded` into `slotCount` slots indexed by 2-char prefix
// `00`..`<slotCount-1>` (zero-padded, base 10). Returns a list of length
// `slotCount`. Tokens whose prefix is out of range or duplicated are silently
// dropped — the dialog editor only ever round-trips the first value per slot.
// Note: locals are named `out` / `parts` instead of `slots` because Qt
// `#define`s `slots` to nothing in qobjectdefs.h, which mangles any local
// or parameter named `slots` even in non-Q_OBJECT translation units.
QStringList decodePrefixedSlots(const QString& encoded, int slotCount)
{
    QStringList out;
    out.reserve(slotCount);
    for (int i = 0; i < slotCount; ++i) out.append(QString());

    const QStringList tokens = encoded.split(QChar(' '), Qt::SkipEmptyParts);
    for (const QString& tok : tokens) {
        if (tok.size() < 2) continue;                     // malformed prefix
        bool ok = false;
        const int idx = tok.left(2).toInt(&ok, 10);
        if (!ok || idx < 0 || idx >= slotCount) continue;
        if (out[idx].isEmpty()) out[idx] = tok.mid(2);
    }
    return out;
}

// Inverse of decodePrefixedSlots. Empty slots are dropped (no token emitted).
QString encodePrefixedSlots(const QStringList& parts)
{
    QStringList out;
    out.reserve(parts.size());
    for (int i = 0; i < parts.size(); ++i) {
        if (parts[i].isEmpty()) continue;
        out.append(QString::asprintf("%02d", i) + parts[i]);
    }
    return out.join(QChar(' '));
}

} // namespace

// 5 prefix slots for `keywords[SCE_USER_KWLIST_COMMENTS]`:
//   0 = line-comment opener, 1 = continuation, 2 = closer,
//   3 = block-comment open, 4 = block-comment close.
QStringList decodeComments(const QString& encoded)
{
    return decodePrefixedSlots(encoded, 5);
}

QString encodeComments(const QStringList& slots5)
{
    QStringList padded = slots5;
    while (padded.size() < 5) padded.append(QString());
    if (padded.size() > 5) padded = padded.mid(0, 5);
    return encodePrefixedSlots(padded);
}

// 24 prefix slots for `keywords[SCE_USER_KWLIST_DELIMITERS]` — 8 triples of
// (open, escape, close). Reordered into 8 DelimiterPair structs.
QVector<DelimiterPair> decodeDelimiters(const QString& encoded)
{
    const QStringList parts = decodePrefixedSlots(encoded, 24);
    QVector<DelimiterPair> pairs;
    pairs.reserve(8);
    for (int i = 0; i < 8; ++i) {
        DelimiterPair p;
        p.open   = parts[i * 3 + 0];
        p.escape = parts[i * 3 + 1];
        p.close  = parts[i * 3 + 2];
        pairs.append(p);
    }
    return pairs;
}

QString encodeDelimiters(const QVector<DelimiterPair>& pairs)
{
    QStringList parts;
    parts.reserve(24);
    for (int i = 0; i < 8; ++i) {
        const DelimiterPair p = (i < pairs.size()) ? pairs[i] : DelimiterPair{};
        parts.append(p.open);
        parts.append(p.escape);
        parts.append(p.close);
    }
    return encodePrefixedSlots(parts);
}

// Phase 5U.3-polish-tail (quote-stripping). Mirrors the
// quote-stripping pass in upstream's `setUserLexer`
// (ScintillaEditView.cpp:1116-1177). Lets UDL authors write
// multi-word keywords like `"public class"` or `'switch case'` —
// the lexer reads the resulting `\v` (between-words inside double
// quotes) or `\b` (inside single quotes) marker as "this space is
// part of the keyword, not a separator." Quote characters
// themselves are dropped; `\"`, `\'`, `\\` escapes copy through.
//
// Applied ONLY to entries that go through SCI_SETKEYWORDS — the
// SCI_SETPROPERTY path (comments, numbers, operators1, delimiters,
// fold-in-code1) reaches the lexer's GetProperty call site
// unchanged, so quote-stripping there would corrupt the literal
// markers users typed.
// Phase 5U.3-polish-tail.1 — `<WordsStyle name="..." />` to SCE_USER_STYLE_*.
// Mirrors upstream's GlobalMappers::styleIdMapper (UserDefineDialog.h:147-170)
// for the canonical "post 2.0" names + the pre-2.0 aliases. The lexer reads
// `userDefine.nesting.<styleID>` (zero-padded 2-digit decimal) — see
// ScintillaEditView.cpp:1199-1202.
struct StyleNameMap { const char* name; int id; };
constexpr StyleNameMap kStyleNameToId[] = {
    // post-2.0 canonical names
    {"DEFAULT",            SCE_USER_STYLE_DEFAULT},
    {"COMMENTS",           SCE_USER_STYLE_COMMENT},
    {"LINE COMMENTS",      SCE_USER_STYLE_COMMENTLINE},
    {"NUMBERS",            SCE_USER_STYLE_NUMBER},
    {"KEYWORDS1",          SCE_USER_STYLE_KEYWORD1},
    {"KEYWORDS2",          SCE_USER_STYLE_KEYWORD2},
    {"KEYWORDS3",          SCE_USER_STYLE_KEYWORD3},
    {"KEYWORDS4",          SCE_USER_STYLE_KEYWORD4},
    {"KEYWORDS5",          SCE_USER_STYLE_KEYWORD5},
    {"KEYWORDS6",          SCE_USER_STYLE_KEYWORD6},
    {"KEYWORDS7",          SCE_USER_STYLE_KEYWORD7},
    {"KEYWORDS8",          SCE_USER_STYLE_KEYWORD8},
    {"OPERATORS",          SCE_USER_STYLE_OPERATOR},
    {"FOLDER IN CODE1",    SCE_USER_STYLE_FOLDER_IN_CODE1},
    {"FOLDER IN CODE2",    SCE_USER_STYLE_FOLDER_IN_CODE2},
    {"FOLDER IN COMMENT",  SCE_USER_STYLE_FOLDER_IN_COMMENT},
    {"DELIMITERS1",        SCE_USER_STYLE_DELIMITER1},
    {"DELIMITERS2",        SCE_USER_STYLE_DELIMITER2},
    {"DELIMITERS3",        SCE_USER_STYLE_DELIMITER3},
    {"DELIMITERS4",        SCE_USER_STYLE_DELIMITER4},
    {"DELIMITERS5",        SCE_USER_STYLE_DELIMITER5},
    {"DELIMITERS6",        SCE_USER_STYLE_DELIMITER6},
    {"DELIMITERS7",        SCE_USER_STYLE_DELIMITER7},
    {"DELIMITERS8",        SCE_USER_STYLE_DELIMITER8},
    // pre-2.0 aliases (older UDLs from before NPP 2.0)
    {"FOLDEROPEN",         SCE_USER_STYLE_FOLDER_IN_CODE1},
    {"FOLDERCLOSE",        SCE_USER_STYLE_FOLDER_IN_CODE1},
    {"KEYWORD1",           SCE_USER_STYLE_KEYWORD1},
    {"KEYWORD2",           SCE_USER_STYLE_KEYWORD2},
    {"KEYWORD3",           SCE_USER_STYLE_KEYWORD3},
    {"KEYWORD4",           SCE_USER_STYLE_KEYWORD4},
    {"COMMENT",            SCE_USER_STYLE_COMMENT},
    {"COMMENT LINE",       SCE_USER_STYLE_COMMENTLINE},
    {"NUMBER",             SCE_USER_STYLE_NUMBER},
    {"OPERATOR",           SCE_USER_STYLE_OPERATOR},
    {"DELIMINER1",         SCE_USER_STYLE_DELIMITER1},   // upstream typo
    {"DELIMINER2",         SCE_USER_STYLE_DELIMITER2},
    {"DELIMINER3",         SCE_USER_STYLE_DELIMITER3},
};

int styleIdForName(const QString& name)
{
    const QByteArray nameBytes = name.toUtf8();
    for (const auto& m : kStyleNameToId) {
        if (std::strcmp(m.name, nameBytes.constData()) == 0) return m.id;
    }
    return -1;
}

QByteArray applyQuoteStripping(const QByteArray& raw)
{
    QByteArray out;
    out.reserve(raw.size());

    bool inDouble = false;
    bool inSingle = false;
    bool nonWsFound = false;
    const int len = raw.size();
    const char* s = raw.constData();

    for (int j = 0; j < len; ++j) {
        const char c = s[j];

        // Quote toggles. In a balanced double-quote block, `'` is just
        // a literal char (and vice versa).
        if (!inSingle && c == '"') { inDouble = !inDouble; continue; }
        if (!inDouble && c == '\'') { inSingle = !inSingle; continue; }

        // Backslash escapes. \" \' \\ — copy the second char literally.
        if (c == '\\' && j + 1 < len
            && (s[j+1] == '"' || s[j+1] == '\'' || s[j+1] == '\\'))
        {
            out.append(s[j+1]);
            ++j;
            continue;
        }

        if (inDouble || inSingle) {
            if (c > ' ') {
                out.append(c);
                if (!nonWsFound) nonWsFound = true;
            } else if (nonWsFound
                    && j > 0 && s[j-1] != '"'
                    && j + 1 < len && s[j+1] != '"'
                    && s[j+1] > ' ')
            {
                // Inter-word space inside quotes. Emit the lexer-known
                // "space-as-glue" marker so the keyword stays a single
                // token. \v for double-quoted, \b for single-quoted —
                // matches upstream's two markers.
                out.append(inDouble ? '\v' : '\b');
            }
            // Other whitespace inside quotes is dropped silently.
        } else {
            out.append(c);
        }
    }
    return out;
}

QString configFilePath()
{
    // Phase 12 — route through Config::configFilePath() so the cloud
    // sync override applies. Pre-Phase-12 this resolved straight to
    // QStandardPaths::AppConfigLocation; now it goes via the sentinel.
    const QString cfg = Config::configFilePath();
    QFileInfo fi(cfg);
    return fi.absolutePath() + QStringLiteral("/userDefineLangs.xml");
}

QString perFileDirPath()
{
    const QString cfg = Config::configFilePath();
    QFileInfo fi(cfg);
    return fi.absolutePath() + QStringLiteral("/userDefineLangs");
}

// Parse one `<NotepadPlus>...</NotepadPlus>` document at `path`, append every
// non-empty-named `<UserLang>` child to `result`, dedup'd by name against
// already-present entries (first occurrence wins). Missing or malformed
// files are silently no-ops — first-run + bad-XML both fall through.
namespace {
void appendUDLsFromFile(const QString& path, QVector<UDL>& result)
{
    QFile f(path);
    if (!f.exists() || !f.open(QIODevice::ReadOnly)) return;
    const QByteArray bytes = f.readAll();
    f.close();
    if (bytes.isEmpty()) return;

    pugi::xml_document doc;
    const auto pr = doc.load_buffer(bytes.constData(),
                                    static_cast<size_t>(bytes.size()));
    if (!pr) return;

    const auto root = doc.child("NotepadPlus");
    for (pugi::xml_node node : root.children("UserLang")) {
        UDL udl;
        parseUserLang(node, udl);
        if (udl.name.isEmpty()) continue;
        // Dedup by name: first occurrence wins. The combined file is
        // loaded first, so per-file entries that collide with combined-
        // file entries get silently shadowed. Power users editing
        // through the dialog won't have their edits clobbered by a
        // stale per-file XML.
        bool dup = false;
        for (const UDL& existing : result) {
            if (existing.name == udl.name) { dup = true; break; }
        }
        if (dup) continue;
        result.push_back(std::move(udl));
    }
}
} // namespace

QVector<UDL> loadAll()
{
    QVector<UDL> result;

    // 1. Combined file (the canonical, dialog-writable container).
    appendUDLsFromFile(configFilePath(), result);

    // 2. Per-file UDLs in the userDefineLangs/ subdir, sorted for
    //    determinism. Phase 5U.3-polish-tail.3 — drop in upstream's
    //    installer's per-language XMLs (or any file with a
    //    <NotepadPlus><UserLang>...</UserLang></NotepadPlus> shape) and
    //    they'll auto-load on next launch. Saves with the dialog still
    //    write only to the combined file; per-file UDLs that collide
    //    with a combined-file entry by name are shadowed (combined
    //    wins). Edits to a per-file UDL via the dialog "promote" it
    //    into the combined file on Save.
    const QDir perFile(perFileDirPath());
    if (perFile.exists()) {
        const QStringList xmls = perFile.entryList(
            {QStringLiteral("*.xml")}, QDir::Files, QDir::Name);
        for (const QString& name : xmls) {
            appendUDLsFromFile(perFile.absoluteFilePath(name), result);
        }
    }

    return result;
}

bool saveAll(const QVector<UDL>& udls)
{
    const QString target = configFilePath();
    const QString tmp    = target + QStringLiteral(".tmp");

    pugi::xml_document doc;
    pugi::xml_node decl = doc.append_child(pugi::node_declaration);
    decl.append_attribute("version")  = "1.0";
    decl.append_attribute("encoding") = "UTF-8";
    pugi::xml_node root = doc.append_child("NotepadPlus");
    for (const UDL& udl : udls) {
        if (udl.name.isEmpty()) continue;
        writeUserLang(root, udl);
    }

    // Ensure the parent directory exists. Qt's AppConfigLocation may not
    // exist on first run if Config::save() hasn't created it yet.
    const QFileInfo fi(target);
    fi.absoluteDir().mkpath(QStringLiteral("."));

    if (!doc.save_file(tmp.toUtf8().constData(), "  ",
                       pugi::format_indent | pugi::format_save_file_text,
                       pugi::encoding_utf8))
    {
        return false;
    }
    QFile::remove(target);    // ignore errors; rename below is the atomic step
    return QFile::rename(tmp, target);
}

void applyToLexer(ScintillaEditBase* editor, const UDL& udl)
{
    if (!editor) return;

    auto setProp = [editor](const char* key, const char* val) {
        editor->send(static_cast<unsigned int>(Message::SetProperty),
                     reinterpret_cast<Scintilla::uptr_t>(key),
                     reinterpret_cast<Scintilla::sptr_t>(val));
    };

    // Boolean / int settings — see ScintillaEditView.cpp:1104-1108 for the
    // canonical sequence. Scintilla's lexer reads these via
    // styler.GetPropertyInt (LexUser.cxx:1254-1273).
    setProp("fold", "1");
    setProp("userDefine.isCaseIgnored",       udl.isCaseIgnored       ? "1" : "0");
    setProp("userDefine.allowFoldOfComments", udl.allowFoldOfComments ? "1" : "0");
    setProp("userDefine.foldCompact",         udl.foldCompact         ? "1" : "0");

    // userDefine.prefixKeywords1..8 — per LexUser.cxx:1266-1273.
    for (int i = 0; i < 8; ++i) {
        char key[32];
        std::snprintf(key, sizeof(key), "userDefine.prefixKeywords%d", i + 1);
        setProp(key, udl.isPrefix[i] ? "1" : "0");
    }

    // forcePureLC + decimalSeparator — the lexer reads these as ints.
    {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%d", udl.forcePureLC);
        setProp("userDefine.forcePureLC", buf);
        std::snprintf(buf, sizeof(buf), "%d", udl.decimalSeparator);
        setProp("userDefine.decimalSeparator", buf);
    }

    // Keyword lists. Some go through SCI_SETPROPERTY, the rest through
    // SCI_SETKEYWORDS. The SCI_SETKEYWORDS path runs through the
    // applyQuoteStripping pass (Phase 5U.3-polish-tail.2) so multi-word
    // keywords written as `"new line"` / `'public class'` reach the
    // lexer with their inter-word whitespace replaced by the
    // single-token glue markers (\v / \b).
    int kwSet = 0;
    QVector<QByteArray> kwBytes;       // own the c_str backing
    kwBytes.reserve(28);
    for (int i = 0; i < 28; ++i) {
        QByteArray bytes = udl.keywords[i].toUtf8();
        if (!kKwlPropertyKey[i]) {
            // SCI_SETKEYWORDS path — strip and rewrite.
            bytes = applyQuoteStripping(bytes);
        }
        kwBytes.push_back(bytes);
        const char* cstr = kwBytes.last().constData();
        if (kKwlPropertyKey[i]) {
            setProp(kKwlPropertyKey[i], cstr);
        } else {
            // counter increments only for non-property entries — matches
            // upstream's interleaved order in setUserLexer.
            editor->sends(static_cast<unsigned int>(Message::SetKeyWords),
                          static_cast<Scintilla::uptr_t>(kwSet++),
                          cstr);
        }
    }

    // Phase 5U.3-polish-tail.1 — per-style nesting bitmasks. Mirrors
    // upstream's setUserLexer (ScintillaEditView.cpp:1194-1205) which
    // pushes one `userDefine.nesting.<styleID>` property per style row
    // (zero-padded 2-digit decimal id). Skip styles whose name doesn't
    // map to a known SCE_USER_STYLE_* id (older UDLs may have empty or
    // typo'd names) and skip nesting=0 styles to avoid a no-op push
    // that would clutter the lexer's property store. Editing nesting
    // through the dialog UI is still future work — for now this honours
    // the value present in the loaded UDL XML, so upstream-installer
    // UDLs with non-zero nesting values get the right lexer behaviour.
    char keyBuf[32];
    char valBuf[16];
    for (const UDLStyle& s : udl.styles) {
        if (s.nesting == 0 || s.name.isEmpty()) continue;
        const int sid = styleIdForName(s.name);
        if (sid < 0) continue;
        std::snprintf(keyBuf, sizeof(keyBuf), "userDefine.nesting.%02d", sid);
        std::snprintf(valBuf, sizeof(valBuf), "%d", s.nesting);
        setProp(keyBuf, valBuf);
    }
}

} // namespace UserDefineLang
