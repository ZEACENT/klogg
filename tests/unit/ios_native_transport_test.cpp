/*
 * Copyright (C) 2026
 *
 * This file is part of klogg.
 *
 * RED contract for the Qt adapter around the native iOS worker. The scripted
 * worker is fully in-memory and deterministic; no vendor code or device runs.
 */

#include <catch2/catch.hpp>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QMetaObject>
#include <QPointer>
#include <QString>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "iosnativeapi.h"
#include "iosnativestream.h"
#include "iosnativetransport.h"
#include "livedataqueue.h"
#include "livesourcetransport.h"
#include "test_utils.h"

namespace {
using klogg::livecapture::ErrorCategory;
using klogg::livecapture::ErrorScope;
using klogg::livecapture::Generation;
using klogg::livecapture::LiveDataBatch;
using klogg::livecapture::LiveDataChunk;
using klogg::livecapture::LiveDataEnqueueResult;
using klogg::livecapture::LiveDataQueue;
using klogg::livecapture::LiveDataStatistics;
using klogg::livecapture::LiveSourceError;
using klogg::livecapture::RetryPolicy;
using namespace klogg::livecapture::ios;

void drainQtEvents()
{
    QCoreApplication::sendPostedEvents( nullptr, QEvent::MetaCall );
    QCoreApplication::sendPostedEvents( nullptr, QEvent::DeferredDelete );
    QCoreApplication::processEvents();
}

ClassifiedIosNativeError disconnectError( std::string detail = "mux disconnected" )
{
    return { LiveSourceError{ ErrorCategory::Device, "ios-device-disconnected", ErrorScope::Device,
                              RetryPolicy::WaitForDevice, "The iOS device disconnected.",
                              std::move( detail ) },
             std::nullopt };
}

struct ScriptedSessionState {
    explicit ScriptedSessionState( IosNativeStreamConfig value, IosNativeStreamCallbacks observed )
        : config( std::move( value ) )
        , callbacks( std::move( observed ) )
        , queue( config.queueLimits, config.generation, [] {} )
    {
    }

    IosNativeStreamConfig config;
    IosNativeStreamCallbacks callbacks;
    LiveDataQueue queue;
    int startCalls{ 0 };
    int stopCalls{ 0 };
    int shutdownCalls{ 0 };
    bool destroyed{ false };
    bool throwFromDrain{ false };
    bool throwFromStatistics{ false };
    std::optional<int> throwOnDrainCall;
    std::optional<int> throwOnStatisticsCall;
    int drainCalls{ 0 };
    mutable int statisticsCalls{ 0 };
    std::size_t rejectedBeforeEnqueueBytes{ 0u };
    std::size_t rejectedBeforeEnqueueChunks{ 0u };
    std::function<void()> afterDrain;
};

class ScriptedSession final : public IosNativeStreamSession {
public:
    explicit ScriptedSession( std::shared_ptr<ScriptedSessionState> state )
        : state_( std::move( state ) )
    {
    }

    ~ScriptedSession() override
    {
        state_->destroyed = true;
    }

    bool start() override
    {
        ++state_->startCalls;
        return true;
    }

    void stop( Generation generation ) noexcept override
    {
        if ( generation == state_->config.generation ) {
            ++state_->stopCalls;
        }
    }

    void shutdown() noexcept override
    {
        ++state_->shutdownCalls;
    }

    std::optional<LiveDataBatch> drain() override
    {
        ++state_->drainCalls;
        if ( state_->throwFromDrain
             || state_->throwOnDrainCall == state_->drainCalls ) {
            throw std::runtime_error( "scripted drain failure" );
        }
        auto batch = state_->queue.drain();
        if ( state_->afterDrain ) {
            auto callback = std::exchange( state_->afterDrain, {} );
            callback();
        }
        return batch;
    }

    LiveDataStatistics statistics() const override
    {
        ++state_->statisticsCalls;
        if ( state_->throwFromStatistics
             || state_->throwOnStatisticsCall == state_->statisticsCalls ) {
            throw std::runtime_error( "scripted statistics failure" );
        }
        auto result = state_->queue.statistics();
        result.rejectedBeforeEnqueueBytes = state_->rejectedBeforeEnqueueBytes;
        result.rejectedBeforeEnqueueChunks = state_->rejectedBeforeEnqueueChunks;
        return result;
    }

private:
    std::shared_ptr<ScriptedSessionState> state_;
};

class RejectingWorkerFactory final : public IosNativeStreamWorkerFactory {
public:
    IosNativeStreamSessionCreation
    create( const IosNativeStreamConfig&, IosNativeStreamCallbacks ) const override
    {
        return IosNativeStreamSessionCreation{
            nullptr,
            ClassifiedIosNativeError{
                LiveSourceError{ ErrorCategory::Backend,
                                 "ios-native-test-rejection",
                                 ErrorScope::Stream,
                                 RetryPolicy::Backoff,
                                 "The test factory rejected native session creation.",
                                 "Synthetic typed factory rejection." },
                std::nullopt }
        };
    }
};

class ScriptedWorkerFactory final : public IosNativeStreamWorkerFactory {
public:
    IosNativeStreamSessionCreation
    create( const IosNativeStreamConfig& config, IosNativeStreamCallbacks callbacks ) const override
    {
        auto state = std::make_shared<ScriptedSessionState>( config, std::move( callbacks ) );
        sessions.push_back( state );
        return IosNativeStreamSessionCreation{
            std::make_unique<ScriptedSession>( std::move( state ) ), std::nullopt
        };
    }

    std::shared_ptr<ScriptedSessionState> latest() const
    {
        REQUIRE_FALSE( sessions.empty() );
        return sessions.back();
    }

    void publishReady( std::size_t index ) const
    {
        const auto state = sessions.at( index );
        state->callbacks.ready( state->config.generation );
    }

    void publishBytes( std::size_t index, std::string bytes, bool notify = true ) const
    {
        const auto state = sessions.at( index );
        const auto generation = state->config.generation;
        const LiveDataChunk chunk{ generation,
                                   std::vector<std::uint8_t>( bytes.begin(), bytes.end() ) };
        REQUIRE( state->queue.tryEnqueue( chunk ) == LiveDataEnqueueResult::Accepted );
        if ( notify ) {
            state->callbacks.bytesAvailable( generation );
        }
    }

    void publishFailure( std::size_t index, ClassifiedIosNativeError error ) const
    {
        const auto state = sessions.at( index );
        state->callbacks.failed( state->config.generation, error );
    }

    void publishStopped( std::size_t index ) const
    {
        const auto state = sessions.at( index );
        state->callbacks.stopped( state->config.generation );
    }

    mutable std::vector<std::shared_ptr<ScriptedSessionState>> sessions;
};

struct LegacySyslogNativeState {
    std::mutex mutex;
    std::condition_variable changed;
    NativeSyslogRelayCallback callback{ nullptr };
    NativeSyslogRelayErrorCallback errorCallback{ nullptr };
    void* context{ nullptr };
    bool started{ false };

