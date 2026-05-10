#include "Session.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

#include "Buffer.h"
#include "Config.h"     // Phase 12 — cloudConfigDir override
#include "EditorTabs.h"
#include "ScintillaEditBase.h"
#include "ScintillaMessages.h"

#include "pugixml.hpp"

using Scintilla::Message;

namespace {

QString sessionFile()
{
    // Phase 12 — route through the cloud-sync override. The Config
    // module's configFilePath() resolves cloud.path; we strip the
    // /config.xml suffix to get the resolved directory.
    const QString cfgPath = Config::configFilePath();
    QFileInfo fi(cfgPath);
    return fi.absolutePath() + QStringLiteral("/session.xml");
}

// Open every <File> child of `parent` into `pane`. Restores the per-pane
// activeIndex relative to the buffers actually opened (untitled / missing
// files are skipped, so the saved index needs to be remapped).
int restorePane(EditorTabs* pane, const pugi::xml_node& parent)
{
    if (!pane) return 0;

    const int firstNewIndex = pane->bufferCount();   // before any restored opens
    int opened = 0;
    for (pugi::xml_node node : parent.children("File")) {
        const char* fn = node.attribute("filename").value();
        if (!fn || !*fn) continue;
        const QString path = QString::fromUtf8(fn);
        if (!QFileInfo::exists(path)) continue;

        Buffer* b = pane->openFile(path);
        if (!b) continue;

        const long long pos = node.attribute("position").as_llong(0);
        if (pos > 0) {
            b->editor()->send(static_cast<unsigned int>(Message::GotoPos),
                              static_cast<Scintilla::uptr_t>(pos), 0);
            b->editor()->send(static_cast<unsigned int>(Message::ScrollCaret));
        }
        ++opened;
    }

    const int activeIndex = parent.attribute("activeIndex").as_int(0);
    const int target = firstNewIndex + activeIndex;
    if (target >= 0 && target < pane->bufferCount()) {
        pane->setCurrentIndex(target);
    }
    return opened;
}

// Walk every Buffer with a file path on disk in `pane` and append a <File>
// child to `paneNode`. Returns the active index relative to the saved
// buffers (skipping Untitled).
int writePane(EditorTabs* pane, pugi::xml_node& paneNode)
{
    if (!pane) return 0;

    const int currentIdx = pane->currentIndex();
    int savedActiveIndex = 0;
    int savedSoFar = 0;

    for (int i = 0; i < pane->bufferCount(); ++i) {
        Buffer* b = pane->bufferAt(i);
        if (!b || !b->hasFile()) continue;
        const QString path = QFileInfo(b->filePath()).absoluteFilePath();
        if (path.isEmpty()) continue;

        if (i == currentIdx) savedActiveIndex = savedSoFar;
        ++savedSoFar;

        pugi::xml_node file = paneNode.append_child("File");
        const QByteArray utf8 = path.toUtf8();
        file.append_attribute("filename") = utf8.constData();
        const Scintilla::sptr_t pos =
            b->editor()->send(static_cast<unsigned int>(Message::GetCurrentPos));
        file.append_attribute("position") = static_cast<long long>(pos);
    }

    paneNode.append_attribute("activeIndex") = savedActiveIndex;
    return savedSoFar;
}

} // namespace

namespace Session {

int restore(EditorTabs* left, EditorTabs* right,
            bool* outSplitVisible, int* outActiveView)
{
    return restoreFromFile(left, right, outSplitVisible, outActiveView,
                           sessionFile());
}

int restoreFromFile(EditorTabs* left, EditorTabs* right,
                    bool* outSplitVisible, int* outActiveView,
                    const QString& path)
{
    if (!left) return 0;

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return 0;
    const QByteArray bytes = f.readAll();
    f.close();
    if (bytes.isEmpty()) return 0;

    pugi::xml_document doc;
    const pugi::xml_parse_result r = doc.load_buffer(bytes.constData(),
        static_cast<size_t>(bytes.size()));
    if (!r) return 0;

    const pugi::xml_node session = doc.child("NotepadPlus").child("Session");
    if (!session) return 0;

    if (outSplitVisible) {
        const QString sv = QString::fromUtf8(
            session.attribute("splitVisible").value());
        *outSplitVisible = (sv == QStringLiteral("yes")
                         || sv == QStringLiteral("true")
                         || sv == QStringLiteral("1"));
    }
    if (outActiveView) {
        *outActiveView = session.attribute("activeView").as_int(0);
    }

    int opened = 0;

    // Phase 3d schema: <Pane index="N"> children. Forward-load v1: if there
    // are no <Pane> children but there are direct <File> children, treat
    // the whole list as pane 0 with split hidden.
    const bool hasPanes = !!session.child("Pane");
    if (hasPanes) {
        for (pugi::xml_node pn : session.children("Pane")) {
            const int idx = pn.attribute("index").as_int(0);
            EditorTabs* target = (idx == 1) ? right : left;
            opened += restorePane(target, pn);
        }
    } else {
        // Pre-Phase-3d session — treat as pane 0; right pane stays empty.
        opened += restorePane(left, session);
        if (outSplitVisible) *outSplitVisible = false;
        if (outActiveView)   *outActiveView   = 0;
    }

    return opened;
}

void save(EditorTabs* left, EditorTabs* right,
          bool splitVisible, int activeView)
{
    saveToFile(left, right, splitVisible, activeView, sessionFile());
}

bool saveToFile(EditorTabs* left, EditorTabs* right,
                bool splitVisible, int activeView,
                const QString& path)
{
    if (!left) return false;

    // Ensure target directory exists. For the default sessionFile() path
    // this matters on first launch; for user-chosen paths the parent
    // dir already exists (the file dialog wouldn't have offered it
    // otherwise).
    const QString parent = QFileInfo(path).absolutePath();
    if (!parent.isEmpty()) QDir().mkpath(parent);

    pugi::xml_document doc;
    pugi::xml_node decl = doc.append_child(pugi::node_declaration);
    decl.append_attribute("version") = "1.0";
    decl.append_attribute("encoding") = "UTF-8";

    pugi::xml_node root    = doc.append_child("NotepadPlus");
    pugi::xml_node session = root.append_child("Session");
    session.append_attribute("activeView")   = activeView;
    session.append_attribute("splitVisible") = splitVisible ? "yes" : "no";

    pugi::xml_node leftPane = session.append_child("Pane");
    leftPane.append_attribute("index") = 0;
    writePane(left, leftPane);

    if (right && right->bufferCount() > 0) {
        pugi::xml_node rightPane = session.append_child("Pane");
        rightPane.append_attribute("index") = 1;
        writePane(right, rightPane);
    }

    // Atomic write: .tmp + rename.
    struct Writer : pugi::xml_writer {
        QByteArray bytes;
        void write(const void* data, size_t size) override {
            bytes.append(static_cast<const char*>(data),
                         static_cast<int>(size));
        }
    } w;
    doc.save(w, "    ");

    const QString target = path;
    const QString tmp    = target + QStringLiteral(".tmp");
    QFile f(tmp);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    f.write(w.bytes);
    f.close();
    QFile::remove(target);
    return QFile::rename(tmp, target);
}

} // namespace Session
