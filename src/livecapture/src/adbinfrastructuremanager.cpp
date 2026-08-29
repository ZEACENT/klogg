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

#include "adbinfrastructuremanager.h"

#include <algorithm>
#include <utility>

namespace klogg::livecapture::adb {
namespace {

LiveSourceError infrastructureUnavailableError( const std::optional<LiveSourceError>& error )
{
    if ( error.has_value() ) {
        auto result = *error;
        if ( result.retryPolicy == RetryPolicy::Immediate
             || result.retryPolicy == RetryPolicy::Backoff ) {
            result.category = ErrorCategory::Infrastructure;
            result.scope = ErrorScope::Infrastructure;
            result.retryPolicy = RetryPolicy::WaitForInfrastructure;
            if ( result.code.empty() ) {
                result.code = "adb-infrastructure-unavailable";
            }
        }
        return result;
    }

    return LiveSourceError{ ErrorCategory::Infrastructure,
                            "adb-infrastructure-unavailable",
                            ErrorScope::Infrastructure,
                            RetryPolicy::WaitForInfrastructure,
                            "The shared ADB infrastructure is unavailable.",
                            "The shared ADB infrastructure is unavailable." };
}

bool equalInfrastructure( const InfrastructureState& lhs, const InfrastructureState& rhs )
{
    return lhs.status == rhs.status && lhs.ownership == rhs.ownership;
}

bool equalError( const LiveSourceError& lhs, const LiveSourceError& rhs )
{
    return lhs.category == rhs.category && lhs.code == rhs.code && lhs.scope == rhs.scope
           && lhs.retryPolicy == rhs.retryPolicy && lhs.message == rhs.message
           && lhs.nativeDetail == rhs.nativeDetail;
}

bool equalError( const std::optional<LiveSourceError>& lhs,
                 const std::optional<LiveSourceError>& rhs )
{
    if ( lhs.has_value() != rhs.has_value() ) {
        return false;
    }
    return !lhs.has_value() || equalError( *lhs, *rhs );
}

void registerSignalMetaTypes()
{
    qRegisterMetaType<AdbInfrastructureSnapshot>(
        "klogg::livecapture::adb::AdbInfrastructureSnapshot" );
}

} // namespace

class AdbInfrastructureManager::Impl final {
public:
    Impl( AdbInfrastructureManager& manager, AdbInfrastructureManagerConfig config,
          AdbInfrastructureManagerDependencies dependencies )
        : manager_( manager )
        , supervisor_( std::move( config.server ), dependencies.probe, dependencies.launcher,
                       dependencies.startupLock, dependencies.keyStore,
                       dependencies.supervisorScheduler )
        , tracker_( std::move( config.tracker ), dependencies.trackerClient,
                    dependencies.trackerScheduler )
        , callbackGate_( std::make_shared<CallbackGate>() )
    {
        callbackGate_->owner = this;
        QObject::connect( &supervisor_, &AdbServerSupervisor::stateChanged, &manager_,
                          [ this ]( Generation generation, std::uint64_t epoch,
                                    const AdbServerSupervisorSnapshot& state ) {
                              supervisorChanged( generation, epoch, state );
                          } );
        QObject::connect( &supervisor_, &AdbServerSupervisor::consentRequired, &manager_,
                          [ this ]( Generation generation, std::uint64_t epoch ) {
                              if ( !shuttingDown_ && generation == snapshot_.generation
                                   && epoch == snapshot_.infrastructureEpoch ) {
                                  Q_EMIT manager_.keyConsentRequired( generation, epoch );
                              }
                          } );
        QObject::connect(
            &tracker_, &AdbDeviceTracker::snapshotChanged, &manager_,
            [ this ]( const AdbTrackedDeviceSnapshot& devices ) { trackerChanged( devices ); } );
    }

    ~Impl()
    {
        callbackGate_->owner = nullptr;
        tracker_.stop();
    }

    AdbInfrastructureLease acquireLease()
    {
        if ( shuttingDown_ ) {
            return {};
        }

        const auto wasIdle = activeLeaseCount_ == 0u;
        ++activeLeaseCount_;
        if ( !started_ || ( wasIdle && retryableTerminalFailure() ) ) {
            started_ = true;
            snapshot_.generation = ++nextGeneration_;
            supervisor_.start( snapshot_.generation );
        }

        struct LeaseToken {
            std::weak_ptr<CallbackGate> gate;

            ~LeaseToken()
            {
                const auto locked = gate.lock();
                if ( locked != nullptr && locked->owner != nullptr ) {
                    locked->owner->releaseLease();
                }
            }
        };

        auto token = std::make_shared<LeaseToken>();
        token->gate = callbackGate_;
        return AdbInfrastructureLease{ std::static_pointer_cast<void>( token ) };
    }

    std::size_t activeLeaseCount() const noexcept
    {
        return activeLeaseCount_;
    }

    const AdbInfrastructureSnapshot& snapshot() const noexcept
    {
        return snapshot_;
    }

    std::optional<AdbDeviceInfo> defaultOnlineDevice() const
    {
        if ( !snapshot_.hasCurrentDevices() ) {
            return std::nullopt;
        }

        const auto found = std::find_if(
            snapshot_.devices.devices.begin(), snapshot_.devices.devices.end(),
            []( const auto& device ) { return device.state == AdbDeviceState::Online; } );
        if ( found == snapshot_.devices.devices.end() ) {
            return std::nullopt;
        }
        return *found;
    }

