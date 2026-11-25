#ifndef KLOGG_INDEXEDLINELENGTH_H
#define KLOGG_INDEXEDLINELENGTH_H

#include <cassert>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace klogg::indexing {
// Saturate at the native overflow sentinel before narrowing. Parameterizing the
// width exercises both Qt container widths without multi-gigabyte fixtures.
template <typename Length>
Length lineLength( std::int64_t bytes, Length additionalSpaces, int lineFeedWidth )
{
    static_assert( std::is_integral_v<Length> && std::is_signed_v<Length>
                   && sizeof( Length ) <= sizeof( std::int64_t ) );
    assert( bytes >= 0 && additionalSpaces >= 0 && lineFeedWidth > 0 );
    constexpr auto maximum = std::numeric_limits<Length>::max();
    const auto characters = bytes / lineFeedWidth;
    if ( characters >= maximum || additionalSpaces > maximum - characters ) return maximum;
    return static_cast<Length>( characters + additionalSpaces );
}

template <typename Length>
Length tabSpaces( Length additionalSpaces, int tabOffset, int tabStop )
{
    static_assert( std::is_integral_v<Length> && std::is_signed_v<Length>
                   && sizeof( Length ) <= sizeof( std::int64_t ) );
    assert( additionalSpaces >= 0 && tabStop > 0 );
    constexpr std::int64_t maximum = std::numeric_limits<Length>::max();
    const auto spaces = static_cast<std::int64_t>( additionalSpaces );
    if ( tabOffset > 0 && spaces > maximum - tabOffset ) return static_cast<Length>( maximum );
    const auto expanded = spaces + tabOffset;
    const auto added = static_cast<std::int64_t>( tabStop ) - ( expanded % tabStop ) - 1;
    if ( spaces > maximum - added ) return static_cast<Length>( maximum );
    // Both the increment and the sum now fit the destination type.
    additionalSpaces += static_cast<Length>( added );
    return additionalSpaces;
}
}

#endif
