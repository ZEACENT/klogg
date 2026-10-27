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
 * but WITHOUT ANY WARRANTY; even without implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with klogg.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "foldersearchengine.h"

#include <QFile>
#include <QTextCodec>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <exception>
#include <memory>
#include <string_view>
#include <utility>

#include "configuration.h"
#include "encodingdetector.h"
#include "encodingutils.h"
#include "foldersearchtypes.h"
#include "linetypes.h"
#include "log.h"
#include "regularexpression.h"
#include "searchablelogdata.h"

namespace {

// Whole-file vectorscan fast path is used only for files up to this size so a
// folder of many large logs cannot blow up memory; bigger files stream through
// the per-line path below.
constexpr qint64 FastPathMaxBytes = 16LL * 1024 * 1024; // 16 MiB
constexpr int MaxDetachedEnumerations = 1;
constexpr auto CooperativeEnumerationStopGrace = std::chrono::milliseconds( 100 );

// True if `shouldStop` is set and reports stop. A null predicate never stops.
bool stopped( const std::function<bool()>& shouldStop )
{
    return shouldStop && shouldStop();
}

struct FolderEnumerationState {
    std::mutex mutex;
    std::condition_variable changed;
    std::atomic<bool> canceled{ false };
    std::shared_ptr<std::atomic<int>> detachedCount;
    bool done{ false };
    bool detached{ false };
    QStringList filePaths;
    std::exception_ptr failure;
};

} // namespace

FolderSearchEngine::FolderSearchEngine( QObject* parent )
    : FolderSearchEngine( FolderEnumerator{}, parent )
{
}

FolderSearchEngine::FolderSearchEngine( FolderEnumerator folderEnumerator, QObject* parent )
    : QObject( parent )
    , folderEnumerator_( std::move( folderEnumerator ) )
{
    workerThread_ = std::thread( [ this ] { workerLoop(); } );
}

FolderSearchEngine::~FolderSearchEngine()
{
    {
        std::lock_guard<std::mutex> lock( requestMutex_ );
        shutdown_ = true;
        pendingRequest_.valid = false;
    }
    interruptRequested_ = true;
    requestCv_.notify_all();
    if ( workerThread_.joinable() ) {
        workerThread_.join();
    }
}

quint64 FolderSearchEngine::submit( Request request )
{
    request.generation = bumpGeneration();
    request.valid = true;
    interruptRequested_ = false;
    const auto generation = request.generation;
    {
        std::lock_guard<std::mutex> lock( requestMutex_ );
        pendingRequest_ = std::move( request );
    }
    requestCv_.notify_one();
    return generation;
}

quint64 FolderSearchEngine::startSearch( const QStringList& filePaths,
                                         const RegularExpressionPattern& pattern,
                                         klogg::folder::ContextOptions context )
{
    Request request;
    request.source = RequestSource::FilePaths;
    request.filePaths = filePaths;
    request.pattern = pattern;
    request.context = context;
    return submit( std::move( request ) );
}

quint64 FolderSearchEngine::startFolderSearch(
    const QString& folderPath, const RegularExpressionPattern& pattern,
    klogg::folder::ContextOptions context )
{
    Request request;
    request.source = RequestSource::Folder;
    request.folderPath = folderPath;
    request.pattern = pattern;
    request.context = context;
    return submit( std::move( request ) );
}

quint64 FolderSearchEngine::scanSynchronously( const QStringList& filePaths,
                                               const RegularExpressionPattern& pattern,
                                               klogg::folder::ContextOptions context )
{
    const quint64 gen = bumpGeneration();
    interruptRequested_ = false;
    runSearch( gen, filePaths, pattern, context );
    return gen;
}

quint64 FolderSearchEngine::bumpGeneration()
{
    const auto generation = generation_.fetch_add( 1, std::memory_order_relaxed ) + 1;
    const std::lock_guard<std::mutex> lock( resultsMutex_ );
    // Keep the slots allocated while old workers unwind, but release all
    // unclaimed leases. Publication checks the generation under this same lock.
    for ( auto& group : pending_ ) {
        group.reset();
    }
    return generation;
}

void FolderSearchEngine::interrupt()
{
    interruptRequested_ = true;
    std::lock_guard<std::mutex> lock( requestMutex_ );
    pendingRequest_.valid = false;
}

std::vector<klogg::folder::FileGroup> FolderSearchEngine::takeResults()
{
    std::lock_guard<std::mutex> lock( resultsMutex_ );
    std::vector<klogg::folder::FileGroup> out;
    for ( auto& group : pending_ ) {
        if ( group && ( !group->matches.empty() || !group->retentionError.isEmpty() ) ) {
            out.push_back( std::move( *group ) );
        }
        group.reset();
    }
    return out;
}

std::optional<klogg::folder::FileGroup> FolderSearchEngine::takeCompletedGroup( int fileIndex )
{
    std::lock_guard<std::mutex> lock( resultsMutex_ );
    if ( fileIndex < 0 || static_cast<size_t>( fileIndex ) >= pending_.size() ) {
        return std::nullopt;
    }
    return std::exchange( pending_[ static_cast<size_t>( fileIndex ) ], std::nullopt );
}

