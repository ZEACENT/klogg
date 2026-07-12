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

#include "foldercrawlerwidget.h"

#include <QApplication>
#include <QComboBox>
#include <QFont>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QSplitter>
#include <QToolButton>
#include <QVBoxLayout>

#include "abstractlogview.h"
#include "configuration.h"
#include "folderfilteredview.h"
#include "foldersearchengine.h"
#include "foldersearchresults.h"
#include "loadingstatus.h"
#include "log.h"
#include "logdata.h"
#include "logmainview.h"
#include "overviewwidget.h"
#include "quickfindpattern.h"
#include "regularexpressionpattern.h"
#include "searchablelogdata.h"
#include "searchtoolbar.h"

// Implementation of the view context for FolderCrawlerWidget (mirrors
// CrawlerWidgetContext in crawlerwidget.cpp:96-170). Serializes the search
// pattern text + option toggles (+ optional splitter sizes) so folder tabs can
// be persisted and restored across sessions. doSetViewContext restores the
// pattern + toggles WITHOUT auto-running a search (one Enter re-runs), matching
// the requirement that restoring a session does not kick off heavy work.
class FolderCrawlerContext : public ViewContextInterface {
  public:
    // Construct from the stored JSON string (forwarded by setViewContext).
    explicit FolderCrawlerContext( const QString& json )
    {
        loadFromJson( json );
    }

    // Construct from the live widget state (used by doGetViewContext).
    FolderCrawlerContext( QString pattern, bool ignoreCase, bool useRegexp, bool inverse,
                          bool boolean, QList<int> sizes )
        : pattern_{ std::move( pattern ) }
        , ignoreCase_{ ignoreCase }
        , useRegexp_{ useRegexp }
        , inverse_{ inverse }
        , boolean_{ boolean }
        , sizes_{ std::move( sizes ) }
    {
    }

    QString toString() const override
    {
        QVariantMap properties;
        properties[ "P" ] = pattern_;
        properties[ "IC" ] = ignoreCase_;
        properties[ "RE" ] = useRegexp_;
        properties[ "IR" ] = inverse_;
        properties[ "BC" ] = boolean_;

        QVariantList sizeVariants;
        for ( const auto s : sizes_ ) {
            sizeVariants.append( s );
        }
        properties[ "S" ] = sizeVariants;

        return QJsonDocument::fromVariant( properties ).toJson( QJsonDocument::Compact );
    }

    const QString& pattern() const
    {
        return pattern_;
    }
    bool ignoreCase() const
    {
        return ignoreCase_;
    }
    bool useRegexp() const
    {
        return useRegexp_;
    }
    bool inverse() const
    {
        return inverse_;
    }
    bool boolean() const
    {
        return boolean_;
    }
    QList<int> sizes() const
    {
        return sizes_;
    }

  private:
    void loadFromJson( const QString& json )
    {
        const auto properties = QJsonDocument::fromJson( json.toLatin1() ).toVariant().toMap();

        pattern_ = properties.value( "P" ).toString();
        ignoreCase_ = properties.value( "IC" ).toBool();
        useRegexp_ = properties.value( "RE" ).toBool();
        inverse_ = properties.value( "IR" ).toBool();
        boolean_ = properties.value( "BC" ).toBool();

        if ( properties.contains( "S" ) ) {
            const auto sizes = properties.value( "S" ).toList();
            for ( const auto& s : sizes ) {
                sizes_.append( s.toInt() );
            }
        }
    }

    QString pattern_;
    bool ignoreCase_ = false;
    bool useRegexp_ = false;
    bool inverse_ = false;
    bool boolean_ = false;
    QList<int> sizes_;
};

