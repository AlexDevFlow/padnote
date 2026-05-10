// FunctionListXml.h — Phase 5J-polish: faithful XML-driven Function List
// parser, mirroring upstream's installer/functionList/*.xml grammar.
//
// MVP scope: bundle the simpler upstream XMLs as Qt resources (only
// python.xml in this initial commit). Each XML defines a `<parser>`
// with a `commentExpr` (regex matching code that should be masked
// out), zero-or-more `<classRange>` blocks (whose `mainExpr` matches
// a class boundary, then nested `<function>` blocks run inside), and
// zero-or-more top-level `<function>` blocks. Each `<function>` /
// `<className>` has a `<nameExpr>` (or `<funcNameExpr>`) that runs
// against the `mainExpr`'s match to extract the symbol name.
//
// The pattern features upstream's XMLs use (\K, lookbehind,
// (?(DEFINE)…) recursion, named groups) are all PCRE2 features,
// which QRegularExpression also supports under the hood, so we can
// reuse the patterns verbatim without porting to a different regex
// engine.
//
// Bundling more XMLs is a maintenance task: drop the file into
// `PowerEditor/src/linux/functionList/` (mirroring upstream's
// `PowerEditor/installer/functionList/<id>.xml`) and add a
// `<file alias="functionList/<id>.xml">…</file>` line to
// `resources.qrc`. The first call to `parserFor()` will lazy-load it.
//
// Languages without a bundled XML fall back to the curated regex
// path in FunctionList.cpp (Phase 3c.2 MVP).

#pragma once

#include <QString>
#include <QVector>

#include "FunctionList.h"   // FunctionList::Symbol

class Buffer;

namespace FunctionListXml {

// Returns true when an upstream-style XML parser exists for this
// language internalName (e.g. "python"). When true, the XML path
// takes precedence over the curated regex.
bool hasParserFor(const QString& internalName);

// Run the bundled XML parser against the buffer's text. Returns an
// empty vector when the language has no XML parser, the buffer is
// over the size cap, or a regex compilation fails. Callers should
// fall back to FunctionList::extract's curated regex path on empty
// returns.
QVector<FunctionList::Symbol> extract(Buffer* buffer);

// Headless overload — same parser, exercised against arbitrary text
// without needing a Buffer/ScintillaEditBase. Used by the standalone
// probe; the buffer-based extract() above just pulls Scintilla's
// text into a QString and routes here.
QVector<FunctionList::Symbol> extractFromText(const QString& text,
                                              const QString& internalName);

} // namespace FunctionListXml
