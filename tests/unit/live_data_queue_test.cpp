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

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "livedataqueue.h"
#include "livedatastatistics.h"

namespace {
using klogg::livecapture::Generation;
using klogg::livecapture::LiveDataChunk;
using klogg::livecapture::LiveDataEnqueueResult;
using klogg::livecapture::LiveDataQueue;
using klogg::livecapture::LiveDataQueueLimits;
using klogg::livecapture::LiveDataStatistics;

std::vector<std::uint8_t> bytes( const std::string& text )
{
    return { text.begin(), text.end() };
}

LiveDataChunk chunk( Generation generation, const std::string& text )
{
    return { generation, bytes( text ) };
}

bool waitForBlockedProducer( const LiveDataQueue& queue )
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{ 2 };
    while ( std::chrono::steady_clock::now() < deadline ) {
        if ( queue.waitingProducerCount() > 0u ) {
            return true;
        }
        std::this_thread::yield();
    }
    return false;
}

bool hasConsistentAccounting( const LiveDataStatistics& statistics,
                              const LiveDataQueueLimits& limits )
{
    const auto accountedBytes
        = statistics.deliveredBytes + statistics.queuedBytes + statistics.backpressuredBytes;
    const auto accountedChunks
        = statistics.deliveredChunks + statistics.queuedChunks + statistics.backpressuredChunks;

    return statistics.receivedBytes == accountedBytes
           && statistics.receivedChunks == accountedChunks
           && statistics.queuedBytes <= limits.maxQueuedBytes
           && statistics.queuedChunks <= limits.maxQueuedChunks
           && statistics.queuedBytes <= statistics.highWaterQueuedBytes
           && statistics.queuedChunks <= statistics.highWaterQueuedChunks
           && statistics.highWaterQueuedBytes <= limits.maxQueuedBytes
           && statistics.highWaterQueuedChunks <= limits.maxQueuedChunks;
}
} // namespace

TEST_CASE( "LiveDataQueue coalesces generation-tagged chunks without changing byte order",
           "[livecapture][data-queue][contract]" )
{
    constexpr Generation generation = 17u;
    const LiveDataQueueLimits limits{ 64u, 8u };
    std::size_t drainNotifications = 0u;
    LiveDataQueue queue( limits, generation, [ &drainNotifications ] { ++drainNotifications; } );

    REQUIRE( queue.tryEnqueue( chunk( generation, "alpha\nbe" ) )
             == LiveDataEnqueueResult::Accepted );
    REQUIRE( queue.tryEnqueue( chunk( generation, "ta\n" ) ) == LiveDataEnqueueResult::Accepted );
    REQUIRE( queue.tryEnqueue( LiveDataChunk{
                 generation,
                 { static_cast<std::uint8_t>( '\0' ), static_cast<std::uint8_t>( 'g' ),
                   static_cast<std::uint8_t>( 'a' ), static_cast<std::uint8_t>( 'm' ),
                   static_cast<std::uint8_t>( 'm' ), static_cast<std::uint8_t>( 'a' ) } } )
             == LiveDataEnqueueResult::Accepted );

    REQUIRE( drainNotifications == 1u );
    const auto drained = queue.drain();
    REQUIRE( drained.has_value() );
    REQUIRE( drained->generation == generation );
    REQUIRE( drained->sourceChunks == 3u );

    auto expected = bytes( "alpha\nbeta\n" );
    const auto binaryTail = std::vector<std::uint8_t>{
        static_cast<std::uint8_t>( '\0' ), static_cast<std::uint8_t>( 'g' ),
        static_cast<std::uint8_t>( 'a' ),  static_cast<std::uint8_t>( 'm' ),
        static_cast<std::uint8_t>( 'm' ),  static_cast<std::uint8_t>( 'a' )
    };
    expected.insert( expected.end(), binaryTail.begin(), binaryTail.end() );
    REQUIRE( drained->bytes == expected );
    REQUIRE_FALSE( queue.drain().has_value() );

    REQUIRE( queue.tryEnqueue( chunk( generation, "next" ) ) == LiveDataEnqueueResult::Accepted );
    REQUIRE( drainNotifications == 2u );
}

