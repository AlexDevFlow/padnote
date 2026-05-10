#include "FunctionList.h"

#include <QHash>
#include <QRegularExpression>
#include <QRegularExpressionMatch>
#include <QRegularExpressionMatchIterator>

#include "Buffer.h"
#include "FunctionListXml.h"   // Phase 5J-polish — XML-driven parser path
#include "Languages.h"
#include "Scintilla.h"
#include "ScintillaEditBase.h"
#include "ScintillaMessages.h"

#include <vector>

using Scintilla::Message;

namespace {

// One parser entry. The `pattern` is matched against the entire buffer text
// (multi-line mode) with `nameCapture` identifying which capture group holds
// the symbol name. `kind` labels the entry in the tree view. Multiple parsers
// per language run in declaration order; the first match wins per byte
// position (regex iteration just walks left-to-right, so two parsers that
// cover overlapping syntaxes — e.g. "class" and "function" — are fine
// because their non-empty matches don't overlap on the same prefix).
struct Parser {
    QString pattern;
    int     nameCapture = 1;
    QString kind        = QStringLiteral("function");
};

const QHash<QString, std::vector<Parser>>& parsers()
{
    // Patterns are deliberately conservative — they trade a few false
    // negatives for a robustness against malformed/comment-embedded code.
    // Multi-line mode `(?m)` makes ^ / $ match line boundaries.
    static const QHash<QString, std::vector<Parser>> P = {
        // ---- Python ------------------------------------------------------
        // class Foo: ...   |   def bar(...):   |   async def baz(...):
        {QStringLiteral("python"), {
            {QStringLiteral("(?m)^[ \\t]*class[ \\t]+([A-Za-z_][A-Za-z0-9_]*)"),
             1, QStringLiteral("class")},
            {QStringLiteral("(?m)^[ \\t]*(?:async[ \\t]+)?def[ \\t]+([A-Za-z_][A-Za-z0-9_]*)"),
             1, QStringLiteral("function")},
        }},

        // ---- C / C++ / Java / C# / JavaScript / TypeScript / Go / Rust --
        // Shared "cpp-family" treatment — type-and-name pairs. The captured
        // name is just the identifier; signature / return type stay implicit.
        {QStringLiteral("c"), {
            {QStringLiteral("(?m)^[A-Za-z_][A-Za-z0-9_*\\s]*?\\b([A-Za-z_][A-Za-z0-9_]*)\\s*\\([^;]*?\\)\\s*\\{"),
             1, QStringLiteral("function")},
        }},
        {QStringLiteral("cpp"), {
            {QStringLiteral("(?m)^[ \\t]*(?:class|struct)[ \\t]+([A-Za-z_][A-Za-z0-9_]*)"),
             1, QStringLiteral("class")},
            {QStringLiteral("(?m)^[A-Za-z_][A-Za-z0-9_:*&\\s]*?\\b([A-Za-z_][A-Za-z0-9_]*(?:::[A-Za-z_][A-Za-z0-9_]*)?)\\s*\\([^;]*?\\)\\s*(?:const\\s*)?(?:noexcept\\s*)?\\{"),
             1, QStringLiteral("function")},
        }},
        {QStringLiteral("java"), {
            {QStringLiteral("(?m)^[ \\t]*(?:public|private|protected)?[ \\t]*(?:abstract[ \\t]+)?(?:class|interface|enum)[ \\t]+([A-Za-z_][A-Za-z0-9_]*)"),
             1, QStringLiteral("class")},
            {QStringLiteral("(?m)^[ \\t]*(?:public|private|protected)[ \\t]+(?:static[ \\t]+)?(?:final[ \\t]+)?[A-Za-z_<>,\\[\\]\\s]+?\\b([A-Za-z_][A-Za-z0-9_]*)\\s*\\([^;]*?\\)\\s*(?:throws[^{]+)?\\{"),
             1, QStringLiteral("function")},
        }},
        {QStringLiteral("cs"), {
            {QStringLiteral("(?m)^[ \\t]*(?:public|private|internal|protected)?[ \\t]*(?:class|struct|interface|enum)[ \\t]+([A-Za-z_][A-Za-z0-9_]*)"),
             1, QStringLiteral("class")},
            {QStringLiteral("(?m)^[ \\t]*(?:public|private|internal|protected)[ \\t]+(?:static[ \\t]+)?[A-Za-z_<>,\\[\\]\\s]+?\\b([A-Za-z_][A-Za-z0-9_]*)\\s*\\([^;]*?\\)\\s*\\{"),
             1, QStringLiteral("function")},
        }},
        {QStringLiteral("javascript.js"), {
            {QStringLiteral("(?m)^[ \\t]*class[ \\t]+([A-Za-z_$][A-Za-z0-9_$]*)"),
             1, QStringLiteral("class")},
            // function foo(...) { OR const foo = (...) => { OR const foo = function(
            {QStringLiteral("(?m)^[ \\t]*(?:async[ \\t]+)?function\\s*\\*?\\s*([A-Za-z_$][A-Za-z0-9_$]*)\\s*\\("),
             1, QStringLiteral("function")},
            {QStringLiteral("(?m)^[ \\t]*(?:export[ \\t]+)?(?:const|let|var)[ \\t]+([A-Za-z_$][A-Za-z0-9_$]*)\\s*=\\s*(?:async[ \\t]*)?(?:function\\b|\\([^)]*\\)\\s*=>)"),
             1, QStringLiteral("function")},
        }},
        {QStringLiteral("typescript"), {
            {QStringLiteral("(?m)^[ \\t]*(?:export[ \\t]+)?(?:abstract[ \\t]+)?(?:class|interface|enum)[ \\t]+([A-Za-z_$][A-Za-z0-9_$]*)"),
             1, QStringLiteral("class")},
            {QStringLiteral("(?m)^[ \\t]*(?:export[ \\t]+)?(?:async[ \\t]+)?function\\s*\\*?\\s*([A-Za-z_$][A-Za-z0-9_$]*)\\s*[<(]"),
             1, QStringLiteral("function")},
            {QStringLiteral("(?m)^[ \\t]*(?:export[ \\t]+)?(?:const|let|var)[ \\t]+([A-Za-z_$][A-Za-z0-9_$]*)\\s*[:=]"),
             1, QStringLiteral("function")},
        }},
        {QStringLiteral("go"), {
            {QStringLiteral("(?m)^type[ \\t]+([A-Za-z_][A-Za-z0-9_]*)[ \\t]+(?:struct|interface)\\b"),
             1, QStringLiteral("class")},
            {QStringLiteral("(?m)^func\\b(?:\\s*\\([^)]+\\))?\\s+([A-Za-z_][A-Za-z0-9_]*)\\s*\\("),
             1, QStringLiteral("function")},
        }},
        {QStringLiteral("rust"), {
            {QStringLiteral("(?m)^[ \\t]*(?:pub[ \\t]+)?(?:struct|enum|trait|impl)[ \\t]+([A-Za-z_][A-Za-z0-9_]*)"),
             1, QStringLiteral("class")},
            {QStringLiteral("(?m)^[ \\t]*(?:pub[ \\t]+)?(?:async[ \\t]+)?fn[ \\t]+([A-Za-z_][A-Za-z0-9_]*)\\s*[<(]"),
             1, QStringLiteral("function")},
        }},

        // ---- Bash / Shell ------------------------------------------------
        {QStringLiteral("bash"), {
            {QStringLiteral("(?m)^[ \\t]*(?:function[ \\t]+)?([A-Za-z_][A-Za-z0-9_]*)\\s*\\(\\s*\\)\\s*\\{"),
             1, QStringLiteral("function")},
        }},

        // ---- Lua ---------------------------------------------------------
        {QStringLiteral("lua"), {
            {QStringLiteral("(?m)^[ \\t]*(?:local[ \\t]+)?function[ \\t]+([A-Za-z_][A-Za-z0-9_.:]*)\\s*\\("),
             1, QStringLiteral("function")},
        }},

        // ---- INI / props -------------------------------------------------
        {QStringLiteral("ini"), {
            {QStringLiteral("(?m)^\\[([^\\]]+)\\]\\s*$"),
             1, QStringLiteral("section")},
        }},

        // ---- Markdown ----------------------------------------------------
        // ATX headings only (## Foo). Setext (=== / ---) skipped — we'd
        // need to jump to the previous line for the heading text.
        {QStringLiteral("markdown"), {
            {QStringLiteral("(?m)^(#{1,6})[ \\t]+(.+?)[ \\t]*#*\\s*$"),
             2, QStringLiteral("heading")},
        }},

        // ---- CMake -------------------------------------------------------
        {QStringLiteral("cmake"), {
            {QStringLiteral("(?im)^[ \\t]*(?:function|macro)\\s*\\(\\s*([A-Za-z_][A-Za-z0-9_]*)"),
             1, QStringLiteral("function")},
        }},

        // ---- JSON --------------------------------------------------------
        // Top-level keys only (one level of indent). Useful for navigating
        // package.json / tsconfig.json.
        {QStringLiteral("json"), {
            {QStringLiteral("(?m)^  \"([^\"\\\\]+)\"\\s*:"),
             1, QStringLiteral("section")},
        }},
    };
    return P;
}

} // namespace

