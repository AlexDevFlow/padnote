#include "ColumnEditorDialog.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include "Buffer.h"
#include "EditorTabs.h"
#include "Scintilla.h"
#include "ScintillaEditBase.h"
#include "ScintillaMessages.h"

using Scintilla::Message;

namespace {

void insertAtPosWithUndoBatch(ScintillaEditBase* ed,
                              const QList<QPair<Scintilla::sptr_t, QByteArray>>& edits)
{
    // Sort by descending position so each insertion doesn't shift the
    // positions of later ones.
    QList<QPair<Scintilla::sptr_t, QByteArray>> sorted = edits;
    std::sort(sorted.begin(), sorted.end(),
        [](const auto& a, const auto& b) { return a.first > b.first; });

    ed->send(static_cast<unsigned int>(Message::BeginUndoAction));
    for (const auto& e : sorted) {
        ed->sends(static_cast<unsigned int>(Message::InsertText),
                  static_cast<Scintilla::uptr_t>(e.first),
                  e.second.constData());
    }
    ed->send(static_cast<unsigned int>(Message::EndUndoAction));
}

} // namespace

ColumnEditorDialog::ColumnEditorDialog(EditorTabs* tabs, QWidget* parent)
    : QDialog(parent), m_tabs(tabs)
{
    setWindowTitle(tr("Column Editor"));
    setModal(true);

    auto* root = new QVBoxLayout(this);

    auto* group = new QButtonGroup(this);
    m_modeText   = new QRadioButton(tr("Text to insert"), this);
    m_modeNumber = new QRadioButton(tr("Number sequence"), this);
    m_modeText->setChecked(true);
    group->addButton(m_modeText);
    group->addButton(m_modeNumber);

    auto* form = new QFormLayout;
    form->addRow(m_modeText);
    m_textEdit = new QLineEdit(this);
    m_textEdit->setPlaceholderText(tr("Text to insert at every line..."));
    form->addRow(QStringLiteral("    ") + tr("Text:"), m_textEdit);

    form->addRow(m_modeNumber);
    m_startSpin = new QSpinBox(this);
    m_startSpin->setRange(-999999, 999999);
    m_startSpin->setValue(1);
    m_incrSpin = new QSpinBox(this);
    m_incrSpin->setRange(-999, 999);
    m_incrSpin->setValue(1);
    m_leadingZeros = new QCheckBox(tr("Leading zeros (column-aligned)"), this);
    form->addRow(QStringLiteral("    ") + tr("Start:"),     m_startSpin);
    form->addRow(QStringLiteral("    ") + tr("Increment:"), m_incrSpin);
    form->addRow(QStringLiteral("    "),                    m_leadingZeros);
    root->addLayout(form);

    auto* row = new QHBoxLayout;
    auto* aApply = new QPushButton(tr("&Insert"), this);
    auto* aClose = new QPushButton(tr("Close"),   this);
    aApply->setDefault(true);
    row->addStretch(1);
    row->addWidget(aApply);
    row->addWidget(aClose);
    root->addLayout(row);

    connect(aApply, &QPushButton::clicked, this, &ColumnEditorDialog::onApply);
    connect(aClose, &QPushButton::clicked, this, &QDialog::accept);
}

void ColumnEditorDialog::onApply()
{
    if (!m_tabs) return;
    Buffer* b = m_tabs->currentBuffer();
    if (!b) return;
    auto* ed = b->editor();

    // Pull all sub-selections (rectangular mode emits one per line; stream
    // mode emits one). For each, we'll insert at its anchor (the leftmost
    // position).
    const Scintilla::sptr_t nSel =
        ed->send(static_cast<unsigned int>(Message::GetSelections));
    if (nSel <= 0) return;

    // Build per-line insertions.
    QList<QPair<Scintilla::sptr_t, QByteArray>> edits;
    edits.reserve(static_cast<int>(nSel));

    int idx = 0;
    const bool numericMode = m_modeNumber->isChecked();
    const int start = m_startSpin->value();
    const int incr  = m_incrSpin->value();
    const QString text = m_textEdit->text();

    // Compute zero-pad width = digits of largest value.
    int maxAbs = std::abs(start) + std::abs(incr) * (static_cast<int>(nSel) - 1);
    if (maxAbs < 1) maxAbs = 1;
    int width = 1;
    while (maxAbs >= 10) { ++width; maxAbs /= 10; }

    for (int i = 0; i < nSel; ++i) {
        const Scintilla::sptr_t selA = ed->send(
            static_cast<unsigned int>(Message::GetSelectionNStart),
            static_cast<Scintilla::uptr_t>(i));
        QString chunk;
        if (numericMode) {
            const int v = start + incr * idx;
            if (m_leadingZeros->isChecked() && v >= 0) {
                chunk = QStringLiteral("%1").arg(v, width, 10, QChar('0'));
            } else {
                chunk = QString::number(v);
            }
        } else {
            chunk = text;
        }
        edits.append({selA, chunk.toUtf8()});
        ++idx;
    }

    insertAtPosWithUndoBatch(ed, edits);
}
