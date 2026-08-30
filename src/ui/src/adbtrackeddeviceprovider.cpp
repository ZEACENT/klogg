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

#include "adbtrackeddeviceprovider.h"

#include <QString>

#include <utility>

namespace {

AdbDeviceState mapState( klogg::livecapture::adb::AdbDeviceState state )
{
    using DomainState = klogg::livecapture::adb::AdbDeviceState;
    switch ( state ) {
    case DomainState::Online:
        return AdbDeviceState::Online;
    case DomainState::Unauthorized:
        return AdbDeviceState::Unauthorized;
    case DomainState::Offline:
        return AdbDeviceState::Offline;
    case DomainState::Other:
        return AdbDeviceState::Other;
    }
    return AdbDeviceState::Other;
}

QString displayDescription( const klogg::livecapture::adb::AdbDeviceInfo& device )
{
    if ( !device.model.empty() ) {
        return QString::fromStdString( device.model );
    }
    if ( !device.device.empty() ) {
        return QString::fromStdString( device.device );
    }
    if ( !device.product.empty() ) {
        return QString::fromStdString( device.product );
    }
    return QString::fromStdString( device.serial );
}

std::optional<klogg::livecapture::LiveSourceError>
presentationError( const std::optional<klogg::livecapture::LiveSourceError>& error )
{
    if ( !error.has_value() ) {
        return std::nullopt;
    }

    auto result = *error;
    if ( result.scope == klogg::livecapture::ErrorScope::Infrastructure
         && ( result.retryPolicy == klogg::livecapture::RetryPolicy::Immediate
              || result.retryPolicy == klogg::livecapture::RetryPolicy::Backoff ) ) {
        result.retryPolicy = klogg::livecapture::RetryPolicy::WaitForInfrastructure;
    }
    return result;
}

} // namespace

DeviceDiscoveryResult<AdbDeviceInfo> mapTrackedAdbInfrastructureSnapshot(
    klogg::livecapture::Generation refreshGeneration,
    const klogg::livecapture::adb::AdbInfrastructureSnapshot& snapshot )
{
    QList<AdbDeviceInfo> devices;
    if ( snapshot.hasCurrentDevices() ) {
        devices.reserve(
            static_cast<decltype( devices.size() )>( snapshot.devices.devices.size() ) );
        for ( const auto& device : snapshot.devices.devices ) {
            const auto serial = QString::fromStdString( device.serial );
            const auto stateText = QString::fromStdString( device.stateText );
            const auto description = displayDescription( device );
            const auto displayName = device.state == klogg::livecapture::adb::AdbDeviceState::Online
                                         ? QStringLiteral( "%1 (%2)" ).arg( description, serial )
                                         : QStringLiteral( "%1 [%2]" ).arg( serial, stateText );
            devices.push_back( AdbDeviceInfo{ serial, displayName, description,
                                              mapState( device.state ), stateText } );
        }
    }
    return { refreshGeneration, std::move( devices ), presentationError( snapshot.error ) };
}

AdbTrackedDeviceProvider::AdbTrackedDeviceProvider( QObject* parent )
    : QObject( parent )
{
    qRegisterMetaType<DeviceDiscoveryResult<AdbDeviceInfo>>(
        "DeviceDiscoveryResult<AdbDeviceInfo>" );
}

AdbTrackedDeviceProvider::~AdbTrackedDeviceProvider() = default;

ManagerAdbTrackedDeviceProvider::ManagerAdbTrackedDeviceProvider(
    klogg::livecapture::adb::AdbInfrastructureManager& manager, QObject* parent )
    : AdbTrackedDeviceProvider( parent )
    , manager_( manager )
    , snapshot_( mapTrackedAdbInfrastructureSnapshot( 0u, manager.snapshot() ) )
{
    connect( &manager_, &klogg::livecapture::adb::AdbInfrastructureManager::snapshotChanged, this,
             [ this ]( const klogg::livecapture::adb::AdbInfrastructureSnapshot& snapshot ) {
                 publish( snapshot );
             } );
}

klogg::livecapture::adb::AdbInfrastructureLease ManagerAdbTrackedDeviceProvider::acquireLease()
{
    return manager_.acquireLease();
}

DeviceDiscoveryResult<AdbDeviceInfo> ManagerAdbTrackedDeviceProvider::currentSnapshot() const
{
    return snapshot_;
}

void ManagerAdbTrackedDeviceProvider::refresh()
{
    publish( manager_.snapshot() );
}

void ManagerAdbTrackedDeviceProvider::publish(
    const klogg::livecapture::adb::AdbInfrastructureSnapshot& snapshot )
{
    snapshot_ = mapTrackedAdbInfrastructureSnapshot( ++nextSnapshotGeneration_, snapshot );
    Q_EMIT snapshotChanged( snapshot_ );
}
