// Backup.h — periodic snapshot of dirty buffers + recovery on next launch.
//
// Two flavours of recovery feed the same on-disk format:
//
//   * **Hot exit** (clean app shutdown with dirty buffers). closeEvent
//     snapshots every dirty buffer, writes a `hot-exit-pending` marker,
//     and lets the buffers drop without prompting. Next launch sees
//     the marker → silently overlays the snapshots onto whatever the
//     session restored (or creates fresh Untitled tabs for orphan
//     content), and the user resumes exactly where they left off.
//
//   * **Crash recovery** (process killed without clean exit). The
//     periodic-snapshot timer left .bak files behind, but no marker.
//     Next launch finds .baks without the marker → shows the
//     "Recover unsaved work?" prompt.
//
// The backup index is a tiny pugixml document at
// ~/.config/padnote/padnote--/backup/index.xml — atomic .tmp + rename
// on every update. The .bak files themselves live in the same directory
// keyed by UUID.

#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

class Buffer;

namespace Backup {

struct Recovery {
    QString uuid;
    QString displayName;     // shown to the user in the recovery prompt
    QString originalPath;    // empty for untitled buffers
    QString backupPath;      // absolute path to the .bak file
    int     untitledIndex;   // 0 for file-bound; the Buffer's untitled
                             // index (N in "Untitled N") otherwise.
                             // Used by hot-exit overlay to bind a .bak
                             // back to the right tab when Session
                             // restored the Untitled placeholders.
};

// Idempotent setup; ensures the backup dir exists.
void init();

// Snapshot the buffer's current content. Reuses the buffer's stored
// uuid if it already has one. Updates the index. Called periodically
// by MainWindow's timer for dirty buffers.
void writeBuffer(Buffer* b);

// Remove the buffer's .bak (if any) + its index entry. Called on
// clean save and on clean close.
void clearBuffer(Buffer* b);

// Read the index; return everything that's still on disk. Called once
// at startup before MainWindow opens any tabs.
QVector<Recovery> pendingRecoveries();

// Wipe the index + every .bak file + the hot-exit marker. Called
// after a hot-exit restore completes, or after the user dismisses the
// crash-recovery prompt (whether they recovered or not — recovered
// content is now in live buffers, declined content gets explicitly
// thrown away).
void clearAll();

// Write the `hot-exit-pending` marker. Called in closeEvent AFTER all
// dirty buffers have been snapshotted, BEFORE buffers drop. Its
// presence on next launch flips the recovery path from
// crash-prompt → silent-overlay.
void markHotExit();

// Has the previous run left a hot-exit marker? main_qt.cpp checks
// this after Session::restore to decide whether to overlay backups
// silently or show the crash-recovery prompt.
bool isHotExitPending();

} // namespace Backup
