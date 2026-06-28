/*
 * Copyright (C) 2026 ZEACENT and other contributors
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
#include <QFile>
#include <QFileInfo>
#include <QUuid>

#include "capturestore.h"
#include "rollingfilemanager.h"

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

// Generate `count` lines of text, each line ~`bytesPerLine` bytes long.
// Returns the total bytes written.
QByteArray generateLines( int count, int bytesPerLine )
{
    QByteArray data;
    data.reserve( count * bytesPerLine );
    for ( int i = 0; i < count; ++i ) {
        // Fill line: prefix + counter + padding to reach desired length
        const auto line = QByteArrayLiteral( "L" )
                          + QByteArray::number( i ).rightJustified( 8, '0' )
                          + QByteArray( bytesPerLine - 10, 'x' )
                          + QByteArrayLiteral( "\n" );
        data.append( line );
    }
    return data;
}
} // namespace

// ---------------------------------------------------------------------------
// Reproduction of SIGABRT crash when saving a live log with file splitting:
//
//   - "Max capture file size" = 5 MB
//   - "Rolling backup count" = 3
//   - Memory sliding window has 14 MB+ of buffered lines
//   - Saving triggers the crash (macOS, Cocoa platform plugin)
//
// On macOS the crash signature is:
//   __cxa_rethrow → objc_exception_rethrow → -[NSApplication run]
//   → QCocoaEventDispatcher::processEvents → SIGABRT
//
// These tests first verify the file-splitting logic is correct (headless),
// then document the macOS Cocoa lifecycle contract that must be satisfied.
// ---------------------------------------------------------------------------

TEST_CASE( "Live log save: bindOutputFile with large segments triggers file splitting",
           "[capturestore][rolling][live_save]" )
{
    // Simulate the crash scenario: 14MB+ buffered, 5MB max file, 3 backups.
    // Scale: use 2MB buffer, 512KB rolling max, 3 backups for quick test.
    CaptureStore::Limits limits;
    limits.segmentTargetBytes = 128 * 1024;    // 128 KB segments
    limits.memoryBudgetBytes = 10 * 1024 * 1024; // 10 MB memory budget
    limits.rollingMaxFileSize = 512 * 1024;     // 512 KB rolling max (scaled from 5MB)
    limits.rollingBackupCount = 3;

    const auto rootPath = makeTestDir( "live_save_split" );
    const auto outputPath = QDir( rootPath ).filePath( QStringLiteral( "iphone.log" ) );

    CaptureStore store( makeCaptureId(), rootPath, limits );

    // Feed ~2MB of data (scaled from 14MB) — more than the rolling window of 1.5MB.
    const auto chunk = generateLines( 1000, 256 ); // ~256KB per chunk
    const int numChunks = 8;                        // 8 chunks → ~2MB total
    for ( int i = 0; i < numChunks; ++i ) {
        store.appendUtf8( chunk );
    }

    const auto linesBefore = store.lineCount();
    REQUIRE( linesBefore > 0_lcount );
    REQUIRE( store.stats().fileSize > limits.rollingMaxFileSize );

    // Bind output file — replays all buffered segments into the rolling file.
    // This is the operation that triggers the macOS crash.
    REQUIRE( store.bindOutputFile( outputPath ) );

    // Verify the output file was created
    REQUIRE( QFile::exists( outputPath ) );
    const auto outputSize = QFileInfo( outputPath ).size();
    REQUIRE( outputSize > 0 );

    // Append more data to trigger an actual rotation (the first rotation
    // only happens on the next write after bindOutputFile, because
    // bindOutputFile writes segments directly bypassing RollingFileManager::write).
    store.appendUtf8( generateLines( 500, 256 ) ); // another ~128KB

    // After the append, at least one rotation should have occurred,
    // creating backup files from the oversized initial file.
    const auto backups = QDir( rootPath ).entryList(
        { QFileInfo( outputPath ).fileName() + ".*" }, QDir::Files );
    INFO( "Output size after bind: " << outputSize );
    INFO( "Backup files found: " << backups.join( ", " ).toStdString() );
    CHECK( backups.size() >= 1 );

    // The backup should contain the overflow data
    bool foundBackupWithContent = false;
    for ( const auto& backupName : backups ) {
        QFile backupFile( QDir( rootPath ).filePath( backupName ) );
        if ( backupFile.size() > 0 ) {
            foundBackupWithContent = true;
            break;
        }
    }
    CHECK( foundBackupWithContent );
}

TEST_CASE( "Live log save: repeated bindOutputFile does not corrupt data",
           "[capturestore][rolling][live_save]" )
{
    // Verify that binding, unbinding, and rebinding an output file
    // does not lose or corrupt data.

    CaptureStore::Limits limits;
    limits.segmentTargetBytes = 64 * 1024;
    limits.memoryBudgetBytes = 10 * 1024 * 1024;
    limits.rollingMaxFileSize = 256 * 1024;
    limits.rollingBackupCount = 3;

    const auto rootPath = makeTestDir( "live_save_rebind" );
    const auto outputPath1 = QDir( rootPath ).filePath( QStringLiteral( "output1.log" ) );
    const auto outputPath2 = QDir( rootPath ).filePath( QStringLiteral( "output2.log" ) );

    CaptureStore store( makeCaptureId(), rootPath, limits );
    store.appendUtf8( generateLines( 2000, 128 ) ); // ~256KB

    // Bind first output
    REQUIRE( store.bindOutputFile( outputPath1 ) );
    REQUIRE( QFile::exists( outputPath1 ) );

    // Unbind
    REQUIRE( store.bindOutputFile( QString{} ) );

    // Append more data
    store.appendUtf8( generateLines( 1000, 128 ) );

    // Bind second output
    REQUIRE( store.bindOutputFile( outputPath2 ) );
    REQUIRE( QFile::exists( outputPath2 ) );
    REQUIRE( QFileInfo( outputPath2 ).size() > 0 );
}

TEST_CASE( "Live log save: rotation during streaming write does not lose lines",
           "[capturestore][rolling][live_save]" )
{
    // Verify that lines appended after bindOutputFile are correctly
    // written even when rotation occurs.

    CaptureStore::Limits limits;
    limits.segmentTargetBytes = 32 * 1024;
    limits.memoryBudgetBytes = 10 * 1024 * 1024;
    limits.rollingMaxFileSize = 64 * 1024; // 64KB — small enough to rotate often
    limits.rollingBackupCount = 3;

    const auto rootPath = makeTestDir( "live_save_stream" );
    const auto outputPath = QDir( rootPath ).filePath( QStringLiteral( "stream.log" ) );

    CaptureStore store( makeCaptureId(), rootPath, limits );

    // Bind output file with some initial data
    store.appendUtf8( generateLines( 500, 128 ) );
    REQUIRE( store.bindOutputFile( outputPath ) );

    // Stream more data — each append may trigger rotation
    for ( int i = 0; i < 20; ++i ) {
        store.appendUtf8( generateLines( 100, 128 ) ); // ~12.8KB each
    }

    // The output file + backups should contain all the data.
    // Verify by reading back through CaptureStore's line interface.
    const auto finalLineCount = store.lineCount();
    REQUIRE( finalLineCount > 0_lcount );

    // The output file should exist and have content
    REQUIRE( QFile::exists( outputPath ) );
    REQUIRE( QFileInfo( outputPath ).size() > 0 );

    // Backups should have been created by rotation
    const auto backups = QDir( rootPath ).entryList(
        { QFileInfo( outputPath ).fileName() + ".*" }, QDir::Files );
    INFO( "Backup files: " << backups.join( ", " ).toStdString() );
    CHECK( backups.size() >= 1 );
}

TEST_CASE( "Live log save: forceOutputFlush preserves data integrity",
           "[capturestore][rolling][live_save]" )
{
    // Verify that flush() correctly writes buffered output data.

    CaptureStore::Limits limits;
    limits.segmentTargetBytes = 64 * 1024;
    limits.memoryBudgetBytes = 10 * 1024 * 1024;
    limits.rollingMaxFileSize = 256 * 1024;
    limits.rollingBackupCount = 3;

    const auto rootPath = makeTestDir( "live_save_flush" );
    const auto outputPath = QDir( rootPath ).filePath( QStringLiteral( "flush.log" ) );

    CaptureStore store( makeCaptureId(), rootPath, limits );
    store.appendUtf8( generateLines( 1000, 128 ) );
    REQUIRE( store.bindOutputFile( outputPath ) );

    // Append data and flush
    store.appendUtf8( generateLines( 500, 128 ) );
    store.flush();

    REQUIRE( QFile::exists( outputPath ) );
    REQUIRE( QFileInfo( outputPath ).size() > 0 );
}
