#include <catch2/catch.hpp>

#include <future>
#include <vector>
#include "asyncworkbudget.h"

TEST_CASE( "Async work leases retain admission across owner and handle copies", "[async-work-budget]" )
{
    auto owner = std::make_unique<klogg::AsyncWorkBudget>( 2 );
    const auto peer = *owner;
    auto first = owner->tryAcquire();
    auto second = peer.tryAcquire();
    REQUIRE( first );
    REQUIRE( second );
    CHECK_FALSE( owner->tryAcquire() );
    auto retained = first;
    first.reset();
    owner.reset();
    CHECK_FALSE( peer.tryAcquire() );
    retained.reset();
    auto replacement = peer.tryAcquire();
    REQUIRE( replacement );
    CHECK_FALSE( peer.tryAcquire() );
    second.reset();
    CHECK( peer.tryAcquire() );
}

TEST_CASE( "Async work budgets admit no more than the shared limit concurrently", "[async-work-budget]" )
{
    klogg::AsyncWorkBudget budget{ 2 };
    std::promise<void> release;
    auto start = release.get_future().share();
    std::vector<std::future<klogg::AsyncWorkBudget::Lease>> workers;
    for ( int index = 0; index < 8; ++index ) {
        workers.push_back( std::async( std::launch::async, [&budget, start] {
            start.wait();
            return budget.tryAcquire();
        } ) );
    }
    release.set_value();
    std::vector<klogg::AsyncWorkBudget::Lease> leases;
    for ( auto& worker : workers ) {
        if ( auto lease = worker.get() ) leases.push_back( std::move( lease ) );
    }
    REQUIRE( leases.size() == 2 );
    CHECK_FALSE( budget.tryAcquire() );
    leases.clear();
    CHECK( budget.tryAcquire() );
}

TEST_CASE( "A zero async work budget fails closed", "[async-work-budget]" )
{
    const klogg::AsyncWorkBudget budget{ 0 };
    CHECK_FALSE( budget.tryAcquire() );
}
