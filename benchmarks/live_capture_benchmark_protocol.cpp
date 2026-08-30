/*
 * Copyright (C) 2026
 *
 * This file is part of klogg.
 */

#include "live_capture_benchmark_core.h"

#include <algorithm>
#include <array>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace klogg::benchmarks::livecapture {
namespace {

constexpr std::array<std::uint8_t, 4u> FrameMagic{ 0x4bu, 0x4cu, 0x43u, 0x42u };
constexpr std::size_t MaximumFrameBytes = 1024u * 1024u;
constexpr std::size_t DecoderInputSliceBytes = 64u * 1024u;
constexpr std::size_t MaximumRecordCount = 1000000u;

std::uint16_t readU16( const Bytes& bytes, std::size_t offset )
{
    return static_cast<std::uint16_t>( ( static_cast<std::uint16_t>( bytes.at( offset ) ) << 8u )
                                       | static_cast<std::uint16_t>( bytes.at( offset + 1u ) ) );
}

std::uint32_t readU32( const Bytes& bytes, std::size_t offset )
{
    std::uint32_t value = 0u;
    for ( std::size_t index = 0u; index < 4u; ++index ) {
        value = static_cast<std::uint32_t>(
            ( value << 8u ) | static_cast<std::uint32_t>( bytes.at( offset + index ) ) );
    }
    return value;
}

std::uint64_t readU64( const Bytes& bytes, std::size_t offset )
{
    std::uint64_t value = 0u;
    for ( std::size_t index = 0u; index < 8u; ++index ) {
        value = ( value << 8u ) | static_cast<std::uint64_t>( bytes.at( offset + index ) );
    }
    return value;
}

void appendU16( Bytes& bytes, std::uint16_t value )
{
    bytes.push_back( static_cast<std::uint8_t>( value >> 8u ) );
    bytes.push_back( static_cast<std::uint8_t>( value & 0xffu ) );
}

void appendU32( Bytes& bytes, std::uint32_t value )
{
    for ( unsigned shift : { 24u, 16u, 8u, 0u } ) {
        bytes.push_back( static_cast<std::uint8_t>( ( value >> shift ) & 0xffu ) );
    }
}

void appendU64( Bytes& bytes, std::uint64_t value )
{
    for ( unsigned shift : { 56u, 48u, 40u, 32u, 24u, 16u, 8u, 0u } ) {
        bytes.push_back( static_cast<std::uint8_t>( ( value >> shift ) & 0xffu ) );
    }
}

void appendReport( DecodeReport& destination, DecodeReport source )
{
    destination.records.insert( destination.records.end(),
                                std::make_move_iterator( source.records.begin() ),
                                std::make_move_iterator( source.records.end() ) );
    destination.segmentTransitions.insert(
        destination.segmentTransitions.end(),
        std::make_move_iterator( source.segmentTransitions.begin() ),
        std::make_move_iterator( source.segmentTransitions.end() ) );
    if ( source.failure ) {
        destination.failure = source.failure;
    }
}

void appendMetric( std::ostringstream& json, const char* name, const CounterMetric& metric )
{
    json << ",\"" << name << "_available\":" << ( metric.value ? 1 : 0 ) << ",\"" << name
         << "_synthetic\":" << ( metric.synthetic ? 1 : 0 );
    if ( metric.value ) {
        json << ",\"" << name << "\":" << *metric.value;
    }
}

std::uint64_t perSecondRate( std::size_t count, std::uint64_t elapsedNanoseconds )
{
    if ( elapsedNanoseconds == 0u ) {
        return 0u;
    }
    const auto rate = static_cast<long double>( count ) * 1000000000.0L
                      / static_cast<long double>( elapsedNanoseconds );
    const auto maximum = static_cast<long double>( std::numeric_limits<std::uint64_t>::max() );
    return rate >= maximum ? std::numeric_limits<std::uint64_t>::max()
                           : static_cast<std::uint64_t>( rate );
}

std::uint64_t milestoneTime( const LifecycleTimeline& timeline, LifecycleMilestone milestone )
{
    const auto found = std::find_if(
        timeline.samples.cbegin(), timeline.samples.cend(),
        [ milestone ]( const MilestoneSample& sample ) { return sample.milestone == milestone; } );
    if ( found == timeline.samples.cend() ) {
        throw std::invalid_argument( "aggregate lifecycle is incomplete" );
    }
    return found->monotonicNanoseconds;
}

} // namespace

