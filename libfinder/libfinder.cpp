#include "libfinder.h"
#include "ntfsusn_utils.h"

#include <QtConcurrent/QtConcurrent>
#include <QBuffer>
#include <QByteArray>
#include <QCoreApplication>
#include <QDataStream>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QFileSystemWatcher>
#include <QFuture>
#include <QFutureWatcher>
#include <QHash>
#include <QMutex>
#include <QReadWriteLock>
#include <QSet>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QThread>
#include <QTimer>
#include <QVector>
#include <algorithm>
#include <atomic>
#include <climits>
#include <memory>
#include <queue>
#include <utility>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#endif

namespace
{
QStringList enumerateWatchDirectories(const FinderIndexOptions &options)
{
    static const int kMaxWatchDirs = 2048;
    QStringList directories;
    for (const QString &root : options.roots) {
        const QString normalizedRoot = QDir::cleanPath(root);
        if (normalizedRoot.isEmpty()) {
            continue;
        }
        QDir rootDir(normalizedRoot);
        if (!rootDir.exists()) {
            continue;
        }

        directories << rootDir.absolutePath();
        if (directories.size() >= kMaxWatchDirs) {
            break;
        }

        QDirIterator it(normalizedRoot, QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            it.next();
            directories << QDir::cleanPath(it.filePath());
            if (directories.size() >= kMaxWatchDirs) {
                break;
            }
        }
        if (directories.size() >= kMaxWatchDirs) {
            break;
        }
    }
    directories.removeDuplicates();
    return directories;
}

QString backendName(FinderIndexOptions::Backend backend)
{
    switch (backend) {
    case FinderIndexOptions::Backend::Auto:
        return QStringLiteral("auto");
    case FinderIndexOptions::Backend::Generic:
        return QStringLiteral("generic-recursive");
    case FinderIndexOptions::Backend::NtfsUsn:
        return QStringLiteral("ntfs-usn");
    }
    return QStringLiteral("unknown");
}

QStringList defaultSearchRoots()
{
    QStringList roots;
#ifdef Q_OS_WIN
    const QFileInfoList drives = QDir::drives();
    for (const QFileInfo &drive : drives) {
        const QString root = QDir::cleanPath(drive.absoluteFilePath());
        if (!root.isEmpty()) {
            roots << root;
        }
    }
#else
    roots << QDir::homePath();
#endif
    roots.removeDuplicates();
    return roots;
}

bool allRootsAreNtfs(QStringList roots)
{
    if (roots.isEmpty()) {
        roots = defaultSearchRoots();
    }
    if (roots.isEmpty()) {
        return false;
    }

    for (const QString &root : roots) {
        QStorageInfo storage(root);
        if (!storage.isValid() || !storage.isReady()) {
            return false;
        }
        const QString fsType = QString::fromLatin1(storage.fileSystemType());
        if (fsType.compare(QStringLiteral("ntfs"), Qt::CaseInsensitive) != 0) {
            return false;
        }
    }
    return true;
}

FinderIndexOptions::Backend resolveBackend(const FinderIndexOptions &options)
{
    if (options.backend == FinderIndexOptions::Backend::Generic) {
        return FinderIndexOptions::Backend::Generic;
    }
    if (options.backend == FinderIndexOptions::Backend::NtfsUsn) {
        return FinderIndexOptions::Backend::NtfsUsn;
    }

#ifdef Q_OS_WIN
    if (allRootsAreNtfs(options.roots)) {
        return FinderIndexOptions::Backend::NtfsUsn;
    }
#endif
    return FinderIndexOptions::Backend::Generic;
}

void setChannelStatus(FinderIndexStats *stats, const QString &state, const QString &detail = QString())
{
    if (!stats) {
        return;
    }
    stats->channelState = state;
    stats->channelDetail = detail;
    stats->channelUpdatedAt = QDateTime::currentDateTime();
}

QString buildSearchableText(const FinderIndexedRecord &record)
{
    return QStringLiteral("%1 %2").arg(record.nameLower, record.pathLower);
}

QStringList tokenizeName(const QString &nameLower)
{
    QStringList tokens;
    QString current;
    current.reserve(nameLower.size());

    for (const QChar ch : nameLower) {
        if (ch.isLetterOrNumber()) {
            current.append(ch);
            continue;
        }
        if (!current.isEmpty()) {
            tokens.push_back(current);
            current.clear();
        }
    }
    if (!current.isEmpty()) {
        tokens.push_back(current);
    }

    if (tokens.isEmpty() && !nameLower.isEmpty()) {
        tokens.push_back(nameLower);
    }
    tokens.removeDuplicates();
    return tokens;
}

void buildDerivedIndexesFromRecords(const QHash<QString, FinderIndexedRecord> &recordsByPath,
                                    QVector<FinderIndexedRecord> *recordsLinear,
                                    QHash<QString, QVector<int>> *nameInverted,
                                    QHash<QString, QVector<int>> *pathInverted,
                                    QVector<int> *allRecordIndexes)
{
    if (!recordsLinear || !nameInverted || !allRecordIndexes) {
        return;
    }

    recordsLinear->clear();
    nameInverted->clear();
    if (pathInverted) {
        pathInverted->clear();
    }
    allRecordIndexes->clear();

    recordsLinear->reserve(recordsByPath.size());
    allRecordIndexes->reserve(recordsByPath.size());

    int index = 0;
    for (auto it = recordsByPath.begin(); it != recordsByPath.end(); ++it) {
        FinderIndexedRecord record = it.value();
        if (record.nameLower.isEmpty()) {
            record.nameLower = record.entry.name.toLower();
        }
        if (record.pathLower.isEmpty()) {
            record.pathLower = record.entry.path.toLower();
        }
        if (record.searchableText.isEmpty()) {
            record.searchableText = buildSearchableText(record);
        }

        recordsLinear->push_back(record);
        allRecordIndexes->push_back(index);

        const QStringList nameTokens = tokenizeName(record.nameLower);
        for (const QString &token : nameTokens) {
            (*nameInverted)[token].push_back(index);
        }
        if (pathInverted) {
            const QStringList pathTokens = tokenizeName(record.pathLower);
            for (const QString &token : pathTokens) {
                (*pathInverted)[token].push_back(index);
            }
        }
        ++index;
    }

    for (auto it = nameInverted->begin(); it != nameInverted->end(); ++it) {
        it.value().squeeze();
    }
    if (pathInverted) {
        for (auto it = pathInverted->begin(); it != pathInverted->end(); ++it) {
            it.value().squeeze();
        }
    }
}

void buildInvertedIndexesFromLinear(const QVector<FinderIndexedRecord> &recordsLinear,
                                    const QVector<int> &allRecordIndexes,
                                    QHash<QString, QVector<int>> *nameInverted,
                                    QHash<QString, QVector<int>> *pathInverted)
{
    if (!nameInverted || !pathInverted) {
        return;
    }
    nameInverted->clear();
    pathInverted->clear();

    for (int rawIndex : allRecordIndexes) {
        if (rawIndex < 0 || rawIndex >= recordsLinear.size()) {
            continue;
        }
        const FinderIndexedRecord &record = recordsLinear.at(rawIndex);
        const QString nameLower = record.nameLower.isEmpty() ? record.entry.name.toLower() : record.nameLower;
        const QString pathLower = record.pathLower.isEmpty() ? record.entry.path.toLower() : record.pathLower;

        const QStringList nameTokens = tokenizeName(nameLower);
        for (const QString &token : nameTokens) {
            (*nameInverted)[token].push_back(rawIndex);
        }
        const QStringList pathTokens = tokenizeName(pathLower);
        for (const QString &token : pathTokens) {
            (*pathInverted)[token].push_back(rawIndex);
        }
    }

    for (auto it = nameInverted->begin(); it != nameInverted->end(); ++it) {
        it.value().squeeze();
    }
    for (auto it = pathInverted->begin(); it != pathInverted->end(); ++it) {
        it.value().squeeze();
    }
}

class Indexer
{
public:
    bool build(const FinderIndexOptions &options, QHash<QString, FinderIndexedRecord> *records, FinderIndexStats *stats, QString *errorMessage)
    {
        if (!records || !stats) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Internal error: invalid output storage.");
            }
            return false;
        }

        records->clear();

        QStringList roots = options.roots;
        if (roots.isEmpty()) {
            roots = defaultSearchRoots();
        }

        QSet<QString> excludedNormalized;
        for (const QString &excludePath : options.excludes) {
            excludedNormalized.insert(QDir::cleanPath(excludePath).toLower());
        }

        const QDir::Filters filters = options.includeHidden
                ? (QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden)
                : (QDir::AllEntries | QDir::NoDotAndDotDot);

        int scannedFiles = 0;
        int indexedRoots = 0;

        for (const QString &root : roots) {
            const QString normalizedRoot = QDir::cleanPath(root);
            if (normalizedRoot.isEmpty()) {
                continue;
            }

            QDir rootDir(normalizedRoot);
            if (!rootDir.exists()) {
                continue;
            }

            ++indexedRoots;

            QDirIterator it(normalizedRoot, filters, QDirIterator::Subdirectories);
            while (it.hasNext()) {
                if (options.maxFiles > 0 && scannedFiles >= options.maxFiles) {
                    break;
                }

                it.next();
                const QFileInfo info = it.fileInfo();
                if (!info.isFile()) {
                    continue;
                }

                const QString filePath = QDir::cleanPath(info.absoluteFilePath());
                if (isExcluded(filePath, excludedNormalized)) {
                    continue;
                }

                FinderIndexedRecord record;
                record.entry.name = info.fileName();
                record.entry.path = filePath;
                record.entry.sizeBytes = info.size();
                record.entry.lastModified = info.lastModified();
                record.nameLower = record.entry.name.toLower();
                record.pathLower = record.entry.path.toLower();
                record.searchableText = buildSearchableText(record);

                records->insert(record.entry.path, record);
                ++scannedFiles;
            }

            if (options.maxFiles > 0 && scannedFiles >= options.maxFiles) {
                break;
            }
        }

