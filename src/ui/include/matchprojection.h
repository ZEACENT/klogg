#ifndef KLOGG_MATCHPROJECTION_H
#define KLOGG_MATCHPROJECTION_H

#include <QRegularExpression>
#include <QString>
#include <cstdint>

#include "linetypes.h"

namespace klogg {

enum class MatchProjection : std::uint8_t { FullMatch, CapturesOrFullMatch };

// Projection is independent of colors and selection geometry. Callers retain
// occurrence boundaries so overlapping lookahead occurrences stay distinct.
template <typename Visitor>
void visitMatchFragments( const QRegularExpressionMatch& match, MatchProjection projection,
                          const Visitor& visitor )
{
    const bool captures = projection == MatchProjection::CapturesOrFullMatch
                          && match.regularExpression().captureCount() > 0;
    const int first = captures ? 1 : 0;
    const int last = captures ? match.lastCapturedIndex() : 0;
    for ( int capture = first; capture <= last; ++capture ) {
        const auto start = match.capturedStart( capture );
        const auto length = match.capturedLength( capture );
        if ( start >= 0 && length > 0 ) {
            visitor( LineColumn{ start }, LineLength{ length } );
        }
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

} // namespace klogg

#endif
