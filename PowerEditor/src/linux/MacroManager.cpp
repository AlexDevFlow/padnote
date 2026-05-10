#include "MacroManager.h"

#include <QSet>
#include <cstring>

#include "pugixml.hpp"

#include "ScintillaEditBase.h"
#include "ScintillaMessages.h"

using Scintilla::Message;

namespace {

// Scintilla messages whose lparam is a const char* byte buffer. Replay
// must use ed->sends() with the bytes; persistence must base64-encode
// lparam through XML. Subset of the upstream Scintilla.iface "string"
// arg type — covers every text-bearing op our codebase emits and every
// op SCN_MACRORECORD typically reports during interactive typing.
bool messageTakesString(unsigned int msg)
{
    switch (msg) {
        case static_cast<unsigned int>(Message::AddText):
        case static_cast<unsigned int>(Message::AppendText):
        case static_cast<unsigned int>(Message::InsertText):
        case static_cast<unsigned int>(Message::ReplaceSel):
        case static_cast<unsigned int>(Message::ReplaceTarget):
        case static_cast<unsigned int>(Message::ReplaceTargetRE):
        case static_cast<unsigned int>(Message::ReplaceTargetMinimal):
        case static_cast<unsigned int>(Message::SearchInTarget):
        case static_cast<unsigned int>(Message::SetText):
            return true;
        default:
            return false;
    }
}

// For text-bearing ops the wparam is sometimes a length (AddText,
// AppendText, ReplaceTarget...) and sometimes a position (InsertText)
// or "use selection" (ReplaceSel — wparam ignored, takes a 0-terminated
// string). Capture wparam-as-given and rely on Scintilla to interpret.
//
// Persisted lparam length: we capture exactly `wparam` bytes when the
// message is length-prefixed; otherwise (ReplaceSel, InsertText with
// pos-wparam) we strlen(lparam) so the recorded buffer has the right
// size.
qsizetype textBufferLength(unsigned int msg, quintptr wparam,
                           const char* lparam)
{
    if (!lparam) return 0;
    switch (msg) {
        case static_cast<unsigned int>(Message::AddText):
        case static_cast<unsigned int>(Message::AppendText):
        case static_cast<unsigned int>(Message::ReplaceTarget):
        case static_cast<unsigned int>(Message::ReplaceTargetRE):
        case static_cast<unsigned int>(Message::ReplaceTargetMinimal):
        case static_cast<unsigned int>(Message::SearchInTarget):
            return static_cast<qsizetype>(wparam);
        case static_cast<unsigned int>(Message::InsertText):
        case static_cast<unsigned int>(Message::ReplaceSel):
        case static_cast<unsigned int>(Message::SetText):
        default:
            return static_cast<qsizetype>(std::strlen(lparam));
    }
}

} // namespace

MacroManager& MacroManager::instance()
{
    static MacroManager s;
    return s;
}

MacroManager::MacroManager(QObject* parent) : QObject(parent) {}

bool MacroManager::isRecording() const
{
    return m_recording && !m_recordingEditor.isNull();
}

bool MacroManager::isRecordingFor(ScintillaEditBase* ed) const
{
    return m_recording && !m_recordingEditor.isNull()
        && m_recordingEditor.data() == ed;
}

bool MacroManager::hasLastMacro() const
{
    return !m_lastMacro.isEmpty();
}

const Macro& MacroManager::lastMacro() const
{
    return m_lastMacro;
}

QStringList MacroManager::savedNames() const
{
    return m_saved.keys();   // QMap keys are sorted ascending.
}

bool MacroManager::hasSaved(const QString& name) const
{
    return m_saved.contains(name);
}

void MacroManager::start(ScintillaEditBase* targetEditor)
{
    if (!targetEditor) return;
    if (m_recording) return;

    m_recording = true;
    m_recordingEditor = targetEditor;
    m_currentRecording.clear();

    connect(targetEditor, &ScintillaEditBase::macroRecord,
            this, &MacroManager::onMacroRecord, Qt::UniqueConnection);
    connect(targetEditor, &ScintillaEditBase::destroyed,
            this, &MacroManager::onRecordingEditorDestroyed,
            Qt::UniqueConnection);

    targetEditor->send(static_cast<unsigned int>(Message::StartRecord), 0, 0);
    emit recordingStateChanged(true);
}

void MacroManager::stop()
{
    if (!m_recording) return;
    m_recording = false;

    if (auto* ed = m_recordingEditor.data()) {
        ed->send(static_cast<unsigned int>(Message::StopRecord), 0, 0);
        disconnect(ed, &ScintillaEditBase::macroRecord,
                   this, &MacroManager::onMacroRecord);
        disconnect(ed, &ScintillaEditBase::destroyed,
                   this, &MacroManager::onRecordingEditorDestroyed);
    }
    m_recordingEditor.clear();

    if (!m_currentRecording.isEmpty()) {
        m_lastMacro = std::move(m_currentRecording);
        emit lastMacroChanged();
    }
    m_currentRecording.clear();

    emit recordingStateChanged(false);
}

void MacroManager::onRecordingEditorDestroyed()
{
    // The editor went away mid-recording (tab close). Treat as stop;
    // skip the SCI_STOPRECORD send because the QObject is gone.
    if (!m_recording) return;
    m_recording = false;
    m_recordingEditor.clear();

    if (!m_currentRecording.isEmpty()) {
        m_lastMacro = std::move(m_currentRecording);
        emit lastMacroChanged();
    }
    m_currentRecording.clear();
    emit recordingStateChanged(false);
}

