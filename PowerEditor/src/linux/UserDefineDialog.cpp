// UserDefineDialog.cpp — Phase 5U.3 MVP. See UserDefineDialog.h.

#include "UserDefineDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include "Localization.h"   // Phase 8b-polish-2 — applyToDialog
#include "SciLexer.h"   // SCE_USER_KWLIST_*, SCE_USER_MASK_NESTING_*

namespace {

// 21 SCE_USER_MASK_NESTING_* flags upstream surfaces in StylerDlg
// (UserDefineDialog.h:178-198). Order mirrors the upstream group layout:
// delimiters → comments → keywords → operators → numbers. The 6 FOLDER
// nesting masks (CODE2_OPEN/MIDDLE/CLOSE + COMMENT_OPEN/MIDDLE/CLOSE) are
// not exposed in the upstream UI; we round-trip them via UDLStyle.nesting
// without dedicated checkboxes.
struct NestingFlag { const char* label; int mask; };
constexpr NestingFlag kNestingFlags[21] = {
    {"Delimiter 1",  SCE_USER_MASK_NESTING_DELIMITER1},
    {"Delimiter 2",  SCE_USER_MASK_NESTING_DELIMITER2},
    {"Delimiter 3",  SCE_USER_MASK_NESTING_DELIMITER3},
    {"Delimiter 4",  SCE_USER_MASK_NESTING_DELIMITER4},
    {"Delimiter 5",  SCE_USER_MASK_NESTING_DELIMITER5},
    {"Delimiter 6",  SCE_USER_MASK_NESTING_DELIMITER6},
    {"Delimiter 7",  SCE_USER_MASK_NESTING_DELIMITER7},
    {"Delimiter 8",  SCE_USER_MASK_NESTING_DELIMITER8},
    {"Comment",      SCE_USER_MASK_NESTING_COMMENT},
    {"Line comment", SCE_USER_MASK_NESTING_COMMENT_LINE},
    {"Keyword 1",    SCE_USER_MASK_NESTING_KEYWORD1},
    {"Keyword 2",    SCE_USER_MASK_NESTING_KEYWORD2},
    {"Keyword 3",    SCE_USER_MASK_NESTING_KEYWORD3},
    {"Keyword 4",    SCE_USER_MASK_NESTING_KEYWORD4},
    {"Keyword 5",    SCE_USER_MASK_NESTING_KEYWORD5},
    {"Keyword 6",    SCE_USER_MASK_NESTING_KEYWORD6},
    {"Keyword 7",    SCE_USER_MASK_NESTING_KEYWORD7},
    {"Keyword 8",    SCE_USER_MASK_NESTING_KEYWORD8},
    {"Operators 1",  SCE_USER_MASK_NESTING_OPERATORS1},
    {"Operators 2",  SCE_USER_MASK_NESTING_OPERATORS2},
    {"Numbers",      SCE_USER_MASK_NESTING_NUMBERS},
};

// 24 canonical post-2.0 style names. Mirrors the post-2.0 entries in
// upstream's `styleIdMapper` (UserDefineDialog.h:147-170). New UDLs
// have no <WordsStyle> rows yet — these names seed the list so users
// can edit nesting on a fresh UDL without the "save first to populate"
// nudge.
constexpr const char* kCanonicalStyleNames[24] = {
    "DEFAULT",
    "COMMENTS",
    "LINE COMMENTS",
    "NUMBERS",
    "KEYWORDS1", "KEYWORDS2", "KEYWORDS3", "KEYWORDS4",
    "KEYWORDS5", "KEYWORDS6", "KEYWORDS7", "KEYWORDS8",
    "OPERATORS",
    "FOLDER IN CODE1", "FOLDER IN CODE2", "FOLDER IN COMMENT",
    "DELIMITERS1", "DELIMITERS2", "DELIMITERS3", "DELIMITERS4",
    "DELIMITERS5", "DELIMITERS6", "DELIMITERS7", "DELIMITERS8",
};

// Upstream policy: nesting checkboxes are only enabled for COMMENT /
// COMMENTLINE / DELIMITERS1-8 styles. See the StylerDlg constructor
// callsites in UserDefineDialog.cpp:706-748 (DELIMITERS) + 496-502
// (COMMENT/COMMENTLINE) — those omit the `enabledNesters` arg so it
// defaults to -1 (all enabled). Other styles pass NESTING_NONE = 0.
bool styleAllowsNesting(const QString& name)
{
    if (name == QLatin1String("COMMENTS"))      return true;
    if (name == QLatin1String("LINE COMMENTS")) return true;
    // Pre-2.0 aliases — the styleNameMapper round-trips them.
    if (name == QLatin1String("COMMENT"))       return true;
    if (name == QLatin1String("COMMENT LINE"))  return true;
    if (name.startsWith(QLatin1String("DELIMITERS"))) return true;
    if (name.startsWith(QLatin1String("DELIMINER")))  return true;  // upstream typo
    return false;
}

UDLStyle* findOrCreateStyle(UDL& udl, const QString& name)
{
    for (UDLStyle& s : udl.styles) {
        if (s.name == name) return &s;
    }
    UDLStyle st;
    st.name = name;
    udl.styles.append(std::move(st));
    return &udl.styles.last();
}

const UDLStyle* findStyle(const UDL& udl, const QString& name)
{
    for (const UDLStyle& s : udl.styles) {
        if (s.name == name) return &s;
    }
    return nullptr;
}

// Style names to surface in the QListWidget: the 24 canonical names
// always present, plus any extras the loaded UDL XML carries that
// don't already appear (e.g. legacy aliases like KEYWORD1 / DELIMINER1).
// Using a QStringList preserves the canonical ordering for the daily
// case while round-tripping unusual files faithfully.
QStringList allStyleNamesIn(const UDL& udl)
{
    QStringList names;
    names.reserve(24);
    for (const char* n : kCanonicalStyleNames) names.append(QString::fromLatin1(n));
    for (const UDLStyle& s : udl.styles) {
        if (s.name.isEmpty()) continue;
        if (!names.contains(s.name)) names.append(s.name);
    }
    return names;
}

} // namespace

