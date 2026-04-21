#include "findfasterwidget.h"
#include "finderresultitemdelegate.h"
#include "findresultsmodel.h"
#include "ntfsusn_utils.h"
#include "win_shell_context_menu.h"

#include <QtConcurrent/QtConcurrent>
#include <QAbstractItemView>
#include <QComboBox>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QAction>
#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QMenu>
#include <QFuture>
#include <QHash>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QProcess>
#include <QPointer>
#include <QDebug>
#include <QSet>
#include <QScrollBar>
#include <QShowEvent>
#include <QStatusBar>
#include <QTableView>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>
#include <algorithm>
#include <utility>

namespace
{
struct TimedIndexLoadResult
{
    FinderIndexBuildOutcome outcome;
    qint64 decodeMs = -1;
};

QString csvEscapeField(const QString &s)
{
    QString out = s;
    out.replace(QLatin1Char('"'), QStringLiteral("\"\""));
    if (out.contains(QLatin1Char(',')) || out.contains(QLatin1Char('"')) || out.contains(QLatin1Char('\n'))
        || out.contains(QLatin1Char('\r'))) {
        return QStringLiteral("\"%1\"").arg(out);
    }
    return out;
}

QString buildWindowTitle(const QString &keyword)
{
    const QString trimmed = keyword.trimmed();
    if (trimmed.isEmpty()) {
        return QStringLiteral("findfaster");
    }
    return QStringLiteral("%1 - findfaster").arg(trimmed);
}

int computeEmptyQueryPageSize(const FinderIndexStats &stats)
{
    if (stats.totalFiles <= 0) {
        return 50000;
    }
    // 尽量单次拉取全量，避免分页往返带来的启动抖动。
    return qMax(1000, stats.totalFiles);
}
} // namespace

FindFasterWidget::FindFasterWidget(QWidget *parent)
    : QMainWindow(parent)
{
    qRegisterMetaType<FinderSearchOutcome>();
    qRegisterMetaType<FinderSearchJob>();
    qRegisterMetaType<FinderIndexBuildOutcome>();

    buildUi();
    buildMenus();
    applyDisplayResults();
    updateStatus();
}

FindFasterWidget::~FindFasterWidget()
{
    if (m_searchCancel) {
        m_searchCancel->store(true);
    }
    if (m_mainSearchWatcher && m_mainSearchWatcher->isRunning()) {
        m_mainSearchWatcher->waitForFinished();
    }
    if (m_pageSearchWatcher && m_pageSearchWatcher->isRunning()) {
        m_pageSearchWatcher->waitForFinished();
    }
}

void FindFasterWidget::showEvent(QShowEvent *event)
{
    QMainWindow::showEvent(event);
    if (m_deferredIndexScheduled) {
        return;
    }
    m_deferredIndexScheduled = true;
    QTimer::singleShot(0, this, &FindFasterWidget::runDeferredIndexBuild);
}

void FindFasterWidget::refreshSearch()
{
    if (!m_searchEdit) {
        return;
    }

    const QString keyword = m_searchEdit->text();
    setWindowTitle(buildWindowTitle(keyword));
    m_prefetchHitForCurrentQuery = false;
    FinderSearchRequest request;
    request.keyword = keyword;
    request.pageSize = kPrefetchPageSize;
    request.pageIndex = 0;
    request.limit = request.pageSize;
    const QString trimmed = keyword.trimmed();
    const bool hasPathLikeChars = keyword.contains(QLatin1Char('/'))
            || keyword.contains(QLatin1Char('\\'))
            || keyword.contains(QLatin1Char(':'));
    if (!trimmed.isEmpty() && trimmed.size() <= 2 && !hasPathLikeChars) {
        request.fileNamesOnly = true;
    }

    if (keyword.isEmpty()) {
        request.pageSize = computeEmptyQueryPageSize(m_engine.stats());
        request.limit = request.pageSize;
        m_prefetchInProgress = false;
        submitSearch(request, true);
        return;
    }

    if (!shouldStartPrefetch(keyword)) {
        m_prefetchKeyword.clear();
        m_prefetchResults.clear();
        m_prefetchHasMore = false;
        m_prefetchInProgress = false;
        updateStatus();
        return;
    }

    submitSearch(request, false);
}

void FindFasterWidget::onSearchCommitted()
{
    if (!m_searchEdit) {
        return;
    }

    const QString keyword = m_searchEdit->text();
    setWindowTitle(buildWindowTitle(keyword));
    m_prefetchHitForCurrentQuery = false;
    if (!keyword.isEmpty()) {
        m_prefetchHitForCurrentQuery = tryApplyPrefetchForCommittedQuery(keyword);
        if (m_prefetchHitForCurrentQuery) {
            updateStatus();
        }
    }

    FinderSearchRequest request;
    request.keyword = keyword;
    request.pageSize = keyword.trimmed().isEmpty()
            ? computeEmptyQueryPageSize(m_engine.stats())
            : 100000;
    request.pageIndex = 0;
    request.limit = request.pageSize;
    const QString trimmed = keyword.trimmed();
    const bool hasPathLikeChars = keyword.contains(QLatin1Char('/'))
            || keyword.contains(QLatin1Char('\\'))
            || keyword.contains(QLatin1Char(':'));
    if (!trimmed.isEmpty() && trimmed.size() <= 2 && !hasPathLikeChars) {
        request.fileNamesOnly = true;
    }
    submitSearch(request, true);
}

void FindFasterWidget::updateStatus()
{
    if (!m_statusUpdateTimer) {
        renderStatusTextNow();
        return;
    }
    m_statusUpdatePending = true;
    if (!m_statusUpdateTimer->isActive()) {
        m_statusUpdateTimer->start();
    }
}

