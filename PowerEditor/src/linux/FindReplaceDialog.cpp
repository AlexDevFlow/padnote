#include "FindReplaceDialog.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QProgressDialog>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QShortcut>
#include <QTabWidget>
#include <QTextStream>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <vector>

#include "Buffer.h"
#include "Config.h"   // Phase 9b.3 — find/replace history persistence
#include "EditorTabs.h"
#include "Localization.h"   // Phase 8b-polish — applyToDialog
#include "MainWindow.h"
#include "Scintilla.h"  // SCFIND_* flags
#include "ScintillaEditBase.h"
#include "ScintillaMessages.h"
#include "Theme.h"      // Phase 5MK — mark style colours

using Scintilla::Message;
using Scintilla::sptr_t;
using Scintilla::uptr_t;

namespace {

// Wrappers around send() to keep call sites readable.
sptr_t sciSend(ScintillaEditBase* ed, Message m,
               uptr_t w = 0, sptr_t l = 0)
{
    return ed->send(static_cast<unsigned int>(m), w, l);
}
sptr_t sciSends(ScintillaEditBase* ed, Message m,
                uptr_t w, const char* s)
{
    return ed->sends(static_cast<unsigned int>(m), w, s);
}

// Read a base-N digit run of fixed length from 'str'. Returns true on a
// well-formed run; mirrors Searching::readBase in upstream FindReplaceDlg.cpp.
bool readBaseAscii(const QString& s, int from, int size, int base, int* out)
{
    if (from + size > s.size()) return false;
    int value = 0;
    const QChar maxDigit = QChar('0' + (base - 1 < 10 ? base - 1 : 9));
    for (int k = 0; k < size; ++k) {
        QChar c = s.at(from + k);
        int d;
        if (c >= QChar('0') && c <= maxDigit) {
            d = c.unicode() - '0';
        } else if (base > 10) {
            QChar uc = c.toUpper();
            if (uc < QChar('A') || uc > QChar('A' + base - 11)) return false;
            d = uc.unicode() - 'A' + 10;
        } else {
            return false;
        }
        value = value * base + d;
    }
    *out = value;
    return true;
}

} // namespace

FindReplaceDialog::FindReplaceDialog(MainWindow* mw, QWidget* parent)
    : QDockWidget(tr("Find / Replace"), parent),
      m_mw(mw)
{
    setObjectName(QStringLiteral("findReplaceDock"));
    setAllowedAreas(Qt::AllDockWidgetAreas);
    setFeatures(QDockWidget::DockWidgetClosable
              | QDockWidget::DockWidgetMovable
              | QDockWidget::DockWidgetFloatable);

    auto* host = new QWidget(this);
    auto* hostLayout = new QVBoxLayout(host);
    hostLayout->setContentsMargins(6, 6, 6, 6);
    hostLayout->setSpacing(4);

    m_innerTabs = new QTabWidget(host);
    m_innerTabs->addTab(buildFindTab(),            tr("Find"));
    m_innerTabs->addTab(buildReplaceTab(),         tr("Replace"));
    m_innerTabs->addTab(buildFindInFilesTab(),     tr("Find in Files"));
    m_innerTabs->addTab(buildFindInOpenFilesTab(), tr("Find in Open Files"));
    m_innerTabs->addTab(buildMarkTab(), tr("Mark"));
    hostLayout->addWidget(m_innerTabs);

    m_statusLabel = new QLabel(tr("Ready."), host);
    m_statusLabel->setObjectName(QStringLiteral("findReplaceStatus"));
    m_statusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    hostLayout->addWidget(m_statusLabel);

    setWidget(host);

    // Pressing Esc inside the dock closes it (matches NPP behaviour).
    auto* escClose = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    escClose->setContext(Qt::WidgetWithChildrenShortcut);
    connect(escClose, &QShortcut::activated, this, &QDockWidget::close);

    // Phase 9b.3 — restore persisted find/replace history. The find
    // history populates BOTH find combos (Find tab + Replace tab) so
    // the user sees the same dropdown regardless of entry point. Each
    // combo's currentIndex stays at -1 (empty line edit) so the user
    // can type fresh — picking a history entry from the dropdown still
    // works because addItems doesn't change the line edit text.
    {
        const QStringList fh = Config::findHistory();
        if (!fh.isEmpty()) {
            m_findCombo->addItems(fh);
            m_repFindCombo->addItems(fh);
            m_findCombo->setCurrentIndex(-1);
            m_findCombo->lineEdit()->clear();
            m_repFindCombo->setCurrentIndex(-1);
            m_repFindCombo->lineEdit()->clear();
        }
        const QStringList rh = Config::replaceHistory();
        if (!rh.isEmpty()) {
            m_repWithCombo->addItems(rh);
            m_repWithCombo->setCurrentIndex(-1);
            m_repWithCombo->lineEdit()->clear();
        }
    }

    // Phase 8b-polish — apply the active language overlay to widget
    // labels/buttons/groupboxes within this dock. Source-text matching
    // against english.xml's <Dialog><Find>...<Item id="N" name="..."/>
    // rows. Idempotent on language switch (re-call applyToDialog).
    Localization::applyToDialog(this, "Find");
}

void FindReplaceDialog::setEditor(ScintillaEditBase* editor)
{
    m_editor = editor;
}

void FindReplaceDialog::showFindTab(const QString& prefill)
{
    if (!prefill.isEmpty()) {
        m_findCombo->setCurrentText(prefill);
        m_repFindCombo->setCurrentText(prefill);
    }
    m_innerTabs->setCurrentIndex(0);
    show();
    raise();
    m_findCombo->setFocus();
    m_findCombo->lineEdit()->selectAll();
}

void FindReplaceDialog::showReplaceTab(const QString& prefill)
{
    if (!prefill.isEmpty()) {
        m_findCombo->setCurrentText(prefill);
        m_repFindCombo->setCurrentText(prefill);
    }
    m_innerTabs->setCurrentIndex(1);
    show();
    raise();
    m_repFindCombo->setFocus();
    m_repFindCombo->lineEdit()->selectAll();
}

void FindReplaceDialog::showFindInFilesTab(const QString& prefill)
{
    if (!prefill.isEmpty() && m_fifFindCombo) {
        m_fifFindCombo->setCurrentText(prefill);
    }
    m_innerTabs->setCurrentIndex(2);
    show();
    raise();
    if (m_fifFindCombo) {
        m_fifFindCombo->setFocus();
        if (m_fifFindCombo->lineEdit()) m_fifFindCombo->lineEdit()->selectAll();
    }
}

void FindReplaceDialog::showMarkTab(const QString& prefill)
{
    if (!prefill.isEmpty() && m_markText) m_markText->setText(prefill);
    m_innerTabs->setCurrentIndex(4);   // Mark is the 5th tab (index 4)
    show();
    raise();
    if (m_markText) {
        m_markText->setFocus();
        m_markText->selectAll();
    }
}

bool FindReplaceDialog::findNextStandalone(bool forwards)
{
    if (!hasLastSearch()) return false;
    SearchOptions opts = m_lastOptions;
    return doFindNext(opts, forwards);
}

// -----------------------------------------------------------------------------
// UI construction
// -----------------------------------------------------------------------------

