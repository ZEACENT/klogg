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

#include <memory>
#include <optional>
#include <vector>

#include "iosnativeerrors.h"
#include "iosnativestream.h"
#include "livesourcetransport.h"

namespace klogg::livecapture::ios {

class IosNativeTransport final : public LiveSourceTransport {
    Q_OBJECT

public:
    IosNativeTransport( const IosNativeStreamWorkerFactory& workerFactory,
                        IosNativeStreamConfig config, QObject* parent = nullptr );
    ~IosNativeTransport() override;

    IosNativeTransport( const IosNativeTransport& ) = delete;
    IosNativeTransport& operator=( const IosNativeTransport& ) = delete;

    void start( Generation generation ) override;
    void stop( Generation generation ) override;
    void clearRemoteAsync( Generation generation, ClearRequestId requestId ) override;
    QString lastError() const override;
    LiveDataStatistics statistics() const override;

    std::optional<LiveSourceError> lastStructuredError() const override;
    void serviceShutdown();

private:
    struct CallbackGate;
    struct RetiredSession {
        Generation generation{ 0 };
        std::unique_ptr<IosNativeStreamSession> session;
    };

    void postReady( Generation generation );
    void postBytesAvailable( Generation generation );
    void postFailure( Generation generation, ClassifiedIosNativeError error );
    void postStopped( Generation generation );
    void drainCurrent( Generation generation );
    void publishState( Generation generation, State state );
    void retireCurrent( bool requestStop );

    const IosNativeStreamWorkerFactory& workerFactory_;
    IosNativeStreamConfig baseConfig_;
    std::shared_ptr<CallbackGate> callbackGate_;
    std::unique_ptr<IosNativeStreamSession> session_;
    std::vector<RetiredSession> retiredSessions_;
    std::optional<Generation> activeGeneration_;
    std::optional<Generation> stateGeneration_;
    State state_{ State::Disconnected };
    QString lastError_;
    std::optional<LiveSourceError> lastStructuredError_;
    bool shuttingDown_{ false };
};

} // namespace klogg::livecapture::ios
