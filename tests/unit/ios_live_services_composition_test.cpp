/*
 * Copyright (C) 2026
 *
 * This file is part of klogg.
 *
 * RED composition contract: one application-owned iOS root supplies the shared
 * catalog and every native transport. No process, Python, script, PATH, or
 * command-line fallback is permitted for a new iOS capture.
 */

#include <catch2/catch.hpp>

#include <QCoreApplication>
#include <QEvent>
#include <QJsonDocument>
#include <QPointer>
#include <QString>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "adblogcatsource.h"
#include "boundedserialexecutor.h"
#include "ioscatalogprovider.h"
#include "iosliveservices.h"
#include "iosnativestream.h"
#include "iosnativetransport.h"
#include "livesourcetransport.h"
#include "mainwindow.h"
#include "session.h"

namespace {
using klogg::livecapture::Generation;
using klogg::livecapture::LiveDataBatch;
using klogg::livecapture::LiveDataStatistics;
using klogg::livecapture::RetryPolicy;
using namespace klogg::livecapture::ios;

static_assert( std::is_base_of_v<LiveSourceTransportFactory, IosLiveServices> );
static_assert(
    std::is_constructible_v<MainWindow, WindowSession, AdbLiveServices&, IosLiveServices&>,
    "KloggApp must inject both application-owned live-service roots" );
static_assert( std::is_same_v<decltype( std::declval<const MainWindow&>().iosLiveServices() ),
                              const IosLiveServices*> );
static_assert( std::is_constructible_v<Session, const LiveSourceTransportFactory&>,
               "Session must consume the composed application transport factory" );

void drainQtEvents()
{
    QCoreApplication::sendPostedEvents( nullptr, QEvent::MetaCall );
    QCoreApplication::sendPostedEvents( nullptr, QEvent::DeferredDelete );
    QCoreApplication::processEvents();
}

class MemoryCatalog final : public IosCatalogSnapshotProvider, public IosCatalogMetadataRequester {
public:
    struct MetadataRequest {
        IosEndpointKey endpoint;
        Generation catalogGeneration{ 0u };
        Generation endpointEpoch{ 0u };
    };

    IosCatalogSnapshot snapshot() const override
    {
        return snapshot_;
    }

    SubscriptionId subscribe( SnapshotCallback callback ) override
    {
        callbacks.emplace_back( ++nextSubscription_, std::move( callback ) );
        return nextSubscription_;
    }

    void unsubscribe( SubscriptionId subscription ) override
    {
        for ( auto& entry : callbacks ) {
            if ( entry.first == subscription ) {
                entry.second = {};
            }
        }
    }

    void requestMetadata( IosEndpointKey endpoint ) override
    {
        Generation endpointEpoch{ 0u };
        for ( const auto& entry : snapshot_.entries ) {
            if ( entry.endpoint == endpoint ) {
                endpointEpoch = entry.epoch;
                break;
            }
        }
        metadataRequests.push_back(
            MetadataRequest{ std::move( endpoint ), snapshot_.generation, endpointEpoch } );
    }

    void publish( IosCatalogSnapshot snapshot )
    {
        snapshot_ = std::move( snapshot );
        for ( const auto& entry : callbacks ) {
            if ( entry.second ) {
                entry.second( snapshot_ );
            }
        }
    }

    IosCatalogSnapshot snapshot_{ 7u,
                                  { IosCatalogEntry{
                                      IosEndpointKey{ "owned-device", NativeConnectionType::Usb },
                                      3u, std::nullopt, std::nullopt } } };
    std::vector<std::pair<SubscriptionId, SnapshotCallback>> callbacks;
    std::vector<MetadataRequest> metadataRequests;

private:
    SubscriptionId nextSubscription_{ 0u };
};

struct WorkerState {
    std::vector<IosNativeStreamConfig> configs;
    std::vector<IosNativeStreamCallbacks> callbacks;
    int startCalls{ 0 };
    int stopCalls{ 0 };
    int shutdownCalls{ 0 };
};

class RecordingSession final : public IosNativeStreamSession {
public:
    RecordingSession( std::shared_ptr<WorkerState> state, Generation generation )
        : state_( std::move( state ) )
        , generation_( generation )
    {
    }

