/*
 * Copyright (C) 2009, 2010, 2011, 2012, 2013, 2014, 2015 Nicolas Bonnefon and other contributors
 *
 * This file is part of glogg.
 *
 * glogg is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * glogg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with glogg.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Copyright (C) 2016 -- 2019 Anton Filimonov and other contributors
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

// This file implements the CrawlerWidget class.
// It is responsible for creating and managing the two views and all
// the UI elements.  It implements the connection between the UI elements.
// It also interacts with the sets of data (full and filtered).

#include "abstractlogview.h"
#include "active_screen.h"
#include "emptyfilterpolicy.h"
#include "linetypes.h"
#include "log.h"
#include "qtcompat/qtcompat.h"
#include "searchgeneration.h"

#include <algorithm>
#include <cassert>

#include <QAction>
#include <QApplication>
#include <QCompleter>
#include <QEvent>
#include <QFontMetrics>
#include <QInputDialog>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLineEdit>
#include <QListView>
#include <QShortcut>
#include <QStandardItemModel>
#include <QStringListModel>
#include <qglobal.h>
#include <qobject.h>
#include <string>

#include "regularexpression.h"

#include "crawlerwidget.h"

#include "configuration.h"
#include "crawlershortcuts.h"
#include "dispatch_to.h"
#include "filewatcher.h"
#include "fontutils.h"
#include "infoline.h"
#include "predefinedfilters.h"
#include "quickfindpattern.h"
#include "savedsearches.h"
#include "shortcuts.h"

// Throttle intervals (ms) for coalescing search updates during live streaming.
static constexpr int kSearchThrottleActiveMs = 250;
static constexpr int kSearchThrottleInactiveMs = 1000;

// Palette for error signaling (yellow background)
const QPalette CrawlerWidget::ErrorPalette( Qt::darkYellow );

// Implementation of the view context for the CrawlerWidget
class CrawlerWidgetContext : public ViewContextInterface {
public:
    // Construct from the stored string representation
    explicit CrawlerWidgetContext( const QString& string );
    // Construct from the value passsed
    CrawlerWidgetContext( QList<int> sizes, bool ignoreCase, bool autoRefresh, bool followFile,
                          bool useRegexp, bool inverseRegexp, bool useBooleanCombination,
                          QList<LineNumber> markedLines )
        : sizes_( sizes )
        , ignoreCase_( ignoreCase )
        , autoRefresh_( autoRefresh )
        , followFile_( followFile )
        , useRegexp_( useRegexp )
        , inverseRegexp_( inverseRegexp )
        , useBooleanCombination_( useBooleanCombination )
    {
        std::transform( markedLines.cbegin(), markedLines.cend(), std::back_inserter( marks_ ),
                        []( const auto& m ) { return m.get(); } );
    }

    // Implementation of the ViewContextInterface function
    QString toString() const override;

    // Access the Qt sizes array for the QSplitter
    QList<int> sizes() const
    {
        return sizes_;
    }

    bool ignoreCase() const
    {
        return ignoreCase_;
    }
    bool autoRefresh() const
    {
        return autoRefresh_;
    }
    bool followFile() const
    {
        return followFile_;
    }
    bool useRegexp() const
    {
        return useRegexp_;
    }
    bool inverseRegexp() const
    {
        return inverseRegexp_;
    }
    bool useBooleanCombination() const
    {
        return useBooleanCombination_;
    }

    QList<LineNumber::UnderlyingType> marks() const
    {
        return marks_;
    }

private:
    void loadFromString( const QString& string );
    void loadFromJson( const QString& json );

private:
    QList<int> sizes_;

    bool ignoreCase_;
    bool autoRefresh_;
    bool followFile_;
    bool useRegexp_;
    bool inverseRegexp_;
    bool useBooleanCombination_;

    QList<LineNumber::UnderlyingType> marks_;
};

// Constructor only does trivial construction. The real work is done once
// the data is attached.
CrawlerWidget::~CrawlerWidget()
{
    // Join each view's in-flight QuickFind worker BEFORE the data-source members
    // (logData_ / logFilteredData_ / filteredViewsData_) are released. Same
    // destruction-order hazard as ~FolderCrawlerWidget: a view's QuickFind worker
    // is a QThreadPool task that holds a `const AbstractLogData&` into the
    // LogData/LogFilteredData it was constructed over; the default
    // member-destruction order frees those shared_ptrs before ~QObject deletes
    // the child views (whose ~AbstractLogView joins the worker), so the worker
    // would read already-freed memory. stopSearchAndWait only joins the worker
    // (no view deletion / reparent / signals), so it cannot trigger Qt's
    // child-removal cascade. The views themselves are deleted later by ~QObject.
    if ( logMainView_ != nullptr ) {
        logMainView_->stopSearchAndWait();
    }
    for ( const auto& entry : filteredViewsData_ ) {
        if ( entry.first != nullptr ) {
            entry.first->stopSearchAndWait();
        }
    }
}

CrawlerWidget::CrawlerWidget( QWidget* parent )
    : QSplitter( parent )
    , iconLoader_{ this }
    , colorLabelsController_( this, [ this ]() { return activeView(); } )
{
}

// The top line is first one on the main display
LineNumber CrawlerWidget::getTopLine() const
{
    return logMainView_->getTopLine();
}

QString CrawlerWidget::getSelectedText() const
{
    // filteredView_ is nulled when its tab is closed (filteredViewDestroyed);
    // when the filtered view is gone the focus can only be on the main view.
    if ( filteredView_ != nullptr && filteredView_->hasFocus() )
        return filteredView_->getSelectedText();
    else
        return logMainView_->getSelectedText();
}

bool CrawlerWidget::isPartialSelection() const
{
    if ( filteredView_ != nullptr && filteredView_->hasFocus() )
        return filteredView_->isPartialSelection();
    else
        return logMainView_->isPartialSelection();
}

void CrawlerWidget::selectAll()
{
    activeView()->selectAll();
}

std::optional<int> CrawlerWidget::encodingMib() const
{
    return encodingMib_;
}

bool CrawlerWidget::isFollowEnabled() const
{
    return logMainView_->isFollowEnabled();
}

bool CrawlerWidget::isFirstLoadDone() const
{
    return firstLoadDone_;
}

bool CrawlerWidget::isTextWrapEnabled() const
{
    return logMainView_->isTextWrapEnabled();
}

QString CrawlerWidget::encodingText() const
{
    return encodingText_;
}

// Return a pointer to the view in which we should do the QuickFind
SearchableWidgetInterface* CrawlerWidget::doGetActiveSearchable() const
{
    return activeView();
}

// Return all the searchable widgets (views)
std::vector<QObject*> CrawlerWidget::doGetAllSearchables() const
{
    std::vector<QObject*> searchables = { logMainView_, filteredView_ };

    return searchables;
}

// Update the state of the parent
void CrawlerWidget::doSendAllStateSignals()
{
    Q_EMIT newSelection( currentLineNumber_, 0_lcount, 0_lcol, 0_length );
    if ( !loadingInProgress_ )
        Q_EMIT loadingFinished( LoadingStatus::Successful );
}

void CrawlerWidget::changeEvent( QEvent* event )
{
    if ( event->type() == QEvent::StyleChange ) {
        dispatchToMainThread( [ this ] {
            loadIcons();
            searchInfoLineDefaultPalette_ = this->palette();
        } );
    }

    QWidget::changeEvent( event );
}

//
// Public Q_SLOTS:
//

void CrawlerWidget::stopLoading()
{
    logFilteredData_->interruptSearch();
    logData_->interruptLoading();
}

void CrawlerWidget::reload()
{
    searchUpdateThrottleTimer_.stop();
    searchUpdatePending_ = false;
    if ( searchPendingLines_ != 0 ) {
        searchPendingLines_ = 0;
        Q_EMIT searchPendingLinesChanged();
    }
    searchState_.resetState();
    constexpr auto DropCache = true;
    logFilteredData_->clearSearch( DropCache );
    logFilteredData_->clearMarks();
    filteredView_->updateData();
    printSearchInfoMessage();

    logData_->reload();

    // A reload is considered as a first load,
    // this is to prevent the "new data" icon to be triggered.
    firstLoadDone_ = false;
}

void CrawlerWidget::setEncoding( std::optional<int> mib )
{
    encodingMib_ = std::move( mib );
    updateEncoding();

    update();
}

void CrawlerWidget::focusSearchEdit()
{
    searchToolbar_->searchLineEdit()->lineEdit()->setFocus( Qt::ShortcutFocusReason );
}

bool CrawlerWidget::eventFilter( QObject* watched, QEvent* event )
{
    // The search-input Home/End handling now lives in SearchToolbar (which
    // installs its own eventFilter on the combo it owns). CrawlerWidget no
    // longer filters the search input.
    return QSplitter::eventFilter( watched, event );
}

void CrawlerWidget::jumpToTop()
{
    // Scroll filtered view first — it emits newSelection which triggers
    // jumpToMatchingLine, moving the main view to the matching line.
    // Then scroll the main view to absolute line 0, overriding that.
    filteredView_->selectAndDisplayLine( 0_lnum );
    logMainView_->selectAndDisplayLine( 0_lnum );
}

void CrawlerWidget::goToLine()
{
    bool isLineSelected = true;
    auto newLine = QInputDialog::getText( this, "Jump to line", "Line number" )
                       .toULongLong( &isLineSelected );

    if ( isLineSelected ) {
        if ( newLine == 0 ) {
            newLine = 1;
        }

        const auto selectedLine
            = LineNumber( static_cast<LineNumber::UnderlyingType>( newLine - 1 ) );
        filteredView_->trySelectLine( logFilteredData_->getLineIndexNumber( selectedLine ) );
        logMainView_->trySelectLine( selectedLine );
    }
}

//
// Protected functions
//
void CrawlerWidget::doSetData( std::shared_ptr<SearchableLogData> logData,
                               std::shared_ptr<LogFilteredData> filteredData )
{
    logData_ = std::move( logData );
    logFilteredData_ = std::move( filteredData );
}

void CrawlerWidget::doSetQuickFindPattern( std::shared_ptr<QuickFindPattern> qfp )
{
    quickFindPattern_ = std::move( qfp );
}

void CrawlerWidget::doSetSavedSearches( SavedSearches* saved_searches )
{
    savedSearches_ = saved_searches;

    // We do setup now, assuming doSetData has been called before
    // us, that's not great really...
    setup();
}

void CrawlerWidget::doSetViewContext( const QString& view_context )
{
    LOG_DEBUG << "CrawlerWidget::doSetViewContext: " << view_context.toLocal8Bit().data();

    const auto context = CrawlerWidgetContext{ view_context };

    setSizes( context.sizes() );
    searchToolbar_->setMatchCase( !context.ignoreCase() );
    searchToolbar_->setUseRegexp( context.useRegexp() );
    searchToolbar_->setInverse( context.inverseRegexp() );
    searchToolbar_->setBoolean( context.useBooleanCombination() );

    searchToolbar_->setAutoRefresh( context.autoRefresh() );
    // Manually call the handler as it is not called when changing the state programmatically
    searchRefreshChangedHandler( context.autoRefresh() );

    const auto& config = Configuration::get();
    const auto allowFollow = logData_ && ( logData_->isLiveSource() || config.anyFileWatchEnabled() );
    logMainView_->followSet( context.followFile() && allowFollow );

    const auto savedMarks = context.marks();
    std::transform( savedMarks.cbegin(), savedMarks.cend(), std::back_inserter( savedMarkedLines_ ),
                    []( const auto& l ) { return LineNumber( l ); } );
}

std::shared_ptr<const ViewContextInterface> CrawlerWidget::doGetViewContext() const
{
    auto context = std::make_shared<const CrawlerWidgetContext>(
        sizes(), ( !searchToolbar_->isMatchCase() ), searchToolbar_->isAutoRefresh(),
        logMainView_->isFollowEnabled(), searchToolbar_->isUseRegexp(), searchToolbar_->isInverse(),
        searchToolbar_->isBoolean(), logFilteredData_->getMarks() );

    return static_cast<std::shared_ptr<const ViewContextInterface>>( context );
}

//
// Q_SLOTS:
//

void CrawlerWidget::startNewSearch()
{
    if ( searchToolbar_->isKeepResultsChecked() ) {
        searchToolbar_->setKeepResultsChecked( false );

        searchUpdateThrottleTimer_.stop();
        searchUpdatePending_ = false;
        logFilteredData_->interruptSearch();
        logFilteredData_ = logData_->getNewFilteredData();

        filteredView_ = new FilteredView( logFilteredData_.get(), quickFindPattern_.get() );
        filteredViewsData_[ filteredView_ ] = logFilteredData_;

        // Reset context lines for new filtered data
        logFilteredData_->setContextLines( 0, 0 );

        connectAllFilteredViewSlots( filteredView_ );

        auto index = tabbedFilteredView_->addTab( filteredView_, "" );
        tabbedFilteredView_->setTabsClosable( true );
        tabbedFilteredView_->setCurrentIndex( index );

        connect( logFilteredData_.get(), &LogFilteredData::searchProgressed, this,
                 &CrawlerWidget::updateFilteredView,
                 static_cast<Qt::ConnectionType>( Qt::QueuedConnection | Qt::UniqueConnection ) );

        logMainView_->useNewFiltering( logFilteredData_.get() );

        applyConfiguration();
    }

    tabbedFilteredView_->setTabText( tabbedFilteredView_->currentIndex(),
                                     "Find \"" + searchToolbar_->currentSearchText() + "\"" );

    // Record the search line in the recent list (shared with folder mode:
    // reloads first in case another klogg changed it, saves, refreshes the
    // dropdown).
    searchToolbar_->recordSearch();

    // Call the private function to do the search
    replaceCurrentSearch( searchToolbar_->currentSearchText() );
}

void CrawlerWidget::updatePredefinedFiltersWidget()
{
    searchToolbar_->predefinedFilters()->clearCurrentSelection();
}

void CrawlerWidget::stopSearch()
{
    searchUpdateThrottleTimer_.stop();
    searchUpdatePending_ = false;
    if ( searchPendingLines_ != 0 ) {
        searchPendingLines_ = 0;
        Q_EMIT searchPendingLinesChanged();
    }
    logFilteredData_->interruptSearch();
    searchState_.stopSearch();
    printSearchInfoMessage();
}

void CrawlerWidget::clearSearchHistory()
{
    // Clear line
    searchToolbar_->searchLineEdit()->clear();

    // Sync and clear saved searches
    auto& searches = SavedSearches::getSynced();
    savedSearches_->clear();
    searches.save();

    searchToolbar_->searchLineCompleter()->setModel(
        new QStringListModel( {}, searchToolbar_->searchLineCompleter() ) );
}

void CrawlerWidget::editSearchHistory()
{
    // Sync and clear saved searches
    auto& searches = SavedSearches::getSynced();

    auto history = savedSearches_->recentSearches().join( QChar::LineFeed );
    bool ok;
    QString newHistory = QInputDialog::getMultiLineText( this, tr( "klogg" ),
                                                         tr( "Search history:" ), history, &ok );

    if ( ok ) {
        savedSearches_->clear();
        auto items = newHistory.split( QChar::LineFeed, klogg::qtcompat::skipEmptyParts() );
        std::for_each( items.rbegin(), items.rend(), [ this ]( const auto& item ) {
            savedSearches_->addRecent( item );
            LOG_INFO << item;
        } );
    }
    searches.save();

    updateSearchCombo();
}

// When receiving the 'newDataAvailable' signal from LogFilteredData
void CrawlerWidget::updateFilteredView( LinesCount nbMatches, int progress,
                                        LineNumber initialPosition,
                                        quint64 generation )
{
    if ( logFilteredData_ ) {
        const auto activeGeneration = logFilteredData_->currentSearchGeneration();
        if ( klogg::isStaleSearchGeneration( generation, activeGeneration ) ) {
            // Stale signal from a search that has since been replaced.  Without
            // this gate, queued metacalls from the previous SearchOperation can
            // land in updateFilteredView() after replaceCurrentSearch() has
            // started a new search, corrupting match counts and progress UI.
            LOG_DEBUG << "updateFilteredView dropping stale signal: gen " << generation
                      << " != active " << activeGeneration;
            return;
        }
    }

    LOG_DEBUG << "updateFilteredView received.";

    searchInfoLine_->show();

    if ( progress == 100 ) {
        // Reset pending lines when search completes
        if ( searchPendingLines_ != 0 ) {
            searchPendingLines_ = 0;
            Q_EMIT searchPendingLinesChanged();
        }

        // Searching done - apply context lines if mode is active
        if ( contextLinesMode_ > 0 && contextLinesSpinBox_->value() > 0 ) {
            applyContextLines();
        }

        printSearchInfoMessage( nbMatches );
        searchInfoLine_->hideGauge();
        // De-activate the stop button
        searchToolbar_->setSearchInProgress( false );
    }
    else {
        // Search in progress
        // We ignore 0% and 100% to avoid a flash when the search is very short
        if ( progress > 0 ) {
            // Some languages translate the plural the same as the singular, so use the full string

            // For live sources / growing files, show remaining lines instead of
            // percentage since the percentage becomes misleading when the file
            // keeps growing.  progress is already relative to the current
            // search window, so remaining = totalLines * (100 - progress) / 100.
            const auto totalLines = logData_->getNbLine();
            const auto remaining = totalLines.get()
                                 * static_cast<LinesCount::UnderlyingType>( 100 - progress ) / 100;

            // Update pending lines for status bar display
            const auto newPending = static_cast<qint64>( remaining );
            if ( newPending != searchPendingLines_ ) {
                searchPendingLines_ = newPending;
                Q_EMIT searchPendingLinesChanged();
            }

            QString progressText;
            if ( logData_->isLiveSource() && remaining > 0 ) {
                progressText = tr( "Search in progress — %1 lines pending..." )
                                   .arg( QString::number( remaining ) );
            }
            else {
                progressText
                    = tr( "Search in progress (%1 %)..." ).arg( QString::number( progress ) );
            }

            searchInfoLine_->setText(
                progressText
                + ( nbMatches.get() > 1 ? tr( " %1 matches found so far." )
                                              .arg( QString::number( nbMatches.get() ) )
                                        : tr( " %1 match found so far." )
                                              .arg( QString::number( nbMatches.get() ) ) ) );

            searchInfoLine_->displayGauge( progress );
        }
    }

    // If more (or less, e.g. come back to 0) matches have been found
    if ( nbMatches != nbMatches_ ) {
        nbMatches_ = nbMatches;

        // Recompute the content of the filtered window.
        filteredView_->updateData();

        // Update the match overview
        overview_.updateData( logData_->getNbLine() );

        // New data found icon
        if ( initialPosition > 0_lnum ) {
            changeDataStatus( DataStatus::NEW_FILTERED_DATA );
        }

        // Also update the top window for the coloured bullets.
        update();
    }

    // Try to restore the filtered window selection close to where it was
    // only for full searches to avoid disconnecting follow mode!
    if ( ( progress == 100 ) && ( initialPosition == searchStartLine_ )
         && ( !isFollowEnabled() ) ) {
        const auto currenLineIndex = logFilteredData_->getLineIndexNumber( currentLineNumber_ );
        LOG_DEBUG << "updateFilteredView: restoring selection: "
                  << " absolute line number (0based) " << currentLineNumber_ << " index "
                  << currenLineIndex;
        filteredView_->selectAndDisplayLine( currenLineIndex );
        filteredView_->setSearchLimits( searchStartLine_, searchEndLine_ );
    }
}

void CrawlerWidget::jumpToMatchingLine( LineNumber filteredLineNb, LinesCount nLines,
                                        LineColumn startCol, LineLength nSymbols )
{
    const auto mainViewLine = logFilteredData_->getMatchingLineNumber( filteredLineNb );

    logMainView_->selectPortionAndDisplayLine( mainViewLine, nLines, startCol,
                                               nSymbols ); // FIXME: should be done with a signal.
}

void CrawlerWidget::updateLineNumberHandler( LineNumber line, LinesCount nLines,
                                             LineColumn startCol, LineLength nSymbols )
{
    currentLineNumber_ = line;
    Q_EMIT newSelection( line, nLines, startCol, nSymbols );
}

void CrawlerWidget::markLinesFromMain( const klogg::vector<LineNumber>& lines )
{
    for ( const auto& line : lines ) {
        if ( line >= logData_->getNbLine() ) {
            continue;
        }

        if ( !logFilteredData_->lineTypeByLine( line ).testFlag(
                 AbstractLogData::LineTypeFlags::Mark ) ) {
            logFilteredData_->addMark( line );
        }
    }

    // Recompute the content of both window.
    filteredView_->updateData();
    logMainView_->updateData();

    // Update the match overview
    overview_.updateData( logData_->getNbLine() );

    // Also update the top window for the coloured bullets.
    update();
}

void CrawlerWidget::markLinesFromFiltered( const klogg::vector<LineNumber>& lines )
{
    klogg::vector<LineNumber> linesInMain( lines.size() );
    std::transform( lines.cbegin(), lines.cend(), linesInMain.begin(),
                    [ this ]( const auto& filteredLine ) {
                        if ( filteredLine < logData_->getNbLine() ) {
                            return logFilteredData_->getMatchingLineNumber( filteredLine );
                        }
                        else {
                            return maxValue<LineNumber>();
                        }
                    } );

    markLinesFromMain( linesInMain );
}

void CrawlerWidget::deleteMarkLinesFromMain( const klogg::vector<LineNumber>& lines )
{
    for ( const auto& line : lines ) {
        if ( line >= logData_->getNbLine() ) {
            continue;
        }

        if ( logFilteredData_->lineTypeByLine( line ).testFlag(
                 AbstractLogData::LineTypeFlags::Mark ) ) {
            logFilteredData_->deleteMark( line );
        }
    }

    // Recompute the content of both window.
    filteredView_->updateData();
    logMainView_->updateData();

    // Update the match overview
    overview_.updateData( logData_->getNbLine() );

    // Also update the top window for the coloured bullets.
    update();
}

void CrawlerWidget::deleteMarkLinesFromFiltered( const klogg::vector<LineNumber>& lines )
{
    klogg::vector<LineNumber> linesInMain( lines.size() );
    std::transform( lines.cbegin(), lines.cend(), linesInMain.begin(),
                    [ this ]( const auto& filteredLine ) {
                        if ( filteredLine < logData_->getNbLine() ) {
                            return logFilteredData_->getMatchingLineNumber( filteredLine );
                        }
                        else {
                            return maxValue<LineNumber>();
                        }
                    } );

    deleteMarkLinesFromMain( linesInMain );
}

void CrawlerWidget::applyConfiguration()
{
    const auto& config = Configuration::get();
    QFont font = config.mainFont();

    LOG_DEBUG << "CrawlerWidget::applyConfiguration";

    registerShortcuts();

    // Whatever font we use, we should NOT use kerning
    font.setKerning( false );
    font.setFixedPitch( true );

    // Necessary on systems doing subpixel positionning (e.g. Ubuntu 12.04)
    if ( config.forceFontAntialiasing() ) {
        font.setStyleStrategy( QFont::PreferAntialias );
    }

    font.setBold( config.useBoldFont() );

    if ( config.renderAnsiColorSequences() ) {
        logData_->setAnsiProcessingMode( AnsiProcessingMode::Render );
    }
    else if ( config.hideAnsiColorSequences() ) {
        logData_->setAnsiProcessingMode( AnsiProcessingMode::Strip );
    }
    else {
        logData_->setAnsiProcessingMode( AnsiProcessingMode::Plain );
    }
    logData_->setPrefilter( {} );

    logMainView_->setLineNumbersVisible( config.mainLineNumbersVisible() );

    const auto isFollowModeAllowed = logData_ && ( logData_->isLiveSource() || config.anyFileWatchEnabled() );
    logMainView_->allowFollowMode( isFollowModeAllowed );
    overview_.setVisible( config.isOverviewVisible() );
    logMainView_->refreshOverview();
    logMainView_->updateFont( font );

    for ( auto i = 0; i < tabbedFilteredView_->count(); ++i ) {
        auto fv = qobject_cast<FilteredView*>( tabbedFilteredView_->widget( i ) );
        fv->setLineNumbersVisible( config.filteredLineNumbersVisible() );
        fv->allowFollowMode( isFollowModeAllowed );
        fv->updateFont( font );
    }

    // Update the SearchLine (history)
    updateSearchCombo();

    FileWatcher::getFileWatcher().updateConfiguration();

    if ( isFollowEnabled() ) {
        changeDataStatus( DataStatus::OLD_DATA );
    }

    applyEmptyFilterBehavior();
}

void CrawlerWidget::applyEmptyFilterBehavior()
{
    if ( !logFilteredData_ || !filteredView_ || !searchToolbar_ ) {
        return;
    }

    const auto emptyFilterPolicy
        = klogg::emptyLensFilterPolicy( searchToolbar_->currentSearchText().isEmpty(),
                                        Configuration::get().showAllInFilteredViewWhenSearchEmpty() );
    logFilteredData_->setAllLinesVisible( emptyFilterPolicy
                                          == klogg::EmptyFilterPolicy::MirrorAllLines );
    if ( searchToolbar_->currentSearchText().isEmpty() ) {
        filteredView_->updateData();
    }
}

void CrawlerWidget::enteringQuickFind()
{
    LOG_DEBUG << "CrawlerWidget::enteringQuickFind";

    // Remember who had the focus (only if it is one of our views)
    QWidget* focus_widget = QApplication::focusWidget();

    if ( ( focus_widget == logMainView_ ) || ( focus_widget == filteredView_ ) )
        qfSavedFocus_ = focus_widget;
    else
        qfSavedFocus_ = nullptr;
}

void CrawlerWidget::exitingQuickFind()
{
    // Restore the focus once the QFBar has been hidden
    if ( qfSavedFocus_ )
        qfSavedFocus_->setFocus();
}

void CrawlerWidget::loadingFinishedHandler( LoadingStatus status )
{
    LOG_INFO << "file loading finished, status " << static_cast<int>( status );

    // We need to refresh the main window because the view lines on the
    // overview have probably changed.
    overview_.updateData( logData_->getNbLine() );

    // FIXME, handle topLine
    // logMainView_->updateData( logData_, topLine );
    logMainView_->updateData();
    applyEmptyFilterBehavior();

    // Shall we Forbid starting a search when loading in progress?
    // searchButton_->setEnabled( false );

    // searchButton_->setEnabled( true );

    // See if we need to auto-refresh the search
    if ( searchState_.isAutorefreshAllowed() ) {
        searchEndLine_ = LineNumber( logData_->getNbLine().get() );
        if ( searchState_.isFileTruncated() ) {
            // We need to restart the search
            searchUpdateThrottleTimer_.stop();
            searchUpdatePending_ = false;
            replaceCurrentSearch( searchToolbar_->currentSearchText() );
        }
        else if ( logData_->isLiveSource() ) {
            // For live sources, defer search updates through a throttle timer
            // to prevent the main thread from blocking repeatedly on
            // operationsMutex_ inside LogFilteredDataWorker::updateSearch().
            pendingSearchEndLine_ = searchEndLine_;
            searchUpdatePending_ = true;
            if ( !searchUpdateThrottleTimer_.isActive() ) {
                searchUpdateThrottleTimer_.start(
                    window()->isActiveWindow() ? kSearchThrottleActiveMs
                                               : kSearchThrottleInactiveMs );
            }
        }
        else {
            // For static files, also defer through the throttle timer to
            // prevent the main thread from blocking on operationsMutex_ while
            // a previous search is still running.
            pendingSearchEndLine_ = searchEndLine_;
            searchUpdatePending_ = true;
            if ( !searchUpdateThrottleTimer_.isActive() ) {
                searchUpdateThrottleTimer_.start( kSearchThrottleActiveMs );
            }
        }
    }

    // Set the encoding for the views
    updateEncoding();

    clearSearchLimits();

    // Also change the data available icon
    if ( firstLoadDone_ ) {
        changeDataStatus( DataStatus::NEW_DATA );
    }
    else {
        for ( const auto& m : savedMarkedLines_ ) {
            logFilteredData_->addMark( m );
        }
        logMainView_->setFocus();
    }

    loadingInProgress_ = false;
    Q_EMIT loadingFinished( status );

    // Set firstLoadDone_ AFTER emitting loadingFinished so that
    // MainWindow::handleLoadingFinished can distinguish the initial load
    // from incremental updates via isFirstLoadDone().
    firstLoadDone_ = true;
}

void CrawlerWidget::fireThrottledSearchUpdate()
{
    if ( !searchUpdatePending_ || !searchState_.isAutorefreshAllowed() ) {
        searchUpdatePending_ = false;
        return;
    }
    searchUpdatePending_ = false;
    // searchStartLine_ is stable here -- it is only modified by
    // setSearchLimits() which resets the throttle timer.
    logFilteredData_->updateSearch( searchStartLine_, pendingSearchEndLine_ );
}

void CrawlerWidget::fileChangedHandler( MonitoredFileStatus status )
{
    // Handle the case where the file has been truncated
    if ( status == MonitoredFileStatus::Truncated ) {
        // Clear all marks (TODO offer the option to keep them)
        logFilteredData_->clearMarks();
        searchUpdateThrottleTimer_.stop();
        searchUpdatePending_ = false;
        if ( searchPendingLines_ != 0 ) {
            searchPendingLines_ = 0;
            Q_EMIT searchPendingLinesChanged();
        }
        if ( !searchInfoLine_->text().isEmpty() ) {
            // Invalidate the search
            constexpr auto DropCache = true;
            logFilteredData_->clearSearch( DropCache );
            filteredView_->updateData();
            searchState_.truncateFile();
            printSearchInfoMessage();
            nbMatches_ = 0_lcount;
        }
    }
}

// Returns a pointer to the window in which the search should be done
AbstractLogView* CrawlerWidget::activeView() const
{
    QWidget* activeView;

    // Search in the window that has focus, or the window where 'Find' was
    // called from, or the main window.
    if ( filteredView_->hasFocus() || logMainView_->hasFocus() )
        activeView = QApplication::focusWidget();
    else
        activeView = qfSavedFocus_;

    if ( activeView ) {
        auto* view = qobject_cast<AbstractLogView*>( activeView );
        return view;
    }
    else {
        LOG_WARNING << "No active view, defaulting to logMainView";
        return logMainView_;
    }
}

void CrawlerWidget::searchForward()
{
    LOG_DEBUG << "CrawlerWidget::searchForward";

    activeView()->searchForward();
}

void CrawlerWidget::searchBackward()
{
    LOG_DEBUG << "CrawlerWidget::searchBackward";

    activeView()->searchBackward();
}

void CrawlerWidget::resetStateOnSearchPatternChanges()
{
    // We suspend auto-refresh

    searchState_.changeExpression();
    printSearchInfoMessage( logFilteredData_->getNbMatches() );
}

void CrawlerWidget::searchRefreshChangedHandler( bool isRefreshing )
{
    searchState_.setAutorefresh( isRefreshing );
    printSearchInfoMessage( logFilteredData_->getNbMatches() );
}

void CrawlerWidget::changeFilteredViewVisibility( int index )
{
    QStandardItem* item = visibilityModel_->item( index );
    auto visibility = item->data().value<FilteredView::Visibility>();

    filteredView_->setVisibility( visibility );

    if ( logFilteredData_->getNbLine() > 0_lcount ) {
        const auto lineIndex = logFilteredData_->getLineIndexNumber( currentLineNumber_ );
        filteredView_->selectAndDisplayLine( lineIndex );
    }
}

void CrawlerWidget::setSearchPatternFromPredefinedFilters( const QList<PredefinedFilter>& filters )
{
    if ( filters.isEmpty() ) {
        return;
    }

    const auto& filter = filters.front();
    if ( searchToolbar_->isUseRegexp() != filter.useRegex ) {
        searchToolbar_->setUseRegexp( filter.useRegex );
    }

    searchToolbar_->setSearchPattern(
        searchToolbar_->escapeSearchPattern( filter.pattern, filter.useRegex ) );
}

void CrawlerWidget::activityDetected()
{
    changeDataStatus( DataStatus::OLD_DATA );
}

void CrawlerWidget::setSearchLimits( LineNumber startLine, LineNumber endLine )
{
    searchStartLine_ = startLine;
    searchEndLine_ = endLine;

    logMainView_->setSearchLimits( startLine, endLine );
    filteredView_->setSearchLimits( startLine, endLine );
}

void CrawlerWidget::clearSearchLimits()
{
    setSearchLimits( 0_lnum, LineNumber( logData_->getNbLine().get() ) );
}

//
// Private functions
//

// Build the widget and connect all the signals, this must be done once
// the data are attached.
void CrawlerWidget::setup()
{
    LOG_INFO << "Setup crawler widget";
    setOrientation( Qt::Vertical );

    assert( logData_ );
    assert( logFilteredData_ );

    // The views
    auto bottomWindow = new QWidget;
    bottomWindow->setContentsMargins( 2, 0, 2, 0 );

    overviewWidget_ = new OverviewWidget();
    logMainView_
        = new LogMainView( logData_.get(), quickFindPattern_.get(), &overview_, overviewWidget_ );
    logMainView_->setContentsMargins( 2, 0, 2, 0 );

    filteredView_ = new FilteredView( logFilteredData_.get(), quickFindPattern_.get() );
    filteredViewsData_[ filteredView_ ] = logFilteredData_;
    filteredView_->setContentsMargins( 2, 0, 2, 0 );

    overviewWidget_->setOverview( &overview_ );
    overviewWidget_->setParent( logMainView_ );

    // Connect the search to the top view
    logMainView_->useNewFiltering( logFilteredData_.get() );

    // Construct the visibility button
    using VisibilityFlags = LogFilteredData::VisibilityFlags;
    visibilityModel_ = new QStandardItemModel( this );

    QStandardItem* marksAndMatchesItem = new QStandardItem( tr( "Marks and matches" ) );
    marksAndMatchesItem->setData(
        QVariant::fromValue( VisibilityFlags::Marks | VisibilityFlags::Matches ) );
    visibilityModel_->appendRow( marksAndMatchesItem );

    QStandardItem* marksItem = new QStandardItem( tr( "Marks" ) );
    marksItem->setData( QVariant::fromValue<FilteredView::Visibility>( VisibilityFlags::Marks ) );
    visibilityModel_->appendRow( marksItem );

    QStandardItem* matchesItem = new QStandardItem( tr( "Matches" ) );
    matchesItem->setData(
        QVariant::fromValue<FilteredView::Visibility>( VisibilityFlags::Matches ) );
    visibilityModel_->appendRow( matchesItem );

    auto* visibilityView = new QListView( this );
    visibilityView->setMovement( QListView::Static );
    // visibilityView->setMinimumWidth( 170 ); // Only needed with custom style-sheet

    visibilityBox_ = new QComboBox();
    visibilityBox_->setModel( visibilityModel_ );
    visibilityBox_->setView( visibilityView );

    // Select "Marks and matches" by default (same default as the filtered view)
    visibilityBox_->setCurrentIndex( 0 );
    visibilityBox_->setContentsMargins( 2, 2, 2, 2 );

    // TODO: Maybe there is some way to set the popup width to be
    // sized-to-content (as it is when the stylesheet is not overriden) in the
    // stylesheet as opposed to setting a hard min-width on the view above.
    /*visibilityBox_->setStyleSheet( " \
        QComboBox:on {\
            padding: 1px 2px 1px 6px;\
            width: 19px;\
        } \
        QComboBox:!on {\
            padding: 1px 2px 1px 7px;\
            width: 19px;\
            height: 16px;\
            border: 1px solid gray;\
        } \
        QComboBox::drop-down::down-arrow {\
            width: 0px;\
            border-width: 0px;\
        } \
" );*/

    // Construct the Search Info line
    searchInfoLine_ = new InfoLine();
    searchInfoLine_->setFrameStyle( QFrame::StyledPanel );
    searchInfoLine_->setFrameShadow( QFrame::Sunken );
    searchInfoLine_->setLineWidth( 1 );
    searchInfoLine_->setSizePolicy( QSizePolicy::Minimum, QSizePolicy::Minimum );
    auto searchInfoLineSizePolicy = searchInfoLine_->sizePolicy();
    searchInfoLineSizePolicy.setRetainSizeWhenHidden( false );
    searchInfoLine_->setSizePolicy( searchInfoLineSizePolicy );
    searchInfoLineDefaultPalette_ = this->palette();
    searchInfoLine_->setContentsMargins( 2, 2, 2, 2 );

    // Shared search toolbar: owns the search-input QComboBox, option toggles,
    // action buttons, predefined-filters combo and the RegularExpressionPattern
    // construction. CrawlerWidget wires itself to the toolbar's signals below.
    searchToolbar_ = new SearchToolbar( this, savedSearches_ );
    setFocusProxy( searchToolbar_ );

    // Shared view-signal wiring (scratchpad / search composition / splitter /
    // font zoom / exitView / highlightersChange / hover) -- the same component
    // FolderCrawlerWidget uses. Hooks adapt the host-specific parts.
    {
        ViewSignalWiring::Hooks hooks;
        hooks.sendToScratchpad
            = [ this ]( const QString& text ) { Q_EMIT sendToScratchpad( text ); };
        hooks.replaceScratchpad
            = [ this ]( const QString& text ) { Q_EMIT replaceDataInScratchpad( text ); };
        hooks.saveSplitterSizes = [ this ]() { saveSplitterSizes(); };
        hooks.exitView = [ this ]( AbstractLogView* fromView ) {
            if ( fromView == logMainView_ ) {
                if ( filteredView_ != nullptr ) {
                    filteredView_->setFocus();
                }
            }
            else {
                logMainView_->setFocus();
            }
        };
        hooks.applyConfiguration = [ this ]() { applyConfiguration(); };
        hooks.hoveredOverLine = [ this ]( AbstractLogView*, LineNumber line ) {
            overviewWidget_->highlightLine( logFilteredData_->getMatchingLineNumber( line ) );
        };
        hooks.leftHoveringZone = [ this ]() { overviewWidget_->removeHighlight(); };
        viewSignalWiring_
            = std::make_unique<ViewSignalWiring>( this, searchToolbar_, std::move( hooks ) );
    }

    // Context lines controls
    contextLinesSpinBox_ = new QSpinBox();
    contextLinesSpinBox_->setMinimum( 0 );
    contextLinesSpinBox_->setMaximum( 1000 );
    contextLinesSpinBox_->setValue( 0 );
    contextLinesSpinBox_->setToolTip( tr( "Number of context lines" ) );
    contextLinesSpinBox_->setSizePolicy( QSizePolicy::Minimum, QSizePolicy::Minimum );
    const int contextDigitsWidth
        = QFontMetrics( contextLinesSpinBox_->font() ).horizontalAdvance( QStringLiteral( "0000" ) );
    contextLinesSpinBox_->setMinimumWidth( contextDigitsWidth + 32 );

    // Create combobox for context lines mode
    contextLinesComboBox_ = new QComboBox();
    contextLinesComboBox_->setToolTip( tr( "Context lines mode" ) );
    contextLinesComboBox_->setSizePolicy( QSizePolicy::Fixed, QSizePolicy::Fixed );
    contextLinesComboBox_->setContentsMargins( 2, 2, 2, 2 );

    // Add items with user data for mode ID
    contextLinesComboBox_->addItem( tr( "None" ), 0 );
    contextLinesComboBox_->addItem( tr( "Before (-B)" ), 1 );
    contextLinesComboBox_->addItem( tr( "After (-A)" ), 2 );
    contextLinesComboBox_->addItem( tr( "Around (-C)" ), 3 );

    // Set default to None
    contextLinesComboBox_->setCurrentIndex( 0 );

    auto* searchLineLayout = new QHBoxLayout;
    searchLineLayout->setContentsMargins( 2, 2, 2, 2 );

    searchLineLayout->addWidget( visibilityBox_ );
    searchLineLayout->addWidget( searchToolbar_ );
    searchLineLayout->addWidget( contextLinesSpinBox_ );
    searchLineLayout->addWidget( contextLinesComboBox_ );
    searchLineLayout->addWidget( searchInfoLine_ );

    // Construct the bottom window
    tabbedFilteredView_ = new QTabWidget;
    tabbedFilteredView_->setTabsClosable( false );
    tabbedFilteredView_->addTab( filteredView_, "" );
    tabbedFilteredView_->setDocumentMode( true );
    tabbedFilteredView_->setTabBarAutoHide( true );

    auto* bottomMainLayout = new QVBoxLayout;
    bottomMainLayout->addLayout( searchLineLayout );
    bottomMainLayout->addWidget( tabbedFilteredView_ );
    bottomMainLayout->setContentsMargins( 2, 2, 2, 2 );
    bottomWindow->setLayout( bottomMainLayout );

    addWidget( logMainView_ );
    addWidget( bottomWindow );

    // Default search checkboxes (set BEFORE wiring CrawlerWidget to the toolbar
    // signals, matching the original order: the toolbar's internal connections
    // update the completer case sensitivity; the emitted optionsChanged /
    // autoRefreshChanged go nowhere until we connect below).
    auto& config = Configuration::get();
    searchToolbar_->setAutoRefresh( config.isSearchAutoRefreshDefault() );
    searchToolbar_->setMatchCase( !config.isSearchIgnoreCaseDefault() );
    searchToolbar_->setUseRegexp( config.mainRegexpType() == SearchRegexpType::ExtendedRegexp );
    searchToolbar_->setBoolean( config.isSearchLogicalCombiningDefault() );

    // Manually call the handler as it is not called when changing the state programmatically
    searchRefreshChangedHandler( searchToolbar_->isAutoRefresh() );
    resetStateOnSearchPatternChanges();

    // Default splitter position (usually overridden by the config file)
    setSizes( config.splitterSizes() );

    registerShortcuts();
    loadIcons();

    // Wire CrawlerWidget to the shared SearchToolbar's signals.
    // searchRequested (Return / Search clicked, or auto-run from setSearchPattern)
    // -> startNewSearch. Queued so a setSearchPattern-driven auto-run fires on the
    // next event-loop pass (mirrors the original dispatchToMainThread deferral,
    // letting callers like addToSearch/excludeFromSearch unwind first).
    connect( searchToolbar_, &SearchToolbar::searchRequested, this, &CrawlerWidget::startNewSearch,
             Qt::QueuedConnection );
    // stopRequested (Stop clicked) -> stopSearch.
    connect( searchToolbar_, &SearchToolbar::stopRequested, this, &CrawlerWidget::stopSearch );
    // optionsChanged (match case / use regex / boolean toggle) ->
    // resetStateOnSearchPatternChanges (replaces the per-button toggled handlers).
    connect( searchToolbar_, &SearchToolbar::optionsChanged, this,
             &CrawlerWidget::resetStateOnSearchPatternChanges );
    // searchTextChanged (text edited / setSearchPattern) -> resetState + update
    // predefined filters (replaces searchTextChangeHandler).
    connect( searchToolbar_, &SearchToolbar::searchTextChanged, this, [ this ]( QString ) {
        resetStateOnSearchPatternChanges();
        updatePredefinedFiltersWidget();
    } );
    // predefinedFilterActivated (dropdown selection) -> update predefined filters
    // ONLY (must not suspend auto-refresh tracking; restores the original
    // currentIndexChanged -> updatePredefinedFiltersWidget-only split).
    connect( searchToolbar_, &SearchToolbar::predefinedFilterActivated, this,
             [ this ]( QString ) { updatePredefinedFiltersWidget(); } );
    // matchCaseChanged: forwarded to MainWindow (maintains default config).
    connect( searchToolbar_, &SearchToolbar::matchCaseChanged, this, &CrawlerWidget::matchCaseChanged );
    // autoRefreshChanged: run searchRefreshChangedHandler AND forward to MainWindow.
    connect( searchToolbar_, &SearchToolbar::autoRefreshChanged, this,
             &CrawlerWidget::searchRefreshChangedHandler );
    connect( searchToolbar_, &SearchToolbar::autoRefreshChanged, this,
             &CrawlerWidget::searchRefreshChanged );

    // Search-history dialogs remain host-specific.
    connect( searchToolbar_, &SearchToolbar::clearHistoryRequested, this,
             &CrawlerWidget::clearSearchHistory );
    connect( searchToolbar_, &SearchToolbar::editHistoryRequested, this,
             &CrawlerWidget::editSearchHistory );

    // Predefined filters combo is owned by the toolbar; connect its filterChanged.
    connect( searchToolbar_->predefinedFilters(), &PredefinedFiltersComboBox::filterChanged, this,
             &CrawlerWidget::setSearchPatternFromPredefinedFilters );

    // Context lines combo box
    connect( contextLinesComboBox_, QOverload<int>::of( &QComboBox::currentIndexChanged ),
             this, &CrawlerWidget::contextLinesModeChanged );

    // Also connect spinbox changes to update context lines if mode is active
    connect( contextLinesSpinBox_, QOverload<int>::of( &QSpinBox::valueChanged ),
             this, &CrawlerWidget::contextLinesValueChanged );

    connect( visibilityBox_, QOverload<int>::of( &QComboBox::currentIndexChanged ), this,
             &CrawlerWidget::changeFilteredViewVisibility );

    // AbstractLogView self-schedules its viewport repaint on every selection
    // change (mousePressEvent + the keyboard displayLine paths), so the main
    // view no longer needs a host-side connect(newSelection -> update()).
    connect( logMainView_, &LogMainView::newSelection, this,
             &CrawlerWidget::updateLineNumberHandler );

    connect( logMainView_, &LogMainView::markLines, this, &CrawlerWidget::markLinesFromMain );
    connect( logMainView_, &LogMainView::deleteMarkLines, this,
             &CrawlerWidget::deleteMarkLinesFromMain );

    // Shared view-signal wiring: highlightersChange, add/exclude/replace-search,
    // saveDefaultSplitterSizes, changeFontSize, scratchpad, exitView.
    viewSignalWiring_->wireView( logMainView_ );

    // Follow option (up and down)
    connect( this, &CrawlerWidget::followSet, logMainView_, &LogMainView::followSet );
    connect( logMainView_, &LogMainView::followModeChanged, this,
             &CrawlerWidget::followModeChanged );

    connect( this, &CrawlerWidget::textWrapSet, logMainView_, &LogMainView::textWrapSet );

    // Detect activity in the views
    connect( logMainView_, &LogMainView::activity, this, &CrawlerWidget::activityDetected );

    connect( logMainView_, &LogMainView::changeSearchLimits, this,
             &CrawlerWidget::setSearchLimits );

    connect( logMainView_, &LogMainView::clearSearchLimits, this,
             &CrawlerWidget::clearSearchLimits );

    connect( tabbedFilteredView_, &QTabWidget::currentChanged, this,
             &CrawlerWidget::changeFilteredView );

    connect( tabbedFilteredView_, &QTabWidget::tabCloseRequested, this,
             &CrawlerWidget::closeFilteredView );

    connect( logFilteredData_.get(), &LogFilteredData::searchProgressed, this,
             &CrawlerWidget::updateFilteredView,
             static_cast<Qt::ConnectionType>( Qt::QueuedConnection | Qt::UniqueConnection ) );

    // Throttle timer for search updates during live streaming
    searchUpdateThrottleTimer_.setSingleShot( true );
    connect( &searchUpdateThrottleTimer_, &QTimer::timeout, this,
             &CrawlerWidget::fireThrottledSearchUpdate );

    // Sent load file update to MainWindow (for status update)
    connect( logData_.get(), &SearchableLogData::loadingProgressed, this,
             &CrawlerWidget::loadingProgressed );
    connect( logData_.get(), &SearchableLogData::loadingFinished, this,
             &CrawlerWidget::loadingFinishedHandler );
    connect( logData_.get(), &SearchableLogData::fileChanged, this,
             &CrawlerWidget::fileChangedHandler );

    // The option-toggle button connections (search auto-refresh / match case /
    // use regex / boolean) now live in SearchToolbar, which re-emits
    // autoRefreshChanged / matchCaseChanged / optionsChanged. Those are wired
    // above (searchToolbar_ -> handlers / MainWindow-facing signals).

    // Switch between views
    // Color labels: the shared controller owns the manager and keeps every
    // watched view's quick highlighters in sync (context-menu signals +
    // digit shortcuts registered in registerShortcuts).
    colorLabelsController_.watchView( logMainView_ );

    connectAllFilteredViewSlots( filteredView_ );

    updatePredefinedFiltersWidget();
}

