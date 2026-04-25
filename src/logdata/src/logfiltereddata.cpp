/*
 * Copyright (C) 2009, 2010, 2011, 2012, 2013, 2017 Nicolas Bonnefon and other contributors
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

// This file implements LogFilteredData
// It stores a pointer to the LogData that created it,
// so should always be destroyed before the LogData.

#include "log.h"

#include <KDSignalThrottler.h>
#include <QCoreApplication>
#include <QString>
#include <QTimer>

#include <algorithm>
#include <cassert>
#include <functional>
#include <numeric>
#include <set>
#include <tuple>
#include <vector>

#include "logfiltereddata.h"
#include "searchablelogdata.h"

#include "configuration.h"
#include "readablesize.h"
#include "synchronization.h"

// Usual constructor: just copy the data, the search is started by runSearch()
LogFilteredData::LogFilteredData( const SearchableLogData* logData )
    : AbstractLogData()
    , matching_lines_( SearchResultArray() )
    , currentRegExp_()
    , visibility_()
    , workerThread_( *logData )
{
    // Starts with an empty result list
    maxLength_ = 0_length;
    maxLengthMarks_ = 0_length;
    nbLinesProcessed_ = 0_lcount;

    sourceLogData_ = logData;

    visibility_ = VisibilityFlags::Marks | VisibilityFlags::Matches;

    // Forward the update signal
    connect( &workerThread_, &LogFilteredDataWorker::searchProgressed, this,
             &LogFilteredData::handleSearchProgressed, Qt::QueuedConnection );

#if !defined( Q_OS_WIN )
    searchProgressThrottler_.setTimeout( 100 );
    connect( this, &LogFilteredData::searchProgressedThrottled, &searchProgressThrottler_,
             &KDToolBox::KDGenericSignalThrottler::throttle );

    connect( &searchProgressThrottler_, &KDToolBox::KDGenericSignalThrottler::triggered, this,
             &LogFilteredData::handleSearchProgressedThrottled );
#endif
}

LogFilteredData::~LogFilteredData()
{
    shuttingDown_ = true;
    searchProgressThrottler_.blockSignals( true );

    // KDSignalThrottler owns an internal QTimer and its destructor calls
    // maybeEmitTriggered().  On x86/Qt5 we can still hit a timeout/metacall race
    // during teardown unless the internal timer is stopped/disconnected first.
    if ( auto* throttlerTimer = searchProgressThrottler_.findChild<QTimer*>() ) {
        throttlerTimer->stop();
        throttlerTimer->blockSignals( true );
        disconnect( throttlerTimer, nullptr, &searchProgressThrottler_, nullptr );
    }

    interruptSearch();
    // Wait for any in-flight search operations to fully stop before
    // detaching the file reader.  Without this, the worker thread can
    // still be mid-read when the file handle is closed, causing
    // STATUS_HEAP_CORRUPTION (0xC0000374) on Windows.
    workerThread_.waitForDone();

    workerThread_.blockSignals( true );
    disconnect( &workerThread_, nullptr, this, nullptr );
    disconnect( this, nullptr, &workerThread_, nullptr );
    disconnect( &searchProgressThrottler_, nullptr, this, nullptr );
    disconnect( this, nullptr, &searchProgressThrottler_, nullptr );

    detachReaderIfNeeded();

    // Queued MetaCall events can still be pending for this object or the helper
    // QObjects even after disconnect(); remove them before subobject destruction.
    QCoreApplication::removePostedEvents( &workerThread_ );
    QCoreApplication::removePostedEvents( &searchProgressThrottler_ );
    QCoreApplication::removePostedEvents( this );

    sourceLogData_ = nullptr;
}

void LogFilteredData::runSearch( const RegularExpressionPattern& regExp )
{
    runSearch( regExp, 0_lnum, LineNumber( getNbTotalLines().get() ) );
}

// Run the search and send newDataAvailable() signals.
void LogFilteredData::runSearch( const RegularExpressionPattern& regExp, LineNumber startLine,
                                 LineNumber endLine )
{
    LOG_DEBUG << "Entering runSearch";

    const auto& config = Configuration::get();

    clearSearch();
    currentRegExp_ = regExp;
    currentSearchKey_ = makeCacheKey( regExp, startLine, endLine );
    LOG_INFO << "Search cache key: " << regExp.pattern << "_" << startLine.get() << "_"
             << endLine.get();

    bool shouldRunSearch = true;
    if ( config.useSearchResultsCache() ) {
        const auto cachedResults = searchResultsCache_.find( currentSearchKey_ );
        if ( cachedResults != std::end( searchResultsCache_ ) ) {
            LOG_INFO << "Got result from cache";
            shouldRunSearch = false;
            matching_lines_ = cachedResults->second.matching_lines;
            maxLength_ = cachedResults->second.maxLength;

            marks_and_matches_ = matching_lines_ | marks_;

            // Advance the generation counter even though no worker thread
            // runs.  Without this, queued progress signals from the previous
            // real search still carry the current generation and pass the
            // staleness gate in the receiver, corrupting the just-displayed
            // cached result.
            const auto cachedGeneration = workerThread_.bumpGeneration();
            Q_EMIT searchProgressed( LinesCount( matching_lines_.cardinality() ), 100, startLine,
                                     cachedGeneration );
        }
    }

    if ( shouldRunSearch ) {
        attachReaderIfNeeded();
        workerThread_.search( currentRegExp_, startLine, endLine );
    }
}

void LogFilteredData::updateSearch( LineNumber startLine, LineNumber endLine )
{
    LOG_DEBUG << "Entering updateSearch";

    currentSearchKey_ = {};

    attachReaderIfNeeded();
    workerThread_.updateSearch( currentRegExp_, startLine, endLine,
                                LineNumber( nbLinesProcessed_.get() ) );
}

void LogFilteredData::interruptSearch()
{
    LOG_DEBUG << "Entering interruptSearch";

    workerThread_.interrupt();
}

void LogFilteredData::clearSearch( bool dropCache )
{
    interruptSearch();

    currentRegExp_ = {};
    matching_lines_ = {};
    marks_and_matches_ = marks_;
    maxLength_ = 0_length;
    nbLinesProcessed_ = 0_lcount;
    contextLinesListValid_ = false; // Invalidate context lines cache

    if ( dropCache ) {
        searchResultsCache_.clear();
    }
}

LineNumber LogFilteredData::getMatchingLineNumber( LineNumber matchNum ) const
{
    // If context lines are enabled, get the line number from context lines list
    if ( contextLinesBefore_ > 0 || contextLinesAfter_ > 0 ) {
        if ( !contextLinesListValid_ ) {
            rebuildContextLinesList();
        }
        if ( matchNum.get() < static_cast<LineNumber::UnderlyingType>( contextLinesList_.size() ) ) {
            return contextLinesList_[ matchNum.get() ];
        }
        return maxValue<LineNumber>();
    }
    
    // No context lines: use original logic
    return findLogDataLine( matchNum );
}

LineNumber LogFilteredData::getLineIndexNumber( LineNumber lineNumber ) const
{
    return findFilteredLine( lineNumber );
}

// Scan the list for the 'lineNumber' passed
bool LogFilteredData::isLineMatched( LineNumber lineNumber ) const
{
    return matching_lines_.contains( lineNumber.get() );
}

LinesCount LogFilteredData::getNbTotalLines() const
{
    // Safety check: if sourceLogData_ is null (object being destroyed), return 0
    if ( !sourceLogData_ ) {
        return 0_lcount;
    }
    return sourceLogData_->getNbLine();
}

LinesCount LogFilteredData::getNbMatches() const
{
    return LinesCount( matching_lines_.cardinality() );
}

LinesCount LogFilteredData::getNbMarks() const
{
    return LinesCount( marks_.cardinality() );
}

LogFilteredData::LineType LogFilteredData::lineTypeByIndex( LineNumber index ) const
{
    // If context lines are enabled, get the line number from context lines list
    if ( contextLinesBefore_ > 0 || contextLinesAfter_ > 0 ) {
        if ( !contextLinesListValid_ ) {
            rebuildContextLinesList();
        }
        if ( index.get() < static_cast<LineNumber::UnderlyingType>( contextLinesList_.size() ) ) {
            return lineTypeByLine( contextLinesList_[ index.get() ] );
        }
        return LineTypeFlags::Plain;
    }
    
    // No context lines: use original logic
    return lineTypeByLine( findLogDataLine( index ) );
}

LogFilteredData::LineType LogFilteredData::lineTypeByLine( LineNumber lineNumber ) const
{
    LineType line_type = LineTypeFlags::Plain;

    if ( isLineMarked( lineNumber ) )
        line_type |= LineTypeFlags::Mark;

    if ( isLineMatched( lineNumber ) )
        line_type |= LineTypeFlags::Match;

    return line_type;
}

void LogFilteredData::iterateOverLines( const std::function<void( LineNumber )>& callback ) const
{
    using CallbackFn = std::function<void( LineNumber )>;
    const auto& currentResults = currentResultArray();
    currentResults.iterate(
        []( uint64_t line, void* context ) -> bool {
            auto* callbackFn = static_cast<CallbackFn*>( context );
            callbackFn->operator()( LineNumber( line ) );
            return true;
        },
        static_cast<void*>( const_cast<CallbackFn*>( &callback ) ) );
}

// Delegation to our Marks object

void LogFilteredData::toggleMark( LineNumber line )
{
    // Safety check: if sourceLogData_ is null (object being destroyed), return early
    if ( !sourceLogData_ ) {
        return;
    }
    if ( ( line >= 0_lnum ) && line < sourceLogData_->getNbLine() ) {
        if ( !marks_.addChecked( line.get() ) ) {
            marks_.remove( line.get() );
            updateMaxLengthMarks( {}, line );
        }
        else {
            updateMaxLengthMarks( line, {} );
        }
        // marks_and_matches_ is already updated by updateMaxLengthMarks()
        contextLinesListValid_ = false; // Invalidate context lines cache when marks change
    }
    else {
        LOG_ERROR << "LogFilteredData::toggleMark trying to toggle a mark outside of the file.";
    }
}

void LogFilteredData::addMark( LineNumber line )
{
    // Safety check: if sourceLogData_ is null (object being destroyed), return early
    if ( !sourceLogData_ ) {
        return;
    }
    if ( ( line >= 0_lnum ) && line < sourceLogData_->getNbLine() ) {
        marks_.add( line.get() );
        updateMaxLengthMarks( line, {} );
        // marks_and_matches_ is already updated by updateMaxLengthMarks()
        contextLinesListValid_ = false; // Invalidate context lines cache when marks change
    }
    else {
        LOG_ERROR << "LogFilteredData::addMark trying to create a mark outside of the file.";
    }
}

bool LogFilteredData::isLineMarked( LineNumber line ) const
{
    return marks_.contains( line.get() );
}

OptionalLineNumber LogFilteredData::getMarkAfter( LineNumber line ) const
{
    OptionalLineNumber marked_line;
    const LineNumber::UnderlyingType rank = marks_.rank( line.get() );
    LineNumber::UnderlyingType nextMark;
    if ( marks_.select( rank, &nextMark ) ) {
        marked_line = LineNumber( nextMark );
    }

    return marked_line;
}

OptionalLineNumber LogFilteredData::getMarkBefore( LineNumber line ) const
{
    OptionalLineNumber marked_line;

    const LineNumber::UnderlyingType rank = marks_.rank( line.get() );

    if ( rank < 2 ) {
        return marked_line;
    }

    LineNumber::UnderlyingType nextMark;
    if ( marks_.select( rank - 2, &nextMark ) ) {
        marked_line = LineNumber( nextMark );
    }

    return marked_line;
}

void LogFilteredData::deleteMark( LineNumber line )
{
    marks_.remove( line.get() );
    updateMaxLengthMarks( {}, line );
    // marks_and_matches_ is already updated by updateMaxLengthMarks()
    contextLinesListValid_ = false; // Invalidate context lines cache when marks change
}

void LogFilteredData::updateMaxLengthMarks( OptionalLineNumber added_line,
                                            OptionalLineNumber removed_line )
{
    marks_and_matches_ = matching_lines_ | marks_;

    // Safety check: if sourceLogData_ is null (object being destroyed), return early
    if ( !sourceLogData_ ) {
        return;
    }

    if ( added_line.has_value() ) {
        maxLengthMarks_ = qMax( maxLengthMarks_, sourceLogData_->getLineLength( *added_line ) );
    }

    // Now update the max length if needed
    if ( removed_line.has_value()
         && sourceLogData_->getLineLength( *removed_line ) >= maxLengthMarks_ ) {
        LOG_DEBUG << "deleteMark recalculating longest mark";
        maxLengthMarks_ = 0_length;
        marks_.iterate(
            []( uint64_t line, void* context ) -> bool {
                auto* self = static_cast<LogFilteredData*>( context );
                // Safety check: if sourceLogData_ is null, skip this iteration
                if ( !self->sourceLogData_ ) {
                    return false;
                }
                self->maxLengthMarks_
                    = qMax( self->maxLengthMarks_,
                            self->sourceLogData_->getLineLength( LineNumber( line ) ) );
                return true;
            },
            static_cast<void*>( this ) );
    }
}

void LogFilteredData::clearMarks()
{
    marks_ = {};
    marks_and_matches_ = matching_lines_;
    maxLengthMarks_ = 0_length;
    contextLinesListValid_ = false; // Invalidate context lines cache when marks change
}

QList<LineNumber> LogFilteredData::getMarks() const
{
    QList<LineNumber> markedLines;
    marks_.iterate(
        []( uint64_t line, void* context ) -> bool {
            static_cast<QList<LineNumber>*>( context )->append( LineNumber( line ) );
            return true;
        },
        static_cast<void*>( &markedLines ) );

    return markedLines;
}

void LogFilteredData::setVisibility( Visibility visi )
{
    visibility_ = visi;
    contextLinesListValid_ = false; // Invalidate context lines cache when visibility changes
}

LogFilteredData::Visibility LogFilteredData::visibility() const
{
    return visibility_;
}

void LogFilteredData::setContextLines( int before, int after )
{
    contextLinesBefore_ = std::max( 0, before );
    contextLinesAfter_ = std::max( 0, after );
    contextLinesListValid_ = false; // Invalidate cache
}

void LogFilteredData::rebuildContextLinesList() const
{
    // Context lines should always be calculated relative to search matches (matching_lines_),
    // regardless of visibility settings. This is because the feature is designed to provide
    // context around search results similar to grep's -B, -A, -C options.
    // Using currentResultArray() would respect visibility settings, which could cause context
    // lines to be calculated around marks instead of matches when visibility is set to show
    // only marks.

    // If no context lines needed, use simple list of matching lines plus marks
    if ( contextLinesBefore_ == 0 && contextLinesAfter_ == 0 ) {
        contextLinesList_.clear();
        // Build union of matching_lines_ and marks_ (respecting visibility)
        SearchResultArray combinedLines;
        if ( visibility_.testFlag( VisibilityFlags::Matches ) ) {
            combinedLines |= matching_lines_;
        }
        if ( visibility_.testFlag( VisibilityFlags::Marks ) ) {
            combinedLines |= marks_;
        }
        contextLinesList_.reserve( combinedLines.cardinality() );
        combinedLines.iterate(
            []( uint64_t line, void* context ) -> bool {
                static_cast<klogg::vector<LineNumber>*>( context )->emplace_back( LineNumber( line ) );
                return true;
            },
            static_cast<void*>( &contextLinesList_ ) );
        contextLinesListValid_ = true;
        return;
    }

    // Build set of all lines to include (matching lines + context lines)
    std::set<LineNumber::UnderlyingType> linesSet;
    
    // Safety check for sourceLogData_ before accessing it
    if ( !sourceLogData_ ) {
        return;
    }
    const LinesCount totalLines = sourceLogData_->getNbLine();

    // Structure to pass data to lambda via context pointer
    struct ContextData {
        std::set<LineNumber::UnderlyingType>* linesSet;
        LineNumber::UnderlyingType totalLines;
        uint64_t contextLinesBefore;
        uint64_t contextLinesAfter;
    };
    ContextData contextData = { &linesSet, totalLines.get(), 
                                static_cast<uint64_t>( contextLinesBefore_ ),
                                static_cast<uint64_t>( contextLinesAfter_ ) };

    // Lambda to add a line and its context to the set
    auto addLineWithContext = []( uint64_t matchLine, void* context ) -> bool {
        auto* data = static_cast<ContextData*>( context );

        // Add the line itself
        data->linesSet->insert( matchLine );

        // Add context lines before
        for ( uint64_t i = 1; i <= data->contextLinesBefore; ++i ) {
            const LineNumber::UnderlyingType contextLine = matchLine >= i ? matchLine - i : 0;
            if ( contextLine < data->totalLines ) {
                data->linesSet->insert( contextLine );
            }
        }

        // Add context lines after
        for ( uint64_t i = 1; i <= data->contextLinesAfter; ++i ) {
            const LineNumber::UnderlyingType contextLine = matchLine + i;
            if ( contextLine < data->totalLines ) {
                data->linesSet->insert( contextLine );
            }
        }

        return true;
    };

    // Iterate through matching lines and add context (respecting visibility)
    if ( visibility_.testFlag( VisibilityFlags::Matches ) ) {
        matching_lines_.iterate( addLineWithContext, static_cast<void*>( &contextData ) );
    }

    // Also iterate through marks and add context (respecting visibility)
    if ( visibility_.testFlag( VisibilityFlags::Marks ) ) {
        marks_.iterate( addLineWithContext, static_cast<void*>( &contextData ) );
    }

    // Convert set to sorted vector (set is already sorted) and track max length
    contextLinesList_.clear();
    contextLinesList_.reserve( linesSet.size() );
    maxLengthContext_ = 0_length;
    for ( const auto& line : linesSet ) {
        contextLinesList_.emplace_back( LineNumber( line ) );
        maxLengthContext_
            = qMax( maxLengthContext_, sourceLogData_->getLineLength( LineNumber( line ) ) );
    }

    contextLinesListValid_ = true;
}

void LogFilteredData::updateSearchResultsCache()
{
    const auto& config = Configuration::get();
    if ( !config.useSearchResultsCache() ) {
        return;
    }

    if ( currentSearchKey_ == SearchCacheKey{} ) {
        return;
    }

    const uint64_t maxCacheLines = config.searchResultsCacheLines();

    if ( matching_lines_.cardinality() > maxCacheLines ) {
        LOG_DEBUG << "LogFilteredData: too many matches to place in cache";
    }
    else {
        LOG_INFO << "LogFilteredData: caching results for key "
                 << std::get<0>( currentSearchKey_ ).pattern << "_"
                 << std::get<1>( currentSearchKey_ ) << "_" << std::get<2>( currentSearchKey_ );

        searchResultsCache_[ currentSearchKey_ ] = { matching_lines_, maxLength_ };
        auto cacheSize = std::accumulate( searchResultsCache_.cbegin(), searchResultsCache_.cend(),
                                          uint64_t{ 0 }, []( const auto& acc, const auto& next ) {
                                              return acc + next.second.matching_lines.cardinality();
                                          } );

        LOG_INFO << "LogFilteredData: cache size " << cacheSize;

        auto cachedResult = std::begin( searchResultsCache_ );
        while ( cachedResult != std::end( searchResultsCache_ ) && cacheSize > maxCacheLines ) {

            if ( cachedResult->first == currentSearchKey_ ) {
                ++cachedResult;
                continue;
            }

            cacheSize -= cachedResult->second.matching_lines.cardinality();
            cachedResult = searchResultsCache_.erase( cachedResult );
        }
    }
}

//
// Q_SLOTS:
//
void LogFilteredData::handleSearchProgressed( LinesCount nbMatches, int progress,
                                              LineNumber initialLine,
                                              quint64 generation )
{
    if ( shuttingDown_ ) {
        return;
    }

    assert( nbMatches >= 0_lcount );

    const auto searchResults = workerThread_.getSearchResults();

    matching_lines_ |= searchResults.newMatches;
    marks_and_matches_ |= searchResults.newMatches;

    maxLength_ = searchResults.maxLength;
    nbLinesProcessed_ = searchResults.processedLines;
    contextLinesListValid_ = false; // Invalidate context lines cache when search progresses

    if ( progress == 100
         && nbLinesProcessed_.get() == getExpectedSearchEnd( currentSearchKey_ ).get() ) {
        updateSearchResultsCache();
    }

    {
        ScopedLock lock( searchProgressMutex_ );
        searchProgress_ = std::make_tuple( nbMatches, progress, initialLine, generation );
    }

    if ( progress == 100 ) {
        // Do not rely solely on the throttler timer for the terminal update: tests and
        // shutdown paths need a deterministic completion signal even if the throttler
        // event is delayed or dropped during teardown.
        Q_EMIT searchProgressed( nbMatches, progress, initialLine, generation );
        detachReaderIfNeeded();

        LOG_INFO << "Matches size " << readableSize( matching_lines_.getSizeInBytes( false ) )
                 << ", marks size " << readableSize( marks_.getSizeInBytes( false ) )
                 << ", union size " << readableSize( marks_and_matches_.getSizeInBytes( false ) );
    }
    else {
#if defined( Q_OS_WIN )
        // Windows test runs have hit repeated QObject/KDSignalThrottler teardown
        // crashes after many create/search/destroy cycles. Emit progress directly
        // instead of using the throttler on Windows.
        Q_EMIT searchProgressed( nbMatches, progress, initialLine, generation );
#else
        Q_EMIT searchProgressedThrottled();
#endif
    }
}

void LogFilteredData::handleSearchProgressedThrottled()
{
    if ( shuttingDown_ ) {
        return;
    }

    LinesCount nbMatches;
    int progress;
    LineNumber initialLine;
    quint64 generation;
    {
        ScopedLock lock( searchProgressMutex_ );
        std::tie( nbMatches, progress, initialLine, generation ) = searchProgress_;
    }
    Q_EMIT searchProgressed( nbMatches, progress, initialLine, generation );
}

LineNumber LogFilteredData::findLogDataLine( LineNumber index ) const
{
    const auto& currentResults = currentResultArray();

    LineNumber::UnderlyingType line = {};
    if ( currentResults.select( index.get(), &line ) ) {
        return LineNumber( line );
    }
    else {
        if ( !currentResults.isEmpty() ) {
            LOG_ERROR << "Index too big in LogFilteredData: " << index << " cache size "
                      << currentResults.cardinality();
        }
        return maxValue<LineNumber>();
    }
}

const SearchResultArray& LogFilteredData::currentResultArray() const
{
    if ( visibility_.testFlag( VisibilityFlags::Marks )
         && visibility_.testFlag( VisibilityFlags::Matches ) ) {
        return marks_and_matches_;
    }
    else if ( visibility_.testFlag( VisibilityFlags::Matches ) ) {
        return matching_lines_;
    }
    else {
        return marks_;
    }
}

LineNumber LogFilteredData::findFilteredLine( LineNumber lineNum ) const
{
    // If context lines are enabled, search in context lines list
    if ( contextLinesBefore_ > 0 || contextLinesAfter_ > 0 ) {
        if ( !contextLinesListValid_ ) {
            rebuildContextLinesList();
        }
        // Binary search for the line number in the sorted context lines list
        auto it = std::lower_bound( contextLinesList_.begin(), contextLinesList_.end(), lineNum,
                                   []( const LineNumber& a, const LineNumber& b ) { return a < b; } );
        if ( it != contextLinesList_.end() && *it == lineNum ) {
            const auto distance = std::distance( contextLinesList_.begin(), it );
            return LineNumber( static_cast<LineNumber::UnderlyingType>( distance ) );
        }
        return maxValue<LineNumber>();
    }
    
    // No context lines: use original logic
    LineNumber::UnderlyingType index = currentResultArray().rank( lineNum.get() );

    if ( index > 0 ) {
        index--;
    }
    return LineNumber( index );
}

// Implementation of the virtual function.
QString LogFilteredData::doGetLineString( LineNumber index ) const
{
    // Safety check: if sourceLogData_ is null (object being destroyed), return empty string
    if ( !sourceLogData_ ) {
        return QString();
    }

    // If context lines are enabled, use the context lines list
    if ( contextLinesBefore_ > 0 || contextLinesAfter_ > 0 ) {
        if ( !contextLinesListValid_ ) {
            rebuildContextLinesList();
        }
        if ( index.get() < static_cast<LineNumber::UnderlyingType>( contextLinesList_.size() ) ) {
            return sourceLogData_->getLineString( contextLinesList_[ index.get() ] );
        }
        return QString();
    }
    
    // No context lines: use original logic
    const auto line = findLogDataLine( index );
    return sourceLogData_->getLineString( line );
}

// Implementation of the virtual function.
QString LogFilteredData::doGetExpandedLineString( LineNumber index ) const
{
    // Safety check: if sourceLogData_ is null (object being destroyed), return empty string
    if ( !sourceLogData_ ) {
        return QString();
    }

    // If context lines are enabled, use the context lines list
    if ( contextLinesBefore_ > 0 || contextLinesAfter_ > 0 ) {
        if ( !contextLinesListValid_ ) {
            rebuildContextLinesList();
        }
        if ( index.get() < static_cast<LineNumber::UnderlyingType>( contextLinesList_.size() ) ) {
            return sourceLogData_->getExpandedLineString( contextLinesList_[ index.get() ] );
        }
        return QString();
    }
    
    // No context lines: use original logic
    const auto line = findLogDataLine( index );
    return sourceLogData_->getExpandedLineString( line );
}

// Implementation of the virtual function.
klogg::vector<QString> LogFilteredData::doGetLines( LineNumber first_line, LinesCount number ) const
{
    return doGetLines( first_line, number,
                       [ this ]( const auto& line ) { return doGetLineString( line ); } );
}

// Implementation of the virtual function.
klogg::vector<QString> LogFilteredData::doGetExpandedLines( LineNumber first_line,
                                                          LinesCount number ) const
{
    return doGetLines( first_line, number,
                       [ this ]( const auto& line ) { return doGetExpandedLineString( line ); } );
}

klogg::vector<QString>
LogFilteredData::doGetLines( LineNumber first_line, LinesCount number,
                             const std::function<QString( LineNumber )>& lineGetter ) const
{
    klogg::vector<LineNumber::UnderlyingType> lineNumbers( number.get() );
    std::iota( lineNumbers.begin(), lineNumbers.end(), first_line.get() );

    klogg::vector<QString> lines( number.get() );
    std::transform(
        lineNumbers.cbegin(), lineNumbers.cend(), lines.begin(),
        [ &lineGetter ]( const auto& line ) { return lineGetter( LineNumber( line ) ); } );

    return lines;
}

LineNumber LogFilteredData::doGetLineNumber(LineNumber index) const
{
    // If context lines are enabled, return the line number from context lines list
    if ( contextLinesBefore_ > 0 || contextLinesAfter_ > 0 ) {
        if ( !contextLinesListValid_ ) {
            rebuildContextLinesList();
        }
        if ( index.get() < static_cast<LineNumber::UnderlyingType>( contextLinesList_.size() ) ) {
            return contextLinesList_[ index.get() ];
        }
        return maxValue<LineNumber>();
    }
    
    // No context lines: use original logic
    return getMatchingLineNumber( index );
}

// Implementation of the virtual function.
LinesCount LogFilteredData::doGetNbLine() const
{
    // If context lines are enabled, return the size of context lines list
    if ( contextLinesBefore_ > 0 || contextLinesAfter_ > 0 ) {
        if ( !contextLinesListValid_ ) {
            rebuildContextLinesList();
        }
        return LinesCount( contextLinesList_.size() );
    }
    
    // No context lines: use original logic
    const LinesCount::UnderlyingType nbLines = currentResultArray().cardinality();
    return LinesCount( nbLines );
}

// Implementation of the virtual function.
LineLength LogFilteredData::doGetMaxLength() const
{
    LineLength result = qMax( maxLength_, maxLengthMarks_ );
    if ( contextLinesBefore_ > 0 || contextLinesAfter_ > 0 ) {
        if ( !contextLinesListValid_ ) {
            rebuildContextLinesList();
        }
        result = qMax( result, maxLengthContext_ );
    }
    return result;
}

// Implementation of the virtual function.
LineLength LogFilteredData::doGetLineLength( LineNumber lineNum ) const
{
    // Safety check: if sourceLogData_ is null (object being destroyed), return 0
    if ( !sourceLogData_ ) {
        return 0_length;
    }

    // If context lines are enabled, use the context lines list
    if ( contextLinesBefore_ > 0 || contextLinesAfter_ > 0 ) {
        if ( !contextLinesListValid_ ) {
            rebuildContextLinesList();
        }
        if ( lineNum.get() < static_cast<LineNumber::UnderlyingType>( contextLinesList_.size() ) ) {
            return sourceLogData_->getLineLength( contextLinesList_[ lineNum.get() ] );
        }
        return 0_length;
    }
    
    // No context lines: use original logic
    LineNumber line = findLogDataLine( lineNum );
    return sourceLogData_->getLineLength( line );
}

void LogFilteredData::doSetDisplayEncoding( const char* encoding )
{
    LOG_DEBUG << "AbstractLogData::setDisplayEncoding: " << encoding;
}

QTextCodec* LogFilteredData::doGetDisplayEncoding() const
{
    // Safety check: if sourceLogData_ is null (object being destroyed), return nullptr
    if ( !sourceLogData_ ) {
        return nullptr;
    }
    return sourceLogData_->getDisplayEncoding();
}

void LogFilteredData::doAttachReader() const
{
    attachReaderIfNeeded();
}

void LogFilteredData::doDetachReader() const
{
    detachReaderIfNeeded();
}

void LogFilteredData::attachReaderIfNeeded() const
{
    if ( shuttingDown_ || readerAttached_ || sourceLogData_ == nullptr ) {
        return;
    }

    sourceLogData_->attachReader();
    readerAttached_ = true;
}

void LogFilteredData::detachReaderIfNeeded() const
{
    if ( !readerAttached_ || sourceLogData_ == nullptr ) {
        readerAttached_ = false;
        return;
    }

    sourceLogData_->detachReader();
    readerAttached_ = false;
}