void FindFasterWidget::renderStatusTextNow()
{
    const FinderIndexStats stats = m_engine.stats();
    const QString timeText = stats.indexedAt.isValid()
            ? stats.indexedAt.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
            : QStringLiteral("-");

    const int displayCount = m_resultModel ? m_resultModel->rowCount() : 0;
    const QString pagingHint = m_hasMorePages ? QStringLiteral(" | 还有更多，滚动加载") : QString();
    QString prefetchHint;
    if (m_prefetchInProgress) {
        prefetchHint = QStringLiteral(" | 预检索中");
    } else if (m_prefetchHitForCurrentQuery) {
        prefetchHint = QStringLiteral(" | 已命中预检索");
    }
    const QString indexHint = m_indexLoading ? QStringLiteral(" | 索引加载中…") : QString();
    m_leftStatus->setText(QStringLiteral("%1 个对象 | 当前显示 %2 | 索引根目录 %3 | 上次构建 %4%5")
                          .arg(stats.totalFiles)
                          .arg(displayCount)
                          .arg(stats.indexedRoots)
                          .arg(timeText)
                          .arg(pagingHint + prefetchHint + indexHint));
    const QString channelText = NtfsUsnUtils::formatChannelStatus(stats.channelState,
                                                                  stats.channelDetail,
                                                                  stats.channelUpdatedAt);
    const QString metrics = startupMetricsText() + runtimeMetricsText();
    m_rightStatus->setText(QStringLiteral("后端: %1 | 通道: %2 | USN解析: %3 | USN错误: %4%5")
                           .arg(stats.backend)
                           .arg(channelText)
                           .arg(stats.usnParsedRecords)
                           .arg(stats.usnErrorCount)
                           .arg(metrics));
}

void FindFasterWidget::buildUi()
{
    setWindowTitle(QStringLiteral("findfaster"));
    resize(1100, 720);

    QWidget *central = new QWidget(this);
    QVBoxLayout *layout = new QVBoxLayout(central);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(6);

    QHBoxLayout *topRowLayout = new QHBoxLayout();
    topRowLayout->setSpacing(6);

    m_searchEdit = new QLineEdit(central);
    m_searchEdit->setPlaceholderText(QStringLiteral("输入文件名或路径关键字，实时搜索"));
    topRowLayout->addWidget(m_searchEdit, 1);

    m_driveFilterCombo = new QComboBox(central);
    m_driveFilterCombo->addItem(QStringLiteral("全部盘符"), QString());
#ifdef Q_OS_WIN
    const QFileInfoList drives = QDir::drives();
    for (const QFileInfo &drive : drives) {
        const QString root = QDir::cleanPath(drive.absoluteFilePath());
        if (!root.isEmpty()) {
            m_driveFilterCombo->addItem(root, root.toLower());
        }
    }
#endif
    m_driveFilterCombo->setMinimumWidth(120);
    topRowLayout->addWidget(m_driveFilterCombo);
    layout->addLayout(topRowLayout);

    m_resultModel = new FinderResultsModel(central);
    m_itemDelegate = new FinderResultItemDelegate(central);
    m_resultView = new QTableView(central);
    m_resultView->setModel(m_resultModel);
    m_resultView->setItemDelegate(m_itemDelegate);
    m_resultView->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_resultView->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_resultView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_resultView->setAlternatingRowColors(false);
    m_resultView->setShowGrid(false);
    m_resultView->setSortingEnabled(false);
    m_resultView->setWordWrap(false);
    m_resultView->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_resultView->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_resultView->verticalHeader()->setVisible(false);
    m_resultView->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    m_resultView->verticalHeader()->setDefaultSectionSize(22);
    m_resultView->setContextMenuPolicy(Qt::CustomContextMenu);

    QHeaderView *header = m_resultView->horizontalHeader();
    header->setSectionResizeMode(0, QHeaderView::Interactive);
    header->setSectionResizeMode(1, QHeaderView::Stretch);
    header->setSectionResizeMode(2, QHeaderView::Interactive);
    header->setSectionResizeMode(3, QHeaderView::Interactive);
    m_resultView->setColumnWidth(0, 240);
    m_resultView->setColumnWidth(2, 100);
    m_resultView->setColumnWidth(3, 170);

    layout->addWidget(m_resultView, 1);

    setCentralWidget(central);

    m_leftStatus = new QLabel(this);
    m_rightStatus = new QLabel(this);
    statusBar()->addWidget(m_leftStatus, 1);
    statusBar()->addPermanentWidget(m_rightStatus);

    m_searchDebounceTimer = new QTimer(this);
    m_searchDebounceTimer->setInterval(120);
    m_searchDebounceTimer->setSingleShot(true);

    m_loadMoreDebounceTimer = new QTimer(this);
    m_loadMoreDebounceTimer->setInterval(120);
    m_loadMoreDebounceTimer->setSingleShot(true);

    m_progressiveRenderTimer = new QTimer(this);
    m_progressiveRenderTimer->setInterval(0);
    m_progressiveRenderTimer->setSingleShot(false);
    m_statusUpdateTimer = new QTimer(this);
    m_statusUpdateTimer->setInterval(100);
    m_statusUpdateTimer->setSingleShot(true);
    m_resumeIconsTimer = new QTimer(this);
    m_resumeIconsTimer->setInterval(700);
    m_resumeIconsTimer->setSingleShot(true);
    m_iconRefreshTimer = new QTimer(this);
    m_iconRefreshTimer->setInterval(kIconRefreshIntervalMs);
    m_iconRefreshTimer->setSingleShot(false);

    m_mainSearchWatcher = new QFutureWatcher<FinderSearchJob>(this);
    m_pageSearchWatcher = new QFutureWatcher<FinderSearchJob>(this);

    connect(m_searchEdit, &QLineEdit::textChanged, m_searchDebounceTimer, qOverload<>(&QTimer::start));
    connect(m_searchEdit, &QLineEdit::returnPressed, this, &FindFasterWidget::onSearchCommitted);
    connect(m_searchDebounceTimer, &QTimer::timeout, this, &FindFasterWidget::refreshSearch);
    connect(m_mainSearchWatcher, &QFutureWatcher<FinderSearchJob>::finished, this, &FindFasterWidget::onMainSearchFinished);
    connect(m_pageSearchWatcher, &QFutureWatcher<FinderSearchJob>::finished, this, &FindFasterWidget::onPageSearchFinished);
    connect(m_resultView, &QTableView::doubleClicked, this, &FindFasterWidget::openResultFromIndex);
    connect(m_driveFilterCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, &FindFasterWidget::onDriveFilterChanged);
    connect(m_loadMoreDebounceTimer, &QTimer::timeout, this, &FindFasterWidget::onMaybeLoadMore);
    connect(m_resultView->verticalScrollBar(), &QScrollBar::valueChanged, this, &FindFasterWidget::onScrollLoadMoreDebounce);
    connect(m_resultView, &QTableView::customContextMenuRequested, this, &FindFasterWidget::onResultContextMenu);
    connect(m_progressiveRenderTimer, &QTimer::timeout, this, &FindFasterWidget::onProgressiveRenderTick);
    connect(m_statusUpdateTimer, &QTimer::timeout, this, [this]() {
        const bool shouldRender = m_statusUpdatePending;
        m_statusUpdatePending = false;
        if (shouldRender) {
            renderStatusTextNow();
        }
    });
    connect(m_resumeIconsTimer, &QTimer::timeout, this, [this]() {
        if (m_resultModel) {
            m_resultModel->setShowIcons(true);
            m_iconRefreshNextRow = 0;
            if (m_resultModel->rowCount() > 0 && m_iconRefreshTimer) {
                m_iconRefreshTimer->start();
            }
        }
    });
    connect(m_iconRefreshTimer, &QTimer::timeout, this, [this]() {
        if (!m_resultModel || !m_resultModel->showIcons()) {
            m_iconRefreshTimer->stop();
            return;
        }
        const int rows = m_resultModel->rowCount();
        if (rows <= 0 || m_iconRefreshNextRow >= rows) {
            m_iconRefreshTimer->stop();
            return;
        }

        const int first = m_iconRefreshNextRow;
        const int last = qMin(rows - 1, first + kIconRefreshChunkRows - 1);
        m_resultModel->notifyDecorationRowsChanged(first, last);
        m_iconRefreshNextRow = last + 1;
    });
}

