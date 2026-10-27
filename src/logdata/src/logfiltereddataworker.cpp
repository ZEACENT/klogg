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

#include <QtGlobal>
#include <chrono>
#include <cmath>
#include <exception>
#include <iostream>
#include <locale>
#include <limits>
#include <mutex>
#include <qsemaphore.h>
#include <sstream>
#include <utility>

#include <robin_hood.h>
#include <tbb/flow_graph.h>
#include <vector>

#include "configuration.h"
#include "linetypes.h"
#include "log.h"
#include "progress.h"

#include "regularexpression.h"
#include "searchablelogdata.h"

#include "logfiltereddataworker.h"
#include "synchronization.h"

namespace {
constexpr auto LiveUpdateMaxCoalesceDelay = std::chrono::milliseconds( 25 );
constexpr LinesCount::UnderlyingType LiveUpdateSingleThreadChunkMultiplier = 4;

bool traceSearchTerminals()
{
    static const bool enabled = qEnvironmentVariableIsSet( "KLOGG_TEST_TRACE_SEARCH_TERMINALS" );
    return enabled;
}

void traceSearchStart( const char* phase, quint64 generation, quint64 operationId, int type = -1 )
{
    static const bool enabled = qEnvironmentVariableIsSet( "KLOGG_TEST_TRACE_SEARCH_STARTS" );
    if ( !enabled ) {
        return;
    }

    try {
        std::ostringstream record;
        record.imbue( std::locale::classic() );
        record.exceptions( std::ios::badbit | std::ios::failbit );
        record << "Search start " << phase << " generation " << generation << " operation "
               << operationId << " type " << type << '\n';
        const auto text = record.str();
        const auto size = static_cast<std::streamsize>( text.size() );

        static std::mutex traceMutex;
        std::lock_guard<std::mutex> lock( traceMutex );
        auto* buffer = std::cerr.rdbuf();
        if ( !buffer ) {
            return;
        }
        if ( buffer->sputn( text.data(), size ) != size ) {
            return;
        }
        if ( buffer->pubsync() != 0 ) {
            return;
        }
    } catch ( ... ) {
        // Optional diagnostics must not interrupt the primary search request.
        return;
    }
}

// Declare after every flow node and captured local, so exceptional producer
// unwinding cannot destroy graph state while queued tasks still reference it.
class SearchGraphDrain {
public:
    explicit SearchGraphDrain( tbb::flow::graph& graph ) : graph_( &graph ) {}
    SearchGraphDrain( const SearchGraphDrain& ) = delete;
    SearchGraphDrain& operator=( const SearchGraphDrain& ) = delete;
    SearchGraphDrain( SearchGraphDrain&& ) = delete;
    SearchGraphDrain& operator=( SearchGraphDrain&& ) = delete;
    ~SearchGraphDrain()
    {
        if ( drained_ ) return;
        graph_->cancel();
        try { graph_->wait_for_all(); }
        // Preserve the producer's original exception during noexcept destruction.
        // NOLINTNEXTLINE(bugprone-empty-catch)
        catch ( ... ) {}
    }
    void wait()
    {
        graph_->wait_for_all();
        drained_ = true;
    }
private:
    tbb::flow::graph* graph_;
    bool drained_ = false;
};

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

    const auto& config = Configuration::get();

    // Normalize once for both backends. Plain valid UTF8 borrows its original
    // buffer; transformed/CRLF/invalid input keeps the same block-scan fast path
    // over a normalized LF-framed buffer with matching boundaries.
    const auto block = rawLines.buildUtf8Block();
    const auto& lines = block.lines;
    const bool blockScanEligible = config.useBlockScan() && matcher.hasBufferScan()
        && !block.buffer.empty() && !block.endOfLines.empty()
        && block.buffer.size() <= std::numeric_limits<unsigned int>::max();

    if ( blockScanEligible ) {
        klogg::vector<uint64_t> matched;
        matcher.scanBuffer( block.buffer.data(), static_cast<unsigned int>( block.buffer.size() ),
                            block.endOfLines, matched );
        for ( const auto matchedIdx : matched ) {
            if ( matchedIdx < lines.size() ) {
                results.maxLength
                    = qMax( results.maxLength, getUntabifiedLength( lines[ matchedIdx ] ) );
            }
            const auto lineNumber = chunkStart + LinesCount{ matchedIdx };
            results.matchingLines.add( lineNumber.get() );
        }
        return results;
    }

    // Per-line matching (fallback), over the exact same normalized records.
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
    return SearchResults{ std::exchange( newMatches_, {} ), maxLength_, nbLinesProcessed_, generation_ };
}

SearchResults SearchData::copyAllResults() const
{
    SharedLock lock( dataMutex_ );
    return SearchResults{ matches_, maxLength_, nbLinesProcessed_, generation_ };
}

void SearchData::beginGeneration( quint64 generation )
{
    UniqueLock lock( dataMutex_ );
    if ( generation_ == generation ) return;
    generation_ = generation;
    matches_ = {};
    newMatches_ = {};
    maxLength_ = 0_length;
    nbLinesProcessed_ = 0_lcount;
    nbMatches_ = 0_lcount;
    lastProcessedLineMatched_ = false;
}

void SearchData::addAll( LineLength length, const SearchResultArray& matches,
                         LinesCount matchedLines, LinesCount processedLines )
{
    UniqueLock lock( dataMutex_ );

    maxLength_ = qMax( maxLength_, length );
    if ( processedLines >= nbLinesProcessed_ ) {
        nbLinesProcessed_ = processedLines;
        const auto lastProcessedLine = processedLines.get() > 0 ? processedLines.get() - 1 : 0;
        lastProcessedLineMatched_ = processedLines.get() > 0
            && ( matches.contains( lastProcessedLine ) || matches_.contains( lastProcessedLine ) );
    }
    nbMatches_ += matchedLines;
    matches_ |= matches;
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
    if ( nbLinesProcessed_.get() > 0
         && line.get() == nbLinesProcessed_.get() - 1
         && lastProcessedLineMatched_ ) {
        lastProcessedLineMatched_ = false;
        newMatches_.remove( line.get() );
        matches_.remove( line.get() );
        if ( nbMatches_ > 0_lcount ) {
            nbMatches_ -= 1_lcount;
        }
    }
}