    void reset()
    {
        std::lock_guard<std::mutex> lock( mutex );
        callback = nullptr;
        errorCallback = nullptr;
        context = nullptr;
        started = false;
    }

    bool waitUntilStarted()
    {
        std::unique_lock<std::mutex> lock( mutex );
        return changed.wait_for( lock, std::chrono::seconds{ 2 }, [ this ] { return started; } );
    }

    void emit( const std::string& bytes )
    {
        NativeSyslogRelayCallback observedCallback = nullptr;
        void* observedContext = nullptr;
        {
            std::lock_guard<std::mutex> lock( mutex );
            observedCallback = callback;
            observedContext = context;
        }
        REQUIRE( observedCallback != nullptr );
        for ( const auto byte : bytes ) {
            observedCallback( byte, observedContext );
        }
    }
};

LegacySyslogNativeState legacySyslogNative;
const auto LegacyDeviceHandle = reinterpret_cast<NativeIdevice>( 0x1101 );
const auto LegacyLockdownHandle = reinterpret_cast<NativeLockdownClient>( 0x1202 );
const auto LegacyServiceHandle = reinterpret_cast<NativeServiceDescriptor>( 0x1303 );
const auto LegacySyslogHandle = reinterpret_cast<NativeSyslogRelayClient>( 0x1505 );

NativePairRecordResult legacyReadPairRecord( const char*, char** record, std::uint32_t* size )
{
    auto bytes = std::make_unique<char[]>( 1u );
    bytes[ 0 ] = 'p';
    *record = bytes.release();
    *size = 1u;
    return NativePairRecordResult::Present;
}

void legacyFreePairRecord( char* record )
{
    delete[] record;
}

std::int32_t legacyNewDevice( NativeIdevice* device, const char*, NativeConnectionOption )
{
    *device = LegacyDeviceHandle;
    return 0;
}

std::int32_t legacyFreeDevice( NativeIdevice )
{
    return 0;
}

std::int32_t legacyNewLockdown( NativeIdevice, NativeLockdownClient* client, const char* )
{
    *client = LegacyLockdownHandle;
    return 0;
}

std::int32_t legacyFreeLockdown( NativeLockdownClient )
{
    return 0;
}

std::int32_t legacyStartService( NativeLockdownClient, const char*,
                                 NativeServiceDescriptor* service )
{
    *service = LegacyServiceHandle;
    return 0;
}

std::int32_t legacyFreeService( NativeServiceDescriptor )
{
    return 0;
}

std::int32_t legacyGetString( NativeLockdownClient, const char*, const char* key, char** value )
{
    if ( key == nullptr || std::string{ key } != "ProductVersion" ) {
        return -1;
    }
    auto bytes = std::make_unique<char[]>( 4u );
    std::memcpy( bytes.get(), "8.4", 4u );
    *value = bytes.release();
    return 0;
}

void legacyFreeString( char* value )
{
    delete[] value;
}

std::int32_t legacyNewSyslog( NativeIdevice, NativeServiceDescriptor,
                              NativeSyslogRelayClient* client )
{
    *client = LegacySyslogHandle;
    return 0;
}

std::int32_t legacyStartSyslog( NativeSyslogRelayClient, NativeSyslogRelayCallback callback,
                                NativeSyslogRelayErrorCallback error, void* context )
{
    std::lock_guard<std::mutex> lock( legacySyslogNative.mutex );
    legacySyslogNative.callback = callback;
    legacySyslogNative.errorCallback = error;
    legacySyslogNative.context = context;
    legacySyslogNative.started = true;
    legacySyslogNative.changed.notify_all();
    return 0;
}

std::int32_t legacyStopSyslog( NativeSyslogRelayClient )
{
    return 0;
}

std::int32_t legacyFreeSyslog( NativeSyslogRelayClient )
{
    return 0;
}

IosNativeApi legacySyslogApi()
{
    IosNativeApi api{};
    api.deviceNewWithOptions = &legacyNewDevice;
    api.deviceFree = &legacyFreeDevice;
    api.lockdownClientNewWithExistingPair = &legacyNewLockdown;
    api.lockdownClientFree = &legacyFreeLockdown;
    api.lockdownStartService = &legacyStartService;
    api.serviceDescriptorFree = &legacyFreeService;
    api.lockdownGetStringValue = &legacyGetString;
    api.nativeStringFree = &legacyFreeString;
    api.readPairRecord = &legacyReadPairRecord;
    api.pairRecordFree = &legacyFreePairRecord;
    api.syslogRelayClientNew = &legacyNewSyslog;
    api.syslogRelayStart = &legacyStartSyslog;
    api.syslogRelayStop = &legacyStopSyslog;
    api.syslogRelayClientFree = &legacyFreeSyslog;
    return api;
}

IosNativeStreamConfig nativeConfig()
{
    IosNativeStreamConfig config;
    config.endpoint = IosEndpointKey{ "explicit-device", NativeConnectionType::Usb };
    config.ansiOutputEnabled = true;
    config.queueLimits = klogg::livecapture::LiveDataQueueLimits{ 1024u, 8u };
    config.servicePolicy = IosNativeServicePolicy::AutomaticByProductVersion;
    return config;
}

} // namespace

TEST_CASE( "Direct native iOS transport rejects every unsupported filter and JSON option",
           "[ios][native][transport][options][validation][w3-ios-options-red]" )
{
    struct UnsupportedOption {
        const char* name;
        std::function<void( IosNativeStreamConfig& )> apply;
    };
    const std::array cases{
        UnsupportedOption{ "level", []( IosNativeStreamConfig& config ) {
                              config.logOptions.level = "debug";
                          } },
        UnsupportedOption{ "category", []( IosNativeStreamConfig& config ) {
                              config.logOptions.categories = { "network" };
                          } },
        UnsupportedOption{ "subsystem", []( IosNativeStreamConfig& config ) {
                              config.logOptions.subsystem = "com.example.app";
                          } },
        UnsupportedOption{ "JSON", []( IosNativeStreamConfig& config ) {
                              config.logOptions.outputFormat = IosLogOutputFormat::Json;
                          } },
    };

    for ( const auto& value : cases ) {
        DYNAMIC_SECTION( value.name )
        {
            ScriptedWorkerFactory factory;
            auto config = nativeConfig();
            value.apply( config );
            IosNativeTransport transport( factory, std::move( config ) );
            std::vector<LiveSourceTransport::State> states;
            QObject::connect( &transport, &LiveSourceTransport::stateChanged,
                              [&states]( Generation, LiveSourceTransport::State state ) {
                                  states.push_back( state );
                              } );

            transport.start( 700u );

            CHECK( factory.sessions.empty() );
            REQUIRE( transport.lastStructuredError().has_value() );
            CHECK( transport.lastStructuredError()->code == "unsupported-ios-log-options" );
            CHECK( transport.lastStructuredError()->category == ErrorCategory::Configuration );
            CHECK( transport.lastStructuredError()->retryPolicy == RetryPolicy::Never );
            REQUIRE_FALSE( states.empty() );
            CHECK( states.back() == LiveSourceTransport::State::Error );
        }
    }
}

