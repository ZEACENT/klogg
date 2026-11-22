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

#include <QCoreApplication>
#include <QPointer>
#include <QString>
#include <QtGlobal>

#include <algorithm>
#include <cassert>
#include <exception>
#include <functional>
#include <limits>
#include <type_traits>
#include <numeric>
#include <set>
#include <tuple>
#include <vector>

#include "logfiltereddata.h"
#include <QThread>
#include "searchablelogdata.h"

#include "configuration.h"
#include "readablesize.h"

namespace {
constexpr int kSearchStatusPresentationIntervalMs = 100;

bool traceSearchTerminals()
{
    static const bool enabled = qEnvironmentVariableIsSet( "KLOGG_TEST_TRACE_SEARCH_TERMINALS" );
    return enabled;
}
}

namespace {

// The bitmap is compressed, not a per-row GUI copy. Both membership and text
// are owned independently of the mutable filtered model and its visibility.
class FilteredReadSnapshot final : public klogg::LogSnapshot {
public:
    FilteredReadSnapshot( SearchableLogData::ReadSnapshot::Ptr source,
                          SearchResultArray rows, SearchResultArray matches )
        : source_( std::move( source ) ), rows_( std::move( rows ) ),
          matches_( std::move( matches ) ) {}

    LinesCount lineCount() const override { return LinesCount{ rows_.cardinality() }; }
    klogg::SnapshotRow row( LineNumber index ) const override
    {
        const auto line = sourceLine( index );
        auto result = source_->row( line );
        result.match = matches_.contains( line.get() );
        return result;
    }
    QString lineString( LineNumber index ) const override
    {
        return source_->lineString( sourceLine( index ) );
    }
    std::optional<uint64_t> sourceBytesForLine( LineNumber index ) const override
    {
        return source_->sourceBytesForLine( sourceLine( index ) );
    }
    bool preservesPresentationOf( const klogg::LogSnapshot& previous ) const override
    {
        if ( this == &previous ) return true;
        const auto* old = dynamic_cast<const FilteredReadSnapshot*>( &previous );
        if ( !old || !source_->preservesPresentationOf( *old->source_ )
             || !old->rows_.isSubset( rows_ ) ) return false;
        const auto oldCutoff = old->source_->lineCount().get();
        // Rank at the SOURCE cutoff also detects formerly hidden rows after
        // the old last visible row. A last-visible-row test would miss them.
        const auto retainedCount = oldCutoff == 0 ? uint64_t{ 0 } : rows_.rank( oldCutoff - 1 );
        return retainedCount == old->rows_.cardinality();
    }
    OptionalLineNumber rowForIdentity( const klogg::LogRowId& identity ) const override
    {
        const auto line = source_->rowForIdentity( identity );
        if ( !line || !rows_.contains( line->get() ) ) {
            return {};
        }
        return LineNumber{ rows_.rank( line->get() ) - 1 };
    }
private:
    LineNumber sourceLine( LineNumber index ) const
    {
        uint64_t line = 0;
        if ( !rows_.select( index.get(), &line ) ) {
            throw klogg::SnapshotError{ "Filtered snapshot row is out of bounds" };
        }
        return LineNumber{ line };
    }
    SearchableLogData::ReadSnapshot::Ptr source_;
    SearchResultArray rows_;
    SearchResultArray matches_;
};

uint64_t countBitmapValuesInHalfOpenRange( const SearchResultArray& values,
                                           LineNumber first, LineNumber end )
{
    if ( end <= first ) {
        return 0;
    }

    // CRoaring rank is inclusive: rank(x) counts values <= x. Convert the
    // half-open range [first, end) without constructing a temporary bitmap.
    const auto countThroughEnd = values.rank( end.get() - 1 );
    const auto countBeforeFirst
        = first == 0_lnum ? uint64_t{ 0 } : values.rank( first.get() - 1 );
    Q_ASSERT( countThroughEnd >= countBeforeFirst );
    return countThroughEnd - countBeforeFirst;
}

} // namespace

// Usual constructor: just copy the data, the search is started by runSearch()
LogFilteredData::LogFilteredData( const SearchableLogData* logData )
    : AbstractLogData()
    , matching_lines_( SearchResultArray() )
    , currentRegExp_()
    , visibility_()
    , searchResultsRefreshTimer_( klogg::kIncrementalPresentationIntervalMs,
                                  [ this ] { publishPendingSearchResults(); } )
    , searchStatusRefreshTimer_( kSearchStatusPresentationIntervalMs,
                                 [ this ] { publishPendingSearchStatus(); } )
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
             &LogFilteredData::handleSearchProgressed, Qt::DirectConnection );
    connect( &retirementWatcher_, &QFutureWatcher<void>::finished, this,
             &LogFilteredData::finishRetirement );
    connect( sourceLogData_, &AbstractLogData::interpretationChanged, this, [ this ] {
        if ( shuttingDown_ ) return;
        // Retained leases remain readable, but their membership no longer
        // describes the source's current interpretation. Retire queued work
        // before notifying selection/presentation consumers.
        interruptSearch();
        bumpSearchGeneration();
        searchResultsCache_.clear();
        contextLinesListValid_ = false;
        refreshPresentedSourceForDisplay();
        notifyInterpretationChanged();
    } );
    connect( sourceLogData_, &SearchableLogData::loadingFinished, this,
             [ this ]( LoadingStatus ) {
                 if ( !shuttingDown_ ) refreshPresentedSourceForDisplay();
             } );
}

