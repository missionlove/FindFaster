#include "libfinder.h"
#include "ntfsusn_utils.h"

#include <QtConcurrent/QtConcurrent>
#include <QByteArray>
#include <QCoreApplication>
#include <QDataStream>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QFuture>
#include <QFutureWatcher>
#include <QHash>
#include <QReadWriteLock>
#include <QSet>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QTimer>
#include <QVector>
#include <algorithm>
#include <atomic>
#include <climits>
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
                                    QVector<int> *allRecordIndexes)
{
    if (!recordsLinear || !nameInverted || !allRecordIndexes) {
        return;
    }

    recordsLinear->clear();
    nameInverted->clear();
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

        const QStringList tokens = tokenizeName(record.nameLower);
        for (const QString &token : tokens) {
            (*nameInverted)[token].push_back(index);
        }
        ++index;
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

        std::sort(indexes.begin(), indexes.end(), [&](int a, int b) {
            const FinderSearchResult left = makeResult(records.at(a), keyword);
            const FinderSearchResult right = makeResult(records.at(b), keyword);
            if (left.score != right.score) {
                return left.score > right.score;
            }
            if (left.entry.name.length() != right.entry.name.length()) {
                return left.entry.name.length() < right.entry.name.length();
            }
            return left.entry.path < right.entry.path;
        });

        const int limit = request.limit > 0 ? request.limit : indexes.size();
        const int size = std::min(limit, indexes.size());
        results.reserve(size);
        for (int i = 0; i < size; ++i) {
            results.push_back(makeResult(records.at(indexes.at(i)), keyword));
        }
        return results;
    }

private:
    FinderSearchResult makeResult(const FinderIndexedRecord &record, const QString &keyword) const
    {
        FinderSearchResult result;
        result.entry = record.entry;
        result.score = score(record, keyword);
        return result;
    }

    int score(const FinderIndexedRecord &record, const QString &keyword) const
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
        if (lowerPath.contains(keyword)) {
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
    buildDerivedIndexesFromRecords(out.recordsByPath, &out.recordsLinear, &out.nameInverted, &out.allRecordIndexes);
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

struct Libfinder::Impl
{
    QHash<QString, FinderIndexedRecord> recordsByPath;
    QVector<FinderIndexedRecord> recordsLinear;
    QHash<QString, QVector<int>> nameInverted;
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
    QSet<QString> pendingChangedDirectories;
    FinderIndexOptions pendingWatchOptions;
    bool hasPendingWatchOptions = false;
    int watchBuildRevision = 0;
    int runningWatchBuildRevision = 0;
    QReadWriteLock lock;

    void clearWatchedDirectories()
    {
        if (!watcher) {
            return;
        }
        watcher->removePaths(watcher->directories());
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
        buildDerivedIndexesFromRecords(recordsByPath, &recordsLinear, &nameInverted, &allRecordIndexes);
    }
};

Libfinder::Libfinder()
    : d(new Impl)
{
    QString basePath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (basePath.isEmpty()) {
        basePath = QDir::homePath() + QStringLiteral("/.findfast");
    }
    d->persistencePath = QDir::cleanPath(basePath + QStringLiteral("/findfast.index.bin"));

    d->watcher = new QFileSystemWatcher();
    d->watchDebounceTimer = new QTimer();
    d->watchDebounceTimer->setInterval(500);
    d->watchDebounceTimer->setSingleShot(true);
    d->usnPollTimer = new QTimer();
    d->usnPollTimer->setInterval(1000);
    d->usnPollTimer->setSingleShot(false);
    d->watchDirectoryBuildWatcher = new QFutureWatcher<QStringList>();

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
        d->watcher->removePaths(d->watcher->directories());
        const QStringList watchDirs = enumerateWatchDirectories(d->lastIndexOptions);
        if (!watchDirs.isEmpty()) {
            d->watcher->addPaths(watchDirs);
        }
        savePersistedIndex();
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
            savePersistedIndex();
        }
    });
}

