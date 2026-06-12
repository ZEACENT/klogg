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
#include "linetypes.h"
#include "log.h"
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
#include <QMessageBox>
#include <QShortcut>
#include <QStandardItemModel>
#include <QStringListModel>
#include <qglobal.h>
#include <qobject.h>
#include <string>

#include "regularexpression.h"

#include "crawlerwidget.h"

#include "configuration.h"
#include "dispatch_to.h"
#include "filewatcher.h"
#include "filterdiffdialog.h"
#include "fontutils.h"
#include "infoline.h"
#include "predefinedfilters.h"
#include "quickfindpattern.h"
#include "savefavoritedialog.h"
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
CrawlerWidget::CrawlerWidget( QWidget* parent )
    : QSplitter( parent )
    , iconLoader_{ this }
{
}

// The top line is first one on the main display
LineNumber CrawlerWidget::getTopLine() const
{
    return logMainView_->getTopLine();
}

QString CrawlerWidget::getSelectedText() const
{
    if ( filteredView_->hasFocus() )
        return filteredView_->getSelectedText();
    else
        return logMainView_->getSelectedText();
}

bool CrawlerWidget::isPartialSelection() const
{
    if ( filteredView_->hasFocus() )
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

void CrawlerWidget::reloadPredefinedFilters() const
{
    predefinedFilters_->populatePredefinedFilters();
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
    searchLineEdit_->lineEdit()->setFocus( Qt::ShortcutFocusReason );
}

bool CrawlerWidget::eventFilter( QObject* watched, QEvent* event )
{
    const auto* searchEdit = searchLineEdit_ != nullptr ? searchLineEdit_->lineEdit() : nullptr;
    const auto isSearchInput = watched == searchLineEdit_ || watched == searchEdit;
    if ( !isSearchInput ) {
        return QSplitter::eventFilter( watched, event );
    }

    if ( event->type() != QEvent::ShortcutOverride && event->type() != QEvent::KeyPress ) {
        return QSplitter::eventFilter( watched, event );
    }

    auto* keyEvent = static_cast<QKeyEvent*>( event );
    const auto key = keyEvent->key();
    if ( key != Qt::Key_Home && key != Qt::Key_End ) {
        return QSplitter::eventFilter( watched, event );
    }

    const auto modifiers = keyEvent->modifiers() & ~Qt::KeypadModifier;
    if ( modifiers != Qt::NoModifier ) {
        return QSplitter::eventFilter( watched, event );
    }

    if ( event->type() == QEvent::ShortcutOverride ) {
        event->accept();
        return false;
    }

    if ( key == Qt::Key_Home ) {
        searchLineEdit_->lineEdit()->setCursorPosition( 0 );
    }
    else {
        searchLineEdit_->lineEdit()->end( false );
    }
    event->accept();
    return true;
}

void CrawlerWidget::jumpToTop()
{
    logMainView_->selectAndDisplayLine( 0_lnum );
    filteredView_->selectAndDisplayLine( 0_lnum );
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
    matchCaseButton_->setChecked( !context.ignoreCase() );
    useRegexpButton_->setChecked( context.useRegexp() );
    inverseButton_->setChecked( context.inverseRegexp() );
    booleanButton_->setChecked( context.useBooleanCombination() );

    searchRefreshButton_->setChecked( context.autoRefresh() );
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
        sizes(), ( !matchCaseButton_->isChecked() ), searchRefreshButton_->isChecked(),
        logMainView_->isFollowEnabled(), useRegexpButton_->isChecked(), inverseButton_->isChecked(),
        booleanButton_->isChecked(), logFilteredData_->getMarks() );

    return static_cast<std::shared_ptr<const ViewContextInterface>>( context );
}

//
// Q_SLOTS:
//

void CrawlerWidget::startNewSearch()
{
    if ( keepSearchResultsButton_->isChecked() ) {
        keepSearchResultsButton_->setChecked( false );

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
        tabbedFilteredView_->setCurrentIndex( index );

        connect( logFilteredData_.get(), &LogFilteredData::searchProgressed, this,
                 &CrawlerWidget::updateFilteredView,
                 static_cast<Qt::ConnectionType>( Qt::QueuedConnection | Qt::UniqueConnection ) );

        logMainView_->useNewFiltering( logFilteredData_.get() );

        applyConfiguration();
    }

    tabbedFilteredView_->setTabText( tabbedFilteredView_->currentIndex(),
                                     "Find \"" + searchLineEdit_->currentText() + "\"" );

    // Record the search line in the recent list
    // (reload the list first in case another glogg changed it)
    const auto& searches = SavedSearches::getSynced();
    savedSearches_->addRecent( searchLineEdit_->currentText() );
    searches.save();

    // Update the SearchLine (history)
    updateSearchCombo();
    // Call the private function to do the search
    replaceCurrentSearch( searchLineEdit_->currentText() );
}

