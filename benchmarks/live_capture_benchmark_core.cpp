/*
 * Copyright (C) 2026
 *
 * This file is part of klogg.
 */

#include "live_capture_benchmark_core.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#if defined( __APPLE__ ) || defined( __linux__ ) || defined( __FreeBSD__ )
#include <sys/resource.h>
#include <sys/time.h>
#endif

#include <QByteArray>
#include <QCoreApplication>
#include <QEventLoop>
#include <QFileInfo>
#include <QString>
#include <QTemporaryFile>

#include "adblogcatsessiondata.h"
#include "adblogcatsource.h"
#include "iosnativetransport.h"
#include "livedatastatistics.h"
#include "livelogcontroller.h"
#include "livesourcetransport.h"
#include "streaminglogdata.h"

namespace klogg::benchmarks::livecapture {
namespace {

constexpr auto ArmTimeout = std::chrono::seconds{ 30 };

struct ResourceSnapshot {
    std::uint64_t processCpuNanoseconds{ 0u };
    std::uint64_t childCpuNanoseconds{ 0u };
    std::uint64_t peakRssBytes{ 0u };
    std::uint64_t voluntaryContextSwitches{ 0u };
    std::uint64_t involuntaryContextSwitches{ 0u };
    bool available{ false };
};

#if defined( __APPLE__ ) || defined( __linux__ ) || defined( __FreeBSD__ )
std::uint64_t timevalNanoseconds( const timeval& value )
{
    return static_cast<std::uint64_t>( value.tv_sec ) * 1000000000u
           + static_cast<std::uint64_t>( value.tv_usec ) * 1000u;
}

std::uint64_t peakRssBytes( const rusage& usage )
{
#if defined( __APPLE__ )
    return static_cast<std::uint64_t>( usage.ru_maxrss );
#else
    return static_cast<std::uint64_t>( usage.ru_maxrss ) * 1024u;
#endif
}
#endif

ResourceSnapshot resourceSnapshot()
{
    ResourceSnapshot snapshot;
#if defined( __APPLE__ ) || defined( __linux__ ) || defined( __FreeBSD__ )
    rusage processUsage{};
    rusage childUsage{};
    if ( getrusage( RUSAGE_SELF, &processUsage ) != 0
         || getrusage( RUSAGE_CHILDREN, &childUsage ) != 0 ) {
        return snapshot;
    }
    snapshot.processCpuNanoseconds
        = timevalNanoseconds( processUsage.ru_utime ) + timevalNanoseconds( processUsage.ru_stime );
    snapshot.childCpuNanoseconds
        = timevalNanoseconds( childUsage.ru_utime ) + timevalNanoseconds( childUsage.ru_stime );
    snapshot.peakRssBytes = peakRssBytes( processUsage );
    snapshot.voluntaryContextSwitches
        = static_cast<std::uint64_t>( processUsage.ru_nvcsw + childUsage.ru_nvcsw );
    snapshot.involuntaryContextSwitches
        = static_cast<std::uint64_t>( processUsage.ru_nivcsw + childUsage.ru_nivcsw );
    snapshot.available = true;
#endif
    return snapshot;
}

std::uint64_t saturatingDifference( std::uint64_t after, std::uint64_t before )
{
    return after >= before ? after - before : 0u;
}

std::uint64_t elapsedNanoseconds( std::chrono::steady_clock::time_point started )
{
    const auto elapsed = std::chrono::steady_clock::now() - started;
    const auto count = std::chrono::duration_cast<std::chrono::nanoseconds>( elapsed ).count();
    return count > 0 ? static_cast<std::uint64_t>( count ) : 0u;
}

void appendMilestone( LifecycleTimeline& timeline, LifecycleMilestone milestone,
                      std::chrono::steady_clock::time_point started )
{
    timeline.samples.push_back( { milestone, elapsedNanoseconds( started ) } );
}

QString pathToQString( const std::filesystem::path& path )
{
    const auto utf8 = path.u8string();
    if ( utf8.size() > static_cast<std::size_t>( std::numeric_limits<int>::max() ) ) {
        throw std::length_error( "benchmark path exceeds QString limits" );
    }
    return QString::fromUtf8( utf8.data(), static_cast<int>( utf8.size() ) );
}

QByteArray toQByteArray( const Bytes& bytes )
{
    if ( bytes.size() > static_cast<std::size_t>( std::numeric_limits<int>::max() ) ) {
        throw std::length_error( "benchmark payload exceeds QByteArray limits" );
    }
    return QByteArray( reinterpret_cast<const char*>( bytes.data() ),
                       static_cast<int>( bytes.size() ) );
}

Bytes toBytes( const QByteArray& bytes )
{
    Bytes result;
    result.reserve( static_cast<std::size_t>( bytes.size() ) );
    for ( const auto value : bytes ) {
        result.push_back( static_cast<std::uint8_t>( value ) );
    }
    return result;
}

std::vector<Bytes> segmentStreams( const FramedFixture& fixture )
{
    std::vector<Bytes> streams;
    for ( const auto& record : fixture.records ) {
        if ( streams.size() <= record.segment ) {
            streams.resize( static_cast<std::size_t>( record.segment ) + 1u );
        }
        const auto encoded = encodeFramedRecord( record );
        auto& stream = streams.at( record.segment );
        stream.insert( stream.end(), encoded.cbegin(), encoded.cend() );
    }
    Bytes combined;
    for ( const auto& stream : streams ) {
        combined.insert( combined.end(), stream.cbegin(), stream.cend() );
    }
    if ( combined != fixture.framedBytes ) {
        throw std::invalid_argument( "fixture records and framed bytes are inconsistent" );
    }
    return streams;
}

class BenchmarkClock final : public livelog::LiveLogClock {
public:
    explicit BenchmarkClock( std::chrono::steady_clock::time_point started )
        : started_( started )
    {
    }

