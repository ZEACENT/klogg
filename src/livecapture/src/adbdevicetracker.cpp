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

#include "adbdevicetracker.h"

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <utility>

namespace klogg::livecapture::adb {
namespace {

AdbDeviceState stateFromText( const QString& state )
{
    if ( state == QStringLiteral( "device" ) ) {
        return AdbDeviceState::Online;
    }
    if ( state == QStringLiteral( "unauthorized" ) ) {
        return AdbDeviceState::Unauthorized;
    }
    if ( state == QStringLiteral( "offline" ) ) {
        return AdbDeviceState::Offline;
    }
    return AdbDeviceState::Other;
}

std::optional<std::uint64_t> parseTransportId( const QString& text )
{
    bool okay = false;
    const auto value = text.toULongLong( &okay );
    if ( !okay ) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>( value );
}

struct TrackedDevicesParseResult {
    std::vector<AdbDeviceInfo> devices;
    std::optional<QString> diagnostic;
};

TrackedDevicesParseResult parseTrackedDevices( const QByteArray& payload )
{
    TrackedDevicesParseResult result;
    const auto lines = QString::fromUtf8( payload ).split( '\n' );
    result.devices.reserve( static_cast<std::size_t>( lines.size() ) );

    for ( decltype( lines.size() ) index = 0; index < lines.size(); ++index ) {
        auto line = lines.at( index ).trimmed();
        if ( line.isEmpty() ) {
            continue;
        }

        const auto lineNumber = index + 1;
        const auto separator = line.indexOf( '\t' );
        if ( separator <= 0 ) {
            result.diagnostic
                = QStringLiteral( "Malformed ADB track-devices snapshot line %1: expected a "
                                  "serial and state separated by a tab." )
                      .arg( lineNumber );
            return result;
        }

        const auto serial = line.left( separator ).trimmed();
        const auto details = line.mid( separator + 1 ).simplified();
        const auto parts = details.split( ' ' );
        if ( serial.isEmpty() || parts.isEmpty() || parts.front().isEmpty() ) {
            result.diagnostic
                = QStringLiteral( "Malformed ADB track-devices snapshot line %1: serial or state "
                                  "is empty." )
                      .arg( lineNumber );
            return result;
        }

        const auto serialBytes = serial.toStdString();
        if ( std::any_of( result.devices.begin(), result.devices.end(),
                          [ &serialBytes ]( const auto& device ) {
                              return device.serial == serialBytes;
                          } ) ) {
            result.diagnostic
                = QStringLiteral(
                      "Malformed ADB track-devices snapshot line %1: duplicate serial." )
                      .arg( lineNumber );
            return result;
        }

        AdbDeviceInfo device;
        device.serial = serialBytes;
        device.stateText = parts.front().toStdString();
        device.state = stateFromText( parts.front() );
        for ( decltype( parts.size() ) partIndex = 1; partIndex < parts.size(); ++partIndex ) {
            const auto& part = parts.at( partIndex );
            if ( part.startsWith( QStringLiteral( "product:" ) ) ) {
                device.product = part.mid( 8 ).replace( '_', ' ' ).toStdString();
            }
            else if ( part.startsWith( QStringLiteral( "model:" ) ) ) {
                device.model = part.mid( 6 ).replace( '_', ' ' ).toStdString();
            }
            else if ( part.startsWith( QStringLiteral( "device:" ) ) ) {
                device.device = part.mid( 7 ).replace( '_', ' ' ).toStdString();
            }
            else if ( part.startsWith( QStringLiteral( "transport_id:" ) ) ) {
                device.transportId = parseTransportId( part.mid( 13 ) );
                if ( !device.transportId.has_value() ) {
                    result.diagnostic
                        = QStringLiteral( "Malformed ADB track-devices snapshot line %1: invalid "
                                          "transport_id." )
                              .arg( lineNumber );
                    return result;
                }
            }
        }
        result.devices.push_back( std::move( device ) );
    }

    return result;
}

bool equalDevice( const AdbDeviceInfo& lhs, const AdbDeviceInfo& rhs )
{
    return lhs.serial == rhs.serial && lhs.state == rhs.state && lhs.stateText == rhs.stateText
           && lhs.product == rhs.product && lhs.model == rhs.model && lhs.device == rhs.device
           && lhs.transportId == rhs.transportId;
}

bool equalDevices( const std::vector<AdbDeviceInfo>& lhs, const std::vector<AdbDeviceInfo>& rhs )
{
    return lhs.size() == rhs.size()
           && std::equal(
               lhs.begin(), lhs.end(), rhs.begin(),
               []( const auto& left, const auto& right ) { return equalDevice( left, right ); } );
}

bool equalError( const LiveSourceError& lhs, const LiveSourceError& rhs )
{
    return lhs.category == rhs.category && lhs.code == rhs.code && lhs.scope == rhs.scope
           && lhs.retryPolicy == rhs.retryPolicy && lhs.message == rhs.message
           && lhs.nativeDetail == rhs.nativeDetail;
}

bool equalError( const std::optional<LiveSourceError>& lhs,
                 const std::optional<LiveSourceError>& rhs )
{
    if ( lhs.has_value() != rhs.has_value() ) {
        return false;
    }
    return !lhs.has_value() || equalError( *lhs, *rhs );
}

const char* trackerErrorCode( AdbSmartSocketErrorCode code )
{
    switch ( code ) {
    case AdbSmartSocketErrorCode::Connection:
        return "adb-track-connection";
    case AdbSmartSocketErrorCode::Protocol:
        return "adb-track-protocol";
    case AdbSmartSocketErrorCode::RemoteFailure:
        return "adb-track-remote-failure";
    case AdbSmartSocketErrorCode::UnexpectedEof:
        return "adb-track-unexpected-eof";
    case AdbSmartSocketErrorCode::ConnectTimeout:
        return "adb-track-connect-timeout";
    case AdbSmartSocketErrorCode::WriteTimeout:
        return "adb-track-write-timeout";
    case AdbSmartSocketErrorCode::ReadTimeout:
        return "adb-track-read-timeout";
    }
    return "adb-track-failed";
}

LiveSourceError trackerError( AdbSmartSocketErrorCode code, const QString& diagnostic )
{
    return LiveSourceError{ code == AdbSmartSocketErrorCode::RemoteFailure ? ErrorCategory::Service
                                                                           : ErrorCategory::Backend,
                            trackerErrorCode( code ),
                            ErrorScope::Service,
                            RetryPolicy::Backoff,
                            "ADB device tracking was interrupted; reconnecting.",
                            diagnostic.toStdString() };
}

void registerSignalMetaTypes()
{
    qRegisterMetaType<AdbTrackedDeviceSnapshot>(
        "klogg::livecapture::adb::AdbTrackedDeviceSnapshot" );
}

} // namespace

class AdbDeviceTracker::Impl final {
public:
    Impl( AdbDeviceTracker& tracker, AdbDeviceTrackerConfig config, AdbSmartSocketClient& client,
          AdbServerScheduler& scheduler )
        : tracker_( tracker )
        , config_( std::move( config ) )
        , client_( client )
        , scheduler_( scheduler )
        , callbackGate_( std::make_shared<CallbackGate>() )
    {
        callbackGate_->owner = this;
        QObject::connect( &client_, &AdbSmartSocketClient::hostReplyReceived, &tracker_,
                          [ this ]( Generation generation,
                                    AdbSmartSocketClient::OperationId operationId,
                                    const QByteArray& payload ) {
                              replyReceived( generation, operationId, payload );
                          } );
        QObject::connect( &client_, &AdbSmartSocketClient::errorOccurred, &tracker_,
                          [ this ]( Generation generation,
                                    AdbSmartSocketClient::OperationId operationId,
                                    AdbSmartSocketErrorCode code, const QString& diagnostic ) {
                              operationFailed( generation, operationId, code, diagnostic );
                          } );
    }

