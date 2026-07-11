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

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QSplitter>
#include <QToolButton>
#include <QVBoxLayout>

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

FolderCrawlerWidget::FolderCrawlerWidget( QWidget* parent )
    : QWidget( parent )
{
    placeholderData_ = std::make_shared<LogData>();
    currentMainData_ = placeholderData_;
    folderResults_ = std::make_shared<FolderSearchResults>();
    quickFindPattern_ = std::make_shared<QuickFindPattern>();
    engine_ = new FolderSearchEngine( this ); // QObject-owned

    // --- toolbar ---
    searchEdit_ = new QLineEdit( this );
    searchEdit_->setPlaceholderText( tr( "Search regex (e.g. ERROR|WARN)" ) );
    searchButton_ = new QToolButton( this );
    searchButton_->setText( tr( "Search" ) );
    stopButton_ = new QToolButton( this );
    stopButton_->setText( tr( "Stop" ) );
    stopButton_->setEnabled( false );
    collapseAllButton_ = new QToolButton( this );
    collapseAllButton_->setText( tr( "Collapse all" ) );
    expandAllButton_ = new QToolButton( this );
    expandAllButton_->setText( tr( "Expand all" ) );
    statusLabel_ = new QLabel( this );

    auto* toolbar = new QHBoxLayout;
    toolbar->addWidget( searchEdit_, 1 );
    toolbar->addWidget( searchButton_ );
    toolbar->addWidget( stopButton_ );
    toolbar->addSpacing( 12 );
    toolbar->addWidget( collapseAllButton_ );
    toolbar->addWidget( expandAllButton_ );
    toolbar->addSpacing( 12 );
    toolbar->addWidget( statusLabel_ );

    // --- views ---
    overviewWidget_ = new OverviewWidget( this );
    overviewWidget_->setOverview( &overview_ );
    overviewWidget_->hide(); // per-file overview is a v2; LogMainView still needs the pair.
    mainView_ = new LogMainView( placeholderData_.get(), quickFindPattern_.get(), &overview_,
                                 overviewWidget_, this );
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
    connect( searchButton_, &QToolButton::clicked, this, &FolderCrawlerWidget::startSearch );
    connect( stopButton_, &QToolButton::clicked, this, &FolderCrawlerWidget::stopSearch );
    connect( collapseAllButton_, &QToolButton::clicked, this, &FolderCrawlerWidget::collapseAll );
    connect( expandAllButton_, &QToolButton::clicked, this, &FolderCrawlerWidget::expandAll );
    connect( searchEdit_, &QLineEdit::returnPressed, this, &FolderCrawlerWidget::startSearch );

    connect( engine_, &FolderSearchEngine::searchStarted, this, &FolderCrawlerWidget::onSearchStarted );
    connect( engine_, &FolderSearchEngine::searchProgressed, this,
             &FolderCrawlerWidget::onSearchProgressed );
    connect( engine_, &FolderSearchEngine::searchFinished, this,
             &FolderCrawlerWidget::onSearchFinished );

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
    searchEdit_->setText( pattern );
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
    const auto pattern = searchEdit_->text();
    if ( pattern.isEmpty() ) {
        return;
    }
    searchButton_->setEnabled( false );
    stopButton_->setEnabled( true );
    engine_->startSearch( filePaths_, RegularExpressionPattern{ pattern } );
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

void FolderCrawlerWidget::onSearchFinished( quint64 )
{
    searchButton_->setEnabled( true );
    stopButton_->setEnabled( false );

    auto groups = engine_->takeResults();
    quint64 total = 0;
    for ( const auto& g : groups ) {
        total += g.matches.size();
    }
    folderResults_->setResults( std::move( groups ) ); // emits layoutChanged -> view refreshes

    if ( total == 0 ) {
        statusLabel_->setText( tr( "No matches" ) );
    }
    else {
        statusLabel_->setText( tr( "%1 match(es)" ).arg( static_cast<qulonglong>( total ) ) );
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

    // Cached (recently opened) -> swap instantly.
    const auto it = mainViewCache_.find( filePath );
    if ( it != mainViewCache_.end() ) {
        currentMainData_ = it->second;
        currentMainFilePath_ = filePath;
        mainView_->setDataSource( currentMainData_.get() );
        mainView_->jumpToLine( localLine );
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

                 pendingMainData_.reset();
                 pendingMainFilePath_.clear();
             } );

    statusLabel_->setText( tr( "Opening %1..." ).arg( filePath ) );
    pendingMainData_->attachFile( filePath );
}

void FolderCrawlerWidget::cacheMainViewData( const QString& filePath, std::shared_ptr<LogData> data )
{
    if ( filePath.isEmpty() ) {
        return;
    }
    mainViewCache_[ filePath ] = std::move( data );
    while ( mainViewCache_.size() > MainViewCacheLimit ) {
        // Evict an arbitrary (oldest-insertion-order is not tracked; unordered_map
        // gives us some victim) entry to bound memory. The file is re-indexed on
        // next access if needed.
        mainViewCache_.erase( mainViewCache_.begin() );
    }
}