    ::klogg::livecapture::Timestamp now() const noexcept override
    {
        return std::chrono::duration_cast<::klogg::livecapture::Timestamp>(
            std::chrono::steady_clock::now() - started_ );
    }

private:
    std::chrono::steady_clock::time_point started_;
};

class BenchmarkScheduler final : public livelog::LiveLogScheduler {
public:
    Token schedule( ::klogg::livecapture::Timestamp, std::function<void()> callback ) override
    {
        const auto token = ++nextToken_;
        callbacks_.push_back( { token, std::move( callback ) } );
        return token;
    }

    void cancel( Token token ) override
    {
        const auto found
            = std::find_if( callbacks_.begin(), callbacks_.end(),
                            [ token ]( const auto& entry ) { return entry.first == token; } );
        if ( found != callbacks_.end() ) {
            callbacks_.erase( found );
        }
    }

private:
    Token nextToken_{ 0u };
    std::vector<std::pair<Token, std::function<void()>>> callbacks_;
};

class FixtureProcessTransport final : public ProcessLiveSourceTransport {
public:
    FixtureProcessTransport( QString producer, QString fixturePath )
        : producer_( std::move( producer ) )
        , fixturePath_( std::move( fixturePath ) )
    {
    }

protected:
    Command streamingCommand() const override
    {
        return { producer_,
                 { QStringLiteral( "--stream-framed-fixture" ), QStringLiteral( "--fixture" ),
                   fixturePath_, QStringLiteral( "--hold-open" ) } };
    }

    Command clearCommand() const override
    {
        return streamingCommand();
    }

    AsyncStartupTiming asyncStartupTiming() const override
    {
        return { 30000, 0 };
    }

private:
    QString producer_;
    QString fixturePath_;
};

struct SyntheticQueueTotals {
    ::klogg::livecapture::LiveDataStatistics statistics;
};

class FixtureNativeSession final : public ::klogg::livecapture::ios::IosNativeStreamSession {
public:
    FixtureNativeSession( ::klogg::livecapture::Generation generation, std::vector<Bytes> fragments,
                          ::klogg::livecapture::ios::IosNativeStreamCallbacks callbacks,
                          std::shared_ptr<SyntheticQueueTotals> totals )
        : generation_( generation )
        , fragments_( std::move( fragments ) )
        , callbacks_( std::move( callbacks ) )
        , totals_( std::move( totals ) )
    {
        statistics_.generation = generation_;
        for ( const auto& fragment : fragments_ ) {
            statistics_.receivedBytes += fragment.size();
            ++statistics_.receivedChunks;
            statistics_.queuedBytes += fragment.size();
            ++statistics_.queuedChunks;
        }
        statistics_.highWaterQueuedBytes = statistics_.queuedBytes;
        statistics_.highWaterQueuedChunks = statistics_.queuedChunks;
    }

