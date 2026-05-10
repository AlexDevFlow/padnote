// Session.h — open-tabs session persistence (per-pane).
//
// Saves the list of open files in each pane and per-file cursor position to
// $XDG_CONFIG_HOME/padnote/padnote--/session.xml on app close, and
// restores them on next launch (unless the user passed positional CLI
// arguments, which take priority).
//
// Schema (two-pane):
//
//   <Session activeView="0" splitVisible="yes">
//       <Pane index="0" activeIndex="N">
//           <File filename="/abs/path" position="123"/>
//           …
//       </Pane>
//       <Pane index="1" activeIndex="N">
//           <File … />
//       </Pane>
//   </Session>
//
// Forward-load of legacy single-pane sessions: if <Session> has direct
// <File> children but no <Pane> wrappers, treat them as pane 0 with
// splitVisible="no".
//
// Untitled / unsaved buffers are skipped — only files that exist on disk
// are saved. Files that no longer exist at restore time are silently
// skipped (with a warning to the status bar).

#pragma once

#include <QString>

class EditorTabs;

namespace Session {

// Read session.xml and reopen each listed file in the supplied panes.
// Returns the number of files successfully opened across both panes.
// outSplitVisible / outActiveView are set from the session attributes
// (defaults: false / 0) if they're non-null. The caller is responsible
// for showing/hiding the right pane based on outSplitVisible.
int restore(EditorTabs* left, EditorTabs* right,
            bool* outSplitVisible, int* outActiveView);

// Walk both panes and write session.xml. Buffers without a file path on
// disk are skipped. Always overwrites.
void save(EditorTabs* left, EditorTabs* right,
          bool splitVisible, int activeView);

// User-chosen path variants — File -> Load Session... / Save Session...
// (Phase 5j; expanded in 3d to round-trip both panes).
int  restoreFromFile(EditorTabs* left, EditorTabs* right,
                     bool* outSplitVisible, int* outActiveView,
                     const QString& path);
bool saveToFile     (EditorTabs* left, EditorTabs* right,
                     bool splitVisible, int activeView,
                     const QString& path);

} // namespace Session
