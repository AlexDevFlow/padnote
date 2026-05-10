// FunctionListXml.cpp — see FunctionListXml.h for the contract.
//
// Implementation strategy:
//   • Each language's XML lives at `:/functionList/<id>.xml` (Qt
//     resource alias). On first request for a language, parse the
//     XML with PugiXML into an in-memory `Parser` struct caching
//     compiled QRegularExpression objects.
//   • extract() runs the parser:
//       1. If a `commentExpr` is present, replace every match with
//          spaces so the body-scanning regexes don't see comments
//          (preserves byte offsets so line numbers stay accurate).
//       2. For each top-level `<function>`: iterate mainExpr matches
//          on the cleaned text; for each, run `nameExpr` against the
//          matched span to extract the symbol name; record line.
//       3. For each `<classRange>`: iterate mainExpr matches; for
//          each, extract the class name via className/nameExpr; then
//          iterate every inner `<function>`'s mainExpr against the
//          range's text only — qualified names emit as
//          "<class>::<method>".
//
// The 2 MB body cap mirrors FunctionList.cpp's cap so a giant minified
// JS won't freeze the dock.

#include "FunctionListXml.h"

#include <QByteArray>
#include <QFile>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QRegularExpressionMatch>
#include <QRegularExpressionMatchIterator>

#include "Buffer.h"
#include "Languages.h"
#include "ScintillaEditBase.h"
#include "ScintillaMessages.h"
#include "Scintilla.h"

#include "pugixml.hpp"

#include <memory>

using Scintilla::Message;