QWidget* FindReplaceDialog::buildFindTab()
{
    auto* w = new QWidget(this);
    auto* grid = new QGridLayout(w);
    grid->setContentsMargins(4, 4, 4, 4);
    grid->setHorizontalSpacing(8);
    grid->setVerticalSpacing(4);

    int row = 0;

    grid->addWidget(new QLabel(tr("Find what:")), row, 0);
    m_findCombo = new QComboBox(w);
    m_findCombo->setEditable(true);
    m_findCombo->setMinimumWidth(280);
    m_findCombo->setInsertPolicy(QComboBox::InsertAtTop);
    grid->addWidget(m_findCombo, row, 1, 1, 3);
    ++row;

    // Buttons row
    auto* btnLayout = new QHBoxLayout;
    auto* bFindNext = new QPushButton(tr("Find Next"), w);
    auto* bFindPrev = new QPushButton(tr("Find Previous"), w);
    auto* bCount    = new QPushButton(tr("Count"), w);
    auto* bClose    = new QPushButton(tr("Close"), w);
    bFindNext->setDefault(true);
    btnLayout->addWidget(bFindNext);
    btnLayout->addWidget(bFindPrev);
    btnLayout->addWidget(bCount);
    btnLayout->addStretch(1);
    btnLayout->addWidget(bClose);
    grid->addLayout(btnLayout, row, 0, 1, 4);
    ++row;

    connect(bFindNext, &QPushButton::clicked, this, &FindReplaceDialog::onFindNext);
    connect(bFindPrev, &QPushButton::clicked, this, &FindReplaceDialog::onFindPrevious);
    connect(bCount,    &QPushButton::clicked, this, &FindReplaceDialog::onFindCount);
    connect(bClose,    &QPushButton::clicked, this, &FindReplaceDialog::onFindCloseClicked);

    // Pressing Enter in the find combo's line edit triggers Find Next.
    connect(m_findCombo->lineEdit(), &QLineEdit::returnPressed,
            this, &FindReplaceDialog::onFindNext);

    // Options row 1: checkboxes
    m_findMatchCase  = new QCheckBox(tr("Match case"), w);
    m_findWholeWord  = new QCheckBox(tr("Whole word"), w);
    m_findWrapAround = new QCheckBox(tr("Wrap around"), w);
    m_findWrapAround->setChecked(true);
    grid->addWidget(m_findMatchCase,  row, 0);
    grid->addWidget(m_findWholeWord,  row, 1);
    grid->addWidget(m_findWrapAround, row, 2);
    ++row;

    // Options row 2: search mode group
    auto* modeBox = new QGroupBox(tr("Search Mode"), w);
    auto* modeLayout = new QHBoxLayout(modeBox);
    modeLayout->setContentsMargins(8, 4, 8, 4);
    m_findModeNormal = new QRadioButton(tr("Normal"),    modeBox);
    m_findModeExt    = new QRadioButton(tr("Extended (\\n, \\r, \\t, \\0, \\xHH...)"), modeBox);
    m_findModeRegex  = new QRadioButton(tr("Regular expression"), modeBox);
    m_findModeNormal->setChecked(true);
    modeLayout->addWidget(m_findModeNormal);
    modeLayout->addWidget(m_findModeExt);
    modeLayout->addWidget(m_findModeRegex);
    modeLayout->addStretch(1);
    grid->addWidget(modeBox, row, 0, 1, 4);
    ++row;

    // Options row 3: direction group
    auto* dirBox = new QGroupBox(tr("Direction"), w);
    auto* dirLayout = new QHBoxLayout(dirBox);
    dirLayout->setContentsMargins(8, 4, 8, 4);
    m_findDirUp   = new QRadioButton(tr("Up"),   dirBox);
    m_findDirDown = new QRadioButton(tr("Down"), dirBox);
    m_findDirDown->setChecked(true);
    dirLayout->addWidget(m_findDirUp);
    dirLayout->addWidget(m_findDirDown);
    dirLayout->addStretch(1);
    grid->addWidget(dirBox, row, 0, 1, 4);
    ++row;

    grid->setColumnStretch(3, 1);
    grid->setRowStretch(row, 1);

    return w;
}

QWidget* FindReplaceDialog::buildReplaceTab()
{
    auto* w = new QWidget(this);
    auto* grid = new QGridLayout(w);
    grid->setContentsMargins(4, 4, 4, 4);
    grid->setHorizontalSpacing(8);
    grid->setVerticalSpacing(4);

    int row = 0;

    grid->addWidget(new QLabel(tr("Find what:")), row, 0);
    m_repFindCombo = new QComboBox(w);
    m_repFindCombo->setEditable(true);
    m_repFindCombo->setMinimumWidth(280);
    m_repFindCombo->setInsertPolicy(QComboBox::InsertAtTop);
    grid->addWidget(m_repFindCombo, row, 1, 1, 3);
    ++row;

    grid->addWidget(new QLabel(tr("Replace with:")), row, 0);
    m_repWithCombo = new QComboBox(w);
    m_repWithCombo->setEditable(true);
    m_repWithCombo->setMinimumWidth(280);
    m_repWithCombo->setInsertPolicy(QComboBox::InsertAtTop);
    grid->addWidget(m_repWithCombo, row, 1, 1, 3);
    ++row;

    auto* btnLayout = new QHBoxLayout;
    auto* bFind     = new QPushButton(tr("Find Next"),         w);
    auto* bReplace  = new QPushButton(tr("Replace"),           w);
    auto* bRepAll   = new QPushButton(tr("Replace All"),       w);
    auto* bRepAllOD = new QPushButton(tr("Replace All in All Open Documents"), w);
    auto* bClose    = new QPushButton(tr("Close"),             w);
    bReplace->setDefault(true);
    btnLayout->addWidget(bFind);
    btnLayout->addWidget(bReplace);
    btnLayout->addWidget(bRepAll);
    btnLayout->addWidget(bRepAllOD);
    btnLayout->addStretch(1);
    btnLayout->addWidget(bClose);
    grid->addLayout(btnLayout, row, 0, 1, 4);
    ++row;

    connect(bFind,     &QPushButton::clicked, this, &FindReplaceDialog::onReplaceFindNext);
    connect(bReplace,  &QPushButton::clicked, this, &FindReplaceDialog::onReplaceCurrent);
    connect(bRepAll,   &QPushButton::clicked, this, &FindReplaceDialog::onReplaceAll);
    connect(bRepAllOD, &QPushButton::clicked, this, &FindReplaceDialog::onReplaceAllInOpenDocs);
    connect(bClose,    &QPushButton::clicked, this, &FindReplaceDialog::onReplaceCloseClicked);

    connect(m_repFindCombo->lineEdit(), &QLineEdit::returnPressed,
            this, &FindReplaceDialog::onReplaceFindNext);

    m_repMatchCase  = new QCheckBox(tr("Match case"), w);
    m_repWholeWord  = new QCheckBox(tr("Whole word"), w);
    m_repWrapAround = new QCheckBox(tr("Wrap around"), w);
    m_repWrapAround->setChecked(true);
    grid->addWidget(m_repMatchCase,  row, 0);
    grid->addWidget(m_repWholeWord,  row, 1);
    grid->addWidget(m_repWrapAround, row, 2);
    ++row;

    auto* modeBox = new QGroupBox(tr("Search Mode"), w);
    auto* modeLayout = new QHBoxLayout(modeBox);
    modeLayout->setContentsMargins(8, 4, 8, 4);
    m_repModeNormal = new QRadioButton(tr("Normal"),    modeBox);
    m_repModeExt    = new QRadioButton(tr("Extended"),  modeBox);
    m_repModeRegex  = new QRadioButton(tr("Regular expression"), modeBox);
    m_repModeNormal->setChecked(true);
    modeLayout->addWidget(m_repModeNormal);
    modeLayout->addWidget(m_repModeExt);
    modeLayout->addWidget(m_repModeRegex);
    modeLayout->addStretch(1);
    grid->addWidget(modeBox, row, 0, 1, 4);
    ++row;

    auto* dirBox = new QGroupBox(tr("Direction"), w);
    auto* dirLayout = new QHBoxLayout(dirBox);
    dirLayout->setContentsMargins(8, 4, 8, 4);
    m_repDirUp   = new QRadioButton(tr("Up"),   dirBox);
    m_repDirDown = new QRadioButton(tr("Down"), dirBox);
    m_repDirDown->setChecked(true);
    dirLayout->addWidget(m_repDirUp);
    dirLayout->addWidget(m_repDirDown);
    dirLayout->addStretch(1);
    grid->addWidget(dirBox, row, 0, 1, 4);
    ++row;

    grid->setColumnStretch(3, 1);
    grid->setRowStretch(row, 1);

    return w;
}