        stats->totalFiles = records->size();
        stats->indexedRoots = indexedRoots;
        stats->indexedAt = QDateTime::currentDateTime();
        return true;
    }

    int rescanDirectory(const QString &directoryPath, const FinderIndexOptions &options, QHash<QString, FinderIndexedRecord> *records)
    {
        if (!records) {
            return 0;
        }

        const QString normalizedDir = QDir::cleanPath(directoryPath);
        if (normalizedDir.isEmpty() || !QDir(normalizedDir).exists()) {
            return 0;
        }

        const QString dirPrefix = normalizedDir.endsWith('/') ? normalizedDir : normalizedDir + '/';
        const QList<QString> keys = records->keys();
        for (const QString &path : keys) {
            if (path == normalizedDir || path.startsWith(dirPrefix, Qt::CaseInsensitive)) {
                records->remove(path);
            }
        }

        const QDir::Filters filters = options.includeHidden
                ? (QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden)
                : (QDir::AllEntries | QDir::NoDotAndDotDot);

        QSet<QString> excludedNormalized;
        for (const QString &excludePath : options.excludes) {
            excludedNormalized.insert(QDir::cleanPath(excludePath).toLower());
        }

        int added = 0;
        QDirIterator it(normalizedDir, filters, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            it.next();
            const QFileInfo info = it.fileInfo();
            if (!info.isFile()) {
                continue;
            }

            const QString filePath = QDir::cleanPath(info.absoluteFilePath());
            if (isExcluded(filePath, excludedNormalized)) {
                continue;
            }

            FinderIndexedRecord record;
            record.entry.name = info.fileName();
            record.entry.path = filePath;
            record.entry.sizeBytes = info.size();
            record.entry.lastModified = info.lastModified();
            record.nameLower = record.entry.name.toLower();
            record.pathLower = record.entry.path.toLower();
            record.searchableText = buildSearchableText(record);
            records->insert(record.entry.path, record);
            ++added;
        }
        return added;
    }

private:
    bool isExcluded(const QString &path, const QSet<QString> &excludedPaths) const
    {
        const QString lowerPath = path.toLower();
        for (const QString &excluded : excludedPaths) {
            if (lowerPath.startsWith(excluded)) {
                return true;
            }
        }
        return false;
    }
};

class NtfsUsnAdapter
{
public:
    bool buildInitialIndex(const FinderIndexOptions &options,
                           QHash<QString, FinderIndexedRecord> *records,
                           FinderIndexStats *stats,
                           QString *errorMessage,
                           Indexer *fallbackIndexer)
    {
        if (!records || !stats || !fallbackIndexer) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("NTFS adapter initialization parameters are invalid.");
            }
            return false;
        }

#ifndef Q_OS_WIN
        return fallbackIndexer->build(options, records, stats, errorMessage);
#else
        m_volumeStates.clear();

        const QStringList volumeRoots = collectVolumeRoots(options.roots);
        if (volumeRoots.isEmpty()) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("No valid NTFS volume roots found.");
            }
            return false;
        }

        for (const QString &volumeRoot : volumeRoots) {
            VolumeState state;
            state.volumeRoot = volumeRoot;
            QString localError;
            if (refreshVolumeState(state, &localError)) {
                m_volumeStates.insert(volumeRoot, state);
            }
        }

        if (m_volumeStates.isEmpty()) {
            // Keep product usable if permissions are insufficient.
            const bool fallbackOk = fallbackIndexer->build(options, records, stats, errorMessage);
            if (fallbackOk) {
                stats->backend = QStringLiteral("generic-fallback");
                setChannelStatus(stats, QStringLiteral("fallback"), QStringLiteral("usn-unavailable"));
            }
            return fallbackOk;
        }

        records->clear();
        const QStringList normalizedRoots = normalizeRoots(options.roots);
        const QStringList normalizedExcludes = normalizePaths(options.excludes);
        int collected = 0;

        for (auto it = m_volumeStates.constBegin(); it != m_volumeStates.constEnd(); ++it) {
            const VolumeState &state = it.value();
            for (auto nodeIt = state.nodes.constBegin(); nodeIt != state.nodes.constEnd(); ++nodeIt) {
                const UsnNode &node = nodeIt.value();
                if (node.isDirectory) {
                    continue;
                }
                const QString fullPath = buildPath(state, nodeIt.key());
                if (!shouldIncludePath(fullPath, normalizedRoots, normalizedExcludes)) {
                    continue;
                }

                QFileInfo info(fullPath);
                if (!info.exists() || !info.isFile()) {
                    continue;
                }
                if (!options.includeHidden && info.isHidden()) {
                    continue;
                }

                FinderIndexedRecord record = makeRecord(info);
                records->insert(record.entry.path, record);
                ++collected;
                if (options.maxFiles > 0 && collected >= options.maxFiles) {
                    break;
                }
            }
            if (options.maxFiles > 0 && collected >= options.maxFiles) {
                break;
            }
        }

        stats->totalFiles = records->size();
        stats->indexedRoots = normalizedRoots.size();
        stats->indexedAt = QDateTime::currentDateTime();
        stats->backend = QStringLiteral("ntfs-usn-journal");
        stats->usnParsedRecords = 0;
        stats->usnErrorCount = 0;
        setChannelStatus(stats, QStringLiteral("ready"), QStringLiteral("usn-journal"));
        return true;
#endif
    }

    bool establishPollingState(const FinderIndexOptions &options, QString *errorMessage)
    {
#ifndef Q_OS_WIN
        Q_UNUSED(options)
        Q_UNUSED(errorMessage)
        return true;
#else
        m_volumeStates.clear();
        const QStringList volumeRoots = collectVolumeRoots(options.roots);
        if (volumeRoots.isEmpty()) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("No valid NTFS volume roots.");
            }
            return false;
        }

        for (const QString &volumeRoot : volumeRoots) {
            VolumeState state;
            state.volumeRoot = volumeRoot;
            QString localError;
            if (refreshVolumeState(state, &localError)) {
                m_volumeStates.insert(volumeRoot, state);
            }
        }

        if (m_volumeStates.isEmpty()) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("USN polling state could not be established.");
            }
            return false;
        }
        return true;
#endif
    }

    bool applyIncrementalChanges(QHash<QString, FinderIndexedRecord> *records,
                                 const FinderIndexOptions &options,
                                 FinderIndexStats *stats,
                                 QString *errorMessage,
                                 bool *changed,
                                 Indexer *fallbackIndexer)
    {
        if (changed) {
            *changed = false;
        }
        if (!records || !stats || !fallbackIndexer) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("NTFS adapter incremental parameters are invalid.");
            }
            return false;
        }

#ifndef Q_OS_WIN
        Q_UNUSED(options)
        Q_UNUSED(errorMessage)
        return true;
#else
        if (m_volumeStates.isEmpty()) {
            return true;
        }

        const QStringList normalizedRoots = normalizeRoots(options.roots);
        const QStringList normalizedExcludes = normalizePaths(options.excludes);

        bool hasChanges = false;
        bool needFullResync = false;
        QString needFullResyncReason;

        for (auto it = m_volumeStates.begin(); it != m_volumeStates.end(); ++it) {
            VolumeState &state = it.value();

            USN_JOURNAL_DATA_V0 latestJournal;
            ZeroMemory(&latestJournal, sizeof(latestJournal));
            if (!queryJournalSnapshot(state, &latestJournal, errorMessage)) {
                needFullResync = true;
                needFullResyncReason = QStringLiteral("query-journal-failed");
                break;
            }
            if (static_cast<quint64>(latestJournal.UsnJournalID) != state.journalId) {
                needFullResync = true;
                needFullResyncReason = QStringLiteral("journal-id-changed");
                break;
            }
            state.journal = latestJournal;

            HANDLE volumeHandle = openVolumeHandle(state.volumeRoot);
            if (volumeHandle == INVALID_HANDLE_VALUE) {
                continue;
            }

            QByteArray buffer(1024 * 1024, 0);
            bool continueRead = true;
            while (continueRead) {
                READ_USN_JOURNAL_DATA_V0 readData;
                ZeroMemory(&readData, sizeof(readData));
                readData.StartUsn = static_cast<USN>(state.nextUsn);
                readData.ReasonMask = 0xFFFFFFFF;
                readData.ReturnOnlyOnClose = FALSE;
                readData.Timeout = 0;
                readData.BytesToWaitFor = 0;
                readData.UsnJournalID = state.journalId;

                DWORD bytesReturned = 0;
                const BOOL ok = DeviceIoControl(volumeHandle,
                                                FSCTL_READ_USN_JOURNAL,
                                                &readData,
                                                sizeof(readData),
                                                buffer.data(),
                                                static_cast<DWORD>(buffer.size()),
                                                &bytesReturned,
                                                nullptr);
                if (!ok) {
                    const DWORD error = GetLastError();
                    if (error == ERROR_HANDLE_EOF) {
                        break;
                    }
                    if (error == ERROR_JOURNAL_ENTRY_DELETED || error == ERROR_JOURNAL_DELETE_IN_PROGRESS || error == ERROR_INVALID_PARAMETER) {
                        needFullResync = true;
                        needFullResyncReason = QStringLiteral("journal-reset");
                    }
                    break;
                }

                if (bytesReturned <= sizeof(USN)) {
                    break;
                }

                const USN nextUsn = *reinterpret_cast<const USN *>(buffer.constData());
                state.nextUsn = static_cast<quint64>(nextUsn);

                const char *cursor = buffer.constData() + sizeof(USN);
                const char *end = buffer.constData() + bytesReturned;
                QSet<QString> directoriesToRescan;
                while (cursor < end) {
                    const USN_RECORD *baseRecord = reinterpret_cast<const USN_RECORD *>(cursor);
                    if (baseRecord->RecordLength == 0) {
                        break;
                    }

                    UsnEvent event;
                    if (decodeUsnEvent(baseRecord, &event)) {
                        ++stats->usnParsedRecords;
                        const QString oldPath = buildPath(state, event.frn);

                        if (event.reason & USN_REASON_FILE_DELETE) {
                            if (!oldPath.isEmpty()) {
                                if (event.isDirectory) {
                                    removeRecordsUnderPath(records, oldPath);
                                } else {
                                    records->remove(QDir::cleanPath(oldPath));
                                }
                            }
                            state.nodes.remove(event.frn);
                            hasChanges = true;
                        } else {
                            UsnNode node;
                            node.frn = event.frn;
                            node.parentFrn = event.parentFrn;
                            node.name = event.name;
                            node.isDirectory = event.isDirectory;
                            state.nodes.insert(event.frn, node);

                            const QString newPath = buildPath(state, event.frn);
                            if (!oldPath.isEmpty() && oldPath != newPath) {
                                if (event.isDirectory) {
                                    removeRecordsUnderPath(records, oldPath);
                                } else {
                                    records->remove(QDir::cleanPath(oldPath));
                                }
                            }

                            if (event.isDirectory) {
                                if (!newPath.isEmpty()) {
                                    directoriesToRescan.insert(QDir::cleanPath(newPath));
                                }
                            } else if (shouldIncludePath(newPath, normalizedRoots, normalizedExcludes)) {
                                QFileInfo info(newPath);
                                if (info.exists() && info.isFile() && (options.includeHidden || !info.isHidden())) {
                                    records->insert(QDir::cleanPath(newPath), makeRecord(info));
                                } else {
                                    records->remove(QDir::cleanPath(newPath));
                                }
                            } else if (!newPath.isEmpty()) {
                                records->remove(QDir::cleanPath(newPath));
                            }
                            hasChanges = true;
                        }
                    }
                    else {
                        ++stats->usnErrorCount;
                    }

                    cursor += baseRecord->RecordLength;
                }

                for (const QString &dirPath : directoriesToRescan) {
                    if (!dirPath.isEmpty() && QDir(dirPath).exists()) {
                        fallbackIndexer->rescanDirectory(dirPath, options, records);
                    }
                }

                if (bytesReturned < static_cast<DWORD>(buffer.size())) {
                    continueRead = false;
                }
            }

            CloseHandle(volumeHandle);
            if (needFullResync) {
                break;
            }
        }

        if (needFullResync) {
            if (!fallbackIndexer->build(options, records, stats, errorMessage)) {
                ++stats->usnErrorCount;
                setChannelStatus(stats, QStringLiteral("error"), QStringLiteral("incremental-build-failed"));
                return false;
            }
            stats->backend = QStringLiteral("ntfs-usn-resync-fallback");
            setChannelStatus(stats,
                             QStringLiteral("resync"),
                             needFullResyncReason.isEmpty() ? QStringLiteral("unknown") : needFullResyncReason);
            for (auto it = m_volumeStates.begin(); it != m_volumeStates.end(); ++it) {
                VolumeState refreshed = it.value();
                refreshVolumeState(refreshed, nullptr);
                it.value() = refreshed;
            }
            if (changed) {
                *changed = true;
            }
            return true;
        }

        if (hasChanges) {
            stats->totalFiles = records->size();
            stats->indexedAt = QDateTime::currentDateTime();
            stats->backend = QStringLiteral("ntfs-usn-journal");
            setChannelStatus(stats, QStringLiteral("healthy"), QStringLiteral("incremental-applied"));
            if (changed) {
                *changed = true;
            }
        }
        return true;
