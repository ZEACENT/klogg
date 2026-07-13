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

// Unit tests for the Overview's folder-mode explicit match-line path
// (setMatchLines + the else-if branch in recalculatesLines). This path is a
// pure else-branch guarded by logFilteredData_==nullptr, so it never touches the
// single-file LogFilteredData path.

#include <catch2/catch.hpp>

#include "linetypes.h"
#include "overview.h"

#include <vector>

TEST_CASE( "Overview folder-mode setMatchLines maps lines to y positions",
           "[overview][folder]" )
{
    Overview o;
    // 100000-line file, 200px tall overview.
    o.updateData( LinesCount( 100000 ) );

    // Lines 0 and 100 both map to y = 0 (collapse + weight bump);
    // 25000 -> y = 50; 50000 -> y = 100.
    o.setMatchLines( { 0_lnum, 100_lnum, 25000_lnum, 50000_lnum } );
    o.updateView( 200 );

    const auto* matches = o.getMatchLines();
    REQUIRE( matches != nullptr );
    REQUIRE( matches->size() == 3 ); // 0 and 100 collapse onto y=0
    REQUIRE( matches->at( 0 ).position() == 0 );
    REQUIRE( matches->at( 0 ).weight() == 1 ); // load() called once for line 100
    REQUIRE( matches->at( 1 ).position() == 50 );
    REQUIRE( matches->at( 1 ).weight() == 0 );
    REQUIRE( matches->at( 2 ).position() == 100 );
    REQUIRE( matches->at( 2 ).weight() == 0 );

    // Folder mode represents matches only: marks stay empty.
    const auto* marks = o.getMarkLines();
    REQUIRE( marks->empty() );
}

TEST_CASE( "Overview folder-mode y<->line mapping is inverse at exact points",
           "[overview][folder]" )
{
    Overview o;
    o.updateData( LinesCount( 100000 ) );
    o.setMatchLines( { 25000_lnum } );
    o.updateView( 200 );

    // yFromFileLine(25000) == 50 exactly, and fileLineFromY(50) == 25000.
    REQUIRE( o.yFromFileLine( 25000_lnum ) == 50 );
    REQUIRE( o.fileLineFromY( 50 ) == 25000_lnum );
}

TEST_CASE( "Overview folder-mode is reproducible across repeated setMatchLines",
           "[overview][folder]" )
{
    Overview o;
    o.updateData( LinesCount( 100000 ) );
    o.setMatchLines( { 100_lnum, 25000_lnum } );
    o.updateView( 200 );
    const auto sizeBefore = o.getMatchLines()->size();

    // A second setMatchLines on the same Overview must produce the same result
    // (no stale entries carried over from the previous call).
    o.setMatchLines( { 100_lnum, 25000_lnum } );
    o.updateView( 200 );
    REQUIRE( o.getMatchLines()->size() == sizeBefore );
    REQUIRE( o.getMarkLines()->empty() );
}

TEST_CASE( "Overview setFilteredData drops a folder-mode match list",
           "[overview][folder]" )
{
    Overview o;
    o.updateData( LinesCount( 100000 ) );
    o.setMatchLines( { 100_lnum, 25000_lnum } );
    o.updateView( 200 );
    REQUIRE_FALSE( o.getMatchLines()->empty() );

    // Re-associating a (null) LogFilteredData must clear the explicit list, so a
    // later single-file attach cannot inherit folder marks.
    o.setFilteredData( nullptr );
    o.updateView( 200 );
    REQUIRE( o.getMatchLines()->empty() );
    REQUIRE( o.getMarkLines()->empty() );

    // And the folder branch can be re-entered afterwards.
    o.setMatchLines( { 50000_lnum } );
    o.updateView( 200 );
    REQUIRE( o.getMatchLines()->size() == 1 );
}

TEST_CASE( "Overview folder-mode with empty file or empty list draws nothing",
           "[overview][folder]" )
{
    Overview o;
    // No matches and a zero-line file: nothing is drawn.
    o.updateData( LinesCount( 0 ) );
    o.setMatchLines( { 10_lnum } );
    o.updateView( 200 );
    REQUIRE( o.getMatchLines()->empty() );

    // Non-empty file but empty match list: still nothing.
    o.updateData( LinesCount( 1000 ) );
    o.setMatchLines( {} );
    o.updateView( 200 );
    REQUIRE( o.getMatchLines()->empty() );
}