UserDefineDialog::UserDefineDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("User Defined Language"));
    resize(720, 540);

    m_udls = UserDefineLang::loadAll();
    buildUi();

    rebuildCombo(m_udls.isEmpty() ? -1 : 0);

    // Phase 8b-polish-2 — apply active language overlay. The
    // <UserDefine> block in english.xml carries the dialog's
    // window title + 4 sub-tab titles (Folder, Keywords, Comment,
    // Operator); the per-tab labels we wired in 5U.3-polish are
    // largely upstream-aligned so source-text matching covers them.
    Localization::applyToDialog(this, "UserDefine");
}

// -----------------------------------------------------------------------------
// UI construction
// -----------------------------------------------------------------------------

void UserDefineDialog::buildUi()
{
    auto* root = new QVBoxLayout(this);

    // Top row — UDL combo + buttons.
    {
        auto* row = new QHBoxLayout;
        row->addWidget(new QLabel(tr("Language:"), this));
        m_combo = new QComboBox(this);
        m_combo->setMinimumWidth(200);
        connect(m_combo, &QComboBox::currentIndexChanged,
                this, &UserDefineDialog::onSelectionChanged);
        row->addWidget(m_combo, 1);

        m_btnNew    = new QPushButton(tr("&New"),    this);
        m_btnRename = new QPushButton(tr("&Rename"), this);
        m_btnDelete = new QPushButton(tr("&Delete"), this);
        m_btnSave   = new QPushButton(tr("&Save"),   this);
        connect(m_btnNew,    &QPushButton::clicked, this, &UserDefineDialog::onNew);
        connect(m_btnRename, &QPushButton::clicked, this, &UserDefineDialog::onRename);
        connect(m_btnDelete, &QPushButton::clicked, this, &UserDefineDialog::onDelete);
        connect(m_btnSave,   &QPushButton::clicked, this, &UserDefineDialog::onSave);
        row->addWidget(m_btnNew);
        row->addWidget(m_btnRename);
        row->addWidget(m_btnDelete);
        row->addStretch();
        row->addWidget(m_btnSave);
        root->addLayout(row);
    }

    // Tab body
    m_tabs = new QTabWidget(this);
    m_tabs->addTab(buildFolderTab(),               tr("Folder && &Default"));
    m_tabs->addTab(buildKeywordsTab(),             tr("&Keywords Lists"));
    m_tabs->addTab(buildCommentNumberTab(),        tr("&Comment && Number"));
    m_tabs->addTab(buildOperatorsDelimitersTab(),  tr("&Operators && Delimiters"));
    m_tabs->addTab(buildStylesTab(),               tr("&Styles"));
    root->addWidget(m_tabs, 1);

    // Bottom row — Close button only. Save lives on the top row so it's
    // visible regardless of the active tab.
    {
        auto* row = new QHBoxLayout;
        row->addStretch();
        m_btnClose = new QPushButton(tr("&Close"), this);
        connect(m_btnClose, &QPushButton::clicked, this, &UserDefineDialog::onClose);
        row->addWidget(m_btnClose);
        root->addLayout(row);
    }
}