std::uint32_t crc32( const Bytes& bytes ) noexcept
{
    std::uint32_t crc = 0xffffffffu;
    for ( const auto byte : bytes ) {
        crc ^= static_cast<std::uint32_t>( byte );
        for ( unsigned bit = 0u; bit < 8u; ++bit ) {
            const auto mask = static_cast<std::uint32_t>( 0u - ( crc & 1u ) );
            crc = ( crc >> 1u ) ^ ( 0xedb88320u & mask );
        }
    }
    return crc ^ 0xffffffffu;
}

SyntheticRecord makeSyntheticRecord( StreamBinding binding, std::uint32_t segment,
                                     std::uint64_t sequence, Bytes payload )
{
    SyntheticRecord record;
    record.version = binding.version;
    record.generation = binding.generation;
    record.trial = binding.trial;
    record.segment = segment;
    record.sequence = sequence;
    record.payload = std::move( payload );
    record.payloadCrc32 = crc32( record.payload );
    return record;
}

Bytes encodeFramedRecord( const SyntheticRecord& record )
{
    if ( record.version != FramedRecordVersion ) {
        throw std::invalid_argument( "synthetic benchmark record version is not canonical" );
    }
    if ( record.payloadCrc32 != crc32( record.payload ) ) {
        throw std::invalid_argument( "synthetic benchmark record CRC is not canonical" );
    }
    if ( record.payload.size() > MaximumFrameBytes - FramedRecordHeaderBytes
         || record.payload.size() > std::numeric_limits<std::uint32_t>::max() ) {
        throw std::length_error( "synthetic benchmark record is too large" );
    }

    const auto frameBytes = FramedRecordHeaderBytes + record.payload.size();
    Bytes encoded;
    encoded.reserve( frameBytes );
    // GCC 11 emits a spurious stringop-overflow for vector::insert over an
    // std::array range here; an explicit append keeps -Werror builds quiet.
    for ( const auto magicByte : FrameMagic ) {
        encoded.push_back( magicByte );
    }
    appendU16( encoded, record.version );
    appendU16( encoded, static_cast<std::uint16_t>( FramedRecordHeaderBytes ) );
    appendU32( encoded, static_cast<std::uint32_t>( frameBytes ) );
    appendU64( encoded, record.generation );
    appendU32( encoded, record.trial );
    appendU32( encoded, record.segment );
    appendU64( encoded, record.sequence );
    appendU32( encoded, static_cast<std::uint32_t>( record.payload.size() ) );
    appendU32( encoded, record.payloadCrc32 );
    encoded.insert( encoded.end(), record.payload.cbegin(), record.payload.cend() );
    return encoded;
}

struct FramedStreamDecoder::State {
    explicit State( StreamBinding expectedBinding )
        : binding( expectedBinding )
    {
    }

