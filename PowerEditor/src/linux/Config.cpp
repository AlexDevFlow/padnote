#include "Config.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QStandardPaths>

#include <cstring>

#include "pugixml.hpp"

#include "MacroManager.h"   // Phase 5Z — round-trip <Macro> entries

namespace {

constexpr int kDefaultRecentMax = 10;

struct State {
    QString     theme        = QStringLiteral("Light");
    QRect       geometry;     // null/empty until set
    bool        maximized    = false;
    QStringList recentFiles;
    int         recentMax    = kDefaultRecentMax;
    QHash<QString, QString> styleOverrides;   // Phase 5P
    QHash<QString, QString> shortcutOverrides; // Phase 5Q
    bool smartHighlight  = false;             // Phase 5X
    bool bracketAutoPair = true;              // Phase 5X
    bool smartHighlightMatchCase = true;      // Phase 9c.2 (preserves pre-9c behaviour)
    bool smartHighlightWholeWord = true;      // Phase 9c.2
    bool autocompleteAutoTrigger = false;     // Phase 9i — opt-in
    int  autocompleteTriggerChars = 3;        // Phase 9i
    QString     uiLanguage;                   // Phase 7d — empty = English
    bool        documentMapVisible = false;   // Phase 3c.1
    bool        functionListVisible = false;  // Phase 3c.2
    bool        fileBrowserVisible  = false;   // Phase 3c.3
    QString     fileBrowserRoot;               // Phase 3c.3 — empty = $HOME
    // Phase 9q — three project panels. Index 0 = panel 1 (legacy
    // single-panel state), index 1 = panel 2, index 2 = panel 3.
    bool        projectPanelVisible[3] = {false, false, false};
    QString     lastWorkspacePath[3];          // empty = none
    bool        showToolBar     = true;        // Phase 5N.2
    bool        showStatusBar   = true;        // Phase 5N.2
    bool        tabsClosable    = true;        // Phase 5N.2
    int         caretWidth      = 1;           // Phase 5N.3
    int         caretBlinkMs    = 500;         // Phase 5N.3
    QString     defaultEncodingName = QStringLiteral("UTF-8");  // Phase 5N.4
    bool        defaultEncodingHasBom = false; // Phase 5N.4
    int         defaultEolMode = 2;            // Phase 5N.4 — Lf
    QString     defaultLanguage;               // Phase 5N.4 — empty = Plain text
    QString     defaultOpenDirectory;          // Phase 5N.5 — empty = remember-last
    int         defaultTabWidth = 4;           // Phase 5N.8
    bool        defaultUseTabs  = false;       // Phase 5N.8 — modern default
    QString     printHeader;                   // Phase 5N.10 — empty = no header
    QString     printFooter;                   // Phase 5N.10 — empty = no footer
    bool        syntaxHighlightedPrint = true; // Phase 5T — colour/font print
    int         printMagnification = 0;        // Phase 5T-polish — -10..+10
    int         backupIntervalSec = 10;        // Phase 5N.11
    bool        backupEnabled     = true;      // Phase 5N.11
    bool        fileWatcherEnabled = true;     // Phase 5N.11
    QStringList findHistory;                   // Phase 9b.3
    QStringList replaceHistory;                // Phase 9b.3
    QString fifExcludeDirs = QStringLiteral(".git .svn .hg node_modules build");  // Phase 9g
    QString fifExcludeFiles;                                                       // Phase 9g
    QHash<QString, QPair<int, bool>> languageIndentOverrides;   // Phase 9f
    bool        verticalEdgeEnabled = false;   // Phase 9b.4
    int         verticalEdgeColumn  = 80;      // Phase 9b.4
    QString     verticalEdgeColor   = QStringLiteral("#C0C0C0");   // Phase 9b.4
    bool        loaded       = false;
};

State& st()
{
    static State s;
    return s;
}

QString configDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
}

// Phase 12 — sentinel file holding the user's chosen cloud-config
// override path. Lives in the LOCAL default dir, never in the override
// dir, so a machine that wants local-only config doesn't get pulled
// into the override by a sync client carrying the sentinel along.
QString cloudSentinelPath()
{
    return configDir() + QStringLiteral("/cloud.path");
}

QString resolveConfigDir()
{
    // Read sentinel; if it points at a directory we can write to, use
    // it. Otherwise fall back to the default. Resolved on every call —
    // the sentinel is small (~100 bytes) and cached by the kernel
    // page cache, so the cost is one stat per config touch.
    QFile s(cloudSentinelPath());
    if (s.exists() && s.open(QIODevice::ReadOnly)) {
        const QByteArray bytes = s.readAll().trimmed();
        s.close();
        if (!bytes.isEmpty()) {
            const QString dir = QString::fromUtf8(bytes);
            const QFileInfo fi(dir);
            if (fi.isDir() || QDir().mkpath(dir)) {
                return dir;
            }
        }
    }
    return configDir();
}

QString configFile()
{
    // Phase 12 — route through the resolver so cloud sync override
    // takes effect on every read/write.
    return resolveConfigDir() + QStringLiteral("/config.xml");
}