QWidget* UserDefineDialog::buildFolderTab()
{
    auto* w = new QWidget(this);
    auto* root = new QVBoxLayout(w);

    auto* form = new QFormLayout;

    m_extEdit = new QLineEdit(w);
    m_extEdit->setPlaceholderText(tr("e.g. ml mli"));
    m_extEdit->setToolTip(tr(
        "Space-separated extensions, lowercase. A UDL with a matching "
        "extension overrides built-in lexers (e.g. UDL `ml` overrides CAML)."));
    connect(m_extEdit, &QLineEdit::textChanged,
            this, &UserDefineDialog::onTab1FieldChanged);
    form->addRow(tr("Extensions:"), m_extEdit);

    m_caseIgnored = new QCheckBox(tr("Ignore case in keyword matching"), w);
    connect(m_caseIgnored, &QCheckBox::toggled,
            this, &UserDefineDialog::onTab1FieldChanged);
    form->addRow(QString(), m_caseIgnored);

    m_allowFoldOfComments = new QCheckBox(tr("Allow folding of comments"), w);
    connect(m_allowFoldOfComments, &QCheckBox::toggled,
            this, &UserDefineDialog::onTab1FieldChanged);
    form->addRow(QString(), m_allowFoldOfComments);

    m_foldCompact = new QCheckBox(tr("Fold compact"), w);
    connect(m_foldCompact, &QCheckBox::toggled,
            this, &UserDefineDialog::onTab1FieldChanged);
    form->addRow(QString(), m_foldCompact);

    m_forcePureLC = new QComboBox(w);
    m_forcePureLC->addItem(tr("None"),                  0);
    m_forcePureLC->addItem(tr("Beginning of line only"), 1);
    m_forcePureLC->addItem(tr("Whitespace-prefixed"),    2);
    connect(m_forcePureLC, &QComboBox::currentIndexChanged,
            this, &UserDefineDialog::onTab1FieldChanged);
    form->addRow(tr("Force pure line comment:"), m_forcePureLC);

    m_decimalSeparator = new QComboBox(w);
    m_decimalSeparator->addItem(tr("Dot (.)"),   0);
    m_decimalSeparator->addItem(tr("Comma (,)"), 1);
    m_decimalSeparator->addItem(tr("Both"),       2);
    connect(m_decimalSeparator, &QComboBox::currentIndexChanged,
            this, &UserDefineDialog::onTab1FieldChanged);
    form->addRow(tr("Decimal separator:"), m_decimalSeparator);

    root->addLayout(form);

    auto* foldGroup = new QGroupBox(tr("Folder Markers (Code 1)"), w);
    auto* foldForm  = new QFormLayout(foldGroup);

    m_foldOpen = new QLineEdit(foldGroup);
    m_foldOpen->setPlaceholderText(tr("e.g. begin {"));
    connect(m_foldOpen, &QLineEdit::textChanged,
            this, &UserDefineDialog::onTab1FieldChanged);
    foldForm->addRow(tr("Open:"), m_foldOpen);

    m_foldMiddle = new QLineEdit(foldGroup);
    m_foldMiddle->setPlaceholderText(tr("e.g. else"));
    connect(m_foldMiddle, &QLineEdit::textChanged,
            this, &UserDefineDialog::onTab1FieldChanged);
    foldForm->addRow(tr("Middle:"), m_foldMiddle);

    m_foldClose = new QLineEdit(foldGroup);
    m_foldClose->setPlaceholderText(tr("e.g. end }"));
    connect(m_foldClose, &QLineEdit::textChanged,
            this, &UserDefineDialog::onTab1FieldChanged);
    foldForm->addRow(tr("Close:"), m_foldClose);

    root->addWidget(foldGroup);
    root->addStretch(1);
    return w;
}

QWidget* UserDefineDialog::buildKeywordsTab()
{
    auto* w = new QWidget(this);
    auto* root = new QVBoxLayout(w);

    auto* note = new QLabel(
        tr("Eight keyword groups. Each maps to a distinct style (Keyword1..8) "
           "the active theme can colour. Whitespace-separate the words; "
           "prefix mode matches a token that starts-with any word in the list."),
        w);
    note->setWordWrap(true);
    note->setStyleSheet(QStringLiteral("color: gray; font-style: italic;"));
    root->addWidget(note);

    auto* tabs = new QTabWidget(w);
    for (int i = 0; i < 8; ++i) {
        auto* page = new QWidget(tabs);
        auto* pl   = new QVBoxLayout(page);

        m_kwPrefix[i] = new QCheckBox(
            tr("Match as prefix (token starts-with any word)"), page);
        connect(m_kwPrefix[i], &QCheckBox::toggled,
                this, &UserDefineDialog::onTab2FieldChanged);
        pl->addWidget(m_kwPrefix[i]);

        m_kwEdit[i] = new QPlainTextEdit(page);
        m_kwEdit[i]->setPlaceholderText(
            tr("Whitespace-separated keywords for group %1.").arg(i + 1));
        connect(m_kwEdit[i], &QPlainTextEdit::textChanged,
                this, &UserDefineDialog::onTab2FieldChanged);
        pl->addWidget(m_kwEdit[i], 1);

        tabs->addTab(page, tr("Keywords %1").arg(i + 1));
    }
    root->addWidget(tabs, 1);
    return w;
}

