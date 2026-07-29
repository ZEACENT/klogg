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
#include <cstring>
#include <string_view>
#include <utility>

#include "configuration.h"
#include "encodingdetector.h"
#include "encodingutils.h"
#include "foldersearchtypes.h"
#include "linetypes.h"
#include "log.h"
#include "regularexpression.h"

namespace {

// Whole-file vectorscan fast path is used only for files up to this size so a
// folder of many large logs cannot blow up memory; bigger files stream through
// the per-line path below.
constexpr qint64 FastPathMaxBytes = 16LL * 1024 * 1024; // 16 MiB

// True if `shouldStop` is set and reports stop. A null predicate never stops.
bool stopped( const std::function<bool()>& shouldStop )
{
    return shouldStop && shouldStop();
}

} // namespace

FolderSearchEngine::FolderSearchEngine( QObject* parent )
    : QObject( parent )
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

quint64 FolderSearchEngine::startSearch( const QStringList& filePaths,
                                         const RegularExpressionPattern& pattern,
                                         klogg::folder::ContextOptions context )
{
    const quint64 gen = generation_.fetch_add( 1, std::memory_order_relaxed ) + 1;
    interruptRequested_ = false;
    {
        std::lock_guard<std::mutex> lock( requestMutex_ );
        pendingRequest_ = Request{ gen, filePaths, pattern, context, true };
    }
    requestCv_.notify_one();
    return gen;
}

quint64 FolderSearchEngine::scanSynchronously( const QStringList& filePaths,
                                               const RegularExpressionPattern& pattern,
                                               klogg::folder::ContextOptions context )
{
    const quint64 gen = generation_.fetch_add( 1, std::memory_order_relaxed ) + 1;
    interruptRequested_ = false;
    runSearch( gen, filePaths, pattern, context );
    return gen;
}

quint64 FolderSearchEngine::bumpGeneration()
{
    return generation_.fetch_add( 1, std::memory_order_relaxed ) + 1;
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
    out.swap( results_ );
    return out;
}

