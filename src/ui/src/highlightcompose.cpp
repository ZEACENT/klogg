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

#include "highlightcompose.h"

#include <cmath>
#include <algorithm>
#include <optional>

QColor blendSearchMatchOverHighlighter( const QColor& highlighterBack,
                                        const QColor& searchMatchBack )
{
    if ( !highlighterBack.isValid() ) {
        return searchMatchBack;
    }
    if ( !searchMatchBack.isValid() ) {
        return highlighterBack;
    }
    const auto blend = [ ]( int gray, int hl ) {
        return static_cast<int>( std::lround( gray * kSearchMatchBlendAlpha
                                              + hl * ( 1.0 - kSearchMatchBlendAlpha ) ) );
    };
    return QColor{ blend( searchMatchBack.red(), highlighterBack.red() ),
                   blend( searchMatchBack.green(), highlighterBack.green() ),
                   blend( searchMatchBack.blue(), highlighterBack.blue() ) };
}

HighlightedMatchRanges composeLineHighlights( const klogg::vector<HighlightSource>& sources )
{
    // Flatten the per-source matches into spans tagged with layer + colors.
    struct Span {
        LineColumn start;
        LineColumn end; // inclusive
        HighlightLayer layer;
        QColor fore;
        QColor back;
    };
    klogg::vector<Span> spans;
    for ( const auto& src : sources ) {
        for ( const auto& m : src.matches ) {
            if ( m.size() <= 0_length ) {
                continue;
            }
            spans.push_back( Span{ m.startColumn(), m.endColumn(), src.layer, m.foreColor(),
                                   m.backColor() } );
        }
    }
    if ( spans.empty() ) {
        return HighlightedMatchRanges{};
    }

    // Sweep over all match boundaries to form non-overlapping segments, then
    // resolve each segment's (fore, back) by priority + the blend rule. This is
    // the single place that owns the z-order contract.
    klogg::vector<LineColumn::UnderlyingType> bounds;
    bounds.reserve( spans.size() * 2 );
    for ( const auto& s : spans ) {
        bounds.push_back( s.start.get() );
        bounds.push_back( s.end.get() + 1 );
    }
    std::sort( bounds.begin(), bounds.end() );
    bounds.erase( std::unique( bounds.begin(), bounds.end() ), bounds.end() );

    const auto layerPriority = []( HighlightLayer layer ) {
        return static_cast<int>( layer );
    };

    klogg::vector<HighlightedMatch> result;
    for ( size_t i = 0; i + 1 < bounds.size(); ++i ) {
        const LineColumn segStart{ bounds[ i ] };
        const LineColumn segLast{ bounds[ i + 1 ] - 1 }; // inclusive end

        const Span* top = nullptr;
        std::optional<QColor> searchMatchBack;
        for ( const auto& s : spans ) {
            if ( s.start <= segStart && s.end >= segLast ) {
                if ( s.layer == HighlightLayer::SearchMatch ) {
                    searchMatchBack = s.back;
                }
                if ( top == nullptr
                     || layerPriority( s.layer ) >= layerPriority( top->layer ) ) {
                    top = &s;
                }
            }
        }
        if ( top == nullptr ) {
            continue;
        }

        QColor back = top->back;
        // Blend rule: where the search-match gray sits UNDER a configured
        // Highlighter or QuickHighlighter, keep both visible instead of letting
        // one mask the other. Selection/QuickFind (higher layers) still fully
        // cover a search match; ANSI (lower) is covered by the search match.
        if ( searchMatchBack.has_value()
             && ( top->layer == HighlightLayer::Highlighter
                  || top->layer == HighlightLayer::QuickHighlighter ) ) {
            back = blendSearchMatchOverHighlighter( top->back, *searchMatchBack );
        }
        const QColor fore = top->fore;

        // Merge with the previous segment if it is adjacent and identical in
        // fore/back, so the output stays minimal.
        if ( !result.empty() ) {
            auto& prev = result.back();
            if ( prev.endColumn() + 1_length == segStart && prev.foreColor() == fore
                 && prev.backColor() == back ) {
                prev = HighlightedMatch{ prev.startColumn(),
                                         segLast - prev.startColumn() + 1_length,
                                         prev.foreColor(), prev.backColor() };
                continue;
            }
        }
        result.push_back( HighlightedMatch{ segStart, segLast - segStart + 1_length, fore, back } );
    }

    return HighlightedMatchRanges{ std::move( result ) };
}
