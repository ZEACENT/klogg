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

#ifndef KLOGG_ADBDEVICELISTPROVIDER_H
#define KLOGG_ADBDEVICELISTPROVIDER_H

#include "adbdeviceinfo.h"
#include "devicelistprovider.h"

// Provides ADB device enumeration by running `adb devices -l`.
//
// Isolates the blocking subprocess call so transport and UI layers
// do not need to manage QProcess directly.  Use listDevices() for
// synchronous enumeration (modal dialogs) or listDevicesAsync() for
// background enumeration (non-blocking UI).
class AdbDeviceListProvider : public DeviceListProviderBase<AdbDeviceInfo> {
    Q_OBJECT

  public:
    explicit AdbDeviceListProvider( QString adbExecutable,
                                    QObject* parent = nullptr );

    // Resolve the adb executable path through well-known locations.
    // Exposed for "Detect adb" UI affordances.
    static QString detectAdbExecutable();

    // Normalize an adb executable path (expand tilde, resolve known locations).
    static QString normalizedExecutable( const QString& adbExecutable );

  protected:
    QList<AdbDeviceInfo> doListDevices( QString* error ) const override;
    bool deviceMatches( const AdbDeviceInfo& device,
                        const QString& deviceId ) const override;

  private:
    QString adbExecutable_;
};

#endif // KLOGG_ADBDEVICELISTPROVIDER_H