FolderCrawlerWidget::FolderCrawlerWidget( QWidget* parent )
    : QWidget( parent )
{
    placeholderData_ = std::make_shared<LogData>();
    currentMainData_ = placeholderData_;
    folderResults_ = std::make_shared<FolderSearchResults>();
    quickFindPattern_ = std::make_shared<QuickFindPattern>();
    engine_ = new FolderSearchEngine( this ); // QObject-owned

    // --- toolbar ---
    // SearchToolbar (shared with CrawlerWidget) owns the search input + option
    // toggles + Search/Stop buttons. Folder mode passes null SavedSearches
    // (no history). Collapse/expand + status label sit alongside it.
    searchToolbar_ = new SearchToolbar( this, nullptr );
    collapseAllButton_ = new QToolButton( this );
    collapseAllButton_->setText( tr( "Collapse all" ) );
    expandAllButton_ = new QToolButton( this );
    expandAllButton_->setText( tr( "Expand all" ) );
    statusLabel_ = new QLabel( this );

    auto* toolbar = new QHBoxLayout;
    toolbar->addWidget( searchToolbar_, 1 );
    toolbar->addSpacing( 12 );
    toolbar->addWidget( collapseAllButton_ );
    toolbar->addWidget( expandAllButton_ );
    toolbar->addSpacing( 12 );
    toolbar->addWidget( statusLabel_ );

    // --- views ---
    // Overview pair is constructed up-front because LogMainView's ctor needs
    // non-null Overview + OverviewWidget (refreshOverview derefs them). The
    // widget is re-parented to mainView_ below so AbstractLogView owns its
    // placement (geometry set in resizeEvent), exactly like CrawlerWidget.
    overviewWidget_ = new OverviewWidget( this );
    overviewWidget_->setOverview( &overview_ );
    mainView_ = new LogMainView( placeholderData_.get(), quickFindPattern_.get(), &overview_,
                                 overviewWidget_, this );
    overviewWidget_->setParent( mainView_ );
    overview_.setVisible( Configuration::getSynced().isOverviewVisible() );
    mainView_->refreshOverview();
    filteredView_
        = new FolderFilteredView( folderResults_.get(), quickFindPattern_.get(), this );

    // Seed view config + search toggles from Configuration, mirroring CrawlerWidget
    // (crawlerwidget.cpp:842,852 for line numbers; :1311-1314 for search toggles).
    // Set BEFORE the toolbar signal connections below so the toolbar's internal
    // completer/setup runs cleanly with no folder-side handler attached yet
    // (same ordering rationale as CrawlerWidget). Without this, the filtered
    // view's default line-numbers-ON never took effect (AbstractLogView defaults
    // lineNumbersVisible_ to false) and every fresh folder tab opened with all
    // toggles unchecked regardless of the global search defaults.
    const auto& config = Configuration::get();
    mainView_->setLineNumbersVisible( config.mainLineNumbersVisible() );
    filteredView_->setLineNumbersVisible( config.filteredLineNumbersVisible() );
    searchToolbar_->setAutoRefresh( config.isSearchAutoRefreshDefault() );
    searchToolbar_->setMatchCase( !config.isSearchIgnoreCaseDefault() );
    searchToolbar_->setUseRegexp( config.mainRegexpType() == SearchRegexpType::ExtendedRegexp );
    searchToolbar_->setBoolean( config.isSearchLogicalCombiningDefault() );

    splitter_ = new QSplitter( Qt::Vertical, this );
    splitter_->addWidget( mainView_ );
    splitter_->addWidget( filteredView_ );
    splitter_->setStretchFactor( 0, 3 );
    splitter_->setStretchFactor( 1, 2 );

    auto* root = new QVBoxLayout( this );
    root->addLayout( toolbar );
    root->addWidget( splitter_, 1 );

    // --- signals ---
    connect( searchToolbar_, &SearchToolbar::searchRequested, this,
             &FolderCrawlerWidget::startSearch );
    connect( searchToolbar_, &SearchToolbar::stopRequested, this,
             &FolderCrawlerWidget::stopSearch );
    connect( collapseAllButton_, &QToolButton::clicked, this, &FolderCrawlerWidget::collapseAll );
    connect( expandAllButton_, &QToolButton::clicked, this, &FolderCrawlerWidget::expandAll );

    connect( engine_, &FolderSearchEngine::searchStarted, this, &FolderCrawlerWidget::onSearchStarted );
    connect( engine_, &FolderSearchEngine::searchProgressed, this,
             &FolderCrawlerWidget::onSearchProgressed );
    connect( engine_, &FolderSearchEngine::searchFinished, this,
             &FolderCrawlerWidget::onSearchFinished );
    connect( engine_, &FolderSearchEngine::fileGroupReady, this,
             &FolderCrawlerWidget::onFileGroupReady );

    connect( filteredView_, &FolderFilteredView::newSelection, this,
             &FolderCrawlerWidget::onResultSelected );
    connect( filteredView_, &FolderFilteredView::headerClicked, this,
             &FolderCrawlerWidget::onHeaderClicked );

    connect( folderResults_.get(), &FolderSearchResults::layoutChanged, this, [ this ]() {
        if ( filteredView_ != nullptr ) {
            filteredView_->updateData();
            filteredView_->forceRefresh();
        }
    } );

    // Apply Configuration (font, line numbers, overview, view shortcuts) now
    // that both views + the toolbar exist. Also re-applied on every
    // MainWindow::optionsChanged (the folder's applyConfiguration is connected
    // to optionsChanged by MainWindow::currentTabChanged).
    applyConfiguration();
}