void FindFasterWidget::buildMenus()
{
    QMenu *fileMenu = menuBar()->addMenu(QStringLiteral("文件(F)"));
    QMenu *exportMenu = fileMenu->addMenu(QStringLiteral("导出"));
    QAction *exportCsvAction = exportMenu->addAction(QStringLiteral("导出为 CSV..."));
    connect(exportCsvAction, &QAction::triggered, this, &FindFasterWidget::exportResultsToCsv);

    menuBar()->addMenu(QStringLiteral("搜索(S)"));
    QMenu *helpMenu = menuBar()->addMenu(QStringLiteral("帮助(H)"));
    QAction *aboutAction = helpMenu->addAction(QStringLiteral("关于"));
    connect(aboutAction, &QAction::triggered, this, &FindFasterWidget::showAboutDialog);
}

FinderIndexOptions FindFasterWidget::prepareDefaultIndexOptions() const
{
    FinderIndexOptions options;
#ifdef Q_OS_WIN
    const QString preferredDrive = QStringLiteral("D:");
    bool hasPreferredDrive = false;
    const QFileInfoList drives = QDir::drives();
    for (const QFileInfo &drive : drives) {
        const QString root = QDir::cleanPath(drive.absoluteFilePath());
        if (!root.isEmpty()) {
            if (root.compare(preferredDrive, Qt::CaseInsensitive) == 0) {
                hasPreferredDrive = true;
            } else {
                options.roots << root;
            }
        }
    }
    if (hasPreferredDrive) {
        options.roots.prepend(preferredDrive);
    }
#else
    options.roots << QDir::homePath();
#endif
    options.roots.removeDuplicates();
    options.maxFiles = -1;
    options.backend = FinderIndexOptions::Backend::Generic;
    options.excludes << QDir::cleanPath(QDir::homePath() + QStringLiteral("/AppData/Local/Temp"));
    return options;
}

void FindFasterWidget::runDeferredIndexBuild()
{
    m_startupTimingActive = true;
    m_startupMetricsLogged = false;
    m_previewDecodeMs = -1;
    m_previewCommitMs = -1;
    m_fullDecodeMs = -1;
    m_firstVisibleMs = -1;
    m_startupLoadTimer.start();
    m_indexLoading = true;
    updateStatus();

    const FinderIndexOptions options = prepareDefaultIndexOptions();
    const QString persistPath = m_engine.persistenceFilePath();
    auto createFullLoadFuture = [persistPath, options]() {
        return QtConcurrent::run([persistPath, options]() -> TimedIndexLoadResult {
            QElapsedTimer elapsed;
            elapsed.start();
            TimedIndexLoadResult timed;
            QString err;
            FinderIndexBuildOutcome decoded = finderDecodePersistedIndex(persistPath, &err);
            if (decoded.ok && !finderPersistentIndexNeedsRebuild(decoded, options) && decoded.stats.totalFiles > 0) {
                timed.outcome = std::move(decoded);
                timed.decodeMs = elapsed.elapsed();
                return timed;
            }
            err.clear();
            timed.outcome = finderBuildIndexDataset(options, &err);
            timed.decodeMs = elapsed.elapsed();
            return timed;
        });
    };

    auto *fullWatcher = new QFutureWatcher<TimedIndexLoadResult>(this);
    QObject::connect(fullWatcher, &QFutureWatcher<TimedIndexLoadResult>::finished, this, [this, fullWatcher]() {
        QPointer<FindFasterWidget> self(this);
        TimedIndexLoadResult timed = fullWatcher->result();
        FinderIndexBuildOutcome outcome = std::move(timed.outcome);
        fullWatcher->deleteLater();
        if (!self) {
            return;
        }
        m_fullDecodeMs = timed.decodeMs;
        m_indexLoading = false;
        if (!outcome.ok) {
            m_leftStatus->setText(QStringLiteral("索引构建失败: %1").arg(outcome.error));
            m_startupTimingActive = false;
            updateStatus();
            return;
        }
        QString commitErr;
        if (!m_engine.commitIndexBuild(std::move(outcome), &commitErr)) {
            m_leftStatus->setText(QStringLiteral("索引提交失败: %1").arg(commitErr.isEmpty() ? QStringLiteral("unknown") : commitErr));
            m_startupTimingActive = false;
            updateStatus();
            return;
        }
        m_startupTimingActive = false;
        refreshSearch();
        logStartupMetricsIfReady();
        updateStatus();
    });

    auto *previewWatcher = new QFutureWatcher<TimedIndexLoadResult>(this);
    QObject::connect(previewWatcher, &QFutureWatcher<TimedIndexLoadResult>::finished, this, [this, previewWatcher, fullWatcher, createFullLoadFuture]() {
        QPointer<FindFasterWidget> self(this);
        TimedIndexLoadResult timed = previewWatcher->result();
        FinderIndexBuildOutcome preview = std::move(timed.outcome);
        previewWatcher->deleteLater();
        if (!self) {
            return;
        }
        m_previewDecodeMs = timed.decodeMs;

        if (preview.ok && preview.partialData && preview.stats.totalFiles > 0) {
            QElapsedTimer commitElapsed;
            commitElapsed.start();
            QString previewErr;
            if (m_engine.commitIndexBuild(std::move(preview), &previewErr)) {
                m_previewCommitMs = commitElapsed.elapsed();
                refreshSearch();
            }
        }
        logStartupMetricsIfReady();
        updateStatus();
        fullWatcher->setFuture(createFullLoadFuture());
    });

    const QFuture<TimedIndexLoadResult> previewFuture = QtConcurrent::run([persistPath, options]() -> TimedIndexLoadResult {
        QElapsedTimer elapsed;
        elapsed.start();
        TimedIndexLoadResult timed;
        QString err;
        FinderIndexBuildOutcome preview = finderDecodePersistedIndexPreview(persistPath, 2000, &err);
        if (preview.ok && !finderPersistentIndexNeedsRebuild(preview, options) && preview.stats.totalFiles > 0) {
            preview.partialData = true;
            timed.outcome = std::move(preview);
            timed.decodeMs = elapsed.elapsed();
            return timed;
        }
        timed.outcome.ok = false;
        timed.outcome.error = err;
        timed.decodeMs = elapsed.elapsed();
        return timed;
    });
    previewWatcher->setFuture(previewFuture);
}