void CrawlerWidget::changeFilteredView( int tabIndex )
{
    searchUpdateThrottleTimer_.stop();
    searchUpdatePending_ = false;
    logFilteredData_->interruptSearch();
    if ( tabIndex >= 0 ) {
        auto* tabFilteredView
            = qobject_cast<FilteredView*>( tabbedFilteredView_->widget( tabIndex ) );

        filteredView_ = tabFilteredView;
        logFilteredData_ = filteredViewsData_.at( tabFilteredView );

        Q_EMIT filteredViewChanged();

        logMainView_->useNewFiltering( logFilteredData_.get() );
        changeFilteredViewVisibility( visibilityBox_->currentIndex() );
    }
}

void CrawlerWidget::closeFilteredView( int tabIndex )
{
    if ( tabIndex < 0 || tabIndex >= tabbedFilteredView_->count()
         || tabbedFilteredView_->count() <= 1 ) {
        return;
    }

    if ( tabIndex == tabbedFilteredView_->currentIndex() ) {
        const auto replacementIndex = tabIndex == 0 ? 1 : tabIndex - 1;
        tabbedFilteredView_->setCurrentIndex( replacementIndex );
    }

    auto* tabFilteredView
        = qobject_cast<FilteredView*>( tabbedFilteredView_->widget( tabIndex ) );
    if ( tabFilteredView == nullptr ) {
        return;
    }
    connect( tabFilteredView, &QObject::destroyed, this,
             [ this, tabFilteredView ] { filteredViewDestroyed( tabFilteredView ); } );
    if ( tabbedFilteredView_->count() == 2 ) {
        tabbedFilteredView_->setTabsClosable( false );
    }
    tabFilteredView->deleteLater();
}

