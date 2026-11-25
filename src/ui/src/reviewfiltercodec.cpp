#include "reviewfiltercodec.h"
#include "reviewpanemetadata.h"

#include <algorithm>
#include <array>
#include <utility>

namespace klogg::review {
FilterDecodeResult decodeFilterPatch( const QJsonValue& value, const SearchToolbar::FilterSpec& base )
{
    if ( !value.isObject() ) return FilterDecodeError::InvalidArgument;
    using Spec = SearchToolbar::FilterSpec;
    static constexpr std::array<std::pair<const char*, bool Spec::*>, 6> flags{{
        { "match_case", &Spec::matchCase }, { "use_regexp", &Spec::useRegexp },
        { "inverse", &Spec::inverse }, { "boolean", &Spec::boolean },
        { "auto_refresh", &Spec::autoRefresh }, { "keep_results", &Spec::keepResults }
    }};
    auto result = base;
    const auto fields = value.toObject();
    for ( auto it = fields.begin(); it != fields.end(); ++it ) {
        if ( it.key() == QLatin1String( "text" ) ) {
            if ( !it.value().isString() ) return FilterDecodeError::InvalidArgument;
            result.text = it.value().toString();
            continue;
        }
        const auto flag = std::find_if( flags.begin(), flags.end(), [&]( const auto& field ) {
            return it.key() == QLatin1String( field.first );
        } );
        if ( flag == flags.end() || !it.value().isBool() ) return FilterDecodeError::InvalidArgument;
        result.*( flag->second ) = it.value().toBool();
    }
    if ( result.text.size() > MaxFilterPatternUnits ) return FilterDecodeError::ResourceLimit;
    if ( result.text.contains( QChar{} ) ) return FilterDecodeError::InvalidArgument;
    return result;
}

QJsonObject encodeAppliedFilter( const ReviewAppliedFilter& filter )
{
    const auto& pattern = filter.pattern;
    return { { "text", pattern.pattern }, { "match_case", pattern.isCaseSensitive },
        { "use_regexp", !pattern.isPlainText }, { "inverse", pattern.isExclude },
        { "boolean", pattern.isBoolean }, { "complete", filter.complete } };
}

QJsonObject encodeFilterDraft( const SearchToolbar::FilterDraftSnapshot& snapshot )
{
    const auto& filter = snapshot.filter;
    return { { "revision", QString::number( snapshot.revision ) },
        { "text", filter.text }, { "match_case", filter.matchCase },
        { "use_regexp", filter.useRegexp }, { "inverse", filter.inverse },
        { "boolean", filter.boolean }, { "auto_refresh", filter.autoRefresh },
        { "keep_results", filter.keepResults } };
}
}
