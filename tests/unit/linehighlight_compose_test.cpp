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

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <random>

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
// Deliberately slow, column-by-column characterization oracle. It does not use
// compositor boundaries or its blend helper, so sweep/rounding regressions cannot
// hide behind shared implementation. Fixtures keep the coordinate range small.
HighlightedMatchRanges referenceComposition( const klogg::vector<HighlightSource>& sources )
{
    LineColumn::UnderlyingType first = 0;
    LineColumn::UnderlyingType last = -1;
    for ( const auto& source : sources ) {
        for ( const auto& match : source.matches ) {
            if ( match.size() > 0_length ) {
                first = std::min( first, match.startColumn().get() );
                last = std::max( last, match.endColumn().get() );
            }
        }
    }

    klogg::vector<HighlightedMatch> result;
    for ( auto column = first; column <= last; ++column ) {
        const HighlightedMatch* winner = nullptr;
        auto winningLayer = HighlightLayer::Ansi;
        std::optional<QColor> searchBack;
        for ( const auto& source : sources ) {
            for ( const auto& match : source.matches ) {
                if ( match.size() <= 0_length || match.startColumn().get() > column
                     || match.endColumn().get() < column ) {
                    continue;
                }
                if ( source.layer == HighlightLayer::SearchMatch ) {
                    searchBack = match.backColor();
                }
                if ( winner == nullptr || source.layer >= winningLayer ) {
                    winner = &match;
                    winningLayer = source.layer;
                }
            }
        }
        if ( winner == nullptr ) {
            continue;
        }
        auto back = winner->backColor();
        if ( searchBack.has_value()
             && ( winningLayer == HighlightLayer::Highlighter
                  || winningLayer == HighlightLayer::QuickHighlighter ) ) {
            if ( !back.isValid() ) {
                back = *searchBack;
            }
            else if ( searchBack->isValid() ) {
                const auto blend = []( int highlighter, int search ) {
                    return static_cast<int>( std::lround( 0.65 * highlighter + 0.35 * search ) );
                };
                back = QColor{ blend( back.red(), searchBack->red() ),
                               blend( back.green(), searchBack->green() ),
                               blend( back.blue(), searchBack->blue() ) };
            }
        }
        if ( !result.empty() && result.back().endColumn().get() + 1 == column
             && result.back().foreColor() == winner->foreColor()
             && result.back().backColor() == back ) {
            const auto& previous = result.back();
            result.back() = HighlightedMatch{ previous.startColumn(), previous.size() + 1_length,
                                              previous.foreColor(), back };
        }
        else {
            result.push_back( HighlightedMatch{ LineColumn{ column }, 1_length,
                                                winner->foreColor(), back } );
        }
    }
    return HighlightedMatchRanges{ std::move( result ) };
}

void requireSameComposition( const HighlightedMatchRanges& actual,
                             const HighlightedMatchRanges& expected )
{
    const auto actualMatches = actual.matches();
    const auto expectedMatches = expected.matches();
    REQUIRE( actualMatches.size() == expectedMatches.size() );
    for ( size_t index = 0; index < actualMatches.size(); ++index ) {
        CAPTURE( index );
        REQUIRE( actualMatches[ index ].startColumn() == expectedMatches[ index ].startColumn() );
        REQUIRE( actualMatches[ index ].size() == expectedMatches[ index ].size() );
        REQUIRE( actualMatches[ index ].foreColor() == expectedMatches[ index ].foreColor() );
        REQUIRE( actualMatches[ index ].backColor() == expectedMatches[ index ].backColor() );
    }
}
} // namespace

TEST_CASE( "Compositor clips before constructing sweep events", "[highlight-viewport]" )
{
    HighlightSource source{ HighlightLayer::Selection, {} };
    for ( int column = 0; column < 4000; column += 2 ) {
        source.matches.push_back( makeMatch( column, 1, Qt::white, Qt::blue ) );
    }
    HighlightCompositionWork work;
    const auto composed = composeLineHighlights( { source }, &work,
                                                 HighlightViewport{ 40_lcol, 60_lcol } );
    CHECK( work.eventVisits == 22 );
    CHECK( composed.matches().size() == 11 );
    REQUIRE_FALSE( matchCovering( composed, 38 ) );
    REQUIRE( matchCovering( composed, 40 ) );
    REQUIRE( matchCovering( composed, 60 ) );
    REQUIRE_FALSE( matchCovering( composed, 62 ) );
}