#endif
    }

private:
#ifdef Q_OS_WIN
    typedef NtfsUsnUtils::Node UsnNode;

    struct VolumeState
    {
        QString volumeRoot;
        quint64 journalId = 0;
        quint64 nextUsn = 0;
        USN_JOURNAL_DATA_V0 journal;
        QHash<quint64, UsnNode> nodes;
    };

    struct UsnEvent
    {
        quint64 frn = 0;
        quint64 parentFrn = 0;
        QString name;
        bool isDirectory = false;
        DWORD reason = 0;
    };

    QHash<QString, VolumeState> m_volumeStates;

    bool decodeUsnEvent(const USN_RECORD *baseRecord, UsnEvent *event) const
    {
        if (!baseRecord || !event) {
            return false;
        }
        const NtfsUsnUtils::DecodedEvent decoded = NtfsUsnUtils::decodeRecord(baseRecord);
        if (!decoded.valid) {
            return false;
        }
        event->frn = decoded.frn;
        event->parentFrn = decoded.parentFrn;
        event->name = decoded.name;
        event->isDirectory = decoded.isDirectory;
        event->reason = decoded.reason;
        return true;
    }

    QStringList normalizeRoots(QStringList roots) const
    {
        if (roots.isEmpty()) {
            roots = defaultSearchRoots();
        }
        QStringList normalized;
        for (const QString &root : roots) {
            const QString clean = QDir::cleanPath(root);
            if (!clean.isEmpty()) {
                normalized << clean;
            }
        }
        normalized.removeDuplicates();
        return normalized;
    }

    QStringList normalizePaths(const QStringList &paths) const
    {
        QStringList normalized;
        for (const QString &path : paths) {
            const QString clean = QDir::cleanPath(path);
            if (!clean.isEmpty()) {
                normalized << clean.toLower();
            }
        }
        normalized.removeDuplicates();
        return normalized;
    }

    bool hasPrefixPath(const QString &path, const QString &prefix) const
    {
        if (path.compare(prefix, Qt::CaseInsensitive) == 0) {
            return true;
        }
        const QString prefixed = prefix.endsWith('/') ? prefix : prefix + '/';
        return path.startsWith(prefixed, Qt::CaseInsensitive);
    }

    bool shouldIncludePath(const QString &path,
                           const QStringList &normalizedRoots,
                           const QStringList &normalizedExcludes) const
    {
        const QString clean = QDir::cleanPath(path);
        if (clean.isEmpty()) {
            return false;
        }

        bool inRoots = normalizedRoots.isEmpty();
        for (const QString &root : normalizedRoots) {
            if (hasPrefixPath(clean, root)) {
                inRoots = true;
                break;
            }
        }
        if (!inRoots) {
            return false;
        }

        const QString lower = clean.toLower();
        for (const QString &excluded : normalizedExcludes) {
            if (lower.startsWith(excluded)) {
                return false;
            }
        }
        return true;
    }

    void removeRecordsUnderPath(QHash<QString, FinderIndexedRecord> *records, const QString &basePath) const
    {
        NtfsUsnUtils::removeRecordsUnderPath(records, basePath);
    }

    FinderIndexedRecord makeRecord(const QFileInfo &info) const
    {
        FinderIndexedRecord record;
        record.entry.name = info.fileName();
        record.entry.path = QDir::cleanPath(info.absoluteFilePath());
        record.entry.sizeBytes = info.size();
        record.entry.lastModified = info.lastModified();
        record.nameLower = record.entry.name.toLower();
        record.pathLower = record.entry.path.toLower();
        record.searchableText = buildSearchableText(record);
        return record;
    }

    QStringList collectVolumeRoots(QStringList roots) const
    {
        if (roots.isEmpty()) {
            roots = defaultSearchRoots();
        }

        QStringList volumeRoots;
        for (const QString &root : roots) {
            QStorageInfo storage(root);
            if (!storage.isValid() || !storage.isReady()) {
                continue;
            }
            const QString fsType = QString::fromLatin1(storage.fileSystemType());
            if (fsType.compare(QStringLiteral("ntfs"), Qt::CaseInsensitive) != 0) {
                continue;
            }

            const QString volumeRoot = QDir::cleanPath(storage.rootPath());
            if (!volumeRoot.isEmpty()) {
                volumeRoots << volumeRoot;
            }
        }
        volumeRoots.removeDuplicates();
        return volumeRoots;
    }

    HANDLE openVolumeHandle(const QString &volumeRoot) const
    {
        const QString clean = QDir::cleanPath(volumeRoot);
        if (clean.size() < 2 || clean.at(1) != QChar(':')) {
            return INVALID_HANDLE_VALUE;
        }

        const QString drive = clean.left(1).toUpper();
        const QString devicePath = QStringLiteral("\\\\.\\%1:").arg(drive);
        return CreateFileW(reinterpret_cast<LPCWSTR>(devicePath.utf16()),
                           GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr,
                           OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    }

    bool queryJournalSnapshot(const VolumeState &state, USN_JOURNAL_DATA_V0 *journal, QString *errorMessage) const
    {
        if (!journal) {
            return false;
        }

        HANDLE volumeHandle = openVolumeHandle(state.volumeRoot);
        if (volumeHandle == INVALID_HANDLE_VALUE) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Open NTFS volume failed: %1").arg(state.volumeRoot);
            }
            return false;
        }

        DWORD bytesReturned = 0;
        USN_JOURNAL_DATA_V0 queriedJournal;
        ZeroMemory(&queriedJournal, sizeof(queriedJournal));
        const BOOL ok = DeviceIoControl(volumeHandle,
                                        FSCTL_QUERY_USN_JOURNAL,
                                        nullptr,
                                        0,
                                        &queriedJournal,
                                        sizeof(queriedJournal),
                                        &bytesReturned,
                                        nullptr);
        CloseHandle(volumeHandle);

        if (!ok) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Query USN journal failed on %1").arg(state.volumeRoot);
            }
            return false;
        }

        *journal = queriedJournal;
        return true;
    }

    bool refreshVolumeState(VolumeState &state, QString *errorMessage)
    {
        HANDLE volumeHandle = openVolumeHandle(state.volumeRoot);
        if (volumeHandle == INVALID_HANDLE_VALUE) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Open NTFS volume failed: %1").arg(state.volumeRoot);
            }
            return false;
        }

        DWORD bytesReturned = 0;
        USN_JOURNAL_DATA_V0 journal;
        ZeroMemory(&journal, sizeof(journal));
        const BOOL queryOk = DeviceIoControl(volumeHandle,
                                             FSCTL_QUERY_USN_JOURNAL,
                                             nullptr,
                                             0,
                                             &journal,
                                             sizeof(journal),
                                             &bytesReturned,
                                             nullptr);
        if (!queryOk) {
            CloseHandle(volumeHandle);
            if (errorMessage) {
                *errorMessage = QStringLiteral("Query USN journal failed on %1").arg(state.volumeRoot);
            }
            return false;
        }

        state.nodes.clear();

        MFT_ENUM_DATA_V0 enumData;
        ZeroMemory(&enumData, sizeof(enumData));
        enumData.StartFileReferenceNumber = 0;
        enumData.LowUsn = 0;
        enumData.HighUsn = journal.NextUsn;

        QByteArray buffer(1024 * 1024, 0);
        bool done = false;
        while (!done) {
            const BOOL enumOk = DeviceIoControl(volumeHandle,
                                                FSCTL_ENUM_USN_DATA,
                                                &enumData,
                                                sizeof(enumData),
                                                buffer.data(),
                                                static_cast<DWORD>(buffer.size()),
                                                &bytesReturned,
                                                nullptr);
            if (!enumOk) {
                const DWORD error = GetLastError();
                if (error == ERROR_HANDLE_EOF) {
                    break;
                }
                CloseHandle(volumeHandle);
                if (errorMessage) {
                    *errorMessage = QStringLiteral("Enumerate USN data failed on %1").arg(state.volumeRoot);
                }
                return false;
            }

            if (bytesReturned <= sizeof(USN)) {
                break;
            }

            enumData.StartFileReferenceNumber = *reinterpret_cast<const DWORDLONG *>(buffer.constData());

            const char *cursor = buffer.constData() + sizeof(USN);
            const char *end = buffer.constData() + bytesReturned;
            while (cursor < end) {
                const USN_RECORD *baseRecord = reinterpret_cast<const USN_RECORD *>(cursor);
                if (baseRecord->RecordLength == 0) {
                    done = true;
                    break;
                }

                UsnEvent event;
                if (decodeUsnEvent(baseRecord, &event)) {
                    UsnNode node;
                    node.frn = event.frn;
                    node.parentFrn = event.parentFrn;
                    node.name = event.name;
                    node.isDirectory = event.isDirectory;
                    state.nodes.insert(node.frn, node);
                }

                cursor += baseRecord->RecordLength;
            }
        }

        state.journal = journal;
        state.journalId = static_cast<quint64>(journal.UsnJournalID);
        state.nextUsn = static_cast<quint64>(journal.NextUsn);

        CloseHandle(volumeHandle);
        return true;
    }

    QString buildPath(const VolumeState &state, quint64 frn) const
    {
        return NtfsUsnUtils::buildPath(state.volumeRoot, state.nodes, frn);
    }