LogFilteredData::~LogFilteredData()
{
    shuttingDown_ = true;
    cancelPendingPublications();

    interruptSearch();
    // Cancel queued work and wait for any in-flight search operations to fully
    // stop before detaching the file reader.  Without this, a queued search can
    // start after the file handle is closed, causing STATUS_HEAP_CORRUPTION
    // (0xC0000374) on Windows.
    workerThread_.shutdownAndWait();

    workerThread_.blockSignals( true );
    disconnect( &workerThread_, nullptr, this, nullptr );
    disconnect( this, nullptr, &workerThread_, nullptr );

    finishRetirement();

    // Queued MetaCall events can still be pending after disconnect(); remove them
    // before subobject destruction.
    QCoreApplication::removePostedEvents( &workerThread_ );
    QCoreApplication::removePostedEvents( this );

    sourceLogData_ = nullptr;
}

void LogFilteredData::setPresentedSource( SearchableLogData::ReadSnapshot::Ptr source ) const
{
    if ( completedResult_ && completedResult_->source && source
         && !source->preservesPresentationOf( *completedResult_->source ) ) {
        // Never silently attach accepted match membership to replacement text.
        // The presentation can advance independently; canonical readiness cannot.
        completedSourceInvalid_ = true;
    }
    if ( source == presentedSource_ ) return;
    if ( source && presentedSource_ ) {
        const auto key = source->cacheKey();
        if ( key && key == presentedSource_->cacheKey() ) return;
    }
    presentedSource_ = std::move( source );
    contextLinesListValid_ = false;
}

void LogFilteredData::refreshPresentedSourceForDisplay()
{
    try {
        setPresentedSource( sourceLogData_->searchableSnapshot() );
    } catch ( const klogg::SnapshotError& error ) {
        completedSourceInvalid_ = true;
        presentedSource_.reset();
        LOG_ERROR << "Cannot refresh presented source snapshot: " << error.what();
    }
}

CompletedSearchResult::Ptr LogFilteredData::completedSearchResult() const
{
    if ( completedSourceInvalid_ ) return {};
    const auto current = workerThread_.completedSearchResult();
    return current && current == completedResult_ ? current : CompletedSearchResult::Ptr{};
}

bool LogFilteredData::isSearchComplete() const
{
    const auto result = completedSearchResult();
    return result && result->source;
}

SearchResultArray LogFilteredData::presentedRows( const SearchableLogData::ReadSnapshot& source ) const
{
    SearchResultArray rows;
    const auto count = source.lineCount().get();
    if ( allLinesVisible_ && visibility_.testFlag( VisibilityFlags::Matches ) ) {
        rows.addRange( uint64_t{ 0 }, count );
        return rows;
    }
    SearchResultArray seeds;
    if ( visibility_.testFlag( VisibilityFlags::Matches ) ) seeds |= matching_lines_;
    if ( visibility_.testFlag( VisibilityFlags::Marks ) ) seeds |= marks_;
    if ( !seeds.isEmpty() && seeds.maximum() >= count ) {
        throw klogg::SnapshotError{ "Filtered layout exceeds its retained source version" };
    }
    if ( contextLinesBefore_ == 0 && contextLinesAfter_ == 0 ) return seeds;
    const auto before = static_cast<uint64_t>( contextLinesBefore_ );
    const auto after = static_cast<uint64_t>( contextLinesAfter_ );
    for ( const auto line : seeds ) {
        const auto first = line > before ? line - before : 0;
        const auto remaining = count - line - 1;
        rows.addRange( first, line + 1 + std::min( after, remaining ) );
    }
    return rows;
}

klogg::LogSnapshot::Ptr LogFilteredData::createReadSnapshot( klogg::SnapshotScope scope ) const
{
    const auto result = completedSearchResult();
    if ( scope == klogg::SnapshotScope::Matches ) {
        if ( !result || !result->source ) return {};
        return std::make_shared<FilteredReadSnapshot>( result->source, result->matches,
                                                       result->matches );
    }
    // Presented rows are a separate scope: partial/stopped searches and marks
    // beyond an accepted search cutoff still have a current immutable source.
    const auto source = sourceLogData_->searchableSnapshot();
    if ( !source ) return {};
    setPresentedSource( source );
    return std::make_shared<FilteredReadSnapshot>( source, presentedRows( *source ), matching_lines_ );
}

OptionalLineNumber LogFilteredData::rowForIdentity( const klogg::LogRowId& identity ) const
{
    const auto snapshot = createReadSnapshot( klogg::SnapshotScope::PresentedRows );
    return snapshot ? snapshot->rowForIdentity( identity ) : OptionalLineNumber{};
}

bool LogFilteredData::containsRowIdentity( const klogg::LogRowId& identity ) const
{
    return sourceLogData_ && sourceLogData_->containsRowIdentity( identity );
}

QFuture<void> LogFilteredData::retire()
{
    if ( !retirementCompletion_.isStarted() ) {
        shuttingDown_ = true;
        retirementCompletion_.reportStarted();
        cancelPendingPublications();
        retirementWatcher_.setFuture( workerThread_.retire() );
    }
    return retirementCompletion_.future();
}

void LogFilteredData::finishRetirement()
{
    detachReaderIfNeeded();
    if ( retirementCompletion_.isStarted() && !retirementCompletion_.isFinished() ) {
        retirementCompletion_.reportFinished();
    }
}

void LogFilteredData::runSearch( const RegularExpressionPattern& regExp )
{
    if ( shuttingDown_ ) return;
    runSearch( regExp, 0_lnum, LineNumber( getNbTotalLines().get() ) );
}