void CrawlerWidget::filteredViewDestroyed( FilteredView* view )
{
    if ( filteredView_ == view ) {
        filteredView_ = nullptr;
    }
    filteredViewsData_.erase( view );
}

void CrawlerWidget::saveSplitterSizes() const
{
    LOG_INFO << "Saving default splitter size";
    auto& splitterConfig = Configuration::get();
    splitterConfig.setSplitterSizes( sizes() );
    splitterConfig.save();
}

void CrawlerWidget::connectAllFilteredViewSlots( FilteredView* view )
{
    // AbstractLogView self-schedules its viewport repaint on selection change
    // (mousePressEvent), so the filtered view no longer needs a host-side
    // connect(newSelection -> view->update()).
    connect( view, &FilteredView::newSelection, this, &CrawlerWidget::jumpToMatchingLine );

    connect( view, &FilteredView::markLines, this, &CrawlerWidget::markLinesFromFiltered );
    connect( view, &FilteredView::deleteMarkLines, this,
             &CrawlerWidget::deleteMarkLinesFromFiltered );

    connect( this, &CrawlerWidget::followSet, view, &FilteredView::followSet );

    connect( view, &FilteredView::followModeChanged, this, &CrawlerWidget::followModeChanged );

    connect( this, &CrawlerWidget::textWrapSet, view, &FilteredView::textWrapSet );

    connect( view, &FilteredView::activity, this, &CrawlerWidget::activityDetected );

    connect( view, &FilteredView::changeSearchLimits, this, &CrawlerWidget::setSearchLimits );

    connect( view, &FilteredView::clearSearchLimits, this, &CrawlerWidget::clearSearchLimits );

    // Color labels for this (possibly keep-results) filtered view: shared
    // controller, seeded with any labels already set on the other views.
    colorLabelsController_.watchView( view );

    // Shared view-signal wiring (highlightersChange / search composition /
    // splitter / font zoom / scratchpad / exitView) + hover -> minimap
    // highlight (single-file wires hover on the filtered view only).
    viewSignalWiring_->wireView( view );
    viewSignalWiring_->wireHover( view );
}

