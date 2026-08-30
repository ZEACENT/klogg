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

#include "ioscatalogprovider.h"
#include "iosnativeapi.h"

namespace klogg::livecapture::ios {

using IosCatalogTask = std::function<void()>;
// The execution boundary must defer work until after the caller returns. Native
// callbacks use it specifically to avoid invoking observers or unsubscribe while
// libusbmuxd holds its listener mutex. Tasks may run concurrently; the catalog
// serializes snapshot observer delivery.
using IosCatalogExecutor = std::function<void( IosCatalogTask )>;

class IosDeviceCatalog final : public IosCatalogSnapshotProvider,
                               public IosCatalogMetadataRequester {
public:
    IosDeviceCatalog( IosNativeApi api, IosCatalogExecutor executor );
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
