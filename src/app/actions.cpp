#include "app/actions.h"
#include "app/compressdialog.h"
#include "app/progressdialog.h"
#include "app/qtbridge.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QMessageBox>
#include <QPushButton>
#include <QSet>

namespace {
bool isSelected(const QString &path, const QStringList &selection)
{
    if (selection.isEmpty()) return true;
    for (const QString &s : selection)
        if (path == s || path.startsWith(s + u'/')) return true;
    return false;
}

void reportProblems(QWidget *parent, const QString &title, const JobOutcome &outcome)
{
    if (outcome.cancelled) return;
    if (!outcome.ok) {
        QMessageBox::critical(parent, title, outcome.error);
        return;
    }
    const auto &warnings = outcome.result.warnings;
    if (warnings.empty()) return;
    QStringList lines;
    for (std::size_t i = 0; i < warnings.size() && i < 25; ++i) lines << fromUtf8(warnings[i]);
    if (warnings.size() > 25) lines << QCoreApplication::translate("Actions", "… and %1 more").arg(warnings.size() - 25);
    QMessageBox box(QMessageBox::Warning, title,
                    QCoreApplication::translate("Actions", "Finished with %1 problem(s).").arg(warnings.size()),
                    QMessageBox::Ok, parent);
    box.setDetailedText(lines.join(u'\n'));
    box.setInformativeText(lines.mid(0, 5).join(u'\n'));
    box.exec();
}

// Asks once when the extraction would replace existing files.
// Returns false when the user cancels.
bool confirmOverwrite(QWidget *parent, const QString &archive, const QString &destination, const QStringList &selection,
                      bool &overwrite)
{
    overwrite = true;
    vyra::ArchiveListing listing;
    try {
        listing = vyra::listArchive(toPath(archive));
    } catch (const std::exception &) {
        return true; // the extraction itself will report the problem
    }
    int existing = 0;
    const vyra::fs::path dest = toPath(destination);
    std::error_code ec;
    for (const vyra::Entry &e : listing.entries) {
        if (e.directory || !isSelected(fromUtf8(e.path), selection)) continue;
        const vyra::fs::path rel = vyra::safeRelativePath(e.path);
        if (!rel.empty() && vyra::fs::exists(dest / rel, ec)) ++existing;
        if (existing >= 10000) break;
    }
    if (existing == 0) return true;
    QMessageBox box(QMessageBox::Question, QCoreApplication::translate("Actions", "Files already exist"),
                    QCoreApplication::translate("Actions", "%1 file(s) already exist in\n%2").arg(existing).arg(QDir::toNativeSeparators(destination)),
                    QMessageBox::NoButton, parent);
    QPushButton *replace = box.addButton(QCoreApplication::translate("Actions", "Replace all"), QMessageBox::AcceptRole);
    QPushButton *skip = box.addButton(QCoreApplication::translate("Actions", "Keep existing"), QMessageBox::ActionRole);
    box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(replace);
    box.exec();
    if (box.clickedButton() == replace) { overwrite = true; return true; }
    if (box.clickedButton() == skip) { overwrite = false; return true; }
    return false;
}

QString runCompress(QWidget *parent, const QStringList &inputs, const QString &archive, const vyra::CompressOptions &options)
{
    const QString title = QCoreApplication::translate("Actions", "Compressing");
    const JobOutcome outcome = ProgressDialog::run(parent, title, QDir::toNativeSeparators(archive),
        [inputs, archive, options](const vyra::ProgressCallback &progress) {
            return vyra::createArchive(toPath(archive), toPaths(inputs), options, progress);
        });
    reportProblems(parent, title, outcome);
    return outcome.ok ? archive : QString();
}
} // namespace

QString Actions::compressInteractive(QWidget *parent, const QStringList &inputs)
{
    if (inputs.isEmpty()) return {};
    CompressDialog dialog(inputs, parent);
    if (dialog.exec() != QDialog::Accepted) return {};
    const vyra::CompressOptions options = dialog.options();
    if (vyra::singleFileFormat(options.format)) {
        if (!compressEach(parent, inputs, options)) return {};
        // One file: its .gz / .lz4; otherwise the folder holding the outputs.
        const QFileInfo first(inputs.first());
        const QString single = first.absoluteFilePath() + u'.' + QString::fromLatin1(vyra::formatName(options.format));
        if (inputs.size() == 1 && first.isFile() && QFileInfo(single).isFile()) return single;
        return inputs.size() == 1 && first.isDir() ? first.absoluteFilePath() : first.absolutePath();
    }
    return runCompress(parent, inputs, dialog.archivePath(), options);
}