void CrawlerWidget::registerShortcuts()
{
    LOG_INFO << "registering shortcuts for crawler widget";

    for ( auto& shortcut : shortcuts_ ) {
        shortcut.second->deleteLater();
    }

    shortcuts_.clear();

    // Widget-level crawler family (visibility cycling, option toggles,
    // keep-results, auto-refresh, top-view resize, Esc refocus): shared with
    // FolderCrawlerWidget via klogg::registerCrawlerShortcuts.
    klogg::CrawlerShortcutHooks hooks;
    hooks.visibilityBox = [ this ]() { return visibilityBox_; };
    hooks.searchToolbar = [ this ]() { return searchToolbar_; };
    hooks.changeTopViewSize = [ this ]( int delta ) { changeTopViewSize( delta ); };
    hooks.activeView = [ this ]() { return activeView(); };
    klogg::registerCrawlerShortcuts( this, shortcuts_, hooks );

    // Color-label shortcuts (1..9 add, 0 remove, Cmd+D next, Cmd+Shift+0 clear)
    // are owned by the shared controller (same actions, same widget-level
    // context as before; also used by FolderCrawlerWidget).
    colorLabelsController_.registerShortcuts();

    logMainView_->registerShortcuts();
    filteredView_->registerShortcuts();
}

void CrawlerWidget::loadIcons()
{
    // All migrated button icons are owned by the SearchToolbar (which has its
    // own IconLoader and reloads on StyleChange via its own changeEvent path).
    searchToolbar_->loadIcons();
}