TEST_CASE( "Viewport clipping preserves boundary winners and blends", "[highlight-viewport]" )
{
    const klogg::vector<HighlightSource> sources{
        { HighlightLayer::Ansi, { makeMatch( 0, 100, Qt::white, Qt::red ) } },
        { HighlightLayer::Highlighter, { makeMatch( 10, 70, Qt::black, Qt::yellow ) } },
        { HighlightLayer::SearchMatch, { makeMatch( 40, 20, Qt::black, Qt::gray ) } },
        { HighlightLayer::Selection, { makeMatch( 50, 2, Qt::white, Qt::blue ) } }
    };
    auto expected = referenceComposition( sources );
    expected.clamp( 35_lcol, 55_lcol );
    const auto actual = composeLineHighlights( sources, nullptr,
                                               HighlightViewport{ 35_lcol, 55_lcol } );
    requireSameComposition( actual, expected );
    REQUIRE( composeLineHighlights( sources, nullptr,
                HighlightViewport{ 5_lcol, 4_lcol } ).empty() );
}

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

TEST_CASE( "composeLineHighlights preserves layer and source order at shared boundaries",
           "[highlighter][compose][characterization]" )
{
    const QColor black{ Qt::black };
    const QColor white{ Qt::white };
    const QColor red{ Qt::red };
    const QColor blue{ Qt::blue };
    const QColor green{ Qt::green };
    const std::array<HighlightLayer, 6> layers{
        HighlightLayer::Ansi, HighlightLayer::SearchMatch, HighlightLayer::Highlighter,
        HighlightLayer::QuickHighlighter, HighlightLayer::QuickFind, HighlightLayer::Selection };

    for ( const auto first : layers ) {
        for ( const auto second : layers ) {
            CAPTURE( static_cast<int>( first ), static_cast<int>( second ) );
            const klogg::vector<HighlightSource> sources{
                { first, { makeMatch( 0, 10, white, red ), makeMatch( 2, 3, black, blue ) } },
                { second, { makeMatch( 4, 4, white, green ), makeMatch( 8, 2, black, blue ) } } };
            requireSameComposition( composeLineHighlights( sources ), referenceComposition( sources ) );
        }
    }
}

TEST_CASE( "composeLineHighlights restores earlier ties and the last active search blend",
           "[highlighter][compose][characterization]" )
{
    const QColor black{ Qt::black };
    const QColor white{ Qt::white };
    const QColor red{ Qt::red };
    const QColor green{ Qt::green };
    const QColor gray{ Qt::lightGray };
    const QColor darkGray{ Qt::darkGray };
    const klogg::vector<HighlightSource> sources{
        { HighlightLayer::Highlighter, { makeMatch( 0, 14, white, red ) } },
        { HighlightLayer::SearchMatch,
          { makeMatch( 0, 14, black, gray ), makeMatch( 2, 10, black, darkGray ) } },
        { HighlightLayer::Highlighter,
          { makeMatch( 3, 8, black, green ), makeMatch( 5, 2, white, red ) } },
        { HighlightLayer::SearchMatch, { makeMatch( 4, 4, black, QColor{} ) } },
        { HighlightLayer::QuickFind, { makeMatch( 6, 3, QColor{}, QColor{} ) } } };
    requireSameComposition( composeLineHighlights( sources ), referenceComposition( sources ) );
}

TEST_CASE( "composeLineHighlights ignores empty spans and merges only adjacent identical styles",
           "[highlighter][compose][characterization]" )
{
    const QColor black{ Qt::black };
    const QColor red{ Qt::red };
    const klogg::vector<HighlightSource> sources{
        { HighlightLayer::Selection,
          { makeMatch( 2, 0, black, red ), makeMatch( 4, -1, black, red ) } },
        { HighlightLayer::Ansi, { makeMatch( 0, 2, black, red ), makeMatch( 5, 1, black, red ) } },
        { HighlightLayer::QuickFind, { makeMatch( 2, 2, black, red ) } } };
    const HighlightedMatchRanges expected{ { makeMatch( 0, 4, black, red ),
                                             makeMatch( 5, 1, black, red ) } };
    requireSameComposition( composeLineHighlights( sources ), expected );
    REQUIRE( composeLineHighlights( {} ).empty() );
    REQUIRE( composeLineHighlights( { sources.front() } ).empty() );
}

