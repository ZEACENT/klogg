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
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

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
        if ( state_->throwFromDrain ) {
            throw std::runtime_error( "scripted drain failure" );
        }
        return state_->queue.drain();
    }

    LiveDataStatistics statistics() const override
    {
        return state_->queue.statistics();
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
