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

// Regression test for the macOS SIGABRT crash that occurred when saving a live
// log whose in-memory window exceeds the rolling file size, forcing file
// splitting during the save, followed by continued streaming.
//
// Root cause: StreamingLogData::appendUtf8() used to compute the range of lines
// to write to the Strip-mode display file as [previousLineCount, currentLineCount),
// i.e. count = currentLineCount - previousLineCount (both uint64).  When
// CaptureStore::trimToLimits() (invoked during the append once the in-memory
// window exceeds rollingMaxFileSize * rollingBackupCount) removed MORE lines
// than were appended, currentLineCount < previousLineCount and the uint64
// subtraction wrapped to ~2^64.  getLines() then did lines.reserve(lastLine -
// first), which underflowed too and threw std::length_error("vector").
//
// On macOS that exception is thrown from a slot on the main event loop (queued
// QProcess::readyRead -> AdbLogcatSource -> appendUtf8) and is uncaught, so it
// propagates through the Cocoa event dispatcher and aborts with SIGABRT
// (signature: objc_exception_rethrow -> -[NSApplication run]).  An uncaught
// C++ exception from an event-loop slot is what produced the crash that three
// prior commits mis-diagnosed as an AppKit NSException.

#include <catch2/catch.hpp>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUuid>

#include "capturestore.h"
#include "streaminglogdata.h"

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

// iOS-syslog-style line with ANSI color escapes, ~300 bytes.
QByteArray makeAnsiLine( int index )
{
    QByteArray line;
    line.reserve( 320 );
    line.append( "\x1b[90m2026-06-28 17:42:0" );
    line.append( QByteArray::number( index % 10 ) );
    line.append( "Z device\x1b[0m " );
    const char* colors[] = { "\x1b[31m", "\x1b[33m", "\x1b[32m", "\x1b[36m", "\x1b[0m" };
    line.append( colors[ index % 5 ] );
    line.append( "App[" + QByteArray::number( 1000 + index % 500 ) + "]: " );
    line.append( "streaming payload segment " );
    line.append( QByteArray::number( index ) );
    line.append( " " );
    line.append( QByteArray( 200, 'x' ) );
    line.append( "\x1b[0m\n" );
    return line;
}

QByteArray makeChunk( int lineStart, int lineCount )
{
    QByteArray chunk;
    chunk.reserve( lineCount * 340 );
    for ( int i = 0; i < lineCount; ++i ) {
        chunk.append( makeAnsiLine( lineStart + i ) );
    }
    return chunk;
}
} // namespace

TEST_CASE( "StreamingLogData: save with file splitting then stream past window does not crash",
           "[streaming][live_save][live_crash]" )
{
    // Field scenario: Max capture file size = 5 MB, Rolling backup count = 3
    // (window = 15 MB), in-memory window > 14 MB before save.
    constexpr qint64 RollingMaxBytes = 5 * 1024 * 1024;
    constexpr int RollingBackups = 3;
    constexpr qint64 WindowBytes = RollingMaxBytes * RollingBackups;

    StreamingLogData log( QUuid::createUuid().toString( QUuid::WithoutBraces ) );

    CaptureStore::Limits limits;
    limits.segmentTargetBytes = 1 * 1024 * 1024;
    limits.memoryBudgetBytes = 256 * 1024 * 1024;
    limits.rollingMaxFileSize = RollingMaxBytes;
    limits.rollingBackupCount = RollingBackups;
    log.setCaptureLimits( limits );

    // Fill the window to just under the limit so NO trimming happens during the
    // initial feed — mirroring the field case where trimming only kicks in
    // after the save, as streaming continues.
    int lineIndex = 0;
    constexpr int LinesPerChunk = 500;
    constexpr qint64 TargetBytes = 14 * 1024 * 1024;
    qint64 fed = 0;
    while ( fed < TargetBytes ) {
        const auto chunk = makeChunk( lineIndex, LinesPerChunk );
        lineIndex += LinesPerChunk;
        fed += chunk.size();
        log.appendUtf8( chunk );
    }

    const auto linesAtSave = log.getNbLine().get();
    REQUIRE( linesAtSave > 0 );
    REQUIRE( log.getFileSize() > TargetBytes );
    REQUIRE( log.getFileSize() < WindowBytes ); // no trim yet

    const auto dir = makeTestDir( "live_crash_repro" );
    const auto outputPath = QDir( dir ).filePath( QStringLiteral( "iphone.log" ) );

    // The save ("Without ANSI Sequences" = Strip mode).  openDisplayOutputFile()
    // writes the whole >14MB window through the 5MB rolling file, rotating it.
    REQUIRE( log.bindOutputFile( outputPath, LiveLogSaveAnsiMode::Strip ) );
    REQUIRE( QFile::exists( outputPath ) );

    // Continue streaming well past the 15 MB window.  These appends trigger
    // trimToLimits(), which used to underflow the line range and throw.
    bool threw = false;
    std::string what;
    bool observedTrim = false;
    qint64 observedMin = std::numeric_limits<qint64>::max();

    for ( int step = 0; step < 80; ++step ) {
        const auto before = static_cast<qint64>( log.getNbLine().get() );
        try {
            log.appendUtf8( makeChunk( lineIndex, LinesPerChunk / 5 ) );
            lineIndex += LinesPerChunk / 5;
        } catch ( const std::exception& e ) {
            threw = true;
            what = e.what();
            break;
        }
        const auto after = static_cast<qint64>( log.getNbLine().get() );
        observedMin = std::min( observedMin, after );
        // Trimming removes oldest segments, so a net decrease across an append
        // proves the buggy path is exercised.
        if ( after < before ) {
            observedTrim = true;
        }
    }

    INFO( "post-save streaming threw: " << ( threw ? what : std::string( "no" ) ) );
    REQUIRE_FALSE( threw );
    REQUIRE( observedTrim ); // genuinely exercised the trim path
    REQUIRE( observedMin < static_cast<qint64>( linesAtSave ) ); // window is bounded

    // The display file rotated into backups (file splitting).
    const auto backups = QDir( dir ).entryList(
        { QFileInfo( outputPath ).fileName() + QStringLiteral( ".*" ) }, QDir::Files );
    CHECK( backups.size() >= 1 );
}
