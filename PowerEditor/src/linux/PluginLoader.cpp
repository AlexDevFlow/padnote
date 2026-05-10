// PluginLoader.cpp — see PluginLoader.h for the contract.

#include "PluginLoader.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QPointer>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTimer>

#include <dlfcn.h>

#include "Config.h"           // Phase 12 — cloud sync override
#include "MainWindow.h"
#include "EditorTabs.h"
#include "Buffer.h"
#include "ScintillaEditBase.h"

#include "../../../plugins/linuxplugin.h"

namespace {

QVector<PluginLoader::Plugin*>& store()
{
    static QVector<PluginLoader::Plugin*> v;
    return v;
}

// Static cmd-ID counter — each FuncItem gets a unique host-side ID
// across all loaded plugins. Phase 10b uses these as the QAction
// data() value to dispatch back to the right plugin->func.
int nextCmdId()
{
    static int n = 100000;     // start above any built-in menu IDs
    return n++;
}

// Scan one plugin subdir. Expected layout:
//   <plugins>/<name>/<name>.so
// or:
//   <plugins>/<name>/lib<name>.so
// Returns the .so path, or empty if no candidate file is present.
QString findSoIn(const QString& subdir)
{
    QFileInfo asIs(subdir + "/" + QFileInfo(subdir).fileName() + ".so");
    if (asIs.isFile()) return asIs.absoluteFilePath();
    QFileInfo libPrefix(subdir + "/lib" + QFileInfo(subdir).fileName() + ".so");
    if (libPrefix.isFile()) return libPrefix.absoluteFilePath();
    // Fallback: any .so file in the dir wins.
    QDir d(subdir);
    const QStringList sos = d.entryList({"*.so"}, QDir::Files);
    if (!sos.isEmpty()) return d.absoluteFilePath(sos.first());
    return QString();
}

bool loadOne(const QString& soPath, PluginLoader::Plugin& out, QString& err)
{
    out.path = soPath;
    out.handle = ::dlopen(soPath.toUtf8().constData(),
                          RTLD_NOW | RTLD_LOCAL);
    if (!out.handle) {
        err = QString::fromUtf8(::dlerror());
        return false;
    }

    auto sym = [&out](const char* name) -> void* {
        ::dlerror();   // clear
        void* p = ::dlsym(out.handle, name);
        return p;
    };

    // Required: getName, getFuncsArray. setInfo / beNotified /
    // messageProc are optional (some plugins are pure menu bundles).
    using GetNameFn       = const char*       (*)(void);
    using GetFuncsArrayFn = LinuxFuncItem*    (*)(int*);

    auto getNameFn       = reinterpret_cast<GetNameFn>(sym("getName"));
    auto getFuncsArrayFn = reinterpret_cast<GetFuncsArrayFn>(sym("getFuncsArray"));
    if (!getNameFn || !getFuncsArrayFn) {
        err = QStringLiteral("missing required symbols (getName, getFuncsArray)");
        ::dlclose(out.handle);
        out.handle = nullptr;
        return false;
    }

    out.setInfoFn      = reinterpret_cast<PluginLoader::Plugin::SetInfoFn>(
                            sym("setInfo"));
    out.beNotifiedFn   = reinterpret_cast<PluginLoader::Plugin::BeNotifiedFn>(
                            sym("beNotified"));
    out.messageProcFn  = reinterpret_cast<PluginLoader::Plugin::MessageProcFn>(
                            sym("messageProc"));

    const char* nm = getNameFn();
    if (!nm || !*nm) {
        err = QStringLiteral("getName() returned null/empty");
        ::dlclose(out.handle);
        out.handle = nullptr;
        return false;
    }
    out.name = QString::fromUtf8(nm);

    int count = 0;
    LinuxFuncItem* items = getFuncsArrayFn(&count);
    out.functions.reserve(count);
    for (int i = 0; i < count; ++i) {
        items[i].cmdID = nextCmdId();
        out.functions.append(&items[i]);
    }
    return true;
}

LinuxNppData buildNppData(MainWindow* mw)
{
    LinuxNppData d{};
    d.nppHandle = mw;     // opaque to the plugin
    if (mw) {
        if (auto* pane = mw->activePane()) {
            if (auto* b = pane->currentBuffer()) {
                d.scintillaMainHandle = b->editor();
            }
        }
        if (auto* other = (mw->activePane() == mw->leftPane())
                          ? mw->rightPane() : mw->leftPane()) {
            if (other->currentBuffer()) {
                d.scintillaSecondHandle = other->currentBuffer()->editor();
            }
        }
    }
    return d;
}

} // namespace

