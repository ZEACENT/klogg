/*
 * Copyright (C) 2026
 *
 * This file is part of klogg.
 *
 * Task 7 cycle 2: deterministic process-vs-integrated live-capture benchmark
 * contracts. The process is a benchmark-only fixture child; no device or
 * wall-clock wait is used by the assertions.
 */

#include <catch2/catch.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "live_capture_benchmark_core.h"

namespace {
namespace benchmark = klogg::benchmarks::livecapture;

benchmark::Bytes ascii( const std::string& text )
{
    return { text.begin(), text.end() };
}

std::uint8_t hexNibble( char digit )
{
    if ( digit >= '0' && digit <= '9' ) {
        return static_cast<std::uint8_t>( digit - '0' );
    }
    if ( digit >= 'a' && digit <= 'f' ) {
        return static_cast<std::uint8_t>( digit - 'a' + 10 );
    }
    FAIL( "invalid hexadecimal fixture" );
    return 0u;
}

benchmark::Bytes bytesFromHex( const std::string& text )
{
    REQUIRE( text.size() % 2u == 0u );
    benchmark::Bytes bytes;
    bytes.reserve( text.size() / 2u );
    for ( std::size_t index = 0u; index < text.size(); index += 2u ) {
        const auto high = static_cast<std::uint8_t>( hexNibble( text.at( index ) ) << 4u );
        bytes.push_back( static_cast<std::uint8_t>( high | hexNibble( text.at( index + 1u ) ) ) );
    }
    return bytes;
}

benchmark::Bytes concatenate( const std::vector<benchmark::Bytes>& records )
{
    benchmark::Bytes stream;
    for ( const auto& record : records ) {
        stream.insert( stream.end(), record.cbegin(), record.cend() );
    }
    return stream;
}

benchmark::StreamBinding binding()
{
    return { benchmark::FramedRecordVersion, 0x0102030405060708ULL, 0x0a0b0c0dU };
}

benchmark::FramedFixture normalFixture()
{
    return benchmark::makeFramedFixture( benchmark::FixturePlan{
        binding(),
        { benchmark::FixtureSegment{ 0u, { ascii( "fixture-zero" ), ascii( "fixture-one" ) } } },
    } );
}

benchmark::FramedFixture reconnectFixture()
{
    return benchmark::makeFramedFixture( benchmark::FixturePlan{
        binding(),
        {
            benchmark::FixtureSegment{ 0u, { ascii( "before-0" ), ascii( "before-1" ) } },
            benchmark::FixtureSegment{ 1u, { ascii( "after-0" ), ascii( "after-1" ) } },
        },
    } );
}

class TemporaryCaptureRoot {
public:
    explicit TemporaryCaptureRoot( std::string suffix )
    {
        const auto base = std::filesystem::temp_directory_path()
                          / ( "klogg-live-capture-benchmark-contract-" + std::move( suffix ) );
        for ( std::size_t attempt = 0u; attempt < 1024u; ++attempt ) {
            auto candidate = base;
            candidate += "-" + std::to_string( attempt );
            std::error_code error;
            if ( std::filesystem::create_directory( candidate, error ) ) {
                path_ = std::move( candidate );
                return;
            }
            REQUIRE( ( !error || error == std::errc::file_exists ) );
        }
        FAIL( "could not reserve a unique temporary capture root" );
    }

    ~TemporaryCaptureRoot()
    {
        std::error_code ignored;
        std::filesystem::remove_all( path_, ignored );
    }