// Run the search and send newDataAvailable() signals.
void LogFilteredData::runSearch( const RegularExpressionPattern& regExp, LineNumber startLine,
                                 LineNumber endLine )
{
    LOG_DEBUG << "Entering runSearch";
    if ( shuttingDown_ ) return;

    const auto& config = Configuration::get();

    clearSearch();
    setAllLinesVisible( false );
    currentRegExp_ = regExp;
    currentSearchKey_ = makeCacheKey( regExp, startLine, endLine );
    SearchableLogData::ReadSnapshot::Ptr snapshot;
    try {
        snapshot = sourceLogData_->searchableSnapshot();
    } catch ( const klogg::SnapshotError& error ) {
        searchResultsCache_.clear();
        LOG_ERROR << "Cannot acquire filtered search snapshot: " << error.what();
        publishTerminal( 0_lcount, startLine, currentSearchGeneration() );
        return;
    }
    setPresentedSource( snapshot );
    const auto sourceKey = snapshot ? snapshot->cacheKey() : std::nullopt;
    for ( auto entry = searchResultsCache_.begin(); entry != searchResultsCache_.end(); ) {
        const auto& completed = entry->second.completed;
        if ( !sourceKey || !completed || !completed->source
             || completed->source->cacheKey() != sourceKey ) {
            entry = searchResultsCache_.erase( entry );
        }
        else {
            ++entry;
        }
    }
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
            workerThread_.restoreCompletedResult( *cachedResults->second.completed );
            completedResult_ = workerThread_.completedSearchResult();
            nbLinesProcessed_ = completedResult_->processedLines;
            publishTerminal( LinesCount( matching_lines_.cardinality() ), startLine,
                             cachedGeneration );
        }
    }

    if ( shouldRunSearch ) {
        attachReaderIfNeeded();
        workerThread_.search( currentRegExp_, startLine, endLine, std::move( snapshot ) );
    }
}

void LogFilteredData::updateSearch( LineNumber startLine, LineNumber endLine )
{
    LOG_DEBUG << "Entering updateSearch";
    if ( shuttingDown_ ) return;

    cancelPendingPublications();
    completedResult_.reset();
    setAllLinesVisible( false );
    currentSearchKey_ = {};

    attachReaderIfNeeded();
    setPresentedSource( workerThread_.updateSearch(
        currentRegExp_, startLine, endLine, LineNumber( nbLinesProcessed_.get() ) ) );
}

void LogFilteredData::interruptSearch()
{
    LOG_DEBUG << "Entering interruptSearch";

    workerThread_.interrupt();
}

void LogFilteredData::bumpSearchGeneration()
{
    cancelPendingPublications();
    completedResult_.reset();
    completedSourceInvalid_ = false;
    workerThread_.bumpGeneration();
}

