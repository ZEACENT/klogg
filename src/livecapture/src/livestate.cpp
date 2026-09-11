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
#include <tuple>
#include <limits>

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
    transition.snapshot.payloadReceived = false;
}

void invalidateStreamAttempt( LiveStateTransition& transition, bool interrupted )
{
    auto& snapshot = transition.snapshot;
    if ( snapshot.source.stoppingGeneration.has_value() ) {
        return;
    }
    const auto cancelledGeneration = snapshot.generation;
    snapshot.source.stoppingGeneration = cancelledGeneration;
    snapshot.source.stoppingDisposition = StopDisposition::SettleAccepted;
    snapshot.retiringAttemptInterrupted = interrupted;
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
    if ( snapshot.source.stoppingGeneration.has_value() || hasActiveStreamAttempt( snapshot ) ) {
        const auto stoppingReason = snapshot.source.stopReason;
        snapshot.startAfterStop = true;
        snapshot.runIntent = RunIntent::Running;
        invalidateStreamAttempt( transition, false );
        resetSourceAttempt( transition );
        enterSourceState( snapshot, SourceStatus::Stopping );
        snapshot.source.stopReason = stoppingReason;
        transition.accepted = true;
        return;
    }

    snapshot.startAfterStop = false;

    advanceNow( snapshot, event.at );
    snapshot.runIntent = RunIntent::Running;
    ++snapshot.generation;
    resetSourceAttempt( transition );
    snapshot.consecutiveFailures = 0u;

    const auto nextStatus = snapshot.infrastructure.status == InfrastructureStatus::Ready
                                ? SourceStatus::WaitingForDevice
                                : SourceStatus::WaitingForInfrastructure;
    enterSourceState( snapshot, nextStatus );

    transition.effects.insert( transition.effects.begin(),
                               LiveStateEffect{ EffectKind::InvalidateGeneration,
                                                snapshot.generation, Timestamp{ 0 }, 0u } );
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

    advanceNow( snapshot, event.at );
    snapshot.runIntent = RunIntent::Stopped;
    snapshot.startAfterStop = false;
    const auto existingStoppingGeneration = snapshot.source.stoppingGeneration;
    const auto existingDisposition = snapshot.source.stoppingDisposition;
    invalidateStreamAttempt( transition, false );
    snapshot.source.status = SourceStatus::Stopping;
    snapshot.source.stopReason = StopReason::User;
    const auto effectiveDisposition
        = existingStoppingGeneration.has_value()
                  && existingDisposition == StopDisposition::DiscardPending
              ? StopDisposition::DiscardPending
              : event.disposition;
    snapshot.source.stoppingDisposition = effectiveDisposition;
    bool cancellationUpdated = false;
    for ( auto& effect : transition.effects ) {
        if ( effect.kind == EffectKind::CancelStream ) {
            effect.stopDisposition = effectiveDisposition;
            cancellationUpdated = true;
        }
    }
    if ( existingStoppingGeneration.has_value() && !cancellationUpdated
         && effectiveDisposition != existingDisposition ) {
        LiveStateEffect cancellation{ EffectKind::CancelStream, *existingStoppingGeneration,
                                      Timestamp{ 0 }, 0u };
        cancellation.stopDisposition = effectiveDisposition;
        transition.effects.push_back( cancellation );
    }
    snapshot.source.awaitingUserReason.reset();
    snapshot.source.failure.reset();
    resetStreamReadiness( snapshot );

    clearRetry( transition );
    transition.accepted = true;
}

void apply( LiveStateTransition& transition, const DeviceAvailable& event,
            const LiveStateConfig& config );
void apply( LiveStateTransition& transition, const RetryDeadlineReached& event,
            const LiveStateConfig& config );

void apply( LiveStateTransition& transition, const StopCompleted& event, const LiveStateConfig& config )
{
    auto& snapshot = transition.snapshot;
    if ( snapshot.source.stoppingGeneration != event.generation ) {
        return;
    }

    advanceNow( snapshot, event.at );
    snapshot.source.stoppingGeneration.reset();
    snapshot.retiringAttemptInterrupted = false;
    if ( snapshot.source.status == SourceStatus::Failed && snapshot.source.failure
         && snapshot.source.failure->category == ErrorCategory::Capture ) {
        snapshot.startAfterStop = false;
        transition.accepted = true;
        return;
    }
    if ( snapshot.startAfterStop ) {
        apply( transition, StartRequested{ event.at }, config );
    }
    else if ( snapshot.runIntent == RunIntent::Stopped ) {
        enterSourceState( snapshot, SourceStatus::Stopped );
        snapshot.source.stopReason = StopReason::User;
    }
    else if ( snapshot.retryTimer.has_value() && snapshot.now >= snapshot.retryTimer->deadline ) {
        apply( transition, RetryDeadlineReached{ snapshot.generation, snapshot.now }, config );
    }
    else if ( snapshot.devicePresent ) {
        apply( transition, DeviceAvailable{ snapshot.generation, snapshot.now }, config );
    }
    transition.accepted = true;
}