QWidget* UserDefineDialog::buildCommentNumberTab()
{
    auto* w = new QWidget(this);
    auto* root = new QVBoxLayout(w);

    auto* note = new QLabel(
        tr("Comments and numbers share keyword-list slots in the underlying "
           "lexer. Leave a slot blank to disable that style. The decimal "
           "separator and \"force pure line comment\" mode live on the "
           "Folder && Default tab."),
        w);
    note->setWordWrap(true);
    note->setStyleSheet(QStringLiteral("color: gray; font-style: italic;"));
    root->addWidget(note);

    // Comments group — line markers + block markers.
    auto* commentGroup = new QGroupBox(tr("Comments"), w);
    auto* commentForm  = new QFormLayout(commentGroup);

    auto wireLine = [this](QLineEdit*& target, QWidget* parent,
                           const char* placeholder) {
        target = new QLineEdit(parent);
        target->setPlaceholderText(QString::fromLatin1(placeholder));
        connect(target, &QLineEdit::textChanged,
                this, &UserDefineDialog::onTab3FieldChanged);
    };

    wireLine(m_commentLineOpen,     commentGroup, "// or # or --");
    commentForm->addRow(tr("Line opener:"), m_commentLineOpen);

    wireLine(m_commentLineContinue, commentGroup, "rare; e.g. \\\\");
    commentForm->addRow(tr("Line continuation:"), m_commentLineContinue);

    wireLine(m_commentLineClose,    commentGroup, "rare");
    commentForm->addRow(tr("Line closer:"), m_commentLineClose);

    wireLine(m_commentBlockOpen,    commentGroup, "/*");
    commentForm->addRow(tr("Block open:"), m_commentBlockOpen);

    wireLine(m_commentBlockClose,   commentGroup, "*/");
    commentForm->addRow(tr("Block close:"), m_commentBlockClose);

    root->addWidget(commentGroup);

    // Numbers group — keyword slots that the lexer reads as number patterns.
    auto* numberGroup = new QGroupBox(tr("Numbers"), w);
    auto* numberForm  = new QFormLayout(numberGroup);

    wireLine(m_numberPrefix1, numberGroup, "0x 0X");
    numberForm->addRow(tr("Prefix 1:"), m_numberPrefix1);

    wireLine(m_numberPrefix2, numberGroup, "");
    numberForm->addRow(tr("Prefix 2:"), m_numberPrefix2);

    wireLine(m_numberExtras1, numberGroup, "A B C D E F a b c d e f");
    numberForm->addRow(tr("Extras 1:"), m_numberExtras1);

    wireLine(m_numberExtras2, numberGroup, "");
    numberForm->addRow(tr("Extras 2:"), m_numberExtras2);

    wireLine(m_numberSuffix1, numberGroup, "L UL u");
    numberForm->addRow(tr("Suffix 1:"), m_numberSuffix1);

    wireLine(m_numberSuffix2, numberGroup, "");
    numberForm->addRow(tr("Suffix 2:"), m_numberSuffix2);

    wireLine(m_numberRange,   numberGroup, "0 1 2 3 4 5 6 7 8 9");
    numberForm->addRow(tr("Range:"), m_numberRange);

    root->addWidget(numberGroup);
    root->addStretch(1);
    return w;
}

QWidget* UserDefineDialog::buildOperatorsDelimitersTab()
{
    auto* w = new QWidget(this);
    auto* root = new QVBoxLayout(w);

    auto* note = new QLabel(
        tr("Operators are space-separated tokens. Each delimiter pair binds "
           "an open boundary, a close boundary, and an optional escape "
           "character (e.g. open=\", close=\", escape=\\)."),
        w);
    note->setWordWrap(true);
    note->setStyleSheet(QStringLiteral("color: gray; font-style: italic;"));
    root->addWidget(note);

    // Operators group.
    auto* opsGroup = new QGroupBox(tr("Operators"), w);
    auto* opsLayout = new QFormLayout(opsGroup);

    m_operators1 = new QPlainTextEdit(opsGroup);
    m_operators1->setPlaceholderText(tr("Whitespace-separated operator tokens (group 1)."));
    m_operators1->setFixedHeight(60);
    connect(m_operators1, &QPlainTextEdit::textChanged,
            this, &UserDefineDialog::onTab4FieldChanged);
    opsLayout->addRow(tr("Operators 1:"), m_operators1);

    m_operators2 = new QPlainTextEdit(opsGroup);
    m_operators2->setPlaceholderText(tr("Whitespace-separated operator tokens (group 2)."));
    m_operators2->setFixedHeight(60);
    connect(m_operators2, &QPlainTextEdit::textChanged,
            this, &UserDefineDialog::onTab4FieldChanged);
    opsLayout->addRow(tr("Operators 2:"), m_operators2);

    root->addWidget(opsGroup);

    // Delimiter pairs — 8 rows × 3 columns (Open / Escape / Close).
    auto* delGroup = new QGroupBox(tr("Delimiters (8 pairs)"), w);
    auto* delLayout = new QVBoxLayout(delGroup);

    m_delimitersTable = new QTableWidget(8, 3, delGroup);
    m_delimitersTable->setHorizontalHeaderLabels(
        {tr("Open"), tr("Escape"), tr("Close")});
    QStringList rowHeaders;
    for (int i = 1; i <= 8; ++i) rowHeaders << tr("Pair %1").arg(i);
    m_delimitersTable->setVerticalHeaderLabels(rowHeaders);
    m_delimitersTable->horizontalHeader()->setSectionResizeMode(
        QHeaderView::Stretch);
    m_delimitersTable->verticalHeader()->setSectionResizeMode(
        QHeaderView::Fixed);
    m_delimitersTable->setSelectionMode(QAbstractItemView::SingleSelection);
    // Pre-populate with empty items so the cellChanged signal fires for any
    // edit (a null QTableWidgetItem skips the signal until first set).
    for (int r = 0; r < 8; ++r) {
        for (int c = 0; c < 3; ++c) {
            m_delimitersTable->setItem(r, c, new QTableWidgetItem(QString()));
        }
    }
    connect(m_delimitersTable, &QTableWidget::cellChanged,
            this, &UserDefineDialog::onDelimiterCellChanged);
    delLayout->addWidget(m_delimitersTable);

    root->addWidget(delGroup, 1);
    return w;
}

