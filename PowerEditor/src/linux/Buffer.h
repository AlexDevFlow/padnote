// Buffer.h — one document = one ScintillaEditBase + filepath + dirty bit
//                                                               + language.

#pragma once

#include <QChar>
#include <QMetaType>
#include <QObject>
#include <QString>

#include "Encoding.h"

#include "ScintillaTypes.h"   // Update / Position / KeyMod (Phase 9e)

class ScintillaEditBase;
class QWidget;
struct LanguageDef;

class Buffer : public QObject {
    Q_OBJECT
public:
    // Maps to Scintilla's SC_EOL_CRLF=0, SC_EOL_CR=1, SC_EOL_LF=2 — the raw
    // values are NOT sequential, so always go through Buffer.cpp's switch.
    enum class EolMode { Crlf, Cr, Lf };

    explicit Buffer(QWidget* parent);
    ~Buffer() override;

    ScintillaEditBase* editor() const { return m_editor; }
    QString filePath() const { return m_filePath; }
    QString displayName() const;     // basename, or "Untitled N"
    bool isDirty() const { return m_dirty; }
    bool hasFile() const { return !m_filePath.isEmpty(); }
    const LanguageDef* language() const { return m_language; }
    EncodingInfo encoding() const { return m_encoding; }
    EolMode eolMode() const { return m_eolMode; }

    // mtime of the file on disk at the moment we last loaded or wrote it.
    // Used by Phase 5T's file-watcher to distinguish "we just saved" from
    // "an external editor wrote to this path." 0 for untitled buffers.
    qint64 lastKnownMtime() const { return m_lastKnownMtime; }
    void   refreshMtime();

    // Phase 5AA — backup UUID for crash-recovery snapshots. Set on
    // first Backup::writeBuffer; cleared on Backup::clearBuffer.
    QString backupUuid() const { return m_backupUuid; }
    void    setBackupUuid(const QString& u) { m_backupUuid = u; }

    bool loadFromFile(const QString& path, QString* errorOut = nullptr);
    bool saveToFile(const QString& path, QString* errorOut = nullptr);

    // Encode + write the buffer's content to `path` without touching
    // m_filePath, m_dirty, or Scintilla's save-point. Used by File ->
    // Save a Copy As (Phase 5j).
    bool writeCopyTo(const QString& path, QString* errorOut = nullptr);

    void setFilePath(const QString& path);
    void setUntitledIndex(int n) { m_untitledIndex = n; }

    // Attach a Lexilla lexer + apply theme for the language. Pass nullptr
    // (or the "text" entry) for plain text. Re-styles the whole buffer.
    void setLanguage(const LanguageDef* lang);

    // Re-apply the current theme to this buffer (used when the user toggles
    // Light/Dark in the View menu — the LanguageDef hasn't changed but the
    // theme has).
    void reapplyTheme();

    // Phase 5MK — Mark feature. Highlights every occurrence of `text`
    // in this buffer using one of 5 distinct indicator slots (markN
    // is 1..5). Match-case / whole-word flags mirror the find search
    // semantics. Marks persist until clearAllMarks() or the buffer
    // is re-loaded; they are NOT saved across restart (mirrors
    // upstream's behaviour). The colour for each mark slot is
    // pulled from `Theme::markStyleFore(markN)` so dark themes get
    // calm distinct colours.
    int  markAllOccurrences(const QString& text, int markN,
                            bool matchCase, bool wholeWord);
    void clearAllMarks();

    // Re-read the file from disk and decode it as the given encoding,
    // replacing the buffer's content. Returns false if there's no file path
    // or the read fails. Used by the Encoding menu's "treat this file as X"
    // entries.
    bool reloadAsEncoding(const EncodingInfo& enc, QString* errorOut = nullptr);

    // Mark this buffer as having a different encoding for the next save,
    // without changing the visible content. Used by "Convert to X" entries.
    // Marks the buffer dirty (because saving will produce different bytes).
    void convertToEncoding(const EncodingInfo& enc);

    // Record the EOL mode for *newly-typed* lines. Existing line endings
    // in the buffer are left untouched. Doesn't mark the buffer dirty.
    void setEolMode(EolMode mode);

