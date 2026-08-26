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

#ifndef KLOGG_DEVICEDISCOVERY_H
#define KLOGG_DEVICEDISCOVERY_H

#include <QList>

#include <cstdint>
#include <optional>
#include <utility>

#include "livestate.h"

enum class DeviceAvailability : std::uint8_t { Available, Unavailable, Unknown };

struct DeviceAvailabilityResult {
    DeviceAvailability availability{ DeviceAvailability::Unknown };
    std::optional<klogg::livecapture::LiveSourceError> error;
};

template <typename DeviceInfo>
struct DeviceDiscoveryResult {
    klogg::livecapture::Generation generation{ 0 };
    QList<DeviceInfo> devices;
    std::optional<klogg::livecapture::LiveSourceError> error;
};

// Owns the source-neutral state of a refresh sequence. The async request
// boundary stamps provider snapshots with a generation; presentation layers
// apply only the snapshot for the most recently started refresh.
template <typename DeviceInfo>
class DeviceDiscoveryCoordinator {
public:
    using Generation = klogg::livecapture::Generation;
    using Result = DeviceDiscoveryResult<DeviceInfo>;

    Generation beginRefresh()
    {
        return ++currentGeneration_;
    }

    bool accept( Result result )
    {
        if ( result.generation != currentGeneration_ ) {
            return false;
        }

        currentDevices_ = std::move( result.devices );
        currentError_ = std::move( result.error );
        return true;
    }

    Generation currentGeneration() const
    {
        return currentGeneration_;
    }

    const QList<DeviceInfo>& currentDevices() const
    {
        return currentDevices_;
    }

    const std::optional<klogg::livecapture::LiveSourceError>& currentError() const
    {
        return currentError_;
    }

private:
    Generation currentGeneration_{ 0 };
    QList<DeviceInfo> currentDevices_;
    std::optional<klogg::livecapture::LiveSourceError> currentError_;
};

#endif // KLOGG_DEVICEDISCOVERY_H