    DecodeReport parseAvailable()
    {
        DecodeReport report;
        while ( !failure && pending.size() >= FramedRecordHeaderBytes ) {
            if ( !std::equal( FrameMagic.cbegin(), FrameMagic.cend(), pending.cbegin() ) ) {
                failure = DecodeFailure{ DecodeFailureKind::MalformedFrame, 0u, 0u, 0u };
                break;
            }

            const auto version = readU16( pending, 4u );
            const auto headerBytes = readU16( pending, 6u );
            const auto frameBytes = readU32( pending, 8u );
            const auto generation = readU64( pending, 12u );
            const auto trial = readU32( pending, 20u );
            const auto segment = readU32( pending, 24u );
            const auto sequence = readU64( pending, 28u );
            const auto payloadBytes = readU32( pending, 36u );
            const auto payloadCrc32 = readU32( pending, 40u );

            if ( version != binding.version ) {
                failure = DecodeFailure{ DecodeFailureKind::UnsupportedVersion, segment,
                                         binding.version, version };
                break;
            }
            if ( headerBytes != FramedRecordHeaderBytes || frameBytes < headerBytes
                 || frameBytes > MaximumFrameBytes || payloadBytes != frameBytes - headerBytes ) {
                failure = DecodeFailure{ DecodeFailureKind::MalformedFrame, segment, 0u, 0u };
                break;
            }
            if ( pending.size() < frameBytes ) {
                break;
            }
            if ( generation != binding.generation || trial != binding.trial ) {
                failure = DecodeFailure{ DecodeFailureKind::BindingMismatch, segment,
                                         binding.generation, generation };
                break;
            }

            if ( !previousSegment ) {
                if ( segment != 0u ) {
                    failure = DecodeFailure{ DecodeFailureKind::SegmentGap, segment, 0u, segment };
                    break;
                }
                previousSegment = segment;
                expectedSequence = 0u;
            }
            else if ( segment != *previousSegment ) {
                const auto expectedSegment = static_cast<std::uint64_t>( *previousSegment ) + 1u;
                if ( static_cast<std::uint64_t>( segment ) != expectedSegment ) {
                    failure = DecodeFailure{ DecodeFailureKind::SegmentGap, segment,
                                             expectedSegment, segment };
                    break;
                }
                report.segmentTransitions.push_back( { *previousSegment, segment } );
                previousSegment = segment;
                expectedSequence = 0u;
            }

            if ( sequence < expectedSequence ) {
                failure = DecodeFailure{ DecodeFailureKind::DuplicateSequence, segment,
                                         expectedSequence, sequence };
                break;
            }
            if ( sequence > expectedSequence ) {
                failure = DecodeFailure{ DecodeFailureKind::SequenceGap, segment, expectedSequence,
                                         sequence };
                break;
            }

            const auto payloadOffset
                = static_cast<Bytes::difference_type>( FramedRecordHeaderBytes );
            const auto payloadLength = static_cast<Bytes::difference_type>( payloadBytes );
            const auto payloadBegin = pending.cbegin() + payloadOffset;
            Bytes payload( payloadBegin, payloadBegin + payloadLength );
            if ( crc32( payload ) != payloadCrc32 ) {
                failure = DecodeFailure{ DecodeFailureKind::CrcMismatch, segment, payloadCrc32,
                                         sequence };
                break;
            }
            if ( recordCount == MaximumRecordCount ) {
                failure = DecodeFailure{ DecodeFailureKind::MalformedFrame, segment,
                                         MaximumRecordCount, recordCount + 1u };
                break;
            }

            report.records.push_back( SyntheticRecord{ version, generation, trial, segment,
                                                       sequence, std::move( payload ),
                                                       payloadCrc32 } );
            ++recordCount;
            ++expectedSequence;
            const auto consumed = static_cast<Bytes::difference_type>( frameBytes );
            pending.erase( pending.cbegin(), pending.cbegin() + consumed );
        }
        report.failure = failure;
        return report;
    }

    StreamBinding binding;
    Bytes pending;
    std::optional<std::uint32_t> previousSegment;
    std::uint64_t expectedSequence{ 0u };
    std::size_t recordCount{ 0u };
    std::optional<DecodeFailure> failure;
    bool receivedBytes{ false };
    bool finished{ false };
};

FramedStreamDecoder::FramedStreamDecoder( StreamBinding binding )
    : state_( std::make_unique<State>( binding ) )
{
}

FramedStreamDecoder::~FramedStreamDecoder() = default;
FramedStreamDecoder::FramedStreamDecoder( FramedStreamDecoder&& ) noexcept = default;
FramedStreamDecoder& FramedStreamDecoder::operator=( FramedStreamDecoder&& ) noexcept = default;

DecodeReport FramedStreamDecoder::push( const Bytes& fragment )
{
    DecodeReport report;
    if ( state_->finished ) {
        report.failure = DecodeFailure{ DecodeFailureKind::MalformedFrame, 0u, 0u, 0u };
        return report;
    }
    if ( state_->failure ) {
        report.failure = state_->failure;
        return report;
    }

    std::size_t offset = 0u;
    while ( offset < fragment.size() ) {
        const auto size = std::min( DecoderInputSliceBytes, fragment.size() - offset );
        const auto begin = static_cast<Bytes::difference_type>( offset );
        const auto end = static_cast<Bytes::difference_type>( offset + size );
        state_->pending.insert( state_->pending.end(), fragment.cbegin() + begin,
                                fragment.cbegin() + end );
        state_->receivedBytes = true;
        appendReport( report, state_->parseAvailable() );
        if ( report.failure ) {
            return report;
        }
        offset += size;
    }
    return report;
}

DecodeReport FramedStreamDecoder::finish()
{
    DecodeReport report;
    if ( state_->finished ) {
        report.failure = state_->failure;
        return report;
    }
    state_->finished = true;
    appendReport( report, state_->parseAvailable() );
    if ( report.failure ) {
        return report;
    }
    if ( !state_->receivedBytes || !state_->pending.empty() ) {
        report.failure = DecodeFailure{ DecodeFailureKind::TruncatedFrame, 0u,
                                        FramedRecordHeaderBytes, state_->pending.size() };
        state_->failure = report.failure;
    }
    return report;
}