void CrawlerWidget::updatePredefinedFiltersWidget()
{
    predefinedFilters_->updateSearchPattern( searchLineEdit_->currentText(),
                                             booleanButton_->isChecked() );
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
    searchLineEdit_->clear();

    // Sync and clear saved searches
    auto& searches = SavedSearches::getSynced();
    savedSearches_->clear();
    searches.save();

    searchLineCompleter_->setModel( new QStringListModel( {}, searchLineCompleter_ ) );
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
#if QT_VERSION >= QT_VERSION_CHECK( 5, 15, 0 )
        auto items = newHistory.split( QChar::LineFeed, Qt::SkipEmptyParts );
#else
        auto items = newHistory.split( QChar::LineFeed, QString::SkipEmptyParts );
#endif
        std::for_each( items.rbegin(), items.rend(), [ this ]( const auto& item ) {
            savedSearches_->addRecent( item );
            LOG_INFO << item;
        } );
    }
    searches.save();

    updateSearchCombo();
}

void CrawlerWidget::saveAsFavorite()
{
    const auto currentText = searchLineEdit_->currentText().trimmed();
    if ( currentText.isEmpty() ) {
        return;
    }

    auto filters = PredefinedFiltersCollection::getSynced().getFilters();
    const auto useRegex = useRegexpButton_->isChecked();

    SaveFavoriteDialog dialog( currentText, filters, this );
    if ( dialog.exec() != QDialog::Accepted ) {
        return;
    }

    const auto trimmedName = dialog.favoriteName();
    if ( trimmedName.isEmpty() ) {
        return;
    }

    if ( dialog.isCreateNew() ) {
        // Check if a filter with this name already exists
        auto existing = std::find_if(
            filters.begin(), filters.end(), [ &trimmedName ]( const auto& filter ) {
                return filter.name.compare( trimmedName, Qt::CaseInsensitive ) == 0;
            } );

        if ( existing != filters.end() ) {
            const auto isSamePattern = ( existing->pattern == currentText );
            const auto isSameRegex = ( existing->useRegex == useRegex );
            if ( isSamePattern && isSameRegex ) {
                QMessageBox::information(
                    this, tr( "klogg" ),
                    tr( "Favorite \"%1\" already exists with the same content." )
                        .arg( trimmedName ) );
                return;
            }

            // Show diff dialog for confirmation
            FilterDiffDialog diffDialog( trimmedName, *existing, currentText, useRegex, this );
            if ( diffDialog.exec() != QDialog::Accepted ) {
                return;
            }

            existing->pattern = currentText;
            existing->useRegex = useRegex;
        }
        else {
            filters.push_back( { trimmedName, currentText, useRegex } );
        }
    }
    else {
        // Overwriting an existing filter
        const int index = dialog.selectedExistingIndex();
        if ( index < 0 || index >= filters.size() ) {
            return;
        }

        auto& existing = filters[ index ];

        const auto isSamePattern = ( existing.pattern == currentText );
        const auto isSameRegex = ( existing.useRegex == useRegex );
        if ( isSamePattern && isSameRegex ) {
            QMessageBox::information(
                this, tr( "klogg" ),
                tr( "Favorite \"%1\" already has the same content." ).arg( existing.name ) );
            return;
        }

        // Show diff dialog for confirmation
        FilterDiffDialog diffDialog( existing.name, existing, currentText, useRegex, this );
        if ( diffDialog.exec() != QDialog::Accepted ) {
            return;
        }

        existing.pattern = currentText;
        existing.useRegex = useRegex;
    }

    PredefinedFiltersCollection::getSynced().saveToStorage( filters );
    reloadPredefinedFilters();
}

void CrawlerWidget::showSearchContextMenu()
{
    if ( searchLineContextMenu_ )
        searchLineContextMenu_->exec( QCursor::pos( activeScreen( this ) ) );
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
        stopButton_->setEnabled( false );
        stopButton_->hide();
        searchButton_->show();
        clearButton_->show();
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

    reloadPredefinedFilters();
    applyEmptyFilterBehavior();
}