TEST_CASE( "Native queue notification retries one rejected queued dispatch without polling",
           "[ios][native][transport][queue][notification][w3-notification-red]" )
{
    ScriptedWorkerFactory factory;
    int dispatchAttempts = 0;
    IosNativeTransport::QueuedDispatcher dispatcher
        = [&dispatchAttempts]( QObject& context, IosNativeTransport::QueuedTask task ) {
              ++dispatchAttempts;
              if ( dispatchAttempts == 1 ) {
                  return false;
              }
              return QMetaObject::invokeMethod(
                  &context, [ task = std::move( task ) ]() mutable { task(); },
                  Qt::QueuedConnection );
          };
    IosNativeTransport transport( factory, nativeConfig(), std::move( dispatcher ) );
    QByteArray received;
    QObject::connect( &transport, &LiveSourceTransport::bytesReceived,
                      [&received]( Generation, const QByteArray& bytes ) { received += bytes; } );

    transport.start( 699u );
    factory.publishBytes( 0u, "sole-wakeup\n" );
    drainQtEvents();

    CHECK( dispatchAttempts == 2 );
    CHECK( received == QByteArrayLiteral( "sole-wakeup\n" ) );
    CHECK_FALSE( transport.lastStructuredError().has_value() );
}

TEST_CASE( "Native queue notification falls back after a bounded dispatcher rejection",
           "[ios][native][transport][queue][notification][fallback][w3-notification-red]" )
{
    ScriptedWorkerFactory factory;
    int dispatchAttempts = 0;
    IosNativeTransport::QueuedDispatcher dispatcher
        = [&dispatchAttempts]( QObject&, IosNativeTransport::QueuedTask ) {
              ++dispatchAttempts;
              return false;
          };
    IosNativeTransport transport( factory, nativeConfig(), std::move( dispatcher ) );
    QByteArray received;
    QObject::connect( &transport, &LiveSourceTransport::bytesReceived,
                      [&received]( Generation, const QByteArray& bytes ) { received += bytes; } );

    transport.start( 698u );
    factory.publishBytes( 0u, "fallback-wakeup\n" );
    drainQtEvents();

    CHECK( dispatchAttempts == 2 );
    CHECK( received == QByteArrayLiteral( "fallback-wakeup\n" ) );
    CHECK_FALSE( transport.lastStructuredError().has_value() );
}

TEST_CASE( "Native automatic retirement settles accepted tail in bounded deliveries before stopped",
           "[ios][native][transport][w2-tail-red]" )
{
    ScriptedWorkerFactory factory;
    auto config = nativeConfig();
    config.queueLimits.maxQueuedBytes = 512u * 1024u;
    IosNativeTransport transport( factory, config );
    std::size_t delivered = 0;
    std::size_t maximumDelivery = 0;
    unsigned stopped = 0;
    QObject::connect( &transport, &LiveSourceTransport::bytesReceived,
        [&]( auto, const QByteArray& bytes ) {
            delivered += static_cast<std::size_t>( bytes.size() );
            maximumDelivery = std::max( maximumDelivery, static_cast<std::size_t>( bytes.size() ) );
        } );
    QObject::connect( &transport, &LiveSourceTransport::stopped,
        [&]( auto, auto discarded ) {
            CHECK( delivered == 256u * 1024u );
            CHECK( discarded == 0u );
            ++stopped;
        } );
    transport.start( 704u );
    factory.publishBytes( 0u, std::string( 256u * 1024u, 'x' ), false );
    transport.requestStop( 704u, klogg::livecapture::StopDisposition::SettleAccepted );
    CHECK( stopped == 0u );
    factory.publishStopped( 0u );
    for ( unsigned turn = 0; turn < 10u; ++turn ) { drainQtEvents(); }
    CHECK( stopped == 1u );
    CHECK( delivered == 256u * 1024u );
    CHECK( maximumDelivery <= 64u * 1024u );
}

TEST_CASE( "Native retirement counts final pre-enqueue rejections exactly once",
           "[ios][native][transport][stop][statistics][review-rejected-admission]" )
{
    // Explicit settlement, explicit discard, and native automatic failure all
    // retire through the same final gap ledger, independently of accepted bytes.
    const auto stopPath = GENERATE( 0, 1, 2 );
    CAPTURE( stopPath );
    ScriptedWorkerFactory factory;
    auto config = nativeConfig();
    constexpr std::size_t acceptedBytes = 192u * 1024u;
    constexpr std::size_t rejectedBytes = 2u;
    constexpr Generation generation = 708u;
    config.queueLimits.maxQueuedBytes = acceptedBytes;
    IosNativeTransport transport( factory, config );
    std::size_t delivered = 0u;
    std::vector<std::pair<Generation, quint64>> settlements;
    QObject::connect( &transport, &LiveSourceTransport::bytesReceived,
                      [&]( Generation, const QByteArray& bytes ) {
                          delivered += static_cast<std::size_t>( bytes.size() );
                      } );
    QObject::connect( &transport, &LiveSourceTransport::stopped,
                      [&]( Generation value, quint64 discarded ) {
                          settlements.emplace_back( value, discarded );
                      } );

    transport.start( generation );
    const auto session = factory.latest();
    const auto callbacks = session->callbacks;
    factory.publishBytes( 0u, std::string( acceptedBytes, 'a' ), false );
    CHECK( transport.statistics().rejectedBeforeEnqueueBytes == 0u );
    if ( stopPath == 2 ) {
        factory.publishFailure( 0u, disconnectError() );
    }
    else {
        transport.requestStop( generation,
                               stopPath == 0
                                   ? klogg::livecapture::StopDisposition::SettleAccepted
                                   : klogg::livecapture::StopDisposition::DiscardPending );
    }
    CHECK( settlements.empty() );
    // The blocked callback only returns after stop closes its queue. These
    // counters become final at native stopped, not at the earlier stop request.
    session->rejectedBeforeEnqueueBytes = rejectedBytes;
    session->rejectedBeforeEnqueueChunks = 1u;
    factory.publishStopped( 0u );
    for ( unsigned turn = 0u; turn < 8u; ++turn ) { drainQtEvents(); }

    const auto discardedAcceptedBytes = stopPath == 1 ? acceptedBytes : 0u;
    REQUIRE( settlements.size() == 1u );
    CHECK( settlements.front().first == generation );
    CHECK( settlements.front().second
           == static_cast<quint64>( discardedAcceptedBytes + rejectedBytes ) );
    CHECK( delivered == acceptedBytes - discardedAcceptedBytes );
    CHECK( delivered + settlements.front().second == acceptedBytes + rejectedBytes );
    CHECK( session->destroyed );
    const auto statisticsCalls = session->statisticsCalls;
    callbacks.stopped( generation );
    callbacks.bytesAvailable( generation );
    drainQtEvents();
    CHECK( settlements.size() == 1u );
    CHECK( session->statisticsCalls == statisticsCalls );

    // A new one-shot session must not inherit the previous generation's gap.
    transport.start( generation + 1u );
    transport.requestStop( generation + 1u,
                           klogg::livecapture::StopDisposition::SettleAccepted );
    factory.publishStopped( 1u );
    drainQtEvents();
    REQUIRE( settlements.size() == 2u );
    CHECK( settlements.back().first == generation + 1u );
    CHECK( settlements.back().second == 0u );
}

