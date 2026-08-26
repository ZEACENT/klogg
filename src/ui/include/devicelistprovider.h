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

#ifndef KLOGG_DEVICELISTPROVIDER_H
#define KLOGG_DEVICELISTPROVIDER_H

#include <QList>
#include <QObject>
#include <QString>

#include <QtConcurrent>

#include <algorithm>
#include <functional>
#include <type_traits>
#include <utility>

#include "devicediscovery.h"

template <typename DeviceInfo>
using DeviceDiscoveryOperation
    = std::function<DeviceDiscoveryResult<DeviceInfo>( klogg::livecapture::Generation )>;

// The refresh request, not provider work, owns generation metadata. Keeping the
// stamp at this async boundary prevents a provider bug from making current work
// look stale or stale work look current.
template <typename DeviceInfo>
QFuture<DeviceDiscoveryResult<DeviceInfo>>
runDeviceDiscoveryAsync( DeviceDiscoveryOperation<DeviceInfo> operation,
                         klogg::livecapture::Generation generation )
{
    return QtConcurrent::run( [ operation = std::move( operation ), generation ]() mutable {
        auto result = operation( generation );
        result.generation = generation;
        return result;
    } );
}

// Base template for device list providers. Blocking subprocess work is captured
// as an immutable value-owned plan before it is submitted to the thread pool.
template <typename DeviceInfo>
class DeviceListProviderBase : public QObject {
public:
    using Generation = klogg::livecapture::Generation;
    using DiscoveryResult = DeviceDiscoveryResult<DeviceInfo>;
    using AsyncListOperation = DeviceDiscoveryOperation<DeviceInfo>;

    template <typename Operation>
    explicit DeviceListProviderBase( Operation operation, QObject* parent = nullptr )
        : QObject( parent )
        , asyncListOperation_( makeAsyncOperation( std::move( operation ) ) )
    {
    }

    QList<DeviceInfo> listDevices( QString* error = nullptr ) const
    {
        return doListDevices( error );
    }

    QFuture<DiscoveryResult> listDevicesAsync( Generation generation ) const
    {
        return runDeviceDiscoveryAsync<DeviceInfo>( asyncListOperation_, generation );
    }

    // Compatibility path for callers that do not need generation/error metadata.
    QFuture<QList<DeviceInfo>> listDevicesAsync() const
    {
        const auto operation = asyncListOperation_;
        return QtConcurrent::run( [ operation ] { return operation( Generation{ 0 } ).devices; } );
    }

    DeviceAvailabilityResult deviceAvailability( const QString& deviceId ) const
    {
        auto result = asyncListOperation_( Generation{ 0 } );
        if ( result.error ) {
            return { DeviceAvailability::Unknown, std::move( result.error ) };
        }

        const auto available = std::any_of( result.devices.cbegin(), result.devices.cend(),
                                            [ this, &deviceId ]( const auto& device ) {
                                                return deviceMatches( device, deviceId );
                                            } );
        return { available ? DeviceAvailability::Available : DeviceAvailability::Unavailable,
                 std::nullopt };
    }

protected:
    virtual QList<DeviceInfo> doListDevices( QString* error ) const = 0;
    virtual bool deviceMatches( const DeviceInfo& device, const QString& deviceId ) const = 0;

private:
    template <typename Operation>
    static AsyncListOperation makeAsyncOperation( Operation operation )
    {
        constexpr auto ReturnsDiscoveryResult
            = std::is_invocable_r_v<DiscoveryResult, Operation&, Generation>;
        constexpr auto ReturnsDeviceList = std::is_invocable_r_v<QList<DeviceInfo>, Operation&>;
        static_assert( ReturnsDiscoveryResult || ReturnsDeviceList,
                       "device discovery operation has an unsupported signature" );

        return [ operation = std::move( operation ) ]( Generation generation ) mutable {
            if constexpr ( ReturnsDiscoveryResult ) {
                return operation( generation );
            }
            else {
                return DiscoveryResult{ generation, operation(), std::nullopt };
            }
        };
    }

    AsyncListOperation asyncListOperation_;
};

#endif // KLOGG_DEVICELISTPROVIDER_H