void MacroManager::onMacroRecord(Scintilla::Message msg,
                                 Scintilla::uptr_t  wp,
                                 Scintilla::sptr_t  lp)
{
    if (!m_recording) return;

    MacroOp op;
    op.msg    = static_cast<unsigned int>(msg);
    op.wparam = static_cast<quintptr>(wp);

    if (messageTakesString(op.msg)) {
        op.hasText = true;
        const char* s = reinterpret_cast<const char*>(lp);
        const qsizetype n = textBufferLength(op.msg, op.wparam, s);
        if (s && n > 0) {
            op.lparamText = QByteArray(s, static_cast<int>(n));
        }
    } else {
        op.lparamInt = static_cast<qintptr>(lp);
    }
    m_currentRecording.append(op);
}

void MacroManager::play(const Macro& m, ScintillaEditBase* target, int count)
{
    if (!target || m.isEmpty() || count <= 0) return;

    target->send(static_cast<unsigned int>(Message::BeginUndoAction), 0, 0);
    for (int i = 0; i < count; ++i) {
        for (const MacroOp& op : m) {
            if (op.hasText) {
                // sends() takes a uptr_t wparam and a const char* lparam.
                // For length-prefixed messages, wparam IS the length.
                target->sends(op.msg,
                              static_cast<Scintilla::uptr_t>(op.wparam),
                              op.lparamText.constData());
            } else {
                target->send(op.msg,
                             static_cast<Scintilla::uptr_t>(op.wparam),
                             static_cast<Scintilla::sptr_t>(op.lparamInt));
            }
        }
    }
    target->send(static_cast<unsigned int>(Message::EndUndoAction), 0, 0);
}

void MacroManager::playLast(ScintillaEditBase* target, int count)
{
    play(m_lastMacro, target, count);
}

void MacroManager::playSaved(const QString& name,
                             ScintillaEditBase* target, int count)
{
    auto it = m_saved.constFind(name);
    if (it == m_saved.constEnd()) return;
    play(*it, target, count);
}

void MacroManager::saveLast(const QString& name)
{
    if (m_lastMacro.isEmpty() || name.isEmpty()) return;
    const bool isNew = !m_saved.contains(name);
    m_saved.insert(name, m_lastMacro);   // overwrite if exists
    if (isNew) emit savedMacrosChanged();
    else       emit savedMacrosChanged(); // same signal — keeps menu fresh
}

void MacroManager::deleteSaved(const QString& name)
{
    if (m_saved.remove(name) > 0) emit savedMacrosChanged();
}

bool MacroManager::renameSaved(const QString& oldName, const QString& newName)
{
    if (newName.isEmpty() || oldName == newName) return false;
    auto it = m_saved.find(oldName);
    if (it == m_saved.end()) return false;
    if (m_saved.contains(newName)) return false;   // name collision
    const Macro m = it.value();
    m_saved.erase(it);
    m_saved.insert(newName, m);
    emit savedMacrosChanged();
    return true;
}

void MacroManager::clearAll()
{
    m_saved.clear();
    m_lastMacro.clear();
    emit savedMacrosChanged();
    emit lastMacroChanged();
}

void MacroManager::loadFromXml(const pugi::xml_node& root)
{
    m_saved.clear();
    QSet<QString> seen;
    for (pugi::xml_node mn : root.children("Macro")) {
        const char* nameC = mn.attribute("name").value();
        if (!nameC || !*nameC) continue;
        const QString name = QString::fromUtf8(nameC);
        if (seen.contains(name)) continue;     // ignore duplicates
        seen.insert(name);

        Macro m;
        for (pugi::xml_node opn : mn.children("Op")) {
            MacroOp op;
            op.msg    = static_cast<unsigned int>(
                opn.attribute("msg").as_uint(0));
            op.wparam = static_cast<quintptr>(
                opn.attribute("wp").as_ullong(0));
            const char* lpStr = opn.attribute("lpStr").value();
            if (lpStr && *lpStr) {
                op.hasText = true;
                op.lparamText = QByteArray::fromBase64(QByteArray(lpStr));
            } else {
                op.lparamInt = static_cast<qintptr>(
                    opn.attribute("lp").as_llong(0));
            }
            if (op.msg != 0) m.append(op);
        }
        if (!m.isEmpty()) m_saved.insert(name, m);
    }
    emit savedMacrosChanged();
}

void MacroManager::writeToXml(pugi::xml_node& root) const
{
    for (auto it = m_saved.constBegin(); it != m_saved.constEnd(); ++it) {
        pugi::xml_node mn = root.append_child("Macro");
        const QByteArray nameUtf8 = it.key().toUtf8();
        mn.append_attribute("name") = nameUtf8.constData();

        for (const MacroOp& op : it.value()) {
            pugi::xml_node opn = mn.append_child("Op");
            opn.append_attribute("msg") = op.msg;
            opn.append_attribute("wp") =
                static_cast<unsigned long long>(op.wparam);
            if (op.hasText) {
                const QByteArray b64 = op.lparamText.toBase64();
                opn.append_attribute("lpStr") = b64.constData();
            } else {
                opn.append_attribute("lp") =
                    static_cast<long long>(op.lparamInt);
            }
        }
    }
}
