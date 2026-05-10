#include "ProjectPanelDock.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QInputDialog>
#include <QMenu>
#include <QMessageBox>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>
#include <QWidget>

#include <cstring>

#include "Config.h"
#include "EditorTabs.h"
#include "MainWindow.h"

#include "pugixml.hpp"

namespace {

constexpr int kDockDefaultWidth = 240;

// Per-item kind stored in Qt::UserRole. Filenames live in
// Qt::UserRole + 1 for File rows.
constexpr int kRoleKind = Qt::UserRole;
constexpr int kRolePath = Qt::UserRole + 1;

void tagItem(QTreeWidgetItem* item, ProjectPanelDock::NodeKind kind,
             const QString& path = {})
{
    item->setData(0, kRoleKind, static_cast<int>(kind));
    if (!path.isEmpty()) item->setData(0, kRolePath, path);
    if (kind == ProjectPanelDock::NodeKind::File) {
        item->setToolTip(0, path);
    }
}

ProjectPanelDock::NodeKind kindOf(const QTreeWidgetItem* item)
{
    if (!item) return ProjectPanelDock::NodeKind::Workspace;
    return static_cast<ProjectPanelDock::NodeKind>(
        item->data(0, kRoleKind).toInt());
}

QString pathOf(const QTreeWidgetItem* item)
{
    return item ? item->data(0, kRolePath).toString() : QString();
}

} // namespace

ProjectPanelDock::ProjectPanelDock(MainWindow* mw, int panelIndex, QWidget* parent)
    : QDockWidget(panelIndex == 1
                  ? tr("Project Panel")
                  : tr("Project Panel %1").arg(panelIndex), parent),
      m_mw(mw),
      m_panelIndex(panelIndex)
{
    // Phase 9q — panel 1 keeps the legacy objectName so QMainWindow's
    // saveState/restoreState binary blob from older configs places it
    // in the same dock area on upgrade. Panels 2/3 take suffixed
    // objectNames so their state is tracked independently.
    setObjectName(panelIndex == 1
                  ? QStringLiteral("projectPanelDock")
                  : QStringLiteral("projectPanelDock%1").arg(panelIndex));
    setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    setFeatures(QDockWidget::DockWidgetClosable
              | QDockWidget::DockWidgetMovable
              | QDockWidget::DockWidgetFloatable);

    auto* root = new QWidget(this);
    auto* lay = new QVBoxLayout(root);
    lay->setContentsMargins(4, 4, 4, 4);
    lay->setSpacing(4);

    m_tree = new QTreeWidget(root);
    m_tree->setHeaderHidden(true);
    m_tree->setUniformRowHeights(true);
    m_tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    lay->addWidget(m_tree, 1);

    setWidget(root);
    root->resize(kDockDefaultWidth, root->height());

    // Workspace root — always present; rename happens via "New Workspace"
    // / "Load Workspace" / context menu rename.
    auto* wsRoot = new QTreeWidgetItem(m_tree);
    wsRoot->setText(0, tr("(Empty Workspace)"));
    tagItem(wsRoot, NodeKind::Workspace);
    wsRoot->setExpanded(true);

    connect(m_tree, &QTreeWidget::itemActivated,
            this, &ProjectPanelDock::onItemActivated);
    connect(m_tree, &QTreeWidget::itemDoubleClicked,
            this, &ProjectPanelDock::onItemActivated);
    connect(m_tree, &QWidget::customContextMenuRequested,
            this, &ProjectPanelDock::onContextMenu);
}

void ProjectPanelDock::markDirty()
{
    // Future polish hook (autosave / unsaved-changes star). MVP no-op.
}

QTreeWidgetItem* ProjectPanelDock::selectedItem() const
{
    return m_tree ? m_tree->currentItem() : nullptr;
}