QWidget* FindReplaceDialog::buildStubTab(const QString& message)
{
    auto* w = new QWidget(this);
    auto* layout = new QVBoxLayout(w);
    layout->setContentsMargins(16, 16, 16, 16);
    auto* label = new QLabel(message, w);
    label->setWordWrap(true);
    label->setAlignment(Qt::AlignCenter);
    layout->addStretch(1);
    layout->addWidget(label);
    layout->addStretch(2);
    return w;
}

// Phase 5MK — Mark tab. Five Mark N buttons (each tinted with the
// theme's "Mark Style N" colour) + Match Case + Whole Word + a Clear
// All button. Clicking Mark N runs Buffer::markAllOccurrences for the
// active buffer.
QWidget* FindReplaceDialog::buildMarkTab()
{
    auto* w = new QWidget(this);
    auto* layout = new QVBoxLayout(w);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    auto* note = new QLabel(
        tr("Mark every occurrence of a string with one of five distinct "
           "colours from the active theme. Marks persist until Clear All "
           "or the buffer is reloaded; they are not saved to disk."),
        w);
    note->setWordWrap(true);
    note->setStyleSheet(QStringLiteral("color: gray; font-style: italic;"));
    layout->addWidget(note);

    auto* form = new QHBoxLayout;
    auto* lbl  = new QLabel(tr("Mark text:"), w);
    m_markText = new QLineEdit(w);
    m_markText->setPlaceholderText(tr("token to highlight"));
    form->addWidget(lbl);
    form->addWidget(m_markText, 1);
    layout->addLayout(form);

    auto* opts = new QHBoxLayout;
    m_markMatchCase = new QCheckBox(tr("Match case"), w);
    m_markWholeWord = new QCheckBox(tr("Whole word"), w);
    m_markMatchCase->setChecked(true);
    opts->addWidget(m_markMatchCase);
    opts->addWidget(m_markWholeWord);
    opts->addStretch(1);
    layout->addLayout(opts);

    auto* btnRow = new QHBoxLayout;
    auto bgrToCss = [](int bgr) -> QString {
        // Theme uses Scintilla packed BGR; CSS uses RRGGBB.
        const int r = (bgr      ) & 0xFF;
        const int g = (bgr >>  8) & 0xFF;
        const int b = (bgr >> 16) & 0xFF;
        return QString("#%1%2%3")
            .arg(r, 2, 16, QChar('0'))
            .arg(g, 2, 16, QChar('0'))
            .arg(b, 2, 16, QChar('0'));
    };
    for (int i = 0; i < 5; ++i) {
        auto* btn = new QPushButton(tr("Mark %1").arg(i + 1), w);
        const QString css = bgrToCss(Theme::markStyleFore(i + 1));
        btn->setStyleSheet(QString(
            "QPushButton { background-color: %1; color: black; "
            "padding: 4px 12px; }").arg(css));
        m_markButtons[i] = btn;
        const int n = i + 1;
        connect(btn, &QPushButton::clicked, this, [this, n]() { onMarkN(n); });
        btnRow->addWidget(btn);
    }
    btnRow->addStretch(1);
    m_markClearAll = new QPushButton(tr("Clear All"), w);
    connect(m_markClearAll, &QPushButton::clicked,
            this, &FindReplaceDialog::onClearMarks);
    btnRow->addWidget(m_markClearAll);
    layout->addLayout(btnRow);

    layout->addStretch(1);
    return w;
}

void FindReplaceDialog::onMarkN(int n)
{
    if (!m_mw) { setStatus(tr("No tab container.")); return; }
    Buffer* b = m_mw->activePane() ? m_mw->activePane()->currentBuffer() : nullptr;
    if (!b) { setStatus(tr("Mark: no active buffer.")); return; }
    const QString text = m_markText->text();
    if (text.isEmpty()) { setStatus(tr("Mark: empty search text.")); return; }
    const int hits = b->markAllOccurrences(text, n,
        m_markMatchCase->isChecked(), m_markWholeWord->isChecked());
    setStatus(tr("Mark %1: %2 occurrence(s) of '%3'.").arg(n).arg(hits).arg(text));
}

void FindReplaceDialog::onClearMarks()
{
    if (!m_mw) { setStatus(tr("No tab container.")); return; }
    int touched = 0;
    for (Buffer* b : m_mw->allBuffers()) {
        if (!b) continue;
        b->clearAllMarks();
        ++touched;
    }
    setStatus(tr("Cleared marks across %1 buffer(s).").arg(touched));
}

// -----------------------------------------------------------------------------
// Phase 5R — Find in Files / Find in Open Files
// -----------------------------------------------------------------------------