#endif
};

class ResultRanker
{
public:
    QList<FinderSearchResult> rank(const QVector<FinderIndexedRecord> &records, QVector<int> indexes, const FinderSearchRequest &request) const
    {
        QList<FinderSearchResult> results;
        if (indexes.isEmpty()) {
            return results;
        }

        const QString keyword = request.caseSensitivity == Qt::CaseInsensitive
                ? request.keyword.toLower()
                : request.keyword;

        struct SortRow
        {
            int recordIndex = 0;
            int score = 0;
            int nameLen = 0;
            QString path;
        };

        const int limit = request.limit > 0 ? request.limit : indexes.size();
        const auto better = [](const SortRow &a, const SortRow &b) {
            if (a.score != b.score) {
                return a.score > b.score;
            }
            if (a.nameLen != b.nameLen) {
                return a.nameLen < b.nameLen;
            }
            return a.path < b.path;
        };

        QVector<SortRow> rows;
        if (limit > 0 && limit < indexes.size()) {
            // Top-K heap: keep the current K best rows; heap top is the worst among them.
            std::priority_queue<SortRow, std::vector<SortRow>, decltype(better)> topK(better);
            for (int recordIndex : indexes) {
                const FinderIndexedRecord &rec = records.at(recordIndex);
                SortRow row;
                row.recordIndex = recordIndex;
                row.score = score(rec, keyword, request.fileNamesOnly);
                row.nameLen = rec.entry.name.length();
                row.path = rec.entry.path;

                if (topK.size() < static_cast<size_t>(limit)) {
                    topK.push(std::move(row));
                    continue;
                }
                if (better(row, topK.top())) {
                    topK.pop();
                    topK.push(std::move(row));
                }
            }
            rows.reserve(static_cast<int>(topK.size()));
            while (!topK.empty()) {
                rows.push_back(std::move(topK.top()));
                topK.pop();
            }
            std::sort(rows.begin(), rows.end(), better);
        } else {
            rows.reserve(indexes.size());
            for (int recordIndex : indexes) {
                const FinderIndexedRecord &rec = records.at(recordIndex);
                SortRow row;
                row.recordIndex = recordIndex;
                row.score = score(rec, keyword, request.fileNamesOnly);
                row.nameLen = rec.entry.name.length();
                row.path = rec.entry.path;
                rows.push_back(std::move(row));
            }
            std::sort(rows.begin(), rows.end(), better);
        }

        const int size = qMin(limit, rows.size());
        results.reserve(size);
        for (int i = 0; i < size; ++i) {
            FinderSearchResult item;
            item.entry = records.at(rows.at(i).recordIndex).entry;
            item.score = rows.at(i).score;
            results.push_back(std::move(item));
        }
        return results;
    }

private:
    int score(const FinderIndexedRecord &record, const QString &keyword, bool fileNamesOnly) const
    {
        if (keyword.isEmpty()) {
            return 10;
        }

        const QString lowerName = record.entry.name.toLower();
        const QString lowerPath = record.entry.path.toLower();

        if (lowerName == keyword) {
            return 1000;
        }
        if (lowerName.startsWith(keyword)) {
            return 600;
        }
        if (lowerName.contains(keyword)) {
            return 400;
        }
        if (!fileNamesOnly && lowerPath.contains(keyword)) {
            return 200;
        }
        return 0;
    }
};

class QueryCache
{
public:
    bool tryGet(const QString &key, QList<FinderSearchResult> *results) const
    {
        if (!results || !m_cache.contains(key)) {
            return false;
        }
        *results = m_cache.value(key);
        return true;
    }

    void put(const QString &key, const QList<FinderSearchResult> &results)
    {
        static const int kMaxCacheEntries = 64;
        if (!m_order.contains(key)) {
            m_order.push_back(key);
        }
        m_cache.insert(key, results);
        while (m_order.size() > kMaxCacheEntries) {
            const QString oldest = m_order.takeFirst();
            m_cache.remove(oldest);
        }
    }

    void clear()
    {
        m_cache.clear();
        m_order.clear();
    }

private:
    QHash<QString, QList<FinderSearchResult>> m_cache;
    QStringList m_order;
};

QStringList normalizePathListForCompare(QStringList paths)
{
    for (QString &path : paths) {
        path = QDir::cleanPath(path).toLower();
    }
    paths.removeDuplicates();
    std::sort(paths.begin(), paths.end());
    return paths;
}

bool persistentNeedsRebuildImpl(const FinderIndexBuildOutcome &decoded, const FinderIndexOptions &requestedDesired)
{
    const bool rootMismatch = normalizePathListForCompare(decoded.lastIndexOptions.roots)
            != normalizePathListForCompare(requestedDesired.roots);
    const bool excludeMismatch = normalizePathListForCompare(decoded.lastIndexOptions.excludes)
            != normalizePathListForCompare(requestedDesired.excludes);
    if (rootMismatch || excludeMismatch) {
        return true;
    }
    if (decoded.lastIndexOptions.maxFiles != requestedDesired.maxFiles
        || decoded.lastIndexOptions.backend != requestedDesired.backend
        || decoded.lastIndexOptions.includeHidden != requestedDesired.includeHidden) {
        return true;
    }
    return decoded.stats.totalFiles <= 0;
}

FinderIndexBuildOutcome decodePersistedIndexImpl(const QString &path,
                                                 QString *errorMessage,
                                                 int maxRecordsToLoad)
{
    FinderIndexBuildOutcome out;
    QFile file(path);
    if (!file.exists()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Persisted index not found.");
        }
        out.ok = false;
        return out;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Cannot open persisted index file.");
        }
        out.ok = false;
        return out;
    }

    QDataStream in(&file);
    quint32 version = 0;
    quint32 recordCount = 0;
    QStringList roots;
    QStringList excludes;
    bool includeHidden = false;
    int maxFiles = -1;
    qint32 backendValue = static_cast<qint32>(FinderIndexOptions::Backend::Auto);
    qint64 indexedAtMs = 0;
    QString channelState;
    QString channelDetail;
    qint64 channelUpdatedAtMs = 0;
    qint32 usnParsedRecords = 0;
    qint32 usnErrorCount = 0;

    in >> version >> recordCount >> roots >> excludes >> includeHidden >> maxFiles;
    if (version >= 2) {
        in >> backendValue;
    }
    in >> indexedAtMs;
    if (version >= 3) {
        in >> channelState >> channelDetail >> channelUpdatedAtMs >> usnParsedRecords >> usnErrorCount;
    }
    if (version != 1 && version != 2 && version != 3) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Unsupported persisted index version.");
        }
        out.ok = false;
        return out;
    }

    const bool previewMode = maxRecordsToLoad > 0;
    QHash<QString, FinderIndexedRecord> loaded;
    if (!previewMode) {
        loaded.reserve(static_cast<int>(recordCount));
    }
    QVector<FinderIndexedRecord> linear;
    QVector<int> indexes;
    const int reserveCount = previewMode ? qMin(static_cast<int>(recordCount), maxRecordsToLoad)
                                         : static_cast<int>(recordCount);
    linear.reserve(reserveCount);
    indexes.reserve(reserveCount);
    int loadedCount = 0;
    for (quint32 i = 0; i < recordCount; ++i) {
        if (previewMode && loadedCount >= maxRecordsToLoad) {
            break;
        }
        FinderIndexedRecord record;
        qint64 modifiedMs = 0;
        in >> record.entry.name >> record.entry.path >> record.entry.sizeBytes >> modifiedMs;
        record.entry.lastModified = QDateTime::fromMSecsSinceEpoch(modifiedMs);
        if (!previewMode) {
            loaded.insert(record.entry.path, record);
        }
        linear.push_back(record);
        indexes.push_back(loadedCount);
        ++loadedCount;
    }

    if (in.status() != QDataStream::Ok) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Persisted index decode failed.");
        }
        out.ok = false;
        return out;
    }

    out.recordsByPath = std::move(loaded);
    out.recordsLinear = std::move(linear);
    out.allRecordIndexes = std::move(indexes);
    out.nameInverted.clear();
    out.pathInverted.clear();
    out.lastIndexOptions.roots = roots;
    out.lastIndexOptions.excludes = excludes;
    out.lastIndexOptions.includeHidden = includeHidden;
    out.lastIndexOptions.maxFiles = maxFiles;
    out.lastIndexOptions.backend = static_cast<FinderIndexOptions::Backend>(backendValue);
    out.resolvedBackend = resolveBackend(out.lastIndexOptions);
    out.stats.totalFiles = loadedCount;
    out.stats.indexedRoots = roots.size();
    out.stats.indexedAt = QDateTime::fromMSecsSinceEpoch(indexedAtMs);
    out.stats.backend = QStringLiteral("%1+persistent").arg(backendName(out.resolvedBackend));
    if (version >= 3) {
        out.stats.channelState = channelState.isEmpty() ? QStringLiteral("ready") : channelState;
        out.stats.channelDetail = channelDetail;
        out.stats.channelUpdatedAt = QDateTime::fromMSecsSinceEpoch(channelUpdatedAtMs);
        out.stats.usnParsedRecords = usnParsedRecords;
        out.stats.usnErrorCount = usnErrorCount;
    } else {
        setChannelStatus(&out.stats,
                         QStringLiteral("ready"),
                         out.resolvedBackend == FinderIndexOptions::Backend::NtfsUsn
                                 ? QStringLiteral("usn-persistent")
                                 : QStringLiteral("watcher-persistent"));
    }
    out.ntfsFellBackToGenericScan = false;
    out.loadedFromPersistence = true;
    out.partialData = previewMode;
    out.ok = true;
    return out;
}

FinderIndexBuildOutcome buildIndexDatasetImpl(const FinderIndexOptions &options, QString *errorMessage)
{
    FinderIndexBuildOutcome out;
    out.lastIndexOptions = options;
    out.resolvedBackend = resolveBackend(options);
    QString localError;

    Indexer indexer;
    NtfsUsnAdapter ntfsUsnAdapter;
    QHash<QString, FinderIndexedRecord> records;
    FinderIndexStats stats;

    bool ok = false;
    if (out.resolvedBackend == FinderIndexOptions::Backend::NtfsUsn) {
        ok = ntfsUsnAdapter.buildInitialIndex(options, &records, &stats, &localError, &indexer);
        out.ntfsFellBackToGenericScan = stats.backend.contains(QStringLiteral("fallback"), Qt::CaseInsensitive)
                || stats.backend.contains(QStringLiteral("generic-fallback"), Qt::CaseInsensitive);
    } else {
        ok = indexer.build(options, &records, &stats, &localError);
        out.ntfsFellBackToGenericScan = false;
        if (ok) {
            stats.backend = backendName(FinderIndexOptions::Backend::Generic);
            setChannelStatus(&stats, QStringLiteral("ready"), QStringLiteral("watcher"));
        }
    }

    if (!ok) {
        out.ok = false;
        out.error = localError;
        if (errorMessage) {
            *errorMessage = localError;
        }
        return out;
    }

    out.recordsByPath = std::move(records);
    buildDerivedIndexesFromRecords(out.recordsByPath, &out.recordsLinear, &out.nameInverted, &out.pathInverted, &out.allRecordIndexes);
    out.stats = stats;
    out.loadedFromPersistence = false;
    out.partialData = false;
    out.ok = true;
    return out;
}
}

