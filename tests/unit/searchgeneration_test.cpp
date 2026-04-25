/*
 * Copyright (C) 2026
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

#include <QtGlobal>

#include "searchgeneration.h"

// The gate is the receiver-side counterpart to TASK-001.  Pinning it as a
// free function and unit-testing here means future refactors of
// CrawlerWidget::updateFilteredView cannot silently weaken the staleness
// contract (e.g. by changing != to <).

TEST_CASE( "isStaleSearchGeneration accepts only the exact active generation" )
{
    using klogg::isStaleSearchGeneration;

    REQUIRE_FALSE( isStaleSearchGeneration( 0u, 0u ) );
    REQUIRE_FALSE( isStaleSearchGeneration( 1u, 1u ) );
    REQUIRE_FALSE( isStaleSearchGeneration( quint64{ 1 } << 40, quint64{ 1 } << 40 ) );
}

TEST_CASE( "isStaleSearchGeneration treats older generations as stale" )
{
    using klogg::isStaleSearchGeneration;

    REQUIRE( isStaleSearchGeneration( 0u, 1u ) );
    REQUIRE( isStaleSearchGeneration( 99u, 100u ) );
    REQUIRE( isStaleSearchGeneration( 0u, ( quint64{ 1 } << 40 ) ) );
}

TEST_CASE( "isStaleSearchGeneration also treats unexpectedly newer generations as stale" )
{
    using klogg::isStaleSearchGeneration;

    // A receiver that hasn't observed the corresponding runSearch() yet should
    // not silently consume a newer-numbered signal -- both directions are
    // mismatch and both deserve to be dropped.
    REQUIRE( isStaleSearchGeneration( 1u, 0u ) );
    REQUIRE( isStaleSearchGeneration( 100u, 99u ) );
}