void SearchData::clear()
{
    UniqueLock locker( dataMutex_ );

    maxLength_ = LineLength( 0 );
    nbLinesProcessed_ = LinesCount( 0 );
    nbMatches_ = LinesCount( 0 );
    matches_ = {};
    newMatches_ = {};
    lastProcessedLineMatched_ = false;
}

LogFilteredDataWorker::LogFilteredDataWorker( const SearchableLogData& sourceLogData )
    : sourceLogData_( sourceLogData )
{
    qRegisterMetaType<LinesCount>( "LinesCount" );
    qRegisterMetaType<LineNumber>( "LineNumber" );
    dispatchThread_ = std::thread( [ this ] { dispatchLoop(); } );
}

LogFilteredDataWorker::~LogFilteredDataWorker() noexcept
{
    // Signal any running task to stop, then wait for the thread to fully exit
    // before any other members are destroyed.  std::thread::join() guarantees
    // the OS thread has completely exited -- no race with internal Qt thread-pool
    // cleanup (QMutex::lock / QThread::isRunning crashes seen in Qt 5.15/6.9).
    interruptRequested_.set();
    shutdownAndWait();
}

void LogFilteredDataWorker::connectSignalsAndRun( SearchOperation* operationRequested,
                                                  const SearchRequest& request )
{
    const auto generation = request.generation;
    const auto operationId = request.operationId;
    activeRequestTicket_.store( request.requestTicket );
    // SearchOperation is a short-lived QObject created on the std::thread. Using
    // a queued signal-to-signal hop into this worker can leave queued metacalls
    // targeting a connection object that gets torn down with the sender. Forward
    // directly here, but marshal actual LogFilteredDataWorker signal emission
    // back onto the owner's thread to avoid cross-thread QObject signal/disconnect
    // races during teardown.
    connect( operationRequested, &SearchOperation::searchProgressed, this,
             [ this, generation, operationId, operationRequested, &request ](
                 LinesCount nbMatches, int percent, LineNumber initialLine ) {
                 if ( percent == 100 && operationRequested->completed()
                      && !interruptRequested_ && generation == operationGeneration_.load()
                      && operationId == operationId_.load()
                      && operationId > cancelledThroughOperation_.load() ) {
                     auto results = searchData_.copyAllResults();
                     auto completed = std::make_shared<CompletedSearchResult>(
                         CompletedSearchResult{ std::move( results.newMatches ), results.maxLength,
                             results.processedLines, request.sourceSnapshot, generation,
                             operationId, request.requestTicket } );
                     std::lock_guard<std::mutex> lock( completedMutex_ );
                     if ( generation == operationGeneration_.load()
                          && operationId == operationId_.load()
                          && operationId > cancelledThroughOperation_.load() ) {
                         completedResult_ = std::move( completed );
                     }
                 }
                 if ( percent == 100 ) {
                     std::lock_guard<std::mutex> lock( requestMutex_ );
                     if ( generation == operationGeneration_.load()
                          && operationId == operationId_.load()
                          && request.requestTicket == requestedTicket_.load() ) {
                         requestPending_ = false;
                     }
                 }
                 emitSearchProgressedOnOwnerThread( nbMatches, percent, initialLine, generation,
                                                    operationId );
             },
             Qt::DirectConnection );
    connect( operationRequested, &SearchOperation::searchFinished, this,
             [ this, generation, operationId ] {
                 emitSearchFinishedOnOwnerThread( generation, operationId );
             },
             Qt::DirectConnection );

    traceSearchStart( "operation.run", generation, operationId );
    searchDataComplete_ = false;
    searchData_.beginGeneration( generation );
    operationRequested->run( searchData_ );
    searchDataComplete_ = operationRequested->completed() && !interruptRequested_;
    searchDataGeneration_ = generation;
}

void LogFilteredDataWorker::search( const RegularExpressionPattern& regExp, LineNumber startLine,
                                    LineNumber endLine,
                                    SearchableLogData::ReadSnapshot::Ptr sourceSnapshot )
{
    const auto generation = operationGeneration_.fetch_add( 1 ) + 1;
    const auto operationId = operationId_.fetch_add( 1 ) + 1;
    quint64 ticket;
    {
        std::lock_guard<std::mutex> lock( requestMutex_ );
        ticket = requestedTicket_.fetch_add( 1 ) + 1;
        requestPending_ = true;
    }
    try {
        if ( !sourceSnapshot ) {
            sourceSnapshot = sourceLogData_.searchableSnapshot();
        }
    } catch ( const klogg::SnapshotError& error ) {
        interruptRequested_.set();
        LOG_ERROR << "Cannot bind search snapshot: " << error.what();
        emitSearchProgressedOnOwnerThread( 0_lcount, 100, startLine, generation, operationId );
        return;
    }
    LOG_INFO << "Search requested (async dispatch, gen " << generation << ")";
    traceSearchStart( "compile.begin", generation, operationId );
    compiledExpression_ = std::make_shared<RegularExpression>( regExp );
    traceSearchStart( "compile.end", generation, operationId );

    // A new full search implicitly cancels any pending or coalesced live
    // update.  Without this reset, liveUpdateRunning_ would stay true and
    // subsequent updateSearch() calls would keep coalescing forever.
    liveUpdateRunning_.store( false );
    {
        std::lock_guard<std::mutex> lock( requestMutex_ );
        deferredLiveRequest_.reset();
        followupLiveRequest_.reset();
    }

    traceSearchStart( "enqueue.request", generation, operationId );
    enqueueRequest( SearchRequest{ SearchRequest::Type::Full, regExp, startLine, endLine, {},
                                   generation, operationId, compiledExpression_,
                                   std::move( sourceSnapshot ), ticket } );
}

