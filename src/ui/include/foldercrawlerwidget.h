/*
 * Copyright (C) 2026 Anton Filimonov and other contributors
 *
 * This file is part of klogg.
 *
 * klogg is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * klogg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with klogg.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef FOLDERCRAWLERWIDGET_H
#define FOLDERCRAWLERWIDGET_H

#include <QHash>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

#include "linetypes.h"
#include "markprovider.h"
#include "colorlabelscontroller.h"
#include "foldersearchresults.h"
#include "quickfindmux.h"
#include "regularexpressionpattern.h"
#include "viewsignalwiring.h"
#include "abstractcrawlerwidget.h"

#include <QWidget>

#include "overview.h"

class AbstractLogView;
class FolderFilteredView;
class FolderSearchEngine;
class FolderSearchResults;
class LogMainView;
class LogData;
class OverviewWidget;
class QuickFindPattern;
class QSplitter;
class QShortcut;
class QLabel;
class QToolButton;
class QComboBox;
class QSpinBox;
class QTabWidget;
class SavedSearches;
class SearchToolbar;

enum class DataStatus;

// The document widget for "Open Folder" mode: a vertical splitter with a small
// search toolbar, the (initially empty) main view on top, and the folder
// results view (grouped, collapsible per file) on the bottom.
//
//   search toolbar:  [ pattern........ ] [Search] [Stop] [Collapse all] [Expand all]   N matches
//   -----------------------------------------------------------------------
//   main view (LogMainView): empty until a result row is clicked; then shows
//                            that row's source file at the line.
//   -----------------------------------------------------------------------
//   filtered view (FolderFilteredView): grouped results, click a header to
//                                       collapse/expand, click a row to open.
//
// Search is streaming + index-less (FolderSearchEngine). Selecting a match
// opens its source file in the main view: the file is indexed on demand (the
// only place klogg's index is used in folder mode) and cached so re-selecting a
// file is instant.
class FolderCrawlerWidget : public QWidget,
                           public AbstractCrawlerWidget,
                           public QuickFindMuxSelectorInterface {
    Q_OBJECT

  public:
    explicit FolderCrawlerWidget( QWidget* parent = nullptr );
    ~FolderCrawlerWidget() override;

    FolderCrawlerWidget( const FolderCrawlerWidget& ) = delete;
    FolderCrawlerWidget& operator=( const FolderCrawlerWidget& ) = delete;

    // Identify the folder + its (already enumerated, natural-sorted) files.
    void setFolder( const QString& folderPath, const QStringList& filePaths );

    // --- Test access / programmatic driving (no UI events needed) ---
    // folderResults()/filteredView() return the ACTIVE pane's objects (keeps
    // existing single-pane callers + tests working now that results live in a
    // tabbed set of panes).
    FolderSearchResults* folderResults() const { return activeResults(); }
    FolderFilteredView* filteredView() const { return activeFilteredView(); }
    int paneCount() const { return static_cast<int>( panes_.size() ); }
    QTabWidget* resultsTabs() const { return resultsTabs_; }
    LogMainView* mainView() const { return mainView_; }
    // Read-only access to the toolbar (pattern text + option toggles). Used by
    // tests to assert view-context round-trip and to drive toggle state.
    SearchToolbar* searchToolbar() const { return searchToolbar_; }
    // Mutable access for test-driving (e.g. forcing updateView); const variant
    // below for read-only inspection.
    Overview* overview() { return &overview_; }
    const Overview* overview() const { return &overview_; }
    QString currentMainFilePath() const { return currentMainFilePath_; }
    // The last line announced for the file in the main view (the jump target on
    // open). Lets MainWindow restore the "Ln: x/y" field when switching back to
    // a folder tab that already has a file open (single-file tabs get this via
    // the signalMux state broadcast; the folder is intentionally not a mux
    // document, so the broadcast never fires for it).
    LineNumber currentMainViewLine() const { return lastMainViewLine_; }
    // Search-toolbar status text (file count / match count / search state).
    // Exposed so tests can assert no file path leaks into the toolbar.
    QString statusText() const;
    // True if `line` is marked in the file currently shown in the main view.
    // (Test accessor for folder-mode marks, which live in the per-file store.)
    bool isMainViewLineMarked( LineNumber line ) const;
    // True if `line` is marked in `filePath` (the shared per-file mark store).
    // Test accessor for session-persistence coverage.
    bool isLineMarkedInFile( const QString& filePath, LineNumber line ) const;
    // The overview model feeding the minimap (test seam for mark/match ticks).
    Overview* overviewModel() { return &overview_; }
    // True if the result row (a filtered-view line index) is marked -- resolves
    // the row to (file, localLine) via the results model and checks the shared
    // per-file mark store. Test accessor for filtered-view marks.
    bool isFilteredResultRowMarked( LineNumber row ) const;
    // Programmatic mark toggle on a line of the file currently in the main view
    // (the M shortcut / left-margin click do the same through the markLines
    // signal). Exposed for test-driving; forceRefresh re-renders the bullet.
    void markMainViewLine( LineNumber line );
    void unmarkMainViewLine( LineNumber line );
    // True while a search is running (cleared on searchFinished). Lets
    // integration tests wait for completion before asserting exact counts.
    bool isSearchActive() const { return searchActive_; }
    // Set the results-view visibility filter (Marks / Marks and matches /
    // Matches). Public so tests can drive it without manipulating the combo.
    void setResultsVisibility( FolderSearchResults::Visibility visibility );
    // Test accessors for the grep -A/-B/-C context controls.
    QComboBox* contextLinesComboBox() const { return contextLinesComboBox_; }
    QSpinBox* contextLinesSpinBox() const { return contextLinesSpinBox_; }
    std::pair<int, int> currentContext() const { return { contextBefore_, contextAfter_ }; }
    // Test seams for the widget-level shortcut family (F3): the visibility
    // combo the CrawlerChangeVisibility* shortcuts drive, and the splitter the
    // top-view-size shortcuts resize.
    QComboBox* visibilityCombo() const { return visibilityBox_; }
    QSplitter* viewsSplitter() const { return splitter_; }

    // Restores the status label to the pre-search "Ready (N file(s))" text.
    void updateReadyStatus();
    // Set the pattern and kick off a search (async; results land via the
    // searchFinished signal).
    void searchFor( const QString& pattern );
    // Simulate selecting a result row (opens its source file in the main view).
    void selectResultRow( LineNumber line );
    // Simulate clicking a group-header row (toggles that group's collapse).
    void clickHeaderRow( LineNumber line );

    // Collapse / expand every group (wired to the toolbar buttons; public so
    // they can be driven programmatically / from tests).
    void collapseAll();
    void expandAll();

    // Re-apply Configuration (line numbers, font, overview, shortcuts) to both
    // views. Called on construction and on MainWindow::optionsChanged (the
    // View-menu toggles for line numbers / overview / wrap). Overrides
    // AbstractCrawlerWidget.
    void applyConfiguration() override;
    // Register view-level keyboard shortcuts on both views (arrow keys, PgUp/
    // PgDn, jump-to-top/bottom, ...) plus the shared widget-level crawler
    // family. Overrides AbstractCrawlerWidget.
    void registerShortcuts() override;
    // Grow/shrink the main view within splitter_ (the CrawlerIncreseTopViewSize/
    // CrawlerDecreaseTopViewSize shortcuts drive this).
    void changeTopViewSize( int delta );

    // Edit-menu dispatch (AbstractCrawlerWidget): copy/selectAll delegate to the
    // focused view, so MainWindow::copy/selectAll work on folder tabs instead of
    // being enabled no-ops (currentCrawlerWidget() is null for a folder tab).
    QString getSelectedText() const override;
    bool isPartialSelection() const override;
    void selectAll() override;
    // Apply the chosen encoding to the file currently open in the main view
    // (no-op until a file is opened). Overrides AbstractCrawlerWidget.
    void setEncoding( std::optional<int> mib ) override;
    // The encoding override applied via setEncoding (nullopt = auto-detect).
    // Reset when the main-view file changes.
    std::optional<int> encodingMib() const override;
    // Snapshot of the file currently in the main view, for MainWindow's info
    // line (path/size/date/encoding/line-count). Nullopt when no file is open.
    std::optional<MainViewInfo> currentMainViewInfo() const override;

    // Document-level actions MainWindow dispatches polymorphically to every
    // tab kind (single-file tabs are reached via the same virtuals).
    void focusSearchEdit() override;
    void goToLine() override;
    void textWrapSet( bool checked ) override;
    bool isTextWrapEnabled() const override;
    // View -> Go to top / Follow file, applied to the file shown in the MAIN
    // view only: the results view is a cross-file static snapshot, so topping
    // or following it is meaningless (the folder-specific variation of
    // CrawlerWidget, which drives both views).
    void jumpToTop() override;
    void followSet( bool checked ) override;
    bool isFollowEnabled() const override;
    void enteringQuickFind() override;
    void exitingQuickFind() override;

  Q_SIGNALS:
    // Required by TabbedCrawlerWidget::addCrawler (template expects this
    // signal). Folder tabs are static, so this is never emitted for now.
    void dataStatusChanged( DataStatus status );
    // Emitted when the file shown in the main view changes (a result is opened
    // or the encoding is changed) so MainWindow refreshes the info line.
    void mainViewFileChanged();
    // Scratchpad forwarding (single-file parity): emitted from the views'
    // context-menu scratchpad actions via the shared ViewSignalWiring;
    // MainWindow direct-connects these to its scratchpad slots for folder tabs
    // (single-file tabs reach them via SignalMux).
    void sendToScratchpad( QString text );
    void replaceDataInScratchpad( QString text );
    // Emitted when the set of QuickFind-searchable views changes (a results
    // pane is created, switched, or closed) so MainWindow re-registers the
    // selector with the QuickFindMux instead of driving a stale/freed pane.
    void searchablesChanged();
    // Emitted when the main view's follow mode changes (the view's elastic
    // hook disengages on scroll-up, or re-engages at the bottom). Single-file
    // parity with CrawlerWidget::followModeChanged: MainWindow direct-connects
    // this to changeFollowMode so the Follow action's checked state tracks the
    // view (single-file tabs reach that slot via SignalMux).
    void followModeChanged( bool follow );

  protected:
    // ViewInterface (single-file APIs are no-ops in folder mode).
    void doSetData( std::shared_ptr<SearchableLogData> log_data,
                    std::shared_ptr<LogFilteredData> filtered_data ) override;
    void doSetQuickFindPattern( std::shared_ptr<QuickFindPattern> qfp ) override;
    void doSetSavedSearches( SavedSearches* saved_searches ) override;
    void doSetViewContext( const QString& view_context ) override;
    std::shared_ptr<const ViewContextInterface> doGetViewContext() const override;

    // QuickFindMuxSelectorInterface -- the folder's main + filtered views are
    // the searchables; Ctrl+F dispatches to the focused one (or the filtered
    // view by default) via activeView().
    SearchableWidgetInterface* doGetActiveSearchable() const override;
    std::vector<QObject*> doGetAllSearchables() const override;

    // Seeds the splitter from the saved global default the moment the splitter
    // first gets real geometry (its first Resize event). setSizes any earlier
    // (ctor / showEvent / zero-delay timer) is discarded by the splitter's
    // initial layout, which clamps the bottom pane to its minimum size.
    bool eventFilter( QObject* obj, QEvent* event ) override;

    // Re-captures the status label's default palette on StyleChange (mirrors
    // CrawlerWidget's changeEvent) so the invalid-pattern error restore tracks
    // runtime theme switches.
    void changeEvent( QEvent* event ) override;

  private Q_SLOTS:
    void startSearch();
    void stopSearch() override;
    void onSearchStarted( quint64 generation );
    void onSearchProgressed( quint64 nbMatches, int percent, quint64 generation );
    void onSearchFinished( quint64 generation );
    void onFileGroupReady( int fileIndex, quint64 generation );
    void onResultSelected( LineNumber line, LinesCount nLines, LineColumn startCol,
                           LineLength nSymbols );
    void onHeaderClicked( LineNumber line );

  private:
    // The view that should receive copy/selectAll/quickfind: the focused view if
    // one of ours has focus, else the results view (the primary folder surface).
    // Mirrors CrawlerWidget::activeView (crawlerwidget.cpp:1017).
    AbstractLogView* activeView() const;
    void openFileInMainView( const QString& filePath, LineNumber localLine,
                             LinesCount nLines = LinesCount( 1 ),
                             LineColumn startCol = LineColumn( 0 ),
                             LineLength nSymbols = LineLength( 0 ) );
    // (Re)bind the follow/refresh data-flow of the CURRENT currentMainData_ to
    // the main view: the per-file LogData self-registers with FileWatcher and
    // re-indexes on growth, but nothing else forwards those notifications to
    // mainView_, so without this follow mode would set a flag and never track.
    // Called on every main-view file swap (cache hit and async load).
    void bindMainViewDataSignals();
    // Select (and display) a line or portion in the main view: whole-line jump
    // for plain row clicks, portion mirror for drag selections (parity with
    // single-file jumpToMatchingLine).
    void selectInMainView( LineNumber line, LinesCount nLines, LineColumn startCol,
                           LineLength nSymbols );
    // Resolve a hovered results row to (file, localLine) via the pane owning
    // the view and highlight it in the overview when that file is open
    // (single-file mouseHoveredOverMatch parity).
    void highlightOverviewForRow( const AbstractLogView* view, LineNumber row );
    void cacheMainViewData( const QString& filePath, std::shared_ptr<LogData> data );
    // Re-point the per-file overview at the opened file: that file's folder-search
    // matches become the Overview marks, and linesInFile_ is sized to the file so
    // the viewport box + click-to-jump map within the file. No-op until a file is
    // loaded into currentMainData_. Folder mode uses an explicit match-line list
    // (no LogFilteredData), so this is a pure else-branch on the Overview.
    void refreshFileOverview( const QString& filePath );
    // Filter favorites + predefined filters (mirror CrawlerWidget; the host owns
    // the dialogs + the shared PredefinedFiltersCollection persistence).
    void saveAsFavorite();
    void updatePredefinedFiltersWidget();
    void reloadPredefinedFilters() const;
    // Search-history context-menu actions (parity with CrawlerWidget): the
    // toolbar emits clearHistoryRequested / editHistoryRequested; the host owns
    // the shared SavedSearches + dialogs.
    void clearSearchHistory();
    void editSearchHistory();
    // Sync the main view's display codec to the indexer-detected encoding for
    // the file currently in the main view (parity with
    // CrawlerWidget::updateEncoding). Without it a non-UTF-8 file is indexed
    // with correct line positions but displayed decoded as UTF-8 (mojibake) and
    // the info line wrongly reports UTF-8.
    void applyDetectedEncoding();

    QString folderPath_;
    QStringList filePaths_;
    std::shared_ptr<QuickFindPattern> quickFindPattern_;

    // Per-tab color labels (context menu + 1..9/0 shortcuts -> quick
    // highlighters in every view). Shared component with CrawlerWidget;
    // constructed early so the main view and every results pane can be
    // watchView()'d as they are created.
    ColorLabelsController colorLabelsController_;

    // Shared view-signal wiring (scratchpad / search composition / splitter /
    // font zoom / exitView / highlightersChange / hover) -- unique_ptr because
    // it needs searchToolbar_, created in the ctor body. Same component
    // CrawlerWidget uses (single-file parity by construction).
    std::unique_ptr<ViewSignalWiring> viewSignalWiring_;

    FolderSearchEngine* engine_ = nullptr;
    LogMainView* mainView_ = nullptr;
    QSplitter* splitter_ = nullptr;

    // Widget-level shortcuts (crawler family from klogg::registerCrawlerShortcuts).
    std::map<QString, QShortcut*> shortcuts_;

    // Splitter seed state: on the splitter's first Resize (its first real
    // geometry) the proportions are seeded once -- from the session-restored
    // per-tab sizes when setViewContext provided them, else from the saved
    // global default. setSizes any earlier (ctor / showEvent / zero-delay
    // timer) is discarded by the splitter's initial layout, which clamps the
    // bottom pane to its minimum size.
    bool splitterSeedApplied_ = false;
    QList<int> pendingSplitterSizes_;

    // Results are shown in a tabbed set of panes (Keep results in a new window).
    // Each pane owns its own FolderSearchResults + FolderFilteredView + mark
    // provider; the main view is shared. Pane index == resultsTabs_ tab index
    // (append-only + same-index erase keeps them 1:1). markProvider is a
    // unique_ptr because FolderFilteredMarkProvider is defined later in this
    // class (incomplete here).
    class FolderFilteredMarkProvider;
    struct ResultPane {
        ResultPane();
        ~ResultPane();
        std::shared_ptr<FolderSearchResults> results;
        FolderFilteredView* view = nullptr;
        std::unique_ptr<FolderFilteredMarkProvider> markProvider;
        QString title;
    };
    std::vector<std::unique_ptr<ResultPane>> panes_;
    int activePaneIndex_ = -1;
    // The pane the in-flight search streams into. Set in startSearch BEFORE
    // engine_->startSearch; the streaming handlers (onFileGroupReady/
    // onSearchFinished) write here, NOT necessarily the active pane (the user
    // may switch tabs mid-search). Null when no search is active.
    FolderSearchResults* searchTargetResults_ = nullptr;
    QTabWidget* resultsTabs_ = nullptr;
    // Active-pane accessors (clicks come from the visible tab). folderResults()/
    // filteredView() wrap these so existing call sites + tests keep working.
    FolderSearchResults* activeResults() const
    {
        return ( activePaneIndex_ >= 0 && activePaneIndex_ < static_cast<int>( panes_.size() ) )
                   ? panes_[ static_cast<size_t>( activePaneIndex_ ) ]->results.get()
                   : nullptr;
    }
    FolderFilteredView* activeFilteredView() const
    {
        return ( activePaneIndex_ >= 0 && activePaneIndex_ < static_cast<int>( panes_.size() ) )
                   ? panes_[ static_cast<size_t>( activePaneIndex_ ) ]->view
                   : nullptr;
    }

    // LogMainView requires a (non-null) Overview/OverviewWidget pair. In folder
    // mode the Overview is driven per-file: when a result is opened, that file's
    // folder-search matches become the overview marks and linesInFile_ is sized to
    // the file. The widget is re-parented to mainView_ so AbstractLogView places
    // it; visibility follows Configuration::isOverviewVisible() (mirrors
    // CrawlerWidget).
    Overview overview_;
    OverviewWidget* overviewWidget_ = nullptr;

    // Shared search toolbar (replaces the former inline QLineEdit + hand-built
    // Search/Stop buttons). Folder mode passes a null SavedSearches (no
    // history); the toolbar guards that. Collapse/expand buttons live
    // alongside the toolbar in this widget's own layout.
    SearchToolbar* searchToolbar_ = nullptr;
    // Session-wide search history (injected via doSetSavedSearches); folder
    // searches are recorded into it so recent grep patterns appear in the
    // dropdown. Null if the host never injects one.
    SavedSearches* savedSearches_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    // Palette captured right after statusLabel_ is created; restored on every
    // new search to clear the invalid-pattern error highlight.
    QPalette statusLabelDefaultPalette_;
    // True while the status label shows an invalid-pattern error (dark-yellow
    // highlight); guards the StyleChange palette re-capture so the error
    // palette is never snapshotted as the "default".
    bool statusErrorActive_ = false;
    // Encoding override applied via setEncoding (nullopt = auto-detect);
    // reset when the main-view file changes (the override is per shown file).
    std::optional<int> encodingMibOverride_;
    // View that had focus when the QuickFind bar opened (enteringQuickFind);
    // restored on exitingQuickFind. Mirrors CrawlerWidget::qfSavedFocus_.
    QPointer<QWidget> qfSavedFocus_;
    // Last finished search's result text ("No matches" / "N match(es)");
    // re-shown with the stale hint when an option toggle invalidates it.
    QString lastResultStatusText_;
    QToolButton* collapseAllButton_ = nullptr;
    QToolButton* expandAllButton_ = nullptr;
    // Results-view visibility filter (Marks / Marks and matches / Matches),
    // mirroring CrawlerWidget's visibility combo.
    QComboBox* visibilityBox_ = nullptr;
    // grep -A/-B/-C context controls + resolved window (mirrors CrawlerWidget's
    // contextLinesSpinBox_/contextLinesComboBox_).
    QComboBox* contextLinesComboBox_ = nullptr;
    QSpinBox* contextLinesSpinBox_ = nullptr;
    int contextBefore_ = 0;
    int contextAfter_ = 0;

    // Main-view file data: a placeholder (empty) until a row is clicked, then
    // the selected file's LogData. Recently-used files are cached (true LRU,
    // MainViewCacheLimit entries) so switching between files is instant.
    std::shared_ptr<LogData> placeholderData_;
    std::shared_ptr<LogData> currentMainData_;
    QString currentMainFilePath_;
    // Connections bound by bindMainViewDataSignals to the CURRENT
    // currentMainData_. Stored so they can be disconnected before rebinding:
    // LRU-cached LogDatas keep their FileWatcher registration, so stale
    // connections would fire mainView_->updateData() (and the truncation
    // handler) for files that are no longer shown.
    QMetaObject::Connection mainDataLoadingFinishedConn_;
    QMetaObject::Connection mainDataLoadingProgressedConn_;
    QMetaObject::Connection mainDataFileChangedConn_;
    // The last line opened/announced in the main view (jump target). Tracked so
    // MainWindow can restore "Ln: x/y" when switching back to this folder tab.
    LineNumber lastMainViewLine_ = 0_lnum;
    // value: { data, iterator into mainViewCacheOrder_ }; order tracks access
    // recency (front = most-recently-used). std::list iterators stay valid on
    // splice/erase, so the map is never invalidated by reordering. QHash (not
    // std::unordered_map) so it works on Qt 5, which does not provide
    // std::hash<QString>.
    QHash<QString, std::pair<std::shared_ptr<LogData>, std::list<QString>::iterator>>
        mainViewCache_;
    std::list<QString> mainViewCacheOrder_;
    static constexpr size_t MainViewCacheLimit = 8;

    // Pending async load (file clicked before its index finished building).
    std::shared_ptr<LogData> pendingMainData_;
    QString pendingMainFilePath_;
    // The one-shot loadingFinished connection of the CURRENT pendingMainData_.
    // Its lifetime is tied to the open it serves: the lambda self-disconnects
    // when the open completes, and any newer open (async supersede or cached
    // swap) disconnects it before replacing/abandoning the pending state.
    // Without this the connection would outlive its open: the completed
    // LogData moves into the LRU cache and keeps re-indexing on disk changes
    // (self-registered FileWatcher), so its stale lambda would fire while a
    // DIFFERENT file's open is in flight (pendingMainData_ non-null) and run
    // the completion body with the wrong file's half-indexed state --
    // clobbering currentMainData_/currentMainFilePath_ and consuming the
    // pending jump, so the real completion early-returns and the new file's
    // jump/overview/encoding are never applied with the final index state.
    QMetaObject::Connection pendingMainDataConn_;
    LineNumber pendingJumpLine_ = 0_lnum;
    // Portion to mirror into the main view once the pending file is open
    // (plain row clicks use the defaults -> whole-line selection).
    LinesCount pendingJumpNLines_ = LinesCount( 1 );
    LineColumn pendingJumpCol_ = LineColumn( 0 );
    LineLength pendingJumpLen_ = LineLength( 0 );

    // Current search generation: streaming fileGroupReady/searchFinished signals
    // whose generation differs are dropped (superseded by a newer search).
    quint64 currentSearchGeneration_ = 0;
    bool searchActive_ = false;

    // The last search pattern run by startSearch. Forwarded to mainView_ so that
    // when a result is clicked and its file opens, the matched substring is
    // highlighted in the opened file (single-file parity). Stored here so the
    // pattern can be re-applied right after each setDataSource swap in
    // openFileInMainView (setDataSource does not reset searchPattern_, but the
    // re-apply makes the parity intent explicit and is robust to future changes).
    RegularExpressionPattern currentSearchPattern_;

    // --- folder-mode marks (no LogFilteredData) ---
    // file path -> sorted set of marked line numbers (stored as the underlying
    // value). Injected into the main view via mainViewMarkProvider_ so the mark
    // bullet (LineTypeFlags::Mark) and Next/Prev-mark navigation work, and kept
    // per-file so marks survive switching result files within a session.
    QHash<QString, std::set<uint64_t>> folderMarks_;
    // MarkProvider over folderMarks_ for the file currently in the main view;
    // reads currentMainFilePath_ live so it stays correct across file swaps.
    class MainViewMarkProvider : public MarkProvider {
      public:
        const QHash<QString, std::set<uint64_t>>* marks = nullptr;
        const QString* currentFile = nullptr;
        bool isMarked( LineNumber line ) const override
        {
            if ( marks == nullptr || currentFile == nullptr ) {
                return false;
            }
            const auto it = marks->find( *currentFile );
            return it != marks->end() && it->count( line.get() ) > 0;
        }
        std::optional<LineNumber> markAfter( LineNumber line ) const override
        {
            if ( marks == nullptr || currentFile == nullptr ) {
                return {};
            }
            const auto it = marks->find( *currentFile );
            if ( it == marks->end() ) {
                return {};
            }
            const auto& s = it.value();
            const auto ub = s.upper_bound( line.get() );
            if ( ub == s.end() ) {
                return {};
            }
            return LineNumber( *ub );
        }
        std::optional<LineNumber> markBefore( LineNumber line ) const override
        {
            if ( marks == nullptr || currentFile == nullptr ) {
                return {};
            }
            const auto it = marks->find( *currentFile );
            if ( it == marks->end() ) {
                return {};
            }
            const auto& s = it.value();
            auto lb = s.lower_bound( line.get() );
            if ( lb == s.begin() ) {
                return {};
            }
            --lb;
            return LineNumber( *lb );
        }
    };
    MainViewMarkProvider mainViewMarkProvider_;

    // MarkProvider over folderMarks_ for the folder RESULTS (filtered) view,
    // where a view row resolves to (file, localLine) via FolderSearchResults.
    // Reads folderMarks_ live so marks are shared with the main view. The
    // results pointer is stable (folderResults_ is created once and mutated in
    // place across searches).
    class FolderFilteredMarkProvider : public MarkProvider {
      public:
        const QHash<QString, std::set<uint64_t>>* marks = nullptr;
        const FolderSearchResults* results = nullptr;
        bool isMarked( LineNumber row ) const override
        {
            if ( marks == nullptr || results == nullptr ) {
                return false;
            }
            if ( results->lineKind( row ) != LineKind::Data ) {
                return false;
            }
            const auto src = results->sourceForLine( row );
            const auto it = marks->find( src.filePath );
            return it != marks->end() && it->count( src.localLine.get() ) > 0;
        }
        std::optional<LineNumber> markAfter( LineNumber row ) const override
        {
            if ( results == nullptr ) {
                return {};
            }
            const uint64_t total = results->getNbLine().get();
            for ( uint64_t i = row.get() + 1; i < total; ++i ) {
                const LineNumber r{ i };
                if ( isMarked( r ) ) {
                    return r;
                }
            }
            return {};
        }
        std::optional<LineNumber> markBefore( LineNumber row ) const override
        {
            if ( results == nullptr ) {
                return {};
            }
            const uint64_t start = row.get();
            for ( uint64_t i = start; i-- > 0; ) {
                const LineNumber r{ i };
                if ( isMarked( r ) ) {
                    return r;
                }
            }
            return {};
        }
    };
    void addMark( const QString& file, LineNumber line );
    void removeMark( const QString& file, LineNumber line );
    // filteredView_->markLines / deleteMarkLines handlers: resolve each result
    // row to (file, localLine) via the results model and update the shared
    // per-file store, then refresh so the bullet re-renders.
    void onFilteredViewMarkLines( const klogg::vector<LineNumber>& rows );
    void onFilteredViewDeleteMarkLines( const klogg::vector<LineNumber>& rows );
    // Refresh every pane (results + view) for a mark change: marks live in the
    // shared folderMarks_ store, so a mark added in one pane must repaint/rebuild
    // in ALL panes (frozen ones too), not just the active one.
    void refreshAllPanesForMarks();
    // visibilityBox_ currentIndexChanged handler -> FolderSearchResults setVisibility.
    void changeFilteredViewVisibility( int index );
    // Resolve (before, after) from the context combo+spinbox into contextBefore_/
    // contextAfter_, mirroring CrawlerWidget::applyContextLines.
    void updateContextFromControls();
    // Context combo/spinbox changed -> recompute (before,after) and, if a pattern
    // is present, re-run the search (context is a scan-time property).
    void onContextControlsChanged();
    // Results-pane management (Keep results in a new window).
    ResultPane* createPane( const QString& title );
    void onActivePaneChanged( int tabIndex );
    void onClosePane( int tabIndex );
};

#endif // FOLDERCRAWLERWIDGET_H