    bool start() override
    {
        ++state_->startCalls;
        return true;
    }

    void stop( Generation generation ) noexcept override
    {
        if ( generation == generation_ ) {
            ++state_->stopCalls;
        }
    }

    void shutdown() noexcept override
    {
        ++state_->shutdownCalls;
    }

    std::optional<LiveDataBatch> drain() override
    {
        return std::nullopt;
    }

    LiveDataStatistics statistics() const override
    {
        LiveDataStatistics result;
        result.generation = generation_;
        return result;
    }

private:
    std::shared_ptr<WorkerState> state_;
    Generation generation_{ 0u };
};

class RecordingWorkerFactory final : public IosNativeStreamWorkerFactory {
public:
    explicit RecordingWorkerFactory( std::shared_ptr<WorkerState> state )
        : state_( std::move( state ) )
    {
    }

    std::unique_ptr<IosNativeStreamSession>
    create( const IosNativeStreamConfig& config, IosNativeStreamCallbacks callbacks ) const override
    {
        state_->configs.push_back( config );
        state_->callbacks.push_back( std::move( callbacks ) );
        return std::make_unique<RecordingSession>( state_, config.generation );
    }

private:
    std::shared_ptr<WorkerState> state_;
};

LiveSourceTransportConfig iosConfig()
{
    LiveSourceTransportConfig config;
    config.sourceType = LiveLogSourceType::IosLogStream;
    config.iosEndpoint = IosEndpointKey{ "owned-device", NativeConnectionType::Usb };
    config.ansiOutputEnabled = true;
    return config;
}

} // namespace

TEST_CASE( "bounded serial executor releases shutdown when a native task remains blocked",
           "[ios][native][composition][catalog][shutdown][deadline]" )
{
    using namespace std::chrono_literals;
    struct Gate {
        std::mutex mutex;
        std::condition_variable changed;
        bool started{ false };
        bool release{ false };
        bool finished{ false };
    };

    auto gate = std::make_shared<Gate>();
    auto executor = std::make_unique<klogg::livecapture::BoundedSerialExecutor>( 20ms );
    executor->post( [ gate ] {
        std::unique_lock<std::mutex> lock( gate->mutex );
        gate->started = true;
        gate->changed.notify_all();
        gate->changed.wait( lock, [ & ] { return gate->release; } );
        gate->finished = true;
        gate->changed.notify_all();
    } );

    {
        std::unique_lock<std::mutex> lock( gate->mutex );
        REQUIRE( gate->changed.wait_for( lock, 1s, [ & ] { return gate->started; } ) );
    }

    const auto shutdownStarted = std::chrono::steady_clock::now();
    executor.reset();
    CHECK( std::chrono::steady_clock::now() - shutdownStarted < 500ms );

    {
        std::lock_guard<std::mutex> lock( gate->mutex );
        gate->release = true;
    }
    gate->changed.notify_all();
    {
        std::unique_lock<std::mutex> lock( gate->mutex );
        REQUIRE( gate->changed.wait_for( lock, 1s, [ & ] { return gate->finished; } ) );
    }
}

