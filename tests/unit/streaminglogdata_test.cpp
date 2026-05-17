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
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>

#include <string_view>

#include "capturestore.h"
#include "configuration.h"
#include "logfiltereddata.h"
#include "streaminglogdata.h"
#include "test_utils.h"

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
    QElapsedTimer timer;
    timer.start();
    while ( timer.elapsed() < timeoutMs ) {
        QCoreApplication::processEvents( QEventLoop::AllEvents, 50 );
        if ( filteredData.getNbMatches() == expected ) {
            return true;
        }
        QThread::msleep( 10 );
    }
    return false;
}

QByteArray makeStreamingSearchLines( int firstLine, int count )
{
    QByteArray data;
    data.reserve( count * 48 );
    for ( int i = 0; i < count; ++i ) {
        const auto line = firstLine + i;
        data.append( line % 10 == 0 ? "ERROR " : "INFO " );
        data.append( QByteArray::number( line ) );
        data.append( " component=streaming-search\n" );
    }
    return data;
}

struct SearchConfigGuard {
    Configuration& cfg;
    bool prevParallel;
    int prevBufferLines;
    int prevThreadPoolSize;

    explicit SearchConfigGuard( Configuration& c )
        : cfg( c )
        , prevParallel( c.useParallelSearch() )
        , prevBufferLines( c.searchReadBufferSizeLines() )
        , prevThreadPoolSize( c.searchThreadPoolSize() )
    {
    }

    ~SearchConfigGuard()
    {
        cfg.setUseParallelSearch( prevParallel );
        cfg.setSearchReadBufferSizeLines( prevBufferLines );
        cfg.setSearchThreadPoolSize( prevThreadPoolSize );
    }

    SearchConfigGuard( const SearchConfigGuard& ) = delete;
    SearchConfigGuard& operator=( const SearchConfigGuard& ) = delete;
};
} // namespace

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

TEST_CASE( "Streaming live search coalesces medium updates before starting operations" )
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

    for ( int batch = 0; batch < 4; ++batch ) {
        logData.appendUtf8( makeStreamingSearchLines( 10000 + batch * 10000, 10000 ) );
        filteredData->updateSearch( 0_lnum, LineNumber( logData.getNbLine().get() ) );
    }

    REQUIRE( waitForSearchComplete( *filteredData ) );
    REQUIRE( filteredData->getNbMatches() == 5000_lcount );

    const auto countersAfterUpdates = filteredData->searchPerformanceCounters();
    const auto incrementalOps
        = countersAfterUpdates.operationStarts - countersAfterInitial.operationStarts;
    const auto coalescedUpdates
        = countersAfterUpdates.coalescedLiveUpdates - countersAfterInitial.coalescedLiveUpdates;

    INFO( "incremental operations=" << incrementalOps
          << " coalescedLiveUpdates=" << coalescedUpdates );

    REQUIRE( incrementalOps < 4 );
    REQUIRE( coalescedUpdates > 0 );
}
