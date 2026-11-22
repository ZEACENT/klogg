#ifndef KLOGG_REVIEWHIGHLIGHTERCODEC_H
#define KLOGG_REVIEWHIGHLIGHTERCODEC_H

#include <cstdint>
#include <variant>
#include <QJsonObject>
#include <QJsonValue>
#include "highlighterset.h"

namespace klogg::review {

struct HighlighterReplacement {
    QList<HighlighterSet> sets;
    QStringList activeSetIds;
};
enum class HighlighterDecodeError : std::uint8_t { InvalidArgument, ResourceLimit };
using DecodedHighlighterReplacement = std::variant<HighlighterReplacement, HighlighterDecodeError>;

QJsonObject encodeHighlighters( const HighlighterSetCollection::Snapshot& state );
// Decode bounded semantic values only. Regex compilation belongs to private
// transaction preparation, not the GUI-thread wire parser.
DecodedHighlighterReplacement decodeHighlighters( const QJsonValue& sets, const QJsonValue& activeIds );

} // namespace klogg::review
#endif
