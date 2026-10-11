#include "app/archivemodel.h"
#include "app/qtbridge.h"

#include <QFileIconProvider>
#include <QHash>
#include <QLocale>

ArchiveModel::ArchiveModel(QObject *parent) : QStandardItemModel(parent)
{
    setHorizontalHeaderLabels({tr("Name"), tr("Size"), tr("Packed"), tr("Modified"), tr("Method"), tr("CRC")});
    setSortRole(SortRole);
}

void ArchiveModel::setEntries(const std::vector<vyra::Entry> &entries, const QString &filter)
{
    removeRows(0, rowCount());
    QFileIconProvider icons;
    const QIcon folderIcon = icons.icon(QFileIconProvider::Folder);
    const QIcon fileIcon = icons.icon(QFileIconProvider::File);
    QHash<QString, QStandardItem *> folders;
    // Directories may be implicit (only present as path prefixes) or listed
    // after their children, so folders are created on demand.
    auto folderFor = [&](const QString &path) -> QStandardItem * {
        if (path.isEmpty()) return invisibleRootItem();
        if (auto *f = folders.value(path)) return f;
        QStandardItem *parent = invisibleRootItem();
        QString current;
        for (const QString &part : path.split(u'/', Qt::SkipEmptyParts)) {
            current = current.isEmpty() ? part : current + u'/' + part;
            QStandardItem *item = folders.value(current);
            if (!item) {
                item = new QStandardItem(folderIcon, part);
                item->setData(current, PathRole);
                item->setData(true, DirectoryRole);
                item->setData(QStringLiteral("0") + part.toLower(), SortRole);
                QList<QStandardItem *> row{item};
                for (int c = 1; c < 6; ++c) row << new QStandardItem;
                for (QStandardItem *cell : row) cell->setEditable(false);
                parent->appendRow(row);
                folders.insert(current, item);
            }
            parent = item;
        }
        return parent;
    };
    const QLocale locale;
    for (const vyra::Entry &entry : entries) {
        const QString path = fromUtf8(entry.path);
        if (!filter.isEmpty() && !path.contains(filter, Qt::CaseInsensitive)) continue;
        if (entry.directory) {
            QStandardItem *dir = folderFor(path);
            const QModelIndex index = dir->index();
            if (entry.mtime && index.isValid()) {
                QStandardItem *timeCell = itemFromIndex(index.siblingAtColumn(3));
                if (timeCell) timeCell->setText(locale.toString(fromFileTime(entry.mtime), QLocale::ShortFormat));
            }
            continue;
        }
        const int slash = int(path.lastIndexOf(u'/'));
        QStandardItem *parent = folderFor(slash > 0 ? path.left(slash) : QString());
        const QString name = slash >= 0 ? path.mid(slash + 1) : path;
        auto *nameItem = new QStandardItem(fileIcon, name);
        nameItem->setData(path, PathRole);
        nameItem->setData(false, DirectoryRole);
        nameItem->setData(QStringLiteral("1") + name.toLower(), SortRole);
        auto *size = new QStandardItem(formatSize(entry.size));
        size->setData(qulonglong(entry.size), SortRole);
        size->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        auto *packed = new QStandardItem(entry.packedSize ? formatSize(entry.packedSize) : QString());
        packed->setData(qulonglong(entry.packedSize), SortRole);
        packed->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        const QDateTime when = fromFileTime(entry.mtime);
        auto *modified = new QStandardItem(when.isValid() ? locale.toString(when, QLocale::ShortFormat) : QString());
        modified->setData(when, SortRole);
        QString method = QString::fromLatin1(vyra::methodName(entry.method));
        if (entry.encrypted) method += tr(" (encrypted)");
        else if (!entry.supported) method += tr(" (unsupported)");
        auto *methodItem = new QStandardItem(method);
        methodItem->setData(method, SortRole);
        auto *crc = new QStandardItem(entry.hasCrc ? QStringLiteral("%1").arg(entry.crc, 8, 16, QLatin1Char('0')).toUpper() : QString());
        crc->setData(crc->text(), SortRole);
        QList<QStandardItem *> row{nameItem, size, packed, modified, methodItem, crc};
        for (QStandardItem *cell : row) cell->setEditable(false);
        parent->appendRow(row);
    }
}

QString ArchiveModel::pathForIndex(const QModelIndex &index) const
{
    return index.siblingAtColumn(0).data(PathRole).toString();
}
