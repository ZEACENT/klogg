#ifndef KLOGG_REVIEWFILTERCODEC_H
#define KLOGG_REVIEWFILTERCODEC_H

#include <cstdint>
#include <variant>
#include <QJsonObject>
#include <QJsonValue>

#include "searchtoolbar.h"

namespace klogg { struct ReviewAppliedFilter; }

namespace klogg::review {
// Match the default QLineEdit capacity; native admission also checks the actual editor.
inline constexpr int MaxFilterPatternUnits = 32767;
enum class FilterDecodeError : std::uint8_t { InvalidArgument, ResourceLimit };
using FilterDecodeResult = std::variant<SearchToolbar::FilterSpec, FilterDecodeError>;
FilterDecodeResult decodeFilterPatch( const QJsonValue& value, const SearchToolbar::FilterSpec& base );
QJsonObject encodeFilterDraft( const SearchToolbar::FilterDraftSnapshot& snapshot );
QJsonObject encodeAppliedFilter( const ReviewAppliedFilter& filter );
}

#endif