QTreeWidgetItem* ProjectPanelDock::projectAncestorOf(QTreeWidgetItem* item) const
{
    // Walk up to the top-level item. With multi-project (Phase
    // 9q-polish), top-level items are NodeKind::Workspace nodes (the
    // historical name; semantically a project root).
    while (item && item->parent()) item = item->parent();
    return item;
}

QTreeWidgetItem* ProjectPanelDock::currentFolderForInsert()
{
    QTreeWidgetItem* sel = selectedItem();
    if (sel && (kindOf(sel) == NodeKind::Workspace
             || kindOf(sel) == NodeKind::Folder)) {
        return sel;
    }
    if (sel && kindOf(sel) == NodeKind::File && sel->parent()) {
        return sel->parent();
    }
    // Fallback: the first project root. With multi-project, "first"
    // is the leftmost top-level item; we don't pick a "current
    // project" intelligently because the user almost always selects
    // the target before Add Folder / Add File.
    return m_tree && m_tree->topLevelItemCount() > 0
        ? m_tree->topLevelItem(0) : nullptr;
}

// -----------------------------------------------------------------------------
// Tree manipulation
// -----------------------------------------------------------------------------

void ProjectPanelDock::onItemActivated(QTreeWidgetItem* item, int /*column*/)
{
    if (!item || !m_mw) return;
    if (kindOf(item) != NodeKind::File) return;
    const QString path = pathOf(item);
    if (path.isEmpty()) return;
    EditorTabs* pane = m_mw->activePane();
    if (!pane) return;
    pane->openFile(path);
}

void ProjectPanelDock::onContextMenu(const QPoint& pos)
{
    QTreeWidgetItem* item = m_tree ? m_tree->itemAt(pos) : nullptr;
    if (item) m_tree->setCurrentItem(item);

    QMenu menu(this);
    QAction* aAddProject = menu.addAction(tr("Add &Project..."));
    QAction* aAddFolder  = menu.addAction(tr("Add &Folder..."));
    QAction* aAddFile    = menu.addAction(tr("Add &File(s)..."));
    menu.addSeparator();
    QAction* aRename     = menu.addAction(tr("&Rename..."));
    QAction* aRemove     = menu.addAction(tr("Re&move"));
    menu.addSeparator();
    QAction* aSaveAs     = menu.addAction(tr("&Save Workspace As..."));
    QAction* aLoad       = menu.addAction(tr("&Load Workspace..."));
    QAction* aNew        = menu.addAction(tr("&New Workspace"));

    const NodeKind k = kindOf(item);
    aAddProject->setEnabled(true);  // always available — adds a sibling project
    aAddFile->setEnabled(item != nullptr);
    aAddFolder->setEnabled(item == nullptr || k != NodeKind::File);
    aRename->setEnabled(item != nullptr
                     && (k == NodeKind::Workspace || k == NodeKind::Folder));
    // Phase 9q-polish — top-level (Project root) removal is now allowed
    // (the workspace IS the file, not the first project). Removing the
    // last project leaves an empty tree; New Workspace re-seeds.
    aRemove->setEnabled(item != nullptr);

    QAction* picked = menu.exec(m_tree->viewport()->mapToGlobal(pos));
    if (!picked) return;
    if      (picked == aAddProject) onAddProject();
    else if (picked == aAddFolder)  onAddFolder();
    else if (picked == aAddFile)    onAddFile();
    else if (picked == aRename)     onRenameSelected();
    else if (picked == aRemove)     onRemoveSelected();
    else if (picked == aSaveAs)     onSaveAs();
    else if (picked == aLoad)       onLoadFrom();
    else if (picked == aNew)        onNewWorkspace();
}

void ProjectPanelDock::onAddFolder()
{
    QTreeWidgetItem* parent = currentFolderForInsert();
    if (!parent) return;
    bool ok = false;
    const QString name = QInputDialog::getText(this,
        tr("Add Folder"), tr("Folder name:"), QLineEdit::Normal,
        QString(), &ok).trimmed();
    if (!ok || name.isEmpty()) return;

    auto* folder = new QTreeWidgetItem(parent);
    folder->setText(0, name);
    tagItem(folder, NodeKind::Folder);
    parent->setExpanded(true);
    markDirty();
}

