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

#include "mergefileorder.h"

TEST_CASE( "sortedMergeFilePaths sorts files alphabetically by name", "[mergefileorder]" )
{
    const QStringList input{
        "/var/log/c.log",
        "/var/log/a.log",
        "/var/log/b.log",
    };
    const std::vector<QString> result = sortedMergeFilePaths( input );
    REQUIRE( result.size() == 3 );
    REQUIRE( result[ 0 ].endsWith( "/a.log" ) );
    REQUIRE( result[ 1 ].endsWith( "/b.log" ) );
    REQUIRE( result[ 2 ].endsWith( "/c.log" ) );
}

TEST_CASE( "sortedMergeFilePaths sorts naturally so file2 precedes file10", "[mergefileorder]" )
{
    const QStringList input{
        "/dir/file10.log",
        "/dir/file2.log",
        "/dir/file1.log",
    };
    const std::vector<QString> result = sortedMergeFilePaths( input );
    REQUIRE( result[ 0 ].endsWith( "/file1.log" ) );
    REQUIRE( result[ 1 ].endsWith( "/file2.log" ) );
    REQUIRE( result[ 2 ].endsWith( "/file10.log" ) );
}

TEST_CASE( "sortedMergeFilePaths is case-insensitive", "[mergefileorder]" )
{
    // Case-insensitive: 'a.log' precedes 'B.log' (strict case-sensitive would
    // put the uppercase 'B' first).
    const QStringList input{ "/dir/B.log", "/dir/a.log" };
    const std::vector<QString> result = sortedMergeFilePaths( input );
    REQUIRE( result[ 0 ].endsWith( "/a.log" ) );
    REQUIRE( result[ 1 ].endsWith( "/B.log" ) );
}

TEST_CASE( "sortedMergeFilePaths tiebreaks by full path for equal names", "[mergefileorder]" )
{
    const QStringList input{
        "/x/z/same.log",
        "/x/a/same.log",
    };
    const std::vector<QString> result = sortedMergeFilePaths( input );
    REQUIRE( result[ 0 ].endsWith( "/a/same.log" ) );
    REQUIRE( result[ 1 ].endsWith( "/z/same.log" ) );
}

TEST_CASE( "sortedMergeFilePaths preserves elements and handles empty input", "[mergefileorder]" )
{
    REQUIRE( sortedMergeFilePaths( QStringList{} ).empty() );

    const QStringList input{ "/d/b.log", "/d/a.log", "/d/c.log" };
    const auto result = sortedMergeFilePaths( input );
    REQUIRE( result.size() == static_cast<size_t>( input.size() ) );
}
