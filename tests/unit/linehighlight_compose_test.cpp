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
 * along with klogg. If not, see <http://www.gnu.org/licenses/>.
 */

#include <catch2/catch.hpp>

#include "highlightcompose.h"
#include "highlightedmatch.h"
#include "linetypes.h"

#include <QColor>

#include <optional>

namespace {
HighlightedMatch makeMatch( int start, int size, QColor fore, QColor back )
{
    return HighlightedMatch{ LineColumn{ start }, LineLength{ size }, fore, back };
}

// ranges.matches() returns by value, so return the covering match by value
// (not a pointer into a temporary).
std::optional<HighlightedMatch> matchCovering( const HighlightedMatchRanges& ranges, int column )
{
    const auto matches = ranges.matches();
    const auto col = LineColumn{ column };
    for ( const auto& m : matches ) {
        if ( m.startColumn() <= col && col <= m.endColumn() ) {
            return m;
        }
    }
    return std::nullopt;
}
} // namespace

TEST_CASE( "composeLineHighlights blends search-match gray under a configured Highlighter",
           "[highlighter][coexistence]" )
{
    const QColor red{ 0xff, 0x00, 0x00 };
    const QColor gray{ Qt::lightGray };
    const QColor black{ Qt::black };

    // "x ERROR y": ERROR occupies columns 2..6 (size 5). Both the Highlighter
    // and the search match target the same span.
    klogg::vector<HighlightSource> sources;
    sources.push_back( HighlightSource{ HighlightLayer::Highlighter,
                                        { makeMatch( 2, 5, black, red ) } } );
    sources.push_back( HighlightSource{ HighlightLayer::SearchMatch,
                                        { makeMatch( 2, 5, black, gray ) } } );

    const auto ranges = composeLineHighlights( sources );
    REQUIRE_FALSE( ranges.empty() );

    const auto m = matchCovering( ranges, 4 );
    REQUIRE( m.has_value() );
    // Highlighter wins (visible) but the gray is NOT completely covered: the
    // result is the blend, neither pure red nor pure gray.
    REQUIRE( m->backColor() != red );
    REQUIRE( m->backColor() != gray );
    REQUIRE( m->backColor() == blendSearchMatchOverHighlighter( red, gray ) );
}

TEST_CASE( "composeLineHighlights blends search-match gray under a QuickHighlighter (color label)",
           "[highlighter][coexistence]" )
{
    const QColor green{ 0x00, 0xff, 0x00 };
    const QColor gray{ Qt::lightGray };
    const QColor white{ Qt::white };

    klogg::vector<HighlightSource> sources;
    sources.push_back( HighlightSource{ HighlightLayer::QuickHighlighter,
                                        { makeMatch( 0, 4, white, green ) } } );
    sources.push_back( HighlightSource{ HighlightLayer::SearchMatch,
                                        { makeMatch( 0, 4, white, gray ) } } );

    const auto ranges = composeLineHighlights( sources );
    REQUIRE_FALSE( ranges.empty() );
    const auto m = matchCovering( ranges, 1 );
    REQUIRE( m.has_value() );
    REQUIRE( m->backColor() == blendSearchMatchOverHighlighter( green, gray ) );
    REQUIRE( m->backColor() != green );
    REQUIRE( m->backColor() != gray );
}

TEST_CASE( "composeLineHighlights blends only the overlap; pure highlighter elsewhere",
           "[highlighter][coexistence]" )
{
    // A whole-line (LineMatch-style) Highlighter spanning 0..9; search match only 2..6.
    const QColor red{ 0xff, 0x00, 0x00 };
    const QColor gray{ Qt::lightGray };
    const QColor black{ Qt::black };

    klogg::vector<HighlightSource> sources;
    sources.push_back( HighlightSource{ HighlightLayer::Highlighter,
                                        { makeMatch( 0, 10, black, red ) } } );
    sources.push_back( HighlightSource{ HighlightLayer::SearchMatch,
                                        { makeMatch( 2, 5, black, gray ) } } );

    const auto ranges = composeLineHighlights( sources );
    REQUIRE_FALSE( ranges.empty() );

    // Overlap (col 4): blend.
    const auto overlap = matchCovering( ranges, 4 );
    REQUIRE( overlap.has_value() );
    REQUIRE( overlap->backColor() == blendSearchMatchOverHighlighter( red, gray ) );

    // Non-overlap (col 8): pure highlighter, no gray punched in.
    const auto pure = matchCovering( ranges, 8 );
    REQUIRE( pure.has_value() );
    REQUIRE( pure->backColor() == red );
}

TEST_CASE( "composeLineHighlights: search match alone stays gray, highlighter alone stays pure",
           "[highlighter][coexistence]" )
{
    const QColor red{ 0xff, 0x00, 0x00 };
    const QColor gray{ Qt::lightGray };
    const QColor black{ Qt::black };

    SECTION( "search match alone" ) {
        klogg::vector<HighlightSource> sources;
        sources.push_back( HighlightSource{ HighlightLayer::SearchMatch,
                                            { makeMatch( 0, 3, black, gray ) } } );
        const auto ranges = composeLineHighlights( sources );
        REQUIRE_FALSE( ranges.empty() );
        REQUIRE( matchCovering( ranges, 1 )->backColor() == gray );
    }
    SECTION( "highlighter alone" ) {
        klogg::vector<HighlightSource> sources;
        sources.push_back( HighlightSource{ HighlightLayer::Highlighter,
                                            { makeMatch( 0, 3, black, red ) } } );
        const auto ranges = composeLineHighlights( sources );
        REQUIRE_FALSE( ranges.empty() );
        REQUIRE( matchCovering( ranges, 1 )->backColor() == red );
    }
}

TEST_CASE( "composeLineHighlights: Selection fully covers a search match (no blend)",
           "[highlighter][coexistence]" )
{
    const QColor gray{ Qt::lightGray };
    const QColor selBack{ 0x00, 0x00, 0xff };
    const QColor selFore{ Qt::white };
    const QColor black{ Qt::black };

    klogg::vector<HighlightSource> sources;
    sources.push_back( HighlightSource{ HighlightLayer::SearchMatch,
                                        { makeMatch( 0, 4, black, gray ) } } );
    sources.push_back( HighlightSource{ HighlightLayer::Selection,
                                        { makeMatch( 0, 4, selFore, selBack ) } } );

    const auto ranges = composeLineHighlights( sources );
    REQUIRE_FALSE( ranges.empty() );
    const auto m = matchCovering( ranges, 2 );
    REQUIRE( m.has_value() );
    // Selection is above SearchMatch and is NOT a blend target: it wins at full
    // opacity, so the search match is covered (current behavior preserved).
    REQUIRE( m->backColor() == selBack );
    REQUIRE( m->backColor() != blendSearchMatchOverHighlighter( selBack, gray ) );
}
