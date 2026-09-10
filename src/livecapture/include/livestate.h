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

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace klogg::livecapture {

using Generation = std::uint64_t;
using Timestamp = std::chrono::milliseconds;

enum class RunIntent : std::uint8_t { Stopped, Running };
enum class StopDisposition : std::uint8_t { DiscardPending, SettleAccepted };

// Host acceptance is not source completeness or disk durability.
enum class DeliveryDisposition : std::uint8_t { Complete, Rejected, PartialKnown, PartialUnknown };
struct CaptureDeliveryResult {
    DeliveryDisposition disposition{ DeliveryDisposition::Complete };
    std::uint64_t acceptedBytes{ 0 };
    std::uint64_t committedBytes{ 0 };
    std::uint64_t committedLines{ 0 };
    std::optional<std::uint64_t> outputBytes{ 0u };
    bool notificationFailed{ false };
    std::optional<std::string> failureCode;
};

struct IntegrityEvent {
    std::string code;
    std::uint64_t bytes{ 0 };
    bool operator==( const IntegrityEvent& other ) const
    { return code == other.code && bytes == other.bytes; }
};
struct LiveIntegritySummary {
    static constexpr unsigned SchemaVersion = 1;
    static constexpr std::size_t MaxRecentEvents = 32;
    std::uint64_t offeredBytes{ 0 };
    std::uint64_t dequeuedBytes{ 0 };
    std::uint64_t acceptedBytes{ 0 };
    std::uint64_t committedBytes{ 0 };
    std::uint64_t committedLines{ 0 };
    std::uint64_t discardedBytes{ 0 };
    std::uint64_t uncertainBytes{ 0 };
    std::uint64_t outputBytes{ 0 };
    std::uint64_t olderEvents{ 0 };
    bool outputProgressUnknown{ false };
    bool gapPossible{ false };
    bool replayPossible{ false };
    std::vector<IntegrityEvent> recentEvents;
    void record( const std::string& code, std::uint64_t bytes = 0 );
    bool operator==( const LiveIntegritySummary& other ) const;
};

enum class InfrastructureStatus : std::uint8_t {
    Unknown,
    Connecting,
    Ready,
    Unavailable,
    Stopping
};
enum class InfrastructureOwnership : std::uint8_t { ExternalShared, AppShared };

struct InfrastructureState {
    InfrastructureStatus status{ InfrastructureStatus::Unknown };
    std::optional<InfrastructureOwnership> ownership;
};

enum class StopReason : std::uint8_t { NeverStarted, User, Restored };
enum class AwaitingUserReason : std::uint8_t { Authorize, Pair, Trust, Unlock };

enum class SourceStatus : std::uint8_t {
    Stopped,
    WaitingForInfrastructure,
    WaitingForDevice,
    AwaitingUser,
    OpeningStream,
    Streaming,
    RetryWait,
    Stopping,
    Failed
};

enum class OutputBindingState : std::uint8_t { Healthy, Degraded };

enum class ErrorCategory : std::uint8_t {
    Configuration,
    Backend,
    Infrastructure,
    Device,
    Service,
    Stream,
    Capture,
    Cancelled,
    Internal
};

enum class ErrorScope : std::uint8_t { Infrastructure, Device, Service, Stream, Capture };
enum class RetryPolicy : std::uint8_t {
    Never,
    Immediate,
    Backoff,
    WaitForInfrastructure,
    WaitForDevice,
    AwaitUser
};

struct LiveSourceError {
    LiveSourceError() = default;
    LiveSourceError( ErrorCategory errorCategory, std::string errorCode, ErrorScope errorScope,
                     RetryPolicy policy, std::string userMessage, std::string detail,
                     std::optional<AwaitingUserReason> userReason = std::nullopt )
        : category( errorCategory )
        , code( std::move( errorCode ) )
        , scope( errorScope )
        , retryPolicy( policy )
        , message( std::move( userMessage ) )
        , nativeDetail( std::move( detail ) )
        , awaitingUserReason( userReason )
    {
    }

    ErrorCategory category{ ErrorCategory::Internal };
    std::string code;
    ErrorScope scope{ ErrorScope::Stream };
    RetryPolicy retryPolicy{ RetryPolicy::Never };
    std::string message;
    std::string nativeDetail;
    std::optional<AwaitingUserReason> awaitingUserReason;
};

struct RetryState {
    unsigned attempt{ 0 };
    LiveSourceError error;
};

struct RetryTimer {
    Generation generation{ 0 };
    Timestamp deadline{ 0 };
};

struct SourceState {
    SourceStatus status{ SourceStatus::Stopped };
    std::optional<StopReason> stopReason{ StopReason::NeverStarted };
    std::optional<Generation> stoppingGeneration;
    StopDisposition stoppingDisposition{ StopDisposition::SettleAccepted };
    std::optional<AwaitingUserReason> awaitingUserReason;
    std::optional<RetryState> retry;
    std::optional<LiveSourceError> failure;
};

struct LiveStateSnapshot {
    RunIntent runIntent{ RunIntent::Stopped };
    InfrastructureState infrastructure;
    SourceState source;
    OutputBindingState outputBinding{ OutputBindingState::Healthy };
    std::optional<LiveSourceError> outputBindingError;
    bool captureHealthy{ true };
    std::optional<LiveSourceError> captureHealthError;
    Generation generation{ 0 };
    Timestamp now{ 0 };
    unsigned consecutiveFailures{ 0 };
    bool protocolServiceReady{ false };
    bool streamHandlePresent{ false };
    bool readArmed{ false };
    std::optional<Timestamp> streamingSince;
    std::optional<RetryTimer> retryTimer;
    bool devicePresent{ false };
    bool startAfterStop{ false };
};

struct StartRequested {
    Timestamp at{ 0 };
};
struct StopRequested {
    Timestamp at{ 0 };
    StopDisposition disposition{ StopDisposition::DiscardPending };
};
struct StopCompleted {
    Generation generation{ 0 };
    Timestamp at{ 0 };
};
struct InfrastructureChanged {
    InfrastructureStatus status{ InfrastructureStatus::Unknown };
    std::optional<InfrastructureOwnership> ownership;
    Timestamp at{ 0 };
};
struct InfrastructureFailed {
    Generation generation{ 0 };
    LiveSourceError error;
    Timestamp at{ 0 };
};
struct AvailabilityFailed {
    Generation generation{ 0 };
    LiveSourceError error;
    Timestamp at{ 0 };
};
struct DeviceAvailable {
    Generation generation{ 0 };
    Timestamp at{ 0 };
};
struct DeviceAbsent {
    Generation generation{ 0 };
    Timestamp at{ 0 };
};
struct UserActionRequired {
    Generation generation{ 0 };
    AwaitingUserReason reason{ AwaitingUserReason::Authorize };
    Timestamp at{ 0 };
};
struct ProtocolServiceReady {
    Generation generation{ 0 };
    Timestamp at{ 0 };
};
struct StreamHandleOpened {
    Generation generation{ 0 };
    Timestamp at{ 0 };
};
struct StreamReadArmed {
    Generation generation{ 0 };
    Timestamp at{ 0 };
};
struct StreamBytesReceived {
    Generation generation{ 0 };
    std::size_t byteCount{ 0 };
    Timestamp at{ 0 };
};
struct StreamStable {
    Generation generation{ 0 };
    Timestamp at{ 0 };
};
struct RetryRequested {
    Generation generation{ 0 };
    LiveSourceError error;
    unsigned attempt{ 0 };
    Timestamp deadline{ 0 };
    Timestamp at{ 0 };
};
struct RetryDeadlineReached {
    Generation generation{ 0 };
    Timestamp at{ 0 };
};
struct CaptureHealthChanged {
    bool healthy{ true };
    std::optional<LiveSourceError> error;
    Timestamp at{ 0 };
};
struct CaptureFailed {
    Generation generation{ 0 };
    LiveSourceError error;
    Timestamp at{ 0 };
};
struct OutputBindingChanged {
    OutputBindingState state{ OutputBindingState::Healthy };
    std::optional<LiveSourceError> error;
    Timestamp at{ 0 };
};
struct TimeAdvanced {
    Timestamp at{ 0 };
};

using LiveStateEvent
    = std::variant<StartRequested, StopRequested, StopCompleted, InfrastructureChanged,
                   InfrastructureFailed, AvailabilityFailed, DeviceAvailable, DeviceAbsent,
                   UserActionRequired, ProtocolServiceReady, StreamHandleOpened, StreamReadArmed,
                   StreamBytesReceived, StreamStable, RetryRequested, RetryDeadlineReached,
                   CaptureHealthChanged, CaptureFailed, OutputBindingChanged, TimeAdvanced>;

enum class EffectKind : std::uint8_t {
    InvalidateGeneration,
    CancelStream,
    StartInfrastructure,
    OpenStream,
    AppendBytes,
    ArmRetryTimer,
    CancelRetryTimer
};

struct LiveStateEffect {
    EffectKind kind{ EffectKind::OpenStream };
    Generation generation{ 0 };
    Timestamp deadline{ 0 };
    std::size_t byteCount{ 0 };
    StopDisposition stopDisposition{ StopDisposition::SettleAccepted };
};

struct LiveStateConfig {
    unsigned maxRetryAttempts{ 5 };
    Timestamp stabilityInterval{ std::chrono::seconds{ 10 } };
    bool autoReconnectEnabled{ true };
};

struct LiveStateTransition {
    LiveStateSnapshot snapshot;
    std::vector<LiveStateEffect> effects;
    bool accepted{ false };
};

enum class PresentationStatus : std::uint8_t {
    Stopped,
    WaitingForInfrastructure,
    WaitingForDevice,
    AwaitingUser,
    OpeningStream,
    Connected,
    RetryWait,
    Stopping,
    Failed
};

struct LiveStatePresentation {
    PresentationStatus status{ PresentationStatus::Stopped };
    bool disconnectEnabled{ false };
    bool reconnectEnabled{ false };
    bool retryCountdownVisible{ false };
    Timestamp retryRemaining{ 0 };
    std::optional<unsigned> retryAttempt;
    std::optional<AwaitingUserReason> awaitingUserReason;
    std::string failureMessage;
    OutputBindingState outputBinding{ OutputBindingState::Healthy };
};

LiveStateSnapshot initialLiveState();
LiveStateTransition reduce( const LiveStateSnapshot& snapshot, const LiveStateEvent& event,
                            const LiveStateConfig& config );
LiveStatePresentation projectLiveState( const LiveStateSnapshot& snapshot );

} // namespace klogg::livecapture
