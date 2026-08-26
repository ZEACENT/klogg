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
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

#if defined( __APPLE__ ) || defined( __linux__ ) || defined( __FreeBSD__ )
#include <sys/resource.h>
#include <sys/time.h>
#endif

#include <QByteArray>
#include <QCoreApplication>
#include <QEventLoop>
#include <QString>

#include "adblogcatsessiondata.h"
#include "adblogcatsource.h"
#include "ioslogprocesstransport.h"
#include "iosnativeadapter.h"
#include "iosnativestream.h"
#include "iosnativetransport.h"
#include "livelogcontroller.h"
#include "livesourcetransport.h"
#include "streaminglogdata.h"

namespace klogg::benchmarks::livecapture {
namespace {

constexpr auto StartupTimeout = std::chrono::seconds{ 30 };
constexpr std::size_t MaximumPreReadyBytes = 4u * 1024u * 1024u;

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

std::uint64_t rssBytes( const rusage& usage )
{
#if defined( __APPLE__ )
    return static_cast<std::uint64_t>( usage.ru_maxrss );
#else
    return static_cast<std::uint64_t>( usage.ru_maxrss ) * 1024u;
#endif
}
#endif

ResourceSnapshot resources()
{
    ResourceSnapshot snapshot;
#if defined( __APPLE__ ) || defined( __linux__ ) || defined( __FreeBSD__ )
    rusage self{};
    rusage children{};
    if ( getrusage( RUSAGE_SELF, &self ) != 0 || getrusage( RUSAGE_CHILDREN, &children ) != 0 ) {
        return snapshot;
    }
    snapshot.processCpuNanoseconds
        = timevalNanoseconds( self.ru_utime ) + timevalNanoseconds( self.ru_stime );
    snapshot.childCpuNanoseconds
        = timevalNanoseconds( children.ru_utime ) + timevalNanoseconds( children.ru_stime );
    snapshot.peakRssBytes = rssBytes( self );
    snapshot.voluntaryContextSwitches
        = static_cast<std::uint64_t>( self.ru_nvcsw + children.ru_nvcsw );
    snapshot.involuntaryContextSwitches
        = static_cast<std::uint64_t>( self.ru_nivcsw + children.ru_nivcsw );
    snapshot.available = true;
#endif
    return snapshot;
}

std::uint64_t difference( std::uint64_t after, std::uint64_t before )
{
    return after >= before ? after - before : 0u;
}

std::uint64_t elapsedNanoseconds( std::chrono::steady_clock::time_point started )
{
    const auto elapsed = std::chrono::steady_clock::now() - started;
    const auto count = std::chrono::duration_cast<std::chrono::nanoseconds>( elapsed ).count();
    return count > 0 ? static_cast<std::uint64_t>( count ) : 0u;
}

QString toQString( const std::filesystem::path& path )
{
    const auto utf8 = path.u8string();
    if ( utf8.size() > static_cast<std::size_t>( std::numeric_limits<int>::max() ) ) {
        throw std::length_error( "real-device benchmark path exceeds QString limits" );
    }
    return QString::fromUtf8( utf8.data(), static_cast<int>( utf8.size() ) );
}

QString toQString( const std::string& text )
{
    if ( text.size() > static_cast<std::size_t>( std::numeric_limits<int>::max() ) ) {
        throw std::length_error( "real-device benchmark identifier exceeds QString limits" );
    }
    return QString::fromUtf8( text.data(), static_cast<int>( text.size() ) );
}

class RealClock final : public livelog::LiveLogClock {
public:
    explicit RealClock( std::chrono::steady_clock::time_point started )
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

class RealScheduler final : public livelog::LiveLogScheduler {
public:
    Token schedule( ::klogg::livecapture::Timestamp, std::function<void()> callback ) override
    {
        const auto token = ++next_;
        callbacks_.push_back( { token, std::move( callback ) } );
        return token;
    }