void FindFasterWidget::applyDisplayResults()
{
    QElapsedTimer elapsed;
    elapsed.start();
    if (!m_resultModel || !m_driveFilterCombo) {
        return;
    }

    const QString driveFilter = m_driveFilterCombo->currentData().toString();
    if (driveFilter.isEmpty()) {
        scheduleIconResume();
        if (!m_progressiveRenderTimer->isActive() && m_displayedSearchResults.size() != m_allSearchResults.size()) {
            const int firstRow = m_displayedSearchResults.size();
            m_displayedSearchResults.reserve(m_allSearchResults.size());
            for (int i = firstRow; i < m_allSearchResults.size(); ++i) {
                m_displayedSearchResults.push_back(m_allSearchResults.at(i));
            }
            m_progressiveNextIndex = m_displayedSearchResults.size();
            if (m_resultModel->isUsingExternalSource(&m_displayedSearchResults) && firstRow < m_displayedSearchResults.size()) {
                m_resultModel->notifyExternalRowsInserted(firstRow, m_displayedSearchResults.size() - 1);
                m_lastApplyDisplayMs = elapsed.elapsed();
                return;
            }
        }
        m_resultModel->setExternalSource(&m_displayedSearchResults);
        m_lastApplyDisplayMs = elapsed.elapsed();
        return;
    }

    if (m_progressiveRenderTimer->isActive()) {
        m_progressiveRenderTimer->stop();
    }

    QList<FinderSearchResult> filtered;
    filtered.reserve(m_allSearchResults.size());
    for (const FinderSearchResult &result : m_allSearchResults) {
        if (matchesDriveFilter(result.entry.path, driveFilter)) {
            filtered.push_back(result);
        }
    }
    m_resultModel->setOwnedRows(std::move(filtered));
    scheduleIconResume();
    m_lastApplyDisplayMs = elapsed.elapsed();
}

void FindFasterWidget::submitSearch(const FinderSearchRequest &request, bool forDisplay)
{
    if (m_mainSearchWatcher->isRunning()) {
        m_queuedRequest = request;
        m_queuedForDisplay = forDisplay;
        m_hasQueuedRequest = true;
        if (m_searchCancel) {
            m_searchCancel->store(true);
        }
        return;
    }

    beginSearchNow(request, forDisplay);
}

void FindFasterWidget::maybeContinueInitialWarmup()
{
    if (!m_driveFilterCombo || !m_lastSearchRequest.keyword.trimmed().isEmpty()) {
        return;
    }
    if (!m_driveFilterCombo->currentData().toString().isEmpty()) {
        return;
    }
    if (!m_hasMorePages || m_pageLoadInProgress || m_mainSearchWatcher->isRunning()) {
        return;
    }
    QTimer::singleShot(0, this, &FindFasterWidget::startLoadNextPage);
}

QString FindFasterWidget::startupMetricsText() const
{
    if (!m_startupTimingActive && m_previewDecodeMs < 0 && m_fullDecodeMs < 0 && m_firstVisibleMs < 0) {
        return QString();
    }

    QStringList parts;
    if (m_previewDecodeMs >= 0) {
        parts << QStringLiteral("预览解码 %1ms").arg(m_previewDecodeMs);
    }
    if (m_previewCommitMs >= 0) {
        parts << QStringLiteral("预览提交 %1ms").arg(m_previewCommitMs);
    }
    if (m_fullDecodeMs >= 0) {
        parts << QStringLiteral("全量解码 %1ms").arg(m_fullDecodeMs);
    }
    if (m_firstVisibleMs >= 0) {
        parts << QStringLiteral("首屏可见 %1ms").arg(m_firstVisibleMs);
    }
    if (parts.isEmpty()) {
        return QStringLiteral(" | 启动埋点采集中");
    }
    return QStringLiteral(" | 启动埋点: %1").arg(parts.join(QStringLiteral(", ")));
}

QString FindFasterWidget::runtimeMetricsText() const
{
    if (m_lastSearchComputeMs < 0 && m_lastApplyDisplayMs < 0) {
        return QString();
    }
    QStringList parts;
    if (m_lastSearchComputeMs >= 0) {
        parts << QStringLiteral("检索 %1ms").arg(m_lastSearchComputeMs);
    }
    if (m_lastApplyDisplayMs >= 0) {
        parts << QStringLiteral("渲染应用 %1ms").arg(m_lastApplyDisplayMs);
    }
    parts << QStringLiteral("分块 %1行").arg(m_renderChunkRows);
    return QStringLiteral(" | 运行埋点: %1").arg(parts.join(QStringLiteral(", ")));
}