    const std::filesystem::path& path() const
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

benchmark::SyntheticArmPlan armPlan( benchmark::BenchmarkArm arm, benchmark::FramedFixture fixture,
                                     const std::filesystem::path& captureRoot )
{
    benchmark::SyntheticArmPlan plan{
        arm, std::move( fixture ), { 1u, 2u, 3u, 5u, 8u, 13u }, captureRoot, {},
    };
#ifdef KLOGG_LIVE_CAPTURE_FIXTURE_PRODUCER_PATH
    plan.producerExecutable = KLOGG_LIVE_CAPTURE_FIXTURE_PRODUCER_PATH;
#endif
    return plan;
}

void requireCompleteLifecycle( const benchmark::LifecycleTimeline& timeline )
{
    const std::array expected{
        benchmark::LifecycleMilestone::Start,
        benchmark::LifecycleMilestone::Ready,
        benchmark::LifecycleMilestone::FirstByte,
        benchmark::LifecycleMilestone::FirstCommittedRecord,
        benchmark::LifecycleMilestone::Stop,
    };
    REQUIRE( timeline.samples.size() == expected.size() );
    for ( std::size_t index = 0u; index < expected.size(); ++index ) {
        REQUIRE( timeline.samples.at( index ).milestone == expected.at( index ) );
        if ( index != 0u ) {
            REQUIRE( timeline.samples.at( index - 1u ).monotonicNanoseconds
                     <= timeline.samples.at( index ).monotonicNanoseconds );
        }
    }
    REQUIRE_FALSE( benchmark::validateLifecycle( timeline ).failure.has_value() );
}

void requireCanonicalObservation( const benchmark::ArmObservation& observation,
                                  benchmark::BenchmarkArm expectedArm,
                                  const benchmark::FramedFixture& fixture )
{
    REQUIRE( observation.arm == expectedArm );
    REQUIRE( observation.topology == benchmark::RequiredPipelineTopology );
    REQUIRE( observation.binding.version == fixture.binding.version );
    REQUIRE( observation.binding.generation == fixture.binding.generation );
    REQUIRE( observation.binding.trial == fixture.binding.trial );
    REQUIRE( observation.fixtureCrc32 == fixture.fixtureCrc32 );
    REQUIRE( observation.committedRecords == fixture.records.size() );
    REQUIRE( observation.normalStop );
    requireCompleteLifecycle( observation.lifecycle );
}

} // namespace

TEST_CASE( "Synthetic live-capture records have a deterministic versioned binary frame",
           "[benchmark][live-capture][framing][contract]" )
{
    const auto record
        = benchmark::makeSyntheticRecord( binding(), 2u, 0x1112131415161718ULL, ascii( "abc" ) );

    REQUIRE( record.version == benchmark::FramedRecordVersion );
    REQUIRE( record.generation == 0x0102030405060708ULL );
    REQUIRE( record.trial == 0x0a0b0c0dU );
    REQUIRE( record.segment == 2u );
    REQUIRE( record.sequence == 0x1112131415161718ULL );
    REQUIRE( record.payload == ascii( "abc" ) );
    REQUIRE( record.payloadCrc32 == 0x352441c2U );

    // Big-endian, 44-byte header:
    // magic, version, header bytes, frame bytes, generation, trial, segment,
    // sequence, payload bytes, payload CRC32, then raw payload.
    const auto expected = bytesFromHex( "4b4c4342"
                                        "0001"
                                        "002c"
                                        "0000002f"
                                        "0102030405060708"
                                        "0a0b0c0d"
                                        "00000002"
                                        "1112131415161718"
                                        "00000003"
                                        "352441c2"
                                        "616263" );
    REQUIRE( benchmark::encodeFramedRecord( record ) == expected );

    auto invalidCrc = benchmark::makeSyntheticRecord( binding(), 0u, 0u, ascii( "payload" ) );
    invalidCrc.payloadCrc32 ^= 0x01u;
    REQUIRE_THROWS_AS( benchmark::encodeFramedRecord( invalidCrc ), std::invalid_argument );

    auto invalidVersion = benchmark::makeSyntheticRecord( binding(), 0u, 0u, ascii( "payload" ) );
    invalidVersion.version = static_cast<std::uint16_t>( benchmark::FramedRecordVersion + 1u );
    REQUIRE_THROWS_AS( benchmark::encodeFramedRecord( invalidVersion ), std::invalid_argument );

    const auto empty = benchmark::decodeFramedStream( {}, binding() );
    REQUIRE( empty.failure.has_value() );
    REQUIRE( empty.failure->kind == benchmark::DecodeFailureKind::TruncatedFrame );
    REQUIRE( empty.records.empty() );
}

TEST_CASE( "Framed record decoding is independent of deterministic stream fragmentation",
           "[benchmark][live-capture][framing][fragmentation][contract]" )
{
    const auto fixture = reconnectFixture();
    const auto contiguous
        = benchmark::decodeFramedStream( { fixture.framedBytes }, fixture.binding );
    const auto fragmented = benchmark::decodeFramedStream(
        benchmark::fragmentStream( fixture.framedBytes, { 1u, 2u, 3u, 5u, 8u, 13u } ),
        fixture.binding );

    REQUIRE_FALSE( contiguous.failure.has_value() );
    REQUIRE_FALSE( fragmented.failure.has_value() );
    REQUIRE( contiguous.records.size() == fixture.records.size() );
    REQUIRE( fragmented.records.size() == fixture.records.size() );
    REQUIRE( fragmented.segmentTransitions.size() == 1u );
    REQUIRE( fragmented.segmentTransitions.front().previousSegment == 0u );
    REQUIRE( fragmented.segmentTransitions.front().nextSegment == 1u );

    for ( std::size_t index = 0u; index < fixture.records.size(); ++index ) {
        const auto& expected = fixture.records.at( index );
        const auto& actual = fragmented.records.at( index );
        REQUIRE( actual.version == expected.version );
        REQUIRE( actual.generation == expected.generation );
        REQUIRE( actual.trial == expected.trial );
        REQUIRE( actual.segment == expected.segment );
        REQUIRE( actual.sequence == expected.sequence );
        REQUIRE( actual.payload == expected.payload );
        REQUIRE( actual.payloadCrc32 == expected.payloadCrc32 );
    }
}

TEST_CASE( "Framed record decoding detects gaps duplicates and CRC corruption",
           "[benchmark][live-capture][framing][integrity][contract]" )
{
    const auto first = benchmark::makeSyntheticRecord( binding(), 0u, 0u, ascii( "zero" ) );
    const auto second = benchmark::makeSyntheticRecord( binding(), 0u, 1u, ascii( "one" ) );

    SECTION( "sequence gap" )
    {
        const auto third = benchmark::makeSyntheticRecord( binding(), 0u, 2u, ascii( "two" ) );
        const auto report = benchmark::decodeFramedStream(
            { concatenate( { benchmark::encodeFramedRecord( first ),
                             benchmark::encodeFramedRecord( third ) } ) },
            binding() );
        REQUIRE( report.failure.has_value() );
        REQUIRE( report.failure->kind == benchmark::DecodeFailureKind::SequenceGap );
        REQUIRE( report.failure->expected == 1u );
        REQUIRE( report.failure->actual == 2u );
    }

    SECTION( "duplicate sequence" )
    {
        const auto report = benchmark::decodeFramedStream(
            { concatenate( { benchmark::encodeFramedRecord( first ),
                             benchmark::encodeFramedRecord( second ),
                             benchmark::encodeFramedRecord( second ) } ) },
            binding() );
        REQUIRE( report.failure.has_value() );
        REQUIRE( report.failure->kind == benchmark::DecodeFailureKind::DuplicateSequence );
        REQUIRE( report.failure->expected == 2u );
        REQUIRE( report.failure->actual == 1u );
    }

    SECTION( "payload CRC mismatch" )
    {
        auto corrupted = benchmark::encodeFramedRecord( second );
        REQUIRE( corrupted.size() > benchmark::FramedRecordHeaderBytes );
        corrupted.back() ^= 0x01u;
        const auto report = benchmark::decodeFramedStream(
            { concatenate( { benchmark::encodeFramedRecord( first ), corrupted } ) }, binding() );
        REQUIRE( report.failure.has_value() );
        REQUIRE( report.failure->kind == benchmark::DecodeFailureKind::CrcMismatch );
        REQUIRE( report.failure->segment == 0u );
        REQUIRE( report.failure->actual == 1u );
    }
}

TEST_CASE( "Process and integrated arms consume the identical fixture through the live pipeline",
           "[benchmark][live-capture][arms][architecture][contract]" )
{
    const auto fixture = reconnectFixture();
    TemporaryCaptureRoot processRoot( "process" );
    TemporaryCaptureRoot integratedRoot( "integrated" );

    const auto process = benchmark::runSyntheticArm(
        armPlan( benchmark::BenchmarkArm::Process, fixture, processRoot.path() ) );
    const auto integrated = benchmark::runSyntheticArm(
        armPlan( benchmark::BenchmarkArm::Integrated, fixture, integratedRoot.path() ) );

    requireCanonicalObservation( process, benchmark::BenchmarkArm::Process, fixture );
    requireCanonicalObservation( integrated, benchmark::BenchmarkArm::Integrated, fixture );
    REQUIRE( process.fixtureCrc32 == integrated.fixtureCrc32 );
    REQUIRE( process.committedRecords == integrated.committedRecords );
    REQUIRE( process.committedPayloadBytes == integrated.committedPayloadBytes );
    REQUIRE_FALSE( process.queue.highWaterBytes.value.has_value() );
    REQUIRE_FALSE( process.queue.highWaterBytes.synthetic );
    REQUIRE_FALSE( process.queue.highWaterChunks.value.has_value() );
    REQUIRE_FALSE( process.queue.highWaterChunks.synthetic );
    REQUIRE_FALSE( process.queue.backpressureEvents.value.has_value() );
    REQUIRE_FALSE( process.queue.backpressureEvents.synthetic );
    REQUIRE_FALSE( process.queue.droppedRecords.value.has_value() );
    REQUIRE_FALSE( process.queue.droppedRecords.synthetic );
    REQUIRE( integrated.queue.highWaterBytes.value.has_value() );
    REQUIRE( integrated.queue.highWaterBytes.value.value() > 0u );
    REQUIRE( integrated.queue.highWaterBytes.synthetic );
    REQUIRE( integrated.queue.highWaterChunks.value.has_value() );
    REQUIRE( integrated.queue.highWaterChunks.value.value() > 0u );
    REQUIRE( integrated.queue.highWaterChunks.synthetic );
    REQUIRE( integrated.queue.backpressureEvents.value == 0u );
    REQUIRE( integrated.queue.backpressureEvents.synthetic );
    REQUIRE_FALSE( integrated.queue.droppedRecords.value.has_value() );
    REQUIRE_FALSE( integrated.queue.droppedRecords.synthetic );
    REQUIRE( process.runtime.processTreeChildrenStarted.value == 2u );
    REQUIRE( process.runtime.maximumLiveChildren.value == 1u );
    REQUIRE( integrated.runtime.processTreeChildrenStarted.value == 0u );
    REQUIRE( integrated.runtime.maximumLiveChildren.value == 0u );
    REQUIRE( process.lastCommittedRecordNanoseconds
             >= process.lifecycle.samples.at( 3u ).monotonicNanoseconds );
    REQUIRE( process.lastCommittedRecordNanoseconds
             <= process.lifecycle.samples.at( 4u ).monotonicNanoseconds );
    REQUIRE( process.segmentTransitions.size() == integrated.segmentTransitions.size() );
    for ( std::size_t index = 0u; index < process.segmentTransitions.size(); ++index ) {
        REQUIRE( process.segmentTransitions.at( index ).previousSegment
                 == integrated.segmentTransitions.at( index ).previousSegment );
        REQUIRE( process.segmentTransitions.at( index ).nextSegment
                 == integrated.segmentTransitions.at( index ).nextSegment );
    }

    // runSyntheticArm must destroy/delete its StreamingLogData-owned
    // CaptureStore artifacts before returning. The caller-owned root remains.
    REQUIRE( std::filesystem::is_empty( processRoot.path() ) );
    REQUIRE( std::filesystem::is_empty( integratedRoot.path() ) );

    {
        TemporaryCaptureRoot root( "relative-producer" );
        auto plan = armPlan( benchmark::BenchmarkArm::Process, normalFixture(), root.path() );
        plan.producerExecutable = "live_capture_fixture_producer";
        REQUIRE_THROWS_AS( benchmark::runSyntheticArm( plan ), std::invalid_argument );
        REQUIRE( std::filesystem::is_empty( root.path() ) );
    }

    {
        TemporaryCaptureRoot root( "decode-failure" );
        auto invalidFixture = normalFixture();
        ++invalidFixture.binding.generation;
        auto plan = armPlan( benchmark::BenchmarkArm::Integrated, std::move( invalidFixture ),
                             root.path() );
        REQUIRE_THROWS_AS( benchmark::runSyntheticArm( plan ), std::runtime_error );
        REQUIRE( std::filesystem::is_empty( root.path() ) );
    }

    {
        benchmark::FixturePlan fixturePlan;
        fixturePlan.binding = binding();
        benchmark::FixtureSegment segment;
        segment.segment = 0u;
        const auto payload = benchmark::Bytes( 1024u, static_cast<std::uint8_t>( 'x' ) );
        for ( std::size_t index = 0u; index < 128u; ++index ) {
            segment.payloads.push_back( payload );
        }
        fixturePlan.segments.push_back( std::move( segment ) );
        const auto largeFixture = benchmark::makeFramedFixture( fixturePlan );
        REQUIRE( largeFixture.framedBytes.size() > 64u * 1024u );

        TemporaryCaptureRoot root( "partial-read" );
        auto plan = armPlan( benchmark::BenchmarkArm::Process, largeFixture, root.path() );
        plan.fragmentSizes = { 64u * 1024u };
        const auto observation = benchmark::runSyntheticArm( plan );
        REQUIRE( observation.fixtureCrc32 == largeFixture.fixtureCrc32 );
        REQUIRE( observation.committedRecords == largeFixture.records.size() );
        REQUIRE( observation.committedPayloadBytes == 128u * 1024u );
        REQUIRE( std::filesystem::is_empty( root.path() ) );
    }
}

TEST_CASE( "Lifecycle validation requires every explicit milestone in monotonic order",
           "[benchmark][live-capture][lifecycle][contract]" )
{
    const benchmark::LifecycleTimeline complete{ {
        { benchmark::LifecycleMilestone::Start, 10u },
        { benchmark::LifecycleMilestone::Ready, 20u },
        { benchmark::LifecycleMilestone::FirstByte, 30u },
        { benchmark::LifecycleMilestone::FirstCommittedRecord, 40u },
        { benchmark::LifecycleMilestone::Stop, 50u },
    } };
    REQUIRE_FALSE( benchmark::validateLifecycle( complete ).failure.has_value() );

    auto missingReady = complete;
    missingReady.samples.erase( missingReady.samples.begin() + 1 );
    REQUIRE( benchmark::validateLifecycle( missingReady ).failure
             == benchmark::LifecycleFailureKind::MissingMilestone );

    auto outOfOrder = complete;
    std::swap( outOfOrder.samples.at( 1u ), outOfOrder.samples.at( 2u ) );
    REQUIRE( benchmark::validateLifecycle( outOfOrder ).failure
             == benchmark::LifecycleFailureKind::OutOfOrderMilestone );

    auto nonMonotonic = complete;
    nonMonotonic.samples.at( 3u ).monotonicNanoseconds = 29u;
    REQUIRE( benchmark::validateLifecycle( nonMonotonic ).failure
             == benchmark::LifecycleFailureKind::NonMonotonicTimestamp );
}

TEST_CASE( "Queue metrics distinguish measured zero from unavailable and JSON stays aggregate-only",
           "[benchmark][live-capture][metrics][json][privacy][contract]" )
{
    const benchmark::CounterMetric measuredZero{ std::uint64_t{ 0u } };
    const benchmark::CounterMetric unavailable{ std::nullopt };
    REQUIRE( measuredZero.value.has_value() );
    REQUIRE( measuredZero.value.value() == 0u );
    REQUIRE_FALSE( unavailable.value.has_value() );

    benchmark::ArmObservation observation;
    observation.arm = benchmark::BenchmarkArm::Process;
    observation.topology = benchmark::RequiredPipelineTopology;
    observation.binding = binding();
    observation.fixtureCrc32 = 0x12345678u;
    observation.committedRecords = 2u;
    observation.committedPayloadBytes = 24u;
    observation.segmentTransitions = { { 0u, 1u } };
    observation.lifecycle = { {
        { benchmark::LifecycleMilestone::Start, 10u },
        { benchmark::LifecycleMilestone::Ready, 20u },
        { benchmark::LifecycleMilestone::FirstByte, 30u },
        { benchmark::LifecycleMilestone::FirstCommittedRecord, 40u },
        { benchmark::LifecycleMilestone::Stop, 50u },
    } };
    observation.lastCommittedRecordNanoseconds = 45u;
    observation.queue.highWaterBytes = { 128u, true };
    observation.queue.highWaterChunks = { 2u, true };
    observation.queue.backpressureEvents = measuredZero;
    observation.queue.droppedRecords = unavailable;
    observation.normalStop = true;

    const auto aggregate = benchmark::makeAggregateResult( observation );
    const auto firstJson = benchmark::serializeAggregateJson( aggregate );
    const auto secondJson = benchmark::serializeAggregateJson( aggregate );

    REQUIRE( firstJson == secondJson );
    REQUIRE( firstJson.find( "\"schema_version\":1" ) != std::string::npos );
    REQUIRE( firstJson.find( "\"startup_ns\":10" ) != std::string::npos );
    REQUIRE( firstJson.find( "\"first_byte_latency_ns\":10" ) != std::string::npos );
    REQUIRE( firstJson.find( "\"first_commit_latency_ns\":20" ) != std::string::npos );
    REQUIRE( firstJson.find( "\"teardown_ns\":5" ) != std::string::npos );
    REQUIRE( firstJson.find( "\"throughput_payload_bytes_per_second\":1600000000" )
             != std::string::npos );
    REQUIRE( firstJson.find( "\"queue_high_water_bytes_available\":1" ) != std::string::npos );
    REQUIRE( firstJson.find( "\"queue_high_water_bytes_synthetic\":1" ) != std::string::npos );
    REQUIRE( firstJson.find( "\"queue_high_water_bytes\":128" ) != std::string::npos );
    REQUIRE( firstJson.find( "\"queue_high_water_chunks_available\":1" ) != std::string::npos );
    REQUIRE( firstJson.find( "\"queue_high_water_chunks_synthetic\":1" ) != std::string::npos );
    REQUIRE( firstJson.find( "\"queue_high_water_chunks\":2" ) != std::string::npos );
    REQUIRE( firstJson.find( "\"queue_backpressure_events_available\":1" ) != std::string::npos );
    REQUIRE( firstJson.find( "\"queue_backpressure_events_synthetic\":0" ) != std::string::npos );
    REQUIRE( firstJson.find( "\"queue_backpressure_events\":0" ) != std::string::npos );
    REQUIRE( firstJson.find( "\"queue_dropped_records_available\":0" ) != std::string::npos );
    REQUIRE( firstJson.find( "\"queue_dropped_records\":" ) == std::string::npos );
    REQUIRE( firstJson.find( "fixture-zero" ) == std::string::npos );
    REQUIRE( firstJson.find( "before-0" ) == std::string::npos );
    REQUIRE( firstJson.find( "\"payload\"" ) == std::string::npos );
    REQUIRE( firstJson.find( "\"records\"" ) == std::string::npos );
}

TEST_CASE( "Normal stop preserves one segment while reconnect advances exactly one segment",
           "[benchmark][live-capture][lifecycle][reconnect][contract]" )
{
    TemporaryCaptureRoot normalRoot( "normal-stop" );
    TemporaryCaptureRoot reconnectRoot( "reconnect-stop" );
    const auto normal = normalFixture();
    const auto reconnect = reconnectFixture();

    const auto normalObservation = benchmark::runSyntheticArm(
        armPlan( benchmark::BenchmarkArm::Integrated, normal, normalRoot.path() ) );
    REQUIRE( normalObservation.normalStop );
    REQUIRE( normalObservation.segmentTransitions.empty() );
    REQUIRE( normalObservation.committedRecords == 2u );
    requireCompleteLifecycle( normalObservation.lifecycle );

    const auto reconnectObservation = benchmark::runSyntheticArm(
        armPlan( benchmark::BenchmarkArm::Integrated, reconnect, reconnectRoot.path() ) );
    REQUIRE( reconnectObservation.normalStop );
    REQUIRE( reconnectObservation.segmentTransitions.size() == 1u );
    REQUIRE( reconnectObservation.segmentTransitions.front().previousSegment == 0u );
    REQUIRE( reconnectObservation.segmentTransitions.front().nextSegment == 1u );
    REQUIRE( reconnectObservation.committedRecords == 4u );
    requireCompleteLifecycle( reconnectObservation.lifecycle );

    REQUIRE( std::filesystem::is_empty( normalRoot.path() ) );
    REQUIRE( std::filesystem::is_empty( reconnectRoot.path() ) );
}
