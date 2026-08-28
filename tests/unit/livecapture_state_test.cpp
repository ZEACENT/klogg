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

#include <array>
#include <chrono>
#include <utility>

#include "livestate.h"

namespace {
using namespace klogg::livecapture;
using namespace std::chrono_literals;

constexpr Timestamp at( std::int64_t milliseconds )
{
    return Timestamp{ milliseconds };
}

const LiveStateConfig defaultConfig{ 5u, 10s };

template <typename Event>
LiveStateTransition dispatch( const LiveStateSnapshot& snapshot, Event event,
                              const LiveStateConfig& config = defaultConfig )
{
    return reduce( snapshot, LiveStateEvent{ std::move( event ) }, config );
}

LiveSourceError makeError( ErrorCategory category, ErrorScope scope, RetryPolicy policy,
                           std::string code = "test-error" )
{
    return LiveSourceError{ category, std::move( code ),    scope,
                            policy,   "actionable message", "native detail" };
}

LiveStateSnapshot waitingForDeviceState( InfrastructureOwnership ownership
                                         = InfrastructureOwnership::ExternalShared )
{
    auto state = dispatch( initialLiveState(), StartRequested{ at( 10 ) } ).snapshot;
    state = dispatch( state,
                      InfrastructureChanged{ InfrastructureStatus::Ready, ownership, at( 20 ) } )
                .snapshot;
    return state;
}

LiveStateSnapshot openingState()
{
    auto state = waitingForDeviceState();
    return dispatch( state, DeviceAvailable{ state.generation, at( 30 ) } ).snapshot;
}

LiveStateSnapshot streamingState()
{
    auto state = openingState();
    state = dispatch( state, ProtocolServiceReady{ state.generation, at( 40 ) } ).snapshot;
    state = dispatch( state, StreamHandleOpened{ state.generation, at( 50 ) } ).snapshot;
    state = dispatch( state, StreamReadArmed{ state.generation, at( 60 ) } ).snapshot;
    return state;
}

bool hasEffect( const LiveStateTransition& transition, EffectKind kind )
{
    for ( const auto& effect : transition.effects ) {
        if ( effect.kind == kind ) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE( "live state keeps run intent, infrastructure, source, and capture orthogonal",
           "[livecapture][state]" )
{
    auto state = initialLiveState();
    REQUIRE( state.runIntent == RunIntent::Stopped );
    REQUIRE( state.infrastructure.status == InfrastructureStatus::Unknown );
    REQUIRE_FALSE( state.infrastructure.ownership.has_value() );
    REQUIRE( state.source.status == SourceStatus::Stopped );
    REQUIRE( state.source.stopReason == StopReason::NeverStarted );
    REQUIRE( state.capture == CaptureState::OpenHealthy );

    state = dispatch( state, StartRequested{ at( 10 ) } ).snapshot;
    REQUIRE( state.runIntent == RunIntent::Running );
    REQUIRE( state.infrastructure.status == InfrastructureStatus::Unknown );
    REQUIRE( state.source.status == SourceStatus::WaitingForInfrastructure );
    REQUIRE( state.capture == CaptureState::OpenHealthy );

    state = dispatch( state, InfrastructureChanged{ InfrastructureStatus::Connecting,
                                                    InfrastructureOwnership::AppShared, at( 20 ) } )
                .snapshot;
    REQUIRE( state.runIntent == RunIntent::Running );
    REQUIRE( state.infrastructure.status == InfrastructureStatus::Connecting );
    REQUIRE( state.infrastructure.ownership == InfrastructureOwnership::AppShared );
    REQUIRE( state.source.status == SourceStatus::WaitingForInfrastructure );
    REQUIRE( state.capture == CaptureState::OpenHealthy );

    state = dispatch( state, InfrastructureChanged{ InfrastructureStatus::Ready,
                                                    InfrastructureOwnership::AppShared, at( 30 ) } )
                .snapshot;
    REQUIRE( state.runIntent == RunIntent::Running );
    REQUIRE( state.infrastructure.status == InfrastructureStatus::Ready );
    REQUIRE( state.infrastructure.ownership == InfrastructureOwnership::AppShared );
    REQUIRE( state.source.status == SourceStatus::WaitingForDevice );
    REQUIRE( state.capture == CaptureState::OpenHealthy );

    state = dispatch( state, InfrastructureChanged{ InfrastructureStatus::Stopping,
                                                    InfrastructureOwnership::AppShared, at( 40 ) } )
                .snapshot;
    REQUIRE( state.runIntent == RunIntent::Running );
    REQUIRE( state.infrastructure.status == InfrastructureStatus::Stopping );
    REQUIRE( state.infrastructure.ownership == InfrastructureOwnership::AppShared );
    REQUIRE( state.source.status == SourceStatus::WaitingForInfrastructure );

    state = dispatch( state, InfrastructureChanged{ InfrastructureStatus::Unavailable,
                                                    InfrastructureOwnership::AppShared, at( 50 ) } )
                .snapshot;
    REQUIRE( state.runIntent == RunIntent::Running );
    REQUIRE( state.infrastructure.status == InfrastructureStatus::Unavailable );
    REQUIRE( state.infrastructure.ownership == InfrastructureOwnership::AppShared );
    REQUIRE( state.source.status == SourceStatus::WaitingForInfrastructure );
    REQUIRE( state.capture == CaptureState::OpenHealthy );
}

TEST_CASE( "terminal infrastructure failure leaves a running tab in Failed",
           "[livecapture][state]" )
{
    auto state = dispatch( initialLiveState(), StartRequested{ at( 10 ) } ).snapshot;
    state = dispatch( state,
                      InfrastructureChanged{ InfrastructureStatus::Unavailable,
                                             InfrastructureOwnership::AppShared, at( 20 ) } )
                .snapshot;
    const auto error
        = makeError( ErrorCategory::Infrastructure, ErrorScope::Infrastructure,
                     RetryPolicy::Never, "adb-helper-unavailable" );

    const auto failed
        = dispatch( state, InfrastructureFailed{ state.generation, error, at( 30 ) } );

    REQUIRE( failed.accepted );
    REQUIRE( failed.snapshot.source.status == SourceStatus::Failed );
    REQUIRE( failed.snapshot.source.failure.has_value() );
    CHECK( failed.snapshot.source.failure->code == "adb-helper-unavailable" );
    CHECK( projectLiveState( failed.snapshot ).failureMessage == "actionable message" );
}

TEST_CASE( "source state belongs to each tab while infrastructure ownership is shared",
           "[livecapture][state]" )
{
    const auto sharedBaseline = waitingForDeviceState( InfrastructureOwnership::ExternalShared );
    auto firstTab = sharedBaseline;
    const auto secondTab = sharedBaseline;

    firstTab = dispatch( firstTab, DeviceAvailable{ firstTab.generation, at( 40 ) } ).snapshot;

    REQUIRE( firstTab.source.status == SourceStatus::OpeningStream );
    REQUIRE( secondTab.source.status == SourceStatus::WaitingForDevice );
    REQUIRE( firstTab.infrastructure.status == InfrastructureStatus::Ready );
    REQUIRE( secondTab.infrastructure.status == InfrastructureStatus::Ready );
    REQUIRE( firstTab.infrastructure.ownership == InfrastructureOwnership::ExternalShared );
    REQUIRE( secondTab.infrastructure.ownership == InfrastructureOwnership::ExternalShared );
    REQUIRE( firstTab.runIntent == RunIntent::Running );
    REQUIRE( secondTab.runIntent == RunIntent::Running );
}

TEST_CASE( "current-generation device gates present each AwaitingUser reason",
           "[livecapture][state][device]" )
{
    const std::array reasons{ AwaitingUserReason::Authorize, AwaitingUserReason::Pair,
                              AwaitingUserReason::Trust, AwaitingUserReason::Unlock };

    for ( const auto reason : reasons ) {
        const auto opening = openingState();
        const auto awaiting
            = dispatch( opening, UserActionRequired{ opening.generation, reason, at( 40 ) } );

        REQUIRE( awaiting.accepted );
        REQUIRE( awaiting.snapshot.runIntent == RunIntent::Running );
        REQUIRE( awaiting.snapshot.source.status == SourceStatus::AwaitingUser );
        REQUIRE( awaiting.snapshot.source.awaitingUserReason == reason );
        REQUIRE_FALSE( awaiting.snapshot.retryTimer.has_value() );
        REQUIRE( projectLiveState( awaiting.snapshot ).status == PresentationStatus::AwaitingUser );
    }
}

TEST_CASE( "only a fully ready streaming source presents Connected",
           "[livecapture][state][presentation]" )
{
    auto state = openingState();
    REQUIRE( projectLiveState( state ).status == PresentationStatus::OpeningStream );

    state = dispatch( state, ProtocolServiceReady{ state.generation, at( 40 ) } ).snapshot;
    REQUIRE( state.source.status == SourceStatus::OpeningStream );
    REQUIRE( projectLiveState( state ).status == PresentationStatus::OpeningStream );

    state = dispatch( state, StreamHandleOpened{ state.generation, at( 50 ) } ).snapshot;
    REQUIRE( state.source.status == SourceStatus::OpeningStream );
    REQUIRE( projectLiveState( state ).status == PresentationStatus::OpeningStream );

    state = dispatch( state, StreamReadArmed{ state.generation, at( 60 ) } ).snapshot;
    REQUIRE( state.protocolServiceReady );
    REQUIRE( state.streamHandlePresent );
    REQUIRE( state.readArmed );
    REQUIRE( state.source.status == SourceStatus::Streaming );
    REQUIRE( projectLiveState( state ).status == PresentationStatus::Connected );

    const std::array nonConnectedStates{
        SourceStatus::Stopped,          SourceStatus::WaitingForInfrastructure,
        SourceStatus::WaitingForDevice, SourceStatus::AwaitingUser,
        SourceStatus::OpeningStream,    SourceStatus::RetryWait,
        SourceStatus::Stopping,         SourceStatus::Failed,
    };
    for ( const auto sourceStatus : nonConnectedStates ) {
        auto nonConnected = state;
        nonConnected.source.status = sourceStatus;
        REQUIRE( projectLiveState( nonConnected ).status != PresentationStatus::Connected );
    }
}

TEST_CASE( "OpeningStream and RetryWait have distinct presentation and countdown",
           "[livecapture][state][presentation]" )
{
    const auto opening = openingState();
    const auto retryError
        = makeError( ErrorCategory::Stream, ErrorScope::Stream, RetryPolicy::Backoff );
    const auto retry = dispatch( opening, RetryRequested{ opening.generation, retryError, 2u,
                                                          at( 5000 ), at( 1000 ) } )
                           .snapshot;

    const auto openingUi = projectLiveState( opening );
    const auto retryUi = projectLiveState( retry );

    REQUIRE( openingUi.status == PresentationStatus::OpeningStream );
    REQUIRE_FALSE( openingUi.retryCountdownVisible );
    REQUIRE( retryUi.status == PresentationStatus::RetryWait );
    REQUIRE( retryUi.retryCountdownVisible );
    REQUIRE( retryUi.retryRemaining == at( 4000 ) );
}

TEST_CASE( "structured errors preserve category, code, scope, retry policy, and diagnostics",
           "[livecapture][state][error]" )
{
    const auto opening = openingState();
    const auto error = makeError( ErrorCategory::Infrastructure, ErrorScope::Infrastructure,
                                  RetryPolicy::WaitForInfrastructure, "adb-server-lost" );
    const auto retry = dispatch( opening, RetryRequested{ opening.generation, error, 1u, at( 3000 ),
                                                          at( 1000 ) } )
                           .snapshot;

    REQUIRE( retry.source.status == SourceStatus::RetryWait );
    REQUIRE( retry.source.retry.has_value() );
    REQUIRE( retry.source.retry->attempt == 1u );
    REQUIRE( retry.source.retry->error.category == ErrorCategory::Infrastructure );
    REQUIRE( retry.source.retry->error.code == "adb-server-lost" );
    REQUIRE( retry.source.retry->error.scope == ErrorScope::Infrastructure );
    REQUIRE( retry.source.retry->error.retryPolicy == RetryPolicy::WaitForInfrastructure );
    REQUIRE( retry.source.retry->error.message == "actionable message" );
    REQUIRE( retry.source.retry->error.nativeDetail == "native detail" );
}

TEST_CASE( "generation-tagged callbacks accept current work and reject stale work",
           "[livecapture][state][generation]" )
{
    auto state = streamingState();
    const auto currentGeneration = state.generation;

    const auto current
        = dispatch( state, StreamBytesReceived{ currentGeneration, 128u, at( 100 ) } );
    REQUIRE( current.accepted );
    REQUIRE( hasEffect( current, EffectKind::AppendBytes ) );
    REQUIRE( current.effects.back().generation == currentGeneration );
    REQUIRE( current.effects.back().byteCount == 128u );

    const auto stale
        = dispatch( state, StreamBytesReceived{ currentGeneration - 1u, 256u, at( 110 ) } );
    REQUIRE_FALSE( stale.accepted );
    REQUIRE( stale.effects.empty() );
    REQUIRE( stale.snapshot.generation == state.generation );
    REQUIRE( stale.snapshot.source.status == state.source.status );
    REQUIRE( stale.snapshot.capture == state.capture );
    REQUIRE( stale.snapshot.consecutiveFailures == state.consecutiveFailures );
    REQUIRE( stale.snapshot.now == state.now );
}

TEST_CASE( "Stop invalidates the generation before requesting cancellation",
           "[livecapture][state][generation]" )
{
    const auto state = streamingState();
    const auto stopped = dispatch( state, StopRequested{ at( 200 ) } );

    REQUIRE( stopped.accepted );
    REQUIRE( stopped.snapshot.runIntent == RunIntent::Stopped );
    REQUIRE( stopped.snapshot.source.status == SourceStatus::Stopping );
    REQUIRE( stopped.snapshot.generation == state.generation + 1u );
    REQUIRE( stopped.effects.size() >= 2u );
    REQUIRE( stopped.effects.at( 0 ).kind == EffectKind::InvalidateGeneration );
    REQUIRE( stopped.effects.at( 0 ).generation == stopped.snapshot.generation );
    REQUIRE( stopped.effects.at( 1 ).kind == EffectKind::CancelStream );
    REQUIRE( stopped.effects.at( 1 ).generation == state.generation );

    const auto lateBytes
        = dispatch( stopped.snapshot, StreamBytesReceived{ state.generation, 64u, at( 210 ) } );
    REQUIRE_FALSE( lateBytes.accepted );
    REQUIRE_FALSE( hasEffect( lateBytes, EffectKind::AppendBytes ) );
}

TEST_CASE( "retry timer exists exactly while the source is in RetryWait",
           "[livecapture][state][retry]" )
{
    const auto opening = openingState();
    REQUIRE_FALSE( opening.retryTimer.has_value() );

    const auto retryError
        = makeError( ErrorCategory::Stream, ErrorScope::Stream, RetryPolicy::Backoff );
    const auto retry = dispatch(
        opening, RetryRequested{ opening.generation, retryError, 2u, at( 5000 ), at( 1000 ) } );
    REQUIRE( retry.snapshot.source.status == SourceStatus::RetryWait );
    REQUIRE( retry.snapshot.retryTimer.has_value() );
    REQUIRE( retry.snapshot.retryTimer->generation == retry.snapshot.generation );
    REQUIRE( retry.snapshot.retryTimer->deadline == at( 5000 ) );
    REQUIRE( hasEffect( retry, EffectKind::ArmRetryTimer ) );

    const auto deadline
        = dispatch( retry.snapshot, RetryDeadlineReached{ retry.snapshot.generation, at( 5000 ) } );
    REQUIRE( deadline.snapshot.source.status == SourceStatus::OpeningStream );
    REQUIRE_FALSE( deadline.snapshot.retryTimer.has_value() );
    REQUIRE( hasEffect( deadline, EffectKind::CancelRetryTimer ) );

    const auto stopped = dispatch( retry.snapshot, StopRequested{ at( 2000 ) } );
    REQUIRE( stopped.snapshot.source.status == SourceStatus::Stopping );
    REQUIRE_FALSE( stopped.snapshot.retryTimer.has_value() );
    REQUIRE( hasEffect( stopped, EffectKind::CancelRetryTimer ) );
}

TEST_CASE( "first bytes neither prove readiness nor reset backoff; stability does",
           "[livecapture][state][retry][readiness]" )
{
    auto state = openingState();
    state.consecutiveFailures = 3u;
    state = dispatch( state, ProtocolServiceReady{ state.generation, at( 40 ) } ).snapshot;
    state = dispatch( state, StreamHandleOpened{ state.generation, at( 50 ) } ).snapshot;

    const auto firstBytes
        = dispatch( state, StreamBytesReceived{ state.generation, 42u, at( 60 ) } );
    REQUIRE( firstBytes.accepted );
    REQUIRE( firstBytes.snapshot.source.status == SourceStatus::OpeningStream );
    REQUIRE( firstBytes.snapshot.consecutiveFailures == 3u );
    REQUIRE( projectLiveState( firstBytes.snapshot ).status == PresentationStatus::OpeningStream );

    state = dispatch( firstBytes.snapshot,
                      StreamReadArmed{ firstBytes.snapshot.generation, at( 70 ) } )
                .snapshot;
    REQUIRE( state.source.status == SourceStatus::Streaming );
    REQUIRE( state.consecutiveFailures == 3u );

    state = dispatch( state, StreamBytesReceived{ state.generation, 84u, at( 80 ) } ).snapshot;
    REQUIRE( state.consecutiveFailures == 3u );

    state = dispatch( state, TimeAdvanced{ at( 20000 ) } ).snapshot;
    REQUIRE( state.consecutiveFailures == 3u );

    state = dispatch( state, StreamStable{ state.generation, at( 20001 ) } ).snapshot;
    REQUIRE( state.source.status == SourceStatus::Streaming );
    REQUIRE( state.consecutiveFailures == 0u );
}

TEST_CASE( "device absence waits for discovery without restarting shared infrastructure",
           "[livecapture][state][device]" )
{
    const auto streaming = streamingState();
    const auto absent = dispatch( streaming, DeviceAbsent{ streaming.generation, at( 100 ) } );

    REQUIRE( absent.accepted );
    REQUIRE( absent.snapshot.runIntent == RunIntent::Running );
    REQUIRE( absent.snapshot.source.status == SourceStatus::WaitingForDevice );
    REQUIRE( absent.snapshot.infrastructure.status == InfrastructureStatus::Ready );
    REQUIRE( absent.snapshot.infrastructure.ownership == InfrastructureOwnership::ExternalShared );
    REQUIRE_FALSE( hasEffect( absent, EffectKind::StartInfrastructure ) );
    REQUIRE_FALSE( absent.snapshot.retryTimer.has_value() );
}

TEST_CASE( "capture lifecycle and degradation remain orthogonal to Streaming",
           "[livecapture][state][capture]" )
{
    const auto streaming = streamingState();
    const auto captureError = makeError( ErrorCategory::Capture, ErrorScope::Capture,
                                         RetryPolicy::Never, "output-write-failed" );

    const auto degraded = dispatch( streaming, CaptureChanged{ streaming.captureGeneration,
                                                               CaptureState::OutputDegraded,
                                                               captureError, at( 100 ) } );
    REQUIRE( degraded.snapshot.capture == CaptureState::OutputDegraded );
    REQUIRE( degraded.snapshot.captureError.has_value() );
    REQUIRE( degraded.snapshot.captureError->code == "output-write-failed" );
    REQUIRE( degraded.snapshot.source.status == SourceStatus::Streaming );
    REQUIRE( projectLiveState( degraded.snapshot ).status == PresentationStatus::Connected );

    const auto finalizing = dispatch(
        degraded.snapshot, CaptureChanged{ degraded.snapshot.captureGeneration,
                                           CaptureState::Finalizing, std::nullopt, at( 110 ) } );
    REQUIRE( finalizing.snapshot.capture == CaptureState::Finalizing );
    REQUIRE( finalizing.snapshot.source.status == SourceStatus::Streaming );

    const auto finalized = dispatch(
        finalizing.snapshot, CaptureChanged{ finalizing.snapshot.captureGeneration,
                                             CaptureState::Finalized, std::nullopt, at( 120 ) } );
    REQUIRE( finalized.snapshot.capture == CaptureState::Finalized );
    REQUIRE( finalized.snapshot.source.status == SourceStatus::Streaming );

    const auto faulted
        = dispatch( streaming, CaptureChanged{ streaming.captureGeneration, CaptureState::Faulted,
                                               captureError, at( 130 ) } );
    REQUIRE( faulted.snapshot.capture == CaptureState::Faulted );
    REQUIRE( faulted.snapshot.source.status == SourceStatus::Streaming );
}

TEST_CASE( "retry exhaustion presents Failed and disables countdown",
           "[livecapture][state][retry][presentation]" )
{
    const auto opening = openingState();
    const auto error = makeError( ErrorCategory::Service, ErrorScope::Service, RetryPolicy::Backoff,
                                  "service-unavailable" );
    const auto failed = dispatch( opening, RetryRequested{ opening.generation, error,
                                                           defaultConfig.maxRetryAttempts,
                                                           at( 9000 ), at( 1000 ) } );

    REQUIRE( failed.snapshot.source.status == SourceStatus::Failed );
    REQUIRE( failed.snapshot.source.failure.has_value() );
    REQUIRE( failed.snapshot.source.failure->category == ErrorCategory::Service );
    REQUIRE( failed.snapshot.source.failure->scope == ErrorScope::Service );
    REQUIRE( failed.snapshot.source.failure->retryPolicy == RetryPolicy::Backoff );
    REQUIRE_FALSE( failed.snapshot.retryTimer.has_value() );

    const auto ui = projectLiveState( failed.snapshot );
    REQUIRE( ui.status == PresentationStatus::Failed );
    REQUIRE_FALSE( ui.retryCountdownVisible );
    REQUIRE( ui.retryRemaining == 0ms );
}

TEST_CASE( "manual reconnect after retry exhaustion starts with a fresh retry budget",
           "[livecapture][state][retry]" )
{
    const auto opening = openingState();
    const auto error = makeError( ErrorCategory::Service, ErrorScope::Service, RetryPolicy::Backoff,
                                  "service-unavailable" );
    const auto exhausted
        = dispatch( opening, RetryRequested{ opening.generation, error,
                                            defaultConfig.maxRetryAttempts, at( 9000 ), at( 1000 ) } )
              .snapshot;
    REQUIRE( exhausted.source.status == SourceStatus::Failed );
    REQUIRE( exhausted.consecutiveFailures == defaultConfig.maxRetryAttempts );

    const auto restarted = dispatch( exhausted, StartRequested{ at( 2000 ) } );

    REQUIRE( restarted.accepted );
    CHECK( restarted.snapshot.consecutiveFailures == 0u );
}

TEST_CASE( "UI action booleans are a deterministic pure projection of the snapshot",
           "[livecapture][state][presentation]" )
{
    const auto opening = openingState();
    const auto openingUi = projectLiveState( opening );
    const auto repeatedOpeningUi = projectLiveState( opening );
    REQUIRE( openingUi.disconnectEnabled );
    REQUIRE_FALSE( openingUi.reconnectEnabled );
    REQUIRE( repeatedOpeningUi.disconnectEnabled == openingUi.disconnectEnabled );
    REQUIRE( repeatedOpeningUi.reconnectEnabled == openingUi.reconnectEnabled );
    REQUIRE( repeatedOpeningUi.retryCountdownVisible == openingUi.retryCountdownVisible );

    const auto streaming = streamingState();
    const auto streamingUi = projectLiveState( streaming );
    REQUIRE( streamingUi.disconnectEnabled );
    REQUIRE( streamingUi.reconnectEnabled );

    auto stopped = initialLiveState();
    stopped.now = at( 999999 );
    const auto stoppedUi = projectLiveState( stopped );
    REQUIRE_FALSE( stoppedUi.disconnectEnabled );
    REQUIRE( stoppedUi.reconnectEnabled );
    REQUIRE_FALSE( stoppedUi.retryCountdownVisible );

    const auto retryError
        = makeError( ErrorCategory::Stream, ErrorScope::Stream, RetryPolicy::Backoff );
    const auto retry = dispatch( opening, RetryRequested{ opening.generation, retryError, 3u,
                                                          at( 9000 ), at( 1000 ) } )
                           .snapshot;
    const auto retryUi = projectLiveState( retry );
    REQUIRE( retryUi.disconnectEnabled );
    REQUIRE( retryUi.reconnectEnabled );
    REQUIRE( retryUi.retryCountdownVisible );
}

TEST_CASE( "duplicate device discovery cannot replace an active stream",
           "[livecapture][state][device][generation]" )
{
    const auto streaming = streamingState();
    const auto duplicate
        = dispatch( streaming, DeviceAvailable{ streaming.generation, at( 100 ) } );

    REQUIRE_FALSE( duplicate.accepted );
    REQUIRE( duplicate.snapshot.source.status == SourceStatus::Streaming );
    REQUIRE( duplicate.snapshot.generation == streaming.generation );
    REQUIRE_FALSE( hasEffect( duplicate, EffectKind::OpenStream ) );
}

TEST_CASE( "abandoning stream work invalidates its callbacks before cancellation",
           "[livecapture][state][generation][device]" )
{
    const auto streaming = streamingState();
    const auto absent = dispatch( streaming, DeviceAbsent{ streaming.generation, at( 100 ) } );

    REQUIRE( absent.accepted );
    REQUIRE( absent.snapshot.source.status == SourceStatus::WaitingForDevice );
    REQUIRE( absent.snapshot.generation == streaming.generation + 1u );
    REQUIRE( absent.effects.size() >= 2u );
    REQUIRE( absent.effects.at( 0 ).kind == EffectKind::InvalidateGeneration );
    REQUIRE( absent.effects.at( 0 ).generation == absent.snapshot.generation );
    REQUIRE( absent.effects.at( 1 ).kind == EffectKind::CancelStream );
    REQUIRE( absent.effects.at( 1 ).generation == streaming.generation );

    const auto lateReady
        = dispatch( absent.snapshot, ProtocolServiceReady{ streaming.generation, at( 110 ) } );
    REQUIRE_FALSE( lateReady.accepted );
}

TEST_CASE( "retrying starts a new stream generation and rejects callbacks from the failed attempt",
           "[livecapture][state][retry][generation]" )
{
    const auto opening = openingState();
    const auto error = makeError( ErrorCategory::Stream, ErrorScope::Stream, RetryPolicy::Backoff );
    const auto retry = dispatch(
        opening, RetryRequested{ opening.generation, error, 1u, at( 5000 ), at( 1000 ) } );

    REQUIRE( retry.snapshot.generation == opening.generation + 1u );
    REQUIRE( retry.snapshot.retryTimer.has_value() );
    REQUIRE( retry.snapshot.retryTimer->generation == retry.snapshot.generation );
    REQUIRE( hasEffect( retry, EffectKind::InvalidateGeneration ) );
    REQUIRE( hasEffect( retry, EffectKind::CancelStream ) );

    const auto captureUpdate = dispatch(
        retry.snapshot, CaptureChanged{ opening.captureGeneration, CaptureState::OutputDegraded,
                                        makeError( ErrorCategory::Capture, ErrorScope::Capture,
                                                   RetryPolicy::Never ),
                                        at( 1100 ) } );
    REQUIRE( captureUpdate.accepted );
    REQUIRE( captureUpdate.snapshot.capture == CaptureState::OutputDegraded );

    const auto deadline
        = dispatch( retry.snapshot, RetryDeadlineReached{ retry.snapshot.generation, at( 5000 ) } );
    REQUIRE( deadline.snapshot.source.status == SourceStatus::OpeningStream );

    const auto staleReady
        = dispatch( deadline.snapshot, ProtocolServiceReady{ opening.generation, at( 5010 ) } );
    REQUIRE_FALSE( staleReady.accepted );
    REQUIRE_FALSE( staleReady.snapshot.protocolServiceReady );
}

TEST_CASE( "stability interval must elapse before backoff is reset",
           "[livecapture][state][retry][readiness]" )
{
    auto streaming = streamingState();
    streaming.consecutiveFailures = 3u;

    const auto tooEarly = dispatch( streaming, StreamStable{ streaming.generation, at( 61 ) } );
    REQUIRE_FALSE( tooEarly.accepted );
    REQUIRE( tooEarly.snapshot.consecutiveFailures == 3u );

    const auto stable = dispatch( streaming, StreamStable{ streaming.generation, at( 10060 ) } );
    REQUIRE( stable.accepted );
    REQUIRE( stable.snapshot.consecutiveFailures == 0u );
}

TEST_CASE( "stop is idempotent and capture finalization from the cancelled run remains valid",
           "[livecapture][state][generation][capture]" )
{
    const auto streaming = streamingState();
    const auto stopped = dispatch( streaming, StopRequested{ at( 200 ) } );
    const auto prematureRestart = dispatch( stopped.snapshot, StartRequested{ at( 205 ) } );
    const auto duplicateStop = dispatch( stopped.snapshot, StopRequested{ at( 210 ) } );

    REQUIRE_FALSE( prematureRestart.accepted );
    REQUIRE( prematureRestart.snapshot.source.status == SourceStatus::Stopping );
    REQUIRE( prematureRestart.effects.empty() );
    REQUIRE_FALSE( duplicateStop.accepted );
    REQUIRE( duplicateStop.snapshot.generation == stopped.snapshot.generation );
    REQUIRE( duplicateStop.effects.empty() );

    const auto finalizing = dispatch( stopped.snapshot, CaptureChanged{ streaming.captureGeneration,
                                                                        CaptureState::Finalizing,
                                                                        std::nullopt, at( 220 ) } );
    REQUIRE( finalizing.accepted );
    REQUIRE( finalizing.snapshot.capture == CaptureState::Finalizing );

    const auto finalized = dispatch(
        finalizing.snapshot, CaptureChanged{ streaming.captureGeneration, CaptureState::Finalized,
                                             std::nullopt, at( 230 ) } );
    REQUIRE( finalized.accepted );
    REQUIRE( finalized.snapshot.capture == CaptureState::Finalized );

    const auto completed
        = dispatch( finalized.snapshot, StopCompleted{ streaming.generation, at( 240 ) } );
    REQUIRE( completed.accepted );
    REQUIRE( completed.snapshot.source.status == SourceStatus::Stopped );
    REQUIRE( completed.snapshot.source.stopReason == StopReason::User );
    REQUIRE_FALSE( completed.snapshot.source.stoppingGeneration.has_value() );
    REQUIRE( projectLiveState( completed.snapshot ).status == PresentationStatus::Stopped );
}

TEST_CASE( "capture terminal states reject delayed lifecycle regressions",
           "[livecapture][state][capture]" )
{
    const auto streaming = streamingState();
    const auto finalized
        = dispatch( streaming, CaptureChanged{ streaming.captureGeneration, CaptureState::Finalized,
                                               std::nullopt, at( 120 ) } );
    REQUIRE( finalized.accepted );

    const auto delayedFinalizing = dispatch(
        finalized.snapshot, CaptureChanged{ streaming.captureGeneration, CaptureState::Finalizing,
                                            std::nullopt, at( 110 ) } );
    REQUIRE_FALSE( delayedFinalizing.accepted );
    REQUIRE( delayedFinalizing.snapshot.capture == CaptureState::Finalized );
    REQUIRE( delayedFinalizing.snapshot.now == at( 120 ) );
}

TEST_CASE( "accepted events cannot move the reducer clock backwards", "[livecapture][state][time]" )
{
    auto retry = openingState();
    const auto error = makeError( ErrorCategory::Stream, ErrorScope::Stream, RetryPolicy::Backoff );
    retry = dispatch( retry, RetryRequested{ retry.generation, error, 1u, at( 5000 ), at( 1000 ) } )
                .snapshot;

    retry = dispatch( retry, TimeAdvanced{ at( 4000 ) } ).snapshot;
    const auto delayedClock = dispatch( retry, TimeAdvanced{ at( 2000 ) } );

    REQUIRE( delayedClock.accepted );
    REQUIRE( delayedClock.snapshot.now == at( 4000 ) );
    REQUIRE( projectLiveState( delayedClock.snapshot ).retryRemaining == at( 1000 ) );
}

TEST_CASE( "capture failures require structured diagnostics", "[livecapture][state][capture]" )
{
    const auto streaming = streamingState();
    const auto missingError
        = dispatch( streaming, CaptureChanged{ streaming.captureGeneration, CaptureState::Faulted,
                                               std::nullopt, at( 100 ) } );

    REQUIRE_FALSE( missingError.accepted );
    REQUIRE( missingError.snapshot.capture == CaptureState::OpenHealthy );
    REQUIRE_FALSE( missingError.snapshot.captureError.has_value() );
}

TEST_CASE( "explicit reconnect reacquires availability observation while infrastructure stays ready",
           "[livecapture][state][infrastructure][reconnect]" )
{
    auto stopped = initialLiveState();
    stopped.infrastructure = InfrastructureState{ InfrastructureStatus::Ready,
                                                  InfrastructureOwnership::AppShared };
    stopped.runIntent = RunIntent::Stopped;
    stopped.source.status = SourceStatus::Stopped;

    const auto started = dispatch( stopped, StartRequested{ at( 10 ) } );

    REQUIRE( started.accepted );
    REQUIRE( started.snapshot.source.status == SourceStatus::WaitingForDevice );
    REQUIRE( hasEffect( started, EffectKind::StartInfrastructure ) );
}

TEST_CASE( "shared infrastructure observation starts per run and retries only when permitted",
           "[livecapture][state][infrastructure]" )
{
    auto connecting = initialLiveState();
    connecting.infrastructure = InfrastructureState{ InfrastructureStatus::Connecting,
                                                     InfrastructureOwnership::AppShared };

    const auto started = dispatch( connecting, StartRequested{ at( 10 ) } );
    REQUIRE( started.accepted );
    REQUIRE( started.snapshot.source.status == SourceStatus::WaitingForInfrastructure );
    REQUIRE( hasEffect( started, EffectKind::StartInfrastructure ) );

    const auto unavailable = dispatch(
        started.snapshot, InfrastructureChanged{ InfrastructureStatus::Unavailable,
                                                 InfrastructureOwnership::AppShared, at( 20 ) } );
    REQUIRE( unavailable.accepted );
    REQUIRE( hasEffect( unavailable, EffectKind::StartInfrastructure ) );

    const auto externalReady = waitingForDeviceState( InfrastructureOwnership::ExternalShared );
    const auto externalUnavailable = dispatch(
        externalReady, InfrastructureChanged{ InfrastructureStatus::Unavailable,
                                              InfrastructureOwnership::ExternalShared, at( 30 ) } );
    REQUIRE( externalUnavailable.accepted );
    REQUIRE_FALSE( hasEffect( externalUnavailable, EffectKind::StartInfrastructure ) );
}