TEST_CASE( "Native transport reports a quiescent legacy syslog partial exactly once",
           "[ios][native][transport][syslog][stop][partial][accounting][p0-red]" )
{
    auto& nativeState = legacySyslogNative;
    nativeState.reset();
    DefaultIosNativeStreamWorkerFactory workerFactory( legacySyslogApi() );
    IosNativeTransport transport( workerFactory, nativeConfig() );
    SafeQSignalSpy stoppedSpy( &transport, &LiveSourceTransport::stopped );
    constexpr Generation generation = 709u;
    const std::string partialRecord = "unterminated transport bytes";

    transport.start( generation );
    REQUIRE( nativeState.waitUntilStarted() );
    nativeState.emit( partialRecord );

    transport.requestStop( generation, klogg::livecapture::StopDisposition::SettleAccepted );
    REQUIRE( stoppedSpy.safeWait( 3000 ) );
    REQUIRE( stoppedSpy.count() == 1 );
    CHECK( stoppedSpy.at( 0 ).at( 0 ).toULongLong() == generation );
    CHECK( stoppedSpy.at( 0 ).at( 1 ).toULongLong()
           == static_cast<qulonglong>( partialRecord.size() ) );

    transport.requestStop( generation, klogg::livecapture::StopDisposition::SettleAccepted );
    drainQtEvents();
    CHECK( stoppedSpy.count() == 1 );
}

TEST_CASE( "Native retiring drain can be cancelled fairly after one bounded delivery",
           "[ios][native][transport][retire][drain][cancel][fairness][w3-ios-lifecycle]" )
{
    ScriptedWorkerFactory factory;
    auto config = nativeConfig();
    config.queueLimits.maxQueuedBytes = 512u * 1024u;
    IosNativeTransport transport( factory, config );
    constexpr Generation generation = 707u;
    constexpr std::size_t acceptedBytes = 192u * 1024u;
    constexpr std::size_t firstTurnBytes = 64u * 1024u;
    std::size_t delivered = 0u;
    unsigned deliveries = 0u;
    quint64 discarded = 0u;
    unsigned stopped = 0u;
    QObject::connect( &transport, &LiveSourceTransport::bytesReceived,
                      [&]( Generation, const QByteArray& bytes ) {
                          delivered += static_cast<std::size_t>( bytes.size() );
                          ++deliveries;
                          if ( deliveries == 1u ) {
                              transport.requestStop(
                                  generation,
                                  klogg::livecapture::StopDisposition::DiscardPending );
                          }
                      } );
    QObject::connect( &transport, &LiveSourceTransport::stopped,
                      [&]( Generation, quint64 value ) {
                          ++stopped;
                          discarded = value;
                      } );

    transport.start( generation );
    factory.publishBytes( 0u, std::string( acceptedBytes, 'r' ), false );
    transport.requestStop( generation,
                           klogg::livecapture::StopDisposition::SettleAccepted );
    factory.publishStopped( 0u );
    drainQtEvents();

    CHECK( deliveries == 1u );
    CHECK( delivered == firstTurnBytes );
    CHECK( stopped == 1u );
    CHECK( discarded == static_cast<quint64>( acceptedBytes - firstTurnBytes ) );
}

TEST_CASE( "Native retiring continuation failures still settle exactly once",
           "[ios][native][transport][w2-settlement-red]" )
{
    ScriptedWorkerFactory factory;
    auto config = nativeConfig();
    config.queueLimits.maxQueuedBytes = 512u * 1024u;
    IosNativeTransport transport( factory, config );
    QByteArray delivered;
    unsigned stopped = 0;
    quint64 discarded = 0u;
    QObject::connect( &transport, &LiveSourceTransport::bytesReceived,
                      [&]( Generation, const QByteArray& bytes ) { delivered += bytes; } );
    QObject::connect( &transport, &LiveSourceTransport::stopped,
                      [&]( Generation, quint64 value ) {
                          ++stopped;
                          discarded = value;
                      } );

    constexpr Generation generation = 706u;
    transport.start( generation );
    const auto session = factory.latest();
    const auto callbacks = session->callbacks;

    SECTION( "scheduled drain throws with a known accepted tail" )
    {
        const auto deliveredPrefix = std::string( 64u * 1024u, 'p' );
        const auto unsettledTail = std::string( 23u, 'u' );
        session->afterDrain = [&] { factory.publishBytes( 0u, unsettledTail, false ); };
        session->throwOnDrainCall = 2;
        factory.publishBytes( 0u, deliveredPrefix, false );

        transport.requestStop( generation,
                               klogg::livecapture::StopDisposition::SettleAccepted );
        factory.publishStopped( 0u );
        CHECK_NOTHROW( drainQtEvents() );

        CHECK( stopped == 1u );
        CHECK( delivered == QByteArray::fromStdString( deliveredPrefix ) );
        CHECK( discarded == static_cast<quint64>( unsettledTail.size() ) );
    }

    SECTION( "scheduled settlement statistics throws after the whole accepted tail" )
    {
        const auto accepted = std::string( 128u * 1024u, 's' );
        session->throwFromStatistics = true;
        factory.publishBytes( 0u, accepted, false );

        transport.requestStop( generation,
                               klogg::livecapture::StopDisposition::SettleAccepted );
        factory.publishStopped( 0u );
        CHECK_NOTHROW( drainQtEvents() );

        CHECK( stopped == 1u );
        CHECK( delivered == QByteArray::fromStdString( accepted ) );
        CHECK( discarded == 0u );
        REQUIRE( transport.lastStructuredError().has_value() );
        CHECK( transport.lastStructuredError()->nativeDetail
               == "The final native queue statistics were unavailable during retirement." );
    }

    REQUIRE( transport.lastStructuredError().has_value() );
    CHECK( transport.lastStructuredError()->category == ErrorCategory::Capture );
    CHECK( transport.lastStructuredError()->retryPolicy == RetryPolicy::Never );
    CHECK( session->destroyed );

    const auto drainCalls = session->drainCalls;
    const auto statisticsCalls = session->statisticsCalls;
    callbacks.bytesAvailable( generation );
    callbacks.stopped( generation );
    drainQtEvents();
    CHECK( stopped == 1u );
    CHECK( session->drainCalls == drainCalls );
    CHECK( session->statisticsCalls == statisticsCalls );
}

