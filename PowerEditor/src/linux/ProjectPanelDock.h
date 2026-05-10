// ProjectPanelDock.h — Phase 3c.4: dockable workspace tree.
//
// MVP scope: ONE Project Panel (upstream offers three; users with
// multi-workspace habits can wait for 3c.4-polish). The dock holds a
// QTreeWidget rendering a workspace as Folders containing Files.
// Right-click context menu drives Add Folder / Add File / Remove /
// Rename plus the workspace-level Save / Load.
//
// Workspace XML schema — Phase 9p ports to upstream-compatible form:
//
//   <NotepadPlus>
//     <Project name="MyProject">
//       <Folder name="Sources">
//         <File name="src/main.cpp"/>          (relative to .xml's dir)
//         …
//       </Folder>
//       <File name="docs/readme.md"/>
//     </Project>
//   </NotepadPlus>
//
// File paths are relative to the workspace XML's directory (matches
// upstream's `getRelativePath` behaviour); files outside the tree
// keep their `../...` relative form.
//
// Backward compat: `loadWorkspace` still accepts the legacy 5U.4
// schema (`<Workspace name="...">` with `<File path="absolute"/>`) so
// existing user workspaces continue to load. On first save they get
// rewritten in the upstream format above.
//
// Multi-project (Phase 9q-polish): the dock now renders ALL `<Project>`
// siblings as separate top-level QTreeWidgetItems. Each project shows
// as its own root with its own Folders / Files children; the workspace
// is the file holding them. Add Project / Remove Project on the
// context menu let users grow the workspace. Save round-trips every
// top-level item back to a `<Project>` element. The panel does NOT
// track files for external rename / delete. Persisted via
// Config::setLastWorkspacePath so the dock auto-loads it next launch.

#pragma once

#include <QDockWidget>
#include <QPointer>
#include <QString>

class MainWindow;
class QPoint;
class QTreeWidget;
class QTreeWidgetItem;

class ProjectPanelDock : public QDockWidget {
    Q_OBJECT
public:
    enum class NodeKind { Workspace, Folder, File };

    // Phase 9q — panelIndex is 1-based (1, 2, or 3) and drives the
    // dock's title, objectName, and Config key (panel 1 = legacy
    // "ProjectPanel" key for backward compat). The single-arg
    // constructor below stays as a thin wrapper for any caller that
    // hasn't migrated; defaults to panel 1.
    explicit ProjectPanelDock(MainWindow* mw, int panelIndex,
                              QWidget* parent = nullptr);
    explicit ProjectPanelDock(MainWindow* mw, QWidget* parent = nullptr)
        : ProjectPanelDock(mw, 1, parent) {}

    int panelIndex() const { return m_panelIndex; }

    // Load a workspace XML. Empty / non-existing path → silent no-op
    // (used at startup with Config::lastWorkspacePath, which may be
    // unset on first run). Returns true on success.
    bool loadWorkspace(const QString& path);

    // Persist the current tree to `path`. Always overwrites. Returns
    // false if the file can't be written.
    bool saveWorkspace(const QString& path);

    QString currentWorkspacePath() const { return m_currentPath; }

private slots:
    void onItemActivated(QTreeWidgetItem* item, int column);
    void onContextMenu(const QPoint& pos);
    void onAddFolder();
    void onAddFile();
    void onAddProject();   // Phase 9q-polish — multi-project per panel
    void onRenameSelected();
    void onRemoveSelected();
    void onSaveAs();
    void onLoadFrom();
    void onNewWorkspace();

private:
    QTreeWidgetItem* selectedItem() const;
    QTreeWidgetItem* currentFolderForInsert();   // project root or selected folder
    QTreeWidgetItem* projectAncestorOf(QTreeWidgetItem* item) const;
    void             markDirty();   // future polish hook (autosave / *)

    MainWindow*  m_mw   = nullptr;
    QTreeWidget* m_tree = nullptr;
    QString      m_currentPath;     // empty = unsaved/new
    int          m_panelIndex = 1;  // Phase 9q — 1, 2, or 3
};
