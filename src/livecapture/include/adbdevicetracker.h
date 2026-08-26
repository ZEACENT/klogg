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

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "adbserversupervisor.h"
#include "adbsmartsocketclient.h"
#include "livestate.h"

namespace klogg::livecapture::adb {

enum class AdbDeviceState : std::uint8_t { Online, Unauthorized, Offline, Other };

struct AdbDeviceInfo {
    std::string serial;
    AdbDeviceState state{ AdbDeviceState::Other };
    std::string stateText;
    std::string product;
    std::string model;
    std::string device;
    std::optional<std::uint64_t> transportId;
};

struct AdbTrackedDeviceSnapshot {
    Generation generation{ 0 };
    std::uint64_t infrastructureEpoch{ 0 };
    Generation requestGeneration{ 0 };
    std::vector<AdbDeviceInfo> devices;
    std::optional<LiveSourceError> error;
};

struct AdbDeviceTrackerConfig {
    std::vector<std::chrono::milliseconds> reconnectBackoff{ std::chrono::milliseconds{ 250 } };
};

// Maintains one long-lived host:track-devices-l subscription for a ready
// infrastructure epoch. Dependencies are non-owning and must outlive it.
class AdbDeviceTracker final : public QObject {
    Q_OBJECT

public:
    AdbDeviceTracker( AdbDeviceTrackerConfig config, AdbSmartSocketClient& client,
                      AdbServerScheduler& scheduler, QObject* parent = nullptr );
    ~AdbDeviceTracker() override;

    AdbDeviceTracker( const AdbDeviceTracker& ) = delete;
    AdbDeviceTracker& operator=( const AdbDeviceTracker& ) = delete;

    void start( Generation generation, std::uint64_t infrastructureEpoch );
    void stop();

    const AdbTrackedDeviceSnapshot& snapshot() const noexcept;

Q_SIGNALS:
    void snapshotChanged( const klogg::livecapture::adb::AdbTrackedDeviceSnapshot& snapshot );

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace klogg::livecapture::adb

Q_DECLARE_METATYPE( klogg::livecapture::adb::AdbTrackedDeviceSnapshot )