void LogFilteredData::clearSearch( bool dropCache )
{
    if ( shuttingDown_ ) return;
    // Clearing abandons both the model and every queued payload from the old
    // search, so retire its generation before interrupting the worker.
    bumpSearchGeneration();
    interruptSearch();

    allLinesVisible_ = false;
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

void LogFilteredData::setAllLinesVisible( bool visible )
{
    if ( allLinesVisible_ == visible ) {
        return;
    }

    allLinesVisible_ = visible;
    contextLinesListValid_ = false;
}

LineNumber LogFilteredData::getMatchingLineNumber( LineNumber matchNum ) const
{
    if ( allLinesVisible_ && visibility_.testFlag( VisibilityFlags::Matches ) ) {
        return matchNum;
    }

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

LogFilteredData::LineTypeRangeCounts
LogFilteredData::countLineTypesInRange( LineNumber first, LineNumber end ) const
{
    if ( end <= first ) {
        return { 0_lcount, 0_lcount };
    }

    const bool matchesVisible = visibility_.testFlag( VisibilityFlags::Matches );
    const bool marksVisible = visibility_.testFlag( VisibilityFlags::Marks );
    if ( !matchesVisible && !marksVisible ) {
        return { 0_lcount, 0_lcount };
    }

    const auto matchCount = matchesVisible
                                ? countBitmapValuesInHalfOpenRange( matching_lines_, first, end )
                                : uint64_t{ 0 };
    if ( !marksVisible ) {
        return { LinesCount( matchCount ), 0_lcount };
    }

    if ( !matchesVisible ) {
        return { 0_lcount,
                 LinesCount( countBitmapValuesInHalfOpenRange( marks_, first, end ) ) };
    }

    // |matches union marks| - |matches| is exactly the mark-only cardinality,
    // so match precedence needs no intersection bitmap or per-line lookup.
    const auto unionCount
        = countBitmapValuesInHalfOpenRange( marks_and_matches_, first, end );
    Q_ASSERT( unionCount >= matchCount );
    return { LinesCount( matchCount ), LinesCount( unionCount - matchCount ) };
}

LogFilteredData::LineType LogFilteredData::lineTypeByIndex( LineNumber index ) const
{
    if ( allLinesVisible_ && visibility_.testFlag( VisibilityFlags::Matches ) ) {
        return lineTypeByLine( index );
    }

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
    if ( allLinesVisible_ && visibility_.testFlag( VisibilityFlags::Matches ) ) {
        const auto totalLines = sourceLogData_ ? sourceLogData_->getNbLine() : 0_lcount;
        for ( auto line = 0_lnum; line < LineNumber( totalLines.get() ); ++line ) {
            callback( line );
        }
        return;
    }

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

std::optional<klogg::ReviewMarkLocation>
LogFilteredData::resolveReviewMark( const klogg::LogRowId& identity ) const
{
    Q_ASSERT( QThread::currentThread() == thread() );
    const auto row = sourceLogData_ ? sourceLogData_->rowForIdentity( identity ) : OptionalLineNumber{};
    if ( !row ) return {};
    return klogg::ReviewMarkLocation{ {}, *row };
}

std::optional<klogg::ReviewMarksSnapshot> LogFilteredData::reviewMarksSnapshot() const
{
    Q_ASSERT( QThread::currentThread() == thread() );
    if ( marks_.cardinality() > MaxMarks ) return {};
    klogg::ReviewMarksSnapshot result;
    result.revision = marksRevision_;
    if ( !marks_.isEmpty() ) result.groups.push_back( { {}, getMarks() } );
    return result;
}

LogFilteredData::MarksSnapshot LogFilteredData::marksSnapshot() const
{
    return { marksRevision_, getMarks() };
}

struct LogFilteredData::MarksUpdateCapture {
    QString ownerId;
    uint64_t revision = 0;
    bool changed = false;
    SearchResultArray candidate;
    SearchResultArray matches;
    SearchableLogData::ReadSnapshot::Ptr source;
    Visibility visibility;
    bool allVisible = false;
    int before = 0;
    int after = 0;
    bool needsContext = false;
    size_t contextWorkUnits = 0;
    OptionalLineNumber lastContextSeed;
};
struct LogFilteredData::PreparedMarksUpdate {
    std::shared_ptr<const MarksUpdateCapture> captured;
    SearchResultArray marks;
    SearchResultArray combined;
    klogg::vector<LineNumber> context;
    bool consumed = false;
};
struct LogFilteredData::MarksUpdatePublication::State {
    LogFilteredData* owner;
    std::shared_ptr<PreparedMarksUpdate> prepared;
    SearchableLogData::ReadSnapshot::Ptr source;
    bool completedSourceInvalid = false;
};
LogFilteredData::MarksUpdatePublication::MarksUpdatePublication( std::unique_ptr<State> state )
    : state_( std::move( state ) ) {}
LogFilteredData::MarksUpdatePublication::~MarksUpdatePublication()
{
    state_->owner->marksPublicationLocked_ = false;
}
bool LogFilteredData::MarksUpdatePublication::install() noexcept
{
    static_assert( std::is_nothrow_move_assignable_v<SearchResultArray>,
                   "Mark publication requires allocation-free bitmap moves" );
    static_assert( std::is_nothrow_move_assignable_v<klogg::vector<LineNumber>>,
                   "Mark publication requires allocation-free context moves" );
    auto& owner = *state_->owner;
    auto& prepared = *state_->prepared;
    Q_ASSERT( QThread::currentThread() == owner.thread() );
    if ( prepared.consumed ) return false;
    if ( prepared.captured->changed ) {
        owner.marks_ = std::move( prepared.marks );
        owner.marks_and_matches_ = std::move( prepared.combined );
        owner.contextLinesList_ = std::move( prepared.context );
        owner.contextLinesListValid_ = prepared.captured->needsContext;
        owner.presentedSource_ = std::move( state_->source );
        owner.completedSourceInvalid_ = state_->completedSourceInvalid;
        owner.maxLengthContext_.reset();
        owner.maxLengthMarks_ = owner.marks_.isEmpty() ? std::optional<LineLength>{ 0_length } : std::nullopt;
        owner.longestMark_.reset();
        owner.unmeasuredMarks_ = SearchResultArray{};
        ++owner.marksRevision_;
    }
    prepared.consumed = true;
    return true;
}
std::shared_ptr<const LogFilteredData::MarksUpdateCapture>
LogFilteredData::captureMarksUpdate( const QList<LineNumber>& candidate ) const
{
    Q_ASSERT( QThread::currentThread() == thread() );
    if ( !sourceLogData_ || static_cast<size_t>( candidate.size() ) > MaxMarks ) return {};
    auto captured = std::make_shared<MarksUpdateCapture>();
    captured->ownerId = reviewMarksId();
    captured->revision = marksRevision_;
    for ( const auto row : candidate ) captured->candidate.add( row.get() );
    captured->changed = !( captured->candidate == marks_ );
    if ( !captured->changed ) return captured;
    if ( marksRevision_ == std::numeric_limits<uint64_t>::max() ) return {};
    const auto candidateBytes = captured->candidate.getSizeInBytes();
    if ( candidateBytes > MaxPreparedBitmapBytes
         || matching_lines_.getSizeInBytes() > MaxPreparedBitmapBytes - candidateBytes ) return {};
    captured->visibility = visibility_;
    captured->allVisible = allLinesVisible_;
    captured->before = contextLinesBefore_;
    captured->after = contextLinesAfter_;
    captured->needsContext = ( contextLinesBefore_ != 0 || contextLinesAfter_ != 0 )
        && !( allLinesVisible_ && visibility_.testFlag( VisibilityFlags::Matches ) );
    if ( captured->needsContext ) {
        const auto expansion = uint64_t{ 1 } + static_cast<uint64_t>( contextLinesBefore_ )
            + static_cast<uint64_t>( contextLinesAfter_ );
        auto availableSeeds = MaxPreparedContextRows / expansion;
        for ( const auto flag : { VisibilityFlags::Matches, VisibilityFlags::Marks } ) {
            if ( !visibility_.testFlag( flag ) ) continue;
            const auto& seeds = flag == VisibilityFlags::Matches ? matching_lines_ : captured->candidate;
            if ( seeds.cardinality() > availableSeeds ) return {};
            availableSeeds -= seeds.cardinality();
            captured->contextWorkUnits += static_cast<size_t>( seeds.cardinality() * expansion );
            if ( !seeds.isEmpty() ) {
                const LineNumber last{ seeds.maximum() };
                if ( captured->lastContextSeed < last ) captured->lastContextSeed = last;
            }
        }
    }
    captured->source = sourceLogData_->searchableSnapshot();
    if ( !captured->source ) return {};
    const auto count = captured->source->lineCount();
    // Existing stored marks may outlive a source cutoff; preserve them, but
    // never introduce a new bookmark beyond the captured source's admitted rows.
    for ( const auto row : candidate )
        if ( row >= count && !marks_.contains( row.get() ) ) return {};
    captured->matches = matching_lines_;
    return captured;
}
std::shared_ptr<LogFilteredData::PreparedMarksUpdate>
LogFilteredData::prepareMarksUpdate( const std::shared_ptr<const MarksUpdateCapture>& captured )
{
    if ( !captured ) return {};
    auto prepared = std::make_shared<PreparedMarksUpdate>();
    prepared->captured = captured;
    if ( captured->changed ) {
        prepared->marks = captured->candidate;
        prepared->combined = captured->matches | captured->candidate;
        if ( captured->needsContext ) {
            prepared->context = buildContextLines( captured->matches, captured->candidate,
                captured->visibility, captured->before, captured->after, captured->source->lineCount() );
        }
    }
    return prepared;
}
std::unique_ptr<LogFilteredData::MarksUpdatePublication>
LogFilteredData::lockMarksUpdate( const std::shared_ptr<PreparedMarksUpdate>& prepared )
{
    Q_ASSERT( QThread::currentThread() == thread() );
    if ( !prepared || prepared->consumed || marksPublicationLocked_ || !sourceLogData_ ) return {};
    const auto& captured = *prepared->captured;
    if ( captured.ownerId != reviewMarksId() || captured.revision != marksRevision_ ) return {};
    SearchableLogData::ReadSnapshot::Ptr source;
    auto invalid = completedSourceInvalid_;
    if ( captured.changed ) {
        if ( !( captured.matches == matching_lines_ ) || captured.visibility != visibility_
             || captured.allVisible != allLinesVisible_ || captured.before != contextLinesBefore_
             || captured.after != contextLinesAfter_ ) return {};
        source = sourceLogData_->searchableSnapshot();
        if ( !source ) return {};
        const auto key = source->cacheKey();
        const bool unchanged = source == captured.source || ( key && key == captured.source->cacheKey() );
        if ( !unchanged && !source->preservesPresentationOf( *captured.source ) ) return {};
        if ( captured.needsContext && captured.lastContextSeed
             && source->lineCount() != captured.source->lineCount() ) {
            const auto count = captured.source->lineCount().get();
            const auto last = captured.lastContextSeed->get();
            // Sealed append is safe only when no prepared context range was
            // clipped by the old source boundary. Otherwise rebuild privately.
            if ( last >= count || static_cast<uint64_t>( captured.after ) > count - last - 1 ) return {};
        }
        if ( completedResult_ && completedResult_->source
             && !source->preservesPresentationOf( *completedResult_->source ) ) invalid = true;
    }
    auto state = std::make_unique<MarksUpdatePublication::State>(
        MarksUpdatePublication::State{ this, prepared, std::move( source ), invalid } );
    auto publication = std::unique_ptr<MarksUpdatePublication>( new MarksUpdatePublication( std::move( state ) ) );
    marksPublicationLocked_ = true;
    return publication;
}

class LogFilteredData::ReviewMarksUpdate final : public klogg::PreparedReviewMarks {
  public:
    ReviewMarksUpdate( QPointer<LogFilteredData> owner, std::shared_ptr<PreparedMarksUpdate> update )
        : owner_( std::move( owner ) ), update_( std::move( update ) ) {}
    std::unique_ptr<klogg::ReviewMarksPublication> acquire() override
    {
        if ( !owner_ ) return {};
        auto guard = owner_->lockMarksUpdate( update_ );
        if ( !guard ) return {};
        return std::make_unique<Publication>( owner_, std::move( guard ), update_->captured->changed );
    }
  private:
    class Publication final : public klogg::ReviewMarksPublication {
      public:
        Publication( QPointer<LogFilteredData> owner, std::unique_ptr<MarksUpdatePublication> guard,
                     bool changed )
            : owner_( std::move( owner ) ), guard_( std::move( guard ) ), changed_( changed ) {}
        void install() noexcept override
        {
            if ( guard_ && !installed_ ) installed_ = guard_->install();
        }
        void release() noexcept override { guard_.reset(); }
        void notify() override
        {
            if ( guard_ || !installed_ || notified_ || !changed_ || !owner_ ) return;
            notified_ = true;
            Q_EMIT owner_->marksChanged();
        }
      private:
        QPointer<LogFilteredData> owner_;
        std::unique_ptr<MarksUpdatePublication> guard_;
        bool changed_;
        bool installed_ = false;
        bool notified_ = false;
    };
    QPointer<LogFilteredData> owner_;
    std::shared_ptr<PreparedMarksUpdate> update_;
};

klogg::ReviewMarksPreparer LogFilteredData::prepareReviewMarks(
    const klogg::ReviewMarksSnapshot& candidate, klogg::ReviewMarksBudget& budget )
{
    Q_ASSERT( QThread::currentThread() == thread() );
    if ( candidate.scope != klogg::ReviewMarksSnapshot::Scope::SourceModel
         || candidate.revision != marksRevision_ || candidate.groups.size() > 1
         || ( !candidate.groups.empty() && !candidate.groups.front().memberPath.isEmpty() ) ) return {};
    const auto captured = captureMarksUpdate( candidate.groups.empty()
        ? QList<LineNumber>{} : candidate.groups.front().rows );
    if ( !captured ) return {};
    size_t cost = 0;
    if ( captured->changed ) {
        // Charge container overhead and private copies, not just encoded bits.
        cost = 16 * ( captured->matches.getSizeInBytes() + captured->candidate.getSizeInBytes() );
        cost += 64 * captured->contextWorkUnits;
    }
    if ( !budget.charge( cost ) ) return {};
    const QPointer<LogFilteredData> owner{ this };
    return [owner, captured]( const std::atomic<bool>& cancelled ) -> std::shared_ptr<klogg::PreparedReviewMarks> {
        if ( cancelled ) return {};
        auto prepared = prepareMarksUpdate( captured );
        if ( cancelled || !prepared ) return {};
        return std::make_shared<ReviewMarksUpdate>( owner, std::move( prepared ) );
    };
}

void LogFilteredData::toggleMark( LineNumber line )
{
    // Safety check: if sourceLogData_ is null (object being destroyed), return early
    if ( !sourceLogData_ ) {
        return;
    }
    refreshPresentedSourceForDisplay();
    if ( ( line >= 0_lnum ) && line < sourceLogData_->getNbLine() ) {
        ++marksRevision_;
        if ( !marks_.addChecked( line.get() ) ) {
            marks_.remove( line.get() );
            updateMarksProjection( {}, line );
        }
        else {
            updateMarksProjection( line, {} );
        }
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
    refreshPresentedSourceForDisplay();
    if ( ( line >= 0_lnum ) && line < sourceLogData_->getNbLine() ) {
        if ( marks_.addChecked( line.get() ) ) {
            ++marksRevision_;
            updateMarksProjection( line, {} );
        }
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
    // rank() is inclusive (counts elements <= line): when line is itself
    // marked it includes that mark, otherwise it counts exactly the marks
    // strictly before line. Either way `before` is the number of marks
    // strictly before line, so the nearest one sits at index before - 1.
    const LineNumber::UnderlyingType before
        = marks_.contains( line.get() ) ? rank - 1 : rank;

    if ( before == 0 ) {
        return marked_line;
    }

    LineNumber::UnderlyingType nextMark;
    if ( marks_.select( before - 1, &nextMark ) ) {
        marked_line = LineNumber( nextMark );
    }

    return marked_line;
}

void LogFilteredData::deleteMark( LineNumber line )
{
    if ( marks_.removeChecked( line.get() ) ) {
        ++marksRevision_;
        updateMarksProjection( {}, line );
    }
}

void LogFilteredData::updateMarksProjection( OptionalLineNumber addedLine,
                                             OptionalLineNumber removedLine )
{
    marks_and_matches_ = matching_lines_ | marks_;
    contextLinesListValid_ = false;
    if ( removedLine ) {
        unmeasuredMarks_.remove( removedLine->get() );
        if ( longestMark_ == removedLine ) {
            maxLengthMarks_.reset();
            longestMark_.reset();
            unmeasuredMarks_ = {};
        }
    }
    if ( addedLine && maxLengthMarks_ ) unmeasuredMarks_.add( addedLine->get() );
}

LineLength LogFilteredData::marksMaxLength() const
{
    if ( !sourceLogData_ ) return 0_length;
    auto maximum = maxLengthMarks_.value_or( 0_length );
    auto longest = longestMark_;
    const auto& pending = maxLengthMarks_ ? unmeasuredMarks_ : marks_;
    const auto sourceCount = sourceLogData_->getNbLine().get();
    for ( const auto line : pending ) {
        if ( line >= sourceCount ) continue;
        const auto width = sourceLogData_->getLineLength( LineNumber( line ) );
        if ( width > maximum ) {
            maximum = width;
            longest = LineNumber( line );
        }
    }
    // Publish the cache only after every requested measurement succeeds.
    maxLengthMarks_ = maximum;
    longestMark_ = longest;
    unmeasuredMarks_ = {};
    return maximum;
}

void LogFilteredData::clearMarks()
{
    if ( !marks_.isEmpty() ) {
        ++marksRevision_;
    }
    marks_ = {};
    marks_and_matches_ = matching_lines_;
    maxLengthMarks_ = 0_length;
    longestMark_.reset();
    unmeasuredMarks_ = {};
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
    before = std::max( 0, before );
    after = std::max( 0, after );
    if ( before == contextLinesBefore_ && after == contextLinesAfter_ ) return;
    contextLinesBefore_ = before;
    contextLinesAfter_ = after;
    contextLinesListValid_ = false; // Invalidate only a changed projection
}

void LogFilteredData::rebuildContextLinesList() const
{
    maxLengthContext_.reset();
    const bool hasContext = contextLinesBefore_ != 0 || contextLinesAfter_ != 0;
    auto totalLines = 0_lcount;
    if ( hasContext ) {
        if ( !sourceLogData_ ) return;
        totalLines = presentedSource_ ? presentedSource_->lineCount() : sourceLogData_->getNbLine();
    }
    contextLinesList_ = buildContextLines( matching_lines_, marks_, visibility_,
                                           contextLinesBefore_, contextLinesAfter_, totalLines );
    contextLinesListValid_ = true;
}

klogg::vector<LineNumber> LogFilteredData::buildContextLines(
    const SearchResultArray& matches, const SearchResultArray& marks, Visibility visibility,
    int before, int after, LinesCount totalLines )
{
    klogg::vector<LineNumber> lines;
    // Both visible matches and visible marks seed context, matching the native
    // display contract. Membership never decodes payload or accesses an owner.
    if ( before == 0 && after == 0 ) {
        SearchResultArray combinedLines;
        if ( visibility.testFlag( VisibilityFlags::Matches ) ) combinedLines |= matches;
        if ( visibility.testFlag( VisibilityFlags::Marks ) ) combinedLines |= marks;
        lines.reserve( combinedLines.cardinality() );
        combinedLines.iterate(
            []( uint64_t line, void* context ) -> bool {
                static_cast<klogg::vector<LineNumber>*>( context )->emplace_back( LineNumber( line ) );
                return true;
            },
            static_cast<void*>( &lines ) );
        return lines;
    }

    std::set<LineNumber::UnderlyingType> linesSet;
    struct ContextData {
        std::set<LineNumber::UnderlyingType>* linesSet;
        LineNumber::UnderlyingType totalLines;
        uint64_t contextLinesBefore;
        uint64_t contextLinesAfter;
    };
    ContextData contextData = { &linesSet, totalLines.get(),
                                static_cast<uint64_t>( before ), static_cast<uint64_t>( after ) };
    auto addLineWithContext = []( uint64_t matchLine, void* context ) -> bool {
        auto* data = static_cast<ContextData*>( context );
        data->linesSet->insert( matchLine );
        for ( uint64_t i = 1; i <= data->contextLinesBefore; ++i ) {
            const LineNumber::UnderlyingType contextLine = matchLine >= i ? matchLine - i : 0;
            if ( contextLine < data->totalLines ) data->linesSet->insert( contextLine );
        }
        for ( uint64_t i = 1; i <= data->contextLinesAfter; ++i ) {
            const LineNumber::UnderlyingType contextLine = matchLine + i;
            if ( contextLine < data->totalLines ) data->linesSet->insert( contextLine );
        }
        return true;
    };
    if ( visibility.testFlag( VisibilityFlags::Matches ) )
        matches.iterate( addLineWithContext, static_cast<void*>( &contextData ) );
    if ( visibility.testFlag( VisibilityFlags::Marks ) )
        marks.iterate( addLineWithContext, static_cast<void*>( &contextData ) );
    lines.reserve( linesSet.size() );
    for ( const auto line : linesSet ) lines.emplace_back( LineNumber( line ) );
    return lines;
}

void LogFilteredData::updateSearchResultsCache()
{
    const auto& config = Configuration::get();
    if ( !config.useSearchResultsCache() || !completedResult_ || !completedResult_->source
         || !completedResult_->source->cacheKey() ) {
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

        searchResultsCache_[ currentSearchKey_ ] = { matching_lines_, maxLength_, completedResult_ };
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
        if ( progress == 100 && traceSearchTerminals() ) {
            LOG_WARNING << "Search terminal discarded during filtered-data shutdown generation "
                     << generation;
        }
        return;
    }

    assert( nbMatches >= 0_lcount );

    if ( generation != currentSearchGeneration() ) {
        if ( progress == 100 && traceSearchTerminals() ) {
            LOG_WARNING << "Search terminal discarded by filtered-data generation " << generation
                     << " current generation " << currentSearchGeneration();
        }
        return;
    }

    if ( progress == 100 && traceSearchTerminals() ) {
        LOG_WARNING << "Search terminal entered filtered data generation " << generation;
    }
    const auto searchResults = workerThread_.getSearchResults();
    const bool resultsChanged = !searchResults.newMatches.isEmpty();

    matching_lines_ |= searchResults.newMatches;
    marks_and_matches_ |= searchResults.newMatches;

    maxLength_ = searchResults.maxLength;
    nbLinesProcessed_ = searchResults.processedLines;
    contextLinesListValid_ = false;

    if ( progress == 100 ) {
        completedResult_ = workerThread_.completedSearchResult();
        completedSourceInvalid_ = false;
        if ( completedResult_ ) {
            setPresentedSource( presentedSource_ ? presentedSource_ : completedResult_->source );
            // A delta cannot represent removal of a re-searched final row.
            matching_lines_ = completedResult_->matches;
            marks_and_matches_ = matching_lines_ | marks_;
            maxLength_ = completedResult_->maxLength;
            nbLinesProcessed_ = completedResult_->processedLines;
            nbMatches = LinesCount{ matching_lines_.cardinality() };
        }
    }

    if ( progress == 100 && completedResult_
         && nbLinesProcessed_.get() == getExpectedSearchEnd( currentSearchKey_ ).get() ) {
        updateSearchResultsCache();
    }

    if ( progress == 100 ) {
        publishTerminal( nbMatches, initialLine, generation );
        detachReaderIfNeeded();

        LOG_INFO << "Matches size " << readableSize( matching_lines_.getSizeInBytes( false ) )
                 << ", marks size " << readableSize( marks_.getSizeInBytes( false ) )
                 << ", union size " << readableSize( marks_and_matches_.getSizeInBytes( false ) );
        return;
    }

    pendingSearchStatus_ = SearchStatusPayload{ nbMatches, progress, initialLine, generation };
    searchStatusRefreshTimer_.request();

    if ( resultsChanged || !pendingSearchResults_.has_value()
         || pendingSearchResults_->nbMatches != nbMatches ) {
        pendingSearchResults_
            = SearchResultsPayload{ nbMatches, initialLine, false, generation };
        searchResultsRefreshTimer_.request();
    }
}

void LogFilteredData::cancelPendingPublications()
{
    searchResultsRefreshTimer_.cancel();
    searchStatusRefreshTimer_.cancel();
    pendingSearchResults_.reset();
    pendingSearchStatus_.reset();
}

void LogFilteredData::publishPendingSearchResults()
{
    if ( shuttingDown_ || !pendingSearchResults_.has_value() ) {
        pendingSearchResults_.reset();
        return;
    }

    const auto payload = *pendingSearchResults_;
    pendingSearchResults_.reset();
    if ( payload.generation != currentSearchGeneration() ) {
        return;
    }

    Q_EMIT searchResultsChanged( payload.nbMatches, payload.initialLine, payload.terminal,
                                 payload.generation );
}

void LogFilteredData::publishPendingSearchStatus()
{
    if ( shuttingDown_ || !pendingSearchStatus_.has_value() ) {
        pendingSearchStatus_.reset();
        return;
    }

    const auto payload = *pendingSearchStatus_;
    pendingSearchStatus_.reset();
    if ( payload.generation != currentSearchGeneration() ) {
        return;
    }

    Q_EMIT searchProgressed( payload.nbMatches, payload.progress, payload.initialLine,
                             payload.generation );
}

void LogFilteredData::publishTerminal( LinesCount nbMatches, LineNumber initialLine,
                                       quint64 generation )
{
    cancelPendingPublications();
    if ( shuttingDown_ || generation != currentSearchGeneration() ) {
        if ( traceSearchTerminals() ) {
            LOG_WARNING << "Search terminal publication rejected generation " << generation
                     << " current generation " << currentSearchGeneration()
                     << " shutting down " << shuttingDown_;
        }
        return;
    }

    const QPointer<LogFilteredData> owner{ this };
    const auto ticket = workerThread_.currentRequestTicket();
    Q_EMIT searchResultsChanged( nbMatches, initialLine, true, generation );
    if ( !owner || owner->shuttingDown_ || generation != owner->currentSearchGeneration()
         || ticket != owner->workerThread_.currentRequestTicket() ) return;
    Q_EMIT owner->searchProgressed( nbMatches, 100, initialLine, generation );
    if ( traceSearchTerminals() ) {
        LOG_WARNING << "Search terminal published generation " << generation;
    }
}

LineNumber LogFilteredData::findLogDataLine( LineNumber index ) const
{
    if ( allLinesVisible_ && visibility_.testFlag( VisibilityFlags::Matches ) ) {
        return index;
    }

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
    if ( allLinesVisible_ && visibility_.testFlag( VisibilityFlags::Matches ) ) {
        return lineNum;
    }

    // If context lines are enabled, search in context lines list
    if ( contextLinesBefore_ > 0 || contextLinesAfter_ > 0 ) {
        if ( !contextLinesListValid_ ) {
            rebuildContextLinesList();
        }
        if ( contextLinesList_.empty() ) {
            return maxValue<LineNumber>();
        }

        // Match the no-context mapping semantics: select the last visible row at
        // or before the source line, clamped to the first/last visible row.
        auto contextLineIt
            = std::upper_bound( contextLinesList_.begin(), contextLinesList_.end(), lineNum );
        if ( contextLineIt != contextLinesList_.begin() ) {
            --contextLineIt;
        }
        const auto distance = std::distance( contextLinesList_.begin(), contextLineIt );
        return LineNumber{ static_cast<LineNumber::UnderlyingType>( distance ) };
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
    if ( presentedSource_ ) {
        const auto line = getMatchingLineNumber( index );
        if ( line >= presentedSource_->lineCount() ) return {};
        try {
            return presentedSource_->lineString( line );
        } catch ( const std::exception& error ) {
            // Retained storage and strict decoding can also throw standard
            // exceptions. Contain them only at the presentation boundary.
            LOG_ERROR << "Cannot read presented row: " << error.what();
            return {};
        }
    }
    // Safety check: if sourceLogData_ is null (object being destroyed), return empty string
    if ( !sourceLogData_ ) {
        return QString();
    }

    if ( allLinesVisible_ && visibility_.testFlag( VisibilityFlags::Matches ) ) {
        return sourceLogData_->getLineString( index );
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
    if ( presentedSource_ ) {
        return untabify( doGetLineString( index ) );
    }
    // Safety check: if sourceLogData_ is null (object being destroyed), return empty string
    if ( !sourceLogData_ ) {
        return QString();
    }

    if ( allLinesVisible_ && visibility_.testFlag( VisibilityFlags::Matches ) ) {
        return sourceLogData_->getExpandedLineString( index );
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

klogg::vector<AnsiColorSpan> LogFilteredData::doGetLineAnsiColors( LineNumber index ) const
{
    if ( presentedSource_ ) {
        const auto line = getMatchingLineNumber( index );
        if ( line >= presentedSource_->lineCount() ) return {};
        try {
            auto colors = presentedSource_->getLinesRaw( line, 1_lcount ).decodeLineAnsiColors();
            return colors.empty() ? klogg::vector<AnsiColorSpan>{} : std::move( colors.front() );
        } catch ( const std::exception& error ) {
            LOG_ERROR << "Cannot read presented row colors: " << error.what();
            return {};
        }
    }
    if ( !sourceLogData_ ) {
        return {};
    }

    if ( allLinesVisible_ && visibility_.testFlag( VisibilityFlags::Matches ) ) {
        return sourceLogData_->getLineAnsiColors( index );
    }

    if ( contextLinesBefore_ > 0 || contextLinesAfter_ > 0 ) {
        if ( !contextLinesListValid_ ) {
            rebuildContextLinesList();
        }
        if ( index.get() < static_cast<LineNumber::UnderlyingType>( contextLinesList_.size() ) ) {
            return sourceLogData_->getLineAnsiColors( contextLinesList_[ index.get() ] );
        }
        return {};
    }

    return sourceLogData_->getLineAnsiColors( findLogDataLine( index ) );
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
    if ( allLinesVisible_ && visibility_.testFlag( VisibilityFlags::Matches ) ) {
        return index;
    }

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
    if ( allLinesVisible_ && visibility_.testFlag( VisibilityFlags::Matches ) ) {
        return sourceLogData_ ? sourceLogData_->getNbLine() : 0_lcount;
    }

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
    if ( allLinesVisible_ && visibility_.testFlag( VisibilityFlags::Matches ) ) {
        return sourceLogData_ ? sourceLogData_->getMaxLength() : 0_length;
    }

    LineLength result = qMax( maxLength_, marksMaxLength() );
    if ( contextLinesBefore_ > 0 || contextLinesAfter_ > 0 ) {
        if ( !contextLinesListValid_ ) {
            rebuildContextLinesList();
        }
        if ( !maxLengthContext_ ) {
            LineLength maximum = 0_length;
            if ( sourceLogData_ ) {
                for ( const auto line : contextLinesList_ ) {
                    maximum = qMax( maximum, sourceLogData_->getLineLength( line ) );
                }
            }
            maxLengthContext_ = maximum;
        }
        result = qMax( result, *maxLengthContext_ );
    }

    // Fall back to the source data's max length so the horizontal scrollbar
    // range always covers every line in the file.  Without this the filtered
    // view scrollbar range can be too small when matching lines are shorter
    // than other lines that may become visible (e.g. through marks or
    // context lines), causing log content to extend beyond the window even
    // when scrolled to the far right.
    if ( sourceLogData_ ) {
        result = qMax( result, sourceLogData_->getMaxLength() );
    }
    return result;
}

// Implementation of the virtual function.
LineLength LogFilteredData::doGetLineLength( LineNumber lineNum ) const
{
    if ( presentedSource_ ) {
        return LineLength{ doGetExpandedLineString( lineNum ).size() };
    }
    // Safety check: if sourceLogData_ is null (object being destroyed), return 0
    if ( !sourceLogData_ ) {
        return 0_length;
    }

    if ( allLinesVisible_ && visibility_.testFlag( VisibilityFlags::Matches ) ) {
        return sourceLogData_->getLineLength( lineNum );
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