void closeRecoveryGate( LiveStateSnapshot& snapshot )
{
    enterSourceState( snapshot, SourceStatus::Failed );
    snapshot.source.failure = LiveSourceError{
        ErrorCategory::Stream, "automatic-recovery-disabled", ErrorScope::Stream,
        RetryPolicy::Never, "The live stream was interrupted; reconnect explicitly to resume.", {} };
}

void apply( LiveStateTransition& transition, const InfrastructureChanged& event,
            const LiveStateConfig& config )
{
    auto& snapshot = transition.snapshot;
    const auto previousStatus = snapshot.infrastructure.status;
    advanceNow( snapshot, event.at );
    snapshot.infrastructure = InfrastructureState{ event.status, event.ownership };

    if ( snapshot.runIntent == RunIntent::Running
         && snapshot.source.status != SourceStatus::Failed
         && snapshot.source.status != SourceStatus::Stopping ) {
        if ( event.status == InfrastructureStatus::Ready ) {
            if ( snapshot.source.status == SourceStatus::WaitingForInfrastructure ) {
                enterSourceState( snapshot, SourceStatus::WaitingForDevice );
            }
        }
        else {
            const bool interrupted = hasActiveStreamAttempt( snapshot );
            if ( interrupted ) {
                invalidateStreamAttempt( transition, true );
            }
            snapshot.devicePresent = false;
            resetSourceAttempt( transition );
            enterSourceState( snapshot, SourceStatus::WaitingForInfrastructure );
            if ( interrupted && !config.autoReconnectEnabled ) {
                closeRecoveryGate( snapshot );
            }
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

void apply( LiveStateTransition& transition, const AvailabilityFailed& event,
            const LiveStateConfig& )
{
    auto& snapshot = transition.snapshot;
    if ( !hasCurrentRunningGeneration( snapshot, event )
         || ( snapshot.source.status != SourceStatus::WaitingForDevice
              && snapshot.source.status != SourceStatus::AwaitingUser ) ) {
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
    snapshot.devicePresent = true;
    transition.accepted = true;
    if ( snapshot.source.stoppingGeneration.has_value() ) {
        return;
    }
    resetSourceAttempt( transition );
    enterSourceState( snapshot, SourceStatus::OpeningStream );
    transition.effects.push_back(
        LiveStateEffect{ EffectKind::OpenStream, snapshot.generation, Timestamp{ 0 }, 0u } );
    transition.accepted = true;
}

void apply( LiveStateTransition& transition, const DeviceAbsent& event, const LiveStateConfig& config )
{
    auto& snapshot = transition.snapshot;
    if ( !hasCurrentRunningGeneration( snapshot, event )
         || snapshot.source.status == SourceStatus::Failed
         || snapshot.source.status == SourceStatus::Stopping ) {
        return;
    }

    advanceNow( snapshot, event.at );
    snapshot.devicePresent = false;
    const bool interrupted = hasActiveStreamAttempt( snapshot );
    if ( interrupted ) {
        invalidateStreamAttempt( transition, true );
    }
    resetSourceAttempt( transition );
    enterSourceState( snapshot, snapshot.infrastructure.status == InfrastructureStatus::Ready
                                    ? SourceStatus::WaitingForDevice
                                    : SourceStatus::WaitingForInfrastructure );
    if ( interrupted && !config.autoReconnectEnabled ) {
        closeRecoveryGate( snapshot );
    }
    transition.accepted = true;
}

void apply( LiveStateTransition& transition, const UserActionRequired& event,
            const LiveStateConfig& config )
{
    auto& snapshot = transition.snapshot;
    const auto activeStreamAttempt = hasActiveStreamAttempt( snapshot );
    if ( !hasCurrentRunningGeneration( snapshot, event )
         || ( snapshot.source.status != SourceStatus::WaitingForDevice
              && snapshot.source.status != SourceStatus::AwaitingUser && !activeStreamAttempt ) ) {
        return;
    }

    advanceNow( snapshot, event.at );
    snapshot.devicePresent = false;
    if ( activeStreamAttempt ) {
        invalidateStreamAttempt( transition, true );
    }
    resetSourceAttempt( transition );
    enterSourceState( snapshot, SourceStatus::AwaitingUser );
    snapshot.source.awaitingUserReason = event.reason;
    if ( activeStreamAttempt && !config.autoReconnectEnabled ) { closeRecoveryGate( snapshot ); }
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
    const bool retiring = snapshot.source.stoppingGeneration == event.generation
        && snapshot.source.stoppingDisposition == StopDisposition::SettleAccepted
        && !( snapshot.source.failure && snapshot.source.failure->category == ErrorCategory::Capture );
    const bool active = hasCurrentGeneration( snapshot, event )
        && ( snapshot.source.status == SourceStatus::OpeningStream
             || snapshot.source.status == SourceStatus::Streaming );
    if ( !active && !retiring ) {
        return;
    }

    advanceNow( transition.snapshot, event.at );
    if ( event.byteCount != 0u ) {
        transition.snapshot.payloadReceived = true;
    }
    transition.effects.push_back( LiveStateEffect{ EffectKind::AppendBytes, event.generation,
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
    invalidateStreamAttempt( transition, true );
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
         || snapshot.source.stoppingGeneration.has_value()
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

void apply( LiveStateTransition& transition, const CaptureHealthChanged& event, const LiveStateConfig& )
{
    if ( event.healthy == event.error.has_value() ) { return; }
    advanceNow( transition.snapshot, event.at );
    transition.snapshot.captureHealthy = event.healthy;
    transition.snapshot.captureHealthError = event.error;
    transition.accepted = true;
}

void apply( LiveStateTransition& transition, const CaptureFailed& event, const LiveStateConfig& )
{
    auto& snapshot = transition.snapshot;
    if ( event.generation != snapshot.generation
         && event.generation != snapshot.source.stoppingGeneration ) { return; }
    advanceNow( snapshot, event.at );
    if ( hasActiveStreamAttempt( snapshot ) ) { invalidateStreamAttempt( transition, true ); }
    resetSourceAttempt( transition );
    enterSourceState( snapshot, SourceStatus::Failed );
    snapshot.source.failure = event.error;
    transition.accepted = true;
}

void apply( LiveStateTransition& transition, const OutputBindingChanged& event,
            const LiveStateConfig& )
{
    auto& snapshot = transition.snapshot;
    const auto requiresError = event.state == OutputBindingState::Degraded;
    if ( requiresError != event.error.has_value() ) {
        return;
    }

    advanceNow( snapshot, event.at );
    snapshot.outputBinding = event.state;
    snapshot.outputBindingError = event.error;
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
    case SourceStatus::AwaitingUser:
    case SourceStatus::Streaming:
    case SourceStatus::RetryWait:
    case SourceStatus::Failed:
        return true;
    case SourceStatus::WaitingForInfrastructure:
    case SourceStatus::WaitingForDevice:
    case SourceStatus::OpeningStream:
    case SourceStatus::Stopping:
        return false;
    }

    return false;
}

} // namespace

void LiveIntegritySummary::record( const std::string& code, std::uint64_t bytes )
{
    if ( recentEvents.size() >= MaxRecentEvents ) {
        const auto excess = recentEvents.size() - MaxRecentEvents + 1u;
        olderEvents += std::min<std::uint64_t>( excess,
            std::numeric_limits<std::uint64_t>::max() - olderEvents );
        recentEvents.erase( recentEvents.begin(),
                           recentEvents.begin() + static_cast<std::ptrdiff_t>( excess ) );
    }
    recentEvents.push_back( IntegrityEvent{ code.substr( 0, 96 ), bytes } );
}

bool LiveIntegritySummary::operator==( const LiveIntegritySummary& other ) const
{
    return std::tie( offeredBytes, dequeuedBytes, acceptedBytes, committedBytes, committedLines,
                    discardedBytes, uncertainBytes, outputBytes, olderEvents, outputProgressUnknown,
                    gapPossible, replayPossible, recentEvents )
        == std::tie( other.offeredBytes, other.dequeuedBytes, other.acceptedBytes,
                     other.committedBytes, other.committedLines, other.discardedBytes,
                     other.uncertainBytes, other.outputBytes, other.olderEvents,
                     other.outputProgressUnknown, other.gapPossible, other.replayPossible,
                     other.recentEvents );
}

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
    if ( snapshot.source.status == SourceStatus::AwaitingUser ) {
        presentation.awaitingUserReason = snapshot.source.awaitingUserReason;
    }
    if ( snapshot.source.failure.has_value() ) {
        presentation.failureMessage = snapshot.source.failure->message;
    }
    presentation.outputBinding = snapshot.outputBinding;

    return presentation;
}

} // namespace klogg::livecapture