QWidget* UserDefineDialog::buildStylesTab()
{
    auto* w = new QWidget(this);
    auto* root = new QHBoxLayout(w);

    // Left: scrollable QListWidget of style names. Width pinned just
    // wide enough for "FOLDER IN COMMENT" without truncation.
    m_stylesList = new QListWidget(w);
    m_stylesList->setMinimumWidth(180);
    m_stylesList->setMaximumWidth(220);
    connect(m_stylesList, &QListWidget::currentRowChanged,
            this, &UserDefineDialog::onStyleListSelectionChanged);
    root->addWidget(m_stylesList);

    // Right: nesting checkboxes grouped by category, plus a note up top.
    auto* rightW = new QWidget(w);
    auto* right  = new QVBoxLayout(rightW);

    auto* note = new QLabel(
        tr("Per-style nesting determines which other styles can start "
           "inside this one (e.g. Markdown's DELIMITERS4 nests KEYWORD7 + "
           "DELIMITER7 = 65600). Upstream restricts editing to the COMMENTS, "
           "LINE COMMENTS, and DELIMITERS1-8 styles — checkboxes grey out "
           "for other style rows."),
        rightW);
    note->setWordWrap(true);
    note->setStyleSheet(QStringLiteral("color: gray; font-style: italic;"));
    right->addWidget(note);

    int idx = 0;
    auto addGroup = [&](const QString& title, int count) {
        auto* g = new QGroupBox(title, rightW);
        auto* grid = new QGridLayout(g);
        for (int k = 0; k < count; ++k) {
            const auto& nf = kNestingFlags[idx];
            auto* cb = new QCheckBox(tr(nf.label), g);
            connect(cb, &QCheckBox::toggled,
                    this, &UserDefineDialog::onNestingChanged);
            m_nestingChecks[idx] = cb;
            // 2-column grid; row index = k/2 floors evenly.
            grid->addWidget(cb, k / 2, k % 2);
            ++idx;
        }
        right->addWidget(g);
    };

    addGroup(tr("Delimiters"), 8);
    addGroup(tr("Comments"),   2);
    addGroup(tr("Keywords"),   8);
    addGroup(tr("Operators"),  2);
    addGroup(tr("Numbers"),    1);
    right->addStretch(1);

    root->addWidget(rightW, 1);
    return w;
}

// -----------------------------------------------------------------------------
// Combo / model / widgets sync
// -----------------------------------------------------------------------------

void UserDefineDialog::rebuildCombo(int selectIdx)
{
    m_loadingFromModel = true;
    m_combo->clear();
    for (const UDL& udl : m_udls) m_combo->addItem(udl.name);
    m_loadingFromModel = false;

    if (m_udls.isEmpty()) {
        m_currentIdx = -1;
        loadIntoWidgets();           // clears widgets
        updateControlsEnabled();
        return;
    }
    if (selectIdx < 0 || selectIdx >= m_udls.size()) selectIdx = 0;
    m_combo->setCurrentIndex(selectIdx);
    // setCurrentIndex emits currentIndexChanged → onSelectionChanged →
    // m_currentIdx + loadIntoWidgets + updateControlsEnabled.
}

void UserDefineDialog::onSelectionChanged(int row)
{
    if (m_loadingFromModel) return;
    // Persist any pending edits in the previously-active row first.
    if (m_currentIdx >= 0 && m_currentIdx < m_udls.size()) writeBackCurrent();
    m_currentIdx = row;
    loadIntoWidgets();
    updateControlsEnabled();
}

