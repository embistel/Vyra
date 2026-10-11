#include "app/app.h"

#include <QApplication>
#include <QCoreApplication>
#include <QFont>
#include <QLibraryInfo>
#include <QLocale>
#include <QSettings>
#include <QStyleFactory>
#include <QTranslator>

namespace {
// English is the source language. Korean is used when chosen in Tools >
// Language, or by default on a Korean Windows ("auto").
void installAppTranslator()
{
    const QString language = QSettings().value(QStringLiteral("ui/language"), QStringLiteral("auto")).toString();
    const bool korean = language == QStringLiteral("ko") ||
                        (language == QStringLiteral("auto") && QLocale().language() == QLocale::Korean);
    if (!korean) return;
    auto *translator = new QTranslator(QCoreApplication::instance());
    if (translator->load(QStringLiteral("app_ko"), QCoreApplication::applicationDirPath()))
        QCoreApplication::installTranslator(translator);
    else
        delete translator;
    // Qt's own strings (standard dialog buttons): shipped next to the app by
    // the package script, or taken from the Qt installation during development.
    auto *qtTranslator = new QTranslator(QCoreApplication::instance());
    if (qtTranslator->load(QStringLiteral("qtbase_ko"), QCoreApplication::applicationDirPath() + QStringLiteral("/translations")) ||
        qtTranslator->load(QStringLiteral("qtbase_ko"), QLibraryInfo::path(QLibraryInfo::TranslationsPath)))
        QCoreApplication::installTranslator(qtTranslator);
    else
        delete qtTranslator;
}
}

int main(int argc, char *argv[])
{
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("embistel"));
    QCoreApplication::setApplicationName(QStringLiteral("Vyra"));
    QCoreApplication::setApplicationVersion(QStringLiteral(VYRA_VERSION));
    installAppTranslator();
    application.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    application.setFont(QFont(QStringLiteral("Segoe UI"), 10));
    // Same visual language as LMDB Archiver: light surfaces, blue accents.
    application.setStyleSheet(QStringLiteral(
        "QMainWindow, QDialog { background: #f6f7fb; }"
        "QMenuBar { background: #ffffff; border-bottom: 1px solid #dfe3eb; padding: 3px; }"
        "QToolBar { background: #ffffff; border: 0; border-bottom: 1px solid #dfe3eb; padding: 7px; spacing: 5px; }"
        "QToolButton { border: 1px solid transparent; border-radius: 6px; padding: 6px 9px; }"
        "QToolButton:hover { background: #eaf2ff; border-color: #c7dafb; }"
        "QLineEdit, QComboBox { background: #ffffff; border: 1px solid #ccd3df; border-radius: 7px; padding: 6px 10px; }"
        "QLineEdit:focus, QComboBox:focus { border: 2px solid #3478d4; padding: 5px 9px; }"
        "QTreeView { background: #ffffff; border: 1px solid #d9dee8; border-radius: 8px; alternate-background-color: #f8faff; padding: 3px; }"
        "QTreeView::item { min-height: 28px; }"
        "QTreeView::item:selected { background: #cfe3ff; color: #10243e; border-radius: 4px; }"
        "QFrame#ArchiveBanner { background: #ffffff; border: 1px solid #d9dee8; border-radius: 9px; }"
        "QLabel#ArchiveName, QLabel#DialogTitle { color: #14213d; font-size: 15px; font-weight: 700; }"
        "QLabel#ArchiveLocation, QLabel#DialogSubtitle { color: #667085; font-size: 9pt; }"
        "QLabel#EngineChip { background: #e8f7ef; color: #11734b; border: 1px solid #b8e6cd; border-radius: 11px; padding: 4px 10px; font-weight: 600; }"
        "QLabel#EngineChip[cpu=\"true\"] { background: #fff4e5; color: #8a5200; border-color: #f5d39b; }"
        "QLabel#CompatNote { background: #eef4ff; color: #1d3b6e; border: 1px solid #cfdcf5; border-radius: 7px; padding: 8px 10px; }"
        "QProgressBar { background: #e9edf5; border: 0; border-radius: 6px; height: 14px; text-align: center; color: #14213d; }"
        "QProgressBar::chunk { border-radius: 6px; background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #3b82f6, stop:1 #6d28d9); }"
        "QPushButton { background: #ffffff; border: 1px solid #ccd3df; border-radius: 7px; padding: 6px 16px; }"
        "QPushButton:hover { background: #eaf2ff; border-color: #c7dafb; }"
        "QPushButton:default { background: #2563eb; color: #ffffff; border-color: #2563eb; }"
        "QHeaderView::section { background: #eef1f6; border: 0; border-right: 1px solid #d7dce5; border-bottom: 1px solid #d7dce5; padding: 7px; font-weight: 600; }"
        "QStatusBar { background: #ffffff; border-top: 1px solid #dfe3eb; }"));
    return App::run(application);
}
