#ifndef FINDRESULTSMODEL_H
#define FINDRESULTSMODEL_H

#include "libfinder.h"

#include <QAbstractTableModel>
#include <QHash>
#include <QIcon>
#include <QList>

class FinderResultsModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    explicit FinderResultsModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

    void setExternalSource(const QList<FinderSearchResult> *source);
    void setOwnedRows(QList<FinderSearchResult> &&rows);
    void clearResults();
    void notifyExternalRowsInserted(int firstRow, int lastRowInclusive);
    bool isUsingExternalSource(const QList<FinderSearchResult> *source = nullptr) const;

    QString pathAtRow(int row) const;
    const FinderSearchResult &resultAt(int row) const;

private:
    const QList<FinderSearchResult> *m_externalSource = nullptr;
    QList<FinderSearchResult> m_ownedRows;

    const FinderSearchResult &rowAt(int row) const;

    mutable QHash<QString, QIcon> m_iconCache;
};

#endif // FINDRESULTSMODEL_H
