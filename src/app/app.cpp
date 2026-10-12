#include "app/app.h"

#include "app/actions.h"
#include "app/compressdialog.h"
#include "app/mainwindow.h"
#include "app/singleinstance.h"
#include "platform/shellintegration.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QLocale>
#include <QMessageBox>
#include <QSettings>
#include <QTimer>
#include <QUrl>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {
bool koreanUi()
{
    const QString lang = QSettings().value(QStringLiteral("ui/language"), QStringLiteral("auto")).toString();
    return lang == QStringLiteral("ko") || (lang == QStringLiteral("auto") && QLocale().language() == QLocale::Korean);
}

// An Explorer verb's process has nothing left to do after its job: settings
// are flushed, then the orderly teardown (CUDA context, page-locked buffers,
// DLL detach), which takes a few hundred milliseconds, is skipped.
int finishVerb(int result)
{
    QSettings().sync();
#ifdef Q_OS_WIN
    TerminateProcess(GetCurrentProcess(), UINT(result));
#endif
    return result;
}

// Explorer verbs. Each may receive many paths (multi-selection is collected
// into one process by collectInvocations()).
int runVerb(const QString &verb, const QStringList &paths)
{
    if (paths.isEmpty()) return 1;
    bool ok = true;
    if (verb == QStringLiteral("compress")) {
        ok = !Actions::compressInteractive(nullptr, paths).isEmpty();
    } else if (verb == QStringLiteral("add-7z")) {
        ok = !Actions::compressQuick(nullptr, paths, vyra::Format::SevenZip).isEmpty();
    } else if (verb == QStringLiteral("add-zip")) {
        ok = !Actions::compressQuick(nullptr, paths, vyra::Format::Zip).isEmpty();
    } else if (verb == QStringLiteral("extract-here") || verb == QStringLiteral("extract-folder")) {
        for (const QString &archive : paths) {
            const QString dest = verb == QStringLiteral("extract-here") ? Actions::smartDestination(archive)
                                                                       : Actions::folderDestination(archive);
            ok &= Actions::extract(nullptr, archive, dest);
        }
    } else if (verb == QStringLiteral("extract-dialog")) {
        for (const QString &archive : paths) {
            const QString dest = QFileDialog::getExistingDirectory(nullptr, QCoreApplication::translate("App", "Extract %1 to").arg(QFileInfo(archive).fileName()),
                                                                   QFileInfo(archive).absolutePath());
            if (dest.isEmpty()) continue;
            if (Actions::extract(nullptr, archive, dest)) QDesktopServices::openUrl(QUrl::fromLocalFile(dest));
            else ok = false;
        }
    } else if (verb == QStringLiteral("test")) {
        for (const QString &archive : paths) ok &= Actions::test(nullptr, archive);
    }
    return ok ? 0 : 1;
}
}

int App::run(QApplication &application)
{
    QCommandLineParser parser;
    parser.setApplicationDescription(QCoreApplication::translate("App", "GPU accelerated 7z / zip archiver"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(QStringLiteral("paths"), QCoreApplication::translate("App", "Archive to open, or paths for a verb"), QStringLiteral("[paths...]"));
    const QStringList verbs = {QStringLiteral("compress"), QStringLiteral("add-7z"), QStringLiteral("add-zip"),
                               QStringLiteral("extract-here"), QStringLiteral("extract-folder"), QStringLiteral("extract-dialog"),
                               QStringLiteral("test")};
    parser.addOption({QStringLiteral("compress"), QCoreApplication::translate("App", "Show the compression dialog for the paths")});
    parser.addOption({QStringLiteral("add-7z"), QCoreApplication::translate("App", "Compress the paths to <name>.7z")});
    parser.addOption({QStringLiteral("add-zip"), QCoreApplication::translate("App", "Compress the paths to <name>.zip")});
    parser.addOption({QStringLiteral("extract-here"), QCoreApplication::translate("App", "Extract next to the archive")});
    parser.addOption({QStringLiteral("extract-folder"), QCoreApplication::translate("App", "Extract into a folder named after the archive")});
    parser.addOption({QStringLiteral("extract-dialog"), QCoreApplication::translate("App", "Choose where to extract")});
    parser.addOption({QStringLiteral("test"), QCoreApplication::translate("App", "Test archive integrity")});
    parser.addOption({QStringLiteral("install-shell"), QCoreApplication::translate("App", "Install Windows Explorer integration")});
    parser.addOption({QStringLiteral("uninstall-shell"), QCoreApplication::translate("App", "Remove Windows Explorer integration")});
    parser.process(application);

    QStringList paths;
    for (const QString &p : parser.positionalArguments()) paths << QFileInfo(p).absoluteFilePath();

    if (parser.isSet(QStringLiteral("install-shell")) || parser.isSet(QStringLiteral("uninstall-shell"))) {
        QString error;
        const bool ok = parser.isSet(QStringLiteral("install-shell"))
            ? ShellIntegration::install(QCoreApplication::applicationFilePath(), koreanUi(), &error)
            : ShellIntegration::uninstall(&error);
        if (!ok) QMessageBox::critical(nullptr, QStringLiteral("Vyra"), error);
        return ok ? 0 : 1;
    }
    for (const QString &verb : verbs) {
        if (!parser.isSet(verb)) continue;
        application.setQuitOnLastWindowClosed(false);
        const auto collected = collectInvocations(verb, paths);
        if (!collected) return 0; // handed over to the collecting process
        return finishVerb(runVerb(verb, *collected));
    }

    MainWindow window;
    if (paths.size() == 1 && QFileInfo(paths.first()).isFile()) window.openArchive(paths.first());
    window.show();
    // Documentation aid: VYRA_SCREENSHOT=<dir> renders the main window and the
    // compression dialog to PNG files and exits (works with -platform offscreen).
    const QString shots = qEnvironmentVariable("VYRA_SCREENSHOT");
    if (!shots.isEmpty()) {
        QTimer::singleShot(700, &window, [&window, shots, paths] {
            window.grab().save(QDir(shots).filePath(QStringLiteral("main-window.png")));
            CompressDialog dialog(paths.isEmpty() ? QStringList{QDir::homePath()} : paths, &window);
            dialog.show();
            dialog.grab().save(QDir(shots).filePath(QStringLiteral("compress-dialog.png")));
            QCoreApplication::quit();
        });
    }
    return application.exec();
}