namespace {

// Match-options struct that the search loop consumes. Mirrors a subset of
// FindReplaceDialog::SearchOptions but in a flat form so the helper can
// be called without including the dialog's private types.
struct FifMatch {
    int    line;        // 1-based
    int    column;      // 1-based, character offset
    QString preview;    // the line's text, trimmed
};

// Returns the list of matches in 'text'. Plain string match unless `mode`
// is Regex — in which case `pattern` is compiled as ECMAScript regex.
// matchCase / wholeWord apply to both modes.
QList<FifMatch> matchesInText(const QString& text,
                              const QString& pattern,
                              bool matchCase,
                              bool wholeWord,
                              bool regex)
{
    QList<FifMatch> out;
    if (text.isEmpty() || pattern.isEmpty()) return out;

    QRegularExpression::PatternOptions opts =
        QRegularExpression::MultilineOption;
    if (!matchCase) opts |= QRegularExpression::CaseInsensitiveOption;

    QString rx;
    if (regex) {
        rx = pattern;
    } else {
        rx = QRegularExpression::escape(pattern);
        if (wholeWord) rx = QStringLiteral("\\b") + rx + QStringLiteral("\\b");
    }
    QRegularExpression re(rx, opts);
    if (!re.isValid()) return out;

    auto it = re.globalMatch(text);
    while (it.hasNext()) {
        const auto m = it.next();
        const int pos = m.capturedStart();
        // Compute line / column by counting \n up to pos.
        int line = 1;
        int lineStart = 0;
        for (int i = 0; i < pos; ++i) {
            if (text[i] == QChar('\n')) { ++line; lineStart = i + 1; }
        }
        const int col = pos - lineStart + 1;
        // Preview = line containing the match, trimmed.
        int lineEnd = text.indexOf(QChar('\n'), lineStart);
        if (lineEnd < 0) lineEnd = text.size();
        QString preview = text.mid(lineStart, lineEnd - lineStart).trimmed();
        if (preview.size() > 200) preview = preview.left(200) + QStringLiteral("…");
        out.append({line, col, preview});
    }
    return out;
}

// Phase 9g — match a name (file basename or path component) against any
// of the space-separated globs. Empty glob list returns false (no match,
// don't exclude).
bool matchesAnyGlob(const QString& name, const QStringList& globs)
{
    for (const QString& g : globs) {
        if (g.isEmpty()) continue;
        const QRegularExpression re(
            QRegularExpression::wildcardToRegularExpression(g),
            QRegularExpression::CaseInsensitiveOption);
        if (re.match(name).hasMatch()) return true;
    }
    return false;
}

QStringList collectFiles(const QString& dir,
                         const QStringList& filters,
                         bool recursive,
                         const QStringList& excludeDirs,
                         const QStringList& excludeFiles)
{
    QStringList result;
    QDirIterator::IteratorFlags flags = QDirIterator::NoIteratorFlags;
    if (recursive) flags |= QDirIterator::Subdirectories;
    QDirIterator it(dir, filters, QDir::Files | QDir::NoSymLinks, flags);
    const int rootLen = dir.size();
    while (it.hasNext()) {
        const QString path = it.next();
        // Phase 9g — exclude-files: match the basename only.
        if (!excludeFiles.isEmpty()) {
            const QString base = QFileInfo(path).fileName();
            if (matchesAnyGlob(base, excludeFiles)) continue;
        }
        // Phase 9g — exclude-dirs: walk the path components RELATIVE to
        // `dir` (so the user's chosen root isn't itself excluded by name).
        if (!excludeDirs.isEmpty()) {
            QString rel = path.mid(rootLen);
            if (rel.startsWith(QChar('/'))) rel.remove(0, 1);
            const QStringList parts = rel.split(QChar('/'),
                Qt::SkipEmptyParts);
            // The last part is the file basename; all earlier parts are
            // directory names.
            bool excluded = false;
            for (int i = 0; i + 1 < parts.size(); ++i) {
                if (matchesAnyGlob(parts[i], excludeDirs)) {
                    excluded = true;
                    break;
                }
            }
            if (excluded) continue;
        }
        result.append(path);
        // Cap at 5000 to keep the UI responsive on accidental "/"
        // searches. Users wanting more should narrow the filter.
        if (result.size() >= 5000) break;
    }
    return result;
}

QString readFileText(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    QTextStream in(&f);
    return in.readAll();
}

void populateResultsTree(QTreeWidget* tree,
                         const QString& path,
                         const QList<FifMatch>& matches)
{
    if (matches.isEmpty()) return;
    auto* fileItem = new QTreeWidgetItem(tree);
    fileItem->setText(0, QFileInfo(path).fileName()
                     + QStringLiteral("  (%1 matches)").arg(matches.size()));
    fileItem->setText(2, path);
    fileItem->setToolTip(0, path);
    for (const FifMatch& m : matches) {
        auto* it = new QTreeWidgetItem(fileItem);
        it->setText(0, QStringLiteral("  Line %1").arg(m.line));
        it->setText(1, m.preview);
        it->setData(0, Qt::UserRole, path);
        it->setData(0, Qt::UserRole + 1, m.line);
        it->setData(0, Qt::UserRole + 2, m.column);
    }
    fileItem->setExpanded(true);
}

QTreeWidget* makeResultsTree(QWidget* parent)
{
    auto* tree = new QTreeWidget(parent);
    tree->setColumnCount(3);
    tree->setHeaderLabels({QObject::tr("Match"),
                           QObject::tr("Preview"),
                           QObject::tr("Path")});
    tree->setColumnHidden(2, true);   // path tucked away; tooltip suffices
    tree->setAlternatingRowColors(true);
    tree->setUniformRowHeights(true);
    tree->setRootIsDecorated(true);
    return tree;
}

} // namespace

QWidget* FindReplaceDialog::buildFindInFilesTab()
{
    auto* w = new QWidget(this);
    auto* layout = new QGridLayout(w);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setHorizontalSpacing(8);
    layout->setVerticalSpacing(6);
    int row = 0;

    layout->addWidget(new QLabel(tr("Find what:"), w), row, 0);
    m_fifFindCombo = new QComboBox(w);
    m_fifFindCombo->setEditable(true);
    layout->addWidget(m_fifFindCombo, row, 1, 1, 3);
    ++row;

    layout->addWidget(new QLabel(tr("Directory:"), w), row, 0);
    m_fifDirectory = new QLineEdit(QDir::currentPath(), w);
    layout->addWidget(m_fifDirectory, row, 1, 1, 2);
    auto* aBrowse = new QPushButton(tr("Browse..."), w);
    layout->addWidget(aBrowse, row, 3);
    connect(aBrowse, &QPushButton::clicked,
            this, &FindReplaceDialog::onFifBrowseDirectory);
    ++row;

    layout->addWidget(new QLabel(tr("Filters:"), w), row, 0);
    m_fifFilters = new QLineEdit(QStringLiteral("*.cpp *.h *.txt"), w);
    m_fifFilters->setToolTip(tr("Space-separated glob patterns, e.g. *.cpp *.h"));
    layout->addWidget(m_fifFilters, row, 1, 1, 3);
    ++row;

    // Phase 9g — exclude folders / exclude files. Both space-separated.
    layout->addWidget(new QLabel(tr("Exclude folders:"), w), row, 0);
    m_fifExcludeDirs = new QLineEdit(Config::findInFilesExcludeDirs(), w);
    m_fifExcludeDirs->setToolTip(tr(
        "Space-separated folder name globs to skip during the walk. "
        "Matches against any path component, e.g. \".git node_modules\"."));
    layout->addWidget(m_fifExcludeDirs, row, 1, 1, 3);
    ++row;

    layout->addWidget(new QLabel(tr("Exclude files:"), w), row, 0);
    m_fifExcludeFiles = new QLineEdit(Config::findInFilesExcludeFiles(), w);
    m_fifExcludeFiles->setToolTip(tr(
        "Space-separated file basename globs to skip, e.g. "
        "\"*.min.js *~ *.bak\". Matches the basename, not the path."));
    layout->addWidget(m_fifExcludeFiles, row, 1, 1, 3);
    ++row;

    auto* opts = new QHBoxLayout;
    m_fifRecursive = new QCheckBox(tr("In all sub-folders"), w);
    m_fifRecursive->setChecked(true);
    m_fifMatchCase = new QCheckBox(tr("Match case"), w);
    m_fifWholeWord = new QCheckBox(tr("Whole word only"), w);
    opts->addWidget(m_fifRecursive);
    opts->addWidget(m_fifMatchCase);
    opts->addWidget(m_fifWholeWord);
    opts->addStretch(1);
    layout->addLayout(opts, row, 0, 1, 4);
    ++row;

    auto* mode = new QGroupBox(tr("Search Mode"), w);
    auto* modeLay = new QHBoxLayout(mode);
    m_fifModeNormal = new QRadioButton(tr("Normal"),   mode);
    m_fifModeExt    = new QRadioButton(tr("Extended"), mode);
    m_fifModeRegex  = new QRadioButton(tr("Regex"),    mode);
    m_fifModeNormal->setChecked(true);
    modeLay->addWidget(m_fifModeNormal);
    modeLay->addWidget(m_fifModeExt);
    modeLay->addWidget(m_fifModeRegex);
    modeLay->addStretch(1);
    layout->addWidget(mode, row, 0, 1, 4);
    ++row;

    auto* btnRow = new QHBoxLayout;
    auto* aFindAll = new QPushButton(tr("&Find All"), w);
    aFindAll->setDefault(true);
    btnRow->addStretch(1);
    btnRow->addWidget(aFindAll);
    layout->addLayout(btnRow, row, 0, 1, 4);
    connect(aFindAll, &QPushButton::clicked,
            this, &FindReplaceDialog::onFindAllInFiles);
    ++row;

    m_fifResults = makeResultsTree(w);
    layout->addWidget(m_fifResults, row, 0, 1, 4);
    layout->setRowStretch(row, 1);
    connect(m_fifResults, &QTreeWidget::itemActivated,
            this, &FindReplaceDialog::onResultActivated);

    return w;
}