void FolderSearchEngine::runSearch( quint64 gen, const QStringList& filePaths,
                                    const RegularExpressionPattern& pattern,
                                    klogg::folder::ContextOptions context )
{
    // A queued request whose generation is no longer current must do nothing.
    if ( generation_.load( std::memory_order_relaxed ) != gen ) {
        Q_EMIT searchFinished( gen );
        return;
    }

    // The compiled expression is constructed ONCE on this thread. Its
    // createMatcher() clones the shared HsDatabase (shared_ptr refcount, safe)
    // but also prototypes a private HsScratch via hs_clone_scratch, whose read
    // of the shared scratch prototype races under TSan when run concurrently.
    // The per-worker matchers are therefore built SERIALLY on this thread
    // before the pool is spawned (workerMatchers below) and each worker is
    // handed its own pre-built matcher. Concurrent hasMatch() on distinct
    // matchers is safe.
    RegularExpression expression( pattern );
    if ( !expression.isValid() ) {
        LOG_WARNING << "FolderSearchEngine: invalid pattern" << pattern.pattern;
        Q_EMIT searchFinished( gen );
        return;
    }

    const int total = static_cast<int>( filePaths.size() );

    {
        std::lock_guard<std::mutex> lock( resultsMutex_ );
        pending_.assign( static_cast<size_t>( std::max( 0, total ) ), std::nullopt );
    }
    matchCount_ = 0;

    Q_EMIT searchStarted( gen );

    // Bounded pool of bare std::threads, joined before searchFinished. The
    // engine already owns a coordinator std::thread (workerThread_) which runs
    // runSearch; spawning additional threads here and joining them keeps the
    // existing destructor/join flow intact (no deadlock, no orphan threads).
    std::atomic<int> nextFileIndex{ 0 };
    std::atomic<int> completedCount{ 0 };

    const auto shouldStop = [ this, gen ]() {
        return generation_.load( std::memory_order_relaxed ) != gen
               || interruptRequested_.load( std::memory_order_relaxed );
    };

    const auto arena = std::make_shared<klogg::folder::ScanArena>();
    auto worker = [ this, &filePaths, total, gen, context, &nextFileIndex, &completedCount,
                    &shouldStop, &arena ]( std::unique_ptr<PatternMatcher> matcher ) {
        // Each worker owns its own PatternMatcher (private vectorscan scratch +
        // matcher context), pre-built serially by the caller and reused across
        // every file it processes. Concurrent hasMatch() on distinct matchers
        // is safe.
        while ( true ) {
            const int i = nextFileIndex.fetch_add( 1, std::memory_order_relaxed );
            if ( i >= total ) {
                break;
            }
            if ( shouldStop() ) {
                break;
            }

            auto group = scanFile( filePaths[ i ], *matcher, DefaultBlockSize, shouldStop, context, arena );

            if ( !group.matches.empty() ) {
                // Count ONLY real matches: with -A/-B/-C the vector also holds
                // Context rows that must not inflate the status/header count.
                const auto trueMatches = std::count_if(
                    group.matches.begin(), group.matches.end(),
                    []( const klogg::folder::MatchRecord& record ) {
                        return record.role == klogg::folder::RecordRole::Match;
                    } );
                matchCount_.fetch_add( static_cast<quint64>( trueMatches ),
                                       std::memory_order_relaxed );
            }
            {
                std::lock_guard<std::mutex> lock( resultsMutex_ );
                if ( generation_.load( std::memory_order_relaxed ) != gen ) {
                    break;
                }
                pending_[ static_cast<size_t>( i ) ] = std::move( group );
            }
            Q_EMIT fileGroupReady( i, gen );

            const int done = completedCount.fetch_add( 1, std::memory_order_relaxed ) + 1;
            const int percent = total > 0 ? done * 100 / total : 100;
            Q_EMIT searchProgressed( matchCount_.load( std::memory_order_relaxed ), percent, gen );
        }
    };

    unsigned int hwThreads = std::thread::hardware_concurrency();
    if ( hwThreads == 0 ) {
        hwThreads = 4;
    }
    hwThreads = std::min<unsigned int>( hwThreads, 16u );
    int poolSize = static_cast<int>( std::min<unsigned int>( hwThreads, static_cast<unsigned int>( std::max( 0, total ) ) ) );
    if ( poolSize < 1 ) {
        poolSize = 1;
    }

    // Build one matcher per worker SERIALLY on this thread before spawning
    // the pool. createMatcher() on a shared const RegularExpression races under
    // TSan (it prototypes the shared hs scratch via hs_clone_scratch, and the
    // PatternMatcher ctor likewise clones blockScratch_ for the buffer scanner),
    // so it must not run concurrently. Each matcher is then handed exclusively
    // to one worker, preserving the "each worker owns its matcher" design.
    std::vector<std::unique_ptr<PatternMatcher>> workerMatchers;
    workerMatchers.reserve( static_cast<size_t>( poolSize ) );
    for ( int wi = 0; wi < poolSize; ++wi ) {
        workerMatchers.push_back( expression.createMatcher() );
    }

    if ( poolSize <= 1 ) {
        // Single-file / single-core path: run inline to avoid thread overhead.
        worker( std::move( workerMatchers[ 0 ] ) );
    }
    else {
        std::vector<std::thread> pool;
        pool.reserve( static_cast<size_t>( poolSize ) );
        for ( int t = 0; t < poolSize; ++t ) {
            pool.emplace_back( worker, std::move( workerMatchers[ static_cast<size_t>( t ) ] ) );
        }
        for ( auto& thread : pool ) {
            if ( thread.joinable() ) {
                thread.join();
            }
        }
    }

    Q_EMIT searchFinished( gen );
}

