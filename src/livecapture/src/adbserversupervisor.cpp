/*
 * Copyright (C) 2026 ZEACENT and other contributors
 *
 * This file is part of klogg.
 *
 * klogg is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "adbserversupervisor.h"

#include <QDir>

#include <algorithm>
#include <array>
#include <sstream>
#include <utility>

namespace klogg::livecapture::adb {
namespace {

enum class ProbePurpose : std::uint8_t {
    Initial,
    UnderStartupLock,
    LockContended,
    StartupReadiness,
    Health,
    Reconnect
};

constexpr std::size_t scheduleKindCount
    = static_cast<std::size_t>( AdbServerScheduleKind::ReconnectBackoff ) + 1u;

std::size_t scheduleIndex( AdbServerScheduleKind kind )
{
    return static_cast<std::size_t>( kind );
}

std::string diagnosticOr( const std::string& diagnostic, const char* fallback )
{
    return diagnostic.empty() ? std::string{ fallback } : diagnostic;
}

LiveSourceError makeSupervisorError( ErrorCategory category, std::string code, std::string message,
                                     std::string nativeDetail, RetryPolicy retryPolicy )
{
    return LiveSourceError{ category,    std::move( code ),    ErrorScope::Infrastructure,
                            retryPolicy, std::move( message ), std::move( nativeDetail ) };
}

LiveSourceError makeInfrastructureError( std::string code, std::string message,
                                         std::string nativeDetail, RetryPolicy retryPolicy )
{
    return makeSupervisorError( ErrorCategory::Infrastructure, std::move( code ),
                                std::move( message ), std::move( nativeDetail ), retryPolicy );
}

} // namespace

class AdbServerSupervisor::Impl final {
public:
    Impl( AdbServerSupervisor& supervisor, AdbServerSupervisorConfig config, AdbServerProbe& probe,
          AdbServerLauncher& launcher, AdbServerStartupLock& startupLock, AdbKeyStore& keyStore,
          AdbServerScheduler& scheduler )
        : supervisor_( supervisor )
        , config_( std::move( config ) )
        , probe_( probe )
        , launcher_( launcher )
        , startupLock_( startupLock )
        , keyStore_( keyStore )
        , scheduler_( scheduler )
        , callbackGate_( std::make_shared<CallbackGate>() )
    {
        callbackGate_->owner = this;
    }

    // Injected cleanup effects follow the interface contract and do not throw.
    // NOLINTNEXTLINE(bugprone-exception-escape)
    ~Impl()
    {
        callbackGate_->owner = nullptr;
        stopInternal( false );
    }

    void start( Generation generation )
    {
        if ( running_ ) {
            if ( snapshot_.generation == generation ) {
                return;
            }
            stopInternal( false );
        }

        running_ = true;
        ++runSerial_;
        resetRunState( generation );

        if ( config_.configurationError.has_value() ) {
            running_ = false;
            const auto& error = *config_.configurationError;
            fail( AdbServerSupervisorStatus::InvalidConfiguration, error.code, error.message,
                  error.nativeDetail, error.retryPolicy, error.category );
            return;
        }

        const auto validationError = validateConfiguration();
        if ( !validationError.empty() ) {
            running_ = false;
            fail( AdbServerSupervisorStatus::InvalidConfiguration, "invalid-configuration",
                  "Invalid ADB server configuration.", validationError, RetryPolicy::Never,
                  ErrorCategory::Configuration );
            return;
        }

        beginProbe( ProbePurpose::Initial );
    }

    void stop( Generation generation )
    {
        if ( snapshot_.generation != generation ) {
            return;
        }
        stopInternal( true );
    }

    void grantKeyGenerationConsent( Generation generation, bool granted )
    {
        if ( !running_ || snapshot_.generation != generation
             || snapshot_.status != AdbServerSupervisorStatus::AwaitingKeyGenerationConsent ) {
            return;
        }

        if ( !granted ) {
            snapshot_.keyConsent = AdbServerKeyConsentState::Denied;
            failStartup( "key-generation-denied", "ADB key generation was denied.",
                         "ADB standard key generation was denied.", false, RetryPolicy::Never,
                         ErrorCategory::Cancelled );
            return;
        }

        snapshot_.keyConsent = AdbServerKeyConsentState::Granted;
        const auto runSerial = runSerial_;
        publishState();
        if ( !running_ || runSerial != runSerial_
             || snapshot_.status != AdbServerSupervisorStatus::AwaitingKeyGenerationConsent ) {
            return;
        }
        const auto generated = keyStore_.generateStandardKey();
        if ( !generated.generated ) {
            failStartup(
                "key-generation-failed", "Unable to prepare the standard ADB key.",
                diagnosticOr( generated.diagnostic, "Unable to prepare the standard ADB key." ),
                false );
            return;
        }

        launchPackagedServer();
    }

    const AdbServerSupervisorSnapshot& snapshot() const noexcept
    {
        return snapshot_;
    }

private:
    struct CallbackGate {
        Impl* owner{ nullptr };
    };

    struct ProbeInvocation {
        bool completed{ false };
    };

    struct LockInvocation {
        bool returned{ false };
        bool completed{ false };
        bool releaseRequested{ false };
        AdbServerToken token{ 0 };
        AdbServerStartupLockResult result;
    };

    struct LaunchInvocation {
        bool completed{ false };
        bool terminal{ false };
    };

    std::string validateConfiguration() const
    {
        if ( config_.endpoint.address != QHostAddress::LocalHost
             || config_.endpoint.port != 5037u ) {
            return "ADB server supervision requires the standard 127.0.0.1:5037 endpoint.";
        }
        if ( config_.packagedServerPath.isEmpty()
             || !QDir::isAbsolutePath( config_.packagedServerPath ) ) {
            return "ADB server supervision requires an explicit absolute packaged executable path.";
        }
        if ( config_.lockPath.isEmpty() || !QDir::isAbsolutePath( config_.lockPath ) ) {
            return "ADB server supervision requires an explicit per-user lock path.";
        }
        if ( config_.minimumProtocolVersion == 0u ) {
            return "ADB server supervision requires a minimum protocol version.";
        }
        if ( config_.readinessProbeInterval.count() < 0 || config_.startupTimeout.count() <= 0
             || config_.healthProbeInterval.count() <= 0 || config_.reconnectBackoff.empty()
             || std::any_of( config_.reconnectBackoff.begin(), config_.reconnectBackoff.end(),
                             []( const auto delay ) { return delay.count() < 0; } ) ) {
            return "ADB server supervision requires valid monotonic scheduling intervals.";
        }
        return {};
    }

    void resetRunState( Generation generation )
    {
        cancelProbe();
        cancelLockRequest();
        cancelAllSchedules();
        releaseStartupLock();
        cleanupPreReadyLaunch();

        serverPublished_ = false;
        launchStarted_ = false;
        launchCleanupPermitted_ = false;
        activeLaunchToken_ = 0u;
        activeLaunchInvocation_.reset();
        startupRetryAttempt_ = 0u;
        reconnectAttempt_ = 0u;
        lastStartupProbeDiagnostic_.clear();
        snapshot_.generation = generation;
        snapshot_.status = AdbServerSupervisorStatus::Stopped;
        snapshot_.infrastructure
            = InfrastructureState{ InfrastructureStatus::Unknown, std::nullopt };
        snapshot_.keyConsent = AdbServerKeyConsentState::NotRequired;
        snapshot_.serverIdentity.clear();
        snapshot_.protocolVersion = 0;
        snapshot_.error.reset();
    }

    void stopInternal( bool publish )
    {
        if ( !running_ ) {
            return;
        }

        running_ = false;
        ++runSerial_;
        cancelProbe();
        cancelLockRequest();
        cancelAllSchedules();

        if ( !serverPublished_ ) {
            cleanupPreReadyLaunch();
            releaseStartupLock();
            snapshot_.status = AdbServerSupervisorStatus::Stopped;
            snapshot_.infrastructure
                = InfrastructureState{ InfrastructureStatus::Unknown, std::nullopt };
            snapshot_.serverIdentity.clear();
            snapshot_.protocolVersion = 0;
            snapshot_.error = LiveSourceError{
                ErrorCategory::Cancelled,          "adb-supervisor-stopped",
                ErrorScope::Infrastructure,        RetryPolicy::Never,
                "ADB server supervision stopped.", "ADB server supervision stopped."
            };
            if ( publish ) {
                publishState();
            }
        }
        else {
            // Once ready, AppShared is published infrastructure. Stopping a
            // source or the app only drops observation; it never kills it.
            releaseStartupLock();
        }
    }

    void beginProbe( ProbePurpose purpose )
    {
        if ( !running_ ) {
            return;
        }

        cancelProbe();
        const auto runSerial = runSerial_;
        if ( purpose != ProbePurpose::Health ) {
            snapshot_.status = purpose == ProbePurpose::Reconnect
                                   ? AdbServerSupervisorStatus::RetryWait
                                   : AdbServerSupervisorStatus::Probing;
            snapshot_.infrastructure.status = purpose == ProbePurpose::Reconnect
                                                  ? InfrastructureStatus::Unavailable
                                                  : InfrastructureStatus::Connecting;
            snapshot_.infrastructure.ownership.reset();
            publishState();
            if ( !running_ || runSerial != runSerial_ ) {
                return;
            }
        }

        const auto serial = ++probeSerial_;
        const auto invocation = std::make_shared<ProbeInvocation>();
        const std::weak_ptr<CallbackGate> weakGate = callbackGate_;
        const auto token = probe_.probe(
            config_.endpoint, [ weakGate, invocation, runSerial, serial,
                                purpose ]( AdbServerProbeResult result ) mutable {
                invocation->completed = true;
                const auto gate = weakGate.lock();
                if ( gate != nullptr && gate->owner != nullptr ) {
                    gate->owner->probeCompleted( runSerial, serial, purpose, std::move( result ) );
                }
            } );
        if ( running_ && runSerial == runSerial_ && serial == probeSerial_ ) {
            if ( !invocation->completed ) {
                activeProbeToken_ = token;
            }
        }
        else if ( token != 0u && !invocation->completed ) {
            probe_.cancel( token );
        }
    }

    void probeCompleted( std::uint64_t runSerial, std::uint64_t serial, ProbePurpose purpose,
                         AdbServerProbeResult result )
    {
        if ( !running_ || runSerial != runSerial_ || serial != probeSerial_ ) {
            return;
        }
        activeProbeToken_ = 0;

        if ( result.state == AdbServerProbeState::Ready ) {
            if ( !isCompatible( result ) ) {
                const auto detail = compatibilityDiagnostic( result );
                if ( purpose == ProbePurpose::StartupReadiness ) {
                    failStartup( "incompatible-server", "The ADB server is incompatible.", detail,
                                 true, RetryPolicy::Never );
                }
                else {
                    releaseStartupLock();
                    if ( purpose == ProbePurpose::Health ) {
                        ++snapshot_.epoch;
                        snapshot_.serverIdentity.clear();
                        snapshot_.protocolVersion = 0u;
                    }
                    fail( AdbServerSupervisorStatus::Incompatible, "incompatible-server",
                          "The ADB server is incompatible.", detail, RetryPolicy::Never );
                }
                return;
            }

            switch ( purpose ) {
            case ProbePurpose::Initial:
            case ProbePurpose::UnderStartupLock:
            case ProbePurpose::LockContended:
                publishReady( std::move( result ), InfrastructureOwnership::ExternalShared );
                return;
            case ProbePurpose::StartupReadiness:
                publishReady( std::move( result ), InfrastructureOwnership::AppShared );
                return;
            case ProbePurpose::Health: {
                const auto ownership = snapshot_.serverIdentity == result.serverIdentity
                                           ? snapshot_.infrastructure.ownership.value_or(
                                                 InfrastructureOwnership::ExternalShared )
                                           : InfrastructureOwnership::ExternalShared;
                publishReady( std::move( result ), ownership );
                return;
            }
            case ProbePurpose::Reconnect:
                publishReady( std::move( result ), InfrastructureOwnership::ExternalShared );
                return;
            }
        }

        switch ( purpose ) {
        case ProbePurpose::Initial:
            if ( result.state == AdbServerProbeState::Absent ) {
                acquireStartupLock();
            }
            else {
                fail( AdbServerSupervisorStatus::Failed, "probe-failed",
                      "Unable to probe the ADB server.",
                      diagnosticOr( result.diagnostic, "Unable to probe the ADB server." ),
                      RetryPolicy::Backoff );
            }
            return;
        case ProbePurpose::UnderStartupLock:
            if ( result.state == AdbServerProbeState::Absent ) {
                inspectStandardKey();
            }
            else {
                failStartup(
                    "probe-under-lock-failed", "Unable to verify the ADB endpoint.",
                    diagnosticOr( result.diagnostic,
                                  "Unable to verify the ADB endpoint under the startup lock." ),
                    false );
            }
            return;
        case ProbePurpose::LockContended:
            if ( result.state == AdbServerProbeState::Absent ) {
                acquireStartupLock();
            }
            else {
                scheduleLockRetry();
            }
            return;
        case ProbePurpose::StartupReadiness:
            lastStartupProbeDiagnostic_
                = diagnosticOr( result.diagnostic, "ADB server is not ready yet." );
            scheduleReadinessProbe();
            return;
        case ProbePurpose::Health:
            loseReadyServer( result );
            return;
        case ProbePurpose::Reconnect:
            snapshot_.error = makeInfrastructureError(
                "server-unavailable", "The ADB server remains unavailable.",
                diagnosticOr( result.diagnostic, "ADB server remains unavailable." ),
                RetryPolicy::Backoff );
            {
                const auto currentRun = runSerial_;
                publishState();
                if ( running_ && currentRun == runSerial_
                     && snapshot_.status == AdbServerSupervisorStatus::RetryWait ) {
                    scheduleReconnect();
                }
            }
            return;
        }
    }

    bool isCompatible( const AdbServerProbeResult& result ) const
    {
        if ( result.protocolVersion < config_.minimumProtocolVersion ) {
            return false;
        }
        return std::all_of( config_.requiredFeatures.begin(), config_.requiredFeatures.end(),
                            [ &result ]( const std::string& required ) {
                                return std::find( result.features.begin(), result.features.end(),
                                                  required )
                                       != result.features.end();
                            } );
    }

    std::string compatibilityDiagnostic( const AdbServerProbeResult& result ) const
    {
        std::ostringstream diagnostic;
        if ( result.protocolVersion < config_.minimumProtocolVersion ) {
            diagnostic << "ADB server protocol version " << result.protocolVersion
                       << " is older than required version " << config_.minimumProtocolVersion
                       << '.';
            return diagnostic.str();
        }

        diagnostic << "ADB server is missing required feature";
        bool first = true;
        for ( const auto& feature : config_.requiredFeatures ) {
            if ( std::find( result.features.begin(), result.features.end(), feature )
                 == result.features.end() ) {
                diagnostic << ( first ? " " : ", " ) << feature;
                first = false;
            }
        }
        diagnostic << '.';
        return diagnostic.str();
    }

    void acquireStartupLock()
    {
        snapshot_.status = AdbServerSupervisorStatus::WaitingForStartupLock;
        snapshot_.infrastructure
            = InfrastructureState{ InfrastructureStatus::Connecting, std::nullopt };
        const auto runSerial = runSerial_;
        publishState();
        if ( !running_ || runSerial != runSerial_ ) {
            return;
        }

        cancelLockRequest();
        const auto serial = ++lockSerial_;
        const auto invocation = std::make_shared<LockInvocation>();
        const std::weak_ptr<CallbackGate> weakGate = callbackGate_;
        auto* const lockService = &startupLock_;
        const auto token = startupLock_.acquire(
            config_.lockPath, [ weakGate, invocation, lockService, runSerial,
                                serial ]( AdbServerStartupLockResult result ) mutable {
                invocation->completed = true;
                invocation->result = std::move( result );
                const auto gate = weakGate.lock();
                if ( gate != nullptr && gate->owner != nullptr ) {
                    gate->owner->startupLockCompleted( runSerial, serial, invocation );
                }
                else if ( invocation->result.state == AdbServerStartupLockState::Acquired ) {
                    if ( invocation->returned && invocation->token != 0u ) {
                        lockService->release( invocation->token );
                    }
                    else {
                        invocation->releaseRequested = true;
                    }
                }
            } );
        invocation->token = token;
        invocation->returned = true;

        if ( running_ && runSerial == runSerial_ && serial == lockSerial_ ) {
            if ( !invocation->completed ) {
                activeLockRequestToken_ = token;
            }
            else if ( invocation->result.state == AdbServerStartupLockState::Acquired ) {
                pendingHeldLock_.reset();
                if ( invocation->releaseRequested ) {
                    startupLock_.release( token );
                }
                else {
                    heldLockToken_ = token;
                }
            }
        }
        else if ( token != 0u ) {
            if ( invocation->completed
                 && invocation->result.state == AdbServerStartupLockState::Acquired ) {
                startupLock_.release( token );
            }
            else if ( !invocation->completed ) {
                startupLock_.cancel( token );
            }
        }
    }

    void startupLockCompleted( std::uint64_t runSerial, std::uint64_t serial,
                               const std::shared_ptr<LockInvocation>& invocation )
    {
        if ( !running_ || runSerial != runSerial_ || serial != lockSerial_ ) {
            releaseStaleLock( invocation );
            return;
        }

        if ( invocation->returned ) {
            activeLockRequestToken_ = 0;
            if ( invocation->result.state == AdbServerStartupLockState::Acquired ) {
                heldLockToken_ = invocation->token;
            }
        }
        else if ( invocation->result.state == AdbServerStartupLockState::Acquired ) {
            pendingHeldLock_ = invocation;
        }

        switch ( invocation->result.state ) {
        case AdbServerStartupLockState::Acquired:
            beginProbe( ProbePurpose::UnderStartupLock );
            return;
        case AdbServerStartupLockState::Contended:
            scheduleLockRetry();
            return;
        case AdbServerStartupLockState::Failed:
            failStartup( "startup-lock-failed", "Unable to coordinate ADB server startup.",
                         diagnosticOr( invocation->result.diagnostic,
                                       "Unable to acquire the ADB server startup lock." ),
                         false );
            return;
        }
    }

    void releaseStaleLock( const std::shared_ptr<LockInvocation>& invocation )
    {
        if ( invocation->result.state != AdbServerStartupLockState::Acquired ) {
            return;
        }
        if ( invocation->returned && invocation->token != 0u ) {
            startupLock_.release( invocation->token );
        }
        else {
            invocation->releaseRequested = true;
        }
    }

    void scheduleLockRetry()
    {
        snapshot_.status = AdbServerSupervisorStatus::WaitingForStartupLock;
        snapshot_.infrastructure
            = InfrastructureState{ InfrastructureStatus::Connecting, std::nullopt };
        const auto runSerial = runSerial_;
        publishState();
        if ( !running_ || runSerial != runSerial_ ) {
            return;
        }
        schedule( AdbServerScheduleKind::LockRetry, config_.readinessProbeInterval,
                  []( Impl& self ) { self.beginProbe( ProbePurpose::LockContended ); } );
    }

    void inspectStandardKey()
    {
        const auto inspection = keyStore_.inspectStandardKey();
        switch ( inspection.state ) {
        case AdbServerStandardKeyState::Present:
            snapshot_.keyConsent = AdbServerKeyConsentState::NotRequired;
            launchPackagedServer();
            return;
        case AdbServerStandardKeyState::Absent:
            snapshot_.status = AdbServerSupervisorStatus::AwaitingKeyGenerationConsent;
            snapshot_.infrastructure
                = InfrastructureState{ InfrastructureStatus::Connecting, std::nullopt };
            snapshot_.keyConsent = AdbServerKeyConsentState::Required;
            {
                const auto runSerial = runSerial_;
                const auto generation = snapshot_.generation;
                const auto epoch = snapshot_.epoch;
                publishState();
                if ( running_ && runSerial == runSerial_
                     && snapshot_.status
                            == AdbServerSupervisorStatus::AwaitingKeyGenerationConsent ) {
                    Q_EMIT supervisor_.consentRequired( generation, epoch );
                }
            }
            return;
        case AdbServerStandardKeyState::Failed:
            failStartup(
                "key-inspection-failed", "Unable to inspect the standard ADB key.",
                diagnosticOr( inspection.diagnostic, "Unable to inspect the standard ADB key." ),
                false );
            return;
        }
    }

    void launchPackagedServer()
    {
        snapshot_.status = AdbServerSupervisorStatus::Starting;
        snapshot_.infrastructure
            = InfrastructureState{ InfrastructureStatus::Connecting, std::nullopt };
        const auto runSerial = runSerial_;
        publishState();
        if ( !running_ || runSerial != runSerial_ ) {
            return;
        }
        launchStarted_ = false;
        launchCleanupPermitted_ = false;
        serverPublished_ = false;

        const auto serial = ++launchSerial_;
        const auto invocation = std::make_shared<LaunchInvocation>();
        const std::weak_ptr<CallbackGate> weakGate = callbackGate_;
        const AdbServerLaunchRequest request{
            config_.packagedServerPath,
            { QStringLiteral( "server" ), QStringLiteral( "nodaemon" ) },
            false,
            config_.endpoint,
        };
        activeLaunchInvocation_ = invocation;
        const auto token = launcher_.launch( request, [ weakGate, invocation, runSerial, serial ](
                                                          AdbServerLaunchResult result ) mutable {
            invocation->completed = true;
            invocation->terminal = result.state != AdbServerLaunchState::Started;
            const auto gate = weakGate.lock();
            if ( gate != nullptr && gate->owner != nullptr ) {
                gate->owner->launchEvent( runSerial, serial, result );
            }
        } );
        if ( activeLaunchInvocation_ == invocation ) {
            activeLaunchInvocation_.reset();
        }
        // launch() may complete synchronously and advance the supervisor state
        // before it returns its cleanup token.
        // cppcheck-suppress knownConditionTrueFalse
        if ( running_ && runSerial == runSerial_ && serial == launchSerial_ ) {
            if ( !invocation->terminal ) {
                // A synchronous launch callback can publish the server.
                // cppcheck-suppress knownConditionTrueFalse
                if ( serverPublished_ ) {
                    launcher_.release( token );
                }
                else {
                    activeLaunchToken_ = token;
                }
            }
        }
        // A synchronous launch callback can permit cleanup before launch() returns.
        // cppcheck-suppress knownConditionTrueFalse
        else if ( token != 0u && ( !invocation->terminal || launchCleanupPermitted_ ) ) {
            launcher_.cleanup( token );
        }
    }

    void launchEvent( std::uint64_t runSerial, std::uint64_t serial,
                      const AdbServerLaunchResult& result )
    {
        if ( !running_ || runSerial != runSerial_ || serial != launchSerial_ || serverPublished_ ) {
            return;
        }

        launchCleanupPermitted_ = result.cleanupPermitted;
        switch ( result.state ) {
        case AdbServerLaunchState::Started:
            if ( launchStarted_ ) {
                return;
            }
            launchStarted_ = true;
            snapshot_.status = AdbServerSupervisorStatus::Starting;
            snapshot_.infrastructure
                = InfrastructureState{ InfrastructureStatus::Connecting, std::nullopt };
            publishState();
            if ( !running_ || runSerial != runSerial_ || serial != launchSerial_ ) {
                return;
            }
            schedule( AdbServerScheduleKind::StartupTimeout, config_.startupTimeout,
                      []( Impl& self ) { self.startupTimedOut(); } );
            if ( !running_ || runSerial != runSerial_ || serial != launchSerial_
                 || snapshot_.status != AdbServerSupervisorStatus::Starting ) {
                return;
            }
            scheduleReadinessProbe();
            return;
        case AdbServerLaunchState::Failed:
            if ( !result.cleanupPermitted ) {
                activeLaunchToken_ = 0u;
            }
            failStartup(
                "launch-failed", "The packaged ADB server failed to start.",
                diagnosticOr( result.diagnostic, "The packaged ADB server failed to start." ),
                result.cleanupPermitted );
            return;
        case AdbServerLaunchState::Exited:
            if ( !result.cleanupPermitted ) {
                activeLaunchToken_ = 0u;
            }
            failStartup( "pre-ready-exit", "The packaged ADB server exited before becoming ready.",
                         diagnosticOr( result.diagnostic,
                                       "The packaged ADB server exited before becoming ready." ),
                         result.cleanupPermitted );
            return;
        }
    }

    void scheduleReadinessProbe()
    {
        schedule( AdbServerScheduleKind::ReadinessProbe, config_.readinessProbeInterval,
                  []( Impl& self ) { self.beginProbe( ProbePurpose::StartupReadiness ); } );
    }

    void startupTimedOut()
    {
        std::string detail = "ADB server startup timed out.";
        if ( !lastStartupProbeDiagnostic_.empty() ) {
            detail += " Last probe: ";
            detail += lastStartupProbeDiagnostic_;
        }
        failStartup( "startup-timeout", "The packaged ADB server did not become ready.", detail,
                     true );
    }

    void publishReady( AdbServerProbeResult result, InfrastructureOwnership ownership )
    {
        const auto identityChanged = !snapshot_.serverIdentity.empty()
                                     && snapshot_.serverIdentity != result.serverIdentity;
        if ( snapshot_.epoch == 0u || identityChanged ) {
            ++snapshot_.epoch;
        }

        snapshot_.status = AdbServerSupervisorStatus::Ready;
        snapshot_.infrastructure = InfrastructureState{ InfrastructureStatus::Ready, ownership };
        snapshot_.serverIdentity = std::move( result.serverIdentity );
        snapshot_.protocolVersion = result.protocolVersion;
        snapshot_.error.reset();
        startupRetryAttempt_ = 0u;
        reconnectAttempt_ = 0u;
        if ( ownership == InfrastructureOwnership::AppShared ) {
            serverPublished_ = true;
            releasePublishedLaunch();
        }

        cancelSchedule( AdbServerScheduleKind::StartupTimeout );
        cancelSchedule( AdbServerScheduleKind::ReadinessProbe );
        cancelSchedule( AdbServerScheduleKind::LockRetry );
        cancelSchedule( AdbServerScheduleKind::StartupRetry );
        cancelSchedule( AdbServerScheduleKind::ReconnectBackoff );
        releaseStartupLock();
        const auto runSerial = runSerial_;
        publishState();
        if ( running_ && runSerial == runSerial_
             && snapshot_.status == AdbServerSupervisorStatus::Ready ) {
            scheduleHealthProbe();
        }
    }

    void scheduleHealthProbe()
    {
        schedule( AdbServerScheduleKind::HealthProbe, config_.healthProbeInterval,
                  []( Impl& self ) { self.beginProbe( ProbePurpose::Health ); } );
    }

    void loseReadyServer( const AdbServerProbeResult& result )
    {
        ++snapshot_.epoch;
        snapshot_.status = AdbServerSupervisorStatus::RetryWait;
        snapshot_.infrastructure
            = InfrastructureState{ InfrastructureStatus::Unavailable, std::nullopt };
        snapshot_.serverIdentity.clear();
        snapshot_.protocolVersion = 0;
        snapshot_.error = makeInfrastructureError(
            "server-disappeared", "The ADB server became unavailable.",
            diagnosticOr( result.diagnostic, "ADB server became unavailable." ),
            RetryPolicy::Backoff );
        reconnectAttempt_ = 0u;
        const auto runSerial = runSerial_;
        publishState();
        if ( running_ && runSerial == runSerial_
             && snapshot_.status == AdbServerSupervisorStatus::RetryWait ) {
            scheduleReconnect();
        }
    }

    void scheduleStartupRetry( RetryPolicy retryPolicy )
    {
        auto delay = std::chrono::milliseconds{ 0 };
        if ( retryPolicy == RetryPolicy::Backoff ) {
            const auto index
                = std::min( startupRetryAttempt_, config_.reconnectBackoff.size() - 1u );
            delay = config_.reconnectBackoff.at( index );
            if ( startupRetryAttempt_ < config_.reconnectBackoff.size() ) {
                ++startupRetryAttempt_;
            }
        }
        schedule( AdbServerScheduleKind::StartupRetry, delay,
                  []( Impl& self ) { self.beginProbe( ProbePurpose::Initial ); } );
    }

    void scheduleReconnect()
    {
        snapshot_.status = AdbServerSupervisorStatus::RetryWait;
        snapshot_.infrastructure.status = InfrastructureStatus::Unavailable;
        const auto index = std::min( reconnectAttempt_, config_.reconnectBackoff.size() - 1u );
        const auto delay = config_.reconnectBackoff.at( index );
        if ( reconnectAttempt_ < config_.reconnectBackoff.size() ) {
            ++reconnectAttempt_;
        }
        schedule( AdbServerScheduleKind::ReconnectBackoff, delay,
                  []( Impl& self ) { self.beginProbe( ProbePurpose::Reconnect ); } );
    }

    void fail( AdbServerSupervisorStatus status, std::string code, std::string message,
               std::string nativeDetail, RetryPolicy retryPolicy,
               ErrorCategory category = ErrorCategory::Infrastructure )
    {
        snapshot_.status = status;
        snapshot_.infrastructure
            = InfrastructureState{ InfrastructureStatus::Unavailable, std::nullopt };
        snapshot_.error = makeSupervisorError( category, std::move( code ), std::move( message ),
                                               std::move( nativeDetail ), retryPolicy );
        const auto runSerial = runSerial_;
        const auto generation = snapshot_.generation;
        const auto epoch = snapshot_.epoch;
        publishState();
        if ( runSerial == runSerial_ && snapshot_.generation == generation
             && snapshot_.epoch == epoch && snapshot_.error.has_value() ) {
            Q_EMIT supervisor_.errorOccurred( generation, epoch, *snapshot_.error );
        }
        if ( running_ && runSerial == runSerial_ && snapshot_.generation == generation
             && snapshot_.epoch == epoch && snapshot_.status == AdbServerSupervisorStatus::Failed
             && snapshot_.error.has_value()
             && ( snapshot_.error->retryPolicy == RetryPolicy::Immediate
                  || snapshot_.error->retryPolicy == RetryPolicy::Backoff ) ) {
            scheduleStartupRetry( snapshot_.error->retryPolicy );
        }
    }

    void failStartup( std::string code, std::string message, std::string nativeDetail,
                      bool cleanupIfPermitted, RetryPolicy retryPolicy = RetryPolicy::Backoff,
                      ErrorCategory category = ErrorCategory::Infrastructure )
    {
        cancelSchedule( AdbServerScheduleKind::StartupTimeout );
        cancelSchedule( AdbServerScheduleKind::ReadinessProbe );
        cancelProbe();
        if ( cleanupIfPermitted ) {
            cleanupPreReadyLaunch();
        }
        releaseStartupLock();
        fail( AdbServerSupervisorStatus::Failed, std::move( code ), std::move( message ),
              std::move( nativeDetail ), retryPolicy, category );
    }

    void publishState()
    {
        Q_EMIT supervisor_.stateChanged( snapshot_.generation, snapshot_.epoch, snapshot_ );
    }

    template <typename Callback>
    void schedule( AdbServerScheduleKind kind, std::chrono::milliseconds delay, Callback callback )
    {
        cancelSchedule( kind );
        const auto runSerial = runSerial_;
        const auto index = scheduleIndex( kind );
        const auto serial = ++scheduleSerials_.at( index );
        const std::weak_ptr<CallbackGate> weakGate = callbackGate_;
        const auto token = scheduler_.schedule(
            kind, delay, [ weakGate, runSerial, index, serial, callback = std::move( callback ) ] {
                const auto gate = weakGate.lock();
                if ( gate != nullptr && gate->owner != nullptr ) {
                    gate->owner->scheduledCallback( runSerial, index, serial, callback );
                }
            } );
        // schedule() may invoke synchronously and change these serials before
        // returning the cancellation token.
        // cppcheck-suppress knownConditionTrueFalse
        if ( running_ && runSerial == runSerial_ && serial == scheduleSerials_.at( index ) ) {
            scheduleTokens_.at( index ) = token;
        }
        else if ( token != 0u ) {
            scheduler_.cancel( token );
        }
    }

    template <typename Callback>
    void scheduledCallback( std::uint64_t runSerial, std::size_t index, std::uint64_t serial,
                            const Callback& callback )
    {
        if ( !running_ || runSerial != runSerial_ || serial != scheduleSerials_.at( index ) ) {
            return;
        }
        scheduleTokens_.at( index ) = 0;
        callback( *this );
    }

    void cancelProbe()
    {
        ++probeSerial_;
        if ( activeProbeToken_ != 0u ) {
            probe_.cancel( activeProbeToken_ );
            activeProbeToken_ = 0;
        }
    }

    void cancelLockRequest()
    {
        ++lockSerial_;
        if ( activeLockRequestToken_ != 0u ) {
            startupLock_.cancel( activeLockRequestToken_ );
            activeLockRequestToken_ = 0;
        }
    }

    void releaseStartupLock()
    {
        if ( heldLockToken_ != 0u ) {
            const auto token = heldLockToken_;
            heldLockToken_ = 0;
            startupLock_.release( token );
            return;
        }
        if ( pendingHeldLock_ != nullptr ) {
            pendingHeldLock_->releaseRequested = true;
        }
    }

    void releasePublishedLaunch()
    {
        if ( activeLaunchToken_ == 0u ) {
            return;
        }
        const auto token = activeLaunchToken_;
        activeLaunchToken_ = 0;
        launcher_.release( token );
    }

    void cleanupPreReadyLaunch()
    {
        if ( serverPublished_ ) {
            return;
        }
        if ( activeLaunchToken_ != 0u ) {
            ++launchSerial_;
            const auto token = activeLaunchToken_;
            activeLaunchToken_ = 0;
            launcher_.cleanup( token );
            return;
        }
        if ( activeLaunchInvocation_ != nullptr ) {
            // A launcher may report a terminal result before returning the
            // ownership token. Invalidate that callback now; the caller of
            // launch() performs cleanup as soon as the token is available.
            ++launchSerial_;
        }
    }

    void cancelSchedule( AdbServerScheduleKind kind )
    {
        const auto index = scheduleIndex( kind );
        ++scheduleSerials_.at( index );
        if ( scheduleTokens_.at( index ) != 0u ) {
            scheduler_.cancel( scheduleTokens_.at( index ) );
            scheduleTokens_.at( index ) = 0;
        }
    }

    void cancelAllSchedules()
    {
        for ( std::size_t index = 0; index < scheduleKindCount; ++index ) {
            ++scheduleSerials_.at( index );
            if ( scheduleTokens_.at( index ) != 0u ) {
                scheduler_.cancel( scheduleTokens_.at( index ) );
                scheduleTokens_.at( index ) = 0;
            }
        }
    }

private:
    // Injected dependencies are non-owning and must outlive the supervisor.
    // NOLINTBEGIN(cppcoreguidelines-avoid-const-or-ref-data-members)
    AdbServerSupervisor& supervisor_;
    AdbServerSupervisorConfig config_;
    AdbServerProbe& probe_;
    AdbServerLauncher& launcher_;
    AdbServerStartupLock& startupLock_;
    AdbKeyStore& keyStore_;
    AdbServerScheduler& scheduler_;
    // NOLINTEND(cppcoreguidelines-avoid-const-or-ref-data-members)
    std::shared_ptr<CallbackGate> callbackGate_;

    AdbServerSupervisorSnapshot snapshot_;
    bool running_{ false };
    bool serverPublished_{ false };
    bool launchStarted_{ false };
    bool launchCleanupPermitted_{ false };
    std::uint64_t runSerial_{ 0 };
    std::uint64_t probeSerial_{ 0 };
    std::uint64_t lockSerial_{ 0 };
    std::uint64_t launchSerial_{ 0 };
    AdbServerToken activeProbeToken_{ 0 };
    AdbServerToken activeLockRequestToken_{ 0 };
    AdbServerToken heldLockToken_{ 0 };
    std::shared_ptr<LockInvocation> pendingHeldLock_;
    AdbServerToken activeLaunchToken_{ 0 };
    std::shared_ptr<LaunchInvocation> activeLaunchInvocation_;
    std::array<AdbServerToken, scheduleKindCount> scheduleTokens_{};
    std::array<std::uint64_t, scheduleKindCount> scheduleSerials_{};
    std::size_t startupRetryAttempt_{ 0 };
    std::size_t reconnectAttempt_{ 0 };
    std::string lastStartupProbeDiagnostic_;
};

AdbServerSupervisor::AdbServerSupervisor( AdbServerSupervisorConfig config, AdbServerProbe& probe,
                                          AdbServerLauncher& launcher,
                                          AdbServerStartupLock& startupLock, AdbKeyStore& keyStore,
                                          AdbServerScheduler& scheduler, QObject* parent )
    : QObject( parent )
    , impl_( std::make_unique<Impl>( *this, std::move( config ), probe, launcher, startupLock,
                                     keyStore, scheduler ) )
{
}

AdbServerSupervisor::~AdbServerSupervisor() = default;

void AdbServerSupervisor::start( Generation generation )
{
    impl_->start( generation );
}

void AdbServerSupervisor::stop( Generation generation )
{
    impl_->stop( generation );
}

void AdbServerSupervisor::grantKeyGenerationConsent( Generation generation, bool granted )
{
    impl_->grantKeyGenerationConsent( generation, granted );
}

const AdbServerSupervisorSnapshot& AdbServerSupervisor::snapshot() const noexcept
{
    return impl_->snapshot();
}

} // namespace klogg::livecapture::adb
