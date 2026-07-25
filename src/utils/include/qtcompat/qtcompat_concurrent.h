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

#ifndef KLOGG_QTCOMPAT_CONCURRENT_H
#define KLOGG_QTCOMPAT_CONCURRENT_H

#include <QtConcurrent>
#include <QFuture>

// Qt 5: QtConcurrent::run(obj, &Class::method, args...)
// Qt 6: QtConcurrent::run(&Class::method, obj, args...)  (requires overload
//        disambiguation)
//
// runConcurrent normalises both spellings into one call site.
namespace klogg::qtcompat {

#if QT_VERSION < QT_VERSION_CHECK( 6, 0, 0 )

// Qt 5 overload of QtConcurrent::run: obj comes first.
template <typename Method, typename Obj, typename... Args>
auto runConcurrent( Method method, Obj* obj, Args&&... args )
    -> QFuture<decltype( ( std::declval<Obj*>()->*method )( std::forward<Args>( args )... ) )>
{
    return QtConcurrent::run( obj, method, std::forward<Args>( args )... );
}

#else // Qt >= 6.0

template <typename Method, typename Obj, typename... Args>
auto runConcurrent( Method method, Obj* obj, Args&&... args )
{
    return QtConcurrent::run( method, obj, std::forward<Args>( args )... );
}

#endif

} // namespace klogg::qtcompat

#endif // KLOGG_QTCOMPAT_CONCURRENT_H