QWidget* FindReplaceDialog::buildFindInOpenFilesTab()
{
    auto* w = new QWidget(this);
    auto* layout = new QVBoxLayout(w);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);

    auto* findRow = new QHBoxLayout;
    findRow->addWidget(new QLabel(tr("Find what:"), w));
    m_fioFindCombo = new QComboBox(w);
    m_fioFindCombo->setEditable(true);
    findRow->addWidget(m_fioFindCombo, 1);
    layout->addLayout(findRow);

    auto* opts = new QHBoxLayout;
    m_fioMatchCase = new QCheckBox(tr("Match case"), w);
    m_fioWholeWord = new QCheckBox(tr("Whole word only"), w);
    opts->addWidget(m_fioMatchCase);
    opts->addWidget(m_fioWholeWord);
    opts->addStretch(1);
    layout->addLayout(opts);

    auto* mode = new QGroupBox(tr("Search Mode"), w);
    auto* modeLay = new QHBoxLayout(mode);
    m_fioModeNormal = new QRadioButton(tr("Normal"),   mode);
    m_fioModeExt    = new QRadioButton(tr("Extended"), mode);
    m_fioModeRegex  = new QRadioButton(tr("Regex"),    mode);
    m_fioModeNormal->setChecked(true);
    modeLay->addWidget(m_fioModeNormal);
    modeLay->addWidget(m_fioModeExt);
    modeLay->addWidget(m_fioModeRegex);
    modeLay->addStretch(1);
    layout->addWidget(mode);

    auto* btnRow = new QHBoxLayout;
    auto* aFindAll = new QPushButton(tr("&Find All in Open Files"), w);
    aFindAll->setDefault(true);
    btnRow->addStretch(1);
    btnRow->addWidget(aFindAll);
    layout->addLayout(btnRow);
    connect(aFindAll, &QPushButton::clicked,
            this, &FindReplaceDialog::onFindAllInOpenFiles);

    m_fioResults = makeResultsTree(w);
    layout->addWidget(m_fioResults, 1);
    connect(m_fioResults, &QTreeWidget::itemActivated,
            this, &FindReplaceDialog::onResultActivated);

    return w;
}

void FindReplaceDialog::onFifBrowseDirectory()
{
    const QString dir = QFileDialog::getExistingDirectory(this,
        tr("Choose directory"), m_fifDirectory->text());
    if (!dir.isEmpty()) m_fifDirectory->setText(dir);
}

void FindReplaceDialog::onFindAllInFiles()
{
    const QString needle = m_fifFindCombo->currentText();
    if (needle.isEmpty()) {
        setStatus(tr("Find in Files: empty pattern."));
        return;
    }
    const QString dir = m_fifDirectory->text();
    if (!QDir(dir).exists()) {
        setStatus(tr("Find in Files: directory does not exist."));
        return;
    }
    QStringList filters = m_fifFilters->text().split(QChar(' '),
        Qt::SkipEmptyParts);
    if (filters.isEmpty()) filters << QStringLiteral("*");

    // Phase 9g — exclude-folders / exclude-files. Empty inputs skip the
    // exclusion check entirely. Persist current values so the next
    // session reopens with the same exclusions.
    const QStringList excludeDirs = m_fifExcludeDirs->text()
        .split(QChar(' '), Qt::SkipEmptyParts);
    const QStringList excludeFiles = m_fifExcludeFiles->text()
        .split(QChar(' '), Qt::SkipEmptyParts);
    Config::setFindInFilesExcludeDirs(m_fifExcludeDirs->text());
    Config::setFindInFilesExcludeFiles(m_fifExcludeFiles->text());
    Config::save();

    const QStringList files = collectFiles(dir, filters,
        m_fifRecursive->isChecked(), excludeDirs, excludeFiles);
    if (files.isEmpty()) {
        m_fifResults->clear();
        setStatus(tr("Find in Files: no files match the filter."));
        return;
    }

    const bool regex = m_fifModeRegex->isChecked();
    const bool matchCase = m_fifMatchCase->isChecked();
    const bool wholeWord = m_fifWholeWord->isChecked();

    QProgressDialog prog(tr("Searching..."), tr("Cancel"),
        0, files.size(), this);
    prog.setWindowModality(Qt::WindowModal);
    prog.setMinimumDuration(500);

    m_fifResults->clear();
    int totalMatches = 0;
    int filesWithMatches = 0;
    for (int i = 0; i < files.size(); ++i) {
        if (prog.wasCanceled()) break;
        prog.setValue(i);
        const QString text = readFileText(files[i]);
        if (text.isEmpty()) continue;
        const auto matches = matchesInText(text, needle,
            matchCase, wholeWord, regex);
        if (!matches.isEmpty()) {
            populateResultsTree(m_fifResults, files[i], matches);
            totalMatches += matches.size();
            ++filesWithMatches;
        }
    }
    prog.setValue(files.size());

    setStatus(tr("Find in Files: %1 match(es) in %2 of %3 file(s).")
        .arg(totalMatches).arg(filesWithMatches).arg(files.size()));
}

