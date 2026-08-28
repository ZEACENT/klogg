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

#include "livestate.h"

#include <algorithm>

namespace klogg::livecapture {
namespace {

template <typename Event>
bool hasCurrentGeneration( const LiveStateSnapshot& snapshot, const Event& event )
{
    return event.generation == snapshot.generation;
}

template <typename Event>
bool hasCurrentRunningGeneration( const LiveStateSnapshot& snapshot, const Event& event )
{
    return snapshot.runIntent == RunIntent::Running && hasCurrentGeneration( snapshot, event );
}

void advanceNow( LiveStateSnapshot& snapshot, Timestamp timestamp )
{
    snapshot.now = std::max( snapshot.now, timestamp );
}

bool hasActiveStreamAttempt( const LiveStateSnapshot& snapshot )
{
    return snapshot.source.status == SourceStatus::OpeningStream
           || snapshot.source.status == SourceStatus::Streaming;
}

bool shouldStartInfrastructure( const InfrastructureState& infrastructure )
{
    return infrastructure.ownership != InfrastructureOwnership::ExternalShared
           && ( infrastructure.status == InfrastructureStatus::Unknown
                || infrastructure.status == InfrastructureStatus::Unavailable );
}

void resetStreamReadiness( LiveStateSnapshot& snapshot )
{
    snapshot.protocolServiceReady = false;
    snapshot.streamHandlePresent = false;
    snapshot.readArmed = false;
    snapshot.streamingSince.reset();
}

void clearRetry( LiveStateTransition& transition )
{
    if ( transition.snapshot.retryTimer.has_value() ) {
        transition.effects.push_back( LiveStateEffect{
            EffectKind::CancelRetryTimer, transition.snapshot.retryTimer->generation,
            transition.snapshot.retryTimer->deadline, 0u } );
    }

    transition.snapshot.retryTimer.reset();
    transition.snapshot.source.retry.reset();
}

void resetSourceAttempt( LiveStateTransition& transition )
{
    clearRetry( transition );
    resetStreamReadiness( transition.snapshot );
}

void invalidateStreamAttempt( LiveStateTransition& transition )
{
    auto& snapshot = transition.snapshot;
    const auto cancelledGeneration = snapshot.generation;
    ++snapshot.generation;
    transition.effects.push_back( LiveStateEffect{ EffectKind::InvalidateGeneration,
                                                   snapshot.generation, Timestamp{ 0 }, 0u } );
    transition.effects.push_back(
        LiveStateEffect{ EffectKind::CancelStream, cancelledGeneration, Timestamp{ 0 }, 0u } );
}

void enterSourceState( LiveStateSnapshot& snapshot, SourceStatus status )
{
    snapshot.source.status = status;
    snapshot.source.stopReason.reset();
    snapshot.source.stoppingGeneration.reset();
    snapshot.source.awaitingUserReason.reset();
    snapshot.source.retry.reset();
    snapshot.source.failure.reset();
}

void updateStreamingReadiness( LiveStateSnapshot& snapshot )
{
    if ( snapshot.source.status == SourceStatus::OpeningStream && snapshot.protocolServiceReady
         && snapshot.streamHandlePresent && snapshot.readArmed ) {
        snapshot.source.status = SourceStatus::Streaming;
        snapshot.streamingSince = snapshot.now;
    }
}

bool isCaptureTransitionAllowed( CaptureState from, CaptureState targetState )
{
    if ( from == targetState ) {
        return true;
    }

    switch ( from ) {
    case CaptureState::OpenHealthy:
        return true;
    case CaptureState::OutputDegraded:
        return targetState == CaptureState::OpenHealthy || targetState == CaptureState::Finalizing
               || targetState == CaptureState::Finalized || targetState == CaptureState::Faulted;
    case CaptureState::Finalizing:
        return targetState == CaptureState::Finalized || targetState == CaptureState::Faulted;
    case CaptureState::Finalized:
    case CaptureState::Faulted:
        return false;
    }

    return false;
}

bool captureStateRequiresError( CaptureState state )
{
    return state == CaptureState::OutputDegraded || state == CaptureState::Faulted;
}

void applyReadiness( LiveStateTransition& transition, Generation generation, Timestamp timestamp,
                     bool LiveStateSnapshot::* readiness )
{
    auto& snapshot = transition.snapshot;
    if ( generation != snapshot.generation
         || snapshot.source.status != SourceStatus::OpeningStream ) {
        return;
    }

    advanceNow( snapshot, timestamp );
    snapshot.*readiness = true;
    updateStreamingReadiness( snapshot );
    transition.accepted = true;
}

void apply( LiveStateTransition& transition, const StartRequested& event, const LiveStateConfig& )
{
    auto& snapshot = transition.snapshot;
    if ( snapshot.source.status == SourceStatus::Stopping ) {
        return;
    }

    const auto previousGeneration = snapshot.generation;
    const auto startsCapture = snapshot.runIntent == RunIntent::Stopped;

    advanceNow( snapshot, event.at );
    snapshot.runIntent = RunIntent::Running;
    ++snapshot.generation;
    if ( startsCapture ) {
        snapshot.captureGeneration = snapshot.generation;
        snapshot.capture = CaptureState::OpenHealthy;
        snapshot.captureError.reset();
    }
    resetSourceAttempt( transition );
    snapshot.consecutiveFailures = 0u;

    const auto nextStatus = snapshot.infrastructure.status == InfrastructureStatus::Ready
                                ? SourceStatus::WaitingForDevice
                                : SourceStatus::WaitingForInfrastructure;
    enterSourceState( snapshot, nextStatus );

    transition.effects.insert( transition.effects.begin(),
                               LiveStateEffect{ EffectKind::InvalidateGeneration,
                                                snapshot.generation, Timestamp{ 0 }, 0u } );
    if ( previousGeneration != 0u ) {
        transition.effects.push_back(
            LiveStateEffect{ EffectKind::CancelStream, previousGeneration, Timestamp{ 0 }, 0u } );
    }
    // Every explicit run must reacquire availability observation and replay the
    // current shared snapshot, even when the infrastructure itself stayed ready.
    transition.effects.push_back( LiveStateEffect{ EffectKind::StartInfrastructure,
                                                   snapshot.generation, Timestamp{ 0 }, 0u } );
    transition.accepted = true;
}

void apply( LiveStateTransition& transition, const StopRequested& event, const LiveStateConfig& )
{
    auto& snapshot = transition.snapshot;
    if ( snapshot.runIntent == RunIntent::Stopped ) {
        return;
    }

    const auto cancelledGeneration = snapshot.generation;
    advanceNow( snapshot, event.at );
    snapshot.runIntent = RunIntent::Stopped;
    ++snapshot.generation;
    snapshot.source.status = SourceStatus::Stopping;
    snapshot.source.stopReason = StopReason::User;
    snapshot.source.stoppingGeneration = cancelledGeneration;
    snapshot.source.awaitingUserReason.reset();
    snapshot.source.failure.reset();
    resetStreamReadiness( snapshot );

    transition.effects.push_back( LiveStateEffect{ EffectKind::InvalidateGeneration,
                                                   snapshot.generation, Timestamp{ 0 }, 0u } );
    transition.effects.push_back(
        LiveStateEffect{ EffectKind::CancelStream, cancelledGeneration, Timestamp{ 0 }, 0u } );
    clearRetry( transition );
    transition.accepted = true;
}

void apply( LiveStateTransition& transition, const StopCompleted& event, const LiveStateConfig& )
{
    auto& snapshot = transition.snapshot;
    if ( snapshot.runIntent != RunIntent::Stopped
         || snapshot.source.status != SourceStatus::Stopping
         || snapshot.source.stoppingGeneration != event.generation ) {
        return;
    }

    advanceNow( snapshot, event.at );
    enterSourceState( snapshot, SourceStatus::Stopped );
    snapshot.source.stopReason = StopReason::User;
    transition.accepted = true;
}

void apply( LiveStateTransition& transition, const InfrastructureChanged& event,
            const LiveStateConfig& )
{
    auto& snapshot = transition.snapshot;
    const auto previousStatus = snapshot.infrastructure.status;
    advanceNow( snapshot, event.at );
    snapshot.infrastructure = InfrastructureState{ event.status, event.ownership };

    if ( snapshot.runIntent == RunIntent::Running ) {
        if ( event.status == InfrastructureStatus::Ready ) {
            if ( snapshot.source.status == SourceStatus::WaitingForInfrastructure ) {
                enterSourceState( snapshot, SourceStatus::WaitingForDevice );
            }
        }
        else {
            if ( hasActiveStreamAttempt( snapshot ) ) {
                invalidateStreamAttempt( transition );
            }
            resetSourceAttempt( transition );
            enterSourceState( snapshot, SourceStatus::WaitingForInfrastructure );
        }

        if ( shouldStartInfrastructure( snapshot.infrastructure )
             && event.status != previousStatus ) {
            transition.effects.push_back( LiveStateEffect{
                EffectKind::StartInfrastructure, snapshot.generation, Timestamp{ 0 }, 0u } );
        }
    }

    transition.accepted = true;
}

void apply( LiveStateTransition& transition, const InfrastructureFailed& event,
            const LiveStateConfig& )
{
    auto& snapshot = transition.snapshot;
    if ( !hasCurrentRunningGeneration( snapshot, event )
         || snapshot.source.status != SourceStatus::WaitingForInfrastructure ) {
        return;
    }

    advanceNow( snapshot, event.at );
    enterSourceState( snapshot, SourceStatus::Failed );
    snapshot.source.failure = event.error;
    transition.accepted = true;
}

void apply( LiveStateTransition& transition, const DeviceAvailable& event, const LiveStateConfig& )
{
    auto& snapshot = transition.snapshot;
    if ( !hasCurrentRunningGeneration( snapshot, event )
         || snapshot.infrastructure.status != InfrastructureStatus::Ready
         || ( snapshot.source.status != SourceStatus::WaitingForDevice
              && snapshot.source.status != SourceStatus::AwaitingUser ) ) {
        return;
    }

    advanceNow( snapshot, event.at );
    resetSourceAttempt( transition );
    enterSourceState( snapshot, SourceStatus::OpeningStream );
    transition.effects.push_back(
        LiveStateEffect{ EffectKind::OpenStream, snapshot.generation, Timestamp{ 0 }, 0u } );
    transition.accepted = true;
}

void apply( LiveStateTransition& transition, const DeviceAbsent& event, const LiveStateConfig& )
{
    auto& snapshot = transition.snapshot;
    if ( !hasCurrentRunningGeneration( snapshot, event ) ) {
        return;
    }

    advanceNow( snapshot, event.at );
    if ( hasActiveStreamAttempt( snapshot ) ) {
        invalidateStreamAttempt( transition );
    }
    resetSourceAttempt( transition );
    enterSourceState( snapshot, snapshot.infrastructure.status == InfrastructureStatus::Ready
                                    ? SourceStatus::WaitingForDevice
                                    : SourceStatus::WaitingForInfrastructure );
    transition.accepted = true;
}

void apply( LiveStateTransition& transition, const UserActionRequired& event,
            const LiveStateConfig& )
{
    auto& snapshot = transition.snapshot;
    if ( !hasCurrentRunningGeneration( snapshot, event )
         || ( snapshot.source.status != SourceStatus::WaitingForDevice
              && !hasActiveStreamAttempt( snapshot ) ) ) {
        return;
    }

    advanceNow( snapshot, event.at );
    if ( hasActiveStreamAttempt( snapshot ) ) {
        invalidateStreamAttempt( transition );
    }
    resetSourceAttempt( transition );
    enterSourceState( snapshot, SourceStatus::AwaitingUser );
    snapshot.source.awaitingUserReason = event.reason;
    transition.accepted = true;
}

void apply( LiveStateTransition& transition, const ProtocolServiceReady& event,
            const LiveStateConfig& )
{
    applyReadiness( transition, event.generation, event.at,
                    &LiveStateSnapshot::protocolServiceReady );
}

void apply( LiveStateTransition& transition, const StreamHandleOpened& event,
            const LiveStateConfig& )
{
    applyReadiness( transition, event.generation, event.at,
                    &LiveStateSnapshot::streamHandlePresent );
}

void apply( LiveStateTransition& transition, const StreamReadArmed& event, const LiveStateConfig& )
{
    applyReadiness( transition, event.generation, event.at, &LiveStateSnapshot::readArmed );
}

void apply( LiveStateTransition& transition, const StreamBytesReceived& event,
            const LiveStateConfig& )
{
    const auto& snapshot = transition.snapshot;
    if ( !hasCurrentGeneration( snapshot, event )
         || ( snapshot.source.status != SourceStatus::OpeningStream
              && snapshot.source.status != SourceStatus::Streaming ) ) {
        return;
    }

    advanceNow( transition.snapshot, event.at );
    transition.effects.push_back( LiveStateEffect{ EffectKind::AppendBytes, snapshot.generation,
                                                   Timestamp{ 0 }, event.byteCount } );
    transition.accepted = true;
}

void apply( LiveStateTransition& transition, const StreamStable& event,
            const LiveStateConfig& config )
{
    auto& snapshot = transition.snapshot;
    if ( !hasCurrentGeneration( snapshot, event )
         || snapshot.source.status != SourceStatus::Streaming
         || !snapshot.streamingSince.has_value() || event.at < *snapshot.streamingSince
         || event.at - *snapshot.streamingSince < config.stabilityInterval ) {
        return;
    }

    advanceNow( snapshot, event.at );
    snapshot.consecutiveFailures = 0u;
    transition.accepted = true;
}

void apply( LiveStateTransition& transition, const RetryRequested& event,
            const LiveStateConfig& config )
{
    auto& snapshot = transition.snapshot;
    if ( !hasCurrentRunningGeneration( snapshot, event ) || !hasActiveStreamAttempt( snapshot ) ) {
        return;
    }

    advanceNow( snapshot, event.at );
    invalidateStreamAttempt( transition );
    resetSourceAttempt( transition );
    snapshot.consecutiveFailures = std::max( snapshot.consecutiveFailures, event.attempt );

    if ( event.attempt >= config.maxRetryAttempts
         || event.error.retryPolicy == RetryPolicy::Never ) {
        enterSourceState( snapshot, SourceStatus::Failed );
        snapshot.source.failure = event.error;
    }
    else {
        enterSourceState( snapshot, SourceStatus::RetryWait );
        snapshot.source.retry = RetryState{ event.attempt, event.error };
        snapshot.retryTimer = RetryTimer{ snapshot.generation, event.deadline };
        transition.effects.push_back(
            LiveStateEffect{ EffectKind::ArmRetryTimer, snapshot.generation, event.deadline, 0u } );
    }

    transition.accepted = true;
}

void apply( LiveStateTransition& transition, const RetryDeadlineReached& event,
            const LiveStateConfig& )
{
    auto& snapshot = transition.snapshot;
    if ( !hasCurrentGeneration( snapshot, event ) || !snapshot.retryTimer.has_value()
         || snapshot.source.status != SourceStatus::RetryWait
         || event.at < snapshot.retryTimer->deadline ) {
        return;
    }

    advanceNow( snapshot, event.at );
    clearRetry( transition );
    enterSourceState( snapshot, SourceStatus::OpeningStream );
    transition.effects.push_back(
        LiveStateEffect{ EffectKind::OpenStream, snapshot.generation, Timestamp{ 0 }, 0u } );
    transition.accepted = true;
}

void apply( LiveStateTransition& transition, const CaptureChanged& event, const LiveStateConfig& )
{
    auto& snapshot = transition.snapshot;
    const auto requiresError = captureStateRequiresError( event.state );
    if ( event.generation != snapshot.captureGeneration
         || !isCaptureTransitionAllowed( snapshot.capture, event.state )
         || ( requiresError && !event.error.has_value() && !snapshot.captureError.has_value() )
         || ( !requiresError && event.error.has_value() ) ) {
        return;
    }

    advanceNow( snapshot, event.at );
    snapshot.capture = event.state;
    if ( event.error.has_value() || !requiresError ) {
        snapshot.captureError = event.error;
    }
    transition.accepted = true;
}

void apply( LiveStateTransition& transition, const TimeAdvanced& event, const LiveStateConfig& )
{
    advanceNow( transition.snapshot, event.at );
    transition.accepted = true;
}

PresentationStatus presentationStatus( const LiveStateSnapshot& snapshot )
{
    switch ( snapshot.source.status ) {
    case SourceStatus::Stopped:
        return PresentationStatus::Stopped;
    case SourceStatus::WaitingForInfrastructure:
        return PresentationStatus::WaitingForInfrastructure;
    case SourceStatus::WaitingForDevice:
        return PresentationStatus::WaitingForDevice;
    case SourceStatus::AwaitingUser:
        return PresentationStatus::AwaitingUser;
    case SourceStatus::OpeningStream:
        return PresentationStatus::OpeningStream;
    case SourceStatus::Streaming:
        return snapshot.protocolServiceReady && snapshot.streamHandlePresent && snapshot.readArmed
                   ? PresentationStatus::Connected
                   : PresentationStatus::OpeningStream;
    case SourceStatus::RetryWait:
        return PresentationStatus::RetryWait;
    case SourceStatus::Stopping:
        return PresentationStatus::Stopping;
    case SourceStatus::Failed:
        return PresentationStatus::Failed;
    }

    return PresentationStatus::Failed;
}

bool reconnectEnabled( SourceStatus status )
{
    switch ( status ) {
    case SourceStatus::Stopped:
    case SourceStatus::Streaming:
    case SourceStatus::RetryWait:
    case SourceStatus::Failed:
        return true;
    case SourceStatus::WaitingForInfrastructure:
    case SourceStatus::WaitingForDevice:
    case SourceStatus::AwaitingUser:
    case SourceStatus::OpeningStream:
    case SourceStatus::Stopping:
        return false;
    }

    return false;
}

} // namespace

LiveStateSnapshot initialLiveState()
{
    return LiveStateSnapshot{};
}

LiveStateTransition reduce( const LiveStateSnapshot& snapshot, const LiveStateEvent& event,
                            const LiveStateConfig& config )
{
    LiveStateTransition transition{ snapshot, {}, false };
    std::visit( [ &transition, &config ](
                    const auto& concreteEvent ) { apply( transition, concreteEvent, config ); },
                event );
    return transition;
}

LiveStatePresentation projectLiveState( const LiveStateSnapshot& snapshot )
{
    LiveStatePresentation presentation;
    presentation.status = presentationStatus( snapshot );
    presentation.disconnectEnabled = snapshot.runIntent == RunIntent::Running;
    presentation.reconnectEnabled = reconnectEnabled( snapshot.source.status );

    if ( snapshot.source.status == SourceStatus::RetryWait && snapshot.retryTimer.has_value() ) {
        presentation.retryCountdownVisible = true;
        presentation.retryRemaining
            = std::max( Timestamp{ 0 }, snapshot.retryTimer->deadline - snapshot.now );
        if ( snapshot.source.retry.has_value() ) {
            presentation.retryAttempt = snapshot.source.retry->attempt;
        }
    }
    if ( snapshot.source.failure.has_value() ) {
        presentation.failureMessage = snapshot.source.failure->message;
    }

    return presentation;
}

} // namespace klogg::livecapture
