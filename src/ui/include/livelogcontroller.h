/*
 * Copyright (C) 2026
 *
 * This file is part of klogg.
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>

#include <QByteArray>

#include "adbsmartsockettransport.h"
#include "iosnativestream.h"
#include "livelogsession.h"
#include "livesourcetransport.h"

namespace klogg::livelog {

class LiveLogClock {
public:
    virtual ~LiveLogClock() = default;
    virtual livecapture::Timestamp now() const noexcept = 0;
};

class LiveLogScheduler {
public:
    using Token = std::uint64_t;

    virtual ~LiveLogScheduler() = default;
    virtual Token schedule( livecapture::Timestamp deadline, std::function<void()> callback ) = 0;
    virtual void cancel( Token token ) = 0;
};

class LiveLogControllerEffects {
public:
    virtual ~LiveLogControllerEffects() = default;

    virtual void invalidateGeneration( livecapture::Generation generation ) = 0;
    virtual void cancelStream( livecapture::Generation generation ) = 0;
    virtual void startInfrastructure( livecapture::Generation generation ) = 0;
    virtual void openStream( livecapture::Generation generation,
                             const LiveSourceTransportConfig& config ) = 0;
    virtual void appendBytes( livecapture::Generation generation, const QByteArray& bytes ) = 0;
};

struct LiveLogControllerConfig {
    livecapture::LiveStateConfig reducer;
    livecapture::Timestamp initialRetryDelay{ std::chrono::seconds{ 1 } };
    livecapture::Timestamp maximumRetryDelay{ std::chrono::seconds{ 30 } };
};

class LiveLogController {
public:
    LiveLogController( LiveLogSessionSpec spec, LiveLogControllerConfig config,
                       LiveLogClock& clock, LiveLogScheduler& scheduler,
                       LiveLogControllerEffects& effects );
    LiveLogController( LiveLogSessionSpec spec, LiveLogControllerConfig config,
                       LiveLogControllerEffects& effects );
    ~LiveLogController();

    LiveLogController( const LiveLogController& ) = delete;
    LiveLogController& operator=( const LiveLogController& ) = delete;

    const LiveLogSessionSpec& spec() const noexcept;
    const livecapture::LiveStateSnapshot& snapshot() const noexcept;
    livecapture::LiveStatePresentation presentation() const;

    void setChangedCallback( std::function<void()> callback );

    void armRunIntent();
    void startRequested();
    void stopRequested();
    void stopCompleted( livecapture::Generation generation );
    void reconnectRequested();
    void refreshPresentationTime();

    void infrastructureChanged(
        livecapture::InfrastructureStatus status,
        std::optional<livecapture::InfrastructureOwnership> ownership = std::nullopt );
    void infrastructureFailed( livecapture::Generation generation,
                               livecapture::LiveSourceError error );
    void deviceAvailable( livecapture::Generation generation );
    void deviceAbsent( livecapture::Generation generation );
    void userActionRequired( livecapture::Generation generation,
                             livecapture::AwaitingUserReason reason );
    void protocolServiceReady( livecapture::Generation generation );
    void streamHandleOpened( livecapture::Generation generation );
    void streamReadArmed( livecapture::Generation generation );
    void streamBytesReceived( livecapture::Generation generation, const QByteArray& bytes );
    void streamStable( livecapture::Generation generation );
    void streamFailed( livecapture::Generation generation, livecapture::LiveSourceError error );
    void captureChanged( livecapture::Generation generation, livecapture::CaptureState state,
                         std::optional<livecapture::LiveSourceError> error = std::nullopt );

private:
    class ProductionRuntime;

    struct PendingDispatch {
        livecapture::LiveStateEvent event;
        std::optional<QByteArray> bytes;
    };

    void dispatch( const livecapture::LiveStateEvent& event, const QByteArray* bytes = nullptr );
    void execute( const livecapture::LiveStateEffect& effect, const QByteArray* bytes );
    livecapture::Timestamp retryDelay( unsigned attempt ) const;
    LiveSourceTransportConfig transportConfig() const;
    void cancelScheduledRetry();
    void notifyChanged();

    LiveLogSessionSpec spec_;
    LiveLogControllerConfig config_;
    livecapture::LiveStateSnapshot snapshot_;
    std::unique_ptr<LiveLogClock> ownedClock_;
    LiveLogClock* clock_{ nullptr };
    std::unique_ptr<LiveLogScheduler> ownedScheduler_;
    LiveLogScheduler* scheduler_{ nullptr };
    std::unique_ptr<ProductionRuntime> productionRuntime_;
    LiveLogControllerEffects& effects_;
    std::optional<LiveLogScheduler::Token> retryToken_;
    std::deque<PendingDispatch> pendingDispatches_;
    std::function<void()> changedCallback_;
    bool dispatching_{ false };
};

LiveSourceTransportConfig makeLiveSourceTransportConfig( const LiveLogSessionSpec& spec );
std::optional<livecapture::adb::AdbSmartSocketTransportConfig>
makeAdbSmartSocketTransportConfig( const LiveSourceTransportConfig& config );
std::optional<livecapture::ios::IosNativeStreamConfig>
makeIosNativeStreamConfig( const LiveSourceTransportConfig& config );

} // namespace klogg::livelog