TEST_CASE( "Native drain preserves the sole refill wakeup while a sliced batch is pending",
           "[ios][native][transport][queue][w2-wakeup-red]" )
{
    ScriptedWorkerFactory factory;
    auto config = nativeConfig();
    config.queueLimits.maxQueuedBytes = 512u * 1024u;
    IosNativeTransport transport( factory, config );
    QByteArray received;
    QObject::connect( &transport, &LiveSourceTransport::bytesReceived,
                      [&]( Generation, const QByteArray& bytes ) { received += bytes; } );

    transport.start( 705u );
    REQUIRE( factory.sessions.size() == 1u );
    const auto oldBytes = std::string( 192u * 1024u, 'o' );
    factory.latest()->afterDrain = [&] { factory.publishBytes( 0u, "new\n" ); };
    factory.publishBytes( 0u, oldBytes );

    for ( int turn = 0; turn < 8; ++turn ) { drainQtEvents(); }

    CHECK( received.size() == static_cast<int>( oldBytes.size() + 4u ) );
    CHECK( received.left( static_cast<int>( oldBytes.size() ) )
           == QByteArray::fromStdString( oldBytes ) );
    CHECK( received.endsWith( QByteArrayLiteral( "new\n" ) ) );
    CHECK( factory.latest()->queue.statistics().queuedBytes == 0u );
}

TEST_CASE( "Native stop waits for worker admission release before publishing disconnection",
           "[ios][native][transport][w2-native-red]" )
{
    ScriptedWorkerFactory factory;
    IosNativeTransport transport( factory, nativeConfig() );
    unsigned disconnected = 0;
    QObject::connect( &transport, &LiveSourceTransport::stateChanged,
        [&]( auto, auto state ) {
            if ( state == LiveSourceTransport::State::Disconnected ) { ++disconnected; }
        } );
    transport.start( 701u );
    transport.stop( 701u );
    drainQtEvents();
    CHECK( disconnected == 0u );
    CHECK_FALSE( factory.sessions.front()->destroyed );
    transport.start( 702u );
    CHECK( factory.sessions.size() == 1u );
    factory.publishStopped( 0u );
    drainQtEvents();
    CHECK( disconnected == 1u );
    CHECK( factory.sessions.front()->destroyed );
    CHECK( factory.sessions.size() == 2u );
}

TEST_CASE( "Native drain exceptions preserve a useful terminal capture diagnostic",
           "[ios][native][transport][w2-native-red]" )
{
    ScriptedWorkerFactory factory;
    IosNativeTransport transport( factory, nativeConfig() );
    transport.start( 703u );
    factory.latest()->throwFromDrain = true;
    factory.publishBytes( 0u, "accepted-tail\n" );
    CHECK_NOTHROW( drainQtEvents() );
    REQUIRE( transport.lastStructuredError().has_value() );
    CHECK( transport.lastStructuredError()->category == ErrorCategory::Capture );
    CHECK( transport.lastStructuredError()->retryPolicy == RetryPolicy::Never );
}

TEST_CASE( "iOS native transport is Connecting until service handle and read are ready",
           "[ios][native][transport][readiness][idle]" )
{
    ScriptedWorkerFactory factory;
    IosNativeTransport transport( factory, nativeConfig() );
    std::vector<std::pair<Generation, LiveSourceTransport::State>> states;
    QObject::connect( &transport, &LiveSourceTransport::stateChanged,
                      [ &states ]( Generation generation, LiveSourceTransport::State state ) {
                          states.emplace_back( generation, state );
                      } );

    transport.start( 101u );
    REQUIRE( factory.sessions.size() == 1u );
    CHECK( factory.latest()->config.generation == 101u );
    CHECK( factory.latest()->startCalls == 1 );
    REQUIRE( states.size() == 1u );
    CHECK( states.back()
           == std::make_pair( Generation{ 101u }, LiveSourceTransport::State::Connecting ) );

    factory.publishReady( 0u );
    drainQtEvents();
    REQUIRE( states.size() == 2u );
    CHECK( states.back()
           == std::make_pair( Generation{ 101u }, LiveSourceTransport::State::Connected ) );

    // No first-byte watchdog exists: a quiet but armed stream stays connected.
    drainQtEvents();
    CHECK( states.back().second == LiveSourceTransport::State::Connected );
}

TEST_CASE( "iOS native transport preserves typed factory rejection diagnostics",
           "[ios][native][transport][admission][backoff]" )
{
    RejectingWorkerFactory factory;
    IosNativeTransport transport( factory, nativeConfig() );

    transport.start( 104u );

    REQUIRE( transport.lastStructuredError().has_value() );
    CHECK( transport.lastStructuredError()->code == "ios-native-test-rejection" );
    CHECK( transport.lastStructuredError()->retryPolicy == RetryPolicy::Backoff );
}

TEST_CASE( "iOS native transport tolerates synchronous stop from readiness state handlers",
           "[ios][native][transport][reentrant][stop]" )
{
    SECTION( "Connecting handler stops before worker creation continues" )
    {
        ScriptedWorkerFactory factory;
        IosNativeTransport transport( factory, nativeConfig() );
        QObject::connect( &transport, &LiveSourceTransport::stateChanged, &transport,
                          [ & ]( Generation generation, LiveSourceTransport::State state ) {
                              if ( state == LiveSourceTransport::State::Connecting ) {
                                  transport.stop( generation );
                              }
                          } );
        transport.start( 105u );
        CHECK( factory.sessions.empty() );
        CHECK( transport.lastError().isEmpty() );
    }

    SECTION( "Connected handler retires the one-shot worker" )
    {
        ScriptedWorkerFactory factory;
        IosNativeTransport transport( factory, nativeConfig() );
        QObject::connect( &transport, &LiveSourceTransport::stateChanged, &transport,
                          [ & ]( Generation generation, LiveSourceTransport::State state ) {
                              if ( state == LiveSourceTransport::State::Connected ) {
                                  transport.stop( generation );
                              }
                          } );
        transport.start( 106u );
        REQUIRE( factory.sessions.size() == 1u );
        factory.publishReady( 0u );
        drainQtEvents();
        CHECK( factory.sessions.front()->stopCalls == 1 );
    }
}

TEST_CASE( "iOS native transport drains records while native startup is still pending",
           "[ios][native][transport][backpressure][readiness]" )
{
    ScriptedWorkerFactory factory;
    auto config = nativeConfig();
    config.queueLimits = klogg::livecapture::LiveDataQueueLimits{ 2u, 1u };
    IosNativeTransport transport( factory, config );
    QByteArray received;
    std::vector<LiveSourceTransport::State> states;
    QObject::connect( &transport, &LiveSourceTransport::bytesReceived,
                      [ &received ]( Generation, const QByteArray& bytes ) { received += bytes; } );
    QObject::connect( &transport, &LiveSourceTransport::stateChanged,
                      [ &states ]( Generation, LiveSourceTransport::State state ) {
                          states.push_back( state );
                      } );

    transport.start( 105u );
    for ( const auto* record : { "a\n", "b\n", "c\n" } ) {
        factory.publishBytes( 0u, record );
        drainQtEvents();
        CHECK( factory.latest()->queue.statistics().queuedBytes == 0u );
    }
    CHECK( received == "a\nb\nc\n" );
    REQUIRE( states.size() == 1u );
    CHECK( states.back() == LiveSourceTransport::State::Connecting );

    factory.publishReady( 0u );
    drainQtEvents();
    CHECK( states.back() == LiveSourceTransport::State::Connected );
}

