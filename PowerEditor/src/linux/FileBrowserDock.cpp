#include "FileBrowserDock.h"

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QStatusBar>
#include <QTreeView>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>

#include "Config.h"
#include "EditorTabs.h"
#include "MainWindow.h"

namespace {

constexpr int kDockDefaultWidth = 240;

// Resolve a saved/requested root path to something we'll actually root at.
// Falls back to $HOME if the path is empty or doesn't exist.
QString resolveRoot(const QString& wanted)
{
    if (!wanted.isEmpty() && QFileInfo(wanted).isDir()) return wanted;
    return QDir::homePath();
}

} // namespace

FileBrowserDock::FileBrowserDock(MainWindow* mw, QWidget* parent)
    : QDockWidget(tr("File Browser"), parent),
      m_mw(mw)
{
    setObjectName(QStringLiteral("fileBrowserDock"));
    setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    setFeatures(QDockWidget::DockWidgetClosable
              | QDockWidget::DockWidgetMovable
              | QDockWidget::DockWidgetFloatable);

    auto* root = new QWidget(this);
    auto* lay = new QVBoxLayout(root);
    lay->setContentsMargins(4, 4, 4, 4);
    lay->setSpacing(4);

    // Header: "Choose Folder..." button + current path label (elided).
    auto* header = new QHBoxLayout;
    header->setSpacing(4);
    auto* btn = new QPushButton(tr("&Choose Folder..."), root);
    m_pathLbl = new QLabel(root);
    // Compact dock; label can be narrow. Long paths are visible via tooltip
    // (set in setRoot) — Qt has no per-QLabel elide so we elide ourselves
    // when the label gets a path.
    m_pathLbl->setMinimumWidth(40);
    header->addWidget(btn);
    header->addWidget(m_pathLbl, 1);
    lay->addLayout(header);

    // Model + tree. Show only the name column to keep the dock compact.
    m_model = new QFileSystemModel(this);
    m_model->setRootPath(QString());     // initial; setRoot() updates below
    m_model->setOption(QFileSystemModel::DontWatchForChanges, false);

    m_tree = new QTreeView(root);
    m_tree->setModel(m_model);
    for (int col = 1; col < m_model->columnCount(); ++col) {
        m_tree->setColumnHidden(col, true);
    }
    m_tree->header()->hide();
    m_tree->setAnimated(false);
    m_tree->setSortingEnabled(false);
    m_tree->setUniformRowHeights(true);
    m_tree->setExpandsOnDoubleClick(true);
    m_tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    lay->addWidget(m_tree, 1);

    setWidget(root);
    root->resize(kDockDefaultWidth, root->height());

    connect(btn, &QPushButton::clicked, this, &FileBrowserDock::onChooseFolder);
    connect(m_tree, &QTreeView::activated,
            this, &FileBrowserDock::onItemActivated);
    // Single-click + Enter doesn't always emit activated on every Qt style;
    // doubleClicked is the more reliable file-open trigger. Wire both.
    connect(m_tree, &QTreeView::doubleClicked,
            this, &FileBrowserDock::onItemActivated);

    // Phase 9j — right-click → context menu (Rename / Delete / New /
    // Open in External / Copy Path). Right-click on empty area (no
    // selection) targets the dock's current root.
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_tree, &QTreeView::customContextMenuRequested,
            this, &FileBrowserDock::onTreeContextMenu);

    // Initial root: persisted preference, else $HOME.
    setRoot(Config::fileBrowserRoot());
}

void FileBrowserDock::setRoot(const QString& path)
{
    const QString resolved = resolveRoot(path);
    if (resolved == m_currentRoot) return;
    m_currentRoot = resolved;

    // QFileSystemModel emits signals as the directory loads; setRootIndex
    // on the tree narrows the view to the chosen folder.
    const QModelIndex idx = m_model->setRootPath(resolved);
    m_tree->setRootIndex(idx);
    m_tree->collapseAll();

    // Show the absolute path. Manual elide so the label stays compact —
    // QLabel doesn't support setTextElideMode like QListView does. Full
    // path lives in the tooltip for hover discovery.
    if (m_pathLbl) {
        const QString native = QDir::toNativeSeparators(resolved);
        m_pathLbl->setToolTip(native);
        const QFontMetrics fm(m_pathLbl->font());
        m_pathLbl->setText(fm.elidedText(native, Qt::ElideMiddle,
                                         std::max(60, m_pathLbl->width())));
    }
}

