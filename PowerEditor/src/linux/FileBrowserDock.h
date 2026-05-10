// FileBrowserDock.h — Phase 3c.3: dockable file-tree panel.
//
// QFileSystemModel + QTreeView rooted at a user-chosen directory. Double-
// click on a file opens it in MainWindow's active pane via openFile(); on
// a directory, expands/collapses the row (default QTreeView behaviour).
// "Choose Folder..." button at the top of the dock changes the root and
// persists it via Config::setFileBrowserRoot.
//
// Hidden columns: size / type / date — the dock stays compact and the
// tree shows just file names. The model's filter is set to show files +
// directories (no system / hidden files by default), but symlinks are
// followed so the user can navigate into deps / etc.

#pragma once

#include <QDockWidget>
#include <QPointer>
#include <QString>

class MainWindow;
class QFileSystemModel;
class QLabel;
class QModelIndex;
class QTreeView;

class FileBrowserDock : public QDockWidget {
    Q_OBJECT
public:
    explicit FileBrowserDock(MainWindow* mw, QWidget* parent = nullptr);

    // Set the tree root. Empty / non-existing path → falls back to $HOME.
    // Persists through Config::setFileBrowserRoot when called from the
    // user-driven "Choose Folder..." path.
    void setRoot(const QString& path);

    QString currentRoot() const { return m_currentRoot; }

private slots:
    void onItemActivated(const QModelIndex& index);
    void onChooseFolder();
    // Phase 9j — right-click context menu on the tree.
    void onTreeContextMenu(const QPoint& localPos);

private:
    // Phase 9j — context menu actions. All take an absolute path
    // resolved from the right-clicked index (or the dock's root when
    // the user right-clicks empty space).
    void doRename(const QString& path);
    void doDelete(const QString& path);
    void doNewFile(const QString& parentDir);
    void doNewFolder(const QString& parentDir);
    void doOpenInExternal(const QString& path);

    MainWindow*        m_mw       = nullptr;
    QFileSystemModel*  m_model    = nullptr;
    QTreeView*         m_tree     = nullptr;
    QLabel*            m_pathLbl  = nullptr;
    QString            m_currentRoot;
};
