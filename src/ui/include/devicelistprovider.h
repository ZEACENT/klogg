/*
 * Copyright (C) 2026 ZEACENT and other contributors
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

#ifndef KLOGG_DEVICELISTPROVIDER_H
#define KLOGG_DEVICELISTPROVIDER_H

#include <QList>
#include <QObject>
#include <QString>

#include <QtConcurrent>

#include <algorithm>
#include <functional>
#include <utility>

// Base template for device list providers.
//
// Both ADB and iOS need to enumerate connected devices via a subprocess
// (adb devices -l / pymobiledevice3 usbmux list).  Both calls block for
// up to 8 seconds.  This template isolates the blocking call behind a
// uniform interface so that:
//   - Transport classes delegate to a provider instead of running the
//     subprocess directly.
//   - Callers can choose sync (modal dialogs) or async (background)
//     enumeration.
//   - The subprocess logic is independently testable.
//
// Derived classes must implement doListDevices() and provide a
// Q_OBJECT macro (MOC does not support template classes).
template <typename DeviceInfo>
class DeviceListProviderBase : public QObject {
  public:
    using AsyncListOperation = std::function<QList<DeviceInfo>()>;

    explicit DeviceListProviderBase( AsyncListOperation asyncListOperation,
                                     QObject* parent = nullptr )
        : QObject( parent )
        , asyncListOperation_( std::move( asyncListOperation ) )
    {
    }

    // Synchronous device enumeration.  Blocks the calling thread.
    // Returns an empty list on error; check *error for details.
    QList<DeviceInfo> listDevices( QString* error = nullptr ) const
    {
        return doListDevices( error );
    }

    // Asynchronous device enumeration. Runs on the global thread pool from an
    // immutable work plan captured when the provider is constructed. The plan
    // must own its inputs by value and must not capture this, another provider
    // pointer, or any QObject reference. Once this function returns, destroying
    // the provider cannot cancel or alter the submitted enumeration.
    QFuture<QList<DeviceInfo>> listDevicesAsync() const
    {
        const auto operation = asyncListOperation_;
        return QtConcurrent::run( [ operation ] { return operation(); } );
    }

    // Check whether a specific device (serial / UDID) is connected.
    // Returns true if the device is found, false if not found.
    // Returns true on subprocess error (optimistic fallback — let the
    // actual connection attempt handle the real error).
    bool isDeviceAvailable( const QString& deviceId ) const
    {
        QString error;
        const auto devices = doListDevices( &error );
        if ( !error.isEmpty() ) {
            return true; // optimistic fallback
        }
        return std::any_of( devices.cbegin(), devices.cend(), [ this, &deviceId ]( const auto& device ) {
            return deviceMatches( device, deviceId );
        } );
    }

  protected:
    // Subclasses implement the actual subprocess call.
    virtual QList<DeviceInfo> doListDevices( QString* error ) const = 0;

    // Subclasses implement device identifier matching.
    virtual bool deviceMatches( const DeviceInfo& device,
                                const QString& deviceId ) const = 0;

  private:
    AsyncListOperation asyncListOperation_;
};

#endif // KLOGG_DEVICELISTPROVIDER_H