FolderCrawlerWidget::~FolderCrawlerWidget() = default;

void FolderCrawlerWidget::setFolder( const QString& folderPath, const QStringList& filePaths )
{
    folderPath_ = folderPath;
    filePaths_ = filePaths;
    statusLabel_->setText(
        tr( "Folder: %1  (%n file(s))", "", static_cast<int>( filePaths_.size() ) ).arg( folderPath_ ) );
}

void FolderCrawlerWidget::applyConfiguration()
{
    // Mirrors CrawlerWidget::applyConfiguration (crawlerwidget.cpp:811-855) for
    // the subset relevant to folder mode: re-apply line-number visibility, font,
    // and overview to both views, and (re-)register view shortcuts. Called on
    // construction and whenever MainWindow emits optionsChanged (the View-menu
    // toggles), via a direct, deduplicated connection -- the folder is
    // intentionally NOT a signalMux document (it lacks the file/live-source
    // slots the mux routes), so optionsChanged is delivered directly.
    const auto& config = Configuration::get();

    QFont font = config.mainFont();
    font.setKerning( false );
    font.setFixedPitch( true );
    if ( config.forceFontAntialiasing() ) {
        font.setStyleStrategy( QFont::PreferAntialias );
    }
    font.setBold( config.useBoldFont() );

    mainView_->setLineNumbersVisible( config.mainLineNumbersVisible() );
    filteredView_->setLineNumbersVisible( config.filteredLineNumbersVisible() );
    overview_.setVisible( config.isOverviewVisible() );
    mainView_->refreshOverview();
    mainView_->updateFont( font );
    filteredView_->updateFont( font );

    registerShortcuts();
}

void FolderCrawlerWidget::registerShortcuts()
{
    // Register view-level keyboard shortcuts on both views, mirroring
    // CrawlerWidget (crawlerwidget.cpp:1725-1726). Without this, arrow keys,
    // PgUp/PgDn, jump-to-top/bottom, and the other AbstractLogView shortcuts are
    // not active in folder views.
    mainView_->registerShortcuts();
    filteredView_->registerShortcuts();
}

AbstractLogView* FolderCrawlerWidget::activeView() const
{
    // The focused view if one of ours has focus, else the results view (the
    // primary folder surface). Mirrors CrawlerWidget::activeView.
    if ( ( mainView_ != nullptr && mainView_->hasFocus() )
         || ( filteredView_ != nullptr && filteredView_->hasFocus() ) ) {
        return qobject_cast<AbstractLogView*>( QApplication::focusWidget() );
    }
    return filteredView_;
}

QString FolderCrawlerWidget::getSelectedText() const
{
    auto* view = activeView();
    return view != nullptr ? view->getSelectedText() : QString{};
}

bool FolderCrawlerWidget::isPartialSelection() const
{
    auto* view = activeView();
    return view != nullptr ? view->isPartialSelection() : false;
}

