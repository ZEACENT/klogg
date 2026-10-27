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

#ifndef FOLDERSEARCHENGINE_H
#define FOLDERSEARCHENGINE_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "foldersearchtypes.h"
#include "regularexpressionpattern.h"

class PatternMatcher;

// Streams a folder of files through the existing SIMD regex engine and collects
// matches into FolderSearchResults groups. This is the grep-faithful core of
// folder search: no byte-offset index is built, every search is a full scan
// (requirement: "search the whole folder each time, like grep").
//
// Matching reuses RegularExpression/PatternMatcher and the shared normalized
// record contract. Eligible UTF-8 files retain block scanning; the shared
// capability gate routes patterns that cannot preserve per-record semantics
// through hasMatch(line), alongside context and non-UTF-8 streaming scans.
//
// Files are scanned on a worker thread; results are accumulated thread-safely
// and handed to the caller via takeResults() after searchFinished. Signals
// carry only plain quint64/int so they cross thread boundaries on queued
// connections without qRegisterMetaType (mirroring LogFilteredData).
class FolderSearchEngine : public QObject {
    Q_OBJECT

  public:
    // Read granularity for the streaming line-splitter.
    static constexpr qint64 DefaultBlockSize = 1 << 20; // 1 MiB
    // grep -I considers a file binary if it contains a NUL byte in the first
    // few KB; mirror that window rather than scanning the whole first block.
    static constexpr int BinaryDetectionWindow = 32 * 1024;

    using StopPredicate = std::function<bool()>;
    // A stalled request may overlap one replacement request. Implementations must
    // therefore be safe for at most two concurrent invocations and must own any
    // captured state until each invocation returns.
    using FolderEnumerator
        = std::function<QStringList( const QString&, const StopPredicate& )>;

    explicit FolderSearchEngine( QObject* parent = nullptr );
    explicit FolderSearchEngine( FolderEnumerator folderEnumerator,
                                 QObject* parent = nullptr );
    ~FolderSearchEngine() override;

    FolderSearchEngine( const FolderSearchEngine& ) = delete;
    FolderSearchEngine& operator=( const FolderSearchEngine& ) = delete;

    // Async full scan. Bumps the generation (superseding any in-progress scan),
    // posts the request to the worker thread, and returns the new generation.
    // Receivers compare the signal-supplied generation against
    // currentGeneration() to drop stale signals. `context` selects grep -A/-B/-C
    // context lines captured at scan time (re-run startSearch to change it).
    quint64 startSearch( const QStringList& filePaths, const RegularExpressionPattern& pattern,
                         klogg::folder::ContextOptions context = {} );

    // Async latest-snapshot search. Enumeration and scanning share the same
    // generation, cancellation, worker, and destruction lifecycle.
    quint64 startFolderSearch( const QString& folderPath,
                               const RegularExpressionPattern& pattern,
                               klogg::folder::ContextOptions context = {} );

    // Synchronous full scan on the calling thread (bumps generation first).
    // Used by tests and the benchmark. Emits searchStarted/searchProgressed/
    // searchFinished for the new generation.
    quint64 scanSynchronously( const QStringList& filePaths,
                               const RegularExpressionPattern& pattern,
                               klogg::folder::ContextOptions context = {} );

    // Run a scan for a SPECIFIC generation on the calling thread. Aborts before
    // it starts if currentGeneration() != generation, and aborts between files
    // if interrupt was requested. Public so generation-staleness is unit
    // testable without the worker thread.
    void runSearch( quint64 generation, const QStringList& filePaths,
                    const RegularExpressionPattern& pattern,
                    klogg::folder::ContextOptions context = {} );

    // Stop the in-progress scan (and cancel any queued request). Does not bump
    // the generation; the running scan emits its own searchFinished as it winds
    // down.
    void interrupt();

    // Advance the generation without starting a search, so any queued signals
    // from the previous search are recognised as stale (mirrors
    // LogFilteredData::bumpSearchGeneration).
    quint64 bumpGeneration();

    quint64 currentGeneration() const noexcept { return generation_.load( std::memory_order_relaxed ); }
    quint64 matchCount() const noexcept { return matchCount_.load( std::memory_order_relaxed ); }

    // Thread-safe: consume accumulated groups in enumeration order. Successful
    // zero-match members are omitted; retention failures remain explicit.
    std::vector<klogg::folder::FileGroup> takeResults();

    // Thread-safe: move out the per-file streaming group for `fileIndex` (the
    // main-thread consumer pulls each finished group this way as fileGroupReady
    // fires). Returns nullopt if the index is out of range or the group was
    // already taken. Used by the streaming (widget) path; the sync/test/benchmark
    // path uses takeResults() instead.
    std::optional<klogg::folder::FileGroup> takeCompletedGroup( int fileIndex );

    // Scan a single file synchronously. Returns its FileGroup (empty matches if
    // the file is binary, unreadable, or has no matches). The pure, testable
    // unit: a function of (path, matcher, block size, stop predicate, context)
    // only. `context` captures grep -A/-B/-C rows around each match.
    static klogg::folder::FileGroup scanFile( const QString& path, const PatternMatcher& matcher,
                                              qint64 blockSize = DefaultBlockSize,
                                              std::function<bool()> shouldStop = {},
                                              klogg::folder::ContextOptions context = {},
                                              std::shared_ptr<klogg::folder::ScanArena> arena = {} );

  Q_SIGNALS:
    void searchStarted( quint64 generation );
    void searchProgressed( quint64 nbMatches, int progressPercent, quint64 generation );
    void searchFinished( quint64 generation );
    // Published after worker-thread enumeration and before scanning starts.
    void folderSnapshotReady( QStringList filePaths, quint64 generation );
    // Fired per file once its scan completes (crosses thread boundaries on a
    // queued connection; carries only plain int + quint64, like searchProgressed).
    // The main thread responds by calling takeCompletedGroup(fileIndex).
    void fileGroupReady( int fileIndex, quint64 generation );

  private:
    enum class RequestSource : std::uint8_t { FilePaths, Folder };

    void workerLoop();

    std::atomic<quint64> generation_{ 0 };
    std::atomic<bool> interruptRequested_{ false };
    std::atomic<bool> shutdown_{ false };

    std::mutex requestMutex_;
    std::condition_variable requestCv_;
    struct Request {
        quint64 generation = 0;
        RequestSource source = RequestSource::FilePaths;
        QStringList filePaths;
        QString folderPath;
        RegularExpressionPattern pattern;
        klogg::folder::ContextOptions context;
        bool valid = false;
    };
    quint64 submit( Request request );
    void runFolderSearch( const Request& request );
    Request pendingRequest_; // guarded by requestMutex_

    FolderEnumerator folderEnumerator_;
    std::shared_ptr<std::atomic<int>> detachedEnumerationCount_
        = std::make_shared<std::atomic<int>>( 0 );
    std::thread workerThread_;

    std::mutex resultsMutex_;
    // Consume-once buffer in enumeration order. Streaming consumers move one
    // completed group; synchronous consumers move all groups via takeResults.
    // No second result owner copies records or retains drained source leases.
    std::vector<std::optional<klogg::folder::FileGroup>> pending_;
    std::atomic<quint64> matchCount_{ 0 };
};

#endif // FOLDERSEARCHENGINE_H