void FindReplaceDialog::onFindAllInOpenFiles()
{
    if (!m_mw) return;
    const QString needle = m_fioFindCombo->currentText();
    if (needle.isEmpty()) {
        setStatus(tr("Find in Open Files: empty pattern."));
        return;
    }

    const bool regex = m_fioModeRegex->isChecked();
    const bool matchCase = m_fioMatchCase->isChecked();
    const bool wholeWord = m_fioWholeWord->isChecked();

    m_fioResults->clear();
    int totalMatches = 0;
    int buffersWithMatches = 0;
    // Phase 3d — walk both panes via MainWindow::allBuffers().
    for (Buffer* b : m_mw->allBuffers()) {
        if (!b) continue;
        // Pull text from Scintilla via GetText; the buffer may not have
        // a file path yet (untitled).
        auto* ed = b->editor();
        const Scintilla::sptr_t length =
            ed->send(static_cast<unsigned int>(Scintilla::Message::GetTextLength));
        if (length <= 0) continue;
        std::vector<char> buf(static_cast<std::size_t>(length) + 1, '\0');
        ed->send(static_cast<unsigned int>(Scintilla::Message::GetText),
                 static_cast<Scintilla::uptr_t>(buf.size()),
                 reinterpret_cast<Scintilla::sptr_t>(buf.data()));
        const QString text = QString::fromUtf8(buf.data(),
            static_cast<int>(length));
        const auto matches = matchesInText(text, needle,
            matchCase, wholeWord, regex);
        if (!matches.isEmpty()) {
            // Use displayName for untitled; full path otherwise.
            const QString labelPath = b->hasFile()
                ? b->filePath() : b->displayName();
            populateResultsTree(m_fioResults, labelPath, matches);
            totalMatches += matches.size();
            ++buffersWithMatches;
        }
    }
    setStatus(tr("Find in Open Files: %1 match(es) in %2 buffer(s).")
        .arg(totalMatches).arg(buffersWithMatches));
}

void FindReplaceDialog::onResultActivated(QTreeWidgetItem* item, int /*col*/)
{
    if (!item || !m_mw) return;
    const QString path = item->data(0, Qt::UserRole).toString();
    if (path.isEmpty()) return;   // file-level node — ignore double-click on header
    const int line = item->data(0, Qt::UserRole + 1).toInt();

    // For an open-buffer result, path may be a displayName ("Untitled 2");
    // try-open is fine — openFile will silently no-op for non-files. For a
    // real path, openFile either focuses or opens. Phase 3d: open in the
    // active pane; for an untitled match, search both panes' buffer lists.
    Buffer* b = nullptr;
    if (QFileInfo(path).exists()) {
        b = m_mw->activePane()->openFile(path);
    } else {
        // Untitled buffer match — search both panes for the displayName.
        for (Buffer* cand : m_mw->allBuffers()) {
            if (cand && cand->displayName() == path) { b = cand; break; }
        }
        if (b) {
            // Find the pane that owns it and switch.
            EditorTabs* hostL = m_mw->leftPane();
            EditorTabs* hostR = m_mw->rightPane();
            EditorTabs* host = (hostL->indexOfBuffer(b) >= 0) ? hostL : hostR;
            if (host) host->setCurrentIndex(host->indexOfBuffer(b));
        }
    }
    if (!b) return;
    auto* ed = b->editor();
    ed->send(static_cast<unsigned int>(Scintilla::Message::EnsureVisibleEnforcePolicy),
             static_cast<Scintilla::uptr_t>(line - 1));
    ed->send(static_cast<unsigned int>(Scintilla::Message::GotoLine),
             static_cast<Scintilla::uptr_t>(line - 1));
    ed->send(static_cast<unsigned int>(Scintilla::Message::VerticalCentreCaret));
    ed->setFocus();
}

int FindReplaceDialog::searchFiles(const QStringList& /*paths*/,
                                    const SearchOptions& /*opts*/,
                                    QTreeWidget* /*into*/)
{
    // Reserved for a future caller-facing API; today onFindAllInFiles
    // does the work inline. Keeping the signature in the header so callers
    // can be added without re-touching the .h.
    return 0;
}

// -----------------------------------------------------------------------------
// Read options + remember in combo history
// -----------------------------------------------------------------------------

FindReplaceDialog::SearchOptions FindReplaceDialog::readFindOptions() const
{
    SearchOptions o;
    o.findWhat   = m_findCombo->currentText();
    o.matchCase  = m_findMatchCase->isChecked();
    o.wholeWord  = m_findWholeWord->isChecked();
    o.wrapAround = m_findWrapAround->isChecked();
    o.dirDown    = m_findDirDown->isChecked();
    o.mode = m_findModeRegex->isChecked() ? Mode::Regex
           : m_findModeExt->isChecked()   ? Mode::Extended
                                          : Mode::Normal;
    return o;
}

FindReplaceDialog::SearchOptions FindReplaceDialog::readReplaceOptions() const
{
    SearchOptions o;
    o.findWhat    = m_repFindCombo->currentText();
    o.replaceWith = m_repWithCombo->currentText();
    o.matchCase   = m_repMatchCase->isChecked();
    o.wholeWord   = m_repWholeWord->isChecked();
    o.wrapAround  = m_repWrapAround->isChecked();
    o.dirDown     = m_repDirDown->isChecked();
    o.mode = m_repModeRegex->isChecked() ? Mode::Regex
          : m_repModeExt->isChecked()    ? Mode::Extended
                                         : Mode::Normal;
    return o;
}

// -----------------------------------------------------------------------------
// Search engine
// -----------------------------------------------------------------------------

int FindReplaceDialog::scintillaSearchFlags(const SearchOptions& o) const
{
    int flags = 0;
    if (o.matchCase) flags |= SCFIND_MATCHCASE;
    if (o.wholeWord) flags |= SCFIND_WHOLEWORD;
    if (o.mode == Mode::Regex) flags |= SCFIND_REGEXP | SCFIND_POSIX;
    return flags;
}