TEST_CASE( "iOS native transport yields between replenished batches so stop is not starved",
           "[ios][native][transport][backpressure][fairness][stop]" )
{
    ScriptedWorkerFactory factory;
    IosNativeTransport transport( factory, nativeConfig() );
    QByteArray received;
    int deliveredBatches = 0;
    QObject::connect( &transport, &LiveSourceTransport::bytesReceived, &transport,
                      [ & ]( Generation generation, const QByteArray& bytes ) {
                          received += bytes;
                          ++deliveredBatches;
                          if ( deliveredBatches == 1 ) {
                              QMetaObject::invokeMethod(
                                  &transport, [ &transport, generation ] { transport.stop( generation ); },
                                  Qt::QueuedConnection );
                          }
                          // Model the native producer resuming as soon as drain frees capacity.
                          // Bound the old busy-loop behavior so RED fails rather than hangs.
                          if ( deliveredBatches < 4 ) {
                              factory.publishBytes( 0u, "next\n" );
                          }
                      } );

    transport.start( 106u );
    factory.publishReady( 0u );
    drainQtEvents();
    factory.publishBytes( 0u, "first\n" );
    drainQtEvents();

    CHECK( deliveredBatches == 1 );
    CHECK( received == "first\n" );
    CHECK( factory.latest()->stopCalls == 1 );
    CHECK( transport.lastError().isEmpty() );
}

TEST_CASE( "iOS native transport marshals worker callbacks back to its Qt thread",
           "[ios][native][transport][threading]" )
{
    ScriptedWorkerFactory factory;
    IosNativeTransport transport( factory, nativeConfig() );
    const auto guiThread = std::this_thread::get_id();
    std::optional<std::thread::id> observerThread;
    QObject::connect( &transport, &LiveSourceTransport::stateChanged,
                      [ &observerThread ]( Generation, LiveSourceTransport::State state ) {
                          if ( state == LiveSourceTransport::State::Connected ) {
                              observerThread = std::this_thread::get_id();
                          }
                      } );
    transport.start( 110u );

    std::thread nativeWorker( [ &factory ] { factory.publishReady( 0u ); } );
    nativeWorker.join();
    CHECK_FALSE( observerThread.has_value() );
    drainQtEvents();

    REQUIRE( observerThread.has_value() );
    CHECK( observerThread.value() == guiThread );
}

TEST_CASE( "iOS native transport contains exceptions at the queued Qt callback boundary",
           "[ios][native][transport][threading][exception-boundary]" )
{
    ScriptedWorkerFactory factory;
    IosNativeTransport transport( factory, nativeConfig() );
    transport.start( 111u );
    REQUIRE( factory.sessions.size() == 1u );
    factory.sessions.front()->throwFromDrain = true;

    factory.sessions.front()->callbacks.bytesAvailable( 111u );
    CHECK_NOTHROW( drainQtEvents() );
}

TEST_CASE( "iOS native transport drains queued bytes before publishing terminal state",
           "[ios][native][transport][drain][ordering][error]" )
{
    ScriptedWorkerFactory factory;
    IosNativeTransport transport( factory, nativeConfig() );
    std::vector<std::string> events;
    std::vector<LiveSourceTransport::State> states;
    QObject::connect( &transport, &LiveSourceTransport::bytesReceived,
                      [ &events ]( Generation, const QByteArray& bytes ) {
                          events.push_back( "bytes:" + bytes.toStdString() );
                      } );
    QObject::connect( &transport, &LiveSourceTransport::stateChanged,
                      [ &events, &states ]( Generation, LiveSourceTransport::State state ) {
                          states.push_back( state );
                          if ( state == LiveSourceTransport::State::Error ) {
                              events.push_back( "state:error" );
                          }
                      } );
    QObject::connect( &transport, &LiveSourceTransport::errorOccurred,
                      [ &events ]( Generation, const QString& error ) {
                          events.push_back( "error:" + error.toStdString() );
                      } );

    transport.start( 102u );
    factory.publishReady( 0u );
    drainQtEvents();
    factory.publishBytes( 0u, "ordered\n", false );
    factory.publishFailure( 0u, disconnectError( "USB cable removed" ) );
    drainQtEvents();

    REQUIRE( events.size() == 3u );
    CHECK( events.at( 0 ) == "bytes:ordered\n" );
    CHECK( events.at( 1 ) == "state:error" );
    CHECK( events.at( 2 ).find( "USB cable removed" ) != std::string::npos );
    REQUIRE( transport.lastStructuredError().has_value() );
    CHECK( transport.lastStructuredError()->code == "ios-device-disconnected" );
    CHECK( transport.lastStructuredError()->nativeDetail == "USB cable removed" );

    factory.publishStopped( 0u );
    drainQtEvents();
    REQUIRE_FALSE( states.empty() );
    CHECK( states.back() == LiveSourceTransport::State::Error );
}

TEST_CASE( "iOS native transport preserves the terminal diagnostic through reentrant stop",
           "[ios][native][transport][drain][ordering][error][reentrant][generation]" )
{
    ScriptedWorkerFactory factory;
    IosNativeTransport transport( factory, nativeConfig() );
    constexpr Generation generation = 107u;
    const auto expectedText
        = QStringLiteral( "The iOS device disconnected.\nUSB endpoint vanished" );
    std::vector<std::string> events;
    std::vector<LiveSourceError> structuredAtError;
    std::vector<std::pair<Generation, QString>> diagnostics;

    QObject::connect( &transport, &LiveSourceTransport::bytesReceived,
                      [ &events ]( Generation observedGeneration, const QByteArray& bytes ) {
                          events.push_back( "bytes:" + std::to_string( observedGeneration ) + ":"
                                            + bytes.toStdString() );
                      } );
    QObject::connect(
        &transport, &LiveSourceTransport::stateChanged, &transport,
        [ & ]( Generation observedGeneration, LiveSourceTransport::State state ) {
            if ( observedGeneration == generation && state == LiveSourceTransport::State::Error ) {
                events.push_back( "state:error" );
                REQUIRE( transport.lastStructuredError().has_value() );
                structuredAtError.push_back( *transport.lastStructuredError() );
                CHECK( transport.lastError() == expectedText );
                transport.stop( observedGeneration );
            }
        } );
    QObject::connect( &transport, &LiveSourceTransport::errorOccurred,
                      [ & ]( Generation observedGeneration, const QString& diagnostic ) {
                          events.push_back( "diagnostic" );
                          diagnostics.emplace_back( observedGeneration, diagnostic );
                      } );

    transport.start( generation );
    REQUIRE( factory.sessions.size() == 1u );
    factory.publishBytes( 0u, "before-terminal\n", false );
    factory.publishFailure( 0u, disconnectError( "USB endpoint vanished" ) );
    drainQtEvents();

    REQUIRE_FALSE( events.empty() );
    CHECK( events.front() == "bytes:107:before-terminal\n" );
    REQUIRE( structuredAtError.size() == 1u );
    CHECK( structuredAtError.front().code == "ios-device-disconnected" );
    CHECK( structuredAtError.front().retryPolicy == RetryPolicy::WaitForDevice );
    CHECK( structuredAtError.front().nativeDetail == "USB endpoint vanished" );
    CHECK( diagnostics
           == std::vector<std::pair<Generation, QString>>{ { generation, expectedText } } );
    CHECK( factory.sessions.front()->stopCalls == 1 );

    const auto eventCount = events.size();
    const auto diagnosticCount = diagnostics.size();
    const auto callbacks = factory.sessions.front()->callbacks;
    factory.publishBytes( 0u, "stale-after-stop\n", false );
    callbacks.ready( generation );
    callbacks.bytesAvailable( generation );
    callbacks.failed( generation, disconnectError( "late stale failure" ) );
    callbacks.stopped( generation );
    drainQtEvents();

    CHECK( events.size() == eventCount );
    CHECK( diagnostics.size() == diagnosticCount );
    CHECK( structuredAtError.size() == 1u );
}

