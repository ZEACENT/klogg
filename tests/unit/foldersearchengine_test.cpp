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

#include <catch2/catch.hpp>

#include "foldersearchengine.h"
#include "foldersearchtypes.h"
#include "regularexpression.h"
#include "regularexpressionpattern.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTextCodec>

namespace {
QString writeFile( const QTemporaryDir& dir, const QString& name, const QByteArray& bytes )
{
    const QString path = QDir( dir.path() ).absoluteFilePath( name );
    QFile f( path );
    REQUIRE( f.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
    f.write( bytes );
    f.close();
    return path;
}

std::unique_ptr<PatternMatcher> matcherFor( const QString& pattern )
{
    RegularExpression re{ RegularExpressionPattern{ pattern } };
    REQUIRE( re.isValid() );
    return re.createMatcher();
}

// Encodes an ASCII QString into UTF-16 little-endian bytes, optionally with a
// BOM (FF FE). For ASCII text every code unit's high byte is 0x00, which is
// exactly what exercises the multi-byte newline finder.
QByteArray toUtf16LE( const QString& text, bool withBom )
{
    QByteArray out;
    if ( withBom ) {
        out.append( '\xff' );
        out.append( '\xfe' );
    }
    for ( const QChar& ch : text ) {
        const unsigned int u = ch.unicode();
        out.append( static_cast<char>( u & 0xFF ) );
        out.append( static_cast<char>( ( u >> 8 ) & 0xFF ) );
    }
    return out;
}

// Encodes an ASCII QString into UTF-16 big-endian bytes, optionally with a BOM
// (FE FF). For UTF-16BE the LF sequence is 0x00 0x0A, so the 0x0A byte sits at
// lineFeedIndex==1 -- this exercises the !isCheckForward branch of
// findNextMultiByteDelimeter.
QByteArray toUtf16BE( const QString& text, bool withBom )
{
    QByteArray out;
    if ( withBom ) {
        out.append( '\xfe' );
        out.append( '\xff' );
    }
    for ( const QChar& ch : text ) {
        const unsigned int u = ch.unicode();
        out.append( static_cast<char>( ( u >> 8 ) & 0xFF ) );
        out.append( static_cast<char>( u & 0xFF ) );
    }
    return out;
}
} // namespace

TEST_CASE( "scanFile finds known matches with correct line numbers and offsets", "[folder]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    // "foo\n"        -> line 0, bytes [0,4)
    // "bar ERROR\n"  -> line 1, "bar ERROR" is bytes [4,13), newline at 13
    // "baz\n"        -> line 2
    const QString path = writeFile( dir, "a.log", QByteArray( "foo\nbar ERROR\nbaz\n" ) );

    auto matcher = matcherFor( "ERROR" );
    const auto group = FolderSearchEngine::scanFile( path, *matcher );

    REQUIRE( group.filePath == path );
    REQUIRE( group.matches.size() == 1 );
    REQUIRE( group.matches[ 0 ].localLine == 1_lnum );
    REQUIRE( group.matches[ 0 ].lineStartByte == OffsetInFile( 4 ) );
    REQUIRE( group.matches[ 0 ].lineEndByte == OffsetInFile( 14 ) ); // past the newline
    REQUIRE( group.matches[ 0 ].lineLength == 9_length ); // "bar ERROR"
}

TEST_CASE( "scanFile matches across multiple lines", "[folder]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString path = writeFile( dir, "a.log", QByteArray( "MATCH\nnope\nMATCH again\n" ) );

    auto matcher = matcherFor( "MATCH" );
    const auto group = FolderSearchEngine::scanFile( path, *matcher );

    REQUIRE( group.matches.size() == 2 );
    REQUIRE( group.matches[ 0 ].localLine == 0_lnum );
    REQUIRE( group.matches[ 1 ].localLine == 2_lnum );
}

TEST_CASE( "scanFile skips binary files (grep -I)", "[folder]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    QByteArray content = QByteArray( "ERROR line\n" );
    content.append( '\0' ); // NUL within the first 32 KB => binary
    content.append( "more ERROR\n" );
    const QString path = writeFile( dir, "bin.log", content );

    auto matcher = matcherFor( "ERROR" );
    const auto group = FolderSearchEngine::scanFile( path, *matcher );
    REQUIRE( group.matches.empty() );
}

TEST_CASE( "scanFile handles a line spanning multiple read blocks", "[folder]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    // A single long line (>16 B block size) containing a match, then a newline.
    const QByteArray head( 20, 'x' );
    const QByteArray tail( 20, 'y' );
    const QByteArray content = head + " NEEDLE " + tail + "\n";
    const QString path = writeFile( dir, "long.log", content );

    auto matcher = matcherFor( "NEEDLE" );
    const auto group = FolderSearchEngine::scanFile( path, *matcher, 16 ); // tiny block size

    REQUIRE( group.matches.size() == 1 );
    REQUIRE( group.matches[ 0 ].localLine == 0_lnum );
    REQUIRE( group.matches[ 0 ].lineStartByte == OffsetInFile( 0 ) );
    REQUIRE( group.matches[ 0 ].lineEndByte == OffsetInFile( content.size() ) );
}

TEST_CASE( "scanFile handles a final line without trailing newline", "[folder]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString path = writeFile( dir, "a.log", QByteArray( "foo\nMATCH" ) ); // no trailing \n

    auto matcher = matcherFor( "MATCH" );
    const auto group = FolderSearchEngine::scanFile( path, *matcher );
    REQUIRE( group.matches.size() == 1 );
    REQUIRE( group.matches[ 0 ].localLine == 1_lnum );
    REQUIRE( group.matches[ 0 ].lineStartByte == OffsetInFile( 4 ) );
    REQUIRE( group.matches[ 0 ].lineEndByte == OffsetInFile( 9 ) ); // EOF
}

TEST_CASE( "FolderSearchEngine aggregates multiple files by file then line", "[folder]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString aPath = writeFile( dir, "a.log", QByteArray( "x MATCH\ny\nMATCH z\n" ) );
    const QString bPath = writeFile( dir, "b.log", QByteArray( "MATCH\n" ) );

    FolderSearchEngine engine;
    engine.scanSynchronously( QStringList{ aPath, bPath }, RegularExpressionPattern( "MATCH" ) );
    const auto results = engine.takeResults();

    REQUIRE( results.size() == 2 );
    REQUIRE( results[ 0 ].filePath == aPath );
    REQUIRE( results[ 0 ].matches.size() == 2 );
    REQUIRE( results[ 0 ].matches[ 0 ].localLine == 0_lnum );
    REQUIRE( results[ 0 ].matches[ 1 ].localLine == 2_lnum );
    REQUIRE( results[ 1 ].filePath == bPath );
    REQUIRE( results[ 1 ].matches.size() == 1 );
    REQUIRE( engine.matchCount() == 3 );
}

TEST_CASE( "FolderSearchEngine drops files with no matches", "[folder]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString aPath = writeFile( dir, "a.log", QByteArray( "MATCH\n" ) );
    const QString bPath = writeFile( dir, "b.log", QByteArray( "nothing here\n" ) );

    FolderSearchEngine engine;
    engine.scanSynchronously( QStringList{ aPath, bPath }, RegularExpressionPattern( "MATCH" ) );
    const auto results = engine.takeResults();

    REQUIRE( results.size() == 1 );
    REQUIRE( results[ 0 ].filePath == aPath );
}

TEST_CASE( "FolderSearchEngine emits searchStarted and searchFinished", "[folder]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString path = writeFile( dir, "a.log", QByteArray( "MATCH\n" ) );

    FolderSearchEngine engine;
    QSignalSpy startedSpy( &engine, &FolderSearchEngine::searchStarted );
    QSignalSpy finishedSpy( &engine, &FolderSearchEngine::searchFinished );
    const auto gen = engine.scanSynchronously( QStringList{ path }, RegularExpressionPattern( "MATCH" ) );

    REQUIRE( startedSpy.count() == 1 );
    REQUIRE( finishedSpy.count() == 1 );
    REQUIRE( startedSpy.takeFirst()[ 0 ].toULongLong() == gen );
    REQUIRE( finishedSpy.takeFirst()[ 0 ].toULongLong() == gen );
}

TEST_CASE( "FolderSearchEngine stale generation produces no new results", "[folder]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString path = writeFile( dir, "a.log", QByteArray( "MATCH\n" ) );

    FolderSearchEngine engine;
    engine.scanSynchronously( QStringList{ path }, RegularExpressionPattern( "MATCH" ) );
    REQUIRE( engine.takeResults().size() == 1 );

    engine.bumpGeneration(); // a newer search is now current
    engine.runSearch( engine.currentGeneration() - 1, QStringList{ path },
                      RegularExpressionPattern( "MATCH" ) ); // stale gen -> no-op
    REQUIRE( engine.takeResults().empty() );

    // A current-generation run DOES refill results.
    engine.runSearch( engine.currentGeneration(), QStringList{ path },
                      RegularExpressionPattern( "MATCH" ) );
    REQUIRE( engine.takeResults().size() == 1 );
}

TEST_CASE( "FolderSearchEngine interrupt stops the scan", "[folder]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString path = writeFile( dir, "a.log", QByteArray( "MATCH\n" ) );

    FolderSearchEngine engine;
    engine.bumpGeneration();
    engine.interrupt();
    engine.runSearch( engine.currentGeneration(), QStringList{ path },
                      RegularExpressionPattern( "MATCH" ) );
    REQUIRE( engine.takeResults().empty() );
}

// --- multi-encoding support (read-only) ---

TEST_CASE( "scanFile matches UTF-16LE text with correct raw byte offsets", "[folder]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    // UTF-16LE with BOM (FF FE) so detection is deterministic. Each char is
    // 2 bytes, so the BOM shifts every offset by 2 vs. the no-BOM case.
    //   BOM(2) "foo\n"(8) -> line 0 = [0,10)
    //   "ERROR here\n"(22) -> line 1 = [10,32)
    //   "baz\n"(8) -> line 2 = [32,40)
    const QByteArray content = toUtf16LE( QStringLiteral( "foo\nERROR here\nbaz\n" ), true );
    const QString path = writeFile( dir, "utf16le.log", content );

    auto matcher = matcherFor( "ERROR" );
    const auto group = FolderSearchEngine::scanFile( path, *matcher );

    REQUIRE( group.filePath == path );
    REQUIRE( group.matches.size() == 1 );
    REQUIRE( group.sourceCodec != nullptr );
    // Detection must have picked a UTF-16 codec (not fallen back to UTF-8).
    REQUIRE( group.sourceCodec->name().contains( "UTF-16" ) );
    REQUIRE( group.matches[ 0 ].localLine == 1_lnum );
    REQUIRE( group.matches[ 0 ].lineStartByte == OffsetInFile( 10 ) );
    REQUIRE( group.matches[ 0 ].lineEndByte == OffsetInFile( 32 ) ); // past the whole LF
}

TEST_CASE( "scanFile matches UTF-16BE text (lineFeedIndex==1 advance path)", "[folder]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    // UTF-16BE with BOM (FE FF). Same byte layout as the LE case (2 bytes/char
    // + 2-byte BOM), so offsets match -- but the LF sequence is 0x00 0x0A, so
    // lineFeedIndex==1 and the past-newline advance must use nl+1, not nl+2.
    const QByteArray content = toUtf16BE( QStringLiteral( "foo\nERROR here\nbaz\n" ), true );
    const QString path = writeFile( dir, "utf16be.log", content );

    auto matcher = matcherFor( "ERROR" );
    const auto group = FolderSearchEngine::scanFile( path, *matcher );

    REQUIRE( group.matches.size() == 1 );
    REQUIRE( group.sourceCodec != nullptr );
    REQUIRE( group.sourceCodec->name().contains( "UTF-16" ) );
    REQUIRE( group.matches[ 0 ].localLine == 1_lnum );
    REQUIRE( group.matches[ 0 ].lineStartByte == OffsetInFile( 10 ) );
    REQUIRE( group.matches[ 0 ].lineEndByte == OffsetInFile( 32 ) );
}

TEST_CASE( "scanFile does not skip a UTF-16LE file as binary", "[folder]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    // A UTF-16LE ASCII file is full of 0x00 high bytes; the binary NUL check
    // is gated on lineFeedWidth==1, so this must NOT be skipped as binary and
    // MUST still match.
    const QByteArray content = toUtf16LE( QStringLiteral( "all good\nMATCH line\n" ), true );
    const QString path = writeFile( dir, "utf16le_nul.log", content );

    auto matcher = matcherFor( "MATCH" );
    const auto group = FolderSearchEngine::scanFile( path, *matcher );
    REQUIRE( group.matches.size() == 1 );
    REQUIRE( group.matches[ 0 ].localLine == 1_lnum );
}

TEST_CASE( "scanFile multi-byte line spanning a read-block boundary", "[folder]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    // A UTF-16LE line longer than the tiny block size, with the LF sequence
    // split across the carried bytes -- proves carry keeps raw bytes and the
    // multi-byte finder resolves the LF in the joined view.
    const QByteArray content = toUtf16LE(
        QStringLiteral( "aaaaaa NEEDLE bbbbbbbbbbbb\n" ), true );
    const QString path = writeFile( dir, "long16.log", content );

    auto matcher = matcherFor( "NEEDLE" );
    const auto group = FolderSearchEngine::scanFile( path, *matcher, 16 ); // tiny block size
    REQUIRE( group.matches.size() == 1 );
    REQUIRE( group.matches[ 0 ].localLine == 0_lnum );
    // Line 0 includes the BOM in its raw byte range ([0, end)); the codec
    // strips the BOM on decode, so this is correct raw-file-space accounting.
    REQUIRE( group.matches[ 0 ].lineStartByte == OffsetInFile( 0 ) );
    REQUIRE( group.matches[ 0 ].lineEndByte == OffsetInFile( content.size() ) );
}

TEST_CASE( "FolderSearchEngine mixes UTF-8 and UTF-16LE files with per-file codec", "[folder]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString utf8Path = writeFile( dir, "a.log", QByteArray( "MATCH\n" ) );
    const QString utf16Path
        = writeFile( dir, "b.log", toUtf16LE( QStringLiteral( "MATCH\n" ), true ) );

    FolderSearchEngine engine;
    engine.scanSynchronously( QStringList{ utf8Path, utf16Path },
                              RegularExpressionPattern( "MATCH" ) );
    const auto results = engine.takeResults();

    REQUIRE( results.size() == 2 );
    REQUIRE( results[ 0 ].filePath == utf8Path );
    REQUIRE( results[ 0 ].matches.size() == 1 );
    // UTF-8 file: sourceCodec may be the UTF-8 codec, but must not be a UTF-16.
    REQUIRE( results[ 1 ].filePath == utf16Path );
    REQUIRE( results[ 1 ].matches.size() == 1 );
    REQUIRE( results[ 1 ].sourceCodec != nullptr );
    REQUIRE( results[ 1 ].sourceCodec->name().contains( "UTF-16" ) );
    REQUIRE( engine.matchCount() == 2 );
}

TEST_CASE( "scanFile resolves a UTF-16LE line-feed split across a block boundary", "[folder]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    // "AA NEEDLE\nBB NEEDLE\n" in UTF-16LE: 20 bytes/line; the first LF (0x0A 0x00)
    // is at raw bytes 18-19. blockSize=19 leaves the 0x0A at the end of block 1
    // and the 0x00 at the start of block 2 -- a split LF that must still produce
    // TWO matched lines (regression: without seam handling it merges into one).
    const QByteArray content = toUtf16LE( QStringLiteral( "AA NEEDLE\nBB NEEDLE\n" ), true );
    const QString path = writeFile( dir, "le.log", content );

    auto matcher = matcherFor( "NEEDLE" );
    const auto group = FolderSearchEngine::scanFile( path, *matcher, 19 );

    REQUIRE( group.matches.size() == 2 );
    REQUIRE( group.matches[ 0 ].localLine == 0_lnum );
    REQUIRE( group.matches[ 1 ].localLine == 1_lnum );
}

TEST_CASE( "scanFile resolves a UTF-16BE line-feed split across a block boundary", "[folder]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    // UTF-16BE LF is 0x00 0x0A; with blockSize=19 the 0x00 sits at the end of
    // block 1 and the 0x0A at the start of block 2. Must still be two lines.
    const QByteArray content = toUtf16BE( QStringLiteral( "AA NEEDLE\nBB NEEDLE\n" ), true );
    const QString path = writeFile( dir, "be.log", content );

    auto matcher = matcherFor( "NEEDLE" );
    const auto group = FolderSearchEngine::scanFile( path, *matcher, 19 );

    REQUIRE( group.matches.size() == 2 );
    REQUIRE( group.matches[ 0 ].localLine == 0_lnum );
    REQUIRE( group.matches[ 1 ].localLine == 1_lnum );
}