TEST_CASE( "LiveDataQueue enforces byte and chunk limits independently",
           "[livecapture][data-queue][contract][backpressure]" )
{
    constexpr Generation generation = 19u;

    SECTION( "byte bound" )
    {
        LiveDataQueue queue( LiveDataQueueLimits{ 3u, 8u }, generation, [] {} );
        REQUIRE( queue.tryEnqueue( chunk( generation, "abc" ) )
                 == LiveDataEnqueueResult::Accepted );
        REQUIRE( queue.tryEnqueue( chunk( generation, "d" ) )
                 == LiveDataEnqueueResult::Backpressure );
    }

    SECTION( "chunk bound" )
    {
        LiveDataQueue queue( LiveDataQueueLimits{ 64u, 2u }, generation, [] {} );
        REQUIRE( queue.tryEnqueue( chunk( generation, "a" ) ) == LiveDataEnqueueResult::Accepted );
        REQUIRE( queue.tryEnqueue( chunk( generation, "b" ) ) == LiveDataEnqueueResult::Accepted );
        REQUIRE( queue.tryEnqueue( chunk( generation, "c" ) )
                 == LiveDataEnqueueResult::Backpressure );
    }
}

TEST_CASE( "LiveDataQueue rejects impossible blocking writes without waiting for queued data",
           "[livecapture][data-queue][contract][backpressure][blocking]" )
{
    constexpr Generation generation = 21u;
    LiveDataQueue queue( LiveDataQueueLimits{ 2u, 1u }, generation, [] {} );
    REQUIRE( queue.tryEnqueue( chunk( generation, "a" ) ) == LiveDataEnqueueResult::Accepted );

    std::mutex completionMutex;
    std::condition_variable completionCondition;
    bool producerCompleted = false;
    auto producerResult = LiveDataEnqueueResult::Accepted;
    std::thread producer( [ & ] {
        const auto result = queue.enqueueWait( chunk( generation, "oversized" ) );
        {
            std::lock_guard<std::mutex> lock( completionMutex );
            producerResult = result;
            producerCompleted = true;
        }
        completionCondition.notify_one();
    } );

    bool completedWithoutCapacityChange = false;
    {
        std::unique_lock<std::mutex> lock( completionMutex );
        completedWithoutCapacityChange = completionCondition.wait_for(
            lock, std::chrono::seconds{ 2 }, [ &producerCompleted ] { return producerCompleted; } );
    }

    // Always release and join the producer before asserting so a regression
    // cannot strand the test process.
    queue.close();
    producer.join();

    REQUIRE( completedWithoutCapacityChange );
    REQUIRE( producerResult == LiveDataEnqueueResult::Backpressure );
}

TEST_CASE( "LiveDataQueue preserves rejected payloads for an explicit retry",
           "[livecapture][data-queue][contract][backpressure][ownership]" )
{
    constexpr Generation generation = 22u;
    LiveDataQueue queue( LiveDataQueueLimits{ 1u, 1u }, generation, [] {} );
    REQUIRE( queue.tryEnqueue( chunk( generation, "a" ) ) == LiveDataEnqueueResult::Accepted );

    const auto rejected = chunk( generation, "retry" );
    const auto originalBytes = rejected.bytes;
    REQUIRE( queue.tryEnqueue( rejected ) == LiveDataEnqueueResult::Backpressure );
    REQUIRE( rejected.bytes == originalBytes );
}

TEST_CASE( "Live data statistics counters saturate instead of wrapping",
           "[livecapture][data-queue][contract][statistics][overflow]" )
{
    auto statistics = LiveDataStatistics{};
    const auto maximum = std::numeric_limits<std::size_t>::max();
    statistics.receivedBytes = maximum - 1u;
    statistics.receivedChunks = maximum;
    statistics.deliveredBytes = maximum - 2u;
    statistics.deliveredChunks = maximum - 1u;
    statistics.backpressuredBytes = maximum;
    statistics.backpressuredChunks = maximum - 1u;

    klogg::livecapture::recordLiveDataReceived( statistics, 2u, 1u );
    klogg::livecapture::recordLiveDataDelivered( statistics, 3u, 2u );
    klogg::livecapture::recordLiveDataBackpressure( statistics, 1u, 2u );

    REQUIRE( statistics.receivedBytes == maximum );
    REQUIRE( statistics.receivedChunks == maximum );
    REQUIRE( statistics.deliveredBytes == maximum );
    REQUIRE( statistics.deliveredChunks == maximum );
    REQUIRE( statistics.backpressuredBytes == maximum );
    REQUIRE( statistics.backpressuredChunks == maximum );
}