TEST_CASE( "iOS backend persistence makes native fresh and legacy process explicit",
           "[ios][native][composition][persistence][migration]" )
{
    AdbLogcatSessionData native;
    native.sourceType = LiveLogSourceType::IosLogStream;
    native.deviceSerial = QStringLiteral( "native-device" );
    native.iosBackend = IosTransportBackend::Native;
    native.iosEndpoint = IosEndpointKey{ "native-device", NativeConnectionType::Network };
    const auto nativeJson
        = QString::fromUtf8( QJsonDocument( native.toJson() ).toJson( QJsonDocument::Compact ) );
    const auto restoredNative = AdbLogcatSessionData::fromJson( nativeJson );
    CHECK( restoredNative.iosBackend == IosTransportBackend::Native );
    CHECK( restoredNative.iosEndpoint == native.iosEndpoint );

    const auto missingDiscriminator = AdbLogcatSessionData::fromJson( QStringLiteral(
        R"({"sourceType":"ios_log_stream","adbExecutable":"/legacy/python","deviceSerial":"missing-device"})" ) );
    CHECK( missingDiscriminator.iosBackend == IosTransportBackend::Native );
    CHECK( missingDiscriminator.iosEndpoint.udid == "missing-device" );

    const auto explicitLegacy = AdbLogcatSessionData::fromJson( QStringLiteral(
        R"({"sourceType":"ios_log_stream","iosBackend":"legacy_process","adbExecutable":"/legacy/python","deviceSerial":"legacy-device"})" ) );
    CHECK( explicitLegacy.iosBackend == IosTransportBackend::LegacyProcess );
    CHECK( explicitLegacy.iosEndpoint.udid == "legacy-device" );

    const auto tampered = AdbLogcatSessionData::fromJson( QStringLiteral(
        R"({"sourceType":"ios_log_stream","iosBackend":"python","adbExecutable":"/legacy/python","deviceSerial":"tampered-device"})" ) );
    CHECK( tampered.iosBackend == IosTransportBackend::Native );
}

TEST_CASE( "iOS composition requests initial endpoint metadata once with its catalog generation",
           "[ios][native][composition][catalog][metadata][generation]" )
{
    auto catalog = std::make_unique<MemoryCatalog>();
    auto* const catalogAddress = catalog.get();
    IosLiveServices services( std::move( catalog ), nullptr );

    REQUIRE( catalogAddress->metadataRequests.size() == 1u );
    const auto& request = catalogAddress->metadataRequests.front();
    CHECK( request.endpoint == IosEndpointKey{ "owned-device", NativeConnectionType::Usb } );
    CHECK( request.catalogGeneration == 7u );
    CHECK( request.endpointEpoch == 3u );

    catalogAddress->publish( catalogAddress->snapshot() );
    CHECK( catalogAddress->metadataRequests.size() == 1u );

    services.shutdown();
    auto afterShutdown = catalogAddress->snapshot();
    afterShutdown.entries.push_back(
        IosCatalogEntry{ IosEndpointKey{ "ignored-after-shutdown", NativeConnectionType::Usb }, 4u,
                         std::nullopt, std::nullopt } );
    catalogAddress->publish( std::move( afterShutdown ) );
    CHECK( catalogAddress->metadataRequests.size() == 1u );
}

TEST_CASE( "iOS composition requests each newly added endpoint once in the current generation",
           "[ios][native][composition][catalog][metadata][generation][add]" )
{
    auto catalog = std::make_unique<MemoryCatalog>();
    auto* const catalogAddress = catalog.get();
    IosLiveServices services( std::move( catalog ), nullptr );
    REQUIRE( catalogAddress->metadataRequests.size() == 1u );

    auto addedSnapshot = catalogAddress->snapshot();
    addedSnapshot.entries.push_back(
        IosCatalogEntry{ IosEndpointKey{ "new-device", NativeConnectionType::Network }, 4u,
                         std::nullopt, std::nullopt } );
    catalogAddress->publish( addedSnapshot );

    REQUIRE( catalogAddress->metadataRequests.size() == 2u );
    const auto& addedRequest = catalogAddress->metadataRequests.back();
    CHECK( addedRequest.endpoint == IosEndpointKey{ "new-device", NativeConnectionType::Network } );
    CHECK( addedRequest.catalogGeneration == 7u );
    CHECK( addedRequest.endpointEpoch == 4u );

    catalogAddress->publish( addedSnapshot );
    CHECK( catalogAddress->metadataRequests.size() == 2u );
}

TEST_CASE( "iOS composition re-requests an endpoint once after catalog generation changes",
           "[ios][native][composition][catalog][metadata][generation][restart]" )
{
    auto catalog = std::make_unique<MemoryCatalog>();
    auto* const catalogAddress = catalog.get();
    IosLiveServices services( std::move( catalog ), nullptr );
    REQUIRE( catalogAddress->metadataRequests.size() == 1u );

    IosCatalogSnapshot restartedSnapshot{
        8u,
        { IosCatalogEntry{ IosEndpointKey{ "owned-device", NativeConnectionType::Usb }, 5u,
                           std::nullopt, std::nullopt } }
    };
    catalogAddress->publish( restartedSnapshot );

    REQUIRE( catalogAddress->metadataRequests.size() == 2u );
    const auto& restartedRequest = catalogAddress->metadataRequests.back();
    CHECK( restartedRequest.catalogGeneration == 8u );
    CHECK( restartedRequest.endpointEpoch == 5u );

    catalogAddress->publish( restartedSnapshot );
    CHECK( catalogAddress->metadataRequests.size() == 2u );
}

