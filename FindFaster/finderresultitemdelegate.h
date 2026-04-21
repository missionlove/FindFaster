#ifndef FINDERRESULTITEMDELEGATE_H
#define FINDERRESULTITEMDELEGATE_H

#include <QStyledItemDelegate>

class FinderResultItemDelegate : public QStyledItemDelegate
{
    Q_OBJECT
public:
    explicit FinderResultItemDelegate(QObject *parent = nullptr);

    void paint(QPainter *painter,
               const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;
};

#endif // FINDERRESULTITEMDELEGATE_H
