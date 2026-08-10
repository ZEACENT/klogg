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

#include <catch2/catch.hpp>

#include "containers.h"

#include <QByteArray>
#include <QString>

#include <type_traits>

// klogg::ContainerIndex is the Qt-version-adaptive container index type: its
// underlying type IS decltype(QByteArray{}.size()), i.e. int on Qt 5 and
// qsizetype on Qt 6. Indexing a Qt container with it therefore never narrows,
// which eliminates the whole class of "-Werror=conversion: qsizetype -> int"
// failures that only surface on the Qt 5 Linux CI legs (PR #48, PR #56).
TEST_CASE( "ContainerIndex tracks the Qt container size type", "[containers]" )
{
    // Compile-time contract: same type as QByteArray/QString::size() on this Qt.
    static_assert( std::is_same_v<klogg::ContainerIndex, decltype( QByteArray{}.size() )>,
                   "ContainerIndex must match QByteArray::size() return type" );
    static_assert( std::is_same_v<klogg::ContainerIndex, decltype( QString{}.size() )>,
                   "ContainerIndex must match QString::size() return type" );

    // Runtime smoke: indexing with ContainerIndex compiles and round-trips
    // without a cast on either Qt version.
    const QByteArray bytes( "a\nb\nc" );
    klogg::ContainerIndex newlineCount = 0;
    for ( klogg::ContainerIndex i = 0; i < bytes.size(); ++i ) {
        if ( bytes.at( i ) == '\n' ) {
            ++newlineCount;
        }
    }
    REQUIRE( newlineCount == 2 );

    const QString text = QStringLiteral( "xy" );
    REQUIRE( text.at( klogg::ContainerIndex{ 1 } ) == QLatin1Char( 'y' ) );
}