void UserDefineDialog::loadIntoWidgets()
{
    m_loadingFromModel = true;

    UDL blank;   // used when nothing's selected
    const UDL& src = (m_currentIdx >= 0 && m_currentIdx < m_udls.size())
        ? m_udls[m_currentIdx] : blank;

    // Tab 1
    m_extEdit->setText(src.ext);
    m_caseIgnored->setChecked(src.isCaseIgnored);
    m_allowFoldOfComments->setChecked(src.allowFoldOfComments);
    m_foldCompact->setChecked(src.foldCompact);
    m_forcePureLC->setCurrentIndex(qBound(0, src.forcePureLC, 2));
    m_decimalSeparator->setCurrentIndex(qBound(0, src.decimalSeparator, 2));
    m_foldOpen  ->setText(src.keywords[SCE_USER_KWLIST_FOLDERS_IN_CODE1_OPEN]);
    m_foldMiddle->setText(src.keywords[SCE_USER_KWLIST_FOLDERS_IN_CODE1_MIDDLE]);
    m_foldClose ->setText(src.keywords[SCE_USER_KWLIST_FOLDERS_IN_CODE1_CLOSE]);

    // Tab 2
    for (int i = 0; i < 8; ++i) {
        m_kwEdit[i]->setPlainText(src.keywords[SCE_USER_KWLIST_KEYWORDS1 + i]);
        m_kwPrefix[i]->setChecked(src.isPrefix[i]);
    }

    // Tab 3 — Comments are packed into keywords[SCE_USER_KWLIST_COMMENTS]
    // via the prefix-tag encoding (see UserDefineLang::decodeComments).
    {
        const QStringList c = UserDefineLang::decodeComments(
            src.keywords[SCE_USER_KWLIST_COMMENTS]);
        m_commentLineOpen    ->setText(c.value(0));
        m_commentLineContinue->setText(c.value(1));
        m_commentLineClose   ->setText(c.value(2));
        m_commentBlockOpen   ->setText(c.value(3));
        m_commentBlockClose  ->setText(c.value(4));
    }
    m_numberPrefix1->setText(src.keywords[SCE_USER_KWLIST_NUMBER_PREFIX1]);
    m_numberPrefix2->setText(src.keywords[SCE_USER_KWLIST_NUMBER_PREFIX2]);
    m_numberExtras1->setText(src.keywords[SCE_USER_KWLIST_NUMBER_EXTRAS1]);
    m_numberExtras2->setText(src.keywords[SCE_USER_KWLIST_NUMBER_EXTRAS2]);
    m_numberSuffix1->setText(src.keywords[SCE_USER_KWLIST_NUMBER_SUFFIX1]);
    m_numberSuffix2->setText(src.keywords[SCE_USER_KWLIST_NUMBER_SUFFIX2]);
    m_numberRange  ->setText(src.keywords[SCE_USER_KWLIST_NUMBER_RANGE]);

    // Tab 4 — Operators map 1:1 to their keyword slots; delimiter pairs
    // come back from decodeDelimiters as 8 (open, escape, close) triples.
    m_operators1->setPlainText(src.keywords[SCE_USER_KWLIST_OPERATORS1]);
    m_operators2->setPlainText(src.keywords[SCE_USER_KWLIST_OPERATORS2]);
    {
        QSignalBlocker block(m_delimitersTable);   // suppress cellChanged
        const QVector<DelimiterPair> pairs = UserDefineLang::decodeDelimiters(
            src.keywords[SCE_USER_KWLIST_DELIMITERS]);
        for (int r = 0; r < 8; ++r) {
            const DelimiterPair p = (r < pairs.size()) ? pairs[r] : DelimiterPair{};
            m_delimitersTable->item(r, 0)->setText(p.open);
            m_delimitersTable->item(r, 1)->setText(p.escape);
            m_delimitersTable->item(r, 2)->setText(p.close);
        }
    }

    // Tab 5 — Styles. Repopulate the list to reflect this UDL's
    // canonical names + any extras carried in its <Styles> block, then
    // refresh the checkbox panel for whichever row ends up selected.
    {
        QSignalBlocker block(m_stylesList);
        m_stylesList->clear();
        for (const QString& n : allStyleNamesIn(src)) {
            m_stylesList->addItem(n);
        }
        if (m_stylesList->count() > 0) m_stylesList->setCurrentRow(0);
    }
    loadStyleRowIntoWidgets();

    m_loadingFromModel = false;
}

void UserDefineDialog::loadStylesIntoWidgets()
{
    // Convenience wrapper for code paths (currently the styles-tab
    // selection slot) that just need the row refresh without rebuilding
    // the list. Today this is a one-liner; preserved so the .h public
    // surface matches the comment in the header.
    loadStyleRowIntoWidgets();
}

void UserDefineDialog::loadStyleRowIntoWidgets()
{
    const bool prev = m_loadingFromModel;
    m_loadingFromModel = true;

    int  nesting = 0;
    bool allowed = false;
    QString rowName;
    if (m_currentIdx >= 0 && m_currentIdx < m_udls.size()
        && m_stylesList && m_stylesList->currentItem())
    {
        rowName = m_stylesList->currentItem()->text();
        const UDL& udl = m_udls[m_currentIdx];
        if (const UDLStyle* s = findStyle(udl, rowName)) nesting = s->nesting;
        allowed = styleAllowsNesting(rowName);
    }
    for (int i = 0; i < 21; ++i) {
        if (!m_nestingChecks[i]) continue;
        m_nestingChecks[i]->setChecked((nesting & kNestingFlags[i].mask) != 0);
        m_nestingChecks[i]->setEnabled(allowed);
    }

    m_loadingFromModel = prev;
}

