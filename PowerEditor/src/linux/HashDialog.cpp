#include "HashDialog.h"

#include <QComboBox>
#include <QCryptographicHash>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include <vector>

#include "Buffer.h"
#include "EditorTabs.h"
#include "ScintillaEditBase.h"
#include "ScintillaMessages.h"

using Scintilla::Message;

HashDialog::HashDialog(EditorTabs* tabs, QWidget* parent)
    : QDialog(parent),
      m_tabs(tabs)
{
    setWindowTitle(tr("Hash Generator"));
    resize(640, 400);

    auto* root = new QVBoxLayout(this);

    // Algorithm selector
    {
        auto* row = new QHBoxLayout;
        row->addWidget(new QLabel(tr("Algorithm:"), this));
        m_algoCombo = new QComboBox(this);
        m_algoCombo->addItem(QStringLiteral("MD5"),
            static_cast<int>(QCryptographicHash::Md5));
        m_algoCombo->addItem(QStringLiteral("SHA-1"),
            static_cast<int>(QCryptographicHash::Sha1));
        m_algoCombo->addItem(QStringLiteral("SHA-256"),
            static_cast<int>(QCryptographicHash::Sha256));
        m_algoCombo->addItem(QStringLiteral("SHA-512"),
            static_cast<int>(QCryptographicHash::Sha512));
        row->addWidget(m_algoCombo);
        row->addStretch(1);
        root->addLayout(row);
        connect(m_algoCombo,
                QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, &HashDialog::onAlgorithmChanged);
    }

    // Input
    root->addWidget(new QLabel(tr("Input:"), this));
    m_input = new QPlainTextEdit(this);
    m_input->setPlaceholderText(tr("Type or paste text here, or use one of the buttons below."));
    m_input->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    root->addWidget(m_input, 1);
    connect(m_input, &QPlainTextEdit::textChanged,
            this, &HashDialog::onInputTextChanged);

    // Source buttons
    {
        auto* row = new QHBoxLayout;
        m_btnFile = new QPushButton(tr("From &File..."), this);
        m_btnSel  = new QPushButton(tr("From &Selection"), this);
        m_btnDoc  = new QPushButton(tr("From &Whole Document"), this);
        row->addWidget(m_btnFile);
        row->addWidget(m_btnSel);
        row->addWidget(m_btnDoc);
        row->addStretch(1);
        root->addLayout(row);
        connect(m_btnFile, &QPushButton::clicked, this, &HashDialog::onHashFromFile);
        connect(m_btnSel,  &QPushButton::clicked, this, &HashDialog::onHashFromSelection);
        connect(m_btnDoc,  &QPushButton::clicked, this, &HashDialog::onHashFromDocument);
    }

    // Output
    root->addWidget(new QLabel(tr("Hash:"), this));
    m_output = new QLineEdit(this);
    m_output->setReadOnly(true);
    m_output->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_output->setPlaceholderText(tr("(empty input)"));
    root->addWidget(m_output);

    // Close button
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::accept);
    root->addWidget(buttons);

    // Initial empty hash so the user sees what shape the output takes.
    recompute();
}

void HashDialog::setAlgorithm(QCryptographicHash::Algorithm algo)
{
    const int idx = m_algoCombo->findData(static_cast<int>(algo));
    if (idx >= 0) m_algoCombo->setCurrentIndex(idx);
}

void HashDialog::onAlgorithmChanged(int /*index*/)
{
    recompute();
}

void HashDialog::onInputTextChanged()
{
    if (m_suppressRecompute) return;
    m_currentBytes = m_input->toPlainText().toUtf8();
    recompute();
}

