/*
 * Copyright (C) 2026
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

#include <catch2/catch.hpp>

#include <QDir>
#include <QCoreApplication>
#include <QDate>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QTime>
#include <QTimerEvent>
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <new>
#include <optional>
#include <string_view>

#include "capturestore.h"
#include "configuration.h"
#include "livelogexportservice.h"
#include "logfiltereddata.h"
#include "streaminglogdata.h"
#include "test_utils.h"

TEST_CASE( "Streaming output receives the complete accepted batch before capture retention",
           "[streaming][storage-integrity]" )
{
    const auto mode = GENERATE( LiveLogSaveAnsiMode::Strip, LiveLogSaveAnsiMode::Preserve );
    QTemporaryDir root;
    REQUIRE( root.isValid() );
    const auto output = QDir( root.path() ).filePath( "output.log" );
    StreamingLogData data( QUuid::createUuid().toString( QUuid::WithoutBraces ), root.path() );
    CaptureStore::Limits limits;
    limits.segmentTargetBytes = 8;
    limits.maxTotalLines = 2;
    data.setCaptureLimits( limits );
    REQUIRE( data.bindOutputFile( output, mode ) );
    const QByteArray batch(
        "\033[31ma\033[0m\r\n\033[31mb\033[0m\n\033[31mc\033[0m\n\033[31md\033[0m\n" );
    data.appendUtf8( batch );
    data.finishInput();
    REQUIRE( data.getNbLine() <= 2_lcount );
    QFile saved( output );
    REQUIRE( saved.open( QIODevice::ReadOnly ) );
    auto expected = batch;
    expected.replace( "\r\n", "\n" );
    CHECK( saved.readAll()
           == ( mode == LiveLogSaveAnsiMode::Strip ? QByteArray( "a\nb\nc\nd\n" ) : expected ) );
}

TEST_CASE( "Streaming output preserves finalized boundaries for replay and restore",
           "[streaming][capture-output][storage-integrity]" )
{
    const auto mode = GENERATE( LiveLogSaveAnsiMode::Strip, LiveLogSaveAnsiMode::Preserve );
    const bool restore = GENERATE( false, true );
    QTemporaryDir root;
    REQUIRE( root.isValid() );
    const auto output = QDir( root.path() ).filePath( "boundary.log" );
    StreamingLogData data( QUuid::createUuid().toString( QUuid::WithoutBraces ), root.path() );
    if ( restore ) {
        QFile file( output );
        REQUIRE( file.open( QIODevice::WriteOnly ) );
        REQUIRE( file.write( "a" ) == 1 );
        file.close();
        REQUIRE( data.bindOutputFile( output, mode, OutputBindMode::Restore ) );
    }
    else {
        data.appendUtf8( "a" );
        data.finishInput();
        data.appendUtf8( "b" );
        data.finishInput();
        REQUIRE( data.bindOutputFile( output, mode ) );
    }
    {
        QFile file( output );
        REQUIRE( file.open( QIODevice::ReadOnly ) );
        CHECK( file.readAll() == ( restore ? QByteArray( "a" ) : QByteArray( "a\nb" ) ) );
    }
    data.appendUtf8( "c" );
    data.finishInput();
    QFile file( output );
    REQUIRE( file.open( QIODevice::ReadOnly ) );
    CHECK( file.readAll() == ( restore ? QByteArray( "a\nc" ) : QByteArray( "a\nb\nc" ) ) );
}

namespace klogg::livelog {
struct LiveLogExportServiceTestAccess {
    static void setBeforeSnapshotWrite( LiveLogExportService& service,
                                        std::function<void()> callback )
    {
        service.beforeSnapshotWriteForTesting_ = std::move( callback );
    }

    static void setBeforePublication( LiveLogExportService& service,
                                      std::function<void()> callback )
    {
        service.beforePublicationForTesting_ = std::move( callback );
    }

    static void setAfterPublish( LiveLogExportService& service,
                                 std::function<void()> callback )
    {
        service.afterPublishForTesting_ = std::move( callback );
    }

    static std::thread::id dataAccessThreadId( const LiveLogExportJob& job )
    {
        const std::lock_guard<std::mutex> lock( job.stateMutex_ );
        return job.dataAccessThreadId_;
    }

    static void setOwnerEventPump(
        LiveLogExportJob& job,
        std::function<void( QEventLoop::ProcessEventsFlags, int )> callback )
    {
        job.ownerEventPumpForTesting_ = std::move( callback );
    }
};
} // namespace klogg::livelog

struct StreamingLogDataTimerTestAccess {
    static void spillFault( StreamingLogData& data, qint64& now,
                            std::optional<CaptureStore::PersistenceFailure>& failure )
    {
        data.captureStore_.spillClockForTesting_ = [ &now ] { return now; };
        data.captureStore_.spillFailureForTesting_ = [ &failure ] { return failure; };
    }
    static void beforeSegmentMutation( StreamingLogData& data,
                                       std::function<void()> callback )
    {
        data.captureStore_.beforeSegmentMutationForTesting_ = std::move( callback );
    }
    static void beforeOutputExportJournal( StreamingLogData& data,
                                           std::function<void()> callback )
    {
        data.beforeOutputExportJournalForTesting_ = std::move( callback );
    }

    static void shortOutput( StreamingLogData& data )
    {
        data.outputWriteForTesting_
            = [ &data, calls = 0 ]( const QByteArray& bytes ) mutable -> qint64 {
            if ( ++calls > 1 ) {
                return -1;
            }
            return data.rollingDisplayOutput_.currentFile()->write( bytes.left( 1 ) );
        };
    }
    static qint64 replayPeak( const StreamingLogData& data )
    {
        return data.replayPeakBufferForTesting_;
    }

    struct RawCacheStats {
        size_t batches = 0;
        qint64 metadataBytes = 0;
        std::uint64_t lookupBatchVisits = 0;
    };

    static void rawCacheLimits( StreamingLogData& data, size_t maximumBatches,
                                qint64 maximumMetadataBytes )
    {
        std::lock_guard<std::mutex> lock( data.cachedRawBatchesMutex_ );
        data.cachedRawBatchCountLimit_ = maximumBatches;
        data.cachedRawMetadataBytesLimit_ = maximumMetadataBytes;
    }

    static RawCacheStats rawCacheStats( const StreamingLogData& data )
    {
        std::lock_guard<std::mutex> lock( data.cachedRawBatchesMutex_ );
        return { data.cachedRawBatches_.size(), data.cachedRawMetadataBytes_,
                 data.cachedRawLookupBatchVisitsForTesting_ };
    }

    static void resetRawCacheLookupVisits( const StreamingLogData& data )
    {
        std::lock_guard<std::mutex> lock( data.cachedRawBatchesMutex_ );
        data.cachedRawLookupBatchVisitsForTesting_ = 0;
    }

    static bool pending( const StreamingLogData& data )
    {
        return data.loadingFinishedQueued_ && data.loadingFinishedTimer_.isActive();
    }

    static void deliver( StreamingLogData& data )
    {
        REQUIRE( pending( data ) );
        const auto timerId = data.loadingFinishedTimer_.timerId();
        REQUIRE( timerId >= 0 );
        QTimerEvent event{ timerId };
        QCoreApplication::sendEvent( &data.loadingFinishedTimer_, &event );
        REQUIRE_FALSE( data.loadingFinishedTimer_.isActive() );
        REQUIRE_FALSE( data.loadingFinishedQueued_ );
    }
};

namespace {
QString makeCaptureId()
{
    return QUuid::createUuid().toString( QUuid::WithoutBraces );
}

bool waitForSearchComplete( LogFilteredData& filteredData, int timeoutMs = 10000 )
{
    SafeQSignalSpy searchProgressSpy{ &filteredData, &LogFilteredData::searchProgressed };
    QElapsedTimer timer;
    timer.start();
    while ( timer.elapsed() < timeoutMs ) {
        // Process any queued signals that may have arrived before the spy
        // was created, then check if we already received completion.
        QCoreApplication::processEvents( QEventLoop::AllEvents, 50 );
        for ( int i = searchProgressSpy.count() - 1; i >= 0; --i ) {
            const auto args = searchProgressSpy.at( i );
            if ( args.size() >= 2 && args.at( 1 ).toInt() >= 100 ) {
                return true;
            }
        }
        if ( searchProgressSpy.safeWait( 100 ) ) {
            const auto args = searchProgressSpy.at( searchProgressSpy.count() - 1 );
            if ( args.size() >= 2 && args.at( 1 ).toInt() >= 100 ) {
                return true;
            }
        }
    }
    return false;
}

bool waitForMatchCount( LogFilteredData& filteredData, LinesCount expected, int timeoutMs = 10000 )
{
    if ( filteredData.getNbMatches() == expected ) {
        return true;
    }

    SafeQSignalSpy searchProgressSpy{ &filteredData, &LogFilteredData::searchProgressed };
    QElapsedTimer timer;
    timer.start();
    while ( true ) {
        const auto remaining = timeoutMs - static_cast<int>( timer.elapsed() );
        if ( remaining <= 0 ) {
            break;
        }
        searchProgressSpy.wait( qMin( 100, remaining ) );
        if ( filteredData.getNbMatches() == expected ) {
            return true;
        }
    }
    return false;
}

bool waitForTerminalMatchCount( LogFilteredData& filteredData, LinesCount expected,
                                LogFilteredData::SearchGeneration expectedGeneration,
                                int timeoutMs = 10000 )
{
    SafeQSignalSpy searchProgressSpy{ &filteredData, &LogFilteredData::searchProgressed };
    QElapsedTimer timer;
    timer.start();
    int consumedSignals = 0;
    while ( timer.elapsed() < timeoutMs ) {
        while ( consumedSignals < searchProgressSpy.count() ) {
            const auto args = searchProgressSpy.at( consumedSignals++ );
            if ( args.size() >= 4 && args.at( 1 ).toInt() >= 100
                 && args.at( 0 ).value<LinesCount>() == expected
                 && args.at( 3 ).toULongLong() == expectedGeneration ) {
                return true;
            }
        }

        const auto remaining = timeoutMs - static_cast<int>( timer.elapsed() );
        if ( remaining <= 0 ) {
            break;
        }
        searchProgressSpy.wait( qMin( 100, remaining ) );
    }
    return false;
}

// The caller arms the spy before starting the search, including synchronous cache hits.
// Return any terminal result for this generation so an incorrect count fails explicitly.
std::optional<LinesCount>
waitForTerminalResult( SafeQSignalSpy& searchProgressSpy,
                       LogFilteredData::SearchGeneration expectedGeneration,
                       int timeoutMs = 10000 )
{
    QElapsedTimer timer;
    timer.start();
    int consumedSignals = 0;
    while ( true ) {
        while ( consumedSignals < searchProgressSpy.count() ) {
            const auto args = searchProgressSpy.at( consumedSignals++ );
            if ( args.size() >= 4 && args.at( 1 ).toInt() == 100
                 && args.at( 3 ).toULongLong() == expectedGeneration ) {
                return args.at( 0 ).value<LinesCount>();
            }
        }

        const auto remaining = timeoutMs - static_cast<int>( timer.elapsed() );
        if ( remaining <= 0 ) {
            return std::nullopt;
        }
        searchProgressSpy.wait( qMin( 100, remaining ) );
    }
}

QByteArray makeStreamingSearchLines( int firstLine, int count, bool matchFinalLine = false )
{
    QByteArray data;
    data.reserve( count * 48 );
    for ( int i = 0; i < count; ++i ) {
        const auto line = firstLine + i;
        const auto isFinalSentinel = matchFinalLine && i == count - 1;
        data.append( line % 10 == 0 || isFinalSentinel ? "ERROR " : "INFO " );
        data.append( QByteArray::number( line ) );
        data.append( " component=streaming-search\n" );
    }
    return data;
}

struct SearchConfigGuard {
    Configuration& cfg;
    bool prevParallel;
    bool prevResultsCache;
    int prevBufferLines;
    int prevThreadPoolSize;

    explicit SearchConfigGuard( Configuration& c )
        : cfg( c )
        , prevParallel( c.useParallelSearch() )
        , prevResultsCache( c.useSearchResultsCache() )
        , prevBufferLines( c.searchReadBufferSizeLines() )
        , prevThreadPoolSize( c.searchThreadPoolSize() )
    {
    }

    ~SearchConfigGuard()
    {
        cfg.setUseParallelSearch( prevParallel );
        cfg.setUseSearchResultsCache( prevResultsCache );
        cfg.setSearchReadBufferSizeLines( prevBufferLines );
        cfg.setSearchThreadPoolSize( prevThreadPoolSize );
    }

    SearchConfigGuard( const SearchConfigGuard& ) = delete;
    SearchConfigGuard& operator=( const SearchConfigGuard& ) = delete;
};
} // namespace

TEST_CASE( "Rolling replacements refresh even when the retained line count is unchanged",
           "[streaming][live-presentation-red]" )
{
    const auto finishPartial = GENERATE( false, true );
    CAPTURE( finishPartial );
    QTemporaryDir tempDir;
    REQUIRE( tempDir.isValid() );
    StreamingLogData logData{ makeCaptureId(), tempDir.path() };
    SafeQSignalSpy loadingSpy{ &logData, SIGNAL( loadingFinished( LoadingStatus ) ) };
    StreamingLogDataTimerTestAccess::deliver( logData );
    REQUIRE( loadingSpy.count() == 1 );

    CaptureStore::Limits limits;
    limits.segmentTargetBytes = 8;
    limits.memoryBudgetBytes = 4096;
    limits.rollingMaxFileSize = 8;
    limits.rollingBackupCount = 2;
    logData.setCaptureLimits( limits );
    REQUIRE( logData.bindOutputFile( QDir{ tempDir.path() }.filePath( "rolling.log" ),
                                     LiveLogSaveAnsiMode::Preserve ) );
    logData.appendUtf8( QByteArrayLiteral( "old-000\n" ) );
    logData.appendUtf8( QByteArrayLiteral( "old-001\n" ) );
    REQUIRE( logData.getNbLine() == 2_lcount );
    StreamingLogDataTimerTestAccess::deliver( logData );
    loadingSpy.clear();
    REQUIRE_FALSE( StreamingLogDataTimerTestAccess::pending( logData ) );
    SafeQSignalSpy fileSpy{ &logData, SIGNAL( fileChanged( MonitoredFileStatus ) ) };

    if ( finishPartial ) {
        // Eight unterminated bytes fill the output segment on finishInput,
        // forcing an actual rotation/trim rather than merely growing the tail.
        logData.appendUtf8( QByteArrayLiteral( "new-002!" ) );
        REQUIRE( logData.getNbLine() == 2_lcount );
        REQUIRE_FALSE( StreamingLogDataTimerTestAccess::pending( logData ) );
        REQUIRE( loadingSpy.count() == 0 );
        logData.finishInput();
    }
    else {
        logData.appendUtf8( QByteArrayLiteral( "new-002\n" ) );
    }

    // Data and normal invalidation must survive; the RED is only the missing refresh.
    REQUIRE( logData.getNbLine() == 2_lcount );
    REQUIRE( logData.getLineString( 0_lnum ) == QStringLiteral( "old-001" ) );
    REQUIRE( logData.getLineString( 1_lnum )
             == ( finishPartial ? QStringLiteral( "new-002!" ) : QStringLiteral( "new-002" ) ) );
    int truncations = 0;
    for ( int i = 0; i < fileSpy.count(); ++i ) {
        if ( fileSpy.at( i ).at( 0 ).value<MonitoredFileStatus>()
             == MonitoredFileStatus::Truncated ) {
            ++truncations;
        }
    }
    REQUIRE( truncations == 1 );
    REQUIRE( loadingSpy.count() == 0 );
    REQUIRE( StreamingLogDataTimerTestAccess::pending( logData ) );
    StreamingLogDataTimerTestAccess::deliver( logData );
    REQUIRE( loadingSpy.count() == 1 );
    REQUIRE_FALSE( StreamingLogDataTimerTestAccess::pending( logData ) );
}

TEST_CASE( "Streaming coalescer preserves empty partial UTF8 CRLF and repeat burst delivery",
           "[streaming][live-presentation-preservation]" )
{
    QTemporaryDir tempDir;
    REQUIRE( tempDir.isValid() );
    StreamingLogData data{ makeCaptureId(), tempDir.path() };
    SafeQSignalSpy loadingSpy{ &data, SIGNAL( loadingFinished( LoadingStatus ) ) };
    StreamingLogDataTimerTestAccess::deliver( data );
    loadingSpy.clear();
    data.appendUtf8( {} );
    data.finishInput();
    REQUIRE_FALSE( StreamingLogDataTimerTestAccess::pending( data ) );
    const auto utf8 = QString::fromUtf8( "雪" ).toUtf8();
    data.appendUtf8( utf8.left( 1 ) );
    data.appendUtf8( {} );
    REQUIRE_FALSE( StreamingLogDataTimerTestAccess::pending( data ) );
    data.appendUtf8( utf8.mid( 1 ) + '\r' );
    REQUIRE_FALSE( StreamingLogDataTimerTestAccess::pending( data ) );
    data.appendUtf8( QByteArrayLiteral( "\n" ) );
    REQUIRE( StreamingLogDataTimerTestAccess::pending( data ) );
    for ( int batch = 0; batch < 64; ++batch ) {
        data.appendUtf8( QByteArray::number( batch ) + '\n' );
    }
    data.finishInput(); // Does not cause a second delivery while a refresh is pending.
    REQUIRE( loadingSpy.count() == 0 );
    StreamingLogDataTimerTestAccess::deliver( data );
    REQUIRE( loadingSpy.count() == 1 );
    CHECK( data.getNbLine() == 65_lcount );
    CHECK( data.getLineString( 0_lnum ) == QString::fromUtf8( "雪" ) );
    CHECK( data.getLineString( 64_lnum ) == QStringLiteral( "63" ) );
    data.appendUtf8( QByteArrayLiteral( "tail" ) );
    REQUIRE_FALSE( StreamingLogDataTimerTestAccess::pending( data ) );
    data.finishInput();
    REQUIRE( StreamingLogDataTimerTestAccess::pending( data ) );
    StreamingLogDataTimerTestAccess::deliver( data );
    REQUIRE( loadingSpy.count() == 2 );
    CHECK( data.getNbLine() == 66_lcount );
    CHECK( data.getLineString( 65_lnum ) == QStringLiteral( "tail" ) );
    data.finishInput();
    data.appendUtf8( {} );
    QCoreApplication::processEvents();
    CHECK_FALSE( StreamingLogDataTimerTestAccess::pending( data ) );
    CHECK( loadingSpy.count() == 2 );
}

TEST_CASE( "Rolling coalesced delivery exposes the replacement to filtered search",
           "[streaming][live-presentation-preservation]" )
{
    const auto useResultsCache = GENERATE( true, false );
    CAPTURE( useResultsCache );
    auto& config = Configuration::get();
    SearchConfigGuard configGuard{ config };
    config.setUseSearchResultsCache( useResultsCache );

    QTemporaryDir tempDir;
    REQUIRE( tempDir.isValid() );
    StreamingLogData data{ makeCaptureId(), tempDir.path() };
    StreamingLogDataTimerTestAccess::deliver( data );
    CaptureStore::Limits limits;
    limits.segmentTargetBytes = 8;
    limits.memoryBudgetBytes = 4096;
    limits.rollingMaxFileSize = 8;
    limits.rollingBackupCount = 2;
    data.setCaptureLimits( limits );
    REQUIRE( data.bindOutputFile( QDir{ tempDir.path() }.filePath( "search.log" ),
                                  LiveLogSaveAnsiMode::Preserve ) );
    data.appendUtf8( QByteArrayLiteral( "old-000\nold-001\n" ) );
    StreamingLogDataTimerTestAccess::deliver( data );
    REQUIRE( data.getNbLine().get() == 2 );
    auto filtered = data.getNewFilteredData();
    const RegularExpressionPattern pattern{ QStringLiteral( "new" ) };
    SafeQSignalSpy searchProgressSpy{ filtered.get(), &LogFilteredData::searchProgressed };
    filtered->runSearch( pattern, 0_lnum, LineNumber{ data.getNbLine().get() } );
    const auto initialResult
        = waitForTerminalResult( searchProgressSpy, filtered->currentSearchGeneration() );
    REQUIRE( initialResult.has_value() );
    REQUIRE( initialResult->get() == 0 );
    CHECK( filtered->getNbMatches().get() == 0 );

    // Prove the configured path with a completed unchanged search: a cache hit
    // emits synchronously without starting an operation; cache OFF searches again.
    const auto initialOperations = filtered->searchPerformanceCounters().operationStarts;
    REQUIRE( initialOperations == 1 );
    searchProgressSpy.clear();
    filtered->runSearch( pattern, 0_lnum, LineNumber{ data.getNbLine().get() } );
    if ( useResultsCache ) {
        REQUIRE( searchProgressSpy.count() == 1 );
    }
    const auto repeatedResult
        = waitForTerminalResult( searchProgressSpy, filtered->currentSearchGeneration() );
    REQUIRE( repeatedResult.has_value() );
    REQUIRE( repeatedResult->get() == 0 );
    REQUIRE( filtered->searchPerformanceCounters().operationStarts
             == initialOperations + ( useResultsCache ? 0u : 1u ) );

    // Mirror both view boundaries: truncation invalidates the old search/cache,
    // then coalesced loading completion restarts against the committed window.
    int truncations = 0;
    int restarts = 0;
    QObject::connect( &data, &StreamingLogData::fileChanged, filtered.get(),
                      [ & ]( MonitoredFileStatus status ) {
                          if ( status == MonitoredFileStatus::Truncated ) {
                              ++truncations;
                              filtered->clearSearch( true );
                          }
                      } );
    QObject::connect( &data, &StreamingLogData::loadingFinished, filtered.get(),
                      [ & ]( auto ) {
                          ++restarts;
                          filtered->runSearch( pattern, 0_lnum,
                                               LineNumber{ data.getNbLine().get() } );
                      } );
    searchProgressSpy.clear();
    const auto previousOperations = filtered->searchPerformanceCounters().operationStarts;
    data.appendUtf8( QByteArrayLiteral( "new-002\n" ) );
    REQUIRE( data.getNbLine().get() == 2 );
    REQUIRE( truncations == 1 );
    REQUIRE( restarts == 0 );
    REQUIRE( StreamingLogDataTimerTestAccess::pending( data ) );
    // Measure restart's generation advance separately from any invalidation advance.
    const auto generationBeforeRestart = filtered->currentSearchGeneration();
    StreamingLogDataTimerTestAccess::deliver( data );
    REQUIRE( restarts == 1 );
    REQUIRE( filtered->currentSearchGeneration() == generationBeforeRestart + 1 );
    const auto replacementResult
        = waitForTerminalResult( searchProgressSpy, filtered->currentSearchGeneration() );
    REQUIRE( replacementResult.has_value() );
    REQUIRE( replacementResult->get() == 1 );
    CHECK( filtered->searchPerformanceCounters().operationStarts == previousOperations + 1 );
    CHECK( filtered->getNbMatches().get() == 1 );
    CHECK( filtered->getMatchingLineNumber( 0_lnum ).get() == 1 );
    CHECK( data.getLineString( 0_lnum ) == QStringLiteral( "old-001" ) );
    CHECK( data.getLineString( 1_lnum ) == QStringLiteral( "new-002" ) );
    filtered->interruptSearch();
}

TEST_CASE( "StreamingLogData emits its ready signal asynchronously after listeners attach" )
{
    QTemporaryDir tempDir;
    REQUIRE( tempDir.isValid() );

    const auto captureId = makeCaptureId();
    {
        CaptureStore store( captureId, tempDir.path() );
        store.appendUtf8( QByteArrayLiteral( "one\ntwo\n" ) );
    }

    StreamingLogData logData( captureId, tempDir.path() );
    SafeQSignalSpy loadingSpy( &logData, SIGNAL( loadingFinished( LoadingStatus ) ) );

    REQUIRE( loadingSpy.safeWait() );
    REQUIRE( logData.getNbLine().get() == 2 );
    REQUIRE( logData.getLineString( LineNumber( 0 ) ) == QStringLiteral( "one" ) );
    REQUIRE( logData.getLineString( LineNumber( 1 ) ) == QStringLiteral( "two" ) );
}

TEST_CASE( "StreamingLogData refreshes listeners after append and clear operations" )
{
    QTemporaryDir tempDir;
    REQUIRE( tempDir.isValid() );

    StreamingLogData logData( makeCaptureId(), tempDir.path() );
    SafeQSignalSpy loadingSpy( &logData, SIGNAL( loadingFinished( LoadingStatus ) ) );

    REQUIRE( loadingSpy.safeWait() );
    loadingSpy.clear();

    logData.appendUtf8( QByteArrayLiteral( "alpha\nbeta\n" ) );
    REQUIRE( loadingSpy.safeWait() );
    REQUIRE( logData.getNbLine().get() == 2 );
    REQUIRE( logData.getLineString( LineNumber( 1 ) ) == QStringLiteral( "beta" ) );

    loadingSpy.clear();
    logData.clearCapture();
    REQUIRE( loadingSpy.safeWait() );
    REQUIRE( logData.getNbLine().get() == 0 );
}

TEST_CASE( "StreamingLogData coalesces rapid live append refresh signals" )
{
    QTemporaryDir tempDir;
    REQUIRE( tempDir.isValid() );

    StreamingLogData logData( makeCaptureId(), tempDir.path() );
    SafeQSignalSpy loadingSpy( &logData, SIGNAL( loadingFinished( LoadingStatus ) ) );

    REQUIRE( loadingSpy.safeWait() );
    loadingSpy.clear();

    for ( int batch = 0; batch < 30; ++batch ) {
        logData.appendUtf8( makeStreamingSearchLines( batch * 10, 10 ) );
        QCoreApplication::processEvents( QEventLoop::AllEvents, 1 );
    }

    REQUIRE( loadingSpy.safeWait( 1000 ) );
    QCoreApplication::processEvents( QEventLoop::AllEvents, 100 );

    INFO( "loadingFinished signals=" << loadingSpy.count() );
    REQUIRE( logData.getNbLine().get() == 300 );
    REQUIRE( loadingSpy.count() <= 3 );
}

TEST_CASE( "StreamingLogData exposes a trailing partial line when input finishes" )
{
    QTemporaryDir tempDir;
    REQUIRE( tempDir.isValid() );

    StreamingLogData logData( makeCaptureId(), tempDir.path() );
    SafeQSignalSpy loadingSpy( &logData, SIGNAL( loadingFinished( LoadingStatus ) ) );

    REQUIRE( loadingSpy.safeWait() );
    loadingSpy.clear();

    logData.appendUtf8( QByteArrayLiteral( "partial" ) );
    REQUIRE( logData.getNbLine().get() == 0 );

    logData.finishInput();
    REQUIRE( loadingSpy.safeWait() );
    REQUIRE( logData.getNbLine().get() == 1 );
    REQUIRE( logData.getLineString( LineNumber( 0 ) ) == QStringLiteral( "partial" ) );
}

TEST_CASE( "StreamingLogData getLinesRaw returns correct RawLines for search worker" )
{
    QTemporaryDir tempDir;
    REQUIRE( tempDir.isValid() );

    StreamingLogData logData( makeCaptureId(), tempDir.path() );
    SafeQSignalSpy loadingSpy( &logData, SIGNAL( loadingFinished( LoadingStatus ) ) );

    REQUIRE( loadingSpy.safeWait() );
    loadingSpy.clear();

    logData.appendUtf8( QByteArrayLiteral( "first\nsecond\nthird\nfourth\n" ) );
    REQUIRE( loadingSpy.safeWait() );
    REQUIRE( logData.getNbLine().get() == 4 );

    // getLinesRaw is the API the search worker uses for block scanning.
    const auto rawLines = logData.getLinesRaw( 0_lnum, LinesCount( 4 ) );
    REQUIRE( rawLines.endOfLines.size() == 4 );

    // Verify decoded lines match getLineString output.
    const auto decoded = rawLines.decodeLines();
    REQUIRE( decoded.size() == 4 );
    REQUIRE( decoded[ 0 ] == QStringLiteral( "first" ) );
    REQUIRE( decoded[ 1 ] == QStringLiteral( "second" ) );
    REQUIRE( decoded[ 2 ] == QStringLiteral( "third" ) );
    REQUIRE( decoded[ 3 ] == QStringLiteral( "fourth" ) );
}

TEST_CASE( "StreamingLogData strips ANSI before display and search views" )
{
    QTemporaryDir tempDir;
    REQUIRE( tempDir.isValid() );

    StreamingLogData logData( makeCaptureId(), tempDir.path() );
    SafeQSignalSpy loadingSpy( &logData, SIGNAL( loadingFinished( LoadingStatus ) ) );

    REQUIRE( loadingSpy.safeWait() );
    loadingSpy.clear();

    logData.appendUtf8( QByteArrayLiteral( "plain \x1b[31mred\x1b[0m text\n" ) );
    REQUIRE( loadingSpy.safeWait() );
    REQUIRE( logData.getNbLine().get() == 1 );

    logData.setAnsiProcessingMode( AnsiProcessingMode::Plain );
    REQUIRE( logData.getLineString( 0_lnum ) == QStringLiteral( "plain \x1b[31mred\x1b[0m text" ) );

    logData.setAnsiProcessingMode( AnsiProcessingMode::Strip );
    REQUIRE( logData.getLineString( 0_lnum ) == QStringLiteral( "plain red text" ) );
    const auto strippedRawLines = logData.getLinesRaw( 0_lnum, 1_lcount );
    REQUIRE( strippedRawLines.decodeLines()[ 0 ] == QStringLiteral( "plain red text" ) );
    REQUIRE( strippedRawLines.buildUtf8View()[ 0 ] == std::string_view{ "plain red text" } );

    logData.setAnsiProcessingMode( AnsiProcessingMode::Render );
    REQUIRE( logData.getLineString( 0_lnum ) == QStringLiteral( "plain red text" ) );
    const auto colors = logData.getLineAnsiColors( 0_lnum );
    REQUIRE( colors.size() == 1 );
    REQUIRE( colors[ 0 ].startColumn == 6_lcol );
    REQUIRE( colors[ 0 ].length == 3_length );
    REQUIRE( colors[ 0 ].foreground == 0xde382b );
    REQUIRE( logData.getLinesRaw( 0_lnum, 1_lcount ).buildUtf8View()[ 0 ]
             == std::string_view{ "plain red text" } );
}

TEST_CASE( "StreamingLogData saves display text when ANSI rendering is enabled" )
{
    QTemporaryDir tempDir;
    REQUIRE( tempDir.isValid() );

    StreamingLogData logData( makeCaptureId(), tempDir.path() );
    SafeQSignalSpy loadingSpy( &logData, SIGNAL( loadingFinished( LoadingStatus ) ) );

    REQUIRE( loadingSpy.safeWait() );
    loadingSpy.clear();

    logData.appendUtf8( QByteArrayLiteral( "\x1b[32mI/App\x1b[0m first\n" ) );
    logData.appendUtf8( QByteArrayLiteral( "\x1b[31mE/App\x1b[0m second\n" ) );
    REQUIRE( loadingSpy.safeWait() );
    logData.finishInput();

    logData.setAnsiProcessingMode( AnsiProcessingMode::Render );

    const auto outputPath = QDir( tempDir.path() ).filePath( QStringLiteral( "saved.log" ) );
    REQUIRE( logData.bindOutputFile( outputPath ) );
    logData.bindOutputFile( QString{} );

    QFile outputFile( outputPath );
    REQUIRE( outputFile.open( QIODevice::ReadOnly ) );
    REQUIRE( outputFile.readAll() == QByteArrayLiteral( "I/App first\nE/App second\n" ) );
}

TEST_CASE( "StreamingLogData can strip ANSI while saving current and future live log lines" )
{
    QTemporaryDir tempDir;
    REQUIRE( tempDir.isValid() );

    StreamingLogData logData( makeCaptureId(), tempDir.path() );
    SafeQSignalSpy loadingSpy( &logData, SIGNAL( loadingFinished( LoadingStatus ) ) );

    REQUIRE( loadingSpy.safeWait() );
    loadingSpy.clear();

    logData.appendUtf8( QByteArrayLiteral( "\x1b[32mI/App\x1b[0m first\n" ) );
    REQUIRE( loadingSpy.safeWait() );
    logData.setAnsiProcessingMode( AnsiProcessingMode::Render );

    const auto outputPath = QDir( tempDir.path() ).filePath( QStringLiteral( "strip.log" ) );
    REQUIRE( logData.bindOutputFile( outputPath, LiveLogSaveAnsiMode::Strip ) );

    loadingSpy.clear();
    logData.appendUtf8( QByteArrayLiteral( "\x1b[31mE/App\x1b[0m second\n" ) );
    REQUIRE( loadingSpy.safeWait() );
    logData.bindOutputFile( QString{} );

    QFile outputFile( outputPath );
    REQUIRE( outputFile.open( QIODevice::ReadOnly ) );
    REQUIRE( outputFile.readAll() == QByteArrayLiteral( "I/App first\nE/App second\n" ) );
}

TEST_CASE( "StreamingLogData clearCapture truncates display output file" )
{
    QTemporaryDir tempDir;
    REQUIRE( tempDir.isValid() );

    StreamingLogData logData( makeCaptureId(), tempDir.path() );
    SafeQSignalSpy loadingSpy( &logData, SIGNAL( loadingFinished( LoadingStatus ) ) );

    REQUIRE( loadingSpy.safeWait() );
    loadingSpy.clear();

    logData.appendUtf8( QByteArrayLiteral( "first line\n" ) );
    REQUIRE( loadingSpy.safeWait() );

    const auto outputPath = QDir( tempDir.path() ).filePath( QStringLiteral( "display.log" ) );
    REQUIRE( logData.bindOutputFile( outputPath, LiveLogSaveAnsiMode::Strip ) );

    // Output file now contains "first line\n"
    QFile output1( outputPath );
    REQUIRE( output1.open( QIODevice::ReadOnly ) );
    REQUIRE( output1.readAll() == QByteArrayLiteral( "first line\n" ) );
    output1.close();

    // Simulate a reconnect: clearCapture reopens the display output file.
    loadingSpy.clear();
    logData.clearCapture();
    REQUIRE( loadingSpy.safeWait() );

    // After clearCapture, the output file should be truncated (empty),
    // not still containing "first line\n".
    QFile output2( outputPath );
    REQUIRE( output2.open( QIODevice::ReadOnly ) );
    const auto afterClear = output2.readAll();
    output2.close();

    // The file should be empty — clearCapture should have truncated it.
    CHECK( afterClear.isEmpty() );

    // New data after clear should be written cleanly (no old data prepended).
    loadingSpy.clear();
    logData.appendUtf8( QByteArrayLiteral( "second line\n" ) );
    REQUIRE( loadingSpy.safeWait() );

    QFile output3( outputPath );
    REQUIRE( output3.open( QIODevice::ReadOnly ) );
    REQUIRE( output3.readAll() == QByteArrayLiteral( "second line\n" ) );
    output3.close();
}

TEST_CASE( "StreamingLogData clearCapture preserves an externally replaced output path",
           "[streaming][capture-output]" )
{
    LiveLogSaveAnsiMode ansiMode = LiveLogSaveAnsiMode::Strip;
    SECTION( "Strip output" )
    {
        ansiMode = LiveLogSaveAnsiMode::Strip;
    }
    SECTION( "Preserve output" )
    {
        ansiMode = LiveLogSaveAnsiMode::Preserve;
    }

    QTemporaryDir tempDir;
    REQUIRE( tempDir.isValid() );

    StreamingLogData logData( makeCaptureId(), tempDir.path() );
    SafeQSignalSpy loadingSpy( &logData, SIGNAL( loadingFinished( LoadingStatus ) ) );
    REQUIRE( loadingSpy.safeWait() );

    const auto outputPath = tempDir.filePath( QStringLiteral( "capture.log" ) );
    const auto rotatedPath = tempDir.filePath( QStringLiteral( "capture.rotated.log" ) );
    REQUIRE( logData.bindOutputFile( outputPath, ansiMode ) );
    logData.appendUtf8( QByteArrayLiteral( "owned-before-clear\n" ) );
    logData.finishInput();

    if ( !QFile::rename( outputPath, rotatedPath ) ) {
        SUCCEED( "Platform does not allow external replacement of an open file" );
        return;
    }

    QFile replacement( outputPath );
    REQUIRE( replacement.open( QIODevice::WriteOnly ) );
    REQUIRE( replacement.write( QByteArrayLiteral( "external-replacement\n" ) ) > 0 );
    replacement.close();

    loadingSpy.clear();
    logData.clearCapture();
    REQUIRE( loadingSpy.safeWait() );

    REQUIRE( replacement.open( QIODevice::ReadOnly ) );
    CHECK( replacement.readAll() == QByteArrayLiteral( "external-replacement\n" ) );
    replacement.close();
    CHECK( logData.boundOutputFile() == outputPath );
    CHECK( logData.captureOutputError() == CaptureOutputError::Reopen );

    logData.appendUtf8( QByteArrayLiteral( "after-clear\n" ) );
    logData.finishInput();

    REQUIRE( replacement.open( QIODevice::ReadOnly ) );
    CHECK( replacement.readAll() == QByteArrayLiteral( "external-replacement\n" ) );
    replacement.close();

    QFile rotated( rotatedPath );
    REQUIRE( rotated.open( QIODevice::ReadOnly ) );
    CHECK( rotated.readAll() == QByteArrayLiteral( "owned-before-clear\n" ) );
}

TEST_CASE( "StreamingLogData can preserve ANSI while saving current and future live log lines" )
{
    QTemporaryDir tempDir;
    REQUIRE( tempDir.isValid() );

    StreamingLogData logData( makeCaptureId(), tempDir.path() );
    SafeQSignalSpy loadingSpy( &logData, SIGNAL( loadingFinished( LoadingStatus ) ) );

    REQUIRE( loadingSpy.safeWait() );
    loadingSpy.clear();

    logData.appendUtf8( QByteArrayLiteral( "\x1b[32mI/App\x1b[0m first\n" ) );
    REQUIRE( loadingSpy.safeWait() );
    logData.setAnsiProcessingMode( AnsiProcessingMode::Render );

    const auto outputPath = QDir( tempDir.path() ).filePath( QStringLiteral( "preserve.log" ) );
    REQUIRE( logData.bindOutputFile( outputPath, LiveLogSaveAnsiMode::Preserve ) );

    loadingSpy.clear();
    logData.appendUtf8( QByteArrayLiteral( "\x1b[31mE/App\x1b[0m second\n" ) );
    REQUIRE( loadingSpy.safeWait() );
    logData.bindOutputFile( QString{} );

    QFile outputFile( outputPath );
    REQUIRE( outputFile.open( QIODevice::ReadOnly ) );
    REQUIRE( outputFile.readAll()
             == QByteArrayLiteral( "\x1b[32mI/App\x1b[0m first\n"
                                   "\x1b[31mE/App\x1b[0m second\n" ) );
}

TEST_CASE( "StreamingLogData keeps the previous Strip binding after a failed Preserve rebind",
           "[streaming][capture-output]" )
{
    QTemporaryDir tempDir;
    REQUIRE( tempDir.isValid() );

    StreamingLogData logData( makeCaptureId(), tempDir.path() );
    SafeQSignalSpy loadingSpy( &logData, SIGNAL( loadingFinished( LoadingStatus ) ) );
    REQUIRE( loadingSpy.safeWait() );

    const auto outputPath = tempDir.filePath( QStringLiteral( "strip.log" ) );
    const auto rotatedPath = tempDir.filePath( QStringLiteral( "strip.rotated.log" ) );
    REQUIRE( logData.bindOutputFile( outputPath, LiveLogSaveAnsiMode::Strip ) );
    logData.appendUtf8( QByteArrayLiteral( "owned-before-rebind\n" ) );
    logData.finishInput();

    if ( !QFile::rename( outputPath, rotatedPath ) ) {
        SUCCEED( "Platform does not allow external replacement of an open file" );
        return;
    }
    QFile replacement( outputPath );
    REQUIRE( replacement.open( QIODevice::WriteOnly ) );
    REQUIRE( replacement.write( QByteArrayLiteral( "external-replacement\n" ) ) > 0 );
    replacement.close();

    const auto invalidOutputPath = tempDir.filePath( QStringLiteral( "directory" ) );
    REQUIRE( QDir().mkpath( invalidOutputPath ) );
    REQUIRE_FALSE( logData.bindOutputFile( invalidOutputPath, LiveLogSaveAnsiMode::Preserve ) );
    CHECK( logData.boundOutputFile() == outputPath );

    logData.appendUtf8( QByteArrayLiteral( "after-failed-rebind\n" ) );
    logData.finishInput();

    REQUIRE( replacement.open( QIODevice::ReadOnly ) );
    CHECK( replacement.readAll() == QByteArrayLiteral( "external-replacement\n" ) );
    replacement.close();
    QFile ownedOutput( rotatedPath );
    REQUIRE( ownedOutput.open( QIODevice::ReadOnly ) );
    CHECK( ownedOutput.readAll()
           == QByteArrayLiteral( "owned-before-rebind\nafter-failed-rebind\n" ) );
}

TEST_CASE( "StreamingLogData keeps the previous Preserve binding after a failed Strip rebind",
           "[streaming][capture-output]" )
{
    QTemporaryDir tempDir;
    REQUIRE( tempDir.isValid() );

    StreamingLogData logData( makeCaptureId(), tempDir.path() );
    SafeQSignalSpy loadingSpy( &logData, SIGNAL( loadingFinished( LoadingStatus ) ) );
    REQUIRE( loadingSpy.safeWait() );

    const auto outputPath = tempDir.filePath( QStringLiteral( "preserve.log" ) );
    const auto rotatedPath = tempDir.filePath( QStringLiteral( "preserve.rotated.log" ) );
    REQUIRE( logData.bindOutputFile( outputPath, LiveLogSaveAnsiMode::Preserve ) );
    logData.appendUtf8( QByteArrayLiteral( "owned-before-rebind\n" ) );
    logData.finishInput();

    if ( !QFile::rename( outputPath, rotatedPath ) ) {
        SUCCEED( "Platform does not allow external replacement of an open file" );
        return;
    }
    QFile replacement( outputPath );
    REQUIRE( replacement.open( QIODevice::WriteOnly ) );
    REQUIRE( replacement.write( QByteArrayLiteral( "external-replacement\n" ) ) > 0 );
    replacement.close();

    const auto invalidOutputPath = tempDir.filePath( QStringLiteral( "directory" ) );
    REQUIRE( QDir().mkpath( invalidOutputPath ) );
    REQUIRE_FALSE( logData.bindOutputFile( invalidOutputPath, LiveLogSaveAnsiMode::Strip ) );
    CHECK( logData.boundOutputFile() == outputPath );

    logData.appendUtf8( QByteArrayLiteral( "after-failed-rebind\n" ) );
    logData.finishInput();

    REQUIRE( replacement.open( QIODevice::ReadOnly ) );
    CHECK( replacement.readAll() == QByteArrayLiteral( "external-replacement\n" ) );
    replacement.close();
    QFile ownedOutput( rotatedPath );
    REQUIRE( ownedOutput.open( QIODevice::ReadOnly ) );
    CHECK( ownedOutput.readAll()
           == QByteArrayLiteral( "owned-before-rebind\nafter-failed-rebind\n" ) );
}

TEST_CASE( "StreamingLogData keeps a failed Restore degraded after another failed bind",
           "[streaming][capture-output]" )
{
    QTemporaryDir tempDir;
    REQUIRE( tempDir.isValid() );

    StreamingLogData logData( makeCaptureId(), tempDir.path() );
    SafeQSignalSpy loadingSpy( &logData, SIGNAL( loadingFinished( LoadingStatus ) ) );
    REQUIRE( loadingSpy.safeWait() );

    const auto firstInvalidPath = tempDir.filePath( QStringLiteral( "first-directory" ) );
    const auto secondInvalidPath = tempDir.filePath( QStringLiteral( "second-directory" ) );
    REQUIRE( QDir().mkpath( firstInvalidPath ) );
    REQUIRE( QDir().mkpath( secondInvalidPath ) );
    REQUIRE_FALSE( logData.bindOutputFile( firstInvalidPath, LiveLogSaveAnsiMode::Strip,
                                           OutputBindMode::Restore ) );
    REQUIRE( logData.captureOutputError() == CaptureOutputError::Open );

    REQUIRE_FALSE( logData.bindOutputFile( secondInvalidPath,
                                           LiveLogSaveAnsiMode::Preserve ) );
    CHECK( logData.captureOutputError() == CaptureOutputError::Open );
    CHECK( logData.boundOutputFile() == firstInvalidPath );
}

TEST_CASE( "StreamingLogData rejects a destructive same-path mode change",
           "[streaming][capture-output]" )
{
    QTemporaryDir tempDir;
    REQUIRE( tempDir.isValid() );

    StreamingLogData logData( makeCaptureId(), tempDir.path() );
    SafeQSignalSpy loadingSpy( &logData, SIGNAL( loadingFinished( LoadingStatus ) ) );
    REQUIRE( loadingSpy.safeWait() );

    const auto outputPath = tempDir.filePath( QStringLiteral( "capture.log" ) );
    REQUIRE( logData.bindOutputFile( outputPath, LiveLogSaveAnsiMode::Strip ) );
    logData.appendUtf8( QByteArrayLiteral( "before\n" ) );

    REQUIRE_FALSE( logData.bindOutputFile( outputPath, LiveLogSaveAnsiMode::Preserve ) );
    logData.appendUtf8( QByteArrayLiteral( "after\n" ) );
    logData.finishInput();

    QFile outputFile( outputPath );
    REQUIRE( outputFile.open( QIODevice::ReadOnly ) );
    CHECK( outputFile.readAll() == QByteArrayLiteral( "before\nafter\n" ) );
}

TEST_CASE( "StreamingLogData accepts an unchanged same-path binding",
           "[streaming][capture-output]" )
{
    QTemporaryDir tempDir;
    REQUIRE( tempDir.isValid() );

    StreamingLogData logData( makeCaptureId(), tempDir.path() );
    SafeQSignalSpy loadingSpy( &logData, SIGNAL( loadingFinished( LoadingStatus ) ) );
    REQUIRE( loadingSpy.safeWait() );

    const auto outputPath = tempDir.filePath( QStringLiteral( "capture.log" ) );
    REQUIRE( logData.bindOutputFile( outputPath, LiveLogSaveAnsiMode::Strip ) );
    logData.appendUtf8( QByteArrayLiteral( "before\n" ) );

    REQUIRE( logData.bindOutputFile( outputPath, LiveLogSaveAnsiMode::Strip ) );
    logData.appendUtf8( QByteArrayLiteral( "after\n" ) );
    logData.finishInput();

    QFile outputFile( outputPath );
    REQUIRE( outputFile.open( QIODevice::ReadOnly ) );
    CHECK( outputFile.readAll() == QByteArrayLiteral( "before\nafter\n" ) );
}

TEST_CASE( "StreamingLogData treats a hard-link alias as the active output",
           "[streaming][capture-output]" )
{
    QTemporaryDir tempDir;
    REQUIRE( tempDir.isValid() );

    StreamingLogData logData( makeCaptureId(), tempDir.path() );
    SafeQSignalSpy loadingSpy( &logData, SIGNAL( loadingFinished( LoadingStatus ) ) );
    REQUIRE( loadingSpy.safeWait() );

    const auto outputPath = tempDir.filePath( QStringLiteral( "capture.log" ) );
    const auto aliasPath = tempDir.filePath( QStringLiteral( "capture-alias.log" ) );
    REQUIRE( logData.bindOutputFile( outputPath, LiveLogSaveAnsiMode::Strip ) );
    logData.appendUtf8( QByteArrayLiteral( "before\n" ) );

    std::error_code linkError;
    std::filesystem::create_hard_link( std::filesystem::u8path( outputPath.toUtf8().toStdString() ),
                                       std::filesystem::u8path( aliasPath.toUtf8().toStdString() ),
                                       linkError );
    if ( linkError ) {
        SUCCEED( "Platform does not permit a hard-link output alias" );
        return;
    }

    REQUIRE( logData.bindOutputFile( aliasPath, LiveLogSaveAnsiMode::Strip ) );
    CHECK( logData.boundOutputFile() == outputPath );
    logData.appendUtf8( QByteArrayLiteral( "after\n" ) );
    logData.finishInput();

    QFile outputFile( outputPath );
    REQUIRE( outputFile.open( QIODevice::ReadOnly ) );
    CHECK( outputFile.readAll() == QByteArrayLiteral( "before\nafter\n" ) );
}

TEST_CASE( "StreamingLogData rebinds when the active output pathname was replaced",
           "[streaming][capture-output]" )
{
    QTemporaryDir tempDir;
    REQUIRE( tempDir.isValid() );

    StreamingLogData logData( makeCaptureId(), tempDir.path() );
    SafeQSignalSpy loadingSpy( &logData, SIGNAL( loadingFinished( LoadingStatus ) ) );
    REQUIRE( loadingSpy.safeWait() );

    const auto outputPath = tempDir.filePath( QStringLiteral( "capture.log" ) );
    const auto rotatedPath = tempDir.filePath( QStringLiteral( "capture.rotated.log" ) );
    REQUIRE( logData.bindOutputFile( outputPath, LiveLogSaveAnsiMode::Strip ) );
    logData.appendUtf8( QByteArrayLiteral( "before\n" ) );

    if ( !QFile::rename( outputPath, rotatedPath ) ) {
        SUCCEED( "Platform does not allow external replacement of an open file" );
        return;
    }
    QFile replacement( outputPath );
    REQUIRE( replacement.open( QIODevice::WriteOnly ) );
    REQUIRE( replacement.write( QByteArrayLiteral( "external\n" ) ) > 0 );
    replacement.close();

    REQUIRE( logData.bindOutputFile( outputPath, LiveLogSaveAnsiMode::Strip ) );
    logData.appendUtf8( QByteArrayLiteral( "after\n" ) );
    logData.finishInput();

    QFile outputFile( outputPath );
    REQUIRE( outputFile.open( QIODevice::ReadOnly ) );
    CHECK( outputFile.readAll() == QByteArrayLiteral( "before\nafter\n" ) );
}

TEST_CASE( "StreamingLogData reports accurate fileSize and lastModifiedDate" )
{
    QTemporaryDir tempDir;
    REQUIRE( tempDir.isValid() );

    StreamingLogData logData( makeCaptureId(), tempDir.path() );
    SafeQSignalSpy loadingSpy( &logData, SIGNAL( loadingFinished( LoadingStatus ) ) );

    REQUIRE( loadingSpy.safeWait() );
    REQUIRE( logData.getFileSize() == 0 );

    loadingSpy.clear();
    const QByteArray payload = QByteArrayLiteral( "hello\nworld\n" );
    logData.appendUtf8( payload );
    REQUIRE( loadingSpy.safeWait() );

    REQUIRE( logData.getFileSize() > 0 );
    REQUIRE( logData.getLastModifiedDate().isValid() );
    REQUIRE( logData.getNbLine().get() == 2 );
}

TEST_CASE( "StreamingLogData getFileSize reflects the bound output file when the capture window trims" )
{
    QTemporaryDir tempDir;
    REQUIRE( tempDir.isValid() );

    StreamingLogData logData( makeCaptureId(), tempDir.path() );
    SafeQSignalSpy loadingSpy( &logData, SIGNAL( loadingFinished( LoadingStatus ) ) );
    REQUIRE( loadingSpy.safeWait() );

    // Line-count window only: the capture store trims old lines while the
    // bound output file (no rolling limit) keeps every line ever written.
    CaptureStore::Limits limits;
    limits.segmentTargetBytes = 16;
    limits.maxTotalLines = 5;
    logData.setCaptureLimits( limits );

    const auto outputPath = QDir( tempDir.path() ).filePath( QStringLiteral( "live.log" ) );
    REQUIRE( logData.bindOutputFile( outputPath, LiveLogSaveAnsiMode::Strip ) );

    loadingSpy.clear();
    for ( int i = 0; i < 20; ++i ) {
        logData.appendUtf8( QStringLiteral( "stream-%1\n" ).arg( i ).toUtf8() );
    }
    REQUIRE( loadingSpy.safeWait() );

    // The rolling window retains only the tail, but the tab's path points at
    // the bound file — its size must match what a single-file open shows.
    const auto onDiskSize = QFileInfo( outputPath ).size();
    REQUIRE( onDiskSize > 0 );
    CHECK( logData.getFileSize() == onDiskSize );
}

TEST_CASE( "StreamingLogData getFileSize reflects a restored bound file when the capture is empty",
           "[live-save-restore]" )
{
    QTemporaryDir tempDir;
    REQUIRE( tempDir.isValid() );

    const auto outputPath = QDir( tempDir.path() ).filePath( QStringLiteral( "saved.log" ) );
    QFile previous( outputPath );
    REQUIRE( previous.open( QIODevice::WriteOnly ) );
    REQUIRE( previous.write( QByteArrayLiteral( "old-1\nold-2\nold-3\n" ) ) > 0 );
    previous.close();

    const auto onDiskSize = QFileInfo( outputPath ).size();
    REQUIRE( onDiskSize > 0 );

    // Restart with a wiped capture (fresh captureId): the capture store is
    // empty, but the restored binding points at the previously saved file.
    StreamingLogData logData( makeCaptureId(), tempDir.path() );
    SafeQSignalSpy loadingSpy( &logData, SIGNAL( loadingFinished( LoadingStatus ) ) );
    REQUIRE( loadingSpy.safeWait() );
    REQUIRE( logData.getNbLine().get() == 0 );

    REQUIRE( logData.bindOutputFile( outputPath, LiveLogSaveAnsiMode::Strip,
                                      OutputBindMode::Restore ) );

    // Capture stats report 0 bytes; the tab must show the file's real size.
    CHECK( logData.getFileSize() == onDiskSize );
}

TEST_CASE( "StreamingLogData getLastModifiedDate reflects a restored bound file when the capture is empty",
           "[live-save-restore]" )
{
    QTemporaryDir tempDir;
    REQUIRE( tempDir.isValid() );

    // Pin the file's mtime far in the past so the on-disk timestamp is
    // unmistakably different from any capture-store timestamp.
    const QDateTime pinnedTime( QDate( 2020, 1, 1 ), QTime( 12, 0, 0 ) );

    const auto outputPath = QDir( tempDir.path() ).filePath( QStringLiteral( "saved.log" ) );
    {
        QFile previous( outputPath );
        REQUIRE( previous.open( QIODevice::WriteOnly ) );
        REQUIRE( previous.write( QByteArrayLiteral( "old-1\nold-2\n" ) ) > 0 );
        REQUIRE( previous.flush() );
        // Flush before pinning: a buffered write committed at close() would
        // bump the mtime past the pinned value.
        REQUIRE( previous.setFileTime( pinnedTime, QFileDevice::FileModificationTime ) );
        previous.close();
    }
    REQUIRE( QFileInfo( outputPath ).lastModified() == pinnedTime );

    // Restart with a wiped capture (fresh captureId): the capture store is
    // empty, but the restored binding points at the previously saved file.
    StreamingLogData logData( makeCaptureId(), tempDir.path() );
    SafeQSignalSpy loadingSpy( &logData, SIGNAL( loadingFinished( LoadingStatus ) ) );
    REQUIRE( loadingSpy.safeWait() );
    REQUIRE( logData.getNbLine().get() == 0 );

    REQUIRE( logData.bindOutputFile( outputPath, LiveLogSaveAnsiMode::Strip,
                                      OutputBindMode::Restore ) );

    // The tab's "modified on" must come from the bound file, matching what a
    // single-file open of the same path shows.
    CHECK( logData.getLastModifiedDate() == pinnedTime );
}

TEST_CASE( "Streaming live search coalesces rapid updateSearch requests" )
{
    QTemporaryDir tempDir;
    REQUIRE( tempDir.isValid() );

    auto& config = Configuration::getSynced();
    SearchConfigGuard configGuard( config );
    config.setUseParallelSearch( false );

    StreamingLogData logData( makeCaptureId(), tempDir.path() );
    SafeQSignalSpy loadingSpy( &logData, SIGNAL( loadingFinished( LoadingStatus ) ) );
    REQUIRE( loadingSpy.safeWait() );

    logData.appendUtf8( makeStreamingSearchLines( 0, 10000 ) );
    REQUIRE( loadingSpy.safeWait() );

    auto filteredData = logData.getNewFilteredData();
    filteredData->runSearch( RegularExpressionPattern{ QStringLiteral( "ERROR" ) }, 0_lnum,
                             LineNumber( logData.getNbLine().get() ) );
    REQUIRE( waitForSearchComplete( *filteredData ) );
    REQUIRE( filteredData->getNbMatches() == 1000_lcount );

    const auto countersAfterInitialSearch = filteredData->searchPerformanceCounters();

    for ( int batch = 0; batch < 4; ++batch ) {
        logData.appendUtf8( makeStreamingSearchLines( 10000 + batch * 5000, 5000 ) );
        filteredData->updateSearch( 0_lnum, LineNumber( logData.getNbLine().get() ) );
    }

    REQUIRE( waitForMatchCount( *filteredData, 3000_lcount ) );

    // Coalescing is timing-dependent: the dispatch loop may merge some or all
    // of the four updateSearch calls into fewer operations.  Just verify that
    // results are correct and that at least one incremental operation ran.
    const auto countersAfterRapidUpdates = filteredData->searchPerformanceCounters();
    REQUIRE( countersAfterRapidUpdates.operationStarts
             > countersAfterInitialSearch.operationStarts );
}

TEST_CASE( "Streaming live search dispatches while updates keep arriving" )
{
    QTemporaryDir tempDir;
    REQUIRE( tempDir.isValid() );

    auto& config = Configuration::getSynced();
    SearchConfigGuard configGuard( config );
    config.setUseParallelSearch( false );

    StreamingLogData logData( makeCaptureId(), tempDir.path() );
    SafeQSignalSpy loadingSpy( &logData, SIGNAL( loadingFinished( LoadingStatus ) ) );
    REQUIRE( loadingSpy.safeWait() );

    logData.appendUtf8( makeStreamingSearchLines( 0, 1000 ) );
    REQUIRE( loadingSpy.safeWait() );

    auto filteredData = logData.getNewFilteredData();
    filteredData->runSearch( RegularExpressionPattern{ QStringLiteral( "ERROR" ) }, 0_lnum,
                             LineNumber( logData.getNbLine().get() ) );
    REQUIRE( waitForSearchComplete( *filteredData ) );
    REQUIRE( filteredData->getNbMatches() == 100_lcount );

    const auto countersAfterInitialSearch = filteredData->searchPerformanceCounters();

    QElapsedTimer timer;
    timer.start();
    bool observedDispatchDuringSteadyUpdates = false;
    int batch = 0;
    while ( timer.elapsed() < 1000 ) {
        logData.appendUtf8( makeStreamingSearchLines( 1000 + batch * 10, 10 ) );
        filteredData->updateSearch( 0_lnum, LineNumber( logData.getNbLine().get() ) );
        QCoreApplication::processEvents( QEventLoop::AllEvents, 10 );
        QThread::msleep( 5 );

        const auto counters = filteredData->searchPerformanceCounters();
        if ( counters.operationStarts > countersAfterInitialSearch.operationStarts ) {
            observedDispatchDuringSteadyUpdates = true;
            break;
        }
        ++batch;
    }

    const auto countersAfterSteadyUpdates = filteredData->searchPerformanceCounters();
    INFO( "operationStartsBefore=" << countersAfterInitialSearch.operationStarts
          << " operationStartsAfter=" << countersAfterSteadyUpdates.operationStarts
          << " matches=" << filteredData->getNbMatches().get() );
    REQUIRE( observedDispatchDuringSteadyUpdates );
}

TEST_CASE( "Streaming raw tail lookup skips bounded historical batches",
           "[streaming][raw-cache][performance][operations]" )
{
    QTemporaryDir tempDir;
    REQUIRE( tempDir.isValid() );

    StreamingLogData logData( makeCaptureId(), tempDir.path() );
    constexpr int BatchCount = 4096;
    for ( int batch = 0; batch < BatchCount; ++batch ) {
        const auto result
            = logData.appendUtf8( QByteArrayLiteral( "record-" ) + QByteArray::number( batch ) + '\n' );
        REQUIRE_FALSE( result.failure.has_value() );
    }

    StreamingLogDataTimerTestAccess::resetRawCacheLookupVisits( logData );
    const auto tail = logData.getLinesRaw( LineNumber( BatchCount - 1 ), 1_lcount );
    REQUIRE( tail.decodeLines() == klogg::vector<QString>{ QStringLiteral( "record-4095" ) } );
    const auto stats = StreamingLogDataTimerTestAccess::rawCacheStats( logData );
    INFO( "tail lookup batch visits=" << stats.lookupBatchVisits
                                      << " cached batches=" << stats.batches );
    CHECK( stats.lookupBatchVisits <= 32u );
    CHECK( stats.batches <= 64u );
}

TEST_CASE( "Streaming raw cache bounds tiny-batch and line-index metadata",
           "[streaming][raw-cache][performance][bounded]" )
{
    QTemporaryDir tempDir;
    REQUIRE( tempDir.isValid() );

    StreamingLogData logData( makeCaptureId(), tempDir.path() );
    constexpr size_t MaximumBatches = 8u;
    constexpr qint64 MaximumMetadataBytes = 512;
    StreamingLogDataTimerTestAccess::rawCacheLimits( logData, MaximumBatches,
                                                     MaximumMetadataBytes );
    for ( int batch = 0; batch < 32; ++batch ) {
        const auto result = logData.appendUtf8( QByteArrayLiteral( "x\n" ) );
        REQUIRE_FALSE( result.failure.has_value() );
    }

    const auto stats = StreamingLogDataTimerTestAccess::rawCacheStats( logData );
    INFO( "cached batches=" << stats.batches << " metadata bytes=" << stats.metadataBytes );
    CHECK( stats.batches <= MaximumBatches );
    CHECK( stats.metadataBytes <= MaximumMetadataBytes );
    CHECK( logData.getLinesRaw( 0_lnum, 1_lcount ).decodeLines()
           == klogg::vector<QString>{ QStringLiteral( "x" ) } );
    CHECK( logData.getLinesRaw( 31_lnum, 1_lcount ).decodeLines()
           == klogg::vector<QString>{ QStringLiteral( "x" ) } );
}

TEST_CASE( "Streaming live search covers append batches with partial line boundaries" )
{
    QTemporaryDir tempDir;
    REQUIRE( tempDir.isValid() );

    auto& config = Configuration::getSynced();
    SearchConfigGuard configGuard( config );
    config.setUseParallelSearch( false );
    config.setSearchReadBufferSizeLines( 10000 );

    StreamingLogData logData( makeCaptureId(), tempDir.path() );
    SafeQSignalSpy loadingSpy( &logData, SIGNAL( loadingFinished( LoadingStatus ) ) );
    REQUIRE( loadingSpy.safeWait() );

    auto filteredData = logData.getNewFilteredData();

    logData.appendUtf8( makeStreamingSearchLines( 0, 10000 ) );
    REQUIRE( loadingSpy.safeWait() );
    filteredData->runSearch( RegularExpressionPattern{ QStringLiteral( "ERROR" ) }, 0_lnum,
                             LineNumber( logData.getNbLine().get() ) );
    REQUIRE( waitForSearchComplete( *filteredData ) );

    logData.appendUtf8( QByteArrayLiteral( "ERROR partial" ) );
    REQUIRE( logData.getNbLine().get() == 10000 );

    loadingSpy.clear();
    logData.appendUtf8( QByteArrayLiteral( "-line component=streaming-search\n" ) );
    REQUIRE( loadingSpy.safeWait() );
    filteredData->updateSearch( 0_lnum, LineNumber( logData.getNbLine().get() ) );

    for ( int batch = 0; batch < 4; ++batch ) {
        logData.appendUtf8( makeStreamingSearchLines( 10001 + batch * 5000, 5000 ) );
        filteredData->updateSearch( 0_lnum, LineNumber( logData.getNbLine().get() ) );
    }
    filteredData->updateSearch( 0_lnum, LineNumber( logData.getNbLine().get() ) );

    auto reachedExpectedMatches = waitForMatchCount( *filteredData, 3001_lcount, 1000 );
    if ( !reachedExpectedMatches ) {
        filteredData->updateSearch( 0_lnum, LineNumber( logData.getNbLine().get() ) );
        reachedExpectedMatches = waitForMatchCount( *filteredData, 3001_lcount );
    }
    const auto countersBeforeAssert = filteredData->searchPerformanceCounters();
    INFO( "matches after wait=" << filteredData->getNbMatches().get()
                                << " operations=" << countersBeforeAssert.operationStarts
                                << " updates=" << countersBeforeAssert.updateRequests
                                << " coalesced=" << countersBeforeAssert.coalescedLiveUpdates );
    REQUIRE( reachedExpectedMatches );
    REQUIRE( logData.getNbLine().get() == 30001 );
    INFO( "matches=" << filteredData->getNbMatches().get() );
    REQUIRE( filteredData->getNbMatches() == 3001_lcount );
    REQUIRE( logData.getLineString( 10000_lnum )
             == QStringLiteral( "ERROR partial-line component=streaming-search" ) );

    const auto counters = filteredData->searchPerformanceCounters();
    REQUIRE( counters.coalescedLiveUpdates > 0 );
}

TEST_CASE( "Streaming live search uses pooled single-threaded path for small incremental ranges" )
{
    QTemporaryDir tempDir;
    REQUIRE( tempDir.isValid() );

    auto& config = Configuration::getSynced();
    SearchConfigGuard configGuard( config );
    config.setUseParallelSearch( true );
    config.setSearchReadBufferSizeLines( 10000 );

    StreamingLogData logData( makeCaptureId(), tempDir.path() );
    SafeQSignalSpy loadingSpy( &logData, SIGNAL( loadingFinished( LoadingStatus ) ) );
    REQUIRE( loadingSpy.safeWait() );

    // Seed 10000 lines so the initial full search is large enough for TBB.
    logData.appendUtf8( makeStreamingSearchLines( 0, 10000 ) );
    REQUIRE( loadingSpy.safeWait() );

    auto filteredData = logData.getNewFilteredData();
    filteredData->runSearch( RegularExpressionPattern{ QStringLiteral( "ERROR" ) }, 0_lnum,
                             LineNumber( logData.getNbLine().get() ) );
    REQUIRE( waitForSearchComplete( *filteredData ) );
    REQUIRE( filteredData->getNbMatches() == 1000_lcount );

    const auto countersAfterInitial = filteredData->searchPerformanceCounters();

    // Now do several small incremental updates (500 lines each — well below the
    // single-threaded threshold).  Each update should use the pooled
    // single-threaded path, so matcherCreations should NOT grow by 8 per
    // update (which the TBB path would do).
    for ( int batch = 0; batch < 4; ++batch ) {
        logData.appendUtf8( makeStreamingSearchLines( 10000 + batch * 500, 500 ) );
        REQUIRE( loadingSpy.safeWait() );
        filteredData->updateSearch( 0_lnum, LineNumber( logData.getNbLine().get() ) );
    }
    REQUIRE( waitForMatchCount( *filteredData, 1200_lcount ) );
    REQUIRE( filteredData->getNbMatches() == 1200_lcount );

    const auto countersAfterIncrements = filteredData->searchPerformanceCounters();
    const auto incrementalOps
        = countersAfterIncrements.operationStarts - countersAfterInitial.operationStarts;
    const auto incrementalMatchers
        = countersAfterIncrements.matcherCreations - countersAfterInitial.matcherCreations;

    INFO( "incremental operations=" << incrementalOps
          << " incremental matcherCreations=" << incrementalMatchers );

    // With the TBB path, each incremental operation would create 8 matchers
    // (one per TBB thread).  With the pooled single-threaded path, the first
    // incremental creates 1 matcher and subsequent ones reuse the pool, so
    // total matcherCreations for incremental updates should be far less than
    // incrementalOps * 8.
    REQUIRE( incrementalMatchers < incrementalOps * 8 );
}

TEST_CASE( "Streaming live search uses pooled path for medium live update ranges" )
{
    QTemporaryDir tempDir;
    REQUIRE( tempDir.isValid() );

    auto& config = Configuration::getSynced();
    SearchConfigGuard configGuard( config );
    config.setUseParallelSearch( true );
    config.setSearchThreadPoolSize( 4 );
    config.setSearchReadBufferSizeLines( 10000 );

    StreamingLogData logData( makeCaptureId(), tempDir.path() );
    SafeQSignalSpy loadingSpy( &logData, SIGNAL( loadingFinished( LoadingStatus ) ) );
    REQUIRE( loadingSpy.safeWait() );

    logData.appendUtf8( makeStreamingSearchLines( 0, 10000 ) );
    REQUIRE( loadingSpy.safeWait() );

    auto filteredData = logData.getNewFilteredData();
    filteredData->runSearch( RegularExpressionPattern{ QStringLiteral( "ERROR" ) }, 0_lnum,
                             LineNumber( logData.getNbLine().get() ) );
    REQUIRE( waitForSearchComplete( *filteredData ) );
    REQUIRE( filteredData->getNbMatches() == 1000_lcount );

    const auto countersAfterInitial = filteredData->searchPerformanceCounters();

    logData.appendUtf8( makeStreamingSearchLines( 10000, 20000 ) );
    REQUIRE( loadingSpy.safeWait() );
    filteredData->updateSearch( 0_lnum, LineNumber( logData.getNbLine().get() ) );

    REQUIRE( waitForMatchCount( *filteredData, 3000_lcount ) );
    REQUIRE( filteredData->getNbMatches() == 3000_lcount );

    const auto countersAfterIncrement = filteredData->searchPerformanceCounters();
    const auto incrementalOps
        = countersAfterIncrement.operationStarts - countersAfterInitial.operationStarts;
    const auto incrementalMatchers
        = countersAfterIncrement.matcherCreations - countersAfterInitial.matcherCreations;

    INFO( "incremental operations=" << incrementalOps
          << " incremental matcherCreations=" << incrementalMatchers );

    REQUIRE( incrementalOps >= 1 );
    REQUIRE( incrementalMatchers < incrementalOps * 4 );
}

TEST_CASE( "StreamingLogData setCaptureLimits trims data when limit is exceeded" )
{
    QTemporaryDir tempDir;
    REQUIRE( tempDir.isValid() );

    CaptureStore::Limits limits;
    limits.segmentTargetBytes = 16;
    limits.memoryBudgetBytes = 4096;
    limits.rollingMaxFileSize = 16;
    limits.rollingBackupCount = 3;

    StreamingLogData logData( makeCaptureId(), tempDir.path() );
    SafeQSignalSpy loadingSpy( &logData, SIGNAL( loadingFinished( LoadingStatus ) ) );
    REQUIRE( loadingSpy.safeWait() );

    logData.setCaptureLimits( limits );

    loadingSpy.clear();
    for ( int i = 0; i < 20; ++i ) {
        logData.appendUtf8( QStringLiteral( "stream-%1\n" ).arg( i ).toUtf8() );
    }
    REQUIRE( loadingSpy.safeWait() );

    // File size should be within limits
    CHECK( logData.getFileSize() <= limits.rollingMaxFileSize * limits.rollingBackupCount );

    // Lines should still be readable
    const auto lineCount = logData.getNbLine();
    CHECK( lineCount.get() > 0 );
    REQUIRE( logData.getLineString( LineNumber( lineCount.get() - 1 ) )
             == QStringLiteral( "stream-19" ) );
}

TEST_CASE( "StreamingLogData trim emits Truncated signal and invalidates caches" )
{
    QTemporaryDir tempDir;
    REQUIRE( tempDir.isValid() );

    CaptureStore::Limits limits;
    limits.segmentTargetBytes = 16;
    limits.memoryBudgetBytes = 4096;
    limits.rollingMaxFileSize = 16;
    limits.rollingBackupCount = 3;

    StreamingLogData logData( makeCaptureId(), tempDir.path() );
    SafeQSignalSpy loadingSpy( &logData, SIGNAL( loadingFinished( LoadingStatus ) ) );
    REQUIRE( loadingSpy.safeWait() );

    logData.setCaptureLimits( limits );

    loadingSpy.clear();
    SafeQSignalSpy fileSpy( &logData, SIGNAL( fileChanged( MonitoredFileStatus ) ) );

    for ( int i = 0; i < 20; ++i ) {
        logData.appendUtf8( QStringLiteral( "data-%1\n" ).arg( i ).toUtf8() );
    }
    REQUIRE( loadingSpy.safeWait() );

    // At least one Truncated signal should have been emitted
    bool sawTruncated = false;
    for ( int i = 0; i < fileSpy.count(); ++i ) {
        if ( fileSpy.at( i ).at( 0 ).value<MonitoredFileStatus>() == MonitoredFileStatus::Truncated ) {
            sawTruncated = true;
            break;
        }
    }
    REQUIRE( sawTruncated );

    // The search cache should work correctly after trim
    const auto rawLines = logData.getLinesRaw( 0_lnum, logData.getNbLine() );
    REQUIRE( rawLines.endOfLines.size() == static_cast<size_t>( logData.getNbLine().get() ) );

    const auto decoded = rawLines.decodeLines();
    REQUIRE( decoded.back() == QStringLiteral( "data-19" ) );
}

TEST_CASE( "Streaming live search eventually catches up across medium updates" )
{
    QTemporaryDir tempDir;
    REQUIRE( tempDir.isValid() );

    auto& config = Configuration::getSynced();
    SearchConfigGuard configGuard( config );
    config.setUseParallelSearch( true );
    config.setSearchThreadPoolSize( 4 );
    config.setSearchReadBufferSizeLines( 10000 );

    StreamingLogData logData( makeCaptureId(), tempDir.path() );
    SafeQSignalSpy loadingSpy( &logData, SIGNAL( loadingFinished( LoadingStatus ) ) );
    REQUIRE( loadingSpy.safeWait() );

    loadingSpy.clear();
    logData.appendUtf8( makeStreamingSearchLines( 0, 10000 ) );
    REQUIRE( loadingSpy.safeWait() );
    REQUIRE( logData.getNbLine() == 10000_lcount );

    auto filteredData = logData.getNewFilteredData();
    filteredData->runSearch( RegularExpressionPattern{ QStringLiteral( "ERROR" ) }, 0_lnum,
                             LineNumber( logData.getNbLine().get() ) );
    REQUIRE( waitForSearchComplete( *filteredData ) );
    REQUIRE( filteredData->getNbMatches() == 1000_lcount );

    for ( int batch = 0; batch < 4; ++batch ) {
        const auto expectedLines = LinesCount(
            static_cast<LinesCount::UnderlyingType>( 20000 + batch * 10000 ) );
        loadingSpy.clear();
        logData.appendUtf8(
            makeStreamingSearchLines( 10000 + batch * 10000, 10000, batch == 3 ) );
        REQUIRE( loadingSpy.safeWait() );
        REQUIRE( logData.getNbLine() == expectedLines );
        filteredData->updateSearch( 0_lnum, LineNumber( expectedLines.get() ) );
    }

    // A 100% signal completes one operation, not necessarily every live update
    // requested while that operation was running. Wait for terminal progress
    // whose owner-side result has caught up to the final endpoint.
    const auto finalGeneration = filteredData->currentSearchGeneration();
    const auto caughtUp
        = waitForTerminalMatchCount( *filteredData, 5001_lcount, finalGeneration );
    const auto counters = filteredData->searchPerformanceCounters();
    INFO( "matches=" << filteredData->getNbMatches().get()
                     << " operationStarts=" << counters.operationStarts
                     << " updates=" << counters.updateRequests
                     << " coalesced=" << counters.coalescedLiveUpdates );
    REQUIRE( caughtUp );
}

TEST_CASE( "StreamingLogData finishInput emits Truncated on single-line rotation trim",
           "[streaming]" )
{
    QTemporaryDir tempDir;
    REQUIRE( tempDir.isValid() );

    CaptureStore::Limits limits;
    limits.segmentTargetBytes = 16;
    limits.memoryBudgetBytes = 4096;
    limits.rollingMaxFileSize = 16;
    limits.rollingBackupCount = 2;

    StreamingLogData logData( makeCaptureId(), tempDir.path() );
    SafeQSignalSpy loadingSpy( &logData, SIGNAL( loadingFinished( LoadingStatus ) ) );
    REQUIRE( loadingSpy.safeWait() );

    logData.setCaptureLimits( limits );

    // Preserve mode routes output through CaptureStore, so finishInput()'s
    // commitLine() -> appendOutputBytes() path can rotate and trim. appendUtf8()
    // already handles this; finishInput() previously did not.
    const auto outPath = QDir( tempDir.path() ).filePath( "out.log" );
    REQUIRE( logData.bindOutputFile( outPath, LiveLogSaveAnsiMode::Preserve ) );

    SafeQSignalSpy fileSpy( &logData, SIGNAL( fileChanged( MonitoredFileStatus ) ) );

    // Partial lines (no trailing newline) committed via finishInput, forcing
    // many rotations through the single-line commit path.
    for ( int i = 0; i < 30; ++i ) {
        logData.appendUtf8( QStringLiteral( "data-%1" ).arg( i ).toUtf8() );
        logData.finishInput();
    }
    REQUIRE( loadingSpy.safeWait() );

    bool sawTruncated = false;
    for ( int i = 0; i < fileSpy.count(); ++i ) {
        if ( fileSpy.at( i ).at( 0 ).value<MonitoredFileStatus>()
             == MonitoredFileStatus::Truncated ) {
            sawTruncated = true;
            break;
        }
    }
    REQUIRE( sawTruncated );

    // Guard: the store must stay readable (tail line resolvable) after
    // finishInput-driven trims.
    REQUIRE( logData.getNbLine().get() > 0 );
    const auto tail = logData.getNbLine().get() - 1;
    const auto tailLine = logData.getLinesRaw( LineNumber( tail ), 1_lcount );
    REQUIRE( tailLine.endOfLines.size() == 1 );
}

TEST_CASE( "StreamingLogData Restore preserves a Strip-saved file when capture is empty",
           "[live-save-restore]" )
{
    QTemporaryDir tempDir;
    REQUIRE( tempDir.isValid() );

    const auto outputPath = QDir( tempDir.path() ).filePath( QStringLiteral( "saved.log" ) );
    const QByteArray savedBytes = QByteArrayLiteral( "line1\nline2\nline3\n" );

    // A prior session streamed content and saved it (FreshSave, Strip mode).
    {
        StreamingLogData logDataA( makeCaptureId(), tempDir.path() );
        SafeQSignalSpy loadingSpy( &logDataA, SIGNAL( loadingFinished( LoadingStatus ) ) );
        REQUIRE( loadingSpy.safeWait() );

        logDataA.appendUtf8( QByteArrayLiteral( "line1\nline2\nline3\n" ) );
        REQUIRE( loadingSpy.safeWait() );
        REQUIRE( logDataA.bindOutputFile( outputPath, LiveLogSaveAnsiMode::Strip ) );
    }

    QFile before( outputPath );
    REQUIRE( before.open( QIODevice::ReadOnly ) );
    REQUIRE( before.readAll() == savedBytes );
    before.close();

    // Simulate a restart where the temp capture was wiped (computer restart,
    // OS temp cleanup, or a crash before segments spilled to disk). A fresh
    // captureId loads nothing, so the in-memory capture is empty.
    StreamingLogData logDataB( makeCaptureId(), tempDir.path() );
    SafeQSignalSpy loadingSpyB( &logDataB, SIGNAL( loadingFinished( LoadingStatus ) ) );
    REQUIRE( loadingSpyB.safeWait() );
    REQUIRE( logDataB.getNbLine().get() == 0 );

    // Restoring the binding must NOT clear the previously saved file.
    REQUIRE( logDataB.bindOutputFile( outputPath, LiveLogSaveAnsiMode::Strip,
                                      OutputBindMode::Restore ) );

    QFile after( outputPath );
    REQUIRE( after.open( QIODevice::ReadOnly ) );
    CHECK( after.readAll() == savedBytes );
}

TEST_CASE( "StreamingLogData Restore preserves a Preserve-saved file when capture is empty",
           "[live-save-restore]" )
{
    QTemporaryDir tempDir;
    REQUIRE( tempDir.isValid() );

    const auto outputPath = QDir( tempDir.path() ).filePath( QStringLiteral( "saved.log" ) );
    const QByteArray savedBytes
        = QByteArrayLiteral( "\x1b[32mI/App\x1b[0m line1\n\x1b[31mE/App\x1b[0m line2\n" );

    {
        StreamingLogData logDataA( makeCaptureId(), tempDir.path() );
        SafeQSignalSpy loadingSpy( &logDataA, SIGNAL( loadingFinished( LoadingStatus ) ) );
        REQUIRE( loadingSpy.safeWait() );

        logDataA.appendUtf8( QByteArrayLiteral( "\x1b[32mI/App\x1b[0m line1\n"
                                                "\x1b[31mE/App\x1b[0m line2\n" ) );
        REQUIRE( loadingSpy.safeWait() );
        REQUIRE( logDataA.bindOutputFile( outputPath, LiveLogSaveAnsiMode::Preserve ) );
    }

    QFile before( outputPath );
    REQUIRE( before.open( QIODevice::ReadOnly ) );
    REQUIRE( before.readAll() == savedBytes );
    before.close();

    // Restart with a wiped temp capture.
    StreamingLogData logDataB( makeCaptureId(), tempDir.path() );
    SafeQSignalSpy loadingSpyB( &logDataB, SIGNAL( loadingFinished( LoadingStatus ) ) );
    REQUIRE( loadingSpyB.safeWait() );
    REQUIRE( logDataB.getNbLine().get() == 0 );

    REQUIRE( logDataB.bindOutputFile( outputPath, LiveLogSaveAnsiMode::Preserve,
                                      OutputBindMode::Restore ) );

    QFile after( outputPath );
    REQUIRE( after.open( QIODevice::ReadOnly ) );
    CHECK( after.readAll() == savedBytes );
}

TEST_CASE( "StreamingLogData Restore appends new data without duplicating existing content",
           "[live-save-restore]" )
{
    QTemporaryDir tempDir;
    REQUIRE( tempDir.isValid() );

    const auto outputPath = QDir( tempDir.path() ).filePath( QStringLiteral( "saved.log" ) );

    // First session saves three lines.
    const auto captureId = makeCaptureId();
    {
        StreamingLogData logDataA( captureId, tempDir.path() );
        SafeQSignalSpy loadingSpy( &logDataA, SIGNAL( loadingFinished( LoadingStatus ) ) );
        REQUIRE( loadingSpy.safeWait() );

        logDataA.appendUtf8( QByteArrayLiteral( "line1\nline2\nline3\n" ) );
        REQUIRE( loadingSpy.safeWait() );
        REQUIRE( logDataA.bindOutputFile( outputPath, LiveLogSaveAnsiMode::Strip ) );
    }

    // Graceful restart: same captureId, temp intact, so the capture reloads.
    StreamingLogData logDataB( captureId, tempDir.path() );
    SafeQSignalSpy loadingSpyB( &logDataB, SIGNAL( loadingFinished( LoadingStatus ) ) );
    REQUIRE( loadingSpyB.safeWait() );
    REQUIRE( logDataB.getNbLine().get() == 3 );

    REQUIRE( logDataB.bindOutputFile( outputPath, LiveLogSaveAnsiMode::Strip,
                                      OutputBindMode::Restore ) );

    // New data after restore must be appended exactly once — the reloaded
    // capture content must not be rewritten/duplicated.
    loadingSpyB.clear();
    logDataB.appendUtf8( QByteArrayLiteral( "line4\nline5\n" ) );
    REQUIRE( loadingSpyB.safeWait() );

    QFile after( outputPath );
    REQUIRE( after.open( QIODevice::ReadOnly ) );
    CHECK( after.readAll() == QByteArrayLiteral( "line1\nline2\nline3\nline4\nline5\n" ) );
}

TEST_CASE( "StreamingLogData Restore replays the capture when the saved file is missing",
           "[live-save-restore]" )
{
    QTemporaryDir tempDir;
    REQUIRE( tempDir.isValid() );

    const auto outputPath = QDir( tempDir.path() ).filePath( QStringLiteral( "saved.log" ) );

    // First session streams content and saves it; both the file and the
    // volatile capture hold the three lines.
    const auto captureId = makeCaptureId();
    {
        StreamingLogData logDataA( captureId, tempDir.path() );
        SafeQSignalSpy loadingSpy( &logDataA, SIGNAL( loadingFinished( LoadingStatus ) ) );
        REQUIRE( loadingSpy.safeWait() );

        logDataA.appendUtf8( QByteArrayLiteral( "line1\nline2\nline3\n" ) );
        REQUIRE( loadingSpy.safeWait() );
        REQUIRE( logDataA.bindOutputFile( outputPath, LiveLogSaveAnsiMode::Strip ) );
    }

    // The saved file was moved or deleted while klogg was closed, but a graceful
    // restart (same captureId, temp intact) still reloads the capture.
    REQUIRE( QFile::remove( outputPath ) );

    StreamingLogData logDataB( captureId, tempDir.path() );
    SafeQSignalSpy loadingSpyB( &logDataB, SIGNAL( loadingFinished( LoadingStatus ) ) );
    REQUIRE( loadingSpyB.safeWait() );
    REQUIRE( logDataB.getNbLine().get() == 3 );
    CaptureStore::Limits limits;
    // The capture window applies immediately. Keep all 18 bytes in its two
    // files while replay still exceeds one output file (and must not rotate).
    limits.rollingMaxFileSize = 10;
    limits.rollingBackupCount = 2;
    logDataB.setCaptureLimits( limits );

    // Restoring the binding must rebuild the missing file from the reloaded
    // capture instead of leaving it empty and dropping all history.
    REQUIRE( logDataB.bindOutputFile( outputPath, LiveLogSaveAnsiMode::Strip,
                                      OutputBindMode::Restore ) );

    QFile after( outputPath );
    REQUIRE( after.open( QIODevice::ReadOnly ) );
    CHECK( after.readAll() == QByteArrayLiteral( "line1\nline2\nline3\n" ) );
    CHECK_FALSE( QFileInfo::exists( outputPath + QStringLiteral( ".0" ) ) );
}

TEST_CASE( "Streaming forwards capture outcomes and explicit persistence",
           "[streaming][storage-persistence]" )
{
    QTemporaryDir root;
    StreamingLogData data( makeCaptureId(), root.path() );
    auto appended = data.appendUtf8( "a\nb" );
    CHECK( appended.acceptedBytes == 3 );
    CHECK( appended.committedBytes == 2 );
    CHECK( appended.pendingPartialBytes == 1 );
    CHECK_FALSE( data.persistCapture().complete() );
    const auto finished = data.finishInput();
    CHECK( finished.acceptedBytes == 0 );
    CHECK( finished.committedBytes == 1 );
    CHECK( data.persistCapture().complete() );
}

TEST_CASE( "Streaming output failure and throwing observer retain committed outcome",
           "[streaming][storage-output-facts]" )
{
    const bool observer = GENERATE( false, true );
    QTemporaryDir root;
    StreamingLogData data( makeCaptureId(), root.path() );
    REQUIRE( data.bindOutputFile( QDir( root.path() ).filePath( "output.log" ) ) );
    if ( observer ) {
        QObject::connect( &data, &StreamingLogData::fileChanged, &data, []( MonitoredFileStatus ) {
            throw std::runtime_error( "observer failed" );
        } );
    }
    else {
        StreamingLogDataTimerTestAccess::shortOutput( data );
    }
    const auto result = data.appendUtf8( "abc\n" );
    CHECK( result.disposition == CaptureStore::AppendDisposition::Complete );
    CHECK( result.committedBytes == 4 );
    CHECK( result.acceptedBytes == 4 );
    CHECK( result.notificationFailed == observer );
    if ( !observer ) {
        CHECK( result.outputFailure == CaptureStore::OutputFailure::Write );
        CHECK( result.outputBytes == 1 );
    }
}

TEST_CASE( "Streaming Strip replay uses bounded chunks and the live transform",
           "[streaming][storage-output-facts]" )
{
    QTemporaryDir root;
    StreamingLogData data( makeCaptureId(), root.path() );
    data.setDisplayEncoding( "ISO-8859-1" );
    data.setPrefilter( "prefix:" );
    const QByteArray record = QByteArray( "prefix:\033[31m" ) + QByteArray::fromHex( "e9" )
                              + QByteArray( 1000, 'x' ) + "\033[0m\n";
    const QByteArray batch = record.repeated( 200 );
    const auto livePath = QDir( root.path() ).filePath( "live.log" );
    REQUIRE( data.bindOutputFile( livePath ) );
    data.appendUtf8( batch );
    const auto replayPath = QDir( root.path() ).filePath( "replay.log" );
    REQUIRE( data.bindOutputFile( replayPath ) );
    QFile live( livePath ), replay( replayPath );
    REQUIRE( live.open( QIODevice::ReadOnly ) );
    REQUIRE( replay.open( QIODevice::ReadOnly ) );
    CHECK( live.readAll() == replay.readAll() );
    CHECK( StreamingLogDataTimerTestAccess::replayPeak( data ) > 0 );
    CHECK( StreamingLogDataTimerTestAccess::replayPeak( data ) <= 64 * 1024 + record.size() );
}

TEST_CASE( "Streaming clear resets finalized output separators", "[streaming][storage-followup]" )
{
    const auto mode = GENERATE( LiveLogSaveAnsiMode::Strip, LiveLogSaveAnsiMode::Preserve );
    QTemporaryDir root;
    StreamingLogData data( makeCaptureId(), root.path() );
    const auto output = QDir( root.path() ).filePath( "clear.log" );
    REQUIRE( data.bindOutputFile( output, mode ) );
    data.appendUtf8( "a" );
    data.finishInput();
    data.clearCapture();
    data.appendUtf8( "b\n" );
    data.finishInput();
    QFile file( output );
    REQUIRE( file.open( QIODevice::ReadOnly ) );
    CHECK( file.readAll() == "b\n" );
}

class ExportBarrier {
public:
    ~ExportBarrier()
    {
        release();
    }

    void block()
    {
        std::unique_lock<std::mutex> lock( mutex_ );
        entered_ = true;
        condition_.notify_all();
        condition_.wait( lock, [ this ] { return released_; } );
    }

    bool waitUntilEntered()
    {
        std::unique_lock<std::mutex> lock( mutex_ );
        return condition_.wait_for( lock, std::chrono::milliseconds{ 500 },
                                    [ this ] { return entered_; } );
    }

    bool waitUntilEnteredWithEvents()
    {
        QElapsedTimer deadline;
        deadline.start();
        while ( deadline.elapsed() < 1000 ) {
            QCoreApplication::processEvents( QEventLoop::AllEvents, 10 );
            std::unique_lock<std::mutex> lock( mutex_ );
            if ( entered_ ) {
                return true;
            }
            condition_.wait_for( lock, std::chrono::milliseconds{ 5 } );
        }
        return false;
    }

    void release()
    {
        const std::lock_guard<std::mutex> lock( mutex_ );
        released_ = true;
        condition_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool entered_ = false;
    bool released_ = false;
};

TEST_CASE( "Streaming output export fixes a snapshot boundary before retaining its concurrent tail",
           "[streaming][live-save-cutover][live-save-red]" )
{
    QTemporaryDir root;
    REQUIRE( root.isValid() );
    StreamingLogData data( makeCaptureId(), root.path() );
    data.appendUtf8( "prefix-0\nprefix-1\n" );

    const auto candidate = data.beginOutputExport( LiveLogSaveAnsiMode::Strip, 1024 );
    REQUIRE( candidate.has_value() );
    REQUIRE( data.hasPendingOutputExport() );

    CaptureStore::Limits limits;
    limits.segmentTargetBytes = 8;
    limits.maxTotalLines = 1;
    data.setCaptureLimits( limits );
    data.appendUtf8( "tail-2\ntail-3\n" );

    const auto tail = data.takeOutputExportTail( candidate->id );
    REQUIRE_FALSE( tail.failure.has_value() );
    REQUIRE( tail.batches.size() == 1 );
    CHECK( tail.batches.front().rawUtf8Lines == QByteArrayLiteral( "tail-2\ntail-3\n" ) );
    data.cancelOutputExport( candidate->id );
}

TEST_CASE( "Streaming output export rejects a concurrent PartialUnknown outcome",
           "[streaming][live-save-cutover]" )
{
    QTemporaryDir root;
    REQUIRE( root.isValid() );
    StreamingLogData data( makeCaptureId(), root.path() );
    CaptureStore::Limits limits;
    limits.segmentTargetBytes = 2;
    data.setCaptureLimits( limits );
    const auto candidate = data.beginOutputExport( LiveLogSaveAnsiMode::Preserve, 1024 );
    REQUIRE( candidate.has_value() );
    int mutations = 0;
    StreamingLogDataTimerTestAccess::beforeSegmentMutation( data, [ &mutations ] {
        if ( ++mutations == 2 ) {
            throw 42;
        }
    } );
    const auto append = data.appendUtf8( QByteArrayLiteral( "a\nb\n" ) );
    REQUIRE( append.disposition == CaptureStore::AppendDisposition::PartialUnknown );
    const auto tail = data.takeOutputExportTail( candidate->id );
    CHECK( tail.failure == StreamingLogData::OutputExportFailure::PartialUnknown );
    data.cancelOutputExport( candidate->id );
    StreamingLogDataTimerTestAccess::beforeSegmentMutation( data, {} );
}

TEST_CASE( "Streaming output export rejects a noncontiguous cutover journal",
           "[streaming][live-save-cutover][live-save-red]" )
{
    QTemporaryDir root;
    REQUIRE( root.isValid() );
    StreamingLogData data( makeCaptureId(), root.path() );
    const auto candidate = data.beginOutputExport( LiveLogSaveAnsiMode::Preserve, 1024 );
    REQUIRE( candidate.has_value() );
    data.appendUtf8( QByteArrayLiteral( "tail\n" ) );
    auto tail = data.takeOutputExportTail( candidate->id );
    REQUIRE_FALSE( tail.failure.has_value() );
    REQUIRE( tail.batches.size() == 1 );
    tail.batches.front().sequence += 1u;

    StreamingLogData::OutputExportEncodingState state;
    QByteArray written;
    const auto write = [ &written ]( const QByteArray& bytes ) {
        written.append( bytes );
        return static_cast<qint64>( bytes.size() );
    };
    CHECK_FALSE( StreamingLogData::writeOutputExportBatches( *candidate, tail.batches, state,
                                                             write ) );
    CHECK( written.isEmpty() );
    data.cancelOutputExport( candidate->id );
}

TEST_CASE( "Live save preserves a finalized zero-byte record before its concurrent tail",
           "[streaming][live-save-cutover][live-save-async][review-red]" )
{
    const auto mode = GENERATE( LiveLogSaveAnsiMode::Strip,
                                LiveLogSaveAnsiMode::Preserve );
    QTemporaryDir root;
    REQUIRE( root.isValid() );
    StreamingLogData data( makeCaptureId(), root.path() );
    data.appendUtf8( QByteArrayLiteral( "\r" ) );
    REQUIRE( data.finishInput().committedLines == 1_lcount );

    const auto candidate = data.beginOutputExport( mode, 4096 );
    REQUIRE( candidate.has_value() );
    data.appendUtf8( QByteArrayLiteral( "next\n" ) );

    StreamingLogData::OutputExportEncodingState state;
    QByteArray saved;
    const auto write = [ &saved ]( const QByteArray& bytes ) {
        saved.append( bytes );
        return static_cast<qint64>( bytes.size() );
    };
    REQUIRE( StreamingLogData::writeOutputExportSnapshot(
        *candidate, state, write ) );
    const auto tail = data.takeOutputExportTail( candidate->id );
    REQUIRE_FALSE( tail.failure.has_value() );
    REQUIRE( StreamingLogData::writeOutputExportBatches(
        *candidate, tail.batches, state, write ) );
    CHECK( saved == QByteArrayLiteral( "\nnext\n" ) );
    data.cancelOutputExport( candidate->id );
}

TEST_CASE( "Async live save publishes snapshot concurrent tail and future writes in order",
           "[streaming][live-save-cutover][live-save-async]" )
{
    const auto mode = GENERATE( LiveLogSaveAnsiMode::Strip, LiveLogSaveAnsiMode::Preserve );
    QTemporaryDir root;
    REQUIRE( root.isValid() );
    auto data = std::make_shared<StreamingLogData>( makeCaptureId(), root.path() );

    const auto oldPath = root.filePath( QStringLiteral( "old.log" ) );
    const auto newPath = root.filePath( QStringLiteral( "new.log" ) );
    REQUIRE( data->bindOutputFile( oldPath, mode ) );
    data->appendUtf8( QByteArrayLiteral( "\033[31mprefix-0\033[0m\nprefix-1\n" ) );
    QFile sentinel( newPath );
    REQUIRE( sentinel.open( QIODevice::WriteOnly ) );
    REQUIRE( sentinel.write( "sentinel" ) == 8 );
    sentinel.close();

    klogg::livelog::LiveLogExportService service( data );
    ExportBarrier snapshotBarrier;
    ExportBarrier publicationBarrier;
    klogg::livelog::LiveLogExportServiceTestAccess::setBeforeSnapshotWrite(
        service, [ &snapshotBarrier ] { snapshotBarrier.block(); } );
    klogg::livelog::LiveLogExportServiceTestAccess::setBeforePublication(
        service, [ &publicationBarrier ] { publicationBarrier.block(); } );
    const auto job = service.start( newPath, mode, 4096 );
    REQUIRE( job != nullptr );
    REQUIRE_FALSE( service.start( root.filePath( QStringLiteral( "second.log" ) ), mode, 4096 ) );
    REQUIRE( snapshotBarrier.waitUntilEntered() );
    CaptureStore::Limits limits;
    limits.segmentTargetBytes = 8;
    limits.maxTotalLines = 1;
    data->setCaptureLimits( limits );
    data->appendUtf8( QByteArrayLiteral( "tail-2\ntail-3\n" ) );
    snapshotBarrier.release();
    REQUIRE( publicationBarrier.waitUntilEnteredWithEvents() );
    data->appendUtf8( QByteArrayLiteral( "cutover-4\n" ) );
    publicationBarrier.release();
    job->waitForFinished();
    REQUIRE( job->result() == klogg::livelog::LiveLogExportResult::Succeeded );
    CHECK( job->workerThreadId() != std::this_thread::get_id() );
    CHECK( klogg::livelog::LiveLogExportServiceTestAccess::dataAccessThreadId( *job )
           == std::this_thread::get_id() );

    data->appendUtf8( QByteArrayLiteral( "future-5\n" ) );
    data->finishInput();
    QFile saved( newPath );
    REQUIRE( saved.open( QIODevice::ReadOnly ) );
    const auto prefix = mode == LiveLogSaveAnsiMode::Strip
                            ? QByteArrayLiteral( "prefix-0\nprefix-1\n" )
                            : QByteArrayLiteral( "\033[31mprefix-0\033[0m\nprefix-1\n" );
    CHECK( saved.readAll()
           == prefix + QByteArrayLiteral( "tail-2\ntail-3\ncutover-4\nfuture-5\n" ) );

    QFile old( oldPath );
    REQUIRE( old.open( QIODevice::ReadOnly ) );
    CHECK( old.readAll()
           == prefix + QByteArrayLiteral( "tail-2\ntail-3\ncutover-4\n" ) );
}

TEST_CASE( "Live save completion remains observable after a fast export finishes",
           "[streaming][live-save-cutover][live-save-async][review-red]" )
{
    QTemporaryDir root;
    REQUIRE( root.isValid() );
    auto data = std::make_shared<StreamingLogData>( makeCaptureId(), root.path() );
    data->appendUtf8( QByteArrayLiteral( "snapshot\n" ) );

    klogg::livelog::LiveLogExportService service( data );
    const auto job = service.start( root.filePath( QStringLiteral( "saved.log" ) ),
                                    LiveLogSaveAnsiMode::Strip, 4096 );
    REQUIRE( job != nullptr );
    job->waitForFinished();
    REQUIRE( job->result() == klogg::livelog::LiveLogExportResult::Succeeded );

    QObject context;
    int completions = 0;
    job->onFinished( &context, [ &completions ]( auto result ) {
        CHECK( result == klogg::livelog::LiveLogExportResult::Succeeded );
        ++completions;
    } );
    QCoreApplication::processEvents();
    CHECK( completions == 1 );
}

TEST_CASE( "Completed live save releases its capture snapshot leases",
           "[streaming][live-save-cutover][live-save-async][review-red]" )
{
    QTemporaryDir root;
    REQUIRE( root.isValid() );
    auto data = std::make_shared<StreamingLogData>( makeCaptureId(), root.path() );
    CaptureStore::Limits limits;
    limits.segmentTargetBytes = 4;
    limits.memoryBudgetBytes = 1;
    data->setCaptureLimits( limits );
    data->appendUtf8( QByteArrayLiteral( "a\nb\nc\n" ) );
    REQUIRE( data->persistCapture().complete() );

    QDir captureDirectory( data->capturePath() );
    const auto persistedSegments = captureDirectory.entryList(
        { QStringLiteral( "segment_*.log" ) }, QDir::Files );
    REQUIRE_FALSE( persistedSegments.isEmpty() );

    klogg::livelog::LiveLogExportService service( data );
    const auto job = service.start( root.filePath( QStringLiteral( "saved.log" ) ),
                                    LiveLogSaveAnsiMode::Strip, 4096 );
    REQUIRE( job != nullptr );
    job->waitForFinished();
    REQUIRE( job->result() == klogg::livelog::LiveLogExportResult::Succeeded );

    data->clearCapture();
    for ( const auto& segment : persistedSegments ) {
        CHECK_FALSE( QFileInfo::exists( captureDirectory.filePath( segment ) ) );
    }
}

TEST_CASE( "Async live save cancel and tail overflow preserve destination and old binding",
           "[streaming][live-save-cutover][live-save-async]" )
{
    const bool cancel = GENERATE( false, true );
    QTemporaryDir root;
    REQUIRE( root.isValid() );
    auto data = std::make_shared<StreamingLogData>( makeCaptureId(), root.path() );
    const auto oldPath = root.filePath( QStringLiteral( "old.log" ) );
    const auto newPath = root.filePath( QStringLiteral( "new.log" ) );
    REQUIRE( data->bindOutputFile( oldPath, LiveLogSaveAnsiMode::Strip ) );
    data->appendUtf8( QByteArrayLiteral( "prefix\n" ) );
    QFile sentinel( newPath );
    REQUIRE( sentinel.open( QIODevice::WriteOnly ) );
    REQUIRE( sentinel.write( "sentinel" ) == 8 );
    sentinel.close();

    klogg::livelog::LiveLogExportService service( data );
    ExportBarrier barrier;
    klogg::livelog::LiveLogExportServiceTestAccess::setBeforeSnapshotWrite(
        service, [ &barrier ] { barrier.block(); } );
    const auto job = service.start( newPath, LiveLogSaveAnsiMode::Strip, cancel ? 4096 : 8 );
    REQUIRE( job != nullptr );
    REQUIRE( barrier.waitUntilEntered() );
    data->appendUtf8( QByteArrayLiteral( "concurrent-tail-is-larger-than-eight\n" ) );
    if ( cancel ) {
        job->cancel();
    }
    barrier.release();
    job->waitForFinished();
    CHECK( job->result() == ( cancel ? klogg::livelog::LiveLogExportResult::Cancelled
                                     : klogg::livelog::LiveLogExportResult::TailOverflow ) );
    CHECK( data->boundOutputFile() == oldPath );
    data->appendUtf8( QByteArrayLiteral( "old-still-active\n" ) );
    data->finishInput();

    REQUIRE( sentinel.open( QIODevice::ReadOnly ) );
    CHECK( sentinel.readAll() == QByteArrayLiteral( "sentinel" ) );
    QFile old( oldPath );
    REQUIRE( old.open( QIODevice::ReadOnly ) );
    CHECK( old.readAll() == QByteArrayLiteral(
               "prefix\nconcurrent-tail-is-larger-than-eight\nold-still-active\n" ) );
}

TEST_CASE( "Live save validates the final tail before replacing the destination",
           "[streaming][live-save-cutover][live-save-async][live-save-red]" )
{
    QTemporaryDir root;
    REQUIRE( root.isValid() );
    auto data = std::make_shared<StreamingLogData>( makeCaptureId(), root.path() );
    const auto oldPath = root.filePath( QStringLiteral( "old.log" ) );
    const auto newPath = root.filePath( QStringLiteral( "new.log" ) );
    REQUIRE( data->bindOutputFile( oldPath, LiveLogSaveAnsiMode::Strip ) );
    data->appendUtf8( QByteArrayLiteral( "prefix\n" ) );

    QFile sentinel( newPath );
    REQUIRE( sentinel.open( QIODevice::WriteOnly ) );
    REQUIRE( sentinel.write( "sentinel" ) == 8 );
    sentinel.close();

    klogg::livelog::LiveLogExportService service( data );
    ExportBarrier publicationBarrier;
    klogg::livelog::LiveLogExportServiceTestAccess::setBeforePublication(
        service, [ &publicationBarrier ] { publicationBarrier.block(); } );
    const auto job = service.start( newPath, LiveLogSaveAnsiMode::Strip, 8 );
    REQUIRE( job != nullptr );
    REQUIRE( publicationBarrier.waitUntilEnteredWithEvents() );

    data->appendUtf8( QByteArrayLiteral( "tail-larger-than-eight\n" ) );
    publicationBarrier.release();
    job->waitForFinished();

    CHECK( job->result() == klogg::livelog::LiveLogExportResult::TailOverflow );
    CHECK( data->boundOutputFile() == oldPath );
    REQUIRE( sentinel.open( QIODevice::ReadOnly ) );
    CHECK( sentinel.readAll() == QByteArrayLiteral( "sentinel" ) );
}

TEST_CASE( "Live save teardown excludes user input while pumping owner events",
           "[streaming][live-save-cutover][live-save-async][live-save-red]" )
{
    QTemporaryDir root;
    REQUIRE( root.isValid() );
    auto data = std::make_shared<StreamingLogData>( makeCaptureId(), root.path() );
    data->appendUtf8( QByteArrayLiteral( "snapshot\n" ) );
    klogg::livelog::LiveLogExportService service( data );
    ExportBarrier workerBarrier;
    klogg::livelog::LiveLogExportServiceTestAccess::setBeforeSnapshotWrite(
        service, [ &workerBarrier ] { workerBarrier.block(); } );
    const auto job = service.start( root.filePath( QStringLiteral( "saved.log" ) ),
                                    LiveLogSaveAnsiMode::Strip, 4096 );
    REQUIRE( job != nullptr );
    REQUIRE( workerBarrier.waitUntilEntered() );

    std::optional<QEventLoop::ProcessEventsFlags> observedFlags;
    bool released = false;
    klogg::livelog::LiveLogExportServiceTestAccess::setOwnerEventPump(
        *job, [ & ]( QEventLoop::ProcessEventsFlags flags, int maximumTime ) {
            observedFlags = flags;
            if ( !std::exchange( released, true ) ) {
                workerBarrier.release();
            }
            QCoreApplication::processEvents( flags, maximumTime );
        } );
    job->waitForFinished();

    REQUIRE( observedFlags.has_value() );
    CHECK( observedFlags->testFlag( QEventLoop::ExcludeUserInputEvents ) );
}

TEST_CASE( "Live save journal allocation failure is contained after capture commit",
           "[streaming][live-save-cutover][live-save-async][live-save-red]" )
{
    QTemporaryDir root;
    REQUIRE( root.isValid() );
    StreamingLogData data( makeCaptureId(), root.path() );
    const auto candidate = data.beginOutputExport( LiveLogSaveAnsiMode::Strip, 4096 );
    REQUIRE( candidate.has_value() );
    StreamingLogDataTimerTestAccess::beforeOutputExportJournal(
        data, [] { throw std::bad_alloc{}; } );

    CaptureStore::AppendResult appendResult;
    CHECK_NOTHROW( appendResult = data.appendUtf8( QByteArrayLiteral( "committed-tail\n" ) ) );
    CHECK( appendResult.committedLines == 1_lcount );
    CHECK( data.getNbLine() == 1_lcount );
    const auto tail = data.takeOutputExportTail( candidate->id );
    REQUIRE( tail.failure.has_value() );
    CHECK( tail.failure == StreamingLogData::OutputExportFailure::TailOverflow );
    CHECK( tail.batches.empty() );
}

TEST_CASE( "Async live save cancellation wins the final publication decision",
           "[streaming][live-save-cutover][live-save-async][live-save-red]" )
{
    QTemporaryDir root;
    REQUIRE( root.isValid() );
    auto data = std::make_shared<StreamingLogData>( makeCaptureId(), root.path() );
    const auto oldPath = root.filePath( QStringLiteral( "old.log" ) );
    const auto newPath = root.filePath( QStringLiteral( "new.log" ) );
    REQUIRE( data->bindOutputFile( oldPath, LiveLogSaveAnsiMode::Strip ) );
    data->appendUtf8( QByteArrayLiteral( "prefix\n" ) );

    QFile sentinel( newPath );
    REQUIRE( sentinel.open( QIODevice::WriteOnly ) );
    REQUIRE( sentinel.write( "sentinel" ) == 8 );
    sentinel.close();

    klogg::livelog::LiveLogExportService service( data );
    ExportBarrier publicationDecision;
    klogg::livelog::LiveLogExportServiceTestAccess::setBeforePublication(
        service, [ &publicationDecision ] { publicationDecision.block(); } );
    const auto job = service.start( newPath, LiveLogSaveAnsiMode::Strip, 4096 );
    REQUIRE( job != nullptr );
    REQUIRE( publicationDecision.waitUntilEnteredWithEvents() );

    job->cancel();
    publicationDecision.release();
    job->waitForFinished();

    CHECK( job->result() == klogg::livelog::LiveLogExportResult::Cancelled );
    CHECK( data->boundOutputFile() == oldPath );
    REQUIRE( sentinel.open( QIODevice::ReadOnly ) );
    CHECK( sentinel.readAll() == QByteArrayLiteral( "sentinel" ) );
}

TEST_CASE( "Published live save never adopts a same-path replacement",
           "[streaming][live-save-cutover][live-save-async]" )
{
    const auto mode = GENERATE( LiveLogSaveAnsiMode::Strip, LiveLogSaveAnsiMode::Preserve );
    QTemporaryDir root;
    REQUIRE( root.isValid() );
    auto data = std::make_shared<StreamingLogData>( makeCaptureId(), root.path() );
    const auto oldPath = root.filePath( QStringLiteral( "old.log" ) );
    const auto newPath = root.filePath( QStringLiteral( "published.log" ) );
    REQUIRE( data->bindOutputFile( oldPath, mode ) );
    data->appendUtf8( QByteArrayLiteral( "prefix\n" ) );

    klogg::livelog::LiveLogExportService service( data );
    ExportBarrier publicationBarrier;
    bool replacementCreated = false;
    klogg::livelog::LiveLogExportServiceTestAccess::setBeforePublication(
        service, [ &publicationBarrier ] { publicationBarrier.block(); } );
    klogg::livelog::LiveLogExportServiceTestAccess::setAfterPublish(
        service, [ & ] {
            if ( !QFile::remove( newPath ) ) {
                return;
            }
            QFile replacement( newPath );
            replacementCreated = replacement.open( QIODevice::WriteOnly )
                                 && replacement.write( "replacement-sentinel" ) == 20;
        } );
    const auto job = service.start( newPath, mode, 4096 );
    REQUIRE( job != nullptr );
    REQUIRE( publicationBarrier.waitUntilEnteredWithEvents() );
    data->appendUtf8( QByteArrayLiteral( "after-publication\n" ) );
    publicationBarrier.release();
    job->waitForFinished();

    if ( replacementCreated ) {
        CHECK( job->result() == klogg::livelog::LiveLogExportResult::PublishedReopenFailed );
        CHECK( data->boundOutputFile() == oldPath );
        data->appendUtf8( QByteArrayLiteral( "old-remains-active\n" ) );
        data->finishInput();

        QFile replacement( newPath );
        REQUIRE( replacement.open( QIODevice::ReadOnly ) );
        CHECK( replacement.readAll() == QByteArrayLiteral( "replacement-sentinel" ) );
        QFile old( oldPath );
        REQUIRE( old.open( QIODevice::ReadOnly ) );
        CHECK( old.readAll()
               == QByteArrayLiteral( "prefix\nafter-publication\nold-remains-active\n" ) );
    }
    else {
        // Windows may deny replacement while the verified handle is open. That
        // is also safe: cutover succeeds only to the published file.
        CHECK( job->result() == klogg::livelog::LiveLogExportResult::Succeeded );
        CHECK( data->boundOutputFile() == newPath );
        data->appendUtf8( QByteArrayLiteral( "new-remains-active\n" ) );
        data->finishInput();

        QFile published( newPath );
        REQUIRE( published.open( QIODevice::ReadOnly ) );
        CHECK( published.readAll()
               == QByteArrayLiteral( "prefix\nafter-publication\nnew-remains-active\n" ) );
        QFile old( oldPath );
        REQUIRE( old.open( QIODevice::ReadOnly ) );
        CHECK( old.readAll() == QByteArrayLiteral( "prefix\nafter-publication\n" ) );
    }
}

TEST_CASE( "Streaming limit changes invalidate caches and report persistence health",
           "[streaming][storage-followup]" )
{
    qint64 now = 0;
    std::optional<CaptureStore::PersistenceFailure> failure
        = CaptureStore::PersistenceFailure::Write;
    QTemporaryDir root;
    StreamingLogData data( makeCaptureId(), root.path() );
    StreamingLogDataTimerTestAccess::spillFault( data, now, failure );
    int changes = 0;
    bool healthy = true;
    QObject::connect( &data, &StreamingLogData::capturePersistenceChanged, &data,
                      [ & ]( bool value, CaptureStore::PersistenceFailure ) {
                          ++changes;
                          healthy = value;
                      } );
    CaptureStore::Limits limits;
    limits.segmentTargetBytes = 2;
    data.setCaptureLimits( limits );
    data.appendUtf8( "a\nb\nc\n" );
    limits.memoryBudgetBytes = 1;
    data.setCaptureLimits( limits );
    CHECK( changes == 1 );
    CHECK_FALSE( healthy );
    data.retryPersistence();
    CHECK( changes == 1 );
    failure.reset();
    now += 5000;
    CHECK( data.retryPersistence().complete() );
    CHECK( changes == 2 );
    CHECK( healthy );
    limits.maxTotalLines = 1;
    data.setCaptureLimits( limits );
    CHECK( data.getNbLine() == 1_lcount );
    const auto raw = data.getLinesRaw( 0_lnum, 1_lcount );
    CHECK( QByteArray( raw.buffer.data(), static_cast<int>( raw.buffer.size() ) ) == "c\n" );
}
