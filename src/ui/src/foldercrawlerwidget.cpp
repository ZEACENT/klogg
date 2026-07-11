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

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QSplitter>
#include <QToolButton>
#include <QVBoxLayout>

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
}

FolderCrawlerWidget::~FolderCrawlerWidget() = default;

void FolderCrawlerWidget::setFolder( const QString& folderPath, const QStringList& filePaths )
{
    folderPath_ = folderPath;
    filePaths_ = filePaths;
    statusLabel_->setText(
        tr( "Folder: %1  (%n file(s))", "", static_cast<int>( filePaths_.size() ) ).arg( folderPath_ ) );
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

void FolderCrawlerWidget::doSetFolderData( std::shared_ptr<FolderSearchResults> )
{
    // The widget owns its own FolderSearchResults (populated by its engine).
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

void FolderCrawlerWidget::doSetViewContext( const QString& )
{
}

std::shared_ptr<const ViewContextInterface> FolderCrawlerWidget::doGetViewContext() const
{
    return {};
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
    if ( filteredView_ != nullptr ) {
        filteredView_->setSearchPattern( regexpPattern );
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