SearchableLogData::ReadSnapshot::Ptr LogFilteredDataWorker::updateSearch(
                                          const RegularExpressionPattern& regExp,
                                          LineNumber startLine, LineNumber endLine,
                                          LineNumber position )
{
    updateRequests_.fetch_add( 1 );
    quint64 ticket;
    {
        std::lock_guard<std::mutex> lock( requestMutex_ );
        ticket = requestedTicket_.fetch_add( 1 ) + 1;
        requestPending_ = true;
    }
    SearchableLogData::ReadSnapshot::Ptr snapshot;
    try {
        snapshot = sourceLogData_.searchableSnapshot();
    } catch ( const klogg::SnapshotError& error ) {
        interrupt();
        LOG_ERROR << "Cannot bind incremental search snapshot: " << error.what();
        return {};
    }
    const auto boundSnapshot = snapshot;

    if ( sourceLogData_.isLiveSource() ) {
        liveTargetEndLine_.store( endLine.get() );
        {
            std::lock_guard<std::mutex> lock( requestMutex_ );
            if ( liveUpdateRunning_.load() ) {
                // Keep the active execution identity: its partial progress remains valid.
                // The ticket independently revokes readiness for the older fixed prefix.
                followupLiveRequest_ = SearchRequest{ SearchRequest::Type::LiveUpdate, regExp,
                    startLine, endLine, position, operationGeneration_.load(), 0,
                    compiledExpression_, std::move( snapshot ), ticket };
                coalescedLiveUpdates_.fetch_add( 1 );
                return boundSnapshot;
            }
            liveUpdateRunning_.store( true );
        }

        const auto generation = operationGeneration_.load();
        const auto operationId = operationId_.fetch_add( 1 ) + 1;
        LOG_INFO << "Live search update requested from " << position.get()
                 << " to " << endLine.get() << " (async dispatch, gen " << generation << ")";

        auto request = SearchRequest{ SearchRequest::Type::LiveUpdate, regExp, startLine, endLine,
                                      position, generation, operationId, compiledExpression_,
                                      std::move( snapshot ), ticket };
        const auto processedLine = searchData_.getLastProcessedLine();
        const auto pendingLines = endLine > processedLine ? ( endLine - processedLine ).get() : 0;
        if ( pendingLines < liveUpdateCoalesceThreshold() ) {
            liveUpdateRunning_.store( false );
            enqueueOrDeferLiveRequest( std::move( request ) );
        }
        else {
            enqueueRequest( std::move( request ), false );
        }
        return boundSnapshot;
    }

    // Signal any running search to stop at the next chunk boundary so that
    // operationsMutex_ is released quickly.
    interruptRequested_.set();

    // updateSearch extends an existing search (same pattern, expanded range).
    // It must NOT advance the generation counter.  In follow mode, repeated
    // file reloads trigger updateSearch calls; if each one bumped the
    // generation, completion signals from the in-flight search would carry
    // a now-stale generation and be dropped by isStaleSearchGeneration(),
    // causing matches to silently disappear from the filtered view.
    // Only runSearch() (new criteria) and bumpGeneration() (explicit
    // abandon-all-in-flight path in replaceCurrentSearch) advance the counter.
    const auto generation = operationGeneration_.load();
    const auto operationId = operationId_.fetch_add( 1 ) + 1;
    LOG_INFO << "Search update requested from " << position.get()
             << " (async dispatch, gen " << generation << ")";

    enqueueRequest( SearchRequest{ SearchRequest::Type::Update, regExp, startLine, endLine, position,
                                   generation, operationId, compiledExpression_,
                                   std::move( snapshot ), ticket } );
    return boundSnapshot;
}

void LogFilteredDataWorker::enqueueRequest( SearchRequest request, bool interruptRunningSearch )
{
    if ( interruptRunningSearch ) {
        interruptRequested_.set();
    }
    {
        std::lock_guard<std::mutex> lock( requestMutex_ );
        if ( request.type == SearchRequest::Type::LiveUpdate ) {
            deferredLiveRequest_.reset();
        }
        pendingRequest_.emplace( std::move( request ) );
    }
    requestCv_.notify_one();
}

void LogFilteredDataWorker::enqueueOrDeferLiveRequest( SearchRequest request )
{
    {
        std::lock_guard<std::mutex> lock( requestMutex_ );
        auto coalesceRequest = [ &request, this ]( SearchRequest& existing ) {
            existing.endLine = qMax( existing.endLine, request.endLine );
            existing.position = qMin( existing.position, request.position );
            existing.operationId = request.operationId;
            existing.compiled = request.compiled;
            existing.sourceSnapshot = request.sourceSnapshot;
            existing.requestTicket = request.requestTicket;
            coalescedLiveUpdates_.fetch_add( 1 );
        };

        if ( pendingRequest_ && pendingRequest_->type == SearchRequest::Type::LiveUpdate ) {
            coalesceRequest( *pendingRequest_ );
            return;
        }

        if ( deferredLiveRequest_ ) {
            coalesceRequest( *deferredLiveRequest_ );
        }
        else {
            deferredLiveRequest_.emplace( std::move( request ) );
            deferredLiveDeadline_
                = std::chrono::steady_clock::now() + LiveUpdateMaxCoalesceDelay;
        }
    }
    requestCv_.notify_one();
}

