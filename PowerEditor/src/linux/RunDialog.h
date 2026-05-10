// RunDialog.h — Phase 5e: F5 "Run external command" modal dialog.
//
// Small QLineEdit + Run / Save… / Cancel. Modal, freshly constructed per
// invocation (mirrors HashDialog's lifetime). The dialog itself doesn't
// launch anything — it emits runRequested(rawCommand) and accepts; the
// MainWindow does macro substitution + QProcess spawn so process lifetime
// outlives the dialog.

#pragma once

#include <QDialog>
#include <QString>

class QLineEdit;

class RunDialog : public QDialog {
    Q_OBJECT
public:
    explicit RunDialog(QWidget* parent = nullptr);

    void setInitialCommand(const QString& cmd);
    QString command() const;     // raw text the user typed at the moment of Run

signals:
    // Real "save this command for later" lands with the Shortcut Mapper
    // (Phase 5Q). Until then this just bubbles a status-bar string up.
    void saveStubMessage(const QString& msg);

private slots:
    void onRun();
    void onSave();

private:
    QLineEdit* m_cmd = nullptr;
};
