// Languages.h — Phase 4b: language registry built at startup from
// langs.model.xml. Phase 5U.2 added UDL augmentation: user-defined
// languages are appended to the registry via Languages::appendUDL(...)
// between init() and finalize() in main_qt.cpp.
//
// The internalName→(displayName, Lexilla lexer ID) map is still hardcoded
// (mirroring upstream's _langNameInfoArray in
// PowerEditor/src/ScintillaComponent/ScintillaEditView.cpp:99-202 — the XML
// doesn't carry that info). Extensions and keyword sets come from
// PowerEditor/src/langs.model.xml, embedded as a Qt resource.

#pragma once

#include <QString>
#include <QStringList>
#include <string>
#include <vector>

struct UDL;   // PowerEditor/src/linux/UserDefineLang.h — Phase 5U.2

struct LanguageDef {
    std::string  internalName;            // matches upstream _langName, e.g. "cpp"
                                          // UDL entries use "udl:<name>" so they
                                          // can never collide with built-ins.
    QString      displayName;             // human-readable, e.g. "C++"
    std::string  lexilla;                 // lexer ID for Lexilla::CreateLexer.
                                          // UDL entries store "user".
    std::string  extensions;              // space-separated, lowercase
    std::vector<std::string> keywords;    // 0-8 entries, indexed by Scintilla
                                          // keyword set. Empty for UDL — the
                                          // UDL's own keyword tables are pushed
                                          // by UserDefineLang::applyToLexer.
    // Comment delimiters from langs.model.xml (e.g., commentLine="//"
    // commentStart="/*" commentEnd="*/" for C). Either may be empty if the
    // language doesn't support that style.
    std::string  commentLine;             // line-comment prefix, e.g. "//"
    std::string  commentStart;            // block-comment open, e.g. "/*"
    std::string  commentEnd;              // block-comment close, e.g. "*/"

    // Phase 5U.2 — non-null on UDL entries. Points into a parallel
    // `std::vector<std::unique_ptr<UDL>>` in Languages.cpp; address stays
    // stable for the app's lifetime (or until reloadUDLs() in 5U.3+).
    const UDL*   userDefined = nullptr;
};

namespace Languages {

// Initialise the registry from the embedded langs.model.xml resource.
// Idempotent — calls after the first are no-ops. Call once at app startup
// before any findBy* lookup or the language menu is built.
//
// Phase 5U.2 — init() no longer sorts the registry. Caller is expected to
// call appendUDL() zero-or-more times, then finalize() exactly once. The
// existing `g_initialised` guard means re-entrant init() calls are safe.
void init();

// Phase 5U.2 — append a user-defined language entry. Must be called AFTER
// init() and BEFORE finalize(). The UDL is copied into a stable backing
// store; the resulting LanguageDef points at it via `userDefined`. The
// entry's internalName is "udl:<udl.name>" (utf8) so collisions with
// built-ins (cpp / python / …) are impossible.
void appendUDL(const UDL& udl);

// Phase 5U.2 — sort the registry by display name and cache pointer-stable
// references (e.g. plainText). Must be called once after init() + the
// appendUDL() calls. Idempotent.
void finalize();

// Phase 5U.3 — drop every UDL entry from the registry, re-load
// userDefineLangs.xml from disk, append fresh entries, and re-sort.
// Used by UserDefineDialog after a successful Save so newly-defined or
// edited UDLs become visible without restart.
//
// Buffers holding `m_language` pointers to UDL entries see those slots
// rewritten with potentially-different content; callers SHOULD walk
// open buffers and re-resolve UDL languages by `internalName` (which
// is stable: "udl:<name>"). If a UDL was deleted, lookup returns
// nullptr and the buffer should fall back to plainText.
//
// `m_languageActions` and any other QAction* parallel to `all()` MUST
// be torn down and rebuilt by the caller — slot ↔ display-name
// alignment may have shifted across the sort.
void reloadUDLs();

// Returns a pointer to the start of the registry, plus the count.
const LanguageDef* all();
int                count();

// Returns the LanguageDef for a given filesystem path (suffix lookup, plus
// special-case basenames like "Makefile" / "CMakeLists.txt"). Returns the
// "text" entry for unknown extensions; never nullptr post-init.
const LanguageDef* findByExtension(const QString& path);

// Lookup by internalName ("cpp", "python", "udl:MyLang", ...). Returns
// nullptr if not found.
const LanguageDef* findByInternalName(const char* name);

// Convenience: the "plain text" entry. Always present post-init.
const LanguageDef* plainText();

} // namespace Languages