    // Rewrite every line ending in the buffer to `mode`. One undo step.
    // Scintilla emits savePointChanged naturally, which marks the buffer
    // dirty.
    void convertEol(EolMode mode);

    // Phase 9c.1 — user-set read-only flag. SCI_SETREADONLY blocks edits;
    // the flag is purely in-memory (not persisted to session.xml — when
    // the user reopens the file, it loads writable). Distinct from the
    // file-on-disk read-only bit (Phase 9c.1 doesn't read that today;
    // a future polish phase could surface "[RO on disk]" separately).
    bool isReadOnly() const { return m_readOnly; }
    void setReadOnly(bool ro);

    // Phase 9n — user-set pinned flag. Pinned tabs render with a pin
    // icon and refuse Ctrl+W / Close / Close-All-but-Current until
    // the user unpins (status-bar nudge fires on the close attempt).
    // Transient like m_readOnly — not persisted to session.xml so a
    // restart resets every tab to unpinned. Auto-reordering of pinned
    // tabs to the leftmost positions is intentionally NOT done in the
    // MVP (would conflict with drag-reorder + split-view; revisit if
    // demand surfaces).
    bool isPinned() const { return m_pinned; }
    void setPinned(bool pinned);

    // Phase 9c.2 — clear the smart-highlight indicator AND the cached
    // last-highlighted word, forcing the next onUpdateUi to recompute
    // with potentially-new search flags. Called from
    // MainWindow::applyEditingPrefsFromConfig when the user changes
    // match-case / whole-word in Preferences.
    void clearSmartHighlightCache();

    // Phase 9f — push tab width / indent / use-tabs to Scintilla based
    // on the active language's per-language override (if any) or
    // Config::defaultTabWidth() / defaultUseTabs() otherwise. Called
    // from the ctor (initial state) and from
    // MainWindow::applyEditingPrefsFromConfig (live update on
    // Preferences Apply). setLanguage also applies it on every
    // language switch.
    void applyResolvedIndent();

    // Phase 9i — show the autocomplete popup with the current word as
    // prefix. `manualTrigger=true` means the user invoked it explicitly
    // (Edit → Function and word completion / Ctrl+Space): empty buffers,
    // empty prefix, and zero candidates surface a status nudge.
    // `manualTrigger=false` is the auto-trigger path: silent on miss.
    // Returns true if the popup was actually shown.
    bool triggerAutocomplete(bool manualTrigger);

signals:
    void dirtyChanged(Buffer* self, bool dirty);
    void displayNameChanged(Buffer* self);
    void languageChanged(Buffer* self);
    void encodingChanged(Buffer* self);
    void eolModeChanged(Buffer* self);
    void readOnlyChanged(Buffer* self);
    void pinnedChanged(Buffer* self);

private slots:
    void onSavePointChanged(bool dirty);
    // Phase 5X — smart-edit hooks
    void onCharAdded(int ch);
    void onUpdateUi(Scintilla::Update updated);
    // Phase 9e — fold-margin click → toggle fold. Signature mirrors
    // ScintillaEditBase::marginClicked. We only react when margin
    // index matches kMarginFold (defined in Buffer.cpp's anon ns).
    void onMarginClicked(Scintilla::Position position,
                         Scintilla::KeyMod   modifiers,
                         int                 margin);

private:
    ScintillaEditBase* m_editor;
    QString            m_filePath;
    bool               m_dirty;
    int                m_untitledIndex;
    const LanguageDef* m_language;
    EncodingInfo       m_encoding;
    EolMode            m_eolMode;
    qint64             m_lastKnownMtime = 0;

    // Phase 5X — smart-edit per-buffer state.
    QString m_smartHighlightWord;     // current marked word; "" = none
    QChar   m_pendingPairCloser;      // null QChar() = no pending pair
    int     m_pendingPairPos = -1;    // pos in buffer where the auto-closer sits

    // Phase 5AA — Backup module-managed UUID; "" = no backup yet.
    QString m_backupUuid;

    // Phase 9c.1 — user-toggled read-only flag (transient, per buffer).
    bool m_readOnly = false;

    // Phase 9n — user-toggled pinned flag (transient, per buffer).
    bool m_pinned = false;
};

Q_DECLARE_METATYPE(Buffer*)
