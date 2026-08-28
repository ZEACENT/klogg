/*
 * Copyright (C) 2026
 *
 * This file is part of klogg.
 *
 * klogg is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "iosnativeapi.h"
#include "iosnativeerrors.h"
#include "livestate.h"

namespace klogg::livecapture::ios {

struct IosEndpointKey {
    std::string udid;
    NativeConnectionType connectionType{ NativeConnectionType::Usb };
};

inline bool operator==( const IosEndpointKey& left, const IosEndpointKey& right ) noexcept
{
    return left.udid == right.udid && left.connectionType == right.connectionType;
}

struct IosDeviceMetadata {
    std::string displayName;
    std::string productType;
    std::string productVersion;
};

using IosCatalogError = ClassifiedIosNativeError;

struct IosCatalogEntry {
    IosEndpointKey endpoint;
    Generation epoch{ 0 };
    std::optional<IosDeviceMetadata> metadata;
    std::optional<IosCatalogError> error;
};

struct IosCatalogSnapshot {
    Generation generation{ 0 };
    std::vector<IosCatalogEntry> entries;
};

class IosCatalogSnapshotProvider {
public:
    using SubscriptionId = std::uint64_t;
    using SnapshotCallback = std::function<void( const IosCatalogSnapshot& )>;

    virtual ~IosCatalogSnapshotProvider() = default;
    virtual IosCatalogSnapshot snapshot() const = 0;
    virtual SubscriptionId subscribe( SnapshotCallback callback ) = 0;
    virtual void unsubscribe( SubscriptionId subscription ) = 0;
    virtual std::optional<LiveSourceError> startupError() const
    {
        return std::nullopt;
    }
};

class IosCatalogMetadataRequester {
public:
    virtual ~IosCatalogMetadataRequester() = default;
    virtual void requestMetadata( IosEndpointKey endpoint ) = 0;
};

} // namespace klogg::livecapture::ios