std::optional<klogg::folder::FileGroup> FolderSearchEngine::takeCompletedGroup( int fileIndex )
{
    std::lock_guard<std::mutex> lock( resultsMutex_ );
    if ( fileIndex < 0 || static_cast<size_t>( fileIndex ) >= pending_.size() ) {
        return std::nullopt;
    }
    return std::move( pending_[ static_cast<size_t>( fileIndex ) ] );
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
        results_.clear();
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

    auto worker = [ this, &filePaths, total, gen, context, &nextFileIndex, &completedCount,
                    &shouldStop ]( std::unique_ptr<PatternMatcher> matcher ) {
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

            auto group = scanFile( filePaths[ i ], *matcher, DefaultBlockSize, shouldStop, context );

            if ( !group.matches.empty() ) {
                // Count ONLY real matches: with -A/-B/-C the vector also holds
                // Context rows that must not inflate the status/header count.
                const auto trueMatches = std::count_if(
                    group.matches.begin(), group.matches.end(),
                    []( const klogg::folder::MatchRecord& r ) {
                        return r.role == klogg::folder::RecordRole::Match;
                    } );
                matchCount_.fetch_add( static_cast<quint64>( trueMatches ),
                                       std::memory_order_relaxed );
            }
            {
                std::lock_guard<std::mutex> lock( resultsMutex_ );
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
    for ( int t = 0; t < poolSize; ++t ) {
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

    // Rebuild results_ from pending_ in strict enumeration order so takeResults
    // (sync/test/benchmark) returns a deterministic, run-to-run-stable set
    // regardless of which worker finished first. COPY (not move) so that a
    // streaming consumer draining pending_ via takeCompletedGroup is unaffected
    // by this rebuild (the widget path never calls takeResults; results_ is
    // reset at the start of every search).
    {
        std::lock_guard<std::mutex> lock( resultsMutex_ );
        results_.clear();
        for ( int i = 0; i < total; ++i ) {
            const auto& opt = pending_[ static_cast<size_t>( i ) ];
            if ( opt.has_value() && !opt->matches.empty() ) {
                results_.push_back( *opt );
            }
        }
    }

    Q_EMIT searchFinished( gen );
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
        runSearch( req.generation, req.filePaths, req.pattern, req.context );
    }
}

klogg::folder::FileGroup FolderSearchEngine::scanFile( const QString& path,
                                                       const PatternMatcher& matcher,
                                                       qint64 blockSize,
                                                       std::function<bool()> shouldStop,
                                                       klogg::folder::ContextOptions context )
{
    klogg::folder::FileGroup group;
    group.filePath = path;

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
                    const char* np = wdata;
                    const char* const nend = wdata + wbytes;
                    while ( np < nend ) {
                        np = static_cast<const char*>(
                            memchr( np, '\n', static_cast<size_t>( nend - np ) ) );
                        if ( np == nullptr ) {
                            break;
                        }
                        endOfLines.push_back( static_cast<qint64>( np - wdata ) + 1 );
                        ++np;
                    }
                    if ( endOfLines.empty() || endOfLines.back() != wbytes ) {
                        endOfLines.push_back( wbytes ); // sentinel: unterminated last line
                    }

                    klogg::vector<uint64_t> matched;
                    matcher.scanBuffer( wdata, static_cast<unsigned int>( wbytes ), endOfLines,
                                        matched );
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
                        const std::string_view lineView(
                            wdata + start, static_cast<size_t>( std::max<qint64>( contentLen, 0 ) ) );
                        klogg::folder::MatchRecord r;
                        r.localLine = LineNumber( idx );
                        r.lineStartByte = OffsetInFile( start );
                        r.lineEndByte = OffsetInFile( end );
                        r.lineLength = getUntabifiedLength( lineView );
                        r.matchLen = 0_length;
                        r.role = klogg::folder::RecordRole::Match;
                        group.matches.push_back( std::move( r ) );
                    }
                    return group;
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
    QTextCodec* codec = nullptr;
    EncodingParameters params;

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
                             OffsetInFile endByte, bool isMatch ) {
        // Default fast path: no context window active and not a match -> nothing
        // to capture. Keeps before=0/after=0 byte-count-identical to the legacy
        // per-line path (no getUntabifiedLength call for non-matches).
        if ( !isMatch && before == 0 && afterRemaining <= 0 ) {
            return;
        }
        const LineLength len = getUntabifiedLength( lineView );
        if ( isMatch ) {
            if ( before > 0 ) {
                for ( const auto& t : beforeRing ) {
                    if ( !haveEmittedRow || t.localLine > lastEmittedLocalLine ) {
                        klogg::folder::MatchRecord r;
                        r.localLine = t.localLine;
                        r.lineStartByte = t.start;
                        r.lineEndByte = t.end;
                        r.lineLength = t.length;
                        r.matchLen = 0_length;
                        r.role = klogg::folder::RecordRole::Context;
                        group.matches.push_back( std::move( r ) );
                        lastEmittedLocalLine = t.localLine;
                        haveEmittedRow = true;
                    }
                }
                beforeRing.clear();
            }
            klogg::folder::MatchRecord r;
            r.localLine = localLine;
            r.lineStartByte = startByte;
            r.lineEndByte = endByte;
            r.lineLength = len;
            r.matchLen = 0_length;
            r.role = klogg::folder::RecordRole::Match;
            group.matches.push_back( std::move( r ) );
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
                    klogg::folder::MatchRecord r;
                    r.localLine = localLine;
                    r.lineStartByte = startByte;
                    r.lineEndByte = endByte;
                    r.lineLength = len;
                    r.matchLen = 0_length;
                    r.role = klogg::folder::RecordRole::Context;
                    group.matches.push_back( std::move( r ) );
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
                    const auto sf = klogg::encoding::findNextMultiByteDelimeter(
                        params, seamView, '\n' );
                    if ( sf != std::string_view::npos && sf < static_cast<size_t>( tailLen ) ) {
                        // The 0x0A sits in carry's tail and its 0x00 partners are in
                        // block: a genuine seam LF. Resolve the carry line.
                        const int lfInCarry = carrySize - tailLen + static_cast<int>( sf );
                        const int contentEnd = lfInCarry - params.lineFeedIndex;
                        const int feedEnd = lfInCarry
                                            + ( params.lineFeedWidth - params.lineFeedIndex );

                        std::string_view lineView;
                        QByteArray decoded;
                        if ( params.isUtf8Compatible ) {
                            lineView = std::string_view(
                                carry.constData(), static_cast<size_t>( contentEnd ) );
                        }
                        else {
                            decoded = codec->toUnicode( carry.left( contentEnd ) ).toUtf8();
                            lineView = std::string_view(
                                decoded.constData(), static_cast<size_t>( decoded.size() ) );
                        }
                        recordLine( lineView, lineStartAbs,
                                    OffsetInFile( lineStartAbs.get() + feedEnd ),
                                    matcher.hasMatch( lineView ) );
                        ++localLine;
                        lineStartAbs = OffsetInFile( lineStartAbs.get() + feedEnd );
                        carry.clear();
                        scanFrom = feedEnd - carrySize; // resume past the LF, in block space
                        if ( scanFrom < 0 ) {
                            scanFrom = 0;
                        }
                        seamResolved = true;
                        if ( stopped( shouldStop ) ) {
                            return group;
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

            // Build the matcher's view of the line. When the file is UTF-8
            // compatible this is a zero-copy raw string_view into `block` (or
            // the `joined` buffer for boundary-spanning lines) -- byte-identical
            // to the original ASCII/UTF-8 path. Otherwise decode via the source
            // codec into a local QByteArray kept alive for the hasMatch call,
            // mirroring SearchableLogData::RawLines::buildUtf8View.
            std::string_view lineView;
            QByteArray joined;
            QByteArray decoded;
            if ( carry.isEmpty() ) {
                if ( params.isUtf8Compatible ) {
                    lineView = std::string_view(
                        data + scanFrom, static_cast<size_t>( lineContentEnd - scanFrom ) );
                }
                else {
                    decoded = codec->toUnicode( QByteArray::fromRawData(
                                                data + scanFrom, lineContentEnd - scanFrom ) )
                                  .toUtf8();
                    lineView = std::string_view(
                        decoded.constData(), static_cast<size_t>( decoded.size() ) );
                }
            }
            else {
                joined = carry + QByteArray::fromRawData( data + scanFrom,
                                                          lineContentEnd - scanFrom );
                if ( params.isUtf8Compatible ) {
                    lineView = std::string_view(
                        joined.constData(), static_cast<size_t>( joined.size() ) );
                }
                else {
                    decoded = codec->toUnicode( joined ).toUtf8();
                    lineView = std::string_view(
                        decoded.constData(), static_cast<size_t>( decoded.size() ) );
                }
            }

            recordLine( lineView, lineStartAbs,
                        OffsetInFile( fileOffset.get() + lineFeedEnd ),
                        matcher.hasMatch( lineView ) );

            ++localLine;
            carry.clear();
            lineStartAbs = OffsetInFile( fileOffset.get() + lineFeedEnd );
            scanFrom = lineFeedEnd;

            if ( stopped( shouldStop ) ) {
                return group;
            }
        }

        fileOffset = OffsetInFile( fileOffset.get() + size );

        if ( stopped( shouldStop ) ) {
            return group;
        }
        block = file.read( blockSize );
    }

    // Trailing line without a final newline (left over in `carry`).
    if ( !carry.isEmpty() ) {
        std::string_view lineView;
        QByteArray decoded;
        if ( params.isUtf8Compatible ) {
            lineView
                = std::string_view( carry.constData(), static_cast<size_t>( carry.size() ) );
        }
        else {
            decoded = codec->toUnicode( carry ).toUtf8();
            lineView = std::string_view(
                decoded.constData(), static_cast<size_t>( decoded.size() ) );
        }
        recordLine( lineView, lineStartAbs, fileOffset /*EOF*/, matcher.hasMatch( lineView ) );
    }

    return group;
}