void CrawlerWidget::applyEmptyFilterBehavior()
{
    if ( !logFilteredData_ || !filteredView_ || !searchLineEdit_ ) {
        return;
    }

    const bool showAll = searchLineEdit_->currentText().isEmpty()
        && Configuration::get().showAllInFilteredViewWhenSearchEmpty();
    logFilteredData_->setAllLinesVisible( showAll );
    if ( searchLineEdit_->currentText().isEmpty() ) {
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
            replaceCurrentSearch( searchLineEdit_->currentText() );
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

void CrawlerWidget::matchCaseChangedHandler( bool shouldMatchCase )
{
    searchLineCompleter_->setCaseSensitivity( shouldMatchCase ? Qt::CaseSensitive
                                                              : Qt::CaseInsensitive );

    resetStateOnSearchPatternChanges();
}

void CrawlerWidget::booleanCombiningChangedHandler( bool )
{
    resetStateOnSearchPatternChanges();
}

void CrawlerWidget::useRegexpChangeHandler( bool )
{
    resetStateOnSearchPatternChanges();
}

void CrawlerWidget::searchTextChangeHandler( QString )
{
    resetStateOnSearchPatternChanges();
    updatePredefinedFiltersWidget();
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
    if ( useRegexpButton_->isChecked() != filter.useRegex ) {
        useRegexpButton_->setChecked( filter.useRegex );
    }

    setSearchPattern( escapeSearchPattern( filter.pattern, filter.useRegex ) );
}

QString CrawlerWidget::escapeSearchPattern( const QString& pattern, bool isRegex ) const
{
    auto escapedPattern = ( !isRegex && useRegexpButton_->isChecked() )
                              ? QRegularExpression::escape( pattern )
                              : pattern;

    if ( booleanButton_->isChecked() ) {
        escapedPattern.replace( '"', "\"" ).prepend( '"' ).append( '"' );
    }

    return escapedPattern;
}

QString& CrawlerWidget::combinePatterns( QString& currentPattern, const QString& newPattern ) const
{
    if ( !currentPattern.isEmpty() ) {
        if ( booleanButton_->isChecked() ) {
            currentPattern.append( " or " );
        }
        else if ( useRegexpButton_->isChecked() ) {
            currentPattern.append( '|' );
        }
    }

    currentPattern.append( newPattern );

    return currentPattern;
}

void CrawlerWidget::addToSearch( const QString& searchString )
{
    const auto newPattern = escapeSearchPattern( searchString );
    QString currentPattern = searchLineEdit_->currentText();
    setSearchPattern( combinePatterns( currentPattern, newPattern ) );
}

void CrawlerWidget::excludeFromSearch( const QString& searchString )
{
    QString currentPattern = searchLineEdit_->currentText();

    const auto wasInBooleanCombinationMode = booleanButton_->isChecked();
    if ( !wasInBooleanCombinationMode ) {
        currentPattern.replace( '"', "\"" ).prepend( '"' ).append( '"' );
    }

    booleanButton_->setChecked( true );

    const auto newPattern = escapeSearchPattern( searchString );

    if ( !currentPattern.isEmpty() ) {
        currentPattern.append( " and " );
    }

    currentPattern.append( "not(" ).append( newPattern ).append( ')' );
    setSearchPattern( currentPattern );
}

void CrawlerWidget::replaceSearch( const QString& searchString )
{
    setSearchPattern( escapeSearchPattern( searchString ) );
}

void CrawlerWidget::setSearchPattern( const QString& searchPattern )
{
    searchLineEdit_->setEditText( searchPattern );
    updatePredefinedFiltersWidget();
    // Set the focus to lineEdit so that the user can press 'Return' immediately
    searchLineEdit_->lineEdit()->setFocus();

    if ( Configuration::get().autoRunSearchOnPatternChange() ) {
        dispatchToMainThread( [ this ] { startNewSearch(); } );
    }
}

void CrawlerWidget::mouseHoveredOverMatch( LineNumber line )
{
    const auto line_in_mainview = logFilteredData_->getMatchingLineNumber( line );

    overviewWidget_->highlightLine( line_in_mainview );
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

    matchCaseButton_ = new QToolButton();
    matchCaseButton_->setToolTip( tr( "Match case" ) );
    matchCaseButton_->setCheckable( true );
    matchCaseButton_->setFocusPolicy( Qt::NoFocus );
    matchCaseButton_->setContentsMargins( 2, 2, 2, 2 );

    useRegexpButton_ = new QToolButton();
    useRegexpButton_->setToolTip( tr( "Use regex" ) );
    useRegexpButton_->setCheckable( true );
    useRegexpButton_->setFocusPolicy( Qt::NoFocus );
    useRegexpButton_->setContentsMargins( 2, 2, 2, 2 );

    inverseButton_ = new QToolButton();
    inverseButton_->setToolTip( tr( "Inverse match" ) );
    inverseButton_->setCheckable( true );
    inverseButton_->setFocusPolicy( Qt::NoFocus );
    inverseButton_->setContentsMargins( 2, 2, 2, 2 );

    booleanButton_ = new QToolButton();
    booleanButton_->setToolTip( tr( "Enable regular expression logical combining" ) );
    booleanButton_->setCheckable( true );
    booleanButton_->setFocusPolicy( Qt::NoFocus );
    booleanButton_->setContentsMargins( 2, 2, 2, 2 );

    searchRefreshButton_ = new QToolButton();
    searchRefreshButton_->setToolTip( tr( "Auto-refresh" ) );
    searchRefreshButton_->setCheckable( true );
    searchRefreshButton_->setFocusPolicy( Qt::NoFocus );
    searchRefreshButton_->setContentsMargins( 2, 2, 2, 2 );

    // Construct the Search line
    searchLineCompleter_ = new QCompleter( savedSearches_->recentSearches(), this );
    searchLineEdit_ = new QComboBox;
    searchLineEdit_->setEditable( true );
    searchLineEdit_->setCompleter( searchLineCompleter_ );
    searchLineEdit_->addItems( savedSearches_->recentSearches() );
    searchLineEdit_->lineEdit()->clear();
    searchLineEdit_->setSizePolicy( QSizePolicy::Expanding, QSizePolicy::Minimum );
    searchLineEdit_->setSizeAdjustPolicy( QComboBox::AdjustToMinimumContentsLengthWithIcon );
    searchLineEdit_->lineEdit()->setMaxLength( std::numeric_limits<int>::max() / 1024 );
    searchLineEdit_->setContentsMargins( 2, 2, 2, 2 );
    searchLineEdit_->installEventFilter( this );
    searchLineEdit_->lineEdit()->installEventFilter( this );

    QAction* clearSearchHistoryAction = new QAction( tr( "Clear search history" ), this );
    QAction* editSearchHistoryAction = new QAction( tr( "Edit search history" ), this );
    QAction* addFavoriteFilterAction = new QAction( tr( "Add to favorites" ), this );

    searchLineContextMenu_ = searchLineEdit_->lineEdit()->createStandardContextMenu();
    searchLineContextMenu_->addSeparator();
    searchLineContextMenu_->addAction( addFavoriteFilterAction );
    searchLineContextMenu_->addSeparator();
    searchLineContextMenu_->addAction( editSearchHistoryAction );
    searchLineContextMenu_->addAction( clearSearchHistoryAction );
    searchLineEdit_->setContextMenuPolicy( Qt::CustomContextMenu );

    setFocusProxy( searchLineEdit_ );

    clearButton_ = new QToolButton();
    clearButton_->setText( tr( "Clear search text" ) );
    clearButton_->setAutoRaise( true );
    clearButton_->setContentsMargins( 2, 2, 2, 2 );

    searchButton_ = new QToolButton();
    searchButton_->setText( tr( "Search" ) );
    searchButton_->setAutoRaise( true );
    searchButton_->setContentsMargins( 2, 2, 2, 2 );

    keepSearchResultsButton_ = new QToolButton();
    keepSearchResultsButton_->setText( tr( "Keep Results" ) );
    keepSearchResultsButton_->setToolTip(
        tr( "Keep these results and show subsequent results in a new window" ) );
    keepSearchResultsButton_->setCheckable( true );
    keepSearchResultsButton_->setContentsMargins( 2, 2, 2, 2 );

    stopButton_ = new QToolButton();
    stopButton_->setAutoRaise( true );
    stopButton_->setEnabled( false );
    stopButton_->setVisible( false );
    stopButton_->setContentsMargins( 2, 2, 2, 2 );

    predefinedFilters_ = new PredefinedFiltersComboBox( this );
    favoriteFilterButton_ = new QToolButton();
    favoriteFilterButton_->setAutoRaise( true );
    favoriteFilterButton_->setToolTip( tr( "Add current filter to favorites" ) );
    favoriteFilterButton_->setFocusPolicy( Qt::NoFocus );
    favoriteFilterButton_->setContentsMargins( 2, 2, 2, 2 );

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
    searchLineLayout->addWidget( matchCaseButton_ );
    searchLineLayout->addWidget( useRegexpButton_ );
    searchLineLayout->addWidget( inverseButton_ );
    searchLineLayout->addWidget( booleanButton_ );
    searchLineLayout->addWidget( searchRefreshButton_ );
    searchLineLayout->addWidget( predefinedFilters_ );
    searchLineLayout->addWidget( favoriteFilterButton_ );
    searchLineLayout->addWidget( searchLineEdit_ );
    searchLineLayout->addWidget( contextLinesSpinBox_ );
    searchLineLayout->addWidget( contextLinesComboBox_ );
    searchLineLayout->addWidget( clearButton_ );
    searchLineLayout->addWidget( searchButton_ );
    searchLineLayout->addWidget( keepSearchResultsButton_ );
    searchLineLayout->addWidget( stopButton_ );
    searchLineLayout->addWidget( searchInfoLine_ );

    // Construct the bottom window
    tabbedFilteredView_ = new QTabWidget;
    tabbedFilteredView_->setTabsClosable( true );
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

    // Default search checkboxes
    auto& config = Configuration::get();
    searchRefreshButton_->setChecked( config.isSearchAutoRefreshDefault() );
    matchCaseButton_->setChecked( !config.isSearchIgnoreCaseDefault() );
    useRegexpButton_->setChecked( config.mainRegexpType() == SearchRegexpType::ExtendedRegexp );
    booleanButton_->setChecked( config.isSearchLogicalCombiningDefault() );

    // Manually call the handler as it is not called when changing the state programmatically
    searchRefreshChangedHandler( searchRefreshButton_->isChecked() );
    useRegexpChangeHandler( useRegexpButton_->isChecked() );
    matchCaseChangedHandler( matchCaseButton_->isChecked() );
    booleanCombiningChangedHandler( booleanButton_->isChecked() );

    // Default splitter position (usually overridden by the config file)
    setSizes( config.splitterSizes() );

    registerShortcuts();
    loadIcons();

    // Connect the signals
    connect( searchLineEdit_->lineEdit(), &QLineEdit::returnPressed, searchButton_,
             &QToolButton::click );
    connect( searchLineEdit_->lineEdit(), &QLineEdit::textEdited, this,
             &CrawlerWidget::searchTextChangeHandler );

    connect( searchLineEdit_, QOverload<int>::of( &QComboBox::currentIndexChanged ), this,
             [ this ]( auto ) { updatePredefinedFiltersWidget(); } );

    connect( predefinedFilters_, &PredefinedFiltersComboBox::filterChanged, this,
             &CrawlerWidget::setSearchPatternFromPredefinedFilters );

    connect( searchLineEdit_, &QWidget::customContextMenuRequested, this,
             &CrawlerWidget::showSearchContextMenu );
    connect( addFavoriteFilterAction, &QAction::triggered, this,
             &CrawlerWidget::saveAsFavorite );
    connect( clearSearchHistoryAction, &QAction::triggered, this,
             &CrawlerWidget::clearSearchHistory );
    connect( editSearchHistoryAction, &QAction::triggered, this,
             &CrawlerWidget::editSearchHistory );
    connect( searchButton_, &QToolButton::clicked, this, &CrawlerWidget::startNewSearch );
    connect( favoriteFilterButton_, &QToolButton::clicked, this, &CrawlerWidget::saveAsFavorite );
    connect( stopButton_, &QToolButton::clicked, this, &CrawlerWidget::stopSearch );
    connect( clearButton_, &QToolButton::clicked, searchLineEdit_, &QComboBox::clearEditText );

    // Context lines combo box
    connect( contextLinesComboBox_, QOverload<int>::of( &QComboBox::currentIndexChanged ),
             this, &CrawlerWidget::contextLinesModeChanged );
    
    // Also connect spinbox changes to update context lines if mode is active
    connect( contextLinesSpinBox_, QOverload<int>::of( &QSpinBox::valueChanged ),
             this, &CrawlerWidget::contextLinesValueChanged );

    connect( visibilityBox_, QOverload<int>::of( &QComboBox::currentIndexChanged ), this,
             &CrawlerWidget::changeFilteredViewVisibility );

    connect( logMainView_, &LogMainView::newSelection,
             [ this ]( auto ) { logMainView_->update(); } );

    connect( logMainView_, &LogMainView::newSelection, this,
             &CrawlerWidget::updateLineNumberHandler );

    connect( logMainView_, &LogMainView::markLines, this, &CrawlerWidget::markLinesFromMain );
    connect( logMainView_, &LogMainView::deleteMarkLines, this,
             &CrawlerWidget::deleteMarkLinesFromMain );

    connect( logMainView_, &LogMainView::highlightersChange, this,
             &CrawlerWidget::applyConfiguration );

    connect( logMainView_, QOverload<const QString&>::of( &LogMainView::addToSearch ), this,
             &CrawlerWidget::addToSearch );

    connect( logMainView_, QOverload<const QString&>::of( &LogMainView::excludeFromSearch ), this,
             &CrawlerWidget::excludeFromSearch );

    connect( logMainView_, QOverload<const QString&>::of( &LogMainView::replaceSearch ), this,
             &CrawlerWidget::replaceSearch );

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

    connect( logMainView_, &LogMainView::saveDefaultSplitterSizes, this,
             &CrawlerWidget::saveSplitterSizes );

    connect( logMainView_, &LogMainView::changeFontSize, this, &CrawlerWidget::changeFontSize );

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

    // Search auto-refresh
    connect( searchRefreshButton_, &QPushButton::toggled, this,
             &CrawlerWidget::searchRefreshChangedHandler );

    connect( matchCaseButton_, &QPushButton::toggled, this,
             &CrawlerWidget::matchCaseChangedHandler );

    connect( useRegexpButton_, &QPushButton::toggled, this,
             &CrawlerWidget::useRegexpChangeHandler );

    connect( booleanButton_, &QPushButton::toggled, this,
             &CrawlerWidget::booleanCombiningChangedHandler );

    // Advise the parent the checkboxes have been changed
    // (for maintaining default config)
    connect( searchRefreshButton_, &QPushButton::toggled, this,
             &CrawlerWidget::searchRefreshChanged );
    connect( matchCaseButton_, &QPushButton::toggled, this, &CrawlerWidget::matchCaseChanged );

    // Switch between views
    connect( logMainView_, &AbstractLogView::clearColorLabels, this,
             &CrawlerWidget::clearColorLabels );

    connect( logMainView_, &AbstractLogView::addColorLabel, this,
             &CrawlerWidget::addColorLabelToSelection );
    connect( logMainView_, &AbstractLogView::removeColorLabel, this,
             &CrawlerWidget::removeColorLabelFromSelection );
    connect( logMainView_, &AbstractLogView::quickColorLabelDefaultsChanged, this,
             &CrawlerWidget::setQuickColorLabelDefaults );

    connect( logMainView_, &AbstractLogView::sendSelectionToScratchpad, this,
             [ this ]() { Q_EMIT sendToScratchpad( logMainView_->getSelectedText() ); } );

    connect( logMainView_, &AbstractLogView::replaceScratchpadWithSelection, this,
             [ this ]() { Q_EMIT replaceDataInScratchpad( logMainView_->getSelectedText() ); } );

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
    auto* tabFilteredView = tabbedFilteredView_->widget( tabIndex );
    connect( tabFilteredView, &QObject::destroyed, this, &CrawlerWidget::filteredViewDestroyed );
    tabFilteredView->deleteLater();
}

void CrawlerWidget::filteredViewDestroyed( QObject* view )
{
    filteredViewsData_.erase( qobject_cast<FilteredView*>( view ) );
}

void CrawlerWidget::saveSplitterSizes() const
{
    LOG_INFO << "Saving default splitter size";
    auto& splitterConfig = Configuration::get();
    splitterConfig.setSplitterSizes( sizes() );
    splitterConfig.save();
}

void CrawlerWidget::changeFontSize( bool increase )
{
    auto& fontConfig = Configuration::get();

    auto fontInfo = QFontInfo( fontConfig.mainFont() );
    const auto availableSizes = FontUtils::availableFontSizes( fontInfo.family() );

    auto currentSize
        = std::find( availableSizes.cbegin(), availableSizes.cend(), fontInfo.pointSize() );
    if ( increase && currentSize != std::prev( availableSizes.cend() ) ) {
        currentSize = std::next( currentSize );
    }
    else if ( !increase && currentSize != availableSizes.begin() ) {
        currentSize = std::prev( currentSize );
    }

    if ( currentSize != availableSizes.cend() ) {
        QFont newFont{ fontInfo.family(), *currentSize };

        fontConfig.setMainFont( newFont );
        logMainView_->updateFont( newFont );
        filteredView_->updateFont( newFont );
    }
}

void CrawlerWidget::connectAllFilteredViewSlots( FilteredView* view )
{
    connect( view, &FilteredView::newSelection, view, [ view ]( auto ) { view->update(); } );

    connect( view, &FilteredView::newSelection, this, &CrawlerWidget::jumpToMatchingLine );

    connect( view, &FilteredView::markLines, this, &CrawlerWidget::markLinesFromFiltered );
    connect( view, &FilteredView::deleteMarkLines, this,
             &CrawlerWidget::deleteMarkLinesFromFiltered );

    connect( view, &FilteredView::highlightersChange, this, &CrawlerWidget::applyConfiguration );

    connect( view, QOverload<const QString&>::of( &FilteredView::addToSearch ), this,
             &CrawlerWidget::addToSearch );

    connect( view, QOverload<const QString&>::of( &FilteredView::excludeFromSearch ), this,
             &CrawlerWidget::excludeFromSearch );

    connect( view, QOverload<const QString&>::of( &FilteredView::replaceSearch ), this,
             &CrawlerWidget::replaceSearch );

    connect( view, &FilteredView::mouseHoveredOverLine, this,
             &CrawlerWidget::mouseHoveredOverMatch );

    connect( view, &FilteredView::mouseLeftHoveringZone, overviewWidget_,
             &OverviewWidget::removeHighlight );

    connect( this, &CrawlerWidget::followSet, view, &FilteredView::followSet );

    connect( view, &FilteredView::followModeChanged, this, &CrawlerWidget::followModeChanged );

    connect( this, &CrawlerWidget::textWrapSet, view, &FilteredView::textWrapSet );

    connect( view, &FilteredView::activity, this, &CrawlerWidget::activityDetected );

    connect( view, &FilteredView::changeSearchLimits, this, &CrawlerWidget::setSearchLimits );

    connect( view, &FilteredView::saveDefaultSplitterSizes, this,
             &CrawlerWidget::saveSplitterSizes );

    connect( view, &FilteredView::changeFontSize, this, &CrawlerWidget::changeFontSize );

    connect( view, &FilteredView::clearSearchLimits, this, &CrawlerWidget::clearSearchLimits );

    connect( view, &AbstractLogView::addColorLabel, this,
             &CrawlerWidget::addColorLabelToSelection );
    connect( view, &AbstractLogView::removeColorLabel, this,
             &CrawlerWidget::removeColorLabelFromSelection );
    connect( view, &AbstractLogView::quickColorLabelDefaultsChanged, this,
             &CrawlerWidget::setQuickColorLabelDefaults );

    connect( view, &AbstractLogView::sendSelectionToScratchpad, this,
             [ view, this ]() { Q_EMIT sendToScratchpad( view->getSelectedText() ); } );

    connect( view, &AbstractLogView::replaceScratchpadWithSelection, this,
             [ view, this ]() { Q_EMIT replaceDataInScratchpad( view->getSelectedText() ); } );

    connect( view, &FilteredView::exitView, logMainView_,
             QOverload<>::of( &LogMainView::setFocus ) );

    connect( view, &AbstractLogView::clearColorLabels, this, &CrawlerWidget::clearColorLabels );

    connect( logMainView_, &LogMainView::exitView, view,
             QOverload<>::of( &FilteredView::setFocus ) );
}

void CrawlerWidget::registerShortcuts()
{
    LOG_INFO << "registering shortcuts for crawler widget";

    for ( auto& shortcut : shortcuts_ ) {
        shortcut.second->deleteLater();
    }

    shortcuts_.clear();

    const auto& config = Configuration::get();
    const auto& configuredShortcuts = config.shortcuts();

    ShortcutAction::registerShortcut(
        configuredShortcuts, shortcuts_, this, Qt::WidgetWithChildrenShortcut,
        ShortcutAction::CrawlerChangeVisibilityForward, [ this ]() {
            visibilityBox_->setCurrentIndex( ( visibilityBox_->currentIndex() + 1 )
                                             % visibilityBox_->count() );
        } );

    ShortcutAction::registerShortcut(
        configuredShortcuts, shortcuts_, this, Qt::WidgetWithChildrenShortcut,
        ShortcutAction::CrawlerEnableCaseMatching, [ this ]() { matchCaseButton_->toggle(); } );

    ShortcutAction::registerShortcut(
        configuredShortcuts, shortcuts_, this, Qt::WidgetWithChildrenShortcut,
        ShortcutAction::CrawlerEnableRegex, [ this ]() { useRegexpButton_->toggle(); } );

    ShortcutAction::registerShortcut(
        configuredShortcuts, shortcuts_, this, Qt::WidgetWithChildrenShortcut,
        ShortcutAction::CrawlerEnableInverseMatching, [ this ]() { inverseButton_->toggle(); } );

    ShortcutAction::registerShortcut(
        configuredShortcuts, shortcuts_, this, Qt::WidgetWithChildrenShortcut,
        ShortcutAction::CrawlerEnableRegexCombining, [ this ]() { booleanButton_->toggle(); } );

    ShortcutAction::registerShortcut(
        configuredShortcuts, shortcuts_, this, Qt::WidgetWithChildrenShortcut,
        ShortcutAction::CrawlerEnableAutoRefresh, [ this ]() { searchRefreshButton_->toggle(); } );

    ShortcutAction::registerShortcut(
        configuredShortcuts, shortcuts_, this, Qt::WidgetWithChildrenShortcut,
        ShortcutAction::CrawlerKeepResults, [ this ]() { keepSearchResultsButton_->toggle(); } );

    ShortcutAction::registerShortcut( configuredShortcuts, shortcuts_, this,
                                      Qt::WidgetWithChildrenShortcut,
                                      ShortcutAction::CrawlerChangeVisibilityBackward, [ this ]() {
                                          int nextIndex = visibilityBox_->currentIndex() - 1;
                                          if ( nextIndex < 0 ) {
                                              nextIndex = visibilityBox_->count() - 1;
                                          }
                                          visibilityBox_->setCurrentIndex( nextIndex );
                                      } );

    ShortcutAction::registerShortcut(
        configuredShortcuts, shortcuts_, this, Qt::WidgetWithChildrenShortcut,
        ShortcutAction::CrawlerChangeVisibilityToMarksAndMatches, [ this ]() {
            if ( visibilityBox_->count() > 0 ) {
                visibilityBox_->setCurrentIndex( 0 );
            }
        } );

    ShortcutAction::registerShortcut( configuredShortcuts, shortcuts_, this,
                                      Qt::WidgetWithChildrenShortcut,
                                      ShortcutAction::CrawlerChangeVisibilityToMarks, [ this ]() {
                                          if ( visibilityBox_->count() > 1 ) {
                                              visibilityBox_->setCurrentIndex( 1 );
                                          }
                                      } );

    ShortcutAction::registerShortcut( configuredShortcuts, shortcuts_, this,
                                      Qt::WidgetWithChildrenShortcut,
                                      ShortcutAction::CrawlerChangeVisibilityToMatches, [ this ]() {
                                          if ( visibilityBox_->count() > 2 ) {
                                              visibilityBox_->setCurrentIndex( 2 );
                                          }
                                      } );

    ShortcutAction::registerShortcut(
        configuredShortcuts, shortcuts_, this, Qt::WidgetWithChildrenShortcut,
        ShortcutAction::CrawlerIncreseTopViewSize, [ this ]() { changeTopViewSize( 1 ); } );

    ShortcutAction::registerShortcut(
        configuredShortcuts, shortcuts_, this, Qt::WidgetWithChildrenShortcut,
        ShortcutAction::CrawlerDecreaseTopViewSize, [ this ]() { changeTopViewSize( -1 ); } );

    const auto exitSearchKeySequence = QKeySequence( QKeySequence::Cancel );
    ShortcutAction::registerShortcut( exitSearchKeySequence.toString(), shortcuts_, this,
                                      Qt::WidgetWithChildrenShortcut, [ this ]() {
                                          const auto activeView = this->activeView();
                                          if ( activeView ) {
                                              activeView->setFocus();
                                          }
                                      } );

    std::array<std::string, 9> colorLables = {
        ShortcutAction::LogViewAddColorLabel1, ShortcutAction::LogViewAddColorLabel2,
        ShortcutAction::LogViewAddColorLabel3, ShortcutAction::LogViewAddColorLabel4,
        ShortcutAction::LogViewAddColorLabel5, ShortcutAction::LogViewAddColorLabel6,
        ShortcutAction::LogViewAddColorLabel7, ShortcutAction::LogViewAddColorLabel8,
        ShortcutAction::LogViewAddColorLabel9,
    };

    for ( auto label = 0u; label < colorLables.size(); ++label ) {
        ShortcutAction::registerShortcut(
            configuredShortcuts, shortcuts_, this, Qt::WidgetWithChildrenShortcut,
            colorLables[ label ], [ this, label ]() { addColorLabelToSelection( label ); } );
    }

    ShortcutAction::registerShortcut(
        configuredShortcuts, shortcuts_, this, Qt::WidgetWithChildrenShortcut,
        ShortcutAction::LogViewAddNextColorLabel, [ this ]() { addNextColorLabelToSelection(); } );

    ShortcutAction::registerShortcut(
        configuredShortcuts, shortcuts_, this, Qt::WidgetWithChildrenShortcut,
        ShortcutAction::LogViewRemoveColorLabel, [ this ]() { removeColorLabelFromSelection(); } );

    ShortcutAction::registerShortcut(
        configuredShortcuts, shortcuts_, this, Qt::WidgetWithChildrenShortcut,
        ShortcutAction::LogViewClearColorLabels, [ this ]() { clearColorLabels(); } );

    logMainView_->registerShortcuts();
    filteredView_->registerShortcuts();
}

void CrawlerWidget::loadIcons()
{
    searchRefreshButton_->setIcon( iconLoader_.load( "icons8-search-refresh" ) );
    useRegexpButton_->setIcon( iconLoader_.load( "regex" ) );
    inverseButton_->setIcon( iconLoader_.load( "icons8-not-equal" ) );
    booleanButton_->setIcon( iconLoader_.load( "icons8-venn-diagram" ) );
    clearButton_->setIcon( iconLoader_.load( "icons8-delete" ) );
    searchButton_->setIcon( iconLoader_.load( "icons8-search" ) );
    keepSearchResultsButton_->setIcon( iconLoader_.load( "icons8-lock" ) );
    matchCaseButton_->setIcon( iconLoader_.load( "icons8-font-size" ) );
    stopButton_->setIcon( iconLoader_.load( "icons8-close-window" ) );
    favoriteFilterButton_->setIcon( iconLoader_.load( "icons8-star" ) );

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
    if ( searchText.isEmpty() && Configuration::get().showAllInFilteredViewWhenSearchEmpty() ) {
        logFilteredData_->setAllLinesVisible( true );
    }
    filteredView_->updateData();

    // Update the match overview
    overview_.updateData( logData_->getNbLine() );

    if ( !searchText.isEmpty() ) {

        // Constructs the regexp
        auto regexpPattern = RegularExpressionPattern(
            searchText, matchCaseButton_->isChecked(), inverseButton_->isChecked(),
            booleanButton_->isChecked(), !useRegexpButton_->isChecked() );

        RegularExpression hsExpression{ regexpPattern };
        auto isValidExpression = hsExpression.isValid();

        if ( isValidExpression ) {
            // Activate the stop button
            stopButton_->setEnabled( true );
            stopButton_->show();
            clearButton_->hide();
            searchButton_->hide();
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
    const QString text = searchLineEdit_->lineEdit()->text();
    searchLineEdit_->clear();

    auto searchHistory = savedSearches_->recentSearches();

    searchLineEdit_->addItems( searchHistory );
    // In case we had something that wasn't added to the list (blank...):
    searchLineEdit_->lineEdit()->setText( text );

    searchLineCompleter_->setModel( new QStringListModel( searchHistory, searchLineCompleter_ ) );
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

void CrawlerWidget::addColorLabelToSelection( size_t label )
{
    updateColorLabels( colorLabelsManager_.setColorLabel(
        label, getSelectedText(), HighlighterSetCollection::get().quickHighlighterDefaults() ) );
}

void CrawlerWidget::addNextColorLabelToSelection()
{
    updateColorLabels( colorLabelsManager_.setNextColorLabel(
        getSelectedText(), HighlighterSetCollection::get().quickHighlighterDefaults() ) );
}

void CrawlerWidget::removeColorLabelFromSelection()
{
    updateColorLabels( colorLabelsManager_.removeColorLabel( getSelectedText() ) );
}

void CrawlerWidget::clearColorLabels()
{
    updateColorLabels( colorLabelsManager_.clear() );
}

void CrawlerWidget::setQuickColorLabelDefaults( bool ignoreCase, bool wholeWord )
{
    auto& highlighterSetCollection = HighlighterSetCollection::get();
    highlighterSetCollection.setQuickHighlighterDefaults(
        QuickHighlighterDefaults{ ignoreCase, wholeWord } );
    highlighterSetCollection.save();
}

void CrawlerWidget::updateColorLabels(
    const ColorLabelsManager::QuickHighlightersCollection& labels )
{
    logMainView_->setQuickHighlighters( labels );
    filteredView_->setQuickHighlighters( labels );
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