void ProjectPanelDock::onAddFile()
{
    QTreeWidgetItem* parent = currentFolderForInsert();
    if (!parent) return;
    const QStringList files = QFileDialog::getOpenFileNames(this,
        tr("Add file(s) to project"), QDir::homePath());
    if (files.isEmpty()) return;
    for (const QString& f : files) {
        const QString abs = QFileInfo(f).absoluteFilePath();
        auto* fileItem = new QTreeWidgetItem(parent);
        fileItem->setText(0, QFileInfo(abs).fileName());
        tagItem(fileItem, NodeKind::File, abs);
    }
    parent->setExpanded(true);
    markDirty();
}

void ProjectPanelDock::onRenameSelected()
{
    QTreeWidgetItem* sel = selectedItem();
    if (!sel) return;
    const NodeKind k = kindOf(sel);
    if (k == NodeKind::File) return;       // file rows show on-disk filename

    bool ok = false;
    const QString name = QInputDialog::getText(this,
        tr("Rename"),
        k == NodeKind::Workspace ? tr("Workspace name:") : tr("Folder name:"),
        QLineEdit::Normal, sel->text(0), &ok).trimmed();
    if (!ok || name.isEmpty()) return;
    sel->setText(0, name);
    markDirty();
}

void ProjectPanelDock::onAddProject()
{
    if (!m_tree) return;
    bool ok = false;
    const QString name = QInputDialog::getText(this,
        tr("Add Project"), tr("Project name:"), QLineEdit::Normal,
        tr("New Project"), &ok).trimmed();
    if (!ok || name.isEmpty()) return;

    auto* projRoot = new QTreeWidgetItem(m_tree);
    projRoot->setText(0, name);
    tagItem(projRoot, NodeKind::Workspace);   // top-level == project root
    projRoot->setExpanded(true);
    m_tree->setCurrentItem(projRoot);
    markDirty();
}

void ProjectPanelDock::onRemoveSelected()
{
    QTreeWidgetItem* sel = selectedItem();
    if (!sel) return;
    QTreeWidgetItem* parent = sel->parent();
    if (parent) {
        parent->removeChild(sel);
        delete sel;
    } else if (m_tree) {
        // Top-level removal — drop a whole project. With multi-project
        // (Phase 9q-polish), removing one project leaves the rest;
        // removing the last leaves an empty tree until New Workspace
        // / Add Project re-seeds.
        const int idx = m_tree->indexOfTopLevelItem(sel);
        if (idx >= 0) {
            QTreeWidgetItem* taken = m_tree->takeTopLevelItem(idx);
            delete taken;
        }
    }
    markDirty();
}

void ProjectPanelDock::onNewWorkspace()
{
    if (!m_tree) return;
    // Drop ALL existing top-level items, not just the first — with
    // multi-project the workspace might hold N projects.
    while (m_tree->topLevelItemCount() > 0) {
        QTreeWidgetItem* old = m_tree->takeTopLevelItem(0);
        delete old;
    }
    auto* projRoot = new QTreeWidgetItem(m_tree);
    projRoot->setText(0, tr("(Empty Workspace)"));
    tagItem(projRoot, NodeKind::Workspace);
    projRoot->setExpanded(true);
    m_currentPath.clear();
    Config::setLastWorkspacePath(m_panelIndex, QString());
    Config::save();
}

// -----------------------------------------------------------------------------
// XML serialisation
// -----------------------------------------------------------------------------