TEST_CASE( "LiveDataQueue bounds bytes and chunks with explicit backpressure and no silent drops",
           "[livecapture][data-queue][contract][backpressure][statistics]" )
{
    constexpr Generation generation = 23u;
    const LiveDataQueueLimits limits{ 5u, 2u };
    LiveDataQueue queue( limits, generation, [] {} );

    REQUIRE( queue.tryEnqueue( chunk( generation, "ab" ) ) == LiveDataEnqueueResult::Accepted );
    REQUIRE( queue.tryEnqueue( chunk( generation, "cde" ) ) == LiveDataEnqueueResult::Accepted );
    REQUIRE( queue.tryEnqueue( chunk( generation, "x" ) ) == LiveDataEnqueueResult::Backpressure );

    const auto full = queue.statistics();
    REQUIRE( full.generation == generation );
    REQUIRE( full.receivedBytes == 6u );
    REQUIRE( full.receivedChunks == 3u );
    REQUIRE( full.queuedBytes == 5u );
    REQUIRE( full.queuedChunks == 2u );
    REQUIRE( full.deliveredBytes == 0u );
    REQUIRE( full.deliveredChunks == 0u );
    REQUIRE( full.backpressuredBytes == 1u );
    REQUIRE( full.backpressuredChunks == 1u );
    REQUIRE( full.highWaterQueuedBytes == 5u );
    REQUIRE( full.highWaterQueuedChunks == 2u );
    REQUIRE( hasConsistentAccounting( full, limits ) );

    const auto drained = queue.drain();
    REQUIRE( drained.has_value() );
    REQUIRE( drained->bytes == bytes( "abcde" ) );
    REQUIRE( drained->sourceChunks == 2u );

    const auto empty = queue.statistics();
    REQUIRE( empty.receivedBytes == 6u );
    REQUIRE( empty.receivedChunks == 3u );
    REQUIRE( empty.queuedBytes == 0u );
    REQUIRE( empty.queuedChunks == 0u );
    REQUIRE( empty.deliveredBytes == 5u );
    REQUIRE( empty.deliveredChunks == 2u );
    REQUIRE( empty.backpressuredBytes == 1u );
    REQUIRE( empty.backpressuredChunks == 1u );
    REQUIRE( empty.highWaterQueuedBytes == 5u );
    REQUIRE( empty.highWaterQueuedChunks == 2u );
    REQUIRE( hasConsistentAccounting( empty, limits ) );
}

TEST_CASE( "LiveDataQueue close wakes blocked producers and rejects later input",
           "[livecapture][data-queue][contract][close][backpressure]" )
{
    constexpr Generation generation = 31u;
    LiveDataQueue queue( LiveDataQueueLimits{ 1u, 1u }, generation, [] {} );
    REQUIRE( queue.tryEnqueue( chunk( generation, "a" ) ) == LiveDataEnqueueResult::Accepted );

    auto producerResult = LiveDataEnqueueResult::Accepted;
    std::thread producer(
        [ & ] { producerResult = queue.enqueueWait( chunk( generation, "b" ) ); } );

    const auto producerBlocked = waitForBlockedProducer( queue );
    queue.close();
    producer.join();

    REQUIRE( producerBlocked );
    REQUIRE( producerResult == LiveDataEnqueueResult::Closed );
    REQUIRE( queue.tryEnqueue( chunk( generation, "c" ) ) == LiveDataEnqueueResult::Closed );
    REQUIRE( queue.tryEnqueue( chunk( generation - 1u, "stale" ) )
             == LiveDataEnqueueResult::StaleGeneration );

    const auto drained = queue.drain();
    REQUIRE( drained.has_value() );
    REQUIRE( drained->bytes == bytes( "a" ) );
    REQUIRE_FALSE( queue.drain().has_value() );
}

TEST_CASE( "LiveDataQueue close is terminal without synthesizing drain notifications",
           "[livecapture][data-queue][contract][close][notification]" )
{
    constexpr Generation generation = 37u;
    std::size_t drainNotifications = 0u;
    LiveDataQueue queue( LiveDataQueueLimits{ 4u, 2u }, generation,
                         [ &drainNotifications ] { ++drainNotifications; } );

    REQUIRE( queue.tryEnqueue( chunk( generation, "data" ) ) == LiveDataEnqueueResult::Accepted );
    REQUIRE( drainNotifications == 1u );

    queue.close();
    queue.close();
    REQUIRE( drainNotifications == 1u );

    const auto reset = queue.reset( generation + 1u );
    REQUIRE( reset.retiredGeneration == generation );
    REQUIRE( reset.retiredBytes == 4u );
    REQUIRE( reset.retiredChunks == 1u );
    REQUIRE( queue.isClosed() );
    REQUIRE( queue.tryEnqueue( chunk( generation + 1u, "new" ) ) == LiveDataEnqueueResult::Closed );
    REQUIRE( drainNotifications == 1u );
}