void FileBrowserDock::onChooseFolder()
{
    const QString chosen = QFileDialog::getExistingDirectory(this,
        tr("Choose folder"), m_currentRoot);
    if (chosen.isEmpty()) return;
    setRoot(chosen);
    Config::setFileBrowserRoot(chosen);
    Config::save();
}

void FileBrowserDock::onItemActivated(const QModelIndex& index)
{
    if (!index.isValid() || !m_model || !m_mw) return;
    const QFileInfo fi = m_model->fileInfo(index);
    if (fi.isDir()) {
        // Let the default expand/collapse handle directories; activate also
        // fires when the user presses Enter, in which case the tree's own
        // expand toggling has already handled it.
        return;
    }
    if (!fi.isFile()) return;
    EditorTabs* pane = m_mw->activePane();
    if (!pane) return;
    pane->openFile(fi.absoluteFilePath());
}

// =============================================================================
// Phase 9j — Right-click context menu.
//
// Right-click on a file: Open / Open in External / Rename / Delete / Copy Path.
// Right-click on a directory: Open in External / Rename / Delete / New File /
//                              New Folder / Copy Path.
// Right-click on empty area: New File / New Folder (in current root) / Copy
//                             Path (root path).
// =============================================================================

void FileBrowserDock::onTreeContextMenu(const QPoint& localPos)
{
    if (!m_tree || !m_model) return;
    const QModelIndex idx = m_tree->indexAt(localPos);
    QString targetPath;
    bool isDir = false;
    bool isFile = false;
    if (idx.isValid()) {
        const QFileInfo fi = m_model->fileInfo(idx);
        targetPath = fi.absoluteFilePath();
        isDir = fi.isDir();
        isFile = fi.isFile();
    } else {
        // Empty-area click → operations target the dock's current root.
        targetPath = m_currentRoot;
        isDir = true;
    }

    QMenu menu(this);

    if (isFile) {
        QAction* aOpen = menu.addAction(tr("&Open"));
        connect(aOpen, &QAction::triggered, this, [this, targetPath]{
            if (!m_mw || !m_mw->activePane()) return;
            m_mw->activePane()->openFile(targetPath);
        });
    }
    QAction* aOpenExt = menu.addAction(tr("Open in &External"));
    connect(aOpenExt, &QAction::triggered, this, [this, targetPath]{
        doOpenInExternal(targetPath);
    });
    menu.addSeparator();
    if (isDir) {
        QAction* aNewFile = menu.addAction(tr("New &File..."));
        connect(aNewFile, &QAction::triggered, this, [this, targetPath]{
            doNewFile(targetPath);
        });
        QAction* aNewFolder = menu.addAction(tr("New F&older..."));
        connect(aNewFolder, &QAction::triggered, this, [this, targetPath]{
            doNewFolder(targetPath);
        });
        menu.addSeparator();
    }
    if (idx.isValid()) {
        QAction* aRename = menu.addAction(tr("&Rename..."));
        connect(aRename, &QAction::triggered, this, [this, targetPath]{
            doRename(targetPath);
        });
        QAction* aDelete = menu.addAction(tr("&Delete"));
        connect(aDelete, &QAction::triggered, this, [this, targetPath]{
            doDelete(targetPath);
        });
        menu.addSeparator();
    }
    QAction* aCopyPath = menu.addAction(tr("Copy &Path"));
    connect(aCopyPath, &QAction::triggered, this, [targetPath]{
        QGuiApplication::clipboard()->setText(targetPath);
    });

    menu.exec(m_tree->viewport()->mapToGlobal(localPos));
}

