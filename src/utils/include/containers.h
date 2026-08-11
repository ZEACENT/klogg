/*
 * Copyright (C) 2023 Anton Filimonov and other contributors
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

#ifndef KLOGG_CONTAINERS_H
#define KLOGG_CONTAINERS_H

#include <mimalloc.h>
#include <vector>

#include <QByteArray>

#include <type_safe/narrow_cast.hpp>
#include <type_traits>

namespace klogg {
template <typename T>
using vector = std::vector<T, mi_stl_allocator<T>>;

// Qt-version-adaptive container index: int on Qt 5, qsizetype on Qt 6 (it IS
// QByteArray's size() return type). Indexing a QByteArray / QString with
// ContainerIndex never narrows, which eliminates the class of
// "-Werror=conversion: qsizetype -> int" failures that only compile-fail on the
// Qt 5 Linux CI legs and stay invisible on a Qt 6 dev machine (PR #48, PR #56).
// Use it for any index/loop counter passed back into a QByteArray/QString API;
// use klogg::isize when an explicit signed int bound is intended instead.
//
// SCOPE: ContainerIndex tracks QByteArray/QString. Do NOT use it for a
// QStringView receiver -- QStringView has been qsizetype-native since Qt 5.8 on
// BOTH versions, so its indices are plain qsizetype (see wrappedstring.h);
// assigning a QStringView::size() to a ContainerIndex narrows on Qt 5.
using ContainerIndex = decltype( QByteArray{}.size() );

template <class C>
constexpr auto ssize( const C& c )
    -> std::common_type_t<std::ptrdiff_t, std::make_signed_t<decltype( c.size() )>>
{
    using R = std::common_type_t<std::ptrdiff_t, std::make_signed_t<decltype( c.size() )>>;
    return static_cast<R>( c.size() );
}

template <class C>
constexpr int isize( const C& c )
{
    return type_safe::narrow_cast<int>( ssize( c ) );
}


template<class C>
constexpr std::add_const_t<C>& as_const(C& c) noexcept
{
    return c;
}

} // namespace klogg

#endif