namespace {

struct NamedExpr {
    QString             pattern;
    QRegularExpression  re;          // pre-compiled; isValid()==false on bad pattern
};

struct InnerFunction {
    NamedExpr   mainExpr;
    NamedExpr   nameExpr;            // from <functionName><funcNameExpr expr=…/>
};

struct ClassRange {
    NamedExpr   mainExpr;
    NamedExpr   classNameExpr;
    QVector<InnerFunction> functions;
};

struct Parser {
    bool                    loaded = false;     // attempted parse; isValid==false if XML unsound
    bool                    valid  = false;
    NamedExpr               commentExpr;
    QVector<InnerFunction>  topFunctions;       // class-less <function> rows
    QVector<ClassRange>     classRanges;
};

QRegularExpression compile(const QString& pattern)
{
    if (pattern.isEmpty()) return {};
    // Match upstream's `SCFIND_REGEXP | SCFIND_POSIX |
    // SCFIND_REGEXP_DOTMATCHESNL` flag set
    // (functionParser.cpp:395). MultilineOption makes ^/$ match line
    // boundaries; DotMatchesEverythingOption makes `.` cross
    // newlines (so `.*?` can span class bodies); UseUnicodeProperties
    // gets us \w / \s / \d operating over Unicode properly.
    QRegularExpression re(pattern,
        QRegularExpression::PatternOption::UseUnicodePropertiesOption
      | QRegularExpression::PatternOption::MultilineOption
      | QRegularExpression::PatternOption::DotMatchesEverythingOption);
    re.optimize();
    return re;
}

NamedExpr loadNamedExpr(const QString& pattern)
{
    NamedExpr ne;
    ne.pattern = pattern;
    ne.re      = compile(pattern);
    return ne;
}

// Parse `<functionName><nameExpr expr="…"/></functionName>` (or
// `<funcNameExpr/>` — upstream's XMLs use either spelling). Falls
// back to an empty pattern (which compiles to !isValid()) when no
// child element matches.
QString readNameExpr(const pugi::xml_node& parent)
{
    if (auto fn = parent.child("functionName")) {
        for (pugi::xml_node c : fn.children()) {
            const char* expr = c.attribute("expr").value();
            if (expr && *expr) return QString::fromUtf8(expr);
        }
    }
    if (auto cn = parent.child("className")) {
        for (pugi::xml_node c : cn.children()) {
            const char* expr = c.attribute("expr").value();
            if (expr && *expr) return QString::fromUtf8(expr);
        }
    }
    return QString();
}

// Same idea but specifically for the className inside a classRange.
QString readClassNameExpr(const pugi::xml_node& classRange)
{
    if (auto cn = classRange.child("className")) {
        for (pugi::xml_node c : cn.children()) {
            const char* expr = c.attribute("expr").value();
            if (expr && *expr) return QString::fromUtf8(expr);
        }
    }
    return QString();
}

void parseFunction(const pugi::xml_node& fn, InnerFunction& out)
{
    const char* main = fn.attribute("mainExpr").value();
    if (main && *main) out.mainExpr = loadNamedExpr(QString::fromUtf8(main));
    const QString nameExpr = readNameExpr(fn);
    if (!nameExpr.isEmpty()) out.nameExpr = loadNamedExpr(nameExpr);
}

Parser loadParser(const QString& internalName)
{
    Parser p;
    p.loaded = true;

    const QString resPath = QStringLiteral(":/functionList/%1.xml").arg(internalName);
    QFile f(resPath);
    if (!f.exists() || !f.open(QIODevice::ReadOnly)) return p;
    const QByteArray bytes = f.readAll();
    f.close();
    if (bytes.isEmpty()) return p;

    pugi::xml_document doc;
    if (!doc.load_buffer(bytes.constData(),
                         static_cast<size_t>(bytes.size()))) return p;

    const auto root = doc.child("NotepadPlus").child("functionList");
    const auto parserNode = root.child("parser");
    if (!parserNode) return p;

    if (const char* ce = parserNode.attribute("commentExpr").value(); ce && *ce) {
        p.commentExpr = loadNamedExpr(QString::fromUtf8(ce));
    }

    for (auto cr = parserNode.child("classRange"); cr;
         cr = cr.next_sibling("classRange"))
    {
        ClassRange cls;
        if (const char* main = cr.attribute("mainExpr").value(); main && *main) {
            cls.mainExpr = loadNamedExpr(QString::fromUtf8(main));
        }
        const QString classNameExpr = readClassNameExpr(cr);
        if (!classNameExpr.isEmpty()) {
            cls.classNameExpr = loadNamedExpr(classNameExpr);
        }
        for (auto fn = cr.child("function"); fn;
             fn = fn.next_sibling("function"))
        {
            InnerFunction inner;
            parseFunction(fn, inner);
            cls.functions.append(std::move(inner));
        }
        p.classRanges.append(std::move(cls));
    }

    for (auto fn = parserNode.child("function"); fn;
         fn = fn.next_sibling("function"))
    {
        InnerFunction inner;
        parseFunction(fn, inner);
        p.topFunctions.append(std::move(inner));
    }

    p.valid = !(p.topFunctions.isEmpty() && p.classRanges.isEmpty());
    return p;
}

QHash<QString, Parser>& cache()
{
    static QHash<QString, Parser> c;
    return c;
}

QMutex& cacheMutex()
{
    static QMutex m;
    return m;
}

const Parser& getOrLoad(const QString& internalName)
{
    QMutexLocker lock(&cacheMutex());
    auto& c = cache();
    auto it = c.find(internalName);
    if (it == c.end()) {
        it = c.insert(internalName, loadParser(internalName));
    }
    return it.value();
}

// Replace every char in `text` whose byte position falls inside any
// commentExpr match with a space. Preserves byte offsets so per-line
// counts stay accurate. Newlines inside multi-line comments are
// preserved so line numbers don't drift.
QString stripComments(const QString& text, const NamedExpr& commentExpr)
{
    if (!commentExpr.re.isValid()) return text;
    QString out = text;
    auto mi = commentExpr.re.globalMatch(text);
    while (mi.hasNext()) {
        const auto m = mi.next();
        const int start = m.capturedStart();
        const int end   = m.capturedEnd();
        if (start < 0 || end <= start) continue;
        for (int i = start; i < end; ++i) {
            if (out[i] != QChar('\n')) out[i] = QChar(' ');
        }
    }
    return out;
}

int lineFromOffset(const QString& text, int offset)
{
    if (offset <= 0) return 1;
    if (offset > text.size()) offset = text.size();
    int line = 1;
    for (int i = 0; i < offset; ++i) {
        if (text[i] == QChar('\n')) ++line;
    }
    return line;
}

QString matchName(const NamedExpr& nameExpr, const QString& haystack)
{
    if (!nameExpr.re.isValid()) return QString();
    const auto m = nameExpr.re.match(haystack);
    if (!m.hasMatch()) return QString();
    return m.captured(0).trimmed();
}

void runTopLevelFunctions(const Parser& p, const QString& clean,
                          const QString& original,
                          QVector<FunctionList::Symbol>& out)
{
    for (const InnerFunction& fn : p.topFunctions) {
        if (!fn.mainExpr.re.isValid()) continue;
        auto mi = fn.mainExpr.re.globalMatch(clean);
        while (mi.hasNext()) {
            const auto m = mi.next();
            const QString span = m.captured(0);
            QString name = matchName(fn.nameExpr, span);
            if (name.isEmpty()) name = span.trimmed();
            if (name.isEmpty()) continue;
            const int line = lineFromOffset(original, m.capturedStart());
            out.append({name, QStringLiteral("function"), line});
        }
    }
}

void runClassRanges(const Parser& p, const QString& clean,
                    const QString& original,
                    QVector<FunctionList::Symbol>& out)
{
    for (const ClassRange& cls : p.classRanges) {
        if (!cls.mainExpr.re.isValid()) continue;
        auto mi = cls.mainExpr.re.globalMatch(clean);
        while (mi.hasNext()) {
            const auto m = mi.next();
            const QString span = m.captured(0);
            const int spanStart = m.capturedStart();
            QString className = matchName(cls.classNameExpr, span);
            if (className.isEmpty()) className = QStringLiteral("(anon)");
            const int classLine = lineFromOffset(original, spanStart);
            out.append({className, QStringLiteral("class"), classLine});
            for (const InnerFunction& fn : cls.functions) {
                if (!fn.mainExpr.re.isValid()) continue;
                auto innerMi = fn.mainExpr.re.globalMatch(span);
                while (innerMi.hasNext()) {
                    const auto im = innerMi.next();
                    const QString innerSpan = im.captured(0);
                    QString methodName = matchName(fn.nameExpr, innerSpan);
                    if (methodName.isEmpty()) methodName = innerSpan.trimmed();
                    if (methodName.isEmpty()) continue;
                    const int innerLine = lineFromOffset(
                        original, spanStart + im.capturedStart());
                    out.append({className + QStringLiteral("::") + methodName,
                                QStringLiteral("function"), innerLine});
                }
            }
        }
    }
}

} // namespace

