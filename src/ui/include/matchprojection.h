#ifndef KLOGG_MATCHPROJECTION_H
#define KLOGG_MATCHPROJECTION_H

#include <QRegularExpression>
#include <QString>
#include <algorithm>
#include <cstdint>
#include <iterator>
#include <utility>
#include <vector>

#include "containers.h"
#include "linetypes.h"

namespace klogg {

enum class MatchProjection : std::uint8_t { FullMatch, CapturesOrFullMatch };

struct MatchFragment {
    LineColumn start;
    LineLength length;
};

struct MatchOccurrence {
    // The navigation anchor is independent of the projected copy/highlight spans.
    LineColumn start;
    LineLength length;
    std::vector<MatchFragment> fragments;
};

inline MatchOccurrence projectMatchOccurrence(
    const QRegularExpressionMatch& match,
    MatchProjection projection = MatchProjection::CapturesOrFullMatch )
{
    MatchOccurrence result{ LineColumn{ match.capturedStart() },
                            LineLength{ match.capturedLength() }, {} };
    const bool captures = projection == MatchProjection::CapturesOrFullMatch
                          && match.regularExpression().captureCount() > 0;
    const int first = captures ? 1 : 0;
    const int last = captures ? match.lastCapturedIndex() : 0;
    for ( int capture = first; capture <= last; ++capture ) {
        const auto start = match.capturedStart( capture );
        const auto length = match.capturedLength( capture );
        if ( start >= 0 && length > 0 ) {
            result.fragments.push_back( { LineColumn{ start }, LineLength{ length } } );
        }
    }
    std::stable_sort( result.fragments.begin(), result.fragments.end(),
                      []( const auto& left, const auto& right ) {
                          return left.start < right.start;
                      } );
    size_t count = 0;
    for ( const auto& fragment : result.fragments ) {
        if ( count > 0
             && fragment.start < result.fragments[ count - 1 ].start
                                     + result.fragments[ count - 1 ].length ) {
            auto& previous = result.fragments[ count - 1 ];
            previous.length = std::max( previous.start + previous.length,
                                        fragment.start + fragment.length )
                              - previous.start;
        }
        else {
            result.fragments[ count++ ] = fragment;
        }
    }
    result.fragments.resize( count );
    return result;
}

// Merge only within one occurrence. Adjacent captures and distinct overlapping
// occurrences remain independent semantic entries, irrespective of paint merging.
template <typename Visitor>
void visitMatchFragments( const QRegularExpressionMatch& match, MatchProjection projection,
                          const Visitor& visitor )
{
    for ( const auto& fragment : projectMatchOccurrence( match, projection ).fragments ) {
        visitor( fragment.start, fragment.length );
    }
}

template <typename Visitor>
bool visitLineMatchFragments( const QRegularExpression& expression, const QString& text,
                              MatchProjection projection, const Visitor& visitor )
{
    bool matched = false;
    auto iterator = expression.globalMatch( text );
    while ( iterator.hasNext() ) {
        matched = true;
        visitMatchFragments( iterator.next(), projection, visitor );
    }
    return matched;
}

// Matching uses decoded, ANSI-normalized source text. Tab expansion changes only
// display geometry; it must never change the regex input in one entry point.
class MatchTextColumns {
  public:
    explicit MatchTextColumns( const QString& text )
    {
        LineColumn::UnderlyingType delta = 0;
        auto position = text.indexOf( QChar::Tabulation );
        while ( position >= 0 ) {
            delta += TabStop - ( ( position + delta ) % TabStop ) - 1;
            tabs_.emplace_back( position, delta );
            position = text.indexOf( QChar::Tabulation, position + 1 );
        }
    }

    LineColumn displayColumn( LineColumn column ) const
    {
        const auto next = std::lower_bound(
            tabs_.begin(), tabs_.end(), column.get(),
            []( const auto& tab, auto offset ) { return tab.first < offset; } );
        return LineColumn{ column.get()
                           + ( next == tabs_.begin() ? 0 : std::prev( next )->second ) };
    }

    LineColumn searchColumn( LineColumn column ) const
    {
        LineColumn::UnderlyingType previousDelta = 0;
        for ( const auto& tab : tabs_ ) {
            if ( column.get() <= tab.first + previousDelta ) {
                break;
            }
            if ( column.get() < tab.first + 1 + tab.second ) {
                return LineColumn{ tab.first };
            }
            previousDelta = tab.second;
        }
        return LineColumn{ column.get() - previousDelta };
    }

  private:
    std::vector<std::pair<LineColumn::UnderlyingType, LineColumn::UnderlyingType>> tabs_;
};

} // namespace klogg

#endif
