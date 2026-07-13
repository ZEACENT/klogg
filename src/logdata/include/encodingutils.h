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
 * along with klogg.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef KLOGG_ENCODINGUTILS_H
#define KLOGG_ENCODINGUTILS_H

#include <iterator>
#include <string_view>

#include <type_safe/narrow_cast.hpp>

#include "encodingdetector.h"

// Shared multi-byte / single-byte newline (and tab) delimiter finders.
//
// These were extracted verbatim from logdataworker.cpp's private
// parse_data_block namespace so that folder search (foldersearchengine.cpp)
// can reuse the exact same multi-byte newline handling the indexer uses,
// without duplicating the logic. Pure refactor: no behaviour change.
namespace klogg::encoding {

// Finds the next delimiter byte in `data`, validating the surrounding 0x00
// bytes for multi-byte line-feeds (UTF-16LE 0x0A00, UTF-16BE 0x000A,
// UTF-32 LE/BE). An LF byte within lineFeedWidth of a block end is rejected
// as "not delimiter" and only resolves once the carried bytes let the next
// block see the full LF sequence.
inline std::string_view::size_type findNextMultiByteDelimeter( EncodingParameters encodingParams,
                                                               std::string_view data, char delimeter )
{
    auto nextDelimeter = data.find( delimeter );

    if ( nextDelimeter == std::string_view::npos ) {
        return nextDelimeter;
    }

    const auto isNotDelimeter = [ &encodingParams, data ]( std::string_view::size_type checkPos ) {
        const auto lineFeedWidth
            = static_cast<std::string_view::size_type>( encodingParams.lineFeedWidth );

        const auto isCheckForward = encodingParams.lineFeedIndex == 0;

        if ( isCheckForward && checkPos + lineFeedWidth > data.size() ) {
            return true;
        }
        else if ( !isCheckForward && checkPos < lineFeedWidth - 1 ) {
            return true;
        }

        for ( auto i = 1u; i < lineFeedWidth; ++i ) {
            const auto nextByte = isCheckForward ? data[ checkPos + i ] : data[ checkPos - i ];
            if ( nextByte != '\0' ) {
                return true;
            }
        }

        return false;
    };

    while ( nextDelimeter != std::string_view::npos && isNotDelimeter( nextDelimeter ) ) {
        nextDelimeter = data.find( delimeter, nextDelimeter + 1 );
    }

    return nextDelimeter;
}

// Single-byte (UTF-8 / ASCII / legacy 8-bit) delimiter search: a plain
// string_view::find, kept as a function so the caller can dispatch through a
// uniform function-pointer type alongside the multi-byte finder.
inline std::string_view::size_type findNextSingleByteDelimeter( EncodingParameters,
                                                                std::string_view data, char delimeter )
{
    return data.find( delimeter );
}

// Maps a pointer inside a block back to a char offset, accounting for the LF
// byte sitting lineFeedIndex bytes into the multi-byte LF sequence.
inline int charOffsetWithinBlock( const char* blockStart, const char* pointer,
                                  const EncodingParameters& encodingParams )
{
    return type_safe::narrow_cast<int>( std::distance( blockStart, pointer ) )
           - encodingParams.getBeforeCrOffset();
}

} // namespace klogg::encoding

#endif // KLOGG_ENCODINGUTILS_H
