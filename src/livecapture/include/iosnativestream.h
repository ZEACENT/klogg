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

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ioscatalogprovider.h"
#include "iosnativeapi.h"
#include "iosnativeerrors.h"
#include "livedataqueue.h"

namespace klogg::livecapture::ios {

enum class IosNativeServicePolicy : std::uint8_t {
    AutomaticByProductVersion,
    OsTrace,
    LegacySyslog
};

enum class IosLogOutputFormat : std::uint8_t { Default, Json };

struct IosLogOptions {
    std::string level;
    std::vector<std::string> categories;
    std::string subsystem;
    IosLogOutputFormat outputFormat{ IosLogOutputFormat::Default };
};

inline constexpr std::chrono::milliseconds DefaultIosNativeShutdownDeadline{ 750 };

struct IosNativeStreamConfig {
    IosEndpointKey endpoint;
    Generation generation{ 0 };
    bool ansiOutputEnabled{ false };
    LiveDataQueueLimits queueLimits{ std::size_t{ 4u } * 1024u * 1024u, 256u };
    std::size_t maximumSyslogRecordBytes{ std::size_t{ 1u } * 1024u * 1024u };
    IosNativeServicePolicy servicePolicy{ IosNativeServicePolicy::AutomaticByProductVersion };
    IosLogOptions logOptions;
    std::chrono::milliseconds cleanupDeadline{ DefaultIosNativeShutdownDeadline };
};

struct IosNativeStreamCallbacks {
    std::function<void( Generation )> ready;
    std::function<void( Generation )> bytesAvailable;
    std::function<void( Generation, const ClassifiedIosNativeError& )> failed;
    std::function<void( Generation )> stopped;
};

using IosNativeStreamTask = std::function<void()>;
using IosNativeStreamExecutor = std::function<void( IosNativeStreamTask )>;

class IosNativeStreamSession {
public:
    virtual ~IosNativeStreamSession() = default;
    virtual bool start() = 0;
    virtual void stop( Generation generation ) noexcept = 0;
    virtual void shutdown() noexcept = 0;
    virtual std::optional<LiveDataBatch> drain() = 0;
    virtual LiveDataStatistics statistics() const = 0;
};

class IosNativeStreamWorker final : public IosNativeStreamSession {
public:
    IosNativeStreamWorker( IosNativeApi api, IosNativeStreamExecutor executor,
                           IosNativeStreamConfig config, IosNativeStreamCallbacks callbacks );
    ~IosNativeStreamWorker() override;

    IosNativeStreamWorker( const IosNativeStreamWorker& ) = delete;
    IosNativeStreamWorker& operator=( const IosNativeStreamWorker& ) = delete;

    bool start() override;
    void stop( Generation generation ) noexcept override;
    void shutdown() noexcept override;
    std::optional<LiveDataBatch> drain() override;
    LiveDataStatistics statistics() const override;

private:
    struct State;
    std::shared_ptr<State> state_;
};

class IosNativeStreamWorkerFactory {
public:
    virtual ~IosNativeStreamWorkerFactory() = default;
    virtual std::unique_ptr<IosNativeStreamSession>
    create( const IosNativeStreamConfig& config, IosNativeStreamCallbacks callbacks ) const = 0;
};

class DefaultIosNativeStreamWorkerFactory final : public IosNativeStreamWorkerFactory {
public:
    explicit DefaultIosNativeStreamWorkerFactory( IosNativeApi api );

    std::unique_ptr<IosNativeStreamSession>
    create( const IosNativeStreamConfig& config,
            IosNativeStreamCallbacks callbacks ) const override;

private:
    IosNativeApi api_{};
};

} // namespace klogg::livecapture::ios