void UserDefineDialog::writeBackCurrent()
{
    if (m_currentIdx < 0 || m_currentIdx >= m_udls.size()) return;
    UDL& dst = m_udls[m_currentIdx];

    dst.ext = m_extEdit->text();
    dst.isCaseIgnored       = m_caseIgnored->isChecked();
    dst.allowFoldOfComments = m_allowFoldOfComments->isChecked();
    dst.foldCompact         = m_foldCompact->isChecked();
    dst.forcePureLC         = m_forcePureLC->currentIndex();
    dst.decimalSeparator    = m_decimalSeparator->currentIndex();
    dst.keywords[SCE_USER_KWLIST_FOLDERS_IN_CODE1_OPEN]   = m_foldOpen->text();
    dst.keywords[SCE_USER_KWLIST_FOLDERS_IN_CODE1_MIDDLE] = m_foldMiddle->text();
    dst.keywords[SCE_USER_KWLIST_FOLDERS_IN_CODE1_CLOSE]  = m_foldClose->text();

    for (int i = 0; i < 8; ++i) {
        dst.keywords[SCE_USER_KWLIST_KEYWORDS1 + i] = m_kwEdit[i]->toPlainText();
        dst.isPrefix[i] = m_kwPrefix[i]->isChecked();
    }

    // Tab 3 — pack the 5 comment slots back into the prefix-tag string.
    {
        QStringList c;
        c << m_commentLineOpen->text()
          << m_commentLineContinue->text()
          << m_commentLineClose->text()
          << m_commentBlockOpen->text()
          << m_commentBlockClose->text();
        dst.keywords[SCE_USER_KWLIST_COMMENTS] = UserDefineLang::encodeComments(c);
    }
    dst.keywords[SCE_USER_KWLIST_NUMBER_PREFIX1] = m_numberPrefix1->text();
    dst.keywords[SCE_USER_KWLIST_NUMBER_PREFIX2] = m_numberPrefix2->text();
    dst.keywords[SCE_USER_KWLIST_NUMBER_EXTRAS1] = m_numberExtras1->text();
    dst.keywords[SCE_USER_KWLIST_NUMBER_EXTRAS2] = m_numberExtras2->text();
    dst.keywords[SCE_USER_KWLIST_NUMBER_SUFFIX1] = m_numberSuffix1->text();
    dst.keywords[SCE_USER_KWLIST_NUMBER_SUFFIX2] = m_numberSuffix2->text();
    dst.keywords[SCE_USER_KWLIST_NUMBER_RANGE]   = m_numberRange->text();

    // Tab 4 — operators map 1:1; delimiter table packs into the prefix string.
    dst.keywords[SCE_USER_KWLIST_OPERATORS1] = m_operators1->toPlainText();
    dst.keywords[SCE_USER_KWLIST_OPERATORS2] = m_operators2->toPlainText();
    {
        QVector<DelimiterPair> pairs;
        pairs.reserve(8);
        for (int r = 0; r < 8; ++r) {
            DelimiterPair p;
            if (auto* it = m_delimitersTable->item(r, 0)) p.open   = it->text();
            if (auto* it = m_delimitersTable->item(r, 1)) p.escape = it->text();
            if (auto* it = m_delimitersTable->item(r, 2)) p.close  = it->text();
            pairs.append(p);
        }
        dst.keywords[SCE_USER_KWLIST_DELIMITERS] = UserDefineLang::encodeDelimiters(pairs);
    }
}

void UserDefineDialog::onTab1FieldChanged()
{
    if (m_loadingFromModel) return;
    writeBackCurrent();
}

void UserDefineDialog::onTab2FieldChanged()
{
    if (m_loadingFromModel) return;
    writeBackCurrent();
}

void UserDefineDialog::onTab3FieldChanged()
{
    if (m_loadingFromModel) return;
    writeBackCurrent();
}

void UserDefineDialog::onTab4FieldChanged()
{
    if (m_loadingFromModel) return;
    writeBackCurrent();
}

void UserDefineDialog::onDelimiterCellChanged(int /*row*/, int /*col*/)
{
    if (m_loadingFromModel) return;
    writeBackCurrent();
}

void UserDefineDialog::onStyleListSelectionChanged(int /*row*/)
{
    // Per-row state is committed inline on every checkbox toggle, so a
    // row switch only needs to refresh the checkbox panel — no
    // pending-write step.
    if (m_loadingFromModel) return;
    loadStyleRowIntoWidgets();
}

void UserDefineDialog::onNestingChanged()
{
    if (m_loadingFromModel) return;
    if (m_currentIdx < 0 || m_currentIdx >= m_udls.size()) return;
    if (!m_stylesList || !m_stylesList->currentItem()) return;

    const QString name = m_stylesList->currentItem()->text();
    UDL& udl   = m_udls[m_currentIdx];
    UDLStyle* s = findOrCreateStyle(udl, name);

    int nesting = s->nesting;
    for (int i = 0; i < 21; ++i) {
        const int mask = kNestingFlags[i].mask;
        if (m_nestingChecks[i] && m_nestingChecks[i]->isChecked()) nesting |= mask;
        else                                                       nesting &= ~mask;
    }
    s->nesting = nesting;
}

void UserDefineDialog::updateControlsEnabled()
{
    const bool haveOne = (m_currentIdx >= 0 && m_currentIdx < m_udls.size());
    m_btnRename ->setEnabled(haveOne);
    m_btnDelete ->setEnabled(haveOne);
    m_tabs      ->setEnabled(haveOne);
}