// Create a new search using the text passed, replace the currently
// used one and destroy the old one.
void CrawlerWidget::replaceCurrentSearch( const QString& searchText )
{
    LOG_INFO << "replacing current search with " << searchText;
    searchUpdateThrottleTimer_.stop();
    searchUpdatePending_ = false;

    // Advance the generation counter BEFORE interrupting.  Every code path
    // out of this function abandons the prior search results (clearSearch()
    // is called below regardless of whether the user typed empty text, an
    // invalid regex, or a valid one); progress signals already queued from
    // the prior search must therefore be treated as stale.
    //
    // The follow-up runSearch() on the valid-regex path will advance the
    // counter again, which is harmless -- only equality matters for the
    // staleness gate.  The bump must NOT live inside interruptSearch():
    // CrawlerWidget::stopSearch also calls interruptSearch() and depends
    // on the final progress signal reaching updateFilteredView() to run
    // the Stop-button UI cleanup.
    logFilteredData_->bumpSearchGeneration();
    logFilteredData_->interruptSearch();

    nbMatches_ = 0_lcount;

    // Switch to "Marks and matches" view when in "Marks" view
    using VisibilityFlags = LogFilteredData::VisibilityFlags;
    if ( !filteredView_->visibility().testFlag( VisibilityFlags::Matches ) ) {
        visibilityBox_->setCurrentIndex( 0 );
    }

    // Clear and recompute the content of the filtered window.
    logFilteredData_->clearSearch();
    if ( klogg::emptyLensFilterPolicy(
             searchText.isEmpty(),
             Configuration::get().showAllInFilteredViewWhenSearchEmpty() )
         == klogg::EmptyFilterPolicy::MirrorAllLines ) {
        logFilteredData_->setAllLinesVisible( true );
    }
    filteredView_->updateData();

    // Update the match overview
    overview_.updateData( logData_->getNbLine() );

    if ( !searchText.isEmpty() ) {

        // Constructs the regexp (verbatim construction now lives in
        // SearchToolbar::currentRegularExpressionPattern()).
        auto regexpPattern = searchToolbar_->currentRegularExpressionPattern();

        RegularExpression hsExpression{ regexpPattern };
        auto isValidExpression = hsExpression.isValid();

        if ( isValidExpression ) {
            // Activate the stop button
            searchToolbar_->setSearchInProgress( true );
            // Start a new asynchronous search
            logFilteredData_->runSearch( regexpPattern, searchStartLine_, searchEndLine_ );
            // Accept auto-refresh of the search
            searchState_.startSearch();
            searchInfoLine_->hide();
            logMainView_->setSearchPattern( regexpPattern );
            filteredView_->setSearchPattern( regexpPattern );
        }
        else {
            // The regexp is wrong
            logFilteredData_->clearSearch();
            filteredView_->updateData();
            searchState_.resetState();

            // Inform the user
            QString errorString = hsExpression.errorString();
            QString errorMessage = tr( "Error in expression" );
            // const int offset = regexp.patternErrorOffset();
            // if ( offset != -1 ) {
            //     errorMessage += " at position ";
            //     errorMessage += QString::number( offset );
            // }
            errorMessage += ": ";
            errorMessage += errorString;
            searchInfoLine_->setPalette( ErrorPalette );
            searchInfoLine_->setText( errorMessage );
            searchInfoLine_->show();

            logMainView_->setSearchPattern( {} );
            filteredView_->setSearchPattern( {} );
        }
    }
    else {
        searchState_.resetState();
        printSearchInfoMessage();
    }
}

