#include "SingleInstance.h"

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>

#include "MainWindow.h"

namespace {
constexpr const char* kService = "org.padnote.Instance";
constexpr const char* kPath    = "/Instance";
constexpr const char* kIface   = "org.padnote.Instance";
} // namespace

SingleInstance::SingleInstance(MainWindow* mw, QObject* parent)
    : QObject(parent), m_mw(mw)
{}

bool SingleInstance::acquire()
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) return false;
    if (!bus.registerService(QString::fromLatin1(kService))) return false;
    return bus.registerObject(QString::fromLatin1(kPath), this,
        QDBusConnection::ExportScriptableSlots
        | QDBusConnection::ExportScriptableProperties);
}

bool SingleInstance::dispatchToExistingInstance(const QStringList& files)
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) return false;

    // interface() lookup gracefully fails when no peer is registered;
    // QDBusInterface::isValid() returns false in that case.
    QDBusInterface iface(QString::fromLatin1(kService),
                         QString::fromLatin1(kPath),
                         QString::fromLatin1(kIface),
                         bus);
    if (!iface.isValid()) return false;

    // Even an empty file list dispatches — the existing instance will
    // raise its window. That covers the "I clicked the .desktop launcher"
    // case where the user wants to bring padnote to the foreground without
    // opening anything new.
    QDBusReply<void> reply = iface.call(QStringLiteral("openFiles"), files);
    return reply.isValid();
}

void SingleInstance::openFiles(const QStringList& files)
{
    if (!m_mw) return;

    // Open files in the existing window's active pane (the pane the
    // user last interacted with — most intuitive target for an external
    // file-open dispatch).
    m_mw->openFilesInActivePane(files);

    // Raise the existing window so the user actually sees what just
    // happened. activateWindow on a focus-stealing-prevention WM is
    // best-effort, but combined with raise() and show() the result is
    // usable on every mainstream Linux desktop.
    m_mw->show();
    m_mw->raise();
    m_mw->activateWindow();
}
