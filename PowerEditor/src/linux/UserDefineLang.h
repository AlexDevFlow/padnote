// UserDefineLang.h — User-Defined Language XML round-trip + Lexilla
//                    "user" lexer wiring.
//
// Storage path: $XDG_CONFIG_HOME/padnote/padnote--/userDefineLangs.xml
// (Qt's AppConfigLocation honours both organizationName + applicationName).
//
// • UDL struct = data model.
// • loadAll() / saveAll() — atomic round-trip via PugiXML.
// • applyToLexer(editor, udl) — pushes properties + keyword sets to
//   Scintilla so Lexilla's `lmUserDefine` lexer (lexilla/lexers/LexUser.cxx)
//   colourises the buffer with the UDL's keywords / comment markers.
//
// This file is deliberately UI-free so the round-trip + lexer push can
// be tested headlessly. The editor dialog lives in UserDefineDialog.

#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

class ScintillaEditBase;

// Round-trip preservation of a per-style row (`<WordsStyle>` element).
// The MVP doesn't *interpret* these — the lexer renders UDLs using the
// active theme's "User Defined" style entries. But preserving them on
// load and re-emitting them on save means Win32 NPP UDL files survive
// round-trip without losing their style data.
struct UDLStyle {
    QString name;          // upstream style name, e.g. "DEFAULT", "KEYWORDS1"
    QString fgColor;       // hex without leading #, e.g. "000000"
    QString bgColor;
    int     colorStyle = -1;  // -1 = unset (omit on write)
    QString fontName;
    int     fontStyle  = 0;
    int     fontSize   = 0;   // 0 = default (write empty string)
    int     nesting    = 0;
};

// Data model for a single user-defined language. Mirrors the subset of
// upstream's `UserLangContainer` (PowerEditor/src/Parameters.h:1008) that
// the Linux port's MVP exercises.
struct UDL {
    QString name;                      // user-visible display name (and
                                       // "udl:<name>" internalName seed)
    QString ext;                       // space-separated extensions
    QString udlVersion = QStringLiteral("2.1"); // preserved on round-trip
    bool    isDarkModeTheme = false;

    // Settings/Global
    bool isCaseIgnored       = false;
    bool allowFoldOfComments = false;
    bool foldCompact         = false;
    int  forcePureLC         = 0;      // 0/1/2 = NONE/BOL/WSP
    int  decimalSeparator    = 0;      // 0/1/2 = dot/comma/both

    // Settings/Prefix — 8 booleans, indexed 0..7 == KEYWORDS1..8.
    // When true, the keyword list is matched as a prefix rather than
    // a whole-token match.
    bool isPrefix[8] = {false, false, false, false,
                        false, false, false, false};

    // Keyword lists indexed by SCE_USER_KWLIST_* (see
    // lexilla/include/SciLexer.h:2391-2419). 28 entries.
    //   [0]  Comments
    //   [1]  Numbers, prefix1
    //   [2]  Numbers, prefix2
    //   [3]  Numbers, extras1
    //   [4]  Numbers, extras2
    //   [5]  Numbers, suffix1
    //   [6]  Numbers, suffix2
    //   [7]  Numbers, range
    //   [8]  Operators1
    //   [9]  Operators2
    //   [10] Folders in code1, open
    //   [11] Folders in code1, middle
    //   [12] Folders in code1, close
    //   [13] Folders in code2, open
    //   [14] Folders in code2, middle
    //   [15] Folders in code2, close
    //   [16] Folders in comment, open
    //   [17] Folders in comment, middle
    //   [18] Folders in comment, close
    //   [19] Keywords1
    //   ...
    //   [26] Keywords8
    //   [27] Delimiters
    QString keywords[28];

    // Round-trip preservation only; not interpreted in 5U.2.
    QVector<UDLStyle> styles;
};

// One row of the upstream 8-pair delimiter editor. Each pair has an open
// boundary, an optional escape char, and a close boundary. Round-trip via
// encodeDelimiters / decodeDelimiters into `keywords[SCE_USER_KWLIST_DELIMITERS]`.
struct DelimiterPair {
    QString open;
    QString escape;
    QString close;
};

namespace UserDefineLang {

// Absolute path to userDefineLangs.xml in the user's config dir.
QString configFilePath();

// Absolute path to the per-file UDL subdir (userDefineLangs/) — sibling
// of the combined userDefineLangs.xml. Read-only by convention: drop
// upstream's installer's per-language XMLs there to have them load
// alongside the user's combined file. The dialog's Save path writes
// only to the combined file; per-file UDLs that share a name with a
// combined-file entry are silently shadowed by the combined entry.
QString perFileDirPath();

// -----------------------------------------------------------------------------
// Comment / Delimiter encoding helpers (Phase 5U.3-polish).
//
// Upstream packs the comment-marker fields and the 8 delimiter pairs into a
// single space-separated string per keyword list, where each token is preceded
// by a 2-char prefix tag identifying its slot (e.g. "00", "01", ..., "23").
// See UserDefineDialog.cpp::convertTo / ::retrieve in the upstream tree.
//
// decodeComments / encodeComments — 5 slots in `keywords[SCE_USER_KWLIST_COMMENTS]`:
//   [0] line-comment opener         (e.g. "//")  — prefix "00"
//   [1] line-comment continuation   (e.g. "\\")  — prefix "01"
//   [2] line-comment closer         (rare)        — prefix "02"
//   [3] block-comment opener        (e.g. "/*")  — prefix "03"
//   [4] block-comment closer        (e.g. "*/")  — prefix "04"
//
// decodeDelimiters / encodeDelimiters — 8 pairs in
// `keywords[SCE_USER_KWLIST_DELIMITERS]`, each with open/escape/close:
//   pair[0].open=prefix "00", .escape=prefix "01", .close=prefix "02"
//   pair[1].open=prefix "03", .escape=prefix "04", .close=prefix "05"
//   ... pair[7] = prefixes "21" / "22" / "23".
//
// Both helpers are tolerant: an empty input produces empty slots; missing
// prefixes leave the corresponding slot empty; the order of tokens in the
// input doesn't matter.
QStringList         decodeComments(const QString& encoded);
QString             encodeComments(const QStringList& slots5);
QVector<DelimiterPair> decodeDelimiters(const QString& encoded);
QString             encodeDelimiters(const QVector<DelimiterPair>& pairs);

// Read every <UserLang> element from userDefineLangs.xml. Returns an empty
// vector if the file doesn't exist (first-run case) or fails to parse.
QVector<UDL> loadAll();

// Atomically write the supplied UDLs back to userDefineLangs.xml using the
// existing `.tmp + rename` pattern (see Config.cpp / Session.cpp). Returns
// true on success; false if the temp-write or the rename fails.
bool saveAll(const QVector<UDL>& udls);

// Push the UDL's settings + keyword lists to a Scintilla editor that's
// already attached to Lexilla's `user` lexer (CreateLexer("user")). The
// pushes use SCI_SETPROPERTY for entries the lexer reads via
// styler.GetProperty (LexUser.cxx:1254-1290 — `userDefine.*` keys) and
// SCI_SETKEYWORDS for the rest.
//
// Caller is expected to call CreateLexer + SetILexer BEFORE this; the
// lexer reads the properties when Scintilla colourises (after this push).
void applyToLexer(ScintillaEditBase* editor, const UDL& udl);

} // namespace UserDefineLang
