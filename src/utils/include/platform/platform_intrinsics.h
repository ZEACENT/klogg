/*
 * Copyright (C) 2024 Anton Filimonov and other contributors
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

#ifndef KLOGG_PLATFORM_INTRINSICS_H
#define KLOGG_PLATFORM_INTRINSICS_H

#include <cstdint>

#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#include <intrin.h>
#pragma warning( disable : 4244 )
#endif

namespace klogg::platform {

// Count leading zero bits. Uses MSVC intrinsics on Windows, GCC/Clang
// builtins elsewhere.  __builtin_clzll(0) is UB on GCC/Clang, so guard zero
// before the platform branches.
inline int countLeadingZeroes( uint64_t value )
{
    if ( value == 0 ) {
        return 64;
    }
#ifdef Q_OS_WIN
#if _WIN64
    unsigned long leading_zero = 0;
    if ( _BitScanReverse64( &leading_zero, value ) ) {
        return 63 - static_cast<int>( leading_zero );
    }
    return 64;
#else
    unsigned long leading_zero = 0;
    if ( _BitScanReverse( &leading_zero, static_cast<uint32_t>( value ) ) ) {
        return 63 - static_cast<int>( leading_zero );
    }
    return 64;
#endif
#else
    return __builtin_clzll( value );
#endif
}

} // namespace klogg::platform

#endif // KLOGG_PLATFORM_INTRINSICS_H
