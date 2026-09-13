/*
 * Copyright (C) 2026 Anton Filimonov and other contributors
 *
 * This file is part of klogg.
 *
 * klogg is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include <catch2/catch.hpp>

#include <type_traits>

#include <QCoreApplication>

#include "coalescingtimer.h"

static_assert( !std::is_copy_constructible_v<klogg::CoalescingTimer> );
static_assert( !std::is_copy_assignable_v<klogg::CoalescingTimer> );
static_assert( !std::is_move_constructible_v<klogg::CoalescingTimer> );
static_assert( !std::is_move_assignable_v<klogg::CoalescingTimer> );

namespace {
constexpr int kPresentationIntervalMs = 33;
}

TEST_CASE( "CoalescingTimer owns one deterministic callback per fixed window",
           "[coalescer][refresh-throttling][presentation]" )
{
    int callbacks = 0;
    klogg::CoalescingTimer coalescer{ kPresentationIntervalMs,
                                      [ &callbacks ] { ++callbacks; } };

    coalescer.request();
    REQUIRE( coalescer.isPending() );
    REQUIRE( coalescer.isActive() );
    coalescer.request();
    coalescer.request();
    CHECK( callbacks == 0 );

    CHECK( coalescer.flushPending() );
    CHECK_FALSE( coalescer.isPending() );
    CHECK_FALSE( coalescer.isActive() );
    CHECK( callbacks == 1 );
    CHECK_FALSE( coalescer.flushPending() );
    CHECK( callbacks == 1 );

    coalescer.request();
    REQUIRE( coalescer.isPending() );
    REQUIRE( coalescer.isActive() );
    CHECK( coalescer.flushPending() );
    CHECK( callbacks == 2 );
}

TEST_CASE( "CoalescingTimer callback can request the next fixed window",
           "[coalescer][refresh-throttling][presentation]" )
{
    int callbacks = 0;
    klogg::CoalescingTimer* timer = nullptr;
    klogg::CoalescingTimer coalescer{
        kPresentationIntervalMs,
        [ & ] {
            ++callbacks;
            if ( callbacks == 1 ) {
                timer->request();
            }
        } };
    timer = &coalescer;

    coalescer.request();
    CHECK( coalescer.flushPending() );
    CHECK( callbacks == 1 );
    CHECK( coalescer.isPending() );
    CHECK( coalescer.isActive() );

    CHECK( coalescer.flushPending() );
    CHECK( callbacks == 2 );
    CHECK_FALSE( coalescer.isPending() );
    CHECK_FALSE( coalescer.isActive() );
}

TEST_CASE( "CoalescingTimer cancel and destruction discard pending callbacks",
           "[coalescer][refresh-throttling][presentation]" )
{
    int callbacks = 0;
    {
        klogg::CoalescingTimer coalescer{ kPresentationIntervalMs,
                                          [ &callbacks ] { ++callbacks; } };
        coalescer.request();
        REQUIRE( coalescer.isPending() );
        REQUIRE( coalescer.isActive() );
        coalescer.cancel();
        CHECK_FALSE( coalescer.isPending() );
        CHECK_FALSE( coalescer.isActive() );
        CHECK_FALSE( coalescer.flushPending() );
        CHECK( callbacks == 0 );

        coalescer.request();
        REQUIRE( coalescer.isPending() );
    }

    QCoreApplication::processEvents();
    CHECK( callbacks == 0 );
}
