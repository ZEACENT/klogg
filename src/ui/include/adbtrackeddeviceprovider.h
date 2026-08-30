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

#ifndef KLOGG_ADBTRACKEDDEVICEPROVIDER_H
#define KLOGG_ADBTRACKEDDEVICEPROVIDER_H

#include <QObject>

#include "adbdeviceinfo.h"
#include "adbinfrastructuremanager.h"
#include "devicediscovery.h"

DeviceDiscoveryResult<AdbDeviceInfo> mapTrackedAdbInfrastructureSnapshot(
    klogg::livecapture::Generation refreshGeneration,
    const klogg::livecapture::adb::AdbInfrastructureSnapshot& snapshot );

class AdbTrackedDeviceProvider : public QObject {
    Q_OBJECT

public:
    explicit AdbTrackedDeviceProvider( QObject* parent = nullptr );
    ~AdbTrackedDeviceProvider() override;

    virtual klogg::livecapture::adb::AdbInfrastructureLease acquireLease() = 0;
    virtual DeviceDiscoveryResult<AdbDeviceInfo> currentSnapshot() const = 0;
    virtual void refresh() = 0;

Q_SIGNALS:
    void snapshotChanged( const DeviceDiscoveryResult<AdbDeviceInfo>& snapshot );
};

class ManagerAdbTrackedDeviceProvider final : public AdbTrackedDeviceProvider {
public:
    explicit ManagerAdbTrackedDeviceProvider(
        klogg::livecapture::adb::AdbInfrastructureManager& manager, QObject* parent = nullptr );

    klogg::livecapture::adb::AdbInfrastructureLease acquireLease() override;
    DeviceDiscoveryResult<AdbDeviceInfo> currentSnapshot() const override;
    void refresh() override;

private:
    void publish( const klogg::livecapture::adb::AdbInfrastructureSnapshot& snapshot );

    // The application composition root owns the manager for longer than this provider.
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members)
    klogg::livecapture::adb::AdbInfrastructureManager& manager_;
    DeviceDiscoveryResult<AdbDeviceInfo> snapshot_;
    klogg::livecapture::Generation nextSnapshotGeneration_{ 0 };
};

Q_DECLARE_METATYPE( DeviceDiscoveryResult<AdbDeviceInfo> )

#endif // KLOGG_ADBTRACKEDDEVICEPROVIDER_H
