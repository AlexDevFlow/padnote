// PluginLoader.h — dlopen-based plugin runtime.
//
// On startup (after MainWindow construction), `PluginLoader::loadAll()`
// walks `~/.config/padnote/padnote--/plugins/` looking for
// subdirectories of the form `<name>/<name>.so`. Each .so is dlopen'd
// and queried for the Linux Plugin ABI entry points (see
// `plugins/linuxplugin.h`):
//   • getName(void) -> const char*
//   • setInfo(LinuxNppData) -> void
//   • getFuncsArray(int*) -> LinuxFuncItem*
//   • beNotified(void*) -> void
//   • messageProc(unsigned, uintptr_t, intptr_t) -> intptr_t
//
// Missing or null symbols abort that plugin's load with a status-bar
// message; the host stays running.
//
// Plugins are loaded ONCE at startup and unloaded at shutdown. Hot
// reload is intentionally out of scope — too easy for a half-loaded
// plugin to leave the host in an inconsistent state.
//
// Phase 10b wires the loaded plugins into the Plugins menu via
// `MainWindow::buildPluginsMenu` (which calls `PluginLoader::all()`).

#pragma once

#include <QString>
#include <QVector>

class MainWindow;

// Forward-declarations of the ABI types so callers don't have to
// transitively include <linuxplugin.h>.
struct LinuxFuncItem;
struct LinuxNppData;

namespace PluginLoader {

struct Plugin {
    QString name;                           // from getName()
    QString path;                           // absolute .so path
    void*   handle = nullptr;               // dlopen result
    QVector<LinuxFuncItem*> functions;      // pointers into the
                                            // plugin's static array;
                                            // not owned

    // Cached function pointers. Null if the plugin failed to export.
    typedef void          (*SetInfoFn)(LinuxNppData);
    typedef void          (*BeNotifiedFn)(void*);
    typedef intptr_t      (*MessageProcFn)(unsigned int, uintptr_t, intptr_t);

    SetInfoFn      setInfoFn      = nullptr;
    BeNotifiedFn   beNotifiedFn   = nullptr;
    MessageProcFn  messageProcFn  = nullptr;
};

// Load every plugin under the user's plugin dir. Idempotent — repeat
// calls are no-ops after the first. Pushes status-bar messages to
// `mw` for each successful load + each failure.
void loadAll(MainWindow* mw);

// dlclose every loaded plugin. Called at MainWindow destruction.
void unloadAll();

// Iterate the loaded plugins. Stable order (alphabetical by name).
const QVector<Plugin*>& all();

// Filesystem path that loadAll() walks. Honoured by the cloud-sync
// override (Phase 12) — plugins live alongside config.xml so they
// also propagate to a synced machine.
QString pluginsDir();

// Dispatch an SCNotification to every plugin's beNotified callback.
// Called by Buffer when relevant editor events fire.
void notifyAll(void* scNotification);

} // namespace PluginLoader