namespace {

// Phase 9p — emit upstream-compatible `<File name="relative-path"/>`
// instead of the legacy `<File path="absolute-path"/>`. Paths are made
// relative to the workspace XML's directory; files outside that tree
// retain their `../...` relative form (consistent with upstream's
// behaviour). Folders nest naturally as `<Folder name="...">` blocks.
void writeChildren(QTreeWidgetItem* qParent, pugi::xml_node& parent,
                   const QDir& workspaceDir)
{
    if (!qParent) return;
    for (int i = 0; i < qParent->childCount(); ++i) {
        QTreeWidgetItem* c = qParent->child(i);
        const auto k = kindOf(c);
        if (k == ProjectPanelDock::NodeKind::Folder) {
            pugi::xml_node fn = parent.append_child("Folder");
            const QByteArray nm = c->text(0).toUtf8();
            fn.append_attribute("name") = nm.constData();
            writeChildren(c, fn, workspaceDir);
        } else if (k == ProjectPanelDock::NodeKind::File) {
            pugi::xml_node fn = parent.append_child("File");
            const QString abs = pathOf(c);
            const QString rel = workspaceDir.relativeFilePath(abs);
            const QByteArray pa = rel.toUtf8();
            fn.append_attribute("name") = pa.constData();
        }
    }
}

// Phase 9p — accept both upstream `<File name="...">` (relative path)
// and legacy linux-port `<File path="...">` (absolute). Resolves
// relative paths against `workspaceDir` (the directory holding the
// workspace XML).
void readChildren(QTreeWidgetItem* qParent, const pugi::xml_node& parent,
                  const QDir& workspaceDir)
{
    if (!qParent) return;
    for (pugi::xml_node n : parent.children()) {
        if (std::strcmp(n.name(), "Folder") == 0) {
            const char* nm = n.attribute("name").value();
            auto* folder = new QTreeWidgetItem(qParent);
            folder->setText(0, QString::fromUtf8(nm && *nm ? nm : "Folder"));
            tagItem(folder, ProjectPanelDock::NodeKind::Folder);
            folder->setExpanded(true);
            readChildren(folder, n, workspaceDir);
        } else if (std::strcmp(n.name(), "File") == 0) {
            // Upstream: <File name="rel/path">. Legacy: <File path="abs">.
            // Whichever populates wins; preferring `name` matches the
            // upstream-canonical schema we now write back as.
            const char* nameAttr = n.attribute("name").value();
            const char* pathAttr = n.attribute("path").value();
            QString resolved;
            if (nameAttr && *nameAttr) {
                resolved = workspaceDir.absoluteFilePath(
                    QString::fromUtf8(nameAttr));
                resolved = QDir::cleanPath(resolved);
            } else if (pathAttr && *pathAttr) {
                resolved = QString::fromUtf8(pathAttr);
            } else {
                continue;
            }
            auto* file = new QTreeWidgetItem(qParent);
            file->setText(0, QFileInfo(resolved).fileName());
            tagItem(file, ProjectPanelDock::NodeKind::File, resolved);
        }
    }
}

} // namespace

bool ProjectPanelDock::saveWorkspace(const QString& path)
{
    if (path.isEmpty() || !m_tree || m_tree->topLevelItemCount() == 0)
        return false;

    pugi::xml_document doc;
    pugi::xml_node decl = doc.append_child(pugi::node_declaration);
    decl.append_attribute("version") = "1.0";
    decl.append_attribute("encoding") = "UTF-8";

    // Phase 9p — emit upstream-compatible `<Project>` (was `<Workspace>`).
    // Phase 9q-polish — emit ONE `<Project>` per top-level item so
    // multi-project workspaces round-trip. Cross-fork compat: every
    // file written here loads in upstream NPP on Wine without
    // modification (single-project files match the schema upstream's
    // own `Save Workspace As` produces; multi-project files get parsed
    // by upstream's reader which iterates `<Project>` siblings).
    pugi::xml_node root = doc.append_child("NotepadPlus");
    const QDir workspaceDir = QFileInfo(path).absoluteDir();
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* projRoot = m_tree->topLevelItem(i);
        pugi::xml_node ws = root.append_child("Project");
        const QByteArray nm = projRoot->text(0).toUtf8();
        ws.append_attribute("name") = nm.constData();
        writeChildren(projRoot, ws, workspaceDir);
    }

    struct Writer : pugi::xml_writer {
        QByteArray bytes;
        void write(const void* data, size_t size) override {
            bytes.append(static_cast<const char*>(data),
                         static_cast<int>(size));
        }
    } w;
    doc.save(w, "    ");

    const QString tmp = path + QStringLiteral(".tmp");
    QFile f(tmp);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    f.write(w.bytes);
    f.close();
    QFile::remove(path);
    if (!QFile::rename(tmp, path)) return false;

    m_currentPath = path;
    Config::setLastWorkspacePath(m_panelIndex, path);
    Config::save();
    return true;
}