// Updates the content of the drop down list for the saved searches,
// called when the SavedSearch has been changed.
void CrawlerWidget::updateSearchCombo()
{
    auto searchHistory = savedSearches_->recentSearches();
    searchToolbar_->setItems( searchHistory );
}

// Print the search info message.
void CrawlerWidget::printSearchInfoMessage( LinesCount nbMatches )
{
    QString text;

    switch ( searchState_.getState() ) {
    case SearchState::NoSearch:
        // Blank text is fine
        break;
    case SearchState::Static:
    case SearchState::Autorefreshing:
        // Some languages translate the plural the same as the singular, so use the full string
        text = nbMatches.get() > 1 ? tr( "%1 matches found" ).arg( nbMatches.get() )
                                   : tr( "%1 match found" ).arg( nbMatches.get() );
        break;
    case SearchState::FileTruncated:
    case SearchState::TruncatedAutorefreshing:
        text = tr( "File truncated on disk" );
        break;
    }

    searchInfoLine_->setPalette( searchInfoLineDefaultPalette_ );
    searchInfoLine_->setText( text );
    searchInfoLine_->setVisible( !text.isEmpty() );
}

// Change the data status and, if needed, advise upstream.
void CrawlerWidget::changeDataStatus( DataStatus status )
{
    if ( ( status != dataStatus_ )
         && ( !( dataStatus_ == DataStatus::NEW_FILTERED_DATA
                 && status == DataStatus::NEW_DATA ) ) ) {
        dataStatus_ = status;
        Q_EMIT dataStatusChanged( dataStatus_ );
    }
}

