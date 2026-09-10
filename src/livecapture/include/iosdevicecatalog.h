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

#include <functional>
#include <memory>
#include <string>

#include "ioscatalogprovider.h"
#include "iosnativeapi.h"

namespace klogg::livecapture::ios {

using IosCatalogTask = std::function<void()>;
// Execution boundaries must defer work until after the caller returns. Native
// callbacks use the publication executor specifically to avoid invoking observers
// or unsubscribe while libusbmuxd holds its listener mutex. Metadata execution is
// a separate bounded dependency so a blocking RPC cannot delay publication.
using IosCatalogExecutor = std::function<void( IosCatalogTask )>;

// Metadata is owned by endpoint identity rather than an unkeyed FIFO. The
// executor retains at most one pending replacement per key, while cancellation
// leaves an already-running request to its catalog generation/epoch stale gate.
struct IosCatalogMetadataExecutor {
    std::function<bool( std::string, IosCatalogTask )> submitLatest;
    std::function<bool( const std::string& )> cancelLatest;
    std::function<void()> clearPendingLatest;
};

class IosDeviceCatalog final : public IosCatalogSnapshotProvider,
                               public IosCatalogMetadataRequester {
public:
    IosDeviceCatalog( const IosNativeApi& api, IosCatalogExecutor executor );
    IosDeviceCatalog( const IosNativeApi& api, IosCatalogExecutor publicationExecutor,
                      IosCatalogExecutor metadataExecutor );
    IosDeviceCatalog( const IosNativeApi& api, IosCatalogExecutor publicationExecutor,
                      IosCatalogMetadataExecutor metadataExecutor );
    ~IosDeviceCatalog() override;

    IosDeviceCatalog( const IosDeviceCatalog& ) = delete;
    IosDeviceCatalog& operator=( const IosDeviceCatalog& ) = delete;

    bool start();
    void stop();
    void requestMetadata( IosEndpointKey endpoint ) override;

    IosCatalogSnapshot snapshot() const override;
    SubscriptionId subscribe( SnapshotCallback callback ) override;
    void unsubscribe( SubscriptionId subscription ) override;
    std::optional<LiveSourceError> startupError() const override;

private:
    struct State;

    static void nativeEventCallback( const NativeDeviceEvent* event, void* context );

    std::shared_ptr<State> state_;
};

} // namespace klogg::livecapture::ios
