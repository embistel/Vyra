#pragma once

#include "core/archive.h"

#include <QMainWindow>

class ArchiveModel;
class DropView;
class QAction;
class QLabel;
class QLineEdit;
class QModelIndex;

class MainWindow final : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    bool openArchive(const QString &path);

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void compressFiles();
    void compressFolder();
    void chooseArchive();
    void handleDrop(const QStringList &paths);
    void dragSelectedOut(const QStringList &paths);
    void openSelected(const QModelIndex &index);
    void showEntryMenu(const QPoint &position);
    void extractSelected();
    void extractAll();
    void testArchive();
    void copySelected();
    void showProperties();
    void refresh();
    void configureShell();
    void showAbout();

private:
    void compress(const QStringList &inputs);
    void switchLanguage(const QString &languageCode);
    QStringList selectedPaths() const;
    QStringList exportPaths(const QStringList &paths);
    static void cleanStaleExports();
    void updateActions();
    void updateEngineChip();
    void showError(const QString &message);

    QString m_archivePath;
    vyra::ArchiveListing m_listing;
    ArchiveModel *m_model = nullptr;
    DropView *m_view = nullptr;
    QLineEdit *m_search = nullptr;
    QLabel *m_summary = nullptr;
    QLabel *m_archiveName = nullptr;
    QLabel *m_archiveLocation = nullptr;
    QLabel *m_engineChip = nullptr;
    QAction *m_extractAction = nullptr;
    QAction *m_extractAllAction = nullptr;
    QAction *m_testAction = nullptr;
    QAction *m_copyAction = nullptr;
    QAction *m_openEntryAction = nullptr;
    QAction *m_propertiesAction = nullptr;
    QAction *m_refreshAction = nullptr;
    QAction *m_englishAction = nullptr;
    QAction *m_koreanAction = nullptr;
};
