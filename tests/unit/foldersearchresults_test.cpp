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

#include "foldersearchresults.h"
#include "foldersearchtypes.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTextCodec>

#include <atomic>
#include <thread>

namespace {
klogg::folder::MatchRecord match( uint64_t localLine, int64_t start, int64_t end,
                                  int expandedLen, int matchLen )
{
    klogg::folder::MatchRecord m;
    m.localLine = LineNumber( localLine );
    m.lineStartByte = OffsetInFile( start );
    m.lineEndByte = OffsetInFile( end );
    m.lineLength = LineLength( expandedLen );
    m.matchLen = LineLength( matchLen );
    return m;
}

// Writes the given lines (joined by '\n', with a trailing newline) to a temp
// file and returns its absolute path. Used to exercise the on-demand text fetch.
QString writeFile( const QTemporaryDir& dir, const QString& name, const QStringList& lines )
{
    const QString path = QDir( dir.path() ).absoluteFilePath( name );
    QFile f( path );
    REQUIRE( f.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
    f.write( lines.join( '\n' ).toUtf8() );
    f.write( "\n" );
    f.close();
    return path;
}
} // namespace

TEST_CASE( "FolderSearchResults empty has 0 lines", "[folder]" )
{
    FolderSearchResults r;
    REQUIRE( r.getNbLine() == 0_lcount );
    REQUIRE( r.getMaxLength() == 0_length );
    REQUIRE( r.groupCount() == 0 );
}

TEST_CASE( "FolderSearchResults single file single match is header + match row", "[folder]" )
{
    FolderSearchResults r;
    klogg::folder::FileGroup g;
    g.filePath = "/tmp/a.log";
    g.matches.push_back( match( 4, 0, 10, 10, 5 ) );
    r.setResults( { g } );

    REQUIRE( r.groupCount() == 1 );
    REQUIRE( r.getNbLine() == 2_lcount ); // 1 header + 1 match
    REQUIRE( r.lineKind( 0_lnum ) == LineKind::Header );
    REQUIRE( r.lineKind( 1_lnum ) == LineKind::Data );

    const auto src = r.sourceForLine( 1_lnum );
    REQUIRE( src.filePath == "/tmp/a.log" );
    REQUIRE( src.localLine == 4_lnum );
}

TEST_CASE( "FolderSearchResults header text shows path and total count", "[folder]" )
{
    FolderSearchResults r;
    klogg::folder::FileGroup g;
    g.filePath = "/var/log/srv.log";
    g.matches.push_back( match( 1, 0, 10, 10, 3 ) );
    g.matches.push_back( match( 2, 0, 10, 10, 3 ) );
    r.setResults( { g } );

    const auto header = r.getLineString( 0_lnum );
    INFO( "header: " << header.toStdString() );
    REQUIRE( header.contains( "srv.log" ) );
    REQUIRE( header.contains( "2" ) ); // total matches
}

TEST_CASE( "FolderSearchResults files with no matches are hidden", "[folder]" )
{
    FolderSearchResults r;
    klogg::folder::FileGroup a;
    a.filePath = "/a.log";
    a.matches.push_back( match( 0, 0, 5, 5, 1 ) );
    klogg::folder::FileGroup empty;
    empty.filePath = "/empty.log"; // zero matches
    r.setResults( { a, empty } );

    REQUIRE( r.groupCount() == 1 ); // empty group dropped
    REQUIRE( r.getNbLine() == 2_lcount );
}

TEST_CASE( "FolderSearchResults multiple groups keep file/line order", "[folder]" )
{
    FolderSearchResults r;
    klogg::folder::FileGroup a;
    a.filePath = "/a.log";
    a.matches.push_back( match( 0, 0, 5, 5, 1 ) );
    a.matches.push_back( match( 3, 0, 5, 5, 1 ) );
    klogg::folder::FileGroup b;
    b.filePath = "/b.log";
    b.matches.push_back( match( 1, 0, 5, 5, 1 ) );
    r.setResults( { a, b } );

    // a: header(0) + 2 matches (1,2); b: header(3) + 1 match (4)
    REQUIRE( r.getNbLine() == 5_lcount );
    REQUIRE( r.lineKind( 0_lnum ) == LineKind::Header );
    REQUIRE( r.sourceForLine( 1_lnum ).filePath == "/a.log" );
    REQUIRE( r.sourceForLine( 2_lnum ).filePath == "/a.log" );
    REQUIRE( r.lineKind( 3_lnum ) == LineKind::Header );
    REQUIRE( r.sourceForLine( 4_lnum ).filePath == "/b.log" );
}

TEST_CASE( "FolderSearchResults collapse hides a group's matches", "[folder]" )
{
    FolderSearchResults r;
    klogg::folder::FileGroup a;
    a.filePath = "/a.log";
    a.matches.push_back( match( 0, 0, 5, 5, 1 ) );
    klogg::folder::FileGroup b;
    b.filePath = "/b.log";
    b.matches.push_back( match( 1, 0, 5, 5, 1 ) );
    b.matches.push_back( match( 2, 0, 5, 5, 1 ) );
    r.setResults( { a, b } );
    REQUIRE( r.getNbLine() == 5_lcount ); // a(2) + b(3)

    r.toggleCollapse( 1 ); // collapse b (fileId == group index)
    REQUIRE( r.isCollapsed( 1 ) );
    REQUIRE( r.getNbLine() == 3_lcount ); // a(2) + b header only(1)

    // b's header is now at visible index 2 and still reports its total count.
    REQUIRE( r.lineKind( 2_lnum ) == LineKind::Header );
    const auto bHeader = r.getLineString( 2_lnum );
    INFO( "b header: " << bHeader.toStdString() );
    REQUIRE( bHeader.contains( "b.log" ) );
    REQUIRE( bHeader.contains( "2" ) ); // total matches still reported

    r.toggleCollapse( 1 ); // expand
    REQUIRE_FALSE( r.isCollapsed( 1 ) );
    REQUIRE( r.getNbLine() == 5_lcount );
}

TEST_CASE( "FolderSearchResults collapsed visible indices remap", "[folder]" )
{
    FolderSearchResults r;
    klogg::folder::FileGroup a;
    a.filePath = "/a.log";
    a.matches.push_back( match( 0, 0, 5, 5, 1 ) );
    a.matches.push_back( match( 1, 0, 5, 5, 1 ) );
    klogg::folder::FileGroup b;
    b.filePath = "/b.log";
    b.matches.push_back( match( 0, 0, 5, 5, 1 ) );
    r.setResults( { a, b } ); // a(header+2)=3, b(header+1)=2 -> total 5

    r.toggleCollapse( 0 ); // collapse a -> a header(1) + b(header+1)=2 -> total 3
    REQUIRE( r.getNbLine() == 3_lcount );
    REQUIRE( r.lineKind( 0_lnum ) == LineKind::Header ); // a header
    REQUIRE( r.lineKind( 1_lnum ) == LineKind::Header ); // b header
    REQUIRE( r.lineKind( 2_lnum ) == LineKind::Data );   // b match
    REQUIRE( r.sourceForLine( 2_lnum ).filePath == "/b.log" );
}

TEST_CASE( "FolderSearchResults collapseAll and expandAll", "[folder]" )
{
    FolderSearchResults r;
    klogg::folder::FileGroup a;
    a.filePath = "/a.log";
    a.matches.push_back( match( 0, 0, 5, 5, 1 ) );
    klogg::folder::FileGroup b;
    b.filePath = "/b.log";
    b.matches.push_back( match( 0, 0, 5, 5, 1 ) );
    r.setResults( { a, b } );
    REQUIRE( r.getNbLine() == 4_lcount ); // 2 headers + 2 matches

    r.collapseAll();
    REQUIRE( r.getNbLine() == 2_lcount ); // only the 2 headers
    REQUIRE( r.lineKind( 0_lnum ) == LineKind::Header );
    REQUIRE( r.lineKind( 1_lnum ) == LineKind::Header );

    r.expandAll();
    REQUIRE( r.getNbLine() == 4_lcount );
    REQUIRE_FALSE( r.isCollapsed( 0 ) );
    REQUIRE_FALSE( r.isCollapsed( 1 ) );
}

TEST_CASE( "FolderSearchResults reads match line text from the source file", "[folder]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    // "alpha\n"  -> line 0, bytes [0,6)
    // "beta ERROR\n" -> line 1, bytes [6,17)
    // "gamma\n"  -> line 2, bytes [17,23)
    const QStringList lines{ "alpha", "beta ERROR", "gamma" };
    const QString path = writeFile( dir, "a.log", lines );

    FolderSearchResults r;
    klogg::folder::FileGroup g;
    g.filePath = path;
    g.matches.push_back( match( 1, 6, 17, 10, 5 ) ); // "beta ERROR", match "ERROR"
    r.setResults( { g } );

    REQUIRE( r.lineKind( 0_lnum ) == LineKind::Header );
    REQUIRE( r.lineKind( 1_lnum ) == LineKind::Data );
    REQUIRE( r.getLineString( 1_lnum ) == QString( "beta ERROR" ) );
    REQUIRE( r.getLineLength( 1_lnum ) == 10_length );
}

TEST_CASE( "FolderSearchResults readMatchLine decodes with the file source codec", "[folder]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    // Write a UTF-16LE file (with BOM) holding:
    //   BOM(2) "foo\n"(8) -> line 0 = [0,10)
    //   "ERROR here\n"(22) -> line 1 = [10,32)
    QByteArray content;
    content.append( '\xff' );
    content.append( '\xfe' ); // BOM
    const std::string text = "foo\nERROR here\nbaz\n";
    for ( char c : text ) {
        content.append( c );
        content.append( '\0' ); // UTF-16LE low/high
    }
    const QString path = QDir( dir.path() ).absoluteFilePath( "utf16le.log" );
    {
        QFile f( path );
        REQUIRE( f.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
        f.write( content );
        f.close();
    }

    FolderSearchResults r;
    klogg::folder::FileGroup g;
    g.filePath = path;
    g.sourceCodec = QTextCodec::codecForName( "UTF-16LE" );
    g.matches.push_back( match( 1, 10, 32, 10, 5 ) ); // "ERROR here"
    r.setResults( { g } );

    REQUIRE( r.lineKind( 0_lnum ) == LineKind::Header );
    REQUIRE( r.lineKind( 1_lnum ) == LineKind::Data );
    // Decoded with the SOURCE codec, not the default UTF-8 display encoding:
    // the raw bytes [10,32) are "ERROR here\n" in UTF-16LE.
    const QString line = r.getLineString( 1_lnum );
    INFO( "decoded line: " << line.toStdString() );
    REQUIRE( line == QStringLiteral( "ERROR here" ) );
    // No BOM leaks into non-line-0 lines.
    REQUIRE_FALSE( line.startsWith( QChar( 0xFEFF ) ) );
}

TEST_CASE( "FolderSearchResults readMatchLine falls back to UTF-8 with no source codec", "[folder]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QStringList lines{ "alpha", "beta ERROR", "gamma" };
    const QString path = writeFile( dir, "a.log", lines );

    FolderSearchResults r;
    klogg::folder::FileGroup g;
    g.filePath = path;
    // sourceCodec left null -> readMatchLine must decode as UTF-8.
    g.matches.push_back( match( 1, 6, 17, 10, 5 ) ); // "beta ERROR"
    r.setResults( { g } );

    REQUIRE( r.getLineString( 1_lnum ) == QString( "beta ERROR" ) );
}

TEST_CASE( "FolderSearchResults a marked UTF-16 line under the cache cap decodes correctly",
           "[folder][marks]" )
{
    // The stateful-codec path (UTF-16) uses the whole-file decoded-line cache
    // (a byte scan cannot find line boundaries in UTF-16). UNDER the cap the
    // marked line must decode correctly and report Available.
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString path = QDir( dir.path() ).absoluteFilePath( "utf16.log" );
    {
        QFile f( path );
        REQUIRE( f.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
        // BOM + "alpha\nbeta\ngamma\n" in UTF-16LE.
        QByteArray bytes;
        bytes.append( "\xFF\xFE", 2 );
        for ( const QString& l : { QStringLiteral( "alpha" ), QStringLiteral( "beta" ),
                                   QStringLiteral( "gamma" ) } ) {
            for ( const QChar c : l ) {
                bytes.append( static_cast<char>( c.unicode() & 0xFF ) );
                bytes.append( static_cast<char>( ( c.unicode() >> 8 ) & 0xFF ) );
            }
            bytes.append( '\n' );
            bytes.append( '\0' );
        }
        f.write( bytes );
        f.close();
    }

    FolderSearchResults r;
    klogg::folder::FileGroup g;
    g.filePath = path;
    g.sourceCodec = QTextCodec::codecForName( "UTF-16LE" );
    // One real match so the group exists; the mark is on a different line.
    g.matches.push_back( match( 0, 2, 12, 5, 5 ) ); // "alpha" (BOM at 0-1)
    r.setResults( { g } );

    // Mark line 2 ("gamma") -- not a match -> injected as a mark row.
    QHash<QString, std::set<uint64_t>> marks;
    marks[ path ].insert( 2 );
    r.setMarksStore( &marks );

    // rows: [H0, D1(match line0), D2(mark row line2)].
    REQUIRE( r.getNbLine() == 3_lcount );
    const auto src = r.sourceForLine( 2_lnum );
    REQUIRE( src.localLine == 2_lnum );
    REQUIRE( r.markLineTextStatus( 2_lnum )
             == FolderSearchResults::MarkLineTextStatus::Available );
    REQUIRE( r.getLineString( 2_lnum ) == QStringLiteral( "gamma" ) );
}

TEST_CASE( "FolderSearchResults setResults keeps marks-only paths when membership is unspecified",
           "[folder][marks]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString path = writeFile( dir, "marked-only.log", { "marked text" } );

    FolderSearchResults results;
    results.setResults( {} );
    QHash<QString, std::set<uint64_t>> marks;
    marks[ path ].insert( 0 );
    results.setMarksStore( &marks );

    REQUIRE( results.getNbLine() == 2_lcount );
    REQUIRE( results.sourceForLine( 1_lnum ).filePath == path );
    REQUIRE( results.sourceForLine( 1_lnum ).localLine == 0_lnum );
}

TEST_CASE( "FolderSearchResults beginSearch invalidates the seek-based marked-line text cache",
           "[folder][marks][mark-cache-refresh]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString path = writeFile( dir, "seek.log", { "old text", "ERROR" } );

    auto makeGroup = [ & ] {
        klogg::folder::FileGroup group;
        group.filePath = path;
        group.matches.push_back( match( 1, 9, 15, 5, 5 ) );
        return group;
    };

    FolderSearchResults results;
    results.setResults( { makeGroup() } );
    QHash<QString, std::set<uint64_t>> marks;
    marks[ path ].insert( 0 );
    results.setMarksStore( &marks );

    // rows: [header, marked non-match line 0, match line 1]. Reading the mark
    // populates markTextCache_, the byte-newline-safe seek-path cache.
    REQUIRE( results.sourceForLine( 1_lnum ).localLine == 0_lnum );
    REQUIRE( results.getLineString( 1_lnum ) == QStringLiteral( "old text" ) );

    REQUIRE( writeFile( dir, "seek.log", { "new text", "ERROR" } ) == path );
    results.beginSearch( QStringList{ path } );
    results.addFileGroup( 0, makeGroup() );

    REQUIRE( results.sourceForLine( 1_lnum ).localLine == 0_lnum );
    REQUIRE( results.getLineString( 1_lnum ) == QStringLiteral( "new text" ) );
}

TEST_CASE( "FolderSearchResults beginSearch invalidates the UTF-16 marked-line decode cache",
           "[folder][marks][mark-cache-refresh]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString path = QDir( dir.path() ).absoluteFilePath( "utf16-refresh.log" );

    auto writeUtf16 = [ & ]( const QString& firstLine ) {
        QFile file( path );
        REQUIRE( file.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
        QByteArray bytes;
        bytes.append( "\xFF\xFE", 2 );
        for ( const QString& line : { firstLine, QStringLiteral( "ERROR" ) } ) {
            for ( const QChar c : line ) {
                bytes.append( static_cast<char>( c.unicode() & 0xFF ) );
                bytes.append( static_cast<char>( ( c.unicode() >> 8 ) & 0xFF ) );
            }
            bytes.append( '\n' );
            bytes.append( '\0' );
        }
        REQUIRE( file.write( bytes ) == bytes.size() );
    };
    auto makeGroup = [ & ] {
        klogg::folder::FileGroup group;
        group.filePath = path;
        group.sourceCodec = QTextCodec::codecForName( "UTF-16LE" );
        group.matches.push_back( match( 1, 14, 26, 5, 5 ) );
        return group;
    };

    writeUtf16( QStringLiteral( "alpha" ) );
    FolderSearchResults results;
    results.setResults( { makeGroup() } );
    QHash<QString, std::set<uint64_t>> marks;
    marks[ path ].insert( 0 );
    results.setMarksStore( &marks );

    // The stateful codec forces the whole-file markLineCache_ path.
    REQUIRE( results.sourceForLine( 1_lnum ).localLine == 0_lnum );
    REQUIRE( results.getLineString( 1_lnum ) == QStringLiteral( "alpha" ) );

    writeUtf16( QStringLiteral( "omega" ) );
    results.beginSearch( QStringList{ path } );
    results.addFileGroup( 0, makeGroup() );

    REQUIRE( results.sourceForLine( 1_lnum ).localLine == 0_lnum );
    REQUIRE( results.getLineString( 1_lnum ) == QStringLiteral( "omega" ) );
}

TEST_CASE( "FolderSearchResults a marked line over the cache cap with a stateful codec "
           "reports Unavailable instead of silently rendering empty",
           "[folder][marks][regression]" )
{
    // Regression guard for the 16 MiB whole-file mark-line cache cap: a marked
    // line in a file OVER the cap with a STATEFUL codec (UTF-16 -- the seek
    // path cannot apply) has no text. The model must expose that as an explicit
    // Unavailable state so the view can distinguish "text unavailable" from "the
    // line is empty" -- silently rendering blank was the original defect. The
    // fixture size derives from the SAME kMarkLineCacheCap constant the
    // production cap uses, so a cap change keeps this test on the over-cap side.
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString path = QDir( dir.path() ).absoluteFilePath( "big-utf16.log" );
    {
        QFile f( path );
        REQUIRE( f.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
        QByteArray bytes;
        bytes.append( "\xFF\xFE", 2 ); // BOM
        // Build UTF-16LE filler without QTextCodec to keep the fixture simple.
        QByteArray line;
        for ( const QChar c : QStringLiteral( "filler line to push past the cap\n" ) ) {
            line.append( static_cast<char>( c.unicode() & 0xFF ) );
            line.append( static_cast<char>( ( c.unicode() >> 8 ) & 0xFF ) );
        }
        while ( bytes.size() <= FolderSearchResults::kMarkLineCacheCap ) {
            bytes.append( line );
        }
        f.write( bytes );
        f.close();
    }

    FolderSearchResults r;
    klogg::folder::FileGroup g;
    g.filePath = path;
    g.sourceCodec = QTextCodec::codecForName( "UTF-16LE" );
    g.matches.push_back( match( 0, 2, 12, 5, 5 ) );
    r.setResults( { g } );

    QHash<QString, std::set<uint64_t>> marks;
    marks[ path ].insert( 3 ); // a non-match filler line
    r.setMarksStore( &marks );

    REQUIRE( r.getNbLine() == 3_lcount ); // header + match + mark row
    const auto src = r.sourceForLine( 2_lnum );
    REQUIRE( src.localLine == 3_lnum );
    // RED before the contract: over-cap mark text silently rendered as empty
    // with no way to tell. Now it must report Unavailable.
    REQUIRE( r.markLineTextStatus( 2_lnum )
             == FolderSearchResults::MarkLineTextStatus::Unavailable );
    // And the results view must render an explicit placeholder for the
    // unavailable text, not a silent blank line (the user-facing symptom).
    REQUIRE( r.getLineString( 2_lnum )
             == FolderSearchResults::unavailableMarkLineText() );
    REQUIRE( r.getExpandedLineString( 2_lnum )
             == FolderSearchResults::unavailableMarkLineText() );
}

TEST_CASE( "FolderSearchResults matchLinesForFile returns ascending local lines", "[folder]" )
{
    FolderSearchResults r;
    klogg::folder::FileGroup a;
    a.filePath = "/a.log";
    a.matches.push_back( match( 3, 0, 5, 5, 1 ) );
    a.matches.push_back( match( 7, 0, 5, 5, 1 ) );
    a.matches.push_back( match( 42, 0, 5, 5, 1 ) );
    klogg::folder::FileGroup b;
    b.filePath = "/b.log";
    b.matches.push_back( match( 1, 0, 5, 5, 1 ) );
    r.setResults( { a, b } );

    const auto aLines = r.matchLinesForFile( "/a.log" );
    REQUIRE( aLines.size() == 3 );
    REQUIRE( aLines[ 0 ] == 3_lnum );
    REQUIRE( aLines[ 1 ] == 7_lnum );
    REQUIRE( aLines[ 2 ] == 42_lnum );

    const auto bLines = r.matchLinesForFile( "/b.log" );
    REQUIRE( bLines.size() == 1 );
    REQUIRE( bLines[ 0 ] == 1_lnum );
}

TEST_CASE( "FolderSearchResults matchLinesForFile is empty for an absent file", "[folder]" )
{
    FolderSearchResults r;
    klogg::folder::FileGroup a;
    a.filePath = "/a.log";
    a.matches.push_back( match( 0, 0, 5, 5, 1 ) );
    r.setResults( { a } );

    REQUIRE( r.matchLinesForFile( "/not/here.log" ).empty() );
    REQUIRE( r.matchLinesForFile( QString() ).empty() );
}

TEST_CASE( "FolderSearchResults matchLinesForFile ignores collapse state", "[folder]" )
{
    // matchLinesForFile reads groups_, not visibleRows_, so collapsing a group
    // must NOT change the per-file match list (the folder overview needs the full
    // set even when the results view is collapsed).
    FolderSearchResults r;
    klogg::folder::FileGroup a;
    a.filePath = "/a.log";
    a.matches.push_back( match( 0, 0, 5, 5, 1 ) );
    a.matches.push_back( match( 1, 0, 5, 5, 1 ) );
    a.matches.push_back( match( 2, 0, 5, 5, 1 ) );
    r.setResults( { a } );
    REQUIRE( r.matchLinesForFile( "/a.log" ).size() == 3 );

    r.collapseAll();
    REQUIRE( r.getNbLine() == 1_lcount ); // only the header is visible now
    REQUIRE( r.matchLinesForFile( "/a.log" ).size() == 3 ); // ...but still 3 matches

    r.toggleCollapse( 0 );
    REQUIRE( r.matchLinesForFile( "/a.log" ).size() == 3 );
}

TEST_CASE( "FolderSearchResults getLineNumber maps rows to source lines", "[folder]" )
{
    // Single-file parity: copying with line numbers (or any getLineNumber
    // consumer) must see the SOURCE line numbers, not the result-row indices
    // (cluster E of the 2026-07-18 audit).
    FolderSearchResults r;
    klogg::folder::FileGroup g;
    g.filePath = "/tmp/a.log";
    g.matches.push_back( match( 4, 0, 10, 10, 5 ) );
    g.matches.push_back( match( 9, 0, 10, 10, 5 ) );
    r.setResults( { g } );

    // Row 0 is the group header; rows 1/2 map to source lines 4/9 (0-based).
    // getLineNumber is 1-based, so the source line numbers are 5 and 10.
    REQUIRE( r.getLineNumber( 1_lnum ) == 5_lnum );
    REQUIRE( r.getLineNumber( 2_lnum ) == 10_lnum );
}

TEST_CASE( "FolderSearchResults header rows are not copyable", "[folder]" )
{
    // Group headers are UI chrome, not source lines: the copy / search-
    // composition path skips them via the AbstractLogData copyable hook.
    FolderSearchResults r;
    klogg::folder::FileGroup g;
    g.filePath = "/tmp/a.log";
    g.matches.push_back( match( 4, 0, 10, 10, 5 ) );
    r.setResults( { g } );

    REQUIRE_FALSE( r.isLineCopyable( 0_lnum ) );
    REQUIRE( r.isLineCopyable( 1_lnum ) );
}

TEST_CASE( "FolderSearchResults decodes with the per-file encoding override", "[folder]" )
{
    // Parity with single-file setEncoding: overriding the display encoding of
    // a file must also apply to its RESULT rows (they otherwise decode with
    // the codec detected at scan time, and a misdetection stays mojibake).
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    // "caf\xE9" (e-acute in Latin-1) -- invalid as UTF-8.
    const QString path = QDir( dir.path() ).absoluteFilePath( "latin1.log" );
    {
        QFile f( path );
        REQUIRE( f.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
        f.write( QByteArray( "caf\xE9\n" ) );
        f.close();
    }

    FolderSearchResults r;
    klogg::folder::FileGroup g;
    g.filePath = path;
    // Simulate a scan-time misdetection as UTF-8.
    g.sourceCodec = QTextCodec::codecForName( "UTF-8" );
    g.matches.push_back( match( 0, 0, 4, 4, 3 ) );
    r.setResults( { g } );

    // Without an override the row decodes with the (wrong) detected codec.
    REQUIRE( r.getLineString( 1_lnum ) != QStringLiteral( "café" ) );

    // The override takes precedence over the detected codec, and notifies the
    // view (documented in the header: the view re-renders on layoutChanged).
    QSignalSpy layoutSpy( &r, &FolderSearchResults::layoutChanged );
    r.setEncodingOverrideForFile( path, "ISO 8859-1" );
    REQUIRE( r.getLineString( 1_lnum ) == QStringLiteral( "café" ) );
    REQUIRE( layoutSpy.count() == 1 );

    // Clearing the override returns to the detected codec (and notifies once,
    // only because an entry was actually removed).
    r.clearEncodingOverrideForFile( path );
    REQUIRE( r.getLineString( 1_lnum ) != QStringLiteral( "café" ) );
    REQUIRE( layoutSpy.count() == 2 );

    // No-op clear (nothing to remove) does not notify.
    r.clearEncodingOverrideForFile( path );
    REQUIRE( layoutSpy.count() == 2 );
}

TEST_CASE( "FolderSearchResults byte-newline-safe codec gate is an allowlist", "[folder][marks]" )
{
    // codecIsByteNewlineSafe gates the seek-based mark-line read. It must be an
    // ALLOWLIST: only UTF-8 / known stateless single-byte codecs qualify, and
    // any stateful or unlisted codec returns false so it stays on the whole-file
    // decode path. A denylist would let an unlisted codec with a non-ASCII (or
    // multi-byte) newline encoding silently mis-seek.
    REQUIRE( FolderSearchResults::codecIsByteNewlineSafe( nullptr ) ); // UTF-8 default
    REQUIRE( FolderSearchResults::codecIsByteNewlineSafe( QTextCodec::codecForName( "UTF-8" ) ) );
    REQUIRE( FolderSearchResults::codecIsByteNewlineSafe(
        QTextCodec::codecForName( "ISO 8859-1" ) ) );
    REQUIRE( FolderSearchResults::codecIsByteNewlineSafe(
        QTextCodec::codecForName( "windows-1251" ) ) );

    // Stateful / multi-byte / unlisted codecs must NOT be treated as
    // byte-newline-safe.
    REQUIRE_FALSE(
        FolderSearchResults::codecIsByteNewlineSafe( QTextCodec::codecForName( "UTF-16LE" ) ) );
    REQUIRE_FALSE(
        FolderSearchResults::codecIsByteNewlineSafe( QTextCodec::codecForName( "UTF-16" ) ) );
    REQUIRE_FALSE(
        FolderSearchResults::codecIsByteNewlineSafe( QTextCodec::codecForName( "UTF-32" ) ) );
    if ( auto* sjis = QTextCodec::codecForName( "Shift-JIS" ) ) {
        REQUIRE_FALSE( FolderSearchResults::codecIsByteNewlineSafe( sjis ) );
    }
    if ( auto* gbk = QTextCodec::codecForName( "GBK" ) ) {
        REQUIRE_FALSE( FolderSearchResults::codecIsByteNewlineSafe( gbk ) );
    }
}

TEST_CASE( "FolderSearchResults getters are race-free against streaming and concurrent readers",
           "[folder]" )
{
    // QuickFind runs on a QtConcurrent worker reading the results model while
    // the main thread streams groups in, and the view's paints hit the same
    // getters (quickfind.cpp:211+). Before the model was internally
    // synchronized, concurrent reads tore the row/group containers and shared
    // per-group QFile cursors (garbage text, OOB access, crash).
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );

    constexpr int fileCount = 8;
    constexpr int groupsPerFile = 8; // 64 groups -> 3264 visible rows
    constexpr int matchesPerGroup = 50;

    QStringList paths;
    std::vector<std::vector<int64_t>> lineOffsets;
    for ( int f = 0; f < fileCount; ++f ) {
        QStringList lines;
        std::vector<int64_t> offsets;
        int64_t running = 0;
        for ( int l = 0; l < 200; ++l ) {
            const auto line = QString( "file%1 line %2" ).arg( f ).arg( l );
            offsets.push_back( running );
            running += line.size() + 1;
            lines << line;
        }
        paths << writeFile( dir, QString( "f%1.log" ).arg( f ), lines );
        lineOffsets.push_back( std::move( offsets ) );
    }

    FolderSearchResults results;
    QStringList order;
    for ( int g = 0; g < fileCount * groupsPerFile; ++g ) {
        order << paths[ g % static_cast<int>( paths.size() ) ];
    }
    results.beginSearch( order );

    std::atomic<bool> stop{ false };
    std::atomic<int> failures{ 0 };

    auto reader = [ & ] {
        while ( !stop.load( std::memory_order_relaxed ) ) {
            try {
                const auto nb = results.getNbLine().get();
                for ( LinesCount::UnderlyingType i = 0; i < nb; ++i ) {
                    const auto row = LineNumber( i );
                    if ( results.lineKind( row ) != LineKind::Data ) {
                        continue;
                    }
                    const auto src = results.sourceForLine( row );
                    const auto text = results.getExpandedLineString( row );
                    if ( src.filePath.isEmpty() || text.isEmpty() ) {
                        continue;
                    }
                    // The row's text must come from ITS file; a torn read on a
                    // shared QFile handle returns another file's content.
                    const auto expectedPrefix = QLatin1String( "file" )
                                                + src.filePath.at( src.filePath.size() - 5 );
                    if ( !text.startsWith( expectedPrefix ) ) {
                        ++failures;
                    }
                }
            }
            catch ( ... ) {
                ++failures;
            }
        }
    };

    std::thread readerA( reader );
    std::thread readerB( reader );

    for ( int g = 0; g < fileCount * groupsPerFile; ++g ) {
        const int f = g % fileCount;
        klogg::folder::FileGroup group;
        group.filePath = paths[ f ];
        for ( int m = 0; m < matchesPerGroup; ++m ) {
            const auto start = lineOffsets[ static_cast<size_t>( f ) ][ static_cast<size_t>( m ) ];
            const auto lineText = QString( "file%1 line %2" ).arg( f ).arg( m );
            const auto textLen = static_cast<int64_t>( lineText.size() );
            group.matches.push_back( match( static_cast<uint64_t>( m ), start, start + textLen,
                                            static_cast<int>( textLen ), 4 ) );
        }
        results.addFileGroup( g, std::move( group ) );
    }

    stop.store( true );
    readerA.join();
    readerB.join();

    REQUIRE( failures.load() == 0 );
    REQUIRE( results.getNbLine().get()
             == static_cast<uint64_t>( fileCount * groupsPerFile * ( 1 + matchesPerGroup ) ) );
}
