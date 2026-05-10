#include "Languages.h"

#include <QByteArray>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <algorithm>
#include <cstring>
#include <memory>
#include <unordered_map>

#include "pugixml.hpp"
#include "UserDefineLang.h"   // Phase 5U.2 — UDL struct definition

namespace {

// Hardcoded (internalName → displayName + Lexilla lexer ID) map. Mined from
// upstream's _langNameInfoArray in PowerEditor/src/ScintillaComponent/
// ScintillaEditView.cpp:99-202. The langs.model.xml doesn't carry the lexer
// ID column — it lives in the C++ table — so we keep it here and let the XML
// supply extensions + keywords for each entry by name.
struct HardcodedInfo {
    QString     displayName;
    std::string lexilla;
};

const std::unordered_map<std::string, HardcodedInfo>& hardcodedMap()
{
    static const std::unordered_map<std::string, HardcodedInfo> map = {
        {"normal",        {QStringLiteral("Plain text"),               "null"}},
        {"php",           {QStringLiteral("PHP"),                      "phpscript"}},
        {"c",             {QStringLiteral("C"),                         "cpp"}},
        {"cpp",           {QStringLiteral("C++"),                       "cpp"}},
        {"cs",            {QStringLiteral("C#"),                        "cpp"}},
        {"objc",          {QStringLiteral("Objective-C"),               "objc"}},
        {"java",          {QStringLiteral("Java"),                      "cpp"}},
        {"rc",            {QStringLiteral("RC"),                        "cpp"}},
        {"html",          {QStringLiteral("HTML"),                      "hypertext"}},
        {"xml",           {QStringLiteral("XML"),                       "xml"}},
        {"makefile",      {QStringLiteral("Makefile"),                  "makefile"}},
        {"pascal",        {QStringLiteral("Pascal"),                    "pascal"}},
        {"batch",         {QStringLiteral("Batch"),                     "batch"}},
        {"ini",           {QStringLiteral("ini"),                       "props"}},
        {"nfo",           {QStringLiteral("NFO"),                       "null"}},
        {"asp",           {QStringLiteral("ASP"),                       "hypertext"}},
        {"sql",           {QStringLiteral("SQL"),                       "sql"}},
        {"vb",            {QStringLiteral("Visual Basic"),              "vb"}},
        {"css",           {QStringLiteral("CSS"),                       "css"}},
        {"perl",          {QStringLiteral("Perl"),                      "perl"}},
        {"python",        {QStringLiteral("Python"),                    "python"}},
        {"lua",           {QStringLiteral("Lua"),                       "lua"}},
        {"tex",           {QStringLiteral("TeX"),                       "tex"}},
        {"fortran",       {QStringLiteral("Fortran free form"),         "fortran"}},
        {"bash",          {QStringLiteral("Shell"),                     "bash"}},
        {"actionscript",  {QStringLiteral("ActionScript"),              "cpp"}},
        {"nsis",          {QStringLiteral("NSIS"),                      "nsis"}},
        {"tcl",           {QStringLiteral("TCL"),                       "tcl"}},
        {"lisp",          {QStringLiteral("Lisp"),                      "lisp"}},
        {"scheme",        {QStringLiteral("Scheme"),                    "lisp"}},
        {"asm",           {QStringLiteral("Assembly"),                  "asm"}},
        {"diff",          {QStringLiteral("Diff"),                      "diff"}},
        {"props",         {QStringLiteral("Properties file"),           "props"}},
        {"postscript",    {QStringLiteral("PostScript"),                "ps"}},
        {"ruby",          {QStringLiteral("Ruby"),                      "ruby"}},
        {"smalltalk",     {QStringLiteral("Smalltalk"),                 "smalltalk"}},
        {"vhdl",          {QStringLiteral("VHDL"),                      "vhdl"}},
        {"kix",           {QStringLiteral("KiXtart"),                   "kix"}},
        {"autoit",        {QStringLiteral("AutoIt"),                    "au3"}},
        {"caml",          {QStringLiteral("CAML"),                      "caml"}},
        {"ada",           {QStringLiteral("Ada"),                       "ada"}},
        {"verilog",       {QStringLiteral("Verilog"),                   "verilog"}},
        {"matlab",        {QStringLiteral("MATLAB"),                    "matlab"}},
        {"haskell",       {QStringLiteral("Haskell"),                   "haskell"}},
        {"inno",          {QStringLiteral("Inno Setup"),                "inno"}},
        {"cmake",         {QStringLiteral("CMake"),                     "cmake"}},
        {"yaml",          {QStringLiteral("YAML"),                      "yaml"}},
        {"cobol",         {QStringLiteral("COBOL"),                     "COBOL"}},
        {"gui4cli",       {QStringLiteral("Gui4Cli"),                   "gui4cli"}},
        {"d",             {QStringLiteral("D"),                         "d"}},
        {"powershell",    {QStringLiteral("PowerShell"),                "powershell"}},
        {"r",             {QStringLiteral("R"),                         "r"}},
        {"jsp",           {QStringLiteral("JSP"),                       "hypertext"}},
        {"coffeescript",  {QStringLiteral("CoffeeScript"),              "coffeescript"}},
        {"json",          {QStringLiteral("JSON"),                      "json"}},
        {"javascript.js", {QStringLiteral("JavaScript"),                "cpp"}},
        {"fortran77",     {QStringLiteral("Fortran fixed form"),        "f77"}},
        {"baanc",         {QStringLiteral("BaanC"),                     "baan"}},
        {"srec",          {QStringLiteral("S-Record"),                  "srec"}},
        {"ihex",          {QStringLiteral("Intel HEX"),                 "ihex"}},
        {"tehex",         {QStringLiteral("Tektronix extended HEX"),    "tehex"}},
        {"swift",         {QStringLiteral("Swift"),                     "cpp"}},
        {"asn1",          {QStringLiteral("ASN.1"),                     "asn1"}},
        {"avs",           {QStringLiteral("AviSynth"),                  "avs"}},
        {"blitzbasic",    {QStringLiteral("BlitzBasic"),                "blitzbasic"}},
        {"purebasic",     {QStringLiteral("PureBasic"),                 "purebasic"}},
        {"freebasic",     {QStringLiteral("FreeBasic"),                 "freebasic"}},
        {"csound",        {QStringLiteral("Csound"),                    "csound"}},
        {"erlang",        {QStringLiteral("Erlang"),                    "erlang"}},
        {"escript",       {QStringLiteral("ESCRIPT"),                   "escript"}},
        {"forth",         {QStringLiteral("Forth"),                     "forth"}},
        {"latex",         {QStringLiteral("LaTeX"),                     "latex"}},
        {"mmixal",        {QStringLiteral("MMIXAL"),                    "mmixal"}},
        {"nim",           {QStringLiteral("Nim"),                       "nimrod"}},
        {"nncrontab",     {QStringLiteral("Nncrontab"),                 "nncrontab"}},
        {"oscript",       {QStringLiteral("OScript"),                   "oscript"}},
        {"rebol",         {QStringLiteral("REBOL"),                     "rebol"}},
        {"registry",      {QStringLiteral("Registry"),                  "registry"}},
        {"rust",          {QStringLiteral("Rust"),                      "rust"}},
        {"spice",         {QStringLiteral("Spice"),                     "spice"}},
        {"txt2tags",      {QStringLiteral("txt2tags"),                  "txt2tags"}},
        {"visualprolog",  {QStringLiteral("Visual Prolog"),             "visualprolog"}},
        {"typescript",    {QStringLiteral("TypeScript"),                "cpp"}},
        {"json5",         {QStringLiteral("JSON5"),                     "json"}},
        {"mssql",         {QStringLiteral("MS-SQL"),                    "mssql"}},
        {"gdscript",      {QStringLiteral("GDScript"),                  "gdscript"}},
        {"hollywood",     {QStringLiteral("Hollywood"),                 "hollywood"}},
        {"go",            {QStringLiteral("Go"),                        "cpp"}},
        {"raku",          {QStringLiteral("Raku"),                      "raku"}},
        {"toml",          {QStringLiteral("TOML"),                      "toml"}},
        {"sas",           {QStringLiteral("SAS"),                       "sas"}},
        // langs.model.xml has "markdown" as a separate entry; upstream's
        // _langNameInfoArray doesn't list it (its LangType lives elsewhere).
        // We add it manually here so users can pick it from the menu.
        {"markdown",      {QStringLiteral("Markdown"),                  "markdown"}},
    };
    return map;
}

// Module-level storage. Phase 5U.2: split into three states —
//   1. init()     — populates built-ins from langs.model.xml; UNSORTED.
//   2. appendUDL — zero-or-more UDL entries appended.
//   3. finalize() — sorts the registry; caches `g_plainText`.
// `g_registry` is reserved generously (`kRegistryReserve`) so appendUDL()
// never reallocates after init(); LanguageDef* pointers stay stable for
// the app's lifetime.
//
// UDL backing store: `g_udlStore` owns each UDL by unique_ptr so its
// address is stable across registry sorts/reloads. `LanguageDef::userDefined`
// in g_registry points into this vector.
constexpr std::size_t kRegistryReserve = 256;

std::vector<LanguageDef>            g_registry;
std::vector<std::unique_ptr<UDL>>   g_udlStore;
const LanguageDef*                  g_plainText  = nullptr;
bool                                g_initialised = false;
bool                                g_finalised  = false;

bool containsToken(const std::string& list, const QString& needle)
{
    if (list.empty() || needle.isEmpty()) return false;
    const QByteArray nBytes = needle.toLatin1();
    const char* n = nBytes.constData();
    const std::size_t nLen = std::strlen(n);

    const char* p = list.c_str();
    while (*p) {
        while (*p == ' ') ++p;
        const char* tokStart = p;
        while (*p && *p != ' ') ++p;
        const std::size_t tokLen = static_cast<std::size_t>(p - tokStart);
        if (tokLen == nLen && std::strncmp(tokStart, n, nLen) == 0) return true;
    }
    return false;
}

void doInit()
{
    if (g_initialised) return;

    // Reserve up-front so subsequent appendUDL() calls don't reallocate
    // (would invalidate the LanguageDef* pointers other code holds).
    g_registry.reserve(kRegistryReserve);

    QFile f(QStringLiteral(":/langs.model.xml"));
    if (!f.open(QIODevice::ReadOnly)) {
        // Resource missing — leave the registry empty so the app degrades to
        // plain text only. This shouldn't happen in a correctly-built binary.
        g_initialised = true;
        return;
    }
    const QByteArray data = f.readAll();
    f.close();

    pugi::xml_document doc;
    const pugi::xml_parse_result r = doc.load_buffer(data.constData(),
        static_cast<size_t>(data.size()));
    if (!r) {
        g_initialised = true;
        return;
    }

    const auto root = doc.child("NotepadPlus").child("Languages");
    const auto& hc = hardcodedMap();

    for (pugi::xml_node node : root.children("Language")) {
        const char* nameAttr = node.attribute("name").value();
        if (!nameAttr || !*nameAttr) continue;
        std::string name = nameAttr;

        const auto it = hc.find(name);
        if (it == hc.end()) continue;   // not in our supported set

        LanguageDef d;
        d.internalName = std::move(name);
        d.displayName  = it->second.displayName;
        d.lexilla      = it->second.lexilla;

        const char* extAttr = node.attribute("ext").value();
        d.extensions = extAttr ? extAttr : "";

        const char* clAttr = node.attribute("commentLine").value();
        const char* csAttr = node.attribute("commentStart").value();
        const char* ceAttr = node.attribute("commentEnd").value();
        if (clAttr) d.commentLine  = clAttr;
        if (csAttr) d.commentStart = csAttr;
        if (ceAttr) d.commentEnd   = ceAttr;

        // Collect all <Keywords>…</Keywords> children. The XML attribute
        // "name" (instre1, type1, substyle1, …) determines which Scintilla
        // keyword set the content goes into. We map them to indices 0-7
        // suitable for SCI_SETKEYWORDS:
        //   instre1 -> 0   instre2 -> 1
        //   type1   -> 2   type2   -> 3   type3 -> 4   type4 -> 5
        //   type5   -> 6   type6   -> 7
        // (substyle* entries are upstream-only — they go into Scintilla
        // sub-styles, not the primary keyword sets — so we skip them.)
        d.keywords.assign(8, std::string{});
        bool anySet = false;
        for (pugi::xml_node kw : node.children("Keywords")) {
            const char* kn = kw.attribute("name").value();
            if (!kn || !*kn) continue;
            int idx = -1;
            if      (std::strcmp(kn, "instre1") == 0) idx = 0;
            else if (std::strcmp(kn, "instre2") == 0) idx = 1;
            else if (std::strcmp(kn, "type1")   == 0) idx = 2;
            else if (std::strcmp(kn, "type2")   == 0) idx = 3;
            else if (std::strcmp(kn, "type3")   == 0) idx = 4;
            else if (std::strcmp(kn, "type4")   == 0) idx = 5;
            else if (std::strcmp(kn, "type5")   == 0) idx = 6;
            else if (std::strcmp(kn, "type6")   == 0) idx = 7;
            else continue;

            d.keywords[idx] = kw.text().get();
            anySet = true;
        }
        if (!anySet) d.keywords.clear();

        g_registry.push_back(std::move(d));
    }

    // "Plain text" is rarely shipped in langs.model.xml's <Language> list as
    // anything more than `<Language name="normal" ext="txt"/>`, so the loop
    // above adds it. But guarantee the entry exists even if the XML is
    // somehow missing it.
    if (std::none_of(g_registry.begin(), g_registry.end(),
                     [](const LanguageDef& d){
                         return d.internalName == "normal";
                     })) {
        LanguageDef d;
        d.internalName = "normal";
        d.displayName  = QStringLiteral("Plain text");
        d.lexilla      = "null";
        d.extensions   = "txt";
        g_registry.push_back(std::move(d));
    }

    g_initialised = true;
}

void doFinalize()
{
    if (!g_initialised) doInit();
    if (g_finalised) return;

    // Sort by displayName for the Language menu (case-insensitive).
    std::sort(g_registry.begin(), g_registry.end(),
              [](const LanguageDef& a, const LanguageDef& b) {
                  return QString::localeAwareCompare(a.displayName, b.displayName) < 0;
              });

    // Cache the plain-text pointer post-sort.
    g_plainText = nullptr;
    for (const auto& d : g_registry) {
        if (d.internalName == "normal") { g_plainText = &d; break; }
    }

    g_finalised = true;
}

} // namespace