void FindFasterWidget::scheduleIconResume()
{
    if (!m_resultModel) {
        return;
    }
    if (m_iconRefreshTimer) {
        m_iconRefreshTimer->stop();
    }
    m_iconRefreshNextRow = 0;
    m_resultModel->setShowIcons(false);
    if (m_resumeIconsTimer) {
        m_resumeIconsTimer->stop();
        m_resumeIconsTimer->start();
    } else {
        m_resultModel->setShowIcons(true);
        if (m_iconRefreshTimer && m_resultModel->rowCount() > 0) {
            m_iconRefreshTimer->start();
        }
    }
}

void FindFasterWidget::adaptRenderChunkRows()
{
    int chunk = m_renderChunkRows;

    if (m_lastApplyDisplayMs > 16) {
        chunk = qMax(kRenderChunkRowsMin, chunk - 40);
    } else if (m_lastApplyDisplayMs >= 0 && m_lastApplyDisplayMs < 8) {
        chunk = qMin(kRenderChunkRowsMax, chunk + 30);
    }

    if (m_lastSearchComputeMs >= 0 && m_lastSearchComputeMs < 10) {
        chunk = qMin(kRenderChunkRowsMax, chunk + 40);
    } else if (m_lastSearchComputeMs > 50) {
        chunk = qMax(kRenderChunkRowsMin, chunk - 20);
    }

    m_renderChunkRows = chunk;
}

void FindFasterWidget::logStartupMetricsIfReady()
{
    if (m_startupMetricsLogged || m_previewDecodeMs < 0 || m_previewCommitMs < 0 || m_fullDecodeMs < 0 || m_firstVisibleMs < 0) {
        return;
    }
    m_startupMetricsLogged = true;
    qInfo().noquote() << QStringLiteral("[startup-metrics] preview_decode_ms=%1 preview_commit_ms=%2 full_decode_ms=%3 first_visible_ms=%4")
                                 .arg(m_previewDecodeMs)
                                 .arg(m_previewCommitMs)
                                 .arg(m_fullDecodeMs)
                                 .arg(m_firstVisibleMs);

    const QFileInfo persistenceInfo(m_engine.persistenceFilePath());
    QDir dir = persistenceInfo.dir();
    if (dir.exists() || dir.mkpath(QStringLiteral("."))) {
        QFile file(dir.filePath(QStringLiteral("startup-metrics.log")));
        if (file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
            const QString line = QStringLiteral("%1 preview_decode_ms=%2 preview_commit_ms=%3 full_decode_ms=%4 first_visible_ms=%5\n")
                                         .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")))
                                         .arg(m_previewDecodeMs)
                                         .arg(m_previewCommitMs)
                                         .arg(m_fullDecodeMs)
                                         .arg(m_firstVisibleMs);
            file.write(line.toUtf8());
        }
    }
}

void FindFasterWidget::beginSearchNow(const FinderSearchRequest &request, bool forDisplay)
{
    ++m_searchToken;
    if (m_searchCancel) {
        m_searchCancel->store(true);
    }
    m_searchCancel = std::make_shared<std::atomic<bool>>(false);

    if (m_pageSearchWatcher->isRunning()) {
        m_pageLoadInProgress = false;
    }

    const int jobToken = m_searchToken;
    const auto cancel = m_searchCancel;
    if (forDisplay) {
        m_lastSearchRequest = request;
        m_prefetchInProgress = false;
        if (m_progressiveRenderTimer && m_progressiveRenderTimer->isActive()) {
            m_progressiveRenderTimer->stop();
        }
    } else {
        m_prefetchInProgress = true;
    }
    updateStatus();

    const QFuture<FinderSearchJob> future = QtConcurrent::run([this, request, cancel, jobToken, forDisplay]() {
        FinderSearchJob job;
        job.token = jobToken;
        job.forDisplay = forDisplay;
        job.keyword = request.keyword;
        QElapsedTimer timer;
        timer.start();
        if (request.keyword.trimmed().isEmpty()) {
            job.outcome = m_engine.browseIndexed(request.pageSize, request.pageIndex, cancel.get());
        } else {
            job.outcome = m_engine.search(request, cancel.get());
        }
        job.searchMs = timer.elapsed();
        return job;
    });
    m_mainSearchWatcher->setFuture(future);
}

void FindFasterWidget::onMainSearchFinished()
{
    FinderSearchJob job = m_mainSearchWatcher->result();
    m_lastSearchComputeMs = job.searchMs;
    adaptRenderChunkRows();

    if (m_hasQueuedRequest) {
        m_hasQueuedRequest = false;
        const FinderSearchRequest next = m_queuedRequest;
        const bool nextForDisplay = m_queuedForDisplay;
        beginSearchNow(next, nextForDisplay);
        return;
    }

    if (job.token != m_searchToken) {
        return;
    }

    if (!job.forDisplay) {
        m_prefetchInProgress = false;
        m_prefetchKeyword = job.keyword;
        m_prefetchResults = std::move(job.outcome.results);
        m_prefetchHasMore = job.outcome.hasMore;
        updateStatus();
        return;
    }

    QList<FinderSearchResult> finalizedResults = std::move(job.outcome.results);
    const bool noDriveFilter = m_driveFilterCombo && m_driveFilterCombo->currentData().toString().isEmpty();
    if (m_prefetchHitForCurrentQuery && noDriveFilter) {
        stabilizeFirstRows(&finalizedResults);
    }

    m_allSearchResults = std::move(finalizedResults);
    m_displayedSearchResults.clear();
    m_progressiveNextIndex = 0;
    m_hasMorePages = job.outcome.hasMore;
    m_nextPageIndex = 1;
    m_pageLoadInProgress = false;
    if (noDriveFilter) {
        startProgressiveDisplay();
    } else {
        applyDisplayResults();
    }
    if (m_startupTimingActive && m_firstVisibleMs < 0 && !m_allSearchResults.isEmpty() && m_startupLoadTimer.isValid()) {
        m_firstVisibleMs = m_startupLoadTimer.elapsed();
        logStartupMetricsIfReady();
    }
    maybeContinueInitialWarmup();
    updateStatus();
}