void FolderCrawlerWidget::selectAll()
{
    if ( auto* view = activeView() ) {
        view->selectAll();
    }
}

void FolderCrawlerWidget::searchFor( const QString& pattern )
{
    // Set the pattern text WITHOUT triggering setSearchPattern's auto-run /
    // reset side effects (which would double-fire startSearch when
    // autoRunSearchOnPatternChange is on), then start exactly one search.
    auto* combo = searchToolbar_->searchLineEdit();
    const bool wasBlocked = combo->blockSignals( true );
    combo->setEditText( pattern );
    combo->blockSignals( wasBlocked );
    startSearch();
}

void FolderCrawlerWidget::selectResultRow( LineNumber line )
{
    onResultSelected( line, 1_lcount, 0_lcol, 0_length );
}

void FolderCrawlerWidget::clickHeaderRow( LineNumber line )
{
    onHeaderClicked( line );
}

void FolderCrawlerWidget::doSetData( std::shared_ptr<SearchableLogData>,
                                     std::shared_ptr<LogFilteredData> )
{
    // Single-file data injection does not apply to folder mode.
}

void FolderCrawlerWidget::doSetQuickFindPattern( std::shared_ptr<QuickFindPattern> qfp )
{
    // Folder mode uses its own QuickFindPattern for v1; accept but don't rewire.
    if ( qfp != nullptr ) {
        quickFindPattern_ = std::move( qfp );
    }
}

void FolderCrawlerWidget::doSetSavedSearches( SavedSearches* )
{
}

void FolderCrawlerWidget::doSetViewContext( const QString& view_context )
{
    // Parse the stored context and restore the pattern text + option toggles
    // WITHOUT auto-running a search (one Enter re-runs). Mirrors searchFor's
    // blockSignals+setEditText pattern (foldercrawlerwidget.cpp) minus the
    // startSearch() call, and CrawlerWidget::doSetViewContext's toggle restore.
    const FolderCrawlerContext context{ view_context };

    if ( splitter_ != nullptr && !context.sizes().isEmpty() ) {
        splitter_->setSizes( context.sizes() );
    }

    // Restore toggles first (idempotent; optionsChanged is not wired to a
    // search in folder mode, so no search is triggered).
    searchToolbar_->setMatchCase( !context.ignoreCase() );
    searchToolbar_->setUseRegexp( context.useRegexp() );
    searchToolbar_->setInverse( context.inverse() );
    searchToolbar_->setBoolean( context.boolean() );

    // Restore the pattern text into the combo without emitting searchRequested
    // (blockSignals so setEditText does not fire auto-run on pattern change).
    if ( !context.pattern().isNull() ) {
        auto* combo = searchToolbar_->searchLineEdit();
        const bool wasBlocked = combo->blockSignals( true );
        combo->setEditText( context.pattern() );
        combo->blockSignals( wasBlocked );
    }
}

std::shared_ptr<const ViewContextInterface> FolderCrawlerWidget::doGetViewContext() const
{
    // Build the context from the live toolbar state. NEVER return null: the
    // session save path dereferences this (WindowSession::save calls
    // view_context->toString()), and a null shared_ptr crashes it.
    QList<int> sizes;
    if ( splitter_ != nullptr ) {
        sizes = splitter_->sizes();
    }

    // searchToolbar_ is constructed unconditionally in the ctor and never
    // reset, so it is always non-null here -- no null-guard needed (mirrors
    // CrawlerWidget, which derefs searchToolbar_ unconditionally throughout).
    auto context = std::make_shared<const FolderCrawlerContext>(
        searchToolbar_->currentSearchText(),
        !searchToolbar_->isMatchCase(),
        searchToolbar_->isUseRegexp(),
        searchToolbar_->isInverse(),
        searchToolbar_->isBoolean(), sizes );

    return static_cast<std::shared_ptr<const ViewContextInterface>>( context );
}