FinderIndexBuildOutcome finderDecodePersistedIndex(const QString &filePath, QString *errorMessage)
{
    return decodePersistedIndexImpl(filePath, errorMessage, -1);
}

FinderIndexBuildOutcome finderDecodePersistedIndexPreview(const QString &filePath,
                                                          int maxRecords,
                                                          QString *errorMessage)
{
    return decodePersistedIndexImpl(filePath, errorMessage, qMax(1, maxRecords));
}

FinderIndexBuildOutcome finderBuildIndexDataset(const FinderIndexOptions &options, QString *errorMessage)
{
    return buildIndexDatasetImpl(options, errorMessage);
}

bool finderPersistentIndexNeedsRebuild(const FinderIndexBuildOutcome &decoded,
                                       const FinderIndexOptions &requestedDesired)
{
    return persistentNeedsRebuildImpl(decoded, requestedDesired);
}

struct SearchIndexSnapshot
{
    QVector<FinderIndexedRecord> recordsLinear;
    QVector<int> allRecordIndexes;
    QHash<QString, QVector<int>> nameInverted;
    QHash<QString, QVector<int>> pathInverted;
};

struct DeferredInvertedBuildResult
{
    QHash<QString, QVector<int>> nameInverted;
    QHash<QString, QVector<int>> pathInverted;
    bool ok = false;
};

struct Libfinder::Impl
{
    QHash<QString, FinderIndexedRecord> recordsByPath;
    QVector<FinderIndexedRecord> recordsLinear;
    QHash<QString, QVector<int>> nameInverted;
    QHash<QString, QVector<int>> pathInverted;
    QVector<int> allRecordIndexes;
    FinderIndexStats stats;
    FinderIndexOptions lastIndexOptions;
    FinderIndexOptions::Backend activeBackend = FinderIndexOptions::Backend::Generic;
    QString persistencePath;
    Indexer indexer;
    NtfsUsnAdapter ntfsUsnAdapter;
    ResultRanker ranker;
    QueryCache cache;
    QFileSystemWatcher *watcher = nullptr;
    QTimer *watchDebounceTimer = nullptr;
    QTimer *usnPollTimer = nullptr;
    QFutureWatcher<QStringList> *watchDirectoryBuildWatcher = nullptr;
    QTimer *persistDebounceTimer = nullptr;
    QFutureWatcher<bool> *persistWriteWatcher = nullptr;
    QTimer *deferredInvertedBuildTimer = nullptr;
    QFutureWatcher<DeferredInvertedBuildResult> *deferredInvertedBuildWatcher = nullptr;
    bool deferredInvertedBuildPending = false;
    quint64 deferredInvertedBuildRevision = 0;
    quint64 runningDeferredInvertedBuildRevision = 0;
    int deferredInvertedBuildDelayMs = 1500;
    bool persistDirty = false;
    bool persistCoalesceAfterSave = false;
    int persistDebounceMs = 3000;
    bool persistAsyncWriteEnabled = true;
    QSet<QString> pendingChangedDirectories;
    FinderIndexOptions pendingWatchOptions;
    bool hasPendingWatchOptions = false;
    int watchBuildRevision = 0;
    int runningWatchBuildRevision = 0;
    QReadWriteLock lock;
    mutable QMutex searchSnapshotMutex;
    std::shared_ptr<const SearchIndexSnapshot> searchSnapshot;

    void refreshSearchSnapshot()
    {
        auto snap = std::make_shared<SearchIndexSnapshot>();
        snap->recordsLinear = recordsLinear;
        snap->allRecordIndexes = allRecordIndexes;
        snap->nameInverted = nameInverted;
        snap->pathInverted = pathInverted;
        QMutexLocker locker(&searchSnapshotMutex);
        searchSnapshot = std::move(snap);
    }

    void clearWatchedDirectories()
    {
        if (!watcher) {
            return;
        }
        const QStringList dirs = watcher->directories();
        if (!dirs.isEmpty()) {
            watcher->removePaths(dirs);
        }
    }

    void startPendingWatchDirectoryRefresh()
    {
        if (!watchDirectoryBuildWatcher || watchDirectoryBuildWatcher->isRunning() || !hasPendingWatchOptions) {
            return;
        }

        hasPendingWatchOptions = false;
        const FinderIndexOptions options = pendingWatchOptions;
        runningWatchBuildRevision = watchBuildRevision;
        const QFuture<QStringList> future = QtConcurrent::run([options]() {
            return enumerateWatchDirectories(options);
        });
        watchDirectoryBuildWatcher->setFuture(future);
    }

    void requestWatchDirectoryRefresh(const FinderIndexOptions &options)
    {
        pendingWatchOptions = options;
        hasPendingWatchOptions = true;
        ++watchBuildRevision;
        startPendingWatchDirectoryRefresh();
    }

    void cancelWatchDirectoryRefresh()
    {
        hasPendingWatchOptions = false;
        ++watchBuildRevision;
    }

    void onWatchDirectoryRefreshFinished()
    {
        if (!watchDirectoryBuildWatcher) {
            return;
        }

        const QStringList watchDirs = watchDirectoryBuildWatcher->result();
        const int finishedRevision = runningWatchBuildRevision;
        bool shouldApply = false;
        {
            QReadLocker locker(&lock);
            shouldApply = activeBackend == FinderIndexOptions::Backend::Generic
                    && finishedRevision == watchBuildRevision;
        }
        if (shouldApply) {
            clearWatchedDirectories();
            if (!watchDirs.isEmpty()) {
                watcher->addPaths(watchDirs);
            }
        }

        if (hasPendingWatchOptions) {
            startPendingWatchDirectoryRefresh();
        }
    }

    void rebuildDerivedIndexes()
    {
        if (deferredInvertedBuildTimer) {
            deferredInvertedBuildTimer->stop();
        }
        deferredInvertedBuildPending = false;
        ++deferredInvertedBuildRevision;
        buildDerivedIndexesFromRecords(recordsByPath, &recordsLinear, &nameInverted, &pathInverted, &allRecordIndexes);
        refreshSearchSnapshot();
    }

    /// 调用方须已持有 `lock` 的读锁（或写锁）；仅访问本结构成员。
    bool serializePersistedToBuffer(QByteArray *out, QString *errorMessage) const;
};

bool Libfinder::Impl::serializePersistedToBuffer(QByteArray *out, QString *errorMessage) const
{
    if (!out) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Internal error: invalid serialization target.");
        }
        return false;
    }

    QFileInfo fileInfo(persistencePath);
    QDir targetDir = fileInfo.dir();
    if (!targetDir.exists() && !targetDir.mkpath(QStringLiteral("."))) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Cannot create index directory.");
        }
        return false;
    }

    QBuffer buffer(out);
    if (!buffer.open(QIODevice::WriteOnly)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Cannot allocate index buffer.");
        }
        return false;
    }

    QDataStream stream(&buffer);
    const quint32 version = 3;
    const quint32 recordCount = static_cast<quint32>(recordsByPath.size());
    const qint64 indexedAtMs = stats.indexedAt.toMSecsSinceEpoch();
    const qint32 backendValue = static_cast<qint32>(lastIndexOptions.backend);
    const qint64 channelUpdatedAtMs = stats.channelUpdatedAt.toMSecsSinceEpoch();

    stream << version
           << recordCount
           << lastIndexOptions.roots
           << lastIndexOptions.excludes
           << lastIndexOptions.includeHidden
           << lastIndexOptions.maxFiles
           << backendValue
           << indexedAtMs
           << stats.channelState
           << stats.channelDetail
           << channelUpdatedAtMs
           << stats.usnParsedRecords
           << stats.usnErrorCount;

    for (auto it = recordsByPath.constBegin(); it != recordsByPath.constEnd(); ++it) {
        const FinderIndexedRecord &record = it.value();
        stream << record.entry.name
               << record.entry.path
               << record.entry.sizeBytes
               << record.entry.lastModified.toMSecsSinceEpoch();
    }

    if (stream.status() != QDataStream::Ok) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Persisted index encode failed.");
        }
        return false;
    }
    return true;
}

static bool writeIndexBytesWithSaveFile(const QString &persistencePath, QByteArray bytes, QString *errorMessage)
{
    QSaveFile file(persistencePath);
    if (!file.open(QIODevice::WriteOnly)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Cannot write persisted index file.");
        }
        return false;
    }
    if (file.write(bytes) != bytes.size()) {
        file.cancelWriting();
        if (errorMessage) {
            *errorMessage = QStringLiteral("Persisted index write incomplete.");
        }
        return false;
    }
    if (!file.commit()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Persisted index commit failed.");
        }
        return false;
    }
    return true;
}

static int autoDeferredInvertedBuildDelayMs()
{
    const int cores = qMax(1, QThread::idealThreadCount());
    quint64 ramGiB = 8;
#ifdef Q_OS_WIN
    MEMORYSTATUSEX memStatus;
    ZeroMemory(&memStatus, sizeof(memStatus));
    memStatus.dwLength = sizeof(memStatus);
    if (GlobalMemoryStatusEx(&memStatus)) {
        ramGiB = static_cast<quint64>(memStatus.ullTotalPhys / (1024ull * 1024ull * 1024ull));
    }
#endif

    if (cores >= 16 && ramGiB >= 32) {
        return 700;
    }
    if (cores >= 8 && ramGiB >= 16) {
        return 1000;
    }
    if (cores >= 6 && ramGiB >= 8) {
        return 1300;
    }
    if (cores <= 2 || ramGiB < 4) {
        return 2200;
    }
    return 1600;
}

void Libfinder::waitForAsyncPersistFinished()
{
    if (!d->persistWriteWatcher) {
        return;
    }
    if (d->persistWriteWatcher->isRunning()) {
        d->persistWriteWatcher->waitForFinished();
    }
}

void Libfinder::schedulePersistedIndexSave()
{
    int debounceMs = 0;
    {
        QWriteLocker locker(&d->lock);
        d->persistDirty = true;
        debounceMs = d->persistDebounceMs;
    }
    if (debounceMs <= 0) {
        flushDebouncedPersistSave();
        return;
    }
    if (d->persistDebounceTimer) {
        d->persistDebounceTimer->setInterval(debounceMs);
        d->persistDebounceTimer->stop();
        d->persistDebounceTimer->start();
    }
}