void FindFasterWidget::onPageSearchFinished()
{
    FinderSearchJob job = m_pageSearchWatcher->result();
    m_lastSearchComputeMs = job.searchMs;
    adaptRenderChunkRows();
    m_pageLoadInProgress = false;

    if (job.token != m_searchToken) {
        return;
    }

    if (job.outcome.results.isEmpty()) {
        m_hasMorePages = job.outcome.hasMore;
        updateStatus();
        return;
    }

    const int oldSize = m_allSearchResults.size();
    for (FinderSearchResult &item : job.outcome.results) {
        m_allSearchResults.append(std::move(item));
    }
    if (m_driveFilterCombo->currentData().toString().isEmpty() && m_resultModel) {
        if (m_displayedSearchResults.isEmpty() && oldSize == 0) {
            startProgressiveDisplay();
        } else if (m_displayedSearchResults.size() < m_allSearchResults.size() && !m_progressiveRenderTimer->isActive()) {
            m_progressiveRenderTimer->start();
        }
    } else {
        applyDisplayResults();
    }

    m_hasMorePages = job.outcome.hasMore;
    maybeContinueInitialWarmup();
    updateStatus();
}

void FindFasterWidget::onDriveFilterChanged(int index)
{
    Q_UNUSED(index)
    m_hasMorePages = false;
    applyDisplayResults();
    updateStatus();
}

void FindFasterWidget::onScrollLoadMoreDebounce()
{
    if (m_resultView) {
        const int viewportTop = m_resultView->rowAt(0);
        const int viewportBottom = m_resultView->rowAt(m_resultView->viewport()->height() - 1);
        if (viewportTop >= 0 && viewportBottom >= 0) {
            m_viewportAnchorRow = (viewportTop + viewportBottom) / 2;
        } else if (m_resultView->verticalScrollBar()) {
            QScrollBar *bar = m_resultView->verticalScrollBar();
            if (bar->maximum() > 0 && !m_allSearchResults.isEmpty()) {
                const double ratio = static_cast<double>(bar->value()) / static_cast<double>(bar->maximum());
                m_viewportAnchorRow = static_cast<int>(ratio * qMax(0, m_allSearchResults.size() - 1));
            }
        }
        if (m_progressiveRenderTimer && !m_progressiveRenderTimer->isActive()
            && m_progressiveNextIndex < m_allSearchResults.size()) {
            m_progressiveRenderTimer->start();
        }
    }
    if (!m_loadMoreDebounceTimer->isActive()) {
        m_loadMoreDebounceTimer->start();
    }
}

void FindFasterWidget::onMaybeLoadMore()
{
    if (!m_resultView || !m_driveFilterCombo) {
        return;
    }
    if (!m_driveFilterCombo->currentData().toString().isEmpty()) {
        return;
    }
    if (!m_hasMorePages || m_pageLoadInProgress || m_mainSearchWatcher->isRunning()) {
        return;
    }

    QScrollBar *bar = m_resultView->verticalScrollBar();
    if (bar->maximum() <= 0) {
        return;
    }
    if (bar->value() < static_cast<int>(bar->maximum() * 0.88)) {
        return;
    }

    startLoadNextPage();
}

void FindFasterWidget::startLoadNextPage()
{
    if (!m_hasMorePages || m_pageLoadInProgress || m_mainSearchWatcher->isRunning()) {
        return;
    }
    if (!m_driveFilterCombo->currentData().toString().isEmpty()) {
        return;
    }

    m_pageLoadInProgress = true;
    const int jobToken = m_searchToken;
    const auto cancel = m_searchCancel;

    FinderSearchRequest request = m_lastSearchRequest;
    request.pageIndex = m_nextPageIndex++;
    request.limit = request.pageSize;

    const QFuture<FinderSearchJob> future = QtConcurrent::run([this, request, cancel, jobToken]() {
        FinderSearchJob job;
        job.token = jobToken;
        QElapsedTimer timer;
        timer.start();
        if (request.keyword.trimmed().isEmpty()) {
            job.outcome = m_engine.browseIndexed(request.pageSize, request.pageIndex, cancel.get());
        } else {
            job.outcome = m_engine.search(request, cancel.get());
        }
        job.searchMs = timer.elapsed();
        return job;
    });
    m_pageSearchWatcher->setFuture(future);
}

void FindFasterWidget::exportResultsToCsv()
{
    if (!m_resultModel || m_resultModel->rowCount() == 0) {
        QMessageBox::information(this, QStringLiteral("导出"), QStringLiteral("当前没有可导出的结果。"));
        return;
    }

    const QString defaultPath = QDir::homePath() + QStringLiteral("/findfast_results.csv");
    QString path = QFileDialog::getSaveFileName(this, QStringLiteral("导出为 CSV"), defaultPath,
                                                QStringLiteral("CSV 文件 (*.csv);;所有文件 (*.*)"));
    if (path.isEmpty()) {
        return;
    }
    if (!path.endsWith(QStringLiteral(".csv"), Qt::CaseInsensitive)) {
        path += QStringLiteral(".csv");
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::warning(this, QStringLiteral("导出失败"), QStringLiteral("无法写入文件：%1").arg(path));
        return;
    }

    file.write(QByteArrayLiteral("\xEF\xBB\xBF"));

    auto writeLine = [&file](const QString &line) {
        file.write(line.toUtf8());
        file.write("\n");
    };

    QStringList headerLine;
    headerLine << m_resultModel->headerData(0, Qt::Horizontal, Qt::DisplayRole).toString();
    headerLine << m_resultModel->headerData(1, Qt::Horizontal, Qt::DisplayRole).toString();
    headerLine << QStringLiteral("大小(可读)");
    headerLine << QStringLiteral("大小(字节)");
    headerLine << m_resultModel->headerData(3, Qt::Horizontal, Qt::DisplayRole).toString();
    headerLine << QStringLiteral("评分");

    QStringList escapedHeader;
    for (const QString &h : headerLine) {
        escapedHeader << csvEscapeField(h);
    }
    writeLine(escapedHeader.join(QLatin1Char(',')));

    const int n = m_resultModel->rowCount();
    for (int row = 0; row < n; ++row) {
        const FinderSearchResult &r = m_resultModel->resultAt(row);
        const QString sizeReadable = m_resultModel->data(m_resultModel->index(row, 2), Qt::DisplayRole).toString();
        const QString timeStr = m_resultModel->data(m_resultModel->index(row, 3), Qt::DisplayRole).toString();
        QStringList fields;
        fields << csvEscapeField(r.entry.name);
        fields << csvEscapeField(r.entry.path);
        fields << csvEscapeField(sizeReadable);
        fields << QString::number(r.entry.sizeBytes);
        fields << csvEscapeField(timeStr);
        fields << QString::number(r.score);
        writeLine(fields.join(QLatin1Char(',')));
    }

    file.close();
    QMessageBox::information(this, QStringLiteral("导出"),
                             QStringLiteral("已导出 %1 行到：\n%2").arg(n).arg(QDir::toNativeSeparators(path)));
}