// Determine the right encoding and set the views.
void CrawlerWidget::updateEncoding()
{
    const QTextCodec* textCodec = [ this ]() {
        QTextCodec* codec = nullptr;
        if ( !encodingMib_ ) {
            codec = logData_->getDetectedEncoding();
        }
        else {
            codec = QTextCodec::codecForMib( *encodingMib_ );
        }

        if ( codec ) {
            return codec;
        }

        const auto defaultEncodingMib = Configuration::get().defaultEncodingMib();
        if ( defaultEncodingMib >= 0 ) {
            codec = QTextCodec::codecForMib( defaultEncodingMib );
        }

        return codec ? codec : QTextCodec::codecForName( "UTF-8" );
    }();

    QString encodingPrefix = encodingMib_ ? tr( "Displayed as %1" ) : tr( "Detected as %1" );
    encodingText_ = encodingPrefix.arg( textCodec->name().constData() );

    logData_->interruptLoading();

    logData_->setDisplayEncoding( textCodec->name().constData() );
    logMainView_->forceRefresh();
    logFilteredData_->setDisplayEncoding( textCodec->name().constData() );
    filteredView_->forceRefresh();
}

// Change the respective size of the two views
void CrawlerWidget::changeTopViewSize( int32_t delta )
{
    int min, max;
    getRange( 1, &min, &max );
    LOG_DEBUG << "CrawlerWidget::changeTopViewSize " << sizes().at( 0 ) << " " << min << " " << max;
    moveSplitter( closestLegalPosition( sizes().at( 0 ) + ( delta * 10 ), 1 ), 1 );
    LOG_DEBUG << "CrawlerWidget::changeTopViewSize " << sizes().at( 0 );
}

