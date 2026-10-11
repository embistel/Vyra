#include "app/mainwindow.h"

#include "app/actions.h"
#include "app/archivemodel.h"
#include "app/dropview.h"
#include "app/qtbridge.h"
#include "core/backend.h"
#include "platform/shellintegration.h"

#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDrag>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyle>
#include <QTimer>
#include <QToolBar>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>

namespace {
QAction *makeAction(QObject *parent, const QIcon &icon, const QString &text, const QKeySequence &shortcut = {})
{
    auto *action = new QAction(icon, text, parent);
    if (!shortcut.isEmpty()) action->setShortcut(shortcut);
    return action;
}

QString exportsRoot()
{
    return QStandardPaths::writableLocation(QStandardPaths::TempLocation) + QStringLiteral("/Vyra/exports");
}
}

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
    cleanStaleExports();
    setWindowTitle(QStringLiteral("Vyra"));
    setWindowIcon(QIcon(QStringLiteral(":/icons/app.svg")));
    resize(1120, 720);
    setMinimumSize(760, 480);

    auto *container = new QWidget(this);
    auto *layout = new QVBoxLayout(container);
    layout->setContentsMargins(12, 10, 12, 8);
    layout->setSpacing(8);

    auto *banner = new QFrame(container);
    banner->setObjectName(QStringLiteral("ArchiveBanner"));
    auto *bannerLayout = new QHBoxLayout(banner);
    bannerLayout->setContentsMargins(14, 10, 14, 10);
    auto *bannerIcon = new QLabel(banner);
    bannerIcon->setPixmap(QIcon(QStringLiteral(":/icons/app.svg")).pixmap(40, 40));
    bannerLayout->addWidget(bannerIcon);
    auto *bannerText = new QVBoxLayout;
    bannerText->setSpacing(1);
    m_archiveName = new QLabel(tr("Vyra — GPU accelerated archiver"), banner);
    m_archiveName->setObjectName(QStringLiteral("ArchiveName"));
    m_archiveLocation = new QLabel(tr("Compress files to 7z / zip at GPU speed, or open an archive."), banner);
    m_archiveLocation->setObjectName(QStringLiteral("ArchiveLocation"));
    bannerText->addWidget(m_archiveName);
    bannerText->addWidget(m_archiveLocation);
    bannerLayout->addLayout(bannerText, 1);
    m_engineChip = new QLabel(banner);
    m_engineChip->setObjectName(QStringLiteral("EngineChip"));
    bannerLayout->addWidget(m_engineChip, 0, Qt::AlignVCenter);
    layout->addWidget(banner);

    m_search = new QLineEdit(container);
    m_search->setPlaceholderText(tr("Search archive..."));
    m_search->setClearButtonEnabled(true);
    m_search->setEnabled(false);
    layout->addWidget(m_search);

    m_view = new DropView(container);
    m_model = new ArchiveModel(m_view);
    m_view->setModel(m_model);
    m_view->setAlternatingRowColors(true);
    m_view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_view->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_view->setContextMenuPolicy(Qt::CustomContextMenu);
    m_view->setSortingEnabled(true);
    m_view->sortByColumn(0, Qt::AscendingOrder);
    m_view->header()->setStretchLastSection(false);
    m_view->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int column = 1; column < 6; ++column) m_view->header()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    layout->addWidget(m_view, 1);
    setCentralWidget(container);

    auto *fileMenu = menuBar()->addMenu(tr("&File"));
    auto *compressAction = makeAction(this, QIcon(QStringLiteral(":/icons/compress.svg")), tr("&Compress Files..."), QKeySequence::New);
    auto *compressFolderAction = makeAction(this, QIcon(QStringLiteral(":/icons/add-folder.svg")), tr("Compress &Folder..."));
    auto *openAction = makeAction(this, QIcon(QStringLiteral(":/icons/open.svg")), tr("&Open Archive..."), QKeySequence::Open);
    compressAction->setIconText(tr("Compress"));
    openAction->setIconText(tr("Open"));
    auto *exitAction = makeAction(this, {}, tr("E&xit"), QKeySequence::Quit);
    fileMenu->addActions({compressAction, compressFolderAction, openAction});
    fileMenu->addSeparator();
    fileMenu->addAction(exitAction);

    auto *archiveMenu = menuBar()->addMenu(tr("&Archive"));
    m_extractAction = makeAction(this, QIcon(QStringLiteral(":/icons/extract.svg")), tr("&Extract Selected..."), QKeySequence(Qt::CTRL | Qt::Key_E));
    m_extractAction->setIconText(tr("Extract"));
    m_extractAllAction = makeAction(this, QIcon(QStringLiteral(":/icons/extract.svg")), tr("Extract &All..."), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_E));
    m_extractAllAction->setIconText(tr("Extract All"));
    m_testAction = makeAction(this, QIcon(QStringLiteral(":/icons/verify.svg")), tr("&Test Archive"), QKeySequence(Qt::CTRL | Qt::Key_T));
    m_testAction->setIconText(tr("Test"));
    m_copyAction = makeAction(this, QIcon(QStringLiteral(":/icons/copy.svg")), tr("&Copy Selected to Explorer"), QKeySequence::Copy);
    m_openEntryAction = makeAction(this, QIcon(QStringLiteral(":/icons/open.svg")), tr("Open Selected"), QKeySequence(Qt::Key_Return));
    m_propertiesAction = makeAction(this, QIcon(QStringLiteral(":/icons/info.svg")), tr("&Properties"), QKeySequence(Qt::ALT | Qt::Key_Return));
    m_propertiesAction->setIconText(tr("Info"));
    m_refreshAction = makeAction(this, style()->standardIcon(QStyle::SP_BrowserReload), tr("Refresh"), QKeySequence::Refresh);
    archiveMenu->addActions({m_extractAction, m_extractAllAction, m_testAction});
    archiveMenu->addSeparator();
    archiveMenu->addActions({m_copyAction, m_propertiesAction, m_refreshAction});

    auto *toolsMenu = menuBar()->addMenu(tr("&Tools"));
    auto *shellAction = makeAction(this, {}, tr("Windows Explorer Integration..."));
    toolsMenu->addAction(shellAction);
    toolsMenu->addSeparator();
    // Language selection (English default, Korean optional). Applied on restart.
    auto *languageMenu = toolsMenu->addMenu(tr("Language"));
    auto *langGroup = new QActionGroup(this);
    langGroup->setExclusive(true);
    m_englishAction = langGroup->addAction(QStringLiteral("English"));
    m_koreanAction = langGroup->addAction(QStringLiteral("\xed\x95\x9c\xea\xb5\xad\xec\x96\xb4")); // 한국어
    for (auto *act : {m_englishAction, m_koreanAction}) {
        act->setCheckable(true);
        languageMenu->addAction(act);
    }
    const QString currentLang = QSettings().value(QStringLiteral("ui/language"), QStringLiteral("auto")).toString();
    const bool korean = currentLang == QStringLiteral("ko") ||
                        (currentLang == QStringLiteral("auto") && QLocale().language() == QLocale::Korean);
    (korean ? m_koreanAction : m_englishAction)->setChecked(true);
    connect(m_englishAction, &QAction::triggered, this, [this] { switchLanguage(QStringLiteral("en")); });
    connect(m_koreanAction, &QAction::triggered, this, [this] { switchLanguage(QStringLiteral("ko")); });
    auto *helpMenu = menuBar()->addMenu(tr("&Help"));
    auto *aboutAction = makeAction(this, {}, tr("About Vyra"));
    helpMenu->addAction(aboutAction);

    auto *toolbar = addToolBar(tr("Main Toolbar"));
    toolbar->setObjectName(QStringLiteral("MainToolbar"));
    toolbar->setMovable(false);
    toolbar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    toolbar->addActions({compressAction, openAction});
    toolbar->addSeparator();
    toolbar->addActions({m_extractAction, m_extractAllAction, m_testAction, m_propertiesAction});

    m_summary = new QLabel(tr("Drag files here to compress, or open a .7z / .zip archive."), this);
    statusBar()->addWidget(m_summary, 1);

    connect(compressAction, &QAction::triggered, this, &MainWindow::compressFiles);
    connect(compressFolderAction, &QAction::triggered, this, &MainWindow::compressFolder);
    connect(openAction, &QAction::triggered, this, &MainWindow::chooseArchive);
    connect(exitAction, &QAction::triggered, this, &QWidget::close);
    connect(m_extractAction, &QAction::triggered, this, &MainWindow::extractSelected);
    connect(m_extractAllAction, &QAction::triggered, this, &MainWindow::extractAll);
    connect(m_testAction, &QAction::triggered, this, &MainWindow::testArchive);
    connect(m_copyAction, &QAction::triggered, this, &MainWindow::copySelected);
    connect(m_openEntryAction, &QAction::triggered, this, [this] { openSelected(m_view->currentIndex()); });
    connect(m_propertiesAction, &QAction::triggered, this, &MainWindow::showProperties);
    connect(m_refreshAction, &QAction::triggered, this, &MainWindow::refresh);
    connect(shellAction, &QAction::triggered, this, &MainWindow::configureShell);
    connect(aboutAction, &QAction::triggered, this, &MainWindow::showAbout);
    connect(m_view, &DropView::localFilesDropped, this, &MainWindow::handleDrop);
    connect(m_view, &DropView::archiveDragRequested, this, &MainWindow::dragSelectedOut);
    connect(m_view, &QTreeView::doubleClicked, this, &MainWindow::openSelected);
    connect(m_view, &QWidget::customContextMenuRequested, this, &MainWindow::showEntryMenu);
    connect(m_view->selectionModel(), &QItemSelectionModel::selectionChanged, this, &MainWindow::updateActions);
    connect(m_search, &QLineEdit::textChanged, this, [this](const QString &text) {
        m_model->setEntries(m_listing.entries, text);
        if (!text.isEmpty()) m_view->expandAll();
        else m_view->expandToDepth(0);
    });
    updateActions();
    restoreGeometry(QSettings().value(QStringLiteral("window/geometry")).toByteArray());
    // Querying CUDA takes a moment; do it after the window is shown.
    m_engineChip->setText(tr("Detecting GPU…"));
    QTimer::singleShot(0, this, &MainWindow::updateEngineChip);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    QSettings().setValue(QStringLiteral("window/geometry"), saveGeometry());
    QMainWindow::closeEvent(event);
}