void FolderSearchEngine::runFolderSearch( const Request& request )
{
    const auto requestStopped = [ this, generation = request.generation ]() {
        return shutdown_.load( std::memory_order_relaxed )
               || generation_.load( std::memory_order_relaxed ) != generation
               || interruptRequested_.load( std::memory_order_relaxed );
    };
    if ( requestStopped() ) {
        Q_EMIT searchFinished( request.generation );
        return;
    }

    matchCount_ = 0;
    {
        std::lock_guard<std::mutex> lock( resultsMutex_ );
        pending_.clear();
    }

    const RegularExpression validation( request.pattern );
    if ( !validation.isValid() || !folderEnumerator_ ) {
        LOG_WARNING << "FolderSearchEngine: folder request has no valid enumerator or pattern";
        Q_EMIT searchFinished( request.generation );
        return;
    }

    auto state = std::make_shared<FolderEnumerationState>();
    state->detachedCount = detachedEnumerationCount_;
    const auto enumerator = folderEnumerator_;
    const auto folderPath = request.folderPath;
    std::thread enumerationThread;
    try {
        enumerationThread = std::thread( [ state, enumerator, folderPath ] {
            try {
                auto filePaths = enumerator( folderPath, [ state ] {
                    return state->canceled.load( std::memory_order_relaxed );
                } );
                std::lock_guard<std::mutex> lock( state->mutex );
                state->filePaths = std::move( filePaths );
            } catch ( ... ) {
                std::lock_guard<std::mutex> lock( state->mutex );
                state->failure = std::current_exception();
            }
            {
                std::lock_guard<std::mutex> lock( state->mutex );
                state->done = true;
                if ( state->detached ) {
                    state->detachedCount->fetch_sub( 1, std::memory_order_relaxed );
                }
            }
            state->changed.notify_all();
        } );
    } catch ( const std::exception& error ) {
        LOG_WARNING << "FolderSearchEngine: failed to start folder enumeration: " << error.what();
        Q_EMIT searchFinished( request.generation );
        return;
    }

    QStringList filePaths;
    std::exception_ptr enumerationFailure;
    bool cancellationRequested = false;
    while ( true ) {
        {
            std::unique_lock<std::mutex> lock( state->mutex );
            state->changed.wait_for( lock, std::chrono::milliseconds( 25 ),
                                     [ &state ] { return state->done; } );
            if ( state->done ) {
                filePaths = std::move( state->filePaths );
                enumerationFailure = state->failure;
                break;
            }
        }

        if ( requestStopped() ) {
            state->canceled = true;
            if ( !cancellationRequested ) {
                cancellationRequested = true;
                std::unique_lock<std::mutex> lock( state->mutex );
                state->changed.wait_for( lock, CooperativeEnumerationStopGrace,
                                         [ &state ] { return state->done; } );
                if ( state->done ) {
                    break;
                }
            }

            // Permit one quarantined filesystem call so a latest request can
            // bypass a stalled mount, but bound ordinary supersession so repeated
            // searches cannot create an unbounded detached-thread pileup. Shutdown
            // may force one additional detach for the active coordinator request.
            int detached = detachedEnumerationCount_->load( std::memory_order_relaxed );
            while ( detached < MaxDetachedEnumerations
                    && !detachedEnumerationCount_->compare_exchange_weak(
                        detached, detached + 1, std::memory_order_relaxed ) ) {
            }
            bool reservedDetach = detached < MaxDetachedEnumerations;
            if ( !reservedDetach && shutdown_.load( std::memory_order_relaxed ) ) {
                detachedEnumerationCount_->fetch_add( 1, std::memory_order_relaxed );
                reservedDetach = true;
            }
            if ( reservedDetach ) {
                {
                    std::lock_guard<std::mutex> lock( state->mutex );
                    if ( state->done ) {
                        detachedEnumerationCount_->fetch_sub( 1,
                                                              std::memory_order_relaxed );
                        break;
                    }
                    state->detached = true;
                }
                enumerationThread.detach();
                Q_EMIT searchFinished( request.generation );
                return;
            }
        }
    }
    enumerationThread.join();

    if ( requestStopped() ) {
        Q_EMIT searchFinished( request.generation );
        return;
    }
    if ( enumerationFailure != nullptr ) {
        try {
            std::rethrow_exception( enumerationFailure );
        } catch ( const std::exception& error ) {
            LOG_WARNING << "FolderSearchEngine: folder enumeration failed: " << error.what();
        } catch ( ... ) {
            LOG_WARNING << "FolderSearchEngine: folder enumeration failed";
        }
        Q_EMIT searchFinished( request.generation );
        return;
    }

    Q_EMIT folderSnapshotReady( filePaths, request.generation );
    runSearch( request.generation, filePaths, request.pattern, request.context );
}