    ~FixtureNativeSession() override
    {
        publishTotals();
    }

    bool start() override
    {
        callbacks_.ready( generation_ );
        callbacks_.bytesAvailable( generation_ );
        return true;
    }

    void stop( ::klogg::livecapture::Generation generation ) noexcept override
    {
        publishTotals();
        callbacks_.stopped( generation );
    }

    void shutdown() noexcept override
    {
        publishTotals();
    }

    std::optional<::klogg::livecapture::LiveDataBatch> drain() override
    {
        if ( nextFragment_ == fragments_.size() ) {
            return std::nullopt;
        }
        auto bytes = std::move( fragments_.at( nextFragment_ ) );
        ++nextFragment_;
        statistics_.queuedBytes -= bytes.size();
        --statistics_.queuedChunks;
        statistics_.deliveredBytes += bytes.size();
        ++statistics_.deliveredChunks;
        return ::klogg::livecapture::LiveDataBatch{ generation_, std::move( bytes ), 1u };
    }

    ::klogg::livecapture::LiveDataStatistics statistics() const override
    {
        return statistics_;
    }

private:
    void publishTotals() noexcept
    {
        if ( published_ || !totals_ ) {
            return;
        }
        ::klogg::livecapture::accumulateLiveDataStatistics( totals_->statistics, statistics_ );
        published_ = true;
    }

private:
    ::klogg::livecapture::Generation generation_{ 0u };
    std::vector<Bytes> fragments_;
    ::klogg::livecapture::ios::IosNativeStreamCallbacks callbacks_;
    std::shared_ptr<SyntheticQueueTotals> totals_;
    ::klogg::livecapture::LiveDataStatistics statistics_;
    std::size_t nextFragment_{ 0u };
    bool published_{ false };
};

class BenchmarkTransportFactory final
    : public LiveSourceTransportFactory,
      public ::klogg::livecapture::ios::IosNativeStreamWorkerFactory {
public:
    BenchmarkTransportFactory( BenchmarkArm arm, std::filesystem::path producerExecutable,
                               std::filesystem::path captureRoot, std::vector<Bytes> streams,
                               std::vector<std::size_t> fragmentSizes )
        : arm_( arm )
        , producerExecutable_( std::move( producerExecutable ) )
        , captureRoot_( std::move( captureRoot ) )
        , streams_( std::move( streams ) )
        , fragmentSizes_( std::move( fragmentSizes ) )
        , totals_( std::make_shared<SyntheticQueueTotals>() )
    {
    }

    std::unique_ptr<LiveSourceTransport>
    create( const LiveSourceTransportConfig& config ) const override
    {
        if ( nextSegment_ >= streams_.size() ) {
            return nullptr;
        }
        const auto& stream = streams_.at( nextSegment_++ );
        if ( arm_ == BenchmarkArm::Process ) {
            auto file = std::make_unique<QTemporaryFile>(
                pathToQString( captureRoot_ / "fixture-XXXXXX" ) );
            file->setAutoRemove( true );
            if ( !file->open() ) {
                throw std::runtime_error(
                    "could not open the synthetic process fixture: "
                    + file->errorString().toStdString() );
            }
            const auto payload = toQByteArray( stream );
            const auto expectedBytes = static_cast<qint64>( payload.size() );
            const auto writtenBytes = file->write( payload );
            if ( writtenBytes != expectedBytes ) {
                throw std::runtime_error(
                    "could not write the complete synthetic process fixture: expected "
                    + std::to_string( expectedBytes ) + ", wrote "
                    + std::to_string( writtenBytes ) );
            }
            if ( !file->flush() ) {
                throw std::runtime_error(
                    "could not flush the synthetic process fixture: "
                    + file->errorString().toStdString() );
            }
            const auto fixturePath = file->fileName();
            file->close();
            fixtureFiles_.push_back( std::move( file ) );
            return std::make_unique<FixtureProcessTransport>( pathToQString( producerExecutable_ ),
                                                              fixturePath );
        }

        pendingNativeFragments_ = fragmentStream( stream, fragmentSizes_ );
        ::klogg::livecapture::ios::IosNativeStreamConfig nativeConfig;
        nativeConfig.endpoint = config.iosEndpoint;
        return std::make_unique<::klogg::livecapture::ios::IosNativeTransport>(
            *this, std::move( nativeConfig ) );
    }

    ::klogg::livecapture::ios::IosNativeStreamSessionCreation
    create( const ::klogg::livecapture::ios::IosNativeStreamConfig& config,
            ::klogg::livecapture::ios::IosNativeStreamCallbacks callbacks ) const override
    {
        auto fragments = std::exchange( pendingNativeFragments_, {} );
        return ::klogg::livecapture::ios::IosNativeStreamSessionCreation{
            std::make_unique<FixtureNativeSession>( config.generation, std::move( fragments ),
                                                    std::move( callbacks ), totals_ ),
            std::nullopt
        };
    }

    QueueMetrics queueMetrics() const
    {
        QueueMetrics metrics;
        if ( arm_ == BenchmarkArm::Process ) {
            return metrics;
        }
        const auto& statistics = totals_->statistics;
        metrics.highWaterBytes
            = { static_cast<std::uint64_t>( statistics.highWaterQueuedBytes ), true };
        metrics.highWaterChunks
            = { static_cast<std::uint64_t>( statistics.highWaterQueuedChunks ), true };
        metrics.backpressureEvents
            = { static_cast<std::uint64_t>( statistics.backpressuredChunks ), true };
        metrics.droppedRecords = { std::nullopt, false };
        return metrics;
    }

    std::uint64_t childrenStarted() const noexcept
    {
        return arm_ == BenchmarkArm::Process ? static_cast<std::uint64_t>( nextSegment_ ) : 0u;
    }

    std::uint64_t maximumLiveChildren() const noexcept
    {
        return childrenStarted() == 0u ? 0u : 1u;
    }

    bool removeFixtureFiles()
    {
        bool removed = true;
        for ( auto& file : fixtureFiles_ ) {
            if ( file && QFileInfo::exists( file->fileName() ) && !file->remove() ) {
                removed = false;
            }
        }
        if ( removed ) {
            fixtureFiles_.clear();
        }
        return removed;
    }

private:
    BenchmarkArm arm_{ BenchmarkArm::Integrated };
    std::filesystem::path producerExecutable_;
    std::filesystem::path captureRoot_;
    std::vector<Bytes> streams_;
    std::vector<std::size_t> fragmentSizes_;
    std::shared_ptr<SyntheticQueueTotals> totals_;
    mutable std::size_t nextSegment_{ 0u };
    mutable std::vector<std::unique_ptr<QTemporaryFile>> fixtureFiles_;
    mutable std::vector<Bytes> pendingNativeFragments_;
};

livelog::LiveLogSessionSpec benchmarkSessionSpec( const QString& captureId, BenchmarkArm arm )
{
    livelog::LiveLogSessionSpec spec;
    spec.captureId = captureId;
    spec.device.deviceId = QStringLiteral( "synthetic-benchmark-device" );
    spec.device.displayName = QStringLiteral( "Synthetic Benchmark" );
    spec.runIntent = ::klogg::livecapture::RunIntent::Running;
    spec.capture.autoReconnectEnabled = false;
    if ( arm == BenchmarkArm::Process ) {
        spec.sourceKind = livelog::SourceKind::AndroidLogcat;
        spec.androidBackend = livelog::AndroidBackend::LegacyProcess;
        // Qt 5 + GCC sees a brace-list assignment to QStringList as ambiguous.
        spec.android.buffers = QStringList{ QStringLiteral( "main" ) };
    }
    else {
        spec.sourceKind = livelog::SourceKind::IosSyslog;
        spec.iosBackend = livelog::IosBackend::Native;
        spec.device.connection = livelog::DeviceIdentity::Connection::Usb;
    }
    return spec;
}

AdbLogcatSessionData benchmarkSessionData( const QString& captureId, BenchmarkArm arm )
{
    AdbLogcatSessionData data;
    data.captureId = captureId;
    data.deviceSerial = QStringLiteral( "synthetic-benchmark-device" );
    data.deviceDescription = QStringLiteral( "Synthetic Benchmark" );
    data.runIntent = ::klogg::livecapture::RunIntent::Running;
    if ( arm == BenchmarkArm::Process ) {
        data.sourceType = LiveLogSourceType::AdbLogcat;
        data.adbBackend = AdbTransportBackend::Process;
    }
    else {
        data.sourceType = LiveLogSourceType::IosLogStream;
        data.iosBackend = IosTransportBackend::Native;
    }
    return data;
}

livelog::LiveLogControllerConfig benchmarkControllerConfig()
{
    livelog::LiveLogControllerConfig config;
    config.reducer.maxRetryAttempts = 0u;
    return config;
}

class BenchmarkPipelineEffects final : public livelog::LiveLogControllerEffects {
public:
    BenchmarkPipelineEffects( const SyntheticArmPlan& plan, ArmObservation& observation,
                              std::chrono::steady_clock::time_point started,
                              BenchmarkTransportFactory& factory )
        : plan_( plan )
        , observation_( observation )
        , started_( started )
        , decoder_( plan.fixture.binding )
        , logData_( std::make_shared<StreamingLogData>(
              QStringLiteral( "benchmark-%1-%2" )
                  .arg( plan.arm == BenchmarkArm::Process ? QStringLiteral( "process" )
                                                          : QStringLiteral( "integrated" ) )
                  .arg( plan.fixture.fixtureCrc32 ),
              pathToQString( plan.captureRoot ) ) )
        , source_( std::make_unique<AdbLogcatSource>(
              benchmarkSessionData( logData_->captureId(), plan.arm ), logData_, factory ) )
    {
    }

