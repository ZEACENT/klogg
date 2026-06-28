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

#ifndef KLOGG_IOSDEVICELISTPROVIDER_H
#define KLOGG_IOSDEVICELISTPROVIDER_H

#include "devicelistprovider.h"
#include "iosdeviceparser.h"

// Provides iOS device enumeration by running `pymobiledevice3 usbmux list`.
//
// Isolates the blocking subprocess call so transport and UI layers
// do not need to manage QProcess directly.  Use listDevices() for
// synchronous enumeration (modal dialogs) or listDevicesAsync() for
// background enumeration (non-blocking UI).
//
// Only functional on macOS; returns empty with an error on other platforms.
class IosDeviceListProvider : public DeviceListProviderBase<IosDeviceInfo> {
    Q_OBJECT

  public:
    explicit IosDeviceListProvider( QString executable, QObject* parent = nullptr );

    // Probe well-known pymobiledevice3 install locations and return the
    // absolute path of the first executable hit.
    static QString detectIosSyslogExecutable();

    // Normalize the pymobiledevice3 executable path.
    static QString normalizedExecutable( const QString& executable );

  protected:
    QList<IosDeviceInfo> doListDevices( QString* error ) const override;
    bool deviceMatches( const IosDeviceInfo& device,
                        const QString& deviceId ) const override;

  private:
    QString executable_;
};

#endif // KLOGG_IOSDEVICELISTPROVIDER_H
