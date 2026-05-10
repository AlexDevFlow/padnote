// HashDialog.h — MD5 / SHA-1 / SHA-256 / SHA-512 generator.
//
// One reusable QDialog. The Tools → Hash menu has four entries that all
// open this dialog with the algorithm preselected. Hashing delegates to
// Qt's QCryptographicHash (built into Qt6::Core).

#pragma once

#include <QCryptographicHash>
#include <QDialog>
#include <QString>

class EditorTabs;
class QComboBox;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;

class HashDialog : public QDialog {
    Q_OBJECT
public:
    explicit HashDialog(EditorTabs* tabs, QWidget* parent = nullptr);

    // Preselect an algorithm. Idempotent.
    void setAlgorithm(QCryptographicHash::Algorithm algo);

private slots:
    void onAlgorithmChanged(int index);
    void onInputTextChanged();
    void onHashFromFile();
    void onHashFromSelection();
    void onHashFromDocument();

private:
    void recompute();                    // hashes the current input bytes
    void setBytesAndRefresh(const QByteArray& bytes);

    EditorTabs* m_tabs = nullptr;        // not owned

    QComboBox*      m_algoCombo = nullptr;
    QPlainTextEdit* m_input     = nullptr;
    QLineEdit*      m_output    = nullptr;
    QPushButton*    m_btnFile   = nullptr;
    QPushButton*    m_btnSel    = nullptr;
    QPushButton*    m_btnDoc    = nullptr;

    QByteArray m_currentBytes;           // bytes most recently hashed (for algo-switch)
    bool m_suppressRecompute = false;
};
