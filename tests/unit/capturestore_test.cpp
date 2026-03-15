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