    ~Impl()
    {
        callbackGate_->owner = nullptr;
        stop();
    }

    void start( Generation generation, std::uint64_t infrastructureEpoch )
    {
        if ( running_ && managerGeneration_ == generation
             && infrastructureEpoch_ == infrastructureEpoch ) {
            return;
        }

        stopOperation();
        cancelReconnect();
        running_ = true;
        ++runSerial_;
        managerGeneration_ = generation;
        infrastructureEpoch_ = infrastructureEpoch;
        // A snapshot does not prove a long-lived subscription is stable; only a new
        // infrastructure epoch resets failures accumulated by this tracker run.
        reconnectAttempt_ = 0u;
        beginTracking();
    }

    void stop()
    {
        if ( !running_ && activeRequestGeneration_ == 0u && reconnectToken_ == 0u ) {
            return;
        }
        running_ = false;
        ++runSerial_;
        cancelReconnect();
        stopOperation();
    }

    const AdbTrackedDeviceSnapshot& snapshot() const noexcept
    {
        return snapshot_;
    }

private:
    struct CallbackGate {
        Impl* owner{ nullptr };
    };

    struct ScheduleInvocation {
        bool fired{ false };
    };

    void beginTracking()
    {
        if ( !running_ ) {
            return;
        }

        cancelReconnect();
        stopOperation();
        activeRequestGeneration_ = ++nextRequestGeneration_;
        activeOperationId_ = ++nextOperationId_;
        client_.requestHostService( activeRequestGeneration_, activeOperationId_,
                                    HostService::TrackDevicesLong );
    }