void LogFilteredDataWorker::dispatchLoop()
{
    while ( true ) {
        SearchRequest request;
        {
            std::unique_lock<std::mutex> lock( requestMutex_ );
            requestCv_.wait( lock, [ this ] {
                return pendingRequest_.has_value() || deferredLiveRequest_.has_value()
                    || dispatchShutdown_;
            } );
            if ( dispatchShutdown_ ) {
                pendingRequest_.reset();
                deferredLiveRequest_.reset();
                return;
            }
            while ( !pendingRequest_.has_value() && deferredLiveRequest_.has_value()
                    && !dispatchShutdown_ ) {
                const auto now = std::chrono::steady_clock::now();
                if ( now < deferredLiveDeadline_ ) {
                    requestCv_.wait_until( lock, deferredLiveDeadline_ );
                    continue;
                }
                pendingRequest_ = std::move( deferredLiveRequest_ );
                deferredLiveRequest_.reset();
            }
            if ( dispatchShutdown_ ) {
                pendingRequest_.reset();
                deferredLiveRequest_.reset();
                return;
            }
            if ( !pendingRequest_.has_value() ) {
                continue;
            }
            request = std::move( *pendingRequest_ );
            pendingRequest_.reset();
            if ( request.type == SearchRequest::Type::LiveUpdate ) {
                liveUpdateRunning_.store( true );
            }
        }

        traceSearchStart( "dispatch.selected", request.generation, request.operationId,
                          static_cast<int>( request.type ) );
        // Check if this request has been superseded by a newer one before
        // doing any work. Serialization with the previous operation is
        // provided by joining opThread_ below; the worker itself acquires
        // operationsMutex_ for the duration of its run.
        if ( request.generation != operationGeneration_.load()
             || request.operationId != operationId_.load() ) {
            continue;
        }

        joinOperationThread();
        // Stop may arrive while dispatch waits for the previous operation. Never
        // clear that cancellation and accidentally launch the stopped request.
        if ( request.generation != operationGeneration_.load()
             || request.operationId != operationId_.load()
             || request.operationId <= cancelledThroughOperation_.load() ) {
            continue;
        }
        interruptRequested_.clear();

        QSemaphore operationStarted;
        {
            std::lock_guard<std::mutex> opLock( opThreadMutex_ );
            opThread_ = std::thread( [ this, &operationStarted, request ] {
                operationStarted.release();
                ScopedLock operationLock( operationsMutex_ );
                if ( request.generation != operationGeneration_.load()
                     || request.operationId != operationId_.load()
                     || request.operationId <= cancelledThroughOperation_.load() ) {
                    return;
                }
                CompletedSearchResult::Ptr prefix;
                {
                    std::lock_guard<std::mutex> lock( completedMutex_ );
                    prefix = completedResult_;
                }
                const bool compatibleSource = request.sourceSnapshot
                    ? prefix && prefix->source
                          && request.sourceSnapshot->isAppendOf( *prefix->source )
                    : !prefix || !prefix->source;
                const bool incremental = request.type != SearchRequest::Type::Full
                    && searchDataComplete_ && searchDataGeneration_ == request.generation
                    && compatibleSource && prefix
                    && prefix->processedLines.get() <= request.endLine.get();
                operationStarts_.fetch_add( 1 );
                std::unique_ptr<SearchOperation> operation;
                if ( incremental ) {
                    operation = std::make_unique<UpdateSearchOperation>(
                        sourceLogData_, interruptRequested_, request.regExp, request.startLine,
                        request.endLine, request.position, request.compiled,
                        request.type == SearchRequest::Type::LiveUpdate ? &liveTargetEndLine_ : nullptr,
                        &matcherCreations_, &matcherCache_, request.sourceSnapshot );
                }
                else {
                    operation = std::make_unique<FullSearchOperation>(
                        sourceLogData_, interruptRequested_, request.regExp, request.startLine,
                        request.endLine, request.compiled, &matcherCreations_, &matcherCache_,
                        request.sourceSnapshot );
                }
                connectSignalsAndRun( operation.get(), request );
                if ( request.type == SearchRequest::Type::LiveUpdate ) {
                    finishLiveUpdateAndRestartIfNeeded( request );
                }
            } );
        }
        operationStarted.acquire();
    }
}

LinesCount::UnderlyingType LogFilteredDataWorker::liveUpdateCoalesceThreshold() const
{
    const auto& config = Configuration::get();
    const auto matchingThreadsCount = static_cast<LinesCount::UnderlyingType>( [ &config ]() {
        if ( !config.useParallelSearch() ) {
            return 1;
        }
        const auto configuredThreadPoolSize = config.searchThreadPoolSize();
        return qMax( 1, configuredThreadPoolSize == 0 ? tbb::info::default_concurrency()
                                                      : configuredThreadPoolSize );
    }() );

    return static_cast<LinesCount::UnderlyingType>( config.searchReadBufferSizeLines() )
         * matchingThreadsCount;
}

void LogFilteredDataWorker::promoteDeferredLiveRequest()
{
    std::lock_guard<std::mutex> lock( requestMutex_ );
    if ( deferredLiveRequest_ && !pendingRequest_ ) {
        pendingRequest_ = std::move( deferredLiveRequest_ );
        deferredLiveRequest_.reset();
    }
    requestCv_.notify_one();
}

void LogFilteredDataWorker::finishLiveUpdateAndRestartIfNeeded( const SearchRequest& request )
{
    std::lock_guard<std::mutex> lock( requestMutex_ );
    if ( request.generation != operationGeneration_.load()
         || request.operationId != operationId_.load() ) {
        return;
    }
    liveUpdateRunning_.store( false );
    if ( request.operationId <= cancelledThroughOperation_.load()
         || interruptRequested_ || !followupLiveRequest_ ) {
        return;
    }
    // Every request already owns its source version. This also catches a changed
    // unterminated tail whose row count is unchanged.
    auto next = std::move( *followupLiveRequest_ );
    followupLiveRequest_.reset();
    next.position = searchData_.getLastProcessedLine();
    next.operationId = operationId_.fetch_add( 1 ) + 1;
    pendingRequest_ = std::move( next );
    requestCv_.notify_one();
}