TEST_CASE( "one application iOS root shares catalog identity and creates native transports",
           "[ios][native][composition][ownership][catalog][transport]" )
{
    auto catalog = std::make_unique<MemoryCatalog>();
    const auto* const catalogAddress = catalog.get();
    auto workerState = std::make_shared<WorkerState>();
    auto workerFactory = std::make_unique<RecordingWorkerFactory>( workerState );
    IosLiveServices services( std::move( catalog ), std::move( workerFactory ) );

    CHECK( &services.catalogProvider() == catalogAddress );
    CHECK( services.catalogProvider().snapshot().entries.size() == 1u );
    Session applicationSession( services );
    CHECK( &applicationSession.transportFactory() == &services );

    auto first = services.create( iosConfig() );
    auto second = services.create( iosConfig() );
    REQUIRE( first != nullptr );
    REQUIRE( second != nullptr );
    CHECK( dynamic_cast<IosNativeTransport*>( first.get() ) != nullptr );
    CHECK( dynamic_cast<IosNativeTransport*>( second.get() ) != nullptr );
    CHECK( dynamic_cast<ProcessLiveSourceTransport*>( first.get() ) == nullptr );
    CHECK( dynamic_cast<ProcessLiveSourceTransport*>( second.get() ) == nullptr );

    first->start( 101u );
    second->start( 102u );
    REQUIRE( workerState->configs.size() == 2u );
    const IosEndpointKey expectedEndpoint{ "owned-device", NativeConnectionType::Usb };
    CHECK( workerState->configs.at( 0 ).endpoint == expectedEndpoint );
    CHECK( workerState->configs.at( 1 ).endpoint == expectedEndpoint );
    CHECK( workerState->configs.at( 0 ).generation == 101u );
    CHECK( workerState->configs.at( 1 ).generation == 102u );
    CHECK( workerState->configs.at( 0 ).ansiOutputEnabled );
}

TEST_CASE( "iOS composition preserves the native stack load diagnostic on create",
           "[ios][native][composition][configuration][diagnostic]" )
{
    IosLiveServicesConfig config;
    config.nativeStackRoot = "/definitely/missing/klogg-ios-native-stack";
    IosLiveServices services( std::move( config ) );

    CHECK( services.create( iosConfig() ) == nullptr );
    const auto error = services.lastConfigurationError();
    REQUIRE( error.has_value() );
    CHECK( error->code == "ios-native-stack-unavailable" );
    CHECK_FALSE( error->nativeDetail.empty() );
}

TEST_CASE( "iOS composition fails closed when the native worker factory is unavailable",
           "[ios][native][composition][configuration][fail-closed]" )
{
    IosLiveServices services( std::make_unique<MemoryCatalog>(), nullptr );
    auto transport = services.create( iosConfig() );
    CHECK( transport == nullptr );
    REQUIRE( services.lastConfigurationError().has_value() );
    CHECK( services.lastConfigurationError()->code == "ios-native-services-unavailable" );
}

TEST_CASE( "iOS composition refuses legacy executable and free-form arguments without fallback",
           "[ios][native][composition][configuration][no-process-fallback]" )
{
    auto workerState = std::make_shared<WorkerState>();
    IosLiveServices services( std::make_unique<MemoryCatalog>(),
                              std::make_unique<RecordingWorkerFactory>( workerState ) );
    auto config = iosConfig();
    config.executable = QStringLiteral( "/attacker/python" );
    config.extraArgs = QStringLiteral( "-m pymobiledevice3 --script /usr/bin/script" );

    auto transport = services.create( config );

    CHECK( transport == nullptr );
    CHECK( workerState->configs.empty() );
    REQUIRE( services.lastConfigurationError().has_value() );
    CHECK( services.lastConfigurationError()->code == "ios-legacy-process-options-unsupported" );
    CHECK( services.lastConfigurationError()->retryPolicy == RetryPolicy::Never );
    CHECK( services.lastConfigurationError()->nativeDetail.find( "python" ) != std::string::npos );
}