    void replyReceived( Generation generation, AdbSmartSocketClient::OperationId operationId,
                        const QByteArray& payload )
    {
        if ( !isCurrentOperation( generation, operationId ) ) {
            return;
        }

        auto parsed = parseTrackedDevices( payload );
        if ( parsed.diagnostic.has_value() ) {
            failCurrentOperation(
                trackerError( AdbSmartSocketErrorCode::Protocol, *parsed.diagnostic ) );
            return;
        }

        const auto changed = snapshot_.generation != managerGeneration_
                             || snapshot_.infrastructureEpoch != infrastructureEpoch_
                             || !equalDevices( snapshot_.devices, parsed.devices )
                             || snapshot_.error.has_value();
        if ( !changed ) {
            return;
        }

        snapshot_.generation = managerGeneration_;
        snapshot_.infrastructureEpoch = infrastructureEpoch_;
        snapshot_.requestGeneration = activeRequestGeneration_;
        snapshot_.devices = std::move( parsed.devices );
        snapshot_.error.reset();
        Q_EMIT tracker_.snapshotChanged( snapshot_ );
    }

    void operationFailed( Generation generation, AdbSmartSocketClient::OperationId operationId,
                          AdbSmartSocketErrorCode code, const QString& diagnostic )
    {
        if ( !isCurrentOperation( generation, operationId ) ) {
            return;
        }
        const auto runSerial = runSerial_;
        activeRequestGeneration_ = 0u;
        activeOperationId_ = 0u;
        publishError( trackerError( code, diagnostic ), generation );
        if ( running_ && runSerial == runSerial_ ) {
            scheduleReconnect();
        }
    }

    void failCurrentOperation( LiveSourceError error )
    {
        const auto runSerial = runSerial_;
        const auto requestGeneration = activeRequestGeneration_;
        activeRequestGeneration_ = 0u;
        activeOperationId_ = 0u;
        client_.cancelGeneration( requestGeneration );
        publishError( std::move( error ), requestGeneration );
        if ( running_ && runSerial == runSerial_ ) {
            scheduleReconnect();
        }
    }

    void publishError( LiveSourceError error, Generation requestGeneration )
    {
        const auto changed
            = snapshot_.generation != managerGeneration_
              || snapshot_.infrastructureEpoch != infrastructureEpoch_
              || snapshot_.requestGeneration != requestGeneration
              || !equalError( snapshot_.error, std::optional<LiveSourceError>{ error } );
        snapshot_.generation = managerGeneration_;
        snapshot_.infrastructureEpoch = infrastructureEpoch_;
        snapshot_.requestGeneration = requestGeneration;
        snapshot_.error = std::move( error );
        if ( changed ) {
            Q_EMIT tracker_.snapshotChanged( snapshot_ );
        }
    }

    bool isCurrentOperation( Generation generation,
                             AdbSmartSocketClient::OperationId operationId ) const
    {
        return running_ && generation != 0u && generation == activeRequestGeneration_
               && operationId == activeOperationId_;
    }