// -----------------------------------------------------------------------------
// New / Rename / Delete / Save / Close
// -----------------------------------------------------------------------------

void UserDefineDialog::onNew()
{
    bool ok = false;
    const QString name = QInputDialog::getText(this,
        tr("New Language"),
        tr("Name for the new language:"),
        QLineEdit::Normal, QString(), &ok).trimmed();
    if (!ok || name.isEmpty()) return;

    for (const UDL& u : m_udls) {
        if (u.name == name) {
            QMessageBox::warning(this, tr("Duplicate"),
                tr("A language called \"%1\" already exists.").arg(name));
            return;
        }
    }

    if (m_currentIdx >= 0) writeBackCurrent();   // persist pending edits

    UDL fresh;
    fresh.name = name;
    m_udls.push_back(std::move(fresh));
    rebuildCombo(m_udls.size() - 1);
}

void UserDefineDialog::onRename()
{
    if (m_currentIdx < 0 || m_currentIdx >= m_udls.size()) return;
    bool ok = false;
    const QString name = QInputDialog::getText(this,
        tr("Rename Language"),
        tr("New name:"),
        QLineEdit::Normal, m_udls[m_currentIdx].name, &ok).trimmed();
    if (!ok || name.isEmpty()) return;
    if (name == m_udls[m_currentIdx].name) return;

    for (int i = 0; i < m_udls.size(); ++i) {
        if (i == m_currentIdx) continue;
        if (m_udls[i].name == name) {
            QMessageBox::warning(this, tr("Duplicate"),
                tr("A language called \"%1\" already exists.").arg(name));
            return;
        }
    }

    m_udls[m_currentIdx].name = name;
    const int keep = m_currentIdx;
    rebuildCombo(keep);
}

void UserDefineDialog::onDelete()
{
    if (m_currentIdx < 0 || m_currentIdx >= m_udls.size()) return;
    const auto reply = QMessageBox::question(this, tr("Delete Language"),
        tr("Delete the user-defined language \"%1\"? "
           "(This is not persisted until you click Save.)")
            .arg(m_udls[m_currentIdx].name));
    if (reply != QMessageBox::Yes) return;
    m_udls.removeAt(m_currentIdx);
    const int next = m_currentIdx >= m_udls.size() ? m_udls.size() - 1
                                                    : m_currentIdx;
    m_currentIdx = -1;        // force a clean reload via rebuildCombo
    rebuildCombo(next);
}

void UserDefineDialog::onSave()
{
    if (m_currentIdx >= 0) writeBackCurrent();

    // Soft validation — warn the user about likely-mistake configurations
    // (e.g. delimiter open chars that collide with identifier scanning) but
    // never block. Edge cases like single-char block-comment markers in
    // assembly languages are real, so a hard reject would lose use cases.
    QStringList warnings;
    for (const UDL& udl : m_udls) {
        if (udl.name.isEmpty()) continue;

        // Decode the comments slot to check line vs block opener overlap.
        const QStringList c = UserDefineLang::decodeComments(
            udl.keywords[SCE_USER_KWLIST_COMMENTS]);
        const QString lineOpen  = c.value(0);
        const QString blockOpen = c.value(3);
        if (!lineOpen.isEmpty() && lineOpen == blockOpen) {
            warnings << tr("• \"%1\": line-comment opener and block-comment "
                           "opener are identical (\"%2\"). The lexer will treat "
                           "the marker as a line comment.")
                          .arg(udl.name, lineOpen);
        }

        // Delimiter open chars that start with letter / digit collide with
        // identifier scanning — usually a typo (e.g. typing "n" instead of \\n).
        const QVector<DelimiterPair> pairs = UserDefineLang::decodeDelimiters(
            udl.keywords[SCE_USER_KWLIST_DELIMITERS]);
        for (int i = 0; i < pairs.size(); ++i) {
            const QString& open = pairs[i].open;
            if (open.isEmpty()) continue;
            const QChar first = open.at(0);
            if (first.isLetterOrNumber()) {
                warnings << tr("• \"%1\": delimiter pair %2's open boundary "
                               "starts with \"%3\" — letters and digits "
                               "collide with identifier scanning.")
                              .arg(udl.name).arg(i + 1).arg(first);
            }
        }
    }

    if (!warnings.isEmpty()) {
        QMessageBox box(QMessageBox::Warning,
            tr("UDL save — review warnings?"),
            tr("The following may need attention:\n\n%1\n\nSave anyway?")
                .arg(warnings.join(QStringLiteral("\n"))),
            QMessageBox::Save | QMessageBox::Cancel, this);
        box.setDefaultButton(QMessageBox::Save);
        if (box.exec() != QMessageBox::Save) return;
    }

    if (!UserDefineLang::saveAll(m_udls)) {
        QMessageBox::warning(this, tr("Save failed"),
            tr("Unable to write %1.").arg(UserDefineLang::configFilePath()));
        return;
    }
    emit udlsSaved();
}

void UserDefineDialog::onClose()
{
    accept();
}
