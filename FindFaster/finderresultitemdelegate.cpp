#include "finderresultitemdelegate.h"

#include <QApplication>
#include <QPainter>
#include <QStyle>

FinderResultItemDelegate::FinderResultItemDelegate(QObject *parent)
    : QStyledItemDelegate(parent)
{
}

void FinderResultItemDelegate::paint(QPainter *painter,
                                     const QStyleOptionViewItem &option,
                                     const QModelIndex &index) const
{
    QStyleOptionViewItem opt(option);
    initStyleOption(&opt, index);

    QStyle *style = opt.widget ? opt.widget->style() : QApplication::style();
    style->drawPrimitive(QStyle::PE_PanelItemViewItem, &opt, painter, opt.widget);

    QRect textRect = opt.rect.adjusted(6, 0, -6, 0);
    if (index.column() == 0) {
        const QVariant iconVar = index.data(Qt::DecorationRole);
        if (iconVar.isValid()) {
            const QIcon icon = qvariant_cast<QIcon>(iconVar);
            const QSize iconSize(16, 16);
            const QRect iconRect(textRect.left(),
                                 textRect.top() + (textRect.height() - iconSize.height()) / 2,
                                 iconSize.width(),
                                 iconSize.height());
            icon.paint(painter, iconRect, Qt::AlignCenter, QIcon::Normal, QIcon::On);
            textRect.adjust(iconSize.width() + 6, 0, 0, 0);
        }
    }

    const QString text = index.data(Qt::DisplayRole).toString();
    const int align = index.data(Qt::TextAlignmentRole).isValid()
            ? index.data(Qt::TextAlignmentRole).toInt()
            : static_cast<int>(Qt::AlignLeft | Qt::AlignVCenter);

    painter->save();
    painter->setPen(opt.palette.color((opt.state & QStyle::State_Selected) ? QPalette::HighlightedText : QPalette::Text));
    painter->drawText(textRect, align, text);
    painter->restore();
}