    void cancel( Token token ) override
    {
        const auto found
            = std::find_if( callbacks_.begin(), callbacks_.end(),
                            [ token ]( const auto& item ) { return item.first == token; } );
        if ( found != callbacks_.end() ) {
            callbacks_.erase( found );
        }
    }

private:
    Token next_{ 0u };
    std::vector<std::pair<Token, std::function<void()>>> callbacks_;
};

class RealTransportFactory final : public LiveSourceTransportFactory {
public:
    explicit RealTransportFactory( const RealDevicePlan& plan )
        : plan_( plan )
    {
        if ( plan_.arm == RealDeviceArm::NativeIntegrated ) {
            std::string error;
            const auto api = ::klogg::livecapture::ios::loadIosNativeApiFromBundle(
                plan_.nativeStackRoot.string(), &error );
            if ( api.getDeviceListExtended == nullptr ) {
                throw std::runtime_error( "packaged native API load failed: " + error );
            }
            nativeWorkerFactory_
                = std::make_unique<::klogg::livecapture::ios::DefaultIosNativeStreamWorkerFactory>(
                    api );
        }
    }

    std::unique_ptr<LiveSourceTransport>
    create( const LiveSourceTransportConfig& config ) const override
    {
        ++transportsCreated_;
        if ( plan_.arm == RealDeviceArm::BaselineProcess ) {
            auto transport = std::make_unique<IosLogProcessTransport>(
                toQString( plan_.baselineExecutable ), toQString( plan_.deviceIdentifier ),
                QString{}, plan_.ansiEnabled );
            latest_ = transport.get();
            return transport;
        }

        const auto native = livelog::makeIosNativeStreamConfig( config );
        if ( !native || nativeWorkerFactory_ == nullptr ) {
            return nullptr;
        }
        auto nativeConfig = *native;
        nativeConfig.endpoint.udid = plan_.deviceIdentifier;
        nativeConfig.endpoint.connectionType = ::klogg::livecapture::ios::NativeConnectionType::Usb;
        switch ( plan_.nativeService ) {
        case RealDeviceNativeService::Automatic:
            nativeConfig.servicePolicy
                = ::klogg::livecapture::ios::IosNativeServicePolicy::AutomaticByProductVersion;
            break;
        case RealDeviceNativeService::LegacySyslog:
            nativeConfig.servicePolicy
                = ::klogg::livecapture::ios::IosNativeServicePolicy::LegacySyslog;
            break;
        case RealDeviceNativeService::OsTrace:
            nativeConfig.servicePolicy = ::klogg::livecapture::ios::IosNativeServicePolicy::OsTrace;
            break;
        }
        auto transport = std::make_unique<::klogg::livecapture::ios::IosNativeTransport>(
            *nativeWorkerFactory_, std::move( nativeConfig ) );
        latest_ = transport.get();
        return transport;
    }

    QueueMetrics queueMetrics() const
    {
        QueueMetrics metrics;
        if ( plan_.arm == RealDeviceArm::BaselineProcess || latest_ == nullptr ) {
            return metrics;
        }
        const auto statistics = latest_->statistics();
        metrics.highWaterBytes
            = { static_cast<std::uint64_t>( statistics.highWaterQueuedBytes ), false };
        metrics.highWaterChunks
            = { static_cast<std::uint64_t>( statistics.highWaterQueuedChunks ), false };
        metrics.backpressureEvents
            = { static_cast<std::uint64_t>( statistics.backpressuredChunks ), false };
        metrics.droppedRecords = { std::nullopt, false };
        return metrics;
    }