namespace FunctionList {

bool hasParserFor(const QString& internalName)
{
    // Phase 5J-polish — XML parser takes precedence; the curated regex
    // is the fallback for languages without a bundled XML.
    if (FunctionListXml::hasParserFor(internalName)) return true;
    return parsers().contains(internalName);
}

QVector<Symbol> extract(Buffer* buffer)
{
    if (!buffer) return {};
    const LanguageDef* lang = buffer->language();
    if (!lang) return {};

    // Phase 5J-polish — try the XML parser first; on empty result fall
    // through to the curated regex. The XML path returns empty when
    // either the XML is absent OR the buffer exceeds its size cap (the
    // curated path's cap covers the latter symmetrically).
    {
        QVector<Symbol> xmlSymbols = FunctionListXml::extract(buffer);
        if (!xmlSymbols.isEmpty()) return xmlSymbols;
    }

    const QString key = QString::fromUtf8(lang->internalName.c_str());
    const auto it = parsers().constFind(key);
    if (it == parsers().constEnd()) return {};

    // Pull the buffer's UTF-8 bytes. For very large buffers (>2 MB) we
    // skip — running 2-3 multiline regexes against a multi-megabyte file
    // would freeze the dock noticeably; better to surface "buffer too
    // large" than block.
    auto* ed = buffer->editor();
    const Scintilla::sptr_t length =
        ed->send(static_cast<unsigned int>(Message::GetTextLength));
    if (length <= 0) return {};
    if (length > 2 * 1024 * 1024) return {};   // 2 MB cap

    std::vector<char> buf(static_cast<std::size_t>(length) + 1, '\0');
    ed->send(static_cast<unsigned int>(Message::GetText),
             static_cast<Scintilla::uptr_t>(buf.size()),
             reinterpret_cast<Scintilla::sptr_t>(buf.data()));
    const QString text = QString::fromUtf8(buf.data(), static_cast<int>(length));

    QVector<Symbol> out;
    out.reserve(64);

    for (const Parser& p : it.value()) {
        QRegularExpression re(p.pattern);
        if (!re.isValid()) continue;
        QRegularExpressionMatchIterator mi = re.globalMatch(text);
        while (mi.hasNext()) {
            QRegularExpressionMatch m = mi.next();
            QString name = m.captured(p.nameCapture).trimmed();
            if (name.isEmpty()) continue;
            // For markdown headings, prefix with the heading-level hashes
            // so the user can see depth in the tree.
            if (p.kind == QStringLiteral("heading")) {
                const QString hashes = m.captured(1);
                name = hashes + QChar(' ') + name;
            }
            // Compute 1-based line number from the match's UTF-16 offset.
            const int offset = m.capturedStart(p.nameCapture);
            const int line = static_cast<int>(
                std::count(text.constBegin(),
                           text.constBegin() + offset,
                           QChar('\n'))) + 1;
            out.append({name, p.kind, line});
        }
    }

    // Sort by line so the tree mirrors document order regardless of which
    // parser matched first.
    std::sort(out.begin(), out.end(),
        [](const Symbol& a, const Symbol& b) { return a.line < b.line; });

    return out;
}

} // namespace FunctionList