QByteArray FindReplaceDialog::expandToBytes(const QString& s, Mode m) const
{
    if (m != Mode::Extended) return s.toUtf8();

    // Port of Searching::convertExtendedToString from upstream
    // FindReplaceDlg.cpp:102-193, adapted to UTF-8 output. Walks codepoints
    // in 's' and emits a QByteArray (size-aware so embedded \0 survives).
    QByteArray out;
    out.reserve(s.size());

    auto appendCodepoint = [&out](uint cp) {
        // Encode a Unicode codepoint as UTF-8.
        if (cp < 0x80) {
            out.append(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.append(static_cast<char>(0xC0 | (cp >> 6)));
            out.append(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.append(static_cast<char>(0xE0 | (cp >> 12)));
            out.append(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.append(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.append(static_cast<char>(0xF0 | (cp >> 18)));
            out.append(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.append(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.append(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    };

    int i = 0;
    const int n = s.size();
    while (i < n) {
        QChar ch = s.at(i);
        if (ch == QChar('\\') && i + 1 < n) {
            QChar next = s.at(i + 1);
            switch (next.unicode()) {
                case 'r':  out.append('\r'); i += 2; continue;
                case 'n':  out.append('\n'); i += 2; continue;
                case 't':  out.append('\t'); i += 2; continue;
                case '0':  out.append('\0'); i += 2; continue;
                case '\\': out.append('\\'); i += 2; continue;
                case 'b': case 'd': case 'o': case 'x': case 'u': {
                    int size = 0, base = 0;
                    if (next == 'b')      { size = 8; base = 2; }
                    else if (next == 'o') { size = 3; base = 8; }
                    else if (next == 'd') { size = 3; base = 10; }
                    else if (next == 'x') { size = 2; base = 16; }
                    else                  { size = 4; base = 16; } // 'u'
                    int value = 0;
                    if (readBaseAscii(s, i + 2, size, base, &value)) {
                        appendCodepoint(static_cast<uint>(value));
                        i += 2 + size;
                        continue;
                    }
                    // Malformed escape: fall through to default (treat as text).
                    [[fallthrough]];
                }
                default:
                    out.append('\\');
                    out.append(QString(next).toUtf8());
                    i += 2;
                    continue;
            }
        }
        // Plain character: encode as UTF-8 (handles surrogate pairs via toUtf8).
        // Take the smallest substring that includes a full codepoint.
        if (ch.isHighSurrogate() && i + 1 < n && s.at(i + 1).isLowSurrogate()) {
            out.append(s.mid(i, 2).toUtf8());
            i += 2;
        } else {
            out.append(QString(ch).toUtf8());
            ++i;
        }
    }
    return out;
}

bool FindReplaceDialog::doFindNext(const SearchOptions& o, bool forwards)
{
    if (!m_editor) {
        setStatus(tr("No active editor."));
        return false;
    }
    if (o.findWhat.isEmpty()) {
        setStatus(tr("Find: empty search string."));
        return false;
    }

    const QByteArray needle = expandToBytes(o.findWhat, o.mode);
    if (needle.isEmpty()) {
        setStatus(tr("Find: search string expanded to empty."));
        return false;
    }
    const int flags = scintillaSearchFlags(o);
    sciSend(m_editor, Message::SetSearchFlags, static_cast<uptr_t>(flags));

    const sptr_t docLen = sciSend(m_editor, Message::GetLength);
    const sptr_t selStart = sciSend(m_editor, Message::GetSelectionStart);
    const sptr_t selEnd   = sciSend(m_editor, Message::GetSelectionEnd);

    auto runOnce = [&](sptr_t from, sptr_t to) -> sptr_t {
        sciSend(m_editor, Message::SetTargetStart, static_cast<uptr_t>(from));
        sciSend(m_editor, Message::SetTargetEnd,   static_cast<uptr_t>(to));
        return sciSends(m_editor, Message::SearchInTarget,
                        static_cast<uptr_t>(needle.size()), needle.constData());
    };

    sptr_t first  = forwards ? selEnd  : selStart;
    sptr_t firstE = forwards ? docLen  : 0;

    sptr_t pos = runOnce(first, firstE);
    bool wrapped = false;
    if (pos < 0 && o.wrapAround) {
        sptr_t wrapStart = forwards ? 0      : docLen;
        sptr_t wrapEnd   = forwards ? first  : first;
        pos = runOnce(wrapStart, wrapEnd);
        wrapped = (pos >= 0);
    }

    if (pos < 0) {
        setStatus(tr("Find: '%1' — 0 occurrences found.").arg(o.findWhat));
        return false;
    }

    const sptr_t mStart = sciSend(m_editor, Message::GetTargetStart);
    const sptr_t mEnd   = sciSend(m_editor, Message::GetTargetEnd);
    sciSend(m_editor, Message::SetSel,
            static_cast<uptr_t>(mStart), static_cast<sptr_t>(mEnd));
    sciSend(m_editor, Message::ScrollCaret);

    const sptr_t line = sciSend(m_editor, Message::LineFromPosition,
                                static_cast<uptr_t>(mStart)) + 1;
    if (wrapped) {
        setStatus(forwards
            ? tr("Find: reached end of document, wrapped to top — line %1.").arg(line)
            : tr("Find: reached top of document, wrapped to bottom — line %1.").arg(line));
    } else {
        setStatus(tr("Find: '%1' — match at line %2.").arg(o.findWhat).arg(line));
    }
    return true;
}

void FindReplaceDialog::doReplaceCurrent(const SearchOptions& o)
{
    if (!m_editor) {
        setStatus(tr("No active editor."));
        return;
    }
    if (o.findWhat.isEmpty()) {
        setStatus(tr("Replace: empty search string."));
        return;
    }

    // The upstream dialog's "Replace" button replaces the *current selection*
    // if it matches the search target, then advances to the next match. If the
    // current selection doesn't match (first click), it just advances to the
    // first match without replacing.
    const sptr_t selStart = sciSend(m_editor, Message::GetSelectionStart);
    const sptr_t selEnd   = sciSend(m_editor, Message::GetSelectionEnd);

    bool selectionIsMatch = false;
    if (selEnd > selStart) {
        const QByteArray needle = expandToBytes(o.findWhat, o.mode);
        const int flags = scintillaSearchFlags(o);
        sciSend(m_editor, Message::SetSearchFlags, static_cast<uptr_t>(flags));
        sciSend(m_editor, Message::SetTargetStart, static_cast<uptr_t>(selStart));
        sciSend(m_editor, Message::SetTargetEnd,   static_cast<uptr_t>(selEnd));
        const sptr_t found = sciSends(m_editor, Message::SearchInTarget,
            static_cast<uptr_t>(needle.size()), needle.constData());
        if (found >= 0) {
            const sptr_t mStart = sciSend(m_editor, Message::GetTargetStart);
            const sptr_t mEnd   = sciSend(m_editor, Message::GetTargetEnd);
            selectionIsMatch = (mStart == selStart && mEnd == selEnd);
        }
    }

    if (selectionIsMatch) {
        const QByteArray repl = expandToBytes(o.replaceWith, o.mode);
        // SetTargetStart/End were just set above to the selection range.
        sciSend(m_editor, Message::SetTargetStart, static_cast<uptr_t>(selStart));
        sciSend(m_editor, Message::SetTargetEnd,   static_cast<uptr_t>(selEnd));
        if (o.mode == Mode::Regex) {
            sciSends(m_editor, Message::ReplaceTargetRE,
                     static_cast<uptr_t>(repl.size()), repl.constData());
        } else {
            sciSends(m_editor, Message::ReplaceTarget,
                     static_cast<uptr_t>(repl.size()), repl.constData());
        }
        // After replacement, target end is updated; place caret there so the
        // next find advances past the replacement.
        const sptr_t after = sciSend(m_editor, Message::GetTargetEnd);
        sciSend(m_editor, Message::SetSel,
                static_cast<uptr_t>(after), static_cast<sptr_t>(after));
    }

    // Advance to next match.
    doFindNext(o, o.dirDown);
}

int FindReplaceDialog::doReplaceAllInEditor(ScintillaEditBase* editor,
                                            const SearchOptions& o)
{
    if (!editor || o.findWhat.isEmpty()) return 0;

    const QByteArray needle = expandToBytes(o.findWhat, o.mode);
    const QByteArray repl   = expandToBytes(o.replaceWith, o.mode);
    if (needle.isEmpty()) return 0;

    const int flags = scintillaSearchFlags(o);
    sciSend(editor, Message::SetSearchFlags, static_cast<uptr_t>(flags));
    sciSend(editor, Message::BeginUndoAction);

    int count = 0;
    sptr_t pos = 0;
    sptr_t docLen = sciSend(editor, Message::GetLength);

    while (true) {
        sciSend(editor, Message::SetTargetStart, static_cast<uptr_t>(pos));
        sciSend(editor, Message::SetTargetEnd,   static_cast<uptr_t>(docLen));
        const sptr_t found = sciSends(editor, Message::SearchInTarget,
            static_cast<uptr_t>(needle.size()), needle.constData());
        if (found < 0) break;

        const sptr_t matchStart = sciSend(editor, Message::GetTargetStart);
        sptr_t matchEnd   = sciSend(editor, Message::GetTargetEnd);

        sptr_t replLen;
        if (o.mode == Mode::Regex) {
            replLen = sciSends(editor, Message::ReplaceTargetRE,
                static_cast<uptr_t>(repl.size()), repl.constData());
        } else {
            replLen = sciSends(editor, Message::ReplaceTarget,
                static_cast<uptr_t>(repl.size()), repl.constData());
        }
        ++count;

        // Advance past the replacement; refresh doc length.
        const sptr_t newEnd = matchStart + replLen;
        // Empty match (e.g. regex /^/ or //) would cause an infinite loop —
        // step forward one byte to break it.
        pos = (newEnd > matchEnd) ? newEnd
            : (newEnd > matchStart) ? newEnd
            : matchStart + 1;
        docLen = sciSend(editor, Message::GetLength);
        if (pos > docLen) break;
    }

    sciSend(editor, Message::EndUndoAction);
    return count;
}

// -----------------------------------------------------------------------------
// Slots
// -----------------------------------------------------------------------------

void FindReplaceDialog::onFindNext()
{
    SearchOptions o = readFindOptions();
    rememberLastSearch(o);
    pushAndPersistHistory(o);
    doFindNext(o, true);
}

void FindReplaceDialog::onFindPrevious()
{
    SearchOptions o = readFindOptions();
    rememberLastSearch(o);
    pushAndPersistHistory(o);
    doFindNext(o, false);
}

void FindReplaceDialog::onFindCount()
{
    if (!m_editor) { setStatus(tr("No active editor.")); return; }
    SearchOptions o = readFindOptions();
    if (o.findWhat.isEmpty()) {
        setStatus(tr("Count: empty search string."));
        return;
    }
    rememberLastSearch(o);
    pushAndPersistHistory(o);

    const QByteArray needle = expandToBytes(o.findWhat, o.mode);
    if (needle.isEmpty()) { setStatus(tr("Count: empty needle.")); return; }
    const int flags = scintillaSearchFlags(o);
    sciSend(m_editor, Message::SetSearchFlags, static_cast<uptr_t>(flags));

    int count = 0;
    sptr_t pos = 0;
    const sptr_t docLen = sciSend(m_editor, Message::GetLength);
    while (true) {
        sciSend(m_editor, Message::SetTargetStart, static_cast<uptr_t>(pos));
        sciSend(m_editor, Message::SetTargetEnd,   static_cast<uptr_t>(docLen));
        const sptr_t found = sciSends(m_editor, Message::SearchInTarget,
            static_cast<uptr_t>(needle.size()), needle.constData());
        if (found < 0) break;
        ++count;
        const sptr_t mStart = sciSend(m_editor, Message::GetTargetStart);
        const sptr_t mEnd   = sciSend(m_editor, Message::GetTargetEnd);
        pos = (mEnd > mStart) ? mEnd : mStart + 1;
        if (pos > docLen) break;
    }
    setStatus(tr("Count: '%1' — %2 occurrence(s).").arg(o.findWhat).arg(count));
}

void FindReplaceDialog::onFindCloseClicked()
{
    close();
}

void FindReplaceDialog::onReplaceFindNext()
{
    SearchOptions o = readReplaceOptions();
    rememberLastSearch(o);
    pushAndPersistHistory(o);
    doFindNext(o, o.dirDown);
}

void FindReplaceDialog::onReplaceCurrent()
{
    SearchOptions o = readReplaceOptions();
    rememberLastSearch(o);
    pushAndPersistHistory(o);
    doReplaceCurrent(o);
}

void FindReplaceDialog::onReplaceAll()
{
    if (!m_editor) { setStatus(tr("No active editor.")); return; }
    SearchOptions o = readReplaceOptions();
    rememberLastSearch(o);
    pushAndPersistHistory(o);
    if (o.findWhat.isEmpty()) {
        setStatus(tr("Replace All: empty search string."));
        return;
    }
    const int n = doReplaceAllInEditor(m_editor, o);
    setStatus(n == 0
        ? tr("Replace All: '%1' — 0 occurrences replaced.").arg(o.findWhat)
        : tr("Replace All: replaced %1 occurrence(s) of '%2'.").arg(n).arg(o.findWhat));
}

void FindReplaceDialog::onReplaceAllInOpenDocs()
{
    if (!m_mw) { setStatus(tr("No tab container.")); return; }
    SearchOptions o = readReplaceOptions();
    rememberLastSearch(o);
    pushAndPersistHistory(o);
    if (o.findWhat.isEmpty()) {
        setStatus(tr("Replace All in All Open Documents: empty search string."));
        return;
    }
    int totalReplacements = 0;
    int touchedDocs = 0;
    // Phase 3d — walk both panes via MainWindow::allBuffers().
    for (Buffer* b : m_mw->allBuffers()) {
        if (!b) continue;
        const int n = doReplaceAllInEditor(b->editor(), o);
        if (n > 0) { totalReplacements += n; ++touchedDocs; }
    }
    setStatus(tr("Replace All in All Open Documents: replaced %1 occurrence(s) "
                 "across %2 document(s).").arg(totalReplacements).arg(touchedDocs));
}

void FindReplaceDialog::onReplaceCloseClicked()
{
    close();
}

// -----------------------------------------------------------------------------

void FindReplaceDialog::setStatus(const QString& msg)
{
    if (m_statusLabel) m_statusLabel->setText(msg);
    emit statusMessage(msg);
}

// Phase 9b.3 — push the current find/replace strings to the combo
// history (move-to-front, dedupe, cap at 10) and persist via Config.
// Called from rememberAndPersistHistory at every successful Find /
// Replace site so a crash mid-session preserves history. Both find
// combos stay in sync.
namespace {
void pushFront(QComboBox* combo, const QString& text, int cap)
{
    if (!combo || text.isEmpty()) return;
    // Remove every prior occurrence so the new item floats to the top.
    int existing = combo->findText(text);
    while (existing >= 0) {
        combo->removeItem(existing);
        existing = combo->findText(text);
    }
    combo->insertItem(0, text);
    while (combo->count() > cap) combo->removeItem(combo->count() - 1);
}

QStringList captureItems(QComboBox* combo, int cap)
{
    QStringList out;
    if (!combo) return out;
    const int n = qMin(combo->count(), cap);
    out.reserve(n);
    for (int i = 0; i < n; ++i) out.append(combo->itemText(i));
    return out;
}
} // namespace

void FindReplaceDialog::pushAndPersistHistory(const SearchOptions& o)
{
    constexpr int kCap = 10;
    if (!o.findWhat.isEmpty()) {
        pushFront(m_findCombo,    o.findWhat, kCap);
        pushFront(m_repFindCombo, o.findWhat, kCap);
    }
    if (!o.replaceWith.isEmpty()) {
        pushFront(m_repWithCombo, o.replaceWith, kCap);
    }
    Config::setFindHistory(captureItems(m_findCombo, kCap));
    Config::setReplaceHistory(captureItems(m_repWithCombo, kCap));
    Config::save();
}