    void grantKeyGenerationConsent( bool granted )
    {
        if ( !shuttingDown_ && snapshot_.generation != 0u ) {
            supervisor_.grantKeyGenerationConsent( snapshot_.generation, granted );
        }
    }

    void shutdown()
    {
        if ( shuttingDown_ ) {
            return;
        }

        shuttingDown_ = true;
        tracker_.stop();
        if ( snapshot_.generation != 0u ) {
            supervisor_.stop( snapshot_.generation );
        }
        activeLeaseCount_ = 0u;
        started_ = false;
        snapshot_.infrastructure
            = InfrastructureState{ InfrastructureStatus::Unavailable, std::nullopt };
        snapshot_.error = LiveSourceError{
            ErrorCategory::Cancelled,           "adb-infrastructure-shutdown",
            ErrorScope::Infrastructure,         RetryPolicy::Never,
            "ADB infrastructure is shut down.", "ADB infrastructure is shut down."
        };
        Q_EMIT manager_.snapshotChanged( snapshot_ );
    }

private:
    struct CallbackGate {
        Impl* owner{ nullptr };
    };

    bool retryableTerminalFailure() const
    {
        const auto& state = supervisor_.snapshot();
        if ( state.status != AdbServerSupervisorStatus::Failed || !state.error.has_value() ) {
            return false;
        }
        return state.error->retryPolicy == RetryPolicy::Immediate
               || state.error->retryPolicy == RetryPolicy::Backoff;
    }

    void releaseLease()
    {
        if ( activeLeaseCount_ > 0u ) {
            --activeLeaseCount_;
        }
    }

    void supervisorChanged( Generation generation, std::uint64_t epoch,
                            const AdbServerSupervisorSnapshot& state )
    {
        if ( shuttingDown_ || generation == 0u || generation != snapshot_.generation ) {
            return;
        }

        std::optional<LiveSourceError> projectedError;
        if ( state.infrastructure.status == InfrastructureStatus::Unavailable ) {
            projectedError = infrastructureUnavailableError( state.error );
        }
        else if ( state.infrastructure.status == InfrastructureStatus::Ready ) {
            if ( snapshot_.devices.generation == generation
                 && snapshot_.devices.infrastructureEpoch == epoch ) {
                projectedError = snapshot_.devices.error;
            }
        }
        else {
            projectedError = state.error;
        }

        const auto changed
            = snapshot_.infrastructureEpoch != epoch
              || !equalInfrastructure( snapshot_.infrastructure, state.infrastructure )
              || !equalError( snapshot_.error, projectedError );
        snapshot_.infrastructureEpoch = epoch;
        snapshot_.infrastructure = state.infrastructure;
        snapshot_.error = std::move( projectedError );

        if ( state.infrastructure.status == InfrastructureStatus::Ready ) {
            tracker_.start( generation, epoch );
        }
        else {
            tracker_.stop();
        }
        if ( changed ) {
            Q_EMIT manager_.snapshotChanged( snapshot_ );
        }
    }

    void trackerChanged( const AdbTrackedDeviceSnapshot& devices )
    {
        if ( shuttingDown_ || devices.generation != snapshot_.generation
             || devices.infrastructureEpoch != snapshot_.infrastructureEpoch
             || snapshot_.infrastructure.status != InfrastructureStatus::Ready ) {
            return;
        }

        snapshot_.devices = devices;
        snapshot_.error = devices.error;
        Q_EMIT manager_.snapshotChanged( snapshot_ );
    }

private:
    AdbInfrastructureManager& manager_;
    AdbServerSupervisor supervisor_;
    AdbDeviceTracker tracker_;
    std::shared_ptr<CallbackGate> callbackGate_;

    AdbInfrastructureSnapshot snapshot_;
    Generation nextGeneration_{ 0 };
    std::size_t activeLeaseCount_{ 0 };
    bool started_{ false };
    bool shuttingDown_{ false };
};

AdbInfrastructureLease::AdbInfrastructureLease( std::shared_ptr<void> token )
    : token_( std::move( token ) )
{
}

void AdbInfrastructureLease::reset() noexcept
{
    token_.reset();
}

AdbInfrastructureLease::operator bool() const noexcept
{
    return token_ != nullptr;
}

AdbInfrastructureManager::AdbInfrastructureManager(
    AdbInfrastructureManagerConfig config, AdbInfrastructureManagerDependencies dependencies,
    QObject* parent )
    : QObject( parent )
    , impl_( std::make_unique<Impl>( *this, std::move( config ), dependencies ) )
{
    registerSignalMetaTypes();
}

AdbInfrastructureManager::~AdbInfrastructureManager() = default;

AdbInfrastructureLease AdbInfrastructureManager::acquireLease()
{
    return impl_->acquireLease();
}

std::size_t AdbInfrastructureManager::activeLeaseCount() const noexcept
{
    return impl_->activeLeaseCount();
}

const AdbInfrastructureSnapshot& AdbInfrastructureManager::snapshot() const noexcept
{
    return impl_->snapshot();
}

std::optional<AdbDeviceInfo> AdbInfrastructureManager::defaultOnlineDevice() const
{
    return impl_->defaultOnlineDevice();
}

void AdbInfrastructureManager::grantKeyGenerationConsent( bool granted )
{
    impl_->grantKeyGenerationConsent( granted );
}

void AdbInfrastructureManager::shutdown()
{
    impl_->shutdown();
}

} // namespace klogg::livecapture::adb
