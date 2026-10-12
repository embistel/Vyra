#pragma once

#include "core/archive.h"

#include <QString>
#include <QStringList>

class QWidget;

// User-level operations shared by the main window and the Explorer verbs.
// Each runs the core job behind a progress window and reports problems itself.
namespace Actions {
// Shows the "Add to archive" dialog first. Returns the created archive or "".
// gz / lz4 (one output per file): returns the output of a single file, or
// the folder holding the outputs.
QString compressInteractive(QWidget *parent, const QStringList &inputs);
// No dialog: <name>.7z / <name>.zip next to the inputs with the saved options.
QString compressQuick(QWidget *parent, const QStringList &inputs, vyra::Format format);
// gz / lz4: every file of the inputs (folders: every file inside) becomes
// <file>.gz / <file>.lz4 next to it. Returns false on failure or cancel.
bool compressEach(QWidget *parent, const QStringList &inputs, const vyra::CompressOptions &options);
bool extract(QWidget *parent, const QString &archive, const QString &destination, const QStringList &selection = {});
bool test(QWidget *parent, const QString &archive);
// "Extract Here": straight into the archive's folder when it has a single
// top-level item, otherwise into a new folder named after the archive.
QString smartDestination(const QString &archive);
QString folderDestination(const QString &archive);
bool isArchivePath(const QString &path);
}
