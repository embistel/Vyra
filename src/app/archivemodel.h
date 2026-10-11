#pragma once

#include "core/common.h"

#include <QStandardItemModel>

#include <vector>

class ArchiveModel final : public QStandardItemModel {
    Q_OBJECT
public:
    enum Roles { PathRole = Qt::UserRole + 1, DirectoryRole, SortRole };
    explicit ArchiveModel(QObject *parent = nullptr);
    void setEntries(const std::vector<vyra::Entry> &entries, const QString &filter = {});
    QString pathForIndex(const QModelIndex &index) const;
};
