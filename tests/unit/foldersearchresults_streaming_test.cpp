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

// Unit tests for the FolderSearchResults streaming API (beginSearch /
// addFileGroup / flushPending). These exercise the in-order commit cursor
// directly, without the engine or any worker threads.

#include <catch2/catch.hpp>

#include "foldersearchresults.h"
#include "foldersearchtypes.h"

#include <utility>

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

klogg::folder::FileGroup groupWithPath( const QString& path )
{
    klogg::folder::FileGroup g;
    g.filePath = path;
    g.matches.push_back( match( 0, 0, 5, 5, 1 ) );
    return g;
}
} // namespace

TEST_CASE( "FolderSearchResults addFileGroup commits in enumeration order regardless of arrival order",
           "[folder]" )
{
    FolderSearchResults r;
    r.beginSearch( QStringList{ "/a.log", "/b.log", "/c.log" } );
    REQUIRE( r.groupCount() == 0 );
    REQUIRE( r.getNbLine() == 0_lcount );

    // C arrives first -> must wait on A and B (no rows committed yet).
    r.addFileGroup( 2, groupWithPath( "/c.log" ) );
    REQUIRE( r.groupCount() == 0 );
    REQUIRE( r.getNbLine() == 0_lcount );

    // A arrives -> A commits (header + match).
    r.addFileGroup( 0, groupWithPath( "/a.log" ) );
    REQUIRE( r.groupCount() == 1 );
    REQUIRE( r.getNbLine() == 2_lcount );
    REQUIRE( r.lineKind( 0_lnum ) == LineKind::Header );
    REQUIRE( r.sourceForLine( 0_lnum ).filePath == "/a.log" );

    // B arrives -> B and the previously-buffered C both commit in order.
    r.addFileGroup( 1, groupWithPath( "/b.log" ) );
    REQUIRE( r.groupCount() == 3 );
    // Layout: headerA(0) matchA(1) headerB(2) matchB(3) headerC(4) matchC(5)
    REQUIRE( r.getNbLine() == 6_lcount );
    REQUIRE( r.sourceForLine( 0_lnum ).filePath == "/a.log" );
    REQUIRE( r.lineKind( 2_lnum ) == LineKind::Header );
    REQUIRE( r.sourceForLine( 2_lnum ).filePath == "/b.log" );
    REQUIRE( r.lineKind( 4_lnum ) == LineKind::Header );
    REQUIRE( r.sourceForLine( 4_lnum ).filePath == "/c.log" );
}

TEST_CASE( "FolderSearchResults addFileGroup skips empty groups but advances the cursor", "[folder]" )
{
    FolderSearchResults r;
    r.beginSearch( QStringList{ "/a.log", "/b.log" } );

    // Empty group at index 0: no header created, but the cursor advances so the
    // next group can commit (grep-style "no header for zero-match files").
    klogg::folder::FileGroup empty;
    empty.filePath = "/a.log"; // zero matches
    r.addFileGroup( 0, std::move( empty ) );
    REQUIRE( r.groupCount() == 0 );
    REQUIRE( r.getNbLine() == 0_lcount );

    r.addFileGroup( 1, groupWithPath( "/b.log" ) );
    REQUIRE( r.groupCount() == 1 );
    REQUIRE( r.getNbLine() == 2_lcount ); // header + match
    // The single committed group is B at fileId 0.
    REQUIRE( r.sourceForLine( 0_lnum ).filePath == "/b.log" );
    REQUIRE( r.lineKind( 1_lnum ) == LineKind::Data );
}

TEST_CASE( "FolderSearchResults flushPending commits present groups and skips gaps", "[folder]" )
{
    FolderSearchResults r;
    r.beginSearch( QStringList{ "/a.log", "/b.log", "/c.log" } );

    r.addFileGroup( 0, groupWithPath( "/a.log" ) ); // A commits
    REQUIRE( r.groupCount() == 1 );

    r.addFileGroup( 2, groupWithPath( "/c.log" ) ); // C buffered, waits on B
    REQUIRE( r.groupCount() == 1 );

    // B never arrived (interrupted scan). flushPending skips the gap and commits
    // C so the display does not stall.
    r.flushPending();
    REQUIRE( r.groupCount() == 2 );
    // Layout: headerA(0) matchA(1) headerC(2) matchC(3)
    REQUIRE( r.getNbLine() == 4_lcount );
    REQUIRE( r.sourceForLine( 0_lnum ).filePath == "/a.log" );
    REQUIRE( r.lineKind( 2_lnum ) == LineKind::Header );
    REQUIRE( r.sourceForLine( 2_lnum ).filePath == "/c.log" );
}