    void scheduleReconnect()
    {
        if ( !running_ || config_.reconnectBackoff.empty() ) {
            return;
        }

        cancelReconnect();
        const auto index = std::min( reconnectAttempt_, config_.reconnectBackoff.size() - 1u );
        const auto delay = config_.reconnectBackoff.at( index );
        if ( reconnectAttempt_ < config_.reconnectBackoff.size() ) {
            ++reconnectAttempt_;
        }

        const auto runSerial = runSerial_;
        const auto scheduleSerial = ++reconnectSerial_;
        const auto invocation = std::make_shared<ScheduleInvocation>();
        const std::weak_ptr<CallbackGate> weakGate = callbackGate_;
        const auto token
            = scheduler_.schedule( AdbServerScheduleKind::ReconnectBackoff, delay,
                                   [ weakGate, invocation, runSerial, scheduleSerial ] {
                                       invocation->fired = true;
                                       const auto gate = weakGate.lock();
                                       if ( gate != nullptr && gate->owner != nullptr ) {
                                           gate->owner->reconnectFired( runSerial, scheduleSerial );
                                       }
                                   } );
        // A scheduler is allowed to invoke synchronously and mutate the run or
        // reconnect serial before schedule() returns.
        // cppcheck-suppress knownConditionTrueFalse
        if ( running_ && runSerial == runSerial_ && scheduleSerial == reconnectSerial_
             && !invocation->fired ) {
            reconnectToken_ = token;
        }
        else if ( token != 0u && !invocation->fired ) {
            scheduler_.cancel( token );
        }
    }

    void reconnectFired( std::uint64_t runSerial, std::uint64_t scheduleSerial )
    {
        if ( !running_ || runSerial != runSerial_ || scheduleSerial != reconnectSerial_ ) {
            return;
        }
        reconnectToken_ = 0u;
        beginTracking();
    }

    void stopOperation()
    {
        if ( activeRequestGeneration_ == 0u ) {
            return;
        }
        const auto generation = activeRequestGeneration_;
        activeRequestGeneration_ = 0u;
        activeOperationId_ = 0u;
        client_.cancelGeneration( generation );
    }

    void cancelReconnect()
    {
        ++reconnectSerial_;
        if ( reconnectToken_ != 0u ) {
            scheduler_.cancel( reconnectToken_ );
            reconnectToken_ = 0u;
        }
    }

private:
    // Injected services are non-owning and outlive the tracker by contract.
    // NOLINTBEGIN(cppcoreguidelines-avoid-const-or-ref-data-members)
    AdbDeviceTracker& tracker_;
    AdbDeviceTrackerConfig config_;
    AdbSmartSocketClient& client_;
    AdbServerScheduler& scheduler_;
    // NOLINTEND(cppcoreguidelines-avoid-const-or-ref-data-members)
    std::shared_ptr<CallbackGate> callbackGate_;

    AdbTrackedDeviceSnapshot snapshot_;
    bool running_{ false };
    Generation managerGeneration_{ 0 };
    std::uint64_t infrastructureEpoch_{ 0 };
    Generation nextRequestGeneration_{ 0 };
    Generation activeRequestGeneration_{ 0 };
    AdbSmartSocketClient::OperationId nextOperationId_{ 0 };
    AdbSmartSocketClient::OperationId activeOperationId_{ 0 };
    std::uint64_t runSerial_{ 0 };
    std::uint64_t reconnectSerial_{ 0 };
    AdbServerToken reconnectToken_{ 0 };
    std::size_t reconnectAttempt_{ 0 };
};

AdbDeviceTracker::AdbDeviceTracker( AdbDeviceTrackerConfig config, AdbSmartSocketClient& client,
                                    AdbServerScheduler& scheduler, QObject* parent )
    : QObject( parent )
    , impl_( std::make_unique<Impl>( *this, std::move( config ), client, scheduler ) )
{
    registerSignalMetaTypes();
}

AdbDeviceTracker::~AdbDeviceTracker() = default;

void AdbDeviceTracker::start( Generation generation, std::uint64_t infrastructureEpoch )
{
    impl_->start( generation, infrastructureEpoch );
}

void AdbDeviceTracker::stop()
{
    impl_->stop();
}

const AdbTrackedDeviceSnapshot& AdbDeviceTracker::snapshot() const noexcept
{
    return impl_->snapshot();
}

} // namespace klogg::livecapture::adb
