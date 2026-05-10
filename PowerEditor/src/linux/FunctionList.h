// FunctionList.h — Phase 3c.2: per-buffer symbol extraction.
//
// MVP scope: a curated set of per-language QRegularExpression patterns that
// pull function / class / heading / section names from the active buffer.
// Faithful Boost.Regex-driven parsing of upstream's installer/functionList/*.xml
// (which uses \K, named subroutines, recursion) is deferred to a future
// polish phase — that XML grammar is non-trivial to port and the curated
// approach covers ~80% of daily-driver languages.
//
// Per-language coverage:
//   Python, C, C++, C#, Java, JavaScript / TypeScript, Bash, Lua, INI,
//   Markdown, CMake.
//
// Languages without a pattern return an empty list; the dock shows a
// "(no symbols available for <Language>)" placeholder in that case.

#pragma once

#include <QString>
#include <QVector>

class Buffer;

namespace FunctionList {

struct Symbol {
    QString name;       // e.g. "myFunction" or "MyClass::method"
    QString kind;       // "function", "class", "section", "heading"
    int     line;       // 1-based line number for jump
};

// Extract symbols from the buffer's current text. Returns an empty vector
// if the buffer is null, has no language, or the language has no pattern.
QVector<Symbol> extract(Buffer* buffer);

// Returns true if at least one pattern is wired for the given language.
// Used by the dock to render a friendlier "(unsupported)" placeholder.
bool hasParserFor(const QString& internalName);

} // namespace FunctionList