    std::uint64_t childrenStarted() const noexcept
    {
        return plan_.arm == RealDeviceArm::BaselineProcess
                   ? static_cast<std::uint64_t>( transportsCreated_ )
                   : 0u;
    }

private:
    const RealDevicePlan& plan_;
    std::unique_ptr<::klogg::livecapture::ios::DefaultIosNativeStreamWorkerFactory>
        nativeWorkerFactory_;
    mutable LiveSourceTransport* latest_{ nullptr };
    mutable std::size_t transportsCreated_{ 0u };
};

livelog::LiveLogSessionSpec realSpec( const RealDevicePlan& plan, const QString& captureId )
{
    livelog::LiveLogSessionSpec spec;
    spec.captureId = captureId;
    spec.sourceKind = livelog::SourceKind::IosSyslog;
    spec.iosBackend = plan.arm == RealDeviceArm::NativeIntegrated
                          ? livelog::IosBackend::Native
                          : livelog::IosBackend::LegacyProcess;
    spec.device.deviceId = toQString( plan.deviceIdentifier );
    spec.device.displayName = QStringLiteral( "Acceptance endpoint" );
    spec.device.connection = livelog::DeviceIdentity::Connection::Usb;
    spec.runIntent = ::klogg::livecapture::RunIntent::Running;
    spec.capture.ansiOutputEnabled = plan.ansiEnabled;
    spec.capture.autoReconnectEnabled = false;
    return spec;
}

AdbLogcatSessionData realSessionData( const RealDevicePlan& plan, const QString& captureId )
{
    AdbLogcatSessionData data;
    data.captureId = captureId;
    data.deviceSerial = toQString( plan.deviceIdentifier );
    data.deviceDescription = QStringLiteral( "Acceptance endpoint" );
    data.sourceType = LiveLogSourceType::IosLogStream;
    data.iosBackend = plan.arm == RealDeviceArm::NativeIntegrated
                          ? IosTransportBackend::Native
                          : IosTransportBackend::LegacyProcess;
    data.iosEndpoint.udid = plan.deviceIdentifier;
    data.iosEndpoint.connectionType = ::klogg::livecapture::ios::NativeConnectionType::Usb;
    data.ansiOutputEnabled = plan.ansiEnabled;
    data.runIntent = ::klogg::livecapture::RunIntent::Running;
    return data;
}

livelog::LiveLogControllerConfig realControllerConfig()
{
    livelog::LiveLogControllerConfig config;
    config.reducer.maxRetryAttempts = 0u;
    return config;
}

class RealEffects final : public livelog::LiveLogControllerEffects {
public:
    RealEffects( const RealDevicePlan& plan, RealDeviceObservation& observation,
                 std::chrono::steady_clock::time_point started, RealTransportFactory& factory )
        : observation_( observation )
        , started_( started )
        , logData_( std::make_shared<StreamingLogData>(
              QStringLiteral( "real-acceptance-%1-%2" )
                  .arg( plan.arm == RealDeviceArm::NativeIntegrated ? QStringLiteral( "native" )
                                                                    : QStringLiteral( "baseline" ) )
                  .arg( elapsedNanoseconds( started ) ),
              toQString( plan.captureRoot ) ) )
        , source_( std::make_unique<AdbLogcatSource>(
              realSessionData( plan, logData_->captureId() ), logData_, factory ) )
    {
    }

    ~RealEffects() override
    {
        detach();
        cleanup();
    }

