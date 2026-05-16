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

#ifndef LOGFILTEREDDATAWORKERTHREAD_H
#define LOGFILTEREDDATAWORKERTHREAD_H

#include <QObject>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <thread>

#ifndef Q_MOC_RUN
#include <roaring.hh>
#include <roaring64map.hh>
#include <tbb/task_group.h>
#endif

#include "atomicflag.h"
#include "linetypes.h"
#include "matchercache.h"
#include "regularexpression.h"
#include "synchronization.h"

class SearchableLogData;

// Class encapsulating a single matching line
// Contains the line number the line was found in and its content.
class MatchingLine {
public:
    MatchingLine( LineNumber line )
        : lineNumber_{ line }
    {
    }

    // Accessors
    LineNumber lineNumber() const
    {
        return lineNumber_;
    }

    bool operator<( const MatchingLine& other ) const
    {
        return lineNumber_ < other.lineNumber_;
    }

private:
    LineNumber lineNumber_;
};

// This is an array of matching lines.
// It shall be implemented for random lookup speed, so
// a fixed "in-place" array (vector) is probably fine.
using SearchResultArray = roaring::Roaring64Map;

struct SearchResults {
    SearchResultArray newMatches;
    LineLength maxLength;
    LinesCount processedLines;
};

struct SearchPerformanceCounters {
    quint64 operationStarts = 0;
    quint64 matcherCreations = 0;
    quint64 updateRequests = 0;
    quint64 coalescedLiveUpdates = 0;
};

// This class is a mutex protected set of search result data.
// It is thread safe.
class SearchData {
public:
    // will clear new matches
    SearchResults takeCurrentResults() const;

    // Atomically add to all the existing search data.
    void addAll( LineLength length, const SearchResultArray& matches, LinesCount nbMatches,
                 LinesCount nbLinesProcessed );
    // Get the number of matches
    LinesCount getNbMatches() const;
    // Get the last matched line number
    // That is "last" as in biggest, not latest
    // 0 if no matches have been found yet
    LineNumber getLastMatchedLineNumber() const;

    LineNumber getLastProcessedLine() const;

    // Delete the match for the passed line (if it exist)
    void deleteMatch( LineNumber line );

    // Atomically clear the data.
    void clear();

private:
    mutable SharedMutex dataMutex_;

    SearchResultArray matches_;
    mutable SearchResultArray newMatches_;
    LineLength maxLength_{ 0 };
    LinesCount nbLinesProcessed_{ 0 };
    LinesCount nbMatches_{ 0 };
    bool lastProcessedLineMatched_ = false;
};

class SearchOperation : public QObject {
    Q_OBJECT
public:
    SearchOperation( const SearchableLogData& sourceLogData, AtomicFlag& interruptRequested,
                     const RegularExpressionPattern& regExp, LineNumber startLine,
                     LineNumber endLine,
                     std::shared_ptr<RegularExpression> compiledRegExp = nullptr,
                     std::atomic<LineNumber::UnderlyingType>* liveTargetEndLine = nullptr,
                     std::atomic<quint64>* matcherCreations = nullptr,
                     MatcherCache* matcherCache = nullptr );

    // Run the search operation, returns true if it has been done
    // and false if it has been cancelled (results not copied)
    virtual void run( SearchData& result ) = 0;

Q_SIGNALS:
    // Operations are unaware of search generations -- the worker assigns and
    // forwards the generation when re-emitting these onto the owner thread.
    void searchProgressed( LinesCount nbMatches, int percent, LineNumber initialLine );
    void searchFinished();

protected:
    // Implement the common part of the search, passing
    // the shared results and the line to begin the search from.
    void doSearch( SearchData& result, LineNumber initialLine );

    AtomicFlag& interruptRequested_;
    const RegularExpressionPattern regexp_;
    const SearchableLogData& sourceLogData_;
    LineNumber startLine_;
    LineNumber endLine_;
    std::shared_ptr<RegularExpression> compiledRegExp_;
    std::atomic<LineNumber::UnderlyingType>* liveTargetEndLine_;
    std::atomic<quint64>* matcherCreations_;
    MatcherCache* matcherCache_;
};