//
// SearchState implementation
//
void CrawlerWidget::SearchState::resetState()
{
    state_ = NoSearch;
}

void CrawlerWidget::SearchState::setAutorefresh( bool refresh )
{
    autoRefreshRequested_ = refresh;

    if ( refresh ) {
        if ( state_ == Static )
            state_ = Autorefreshing;
        /*
        else if ( state_ == FileTruncated )
            state_ = TruncatedAutorefreshing;
        */
    }
    else {
        if ( state_ == Autorefreshing )
            state_ = Static;
        else if ( state_ == TruncatedAutorefreshing )
            state_ = FileTruncated;
    }
}

void CrawlerWidget::SearchState::truncateFile()
{
    if ( state_ == Autorefreshing || state_ == TruncatedAutorefreshing ) {
        state_ = TruncatedAutorefreshing;
    }
    else {
        state_ = FileTruncated;
    }
}

void CrawlerWidget::SearchState::changeExpression()
{
    if ( state_ == Autorefreshing )
        state_ = Static;
}

void CrawlerWidget::SearchState::stopSearch()
{
    if ( state_ == Autorefreshing )
        state_ = Static;
}

void CrawlerWidget::SearchState::startSearch()
{
    if ( autoRefreshRequested_ )
        state_ = Autorefreshing;
    else
        state_ = Static;
}

/*
 * CrawlerWidgetContext
 */
CrawlerWidgetContext::CrawlerWidgetContext( const QString& string )
{
    if ( string.startsWith( '{' ) ) {
        loadFromJson( string );
    }
    else {
        loadFromString( string );
    }
}

void CrawlerWidgetContext::loadFromString( const QString& string )
{
    QRegularExpression regex( "S(\\d+):(\\d+)" );
    QRegularExpressionMatch match = regex.match( string );
    if ( match.hasMatch() ) {
        sizes_ = { match.captured( 1 ).toInt(), match.captured( 2 ).toInt() };
        LOG_DEBUG << "sizes_: " << sizes_[ 0 ] << " " << sizes_[ 1 ];
    }
    else {
        LOG_WARNING << "Unrecognised view size: " << string.toLocal8Bit().data();

        // Default values;
        sizes_ = { 400, 100 };
    }

    QRegularExpression case_refresh_regex( "IC(\\d+):AR(\\d+)" );
    match = case_refresh_regex.match( string );
    if ( match.hasMatch() ) {
        ignoreCase_ = ( match.captured( 1 ).toInt() == 1 );
        autoRefresh_ = ( match.captured( 2 ).toInt() == 1 );

        LOG_DEBUG << "ignore_case_: " << ignoreCase_ << " auto_refresh_: " << autoRefresh_;
    }
    else {
        LOG_WARNING << "Unrecognised case/refresh: " << string.toLocal8Bit().data();
        ignoreCase_ = false;
        autoRefresh_ = false;
    }

    QRegularExpression follow_regex( "AR(\\d+):FF(\\d+)" );
    match = follow_regex.match( string );
    if ( match.hasMatch() ) {
        followFile_ = ( match.captured( 2 ).toInt() == 1 );

        LOG_DEBUG << "follow_file_: " << followFile_;
    }
    else {
        LOG_WARNING << "Unrecognised follow file " << string.toLocal8Bit().data();
        followFile_ = false;
    }

    useRegexp_ = Configuration::get().mainRegexpType() == SearchRegexpType::ExtendedRegexp;
}

void CrawlerWidgetContext::loadFromJson( const QString& json )
{
    const auto properties = QJsonDocument::fromJson( json.toLatin1() ).toVariant().toMap();

    if ( properties.contains( "S" ) ) {
        const auto sizes = properties.value( "S" ).toList();
        for ( const auto& s : sizes ) {
            sizes_.append( s.toInt() );
        }
    }

    ignoreCase_ = properties.value( "IC" ).toBool();
    autoRefresh_ = properties.value( "AR" ).toBool();
    followFile_ = properties.value( "FF" ).toBool();
    if ( properties.contains( "RE" ) ) {
        useRegexp_ = properties.value( "RE" ).toBool();
    }
    else {
        useRegexp_ = Configuration::get().mainRegexpType() == SearchRegexpType::ExtendedRegexp;
    }

    if ( properties.contains( "IR" ) ) {
        inverseRegexp_ = properties.value( "IR" ).toBool();
    }
    else {
        inverseRegexp_ = false;
    }

    if ( properties.contains( "BC" ) ) {
        useBooleanCombination_ = properties.value( "BC" ).toBool();
    }
    else {
        useBooleanCombination_ = false;
    }

    if ( properties.contains( "M" ) ) {
        const auto marks = properties.value( "M" ).toList();
        for ( const auto& m : marks ) {
            marks_.append( m.toUInt() );
        }
    }
}

QString CrawlerWidgetContext::toString() const
{
    const auto toVariantList = []( const auto& list ) -> QVariantList {
        QVariantList variantList;
        for ( const auto& item : list ) {
            variantList.append( static_cast<qulonglong>( item ) );
        }
        return variantList;
    };

    QVariantMap properies;

    properies[ "S" ] = toVariantList( sizes_ );
    properies[ "IC" ] = ignoreCase_;
    properies[ "AR" ] = autoRefresh_;
    properies[ "FF" ] = followFile_;
    properies[ "RE" ] = useRegexp_;
    properies[ "IR" ] = inverseRegexp_;
    properies[ "BC" ] = useBooleanCombination_;
    properies[ "M" ] = toVariantList( marks_ );

    return QJsonDocument::fromVariant( properies ).toJson( QJsonDocument::Compact );
}

void CrawlerWidget::contextLinesModeChanged( int index )
{
    // Get mode from user data
    const int mode = contextLinesComboBox_->itemData( index ).toInt();
    contextLinesMode_ = mode;
    
    // Apply context lines if value is non-zero
    if ( contextLinesSpinBox_->value() > 0 ) {
        applyContextLines();
    }
    else {
        // If value is 0, clear context lines
        logFilteredData_->setContextLines( 0, 0 );
        filteredView_->updateData();
    }
}

void CrawlerWidget::contextLinesValueChanged( int value )
{
    // Apply context lines if mode is active
    if ( contextLinesMode_ > 0 && value > 0 ) {
        applyContextLines();
    }
    else {
        // Clear context lines if value is 0 or no mode selected
        logFilteredData_->setContextLines( 0, 0 );
        // Note: We intentionally don't reset contextLinesMode_ here when value becomes 0,
        // so that the user's selection (-A/-B/-C) persists for future value changes.
        filteredView_->updateData();
    }
}

void CrawlerWidget::applyContextLines()
{
    if ( !logFilteredData_ || contextLinesMode_ == 0 || contextLinesSpinBox_->value() == 0 ) {
        return;
    }
    
    const int n = contextLinesSpinBox_->value();
    
    // Apply context lines based on mode: 1 = before (-B), 2 = after (-A), 3 = both (-C)
    switch ( contextLinesMode_ ) {
        case 1: // -B
            logFilteredData_->setContextLines( n, 0 );
            break;
        case 2: // -A
            logFilteredData_->setContextLines( 0, n );
            break;
        case 3: // -C
            logFilteredData_->setContextLines( n, n );
            break;
        default:
            logFilteredData_->setContextLines( 0, 0 );
            return;
    }
    
    // Use updateData() instead of update() for better performance
    // This triggers a full refresh but is more efficient than multiple updates
    filteredView_->updateData();
}