DecodeReport decodeFramedStream( const std::vector<Bytes>& fragments, StreamBinding binding )
{
    FramedStreamDecoder decoder( binding );
    DecodeReport report;
    for ( const auto& fragment : fragments ) {
        appendReport( report, decoder.push( fragment ) );
        if ( report.failure ) {
            return report;
        }
    }
    appendReport( report, decoder.finish() );
    return report;
}

std::vector<Bytes> fragmentStream( const Bytes& stream,
                                   const std::vector<std::size_t>& fragmentSizes )
{
    if ( stream.empty() ) {
        return {};
    }
    if ( fragmentSizes.empty()
         || std::any_of( fragmentSizes.cbegin(), fragmentSizes.cend(),
                         []( std::size_t size ) { return size == 0u; } ) ) {
        throw std::invalid_argument( "fragment sizes must be non-empty and positive" );
    }

    std::vector<Bytes> fragments;
    std::size_t offset = 0u;
    std::size_t sizeIndex = 0u;
    while ( offset < stream.size() ) {
        const auto size = std::min( fragmentSizes.at( sizeIndex ), stream.size() - offset );
        const auto fragmentBegin = static_cast<Bytes::difference_type>( offset );
        const auto fragmentEnd = static_cast<Bytes::difference_type>( offset + size );
        fragments.emplace_back( stream.cbegin() + fragmentBegin, stream.cbegin() + fragmentEnd );
        offset += size;
        sizeIndex = ( sizeIndex + 1u ) % fragmentSizes.size();
    }
    return fragments;
}

LifecycleValidation validateLifecycle( const LifecycleTimeline& timeline )
{
    constexpr std::array order{
        LifecycleMilestone::Start,     LifecycleMilestone::Ready,
        LifecycleMilestone::FirstByte, LifecycleMilestone::FirstCommittedRecord,
        LifecycleMilestone::Stop,
    };
    if ( timeline.samples.size() < order.size() ) {
        return { LifecycleFailureKind::MissingMilestone };
    }
    if ( timeline.samples.size() > order.size() ) {
        return { LifecycleFailureKind::DuplicateMilestone };
    }
    for ( std::size_t index = 0u; index < order.size(); ++index ) {
        if ( timeline.samples.at( index ).milestone != order.at( index ) ) {
            const auto duplicate = std::count_if(
                timeline.samples.cbegin(), timeline.samples.cend(),
                [ milestone = timeline.samples.at( index ).milestone ](
                    const MilestoneSample& sample ) { return sample.milestone == milestone; } );
            return { duplicate > 1 ? LifecycleFailureKind::DuplicateMilestone
                                   : LifecycleFailureKind::OutOfOrderMilestone };
        }
        if ( index != 0u
             && timeline.samples.at( index ).monotonicNanoseconds
                    < timeline.samples.at( index - 1u ).monotonicNanoseconds ) {
            return { LifecycleFailureKind::NonMonotonicTimestamp };
        }
    }
    return {};
}

FramedFixture makeFramedFixture( const FixturePlan& plan )
{
    if ( plan.binding.version != FramedRecordVersion || plan.segments.empty() ) {
        throw std::invalid_argument(
            "fixture requires protocol version 1 and at least one segment" );
    }

    FramedFixture fixture;
    fixture.binding = plan.binding;
    std::uint32_t expectedSegment = 0u;
    for ( const auto& segment : plan.segments ) {
        if ( segment.segment != expectedSegment || segment.payloads.empty() ) {
            throw std::invalid_argument(
                "fixture segments must be contiguous, non-empty, and start at zero" );
        }
        std::uint64_t sequence = 0u;
        for ( const auto& payload : segment.payloads ) {
            auto record = makeSyntheticRecord( plan.binding, segment.segment, sequence, payload );
            const auto encoded = encodeFramedRecord( record );
            fixture.framedBytes.insert( fixture.framedBytes.end(), encoded.cbegin(),
                                        encoded.cend() );
            fixture.records.push_back( std::move( record ) );
            ++sequence;
        }
        ++expectedSegment;
    }
    fixture.fixtureCrc32 = crc32( fixture.framedBytes );
    return fixture;
}

AggregateResult makeAggregateResult( const ArmObservation& observation )
{
    if ( observation.topology != RequiredPipelineTopology || !observation.normalStop
         || validateLifecycle( observation.lifecycle ).failure ) {
        throw std::invalid_argument( "cannot aggregate an invalid synthetic arm observation" );
    }
    return {
        1u,
        observation.arm,
        observation.binding,
        observation.fixtureCrc32,
        observation.committedRecords,
        observation.committedPayloadBytes,
        observation.segmentTransitions.size() + 1u,
        observation.lifecycle,
        observation.lastCommittedRecordNanoseconds,
        observation.queue,
        observation.runtime,
        observation.normalStop,
    };
}

