#ifndef FINDFASTERWIDGET_H
#define FINDFASTERWIDGET_H

#include "libfinder.h"

#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QList>
#include <QMainWindow>
#include <QModelIndex>
#include <QPoint>

#include <atomic>
#include <memory>

class QLabel;
class QLineEdit;
class QTableView;
class QTimer;
class QComboBox;
class QScrollBar;
class QShowEvent;
class FinderResultsModel;

struct FinderSearchJob
{
    int token = 0;
    bool forDisplay = true;
    QString keyword;
    FinderSearchOutcome outcome;
};

Q_DECLARE_METATYPE(FinderSearchJob)

class FindFasterWidget : public QMainWindow
{
    Q_OBJECT

public:
    FindFasterWidget(QWidget *parent = nullptr);
    ~FindFasterWidget();

protected:
    void showEvent(QShowEvent *event) override;

private slots:
    void refreshSearch();
    void onSearchCommitted();
    void updateStatus();
    void onMainSearchFinished();
    void onPageSearchFinished();
    void openResultFromIndex(const QModelIndex &index);
    void onDriveFilterChanged(int index);
    void onScrollLoadMoreDebounce();
    void onMaybeLoadMore();
    void onResultContextMenu(const QPoint &pos);
    void exportResultsToCsv();
    void showAboutDialog();

private:
    static constexpr int kPrefetchPageSize = 50000;
    static constexpr int kInitialDisplayPageSize = 3000;
    static constexpr int kFirstFramePageSize = 400;
    static constexpr int kFirstPaintRows = 50;
    static constexpr int kRenderChunkRows = 200;
    static constexpr int kStableHeadRows = 20;
    void buildUi();
    void buildMenus();
    void runDeferredIndexBuild();
    FinderIndexOptions prepareDefaultIndexOptions() const;
    void submitSearch(const FinderSearchRequest &request, bool forDisplay);
    void beginSearchNow(const FinderSearchRequest &request, bool forDisplay);
    void maybeContinueInitialWarmup();
    QString startupMetricsText() const;
    void logStartupMetricsIfReady();
    void applyDisplayResults();
    void startLoadNextPage();
    void startProgressiveDisplay();
    void onProgressiveRenderTick();
    void stabilizeFirstRows(QList<FinderSearchResult> *results) const;
    bool matchesDriveFilter(const QString &filePath, const QString &driveFilter) const;
    bool shouldStartPrefetch(const QString &keyword) const;
    bool tryApplyPrefetchForCommittedQuery(const QString &keyword);
    static bool committedMatch(const FinderSearchResult &result, const QStringList &tokens);

    Libfinder m_engine;
    QLineEdit *m_searchEdit = nullptr;
    QComboBox *m_driveFilterCombo = nullptr;
    QTableView *m_resultView = nullptr;
    FinderResultsModel *m_resultModel = nullptr;
    QLabel *m_leftStatus = nullptr;
    QLabel *m_rightStatus = nullptr;
    QTimer *m_searchDebounceTimer = nullptr;
    QTimer *m_loadMoreDebounceTimer = nullptr;
    QTimer *m_progressiveRenderTimer = nullptr;

    QFutureWatcher<FinderSearchJob> *m_mainSearchWatcher = nullptr;
    QFutureWatcher<FinderSearchJob> *m_pageSearchWatcher = nullptr;

    std::shared_ptr<std::atomic<bool>> m_searchCancel;
    int m_searchToken = 0;

    QList<FinderSearchResult> m_allSearchResults;
    QList<FinderSearchResult> m_displayedSearchResults;
    int m_progressiveNextIndex = 0;
    FinderSearchRequest m_lastSearchRequest;
    FinderSearchRequest m_queuedRequest;
    bool m_queuedForDisplay = true;
    bool m_hasQueuedRequest = false;
    QString m_prefetchKeyword;
    QList<FinderSearchResult> m_prefetchResults;
    bool m_prefetchHasMore = false;
    bool m_prefetchInProgress = false;
    bool m_prefetchHitForCurrentQuery = false;

    bool m_hasMorePages = false;
    int m_nextPageIndex = 1;
    bool m_pageLoadInProgress = false;

    bool m_deferredIndexScheduled = false;
    bool m_indexLoading = false;
    bool m_startupTimingActive = false;
    bool m_startupMetricsLogged = false;
    QElapsedTimer m_startupLoadTimer;
    qint64 m_previewDecodeMs = -1;
    qint64 m_previewCommitMs = -1;
    qint64 m_fullDecodeMs = -1;
    qint64 m_firstVisibleMs = -1;
};
#endif // FINDFASTERWIDGET_H