    void attach( livelog::LiveLogController& controller )
    {
        controller_ = &controller;
        source_->setControllerCallbacks(
            [ this ]( auto generation, const QByteArray& bytes ) { receive( generation, bytes ); },
            [ this ]( auto generation, LiveSourceTransport::State state ) {
                stateChanged( generation, state );
            },
            [ this ]( auto, ::klogg::livecapture::LiveSourceError error ) {
                ++observation_.errorCount;
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
        refreshCommittedLines();
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

    bool ready() const noexcept
    {
        return ready_;
    }

    bool failed() const noexcept
    {
        return failure_.has_value();
    }

    std::string diagnostic() const
    {
        return failure_ ? failure_->code + ": " + failure_->nativeDetail
                        : std::string{ "timeout waiting for live data" };
    }

    void cleanup()
    {
        if ( cleaned_ || !source_ ) {
            return;
        }
        source_->deleteCaptureFiles();
        cleaned_ = true;
    }

    void finalizeFormat()
    {
        refreshCommittedLines();
        observation_.formatComplete
            = observation_.bytesReceived > 0u && observation_.linesCommitted > 0u && !nulObserved_;
    }

private:
    void stateChanged( ::klogg::livecapture::Generation generation,
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
            if ( !ready_ ) {
                observation_.lifecycle.samples.push_back(
                    { LifecycleMilestone::Ready, elapsedNanoseconds( started_ ) } );
                ready_ = true;
            }
            if ( !pending_.isEmpty() ) {
                auto pending = std::exchange( pending_, {} );
                receiveReadyBytes( generation, pending );
            }
            break;
        case LiveSourceTransport::State::Error:
            ++observation_.errorCount;
            failure_ = ::klogg::livecapture::LiveSourceError{
                ::klogg::livecapture::ErrorCategory::Stream,
                "real-transport-error",
                ::klogg::livecapture::ErrorScope::Stream,
                ::klogg::livecapture::RetryPolicy::Never,
                "The acceptance transport failed.",
                source_->lastError().toStdString()
            };
            break;
        case LiveSourceTransport::State::Disconnected:
            if ( controller_->snapshot().runIntent == ::klogg::livecapture::RunIntent::Running ) {
                ++observation_.errorCount;
                failure_ = ::klogg::livecapture::LiveSourceError{
                    ::klogg::livecapture::ErrorCategory::Stream,
                    "unexpected-disconnect",
                    ::klogg::livecapture::ErrorScope::Stream,
                    ::klogg::livecapture::RetryPolicy::Never,
                    "The acceptance stream disconnected.",
                    "Disconnected before bounded stop."
                };
            }
            break;
        case LiveSourceTransport::State::Connecting:
            break;
        }
    }

    void receive( ::klogg::livecapture::Generation generation, const QByteArray& bytes )
    {
        if ( bytes.isEmpty() || failure_ ) {
            return;
        }
        if ( !ready_ ) {
            if ( pending_.size() > static_cast<qint64>( MaximumPreReadyBytes ) - bytes.size() ) {
                ++observation_.errorCount;
                failure_ = ::klogg::livecapture::LiveSourceError{
                    ::klogg::livecapture::ErrorCategory::Stream,
                    "pre-ready-buffer-overflow",
                    ::klogg::livecapture::ErrorScope::Stream,
                    ::klogg::livecapture::RetryPolicy::Never,
                    "The acceptance stream exceeded its pre-ready bound.",
                    "More than four MiB arrived before Connected."
                };
                return;
            }
            pending_.append( bytes );
            return;
        }
        receiveReadyBytes( generation, bytes );
    }

    void receiveReadyBytes( ::klogg::livecapture::Generation generation, const QByteArray& bytes )
    {
        if ( !firstByte_ ) {
            observation_.lifecycle.samples.push_back(
                { LifecycleMilestone::FirstByte, elapsedNanoseconds( started_ ) } );
            firstByte_ = true;
        }
        observation_.bytesReceived += static_cast<std::uint64_t>( bytes.size() );
        for ( const auto value : bytes ) {
            if ( value == '\0' ) {
                nulObserved_ = true;
            }
            if ( static_cast<unsigned char>( value ) == 0x1bu ) {
                ++observation_.ansiEscapeCount;
            }
        }
        const auto before = logData_->getNbLine();
        controller_->streamBytesReceived( generation, bytes );
        const auto after = logData_->getNbLine();
        if ( after > before ) {
            const auto committedAt = elapsedNanoseconds( started_ );
            if ( observation_.lifecycle.samples.size() == 3u ) {
                observation_.lifecycle.samples.push_back(
                    { LifecycleMilestone::FirstCommittedRecord, committedAt } );
            }
            observation_.lastCommittedRecordNanoseconds = committedAt;
            observation_.linesCommitted = after.get<std::uint64_t>();
        }
    }

    void refreshCommittedLines()
    {
        const auto count = logData_->getNbLine().get<std::uint64_t>();
        if ( count > observation_.linesCommitted ) {
            observation_.linesCommitted = count;
            observation_.lastCommittedRecordNanoseconds = elapsedNanoseconds( started_ );
            if ( observation_.lifecycle.samples.size() == 3u ) {
                observation_.lifecycle.samples.push_back(
                    { LifecycleMilestone::FirstCommittedRecord,
                      observation_.lastCommittedRecordNanoseconds } );
            }
        }
    }

private:
    RealDeviceObservation& observation_;
    std::chrono::steady_clock::time_point started_;
    std::shared_ptr<StreamingLogData> logData_;
    std::unique_ptr<AdbLogcatSource> source_;
    livelog::LiveLogController* controller_{ nullptr };
    std::optional<::klogg::livecapture::LiveSourceError> failure_;
    QByteArray pending_;
    bool ready_{ false };
    bool firstByte_{ false };
    bool nulObserved_{ false };
    bool cleaned_{ false };
};

bool pumpUntil( const std::function<bool()>& predicate,
                std::chrono::steady_clock::time_point deadline )
{
    while ( !predicate() && std::chrono::steady_clock::now() < deadline ) {
        QCoreApplication::processEvents( QEventLoop::AllEvents, 10 );
    }
    return predicate();
}

void appendMetric( std::ostringstream& json, const char* name, const CounterMetric& metric )
{
    json << ",\"" << name << "_available\":" << ( metric.value ? 1 : 0 ) << ",\"" << name
         << "_synthetic\":" << ( metric.synthetic ? 1 : 0 );
    if ( metric.value ) {
        json << ",\"" << name << "\":" << *metric.value;
    }
}

std::uint64_t milestone( const LifecycleTimeline& timeline, LifecycleMilestone value )
{
    const auto found = std::find_if(
        timeline.samples.cbegin(), timeline.samples.cend(),
        [ value ]( const MilestoneSample& sample ) { return sample.milestone == value; } );
    if ( found == timeline.samples.cend() ) {
        throw std::invalid_argument( "real-device lifecycle is incomplete" );
    }
    return found->monotonicNanoseconds;
}

} // namespace

RealDeviceObservation runRealDeviceArm( const RealDevicePlan& plan )
{
    if ( QCoreApplication::instance() == nullptr ) {
        throw std::logic_error( "real-device benchmark requires QCoreApplication" );
    }
    if ( plan.deviceIdentifier.empty() || plan.captureRoot.empty()
         || plan.durationMilliseconds == 0u ) {
        throw std::invalid_argument( "real-device benchmark plan is incomplete" );
    }
    if ( plan.arm == RealDeviceArm::BaselineProcess ) {
        std::error_code error;
        const auto canonical = std::filesystem::canonical( plan.baselineExecutable, error );
        if ( error || !plan.baselineExecutable.is_absolute() || canonical != plan.baselineExecutable
             || !std::filesystem::is_regular_file( canonical, error ) || error ) {
            throw std::invalid_argument(
                "baseline executable must be a canonical absolute regular file" );
        }
    }
    if ( plan.arm == RealDeviceArm::NativeIntegrated ) {
        if ( !plan.nativeStackRoot.is_absolute()
             || !std::filesystem::is_directory( plan.nativeStackRoot ) ) {
            throw std::invalid_argument( "native stack root must be an absolute directory" );
        }
    }

    const auto before = resources();
    const auto started = std::chrono::steady_clock::now();
    RealDeviceObservation observation;
    observation.arm = plan.arm;
    observation.topology = RequiredPipelineTopology;
    observation.lifecycle.samples.push_back( { LifecycleMilestone::Start, 0u } );
    RealTransportFactory factory( plan );
    RealClock clock( started );
    RealScheduler scheduler;
    std::exception_ptr failure;
    try {
        RealEffects effects( plan, observation, started, factory );
        livelog::LiveLogController controller(
            realSpec( plan, QStringLiteral( "real-acceptance" ) ), realControllerConfig(), clock,
            scheduler, effects );
        effects.attach( controller );
        controller.armRunIntent();
        if ( !pumpUntil( [ &effects ] { return effects.ready() || effects.failed(); },
                         std::chrono::steady_clock::now() + StartupTimeout )
             || effects.failed() ) {
            throw std::runtime_error( "real-device startup failed: " + effects.diagnostic() );
        }

        const auto captureDeadline = std::chrono::steady_clock::now()
                                     + std::chrono::milliseconds{ plan.durationMilliseconds };
        if ( !pumpUntil(
                 [ &effects, captureDeadline ] {
                     return effects.failed() || std::chrono::steady_clock::now() >= captureDeadline;
                 },
                 captureDeadline )
             || effects.failed() ) {
            throw std::runtime_error( "real-device capture failed: " + effects.diagnostic() );
        }

        observation.queue = factory.queueMetrics();
        controller.stopRequested();
        if ( !pumpUntil(
                 [ &controller ] {
                     return controller.snapshot().source.status
                            == ::klogg::livecapture::SourceStatus::Stopped;
                 },
                 std::chrono::steady_clock::now() + StartupTimeout ) ) {
            throw std::runtime_error( "real-device transport did not stop" );
        }
        effects.finalizeFormat();
        if ( effects.failed() || !observation.formatComplete || observation.errorCount != 0u ) {
            throw std::runtime_error( "real-device format or error gate failed" );
        }
        observation.normalStop = true;
        effects.detach();
        effects.cleanup();
    } catch ( ... ) {
        failure = std::current_exception();
    }

    if ( !pumpUntil(
             [ &plan ] {
                 std::error_code error;
                 return std::filesystem::is_empty( plan.captureRoot, error ) && !error;
             },
             std::chrono::steady_clock::now() + StartupTimeout ) ) {
        throw std::runtime_error( "real-device CaptureStore cleanup failed" );
    }
    observation.cleanupVerified = true;
    const auto after = resources();
    if ( before.available && after.available ) {
        observation.runtime.processCpuNanoseconds
            = { difference( after.processCpuNanoseconds, before.processCpuNanoseconds ), false };
        observation.runtime.childCpuNanoseconds
            = { difference( after.childCpuNanoseconds, before.childCpuNanoseconds ), false };
        observation.runtime.peakRssBytes = { after.peakRssBytes, false };
        observation.runtime.voluntaryContextSwitches
            = { difference( after.voluntaryContextSwitches, before.voluntaryContextSwitches ),
                false };
        observation.runtime.involuntaryContextSwitches
            = { difference( after.involuntaryContextSwitches, before.involuntaryContextSwitches ),
                false };
    }
    observation.runtime.processTreeChildrenStarted = { factory.childrenStarted(), false };
    observation.runtime.maximumLiveChildren = { factory.childrenStarted() == 0u ? 0u : 1u, false };
    if ( failure ) {
        std::rethrow_exception( failure );
    }
    observation.lifecycle.samples.push_back(
        { LifecycleMilestone::Stop, elapsedNanoseconds( started ) } );
    if ( validateLifecycle( observation.lifecycle ).failure ) {
        throw std::runtime_error( "real-device lifecycle validation failed" );
    }
    return observation;
}

std::string serializeRealDeviceAggregateJson( const RealDeviceObservation& observation,
                                              std::uint64_t durationMilliseconds, bool ansiEnabled )
{
    if ( observation.topology != RequiredPipelineTopology || !observation.normalStop
         || !observation.cleanupVerified || !observation.formatComplete
         || observation.errorCount != 0u || validateLifecycle( observation.lifecycle ).failure ) {
        throw std::invalid_argument( "real-device observation is not successful" );
    }
    const auto start = milestone( observation.lifecycle, LifecycleMilestone::Start );
    const auto ready = milestone( observation.lifecycle, LifecycleMilestone::Ready );
    const auto firstByte = milestone( observation.lifecycle, LifecycleMilestone::FirstByte );
    const auto firstCommit
        = milestone( observation.lifecycle, LifecycleMilestone::FirstCommittedRecord );
    const auto stop = milestone( observation.lifecycle, LifecycleMilestone::Stop );
    if ( observation.lastCommittedRecordNanoseconds < firstCommit
         || observation.lastCommittedRecordNanoseconds > stop ) {
        throw std::invalid_argument( "real-device last-commit timestamp is invalid" );
    }
    const auto captureNanoseconds = durationMilliseconds * 1000000u;
    const auto throughput = captureNanoseconds == 0u
                                ? 0u
                                : observation.bytesReceived * 1000000000u / captureNanoseconds;

    std::ostringstream json;
    json << "{\"schema_version\":1,\"benchmark\":\"ios-real-live-capture-"
         << ( observation.arm == RealDeviceArm::NativeIntegrated ? "native" : "baseline" )
         << "\",\"status\":\"ok\",\"reason_code\":null,\"message\":\"completed\",\"metrics\":{"
         << "\"arm_native\":" << ( observation.arm == RealDeviceArm::NativeIntegrated ? 1 : 0 )
         << ",\"duration_ms\":" << durationMilliseconds
         << ",\"ansi_enabled\":" << ( ansiEnabled ? 1 : 0 ) << ",\"lifecycle_start_ns\":" << start
         << ",\"lifecycle_ready_ns\":" << ready << ",\"lifecycle_first_byte_ns\":" << firstByte
         << ",\"lifecycle_first_committed_record_ns\":" << firstCommit
         << ",\"lifecycle_last_committed_record_ns\":" << observation.lastCommittedRecordNanoseconds
         << ",\"lifecycle_stop_ns\":" << stop << ",\"startup_ns\":" << ( ready - start )
         << ",\"first_byte_latency_ns\":" << ( firstByte - ready )
         << ",\"first_commit_latency_ns\":" << ( firstCommit - ready )
         << ",\"teardown_ns\":" << ( stop - observation.lastCommittedRecordNanoseconds )
         << ",\"bytes_received\":" << observation.bytesReceived
         << ",\"lines_committed\":" << observation.linesCommitted
         << ",\"throughput_bytes_per_second\":" << throughput
         << ",\"ansi_escape_count\":" << observation.ansiEscapeCount
         << ",\"format_complete\":1,\"error_count\":0,\"cleanup_verified\":1";
    appendMetric( json, "process_cpu_ns", observation.runtime.processCpuNanoseconds );
    appendMetric( json, "child_cpu_ns", observation.runtime.childCpuNanoseconds );
    appendMetric( json, "peak_rss_bytes", observation.runtime.peakRssBytes );
    appendMetric( json, "voluntary_context_switches",
                  observation.runtime.voluntaryContextSwitches );
    appendMetric( json, "involuntary_context_switches",
                  observation.runtime.involuntaryContextSwitches );
    appendMetric( json, "process_tree_children_started",
                  observation.runtime.processTreeChildrenStarted );
    appendMetric( json, "maximum_live_children", observation.runtime.maximumLiveChildren );
    appendMetric( json, "queue_high_water_bytes", observation.queue.highWaterBytes );
    appendMetric( json, "queue_high_water_chunks", observation.queue.highWaterChunks );
    appendMetric( json, "queue_backpressure_events", observation.queue.backpressureEvents );
    appendMetric( json, "queue_dropped_records", observation.queue.droppedRecords );
    json << "}}\n";
    return json.str();
}

} // namespace klogg::benchmarks::livecapture