void readFromXml(const QByteArray& bytes)
{
    pugi::xml_document doc;
    const pugi::xml_parse_result r = doc.load_buffer(bytes.constData(),
        static_cast<size_t>(bytes.size()));
    if (!r) return;

    const pugi::xml_node root = doc.child("NotepadPlus");
    if (!root) return;

    // <History nbMaxFile="N"><File filename="..."/></History>
    if (pugi::xml_node history = root.child("History")) {
        const int nbMax = history.attribute("nbMaxFile").as_int(kDefaultRecentMax);
        st().recentMax = nbMax > 0 ? nbMax : kDefaultRecentMax;
        st().recentFiles.clear();
        for (pugi::xml_node f : history.children("File")) {
            const char* fn = f.attribute("filename").value();
            if (fn && *fn) st().recentFiles.append(QString::fromUtf8(fn));
            if (st().recentFiles.size() >= st().recentMax) break;
        }
    }

    // <GUIConfig name="..."> entries.
    for (pugi::xml_node g : root.children("GUIConfig")) {
        const char* name = g.attribute("name").value();
        if (!name || !*name) continue;

        if (std::strcmp(name, "DarkMode") == 0) {
            const char* enable = g.attribute("enable").value();
            st().theme = (enable && std::strcmp(enable, "yes") == 0)
                         ? QStringLiteral("Dark") : QStringLiteral("Light");
        }
        else if (std::strcmp(name, "StyleOverride") == 0) {
            const char* k = g.attribute("key").value();
            const char* v = g.attribute("value").value();
            if (k && *k && v) {
                st().styleOverrides.insert(QString::fromUtf8(k),
                                           QString::fromUtf8(v));
            }
        }
        else if (std::strcmp(name, "ShortcutOverride") == 0) {
            const char* k = g.attribute("key").value();
            const char* v = g.attribute("value").value();
            if (k && *k && v) {
                st().shortcutOverrides.insert(QString::fromUtf8(k),
                                              QString::fromUtf8(v));
            }
        }
        else if (std::strcmp(name, "SmartEdit") == 0) {
            const char* sh = g.attribute("smartHighlight").value();
            const char* bp = g.attribute("bracketAutoPair").value();
            if (sh && *sh) st().smartHighlight  = (std::strcmp(sh, "yes") == 0);
            if (bp && *bp) st().bracketAutoPair = (std::strcmp(bp, "yes") == 0);
            // Phase 9c.2 — match-case + whole-word fine-tune for the
            // smart-highlight search. Default-true preserves pre-9c
            // behaviour when the attribute is absent.
            const char* mc = g.attribute("smartHighlightMatchCase").value();
            const char* ww = g.attribute("smartHighlightWholeWord").value();
            if (mc && *mc) st().smartHighlightMatchCase =
                (std::strcmp(mc, "yes") == 0);
            if (ww && *ww) st().smartHighlightWholeWord =
                (std::strcmp(ww, "yes") == 0);
            // Phase 9i — auto-trigger autocomplete after N identifier chars.
            const char* aa = g.attribute("autocompleteAutoTrigger").value();
            const int   ac = g.attribute("autocompleteTriggerChars").as_int(-1);
            if (aa && *aa) st().autocompleteAutoTrigger =
                (std::strcmp(aa, "yes") == 0);
            if (ac >= 2 && ac <= 6) st().autocompleteTriggerChars = ac;
        }
        else if (std::strcmp(name, "UiLanguage") == 0) {
            const char* v = g.attribute("value").value();
            if (v) st().uiLanguage = QString::fromUtf8(v);
        }
        else if (std::strcmp(name, "DocumentMap") == 0) {
            const char* v = g.attribute("visible").value();
            st().documentMapVisible = (v && std::strcmp(v, "yes") == 0);
        }
        else if (std::strcmp(name, "FunctionList") == 0) {
            const char* v = g.attribute("visible").value();
            st().functionListVisible = (v && std::strcmp(v, "yes") == 0);
        }
        else if (std::strcmp(name, "FileBrowser") == 0) {
            const char* v = g.attribute("visible").value();
            st().fileBrowserVisible = (v && std::strcmp(v, "yes") == 0);
            const char* r = g.attribute("root").value();
            if (r) st().fileBrowserRoot = QString::fromUtf8(r);
        }
        else if (std::strcmp(name, "ProjectPanel")  == 0
              || std::strcmp(name, "ProjectPanel2") == 0
              || std::strcmp(name, "ProjectPanel3") == 0)
        {
            // Phase 9q — Panel 1 keeps the legacy "ProjectPanel" key
            // (no suffix) for backward compat with 3c.4 configs.
            const int idx = (std::strcmp(name, "ProjectPanel2") == 0) ? 1
                          : (std::strcmp(name, "ProjectPanel3") == 0) ? 2
                          : 0;
            const char* v = g.attribute("visible").value();
            st().projectPanelVisible[idx] = (v && std::strcmp(v, "yes") == 0);
            const char* w = g.attribute("lastWorkspace").value();
            if (w && *w) st().lastWorkspacePath[idx] = QString::fromUtf8(w);
        }
        else if (std::strcmp(name, "UiVisibility") == 0) {
            const char* tb = g.attribute("toolbar").value();
            const char* sb = g.attribute("statusbar").value();
            const char* tc = g.attribute("tabsClosable").value();
            if (tb && *tb) st().showToolBar   = (std::strcmp(tb, "yes") == 0);
            if (sb && *sb) st().showStatusBar = (std::strcmp(sb, "yes") == 0);
            if (tc && *tc) st().tabsClosable  = (std::strcmp(tc, "yes") == 0);
        }
        else if (std::strcmp(name, "Editing") == 0) {
            const int cw = g.attribute("caretWidth").as_int(-1);
            const int cb = g.attribute("caretBlink").as_int(-1);
            if (cw >= 1 && cw <= 3)    st().caretWidth   = cw;
            if (cb >= 0 && cb <= 2000) st().caretBlinkMs = cb;
        }
        else if (std::strcmp(name, "NewDocument") == 0) {
            const char* enc = g.attribute("encoding").value();
            if (enc && *enc) st().defaultEncodingName = QString::fromUtf8(enc);
            const char* bom = g.attribute("hasBom").value();
            if (bom && *bom) st().defaultEncodingHasBom =
                (std::strcmp(bom, "yes") == 0);
            const int eol = g.attribute("eol").as_int(-1);
            if (eol >= 0 && eol <= 2) st().defaultEolMode = eol;
            const char* lang = g.attribute("language").value();
            if (lang) st().defaultLanguage = QString::fromUtf8(lang);
        }
        else if (std::strcmp(name, "DefaultDirectory") == 0) {
            const char* p = g.attribute("path").value();
            if (p) st().defaultOpenDirectory = QString::fromUtf8(p);
        }
        else if (std::strcmp(name, "LanguageIndent") == 0) {
            const int tw = g.attribute("tabWidth").as_int(-1);
            if (tw >= 1 && tw <= 16) st().defaultTabWidth = tw;
            const char* ut = g.attribute("useTabs").value();
            if (ut && *ut) st().defaultUseTabs = (std::strcmp(ut, "yes") == 0);
        }
        else if (std::strcmp(name, "Print") == 0) {
            const char* h = g.attribute("header").value();
            if (h) st().printHeader = QString::fromUtf8(h);
            const char* f = g.attribute("footer").value();
            if (f) st().printFooter = QString::fromUtf8(f);
            const char* sh = g.attribute("syntaxHighlight").value();
            if (sh && *sh) st().syntaxHighlightedPrint =
                (std::strcmp(sh, "yes") == 0);
            const auto mag = g.attribute("magnification");
            if (mag) {
                const int v = mag.as_int(0);
                st().printMagnification = qBound(-10, v, 10);
            }
        }
        else if (std::strcmp(name, "Backup") == 0) {
            const int s = g.attribute("intervalSec").as_int(-1);
            if (s >= 1 && s <= 300) st().backupIntervalSec = s;
            const char* e = g.attribute("enabled").value();
            if (e && *e) st().backupEnabled = (std::strcmp(e, "yes") == 0);
        }
        else if (std::strcmp(name, "FileWatcher") == 0) {
            const char* e = g.attribute("enabled").value();
            if (e && *e) st().fileWatcherEnabled = (std::strcmp(e, "yes") == 0);
        }
        else if (std::strcmp(name, "FindInFiles") == 0) {
            const char* d = g.attribute("excludeDirs").value();
            const char* f = g.attribute("excludeFiles").value();
            if (d) st().fifExcludeDirs  = QString::fromUtf8(d);
            if (f) st().fifExcludeFiles = QString::fromUtf8(f);
        }
        else if (std::strcmp(name, "FindHistory") == 0) {
            const char* v = g.attribute("entries").value();
            if (v && *v) {
                st().findHistory = QString::fromUtf8(v).split(QChar('\n'),
                    Qt::SkipEmptyParts);
                while (st().findHistory.size() > 10)
                    st().findHistory.removeLast();
            }
        }
        else if (std::strcmp(name, "ReplaceHistory") == 0) {
            const char* v = g.attribute("entries").value();
            if (v && *v) {
                st().replaceHistory = QString::fromUtf8(v).split(QChar('\n'),
                    Qt::SkipEmptyParts);
                while (st().replaceHistory.size() > 10)
                    st().replaceHistory.removeLast();
            }
        }
        else if (std::strcmp(name, "LanguageIndentOverride") == 0) {
            // Phase 9f — multiple instances; one per language.
            const char* langC = g.attribute("lang").value();
            if (!langC || !*langC) continue;
            const int tw = g.attribute("tabWidth").as_int(-1);
            const char* ut = g.attribute("useTabs").value();
            if (tw < 1 || tw > 16) continue;
            const bool useTabs = (ut && std::strcmp(ut, "yes") == 0);
            st().languageIndentOverrides.insert(
                QString::fromUtf8(langC),
                qMakePair(tw, useTabs));
        }
        else if (std::strcmp(name, "VerticalEdge") == 0) {
            const char* en = g.attribute("enabled").value();
            if (en && *en) st().verticalEdgeEnabled =
                (std::strcmp(en, "yes") == 0);
            const int col = g.attribute("column").as_int(-1);
            if (col >= 1 && col <= 200) st().verticalEdgeColumn = col;
            const char* c = g.attribute("color").value();
            if (c && *c) st().verticalEdgeColor = QString::fromUtf8(c);
        }
        else if (std::strcmp(name, "MainWindow") == 0) {
            const int x = g.attribute("x").as_int(-1);
            const int y = g.attribute("y").as_int(-1);
            const int w = g.attribute("w").as_int(-1);
            const int h = g.attribute("h").as_int(-1);
            if (x >= 0 && y >= 0 && w > 0 && h > 0) {
                st().geometry = QRect(x, y, w, h);
            }
            const char* mx = g.attribute("maximized").value();
            st().maximized = (mx && std::strcmp(mx, "yes") == 0);
        }
    }

    // Phase 5Z — saved <Macro name="..."> entries (siblings of GUIConfig).
    MacroManager::instance().loadFromXml(root);
}

