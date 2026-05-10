// MacroManager.h — record / play / save Scintilla macros.
//
// Macros are global (one recording at a time for the whole app), and
// each recorded primitive is a Scintilla message ID + wparam + lparam
// triplet emitted via SCN_MACRORECORD. A singleton MacroManager
// subscribes to the active editor's macroRecord signal between
// Start/StopRecord, holds the "last macro" in memory after stop, and
// persists user-named macros to config.xml.
//
// Buffer.cpp queries `MacroManager::instance().isRecordingFor(editor)`
// in onCharAdded to suppress its programmatic auto-pair InsertText /
// DeleteRange sends — otherwise those would pollute the recording with
// closer-character ops the user didn't actually type.

#pragma once

#include <QByteArray>
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVector>

#include "ScintillaTypes.h"
#include "ScintillaMessages.h"   // Scintilla::Message enum used in slot signature

class ScintillaEditBase;

namespace pugi { class xml_node; }

// One recorded primitive op. lparamText is non-empty iff the message
// carries a const char* buffer (AddText, InsertText, ReplaceSel, …); for
// numeric-only messages (LineDown, GotoPos, …) we keep lparamInt.
struct MacroOp {
    unsigned int msg = 0;
    quintptr     wparam = 0;
    qintptr      lparamInt = 0;
    QByteArray   lparamText;
    bool         hasText = false;
};

using Macro = QVector<MacroOp>;

class MacroManager : public QObject {
    Q_OBJECT
public:
    static MacroManager& instance();

    // Recording state.
    bool isRecording() const;
    bool isRecordingFor(ScintillaEditBase* ed) const;

    // Last finished recording. Empty until the first Stop after at least
    // one captured op.
    bool         hasLastMacro() const;
    const Macro& lastMacro() const;

    // Saved (named) macros. Names are unique; saveLast(name) overwrites
    // an existing entry. Sorted alphabetically.
    QStringList savedNames() const;
    bool        hasSaved(const QString& name) const;

    // ---- Control ------------------------------------------------------
    void start(ScintillaEditBase* targetEditor);
    void stop();
    void play(const Macro& m, ScintillaEditBase* target, int count);
    void playLast(ScintillaEditBase* target, int count);
    void playSaved(const QString& name, ScintillaEditBase* target, int count);
    void saveLast(const QString& name);
    void deleteSaved(const QString& name);
    // Phase 9d.3 — rename an existing saved macro. Returns true on
    // success, false if oldName doesn't exist or newName collides with
    // a different saved macro. Emits savedMacrosChanged on success.
    bool renameSaved(const QString& oldName, const QString& newName);

    // ---- Persistence (called by Config::load / Config::save) ---------
    void loadFromXml(const pugi::xml_node& root);  // walks <Macro> children
    void writeToXml(pugi::xml_node& root) const;
    void clearAll();

signals:
    void recordingStateChanged(bool recording);
    void lastMacroChanged();
    void savedMacrosChanged();

private slots:
    void onMacroRecord(Scintilla::Message msg,
                       Scintilla::uptr_t  wp,
                       Scintilla::sptr_t  lp);
    void onRecordingEditorDestroyed();

private:
    explicit MacroManager(QObject* parent = nullptr);
    MacroManager(const MacroManager&) = delete;
    MacroManager& operator=(const MacroManager&) = delete;

    bool                          m_recording = false;
    QPointer<ScintillaEditBase>   m_recordingEditor;
    Macro                         m_currentRecording;
    Macro                         m_lastMacro;
    QMap<QString, Macro>          m_saved;
};
