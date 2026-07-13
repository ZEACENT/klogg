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
#include <QTemporaryDir>
#include <QTextCodec>

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