    ~BenchmarkPipelineEffects() override
    {
        detach();
        cleanup();
    }

    void attach( livelog::LiveLogController& controller )
    {
        controller_ = &controller;
        source_->setControllerCallbacks(
            [ this ]( auto generation, const QByteArray& bytes ) {
                acceptTransportBytes( generation, bytes );
            },
            [ this ]( auto generation, LiveSourceTransport::State state ) {
                transportStateChanged( generation, state );
            },
            [ this ]( auto, ::klogg::livecapture::LiveSourceError error ) {
                failure_ = std::move( error );
            } );
    }

    void detach()
    {
        if ( source_ ) {
            source_->setControllerCallbacks( {}, {}, {} );
        }
        controller_ = nullptr;
    }

    void invalidateGeneration( ::klogg::livecapture::Generation generation ) override
    {
        source_->invalidateTransportGeneration( generation );
    }

    void cancelStream( ::klogg::livecapture::Generation generation ) override
    {
        source_->cancelTransport( generation );
        if ( controller_ != nullptr
             && controller_->snapshot().source.status
                    == ::klogg::livecapture::SourceStatus::Stopping ) {
            controller_->stopCompleted( generation );
        }
    }

    void startInfrastructure( ::klogg::livecapture::Generation generation ) override
    {
        static_cast<void>( generation );
        controller_->infrastructureChanged(
            ::klogg::livecapture::InfrastructureStatus::Ready,
            ::klogg::livecapture::InfrastructureOwnership::AppShared );
        controller_->deviceAvailable( controller_->snapshot().generation );
    }