class FullSearchOperation : public SearchOperation {
    Q_OBJECT
public:
    FullSearchOperation( const SearchableLogData& sourceLogData, AtomicFlag& interruptRequested,
                         const RegularExpressionPattern& regExp, LineNumber startLine,
                         LineNumber endLine,
                         std::shared_ptr<RegularExpression> compiledRegExp = nullptr,
                         std::atomic<quint64>* matcherCreations = nullptr,
                         MatcherCache* matcherCache = nullptr )
        : SearchOperation( sourceLogData, interruptRequested, regExp, startLine, endLine,
                           std::move( compiledRegExp ), nullptr, matcherCreations, matcherCache )
    {
    }

    void run( SearchData& result ) override;
};

class UpdateSearchOperation : public SearchOperation {
    Q_OBJECT
public:
    UpdateSearchOperation( const SearchableLogData& sourceLogData, AtomicFlag& interruptRequested,
                           const RegularExpressionPattern& regExp, LineNumber startLine,
                           LineNumber endLine, LineNumber position,
                           std::shared_ptr<RegularExpression> compiledRegExp = nullptr,
                           std::atomic<LineNumber::UnderlyingType>* liveTargetEndLine = nullptr,
                           std::atomic<quint64>* matcherCreations = nullptr,
                           MatcherCache* matcherCache = nullptr )
        : SearchOperation( sourceLogData, interruptRequested, regExp, startLine, endLine,
                           std::move( compiledRegExp ), liveTargetEndLine, matcherCreations, matcherCache )
        , initialPosition_( position )
    {
    }

    void run( SearchData& result ) override;

private:
    LineNumber initialPosition_;
};

class LogFilteredDataWorker : public QObject {
    Q_OBJECT

public:
    explicit LogFilteredDataWorker( const SearchableLogData& sourceLogData );
    ~LogFilteredDataWorker() noexcept override;

    LogFilteredDataWorker( const LogFilteredDataWorker& ) = delete;
    LogFilteredDataWorker& operator=( const LogFilteredDataWorker&& ) = delete;

    LogFilteredDataWorker( LogFilteredDataWorker&& ) = delete;
    LogFilteredDataWorker& operator=( LogFilteredDataWorker&& ) = delete;

    // Start the search with the passed regexp (non-blocking).
    // The actual search is started on an internal dispatch thread,
    // so the caller (typically the main/UI thread) is never blocked
    // waiting for a previous search to release the operations mutex.
    void search( const RegularExpressionPattern& regExp, LineNumber startLine, LineNumber endLine );
    // Continue the previous search starting at the passed position
    // in the source file (line number).  Also non-blocking.
    void updateSearch( const RegularExpressionPattern& regExp, LineNumber startLine,
                       LineNumber endLine, LineNumber position );

    // Interrupts the search if one is in progress
    void interrupt();

    // Blocks until all in-flight search operations have completed.
    // Must be called before any reader detach to prevent use-after-close.
    void waitForDone();

    // Cancels queued work, stops the dispatch thread, and blocks until any
    // running operation has completed.  Use during owner teardown before
    // detaching the reader; waitForDone() intentionally keeps dispatch alive
    // for normal progress-completion waits.
    void shutdownAndWait();

    // get the current indexing data
    SearchResults getSearchResults() const;

    // Alias kept for callers that prefer a self-documenting name; the wire
    // type used in signals is plain quint64 because moc / QSignalSpy treat
    // typedefs of non-builtin types as unregistered metatypes (the wire-side
    // QVariant decodes back to 0).  Built-in Qt types like quint64 round-trip
    // cleanly through queued connections without explicit qRegisterMetaType.
    using OperationGeneration = quint64;

    // Generation of the search currently active (most recently started via
    // search()).  updateSearch() keeps the same generation because it extends
    // the same logical search criteria.  Stale signals from superseded logical
    // searches carry an older generation and are filtered out by the worker
    // before re-emission; downstream consumers can additionally compare against
    // this value to drop signals that were already in their event queue when
    // the search was replaced.
    OperationGeneration currentGeneration() const { return operationGeneration_.load(); }

