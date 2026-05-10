// Backup.h — periodic snapshot of dirty buffers + crash recovery on
// next launch.
//
// On a clean save or close, the snapshot for that buffer is deleted.
// On a crash (process killed without clean exit), the snapshot is
// preserved; next launch reads the backup index and offers to recover
// every still-present .bak as a fresh Untitled buffer with the original
// display name preserved.
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

// Wipe the index + every .bak file under the backup dir. Called after
// the user dismisses the recovery prompt (whether they recovered or
// not — recovered content is now in live buffers, declined content
// gets explicitly thrown away).
void clearAll();

} // namespace Backup