    void openStream( ::klogg::livecapture::Generation generation,
                     const LiveSourceTransportConfig& config ) override
    {
        source_->openTransport( generation, config );
    }

    void appendBytes( ::klogg::livecapture::Generation generation,
                      const QByteArray& bytes ) override
    {
        source_->appendTransportBytes( generation, bytes );
    }

    bool failed() const noexcept
    {
        return failure_.has_value() || decodeFailure_.has_value();
    }

    std::string diagnostic() const
    {
        if ( failure_ ) {
            return failure_->code + ": " + failure_->nativeDetail;
        }
        if ( decodeFailure_ ) {
            return "framed decode failure kind "
                   + std::to_string( static_cast<unsigned>( decodeFailure_->kind ) );
        }
        return "timeout waiting for transport progress";
    }

    std::size_t committedRecords() const noexcept
    {
        return observation_.committedRecords;
    }

    void finishDecoder()
    {
        auto final = decoder_.finish();
        if ( final.failure ) {
            decodeFailure_ = final.failure;
        }
        acceptDecoded( controller_ != nullptr ? controller_->snapshot().generation : 0u,
                       std::move( final ) );
        if ( receivedFramedBytes_ != plan_.fixture.framedBytes ) {
            decodeFailure_
                = DecodeFailure{ DecodeFailureKind::MalformedFrame, 0u,
                                 plan_.fixture.framedBytes.size(), receivedFramedBytes_.size() };
        }
    }

