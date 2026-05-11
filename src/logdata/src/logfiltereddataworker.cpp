/*
 * Copyright (C) 2009, 2010 Nicolas Bonnefon and other contributors
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

#include <chrono>
#include <cmath>
#include <cstdio>
#include <exception>
#include <qsemaphore.h>
#include <utility>

#include <robin_hood.h>
#include <tbb/flow_graph.h>
#include <vector>

#include "configuration.h"
#include "dispatch_to.h"
#include "issuereporter.h"
#include "linetypes.h"
#include "log.h"
#include "progress.h"

#include "regularexpression.h"
#include "searchablelogdata.h"

#include "logfiltereddataworker.h"
#include "synchronization.h"

namespace {
struct PartialSearchResults {
    PartialSearchResults() = default;

    PartialSearchResults( const PartialSearchResults& ) = delete;
    PartialSearchResults( PartialSearchResults&& ) = default;
    PartialSearchResults& operator=( const PartialSearchResults& ) = delete;
    PartialSearchResults& operator=( PartialSearchResults&& ) = default;

    SearchResultArray matchingLines;
    LineLength maxLength;

    LineNumber chunkStart;
    LinesCount processedLines;
};

struct SearchBlockData {
    SearchBlockData() = default;
    SearchBlockData( LineNumber start, SearchableLogData::RawLines blockLines )
        : chunkStart( start )
        , lines( std::move( blockLines ) )
    {
    }

    SearchBlockData( const SearchBlockData& ) = delete;
    SearchBlockData( SearchBlockData&& ) = default;

    SearchBlockData& operator=( const SearchBlockData& ) = delete;
    SearchBlockData& operator=( SearchBlockData&& ) = default;

    LineNumber chunkStart;
    SearchableLogData::RawLines lines;

    PartialSearchResults searchResults;
};

PartialSearchResults filterLines( const PatternMatcher& matcher,
                                  const SearchableLogData::RawLines& rawLines,
                                  LineNumber chunkStart )
{
    LOG_DEBUG << "Filter lines at " << chunkStart;
    PartialSearchResults results;
    results.chunkStart = chunkStart;
    results.processedLines = LinesCount{ rawLines.endOfLines.size() };

    // Block scan (one hs_scan over the whole buffer) saves per-line call
    // overhead but pays callback + binary-search + dedup cost per match
    // position.  Benchmarks show this is only beneficial for small chunks
    // (<= ~5000 lines) where SIMD startup cost dominates.  For larger chunks
    // the sequential per-line access pattern has better cache locality.
    // Disabled for now: per-line is consistently faster at production scales.
    //
    // Known issue: block scan produces ~10% fewer matches for complex
    // patterns (alternations, optional groups) due to the `to-1` byte
    // offset in the callback not always landing inside the matching line
    // for multi-position match reports.
    //
    // TODO: re-evaluate after fixing the callback offset logic and
    //       optimizing the callback path (bitmap dedup, cached segment
    //       lookup, or Vectorscan streaming mode).

    // Per-line matching
    const auto& lines = rawLines.buildUtf8View();

    for ( auto offset = 0u; offset < lines.size(); ++offset ) {
        const auto& line = lines[ offset ];

        const auto hasMatch = matcher.hasMatch( line );

        if ( hasMatch ) {
            results.maxLength = qMax( results.maxLength, getUntabifiedLength( line ) );
            const auto lineNumber = chunkStart + LinesCount{ offset };
            results.matchingLines.add( lineNumber.get() );
        }
    }
    return results;
}

} // namespace

SearchResults SearchData::takeCurrentResults() const
{
    UniqueLock lock( dataMutex_ );
    return SearchResults{ std::exchange( newMatches_, {} ), maxLength_, nbLinesProcessed_ };
}

void SearchData::addAll( LineLength length, const SearchResultArray& matches,
                         LinesCount matchedLines, LinesCount processedLines )
{
    UniqueLock lock( dataMutex_ );

    maxLength_ = qMax( maxLength_, length );
    nbLinesProcessed_ = qMax( nbLinesProcessed_, processedLines );
    nbMatches_ += matchedLines;

    newMatches_ |= matches;
}

LinesCount SearchData::getNbMatches() const
{
    SharedLock lock( dataMutex_ );
    return nbMatches_;
}

LineNumber SearchData::getLastProcessedLine() const
{
    SharedLock lock( dataMutex_ );
    return LineNumber{ nbLinesProcessed_.get() };
}

void SearchData::deleteMatch( LineNumber line )
{
    UniqueLock lock( dataMutex_ );
    matches_.remove( line.get() );
}

void SearchData::clear()
{
    UniqueLock locker( dataMutex_ );

    maxLength_ = LineLength( 0 );
    nbLinesProcessed_ = LinesCount( 0 );
    nbMatches_ = LinesCount( 0 );
    matches_ = {};
    newMatches_ = {};
}

LogFilteredDataWorker::LogFilteredDataWorker( const SearchableLogData& sourceLogData )
    : sourceLogData_( sourceLogData )
{
    dispatchThread_ = std::thread( [ this ] { dispatchLoop(); } );
}

LogFilteredDataWorker::~LogFilteredDataWorker() noexcept
{
    // Shut down the dispatch thread first.
    {
        std::lock_guard<std::mutex> lock( requestMutex_ );
        dispatchShutdown_ = true;
    }
    requestCv_.notify_one();
    if ( dispatchThread_.joinable() ) {
        dispatchThread_.join();
    }

    // Signal any running task to stop, then wait for the thread to fully exit
    // before any other members are destroyed.  std::thread::join() guarantees
    // the OS thread has completely exited -- no race with internal Qt thread-pool
    // cleanup (QMutex::lock / QThread::isRunning crashes seen in Qt 5.15/6.9).
    interruptRequested_.set();
    if ( opThread_.joinable() ) {
        opThread_.join();
    }
}

void LogFilteredDataWorker::connectSignalsAndRun( SearchOperation* operationRequested,
                                                  OperationGeneration generation )
{
    // SearchOperation is a short-lived QObject created on the std::thread. Using
    // a queued signal-to-signal hop into this worker can leave queued metacalls
    // targeting a connection object that gets torn down with the sender. Forward
    // directly here, but marshal actual LogFilteredDataWorker signal emission
    // back onto the owner's thread to avoid cross-thread QObject signal/disconnect
    // races during teardown.
    connect( operationRequested, &SearchOperation::searchProgressed, this,
             [ this, generation ]( LinesCount nbMatches, int percent, LineNumber initialLine ) {
                 emitSearchProgressedOnOwnerThread( nbMatches, percent, initialLine, generation );
             },
             Qt::DirectConnection );
    connect( operationRequested, &SearchOperation::searchFinished, this,
             [ this, generation ] { emitSearchFinishedOnOwnerThread( generation ); },
             Qt::DirectConnection );

    operationRequested->run( searchData_ );
}

void LogFilteredDataWorker::search( const RegularExpressionPattern& regExp, LineNumber startLine,
                                    LineNumber endLine )
{
    const auto generation = operationGeneration_.fetch_add( 1 ) + 1;
    LOG_INFO << "Search requested (async dispatch, gen " << generation << ")";
    compiledExpression_ = std::make_shared<RegularExpression>( regExp );

    enqueueRequest( SearchRequest{ SearchRequest::Type::Full, regExp, startLine, endLine, {},
                                   generation, compiledExpression_ } );
}

void LogFilteredDataWorker::updateSearch( const RegularExpressionPattern& regExp,
                                          LineNumber startLine, LineNumber endLine,
                                          LineNumber position )
{
    // Signal any running search to stop at the next chunk boundary so that
    // operationsMutex_ is released quickly.
    interruptRequested_.set();

    const auto generation = operationGeneration_.fetch_add( 1 ) + 1;
    LOG_INFO << "Search update requested from " << position.get()
             << " (async dispatch, gen " << generation << ")";

    enqueueRequest( SearchRequest{ SearchRequest::Type::Update, regExp, startLine, endLine, position,
                                   generation, compiledExpression_ } );
}

void LogFilteredDataWorker::enqueueRequest( SearchRequest request )
{
    interruptRequested_.set();
    {
        std::lock_guard<std::mutex> lock( requestMutex_ );
        pendingRequest_.emplace( std::move( request ) );
    }
    requestCv_.notify_one();
}

void LogFilteredDataWorker::dispatchLoop()
{
    while ( true ) {
        SearchRequest request;
        {
            std::unique_lock<std::mutex> lock( requestMutex_ );
            requestCv_.wait( lock, [ this ] { return pendingRequest_.has_value() || dispatchShutdown_; } );
            if ( dispatchShutdown_ && !pendingRequest_.has_value() ) {
                return;
            }
            if ( !pendingRequest_.has_value() ) {
                continue;
            }
            request = std::move( *pendingRequest_ );
            pendingRequest_.reset();
        }

        // Acquire the operations mutex — this may block if a previous
        // search is still running, but that's OK because we're on the
        // dispatch thread, not the UI thread.
        ScopedLock locker( operationsMutex_ );

        // Check if this request has been superseded by a newer one.
        if ( request.generation != operationGeneration_.load() ) {
            continue;
        }

        if ( opThread_.joinable() ) {
            opThread_.join();
        }
        interruptRequested_.clear();

        QSemaphore operationStarted;
        if ( request.type == SearchRequest::Type::Full ) {
            opThread_ = std::thread(
                [ this, &operationStarted, request ] {
                    operationStarted.release();
                    ScopedLock operationLock( operationsMutex_ );
                    if ( request.generation != operationGeneration_.load() ) {
                        return;
                    }
                    auto operationRequested = std::make_unique<FullSearchOperation>(
                        sourceLogData_, interruptRequested_, request.regExp, request.startLine,
                        request.endLine, request.compiled );
                    connectSignalsAndRun( operationRequested.get(), request.generation );
                } );
        }
        else {
            opThread_ = std::thread(
                [ this, &operationStarted, request ] {
                    operationStarted.release();
                    ScopedLock operationLock( operationsMutex_ );
                    if ( request.generation != operationGeneration_.load() ) {
                        return;
                    }
                    auto operationRequested = std::make_unique<UpdateSearchOperation>(
                        sourceLogData_, interruptRequested_, request.regExp, request.startLine,
                        request.endLine, request.position, request.compiled );
                    connectSignalsAndRun( operationRequested.get(), request.generation );
                } );
        }
        operationStarted.acquire();
    }
}

void LogFilteredDataWorker::interrupt()
{
    LOG_INFO << "Search interruption requested";
    interruptRequested_.set();
    // Intentionally NOT advancing the generation here.  A user pressing the
    // Stop button calls this path and still needs the in-flight progress
    // signal (typically progress < 100, but completion semantics live in
    // CrawlerWidget::updateFilteredView) to reach the receiver -- otherwise
    // the Stop-button UI cleanup never runs.  The replaceCurrentSearch
    // pathway, which DOES want stale signals dropped, calls
    // LogFilteredData::bumpSearchGeneration() explicitly.
}

void LogFilteredDataWorker::waitForDone()
{
    if ( opThread_.joinable() ) {
        opThread_.join();
    }
}

void LogFilteredDataWorker::emitSearchProgressedOnOwnerThread( LinesCount nbMatches, int percent,
                                                               LineNumber initialLine,
                                                               OperationGeneration generation )
{
    dispatchToObject(
        [ this, nbMatches, percent, initialLine, generation ] {
            if ( generation != operationGeneration_.load() ) {
                return;
            }

            if ( percent == 100 ) {
                // Ensure terminal progress is delivered after the corresponding
                // worker std::thread has fully exited, but avoid joining a newer
                // search when this callback is delayed in the event queue.
                waitForDone();
            }

            Q_EMIT searchProgressed( nbMatches, percent, initialLine, generation );
        },
        this );
}

void LogFilteredDataWorker::emitSearchFinishedOnOwnerThread( OperationGeneration generation )
{
    dispatchToObject(
        [ this, generation ] {
            if ( generation != operationGeneration_.load() ) {
                return;
            }
            Q_EMIT searchFinished();
        },
        this );
}

// This will do an atomic copy of the object
SearchResults LogFilteredDataWorker::getSearchResults() const
{
    return searchData_.takeCurrentResults();
}

//
// Operations implementation
//

SearchOperation::SearchOperation( const SearchableLogData& sourceLogData,
                                  AtomicFlag& interruptRequested,
                                  const RegularExpressionPattern& regExp, LineNumber startLine,
                                  LineNumber endLine,
                                  std::shared_ptr<RegularExpression> compiledRegExp )

    : interruptRequested_( interruptRequested )
    , regexp_( regExp )
    , sourceLogData_( sourceLogData )
    , startLine_( startLine )
    , endLine_( endLine )
    , compiledRegExp_( std::move( compiledRegExp ) )
{
}

void SearchOperation::doSearch( SearchData& searchData, LineNumber initialLine )
{
    const auto nbSourceLines = sourceLogData_.getNbLine();

    LOG_INFO << "Searching from line " << initialLine << " to " << nbSourceLines;

    using namespace std::chrono;
    high_resolution_clock::time_point t1 = high_resolution_clock::now();

    const auto& config = Configuration::get();
    const auto matchingThreadsCount = static_cast<uint32_t>( [ &config ]() {
        if ( !config.useParallelSearch() ) {
            return 1;
        }
        const auto configuredThreadPoolSize = config.searchThreadPoolSize();
        return qMax( 1, configuredThreadPoolSize == 0 ? tbb::info::default_concurrency()
                                                      : configuredThreadPoolSize );
    }() );

    LOG_INFO << "Using " << matchingThreadsCount << " matching threads";

    // Avoid the TBB flow-graph path for the single-threaded case. It adds
    // unnecessary teardown complexity and has been a source of intermittent
    // crashes in tests during node destruction, while a straightforward loop is
    // simpler and deterministic.
    if ( matchingThreadsCount == 1 ) {
        if ( initialLine < startLine_ ) {
            initialLine = startLine_;
        }

        const auto endLine = qMin( LineNumber( nbSourceLines.get() ), endLine_ );
        const auto nbLinesInChunk = LinesCount(
            static_cast<LinesCount::UnderlyingType>( config.searchReadBufferSizeLines() ) );

        std::chrono::microseconds fileReadingDuration{ 0 };
        std::chrono::microseconds matchCombiningDuration{ 0 };
        std::chrono::microseconds matchDuration{ 0 };

        const auto totalLines = endLine - initialLine;
        LinesCount totalProcessedLines = 0_lcount;
        LineLength maxLength = 0_length;
        LinesCount nbMatches = searchData.getNbMatches();
        auto reportedMatches = nbMatches;
        int reportedPercentage = 0;

        // Reuse pre-compiled expression if available, avoiding expensive
        // Vectorscan database recompilation on incremental search updates.
        std::shared_ptr<RegularExpression> ownedExpression;
        if ( !compiledRegExp_ ) {
            ownedExpression = std::make_shared<RegularExpression>( regexp_ );
        }
        const auto& regularExpression = compiledRegExp_ ? *compiledRegExp_ : *ownedExpression;
        auto matcher = regularExpression.createMatcher();

        auto chunkStart = initialLine;
        while ( chunkStart < endLine && !interruptRequested_ ) {
            const auto lineSourceStartTime = high_resolution_clock::now();
            LOG_DEBUG << "Reading chunk starting at " << chunkStart;

            const auto linesInChunk
                = LinesCount( qMin( nbLinesInChunk.get(), ( endLine - chunkStart ).get() ) );
            auto lines = sourceLogData_.getLinesRaw( chunkStart, linesInChunk );

            const auto lineSourceEndTime = high_resolution_clock::now();
            fileReadingDuration += duration_cast<microseconds>( lineSourceEndTime
                                                                - lineSourceStartTime );

            auto matchStartTime = high_resolution_clock::now();
            auto searchResults = filterLines( *matcher, lines, chunkStart );
            auto matchEndTime = high_resolution_clock::now();
            matchDuration += duration_cast<microseconds>( matchEndTime - matchStartTime );

            const auto matchProcessorStartTime = high_resolution_clock::now();
            if ( searchResults.processedLines.get() ) {
                maxLength = qMax( maxLength, searchResults.maxLength );
                const auto matchesCount = LinesCount( searchResults.matchingLines.cardinality() );
                nbMatches += matchesCount;

                const auto processedLines = LinesCount{ searchResults.chunkStart.get()
                                                        + searchResults.processedLines.get() };
                totalProcessedLines += searchResults.processedLines;

                searchData.addAll( maxLength, searchResults.matchingLines, matchesCount,
                                   processedLines );

                LOG_DEBUG << "done Searching chunk starting at " << searchResults.chunkStart
                          << ", " << searchResults.processedLines << " lines read.";
            }

            const auto matchProcessorEndTime = high_resolution_clock::now();
            matchCombiningDuration += duration_cast<microseconds>( matchProcessorEndTime
                                                                   - matchProcessorStartTime );

            const int percentage = calculateProgress( totalProcessedLines.get(), totalLines.get() );
            if ( percentage > reportedPercentage || nbMatches > reportedMatches ) {
                Q_EMIT searchProgressed( nbMatches, std::min( 99, percentage ), initialLine );
                reportedPercentage = percentage;
                reportedMatches = nbMatches;
            }

            chunkStart = chunkStart + nbLinesInChunk;
        }

        const auto t2 = high_resolution_clock::now();
        const auto durationUs = duration_cast<microseconds>( t2 - t1 );
        const auto durationMs = duration_cast<milliseconds>( t2 - t1 );

        LOG_INFO << "Searching done, overall duration " << durationUs;
        LOG_INFO << "Line reading took " << fileReadingDuration;
        LOG_INFO << "Results combining took " << matchCombiningDuration;
        LOG_INFO << "Matching took " << matchDuration;

        const auto totalFileSize = sourceLogData_.getFileSize();
        LOG_INFO << "Searching perf "
                 << static_cast<uint64_t>(
                        std::floor( 1000.f * static_cast<float>( ( endLine - initialLine ).get() )
                                    / static_cast<float>( durationMs.count() ) ) )
                 << " lines/s";
        LOG_INFO << "Searching io perf "
                 << ( 1000.f * static_cast<float>( totalFileSize )
                      / static_cast<float>( durationMs.count() ) )
                        / ( 1024 * 1024 )
                 << " MiB/s";

        Q_EMIT searchProgressed( nbMatches, 100, initialLine );
        Q_EMIT searchFinished();
        return;
    }

    tbb::flow::graph searchGraph;

    if ( initialLine < startLine_ ) {
        initialLine = startLine_;
    }

    const auto endLine = qMin( LineNumber( nbSourceLines.get() ), endLine_ );
    const auto nbLinesInChunk = LinesCount(
        static_cast<LinesCount::UnderlyingType>( config.searchReadBufferSizeLines() ) );

    std::chrono::microseconds fileReadingDuration{ 0 };

    using BlockDataType = SearchBlockData*;
    auto blockPrefetcher
        = tbb::flow::limiter_node<BlockDataType>( searchGraph, matchingThreadsCount * 3 );

    auto lineBlocksQueue = tbb::flow::buffer_node<BlockDataType>( searchGraph );

    using RegexMatcherNode
        = tbb::flow::function_node<BlockDataType, BlockDataType, tbb::flow::rejecting>;

    using PatternMatcherPtr = std::unique_ptr<PatternMatcher>;

    // tbb::flow::function_node has a copy constructor but no move constructor.
    // Storing it directly in a vector via emplace_back would copy-construct it from
    // a temporary (which registers+deregisters with the graph), causing TBB internal
    // node-list corruption when matchingThreadsCount >= 2.  Use unique_ptr so each
    // node is constructed directly in-place on the heap -- no copy, no move.
    struct MatcherData {
        PatternMatcherPtr matcher;
        microseconds matchTime{};
    };

    klogg::vector<MatcherData> matcherData;
    matcherData.reserve( matchingThreadsCount );
    std::shared_ptr<RegularExpression> ownedExpression;
    if ( !compiledRegExp_ ) {
        ownedExpression = std::make_shared<RegularExpression>( regexp_ );
    }
    const auto& regularExpression = compiledRegExp_ ? *compiledRegExp_ : *ownedExpression;
    for ( auto index = 0u; index < matchingThreadsCount; ++index ) {
        matcherData.push_back( { regularExpression.createMatcher(), microseconds{ 0 } } );
    }

    // resultsQueue must be declared BEFORE matcherNodes so that it is destroyed AFTER
    // matcherNodes.  C++ destroys locals in reverse declaration order; each matcherNode
    // deregisters itself from resultsQueue's predecessor list during destruction, so
    // resultsQueue must still be alive when that happens.
    auto resultsQueue = tbb::flow::buffer_node<BlockDataType>( searchGraph );

    std::vector<std::unique_ptr<RegexMatcherNode>> matcherNodes;
    matcherNodes.reserve( matchingThreadsCount );
    for ( auto index = 0u; index < matchingThreadsCount; ++index ) {
        matcherNodes.push_back( std::make_unique<RegexMatcherNode>(
            searchGraph, 1, [ &matcherData, index, this ]( const BlockDataType& blockData ) {
                if ( interruptRequested_ ) {
                    LOG_INFO << "Matcher " << index << " interrupted";
                    blockData->searchResults.chunkStart = blockData->chunkStart;
                    blockData->searchResults.processedLines
                        = LinesCount{ blockData->lines.endOfLines.size() };
                    return blockData;
                }

                const auto& matcher = *matcherData[ index ].matcher;
                const auto matchStartTime = high_resolution_clock::now();

                blockData->searchResults
                    = filterLines( matcher, blockData->lines, blockData->chunkStart );

                const auto matchEndTime = high_resolution_clock::now();

                matcherData[ index ].matchTime
                    += duration_cast<microseconds>( matchEndTime - matchStartTime );
                LOG_DEBUG << "Searcher " << index << " block " << blockData->chunkStart
                          << " sending matches "
                          << blockData->searchResults.matchingLines.cardinality();
                return blockData;
            } ) );
    }

    const auto totalLines = endLine - initialLine;
    LinesCount totalProcessedLines = 0_lcount;
    LineLength maxLength = 0_length;
    LinesCount nbMatches = searchData.getNbMatches();
    auto reportedMatches = nbMatches;
    int reportedPercentage = 0;

    std::chrono::microseconds matchCombiningDuration{ 0 };

    auto matchProcessor
        = tbb::flow::function_node<BlockDataType, tbb::flow::continue_msg, tbb::flow::rejecting>(
            searchGraph, 1, [ & ]( const BlockDataType& blockData ) {
                if ( interruptRequested_ ) {
                    LOG_INFO << "Match processor interrupted";
                    delete blockData;
                    return tbb::flow::continue_msg{};
                }

                const auto& matchResults = blockData->searchResults;

                const auto matchProcessorStartTime = high_resolution_clock::now();

                if ( matchResults.processedLines.get() ) {

                    maxLength = qMax( maxLength, matchResults.maxLength );
                    const LinesCount matchesCount
                        = LinesCount( matchResults.matchingLines.cardinality() );
                    nbMatches += matchesCount;

                    const auto processedLines = LinesCount{ matchResults.chunkStart.get()
                                                            + matchResults.processedLines.get() };

                    totalProcessedLines += matchResults.processedLines;

                    // After each block, copy the data to shared data
                    // and update the client
                    searchData.addAll( maxLength, matchResults.matchingLines, matchesCount,
                                       processedLines );

                    LOG_DEBUG << "done Searching chunk starting at " << matchResults.chunkStart
                              << ", " << matchResults.processedLines << " lines read.";
                }

                delete blockData;

                const auto matchProcessorEndTime = high_resolution_clock::now();
                matchCombiningDuration += duration_cast<microseconds>( matchProcessorEndTime
                                                                       - matchProcessorStartTime );
                const int percentage
                    = calculateProgress( totalProcessedLines.get(), totalLines.get() );

                if ( percentage > reportedPercentage || nbMatches > reportedMatches ) {

                    Q_EMIT searchProgressed( nbMatches, std::min( 99, percentage ), initialLine );

                    reportedPercentage = percentage;
                    reportedMatches = nbMatches;
                }

                return tbb::flow::continue_msg{};
            } );

    tbb::flow::make_edge( blockPrefetcher, lineBlocksQueue );

    for ( auto& matcherNode : matcherNodes ) {
        tbb::flow::make_edge( lineBlocksQueue, *matcherNode );
        tbb::flow::make_edge( *matcherNode, resultsQueue );
    }

    tbb::flow::make_edge( resultsQueue, matchProcessor );
    tbb::flow::make_edge( matchProcessor, blockPrefetcher.decrementer() );

    auto chunkStart = initialLine;
    while ( chunkStart < endLine && !interruptRequested_ ) {
        const auto lineSourceStartTime = high_resolution_clock::now();
        LOG_DEBUG << "Reading chunk starting at " << chunkStart;

        const auto linesInChunk
            = LinesCount( qMin( nbLinesInChunk.get(), ( endLine - chunkStart ).get() ) );
        auto lines = sourceLogData_.getLinesRaw( chunkStart, linesInChunk );

        /*LOG_DEBUG << "Sending chunk starting at " << chunkStart << ", " <<
            lines.second.size()
                << " lines read.";*/
        BlockDataType blockData = new SearchBlockData{ chunkStart, std::move( lines ) };

        const auto lineSourceEndTime = high_resolution_clock::now();
        const auto chunkReadTime
            = duration_cast<microseconds>( lineSourceEndTime - lineSourceStartTime );

        /*LOG_DEBUG << "Sent chunk starting at " << chunkStart << ", " <<
        blockData->lines.second.size()
                << " lines read in " << static_cast<float>( chunkReadTime.count() )
        / 1000.f
                << " ms";*/

        chunkStart = chunkStart + nbLinesInChunk;
        fileReadingDuration += chunkReadTime;

        bool pushed = false;
        while ( !pushed && !interruptRequested_ ) {
            pushed = blockPrefetcher.try_put( blockData );
            if ( !pushed ) {
                std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
            }
        }
        if ( !pushed ) {
            delete blockData;
        }
    }

    searchGraph.wait_for_all();

    high_resolution_clock::time_point t2 = high_resolution_clock::now();
    const auto durationUs = duration_cast<microseconds>( t2 - t1 );
    const auto durationMs = duration_cast<milliseconds>( t2 - t1 );

    LOG_INFO << "Searching done, overall duration " << durationUs;
    LOG_INFO << "Line reading took " << fileReadingDuration;
    LOG_INFO << "Results combining took " << matchCombiningDuration;

    for ( const auto& data : matcherData ) {
        LOG_INFO << "Matching took " << data.matchTime;
    }

    const auto totalFileSize = sourceLogData_.getFileSize();

    LOG_INFO << "Searching perf "
             << static_cast<uint64_t>(
                    std::floor( 1000.f * static_cast<float>( ( endLine - initialLine ).get() )
                                / static_cast<float>( durationMs.count() ) ) )
             << " lines/s";
    LOG_INFO << "Searching io perf "
             << ( 1000.f * static_cast<float>( totalFileSize )
                  / static_cast<float>( durationMs.count() ) )
                    / ( 1024 * 1024 )
             << " MiB/s";

    Q_EMIT searchProgressed( nbMatches, 100, initialLine );
    Q_EMIT searchFinished();
}