void MainWindow::updateEngineChip()
{
    const vyra::GpuStatus gpu = vyra::queryGpu();
    if (gpu.available) {
        m_engineChip->setText(QStringLiteral("⚡ ") + fromUtf8(gpu.description));
        m_engineChip->setToolTip(tr("Compression runs on the GPU with NVIDIA nvCOMP."));
    } else {
        m_engineChip->setText(tr("CPU mode"));
        m_engineChip->setToolTip(tr("GPU unavailable: %1").arg(fromUtf8(gpu.description)));
        m_engineChip->setProperty("cpu", true);
        m_engineChip->style()->unpolish(m_engineChip);
        m_engineChip->style()->polish(m_engineChip);
    }
}

bool MainWindow::openArchive(const QString &path)
{
    try {
        m_listing = vyra::listArchive(toPath(path));
    } catch (const std::exception &e) {
        showError(tr("Cannot open %1:\n%2").arg(QDir::toNativeSeparators(path), QString::fromUtf8(e.what())));
        return false;
    }
    m_archivePath = QFileInfo(path).absoluteFilePath();
    setWindowTitle(QFileInfo(path).fileName() + QStringLiteral(" — Vyra"));
    m_archiveName->setText(QFileInfo(path).fileName());
    m_archiveLocation->setText(QDir::toNativeSeparators(m_archivePath));
    m_model->setEntries(m_listing.entries, m_search->text());
    m_view->expandToDepth(0);
    std::uint64_t total = 0;
    std::size_t files = 0;
    for (const auto &e : m_listing.entries)
        if (!e.directory) { total += e.size; ++files; }
    const double ratio = total ? 100.0 * double(m_listing.archiveSize) / double(total) : 0;
    m_summary->setText(tr("%1 files · %2 → %3 (%4%) · %5 %6")
                           .arg(files)
                           .arg(formatSize(total), formatSize(m_listing.archiveSize))
                           .arg(ratio, 0, 'f', 1)
                           .arg(QString::fromLatin1(vyra::formatName(m_listing.format)), fromUtf8(m_listing.methods)));
    m_search->setEnabled(true);
    updateActions();
    return true;
}

