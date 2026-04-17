#include "findresultsmodel.h"

#include <QFileIconProvider>
#include <QFileInfo>
#include <QtGlobal>
#include <utility>

namespace
{
QString formatFileSize(qint64 bytes)
{
    const QStringList units = {QStringLiteral("B"), QStringLiteral("KB"), QStringLiteral("MB"),
                               QStringLiteral("GB"), QStringLiteral("TB")};
    double size = static_cast<double>(qMax<qint64>(bytes, 0));
    int unitIndex = 0;
    while (size >= 1024.0 && unitIndex < units.size() - 1) {
        size /= 1024.0;
        ++unitIndex;
    }

    if (unitIndex == 0) {
        return QStringLiteral("%1 %2").arg(static_cast<qint64>(size)).arg(units.at(unitIndex));
    }
    return QStringLiteral("%1 %2").arg(QString::number(size, 'f', 1)).arg(units.at(unitIndex));
}
}

FinderResultsModel::FinderResultsModel(QObject *parent)
    : QAbstractTableModel(parent)
{
}

const FinderSearchResult &FinderResultsModel::rowAt(int row) const
{
    if (m_externalSource) {
        return m_externalSource->at(row);
    }
    return m_ownedRows.at(row);
}

int FinderResultsModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid()) {
        return 0;
    }
    if (m_externalSource) {
        return m_externalSource->size();
    }
    return m_ownedRows.size();
}

int FinderResultsModel::columnCount(const QModelIndex &parent) const
{
    if (parent.isValid()) {
        return 0;
    }
    return 4;
}

QVariant FinderResultsModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()) {
        return QVariant();
    }

    const FinderSearchResult &row = rowAt(index.row());
    const QString &path = row.entry.path;

    if (role == Qt::UserRole) {
        return path;
    }

    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case 0:
            return row.entry.name;
        case 1:
            return row.entry.path;
        case 2:
            return formatFileSize(row.entry.sizeBytes);
        case 3:
            return row.entry.lastModified.isValid()
                    ? row.entry.lastModified.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
                    : QStringLiteral("-");
        default:
            return QVariant();
        }
    }

    if (role == Qt::DecorationRole && index.column() == 0) {
        static QFileIconProvider iconProvider;
        const QFileInfo info(path);
        const QString suffix = info.suffix().toLower();
        const QString cacheKey = suffix.isEmpty() ? QStringLiteral("__no_suffix__") : suffix;
        if (m_iconCache.contains(cacheKey)) {
            return m_iconCache.value(cacheKey);
        }
        const QIcon icon = iconProvider.icon(info);
        m_iconCache.insert(cacheKey, icon);
        return icon;
    }

    if (role == Qt::TextAlignmentRole && index.column() == 2) {
        return QVariant(static_cast<int>(Qt::AlignRight | Qt::AlignVCenter));
    }

    if (role == Qt::ToolTipRole && index.column() == 0) {
        return QStringLiteral("score=%1").arg(row.score);
    }

    return QVariant();
}

QVariant FinderResultsModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
        return QAbstractTableModel::headerData(section, orientation, role);
    }
    switch (section) {
    case 0:
        return QStringLiteral("名称");
    case 1:
        return QStringLiteral("路径");
    case 2:
        return QStringLiteral("大小");
    case 3:
        return QStringLiteral("修改时间");
    default:
        return QVariant();
    }
}

void FinderResultsModel::setExternalSource(const QList<FinderSearchResult> *source)
{
    beginResetModel();
    m_externalSource = source;
    m_ownedRows.clear();
    endResetModel();
}

void FinderResultsModel::setOwnedRows(QList<FinderSearchResult> &&rows)
{
    beginResetModel();
    m_externalSource = nullptr;
    m_ownedRows = std::move(rows);
    endResetModel();
}

void FinderResultsModel::clearResults()
{
    beginResetModel();
    m_externalSource = nullptr;
    m_ownedRows.clear();
    endResetModel();
}

void FinderResultsModel::notifyExternalRowsInserted(int firstRow, int lastRowInclusive)
{
    if (!m_externalSource || firstRow > lastRowInclusive || firstRow < 0) {
        return;
    }
    beginInsertRows(QModelIndex(), firstRow, lastRowInclusive);
    endInsertRows();
}

QString FinderResultsModel::pathAtRow(int row) const
{
    if (row < 0 || row >= rowCount()) {
        return QString();
    }
    return rowAt(row).entry.path;
}

const FinderSearchResult &FinderResultsModel::resultAt(int row) const
{
    if (row < 0 || row >= rowCount()) {
        static const FinderSearchResult s_empty;
        return s_empty;
    }
    return rowAt(row);
}
