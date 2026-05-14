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
#include <thread>
#include <QDir>
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
    REQUIRE( limits.memoryBudgetBytes == 32 * 1024 * 1024 );
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