void FindFasterWidget::showAboutDialog()
{
    const FinderIndexStats stats = m_engine.stats();
    QString appVersion = QCoreApplication::applicationVersion();
    if (appVersion.isEmpty()) {
        appVersion = QStringLiteral("0.1.0");
    }

    const QString aboutText = QStringLiteral(
            "FindFaster\n\n"
            "版本: %1\n"
            "作者: CSDN键盘会跳舞\n"
            "Qt: %2\n"
            "索引后端: %3\n"
            "索引对象数: %4\n"
            "构建时间: %5 %6")
                                      .arg(appVersion)
                                      .arg(QString::fromLatin1(qVersion()))
                                      .arg(stats.backend)
                                      .arg(stats.totalFiles)
                                      .arg(QStringLiteral(__DATE__))
                                      .arg(QStringLiteral(__TIME__));
    QMessageBox::about(this, QStringLiteral("关于 FindFaster"), aboutText);
}

void FindFasterWidget::onResultContextMenu(const QPoint &pos)
{
    if (!m_resultModel || !m_resultView->selectionModel()) {
        return;
    }

    QModelIndexList indices = m_resultView->selectionModel()->selectedRows();
    if (indices.isEmpty()) {
        const QModelIndex idx = m_resultView->indexAt(pos);
        if (!idx.isValid()) {
            return;
        }
        indices << idx;
    }

    QStringList paths;
    for (const QModelIndex &idx : indices) {
        const QString p = m_resultModel->pathAtRow(idx.row());
        if (!p.isEmpty()) {
            paths << p;
        }
    }
    paths.removeDuplicates();
    if (paths.isEmpty()) {
        return;
    }

    const QPoint globalPos = m_resultView->viewport()->mapToGlobal(pos);
    showWindowsShellContextMenu(this, paths, globalPos);
}

void FindFasterWidget::openResultFromIndex(const QModelIndex &index)
{
    if (!index.isValid() || !m_resultModel) {
        return;
    }

    const QString filePath = m_resultModel->pathAtRow(index.row());
    if (filePath.isEmpty()) {
        return;
    }

    const QFileInfo fileInfo(filePath);
    if (!fileInfo.exists()) {
        QMessageBox::warning(this,
                             QStringLiteral("文件不存在"),
                             QStringLiteral("目标文件不存在：%1").arg(filePath));
        return;
    }

    const bool opened = QDesktopServices::openUrl(QUrl::fromLocalFile(filePath));
    if (opened) {
        return;
    }

#ifdef Q_OS_WIN
    const bool openAsLaunched = QProcess::startDetached(QStringLiteral("rundll32.exe"),
                                                        QStringList()
                                                            << QStringLiteral("shell32.dll,OpenAs_RunDLL")
                                                            << QDir::toNativeSeparators(filePath));
    if (!openAsLaunched) {
        QMessageBox::warning(this,
                             QStringLiteral("打开失败"),
                             QStringLiteral("无法打开文件，也无法弹出打开方式选择：%1").arg(filePath));
    }
#else
    QMessageBox::warning(this,
                         QStringLiteral("打开失败"),
                         QStringLiteral("无法使用默认方式打开该文件：%1").arg(filePath));
#endif
}

bool FindFasterWidget::matchesDriveFilter(const QString &filePath, const QString &driveFilter) const
{
    if (driveFilter.isEmpty()) {
        return true;
    }

    const QString cleanPath = QDir::cleanPath(filePath).toLower();
    const QString cleanDrive = QDir::cleanPath(driveFilter).toLower();
    if (cleanPath == cleanDrive) {
        return true;
    }
    const QString prefix = cleanDrive.endsWith('/') ? cleanDrive : cleanDrive + '/';
    return cleanPath.startsWith(prefix);
}

bool FindFasterWidget::shouldStartPrefetch(const QString &keyword) const
{
    const QString trimmed = keyword.trimmed();
    if (trimmed.isEmpty()) {
        return false;
    }

    bool hasCjk = false;
    for (const QChar ch : trimmed) {
        const ushort code = ch.unicode();
        if ((code >= 0x4E00 && code <= 0x9FFF) || (code >= 0x3400 && code <= 0x4DBF)) {
            hasCjk = true;
            break;
        }
    }

    const int threshold = hasCjk ? 2 : 3;
    return trimmed.size() >= threshold;
}

bool FindFasterWidget::committedMatch(const FinderSearchResult &result, const QStringList &tokens)
{
    const QString name = result.entry.name.toLower();
    const QString path = result.entry.path.toLower();
    for (const QString &token : tokens) {
        if (!name.contains(token) && !path.contains(token)) {
            return false;
        }
    }
    return true;
}

bool FindFasterWidget::tryApplyPrefetchForCommittedQuery(const QString &keyword)
{
    const QString committed = keyword.trimmed();
    if (committed.isEmpty() || m_prefetchResults.isEmpty()) {
        return false;
    }

    const QString prefetch = m_prefetchKeyword.trimmed();
    if (prefetch.isEmpty()) {
        return false;
    }
    if (!committed.startsWith(prefetch, Qt::CaseInsensitive)
        && committed.compare(prefetch, Qt::CaseInsensitive) != 0) {
        return false;
    }

    QStringList tokens = committed.toLower().simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (tokens.isEmpty()) {
        tokens << committed.toLower();
    }

    QList<FinderSearchResult> filtered;
    filtered.reserve(m_prefetchResults.size());
    for (const FinderSearchResult &result : m_prefetchResults) {
        if (committedMatch(result, tokens)) {
            filtered.push_back(result);
        }
    }

    m_allSearchResults = std::move(filtered);
    m_displayedSearchResults = m_allSearchResults;
    m_progressiveNextIndex = m_displayedSearchResults.size();
    if (m_progressiveRenderTimer && m_progressiveRenderTimer->isActive()) {
        m_progressiveRenderTimer->stop();
    }
    m_hasMorePages = false;
    m_nextPageIndex = 1;
    m_pageLoadInProgress = false;
    applyDisplayResults();
    updateStatus();
    return true;
}

