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

#include <QObject>

#include <chrono>
#include <memory>
#include <optional>
#include <string>

#include "ioscatalogprovider.h"
#include "iosnativestream.h"
#include "livesourcetransport.h"

namespace klogg::livecapture::ios {

struct IosLiveServicesConfig {
    std::string nativeStackRoot;
    std::chrono::milliseconds catalogShutdownDeadline{ DefaultIosNativeShutdownDeadline };
};

class IosLiveServices final : public QObject, public LiveSourceTransportFactory {
    Q_OBJECT

public:
    explicit IosLiveServices( IosLiveServicesConfig config, QObject* parent = nullptr );
    IosLiveServices( std::unique_ptr<IosCatalogSnapshotProvider> catalog,
                     std::unique_ptr<IosNativeStreamWorkerFactory> workerFactory,
                     QObject* parent = nullptr );
    ~IosLiveServices() override;

    IosLiveServices( const IosLiveServices& ) = delete;
    IosLiveServices& operator=( const IosLiveServices& ) = delete;

    IosCatalogSnapshotProvider& catalogProvider() noexcept;
    const IosCatalogSnapshotProvider& catalogProvider() const noexcept;

    std::unique_ptr<LiveSourceTransport>
    create( const LiveSourceTransportConfig& config ) const override;

    std::optional<LiveSourceError> lastConfigurationError() const;
    void shutdown();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace klogg::livecapture::ios
