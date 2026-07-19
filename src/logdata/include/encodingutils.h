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

// Cheap structural UTF-8 check used by EncodingDetector's fast path: true iff
// the sample contains no NUL bytes and is well-formed UTF-8 (pure ASCII
// qualifies). NULs are rejected so BOM-less UTF-16/32 and binary files still
// reach the full detection pipeline (uchardet + codecForUtfText null-pattern
// heuristics). Overlong forms, surrogates, > U+10FFFF and stray continuation
// bytes are all rejected; a sample truncated mid-sequence (detection windows
// can cut a multibyte char) is conservatively rejected too, which only costs a
// uchardet run for that file, never a wrong guess.
inline bool isLikelyUtf8( std::string_view data )
{
    const auto* const bytes = reinterpret_cast<const unsigned char*>( data.data() );
    const auto size = data.size();
    std::string_view::size_type i = 0;
    while ( i < size ) {
        const unsigned char lead = bytes[ i ];
        if ( lead == 0 ) {
            return false;
        }
        if ( lead < 0x80 ) {
            ++i;
            continue;
        }

        std::string_view::size_type continuation = 0;
        unsigned char rangeLow = 0x80;
        unsigned char rangeHigh = 0xBF;
        if ( lead >= 0xC2 && lead <= 0xDF ) {
            continuation = 1;
        }
        else if ( lead >= 0xE0 && lead <= 0xEF ) {
            continuation = 2;
            if ( lead == 0xE0 ) {
                rangeLow = 0xA0; // no overlong 3-byte
            }
            else if ( lead == 0xED ) {
                rangeHigh = 0x9F; // no UTF-16 surrogates
            }
        }
        else if ( lead >= 0xF0 && lead <= 0xF4 ) {
            continuation = 3;
            if ( lead == 0xF0 ) {
                rangeLow = 0x90; // no overlong 4-byte
            }
            else if ( lead == 0xF4 ) {
                rangeHigh = 0x8F; // <= U+10FFFF
            }
        }
        else {
            return false; // 0x80-0xC1 stray/deprecated lead, 0xF5+ out of range
        }

        if ( i + continuation >= size ) {
            return false; // truncated sequence at the sample edge
        }
        if ( bytes[ i + 1 ] < rangeLow || bytes[ i + 1 ] > rangeHigh ) {
            return false;
        }
        for ( std::string_view::size_type k = 2; k <= continuation; ++k ) {
            if ( bytes[ i + k ] < 0x80 || bytes[ i + k ] > 0xBF ) {
                return false;
            }
        }
        i += continuation + 1;
    }
    return true;
}

// True when the sample starts with a Unicode byte-order mark (UTF-8/16/32,
// LE/BE). BOM-bearing files keep the full detection pipeline so the BOM
// handling in codecForUtfText is preserved exactly.
inline bool startsWithUnicodeBom( std::string_view data )
{
    static constexpr char Utf8Bom[] = { '\xEF', '\xBB', '\xBF' };
    static constexpr char Utf32LeBom[] = { '\xFF', '\xFE', '\x00', '\x00' };
    static constexpr char Utf32BeBom[] = { '\x00', '\x00', '\xFE', '\xFF' };
    if ( data.size() >= 4
         && ( data.compare( 0, 4, Utf32LeBom, 4 ) == 0
              || data.compare( 0, 4, Utf32BeBom, 4 ) == 0 ) ) {
        return true;
    }
    if ( data.size() >= 3 && data.compare( 0, 3, Utf8Bom, 3 ) == 0 ) {
        return true;
    }
    return data.size() >= 2
           && ( ( data[ 0 ] == '\xFF' && data[ 1 ] == '\xFE' )
                || ( data[ 0 ] == '\xFE' && data[ 1 ] == '\xFF' ) );
}

} // namespace klogg::encoding

#endif // KLOGG_ENCODINGUTILS_H