void LogFilteredDataWorker::interrupt()
{
    LOG_INFO << "Search interruption requested";
    OperationGeneration generation;
    OperationId operationId;
    {
        std::lock_guard<std::mutex> lock( requestMutex_ );
        if ( !requestPending_ ) return;
        requestPending_ = false;
        generation = operationGeneration_.load();
        operationId = operationId_.load();
        interruptRequested_.set();
        cancelledThroughOperation_.store( operationId );
        pendingRequest_.reset();
        deferredLiveRequest_.reset();
        followupLiveRequest_.reset();
        liveUpdateRunning_.store( false );
    }
    // The stopped execution, never a generation already retired by clearSearch,
    // owns this cleanup. Idle completed and cached results remain readable.
    emitSearchProgressedOnOwnerThread( searchData_.getNbMatches(), 100, 0_lnum,
                                      generation, operationId );
    // Intentionally NOT advancing the generation here.  A user pressing the
    // Stop button calls this path and still needs the in-flight progress
    // signal (typically progress < 100, while terminal cleanup is consumed by
    // CrawlerWidget::updateSearchStatus) to reach the receiver -- otherwise
    // the Stop-button UI cleanup never runs.  The replaceCurrentSearch
    // pathway, which DOES want stale signals dropped, calls
    // LogFilteredData::bumpSearchGeneration() explicitly.
}

void LogFilteredDataWorker::waitForDone()
{
    // Wait for the current operation thread to fully exit.  Unlike the
    // destructor, this does NOT shut down the dispatch thread — subsequent
    // updateSearch calls must still be processable after a search completes.
    // The dispatch thread is only shut down in the destructor.
    promoteDeferredLiveRequest();
    joinOperationThread();
}

void LogFilteredDataWorker::shutdownAndWait()
{
    {
        std::lock_guard<std::mutex> lock( requestMutex_ );
        dispatchShutdown_ = true;
        pendingRequest_.reset();
        deferredLiveRequest_.reset();
        followupLiveRequest_.reset();
    }
    requestCv_.notify_one();
    if ( dispatchThread_.joinable() ) {
        dispatchThread_.join();
    }

    joinOperationThread();
}

void LogFilteredDataWorker::joinOperationThread()
{
    std::lock_guard<std::mutex> lock( opThreadMutex_ );
    if ( opThread_.joinable() ) {
        opThread_.join();
    }
}

void LogFilteredDataWorker::emitSearchProgressedOnOwnerThread( LinesCount nbMatches, int percent,
                                                               LineNumber initialLine,
                                                               OperationGeneration generation,
                                                               OperationId operationId )
{
    // Use a named queued slot rather than a cross-thread functor. Qt owns and
    // copies these registered value arguments in its event queue; no short-lived
    // QCallableObject is concurrently destroyed while the owner thread invokes it.
    const auto queued = QMetaObject::invokeMethod( this, "deliverSearchProgressed",
                                                    Qt::QueuedConnection,
                                                    Q_ARG( LinesCount, nbMatches ),
                                                    Q_ARG( int, percent ),
                                                    Q_ARG( LineNumber, initialLine ),
                                                    Q_ARG( quint64, generation ),
                                                    Q_ARG( quint64, operationId ) );
    if ( percent == 100 && traceSearchTerminals() ) {
        LOG_WARNING << "Search terminal enqueue generation " << generation << " operation "
                 << operationId << " accepted " << queued;
    }
}

void LogFilteredDataWorker::emitSearchFinishedOnOwnerThread( OperationGeneration generation,
                                                             OperationId operationId )
{
    QMetaObject::invokeMethod( this, "deliverSearchFinished", Qt::QueuedConnection,
                               Q_ARG( quint64, generation ), Q_ARG( quint64, operationId ) );
}

void LogFilteredDataWorker::deliverSearchProgressed( LinesCount nbMatches, int percent,
                                                     LineNumber initialLine, quint64 generation,
                                                     quint64 operationId )
{
    if ( generation != operationGeneration_.load() || operationId != operationId_.load() ) {
        if ( percent == 100 && traceSearchTerminals() ) {
            LOG_WARNING << "Search terminal rejected generation " << generation << " operation "
                     << operationId << " current generation " << operationGeneration_.load()
                     << " current operation " << operationId_.load();
        }
        return;
    }

    // Search results are published under searchData_.dataMutex_ before 100% is
    // queued. Terminal progress means results are visible, not that opThread_
    // has already returned; teardown uses shutdownAndWait() for that contract.
    if ( percent == 100 && !interruptRequested_
         && operationId > cancelledThroughOperation_.load()
         && activeRequestTicket_.load() < requestedTicket_.load() ) {
        // A fixed prefix ended, but another bound version is waiting. Publish
        // its useful progress without declaring the logical catch-up terminal.
        percent = 99;
    }
    if ( percent == 100 ) {
        std::lock_guard<std::mutex> lock( requestMutex_ );
        if ( generation == operationGeneration_.load() && operationId == operationId_.load() ) {
            requestPending_ = false;
        }
    }
    Q_EMIT searchProgressed( nbMatches, percent, initialLine, generation );
    if ( percent == 100 && traceSearchTerminals() ) {
        LOG_WARNING << "Search terminal emitted by owner generation " << generation << " operation "
                 << operationId;
    }
}

void LogFilteredDataWorker::deliverSearchFinished( quint64 generation, quint64 operationId )
{
    if ( generation != operationGeneration_.load() || operationId != operationId_.load() ) {
        return;
    }
    Q_EMIT searchFinished();
}

// This will do an atomic copy of the object
SearchResults LogFilteredDataWorker::getSearchResults() const
{
    auto results = searchData_.takeCurrentResults();
    if ( results.generation != operationGeneration_.load() ) return {};
    return results;
}