void FolderCrawlerWidget::startSearch()
{
    if ( filePaths_.isEmpty() ) {
        return;
    }
    const auto pattern = searchToolbar_->currentSearchText();
    if ( pattern.isEmpty() ) {
        return;
    }
    searchToolbar_->setSearchInProgress( true );
    searchActive_ = true;
    // Reset the view for streaming: beginSearch sizes the pending buffer and
    // clears any prior result set so file groups stream into a clean view. This
    // also covers the invalid-pattern path (which never emits searchStarted).
    folderResults_->beginSearch( filePaths_ );

    // Build the pattern from the toolbar's option flags (case / regex / inverse
    // / boolean). This UPGRADES folder search from the former plain-text
    // RegularExpressionPattern{ pattern } to honor the toggles for free.
    const auto regexpPattern = searchToolbar_->currentRegularExpressionPattern();
    currentSearchGeneration_ = engine_->startSearch( filePaths_, regexpPattern );
    // Store + forward the pattern to BOTH views so that the matched substring
    // is highlighted in the filtered results AND in any file already open in
    // the main view (single-file parity: crawlerwidget.cpp forwards to both
    // logMainView_ and filteredView_). Storing here also lets openFileInMainView
    // re-apply the pattern right after each setDataSource swap.
    currentSearchPattern_ = regexpPattern;
    if ( filteredView_ != nullptr ) {
        filteredView_->setSearchPattern( regexpPattern );
    }
    if ( mainView_ != nullptr ) {
        mainView_->setSearchPattern( regexpPattern );
    }
}

void FolderCrawlerWidget::stopSearch()
{
    engine_->interrupt();
}

void FolderCrawlerWidget::onSearchStarted( quint64 )
{
    statusLabel_->setText( tr( "Searching..." ) );
}

void FolderCrawlerWidget::onSearchProgressed( quint64 nbMatches, int percent, quint64 )
{
    statusLabel_->setText( tr( "%1 match(es)  %2%" ).arg( static_cast<qulonglong>( nbMatches ) ).arg( percent ) );
}

void FolderCrawlerWidget::onSearchFinished( quint64 generation )
{
    if ( generation != currentSearchGeneration_ ) {
        return; // stale: superseded by a newer search
    }

    // Defensive safety net: commit any groups still buffered (interrupted
    // scans may leave a predecessor that never arrived). Under normal
    // completion all per-file groups were already committed incrementally via
    // onFileGroupReady before searchFinished was emitted.
    folderResults_->flushPending();

    searchToolbar_->setSearchInProgress( false );
    searchActive_ = false;

    const quint64 total = engine_->matchCount();
    if ( total == 0 ) {
        statusLabel_->setText( tr( "No matches" ) );
    }
    else {
        statusLabel_->setText( tr( "%1 match(es)" ).arg( static_cast<qulonglong>( total ) ) );
    }
}

void FolderCrawlerWidget::onFileGroupReady( int fileIndex, quint64 generation )
{
    if ( generation != currentSearchGeneration_ ) {
        return; // stale: superseded by a newer search
    }
    auto group = engine_->takeCompletedGroup( fileIndex );
    if ( group.has_value() ) {
        folderResults_->addFileGroup( fileIndex, std::move( *group ) );
    }
}

void FolderCrawlerWidget::onResultSelected( LineNumber line, LinesCount, LineColumn, LineLength )
{
    if ( folderResults_ == nullptr || line >= folderResults_->getNbLine() ) {
        return;
    }
    if ( folderResults_->lineKind( line ) != LineKind::Data ) {
        return;
    }
    const auto source = folderResults_->sourceForLine( line );
    openFileInMainView( source.filePath, source.localLine );
}

void FolderCrawlerWidget::onHeaderClicked( LineNumber line )
{
    if ( folderResults_ == nullptr ) {
        return;
    }
    const auto fileId = folderResults_->fileIdForLine( line );
    if ( fileId < 0 ) {
        return;
    }
    folderResults_->toggleCollapse( fileId ); // emits layoutChanged -> view refreshes
}

void FolderCrawlerWidget::collapseAll()
{
    if ( folderResults_ != nullptr ) {
        folderResults_->collapseAll();
    }
}

void FolderCrawlerWidget::expandAll()
{
    if ( folderResults_ != nullptr ) {
        folderResults_->expandAll();
    }
}