void writeToXml(QByteArray* outBytes)
{
    pugi::xml_document doc;

    // XML declaration.
    pugi::xml_node decl = doc.append_child(pugi::node_declaration);
    decl.append_attribute("version") = "1.0";
    decl.append_attribute("encoding") = "UTF-8";

    pugi::xml_node root = doc.append_child("NotepadPlus");

    // <History> — upstream-compatible.
    pugi::xml_node history = root.append_child("History");
    history.append_attribute("nbMaxFile") = st().recentMax;
    for (const QString& path : st().recentFiles) {
        pugi::xml_node f = history.append_child("File");
        const QByteArray utf8 = path.toUtf8();
        f.append_attribute("filename") = utf8.constData();
    }

    // <GUIConfig name="DarkMode"> — upstream-compatible (subset of attrs).
    {
        pugi::xml_node g = root.append_child("GUIConfig");
        g.append_attribute("name")   = "DarkMode";
        g.append_attribute("enable") = (st().theme == QStringLiteral("Dark"))
                                        ? "yes" : "no";
    }

    // <GUIConfig name="MainWindow"> — linux-port-specific.
    {
        pugi::xml_node g = root.append_child("GUIConfig");
        g.append_attribute("name")      = "MainWindow";
        g.append_attribute("x")         = st().geometry.x();
        g.append_attribute("y")         = st().geometry.y();
        g.append_attribute("w")         = st().geometry.width();
        g.append_attribute("h")         = st().geometry.height();
        g.append_attribute("maximized") = st().maximized ? "yes" : "no";
    }

    // <GUIConfig name="StyleOverride" key="..." value="..."/> — Phase 5P.
    // Each override gets its own GUIConfig element so the schema stays
    // flat and round-trips through pugi without nested-element games.
    for (auto it = st().styleOverrides.constBegin();
         it != st().styleOverrides.constEnd(); ++it) {
        pugi::xml_node g = root.append_child("GUIConfig");
        g.append_attribute("name")  = "StyleOverride";
        const QByteArray k = it.key().toUtf8();
        const QByteArray v = it.value().toUtf8();
        g.append_attribute("key")   = k.constData();
        g.append_attribute("value") = v.constData();
    }

    // <GUIConfig name="ShortcutOverride" key="..." value="..."/> — Phase 5Q.
    // Same flat schema as StyleOverride. Key = menu path ("File / New"),
    // value = QKeySequence::toString(PortableText) form.
    for (auto it = st().shortcutOverrides.constBegin();
         it != st().shortcutOverrides.constEnd(); ++it) {
        pugi::xml_node g = root.append_child("GUIConfig");
        g.append_attribute("name")  = "ShortcutOverride";
        const QByteArray k = it.key().toUtf8();
        const QByteArray v = it.value().toUtf8();
        g.append_attribute("key")   = k.constData();
        g.append_attribute("value") = v.constData();
    }

    // <GUIConfig name="SmartEdit" .../> — Phase 5X (+ 9c.2 fine-tune).
    {
        pugi::xml_node g = root.append_child("GUIConfig");
        g.append_attribute("name")            = "SmartEdit";
        g.append_attribute("smartHighlight")  = st().smartHighlight  ? "yes" : "no";
        g.append_attribute("bracketAutoPair") = st().bracketAutoPair ? "yes" : "no";
        g.append_attribute("smartHighlightMatchCase") =
            st().smartHighlightMatchCase ? "yes" : "no";
        g.append_attribute("smartHighlightWholeWord") =
            st().smartHighlightWholeWord ? "yes" : "no";
        // Phase 9i.
        g.append_attribute("autocompleteAutoTrigger") =
            st().autocompleteAutoTrigger ? "yes" : "no";
        g.append_attribute("autocompleteTriggerChars") =
            st().autocompleteTriggerChars;
    }

    // <GUIConfig name="UiLanguage" value="..."/> — Phase 7d. Empty
    // omitted to keep config.xml tight on first run.
    if (!st().uiLanguage.isEmpty()) {
        pugi::xml_node g = root.append_child("GUIConfig");
        g.append_attribute("name") = "UiLanguage";
        const QByteArray v = st().uiLanguage.toUtf8();
        g.append_attribute("value") = v.constData();
    }

    // <GUIConfig name="DocumentMap" visible="yes|no"/> — Phase 3c.1.
    // Default is hidden; only persist when the user has shown it.
    if (st().documentMapVisible) {
        pugi::xml_node g = root.append_child("GUIConfig");
        g.append_attribute("name") = "DocumentMap";
        g.append_attribute("visible") = "yes";
    }

    // <GUIConfig name="FunctionList" visible="yes|no"/> — Phase 3c.2.
    if (st().functionListVisible) {
        pugi::xml_node g = root.append_child("GUIConfig");
        g.append_attribute("name") = "FunctionList";
        g.append_attribute("visible") = "yes";
    }

    // <GUIConfig name="FileBrowser" visible="yes" root="/abs/path"/> —
    // Phase 3c.3. Visibility-or-root being non-default writes the element.
    if (st().fileBrowserVisible || !st().fileBrowserRoot.isEmpty()) {
        pugi::xml_node g = root.append_child("GUIConfig");
        g.append_attribute("name") = "FileBrowser";
        g.append_attribute("visible") = st().fileBrowserVisible ? "yes" : "no";
        if (!st().fileBrowserRoot.isEmpty()) {
            const QByteArray p = st().fileBrowserRoot.toUtf8();
            g.append_attribute("root") = p.constData();
        }
    }

    // <GUIConfig name="ProjectPanel"  …/> — Panel 1 (legacy 3c.4 key)
    // <GUIConfig name="ProjectPanel2" …/> — Panel 2 (Phase 9q)
    // <GUIConfig name="ProjectPanel3" …/> — Panel 3 (Phase 9q)
    // Each element written only when its panel is visible OR has a
    // lastWorkspace value, so first-run config.xml stays tight.
    static const char* const kProjectPanelKey[3] = {
        "ProjectPanel", "ProjectPanel2", "ProjectPanel3"
    };
    for (int i = 0; i < 3; ++i) {
        if (!st().projectPanelVisible[i]
         && st().lastWorkspacePath[i].isEmpty()) continue;
        pugi::xml_node g = root.append_child("GUIConfig");
        g.append_attribute("name") = kProjectPanelKey[i];
        g.append_attribute("visible") =
            st().projectPanelVisible[i] ? "yes" : "no";
        if (!st().lastWorkspacePath[i].isEmpty()) {
            const QByteArray p = st().lastWorkspacePath[i].toUtf8();
            g.append_attribute("lastWorkspace") = p.constData();
        }
    }

    // <GUIConfig name="UiVisibility" toolbar=… statusbar=… tabsClosable=…/>
    // — Phase 5N.2. All default to true; element written only when at least
    // one differs so first-run config.xml stays tight.
    if (!st().showToolBar || !st().showStatusBar || !st().tabsClosable) {
        pugi::xml_node g = root.append_child("GUIConfig");
        g.append_attribute("name")         = "UiVisibility";
        g.append_attribute("toolbar")      = st().showToolBar   ? "yes" : "no";
        g.append_attribute("statusbar")    = st().showStatusBar ? "yes" : "no";
        g.append_attribute("tabsClosable") = st().tabsClosable  ? "yes" : "no";
    }

    // <GUIConfig name="Editing" caretWidth="…" caretBlink="…"/> — Phase 5N.3.
    if (st().caretWidth != 1 || st().caretBlinkMs != 500) {
        pugi::xml_node g = root.append_child("GUIConfig");
        g.append_attribute("name")       = "Editing";
        g.append_attribute("caretWidth") = st().caretWidth;
        g.append_attribute("caretBlink") = st().caretBlinkMs;
    }

    // <GUIConfig name="NewDocument" encoding="…" hasBom="…" eol="…"
    //   language="…"/> — Phase 5N.4. Element written when ANY field
    // differs from defaults (UTF-8 / no-BOM / Lf / "").
    if (st().defaultEncodingName != QStringLiteral("UTF-8")
     || st().defaultEncodingHasBom
     || st().defaultEolMode != 2
     || !st().defaultLanguage.isEmpty()) {
        pugi::xml_node g = root.append_child("GUIConfig");
        g.append_attribute("name") = "NewDocument";
        const QByteArray enc = st().defaultEncodingName.toUtf8();
        g.append_attribute("encoding") = enc.constData();
        g.append_attribute("hasBom")   = st().defaultEncodingHasBom ? "yes" : "no";
        g.append_attribute("eol")      = st().defaultEolMode;
        if (!st().defaultLanguage.isEmpty()) {
            const QByteArray lang = st().defaultLanguage.toUtf8();
            g.append_attribute("language") = lang.constData();
        }
    }

    // <GUIConfig name="DefaultDirectory" path="/abs/path"/> — Phase 5N.5.
    // Element written only when set (empty = Qt's remember-last behaviour).
    if (!st().defaultOpenDirectory.isEmpty()) {
        pugi::xml_node g = root.append_child("GUIConfig");
        g.append_attribute("name") = "DefaultDirectory";
        const QByteArray p = st().defaultOpenDirectory.toUtf8();
        g.append_attribute("path") = p.constData();
    }

    // <GUIConfig name="LanguageIndent" tabWidth="4" useTabs="no"/> —
    // Phase 5N.8. Element written when either differs from defaults
    // (4 / false).
    if (st().defaultTabWidth != 4 || st().defaultUseTabs) {
        pugi::xml_node g = root.append_child("GUIConfig");
        g.append_attribute("name")     = "LanguageIndent";
        g.append_attribute("tabWidth") = st().defaultTabWidth;
        g.append_attribute("useTabs")  = st().defaultUseTabs ? "yes" : "no";
    }

    // <GUIConfig name="Print" header="…" footer="…" syntaxHighlight="…"
    //            magnification="N"/> — Phase 5N.10 + Phase 5T +
    // Phase 5T-polish. Written when any field differs from defaults
    // (header/footer empty, syntaxHighlight true, magnification 0).
    if (!st().printHeader.isEmpty() || !st().printFooter.isEmpty()
        || !st().syntaxHighlightedPrint
        || st().printMagnification != 0) {
        pugi::xml_node g = root.append_child("GUIConfig");
        g.append_attribute("name") = "Print";
        const QByteArray h = st().printHeader.toUtf8();
        const QByteArray f = st().printFooter.toUtf8();
        g.append_attribute("header") = h.constData();
        g.append_attribute("footer") = f.constData();
        g.append_attribute("syntaxHighlight") =
            st().syntaxHighlightedPrint ? "yes" : "no";
        g.append_attribute("magnification") = st().printMagnification;
    }

    // <GUIConfig name="Backup" intervalSec="N" enabled="yes|no"/> —
    // Phase 5N.11. Written when either differs from defaults (10 / true).
    if (st().backupIntervalSec != 10 || !st().backupEnabled) {
        pugi::xml_node g = root.append_child("GUIConfig");
        g.append_attribute("name")        = "Backup";
        g.append_attribute("intervalSec") = st().backupIntervalSec;
        g.append_attribute("enabled")     = st().backupEnabled ? "yes" : "no";
    }

    // <GUIConfig name="FileWatcher" enabled="yes|no"/> — Phase 5N.11.
    // Written only when disabled (default = on).
    if (!st().fileWatcherEnabled) {
        pugi::xml_node g = root.append_child("GUIConfig");
        g.append_attribute("name")    = "FileWatcher";
        g.append_attribute("enabled") = "no";
    }

    // <GUIConfig name="FindInFiles" excludeDirs="..." excludeFiles="..."/>
    // — Phase 9g. Written when either differs from default
    // (".git .svn .hg node_modules build" / "").
    if (st().fifExcludeDirs != QStringLiteral(".git .svn .hg node_modules build")
     || !st().fifExcludeFiles.isEmpty()) {
        pugi::xml_node g = root.append_child("GUIConfig");
        g.append_attribute("name") = "FindInFiles";
        const QByteArray d = st().fifExcludeDirs.toUtf8();
        const QByteArray f = st().fifExcludeFiles.toUtf8();
        g.append_attribute("excludeDirs")  = d.constData();
        g.append_attribute("excludeFiles") = f.constData();
    }

    // <GUIConfig name="FindHistory"    entries="e1\ne2\n..."/> — Phase 9b.3.
    // <GUIConfig name="ReplaceHistory" entries="e1\ne2\n..."/>
    // Written only when non-empty so first-run config.xml stays tight.
    if (!st().findHistory.isEmpty()) {
        pugi::xml_node g = root.append_child("GUIConfig");
        g.append_attribute("name") = "FindHistory";
        const QByteArray v = st().findHistory.join(QChar('\n')).toUtf8();
        g.append_attribute("entries") = v.constData();
    }
    if (!st().replaceHistory.isEmpty()) {
        pugi::xml_node g = root.append_child("GUIConfig");
        g.append_attribute("name") = "ReplaceHistory";
        const QByteArray v = st().replaceHistory.join(QChar('\n')).toUtf8();
        g.append_attribute("entries") = v.constData();
    }

    // <GUIConfig name="LanguageIndentOverride" lang="..." tabWidth="N"
    //   useTabs="yes|no"/> — Phase 9f. One element per language with an
    // override; absent for languages following the global default.
    {
        // Iterate in sorted-key order so config.xml is reproducible.
        QStringList keys = st().languageIndentOverrides.keys();
        keys.sort();
        for (const QString& key : keys) {
            const auto& v = st().languageIndentOverrides.value(key);
            pugi::xml_node g = root.append_child("GUIConfig");
            g.append_attribute("name") = "LanguageIndentOverride";
            const QByteArray k = key.toUtf8();
            g.append_attribute("lang")     = k.constData();
            g.append_attribute("tabWidth") = v.first;
            g.append_attribute("useTabs")  = v.second ? "yes" : "no";
        }
    }

    // <GUIConfig name="VerticalEdge" enabled=… column=… color=…/> — Phase 9b.4.
    // Written when ANY field differs from defaults (off / 80 / #C0C0C0).
    if (st().verticalEdgeEnabled
     || st().verticalEdgeColumn != 80
     || st().verticalEdgeColor != QStringLiteral("#C0C0C0")) {
        pugi::xml_node g = root.append_child("GUIConfig");
        g.append_attribute("name")    = "VerticalEdge";
        g.append_attribute("enabled") = st().verticalEdgeEnabled ? "yes" : "no";
        g.append_attribute("column")  = st().verticalEdgeColumn;
        const QByteArray c = st().verticalEdgeColor.toUtf8();
        g.append_attribute("color")   = c.constData();
    }

    // <Macro name="..."><Op .../>...</Macro> — Phase 5Z. Each saved
    // macro is a top-level sibling of <GUIConfig>; ops carry msg/wp/lp
    // (or lpStr base64-encoded for text-bearing messages like ReplaceSel).
    MacroManager::instance().writeToXml(root);

    struct Writer : pugi::xml_writer {
        QByteArray* out;
        void write(const void* data, size_t size) override {
            out->append(static_cast<const char*>(data),
                        static_cast<int>(size));
        }
    };
    Writer w;
    w.out = outBytes;
    doc.save(w, "    ");
}

} // namespace