void Libfinder::cancelDebouncedPersistedIndexSave()
{
    if (d->persistDebounceTimer) {
        d->persistDebounceTimer->stop();
    }
    waitForAsyncPersistFinished();
    QWriteLocker locker(&d->lock);
    d->persistDirty = false;
    d->persistCoalesceAfterSave = false;
}

void Libfinder::flushDebouncedPersistSave()
{
    if (!d->persistAsyncWriteEnabled) {
        QString err;
        if (savePersistedIndex(&err)) {
            QWriteLocker locker(&d->lock);
            d->persistDirty = false;
        }
        return;
    }
    startAsyncPersistWrite();
}

void Libfinder::startAsyncPersistWrite()
{
    if (d->persistWriteWatcher->isRunning()) {
        QWriteLocker locker(&d->lock);
        d->persistCoalesceAfterSave = true;
        return;
    }

    QByteArray blob;
    QString pathCopy;
    {
        QReadLocker locker(&d->lock);
        pathCopy = d->persistencePath;
        QString serErr;
        if (!d->serializePersistedToBuffer(&blob, &serErr)) {
            Q_UNUSED(serErr);
            return;
        }
    }

    QFuture<bool> future = QtConcurrent::run([pathCopy, blob]() mutable {
        QString writeErr;
        return writeIndexBytesWithSaveFile(pathCopy, std::move(blob), &writeErr);
    });
    d->persistWriteWatcher->setFuture(future);
}

Libfinder::Libfinder()
    : d(new Impl)
{
    QString basePath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (basePath.isEmpty()) {
        basePath = QDir::homePath() + QStringLiteral("/.findfast");
    }
    d->persistencePath = QDir::cleanPath(basePath + QStringLiteral("/findfast.index.bin"));
    d->refreshSearchSnapshot();

    d->watcher = new QFileSystemWatcher();
    d->watchDebounceTimer = new QTimer();
    d->watchDebounceTimer->setInterval(500);
    d->watchDebounceTimer->setSingleShot(true);
    d->usnPollTimer = new QTimer();
    d->usnPollTimer->setInterval(1000);
    d->usnPollTimer->setSingleShot(false);
    d->persistDebounceTimer = new QTimer();
    d->persistDebounceTimer->setInterval(d->persistDebounceMs);
    d->persistDebounceTimer->setSingleShot(true);
    d->persistWriteWatcher = new QFutureWatcher<bool>();
    d->deferredInvertedBuildDelayMs = autoDeferredInvertedBuildDelayMs();
    d->deferredInvertedBuildTimer = new QTimer();
    d->deferredInvertedBuildTimer->setInterval(d->deferredInvertedBuildDelayMs);
    d->deferredInvertedBuildTimer->setSingleShot(true);
    d->deferredInvertedBuildWatcher = new QFutureWatcher<DeferredInvertedBuildResult>();
    d->watchDirectoryBuildWatcher = new QFutureWatcher<QStringList>();

    QObject::connect(d->persistWriteWatcher, &QFutureWatcher<bool>::finished, [this]() {
        const bool ok = d->persistWriteWatcher->result();
        bool needReschedule = false;
        {
            QWriteLocker locker(&d->lock);
            if (ok) {
                d->persistDirty = false;
            }
            if (d->persistCoalesceAfterSave) {
                d->persistCoalesceAfterSave = false;
                needReschedule = true;
            }
        }
        if (needReschedule) {
            schedulePersistedIndexSave();
        }
    });

    QObject::connect(d->persistDebounceTimer, &QTimer::timeout, [this]() {
        flushDebouncedPersistSave();
    });

    QObject::connect(d->deferredInvertedBuildTimer, &QTimer::timeout, [this]() {
        QVector<FinderIndexedRecord> linearCopy;
        QVector<int> indexCopy;
        quint64 revision = 0;
        {
            QReadLocker locker(&d->lock);
            if (!d->deferredInvertedBuildPending || d->deferredInvertedBuildWatcher->isRunning()) {
                return;
            }
            linearCopy = d->recordsLinear;
            indexCopy = d->allRecordIndexes;
            revision = d->deferredInvertedBuildRevision;
        }
        if (linearCopy.isEmpty() || indexCopy.isEmpty()) {
            return;
        }
        {
            QWriteLocker locker(&d->lock);
            if (!d->deferredInvertedBuildPending
                || d->deferredInvertedBuildWatcher->isRunning()
                || revision != d->deferredInvertedBuildRevision) {
                return;
            }
            d->deferredInvertedBuildPending = false;
            d->runningDeferredInvertedBuildRevision = revision;
        }

        QFuture<DeferredInvertedBuildResult> future = QtConcurrent::run([linearCopy, indexCopy]() mutable {
            DeferredInvertedBuildResult result;
            buildInvertedIndexesFromLinear(linearCopy, indexCopy, &result.nameInverted, &result.pathInverted);
            result.ok = !result.nameInverted.isEmpty() || !result.pathInverted.isEmpty();
            return result;
        });
        d->deferredInvertedBuildWatcher->setFuture(future);
    });

    QObject::connect(d->deferredInvertedBuildWatcher, &QFutureWatcher<DeferredInvertedBuildResult>::finished, [this]() {
        const DeferredInvertedBuildResult built = d->deferredInvertedBuildWatcher->result();
        bool shouldRestart = false;
        {
            QWriteLocker locker(&d->lock);
            const bool fresh = d->runningDeferredInvertedBuildRevision != 0
                    && d->runningDeferredInvertedBuildRevision == d->deferredInvertedBuildRevision;
            if (fresh && built.ok) {
                d->nameInverted = built.nameInverted;
                d->pathInverted = built.pathInverted;
                d->refreshSearchSnapshot();
                d->cache.clear();
            }
            d->runningDeferredInvertedBuildRevision = 0;
            shouldRestart = d->deferredInvertedBuildPending;
        }
        if (shouldRestart && d->deferredInvertedBuildTimer) {
            d->deferredInvertedBuildTimer->stop();
            d->deferredInvertedBuildTimer->start();
        }
    });

    QObject::connect(d->watcher, &QFileSystemWatcher::directoryChanged, [this](const QString &path) {
        if (d->activeBackend != FinderIndexOptions::Backend::Generic) {
            return;
        }
        d->pendingChangedDirectories.insert(QDir::cleanPath(path));
        d->watchDebounceTimer->start();
    });
    QObject::connect(d->watchDebounceTimer, &QTimer::timeout, [this]() {
        if (d->activeBackend != FinderIndexOptions::Backend::Generic) {
            return;
        }
        {
            QWriteLocker locker(&d->lock);
            const QList<QString> changedDirs = d->pendingChangedDirectories.values();
            d->pendingChangedDirectories.clear();

            for (const QString &dir : changedDirs) {
                d->indexer.rescanDirectory(dir, d->lastIndexOptions, &d->recordsByPath);
            }

            d->rebuildDerivedIndexes();
            d->stats.totalFiles = d->recordsByPath.size();
            d->stats.indexedAt = QDateTime::currentDateTime();
            setChannelStatus(&d->stats, QStringLiteral("healthy"), QStringLiteral("watcher-incremental"));
            d->cache.clear();
        }
        const QStringList existingWatchDirs = d->watcher->directories();
        if (!existingWatchDirs.isEmpty()) {
            d->watcher->removePaths(existingWatchDirs);
        }
        const QStringList watchDirs = enumerateWatchDirectories(d->lastIndexOptions);
        if (!watchDirs.isEmpty()) {
            d->watcher->addPaths(watchDirs);
        }
        schedulePersistedIndexSave();
    });
    QObject::connect(d->watchDirectoryBuildWatcher, &QFutureWatcher<QStringList>::finished, [this]() {
        d->onWatchDirectoryRefreshFinished();
    });
    QObject::connect(d->usnPollTimer, &QTimer::timeout, [this]() {
        if (d->activeBackend != FinderIndexOptions::Backend::NtfsUsn) {
            return;
        }

        bool changed = false;
        QString error;
        {
            QWriteLocker locker(&d->lock);
            if (!d->ntfsUsnAdapter.applyIncrementalChanges(&d->recordsByPath,
                                                           d->lastIndexOptions,
                                                           &d->stats,
                                                           &error,
                                                           &changed,
                                                           &d->indexer)) {
                ++d->stats.usnErrorCount;
                setChannelStatus(&d->stats,
                                 QStringLiteral("error"),
                                 QStringLiteral("usn-poll:%1").arg(error.isEmpty() ? QStringLiteral("unknown") : error));
                return;
            }
            if (changed) {
                d->rebuildDerivedIndexes();
                d->stats.totalFiles = d->recordsByPath.size();
                d->stats.indexedAt = QDateTime::currentDateTime();
                d->cache.clear();
            }
        }

        if (changed) {
            schedulePersistedIndexSave();
        }
    });
}

Libfinder::~Libfinder()
{
    if (d->usnPollTimer) {
        d->usnPollTimer->stop();
    }
    if (d->persistDebounceTimer) {
        d->persistDebounceTimer->stop();
    }
    if (d->deferredInvertedBuildTimer) {
        d->deferredInvertedBuildTimer->stop();
    }
    if (d->watchDirectoryBuildWatcher && d->watchDirectoryBuildWatcher->isRunning()) {
        d->watchDirectoryBuildWatcher->waitForFinished();
    }
    if (d->deferredInvertedBuildWatcher && d->deferredInvertedBuildWatcher->isRunning()) {
        d->deferredInvertedBuildWatcher->waitForFinished();
    }
    waitForAsyncPersistFinished();
    bool dirtyFlush = false;
    {
        QReadLocker locker(&d->lock);
        dirtyFlush = d->persistDirty;
    }
    if (dirtyFlush) {
        QString err;
        if (savePersistedIndex(&err)) {
            QWriteLocker locker(&d->lock);
            d->persistDirty = false;
        }
    }
    delete d->watchDirectoryBuildWatcher;
    delete d->deferredInvertedBuildWatcher;
    delete d->deferredInvertedBuildTimer;
    delete d->persistWriteWatcher;
    delete d->persistDebounceTimer;
    delete d->watchDebounceTimer;
    delete d->usnPollTimer;
    delete d->watcher;
    delete d;
}

bool Libfinder::rebuildIndex(const FinderIndexOptions &options, QString *errorMessage)
{
    FinderIndexBuildOutcome bundle = finderBuildIndexDataset(options, errorMessage);
    if (!bundle.ok) {
        return false;
    }
    return commitIndexBuild(std::move(bundle), errorMessage);
}