void FolderSearchEngine::workerLoop()
{
    while ( true ) {
        Request req;
        {
            std::unique_lock<std::mutex> lock( requestMutex_ );
            requestCv_.wait( lock, [ this ]() {
                return shutdown_.load( std::memory_order_relaxed ) || pendingRequest_.valid;
            } );
            if ( shutdown_.load( std::memory_order_relaxed ) ) {
                return;
            }
            req = std::move( pendingRequest_ );
            pendingRequest_.valid = false;
        }
        if ( req.source == RequestSource::Folder ) {
            runFolderSearch( req );
        }
        else {
            runSearch( req.generation, req.filePaths, req.pattern, req.context );
        }
    }
}

klogg::folder::FileGroup FolderSearchEngine::scanFile( const QString& path,
                                                       const PatternMatcher& matcher,
                                                       qint64 blockSize,
                                                       std::function<bool()> shouldStop,
                                                       klogg::folder::ContextOptions context,
                                                       std::shared_ptr<klogg::folder::ScanArena> arena )
{
    klogg::folder::FileGroup group;
    group.filePath = path;
    try {
        if ( !arena ) {
            arena = std::make_shared<klogg::folder::ScanArena>();
        }
        const auto member = arena->nextMember();
        std::vector<klogg::folder::ScanSpan> spans;
        std::vector<klogg::folder::ScanSpan> rowSpans;
        std::vector<qint64> pendingLineEnds;
        uint64_t flushedLines = 0;
        qint64 retainedSize = 0;
        LinesCount retainedLines = 0_lcount;
        const auto flushLineEnds = [&]() {
            if ( !pendingLineEnds.empty() ) {
                arena->appendLineEnds( rowSpans, flushedLines, pendingLineEnds.data(),
                                       pendingLineEnds.size() );
                flushedLines += static_cast<uint64_t>( pendingLineEnds.size() );
                pendingLineEnds.clear();
            }
        };
        const auto retainLineEnd = [&]( qint64 end ) {
            if ( pendingLineEnds.capacity() == 0 ) {
                pendingLineEnds.reserve( klogg::folder::ScanArena::BoundaryBatchLines );
            }
            pendingLineEnds.push_back( end );
            if ( pendingLineEnds.size() == klogg::folder::ScanArena::BoundaryBatchLines ) {
                flushLineEnds();
            }
        };
        const auto retain = [&]( const QByteArray& bytes ) {
            arena->append( spans, retainedSize, bytes );
            retainedSize += bytes.size();
        };
        const auto finish = [&]() {
            flushLineEnds();
            group.content = arena->seal( std::move( spans ), retainedSize, member,
                                        group.sourceCodec ? group.sourceCodec->name()
                                                          : QByteArrayLiteral( "UTF-8" ),
                                        retainedLines, std::move( rowSpans ) );
            return std::move( group );
        };

        if ( blockSize <= 0 ) {
            blockSize = DefaultBlockSize;
        }

        QFile file( path );
        if ( !file.open( QIODevice::ReadOnly | QIODevice::Unbuffered ) ) {
            return group; // unreadable -> no matches
        }

        // === Whole-file vectorscan fast path ===
        // For a single (non-boolean/non-inverse) regex on a UTF-8 file that fits in
        // memory and needs no context (-A/-B/-C) capture, scan the entire file with
        // one vectorscan pass (PatternMatcher::scanBuffer) instead of a hasMatch
        // call per line. This is the same block-scan technique the single-file
        // (LogFilteredData) path uses, and lifts folder search throughput to
        // single-file parity -- competitive with / faster than ripgrep. The streaming
        // per-line scan below remains the fallback for multi-byte encodings,
        // boolean/inverse patterns, context capture, or files over the cap.
        if ( Configuration::get().useBlockScan() && matcher.hasBufferScan()
             && context.before == 0 && context.after == 0 ) {
            const qint64 fastFileSize = file.size();
            if ( fastFileSize > 0 && fastFileSize <= FastPathMaxBytes ) {
                // Honor a mid-scan stop request BEFORE the readAll + memchr + scan
                // pass (which is byte-bounded by FastPathMaxBytes but wall-time-
                // unbounded on slow/network mounts). The streaming per-line path and
                // runSearch's between-file loop already check stopped(); without this
                // gate the fast path would still scan the whole file. Returning the
                // empty per-file group drops this file; the caller's between-file
                // check then halts the loop.
                if ( stopped( shouldStop ) ) {
                    return group;
                }
                QByteArray whole = file.readAll();
                if ( whole.size() == static_cast<int>( fastFileSize ) ) {
                    const char* const wdata = whole.constData();
                    const qint64 wbytes = whole.size();
                    const qint64 detectLen = std::min<qint64>( wbytes, BinaryDetectionWindow );
                    // Binary short-circuit BEFORE any detection work (grep -I): a
                    // BOM-less sample containing NULs is binary -- skip it without
                    // paying for uchardet. BOM-bearing UTF-16/32 files still take
                    // the full detection path below (their NULs are legitimate).
                    const std::string_view sampleView( wdata,
                                                       static_cast<size_t>( detectLen ) );
                    if ( !klogg::encoding::startsWithUnicodeBom( sampleView )
                         && sampleView.find( '\0' ) != std::string_view::npos ) {
                        return group;
                    }
                    const klogg::vector<char> firstVec( wdata, wdata + detectLen );
                    QTextCodec* fastCodec
                        = EncodingDetector::getInstance().detectEncoding( firstVec );
                    EncodingParameters fastParams;
                    if ( fastCodec != nullptr ) {
                        fastParams = EncodingParameters( fastCodec );
                        group.sourceCodec = fastCodec;
                    }
                    const bool utf8SingleByte
                        = fastParams.isUtf8Compatible && fastParams.lineFeedWidth == 1;
                    const bool binary = memchr( firstVec.data(), '\0',
                                                 static_cast<size_t>( firstVec.size() ) )
                                        != nullptr;
                    if ( utf8SingleByte && !binary ) {
                        // Byte offset one past each LF; append a sentinel == file size
                        // when the final line has no trailing newline so every line
                        // maps to exactly one endOfLines entry (scanBuffer attributes a
                        // match's end byte to a line via upper_bound on this vector).
                        klogg::vector<qint64> endOfLines;
                        endOfLines.reserve( static_cast<size_t>( wbytes / 40 ) + 1 );
                        // memchr (SIMD) locates newlines far faster than a per-byte
                        // branch loop; this line-boundary precompute is the dominant
                        // remaining gap to ripgrep (which attributes lines lazily).
                        const char* newlineCursor = wdata;
                        const char* const nend = wdata + wbytes;
                        while ( newlineCursor < nend ) {
                            newlineCursor = static_cast<const char*>(
                                memchr( newlineCursor, '\n', static_cast<size_t>( nend - newlineCursor ) ) );
                            if ( newlineCursor == nullptr ) {
                                break;
                            }
                            endOfLines.push_back( static_cast<qint64>( newlineCursor - wdata ) + 1 );
                            ++newlineCursor;
                        }
                        if ( endOfLines.empty() || endOfLines.back() != wbytes ) {
                            endOfLines.push_back( wbytes ); // sentinel: unterminated last line
                        }

                        klogg::vector<uint64_t> matched;
                        std::optional<SearchableLogData::RawLines> normalizedRows;
                        SearchableLogData::RawLines::Utf8Block normalizedBlock;
                        if ( SearchableLogData::RawLines::canBorrowNormalizedUtf8(
                                 std::string_view{ wdata, static_cast<size_t>( wbytes ) }, fastParams ) ) {
                            matcher.scanBuffer( wdata, static_cast<unsigned int>( wbytes ),
                                                endOfLines, matched );
                        }
                        else {
                            // Keep original source boundaries/bytes for retention and
                            // row offsets; only matching and widths use canonical text.
                            auto& raw = normalizedRows.emplace();
                            raw.startLine = 0_lnum;
                            raw.buffer.assign( wdata, wdata + wbytes );
                            raw.endOfLines = endOfLines;
                            if ( wdata[ wbytes - 1 ] != '\n' ) {
                                ++raw.endOfLines.back();
                            }
                            raw.textDecoder = { std::make_unique<QTextDecoder>( fastCodec ), fastParams };
                            normalizedBlock = raw.buildUtf8Block();
                            matcher.scanBuffer( normalizedBlock.buffer.data(),
                                static_cast<unsigned int>( normalizedBlock.buffer.size() ),
                                normalizedBlock.endOfLines, matched );
                        }
                        // scanBuffer returns matched line indices ascending and
                        // deduped (occupancy-vector collect), which is exactly the
                        // localLine-ascending, deduped order the results layer expects.

                        group.matches.reserve( matched.size() );
                        for ( const uint64_t idx : matched ) {
                            const qint64 end = endOfLines[ idx ];
                            const qint64 start = ( idx == 0u ) ? 0 : endOfLines[ idx - 1u ];
                            // Content excludes the trailing LF (it sits at end-1).
                            const qint64 contentLen
                                = ( end > start && wdata[ end - 1 ] == '\n' ) ? end - 1 - start
                                                                              : end - start;
                            const auto lineView = normalizedRows
                                ? normalizedBlock.lines[ static_cast<size_t>( idx ) ]
                                : std::string_view( wdata + start,
                                    static_cast<size_t>( std::max<qint64>( contentLen, 0 ) ) );
                            klogg::folder::MatchRecord record;
                            record.localLine = LineNumber( idx );
                            record.lineStartByte = OffsetInFile( start );
                            record.lineEndByte = OffsetInFile( end );
                            record.lineLength = getUntabifiedLength( lineView );
                            record.matchLen = 0_length;
                            record.role = klogg::folder::RecordRole::Match;
                            group.matches.push_back( record );
                        }
                        retainedLines = LinesCount( endOfLines.size() );
                        retain( whole );
                        for ( size_t first = 0; first < endOfLines.size(); ) {
                            const auto count = std::min( klogg::folder::ScanArena::BoundaryBatchLines,
                                                          endOfLines.size() - first );
                            arena->appendLineEnds( rowSpans, static_cast<uint64_t>( first ),
                                                   endOfLines.data() + first, count );
                            first += count;
                        }
                        return finish();
                    }
                }
                // Fast path not taken: rewind for the streaming per-line scan.
                file.seek( 0 );
            }
        }

        OffsetInFile fileOffset = 0_offset; // absolute byte offset of block[0]
        OffsetInFile lineStartAbs = 0_offset; // absolute byte offset of the current line's start
        LineNumber localLine = 0_lnum; // 0-based line counter within the file
        QByteArray carry; // raw bytes of the current line carried across a block boundary
        bool firstBlock = true;

        // Detected once on the first block and cached for the whole file (mirrors
        // IndexOperation::guessEncoding, logdataworker.cpp). Defaults leave the
        // single-byte UTF-8/ASCII fast path active until detection runs.
        QTextCodec* codec = QTextCodec::codecForName( "UTF-8" );
        EncodingParameters params( codec );

        // --- grep -A/-B/-C context capture (per-file; never crosses a file
        // boundary since scanFile is called once per file) ---
        // beforeRing holds the last `before` completed lines (-B candidates);
        // afterRemaining is the -A countdown armed on each match. Dedup relies on
        // the monotonic invariant: emission is strictly ascending in localLine, so a
        // candidate already emitted (as Match or Context) satisfies
        // `localLine <= lastEmittedLocalLine` and is skipped (grep overlap merge).
        const int before = context.before;
        const int after = context.after;
        struct LineTuple {
            OffsetInFile start;
            OffsetInFile end;
            LineNumber localLine;
            LineLength length;
        };
        std::vector<LineTuple> beforeRing;
        if ( before > 0 ) {
            beforeRing.reserve( static_cast<size_t>( before ) );
        }
        int afterRemaining = 0;
        LineNumber lastEmittedLocalLine = 0_lnum;
        bool haveEmittedRow = false;

        // Emits one Match or Context record for the line at (localLine, lineStartAbs
        // .. endByte). On a match it also flushes the -B ring (as Context) and arms
        // the -A countdown; a non-match either feeds the ring or drains the -A
        // window. Mirrors LogFilteredData::rebuildContextLinesList's std::set union
        // via the streaming monotonic dedup above.
        auto recordLine = [ & ]( std::string_view lineView, OffsetInFile startByte,
                                 OffsetInFile endByte ) {
            std::optional<SearchableLogData::RawLines> normalized;
            if ( !SearchableLogData::RawLines::canBorrowNormalizedUtf8( lineView, params ) ) {
                auto& raw = normalized.emplace();
                raw.startLine = localLine;
                raw.buffer.assign( lineView.begin(), lineView.end() );
                // The splitter excludes the physical separator. Shared decoding
                // accepts this synthetic boundary for complete and EOF records.
                raw.endOfLines.push_back( static_cast<qint64>( lineView.size() ) + params.lineFeedWidth );
                raw.textDecoder = { std::make_unique<QTextDecoder>( params.codec ), params };
                const auto block = raw.buildUtf8Block();
                lineView = block.lines.front();
            }
            const bool isMatch = matcher.hasMatch( lineView );
            // Count every consumed line, not only matches/context. Source identity
            // existence must survive presentation filtering, including zero-match files.
            retainLineEnd( endByte.get() );
            retainedLines = LinesCount( localLine.get() + 1 );
            // Default fast path: no context window active and not a match -> nothing
            // to capture. Keeps before=0/after=0 byte-count-identical to the legacy
            // per-line path (no getUntabifiedLength call for non-matches).
            if ( !isMatch && before == 0 && afterRemaining <= 0 ) {
                return;
            }
            const LineLength len = getUntabifiedLength( lineView );
            if ( isMatch ) {
                if ( before > 0 ) {
                    for ( const auto& preceding : beforeRing ) {
                        if ( !haveEmittedRow || preceding.localLine > lastEmittedLocalLine ) {
                            klogg::folder::MatchRecord record;
                            record.localLine = preceding.localLine;
                            record.lineStartByte = preceding.start;
                            record.lineEndByte = preceding.end;
                            record.lineLength = preceding.length;
                            record.matchLen = 0_length;
                            record.role = klogg::folder::RecordRole::Context;
                            group.matches.push_back( record );
                            lastEmittedLocalLine = preceding.localLine;
                            haveEmittedRow = true;
                        }
                    }
                    beforeRing.clear();
                }
                klogg::folder::MatchRecord record;
                record.localLine = localLine;
                record.lineStartByte = startByte;
                record.lineEndByte = endByte;
                record.lineLength = len;
                record.matchLen = 0_length;
                record.role = klogg::folder::RecordRole::Match;
                group.matches.push_back( record );
                lastEmittedLocalLine = localLine;
                haveEmittedRow = true;
                afterRemaining = after;
            }
            else {
                if ( before > 0 ) {
                    beforeRing.push_back( LineTuple{ startByte, endByte, localLine, len } );
                    if ( static_cast<int>( beforeRing.size() ) > before ) {
                        beforeRing.erase( beforeRing.begin() );
                    }
                }
                // Only drain an ACTIVE after-window. The fast-path guard above does
                // NOT short-circuit when before>0 (the ring must still be fed), so
                // gate the after-context emit explicitly on afterRemaining > 0.
                if ( afterRemaining > 0 ) {
                    if ( !haveEmittedRow || localLine > lastEmittedLocalLine ) {
                        klogg::folder::MatchRecord record;
                        record.localLine = localLine;
                        record.lineStartByte = startByte;
                        record.lineEndByte = endByte;
                        record.lineLength = len;
                        record.matchLen = 0_length;
                        record.role = klogg::folder::RecordRole::Context;
                        group.matches.push_back( record );
                        lastEmittedLocalLine = localLine;
                        haveEmittedRow = true;
                    }
                    --afterRemaining;
                }
            }
        };

        QByteArray block = file.read( blockSize );
        while ( !block.isEmpty() ) {
            if ( firstBlock ) {
                firstBlock = false;
                // Same binary short-circuit as the fast path, before detection:
                // BOM-less + NUL in the window -> binary, skip without uchardet.
                const auto* blockData = block.constData();
                const auto blockBytes = block.size();
                const int binaryWindow
                    = std::min<int>( static_cast<int>( block.size() ), BinaryDetectionWindow );
                const std::string_view blockSample( blockData,
                                                    static_cast<size_t>( binaryWindow ) );
                if ( !klogg::encoding::startsWithUnicodeBom( blockSample )
                     && blockSample.find( '\0' ) != std::string_view::npos ) {
                    return group;
                }
                // Detect-on-first-block: one copy of the first block into a
                // klogg::vector<char> for uchardet + QTextCodec::codecForUtfText,
                // exactly as the indexer does.
                klogg::vector<char> firstBlockVec( blockData, blockData + blockBytes );
                codec = EncodingDetector::getInstance().detectEncoding( firstBlockVec );
                if ( codec != nullptr ) {
                    params = EncodingParameters( codec );
                    group.sourceCodec = codec;
                }

                // grep -I: a NUL byte in the opening window means "binary" -> skip.
                // Only meaningful for single-byte encodings: UTF-16/UTF-32 encode
                // every ASCII char with a 0x00 high byte, so a NUL scan would
                // misclassify them. Codec detection has already confirmed those are
                // text, so the check is gated on lineFeedWidth == 1.
                if ( params.lineFeedWidth == 1 ) {
                    const int window
                        = std::min<int>( static_cast<int>( block.size() ), BinaryDetectionWindow );
                    if ( block.left( window ).contains( '\0' ) ) {
                        return group;
                    }
                }
            }

            retain( block );
            const char* const data = block.constData();
            const int size = static_cast<int>( block.size() );
            int scanFrom = 0;
            while ( true ) {
                // Locate the next line-feed. For the single-byte fast path this is a
                // plain QByteArray::indexOf (zero regression vs. the original code);
                // for multi-byte encodings the shared findNextMultiByteDelimeter
                // validates the surrounding 0x00 bytes of the LF sequence.
                int lfPos = -1; // block-relative index of the 0x0A byte, or -1
                if ( params.lineFeedWidth == 1 ) {
                    lfPos = static_cast<int>( block.indexOf( '\n', scanFrom ) );
                }
                else {
                    // Multi-byte: a line-feed sequence (UTF-16 0x0A 0x00 / 0x00 0x0A,
                    // UTF-32, ...) can straddle the carry/block seam. First probe the
                    // seam (tail of carry + head of block); if an LF completes there,
                    // resolve the carry line directly and resume in the block.
                    // Otherwise the block-local search below handles LFs fully inside
                    // the block (its surrounding 0x00 partners are in view).
                    bool seamResolved = false;
                    if ( !carry.isEmpty() ) {
                        const int carrySize = static_cast<int>( carry.size() );
                        const int tailLen
                            = std::min<int>( carrySize, params.lineFeedWidth - 1 );
                        const QByteArray seam
                            = carry.right( tailLen ) + block.left( params.lineFeedWidth );
                        const std::string_view seamView(
                            seam.constData(), static_cast<size_t>( seam.size() ) );
                        const auto seamFeed = klogg::encoding::findNextMultiByteDelimeter(
                            params, seamView, '\n' );
                        if ( seamFeed != std::string_view::npos && seamFeed < static_cast<size_t>( tailLen ) ) {
                            // The 0x0A sits in carry's tail and its 0x00 partners are in
                            // block: a genuine seam LF. Resolve the carry line.
                            const int lfInCarry = carrySize - tailLen + static_cast<int>( seamFeed );
                            const int contentEnd = lfInCarry - params.lineFeedIndex;
                            const int feedEnd = lfInCarry
                                                + ( params.lineFeedWidth - params.lineFeedIndex );

                            const std::string_view lineView(
                                carry.constData(), static_cast<size_t>( contentEnd ) );
                            recordLine( lineView, lineStartAbs,
                                        OffsetInFile( lineStartAbs.get() + feedEnd ) );
                            ++localLine;
                            lineStartAbs = OffsetInFile( lineStartAbs.get() + feedEnd );
                            carry.clear();
                            scanFrom = feedEnd - carrySize; // resume past the LF, in block space
                            scanFrom = std::max( scanFrom, 0 );
                            seamResolved = true;
                            if ( stopped( shouldStop ) ) {
                                return finish();
                            }
                            continue; // re-scan from the new block offset (carry now empty)
                        }
                    }

                    if ( !seamResolved ) {
                        const std::string_view blockView(
                            data + scanFrom, static_cast<size_t>( size - scanFrom ) );
                        const auto found = klogg::encoding::findNextMultiByteDelimeter(
                            params, blockView, '\n' );
                        lfPos = ( found == std::string_view::npos )
                                    ? -1
                                    : scanFrom + static_cast<int>( found );
                    }
                }

                if ( lfPos < 0 ) {
                    // No more newlines in this block: the remainder is a continuation
                    // of the still-incomplete current line. Append it to `carry` and
                    // read the next block. Do NOT evaluate here -- the line is not
                    // complete; it is evaluated when a newline completes it, or at
                    // EOF if the file ends without a trailing newline. Carry stays
                    // RAW bytes so a block edge splitting the LF sequence resolves
                    // when the carried+joined view re-finds the LF next block.
                    carry += block.mid( scanFrom, size - scanFrom );
                    break;
                }

                // The 0x0A byte sits lineFeedIndex bytes into the multi-byte LF
                // sequence. lineContentEnd is where the line's text ends (start of
                // the LF sequence); lineFeedEnd is just past the WHOLE LF sequence,
                // i.e. the next line's start. For UTF-8/ASCII (w=1,i=0) both reduce
                // to the original nl / nl+1 values, so offsets are byte-identical.
                const int lineContentEnd = lfPos - params.lineFeedIndex;
                const int lineFeedEnd = lfPos + ( params.lineFeedWidth - params.lineFeedIndex );

                // Preserve raw bytes here; recordLine applies the shared codec,
                // replacement, BOM, and framing rules once for every backend.
                std::string_view lineView;
                QByteArray joined;
                if ( carry.isEmpty() ) {
                    lineView = std::string_view(
                        data + scanFrom, static_cast<size_t>( lineContentEnd - scanFrom ) );
                }
                else {
                    joined = carry + QByteArray::fromRawData( data + scanFrom,
                                                              lineContentEnd - scanFrom );
                    lineView = std::string_view(
                        joined.constData(), static_cast<size_t>( joined.size() ) );
                }

                recordLine( lineView, lineStartAbs,
                            OffsetInFile( fileOffset.get() + lineFeedEnd ) );

                ++localLine;
                carry.clear();
                lineStartAbs = OffsetInFile( fileOffset.get() + lineFeedEnd );
                scanFrom = lineFeedEnd;

                if ( stopped( shouldStop ) ) {
                    return finish();
                }
            }

            fileOffset = OffsetInFile( fileOffset.get() + size );

            if ( stopped( shouldStop ) ) {
                return finish();
            }
            block = file.read( blockSize );
        }

        // Trailing line without a final newline (left over in `carry`).
        if ( !carry.isEmpty() ) {
            const std::string_view lineView( carry.constData(), static_cast<size_t>( carry.size() ) );
            recordLine( lineView, lineStartAbs, fileOffset /*EOF*/ );
        }

        if ( file.error() != QFileDevice::NoError ) {
            throw IndexedContentError( "Cannot finish retaining scanned folder source" );
        }
        return finish();
    }
    catch ( const std::exception& error ) {
        // Keep already-found records but refuse immutable reads of this member.
        // Never let retention/normalization failures escape the scanner worker.
        LOG_WARNING << "Folder scanned source unavailable for" << path << error.what();
        group.content.reset();
        group.retentionError = QString::fromUtf8( error.what() );
        return group;
    }
}