TEST_CASE( "iOS native transport retires a terminal session without erasing Error state",
           "[ios][native][transport][terminal][cleanup][state]" )
{
    ScriptedWorkerFactory factory;
    IosNativeTransport transport( factory, nativeConfig() );
    std::vector<LiveSourceTransport::State> states;
    QObject::connect( &transport, &LiveSourceTransport::stateChanged,
                      [ & ]( Generation generation, LiveSourceTransport::State state ) {
                          if ( generation == 103u ) {
                              states.push_back( state );
                          }
                      } );

    transport.start( 103u );
    factory.publishReady( 0u );
    factory.publishFailure( 0u, disconnectError( "terminal cleanup" ) );
    factory.publishStopped( 0u );
    drainQtEvents();

    REQUIRE_FALSE( states.empty() );
    CHECK( states.back() == LiveSourceTransport::State::Error );
    CHECK( factory.sessions.front()->destroyed );
    CHECK( transport.lastStructuredError().has_value() );
}

TEST_CASE( "iOS native transport settles a multi-turn accepted tail before terminal Error",
           "[ios][native][transport][terminal][tail][lifetime][review-red]" )
{
    ScriptedWorkerFactory factory;
    auto config = nativeConfig();
    config.queueLimits.maxQueuedBytes = 256u * 1024u;
    auto transport = std::make_unique<IosNativeTransport>( factory, config );
    QPointer<IosNativeTransport> guard( transport.get() );
    constexpr Generation generation = 119u;
    const auto accepted = std::string( 128u * 1024u, 't' );
    std::size_t delivered = 0u;
    std::vector<QString> order;
    QObject::connect( transport.get(), &LiveSourceTransport::bytesReceived,
                      [&]( Generation, const QByteArray& bytes ) {
                          delivered += static_cast<std::size_t>( bytes.size() );
                          order.push_back( QStringLiteral( "bytes" ) );
                      } );
    QObject::connect( transport.get(), &LiveSourceTransport::stateChanged,
                      [&]( Generation, LiveSourceTransport::State state ) {
                          if ( state == LiveSourceTransport::State::Error ) {
                              order.push_back( QStringLiteral( "error" ) );
                              transport.reset();
                          }
                      } );

    transport->start( generation );
    factory.publishBytes( 0u, accepted, false );
    factory.publishFailure( 0u, disconnectError( "failure after accepted tail" ) );
    drainQtEvents();

    CHECK( guard.isNull() );
    CHECK( delivered == accepted.size() );
    REQUIRE( order.size() == 3u );
    CHECK( order.at( 0 ) == QStringLiteral( "bytes" ) );
    CHECK( order.at( 1 ) == QStringLiteral( "bytes" ) );
    CHECK( order.at( 2 ) == QStringLiteral( "error" ) );
}

TEST_CASE( "iOS native transport survives synchronous destruction from terminal error handlers",
           "[ios][native][transport][reentrant][destroy][error]" )
{
    ScriptedWorkerFactory factory;
    auto transport = std::make_unique<IosNativeTransport>( factory, nativeConfig() );
    QPointer<IosNativeTransport> guard( transport.get() );
    QObject::connect( transport.get(), &LiveSourceTransport::stateChanged, transport.get(),
                      [ & ]( Generation, LiveSourceTransport::State state ) {
                          if ( state == LiveSourceTransport::State::Error ) {
                              transport.reset();
                          }
                      } );
    transport->start( 120u );
    REQUIRE( factory.sessions.size() == 1u );
    factory.publishFailure( 0u, disconnectError( "destroy in error handler" ) );
    drainQtEvents();

    CHECK( guard.isNull() );
    CHECK( factory.sessions.front()->shutdownCalls == 1 );
    CHECK( factory.sessions.front()->destroyed );
}

TEST_CASE( "iOS native delivery callbacks survive synchronous transport destruction",
           "[ios][native][transport][drain][lifetime][reentrant]" )
{
    SECTION( "active scheduled continuation" )
    {
        ScriptedWorkerFactory factory;
        auto config = nativeConfig();
        config.queueLimits.maxQueuedBytes = 256u * 1024u;
        auto transport = std::make_unique<IosNativeTransport>( factory, config );
        QPointer<IosNativeTransport> guard( transport.get() );
        int deliveries = 0;
        QObject::connect( transport.get(), &LiveSourceTransport::bytesReceived,
                          [&]( Generation, const QByteArray& ) {
                              if ( ++deliveries == 2 ) { transport.reset(); }
                          } );
        transport->start( 121u );
        factory.publishBytes( 0u, std::string( 128u * 1024u, 'a' ) );

        CHECK_NOTHROW( drainQtEvents() );
        CHECK( guard.isNull() );
        CHECK( deliveries == 2 );
    }

    SECTION( "retiring scheduled continuation" )
    {
        ScriptedWorkerFactory factory;
        auto config = nativeConfig();
        config.queueLimits.maxQueuedBytes = 256u * 1024u;
        auto transport = std::make_unique<IosNativeTransport>( factory, config );
        QPointer<IosNativeTransport> guard( transport.get() );
        int deliveries = 0;
        QObject::connect( transport.get(), &LiveSourceTransport::bytesReceived,
                          [&]( Generation, const QByteArray& ) {
                              if ( ++deliveries == 2 ) { transport.reset(); }
                          } );
        transport->start( 122u );
        factory.publishBytes( 0u, std::string( 128u * 1024u, 'r' ), false );
        transport->requestStop( 122u, klogg::livecapture::StopDisposition::SettleAccepted );
        factory.publishStopped( 0u );

        CHECK_NOTHROW( drainQtEvents() );
        CHECK( guard.isNull() );
        CHECK( deliveries == 2 );
    }

    SECTION( "post-stopped direct delivery" )
    {
        ScriptedWorkerFactory factory;
        auto transport = std::make_unique<IosNativeTransport>( factory, nativeConfig() );
        QPointer<IosNativeTransport> guard( transport.get() );
        int deliveries = 0;
        QObject::connect( transport.get(), &LiveSourceTransport::bytesReceived,
                          [&]( Generation, const QByteArray& ) {
                              ++deliveries;
                              transport.reset();
                          } );
        transport->start( 123u );
        factory.publishBytes( 0u, "retiring-tail\n", false );
        transport->requestStop( 123u, klogg::livecapture::StopDisposition::SettleAccepted );
        factory.publishStopped( 0u );

        CHECK_NOTHROW( drainQtEvents() );
        CHECK( guard.isNull() );
        CHECK( deliveries == 1 );
    }
}

