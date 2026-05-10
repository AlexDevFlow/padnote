// SingleInstance.h — D-Bus-driven single-instance dispatch.
//
// On launch, main_qt.cpp tries to claim the well-known service name
// "org.padnote.Instance" on the user's session bus.
// • If it succeeds, this process becomes THE padnote instance and
//   exposes openFiles(QStringList) on /Instance.
// • If it fails (another instance already owns the name), we forward
//   our CLI args via a D-Bus call to that existing instance and exit 0.
//
// The --new-instance CLI flag bypasses both paths: a second window
// always opens, useful for debugging or running two configs side by
// side.

#pragma once

#include <QObject>
#include <QStringList>

class MainWindow;

class SingleInstance : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.padnote.Instance")
public:
    explicit SingleInstance(MainWindow* mw, QObject* parent = nullptr);

    // Try to register as THE instance. Returns true on success (we own
    // the bus name); false otherwise (another instance already owns it,
    // or the session bus is unavailable).
    bool acquire();

    // Forward `files` to an already-running instance. Returns true if the
    // call was dispatched (regardless of whether the existing instance
    // succeeded in opening every file). Returns false if no existing
    // instance is reachable on the session bus, or if D-Bus itself is
    // unavailable — caller should fall through to becoming the instance.
    static bool dispatchToExistingInstance(const QStringList& files);

public slots:
    // D-Bus-exported. Called by remote QDBusInterface::call("openFiles", …).
    Q_SCRIPTABLE void openFiles(const QStringList& files);

private:
    MainWindow* m_mw;
};