Libfinder::~Libfinder()
{
    if (d->usnPollTimer) {
        d->usnPollTimer->stop();
    }
    if (d->watchDirectoryBuildWatcher && d->watchDirectoryBuildWatcher->isRunning()) {
        d->watchDirectoryBuildWatcher->waitForFinished();
    }
    delete d->watchDirectoryBuildWatcher;
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

FinderSearchOutcome Libfinder::search(const FinderSearchRequest &request, std::atomic<bool> *cancelToken)
{
    FinderSearchOutcome outcome;
    const QString cacheKey = QStringLiteral("%1|%2|%3|%4|%5")
            .arg(request.keyword)
            .arg(static_cast<int>(request.caseSensitivity))
            .arg(request.limit)
            .arg(request.pageSize)
            .arg(request.pageIndex);

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

    const int maxBucket = (request.pageSize <= 0) ? (INT_MAX / 4) : request.pageSize;
    const qint64 pageSize64 = (request.pageSize <= 0)
            ? (std::numeric_limits<qint64>::max() / 4)
            : static_cast<qint64>(request.pageSize);
    const qint64 skipMatches = static_cast<qint64>(qMax(0, request.pageIndex)) * pageSize64;

    QVector<FinderIndexedRecord> bucket;
    bucket.reserve(qMin(maxBucket, 65536));

    qint64 matchedOrdinal = 0;
    int iterTick = 0;

    {
        QReadLocker locker(&d->lock);
        const QString keyword = request.caseSensitivity == Qt::CaseInsensitive
                ? request.keyword.toLower()
                : request.keyword;

        if (emptyKeyword) {
            const int begin = static_cast<int>(qMin(skipMatches, static_cast<qint64>(d->recordsLinear.size())));
            const int end = qMin(begin + maxBucket, d->recordsLinear.size());
            if (end > begin) {
                bucket.reserve(end - begin);
                for (int i = begin; i < end; ++i) {
                    bucket.push_back(d->recordsLinear.at(i));
                }
            }
            outcome.hasMore = end < d->recordsLinear.size();
        } else {
        const bool hasPathLikeChars = keyword.contains(QLatin1Char('/'))
                || keyword.contains(QLatin1Char('\\'))
                || keyword.contains(QLatin1Char(':'));

        QVector<int> candidateIndexes;
        bool useInvertedCandidates = false;

        if (!emptyKeyword
            && request.caseSensitivity == Qt::CaseInsensitive
            && !hasPathLikeChars
            && !d->nameInverted.isEmpty()) {
            const QStringList queryTokens = tokenizeName(keyword);
            if (!queryTokens.isEmpty()) {
                bool canUse = true;
                QVector<int> merged;

                for (const QString &token : queryTokens) {
                    const auto postingIt = d->nameInverted.constFind(token);
                    if (postingIt == d->nameInverted.constEnd()) {
                        canUse = false;
                        break;
                    }

                    const QVector<int> &posting = postingIt.value();
                    if (merged.isEmpty()) {
                        merged = posting;
                        continue;
                    }

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
                        break;
                    }
                }

                if (canUse) {
                    candidateIndexes = std::move(merged);
                    useInvertedCandidates = true;
                }
            }
        }

        const QVector<int> &scanIndexes = useInvertedCandidates ? candidateIndexes : d->allRecordIndexes;

        for (int rawIndex : scanIndexes) {
            if (((++iterTick) & 2047) == 0 && cancelToken && cancelToken->load()) {
                outcome.truncated = true;
                return outcome;
            }

            if (rawIndex < 0 || rawIndex >= d->recordsLinear.size()) {
                continue;
            }
            const FinderIndexedRecord &record = d->recordsLinear.at(rawIndex);
            bool matched = emptyKeyword;
            if (!matched) {
                QString haystack;
                if (request.caseSensitivity == Qt::CaseInsensitive) {
                    if (!record.searchableText.isEmpty()) {
                        haystack = record.searchableText;
                    } else {
                        const QString nameLower = record.nameLower.isEmpty()
                                ? record.entry.name.toLower()
                                : record.nameLower;
                        const QString pathLower = record.pathLower.isEmpty()
                                ? record.entry.path.toLower()
                                : record.pathLower;
                        haystack = QStringLiteral("%1 %2").arg(nameLower, pathLower);
                    }
                } else {
                    haystack = QStringLiteral("%1 %2").arg(record.entry.name, record.entry.path);
                }
                matched = haystack.contains(keyword, request.caseSensitivity);
            }
            if (!matched) {
                continue;
            }

            if (matchedOrdinal++ < skipMatches) {
                continue;
            }

            if (bucket.size() >= maxBucket) {
                outcome.hasMore = true;
                break;
            }
            bucket.push_back(record);
        }
        }
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

    {
        QWriteLocker locker(&d->lock);
        d->recordsByPath = std::move(outcome.recordsByPath);
        d->recordsLinear = std::move(outcome.recordsLinear);
        d->nameInverted = std::move(outcome.nameInverted);
        d->allRecordIndexes = std::move(outcome.allRecordIndexes);
        d->lastIndexOptions = outcome.lastIndexOptions;
        d->activeBackend = outcome.resolvedBackend;
        d->stats = outcome.stats;
        if (d->recordsLinear.size() != d->recordsByPath.size() || d->allRecordIndexes.size() != d->recordsByPath.size()) {
            if (!partialData) {
                d->rebuildDerivedIndexes();
            }
        }
        if (!partialData) {
            d->stats.totalFiles = d->recordsByPath.size();
        }
        d->cache.clear();
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

bool Libfinder::savePersistedIndex(QString *errorMessage) const
{
    QReadLocker locker(&d->lock);

    QFileInfo fileInfo(d->persistencePath);
    QDir targetDir = fileInfo.dir();
    if (!targetDir.exists() && !targetDir.mkpath(QStringLiteral("."))) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Cannot create index directory.");
        }
        return false;
    }

    QFile file(d->persistencePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Cannot write persisted index file.");
        }
        return false;
    }

    QDataStream out(&file);
    const quint32 version = 3;
    const quint32 recordCount = static_cast<quint32>(d->recordsByPath.size());
    const qint64 indexedAtMs = d->stats.indexedAt.toMSecsSinceEpoch();
    const qint32 backendValue = static_cast<qint32>(d->lastIndexOptions.backend);
    const qint64 channelUpdatedAtMs = d->stats.channelUpdatedAt.toMSecsSinceEpoch();

    out << version
        << recordCount
        << d->lastIndexOptions.roots
        << d->lastIndexOptions.excludes
        << d->lastIndexOptions.includeHidden
        << d->lastIndexOptions.maxFiles
        << backendValue
        << indexedAtMs
        << d->stats.channelState
        << d->stats.channelDetail
        << channelUpdatedAtMs
        << d->stats.usnParsedRecords
        << d->stats.usnErrorCount;

    for (auto it = d->recordsByPath.constBegin(); it != d->recordsByPath.constEnd(); ++it) {
        const FinderIndexedRecord &record = it.value();
        out << record.entry.name
            << record.entry.path
            << record.entry.sizeBytes
            << record.entry.lastModified.toMSecsSinceEpoch();
    }

    if (out.status() != QDataStream::Ok) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Persisted index encode failed.");
        }
        return false;
    }
    return true;
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
    QWriteLocker locker(&d->lock);
    d->recordsByPath.clear();
    d->recordsLinear.clear();
    d->nameInverted.clear();
    d->allRecordIndexes.clear();
    d->stats = FinderIndexStats();
    d->cache.clear();
    d->pendingChangedDirectories.clear();
    d->activeBackend = FinderIndexOptions::Backend::Generic;
    if (d->usnPollTimer) {
        d->usnPollTimer->stop();
    }
    if (d->watcher) {
        d->watcher->removePaths(d->watcher->directories());
    }
}

FinderIndexStats Libfinder::stats() const
{
    QReadLocker locker(&d->lock);
    return d->stats;
}
