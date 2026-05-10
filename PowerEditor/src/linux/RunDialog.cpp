#include "RunDialog.h"

#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

RunDialog::RunDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Run command"));
    setModal(true);
    resize(640, 0);

    auto* root = new QVBoxLayout(this);

    auto* help = new QLabel(
        tr("Macros: <code>$(FULL_CURRENT_PATH)</code>, "
           "<code>$(CURRENT_DIRECTORY)</code>, "
           "<code>$(FILE_NAME)</code>, "
           "<code>$(NAME_PART)</code>, "
           "<code>$(EXT_PART)</code>."),
        this);
    help->setTextFormat(Qt::RichText);
    help->setWordWrap(true);
    root->addWidget(help);

    m_cmd = new QLineEdit(this);
    m_cmd->setPlaceholderText(
        tr("Command to run (e.g. \"python $(FULL_CURRENT_PATH)\")"));
    m_cmd->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    root->addWidget(m_cmd);

    auto* buttons = new QHBoxLayout;
    auto* aRun    = new QPushButton(tr("&Run"),    this);
    auto* aSave   = new QPushButton(tr("&Save..."), this);
    auto* aCancel = new QPushButton(tr("Cancel"),  this);
    aRun->setDefault(true);
    buttons->addStretch(1);
    buttons->addWidget(aRun);
    buttons->addWidget(aSave);
    buttons->addWidget(aCancel);
    root->addLayout(buttons);

    connect(aRun,    &QPushButton::clicked, this, &RunDialog::onRun);
    connect(aSave,   &QPushButton::clicked, this, &RunDialog::onSave);
    connect(aCancel, &QPushButton::clicked, this, &QDialog::reject);

    m_cmd->setFocus();
}

void RunDialog::setInitialCommand(const QString& cmd)
{
    m_cmd->setText(cmd);
    m_cmd->selectAll();
}

QString RunDialog::command() const
{
    return m_cmd->text();
}

void RunDialog::onRun()
{
    if (m_cmd->text().trimmed().isEmpty()) return;
    accept();
}

void RunDialog::onSave()
{
    emit saveStubMessage(
        tr("Saving Run commands lands with the Shortcut Mapper (Phase 5Q)."));
}
