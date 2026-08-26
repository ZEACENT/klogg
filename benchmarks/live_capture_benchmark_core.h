/*
 * Copyright (C) 2026
 *
 * This file is part of klogg.
 *
 * Task 7 cycle 2: benchmark-local API shared by the deterministic fixture
 * producer and the process-vs-integrated benchmark executable. The benchmark
 * implementation remains isolated from release behavior.
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace klogg::benchmarks::livecapture {

using Bytes = std::vector<std::uint8_t>;

constexpr std::uint16_t FramedRecordVersion = 1u;
constexpr std::size_t FramedRecordHeaderBytes = 44u;

struct SyntheticRecord {
    std::uint16_t version{ FramedRecordVersion };
    std::uint64_t generation{ 0u };
    std::uint32_t trial{ 0u };
    std::uint32_t segment{ 0u };
    std::uint64_t sequence{ 0u };
    Bytes payload;
    std::uint32_t payloadCrc32{ 0u };
};

struct StreamBinding {
    std::uint16_t version{ FramedRecordVersion };
    std::uint64_t generation{ 0u };
    std::uint32_t trial{ 0u };
};

enum class DecodeFailureKind : std::uint8_t {
    None,
    MalformedFrame,
    UnsupportedVersion,
    BindingMismatch,
    SegmentGap,
    SequenceGap,
    DuplicateSequence,
    CrcMismatch,
    TruncatedFrame
};

struct DecodeFailure {
    DecodeFailureKind kind{ DecodeFailureKind::None };
    std::uint32_t segment{ 0u };
    std::uint64_t expected{ 0u };
    std::uint64_t actual{ 0u };
};

struct SegmentTransition {
    std::uint32_t previousSegment{ 0u };
    std::uint32_t nextSegment{ 0u };
};

struct DecodeReport {
    std::vector<SyntheticRecord> records;
    std::vector<SegmentTransition> segmentTransitions;
    std::optional<DecodeFailure> failure;
};

std::uint32_t crc32( const Bytes& bytes ) noexcept;
SyntheticRecord makeSyntheticRecord( StreamBinding binding, std::uint32_t segment,
                                     std::uint64_t sequence, Bytes payload );
Bytes encodeFramedRecord( const SyntheticRecord& record );

class FramedStreamDecoder {
public:
    explicit FramedStreamDecoder( StreamBinding binding );
    ~FramedStreamDecoder();
    FramedStreamDecoder( FramedStreamDecoder&& ) noexcept;
    FramedStreamDecoder& operator=( FramedStreamDecoder&& ) noexcept;
    FramedStreamDecoder( const FramedStreamDecoder& ) = delete;
    FramedStreamDecoder& operator=( const FramedStreamDecoder& ) = delete;

    DecodeReport push( const Bytes& fragment );
    DecodeReport finish();

private:
    struct State;
    std::unique_ptr<State> state_;
};

DecodeReport decodeFramedStream( const std::vector<Bytes>& fragments, StreamBinding binding );
std::vector<Bytes> fragmentStream( const Bytes& stream,
                                   const std::vector<std::size_t>& fragmentSizes );

enum class BenchmarkArm : std::uint8_t { Process, Integrated };

enum class PipelineStage : std::uint8_t { LiveLogController, StreamingLogData, CaptureStore };

using PipelineTopology = std::array<PipelineStage, 3u>;
constexpr PipelineTopology RequiredPipelineTopology{
    PipelineStage::LiveLogController,
    PipelineStage::StreamingLogData,
    PipelineStage::CaptureStore,
};

enum class LifecycleMilestone : std::uint8_t {
    Start,
    Ready,
    FirstByte,
    FirstCommittedRecord,
    Stop
};

struct MilestoneSample {
    LifecycleMilestone milestone{ LifecycleMilestone::Start };
    std::uint64_t monotonicNanoseconds{ 0u };
};

struct LifecycleTimeline {
    std::vector<MilestoneSample> samples;
};

enum class LifecycleFailureKind : std::uint8_t {
    None,
    MissingMilestone,
    DuplicateMilestone,
    OutOfOrderMilestone,
    NonMonotonicTimestamp
};

struct LifecycleValidation {
    std::optional<LifecycleFailureKind> failure;
};

LifecycleValidation validateLifecycle( const LifecycleTimeline& timeline );

struct CounterMetric {
    std::optional<std::uint64_t> value;
    bool synthetic{ false };
};

struct QueueMetrics {
    CounterMetric highWaterBytes;
    CounterMetric highWaterChunks;
    CounterMetric backpressureEvents;
    CounterMetric droppedRecords;
};

struct RuntimeMetrics {
    CounterMetric processCpuNanoseconds;
    CounterMetric childCpuNanoseconds;
    CounterMetric peakRssBytes;
    CounterMetric voluntaryContextSwitches;
    CounterMetric involuntaryContextSwitches;
    CounterMetric processTreeChildrenStarted;
    CounterMetric maximumLiveChildren;
};

struct FixtureSegment {
    std::uint32_t segment{ 0u };
    std::vector<Bytes> payloads;
};

struct FixturePlan {
    StreamBinding binding;
    std::vector<FixtureSegment> segments;
};

struct FramedFixture {
    StreamBinding binding;
    std::vector<SyntheticRecord> records;
    Bytes framedBytes;
    std::uint32_t fixtureCrc32{ 0u };
};

FramedFixture makeFramedFixture( const FixturePlan& plan );

struct SyntheticArmPlan {
    BenchmarkArm arm{ BenchmarkArm::Integrated };
    FramedFixture fixture;
    std::vector<std::size_t> fragmentSizes;
    std::filesystem::path captureRoot;
    std::filesystem::path producerExecutable;
};

struct ArmObservation {
    BenchmarkArm arm{ BenchmarkArm::Integrated };
    PipelineTopology topology{};
    StreamBinding binding;
    std::uint32_t fixtureCrc32{ 0u };
    std::size_t committedRecords{ 0u };
    std::size_t committedPayloadBytes{ 0u };
    std::vector<SegmentTransition> segmentTransitions;
    LifecycleTimeline lifecycle;
    std::uint64_t lastCommittedRecordNanoseconds{ 0u };
    QueueMetrics queue;
    RuntimeMetrics runtime;
    bool normalStop{ false };
};

// Both arms must enter bytes through LiveLogController. The implementation must
// then commit through StreamingLogData and its owned CaptureStore; a direct
// decoder-to-file or decoder-to-CaptureStore benchmark bypass is not valid.
ArmObservation runSyntheticArm( const SyntheticArmPlan& plan );

struct AggregateResult {
    std::uint16_t schemaVersion{ 1u };
    BenchmarkArm arm{ BenchmarkArm::Integrated };
    StreamBinding binding;
    std::uint32_t fixtureCrc32{ 0u };
    std::size_t committedRecords{ 0u };
    std::size_t committedPayloadBytes{ 0u };
    std::size_t segmentCount{ 0u };
    LifecycleTimeline lifecycle;
    std::uint64_t lastCommittedRecordNanoseconds{ 0u };
    QueueMetrics queue;
    RuntimeMetrics runtime;
    bool normalStop{ false };
};

AggregateResult makeAggregateResult( const ArmObservation& observation );
std::string serializeAggregateJson( const AggregateResult& result );

enum class RealDeviceArm : std::uint8_t { BaselineProcess, NativeIntegrated };
enum class RealDeviceNativeService : std::uint8_t { Automatic, LegacySyslog, OsTrace };

struct RealDevicePlan {
    RealDeviceArm arm{ RealDeviceArm::NativeIntegrated };
    std::filesystem::path baselineExecutable;
    std::filesystem::path nativeStackRoot;
    std::string deviceIdentifier;
    std::filesystem::path captureRoot;
    std::uint64_t durationMilliseconds{ 0u };
    bool ansiEnabled{ false };
    RealDeviceNativeService nativeService{ RealDeviceNativeService::LegacySyslog };
};

struct RealDeviceObservation {
    RealDeviceArm arm{ RealDeviceArm::NativeIntegrated };
    PipelineTopology topology{};
    LifecycleTimeline lifecycle;
    std::uint64_t lastCommittedRecordNanoseconds{ 0u };
    std::uint64_t bytesReceived{ 0u };
    std::uint64_t linesCommitted{ 0u };
    std::uint64_t ansiEscapeCount{ 0u };
    std::uint64_t errorCount{ 0u };
    QueueMetrics queue;
    RuntimeMetrics runtime;
    bool formatComplete{ false };
    bool normalStop{ false };
    bool cleanupVerified{ false };
};

RealDeviceObservation runRealDeviceArm( const RealDevicePlan& plan );
std::string serializeRealDeviceAggregateJson( const RealDeviceObservation& observation,
                                              std::uint64_t durationMilliseconds,
                                              bool ansiEnabled );

} // namespace klogg::benchmarks::livecapture
