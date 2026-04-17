#include "ntfsusn_utils.h"

#include <QDir>

#ifdef Q_OS_WIN
#include <cstring>
#endif

namespace NtfsUsnUtils
{
QString buildPath(const QString &volumeRoot, const QHash<quint64, Node> &nodes, quint64 frn)
{
    if (!nodes.contains(frn)) {
        return QString();
    }

    QStringList segments;
    QSet<quint64> visited;
    quint64 current = frn;

    while (!visited.contains(current)) {
        visited.insert(current);
        const auto it = nodes.find(current);
        if (it == nodes.end()) {
            break;
        }

        const Node &node = it.value();
        if (!node.name.isEmpty()) {
            segments.prepend(node.name);
        }
        if (node.parentFrn == 0 || node.parentFrn == current) {
            break;
        }
        current = node.parentFrn;
    }

    const QString relative = segments.join('/');
    if (relative.isEmpty()) {
        return QDir::cleanPath(volumeRoot);
    }
    return QDir::cleanPath(volumeRoot + QStringLiteral("/") + relative);
}

QString formatChannelStatus(const QString &state, const QString &detail, const QDateTime &timestamp)
{
    const QString timeText = timestamp.isValid()
            ? timestamp.toString(QStringLiteral("HH:mm:ss"))
            : QStringLiteral("--:--:--");
    if (detail.isEmpty()) {
        return QStringLiteral("%1 @ %2").arg(state, timeText);
    }
    return QStringLiteral("%1 | %2 @ %3").arg(state, detail, timeText);
}

#ifdef Q_OS_WIN
namespace
{
quint64 fileIdToU64(const FILE_ID_128 &fileId)
{
    quint64 value = 0;
    memcpy(&value, &fileId, sizeof(quint64));
    return value;
}
}

DecodedEvent decodeRecord(const USN_RECORD *baseRecord)
{
    DecodedEvent event;
    if (!baseRecord) {
        return event;
    }

    if (baseRecord->MajorVersion == 2) {
        const USN_RECORD_V2 *record = reinterpret_cast<const USN_RECORD_V2 *>(baseRecord);
        event.frn = static_cast<quint64>(record->FileReferenceNumber);
        event.parentFrn = static_cast<quint64>(record->ParentFileReferenceNumber);
        event.name = QString::fromWCharArray(record->FileName, record->FileNameLength / sizeof(WCHAR));
        event.isDirectory = (record->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        event.reason = record->Reason;
        event.valid = true;
        return event;
    }

#if defined(NTDDI_VERSION) && (NTDDI_VERSION >= NTDDI_WIN8)
    if (baseRecord->MajorVersion == 3) {
        const USN_RECORD_V3 *record = reinterpret_cast<const USN_RECORD_V3 *>(baseRecord);
        event.frn = fileIdToU64(record->FileReferenceNumber);
        event.parentFrn = fileIdToU64(record->ParentFileReferenceNumber);
        event.name = QString::fromWCharArray(record->FileName, record->FileNameLength / sizeof(WCHAR));
        event.isDirectory = (record->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        event.reason = record->Reason;
        event.valid = true;
        return event;
    }
#endif

    return event;
}
#endif
}