    void cleanup()
    {
        if ( cleaned_ || !source_ ) {
            return;
        }
        source_->deleteCaptureFiles();
        cleaned_ = true;
    }

private:
    void transportStateChanged( ::klogg::livecapture::Generation generation,
                                LiveSourceTransport::State state )
    {
        if ( controller_ == nullptr ) {
            return;
        }
        switch ( state ) {
        case LiveSourceTransport::State::Connected:
            controller_->protocolServiceReady( generation );
            controller_->streamHandleOpened( generation );
            controller_->streamReadArmed( generation );
            if ( !readyRecorded_ ) {
                appendMilestone( observation_.lifecycle, LifecycleMilestone::Ready, started_ );
                readyRecorded_ = true;
            }
            if ( !pendingTransportBytes_.empty() ) {
                auto pending = std::exchange( pendingTransportBytes_, {} );
                decodeTransportFragment( generation, std::move( pending ) );
            }
            break;
        case LiveSourceTransport::State::Error: {
            const auto detail = source_->lastError().isEmpty()
                                    ? std::string{ "The production transport reported Error." }
                                    : source_->lastError().toStdString();
            failure_ = ::klogg::livecapture::LiveSourceError{
                ::klogg::livecapture::ErrorCategory::Stream,
                "synthetic-transport-error",
                ::klogg::livecapture::ErrorScope::Stream,
                ::klogg::livecapture::RetryPolicy::Never,
                "The synthetic benchmark transport failed.",
                detail
            };
            break;
        }
        case LiveSourceTransport::State::Disconnected:
        case LiveSourceTransport::State::Connecting:
            break;
        }
    }

    void acceptTransportBytes( ::klogg::livecapture::Generation generation,
                               const QByteArray& bytes )
    {
        if ( bytes.isEmpty() || decodeFailure_ ) {
            return;
        }
        auto fragment = toBytes( bytes );
        receivedFramedBytes_.insert( receivedFramedBytes_.end(), fragment.cbegin(),
                                     fragment.cend() );
        if ( !readyRecorded_ ) {
            pendingTransportBytes_.insert( pendingTransportBytes_.end(), fragment.cbegin(),
                                           fragment.cend() );
            return;
        }
        decodeTransportFragment( generation, std::move( fragment ) );
    }

    void decodeTransportFragment( ::klogg::livecapture::Generation generation, Bytes fragment )
    {
        if ( !firstByteRecorded_ ) {
            appendMilestone( observation_.lifecycle, LifecycleMilestone::FirstByte, started_ );
            firstByteRecorded_ = true;
        }
        acceptDecoded( generation, decoder_.push( fragment ) );
    }