CompletedSearchResult::Ptr LogFilteredDataWorker::completedSearchResult() const
{
    std::lock_guard<std::mutex> lock( completedMutex_ );
    if ( !completedResult_ || completedResult_->generation != operationGeneration_.load()
         || completedResult_->operationId != operationId_.load()
         || completedResult_->requestTicket != requestedTicket_.load()
         || completedResult_->operationId <= cancelledThroughOperation_.load() ) {
        return {};
    }
    return completedResult_;
}

void LogFilteredDataWorker::restoreCompletedResult( const CompletedSearchResult& result )
{
    auto restored = std::make_shared<CompletedSearchResult>( result );
    restored->generation = operationGeneration_.load();
    restored->operationId = operationId_.load();
    restored->requestTicket = requestedTicket_.load();
    std::lock_guard<std::mutex> lock( completedMutex_ );
    completedResult_ = std::move( restored );
}

//
// Operations implementation
//

SearchOperation::SearchOperation( const SearchableLogData& sourceLogData,
                                  AtomicFlag& interruptRequested,
                                  const RegularExpressionPattern& regExp, LineNumber startLine,
                                  LineNumber endLine,
                                  std::shared_ptr<RegularExpression> compiledRegExp,
                                  std::atomic<LineNumber::UnderlyingType>* liveTargetEndLine,
                                  std::atomic<quint64>* matcherCreations,
                                  MatcherCache* matcherCache,
                                  SearchableLogData::ReadSnapshot::Ptr sourceSnapshot )

    : interruptRequested_( interruptRequested )
    , regexp_( regExp )
    , sourceLogData_( sourceLogData )
    , startLine_( startLine )
    , endLine_( endLine )
    , compiledRegExp_( std::move( compiledRegExp ) )
    , liveTargetEndLine_( liveTargetEndLine )
    , matcherCreations_( matcherCreations )
    , matcherCache_( matcherCache )
    , sourceSnapshot_( std::move( sourceSnapshot ) )
{
}

LinesCount SearchOperation::sourceLineCount() const
{
    return sourceSnapshot_ ? sourceSnapshot_->lineCount() : sourceLogData_.getNbLine();
}

SearchableLogData::RawLines SearchOperation::readLines( LineNumber first, LinesCount count ) const
{
    auto lines = sourceSnapshot_ ? sourceSnapshot_->getLinesRaw( first, count )
                                 : sourceLogData_.getLinesRaw( first, count );
    if ( sourceSnapshot_ && lines.endOfLines.size() != count.get() ) {
        throw klogg::SnapshotError{ "Search snapshot returned an incomplete range" };
    }
    return lines;
}

