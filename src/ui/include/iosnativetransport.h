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

#include <functional>
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
    using QueuedTask = std::function<void()>;
    // Dispatchers must enqueue and return; inline execution can deadlock the
    // callback gate. The bool reports whether ownership of the task was accepted.
    using QueuedDispatcher = std::function<bool( QObject&, QueuedTask )>;

    IosNativeTransport( const IosNativeStreamWorkerFactory& workerFactory,
                        IosNativeStreamConfig config, QObject* parent = nullptr );
    IosNativeTransport( const IosNativeStreamWorkerFactory& workerFactory,
                        IosNativeStreamConfig config, QueuedDispatcher dispatcher,
                        QObject* parent = nullptr );
    ~IosNativeTransport() override;

    IosNativeTransport( const IosNativeTransport& ) = delete;
    IosNativeTransport& operator=( const IosNativeTransport& ) = delete;

    void start( Generation generation ) override;
    void stop( Generation generation ) override;
    void requestStop( Generation generation, StopDisposition disposition ) override;
    void clearRemoteAsync( Generation generation, ClearRequestId requestId ) override;
    QString lastError() const override;
    LiveDataStatistics statistics() const override;

    std::optional<LiveSourceError> lastStructuredError() const override;
    void serviceShutdown();

private:
    struct CallbackGate;

    void postReady( Generation generation );
    void postBytesAvailable( Generation generation );
    void postFailure( Generation generation, ClassifiedIosNativeError error );
    void postStopped( Generation generation );
    bool drainCurrent( Generation generation );
    bool publishPendingFailureIfReady( Generation generation );
    void reportDrainFailure();
    void publishState( Generation generation, State state );
    void completeStopped();
    void scheduleDrain( Generation generation );

    const IosNativeStreamWorkerFactory& workerFactory_;
    IosNativeStreamConfig baseConfig_;
    std::shared_ptr<CallbackGate> callbackGate_;
    std::unique_ptr<IosNativeStreamSession> session_;
    std::optional<LiveDataBatch> pendingBatch_;
    std::size_t pendingOffset_{ 0 };
    bool nativeStopped_{ false };
    bool drainScheduled_{ false };
    bool queueWorkPending_{ false };
    std::optional<Generation> activeGeneration_;
    std::optional<Generation> retiringGeneration_;
    std::optional<Generation> pendingStart_;
    StopDisposition stopDisposition_{ StopDisposition::DiscardPending };
    quint64 discardedBytes_{ 0 };
    bool drainFailed_{ false };
    std::optional<Generation> stateGeneration_;
    State state_{ State::Disconnected };
    QString lastError_;
    std::optional<LiveSourceError> lastStructuredError_;
    std::optional<ClassifiedIosNativeError> pendingFailure_;
    bool shuttingDown_{ false };
};

} // namespace klogg::livecapture::ios