void MainWindow::compress(const QStringList &inputs)
{
    const QString created = Actions::compressInteractive(this, inputs);
    if (created.isEmpty()) return;
    statusBar()->showMessage(tr("Created %1").arg(QDir::toNativeSeparators(created)), 6000);
    openArchive(created);
}

void MainWindow::compressFiles()
{
    const QStringList paths = QFileDialog::getOpenFileNames(this, tr("Files to compress"));
    if (!paths.isEmpty()) compress(paths);
}

void MainWindow::compressFolder()
{
    const QString path = QFileDialog::getExistingDirectory(this, tr("Folder to compress"));
    if (!path.isEmpty()) compress({path});
}

void MainWindow::chooseArchive()
{
    const QString path = QFileDialog::getOpenFileName(this, tr("Open Archive"), QFileInfo(m_archivePath).absolutePath(),
                                                      tr("Archives (*.7z *.zip);;All files (*.*)"));
    if (!path.isEmpty()) openArchive(path);
}

void MainWindow::handleDrop(const QStringList &paths)
{
    if (paths.size() == 1 && Actions::isArchivePath(paths.first()) && QFileInfo(paths.first()).isFile()) {
        openArchive(paths.first());
        return;
    }
    compress(paths);
}

QStringList MainWindow::exportPaths(const QStringList &requestedPaths)
{
    QStringList paths = requestedPaths;
    std::sort(paths.begin(), paths.end(), [](const QString &l, const QString &r) { return l.size() < r.size(); });
    QStringList topLevel;
    for (const QString &path : paths) {
        const bool covered = std::any_of(topLevel.begin(), topLevel.end(),
                                         [&](const QString &parent) { return path == parent || path.startsWith(parent + u'/'); });
        if (!covered) topLevel << path;
    }
    const QString root = exportsRoot() + u'/' + QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!QDir().mkpath(root)) { showError(tr("Could not create a temporary export folder.")); return {}; }
    if (!Actions::extract(this, m_archivePath, root, topLevel)) {
        QDir(root).removeRecursively();
        return {};
    }
    QStringList localPaths;
    for (const QString &path : topLevel) {
        const QString local = QDir(root).absoluteFilePath(fromPath(vyra::safeRelativePath(toUtf8(path))));
        if (QFileInfo::exists(local)) localPaths << local;
    }
    return localPaths;
}