namespace Languages {

void init() { doInit(); }

void appendUDL(const UDL& udl)
{
    if (!g_initialised) doInit();
    // appendUDL is allowed only during the registration window between
    // init() and finalize(). Once finalize() has sorted g_registry, the
    // expectation is that pointers held by buffers and menus stay stable
    // until reloadUDLs() (5U.3+) explicitly tears them down.
    if (g_finalised) return;

    if (g_registry.size() >= kRegistryReserve) return;   // out of pre-reserved slots

    // Stable backing store for the UDL itself.
    g_udlStore.push_back(std::make_unique<UDL>(udl));
    const UDL* udlPtr = g_udlStore.back().get();

    LanguageDef d;
    const QByteArray nameUtf8 = udl.name.toUtf8();
    d.internalName = std::string("udl:") + nameUtf8.constData();
    d.displayName  = udl.name;
    d.lexilla      = "user";
    d.extensions   = udl.ext.toLower().toUtf8().constData();
    d.userDefined  = udlPtr;
    g_registry.push_back(std::move(d));
}

void finalize() { doFinalize(); }

void reloadUDLs()
{
    if (!g_initialised) doInit();

    // Drop every UDL entry from the registry — keep built-ins intact.
    g_registry.erase(
        std::remove_if(g_registry.begin(), g_registry.end(),
                       [](const LanguageDef& d) {
                           return d.userDefined != nullptr;
                       }),
        g_registry.end());

    // Free the old UDL backing store. Any LanguageDef* that pointed at a
    // UDL entry is now stale — the caller must re-resolve via
    // `findByInternalName("udl:<name>")` after this returns.
    g_udlStore.clear();

    // Mark unfinalised so doFinalize() runs the sort + plain-text cache
    // again after the appendUDL() loop below.
    g_finalised = false;
    g_plainText = nullptr;

    const QVector<UDL> udls = UserDefineLang::loadAll();
    for (const UDL& udl : udls) appendUDL(udl);

    doFinalize();
}

const LanguageDef* all()
{
    doFinalize();
    return g_registry.data();
}

int count()
{
    doFinalize();
    return static_cast<int>(g_registry.size());
}

const LanguageDef* plainText()
{
    doFinalize();
    return g_plainText;
}

const LanguageDef* findByInternalName(const char* name)
{
    if (!name) return nullptr;
    doFinalize();
    for (const auto& d : g_registry) {
        if (d.internalName == name) return &d;
    }
    return nullptr;
}

const LanguageDef* findByExtension(const QString& path)
{
    doFinalize();
    if (path.isEmpty()) return plainText();

    const QFileInfo fi(path);
    const QString basename = fi.fileName();
    const QString suffix   = fi.suffix().toLower();

    // Special-case basename matches first — files with no extension or
    // non-extension naming.
    const QString lowerBase = basename.toLower();
    if (lowerBase == QLatin1String("makefile")
        || lowerBase == QLatin1String("gnumakefile")) {
        return findByInternalName("makefile");
    }
    if (lowerBase == QLatin1String("cmakelists.txt")) {
        return findByInternalName("cmake");
    }
    if (lowerBase == QLatin1String("dockerfile")
        || lowerBase.startsWith(QLatin1String("dockerfile."))) {
        return findByInternalName("bash");
    }

    if (suffix.isEmpty()) return plainText();

    // Phase 5U.2 — UDL entries claim extensions ahead of built-ins, mirroring
    // upstream NppParameters::getLangFromExt's "user override wins" semantics
    // (Parameters.cpp:3922-3946). A user dropping a UDL XML that targets ".ml"
    // overrides the built-in CAML lexer for that extension.
    for (const auto& d : g_registry) {
        if (d.userDefined && containsToken(d.extensions, suffix)) return &d;
    }
    for (const auto& d : g_registry) {
        if (containsToken(d.extensions, suffix)) return &d;
    }
    return plainText();
}

} // namespace Languages