namespace PluginLoader {

QString pluginsDir()
{
    // Phase 12 — route through Config::configFilePath() so cloud-sync
    // override applies. Plugins live alongside config.xml; users with
    // a synced config dir get plugins on every machine for free.
    const QString cfg = Config::configFilePath();
    QFileInfo fi(cfg);
    return fi.absolutePath() + QStringLiteral("/plugins");
}

// Plugin search paths. In order of precedence:
//   1. User dir (~/.config/padnote/padnote--/plugins/) — opt-in.
//   2. Build-tree fallback (build/plugins/) — dev builds find
//      first-party plugins without make install.
//   3. System bundle dir (/usr/lib/padnote/plugins/) — installed
//      first-party plugins.
//
// Same-name collisions: user-dir wins, then build-tree, then system.
QStringList searchPaths()
{
    QStringList paths;
    paths.append(PluginLoader::pluginsDir());

    // Build-tree fallback. The binary lives at build/padnote
    // (per CMakeLists.txt's RUNTIME_OUTPUT_DIRECTORY = ${CMAKE_BINARY_DIR}).
    // Plugins land at build/plugins/<name>/<name>.so (per
    // plugins/CMakeLists.txt's LIBRARY_OUTPUT_DIRECTORY).
    const QString appDir = QCoreApplication::applicationDirPath();
    paths.append(appDir + QStringLiteral("/plugins"));

    // System bundle dir — installed by `make install`, lives at
    // <CMAKE_INSTALL_PREFIX>/lib/padnote/plugins/. Resolve relative
    // to the binary path so the AppImage's bundled location works
    // the same as a system /usr install.
    paths.append(appDir + QStringLiteral("/../lib/padnote/plugins"));

    return paths;
}

void loadAll(MainWindow* mw)
{
    if (!store().isEmpty()) return;     // idempotent

    int loaded = 0;
    int failed = 0;
    QStringList failures;
    QStringList alreadyLoaded;          // dedup by plugin name across paths

    for (const QString& root : searchPaths()) {
        QDir rootDir(root);
        if (!rootDir.exists()) continue;

        const QStringList subdirs = rootDir.entryList(
            QDir::AllDirs | QDir::NoDotAndDotDot, QDir::Name);
        for (const QString& sub : subdirs) {
            if (alreadyLoaded.contains(sub)) continue;   // user-dir won

            const QString subPath = rootDir.absoluteFilePath(sub);
            const QString soPath  = findSoIn(subPath);
            if (soPath.isEmpty()) continue;

            auto* plugin = new Plugin;
            QString err;
            if (loadOne(soPath, *plugin, err)) {
                store().push_back(plugin);
                alreadyLoaded.append(sub);
                ++loaded;
                if (plugin->setInfoFn) {
                    plugin->setInfoFn(buildNppData(mw));
                }
            } else {
                ++failed;
                failures.append(QStringLiteral("%1: %2").arg(sub, err));
                delete plugin;
            }
        }
    }

    if (mw && (loaded > 0 || failed > 0)) {
        QString msg;
        if (loaded > 0) msg += QObject::tr("Loaded %1 plugin(s)").arg(loaded);
        if (failed > 0) {
            if (!msg.isEmpty()) msg += QStringLiteral("; ");
            msg += QObject::tr("%1 failed: %2").arg(failed)
                .arg(failures.join(QStringLiteral(", ")));
        }
        // Defer the status message — loadAll() runs before MainWindow's
        // status bar is laid out (Phase 10b). QTimer::singleShot(0)
        // queues the message until the next event-loop turn, by which
        // time buildStatusBar() has run and the message displays.
        QPointer<MainWindow> safeMw(mw);
        QTimer::singleShot(0, mw, [safeMw, msg]() {
            if (safeMw) safeMw->statusBar()->showMessage(msg, 5000);
        });
    }
}

void unloadAll()
{
    for (Plugin* p : store()) {
        if (p && p->handle) ::dlclose(p->handle);
        delete p;
    }
    store().clear();
}

const QVector<Plugin*>& all()
{
    return store();
}

void notifyAll(void* scNotification)
{
    for (Plugin* p : store()) {
        if (p && p->beNotifiedFn) p->beNotifiedFn(scNotification);
    }
}

} // namespace PluginLoader