void MainWindow::cleanStaleExports()
{
    QDir root(exportsRoot());
    if (!root.exists()) return;
    const QDateTime threshold = QDateTime::currentDateTime().addDays(-2);
    for (const QFileInfo &entry : root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot))
        if (entry.lastModified() < threshold) QDir(entry.absoluteFilePath()).removeRecursively();
}

void MainWindow::copySelected()
{
    const QStringList localPaths = exportPaths(selectedPaths());
    if (localPaths.isEmpty()) return;
    QList<QUrl> urls;
    for (const QString &path : localPaths) urls << QUrl::fromLocalFile(path);
    auto *mime = new QMimeData;
    mime->setUrls(urls);
    QApplication::clipboard()->setMimeData(mime);
    statusBar()->showMessage(tr("Copied %1 item(s). Paste in Explorer.").arg(urls.size()), 5000);
}

void MainWindow::dragSelectedOut(const QStringList &paths)
{
    const QStringList localPaths = exportPaths(paths);
    if (localPaths.isEmpty()) return;
    QList<QUrl> urls;
    for (const QString &path : localPaths) urls << QUrl::fromLocalFile(path);
    auto *mime = new QMimeData;
    mime->setUrls(urls);
    auto *drag = new QDrag(m_view);
    drag->setMimeData(mime);
    drag->setPixmap(style()->standardIcon(QStyle::SP_FileIcon).pixmap(32, 32));
    drag->exec(Qt::CopyAction, Qt::CopyAction);
}