TEST_CASE( "LiveDataQueue same-generation reset retires blocked producers",
           "[livecapture][data-queue][contract][generation][reset]" )
{
    constexpr Generation generation = 39u;
    LiveDataQueue queue( LiveDataQueueLimits{ 3u, 1u }, generation, [] {} );
    REQUIRE( queue.tryEnqueue( chunk( generation, "old" ) ) == LiveDataEnqueueResult::Accepted );

    auto producerResult = LiveDataEnqueueResult::Accepted;
    std::thread producer(
        [ & ] { producerResult = queue.enqueueWait( chunk( generation, "new" ) ); } );

    const auto producerBlocked = waitForBlockedProducer( queue );
    const auto reset = queue.reset( generation );
    producer.join();

    REQUIRE( producerBlocked );
    REQUIRE( reset.retiredGeneration == generation );
    REQUIRE( reset.retiredBytes == 3u );
    REQUIRE( reset.retiredChunks == 1u );
    REQUIRE( producerResult == LiveDataEnqueueResult::StaleGeneration );
    REQUIRE_FALSE( queue.drain().has_value() );
    REQUIRE( queue.tryEnqueue( chunk( generation, "new" ) ) == LiveDataEnqueueResult::Accepted );
}

TEST_CASE( "LiveDataQueue reset retires pending data and wakes stale producers",
           "[livecapture][data-queue][contract][generation][reset]" )
{
    constexpr Generation firstGeneration = 41u;
    constexpr Generation nextGeneration = 42u;
    LiveDataQueue queue( LiveDataQueueLimits{ 3u, 1u }, firstGeneration, [] {} );
    REQUIRE( queue.tryEnqueue( chunk( firstGeneration, "old" ) )
             == LiveDataEnqueueResult::Accepted );

    auto producerResult = LiveDataEnqueueResult::Accepted;
    std::thread producer(
        [ & ] { producerResult = queue.enqueueWait( chunk( firstGeneration, "new" ) ); } );

    const auto producerBlocked = waitForBlockedProducer( queue );
    const auto reset = queue.reset( nextGeneration );
    producer.join();

    REQUIRE( producerBlocked );
    REQUIRE( reset.retiredGeneration == firstGeneration );
    REQUIRE( reset.retiredBytes == 3u );
    REQUIRE( reset.retiredChunks == 1u );
    REQUIRE( producerResult == LiveDataEnqueueResult::StaleGeneration );
    REQUIRE_FALSE( queue.drain().has_value() );
    REQUIRE( queue.tryEnqueue( chunk( firstGeneration, "stale" ) )
             == LiveDataEnqueueResult::StaleGeneration );
    REQUIRE( queue.tryEnqueue( chunk( nextGeneration, "new" ) )
             == LiveDataEnqueueResult::Accepted );

    const auto drained = queue.drain();
    REQUIRE( drained.has_value() );
    REQUIRE( drained->generation == nextGeneration );
    REQUIRE( drained->bytes == bytes( "new" ) );

    const auto statistics = queue.statistics();
    REQUIRE( statistics.generation == nextGeneration );
    REQUIRE( statistics.receivedBytes == 3u );
    REQUIRE( statistics.receivedChunks == 1u );
    REQUIRE( statistics.deliveredBytes == 3u );
    REQUIRE( statistics.deliveredChunks == 1u );
}

TEST_CASE( "LiveDataQueue statistics snapshots remain internally consistent during activity",
           "[livecapture][data-queue][contract][statistics][threading]" )
{
    constexpr Generation generation = 53u;
    const LiveDataQueueLimits limits{ 32u, 8u };
    LiveDataQueue queue( limits, generation, [] {} );
    std::atomic<bool> readerReady{ false };
    std::atomic<bool> producerDone{ false };
    std::atomic<bool> producerResultsValid{ true };
    std::atomic<bool> snapshotsConsistent{ true };

    std::thread reader( [ & ] {
        readerReady.store( true );
        do {
            if ( !hasConsistentAccounting( queue.statistics(), limits ) ) {
                snapshotsConsistent.store( false );
            }
        } while ( !producerDone.load() );
    } );

    while ( !readerReady.load() ) {
        std::this_thread::yield();
    }

    for ( std::size_t index = 0u; index < 2000u; ++index ) {
        const auto result = queue.tryEnqueue( chunk( generation, "x" ) );
        if ( result != LiveDataEnqueueResult::Accepted
             && result != LiveDataEnqueueResult::Backpressure ) {
            producerResultsValid.store( false );
        }
        if ( index % 3u == 2u ) {
            static_cast<void>( queue.drain() );
        }
    }
    static_cast<void>( queue.drain() );
    producerDone.store( true );
    reader.join();

    REQUIRE( producerResultsValid.load() );
    REQUIRE( snapshotsConsistent.load() );
    REQUIRE( hasConsistentAccounting( queue.statistics(), limits ) );
}
