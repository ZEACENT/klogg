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

#include <QComboBox>
#include <QCoreApplication>
#include <QEvent>
#include <QJsonDocument>
#include <QLabel>
#include <QPointer>
#include <QString>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
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
#include "ioslogdialog.h"
#include "iosnativestream.h"
#include "iosnativetransport.h"
#include "livesourcetransport.h"
#include "mainwindow.h"
#include "session.h"

namespace {
using klogg::livecapture::ErrorCategory;
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

    std::optional<klogg::livecapture::LiveSourceError> startupError() const override
    {
        return startupError_;
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
    std::optional<klogg::livecapture::LiveSourceError> startupError_;
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

    IosNativeStreamSessionCreation
    create( const IosNativeStreamConfig& config, IosNativeStreamCallbacks callbacks ) const override
    {
        state_->configs.push_back( config );
        state_->callbacks.push_back( std::move( callbacks ) );
        return IosNativeStreamSessionCreation{
            std::make_unique<RecordingSession>( state_, config.generation ), std::nullopt
        };
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

TEST_CASE( "bounded metadata executor reaches but never exceeds named concurrency",
           "[ios][native][composition][catalog][metadata][concurrency][bounded]" )
{
    using namespace std::chrono_literals;
    constexpr std::size_t Capacity = 3u;
    constexpr std::size_t TaskCount = 12u;
    struct Gate {
        std::mutex mutex;
        std::condition_variable changed;
        std::size_t active{ 0u };
        std::size_t maximumActive{ 0u };
        std::size_t started{ 0u };
        std::size_t finished{ 0u };
        bool release{ false };
    } gate;

    klogg::livecapture::BoundedConcurrentExecutor executor( Capacity, 100ms );
    for ( std::size_t task = 0u; task < TaskCount; ++task ) {
        REQUIRE( executor.post( [ &gate ] {
            std::unique_lock<std::mutex> lock( gate.mutex );
            ++gate.active;
            ++gate.started;
            gate.maximumActive = std::max( gate.maximumActive, gate.active );
            gate.changed.notify_all();
            gate.changed.wait( lock, [ & ] { return gate.release; } );
            --gate.active;
            ++gate.finished;
            gate.changed.notify_all();
        } ) );
    }

    {
        std::unique_lock<std::mutex> lock( gate.mutex );
        gate.changed.wait( lock, [ & ] { return gate.started == Capacity; } );
        CHECK( gate.active == Capacity );
        CHECK( gate.maximumActive == Capacity );
        CHECK( gate.started == Capacity );
        gate.release = true;
    }
    gate.changed.notify_all();
    {
        std::unique_lock<std::mutex> lock( gate.mutex );
        gate.changed.wait( lock, [ & ] { return gate.finished == TaskCount; } );
    }

    CHECK( gate.maximumActive == Capacity );
}

TEST_CASE( "bounded metadata executor coalesces keyed backlog without losing capacity or fairness",
           "[ios][native][composition][catalog][metadata][concurrency][bounded][coalescing]" )
{
    constexpr std::size_t Capacity = 3u;
    constexpr std::size_t ReplacementCount = 64u;
    struct Gate {
        std::mutex mutex;
        std::condition_variable changed;
        std::size_t active{ 0u };
        std::size_t maximumActive{ 0u };
        std::size_t started{ 0u };
        std::size_t finished{ 0u };
        bool release{ false };
        bool unrelatedRan{ false };
        std::vector<std::size_t> replacementEpochs;
    } gate;

    klogg::livecapture::BoundedConcurrentExecutor executor( Capacity,
                                                             std::chrono::milliseconds{ 100 } );
    const auto blockingTask = [ &gate ] {
        std::unique_lock<std::mutex> lock( gate.mutex );
        ++gate.active;
        ++gate.started;
        gate.maximumActive = std::max( gate.maximumActive, gate.active );
        gate.changed.notify_all();
        gate.changed.wait( lock, [ & ] { return gate.release; } );
        --gate.active;
        ++gate.finished;
        gate.changed.notify_all();
    };
    REQUIRE( executor.submitLatest( "device-a", blockingTask ) );
    REQUIRE( executor.submitLatest( "device-b", blockingTask ) );
    REQUIRE( executor.submitLatest( "device-c", blockingTask ) );
    {
        std::unique_lock<std::mutex> lock( gate.mutex );
        gate.changed.wait( lock, [ & ] { return gate.started == Capacity; } );
    }

    for ( std::size_t epoch = 1u; epoch <= ReplacementCount; ++epoch ) {
        REQUIRE( executor.submitLatest( "device-a", [ &gate, epoch ] {
            std::lock_guard<std::mutex> lock( gate.mutex );
            gate.replacementEpochs.push_back( epoch );
            ++gate.finished;
            gate.changed.notify_all();
        } ) );
        CHECK( executor.pendingLatestCountForTest() == 1u );
        CHECK( executor.runningLatestCountForTest() == Capacity );
    }
    REQUIRE( executor.submitLatest( "unrelated-device", [ &gate ] {
        std::lock_guard<std::mutex> lock( gate.mutex );
        gate.unrelatedRan = true;
        ++gate.finished;
        gate.changed.notify_all();
    } ) );
    CHECK( executor.pendingLatestCountForTest() == 2u );

    {
        std::lock_guard<std::mutex> lock( gate.mutex );
        gate.release = true;
    }
    gate.changed.notify_all();
    {
        std::unique_lock<std::mutex> lock( gate.mutex );
        gate.changed.wait( lock, [ & ] { return gate.finished == Capacity + 2u; } );
    }

    CHECK( gate.maximumActive == Capacity );
    CHECK( gate.unrelatedRan );
    CHECK( gate.replacementEpochs == std::vector<std::size_t>{ ReplacementCount } );
    CHECK( executor.pendingLatestCountForTest() == 0u );
}

TEST_CASE( "bounded metadata executor cancels keyed pending work deterministically",
           "[ios][native][composition][catalog][metadata][bounded][coalescing][cancellation]" )
{
    struct Gate {
        std::mutex mutex;
        std::condition_variable changed;
        bool started{ false };
        bool release{ false };
        std::vector<std::string> executed;
    } gate;

    klogg::livecapture::BoundedConcurrentExecutor executor( 1u,
                                                             std::chrono::milliseconds{ 100 } );
    REQUIRE( executor.submitLatest( "running", [ &gate ] {
        std::unique_lock<std::mutex> lock( gate.mutex );
        gate.started = true;
        gate.changed.notify_all();
        gate.changed.wait( lock, [ & ] { return gate.release; } );
        gate.executed.emplace_back( "running" );
        gate.changed.notify_all();
    } ) );
    {
        std::unique_lock<std::mutex> lock( gate.mutex );
        gate.changed.wait( lock, [ & ] { return gate.started; } );
    }

    REQUIRE( executor.submitLatest( "removed", [ &gate ] {
        std::lock_guard<std::mutex> lock( gate.mutex );
        gate.executed.emplace_back( "removed" );
        gate.changed.notify_all();
    } ) );
    REQUIRE( executor.submitLatest( "generation-stale", [ &gate ] {
        std::lock_guard<std::mutex> lock( gate.mutex );
        gate.executed.emplace_back( "generation-stale" );
        gate.changed.notify_all();
    } ) );
    CHECK( executor.pendingLatestCountForTest() == 2u );
    CHECK( executor.cancelLatest( "removed" ) );
    CHECK_FALSE( executor.cancelLatest( "missing" ) );
    CHECK( executor.pendingLatestCountForTest() == 1u );
    executor.clearPendingLatest();
    CHECK( executor.pendingLatestCountForTest() == 0u );
    REQUIRE( executor.submitLatest( "current", [ &gate ] {
        std::lock_guard<std::mutex> lock( gate.mutex );
        gate.executed.emplace_back( "current" );
        gate.changed.notify_all();
    } ) );

    {
        std::lock_guard<std::mutex> lock( gate.mutex );
        gate.release = true;
    }
    gate.changed.notify_all();
    {
        std::unique_lock<std::mutex> lock( gate.mutex );
        gate.changed.wait( lock, [ & ] { return gate.executed.size() == 2u; } );
    }

    CHECK( gate.executed == std::vector<std::string>{ "running", "current" } );
}

TEST_CASE( "bounded metadata executor shutdown does not wait for a blocked RPC",
           "[ios][native][composition][catalog][metadata][shutdown][barrier]" )
{
    using namespace std::chrono_literals;
    struct Gate {
        std::mutex mutex;
        std::condition_variable changed;
        bool started{ false };
        bool release{ false };
        bool taskFinished{ false };
        bool shutdownReturned{ false };
    } gate;

    auto executor = std::make_unique<klogg::livecapture::BoundedConcurrentExecutor>( 2u, 100ms );
    REQUIRE( executor->post( [ &gate ] {
        std::unique_lock<std::mutex> lock( gate.mutex );
        gate.started = true;
        gate.changed.notify_all();
        gate.changed.wait( lock, [ & ] { return gate.release; } );
        gate.taskFinished = true;
        gate.changed.notify_all();
    } ) );
    {
        std::unique_lock<std::mutex> lock( gate.mutex );
        gate.changed.wait( lock, [ & ] { return gate.started; } );
    }

    std::thread shutdown( [ & ] {
        executor->shutdownAsync();
        executor.reset();
        {
            std::lock_guard<std::mutex> lock( gate.mutex );
            gate.shutdownReturned = true;
        }
        gate.changed.notify_all();
    } );
    {
        std::unique_lock<std::mutex> lock( gate.mutex );
        gate.changed.wait( lock, [ & ] { return gate.shutdownReturned; } );
        CHECK_FALSE( gate.taskFinished );
        gate.release = true;
    }
    gate.changed.notify_all();
    shutdown.join();
    {
        std::unique_lock<std::mutex> lock( gate.mutex );
        gate.changed.wait( lock, [ & ] { return gate.taskFinished; } );
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
    CHECK( IosLiveServicesTestAccess::metadataObservationEntryCount( services ) == 1u );

    auto completed = catalogAddress->snapshot();
    completed.entries.front().metadata
        = IosDeviceMetadata{ "Owned phone", "iPhone17,1", "20.0" };
    catalogAddress->publish( completed );
    CHECK( catalogAddress->metadataRequests.size() == 1u );
    CHECK( IosLiveServicesTestAccess::metadataObservationEntryCount( services ) == 0u );

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

TEST_CASE( "iOS metadata observation bookkeeping stays bounded across endpoint churn",
           "[ios][native][composition][catalog][metadata][churn][bounded]" )
{
    auto catalog = std::make_unique<MemoryCatalog>();
    auto* const catalogAddress = catalog.get();
    IosLiveServices services( std::move( catalog ), nullptr );
    REQUIRE( catalogAddress->metadataRequests.size() == 1u );
    CHECK( IosLiveServicesTestAccess::metadataObservationEntryCount( services ) == 1u );

    constexpr Generation ChurnCount = 64u;
    for ( Generation cycle = 1u; cycle <= ChurnCount; ++cycle ) {
        auto removed = catalogAddress->snapshot();
        removed.entries.clear();
        catalogAddress->publish( removed );
        CHECK( IosLiveServicesTestAccess::metadataObservationEntryCount( services ) == 0u );

        auto readded = removed;
        readded.entries.push_back( IosCatalogEntry{
            IosEndpointKey{ "owned-device", NativeConnectionType::Usb }, 3u + cycle,
            std::nullopt, std::nullopt } );
        catalogAddress->publish( readded );
        CHECK( IosLiveServicesTestAccess::metadataObservationEntryCount( services ) == 1u );
    }

    CHECK( catalogAddress->metadataRequests.size()
           == static_cast<std::size_t>( ChurnCount + 1u ) );
}

TEST_CASE( "iOS composition re-requests metadata after a recoverable error is rearmed",
           "[ios][native][composition][catalog][metadata][retry]" )
{
    auto catalog = std::make_unique<MemoryCatalog>();
    auto* const catalogAddress = catalog.get();
    IosLiveServices services( std::move( catalog ), nullptr );
    REQUIRE( catalogAddress->metadataRequests.size() == 1u );

    auto failed = catalogAddress->snapshot();
    failed.entries.front().error = IosCatalogError{
        klogg::livecapture::LiveSourceError{ klogg::livecapture::ErrorCategory::Device,
                                            "ios-trust-pending",
                                            klogg::livecapture::ErrorScope::Device,
                                            klogg::livecapture::RetryPolicy::AwaitUser,
                                            "Trust this computer on the iOS device.",
                                            "trust pending" },
        klogg::livecapture::AwaitingUserReason::Trust
    };
    catalogAddress->publish( failed );
    CHECK( catalogAddress->metadataRequests.size() == 1u );

    auto rearmed = failed;
    rearmed.entries.front().epoch = 4u;
    rearmed.entries.front().error.reset();
    catalogAddress->publish( rearmed );

    REQUIRE( catalogAddress->metadataRequests.size() == 2u );
    CHECK( catalogAddress->metadataRequests.back().endpoint == rearmed.entries.front().endpoint );
    CHECK( catalogAddress->metadataRequests.back().endpointEpoch == 4u );
}

TEST_CASE( "iOS picker follows catalog metadata snapshots without manual refresh",
           "[ios][native][composition][dialog][catalog][metadata]" )
{
    MemoryCatalog catalog;
    IosLogDialog dialog( catalog );
    REQUIRE( QMetaObject::invokeMethod( &dialog, "refreshDevices", Qt::DirectConnection ) );
    auto* const combo = dialog.findChild<QComboBox*>( QStringLiteral( "deviceCombo" ) );
    REQUIRE( combo != nullptr );
    REQUIRE( combo->count() == 1 );
    CHECK( combo->currentText().contains( QStringLiteral( "owned-device" ) ) );
    drainQtEvents();

    auto enriched = catalog.snapshot();
    enriched.entries.front().metadata
        = IosDeviceMetadata{ "My iPhone", "iPhone17,1", "20.0" };
    catalog.publish( enriched );
    drainQtEvents();

    REQUIRE( combo->count() == 1 );
    CHECK( combo->currentText().contains( QStringLiteral( "My iPhone" ) ) );
    CHECK( combo->itemData( 0, Qt::ToolTipRole ).toString().contains(
        QStringLiteral( "iPhone17,1" ) ) );
    CHECK( combo->itemData( 0, Qt::ToolTipRole ).toString().contains(
        QStringLiteral( "20.0" ) ) );
}

TEST_CASE( "iOS picker surfaces native catalog startup failures",
           "[ios][native][composition][dialog][catalog][startup-error]" )
{
    MemoryCatalog catalog;
    catalog.snapshot_.entries.clear();
    catalog.startupError_ = klogg::livecapture::LiveSourceError{
        klogg::livecapture::ErrorCategory::Configuration, "ios-catalog-start-failed",
        klogg::livecapture::ErrorScope::Infrastructure, klogg::livecapture::RetryPolicy::Never,
        "The bundled native iOS catalog is unavailable.", "missing native symbol" };
    IosLogDialog dialog( catalog );
    REQUIRE( QMetaObject::invokeMethod( &dialog, "refreshDevices", Qt::DirectConnection ) );

    auto* const status
        = dialog.findChild<QLabel*>( QStringLiteral( "iosLogStatusLabel" ) );
    REQUIRE( status != nullptr );
    CHECK( status->text().contains( QStringLiteral( "unavailable" ),
                                    Qt::CaseInsensitive ) );
    CHECK( status->toolTip().contains( QStringLiteral( "missing native symbol" ) ) );
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

TEST_CASE( "iOS composition rejects unsupported filter and JSON options before worker creation",
           "[ios][native][composition][options][validation][w3-ios-options-red]" )
{
    struct UnsupportedOption {
        const char* name;
        std::function<void( LiveSourceTransportConfig& )> apply;
    };
    const std::array cases{
        UnsupportedOption{ "level", []( LiveSourceTransportConfig& config ) {
                              config.iosLevel = QStringLiteral( "debug" );
                          } },
        UnsupportedOption{ "category", []( LiveSourceTransportConfig& config ) {
                              config.iosCategories = QStringList{ QStringLiteral( "network" ) };
                          } },
        UnsupportedOption{ "subsystem", []( LiveSourceTransportConfig& config ) {
                              config.iosSubsystem = QStringLiteral( "com.example.app" );
                          } },
        UnsupportedOption{ "JSON", []( LiveSourceTransportConfig& config ) {
                              config.iosJsonOutput = true;
                          } },
    };

    for ( const auto& value : cases ) {
        DYNAMIC_SECTION( value.name )
        {
            auto workerState = std::make_shared<WorkerState>();
            IosLiveServices services(
                std::make_unique<MemoryCatalog>(),
                std::make_unique<RecordingWorkerFactory>( workerState ) );
            auto config = iosConfig();
            value.apply( config );

            auto transport = services.create( config );

            CHECK( transport == nullptr );
            CHECK( workerState->configs.empty() );
            REQUIRE( services.lastConfigurationError().has_value() );
            CHECK( services.lastConfigurationError()->code == "unsupported-ios-log-options" );
            CHECK( services.lastConfigurationError()->category == ErrorCategory::Configuration );
            CHECK( services.lastConfigurationError()->retryPolicy == RetryPolicy::Never );
        }
    }
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