void FindFasterWidget::startProgressiveDisplay()
{
    QElapsedTimer elapsed;
    elapsed.start();
    if (!m_resultModel) {
        return;
    }

    if (m_progressiveRenderTimer->isActive()) {
        m_progressiveRenderTimer->stop();
    }
    scheduleIconResume();

    if (m_allSearchResults.size() <= kFastFullAttachRows) {
        m_displayedSearchResults = m_allSearchResults;
        m_progressiveNextIndex = m_displayedSearchResults.size();
        m_viewportAnchorRow = 0;
        m_resultModel->setExternalSource(&m_displayedSearchResults);
        m_lastApplyDisplayMs = elapsed.elapsed();
        return;
    }

    m_displayedSearchResults.clear();
    const int firstPaintGoal = m_startupTimingActive ? qMax(kFirstPaintRows, 200) : kFirstPaintRows;
    const int firstPaintCount = qMin(firstPaintGoal, m_allSearchResults.size());
    if (firstPaintCount > 0) {
        m_displayedSearchResults.reserve(m_allSearchResults.size());
        for (int i = 0; i < firstPaintCount; ++i) {
            m_displayedSearchResults.push_back(m_allSearchResults.at(i));
        }
    }
    m_progressiveNextIndex = firstPaintCount;
    m_viewportAnchorRow = 0;
    m_resultModel->setExternalSource(&m_displayedSearchResults);
    m_lastApplyDisplayMs = elapsed.elapsed();

    if (m_progressiveNextIndex < m_allSearchResults.size()) {
        if (m_startupTimingActive) {
            m_renderChunkRows = qMax(m_renderChunkRows, 1000);
        }
        m_progressiveRenderTimer->start();
    }
}

void FindFasterWidget::stabilizeFirstRows(QList<FinderSearchResult> *results) const
{
    if (!results || results->isEmpty() || m_displayedSearchResults.isEmpty()) {
        return;
    }

    const int windowSize = qMin(kFirstPaintRows, results->size());
    const int stableHead = qMin(kStableHeadRows, m_displayedSearchResults.size());
    if (windowSize <= 0 || stableHead <= 0) {
        return;
    }

    QHash<QString, int> indexByPath;
    indexByPath.reserve(windowSize);
    for (int i = 0; i < windowSize; ++i) {
        const QString &path = results->at(i).entry.path;
        if (!path.isEmpty()) {
            indexByPath.insert(path, i);
        }
    }

    QList<FinderSearchResult> pinned;
    pinned.reserve(stableHead);
    QSet<QString> pinnedPaths;
    pinnedPaths.reserve(stableHead);

    for (int i = 0; i < stableHead; ++i) {
        const FinderSearchResult &oldRow = m_displayedSearchResults.at(i);
        const auto found = indexByPath.constFind(oldRow.entry.path);
        if (found == indexByPath.constEnd()) {
            continue;
        }

        const FinderSearchResult &candidate = results->at(found.value());
        if (pinnedPaths.contains(candidate.entry.path)) {
            continue;
        }
        pinned.push_back(candidate);
        pinnedPaths.insert(candidate.entry.path);
    }

    if (pinned.isEmpty()) {
        return;
    }

    QList<FinderSearchResult> reordered;
    reordered.reserve(results->size());
    for (const FinderSearchResult &row : pinned) {
        reordered.push_back(row);
    }
    for (const FinderSearchResult &row : *results) {
        if (!pinnedPaths.contains(row.entry.path)) {
            reordered.push_back(row);
        }
    }
    *results = std::move(reordered);
}

void FindFasterWidget::onProgressiveRenderTick()
{
    QElapsedTimer elapsed;
    elapsed.start();
    if (!m_resultModel || !m_driveFilterCombo) {
        if (m_progressiveRenderTimer->isActive()) {
            m_progressiveRenderTimer->stop();
        }
        return;
    }
    if (!m_driveFilterCombo->currentData().toString().isEmpty()) {
        m_progressiveRenderTimer->stop();
        return;
    }
    if (m_progressiveNextIndex >= m_allSearchResults.size()) {
        m_progressiveRenderTimer->stop();
        return;
    }

    const int firstRow = m_displayedSearchResults.size();

    int viewportTop = -1;
    int viewportBottom = -1;
    int targetEnd = m_progressiveNextIndex + m_renderChunkRows;
    if (m_resultView) {
        viewportTop = m_resultView->rowAt(0);
        viewportBottom = m_resultView->rowAt(m_resultView->viewport()->height() - 1);
        if (viewportBottom >= 0) {
            targetEnd = qMax(targetEnd, viewportBottom + kViewportAheadRows);
        }
    }
    if (viewportTop >= 0) {
        // 上方回补 + 下方预取：优先保证视口上下缓冲区可用，来回拖动更平滑。
        const int lowerTarget = viewportBottom + kViewportPrefetchRows;
        const int upperTarget = qMax(0, viewportTop - kViewportBackfillRows);
        targetEnd = qMax(targetEnd, lowerTarget);
        targetEnd = qMax(targetEnd, upperTarget + kViewportBackfillRows + kViewportPrefetchRows);
    } else if (m_viewportAnchorRow > 0) {
        targetEnd = qMax(targetEnd, m_viewportAnchorRow + kViewportPrefetchRows);
    }
    const int nextEnd = qMin(targetEnd, m_allSearchResults.size());
    for (int i = m_progressiveNextIndex; i < nextEnd; ++i) {
        m_displayedSearchResults.push_back(m_allSearchResults.at(i));
    }
    m_progressiveNextIndex = nextEnd;

    const int lastRow = m_displayedSearchResults.size() - 1;
    if (firstRow <= lastRow) {
        m_resultModel->notifyExternalRowsInserted(firstRow, lastRow);
    }

    if (m_progressiveNextIndex >= m_allSearchResults.size()) {
        m_progressiveRenderTimer->stop();
    }
    m_lastApplyDisplayMs = elapsed.elapsed();
    adaptRenderChunkRows();
    updateStatus();
}