namespace {

static const int kMaxInvertedPostingSpan = 50000;

static bool haystackContainsKey(const QString &haystack, const QString &needle, Qt::CaseSensitivity cs)
{
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
    return QStringView(haystack).contains(QStringView(needle), cs);
#else
    return haystack.contains(needle, cs);
#endif
}

static bool invertedTokenAllowed(const QString &token)
{
    return token.size() > 1;
}

static bool recordMatchesKeyword(const FinderIndexedRecord &record,
                                 const QString &keyword,
                                 const FinderSearchRequest &request)
{
    if (request.fileNamesOnly) {
        if (request.caseSensitivity == Qt::CaseInsensitive) {
            const QString nameHay = record.nameLower.isEmpty() ? record.entry.name.toLower() : record.nameLower;
            return haystackContainsKey(nameHay, keyword, Qt::CaseInsensitive);
        }
        return record.entry.name.contains(keyword, request.caseSensitivity);
    }
    if (request.caseSensitivity == Qt::CaseInsensitive) {
        if (!record.searchableText.isEmpty()) {
            return haystackContainsKey(record.searchableText, keyword, Qt::CaseInsensitive);
        }
        const QString nameLower = record.nameLower.isEmpty() ? record.entry.name.toLower() : record.nameLower;
        const QString pathLower = record.pathLower.isEmpty() ? record.entry.path.toLower() : record.pathLower;
        const QString haystack = QStringLiteral("%1 %2").arg(nameLower, pathLower);
        return haystackContainsKey(haystack, keyword, Qt::CaseInsensitive);
    }
    const QString haystack = QStringLiteral("%1 %2").arg(record.entry.name, record.entry.path);
    return haystackContainsKey(haystack, keyword, request.caseSensitivity);
}

static bool tryBuildInvertedCandidateIndexes(const SearchIndexSnapshot *snap,
                                             const QString &keyword,
                                             bool fileNamesOnly,
                                             bool hasPathLikeChars,
                                             QVector<int> *outCandidates,
                                             bool *outUsedInverted)
{
    *outUsedInverted = false;
    if (!snap || fileNamesOnly) {
        return false;
    }
    const QHash<QString, QVector<int>> *inv = hasPathLikeChars ? &snap->pathInverted : &snap->nameInverted;
    if (inv->isEmpty()) {
        return false;
    }
    const QStringList queryTokens = tokenizeName(keyword);
    if (queryTokens.isEmpty()) {
        return false;
    }
    struct TokenPostingRef
    {
        const QVector<int> *posting = nullptr;
    };
    QVector<TokenPostingRef> ordered;
    ordered.reserve(queryTokens.size());
    for (const QString &token : queryTokens) {
        if (!invertedTokenAllowed(token)) {
            return false;
        }
        const auto postingIt = inv->constFind(token);
        if (postingIt == inv->constEnd()) {
            return false;
        }
        TokenPostingRef ref{&postingIt.value()};
        ordered.push_back(ref);
    }
    std::sort(ordered.begin(), ordered.end(), [](const TokenPostingRef &a, const TokenPostingRef &b) {
        return a.posting->size() < b.posting->size();
    });
    if (!ordered.isEmpty() && ordered.first().posting->size() > kMaxInvertedPostingSpan) {
        return false;
    }
    QVector<int> merged = *ordered.first().posting;
    for (int ti = 1; ti < ordered.size(); ++ti) {
        const QVector<int> &posting = *ordered.at(ti).posting;
        QVector<int> intersection;
        intersection.reserve(qMin(merged.size(), posting.size()));
        int i = 0;
        int j = 0;
        while (i < merged.size() && j < posting.size()) {
            if (merged.at(i) == posting.at(j)) {
                intersection.push_back(merged.at(i));
                ++i;
                ++j;
            } else if (merged.at(i) < posting.at(j)) {
                ++i;
            } else {
                ++j;
            }
        }
        merged = std::move(intersection);
        if (merged.isEmpty()) {
            return false;
        }
    }
    if (merged.isEmpty()) {
        return false;
    }
    *outCandidates = std::move(merged);
    *outUsedInverted = true;
    return true;
}

static QVector<FinderIndexedRecord> sequentialScanMatches(const SearchIndexSnapshot &snap,
                                                         const QVector<int> &scanIndexes,
                                                         const QString &keyword,
                                                         const FinderSearchRequest &request,
                                                         std::atomic<bool> *cancelToken,
                                                         qint64 *matchedOrdinal,
                                                         const qint64 skipMatches,
                                                         const int maxBucket,
                                                         bool *hasMore,
                                                         bool *truncated)
{
    QVector<FinderIndexedRecord> bucket;
    bucket.reserve(qMin(maxBucket, 65536));
    qint64 ord = *matchedOrdinal;
    int tick = 0;
    for (int rawIndex : scanIndexes) {
        if (((++tick) & 511) == 0 && cancelToken && cancelToken->load()) {
            *truncated = true;
            break;
        }
        if (rawIndex < 0 || rawIndex >= snap.recordsLinear.size()) {
            continue;
        }
        const FinderIndexedRecord &record = snap.recordsLinear.at(rawIndex);
        if (!recordMatchesKeyword(record, keyword, request)) {
            continue;
        }
        if (ord++ < skipMatches) {
            continue;
        }
        if (bucket.size() >= maxBucket) {
            *hasMore = true;
            break;
        }
        bucket.push_back(record);
    }
    *matchedOrdinal = ord;
    return bucket;
}

static QVector<FinderIndexedRecord> parallelScanMatches(const SearchIndexSnapshot &snap,
                                                        const QVector<int> &scanIndexes,
                                                        const QString &keyword,
                                                        const FinderSearchRequest &request,
                                                        std::atomic<bool> *cancelToken,
                                                        qint64 *matchedOrdinalInOut,
                                                        const qint64 skipMatches,
                                                        const int maxBucket,
                                                        bool *hasMore,
                                                        bool *truncated)
{
    const int n = scanIndexes.size();
    if (n < 12000 || cancelToken) {
        return sequentialScanMatches(snap, scanIndexes, keyword, request, cancelToken,
                                     matchedOrdinalInOut, skipMatches, maxBucket, hasMore, truncated);
    }
    const int threads = qMax(1, qMin(QThread::idealThreadCount(), 8));
    const int chunk = qMax(512, (n + threads - 1) / threads);
    QVector<QPair<int, int>> ranges;
    for (int lo = 0; lo < n; lo += chunk) {
        ranges.push_back(qMakePair(lo, qMin(n, lo + chunk)));
    }

    QVector<QVector<FinderIndexedRecord>> parts;
    parts.resize(ranges.size());
    QVector<int> partIndexes;
    partIndexes.reserve(ranges.size());
    for (int i = 0; i < ranges.size(); ++i) {
        partIndexes.push_back(i);
    }

    QtConcurrent::blockingMap(partIndexes, [&](int partIndex) {
        const QPair<int, int> range = ranges.at(partIndex);
        QVector<FinderIndexedRecord> local;
        local.reserve(qMin(chunk, 4096));
        int tick = 0;
        for (int p = range.first; p < range.second; ++p) {
            if (((++tick) & 511) == 0 && cancelToken && cancelToken->load()) {
                break;
            }
            const int rawIndex = scanIndexes.at(p);
            if (rawIndex < 0 || rawIndex >= snap.recordsLinear.size()) {
                continue;
            }
            const FinderIndexedRecord &record = snap.recordsLinear.at(rawIndex);
            if (recordMatchesKeyword(record, keyword, request)) {
                local.push_back(record);
            }
        }
        parts[partIndex] = std::move(local);
    });

    QVector<FinderIndexedRecord> combined;
    int est = 0;
    for (const QVector<FinderIndexedRecord> &part : parts) {
        est += part.size();
    }
    combined.reserve(est);
    for (const QVector<FinderIndexedRecord> &part : parts) {
        combined += part;
    }

    qint64 ord = *matchedOrdinalInOut;
    QVector<FinderIndexedRecord> bucket;
    bucket.reserve(qMin(maxBucket, 65536));
    int tick2 = 0;
    for (const FinderIndexedRecord &rec : combined) {
        if (((++tick2) & 511) == 0 && cancelToken && cancelToken->load()) {
            *truncated = true;
            break;
        }
        if (ord++ < skipMatches) {
            continue;
        }
        if (bucket.size() >= maxBucket) {
            *hasMore = true;
            break;
        }
        bucket.push_back(rec);
    }
    *matchedOrdinalInOut = ord;
    return bucket;
}

} // namespace

FinderSearchOutcome Libfinder::search(const FinderSearchRequest &request, std::atomic<bool> *cancelToken)
{
    FinderSearchOutcome outcome;
    const QString cacheKey = QStringLiteral("%1|%2|%3|%4|%5|%6")
            .arg(request.keyword)
            .arg(static_cast<int>(request.caseSensitivity))
            .arg(request.limit)
            .arg(request.pageSize)
            .arg(request.pageIndex)
            .arg(request.fileNamesOnly ? 1 : 0);

    const bool emptyKeyword = request.keyword.trimmed().isEmpty();
    const bool useCache = !emptyKeyword && !cancelToken && request.pageIndex == 0;

    if (useCache) {
        QReadLocker locker(&d->lock);
        QList<FinderSearchResult> cachedResults;
        if (d->cache.tryGet(cacheKey, &cachedResults)) {
            outcome.results = cachedResults;
            return outcome;
        }
    }

    std::shared_ptr<const SearchIndexSnapshot> snap;
    {
        QMutexLocker locker(&d->searchSnapshotMutex);
        snap = d->searchSnapshot;
    }
    if (!snap) {
        return outcome;
    }

    const int maxBucket = (request.pageSize <= 0) ? (INT_MAX / 4) : request.pageSize;
    const qint64 pageSize64 = (request.pageSize <= 0)
            ? (std::numeric_limits<qint64>::max() / 4)
            : static_cast<qint64>(request.pageSize);
    const qint64 skipMatches = static_cast<qint64>(qMax(0, request.pageIndex)) * pageSize64;

    QVector<FinderIndexedRecord> bucket;
    bucket.reserve(qMin(maxBucket, 65536));

    qint64 matchedOrdinal = 0;

    const QString keyword = request.caseSensitivity == Qt::CaseInsensitive
            ? request.keyword.toLower()
            : request.keyword;

    if (emptyKeyword) {
        const int begin = static_cast<int>(qMin(skipMatches, static_cast<qint64>(snap->recordsLinear.size())));
        const int end = qMin(begin + maxBucket, snap->recordsLinear.size());
        if (end > begin) {
            bucket.reserve(end - begin);
            for (int i = begin; i < end; ++i) {
                bucket.push_back(snap->recordsLinear.at(i));
            }
        }
        outcome.hasMore = end < snap->recordsLinear.size();
    } else {
        const bool hasPathLikeChars = keyword.contains(QLatin1Char('/'))
                || keyword.contains(QLatin1Char('\\'))
                || keyword.contains(QLatin1Char(':'));

        QVector<int> candidateIndexes;
        bool useInvertedCandidates = false;
        if (!emptyKeyword && request.caseSensitivity == Qt::CaseInsensitive) {
            tryBuildInvertedCandidateIndexes(snap.get(), keyword, request.fileNamesOnly, hasPathLikeChars,
                                             &candidateIndexes, &useInvertedCandidates);
        }

        const QVector<int> &scanIndexes = useInvertedCandidates ? candidateIndexes : snap->allRecordIndexes;

        bool hasMore = false;
        bool truncated = false;
        bucket = parallelScanMatches(*snap, scanIndexes, keyword, request, cancelToken, &matchedOrdinal,
                                     skipMatches, maxBucket, &hasMore, &truncated);
        outcome.hasMore = hasMore;
        outcome.truncated = truncated;
    }

    if (emptyKeyword) {
        outcome.results.reserve(bucket.size());
        for (const FinderIndexedRecord &record : bucket) {
            FinderSearchResult item;
            item.entry = record.entry;
            item.score = 10;
            outcome.results.push_back(item);
        }
    } else {
        QVector<int> indexes;
        indexes.reserve(bucket.size());
        for (int i = 0; i < bucket.size(); ++i) {
            indexes.push_back(i);
        }
        FinderSearchRequest rankRequest = request;
        if (rankRequest.limit <= 0) {
            rankRequest.limit = bucket.size();
        }
        outcome.results = d->ranker.rank(bucket, indexes, rankRequest);
    }

    static const int kMaxCachedResults = 20000;
    if (useCache && !outcome.hasMore && outcome.results.size() <= kMaxCachedResults) {
        QWriteLocker locker(&d->lock);
        d->cache.put(cacheKey, outcome.results);
    }
    return outcome;
}

