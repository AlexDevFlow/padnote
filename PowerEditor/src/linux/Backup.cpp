#include "Backup.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QUuid>

#include "Buffer.h"
#include "Scintilla.h"
#include "ScintillaEditBase.h"
#include "ScintillaMessages.h"

#include "pugixml.hpp"

#include <vector>

using Scintilla::Message;

namespace {

QString backupDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
           + QStringLiteral("/backup");
}

QString indexFile()
{
    return backupDir() + QStringLiteral("/index.xml");
}

// Atomic write: .tmp + rename.
bool writeAtomic(const QString& path, const QByteArray& bytes)
{
    const QString tmp = path + QStringLiteral(".tmp");
    QFile f(tmp);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    if (f.write(bytes) != bytes.size()) { f.close(); return false; }
    f.close();
    QFile::remove(path);
    return QFile::rename(tmp, path);
}

struct IndexEntry {
    QString uuid;
    QString displayName;
    QString originalPath;
};

QVector<IndexEntry> readIndex()
{
    QVector<IndexEntry> out;
    QFile f(indexFile());
    if (!f.open(QIODevice::ReadOnly)) return out;
    const QByteArray data = f.readAll();
    f.close();

    pugi::xml_document doc;
    if (!doc.load_buffer(data.constData(), static_cast<size_t>(data.size())))
        return out;

    const pugi::xml_node root = doc.child("NotepadPlus");
    const pugi::xml_node bks  = root.child("Backups");
    for (pugi::xml_node n : bks.children("Backup")) {
        IndexEntry e;
        e.uuid         = QString::fromUtf8(n.attribute("uuid").value());
        e.displayName  = QString::fromUtf8(n.attribute("displayName").value());
        e.originalPath = QString::fromUtf8(n.attribute("filePath").value());
        if (!e.uuid.isEmpty()) out.append(e);
    }
    return out;
}

bool writeIndex(const QVector<IndexEntry>& entries)
{
    pugi::xml_document doc;
    pugi::xml_node decl = doc.append_child(pugi::node_declaration);
    decl.append_attribute("version") = "1.0";
    decl.append_attribute("encoding") = "UTF-8";
    pugi::xml_node root = doc.append_child("NotepadPlus");
    pugi::xml_node bks  = root.append_child("Backups");
    for (const IndexEntry& e : entries) {
        pugi::xml_node n = bks.append_child("Backup");
        const QByteArray u = e.uuid.toUtf8();
        const QByteArray d = e.displayName.toUtf8();
        const QByteArray p = e.originalPath.toUtf8();
        n.append_attribute("uuid")        = u.constData();
        n.append_attribute("displayName") = d.constData();
        n.append_attribute("filePath")    = p.constData();
    }
    struct W : pugi::xml_writer {
        QByteArray bytes;
        void write(const void* data, size_t size) override {
            bytes.append(static_cast<const char*>(data),
                         static_cast<int>(size));
        }
    } w;
    doc.save(w, "    ");
    return writeAtomic(indexFile(), w.bytes);
}

QString bakPathFor(const QString& uuid)
{
    return backupDir() + QStringLiteral("/") + uuid + QStringLiteral(".bak");
}

QByteArray bufferUtf8(Buffer* b)
{
    auto* ed = b->editor();
    const Scintilla::sptr_t length =
        ed->send(static_cast<unsigned int>(Message::GetTextLength));
    if (length <= 0) return {};
    std::vector<char> buf(static_cast<std::size_t>(length) + 1, '\0');
    ed->send(static_cast<unsigned int>(Message::GetText),
             static_cast<Scintilla::uptr_t>(buf.size()),
             reinterpret_cast<Scintilla::sptr_t>(buf.data()));
    return QByteArray(buf.data(), static_cast<int>(length));
}

} // namespace

namespace Backup {

void init()
{
    QDir().mkpath(backupDir());
}

void writeBuffer(Buffer* b)
{
    if (!b) return;
    init();

    // Reuse existing UUID if this Buffer has one already.
    QString uuid = b->backupUuid();
    if (uuid.isEmpty()) {
        uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
        b->setBackupUuid(uuid);
    }

    const QByteArray content = bufferUtf8(b);
    writeAtomic(bakPathFor(uuid), content);

    // Update or insert the index entry.
    QVector<IndexEntry> idx = readIndex();
    bool found = false;
    for (auto& e : idx) {
        if (e.uuid == uuid) {
            e.displayName  = b->displayName();
            e.originalPath = b->hasFile() ? b->filePath() : QString{};
            found = true;
            break;
        }
    }
    if (!found) {
        IndexEntry e;
        e.uuid         = uuid;
        e.displayName  = b->displayName();
        e.originalPath = b->hasFile() ? b->filePath() : QString{};
        idx.append(e);
    }
    writeIndex(idx);
}

void clearBuffer(Buffer* b)
{
    if (!b) return;
    const QString uuid = b->backupUuid();
    if (uuid.isEmpty()) return;

    QFile::remove(bakPathFor(uuid));
    QVector<IndexEntry> idx = readIndex();
    QVector<IndexEntry> kept;
    for (const auto& e : idx) {
        if (e.uuid != uuid) kept.append(e);
    }
    writeIndex(kept);
    b->setBackupUuid(QString{});
}

QVector<Recovery> pendingRecoveries()
{
    QVector<Recovery> out;
    const auto idx = readIndex();
    for (const auto& e : idx) {
        const QString p = bakPathFor(e.uuid);
        if (!QFileInfo::exists(p)) continue;
        Recovery r;
        r.uuid         = e.uuid;
        r.displayName  = e.displayName;
        r.originalPath = e.originalPath;
        r.backupPath   = p;
        out.append(r);
    }
    return out;
}

void clearAll()
{
    QDir d(backupDir());
    if (!d.exists()) return;
    for (const QString& f : d.entryList(
             QStringList() << QStringLiteral("*.bak") << QStringLiteral("index.xml"),
             QDir::Files)) {
        d.remove(f);
    }
}

} // namespace Backup
