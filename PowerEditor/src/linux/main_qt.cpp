// main_qt.cpp — padnote-- entry point.
//
// Standard main() with QApplication. On launch, tries to dispatch CLI
// args to an already-running instance via D-Bus. If that fails (no
// existing instance, or --new-instance was passed), becomes the
// canonical instance itself.

#include <clocale>

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QFileInfo>
#include <QStandardPaths>
#include <QStringList>

#include "Backup.h"
#include "Buffer.h"
#include "Config.h"
#include "EditorTabs.h"
#include "Languages.h"
#include "MainWindow.h"
#include "Scintilla.h"
#include "ScintillaEditBase.h"
#include "ScintillaMessages.h"
#include "Session.h"
#include "SingleInstance.h"
#include "Theme.h"
#include "UserDefineLang.h"   // Phase 5U.2 — userDefineLangs.xml round-trip

#include <QFile>
#include <QMessageBox>

int main(int argc, char* argv[])
{
    // Force a UTF-8 locale for path conversions. See PORTING_QUESTIONS.md Q8.
    std::setlocale(LC_ALL, "C.UTF-8");

    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("padnote--"));
    QApplication::setApplicationDisplayName(QStringLiteral("padnote--"));
    QApplication::setOrganizationName(QStringLiteral("padnote"));
    QApplication::setOrganizationDomain(QStringLiteral("padnote.local"));
    QApplication::setApplicationVersion(QStringLiteral(PADNOTE_VERSION_STRING));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("padnote-- — Qt 6 Linux text editor"));
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption newInstanceOpt(
        QStringLiteral("new-instance"),
        QStringLiteral("Always start a new window, even if another instance is running."));
    parser.addOption(newInstanceOpt);
    parser.addPositionalArgument(
        QStringLiteral("files"),
        QStringLiteral("Files to open (one tab per file)"),
        QStringLiteral("[files...]"));
    parser.process(app);

    // Single-instance dispatch. If another padnote is already on the
    // user's session bus, hand it the file list (absolute paths so the
    // existing instance's CWD doesn't matter) and exit. The
    // --new-instance flag bypasses dispatch entirely.
    QStringList cliFiles = parser.positionalArguments();
    QStringList absFiles;
    absFiles.reserve(cliFiles.size());
    for (const QString& f : cliFiles) absFiles << QFileInfo(f).absoluteFilePath();
    if (!parser.isSet(newInstanceOpt)) {
        if (SingleInstance::dispatchToExistingInstance(absFiles)) {
            return 0;
        }
    }

    // Load persisted user preferences before anything that might consult them.
    Config::load();

    // Build the language registry from the embedded langs.model.xml resource
    // before MainWindow constructs its Language menu and the first Buffer.
    //
    // Phase 5U.2 — UDLs from userDefineLangs.xml are appended between init()
    // (which leaves the registry unsorted) and finalize() (which performs
    // the post-init sort + caches plain-text). main_qt.cpp owns this
    // ordering so any other consumer of `Languages::all()` sees a stable,
    // fully-finalised registry.
    Languages::init();
    {
        const QVector<UDL> udls = UserDefineLang::loadAll();
        for (const UDL& udl : udls) Languages::appendUDL(udl);
    }
    Languages::finalize();

    // Parse all bundled themes (Phase 7f). Config::theme() now stores
    // the theme NAME ("Default Light", "Solarized", "Monokai", ...) —
    // try setThemeByName first; fall back to the historical
    // "Light"/"Dark" two-value behaviour for upgraders.
    Theme::init();
    const QString savedTheme = Config::theme();
    if (!Theme::setThemeByName(savedTheme)) {
        Theme::setMode(savedTheme == QStringLiteral("Dark")
                           ? Theme::Mode::Dark : Theme::Mode::Light);
    }

    MainWindow w;
    w.show();

    // Register on the session bus AFTER the window is up so the openFiles
    // slot sees a fully-constructed MainWindow on its first call. Failure
    // here (no D-Bus, name collision in a race) is non-fatal — we just
    // run as if --new-instance had been passed.
    SingleInstance dbus(&w);
    if (!parser.isSet(newInstanceOpt)) {
        dbus.acquire();
    }

    // CLI files take priority over saved session: if any are passed, open
    // them and ignore the saved session entirely. Otherwise, restore the
    // last session if one exists.
    //
    // Session restore round-trips both panes; the initial Untitled
    // (created by the MainWindow ctor in the LEFT pane) is dropped when
    // the restore added at least one real file. The right pane is shown
    // only if the saved splitVisible attribute was true.
    auto dropInitialUntitledIfReplaced = [&](int restored) {
        if (restored <= 0) return;
        EditorTabs* L = w.leftPane();
        if (L->bufferCount() <= 1) return;
        Buffer* first = L->bufferAt(0);
        if (first && !first->hasFile() && !first->isDirty()) {
            L->dropBufferAt(0);
        }
    };

    if (!absFiles.isEmpty()) {
        const int opened = w.openFilesInActivePane(absFiles);
        dropInitialUntitledIfReplaced(opened);
    } else {
        bool wasSplit = false;
        int  activeView = 0;
        const int restored = Session::restore(w.leftPane(), w.rightPane(),
                                              &wasSplit, &activeView);
        dropInitialUntitledIfReplaced(restored);
        if (wasSplit) w.setSplitVisible(true);
        // Restore which pane had focus. setActivePane is a no-op if
        // the right pane is hidden, so this is safe regardless of
        // wasSplit.
        if (activeView == 1) w.setActivePane(1);
    }

    // Backup recovery runs AFTER session restore so the hot-exit
    // overlay can match recoveries against the just-reopened buffers.
    //
    // Hot-exit pending  → silent overlay; no prompt, dirty buffers
    //                     come back exactly as they were left.
    // No hot-exit marker but recoveries present → previous run
    //                     crashed; show the recover-or-discard prompt.
    {
        const auto recoveries = Backup::pendingRecoveries();
        if (!recoveries.isEmpty()) {
            if (Backup::isHotExitPending()) {
                w.applyHotExitOverlay(recoveries);
            } else {
                const auto reply = QMessageBox::question(&w,
                    QObject::tr("Recover unsaved work"),
                    QObject::tr("%1 unsaved buffer(s) from the previous session "
                                "were not cleanly closed. Recover them?")
                        .arg(recoveries.size()),
                    QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
                if (reply == QMessageBox::Yes) {
                    for (const auto& r : recoveries) {
                        QFile bak(r.backupPath);
                        if (!bak.open(QIODevice::ReadOnly)) continue;
                        const QByteArray bytes = bak.readAll();
                        bak.close();
                        w.adoptCrashRecoveredBuffer(bytes);
                    }
                }
            }
            Backup::clearAll();
        }
    }

    return app.exec();
}
