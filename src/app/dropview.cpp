#include "app/dropview.h"
#include "app/archivemodel.h"

#include <QDragEnterEvent>
#include <QMimeData>
#include <QPainter>
#include <QUrl>

DropView::DropView(QWidget *parent) : QTreeView(parent)
{
    setAcceptDrops(true);
    setDragEnabled(true);
    setDragDropMode(QAbstractItemView::DragDrop);
    setDropIndicatorShown(true);
}

void DropView::startDrag(Qt::DropActions)
{
    QStringList paths;
    for (const QModelIndex &index : selectionModel()->selectedRows(0))
        paths << index.data(ArchiveModel::PathRole).toString();
    paths.removeAll(QString{});
    paths.removeDuplicates();
    if (!paths.isEmpty()) emit archiveDragRequested(paths);
}

void DropView::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls() && event->source() != this) event->acceptProposedAction();
}

void DropView::dragMoveEvent(QDragMoveEvent *event)
{
    if (event->mimeData()->hasUrls() && event->source() != this) event->acceptProposedAction();
}

void DropView::dropEvent(QDropEvent *event)
{
    QStringList paths;
    for (const QUrl &url : event->mimeData()->urls()) if (url.isLocalFile()) paths << url.toLocalFile();
    if (paths.isEmpty()) return;
    emit localFilesDropped(paths);
    event->acceptProposedAction();
}

void DropView::paintEvent(QPaintEvent *event)
{
    QTreeView::paintEvent(event);
    if (model() && model()->rowCount() > 0) return;
    // Empty state: invite a drop.
    QPainter painter(viewport());
    painter.setPen(QColor(0x66, 0x70, 0x85));
    QFont font = painter.font();
    font.setPointSizeF(font.pointSizeF() + 1.5);
    painter.setFont(font);
    painter.drawText(viewport()->rect().adjusted(24, 24, -24, -24), Qt::AlignCenter | Qt::TextWordWrap,
                     tr("Drop files or folders here to compress them\nor drop a .7z / .zip / .gz / .lz4 file to open it."));
}
