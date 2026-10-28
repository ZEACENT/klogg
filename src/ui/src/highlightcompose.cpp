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

#include <algorithm>
#include <cmath>
#include <set>
#include <utility>

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

HighlightedMatchRanges composeLineHighlights( const klogg::vector<HighlightSource>& sources,
                                              HighlightCompositionWork* work,
                                              std::optional<HighlightViewport> viewport )
{
    if ( work != nullptr ) {
        *work = {};
    }
    if ( viewport && viewport->last < viewport->first ) {
        return {};
    }
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
            if ( work != nullptr ) {
                ++work->spanVisits;
            }
            if ( m.size() <= 0_length ) {
                continue;
            }
            const auto first = viewport ? std::max( m.startColumn(), viewport->first ) : m.startColumn();
            const auto last = viewport ? std::min( m.endColumn(), viewport->last ) : m.endColumn();
            if ( last >= first ) {
                spans.push_back( Span{ first, last, src.layer, m.foreColor(), m.backColor() } );
            }
        }
    }
    if ( spans.empty() ) {
        return HighlightedMatchRanges{};
    }

    // Events use half-open spans. Apply every event at one column before
    // resolving the interval to the next column, so simultaneous ends/starts
    // have no transient effect on colors or output merging.
    struct Event {
        LineColumn::UnderlyingType column;
        size_t spanIndex;
        bool entering;
    };
    klogg::vector<Event> events;
    events.reserve( spans.size() * 2 );
    for ( size_t index = 0; index < spans.size(); ++index ) {
        events.push_back( Event{ spans[ index ].start.get(), index, true } );
        events.push_back( Event{ spans[ index ].end.get() + 1, index, false } );
    }
    std::sort( events.begin(), events.end(), []( const Event& lhs, const Event& rhs ) {
        return lhs.column < rhs.column;
    } );

    // Input ordinals preserve both source and match order: the last active
    // span in the highest layer wins, exactly as in the previous >= scan.
    // SearchMatch has its own ordered set because blending needs its last
    // active background even when a higher layer wins. Invalid colors must
    // remain entries too: their presence is different from no search match.
    using ActiveKey = std::pair<HighlightLayer, size_t>;
    std::set<ActiveKey> active;
    std::set<size_t> activeSearchMatches;
    klogg::vector<HighlightedMatch> result;
    size_t eventIndex = 0;
    while ( eventIndex < events.size() ) {
        const auto column = events[ eventIndex ].column;
        while ( eventIndex < events.size() && events[ eventIndex ].column == column ) {
            const auto& event = events[ eventIndex ];
            const auto layer = spans[ event.spanIndex ].layer;
            const ActiveKey key{ layer, event.spanIndex };
            if ( event.entering ) {
                active.insert( key );
                if ( layer == HighlightLayer::SearchMatch ) {
                    activeSearchMatches.insert( event.spanIndex );
                }
            }
            else {
                active.erase( key );
                if ( layer == HighlightLayer::SearchMatch ) {
                    activeSearchMatches.erase( event.spanIndex );
                }
            }
            if ( work != nullptr ) {
                ++work->eventVisits;
            }
            ++eventIndex;
        }

        if ( eventIndex == events.size() ) {
            break;
        }
        if ( work != nullptr ) {
            ++work->segmentResolutions;
        }
        if ( active.empty() ) {
            continue;
        }

        const LineColumn segStart{ column };
        const LineColumn segLast{ events[ eventIndex ].column - 1 }; // inclusive end
        const auto& top = spans[ active.rbegin()->second ];
        if ( work != nullptr ) {
            ++work->spanVisits;
        }
        QColor back = top.back;
        // Only the highest active span and (when blending) the last active
        // SearchMatch are inspected per segment. Sorting and active-set updates
        // cost O(N log N) overall; resolution is O(N), not boundaries times N.
        if ( !activeSearchMatches.empty()
             && ( top.layer == HighlightLayer::Highlighter
                  || top.layer == HighlightLayer::QuickHighlighter ) ) {
            if ( work != nullptr ) {
                ++work->spanVisits;
            }
            back = blendSearchMatchOverHighlighter(
                top.back, spans[ *activeSearchMatches.rbegin() ].back );
        }
        const QColor fore = top.fore;

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