TEST_CASE( "iOS composition never claims Android sources or substitutes a process transport",
           "[ios][native][composition][routing][no-fallback]" )
{
    auto workerState = std::make_shared<WorkerState>();
    IosLiveServices services( std::make_unique<MemoryCatalog>(),
                              std::make_unique<RecordingWorkerFactory>( workerState ) );
    LiveSourceTransportConfig config;
    config.sourceType = LiveLogSourceType::AdbLogcat;
    config.executable = QStringLiteral( "adb" );

    auto transport = services.create( config );

    CHECK( transport == nullptr );
    CHECK( workerState->configs.empty() );
}

TEST_CASE( "application iOS shutdown tolerates one transport destroying a sibling reentrantly",
           "[ios][native][composition][shutdown][reentrant][sibling]" )
{
    auto workerState = std::make_shared<WorkerState>();
    IosLiveServices services( std::make_unique<MemoryCatalog>(),
                              std::make_unique<RecordingWorkerFactory>( workerState ) );
    auto first = services.create( iosConfig() );
    auto second = services.create( iosConfig() );
    REQUIRE( first != nullptr );
    REQUIRE( second != nullptr );
    QPointer<LiveSourceTransport> secondGuard( second.get() );
    first->start( 181u );
    second->start( 182u );
    QObject::connect( first.get(), &LiveSourceTransport::stateChanged, first.get(),
                      [ & ]( Generation, LiveSourceTransport::State state ) {
                          if ( state == LiveSourceTransport::State::Disconnected ) {
                              second.reset();
                          }
                      } );

    services.shutdown();
    drainQtEvents();

    CHECK( secondGuard.isNull() );
    CHECK( workerState->stopCalls == 2 );
    CHECK( workerState->shutdownCalls == 2 );
}

TEST_CASE( "native transport can be destroyed before its application iOS root",
           "[ios][native][composition][lifetime][transport-first]" )
{
    auto workerState = std::make_shared<WorkerState>();
    IosLiveServices services( std::make_unique<MemoryCatalog>(),
                              std::make_unique<RecordingWorkerFactory>( workerState ) );
    auto transport = services.create( iosConfig() );
    REQUIRE( transport != nullptr );
    transport->start( 191u );
    transport.reset();
    drainQtEvents();

    CHECK( workerState->shutdownCalls == 1 );
    services.shutdown();
    CHECK( workerState->shutdownCalls == 1 );
}

TEST_CASE( "application iOS shutdown retires surviving transports before shared services",
           "[ios][native][composition][shutdown][ordering]" )
{
    auto workerState = std::make_shared<WorkerState>();
    IosLiveServices services( std::make_unique<MemoryCatalog>(),
                              std::make_unique<RecordingWorkerFactory>( workerState ) );
    auto transport = services.create( iosConfig() );
    REQUIRE( transport != nullptr );
    std::vector<LiveSourceTransport::State> states;
    QObject::connect( transport.get(), &LiveSourceTransport::stateChanged,
                      [ &states ]( Generation, LiveSourceTransport::State state ) {
                          states.push_back( state );
                      } );
    transport->start( 201u );
    REQUIRE( workerState->callbacks.size() == 1u );
    workerState->callbacks.front().ready( 201u );
    drainQtEvents();
    REQUIRE( states.back() == LiveSourceTransport::State::Connected );

    services.shutdown();
    drainQtEvents();

    CHECK( workerState->stopCalls == 1 );
    CHECK( workerState->shutdownCalls == 1 );
    REQUIRE_FALSE( states.empty() );
    CHECK( states.back() == LiveSourceTransport::State::Disconnected );
    CHECK( services.create( iosConfig() ) == nullptr );
}
