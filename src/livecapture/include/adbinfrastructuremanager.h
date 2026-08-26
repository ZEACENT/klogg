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

#pragma once

#include <QMetaType>
#include <QObject>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

#include "adbdevicetracker.h"
#include "adbserversupervisor.h"
#include "adbsmartsocketclient.h"
#include "livestate.h"

namespace klogg::livecapture::adb {

struct AdbInfrastructureManagerConfig {
    AdbServerSupervisorConfig server;
    AdbDeviceTrackerConfig tracker;
};

struct AdbInfrastructureManagerDependencies {
    // The application composition root owns every service for longer than the manager.
    // NOLINTBEGIN(cppcoreguidelines-avoid-const-or-ref-data-members)
    AdbServerProbe& probe;
    AdbServerLauncher& launcher;
    AdbServerStartupLock& startupLock;
    AdbKeyStore& keyStore;
    AdbServerScheduler& supervisorScheduler;
    AdbSmartSocketClient& trackerClient;
    AdbServerScheduler& trackerScheduler;
    // NOLINTEND(cppcoreguidelines-avoid-const-or-ref-data-members)
};

struct AdbInfrastructureSnapshot {
    Generation generation{ 0 };
    std::uint64_t infrastructureEpoch{ 0 };
    InfrastructureState infrastructure;
    AdbTrackedDeviceSnapshot devices;
    std::optional<LiveSourceError> error;
};

class AdbInfrastructureManager;

class AdbInfrastructureLease final {
public:
    AdbInfrastructureLease() = default;
    ~AdbInfrastructureLease() = default;

    AdbInfrastructureLease( const AdbInfrastructureLease& ) = delete;
    AdbInfrastructureLease& operator=( const AdbInfrastructureLease& ) = delete;
    AdbInfrastructureLease( AdbInfrastructureLease&& ) noexcept = default;
    AdbInfrastructureLease& operator=( AdbInfrastructureLease&& ) noexcept = default;

    void reset() noexcept;
    explicit operator bool() const noexcept;

private:
    friend class AdbInfrastructureManager;
    explicit AdbInfrastructureLease( std::shared_ptr<void> token );

    std::shared_ptr<void> token_;
};

// Application-owned composition root for one supervisor and one device tracker.
// Consumer leases share observation and never own or terminate shared ADB state.
class AdbInfrastructureManager final : public QObject {
    Q_OBJECT

public:
    AdbInfrastructureManager( AdbInfrastructureManagerConfig config,
                              AdbInfrastructureManagerDependencies dependencies,
                              QObject* parent = nullptr );
    ~AdbInfrastructureManager() override;

    AdbInfrastructureManager( const AdbInfrastructureManager& ) = delete;
    AdbInfrastructureManager& operator=( const AdbInfrastructureManager& ) = delete;

    AdbInfrastructureLease acquireLease();
    std::size_t activeLeaseCount() const noexcept;
    const AdbInfrastructureSnapshot& snapshot() const noexcept;
    std::optional<AdbDeviceInfo> defaultOnlineDevice() const;

    void grantKeyGenerationConsent( bool granted );
    void shutdown();

Q_SIGNALS:
    void snapshotChanged( const klogg::livecapture::adb::AdbInfrastructureSnapshot& snapshot );
    void keyConsentRequired( klogg::livecapture::Generation generation, std::uint64_t epoch );

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace klogg::livecapture::adb

Q_DECLARE_METATYPE( klogg::livecapture::adb::AdbInfrastructureSnapshot )