void HashDialog::onHashFromFile()
{
    const QString path = QFileDialog::getOpenFileName(this, tr("Hash file"));
    if (path.isEmpty()) return;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        m_output->setText(tr("(cannot open: %1)").arg(f.errorString()));
        return;
    }
    // Stream the file into QCryptographicHash to handle large files without
    // loading them entirely into memory.
    const auto algo = static_cast<QCryptographicHash::Algorithm>(
        m_algoCombo->currentData().toInt());
    QCryptographicHash hasher(algo);
    if (!hasher.addData(&f)) {
        m_output->setText(tr("(read error)"));
        f.close();
        return;
    }
    f.close();
    const QByteArray hex = hasher.result().toHex();
    m_output->setText(QString::fromLatin1(hex));

    // Show the file name in the input pane (truncated body) so the user can
    // see the source. Don't load multi-GB files into the textarea.
    QFile f2(path);
    if (f2.open(QIODevice::ReadOnly)) {
        const QByteArray peek = f2.read(8192);
        f2.close();
        m_suppressRecompute = true;
        m_input->setPlainText(tr("[file: %1, %2 bytes]\n\n%3")
            .arg(path)
            .arg(QFileInfo(path).size())
            .arg(QString::fromUtf8(peek)));
        m_suppressRecompute = false;
        // m_currentBytes stays as what was actually hashed (the full file
        // content, but we never stored it). Set m_currentBytes to "" so
        // changing the algorithm re-hashes the textarea preview rather than
        // showing a stale hash.
        m_currentBytes.clear();
    }
}

void HashDialog::onHashFromSelection()
{
    if (!m_tabs) return;
    Buffer* b = m_tabs->currentBuffer();
    if (!b) return;
    auto* ed = b->editor();
    const Scintilla::sptr_t selStart =
        ed->send(static_cast<unsigned int>(Message::GetSelectionStart));
    const Scintilla::sptr_t selEnd =
        ed->send(static_cast<unsigned int>(Message::GetSelectionEnd));
    if (selEnd <= selStart) {
        m_output->setText(tr("(no selection)"));
        return;
    }
    const Scintilla::sptr_t bytes = selEnd - selStart;
    std::vector<char> buf(static_cast<std::size_t>(bytes) + 1, '\0');
    ed->send(static_cast<unsigned int>(Message::GetSelText), 0,
             reinterpret_cast<Scintilla::sptr_t>(buf.data()));
    setBytesAndRefresh(QByteArray(buf.data(), static_cast<int>(bytes)));
}

void HashDialog::onHashFromDocument()
{
    if (!m_tabs) return;
    Buffer* b = m_tabs->currentBuffer();
    if (!b) return;
    auto* ed = b->editor();
    const Scintilla::sptr_t length =
        ed->send(static_cast<unsigned int>(Message::GetTextLength));
    if (length <= 0) {
        m_output->setText(tr("(empty document)"));
        return;
    }
    std::vector<char> buf(static_cast<std::size_t>(length) + 1, '\0');
    ed->send(static_cast<unsigned int>(Message::GetText),
             static_cast<Scintilla::uptr_t>(buf.size()),
             reinterpret_cast<Scintilla::sptr_t>(buf.data()));
    setBytesAndRefresh(QByteArray(buf.data(), static_cast<int>(length)));
}

void HashDialog::setBytesAndRefresh(const QByteArray& bytes)
{
    m_currentBytes = bytes;
    m_suppressRecompute = true;
    m_input->setPlainText(QString::fromUtf8(bytes));
    m_suppressRecompute = false;
    recompute();
}

void HashDialog::recompute()
{
    const auto algo = static_cast<QCryptographicHash::Algorithm>(
        m_algoCombo->currentData().toInt());
    if (m_currentBytes.isEmpty()) {
        // Use whatever's in the textarea now (handles initial state).
        const QByteArray bytes = m_input->toPlainText().toUtf8();
        if (bytes.isEmpty()) {
            m_output->clear();
            return;
        }
        const QByteArray hex = QCryptographicHash::hash(bytes, algo).toHex();
        m_output->setText(QString::fromLatin1(hex));
        return;
    }
    const QByteArray hex = QCryptographicHash::hash(m_currentBytes, algo).toHex();
    m_output->setText(QString::fromLatin1(hex));
}