// Called in the worker thread's context
void FullSearchOperation::run( SearchData& searchData )
{
    try {
        // Clear the shared data
        searchData.clear();
        doSearch( searchData, 0_lnum );
    } catch ( const std::exception& err ) {
        const auto errorString = QString( "FullSearchOperation failed: %1" ).arg( err.what() );
        LOG_ERROR << errorString;
        dispatchToMainThread( [ errorString ]() {
            IssueReporter::askUserAndReportIssue( IssueTemplate::Exception, errorString );
        } );
        searchData.clear();
    }
}

// Called in the worker thread's context
void UpdateSearchOperation::run( SearchData& searchData )
{
    try {
        auto initialLine = qMax( searchData.getLastProcessedLine(), initialPosition_ );

        if ( initialLine.get() >= 1 ) {
            // We need to re-search the last line because it might have
            // been updated (if it was not LF-terminated)
            --initialLine;
            // In case the last line matched, we don't want it to match twice.
            searchData.deleteMatch( initialLine );
        }

        doSearch( searchData, initialLine );
    } catch ( const std::exception& err ) {
        const auto errorString = QString( "UpdateSearchOpertaion failed: %1" ).arg( err.what() );
        LOG_ERROR << errorString;
        dispatchToMainThread( [ errorString ]() {
            IssueReporter::askUserAndReportIssue( IssueTemplate::Exception, errorString );
        } );
        searchData.clear();
    }
}