bool Actions::compressEach(QWidget *parent, const QStringList &inputs, const vyra::CompressOptions &options)
{
    if (inputs.isEmpty()) return false;
    const QString title = QCoreApplication::translate("Actions", "Compressing");
    const QString what = QCoreApplication::translate("Actions", "Each file → .%1 next to it")
                             .arg(QString::fromLatin1(vyra::formatName(options.format)));
    const JobOutcome outcome = ProgressDialog::run(parent, title, what,
        [inputs, options](const vyra::ProgressCallback &progress) {
            return vyra::compressEachFile(toPaths(inputs), options, progress);
        });
    reportProblems(parent, title, outcome);
    return outcome.ok;
}

QString Actions::compressQuick(QWidget *parent, const QStringList &inputs, vyra::Format format)
{
    if (inputs.isEmpty()) return {};
    const QString archive = CompressDialog::defaultArchivePath(inputs, QString::fromLatin1(vyra::formatName(format)));
    return runCompress(parent, inputs, archive, CompressDialog::savedOptions(format));
}

bool Actions::extract(QWidget *parent, const QString &archive, const QString &destination, const QStringList &selection)
{
    bool overwrite = true;
    if (!confirmOverwrite(parent, archive, destination, selection, overwrite)) return false;
    vyra::ExtractOptions options;
    options.overwrite = overwrite;
    for (const QString &s : selection) options.selection.push_back(toUtf8(s));
    const QString title = QCoreApplication::translate("Actions", "Extracting");
    const JobOutcome outcome = ProgressDialog::run(parent, title,
        QFileInfo(archive).fileName() + QStringLiteral("  →  ") + QDir::toNativeSeparators(destination),
        [archive, destination, options](const vyra::ProgressCallback &progress) {
            return vyra::extractArchive(toPath(archive), toPath(destination), options, progress);
        });
    reportProblems(parent, title, outcome);
    return outcome.ok && outcome.result.warnings.empty();
}

bool Actions::test(QWidget *parent, const QString &archive)
{
    vyra::ExtractOptions options;
    options.testOnly = true;
    const QString title = QCoreApplication::translate("Actions", "Testing");
    const JobOutcome outcome = ProgressDialog::run(parent, title, QFileInfo(archive).fileName(),
        [archive, options](const vyra::ProgressCallback &progress) {
            return vyra::extractArchive(toPath(archive), {}, options, progress);
        });
    if (outcome.ok && outcome.result.warnings.empty()) {
        QMessageBox::information(parent, title,
            QCoreApplication::translate("Actions", "No errors found.\n\n%1\n%2 files · %3 · %4 s\n%5")
                .arg(QFileInfo(archive).fileName())
                .arg(outcome.result.files)
                .arg(formatSize(outcome.result.inputBytes))
                .arg(outcome.result.seconds, 0, 'f', 2)
                .arg(fromUtf8(outcome.result.backend)));
        return true;
    }
    reportProblems(parent, title, outcome);
    return false;
}

QString Actions::folderDestination(const QString &archive)
{
    const QFileInfo info(archive);
    const QString base = info.absoluteDir().filePath(info.completeBaseName());
    // A file of that name (the original next to its .gz) cannot become the folder.
    QString folder = base;
    for (int i = 2; QFileInfo::exists(folder) && !QFileInfo(folder).isDir(); ++i) folder = QStringLiteral("%1 (%2)").arg(base).arg(i);
    return folder;
}

QString Actions::smartDestination(const QString &archive)
{
    try {
        const vyra::ArchiveListing listing = vyra::listArchive(toPath(archive));
        QSet<QString> top;
        for (const vyra::Entry &e : listing.entries) {
            const QString path = fromUtf8(e.path);
            top.insert(path.section(u'/', 0, 0));
            if (top.size() > 1) break;
        }
        if (top.size() == 1) return QFileInfo(archive).absolutePath();
    } catch (const std::exception &) {
    }
    return folderDestination(archive);
}

bool Actions::isArchivePath(const QString &path)
{
    static const QStringList suffixes = {QStringLiteral("7z"), QStringLiteral("zip"), QStringLiteral("gz"),
                                         QStringLiteral("tgz"), QStringLiteral("lz4")};
    return suffixes.contains(QFileInfo(path).suffix().toLower());
}