TEST_CASE( "composeLineHighlights matches a deterministic column rasterizer",
           "[highlighter][compose][characterization][differential]" )
{
    std::mt19937 random{ 0x43c06f21U };
    const std::array<QColor, 7> colors{ QColor{}, QColor{ Qt::black }, QColor{ Qt::white },
                                      QColor{ Qt::lightGray }, QColor{ 43, 107, 191 },
                                      QColor{ 211, 60, 20, 96 }, QColor{ 0, 201, 77, 0 } };
    for ( int fixture = 0; fixture < 160; ++fixture ) {
        CAPTURE( fixture );
        klogg::vector<HighlightSource> sources;
        const auto sourceCount = random() % 13U;
        for ( std::mt19937::result_type sourceIndex = 0; sourceIndex < sourceCount; ++sourceIndex ) {
            HighlightSource source{ static_cast<HighlightLayer>( random() % 6U ), {} };
            const auto matchCount = random() % 10U;
            for ( std::mt19937::result_type index = 0; index < matchCount; ++index ) {
                const auto start = static_cast<int>( random() % 28U );
                const auto length = static_cast<int>( random() % 12U ) - 1;
                const auto fore = colors[ random() % colors.size() ];
                const auto back = colors[ random() % colors.size() ];
                source.matches.push_back( makeMatch( start, length, fore, back ) );
            }
            sources.push_back( std::move( source ) );
        }
        requireSameComposition( composeLineHighlights( sources ), referenceComposition( sources ) );
    }
}

TEST_CASE( "composeLineHighlights dense span inspections scale linearly with boundaries",
           "[highlighter][compose][work]" )
{
    const QColor black{ Qt::black };
    const QColor red{ Qt::red };
    const QColor blue{ Qt::blue };
    for ( const bool nested : { false, true } ) {
        CAPTURE( nested );
        std::size_t previousVisits = 0;
        for ( const int count : { 128, 256, 512, 1024 } ) {
            CAPTURE( count );
            klogg::vector<HighlightSource> sources{ { HighlightLayer::Selection, {} } };
            for ( int index = 0; index < count; ++index ) {
                // Disjoint short matches and a dense nest exercise both small and
                // large active sets. Alternating styles prevent output merging
                // from hiding the number of necessary segment resolutions.
                const int start = nested ? index : 2 * index;
                const int length = nested ? 2 * ( count - index ) : 1;
                sources.front().matches.push_back(
                    makeMatch( start, length, black, index % 2 == 0 ? red : blue ) );
            }
            HighlightCompositionWork work;
            const auto actual = composeLineHighlights( sources, &work );
            const auto spanCount = static_cast<std::size_t>( count );
            REQUIRE( actual.matches().size() == ( nested ? 2 * spanCount - 1 : spanCount ) );
            REQUIRE( work.segmentResolutions == 2 * spanCount - 1 );
            REQUIRE( work.spanVisits <= 5 * spanCount );
            REQUIRE( work.eventVisits == 2 * spanCount );
            if ( previousVisits != 0 ) {
                REQUIRE( work.spanVisits <= 2 * previousVisits + 2 );
            }
            previousVisits = work.spanVisits;
        }
    }
}

TEST_CASE( "composeLineHighlights dense blending inspects only the active winners",
           "[highlighter][compose][work]" )
{
    const QColor black{ Qt::black };
    const QColor red{ Qt::red };
    const QColor gray{ Qt::lightGray };
    const QColor darkGray{ Qt::darkGray };
    constexpr int count = 512;
    klogg::vector<HighlightSource> sources{
        { HighlightLayer::Highlighter, { makeMatch( 0, 2 * count, black, red ) } },
        { HighlightLayer::SearchMatch, {} } };
    for ( int index = 0; index < count; ++index ) {
        sources.back().matches.push_back(
            makeMatch( index, 2 * ( count - index ), black, index % 2 == 0 ? gray : darkGray ) );
    }
    HighlightCompositionWork work;
    const auto actual = composeLineHighlights( sources, &work );
    REQUIRE( actual.matches().size() == 2 * count - 1 );
    requireSameComposition( actual, referenceComposition( sources ) );
    REQUIRE( work.segmentResolutions == 2 * count - 1 );
    REQUIRE( work.spanVisits <= 5 * ( count + 1 ) );
    REQUIRE( work.eventVisits == 2 * ( count + 1 ) );
}

TEST_CASE( "composeLineHighlights work observations reset for each call",
           "[highlighter][compose][work]" )
{
    HighlightCompositionWork work{ 99, 99, 99 };
    REQUIRE( composeLineHighlights( {}, &work ).empty() );
    REQUIRE( work.spanVisits == 0 );
    REQUIRE( work.eventVisits == 0 );
    REQUIRE( work.segmentResolutions == 0 );
}