void MainWindow::openSelected(const QModelIndex &index)
{
    if (!index.isValid()) return;
    const QModelIndex nameIndex = index.siblingAtColumn(0);
    if (nameIndex.data(ArchiveModel::DirectoryRole).toBool()) {
        m_view->setExpanded(nameIndex, !m_view->isExpanded(nameIndex));
        return;
    }
    const QStringList local = exportPaths({nameIndex.data(ArchiveModel::PathRole).toString()});
    if (!local.isEmpty() && !QDesktopServices::openUrl(QUrl::fromLocalFile(local.first())))
        showError(tr("Could not open the file with the associated program."));
}

void MainWindow::showEntryMenu(const QPoint &position)
{
    const QModelIndex index = m_view->indexAt(position);
    if (!index.isValid()) return;
    if (!m_view->selectionModel()->isSelected(index)) m_view->setCurrentIndex(index);
    QMenu menu(this);
    menu.addAction(m_openEntryAction);
    menu.addAction(m_extractAction);
    menu.addAction(m_copyAction);
    menu.exec(m_view->viewport()->mapToGlobal(position));
}

QStringList MainWindow::selectedPaths() const
{
    QStringList paths;
    for (const QModelIndex &index : m_view->selectionModel()->selectedRows(0)) paths << m_model->pathForIndex(index);
    paths.removeDuplicates();
    return paths;
}

void MainWindow::extractSelected()
{
    const QStringList paths = selectedPaths();
    if (paths.isEmpty()) return;
    const QString destination = QFileDialog::getExistingDirectory(this, tr("Folder to extract the selection to"),
                                                                  QFileInfo(m_archivePath).absolutePath());
    if (destination.isEmpty()) return;
    if (Actions::extract(this, m_archivePath, destination, paths)) statusBar()->showMessage(tr("Extracted the selection."), 4000);
}

void MainWindow::extractAll()
{
    if (m_archivePath.isEmpty()) return;
    QFileDialog dialog(this, tr("Folder to extract everything to"), Actions::folderDestination(m_archivePath));
    dialog.setFileMode(QFileDialog::Directory);
    dialog.setOption(QFileDialog::ShowDirsOnly);
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.selectFile(Actions::folderDestination(m_archivePath));
    if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().isEmpty()) return;
    const QString destination = dialog.selectedFiles().first();
    if (Actions::extract(this, m_archivePath, destination)) {
        statusBar()->showMessage(tr("Extracted to %1").arg(QDir::toNativeSeparators(destination)), 6000);
        QDesktopServices::openUrl(QUrl::fromLocalFile(destination));
    }
}

void MainWindow::testArchive()
{
    if (!m_archivePath.isEmpty()) Actions::test(this, m_archivePath);
}

void MainWindow::showProperties()
{
    if (m_archivePath.isEmpty()) return;
    std::uint64_t total = 0;
    std::size_t files = 0, folders = 0;
    for (const auto &e : m_listing.entries) {
        if (e.directory) ++folders;
        else { total += e.size; ++files; }
    }
    const double ratio = total ? 100.0 * double(m_listing.archiveSize) / double(total) : 0;
    QMessageBox::information(this, tr("Archive Properties"),
        tr("<b>%1</b><br><br>Format: %2<br>Methods: %3%4<br>Files: %5 · Folders: %6<br>"
           "Original size: %7<br>Archive size: %8 (%9%)")
            .arg(QFileInfo(m_archivePath).fileName().toHtmlEscaped(), QString::fromLatin1(vyra::formatName(m_listing.format)),
                 fromUtf8(m_listing.methods), m_listing.solid ? tr(" (solid)") : QString())
            .arg(files)
            .arg(folders)
            .arg(formatSize(total), formatSize(m_listing.archiveSize))
            .arg(ratio, 0, 'f', 1));
}