TEST_CASE( "iOS native transport rejects every callback from retired generations",
           "[ios][native][transport][generation][stale]" )
{
    ScriptedWorkerFactory factory;
    IosNativeTransport transport( factory, nativeConfig() );
    std::vector<std::pair<Generation, QByteArray>> bytes;
    std::vector<std::pair<Generation, LiveSourceTransport::State>> states;
    std::vector<std::pair<Generation, QString>> errors;
    QObject::connect( &transport, &LiveSourceTransport::bytesReceived,
                      [ &bytes ]( Generation generation, const QByteArray& value ) {
                          bytes.emplace_back( generation, value );
                      } );
    QObject::connect( &transport, &LiveSourceTransport::stateChanged,
                      [ &states ]( Generation generation, LiveSourceTransport::State state ) {
                          states.emplace_back( generation, state );
                      } );
    QObject::connect( &transport, &LiveSourceTransport::errorOccurred,
                      [ &errors ]( Generation generation, const QString& value ) {
                          errors.emplace_back( generation, value );
                      } );

    transport.start( 201u );
    const auto retiredCallbacks = factory.sessions.at( 0 )->callbacks;
    transport.start( 202u );
    REQUIRE( factory.sessions.size() == 1u );
    factory.publishStopped( 0u );
    drainQtEvents();
    REQUIRE( factory.sessions.size() == 2u );
    transport.stop( 201u );
    CHECK( factory.sessions.at( 1 )->stopCalls == 0 );
    const auto stateCount = states.size();

    retiredCallbacks.ready( 201u );
    factory.publishBytes( 0u, "stale\n", false );
    retiredCallbacks.bytesAvailable( 201u );
    retiredCallbacks.failed( 201u, disconnectError( "stale" ) );
    retiredCallbacks.stopped( 201u );
    drainQtEvents();

    CHECK( bytes.empty() );
    CHECK( errors.empty() );
    CHECK( states.size() == stateCount );
    CHECK( factory.sessions.at( 0 )->destroyed );

    factory.publishReady( 1u );
    factory.publishBytes( 1u, "current\n" );
    drainQtEvents();
    REQUIRE( bytes.size() == 1u );
    CHECK( bytes.front().first == 202u );
    CHECK( bytes.front().second == QByteArrayLiteral( "current\n" ) );
}

TEST_CASE( "iOS native transport stop is idempotent returns promptly and retires one-shot client",
           "[ios][native][transport][stop][deadline][recreate]" )
{
    ScriptedWorkerFactory factory;
    IosNativeTransport transport( factory, nativeConfig() );
    std::vector<std::pair<Generation, LiveSourceTransport::State>> states;
    QObject::connect( &transport, &LiveSourceTransport::stateChanged,
                      [ &states ]( Generation generation, LiveSourceTransport::State state ) {
                          states.emplace_back( generation, state );
                      } );

    transport.start( 301u );
    QElapsedTimer elapsed;
    elapsed.start();
    transport.stop( 301u );
    const auto stopElapsedMs = elapsed.elapsed();
    transport.stop( 301u );

    CHECK( stopElapsedMs < 100 );
    CHECK( factory.sessions.at( 0 )->stopCalls == 1 );
    factory.publishStopped( 0u );
    drainQtEvents();
    CHECK(
        std::count( states.cbegin(), states.cend(),
                    std::make_pair( Generation{ 301u }, LiveSourceTransport::State::Disconnected ) )
        == 1 );

    transport.start( 302u );
    REQUIRE( factory.sessions.size() == 2u );
    CHECK( factory.sessions.at( 1 )->config.generation == 302u );
    CHECK( factory.sessions.at( 1 )->startCalls == 1 );
}

TEST_CASE( "iOS native transport reconnects after device loss with a fresh correlated run",
           "[ios][native][transport][reconnect][device-loss]" )
{
    ScriptedWorkerFactory factory;
    IosNativeTransport transport( factory, nativeConfig() );
    transport.start( 401u );
    factory.publishReady( 0u );
    factory.publishFailure( 0u, disconnectError() );
    drainQtEvents();
    REQUIRE( transport.lastStructuredError().has_value() );
    CHECK( transport.lastStructuredError()->retryPolicy == RetryPolicy::WaitForDevice );

    transport.start( 402u );
    REQUIRE( factory.sessions.size() == 1u );
    factory.publishStopped( 0u );
    drainQtEvents();
    REQUIRE( factory.sessions.size() == 2u );
    factory.publishReady( 1u );
    drainQtEvents();
    CHECK_FALSE( transport.lastStructuredError().has_value() );
}

TEST_CASE( "iOS remote clear is asynchronous structured unsupported and never fake success",
           "[ios][native][transport][clear][unsupported]" )
{
    ScriptedWorkerFactory factory;
    IosNativeTransport transport( factory, nativeConfig() );
    SafeQSignalSpy clearSpy( &transport, &LiveSourceTransport::clearRemoteFinished );

    transport.clearRemoteAsync( 501u, 9001u );
    CHECK( clearSpy.count() == 0 );
    drainQtEvents();

    REQUIRE( clearSpy.count() == 1 );
    CHECK( clearSpy.at( 0 ).at( 0 ).toULongLong() == 501u );
    CHECK( clearSpy.at( 0 ).at( 1 ).toULongLong() == 9001u );
    CHECK_FALSE( clearSpy.at( 0 ).at( 2 ).toBool() );
    CHECK( clearSpy.at( 0 ).at( 3 ).toString().contains( QStringLiteral( "not supported" ),
                                                         Qt::CaseInsensitive ) );
    REQUIRE( transport.lastStructuredError().has_value() );
    CHECK( transport.lastStructuredError()->code == "ios-clear-unsupported" );
    CHECK( transport.lastStructuredError()->retryPolicy == RetryPolicy::Never );
}

TEST_CASE( "iOS native transport rejects stale clear completion after a newer start",
           "[ios][native][transport][clear][generation][stale]" )
{
    ScriptedWorkerFactory factory;
    IosNativeTransport transport( factory, nativeConfig() );
    SafeQSignalSpy clearSpy( &transport, &LiveSourceTransport::clearRemoteFinished );
    transport.start( 701u );
    transport.start( 702u );
    transport.clearRemoteAsync( 701u, 33u );
    drainQtEvents();

    CHECK( clearSpy.count() == 0 );
    CHECK_FALSE( transport.lastStructuredError().has_value() );
}

TEST_CASE( "iOS native transport shutdown retires its worker tree without callback reentry",
           "[ios][native][transport][shutdown][lifetime]" )
{
    ScriptedWorkerFactory factory;
    auto transport = std::make_unique<IosNativeTransport>( factory, nativeConfig() );
    transport->start( 601u );
    REQUIRE( factory.sessions.size() == 1u );
    const auto session = factory.sessions.front();

    transport.reset();
    drainQtEvents();

    CHECK( session->shutdownCalls == 1 );
    CHECK( session->destroyed );
}
