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

#include <atomic>
#include <chrono>
#include <thread>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTextCodec>
#include <QUuid>

#include "capturestore.h"

namespace {
QString makeTestDir( const QString& prefix )
{
    const auto dirPath = QDir::cleanPath( QDir::currentPath() + QDir::separator()
                                          + QLatin1String( "test_tmp" ) + QDir::separator()
                                          + prefix + QLatin1Char( '_' )
                                          + QUuid::createUuid().toString( QUuid::WithoutBraces ) );
    QDir{}.mkpath( dirPath );
    return dirPath;
}

QString makeCaptureId()
{
    return QUuid::createUuid().toString( QUuid::WithoutBraces );
}

QStringList segmentFiles( const QString& capturePath )
{
    return QDir( capturePath ).entryList( QStringList{ "segment_*.log" }, QDir::Files,
                                          QDir::Name | QDir::IgnoreCase );
}

QString readUtf8File( const QString& filePath )
{
    QFile file( filePath );
    if ( !file.open( QIODevice::ReadOnly ) ) {
        return {};
    }
    return QString::fromUtf8( file.readAll() );
}
} // namespace

TEST_CASE( "CaptureStore default spill limits prefer memory over temp files" )
{
    CaptureStore::Limits limits;

    REQUIRE( limits.segmentTargetBytes == 1024 * 1024 );
    REQUIRE( limits.memoryBudgetBytes == 256 * 1024 * 1024 );
}

TEST_CASE( "CaptureStore spills old segments only after memory budget is exceeded" )
{
    CaptureStore::Limits limits;
    limits.segmentTargetBytes = 8;
    limits.memoryBudgetBytes = 16;

    const auto rootPath = makeTestDir( "capturestore_spill" );
    CaptureStore store( makeCaptureId(), rootPath, limits );

    store.appendUtf8( QByteArrayLiteral( "aaa\nbbb\nccc\n" ) );
    REQUIRE( segmentFiles( store.capturePath() ).empty() );

    store.appendUtf8( QByteArrayLiteral( "ddd\neee\nfff\n" ) );
    REQUIRE_FALSE( segmentFiles( store.capturePath() ).empty() );
    REQUIRE( store.stats().memoryBytes <= limits.memoryBudgetBytes );
    REQUIRE( store.lineCount().get() == 6 );
    REQUIRE( store.lineAt( LineNumber( 0 ), QTextCodec::codecForName( "UTF-8" ),
                           QRegularExpression{} )
             == QStringLiteral( "aaa" ) );
    REQUIRE( store.lineAt( LineNumber( 5 ), QTextCodec::codecForName( "UTF-8" ),
                           QRegularExpression{} )
             == QStringLiteral( "fff" ) );
}

TEST_CASE( "CaptureStore persists buffered segments on destruction for session restore" )
{
    CaptureStore::Limits limits;
    limits.segmentTargetBytes = 64;
    limits.memoryBudgetBytes = 4096;

    const auto rootPath = makeTestDir( "capturestore_restore" );
    const auto captureId = makeCaptureId();
    const auto capturePath = QDir( rootPath ).filePath( captureId );

    {
        CaptureStore store( captureId, rootPath, limits );
        store.appendUtf8( QByteArrayLiteral( "one\ntwo\nthree\n" ) );
        REQUIRE( segmentFiles( capturePath ).empty() );
    }

    REQUIRE_FALSE( segmentFiles( capturePath ).empty() );

    CaptureStore restored( captureId, rootPath, limits );
    REQUIRE( restored.loadFromDisk() );
    REQUIRE( restored.lineCount().get() == 3 );
    REQUIRE( restored.lineAt( LineNumber( 2 ), QTextCodec::codecForName( "UTF-8" ),
                              QRegularExpression{} )
             == QStringLiteral( "three" ) );
}

TEST_CASE( "CaptureStore deleteCaptureFiles suppresses destructor persistence" )
{
    CaptureStore::Limits limits;
    limits.segmentTargetBytes = 64;
    limits.memoryBudgetBytes = 4096;

    const auto rootPath = makeTestDir( "capturestore_delete" );
    const auto captureId = makeCaptureId();
    const auto capturePath = QDir( rootPath ).filePath( captureId );

    {
        CaptureStore store( captureId, rootPath, limits );
        store.appendUtf8( QByteArrayLiteral( "alpha\nbeta\n" ) );
        store.deleteCaptureFiles();
        REQUIRE_FALSE( QFileInfo::exists( capturePath ) );
    }

    REQUIRE_FALSE( QFileInfo::exists( capturePath ) );
}