FinderSearchOutcome Libfinder::browseIndexed(int pageSize, int pageIndex, std::atomic<bool> *cancelToken)
{
    FinderSearchOutcome outcome;
    std::shared_ptr<const SearchIndexSnapshot> snap;
    {
        QMutexLocker locker(&d->searchSnapshotMutex);
        snap = d->searchSnapshot;
    }
    if (!snap) {
        return outcome;
    }

    const int effectivePageSize = (pageSize <= 0) ? (INT_MAX / 4) : pageSize;
    const qint64 pageSize64 = (pageSize <= 0)
            ? (std::numeric_limits<qint64>::max() / 4)
            : static_cast<qint64>(pageSize);
    const qint64 skipMatches = static_cast<qint64>(qMax(0, pageIndex)) * pageSize64;

    const int begin = static_cast<int>(qMin(skipMatches, static_cast<qint64>(snap->recordsLinear.size())));
    const int end = qMin(begin + effectivePageSize, snap->recordsLinear.size());

    const int resultSize = qMax(0, end - begin);
    outcome.results.reserve(resultSize);
    int tick = 0;
    for (int i = begin; i < end; ++i) {
        if (((++tick) & 1023) == 0 && cancelToken && cancelToken->load()) {
            outcome.truncated = true;
            return outcome;
        }
        FinderSearchResult item;
        item.entry = snap->recordsLinear.at(i).entry;
        item.score = 10;
        outcome.results.push_back(std::move(item));
    }
    outcome.hasMore = end < snap->recordsLinear.size();
    return outcome;
}

bool Libfinder::commitIndexBuild(FinderIndexBuildOutcome outcome, QString *errorMessage)
{
    if (!outcome.ok) {
        if (errorMessage) {
            *errorMessage = outcome.error;
        }
        return false;
    }

    const bool ntfsFallback = outcome.ntfsFellBackToGenericScan;
    const FinderIndexOptions::Backend active = outcome.resolvedBackend;
    const bool skipPersistenceWrite = outcome.loadedFromPersistence;
    const bool partialData = outcome.partialData;
    const FinderIndexOptions watchOptions = outcome.lastIndexOptions;
    bool scheduleDeferredInvertedBuild = false;

    {
        QWriteLocker locker(&d->lock);
        d->recordsByPath = std::move(outcome.recordsByPath);
        d->recordsLinear = std::move(outcome.recordsLinear);
        d->nameInverted = std::move(outcome.nameInverted);
        d->pathInverted = std::move(outcome.pathInverted);
        d->allRecordIndexes = std::move(outcome.allRecordIndexes);
        d->lastIndexOptions = outcome.lastIndexOptions;
        d->activeBackend = outcome.resolvedBackend;
        d->stats = outcome.stats;
        if (!partialData) {
            if (d->recordsLinear.size() != d->recordsByPath.size()
                || d->allRecordIndexes.size() != d->recordsByPath.size()) {
                d->rebuildDerivedIndexes();
            } else {
                d->refreshSearchSnapshot();
            }
        } else {
            d->refreshSearchSnapshot();
        }
        if (!partialData) {
            d->stats.totalFiles = d->recordsByPath.size();
        }
        d->cache.clear();

        if (!d->recordsLinear.isEmpty() && (d->nameInverted.isEmpty() || d->pathInverted.isEmpty())) {
            d->deferredInvertedBuildPending = true;
            ++d->deferredInvertedBuildRevision;
            scheduleDeferredInvertedBuild = true;
        } else {
            d->deferredInvertedBuildPending = false;
            ++d->deferredInvertedBuildRevision;
            if (d->deferredInvertedBuildTimer) {
                d->deferredInvertedBuildTimer->stop();
            }
        }
    }

    if (scheduleDeferredInvertedBuild && d->deferredInvertedBuildTimer) {
        d->deferredInvertedBuildTimer->stop();
        d->deferredInvertedBuildTimer->start();
    }

    if (partialData) {
        return true;
    }

#ifdef Q_OS_WIN
    if (active == FinderIndexOptions::Backend::NtfsUsn && !ntfsFallback) {
        QString pollErr;
        if (!d->ntfsUsnAdapter.establishPollingState(d->lastIndexOptions, &pollErr)) {
            QWriteLocker locker(&d->lock);
            setChannelStatus(&d->stats, QStringLiteral("warn"), pollErr);
        }
    }
#else
    Q_UNUSED(ntfsFallback);
#endif

    d->usnPollTimer->stop();
    if (active == FinderIndexOptions::Backend::Generic) {
        QTimer::singleShot(0, d->watcher, [this, watchOptions]() {
            {
                QReadLocker locker(&d->lock);
                if (d->activeBackend != FinderIndexOptions::Backend::Generic) {
                    return;
                }
            }
            d->requestWatchDirectoryRefresh(watchOptions);
        });
    } else {
        d->cancelWatchDirectoryRefresh();
        d->clearWatchedDirectories();
        d->usnPollTimer->start();
    }

    if (skipPersistenceWrite) {
        return true;
    }
    cancelDebouncedPersistedIndexSave();
    return savePersistedIndex(errorMessage);
}

bool Libfinder::loadPersistedIndex(QString *errorMessage)
{
    QString path;
    {
        QReadLocker locker(&d->lock);
        path = d->persistencePath;
    }
    FinderIndexBuildOutcome bundle = finderDecodePersistedIndex(path, errorMessage);
    if (!bundle.ok) {
        return false;
    }
    return commitIndexBuild(std::move(bundle), errorMessage);
}

bool Libfinder::savePersistedIndex(QString *errorMessage)
{
    waitForAsyncPersistFinished();
    QByteArray blob;
    QString pathCopy;
    {
        QReadLocker locker(&d->lock);
        pathCopy = d->persistencePath;
        if (!d->serializePersistedToBuffer(&blob, errorMessage)) {
            return false;
        }
    }
    return writeIndexBytesWithSaveFile(pathCopy, std::move(blob), errorMessage);
}

void Libfinder::setPersistenceSaveDebounceMs(int milliseconds)
{
    const int v = qBound(0, milliseconds, 600000);
    QWriteLocker locker(&d->lock);
    d->persistDebounceMs = v;
    if (d->persistDebounceTimer && v > 0) {
        d->persistDebounceTimer->setInterval(v);
    }
}

int Libfinder::persistenceSaveDebounceMs() const
{
    QReadLocker locker(&d->lock);
    return d->persistDebounceMs;
}

void Libfinder::setPersistenceAsyncWriteEnabled(bool enabled)
{
    QWriteLocker locker(&d->lock);
    d->persistAsyncWriteEnabled = enabled;
}

bool Libfinder::persistenceAsyncWriteEnabled() const
{
    QReadLocker locker(&d->lock);
    return d->persistAsyncWriteEnabled;
}

void Libfinder::setDeferredInvertedBuildDelayMs(int milliseconds)
{
    const int v = qBound(0, milliseconds, 600000);
    QWriteLocker locker(&d->lock);
    d->deferredInvertedBuildDelayMs = v;
    if (d->deferredInvertedBuildTimer) {
        d->deferredInvertedBuildTimer->setInterval(v);
    }
}

int Libfinder::deferredInvertedBuildDelayMs() const
{
    QReadLocker locker(&d->lock);
    return d->deferredInvertedBuildDelayMs;
}

void Libfinder::tuneDeferredInvertedBuildDelayForCurrentMachine()
{
    setDeferredInvertedBuildDelayMs(autoDeferredInvertedBuildDelayMs());
}

void Libfinder::setPersistenceFilePath(const QString &filePath)
{
    QWriteLocker locker(&d->lock);
    d->persistencePath = QDir::cleanPath(filePath);
}

QString Libfinder::persistenceFilePath() const
{
    QReadLocker locker(&d->lock);
    return d->persistencePath;
}

FinderIndexOptions Libfinder::indexOptions() const
{
    QReadLocker locker(&d->lock);
    return d->lastIndexOptions;
}

void Libfinder::clear()
{
    cancelDebouncedPersistedIndexSave();
    if (d->deferredInvertedBuildTimer) {
        d->deferredInvertedBuildTimer->stop();
    }
    QWriteLocker locker(&d->lock);
    d->deferredInvertedBuildPending = false;
    ++d->deferredInvertedBuildRevision;
    d->runningDeferredInvertedBuildRevision = 0;
    d->recordsByPath.clear();
    d->recordsLinear.clear();
    d->nameInverted.clear();
    d->pathInverted.clear();
    d->allRecordIndexes.clear();
    d->stats = FinderIndexStats();
    d->cache.clear();
    d->pendingChangedDirectories.clear();
    d->activeBackend = FinderIndexOptions::Backend::Generic;
    if (d->usnPollTimer) {
        d->usnPollTimer->stop();
    }
    if (d->watcher) {
        const QStringList dirs = d->watcher->directories();
        if (!dirs.isEmpty()) {
            d->watcher->removePaths(dirs);
        }
    }
    d->refreshSearchSnapshot();
}

FinderIndexStats Libfinder::stats() const
{
    QReadLocker locker(&d->lock);
    return d->stats;
}