void FileBrowserDock::doRename(const QString& path)
{
    const QFileInfo fi(path);
    bool ok = false;
    const QString newName = QInputDialog::getText(this,
        tr("Rename"),
        tr("New name for \"%1\":").arg(fi.fileName()),
        QLineEdit::Normal, fi.fileName(), &ok).trimmed();
    if (!ok || newName.isEmpty() || newName == fi.fileName()) return;
    if (newName.contains(QChar('/'))) {
        QMessageBox::warning(this, tr("Rename"),
            tr("New name must not contain '/'."));
        return;
    }
    const QString newPath = fi.absoluteDir().absoluteFilePath(newName);
    if (QFileInfo::exists(newPath)) {
        QMessageBox::warning(this, tr("Rename"),
            tr("\"%1\" already exists.").arg(newName));
        return;
    }
    if (!QFile::rename(path, newPath)) {
        QMessageBox::warning(this, tr("Rename failed"),
            tr("Could not rename %1 to %2.").arg(path, newName));
        return;
    }
}

void FileBrowserDock::doDelete(const QString& path)
{
    const QFileInfo fi(path);
    const auto reply = QMessageBox::question(this,
        tr("Delete"),
        fi.isDir()
            ? tr("Move folder \"%1\" to the system trash?\n\n"
                 "(All contents will be moved with it.)").arg(fi.fileName())
            : tr("Move file \"%1\" to the system trash?").arg(fi.fileName()),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (reply != QMessageBox::Yes) return;
    // Prefer Qt's moveToTrash (XDG-aware on Linux). Falls back to a
    // permanent delete with a final user confirmation if trash isn't
    // available — same pattern as MainWindow::onFileMoveToTrash (Phase 5j).
    QString trashedPath;
    if (!QFile::moveToTrash(path, &trashedPath)) {
        const auto fallback = QMessageBox::question(this,
            tr("Delete"),
            tr("Trash is unavailable. Permanently delete \"%1\" instead?")
                .arg(fi.fileName()),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (fallback != QMessageBox::Yes) return;
        const bool ok = fi.isDir()
            ? QDir(path).removeRecursively()
            : QFile::remove(path);
        if (!ok) {
            QMessageBox::warning(this, tr("Delete failed"),
                tr("Could not delete %1.").arg(path));
            return;
        }
    }
}

void FileBrowserDock::doNewFile(const QString& parentDir)
{
    bool ok = false;
    const QString name = QInputDialog::getText(this,
        tr("New File"),
        tr("Name for new file in:\n%1").arg(parentDir),
        QLineEdit::Normal, QStringLiteral("untitled.txt"), &ok).trimmed();
    if (!ok || name.isEmpty()) return;
    if (name.contains(QChar('/'))) {
        QMessageBox::warning(this, tr("New File"),
            tr("Name must not contain '/'."));
        return;
    }
    const QString newPath = QDir(parentDir).absoluteFilePath(name);
    if (QFileInfo::exists(newPath)) {
        QMessageBox::warning(this, tr("New File"),
            tr("\"%1\" already exists.").arg(name));
        return;
    }
    QFile f(newPath);
    if (!f.open(QIODevice::WriteOnly)) {
        QMessageBox::warning(this, tr("New File failed"),
            tr("Could not create %1: %2").arg(newPath, f.errorString()));
        return;
    }
    f.close();
    // Open the new file in the active pane so the user can start editing.
    if (m_mw && m_mw->activePane()) {
        m_mw->activePane()->openFile(newPath);
    }
}

void FileBrowserDock::doNewFolder(const QString& parentDir)
{
    bool ok = false;
    const QString name = QInputDialog::getText(this,
        tr("New Folder"),
        tr("Name for new folder in:\n%1").arg(parentDir),
        QLineEdit::Normal, QStringLiteral("new-folder"), &ok).trimmed();
    if (!ok || name.isEmpty()) return;
    if (name.contains(QChar('/'))) {
        QMessageBox::warning(this, tr("New Folder"),
            tr("Name must not contain '/'."));
        return;
    }
    QDir d(parentDir);
    if (d.exists(name)) {
        QMessageBox::warning(this, tr("New Folder"),
            tr("\"%1\" already exists.").arg(name));
        return;
    }
    if (!d.mkdir(name)) {
        QMessageBox::warning(this, tr("New Folder failed"),
            tr("Could not create %1.")
                .arg(d.absoluteFilePath(name)));
    }
}

void FileBrowserDock::doOpenInExternal(const QString& path)
{
    // QDesktopServices::openUrl with a file:// URL routes through the
    // user's default handler (xdg-open on Linux): file managers open
    // directories, default app opens files.
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}