TEST_CASE( "CaptureStore cleanupUnusedCapturesAsync removes orphan captures off the startup path" )
{
    const auto rootPath = makeTestDir( "capturestore_async_cleanup" );
    const auto retainedCaptureId = makeCaptureId();
    const auto orphanCaptureId = makeCaptureId();
    const auto retainedPath = QDir( rootPath ).filePath( retainedCaptureId );
    const auto orphanPath = QDir( rootPath ).filePath( orphanCaptureId );

    REQUIRE( QDir{}.mkpath( retainedPath ) );
    REQUIRE( QDir{}.mkpath( orphanPath ) );

    QFile orphanSegment( QDir( orphanPath ).filePath( QStringLiteral( "segment_000000.log" ) ) );
    REQUIRE( orphanSegment.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
    orphanSegment.write( QByteArray( 1024 * 1024, 'x' ) );
    orphanSegment.close();

    QElapsedTimer timer;
    timer.start();
    CaptureStore::cleanupUnusedCapturesAsync( QSet<QString>{ retainedCaptureId }, rootPath );
    const auto elapsedMs = timer.elapsed();

    INFO( "cleanup scheduling elapsed ms: " << elapsedMs );
    CHECK( elapsedMs < 200 );
    REQUIRE( QDir{ retainedPath }.exists() );

    QElapsedTimer deadline;
    deadline.start();
    while ( QDir{ orphanPath }.exists() && deadline.elapsed() < 5000 ) {
        std::this_thread::sleep_for( std::chrono::milliseconds( 20 ) );
    }

    REQUIRE_FALSE( QDir{ orphanPath }.exists() );
    REQUIRE( QDir{ retainedPath }.exists() );
}

TEST_CASE( "CaptureStore cleanupUnusedCaptures preserves captures modified after cutoff" )
{
    const auto rootPath = makeTestDir( "capturestore_cleanup_cutoff" );
    const auto orphanCaptureId = makeCaptureId();
    const auto activeCaptureId = makeCaptureId();
    const auto orphanPath = QDir( rootPath ).filePath( orphanCaptureId );
    const auto activePath = QDir( rootPath ).filePath( activeCaptureId );

    REQUIRE( QDir{}.mkpath( orphanPath ) );
    QFile orphanSegment( QDir( orphanPath ).filePath( QStringLiteral( "segment_000000.log" ) ) );
    REQUIRE( orphanSegment.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
    orphanSegment.write( QByteArrayLiteral( "old\n" ) );
    orphanSegment.close();

    const auto cleanupCutoff = QDateTime::currentDateTimeUtc();
    std::this_thread::sleep_for( std::chrono::milliseconds( 20 ) );

    REQUIRE( QDir{}.mkpath( activePath ) );
    QFile activeSegment( QDir( activePath ).filePath( QStringLiteral( "segment_000000.log" ) ) );
    REQUIRE( activeSegment.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
    activeSegment.write( QByteArrayLiteral( "new\n" ) );
    activeSegment.close();

    CaptureStore::cleanupUnusedCaptures( {}, rootPath, cleanupCutoff );

    REQUIRE_FALSE( QDir{ orphanPath }.exists() );
    REQUIRE( QDir{ activePath }.exists() );
}

TEST_CASE( "CaptureStore bindOutputFile overwrites existing files and replays spilled segments" )
{
    CaptureStore::Limits limits;
    limits.segmentTargetBytes = 8;
    limits.memoryBudgetBytes = 8;

    const auto rootPath = makeTestDir( "capturestore_bind_output" );
    const auto outputPath = QDir( rootPath ).filePath( QStringLiteral( "saved.log" ) );

    QFile staleOutput( outputPath );
    REQUIRE( staleOutput.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
    REQUIRE( staleOutput.write( QByteArrayLiteral( "stale-data\n" ) ) > 0 );
    staleOutput.close();

    CaptureStore store( makeCaptureId(), rootPath, limits );
    store.appendUtf8( QByteArrayLiteral( "alpha\nbeta\ngamma\ndelta\n" ) );
    REQUIRE_FALSE( segmentFiles( store.capturePath() ).empty() );

    REQUIRE( store.bindOutputFile( outputPath ) );
    store.appendUtf8( QByteArrayLiteral( "epsilon\n" ) );
    REQUIRE( store.bindOutputFile( QString{} ) );

    REQUIRE( QFileInfo::exists( outputPath ) );
    REQUIRE( readUtf8File( outputPath )
             == QStringLiteral( "alpha\nbeta\ngamma\ndelta\nepsilon\n" ) );
}

TEST_CASE( "CaptureStore finishInput commits a trailing partial line without adding a newline" )
{
    CaptureStore::Limits limits;
    limits.segmentTargetBytes = 64;
    limits.memoryBudgetBytes = 4096;

    const auto rootPath = makeTestDir( "capturestore_partial_finish" );
    const auto outputPath = QDir( rootPath ).filePath( QStringLiteral( "saved.log" ) );
    auto* codec = QTextCodec::codecForName( "UTF-8" );

    CaptureStore store( makeCaptureId(), rootPath, limits );
    REQUIRE( store.bindOutputFile( outputPath ) );

    store.appendUtf8( QByteArrayLiteral( "partial-line" ) );
    REQUIRE( store.lineCount().get() == 0 );

    store.finishInput();

    REQUIRE( store.lineCount().get() == 1 );
    REQUIRE( store.lineAt( LineNumber( 0 ), codec, QRegularExpression{} )
             == QStringLiteral( "partial-line" ) );
    REQUIRE( readUtf8File( outputPath ) == QStringLiteral( "partial-line" ) );
}

TEST_CASE( "CaptureStore persists a trailing partial line on destruction" )
{
    CaptureStore::Limits limits;
    limits.segmentTargetBytes = 64;
    limits.memoryBudgetBytes = 4096;

    const auto rootPath = makeTestDir( "capturestore_partial_restore" );
    const auto captureId = makeCaptureId();

    {
        CaptureStore store( captureId, rootPath, limits );
        store.appendUtf8( QByteArrayLiteral( "tail-fragment" ) );
    }

    CaptureStore restored( captureId, rootPath, limits );
    REQUIRE( restored.loadFromDisk() );
    REQUIRE( restored.lineCount().get() == 1 );
    REQUIRE( restored.lineAt( LineNumber( 0 ), QTextCodec::codecForName( "UTF-8" ),
                              QRegularExpression{} )
             == QStringLiteral( "tail-fragment" ) );
}

TEST_CASE( "CaptureStore serializes concurrent append and read access" )
{
    CaptureStore::Limits limits;
    limits.segmentTargetBytes = 32;
    limits.memoryBudgetBytes = 64;

    const auto rootPath = makeTestDir( "capturestore_concurrent" );
    CaptureStore store( makeCaptureId(), rootPath, limits );
    auto* codec = QTextCodec::codecForName( "UTF-8" );

    std::atomic<bool> writerDone{ false };
    std::thread writer( [ &store, &writerDone ] {
        for ( int i = 0; i < 200; ++i ) {
            store.appendUtf8( QStringLiteral( "line-%1\n" ).arg( i ).toUtf8() );
        }
        writerDone = true;
    } );

    while ( !writerDone.load() ) {
        const auto lines = store.lineCount();
        if ( lines > 0_lcount ) {
            const auto rawLines = store.buildRawLines( 0_lnum, lines, codec, QRegularExpression{} );
            REQUIRE( rawLines.endOfLines.size() <= static_cast<size_t>( lines.get() ) );
        }
    }

    writer.join();
    REQUIRE( store.lineCount().get() == 200 );
    REQUIRE( store.lineAt( LineNumber( 199 ), codec, QRegularExpression{} ) == QStringLiteral( "line-199" ) );
}

TEST_CASE( "CaptureStore buildRawLines snapshot consistency under concurrent append" )
{
    CaptureStore::Limits limits;
    limits.segmentTargetBytes = 32;
    limits.memoryBudgetBytes = 256;

    const auto rootPath = makeTestDir( "capturestore_snapshot_consistency" );
    CaptureStore store( makeCaptureId(), rootPath, limits );
    auto* codec = QTextCodec::codecForName( "UTF-8" );

    // Pre-populate some data so buildRawLines has something to read from the start.
    for ( int i = 0; i < 50; ++i ) {
        store.appendUtf8( QStringLiteral( "seed-%1\n" ).arg( i ).toUtf8() );
    }

    std::atomic<bool> writerDone{ false };
    std::atomic<int> readerIterations{ 0 };
    bool readerHadError = false;

    std::thread writer( [ &store, &writerDone ] {
        for ( int i = 50; i < 300; ++i ) {
            store.appendUtf8( QStringLiteral( "concurrent-%1\n" ).arg( i ).toUtf8() );
        }
        writerDone = true;
    } );

    // Reader thread: repeatedly call buildRawLines and verify internal consistency.
    std::thread reader( [ &store, &writerDone, &readerIterations, &readerHadError, codec ] {
        while ( !writerDone.load() || readerIterations.load() < 10 ) {
            const auto lines = store.lineCount();
            if ( lines <= 0_lcount ) {
                continue;
            }

            // Request a subset of lines from the middle of the store.
            const auto startLine = LineNumber( lines.get() / 2 );
            const auto count = LinesCount(
                qMin( static_cast<LinesCount::UnderlyingType>( 10 ), lines.get() - startLine.get() ) );
            if ( count <= 0_lcount ) {
                continue;
            }

            const auto rawLines
                = store.buildRawLines( startLine, count, codec, QRegularExpression{} );

            // Verify snapshot consistency: the number of endOfLines entries
            // must not exceed the requested count, and must match the buffer's
            // newline-terminated structure.
            if ( static_cast<LinesCount::UnderlyingType>( rawLines.endOfLines.size() ) > count.get() ) {
                readerHadError = true;
                break;
            }

            // Verify that each endOfLines offset is within the buffer.
            const auto bufferSize = static_cast<qint64>( rawLines.buffer.size() );
            for ( const auto& eol : rawLines.endOfLines ) {
                if ( eol > bufferSize ) {
                    readerHadError = true;
                    break;
                }
            }
            if ( readerHadError ) {
                break;
            }

            readerIterations.fetch_add( 1 );
        }
    } );

    writer.join();
    reader.join();

    REQUIRE_FALSE( readerHadError );
    REQUIRE( readerIterations.load() >= 10 );
    REQUIRE( store.lineCount().get() == 300 );
}

TEST_CASE( "CaptureStore incremental rebuildCumulativeLineCounts stays correct across many segments" )
{
    CaptureStore::Limits limits;
    limits.segmentTargetBytes = 8;
    limits.memoryBudgetBytes = 4096;

    const auto rootPath = makeTestDir( "capturestore_incremental_cumulative" );
    CaptureStore store( makeCaptureId(), rootPath, limits );
    auto* codec = QTextCodec::codecForName( "UTF-8" );

    // Append lines one at a time to force many segment rotations.
    constexpr int totalLines = 60;
    for ( int i = 0; i < totalLines; ++i ) {
        store.appendUtf8( QStringLiteral( "line-%1\n" ).arg( i, 3, 10, QLatin1Char( '0' ) ).toUtf8() );

        // Verify cumulative line count is correct after every append.
        REQUIRE( store.lineCount().get() == static_cast<LinesCount::UnderlyingType>( i + 1 ) );
    }

    // Verify every single line is addressable and contains the expected content.
    for ( int i = 0; i < totalLines; ++i ) {
        INFO( "Checking line " << i );
        const auto expected
            = QStringLiteral( "line-%1" ).arg( i, 3, 10, QLatin1Char( '0' ) );
        REQUIRE( store.lineAt( LineNumber( static_cast<LineNumber::UnderlyingType>( i ) ), codec,
                              QRegularExpression{} )
                 == expected );
    }

    // Also verify buildRawLines covers the full range correctly.
    const auto rawLines = store.buildRawLines( 0_lnum, LinesCount( totalLines ), codec,
                                                QRegularExpression{} );
    REQUIRE( rawLines.endOfLines.size() == static_cast<size_t>( totalLines ) );
}

TEST_CASE( "CaptureStore buildRawLines snapshot spans in-memory and spilled segments" )
{
    CaptureStore::Limits limits;
    limits.segmentTargetBytes = 8;
    limits.memoryBudgetBytes = 24;

    const auto rootPath = makeTestDir( "capturestore_snapshot_spill" );
    CaptureStore store( makeCaptureId(), rootPath, limits );
    auto* codec = QTextCodec::codecForName( "UTF-8" );

    // Append enough data to ensure some segments get spilled to disk.
    const QStringList expectedLines = {
        QStringLiteral( "alpha" ),   QStringLiteral( "bravo" ),
        QStringLiteral( "charlie" ), QStringLiteral( "delta" ),
        QStringLiteral( "echo" ),    QStringLiteral( "foxtrot" ),
        QStringLiteral( "golf" ),    QStringLiteral( "hotel" ),
        QStringLiteral( "india" ),   QStringLiteral( "juliet" ),
    };

    for ( const auto& line : expectedLines ) {
        store.appendUtf8( ( line + QLatin1Char( '\n' ) ).toUtf8() );
    }

    // Confirm spilling actually happened.
    REQUIRE_FALSE( segmentFiles( store.capturePath() ).empty() );
    REQUIRE( store.lineCount().get() == 10 );

    // Build raw lines spanning the entire range (both spilled and in-memory).
    const auto rawLines = store.buildRawLines( 0_lnum, LinesCount( static_cast<uint64_t>( expectedLines.size() ) ),
                                                codec, QRegularExpression{} );
    REQUIRE( rawLines.endOfLines.size() == 10 );

    // Decode and verify every line matches the original.
    const auto decodedLines = rawLines.decodeLines();
    REQUIRE( decodedLines.size() == 10 );
    for ( size_t i = 0; i < decodedLines.size(); ++i ) {
        INFO( "Verifying decoded line " << i );
        REQUIRE( decodedLines[ i ] == expectedLines[ static_cast<int>( i ) ] );
    }

    // Also verify individual lineAt access for a few spilled lines.
    REQUIRE( store.lineAt( LineNumber( 0 ), codec, QRegularExpression{} )
             == QStringLiteral( "alpha" ) );
    REQUIRE( store.lineAt( LineNumber( 4 ), codec, QRegularExpression{} )
             == QStringLiteral( "echo" ) );
    REQUIRE( store.lineAt( LineNumber( 9 ), codec, QRegularExpression{} )
             == QStringLiteral( "juliet" ) );
}

TEST_CASE( "CaptureStore buildRawLines converts non UTF-8 input before search views" )
{
    const auto* latin1Codec = QTextCodec::codecForName( "ISO-8859-1" );
    REQUIRE( latin1Codec != nullptr );

    const auto rootPath = makeTestDir( "capturestore_non_utf8_rawlines" );
    CaptureStore store( makeCaptureId(), rootPath );
    store.appendUtf8( QByteArray::fromHex( "636166e90a" ) ); // cafe acute in ISO-8859-1

    const auto rawLines = store.buildRawLines( 0_lnum, 1_lcount, const_cast<QTextCodec*>( latin1Codec ),
                                               QRegularExpression{} );
    const auto decoded = rawLines.decodeLines();
    REQUIRE( decoded.size() == 1 );
    REQUIRE( decoded[ 0 ] == QString::fromLatin1( "caf\xe9" ) );

    const auto utf8View = rawLines.buildUtf8View();
    REQUIRE( utf8View.size() == 1 );
    REQUIRE( utf8View[ 0 ] == std::string_view{ "caf\xc3\xa9" } );
}

TEST_CASE( "CaptureStore appends large UTF-8 batches within a linear-time budget" )
{
    const auto rootPath = makeTestDir( "capturestore_large_append_budget" );
    CaptureStore store( makeCaptureId(), rootPath );

    constexpr int lineCount = 1000000;
    QByteArray data;
    data.reserve( lineCount * 32 );
    for ( int i = 0; i < lineCount; ++i ) {
        data.append( "line-" );
        data.append( QByteArray::number( i ) );
        data.append( "\r\n" );
    }

    QElapsedTimer timer;
    timer.start();
    store.appendUtf8( data );
    const auto elapsedMs = timer.elapsed();

    REQUIRE( store.lineCount().get() == lineCount );
    REQUIRE( store.lineAt( 0_lnum, QTextCodec::codecForName( "UTF-8" ), QRegularExpression{} )
             == QStringLiteral( "line-0" ) );
    REQUIRE( store.lineAt( LineNumber( lineCount - 1 ), QTextCodec::codecForName( "UTF-8" ),
                           QRegularExpression{} )
             == QStringLiteral( "line-999999" ) );
    CHECK( elapsedMs < 2000 );
}

TEST_CASE( "CaptureStore appends large UTF-8 batches with low per-line metadata overhead" )
{
    const auto rootPath = makeTestDir( "capturestore_large_append_metadata_budget" );
    CaptureStore store( makeCaptureId(), rootPath );

    constexpr int lineCount = 1000000;
    QByteArray data;
    data.reserve( lineCount * 16 );
    for ( int i = 0; i < lineCount; ++i ) {
        data.append( "m-" );
        data.append( QByteArray::number( i ) );
        data.append( '\n' );
    }

    QElapsedTimer timer;
    timer.start();
    store.appendUtf8( data );
    const auto elapsedMs = timer.elapsed();

    REQUIRE( store.lineCount().get() == lineCount );
    REQUIRE( store.lineAt( 0_lnum, QTextCodec::codecForName( "UTF-8" ), QRegularExpression{} )
             == QStringLiteral( "m-0" ) );
    REQUIRE( store.lineAt( LineNumber( lineCount - 1 ), QTextCodec::codecForName( "UTF-8" ),
                           QRegularExpression{} )
             == QStringLiteral( "m-999999" ) );
    REQUIRE( store.stats().memoryBytes == data.size() );
    CHECK( elapsedMs < 200 );
}

TEST_CASE( "CaptureStore batched output defers flush below threshold" )
{
    const auto rootPath = makeTestDir( "capturestore_batched_flush" );
    const auto outputPath = QDir( rootPath ).filePath( QStringLiteral( "batched.log" ) );

    CaptureStore store( makeCaptureId(), rootPath );
    REQUIRE( store.bindOutputFile( outputPath ) );

    // Append a small amount of data (well below 1MB and 1000 lines)
    for ( int i = 0; i < 10; ++i ) {
        store.appendUtf8( QStringLiteral( "short-line-%1\n" ).arg( i ).toUtf8() );
    }

    // Before explicit flush, data may not yet be on disk (deferred)
    {
        QFile preFlush( outputPath );
        REQUIRE( preFlush.open( QIODevice::ReadOnly ) );
        // The file exists (bindOutputFile wrote existing data), but the 10 new
        // lines should not have been flushed yet since we are below all thresholds.
        const auto preContent = QString::fromUtf8( preFlush.readAll() );
        CHECK_FALSE( preContent.contains( QStringLiteral( "short-line-9" ) ) );
    }

    // After explicit flush, all data should be on disk
    store.flush();
    const auto content = readUtf8File( outputPath );
    REQUIRE( content.contains( QStringLiteral( "short-line-9" ) ) );
}

TEST_CASE( "CaptureStore batched output auto-flushes after byte threshold" )
{
    const auto rootPath = makeTestDir( "capturestore_byte_threshold" );
    const auto outputPath = QDir( rootPath ).filePath( QStringLiteral( "big.log" ) );

    CaptureStore store( makeCaptureId(), rootPath );
    REQUIRE( store.bindOutputFile( outputPath ) );

    // Append more than 1MB of data to trigger auto-flush
    const QByteArray bigLine = QByteArray( 1024, 'X' ) + "\n";
    for ( int i = 0; i < 1040; ++i ) {
        store.appendUtf8( bigLine );
    }

    // Data should have been auto-flushed (1040KB > 1MB threshold)
    QFile output( outputPath );
    REQUIRE( output.open( QIODevice::ReadOnly ) );
    REQUIRE( output.size() > 1024 * 1024 );
}

TEST_CASE( "CaptureStore batched output auto-flushes after line threshold" )
{
    const auto rootPath = makeTestDir( "capturestore_line_threshold" );
    const auto outputPath = QDir( rootPath ).filePath( QStringLiteral( "lines.log" ) );

    CaptureStore store( makeCaptureId(), rootPath );
    REQUIRE( store.bindOutputFile( outputPath ) );

    // Append more than 1000 lines (each small enough to stay under 1MB total)
    for ( int i = 0; i < 1010; ++i ) {
        store.appendUtf8( QStringLiteral( "ln-%1\n" ).arg( i ).toUtf8() );
    }

    // Data should have been auto-flushed (1010 lines > 1000 line threshold)
    const auto content = readUtf8File( outputPath );
    REQUIRE( content.contains( QStringLiteral( "ln-0" ) ) );
}

TEST_CASE( "CaptureStore finishInput flushes pending output data" )
{
    const auto rootPath = makeTestDir( "capturestore_finish_flush" );
    const auto outputPath = QDir( rootPath ).filePath( QStringLiteral( "finish.log" ) );

    CaptureStore store( makeCaptureId(), rootPath );
    REQUIRE( store.bindOutputFile( outputPath ) );

    // Append small amount (under all thresholds)
    store.appendUtf8( QByteArrayLiteral( "pending-data\n" ) );

    // finishInput should flush remaining data
    store.finishInput();

    const auto content = readUtf8File( outputPath );
    REQUIRE( content.contains( QStringLiteral( "pending-data" ) ) );
}

TEST_CASE( "CaptureStore buildRawLines bulk-read spans multiple segments" )
{
    CaptureStore::Limits limits;
    limits.segmentTargetBytes = 16;
    limits.memoryBudgetBytes = 4096;

    const auto rootPath = makeTestDir( "capturestore_bulk_multi_segment" );
    CaptureStore store( makeCaptureId(), rootPath, limits );
    auto* codec = QTextCodec::codecForName( "UTF-8" );

    // Each line is short enough that several lines fit per segment,
    // but with segmentTargetBytes=16, many segments will be created.
    const QStringList expectedLines = {
        QStringLiteral( "aa" ), QStringLiteral( "bb" ), QStringLiteral( "cc" ),
        QStringLiteral( "dd" ), QStringLiteral( "ee" ), QStringLiteral( "ff" ),
        QStringLiteral( "gg" ), QStringLiteral( "hh" ), QStringLiteral( "ii" ),
    };

    for ( const auto& line : expectedLines ) {
        store.appendUtf8( ( line + QLatin1Char( '\n' ) ).toUtf8() );
    }

    REQUIRE( store.lineCount().get() == 9 );

    // Request all lines — this forces the bulk read to traverse multiple segments.
    const auto rawLines = store.buildRawLines( 0_lnum, LinesCount( 9 ), codec, QRegularExpression{} );
    REQUIRE( rawLines.endOfLines.size() == 9 );

    const auto decoded = rawLines.decodeLines();
    REQUIRE( decoded.size() == 9 );
    for ( size_t i = 0; i < decoded.size(); ++i ) {
        INFO( "Multi-segment line " << i );
        REQUIRE( decoded[ i ] == expectedLines[ static_cast<int>( i ) ] );
    }

    // Also verify buildUtf8View produces the same content.
    const auto views = rawLines.buildUtf8View();
    REQUIRE( views.size() == 9 );
    for ( size_t i = 0; i < views.size(); ++i ) {
        INFO( "Multi-segment utf8 view " << i );
        REQUIRE( views[ i ] == expectedLines[ static_cast<int>( i ) ].toStdString() );
    }
}

TEST_CASE( "CaptureStore buildRawLines handles unterminated last line" )
{
    CaptureStore::Limits limits;
    limits.segmentTargetBytes = 64;
    limits.memoryBudgetBytes = 4096;

    const auto rootPath = makeTestDir( "capturestore_unterminated" );
    CaptureStore store( makeCaptureId(), rootPath, limits );
    auto* codec = QTextCodec::codecForName( "UTF-8" );

    // Append data where the last line has no trailing newline.
    store.appendUtf8( QByteArrayLiteral( "alpha\nbeta\nno-newline" ) );
    // finishInput commits the trailing partial line.
    store.finishInput();

    REQUIRE( store.lineCount().get() == 3 );

    const auto rawLines = store.buildRawLines( 0_lnum, LinesCount( 3 ), codec, QRegularExpression{} );
    REQUIRE( rawLines.endOfLines.size() == 3 );

    const auto decoded = rawLines.decodeLines();
    REQUIRE( decoded.size() == 3 );
    REQUIRE( decoded[ 0 ] == QStringLiteral( "alpha" ) );
    REQUIRE( decoded[ 1 ] == QStringLiteral( "beta" ) );
    REQUIRE( decoded[ 2 ] == QStringLiteral( "no-newline" ) );

    // Verify the unterminated line gets a synthetic \n appended in the buffer.
    REQUIRE( rawLines.buffer.back() == '\n' );
}

TEST_CASE( "CaptureStore buildRawLines CRLF normalization in slow path" )
{
    const auto* latin1Codec = QTextCodec::codecForName( "ISO-8859-1" );
    REQUIRE( latin1Codec != nullptr );

    const auto rootPath = makeTestDir( "capturestore_crlf_slow_path" );
    CaptureStore store( makeCaptureId(), rootPath );

    // CRLF line endings with a non-UTF8 codec forces the slow path.
    store.appendUtf8( QByteArrayLiteral( "line1\r\nline2\r\nline3\r\n" ) );
    REQUIRE( store.lineCount().get() == 3 );

    const auto rawLines = store.buildRawLines( 0_lnum, LinesCount( 3 ), const_cast<QTextCodec*>( latin1Codec ), QRegularExpression{} );
    REQUIRE( rawLines.endOfLines.size() == 3 );

    const auto decoded = rawLines.decodeLines();
    REQUIRE( decoded.size() == 3 );
    // \r should be stripped in the slow path.
    REQUIRE( decoded[ 0 ] == QStringLiteral( "line1" ) );
    REQUIRE( decoded[ 1 ] == QStringLiteral( "line2" ) );
    REQUIRE( decoded[ 2 ] == QStringLiteral( "line3" ) );
}

TEST_CASE( "CaptureStore buildRawLines applies prefilter pattern in slow path" )
{
    const auto* latin1Codec = QTextCodec::codecForName( "ISO-8859-1" );
    REQUIRE( latin1Codec != nullptr );

    const auto rootPath = makeTestDir( "capturestore_prefilter" );
    CaptureStore store( makeCaptureId(), rootPath );

    store.appendUtf8( QByteArrayLiteral( "[INFO] message-one\n[WARN] message-two\n" ) );
    REQUIRE( store.lineCount().get() == 2 );

    // A prefilter pattern forces the slow path even with UTF-8 data,
    // but we also use a non-UTF8 codec here to ensure the slow path.
    QRegularExpression prefilter( QStringLiteral( "\\[\\w+\\]\\s*" ) );
    const auto rawLines = store.buildRawLines( 0_lnum, LinesCount( 2 ), const_cast<QTextCodec*>( latin1Codec ), prefilter );
    REQUIRE( rawLines.endOfLines.size() == 2 );

    const auto decoded = rawLines.decodeLines();
    REQUIRE( decoded.size() == 2 );
    REQUIRE( decoded[ 0 ] == QStringLiteral( "message-one" ) );
    REQUIRE( decoded[ 1 ] == QStringLiteral( "message-two" ) );
}

TEST_CASE( "CaptureStore buildRawLines reads from spilled disk segments" )
{
    CaptureStore::Limits limits;
    limits.segmentTargetBytes = 8;
    limits.memoryBudgetBytes = 16;

    const auto rootPath = makeTestDir( "capturestore_disk_read" );
    CaptureStore store( makeCaptureId(), rootPath, limits );
    auto* codec = QTextCodec::codecForName( "UTF-8" );

    const QStringList expectedLines = {
        QStringLiteral( "aaa" ), QStringLiteral( "bbb" ),
        QStringLiteral( "ccc" ), QStringLiteral( "ddd" ),
        QStringLiteral( "eee" ), QStringLiteral( "fff" ),
    };

    for ( const auto& line : expectedLines ) {
        store.appendUtf8( ( line + QLatin1Char( '\n' ) ).toUtf8() );
    }

    REQUIRE( store.lineCount().get() == 6 );
    REQUIRE_FALSE( segmentFiles( store.capturePath() ).empty() );

    // Read all lines — at least some must come from disk (spilled segments).
    const auto rawLines = store.buildRawLines( 0_lnum, LinesCount( 6 ), codec, QRegularExpression{} );
    REQUIRE( rawLines.endOfLines.size() == 6 );

    const auto decoded = rawLines.decodeLines();
    REQUIRE( decoded.size() == 6 );
    for ( size_t i = 0; i < decoded.size(); ++i ) {
        INFO( "Disk-read line " << i );
        REQUIRE( decoded[ i ] == expectedLines[ static_cast<int>( i ) ] );
    }

    // Read a subset from the middle — spanning a spilled segment boundary.
    const auto midLines = store.buildRawLines( LineNumber( 2 ), LinesCount( 3 ), codec, QRegularExpression{} );
    REQUIRE( midLines.endOfLines.size() == 3 );
    const auto midDecoded = midLines.decodeLines();
    REQUIRE( midDecoded[ 0 ] == QStringLiteral( "ccc" ) );
    REQUIRE( midDecoded[ 1 ] == QStringLiteral( "ddd" ) );
    REQUIRE( midDecoded[ 2 ] == QStringLiteral( "eee" ) );
}

TEST_CASE( "CaptureStore buildRawLines CRLF fast path strips carriage returns" )
{
    const auto rootPath = makeTestDir( "capturestore_crlf_fast_path" );
    CaptureStore store( makeCaptureId(), rootPath );
    auto* codec = QTextCodec::codecForName( "UTF-8" );

    // CRLF line endings stored in capture — the fast path scans for \n.
    // The \r before each \n should still be present in raw buffer,
    // but decodeLines should strip them.
    store.appendUtf8( QByteArrayLiteral( "row1\r\nrow2\r\nrow3\r\n" ) );
    REQUIRE( store.lineCount().get() == 3 );

    const auto rawLines = store.buildRawLines( 0_lnum, LinesCount( 3 ), codec, QRegularExpression{} );
    REQUIRE( rawLines.endOfLines.size() == 3 );

    const auto decoded = rawLines.decodeLines();
    REQUIRE( decoded.size() == 3 );
    REQUIRE( decoded[ 0 ] == QStringLiteral( "row1" ) );
    REQUIRE( decoded[ 1 ] == QStringLiteral( "row2" ) );
    REQUIRE( decoded[ 2 ] == QStringLiteral( "row3" ) );
}

TEST_CASE( "CaptureStore clear flushes and resets output" )
{
    const auto rootPath = makeTestDir( "capturestore_clear_flush" );
    const auto outputPath = QDir( rootPath ).filePath( QStringLiteral( "clear.log" ) );

    CaptureStore store( makeCaptureId(), rootPath );
    REQUIRE( store.bindOutputFile( outputPath ) );

    store.appendUtf8( QByteArrayLiteral( "before-clear\n" ) );
    store.clear();

    // After clear, the output file should have been truncated
    QFile output( outputPath );
    REQUIRE( output.open( QIODevice::ReadOnly ) );
    REQUIRE( output.size() == 0 );
}