bool ProjectPanelDock::loadWorkspace(const QString& path)
{
    if (path.isEmpty()) return false;
    if (!QFileInfo(path).isFile()) return false;

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray bytes = f.readAll();
    f.close();
    if (bytes.isEmpty()) return false;

    pugi::xml_document doc;
    if (!doc.load_buffer(bytes.constData(), static_cast<size_t>(bytes.size())))
        return false;

    // Phase 9p — accept BOTH the legacy `<Workspace>` element (5U.4
    // linux-port format) AND upstream's `<Project>` (one or more
    // siblings per file).
    // Phase 9q-polish — multiple `<Project>` siblings now load as
    // separate top-level items. Single-project files still load
    // identically (one top-level item).
    const pugi::xml_node npp = doc.child("NotepadPlus");

    // Collect all root nodes: legacy <Workspace> first (one only),
    // then every <Project> sibling.
    QVector<pugi::xml_node> projectNodes;
    if (auto legacy = npp.child("Workspace")) {
        projectNodes.append(legacy);
    }
    for (pugi::xml_node p = npp.child("Project"); p;
         p = p.next_sibling("Project"))
    {
        projectNodes.append(p);
    }
    if (projectNodes.isEmpty()) return false;

    // Replace the existing tree.
    while (m_tree->topLevelItemCount() > 0) {
        QTreeWidgetItem* old = m_tree->takeTopLevelItem(0);
        delete old;
    }

    const QDir workspaceDir = QFileInfo(path).absoluteDir();
    for (const pugi::xml_node& ws : projectNodes) {
        auto* projRoot = new QTreeWidgetItem(m_tree);
        const char* nm = ws.attribute("name").value();
        projRoot->setText(0, QString::fromUtf8(nm && *nm ? nm : "Project"));
        tagItem(projRoot, NodeKind::Workspace);
        projRoot->setExpanded(true);
        readChildren(projRoot, ws, workspaceDir);
    }

    m_currentPath = path;
    return true;
}

void ProjectPanelDock::onSaveAs()
{
    const QString path = QFileDialog::getSaveFileName(this,
        tr("Save Workspace As"),
        m_currentPath.isEmpty() ? QDir::homePath() + QStringLiteral("/workspace.xml")
                                 : m_currentPath,
        tr("Workspace XML (*.xml);;All files (*)"));
    if (path.isEmpty()) return;
    if (!saveWorkspace(path)) {
        QMessageBox::warning(this, tr("Save Workspace"),
            tr("Could not write workspace to %1").arg(path));
    }
}

void ProjectPanelDock::onLoadFrom()
{
    const QString path = QFileDialog::getOpenFileName(this,
        tr("Load Workspace"), QDir::homePath(),
        tr("Workspace XML (*.xml);;All files (*)"));
    if (path.isEmpty()) return;
    if (!loadWorkspace(path)) {
        QMessageBox::warning(this, tr("Load Workspace"),
            tr("Could not read workspace from %1").arg(path));
        return;
    }
    Config::setLastWorkspacePath(m_panelIndex, path);
    Config::save();
}
