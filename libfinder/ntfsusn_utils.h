#ifndef NTFSUSN_UTILS_H
#define NTFSUSN_UTILS_H

#include "libfinder_global.h"

#include <QDateTime>
#include <QDir>
#include <QHash>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#endif

namespace NtfsUsnUtils
{
struct Node
{
    quint64 frn = 0;
    quint64 parentFrn = 0;
    QString name;
    bool isDirectory = false;
};

struct DecodedEvent
{
    quint64 frn = 0;
    quint64 parentFrn = 0;
    QString name;
    bool isDirectory = false;
    quint32 reason = 0;
    bool valid = false;
};

LIBFINDER_EXPORT QString buildPath(const QString &volumeRoot, const QHash<quint64, Node> &nodes, quint64 frn);
LIBFINDER_EXPORT QString formatChannelStatus(const QString &state, const QString &detail, const QDateTime &timestamp);

#ifdef Q_OS_WIN
LIBFINDER_EXPORT DecodedEvent decodeRecord(const USN_RECORD *baseRecord);
#endif

template <typename T>
int removeRecordsUnderPath(QHash<QString, T> *records, const QString &basePath)
{
    if (!records) {
        return 0;
    }

    const QString cleanBase = QDir::cleanPath(basePath);
    if (cleanBase.isEmpty()) {
        return 0;
    }

    const QString prefix = cleanBase.endsWith('/') ? cleanBase : cleanBase + '/';
    const QList<QString> keys = records->keys();
    int removed = 0;
    for (const QString &path : keys) {
        if (path.compare(cleanBase, Qt::CaseInsensitive) == 0 || path.startsWith(prefix, Qt::CaseInsensitive)) {
            records->remove(path);
            ++removed;
        }
    }
    return removed;
}
}

#endif // NTFSUSN_UTILS_H
