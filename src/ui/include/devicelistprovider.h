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
#include <QPointer>
#include <QString>

#include <QtConcurrent>

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
    explicit DeviceListProviderBase( QObject* parent = nullptr )
        : QObject( parent )
    {
    }

    // Synchronous device enumeration.  Blocks the calling thread.
    // Returns an empty list on error; check *error for details.
    QList<DeviceInfo> listDevices( QString* error = nullptr ) const
    {
        return doListDevices( error );
    }

    // Asynchronous device enumeration.  Runs on the global thread pool.
    // The returned QFuture resolves to the device list.
    //
    // The provider is guarded by a QPointer: if the provider (or its owning
    // transport) is destroyed before the pool runs the task, the lambda returns
    // an empty list instead of dereferencing freed memory. (Full mid-execution
    // safety would require shared_ptr ownership of the provider; callers that
    // need that guarantee should hold a shared_ptr themselves.)
    QFuture<QList<DeviceInfo>> listDevicesAsync() const
    {
        const QPointer<DeviceListProviderBase> self(
            const_cast<DeviceListProviderBase*>( this ) );
        return QtConcurrent::run(
            [self]() { return self ? self->doListDevices( nullptr ) : QList<DeviceInfo>{}; } );
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
        for ( const auto& device : devices ) {
            if ( deviceMatches( device, deviceId ) ) {
                return true;
            }
        }
        return false;
    }

  protected:
    // Subclasses implement the actual subprocess call.
    virtual QList<DeviceInfo> doListDevices( QString* error ) const = 0;

    // Subclasses implement device identifier matching.
    virtual bool deviceMatches( const DeviceInfo& device,
                                const QString& deviceId ) const = 0;
};

#endif // KLOGG_DEVICELISTPROVIDER_H
