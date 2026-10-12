#pragma once

#include "core/archive.h"

#include <QDialog>
#include <QStringList>

class QComboBox;
class QLabel;
class QLineEdit;

// "Add to archive" dialog (7-Zip style): target path, format, method, level,
// engine and solid block, with a live note on which programs can open it.
class CompressDialog final : public QDialog {
    Q_OBJECT
public:
    explicit CompressDialog(const QStringList &inputs, QWidget *parent = nullptr);

    QString archivePath() const;
    vyra::CompressOptions options() const;

    // Default archive name for a set of inputs: "<name>.<ext>" next to them.
    static QString defaultArchivePath(const QStringList &inputs, const QString &extension);
    // Options remembered from the last time the dialog was accepted.
    static vyra::CompressOptions savedOptions(vyra::Format format);

    void accept() override;

private:
    void formatChanged();
    void methodChanged();
    void browse();

    QStringList m_inputs;
    QString m_archivePath;            // kept while gz / lz4 (no archive path) is selected
    QLineEdit *m_path = nullptr;
    QWidget *m_browse = nullptr;
    QComboBox *m_format = nullptr;
    QComboBox *m_method = nullptr;
    QComboBox *m_level = nullptr;
    QComboBox *m_backend = nullptr;
    QComboBox *m_solid = nullptr;
    QComboBox *m_chunk = nullptr;
    QLabel *m_compat = nullptr;
    QLabel *m_engine = nullptr;
};
