#ifndef LIBFINDER_H
#define LIBFINDER_H

#include "libfinder_global.h"

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QVector>

#include <atomic>

struct FinderEntry
{
    QString name;
    QString path;
    qint64 sizeBytes = 0;
    QDateTime lastModified;
};

struct FinderSearchRequest
{
    QString keyword;
    Qt::CaseSensitivity caseSensitivity = Qt::CaseInsensitive;
    /// 传给排序器的上限；<=0 表示不额外截断排序结果。
    int limit = 200;
    /// 单次扫描返回的最大条数（分页）。<=0 表示不限制（慎用，内存与耗时会很大）。
    int pageSize = 100000;
    /// 从第几页开始（0 为第一页）；与 pageSize 搭配做跳过。
    int pageIndex = 0;
};

struct FinderSearchResult
{
    FinderEntry entry;
    int score = 0;
};

struct LIBFINDER_EXPORT FinderSearchOutcome
{
    QList<FinderSearchResult> results;
    bool truncated = false;
    bool hasMore = false;
};

Q_DECLARE_METATYPE(FinderSearchOutcome)

struct FinderIndexOptions
{
    enum class Backend
    {
        Auto,
        Generic,
        NtfsUsn
    };

    QStringList roots;
    QStringList excludes;
    bool includeHidden = false;
    int maxFiles = -1;
    Backend backend = Backend::Auto;
};

struct FinderIndexStats
{
    int totalFiles = 0;
    int indexedRoots = 0;
    QDateTime indexedAt;
    QString backend = QStringLiteral("generic-recursive");
    QString channelState = QStringLiteral("idle");
    QString channelDetail;
    QDateTime channelUpdatedAt;
    int usnParsedRecords = 0;
    int usnErrorCount = 0;
};

/// 单条索引记录（可在工作线程构建，不含 QObject）。
struct LIBFINDER_EXPORT FinderIndexedRecord
{
    FinderEntry entry;
    QString nameLower;
    QString pathLower;
    QString searchableText;
};

/// 工作线程产出的整包索引数据；由主线程 `Libfinder::commitIndexBuild` 提交。
struct LIBFINDER_EXPORT FinderIndexBuildOutcome
{
    bool ok = false;
    QString error;
    /// true 表示该结果仅来自持久化文件解码，提交后可跳过立即回写以减少启动 I/O。
    bool loadedFromPersistence = false;
    /// true 表示仅加载了首批预览数据；主线程提交时不做 watcher/timer/persistence 收尾。
    bool partialData = false;
    QHash<QString, FinderIndexedRecord> recordsByPath;
    QVector<FinderIndexedRecord> recordsLinear;
    QHash<QString, QVector<int>> nameInverted;
    QVector<int> allRecordIndexes;
    FinderIndexStats stats;
    FinderIndexOptions lastIndexOptions;
    FinderIndexOptions::Backend resolvedBackend = FinderIndexOptions::Backend::Generic;
    /// NTFS USN 路径是否退化为全盘递归扫描（若为 true，则不再尝试建立 USN 轮询状态）。
    bool ntfsFellBackToGenericScan = false;
};

Q_DECLARE_METATYPE(FinderIndexBuildOutcome)

/// 在工作线程解码持久化索引文件（仅 I/O + 反序列化，不触碰 QObject）。
LIBFINDER_EXPORT FinderIndexBuildOutcome finderDecodePersistedIndex(const QString &filePath,
                                                                    QString *errorMessage = nullptr);
/// 在工作线程快速读取持久化索引前 N 条（用于首帧秒显）。
LIBFINDER_EXPORT FinderIndexBuildOutcome finderDecodePersistedIndexPreview(const QString &filePath,
                                                                           int maxRecords,
                                                                           QString *errorMessage = nullptr);
/// 在工作线程构建索引数据集（扫描/USN，不触碰 QObject）。
LIBFINDER_EXPORT FinderIndexBuildOutcome finderBuildIndexDataset(const FinderIndexOptions &options,
                                                                  QString *errorMessage = nullptr);
/// 判断已解码的持久化索引是否与当前期望选项不一致，需要强制重建。
LIBFINDER_EXPORT bool finderPersistentIndexNeedsRebuild(const FinderIndexBuildOutcome &decoded,
                                                         const FinderIndexOptions &requestedDesired);

class LIBFINDER_EXPORT Libfinder
{
public:
    Libfinder();
    ~Libfinder();

    bool rebuildIndex(const FinderIndexOptions &options, QString *errorMessage = nullptr);
    bool loadPersistedIndex(QString *errorMessage = nullptr);
    /// 将工作线程产出的索引提交到引擎，并在主线程配置 watcher / timer / 持久化。
    bool commitIndexBuild(FinderIndexBuildOutcome outcome, QString *errorMessage = nullptr);
    bool savePersistedIndex(QString *errorMessage = nullptr) const;
    FinderSearchOutcome search(const FinderSearchRequest &request, std::atomic<bool> *cancelToken = nullptr);

    void setPersistenceFilePath(const QString &filePath);
    QString persistenceFilePath() const;
    FinderIndexOptions indexOptions() const;

    void clear();
    FinderIndexStats stats() const;

private:
    struct Impl;
    Impl *d = nullptr;
};

#endif // LIBFINDER_H