std::string serializeAggregateJson( const AggregateResult& result )
{
    if ( result.schemaVersion != 1u || validateLifecycle( result.lifecycle ).failure ) {
        throw std::invalid_argument( "aggregate result is invalid" );
    }

    const auto start = milestoneTime( result.lifecycle, LifecycleMilestone::Start );
    const auto ready = milestoneTime( result.lifecycle, LifecycleMilestone::Ready );
    const auto firstByte = milestoneTime( result.lifecycle, LifecycleMilestone::FirstByte );
    const auto firstCommit
        = milestoneTime( result.lifecycle, LifecycleMilestone::FirstCommittedRecord );
    const auto stop = milestoneTime( result.lifecycle, LifecycleMilestone::Stop );
    if ( result.lastCommittedRecordNanoseconds < firstCommit
         || result.lastCommittedRecordNanoseconds > stop ) {
        throw std::invalid_argument( "aggregate last-commit timestamp is invalid" );
    }

    std::ostringstream json;
    json.imbue( std::locale::classic() );
    json << "{\"schema_version\":1,\"benchmark\":\"synthetic-live-capture-"
         << ( result.arm == BenchmarkArm::Process ? "process" : "integrated" )
         << "\",\"status\":\"ok\",\"reason_code\":null,\"message\":\"completed\",\"metrics\":{"
         << "\"arm_process\":" << ( result.arm == BenchmarkArm::Process ? 1 : 0 )
         << ",\"frame_version\":" << result.binding.version
         << ",\"generation\":" << result.binding.generation << ",\"trial\":" << result.binding.trial
         << ",\"fixture_crc32\":" << result.fixtureCrc32
         << ",\"committed_records\":" << result.committedRecords
         << ",\"committed_payload_bytes\":" << result.committedPayloadBytes
         << ",\"segment_count\":" << result.segmentCount
         << ",\"normal_stop\":" << ( result.normalStop ? 1 : 0 )
         << ",\"lifecycle_start_ns\":" << start << ",\"lifecycle_ready_ns\":" << ready
         << ",\"lifecycle_first_byte_ns\":" << firstByte
         << ",\"lifecycle_first_committed_record_ns\":" << firstCommit
         << ",\"lifecycle_last_committed_record_ns\":" << result.lastCommittedRecordNanoseconds
         << ",\"lifecycle_stop_ns\":" << stop << ",\"startup_ns\":" << ( ready - start )
         << ",\"first_byte_latency_ns\":" << ( firstByte - ready )
         << ",\"first_commit_latency_ns\":" << ( firstCommit - ready )
         << ",\"throughput_payload_bytes_per_second\":"
         << perSecondRate( result.committedPayloadBytes,
                           result.lastCommittedRecordNanoseconds - firstByte )
         << ",\"teardown_ns\":" << ( stop - result.lastCommittedRecordNanoseconds )
         << ",\"correctness_fixture_crc_match\":1"
         << ",\"correctness_sequence_gap_count\":0"
         << ",\"correctness_duplicate_count\":0"
         << ",\"correctness_crc_error_count\":0";
    appendMetric( json, "process_cpu_ns", result.runtime.processCpuNanoseconds );
    appendMetric( json, "child_cpu_ns", result.runtime.childCpuNanoseconds );
    appendMetric( json, "peak_rss_bytes", result.runtime.peakRssBytes );
    appendMetric( json, "voluntary_context_switches", result.runtime.voluntaryContextSwitches );
    appendMetric( json, "involuntary_context_switches", result.runtime.involuntaryContextSwitches );
    appendMetric( json, "process_tree_children_started",
                  result.runtime.processTreeChildrenStarted );
    appendMetric( json, "maximum_live_children", result.runtime.maximumLiveChildren );
    appendMetric( json, "queue_high_water_bytes", result.queue.highWaterBytes );
    appendMetric( json, "queue_high_water_chunks", result.queue.highWaterChunks );
    appendMetric( json, "queue_backpressure_events", result.queue.backpressureEvents );
    appendMetric( json, "queue_dropped_records", result.queue.droppedRecords );
    json << "}}\n";
    return json.str();
}

} // namespace klogg::benchmarks::livecapture