void FolderCrawlerWidget::openFileInMainView( const QString& filePath, LineNumber localLine )
{
    if ( filePath.isEmpty() ) {
        return;
    }

    // Already showing this file -> just jump.
    if ( filePath == currentMainFilePath_ && currentMainData_ != nullptr ) {
        mainView_->jumpToLine( localLine );
        return;
    }

    // Cached (recently opened) -> swap instantly. Promote to most-recently-used
    // so the LRU eviction policy keeps it resident.
    const auto it = mainViewCache_.find( filePath );
    if ( it != mainViewCache_.end() ) {
        mainViewCacheOrder_.splice( mainViewCacheOrder_.begin(), mainViewCacheOrder_,
                                    it->second.second );
        currentMainData_ = it->second.first;
        currentMainFilePath_ = filePath;
        mainView_->setDataSource( currentMainData_.get() );
        // Re-apply the current search pattern so the swapped-in (cached) file
        // highlights its matches at first paint (idempotent: setDataSource does
        // not reset searchPattern_, this is the explicit parity guarantee).
        mainView_->setSearchPattern( currentSearchPattern_ );
        mainView_->jumpToLine( localLine );
        refreshFileOverview( filePath );
        return;
    }

    // Not loaded yet -> index the file asynchronously, then swap + jump.
    pendingMainFilePath_ = filePath;
    pendingJumpLine_ = localLine;
    pendingMainData_ = std::make_shared<LogData>();

    connect( pendingMainData_.get(), &SearchableLogData::loadingFinished, this,
             [ this ]( LoadingStatus ) {
                 if ( pendingMainData_ == nullptr ) {
                     return;
                 }
                 currentMainData_ = pendingMainData_;
                 currentMainFilePath_ = pendingMainFilePath_;
                 cacheMainViewData( currentMainFilePath_, currentMainData_ );

                 mainView_->setDataSource( currentMainData_.get() );
                 // Re-apply the current search pattern so the freshly-indexed
                 // file highlights its matches at first paint. Runs on the main
                 // thread (loadingFinished is a queued signal), so threading is
                 // correct.
                 mainView_->setSearchPattern( currentSearchPattern_ );
                 mainView_->jumpToLine( pendingJumpLine_ );
                 // getNbLine() is only valid now that indexing finished: the
                 // overview repoint MUST happen here, not at attachFile time.
                 refreshFileOverview( pendingMainFilePath_ );

                 pendingMainData_.reset();
                 pendingMainFilePath_.clear();
             } );

    statusLabel_->setText( tr( "Opening %1..." ).arg( filePath ) );
    pendingMainData_->attachFile( filePath );
}

void FolderCrawlerWidget::refreshFileOverview( const QString& filePath )
{
    // No-op until a real file is loaded and the overview is user-visible.
    if ( !overview_.isVisible() || currentMainData_ == nullptr || folderResults_ == nullptr ) {
        return;
    }
    overview_.setMatchLines( folderResults_->matchLinesForFile( filePath ) );
    overview_.updateData( currentMainData_->getNbLine() );
    mainView_->refreshOverview();
}

void FolderCrawlerWidget::cacheMainViewData( const QString& filePath, std::shared_ptr<LogData> data )
{
    if ( filePath.isEmpty() ) {
        return;
    }
    // Insert at the front (most-recently-used). If the file was already cached,
    // drop its prior order entry first so we do not leak stale list nodes.
    const auto existing = mainViewCache_.find( filePath );
    if ( existing != mainViewCache_.end() ) {
        mainViewCacheOrder_.erase( existing->second.second );
    }
    mainViewCacheOrder_.push_front( filePath );
    mainViewCache_[ filePath ] = { std::move( data ), mainViewCacheOrder_.begin() };

    // Evict least-recently-used entries (back of the order list) to bound memory.
    while ( mainViewCache_.size() > MainViewCacheLimit ) {
        const auto lru = mainViewCacheOrder_.back();
        mainViewCache_.erase( lru );
        mainViewCacheOrder_.pop_back();
    }
}