namespace Config {

void load()
{
    if (st().loaded) return;
    st().loaded = true;

    QFile f(configFile());
    if (!f.open(QIODevice::ReadOnly)) return;       // first run — fine
    const QByteArray bytes = f.readAll();
    f.close();
    readFromXml(bytes);
}

void save()
{
    QDir().mkpath(configDir());

    QByteArray bytes;
    writeToXml(&bytes);

    // Write atomically: write to .tmp, rename. Prevents config corruption
    // if the process is killed mid-write.
    const QString target = configFile();
    const QString tmp    = target + QStringLiteral(".tmp");
    QFile f(tmp);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
    f.write(bytes);
    f.close();
    QFile::remove(target);                          // ignore errors (may not exist)
    QFile::rename(tmp, target);
}

QString theme()                              { load(); return st().theme; }
void    setTheme(const QString& mode)        { load(); st().theme = mode; save(); }

QRect   windowGeometry()                     { load(); return st().geometry; }
void    setWindowGeometry(const QRect& g)    { load(); st().geometry = g; }

bool    windowMaximized()                    { load(); return st().maximized; }
void    setWindowMaximized(bool m)           { load(); st().maximized = m; }

int     recentFilesMax()                     { load(); return st().recentMax; }

void setRecentFilesMax(int n)
{
    load();
    if (n < 0)  n = 0;
    if (n > 50) n = 50;
    st().recentMax = n;
    // If the new cap is smaller than the current list size, trim the tail
    // immediately so the persisted list matches the cap.
    while (st().recentFiles.size() > st().recentMax) {
        st().recentFiles.removeLast();
    }
}

QStringList recentFiles()                    { load(); return st().recentFiles; }

void addRecentFile(const QString& path)
{
    if (path.isEmpty()) return;
    load();
    // Canonicalise to absolute so duplicate-detection works across cwd's.
    const QString canonical = QFileInfo(path).absoluteFilePath();
    if (canonical.isEmpty()) return;
    st().recentFiles.removeAll(canonical);
    st().recentFiles.prepend(canonical);
    while (st().recentFiles.size() > st().recentMax) {
        st().recentFiles.removeLast();
    }
    save();
}

void clearRecentFiles()
{
    load();
    st().recentFiles.clear();
    save();
}

// ---- Style overrides (Phase 5P) ----------------------------------------

QString styleOverride(const QString& key)
{
    load();
    return st().styleOverrides.value(key);
}

void setStyleOverride(const QString& key, const QString& value)
{
    load();
    st().styleOverrides.insert(key, value);
    // No autosave here — caller batches a Config::save() after all the
    // setStyleOverride calls so we don't write the file 6 times for one
    // dialog session.
}

void clearStyleOverrides()
{
    load();
    st().styleOverrides.clear();
    save();
}

QStringList styleOverrideKeys()
{
    load();
    return st().styleOverrides.keys();
}

// ---- Per-language style overrides (Phase 8a) ----------------------------

namespace {
QString perLangKey(const QString& internalName, int styleID, const QString& attr)
{
    return QStringLiteral("lang:") + internalName + QChar(':')
         + QString::number(styleID) + QChar(':') + attr;
}
} // namespace

QString perLanguageStyleOverride(const QString& internalName, int styleID,
                                 const QString& attr)
{
    return styleOverride(perLangKey(internalName, styleID, attr));
}

void setPerLanguageStyleOverride(const QString& internalName, int styleID,
                                 const QString& attr, const QString& value)
{
    setStyleOverride(perLangKey(internalName, styleID, attr), value);
}

void clearPerLanguageOverridesFor(const QString& internalName)
{
    load();
    const QString prefix = QStringLiteral("lang:") + internalName + QChar(':');
    auto& hash = st().styleOverrides;
    for (auto it = hash.begin(); it != hash.end(); ) {
        if (it.key().startsWith(prefix)) it = hash.erase(it);
        else                             ++it;
    }
    // No autosave — caller batches Config::save() at end of dialog session.
}

// ---- Shortcut overrides (Phase 5Q) -------------------------------------

QString shortcutOverride(const QString& path)
{
    load();
    return st().shortcutOverrides.value(path);
}

void setShortcutOverride(const QString& path, const QString& value)
{
    load();
    if (value.isEmpty()) {
        // Empty value clears the override — caller (ShortcutMapperDialog)
        // uses this when the user hits "Reset to default", to keep the
        // persisted set tight.
        st().shortcutOverrides.remove(path);
    } else {
        st().shortcutOverrides.insert(path, value);
    }
}

void clearShortcutOverrides()
{
    load();
    st().shortcutOverrides.clear();
    save();
}

// ---- Smart-edit toggles (Phase 5X) -------------------------------------

bool smartHighlightEnabled()        { load(); return st().smartHighlight; }
void setSmartHighlightEnabled(bool v) { load(); st().smartHighlight = v; }

bool bracketAutoPairEnabled()        { load(); return st().bracketAutoPair; }
void setBracketAutoPairEnabled(bool v) { load(); st().bracketAutoPair = v; }

bool smartHighlightMatchCase()        { load(); return st().smartHighlightMatchCase; }
void setSmartHighlightMatchCase(bool v) { load(); st().smartHighlightMatchCase = v; }

bool smartHighlightWholeWord()        { load(); return st().smartHighlightWholeWord; }
void setSmartHighlightWholeWord(bool v) { load(); st().smartHighlightWholeWord = v; }

// ---- Auto-completion auto-trigger (Phase 9i) -----------------------

bool autocompleteAutoTrigger()        { load(); return st().autocompleteAutoTrigger; }
void setAutocompleteAutoTrigger(bool v) { load(); st().autocompleteAutoTrigger = v; }

int  autocompleteTriggerChars()       { load(); return st().autocompleteTriggerChars; }
void setAutocompleteTriggerChars(int n)
{
    load();
    if (n < 2) n = 2;
    if (n > 6) n = 6;
    st().autocompleteTriggerChars = n;
}

// ---- UI language (Phase 7d) --------------------------------------------

QString uiLanguage() { load(); return st().uiLanguage; }

void setUiLanguage(const QString& displayName)
{
    load();
    if (st().uiLanguage == displayName) return;
    st().uiLanguage = displayName;
    save();
}

// ---- Document Map dock (Phase 3c.1) ------------------------------------

bool documentMapVisible() { load(); return st().documentMapVisible; }

void setDocumentMapVisible(bool v)
{
    load();
    st().documentMapVisible = v;
}

// ---- Function List dock (Phase 3c.2) ----------------------------------

bool functionListVisible() { load(); return st().functionListVisible; }

void setFunctionListVisible(bool v)
{
    load();
    st().functionListVisible = v;
}

// ---- File Browser dock (Phase 3c.3) ------------------------------------

bool fileBrowserVisible() { load(); return st().fileBrowserVisible; }

void setFileBrowserVisible(bool v)
{
    load();
    st().fileBrowserVisible = v;
}

QString fileBrowserRoot() { load(); return st().fileBrowserRoot; }

void setFileBrowserRoot(const QString& path)
{
    load();
    st().fileBrowserRoot = path;
}

// ---- Project Panel dock (Phase 3c.4 / 9q) -------------------------------

namespace {
// Clamp an arbitrary input to {0, 1, 2}. Public callers pass 1/2/3
// (1-based, matches the user-visible "Project Panel 1/2/3" naming);
// internally we index into the 3-element state array. Out-of-range
// inputs map to panel 1 silently (defensive — callers shouldn't
// pass bad indices).
int panelIdx(int oneBased) {
    if (oneBased < 1 || oneBased > 3) return 0;
    return oneBased - 1;
}
} // namespace

bool projectPanelVisible(int n) {
    load();
    return st().projectPanelVisible[panelIdx(n)];
}
void setProjectPanelVisible(int n, bool v) {
    load();
    st().projectPanelVisible[panelIdx(n)] = v;
}
QString lastWorkspacePath(int n) {
    load();
    return st().lastWorkspacePath[panelIdx(n)];
}
void setLastWorkspacePath(int n, const QString& path) {
    load();
    st().lastWorkspacePath[panelIdx(n)] = path;
}

// Legacy single-panel aliases (panel 1).
bool projectPanelVisible() { return projectPanelVisible(1); }
void setProjectPanelVisible(bool v) { setProjectPanelVisible(1, v); }
QString lastWorkspacePath() { return lastWorkspacePath(1); }
void setLastWorkspacePath(const QString& path) { setLastWorkspacePath(1, path); }

// ---- UI visibility (Phase 5N.2) ----------------------------------------

bool showToolBar()    { load(); return st().showToolBar; }
void setShowToolBar(bool v)    { load(); st().showToolBar = v; }

bool showStatusBar()  { load(); return st().showStatusBar; }
void setShowStatusBar(bool v)  { load(); st().showStatusBar = v; }

bool tabsClosable()   { load(); return st().tabsClosable; }
void setTabsClosable(bool v)   { load(); st().tabsClosable = v; }

// ---- Editing prefs (Phase 5N.3) ---------------------------------------

int  caretWidth()             { load(); return st().caretWidth; }
void setCaretWidth(int px)
{
    load();
    if (px < 1) px = 1;
    if (px > 3) px = 3;
    st().caretWidth = px;
}

int  caretBlinkMs()           { load(); return st().caretBlinkMs; }
void setCaretBlinkMs(int ms)
{
    load();
    if (ms < 0)    ms = 0;
    if (ms > 2000) ms = 2000;
    st().caretBlinkMs = ms;
}

// ---- New Document defaults (Phase 5N.4) -------------------------------

QString defaultEncodingName() { load(); return st().defaultEncodingName; }
bool    defaultEncodingHasBom() { load(); return st().defaultEncodingHasBom; }
int     defaultEolMode()      { load(); return st().defaultEolMode; }
QString defaultLanguage()     { load(); return st().defaultLanguage; }

void setDefaultEncoding(const QString& name, bool hasBom)
{
    load();
    if (!name.isEmpty()) st().defaultEncodingName = name;
    st().defaultEncodingHasBom = hasBom;
}

void setDefaultEolMode(int mode)
{
    load();
    if (mode < 0) mode = 0;
    if (mode > 2) mode = 2;
    st().defaultEolMode = mode;
}

void setDefaultLanguage(const QString& internalName)
{
    load();
    st().defaultLanguage = internalName;
}

// ---- Default open directory (Phase 5N.5) -----------------------------

QString defaultOpenDirectory() { load(); return st().defaultOpenDirectory; }

void setDefaultOpenDirectory(const QString& path)
{
    load();
    st().defaultOpenDirectory = path;
}

// ---- Language indent defaults (Phase 5N.8) ----------------------------

int  defaultTabWidth() { load(); return st().defaultTabWidth; }
void setDefaultTabWidth(int n)
{
    load();
    if (n < 1)  n = 1;
    if (n > 16) n = 16;
    st().defaultTabWidth = n;
}

bool defaultUseTabs() { load(); return st().defaultUseTabs; }
void setDefaultUseTabs(bool v)
{
    load();
    st().defaultUseTabs = v;
}

// ---- Print header/footer (Phase 5N.10) -------------------------------

QString printHeader() { load(); return st().printHeader; }
void    setPrintHeader(const QString& s) { load(); st().printHeader = s; }

QString printFooter() { load(); return st().printFooter; }
void    setPrintFooter(const QString& s) { load(); st().printFooter = s; }

bool    syntaxHighlightedPrint() { load(); return st().syntaxHighlightedPrint; }
void    setSyntaxHighlightedPrint(bool v) { load(); st().syntaxHighlightedPrint = v; }

int     printMagnification() { load(); return st().printMagnification; }
void    setPrintMagnification(int n) { load();
    st().printMagnification = qBound(-10, n, 10);
}

// ---- Backup snapshot interval (Phase 5N.11) --------------------------

int  backupIntervalSec() { load(); return st().backupIntervalSec; }
void setBackupIntervalSec(int n)
{
    load();
    if (n < 1)   n = 1;
    if (n > 300) n = 300;
    st().backupIntervalSec = n;
}

bool backupEnabled() { load(); return st().backupEnabled; }
void setBackupEnabled(bool v) { load(); st().backupEnabled = v; }

// ---- File watcher (Phase 5N.11) --------------------------------------

bool fileWatcherEnabled() { load(); return st().fileWatcherEnabled; }
void setFileWatcherEnabled(bool v) { load(); st().fileWatcherEnabled = v; }

// ---- Find / Replace history (Phase 9b.3) ----------------------------

QStringList findHistory()    { load(); return st().findHistory; }
QStringList replaceHistory() { load(); return st().replaceHistory; }

// ---- Find in Files exclusions (Phase 9g) ----------------------------

QString findInFilesExcludeDirs()  { load(); return st().fifExcludeDirs; }
void    setFindInFilesExcludeDirs(const QString& s)
{
    load();
    st().fifExcludeDirs = s;
}

QString findInFilesExcludeFiles() { load(); return st().fifExcludeFiles; }
void    setFindInFilesExcludeFiles(const QString& s)
{
    load();
    st().fifExcludeFiles = s;
}

void setFindHistory(const QStringList& entries)
{
    load();
    QStringList trimmed;
    trimmed.reserve(qMin(10, entries.size()));
    for (const QString& s : entries) {
        if (s.isEmpty() || trimmed.contains(s)) continue;
        trimmed.append(s);
        if (trimmed.size() >= 10) break;
    }
    st().findHistory = trimmed;
}

void setReplaceHistory(const QStringList& entries)
{
    load();
    QStringList trimmed;
    trimmed.reserve(qMin(10, entries.size()));
    for (const QString& s : entries) {
        if (s.isEmpty() || trimmed.contains(s)) continue;
        trimmed.append(s);
        if (trimmed.size() >= 10) break;
    }
    st().replaceHistory = trimmed;
}

// ---- Per-language indent override (Phase 9f) -----------------------

bool hasLanguageIndentOverride(const QString& internalName)
{
    load();
    return st().languageIndentOverrides.contains(internalName);
}

LanguageIndent languageIndentOverride(const QString& internalName)
{
    load();
    const auto& it = st().languageIndentOverrides.constFind(internalName);
    if (it != st().languageIndentOverrides.constEnd()) {
        return LanguageIndent{ it.value().first, it.value().second };
    }
    // No override — caller normally checks hasLanguageIndentOverride first;
    // returning the global default keeps the "always returns something
    // sensible" contract for callers that just want the effective values.
    return LanguageIndent{ st().defaultTabWidth, st().defaultUseTabs };
}

void setLanguageIndentOverride(const QString& internalName,
                               int tabWidth, bool useTabs)
{
    load();
    if (internalName.isEmpty()) return;
    if (tabWidth < 1)  tabWidth = 1;
    if (tabWidth > 16) tabWidth = 16;
    st().languageIndentOverrides.insert(internalName,
        qMakePair(tabWidth, useTabs));
}

void clearLanguageIndentOverride(const QString& internalName)
{
    load();
    st().languageIndentOverrides.remove(internalName);
}

QStringList languageIndentOverrideKeys()
{
    load();
    QStringList keys = st().languageIndentOverrides.keys();
    keys.sort();
    return keys;
}

// ---- Vertical-edge marker (Phase 9b.4) ------------------------------

bool verticalEdgeEnabled()        { load(); return st().verticalEdgeEnabled; }
void setVerticalEdgeEnabled(bool v) { load(); st().verticalEdgeEnabled = v; }

int  verticalEdgeColumn()         { load(); return st().verticalEdgeColumn; }
void setVerticalEdgeColumn(int n)
{
    load();
    if (n < 1)   n = 1;
    if (n > 200) n = 200;
    st().verticalEdgeColumn = n;
}

QString verticalEdgeColor()       { load(); return st().verticalEdgeColor; }
void setVerticalEdgeColor(const QString& hex)
{
    load();
    // Loose validation: must look like #RRGGBB. If not, leave the existing
    // value alone — bad input from a hand-edited config.xml shouldn't wipe
    // a sane value.
    if (hex.size() == 7 && hex.startsWith(QChar('#'))) {
        st().verticalEdgeColor = hex;
    }
}

// Phase 12 — cloud sync API. Sentinel-file backed; see header for the
// design rationale.
QString cloudConfigDir()
{
    QFile s(cloudSentinelPath());
    if (!s.exists() || !s.open(QIODevice::ReadOnly)) return QString();
    const QByteArray bytes = s.readAll().trimmed();
    s.close();
    return QString::fromUtf8(bytes);
}

void setCloudConfigDir(const QString& dir)
{
    if (dir.isEmpty()) {
        clearCloudConfigDir();
        return;
    }
    QDir().mkpath(configDir());                  // ensure parent exists
    const QString tmp = cloudSentinelPath() + QStringLiteral(".tmp");
    QFile f(tmp);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
    f.write(dir.toUtf8());
    f.write("\n");
    f.close();
    QFile::remove(cloudSentinelPath());
    QFile::rename(tmp, cloudSentinelPath());
}

void clearCloudConfigDir()
{
    QFile::remove(cloudSentinelPath());
}

QString configFilePath()
{
    return configFile();
}

} // namespace Config