    SearchPerformanceCounters performanceCounters() const
    {
        return SearchPerformanceCounters{ operationStarts_.load(), matcherCreations_.load(),
                                          updateRequests_.load(),
                                          coalescedLiveUpdates_.load() };
    }

    // Advance the generation counter without launching a worker thread.
    // Used by LogFilteredData when a cached result is delivered without
    // touching the worker -- without this bump, queued progress signals
    // from a previous real search would still match the active generation
    // and corrupt the freshly-displayed cached state.  Returns the new
    // generation value.
    OperationGeneration bumpGeneration()
    {
        operationId_.fetch_add( 1 );
        return operationGeneration_.fetch_add( 1 ) + 1;
    }

Q_SIGNALS:
    // Sent during the indexing process to signal progress
    // percent being the percentage of completion.
    // The generation identifies which search() / updateSearch() call this
    // signal belongs to so receivers can drop stale queued signals from a
    // search that has since been replaced.
    void searchProgressed( LinesCount nbMatches, int percent, LineNumber initialLine,
                           quint64 generation );
    // Sent when indexing is finished, signals the client
    // to copy the new data back.
    void searchFinished();

private:
    using OperationId = quint64;

    void connectSignalsAndRun( SearchOperation* operationRequested, OperationGeneration generation,
                               OperationId operationId );
    void emitSearchProgressedOnOwnerThread( LinesCount nbMatches, int percent,
                                            LineNumber initialLine,
                                            OperationGeneration generation, OperationId operationId );
    void emitSearchFinishedOnOwnerThread( OperationGeneration generation, OperationId operationId );

    // Async dispatch ----------------------------------------------------------
    // search() / updateSearch() enqueue a SearchRequest and return immediately.
    // A dedicated dispatch thread waits for the operations mutex, joins the
    // previous search thread, and starts the new one — so the caller (UI
    // thread) never blocks on the mutex.
    struct SearchRequest {
        enum class Type { Full, Update, LiveUpdate } type;
        RegularExpressionPattern regExp;
        LineNumber startLine;
        LineNumber endLine;
        LineNumber position; // only for Update
        OperationGeneration generation;
        OperationId operationId;
        std::shared_ptr<RegularExpression> compiled;
    };

    void dispatchLoop();
    void enqueueRequest( SearchRequest request, bool interruptRunningSearch = true );
    void joinOperationThread();
    void finishLiveUpdateAndRestartIfNeeded( const SearchRequest& request );

    std::thread dispatchThread_;
    std::mutex requestMutex_;
    std::condition_variable requestCv_;
    std::optional<SearchRequest> pendingRequest_;
    bool dispatchShutdown_ = false;

private:
    // sourceLogData_, interruptRequested_, operationsMutex_, and searchData_ must all
    // outlive any running task (the task lambda captures them by reference).  opThread_
    // is declared last so its destructor runs first; but more importantly the destructor
    // body explicitly joins opThread_ before any other members are destroyed.
    const SearchableLogData& sourceLogData_;
    AtomicFlag interruptRequested_;
    Mutex operationsMutex_;

    // Shared indexing data (must outlive the task: passed by reference to SearchOperation::run)
    SearchData searchData_;

    std::atomic<OperationGeneration> operationGeneration_{ 0 };
    std::atomic<OperationId> operationId_{ 0 };
    std::atomic<bool> liveUpdateRunning_{ false };
    std::atomic<LineNumber::UnderlyingType> liveTargetEndLine_{ 0 };

    std::atomic<quint64> operationStarts_{ 0 };
    std::atomic<quint64> matcherCreations_{ 0 };
    std::atomic<quint64> updateRequests_{ 0 };
    std::atomic<quint64> coalescedLiveUpdates_{ 0 };

    MatcherCache matcherCache_;

    // Cached compiled regular expression to avoid recompilation on incremental
    // search updates.  The Vectorscan database compilation is expensive; caching
    // it here means UpdateSearchOperation reuses the compiled database.
    std::shared_ptr<RegularExpression> compiledExpression_;

    // Declared last so it is destroyed first.  The destructor body joins this thread
    // before other members are destroyed, guaranteeing the task has fully exited.
    std::mutex opThreadMutex_;
    std::thread opThread_;
};

#endif