void SearchOperation::doSearch( SearchData& searchData, LineNumber initialLine )
{
    const auto requestedEndLine = [ this ] {
        if ( liveTargetEndLine_ && !sourceSnapshot_ ) {
            return LineNumber( liveTargetEndLine_->load() );
        }
        return endLine_;
    };

    LOG_INFO << "Searching from line " << initialLine << " to " << requestedEndLine();

    using namespace std::chrono;
    high_resolution_clock::time_point t1 = high_resolution_clock::now();

    const auto& config = Configuration::get();
    const auto configuredChunkSize = config.searchReadBufferSizeLines();
    const auto matchingThreadsCount = static_cast<uint32_t>( [ &config ]() {
        if ( !config.useParallelSearch() ) {
            return 1;
        }
        const auto configuredThreadPoolSize = config.searchThreadPoolSize();
        return qMax( 1, configuredThreadPoolSize == 0 ? tbb::info::default_concurrency()
                                                      : configuredThreadPoolSize );
    }() );

    const auto cutoff = qMin( LineNumber( sourceLineCount().get() ), requestedEndLine() );
    initialLine = qMin( qMax( initialLine, startLine_ ), cutoff );
    const auto searchRange = static_cast<LinesCount::UnderlyingType>(
        qMin( LineNumber( sourceLineCount().get() ), requestedEndLine() ).get()
        - initialLine.get() );

    // For incremental searches, especially live-source updates, the TBB
    // flow-graph path is wasteful until the pending range is large enough:
    // graph setup, per-thread matcher creation, and per-chunk mutex combining
    // dominate the actual matching work.  Live updates arrive as many medium
    // ranges, so keep them on the pooled single-threaded path for several
    // chunks before paying the parallel setup cost.
    const auto singleThreadedThreshold = static_cast<LinesCount::UnderlyingType>(
        static_cast<LinesCount::UnderlyingType>( configuredChunkSize )
        * static_cast<LinesCount::UnderlyingType>(
            liveTargetEndLine_ ? matchingThreadsCount * LiveUpdateSingleThreadChunkMultiplier
                               : 1u ) );
    const bool useSingleThreaded
        = matchingThreadsCount == 1 || searchRange <= singleThreadedThreshold;

    LOG_INFO << "Using " << ( useSingleThreaded ? 1u : matchingThreadsCount ) << " matching threads"
             << " (range=" << searchRange << " chunk=" << configuredChunkSize << ")";

    if ( useSingleThreaded ) {
        auto endLine = qMin( LineNumber( sourceLineCount().get() ), requestedEndLine() );
        const auto nbLinesInChunk = LinesCount(
            static_cast<LinesCount::UnderlyingType>( configuredChunkSize ) );

        std::chrono::microseconds fileReadingDuration{ 0 };
        std::chrono::microseconds matchCombiningDuration{ 0 };
        std::chrono::microseconds matchDuration{ 0 };

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
#ifdef KLOGG_PERF_MEASURE_STREAMING
        const auto matcherT0 = std::chrono::steady_clock::now();
#endif
        auto matcher = matcherCache_
                           ? matcherCache_->acquire( compiledRegExp_ ? compiledRegExp_ : ownedExpression )
                           : regularExpression.createMatcher();
        if ( matcherCreations_ && !matcherCache_ ) {
            matcherCreations_->fetch_add( 1 );
        }
#ifdef KLOGG_PERF_MEASURE_STREAMING
        const auto matcherT1 = std::chrono::steady_clock::now();
        const auto matcherAcquireUs
            = std::chrono::duration_cast<std::chrono::microseconds>( matcherT1 - matcherT0 ).count();
        LOG_INFO << "PERF [search] matcher_acquire_us=" << matcherAcquireUs
                 << " pooled=" << ( matcherCache_ ? "yes" : "no" );
#endif

        auto chunkStart = initialLine;
        while ( !interruptRequested_ ) {
            // Only re-check the dynamic end line when we've caught up to the
            // current target.  This avoids a virtual call + mutex lock per chunk
            // for StreamingLogData when the search is still far behind.
            if ( chunkStart >= endLine ) {
                endLine = qMin( LineNumber( sourceLineCount().get() ), requestedEndLine() );
                if ( chunkStart >= endLine ) {
                    break;
                }
            }

            const auto lineSourceStartTime = high_resolution_clock::now();
            LOG_DEBUG << "Reading chunk starting at " << chunkStart;

            const auto linesInChunk
                = LinesCount( qMin( nbLinesInChunk.get(), ( endLine - chunkStart ).get() ) );
            auto lines = readLines( chunkStart, linesInChunk );

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

            const auto currentTotalLines = endLine - initialLine;
            const int percentage
                = calculateProgress( totalProcessedLines.get(), currentTotalLines.get() );
            if ( percentage > reportedPercentage || nbMatches > reportedMatches ) {
                Q_EMIT searchProgressed( nbMatches, std::min( 99, percentage ), initialLine );
                reportedPercentage = percentage;
                reportedMatches = nbMatches;
            }

            chunkStart = chunkStart + linesInChunk;
        }

        const auto t2 = high_resolution_clock::now();
        const auto durationUs = duration_cast<microseconds>( t2 - t1 );
        const auto durationSeconds = duration<double>( t2 - t1 ).count();

        LOG_INFO << "Searching done, overall duration " << durationUs;
        LOG_INFO << "Line reading took " << fileReadingDuration;
        LOG_INFO << "Results combining took " << matchCombiningDuration;
        LOG_INFO << "Matching took " << matchDuration;

        const auto totalFileSize = sourceSnapshot_ ? sourceSnapshot_->byteSize()
                                                  : sourceLogData_.getFileSize();
        const auto searchedLines = static_cast<double>( ( endLine - initialLine ).get() );
        const auto linesPerSecond = durationSeconds > 0.0 ? searchedLines / durationSeconds : 0.0;
        const auto mebibytesPerSecond
            = durationSeconds > 0.0
                  ? static_cast<double>( totalFileSize ) / durationSeconds / ( 1024.0 * 1024.0 )
                  : 0.0;
        LOG_INFO << "Searching perf " << linesPerSecond << " lines/s";
        LOG_INFO << "Searching io perf " << mebibytesPerSecond << " MiB/s";

        completed_ = !interruptRequested_ && chunkStart >= endLine;
        if ( completed_ ) {
            searchData.addAll( maxLength, {}, 0_lcount, LinesCount{ endLine.get() } );
        }
        Q_EMIT searchProgressed( nbMatches, 100, initialLine );
        if ( traceSearchTerminals() ) {
            LOG_WARNING << "Search operation terminal emitted";
        }
        Q_EMIT searchFinished();

        // Return the single-threaded matcher to the pool for reuse.
        if ( matcherCache_ ) {
            matcherCache_->release( std::move( matcher ) );
        }

        return;
    }

    tbb::flow::graph searchGraph;

    auto endLine = qMin( LineNumber( sourceLineCount().get() ), requestedEndLine() );
    const auto nbLinesInChunk = LinesCount(
        static_cast<LinesCount::UnderlyingType>( configuredChunkSize ) );

    std::chrono::microseconds fileReadingDuration{ 0 };

    // Flow nodes may discard queued messages on cancellation. Their payloads
    // therefore own blocks instead of relying on the final consumer's delete.
    using BlockDataType = std::shared_ptr<SearchBlockData>;
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
        if ( matcherCreations_ ) {
            matcherCreations_->fetch_add( 1 );
        }
        matcherData.push_back( { regularExpression.createMatcher(), microseconds{ 0 } } );
    }

    // resultsQueue must be declared BEFORE matcherNodes so that it is destroyed AFTER
    // matcherNodes.  C++ destroys locals in reverse declaration order; each matcherNode
    // deregisters itself from resultsQueue's predecessor list during destruction, so
    // resultsQueue must still be alive when that happens.
    auto resultsQueue = tbb::flow::buffer_node<BlockDataType>( searchGraph );

    std::mutex failureMutex;
    std::exception_ptr failure;
    std::atomic<bool> graphFailed{ false };
    const auto fail = [ & ]( std::exception_ptr error ) {
        {
            std::lock_guard<std::mutex> lock( failureMutex );
            if ( !failure ) failure = error;
        }
        graphFailed.store( true );
        searchGraph.cancel();
    };

    std::vector<std::unique_ptr<RegexMatcherNode>> matcherNodes;
    matcherNodes.reserve( matchingThreadsCount );
    for ( auto index = 0u; index < matchingThreadsCount; ++index ) {
        matcherNodes.push_back( std::make_unique<RegexMatcherNode>(
            searchGraph, 1,
            [ &matcherData, &fail, &graphFailed, index, this ]( const BlockDataType& blockData ) {
                if ( interruptRequested_ || graphFailed.load() ) return blockData;
                try {
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
                } catch ( ... ) {
                    fail( std::current_exception() );
                }
                return blockData;
            } ) );
    }

    LinesCount totalProcessedLines = 0_lcount;
    LineLength maxLength = 0_length;
    LinesCount nbMatches = searchData.getNbMatches();
    auto reportedMatches = nbMatches;
    int reportedPercentage = 0;

    std::chrono::microseconds matchCombiningDuration{ 0 };
    std::atomic<LineNumber::UnderlyingType> currentEndLine{ endLine.get() };

    auto matchProcessor
        = tbb::flow::function_node<BlockDataType, tbb::flow::continue_msg, tbb::flow::rejecting>(
            searchGraph, 1, [ & ]( const BlockDataType& blockData ) {
                if ( interruptRequested_ || graphFailed.load() ) return tbb::flow::continue_msg{};
                try {
                    const auto& matchResults = blockData->searchResults;
                    const auto matchProcessorStartTime = high_resolution_clock::now();
                    if ( matchResults.processedLines.get() ) {
                        maxLength = qMax( maxLength, matchResults.maxLength );
                        const auto matchesCount = LinesCount{ matchResults.matchingLines.cardinality() };
                        nbMatches += matchesCount;
                        const auto processedLines = LinesCount{ matchResults.chunkStart.get()
                                                                + matchResults.processedLines.get() };
                        totalProcessedLines += matchResults.processedLines;
                        searchData.addAll( maxLength, matchResults.matchingLines, matchesCount,
                                           processedLines );
                        LOG_DEBUG << "done Searching chunk starting at " << matchResults.chunkStart
                                  << ", " << matchResults.processedLines << " lines read.";
                    }
                    const auto matchProcessorEndTime = high_resolution_clock::now();
                    matchCombiningDuration += duration_cast<microseconds>(
                        matchProcessorEndTime - matchProcessorStartTime );
                    const auto currentTotalLines = LineNumber( currentEndLine.load() ) - initialLine;
                    const auto percentage = calculateProgress( totalProcessedLines.get(),
                                                               currentTotalLines.get() );
                    if ( percentage > reportedPercentage || nbMatches > reportedMatches ) {
                        Q_EMIT searchProgressed( nbMatches, std::min( 99, percentage ), initialLine );
                        reportedPercentage = percentage;
                        reportedMatches = nbMatches;
                    }
                } catch ( ... ) {
                    fail( std::current_exception() );
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

    SearchGraphDrain drain{ searchGraph };
    auto chunkStart = initialLine;
    while ( !interruptRequested_ && !graphFailed.load() ) {
        if ( chunkStart >= endLine ) {
            endLine = qMin( LineNumber( sourceLineCount().get() ), requestedEndLine() );
            currentEndLine.store( endLine.get() );
            if ( chunkStart >= endLine ) {
                break;
            }
        }

        const auto lineSourceStartTime = high_resolution_clock::now();
        LOG_DEBUG << "Reading chunk starting at " << chunkStart;

        const auto linesInChunk
            = LinesCount( qMin( nbLinesInChunk.get(), ( endLine - chunkStart ).get() ) );
        auto lines = readLines( chunkStart, linesInChunk );

        /*LOG_DEBUG << "Sending chunk starting at " << chunkStart << ", " <<
            lines.second.size()
                << " lines read.";*/
        auto blockData = std::make_shared<SearchBlockData>( chunkStart, std::move( lines ) );

        const auto lineSourceEndTime = high_resolution_clock::now();
        const auto chunkReadTime
            = duration_cast<microseconds>( lineSourceEndTime - lineSourceStartTime );

        /*LOG_DEBUG << "Sent chunk starting at " << chunkStart << ", " <<
        blockData->lines.second.size()
                << " lines read in " << static_cast<float>( chunkReadTime.count() )
        / 1000.f
                << " ms";*/

        chunkStart = chunkStart + linesInChunk;
        fileReadingDuration += chunkReadTime;

        bool pushed = false;
        while ( !pushed && !interruptRequested_ && !graphFailed.load() ) {
            pushed = blockPrefetcher.try_put( blockData );
            if ( !pushed ) {
                std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
            }
        }
    }

    drain.wait();
    {
        std::lock_guard<std::mutex> lock( failureMutex );
        if ( failure ) std::rethrow_exception( failure );
    }

    high_resolution_clock::time_point t2 = high_resolution_clock::now();
    const auto durationUs = duration_cast<microseconds>( t2 - t1 );
    const auto durationMs = duration_cast<milliseconds>( t2 - t1 );

    LOG_INFO << "Searching done, overall duration " << durationUs;
    LOG_INFO << "Line reading took " << fileReadingDuration;
    LOG_INFO << "Results combining took " << matchCombiningDuration;

    for ( const auto& data : matcherData ) {
        LOG_INFO << "Matching took " << data.matchTime;
    }

    const auto totalFileSize = sourceSnapshot_ ? sourceSnapshot_->byteSize()
                                                  : sourceLogData_.getFileSize();

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

    completed_ = !interruptRequested_ && chunkStart >= endLine;
    if ( completed_ ) {
        searchData.addAll( maxLength, {}, 0_lcount, LinesCount{ endLine.get() } );
    }
    Q_EMIT searchProgressed( nbMatches, 100, initialLine );
    if ( traceSearchTerminals() ) {
        LOG_WARNING << "Search operation terminal emitted";
    }
    Q_EMIT searchFinished();
}

void SearchOperation::failSearch( SearchData& searchData, LineNumber initialLine, const char* message )
{
    completed_ = false;
    searchData.clear();
    LOG_ERROR << "Search operation failed: " << message;
    // Resource/read/normalization failures are terminal data errors, not modal
    // issue-report prompts from a background operation.
    Q_EMIT searchProgressed( 0_lcount, 100, initialLine );
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
        failSearch( searchData, startLine_, err.what() );
    } catch ( ... ) {
        failSearch( searchData, startLine_, "Unknown search failure" );
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
        failSearch( searchData, initialPosition_, err.what() );
    } catch ( ... ) {
        failSearch( searchData, initialPosition_, "Unknown incremental search failure" );
    }
}