namespace FunctionListXml {

bool hasParserFor(const QString& internalName)
{
    if (internalName.isEmpty()) return false;
    return getOrLoad(internalName).valid;
}

QVector<FunctionList::Symbol> extractFromText(const QString& text,
                                              const QString& internalName)
{
    if (text.isEmpty() || internalName.isEmpty()) return {};
    if (text.size() > 2 * 1024 * 1024) return {};   // 2 MB cap, mirrors FunctionList.cpp
    const Parser& p = getOrLoad(internalName);
    if (!p.valid) return {};

    const QString clean = stripComments(text, p.commentExpr);

    QVector<FunctionList::Symbol> out;
    out.reserve(64);
    runClassRanges(p, clean, text, out);
    runTopLevelFunctions(p, clean, text, out);

    std::sort(out.begin(), out.end(),
        [](const FunctionList::Symbol& a, const FunctionList::Symbol& b) {
            return a.line < b.line;
        });

    return out;
}

QVector<FunctionList::Symbol> extract(Buffer* buffer)
{
    if (!buffer) return {};
    const LanguageDef* lang = buffer->language();
    if (!lang) return {};
    const QString key = QString::fromUtf8(lang->internalName.c_str());

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
    return extractFromText(text, key);
}

} // namespace FunctionListXml