void MainWindow::refresh()
{
    if (!m_archivePath.isEmpty()) openArchive(m_archivePath);
}

void MainWindow::configureShell()
{
    QString error;
    const bool korean = m_koreanAction->isChecked();
    if (ShellIntegration::isMachineInstalled()) {
        QMessageBox::information(this, tr("Windows Explorer Integration"),
            tr("Explorer integration is installed system-wide by the installer.\nTo change or remove it, use Windows Installed apps."));
    } else if (ShellIntegration::isInstalled()) {
        const auto choice = QMessageBox::question(this, tr("Windows Explorer Integration"),
            tr("Explorer integration is installed.\n\nYes: refresh it (e.g. after moving Vyra or changing the language)\nNo: remove it"),
            QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);
        if (choice == QMessageBox::Yes) {
            if (!ShellIntegration::install(QCoreApplication::applicationFilePath(), korean, &error)) showError(error);
            else QMessageBox::information(this, tr("Done"), tr("Explorer integration updated."));
        } else if (choice == QMessageBox::No) {
            if (!ShellIntegration::uninstall(&error)) showError(error);
            else QMessageBox::information(this, tr("Done"), tr("Explorer integration removed."));
        }
    } else if (QMessageBox::question(this, tr("Windows Explorer Integration"),
                   tr("Add Vyra to the Explorer right-click menu for the current user?\n\n"
                      "• Files and folders: Compress…, Fast compress to .7z, Compress to .zip\n"
                      "• .7z / .zip files: Open, Extract Here, Extract to Folder, Test\n\n"
                      "No administrator rights are needed. On Windows 11 the entries appear under \"Show more options\".")) == QMessageBox::Yes) {
        if (!ShellIntegration::install(QCoreApplication::applicationFilePath(), korean, &error)) showError(error);
        else QMessageBox::information(this, tr("Done"), tr("Explorer integration installed."));
    }
}

void MainWindow::switchLanguage(const QString &languageCode)
{
    QSettings settings;
    settings.setValue(QStringLiteral("ui/language"), languageCode);
    if (ShellIntegration::isInstalled()) ShellIntegration::install(QCoreApplication::applicationFilePath(), languageCode == QStringLiteral("ko"));
    const auto choice = QMessageBox::question(this, tr("Language"), tr("The language will change after restarting the app. Restart now?"),
                                              QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
    if (choice == QMessageBox::Yes) {
        QProcess::startDetached(QCoreApplication::applicationFilePath(), m_archivePath.isEmpty() ? QStringList() : QStringList{m_archivePath});
        QCoreApplication::quit();
    }
}

void MainWindow::showAbout()
{
    const vyra::GpuStatus gpu = vyra::queryGpu();
    QMessageBox::about(this, tr("About Vyra"),
        tr("<h2>Vyra %1</h2><p>GPU accelerated 7z / zip archiver built on NVIDIA nvCOMP.</p>"
           "<p>Engine: %2</p><p>Qt %3 · zlib-ng · LZ4 · Zstandard · LZMA SDK</p><p>MIT License</p>")
            .arg(QStringLiteral(VYRA_VERSION), fromUtf8(gpu.available ? gpu.description : "CPU (" + gpu.description + ")"),
                 QString::fromLatin1(qVersion())));
}

void MainWindow::updateActions()
{
    const bool open = !m_archivePath.isEmpty();
    const bool selected = m_view->selectionModel() && m_view->selectionModel()->hasSelection();
    m_extractAction->setEnabled(open && selected);
    m_extractAllAction->setEnabled(open);
    m_testAction->setEnabled(open);
    m_copyAction->setEnabled(open && selected);
    m_openEntryAction->setEnabled(open && selected);
    m_propertiesAction->setEnabled(open);
    m_refreshAction->setEnabled(open);
}

void MainWindow::showError(const QString &message) { QMessageBox::critical(this, QStringLiteral("Vyra"), message); }