    void acceptDecoded( ::klogg::livecapture::Generation generation, DecodeReport report )
    {
        if ( report.failure ) {
            decodeFailure_ = report.failure;
            return;
        }
        observation_.segmentTransitions.insert( observation_.segmentTransitions.end(),
                                                report.segmentTransitions.cbegin(),
                                                report.segmentTransitions.cend() );
        for ( const auto& record : report.records ) {
            auto committed = record.payload;
            committed.push_back( static_cast<std::uint8_t>( '\n' ) );
            const auto linesBefore = logData_->getNbLine();
            controller_->streamBytesReceived( generation, toQByteArray( committed ) );
            if ( logData_->getNbLine() <= linesBefore ) {
                decodeFailure_
                    = DecodeFailure{ DecodeFailureKind::MalformedFrame, record.segment, 1u, 0u };
                return;
            }
            ++observation_.committedRecords;
            observation_.committedPayloadBytes += record.payload.size();
            observation_.lastCommittedRecordNanoseconds = elapsedNanoseconds( started_ );
            if ( observation_.committedRecords == 1u ) {
                appendMilestone( observation_.lifecycle, LifecycleMilestone::FirstCommittedRecord,
                                 started_ );
            }
        }
    }

private:
    const SyntheticArmPlan& plan_;
    ArmObservation& observation_;
    std::chrono::steady_clock::time_point started_;
    FramedStreamDecoder decoder_;
    std::shared_ptr<StreamingLogData> logData_;
    std::unique_ptr<AdbLogcatSource> source_;
    livelog::LiveLogController* controller_{ nullptr };
    Bytes receivedFramedBytes_;
    Bytes pendingTransportBytes_;
    std::optional<DecodeFailure> decodeFailure_;
    std::optional<::klogg::livecapture::LiveSourceError> failure_;
    bool readyRecorded_{ false };
    bool firstByteRecorded_{ false };
    bool cleaned_{ false };
};

bool pumpEventsUntil( const std::function<bool()>& predicate )
{
    const auto deadline = std::chrono::steady_clock::now() + ArmTimeout;
    while ( !predicate() && std::chrono::steady_clock::now() < deadline ) {
        QCoreApplication::processEvents( QEventLoop::AllEvents, 10 );
    }
    return predicate();
}

std::vector<std::size_t> cumulativeSegmentRecordCounts( const FramedFixture& fixture )
{
    std::vector<std::size_t> counts;
    std::size_t cumulative = 0u;
    std::optional<std::uint32_t> segment;
    for ( const auto& record : fixture.records ) {
        if ( segment && record.segment != *segment ) {
            counts.push_back( cumulative );
        }
        segment = record.segment;
        ++cumulative;
    }
    counts.push_back( cumulative );
    return counts;
}

} // namespace

ArmObservation runSyntheticArm( const SyntheticArmPlan& plan )
{
    if ( QCoreApplication::instance() == nullptr ) {
        throw std::logic_error( "synthetic arm requires QCoreApplication" );
    }
    if ( plan.fixture.records.empty() || plan.captureRoot.empty() ) {
        throw std::invalid_argument(
            "synthetic arm requires a non-empty fixture and capture root" );
    }
    if ( plan.fragmentSizes.empty()
         || std::any_of( plan.fragmentSizes.cbegin(), plan.fragmentSizes.cend(),
                         []( std::size_t size ) { return size == 0u; } ) ) {
        throw std::invalid_argument( "synthetic arm requires positive fragment sizes" );
    }
    if ( plan.arm == BenchmarkArm::Process ) {
        if ( plan.producerExecutable.empty() || !plan.producerExecutable.is_absolute() ) {
            throw std::invalid_argument(
                "process arm requires an absolute fixture producer executable path" );
        }
        std::error_code error;
        if ( !std::filesystem::is_regular_file( plan.producerExecutable, error ) || error ) {
            throw std::invalid_argument( "fixture producer executable path is not a regular file" );
        }
    }

    const auto resourcesBefore = resourceSnapshot();
    const auto started = std::chrono::steady_clock::now();
    ArmObservation observation;
    observation.arm = plan.arm;
    observation.topology = RequiredPipelineTopology;
    observation.binding = plan.fixture.binding;
    observation.fixtureCrc32 = plan.fixture.fixtureCrc32;
    appendMilestone( observation.lifecycle, LifecycleMilestone::Start, started );

    BenchmarkTransportFactory factory( plan.arm, plan.producerExecutable, plan.captureRoot,
                                       segmentStreams( plan.fixture ), plan.fragmentSizes );
    BenchmarkClock clock( started );
    BenchmarkScheduler scheduler;
    std::exception_ptr armFailure;
    try {
        BenchmarkPipelineEffects effects( plan, observation, started, factory );
        livelog::LiveLogController controller(
            benchmarkSessionSpec( QStringLiteral( "benchmark-%1-%2" )
                                      .arg( plan.arm == BenchmarkArm::Process
                                                ? QStringLiteral( "process" )
                                                : QStringLiteral( "integrated" ) )
                                      .arg( plan.fixture.fixtureCrc32 ),
                                  plan.arm ),
            benchmarkControllerConfig(), clock, scheduler, effects );
        effects.attach( controller );
        controller.armRunIntent();

        const auto segmentCounts = cumulativeSegmentRecordCounts( plan.fixture );
        for ( std::size_t segment = 0u; segment < segmentCounts.size(); ++segment ) {
            if ( !pumpEventsUntil( [ &effects, expected = segmentCounts.at( segment ) ] {
                     return effects.failed() || effects.committedRecords() >= expected;
                 } )
                 || effects.failed() ) {
                throw std::runtime_error( "synthetic transport did not commit its fixture segment: "
                                          + effects.diagnostic() + "; controller status "
                                          + std::to_string( static_cast<unsigned>(
                                              controller.snapshot().source.status ) ) );
            }
            if ( segment + 1u < segmentCounts.size() ) {
                controller.reconnectRequested();
                controller.deviceAvailable( controller.snapshot().generation );
            }
        }

        effects.finishDecoder();
        if ( effects.failed() || observation.committedRecords != plan.fixture.records.size() ) {
            throw std::runtime_error( "synthetic fixture failed framed-record validation" );
        }

        controller.stopRequested();
        if ( !pumpEventsUntil( [ &controller ] {
                 return controller.snapshot().source.status
                        == ::klogg::livecapture::SourceStatus::Stopped;
             } ) ) {
            throw std::runtime_error( "synthetic transport did not stop" );
        }
        observation.normalStop = true;
        effects.detach();
        effects.cleanup();
    } catch ( ... ) {
        armFailure = std::current_exception();
    }

    observation.queue = factory.queueMetrics();
    if ( !pumpEventsUntil( [ &factory, &plan ] {
             std::error_code error;
             const auto empty = std::filesystem::is_empty( plan.captureRoot, error );
             return factory.removeFixtureFiles() && !error && empty;
         } ) ) {
        throw std::runtime_error( "CaptureStore cleanup did not empty the benchmark root" );
    }
    const auto resourcesAfter = resourceSnapshot();
    if ( resourcesBefore.available && resourcesAfter.available ) {
        observation.runtime.processCpuNanoseconds
            = { saturatingDifference( resourcesAfter.processCpuNanoseconds,
                                      resourcesBefore.processCpuNanoseconds ),
                false };
        observation.runtime.childCpuNanoseconds
            = { saturatingDifference( resourcesAfter.childCpuNanoseconds,
                                      resourcesBefore.childCpuNanoseconds ),
                false };
        observation.runtime.peakRssBytes = { resourcesAfter.peakRssBytes, false };
        observation.runtime.voluntaryContextSwitches
            = { saturatingDifference( resourcesAfter.voluntaryContextSwitches,
                                      resourcesBefore.voluntaryContextSwitches ),
                false };
        observation.runtime.involuntaryContextSwitches
            = { saturatingDifference( resourcesAfter.involuntaryContextSwitches,
                                      resourcesBefore.involuntaryContextSwitches ),
                false };
    }
    observation.runtime.processTreeChildrenStarted = { factory.childrenStarted(), false };
    observation.runtime.maximumLiveChildren = { factory.maximumLiveChildren(), false };
    if ( armFailure ) {
        std::rethrow_exception( armFailure );
    }
    appendMilestone( observation.lifecycle, LifecycleMilestone::Stop, started );
    if ( validateLifecycle( observation.lifecycle ).failure ) {
        throw std::runtime_error( "synthetic arm lifecycle milestones are invalid" );
    }
    return observation;
}

} // namespace klogg::benchmarks::livecapture
