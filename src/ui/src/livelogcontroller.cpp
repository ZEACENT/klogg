/*
 * Copyright (C) 2026
 *
 * This file is part of klogg.
 */

#include "livelogcontroller.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <QHash>
#include <QObject>
#include <QTimer>

namespace klogg::livelog {
namespace {

namespace live = livecapture;
namespace adb = livecapture::adb;
namespace ios = livecapture::ios;

class SteadyLiveLogClock final : public LiveLogClock {
public:
    live::Timestamp now() const noexcept override
    {
        return std::chrono::duration_cast<live::Timestamp>(
            std::chrono::steady_clock::now().time_since_epoch() );
    }
};

class QtLiveLogScheduler final : public QObject, public LiveLogScheduler {
public:
    explicit QtLiveLogScheduler( LiveLogClock& clock )
        : clock_( clock )
    {
    }

    Token schedule( live::Timestamp deadline, std::function<void()> callback ) override
    {
        const auto token = ++nextToken_;
        auto* timer = new QTimer( this ); // NOLINT(cppcoreguidelines-owning-memory)
        timer->setSingleShot( true );
        timer->setTimerType( Qt::PreciseTimer );
        timers_.insert( token, timer );
        QObject::connect( timer, &QTimer::timeout, this,
                          [ this, token, timer, callback = std::move( callback ) ]() mutable {
                              timers_.remove( token );
                              timer->deleteLater();
                              callback();
                          } );
        const auto remaining = std::max( live::Timestamp{ 0 }, deadline - clock_.now() );
        const auto bounded = std::min<std::int64_t>( remaining.count(),
                                                    std::numeric_limits<int>::max() );
        timer->start( static_cast<int>( bounded ) );
        return token;
    }

    void cancel( Token token ) override
    {
        auto* timer = timers_.take( token );
        if ( timer != nullptr ) {
            timer->stop();
            timer->deleteLater();
        }
    }

private:
    LiveLogClock& clock_;
    QHash<Token, QTimer*> timers_;
    Token nextToken_{ 0 };
};

std::optional<adb::LogBuffer> androidBuffer( const QString& value )
{
    const auto normalized = value.trimmed().toLower();
    if ( normalized == QLatin1String( "main" ) ) {
        return adb::LogBuffer::Main;
    }
    if ( normalized == QLatin1String( "system" ) ) {
        return adb::LogBuffer::System;
    }
    if ( normalized == QLatin1String( "radio" ) ) {
        return adb::LogBuffer::Radio;
    }
    if ( normalized == QLatin1String( "events" ) ) {
        return adb::LogBuffer::Events;
    }
    if ( normalized == QLatin1String( "crash" ) ) {
        return adb::LogBuffer::Crash;
    }
    return std::nullopt;
}

std::optional<adb::LogPriority> androidPriority( const QString& value )
{
    const auto normalized = value.trimmed().toLower();
    if ( normalized == QLatin1String( "v" ) || normalized == QLatin1String( "verbose" ) ) {
        return adb::LogPriority::Verbose;
    }
    if ( normalized == QLatin1String( "d" ) || normalized == QLatin1String( "debug" ) ) {
        return adb::LogPriority::Debug;
    }
    if ( normalized == QLatin1String( "i" ) || normalized == QLatin1String( "info" ) ) {
        return adb::LogPriority::Info;
    }
    if ( normalized == QLatin1String( "w" ) || normalized == QLatin1String( "warn" )
         || normalized == QLatin1String( "warning" ) ) {
        return adb::LogPriority::Warn;
    }
    if ( normalized == QLatin1String( "e" ) || normalized == QLatin1String( "error" ) ) {
        return adb::LogPriority::Error;
    }
    if ( normalized == QLatin1String( "f" ) || normalized == QLatin1String( "fatal" ) ) {
        return adb::LogPriority::Fatal;
    }
    if ( normalized == QLatin1String( "s" ) || normalized == QLatin1String( "silent" ) ) {
        return adb::LogPriority::Silent;
    }
    return std::nullopt;
}

std::optional<std::vector<adb::LogcatFilter>>
androidFilters( const QString& filterSpec, const QString& defaultPriority )
{
    if ( filterSpec.contains( QChar::Null ) || filterSpec.contains( QLatin1Char( '\n' ) )
         || filterSpec.contains( QLatin1Char( '\r' ) ) ) {
        return std::nullopt;
    }

    std::vector<adb::LogcatFilter> filters;
    const auto expressions = filterSpec.split( QLatin1Char( ' ' ), Qt::SkipEmptyParts );
    filters.reserve( static_cast<std::size_t>( expressions.size() ) + 1u );
    for ( const auto& expression : expressions ) {
        const auto separator = expression.lastIndexOf( QLatin1Char( ':' ) );
        const auto tag = separator < 0 ? expression : expression.left( separator );
        const auto priorityText = separator < 0 ? QStringLiteral( "verbose" )
                                                : expression.mid( separator + 1 );
        const auto priority = androidPriority( priorityText );
        if ( tag.isEmpty() || !priority.has_value() ) {
            return std::nullopt;
        }
        filters.push_back( adb::LogcatFilter{ tag.toUtf8().toStdString(), *priority } );
    }

    if ( !defaultPriority.trimmed().isEmpty() ) {
        const auto priority = androidPriority( defaultPriority );
        if ( !priority.has_value() ) {
            return std::nullopt;
        }
        filters.push_back( adb::LogcatFilter{ "*", *priority } );
    }
    return filters;
}

std::vector<std::string> utf8Strings( const QStringList& values )
{
    std::vector<std::string> result;
    result.reserve( static_cast<std::size_t>( values.size() ) );
    for ( const auto& value : values ) {
        result.push_back( value.toUtf8().toStdString() );
    }
    return result;
}

} // namespace

class LiveLogController::ProductionRuntime final {
public:
    explicit ProductionRuntime( LiveLogController& controller )
        : controller_( controller )
    {
        ticker_.setInterval( 250 );
        ticker_.setTimerType( Qt::PreciseTimer );
        QObject::connect( &ticker_, &QTimer::timeout,
                          [ this ] { controller_.refreshPresentationTime(); } );
    }

    void setRetryCountdownActive( bool active )
    {
        if ( active ) {
            if ( !ticker_.isActive() ) {
                ticker_.start();
            }
        }
        else {
            ticker_.stop();
        }
    }

    void armStabilityObservation( live::Generation generation, live::Timestamp interval )
    {
        stabilityTimer_.stop();
        stabilityTimer_.setSingleShot( true );
        stabilityTimer_.setTimerType( Qt::PreciseTimer );
        stabilityTimer_.disconnect();
        QObject::connect( &stabilityTimer_, &QTimer::timeout,
                          [ this, generation ] { controller_.streamStable( generation ); } );
        const auto bounded = std::min<std::int64_t>(
            std::max<std::int64_t>( 0, interval.count() ),
            static_cast<std::int64_t>( std::numeric_limits<int>::max() ) );
        stabilityTimer_.start( static_cast<int>( bounded ) );
    }

private:
    LiveLogController& controller_;
    QTimer ticker_;
    QTimer stabilityTimer_;
};

LiveLogController::LiveLogController( LiveLogSessionSpec spec, LiveLogControllerConfig config,
                                      LiveLogClock& clock, LiveLogScheduler& scheduler,
                                      LiveLogControllerEffects& effects )
    : spec_( std::move( spec ) )
    , config_( config )
    , snapshot_( live::initialLiveState() )
    , clock_( &clock )
    , scheduler_( &scheduler )
    , effects_( effects )
{
}

LiveLogController::LiveLogController( LiveLogSessionSpec spec, LiveLogControllerConfig config,
                                      LiveLogControllerEffects& effects )
    : spec_( std::move( spec ) )
    , config_( config )
    , snapshot_( live::initialLiveState() )
    , ownedClock_( std::make_unique<SteadyLiveLogClock>() )
    , clock_( ownedClock_.get() )
    , ownedScheduler_( std::make_unique<QtLiveLogScheduler>( *clock_ ) )
    , scheduler_( ownedScheduler_.get() )
    , effects_( effects )
{
    productionRuntime_ = std::make_unique<ProductionRuntime>( *this );
}

LiveLogController::~LiveLogController()
{
    cancelScheduledRetry();
}

const LiveLogSessionSpec& LiveLogController::spec() const noexcept
{
    return spec_;
}

const live::LiveStateSnapshot& LiveLogController::snapshot() const noexcept
{
    return snapshot_;
}

live::LiveStatePresentation LiveLogController::presentation() const
{
    return live::projectLiveState( snapshot_ );
}

void LiveLogController::setChangedCallback( std::function<void()> callback )
{
    changedCallback_ = std::move( callback );
}

void LiveLogController::armRunIntent()
{
    if ( spec_.runIntent == live::RunIntent::Running ) {
        dispatch( live::StartRequested{ clock_->now() } );
    }
}

void LiveLogController::startRequested()
{
    spec_.runIntent = live::RunIntent::Running;
    dispatch( live::StartRequested{ clock_->now() } );
}

void LiveLogController::stopRequested()
{
    spec_.runIntent = live::RunIntent::Stopped;
    dispatch( live::StopRequested{ clock_->now() } );
}

void LiveLogController::stopCompleted( live::Generation generation )
{
    dispatch( live::StopCompleted{ generation, clock_->now() } );
}

void LiveLogController::reconnectRequested()
{
    spec_.runIntent = live::RunIntent::Running;
    dispatch( live::StartRequested{ clock_->now() } );
}

void LiveLogController::refreshPresentationTime()
{
    dispatch( live::TimeAdvanced{ clock_->now() } );
}

void LiveLogController::infrastructureChanged(
    live::InfrastructureStatus status,
    std::optional<live::InfrastructureOwnership> ownership )
{
    dispatch( live::InfrastructureChanged{ status, ownership, clock_->now() } );
}

void LiveLogController::infrastructureFailed( live::Generation generation,
                                              live::LiveSourceError error )
{
    dispatch( live::InfrastructureFailed{ generation, std::move( error ), clock_->now() } );
}

void LiveLogController::deviceAvailable( live::Generation generation )
{
    dispatch( live::DeviceAvailable{ generation, clock_->now() } );
}

void LiveLogController::deviceAbsent( live::Generation generation )
{
    dispatch( live::DeviceAbsent{ generation, clock_->now() } );
}

void LiveLogController::userActionRequired( live::Generation generation,
                                            live::AwaitingUserReason reason )
{
    dispatch( live::UserActionRequired{ generation, reason, clock_->now() } );
}

void LiveLogController::protocolServiceReady( live::Generation generation )
{
    dispatch( live::ProtocolServiceReady{ generation, clock_->now() } );
}

void LiveLogController::streamHandleOpened( live::Generation generation )
{
    dispatch( live::StreamHandleOpened{ generation, clock_->now() } );
}

void LiveLogController::streamReadArmed( live::Generation generation )
{
    dispatch( live::StreamReadArmed{ generation, clock_->now() } );
    if ( productionRuntime_ && snapshot_.generation == generation
         && snapshot_.source.status == live::SourceStatus::Streaming ) {
        productionRuntime_->armStabilityObservation( generation,
                                                     config_.reducer.stabilityInterval );
    }
}

void LiveLogController::streamBytesReceived( live::Generation generation, const QByteArray& bytes )
{
    dispatch( live::StreamBytesReceived{ generation, static_cast<std::size_t>( bytes.size() ),
                                         clock_->now() },
              &bytes );
}

void LiveLogController::streamStable( live::Generation generation )
{
    dispatch( live::StreamStable{ generation, clock_->now() } );
}

void LiveLogController::streamFailed( live::Generation generation, live::LiveSourceError error )
{
    if ( generation != snapshot_.generation
         || snapshot_.runIntent != live::RunIntent::Running
         || ( snapshot_.source.status != live::SourceStatus::OpeningStream
              && snapshot_.source.status != live::SourceStatus::Streaming ) ) {
        return;
    }

    const auto now = clock_->now();
    if ( spec_.capture.autoReconnectEnabled ) {
        switch ( error.retryPolicy ) {
        case live::RetryPolicy::WaitForInfrastructure:
            dispatch( live::InfrastructureChanged{ live::InfrastructureStatus::Unavailable,
                                                   snapshot_.infrastructure.ownership, now } );
            return;
        case live::RetryPolicy::WaitForDevice:
            dispatch( live::DeviceAbsent{ generation, now } );
            return;
        case live::RetryPolicy::AwaitUser:
            if ( error.awaitingUserReason.has_value() ) {
                dispatch( live::UserActionRequired{ generation, *error.awaitingUserReason, now } );
                return;
            }
            error.retryPolicy = live::RetryPolicy::Never;
            break;
        case live::RetryPolicy::Never:
        case live::RetryPolicy::Immediate:
        case live::RetryPolicy::Backoff:
            break;
        }
    }
    else {
        error.retryPolicy = live::RetryPolicy::Never;
    }

    const auto attempt = snapshot_.consecutiveFailures == std::numeric_limits<unsigned>::max()
                             ? snapshot_.consecutiveFailures
                             : snapshot_.consecutiveFailures + 1u;
    const auto delay = error.retryPolicy == live::RetryPolicy::Immediate
                           ? live::Timestamp{ 0 }
                           : retryDelay( attempt );
    dispatch( live::RetryRequested{ generation, std::move( error ), attempt, now + delay, now } );
}

void LiveLogController::captureChanged( live::Generation generation, live::CaptureState state,
                                        std::optional<live::LiveSourceError> error )
{
    dispatch( live::CaptureChanged{ generation, state, std::move( error ), clock_->now() } );
}

void LiveLogController::dispatch( const live::LiveStateEvent& event, const QByteArray* bytes )
{
    pendingDispatches_.push_back( PendingDispatch{
        event, bytes != nullptr ? std::optional<QByteArray>{ *bytes } : std::nullopt } );
    if ( std::exchange( dispatching_, true ) ) {
        return;
    }

    try {
        while ( !pendingDispatches_.empty() ) {
            auto pending = std::move( pendingDispatches_.front() );
            pendingDispatches_.pop_front();

            auto transition = live::reduce( snapshot_, pending.event, config_.reducer );
            if ( !transition.accepted ) {
                continue;
            }

            snapshot_ = std::move( transition.snapshot );
            const auto* pendingBytes
                = pending.bytes.has_value() ? &pending.bytes.value() : nullptr;
            for ( const auto& effect : transition.effects ) {
                execute( effect, pendingBytes );
            }
            notifyChanged();
        }
    } catch ( ... ) {
        pendingDispatches_.clear();
        dispatching_ = false;
        throw;
    }
    dispatching_ = false;
}

void LiveLogController::execute( const live::LiveStateEffect& effect, const QByteArray* bytes )
{
    switch ( effect.kind ) {
    case live::EffectKind::InvalidateGeneration:
        effects_.invalidateGeneration( effect.generation );
        break;
    case live::EffectKind::CancelStream:
        effects_.cancelStream( effect.generation );
        break;
    case live::EffectKind::StartInfrastructure:
        effects_.startInfrastructure( effect.generation );
        break;
    case live::EffectKind::OpenStream:
        effects_.openStream( effect.generation, transportConfig() );
        break;
    case live::EffectKind::AppendBytes:
        if ( bytes != nullptr ) {
            effects_.appendBytes( effect.generation, *bytes );
        }
        break;
    case live::EffectKind::ArmRetryTimer: {
        cancelScheduledRetry();
        const auto generation = effect.generation;
        const auto deadline = effect.deadline;
        struct CallbackState {
            bool installing{ true };
            bool fired{ false };
        };
        const auto callbackState = std::make_shared<CallbackState>();
        const auto reachDeadline = [ this, generation, deadline ] {
            if ( !snapshot_.retryTimer.has_value()
                 || snapshot_.retryTimer->generation != generation
                 || snapshot_.retryTimer->deadline != deadline ) {
                return;
            }
            dispatch( live::RetryDeadlineReached{ generation, clock_->now() } );
        };
        const auto token = scheduler_->schedule(
            deadline, [ callbackState, reachDeadline ] {
                callbackState->fired = true;
                if ( !callbackState->installing ) {
                    reachDeadline();
                }
            } );
        retryToken_ = token;
        callbackState->installing = false;
        if ( callbackState->fired ) {
            reachDeadline();
        }
        if ( productionRuntime_ && snapshot_.retryTimer.has_value()
             && snapshot_.retryTimer->generation == generation
             && snapshot_.retryTimer->deadline == deadline ) {
            productionRuntime_->setRetryCountdownActive( true );
        }
        break;
    }
    case live::EffectKind::CancelRetryTimer:
        cancelScheduledRetry();
        break;
    }
}

live::Timestamp LiveLogController::retryDelay( unsigned attempt ) const
{
    auto delay = std::max( live::Timestamp{ 0 }, config_.initialRetryDelay );
    const auto maximum = std::max( delay, config_.maximumRetryDelay );
    for ( unsigned current = 1u; current < attempt && delay < maximum; ++current ) {
        if ( delay.count() > maximum.count() / 2 ) {
            return maximum;
        }
        delay *= 2;
    }
    return std::min( delay, maximum );
}

LiveSourceTransportConfig LiveLogController::transportConfig() const
{
    return makeLiveSourceTransportConfig( spec_ );
}

LiveSourceTransportConfig makeLiveSourceTransportConfig( const LiveLogSessionSpec& spec )
{
    LiveSourceTransportConfig config;
    config.sourceType = spec.sourceKind == SourceKind::IosSyslog ? LiveLogSourceType::IosLogStream
                                                                 : LiveLogSourceType::AdbLogcat;
    config.adbBackend = spec.androidBackend == AndroidBackend::SmartSocket
                            ? AdbTransportBackend::SmartSocket
                            : AdbTransportBackend::Process;
    config.iosBackend = spec.iosBackend == IosBackend::Native ? IosTransportBackend::Native
                                                              : IosTransportBackend::LegacyProcess;
    config.deviceId = spec.device.deviceId;
    config.iosEndpoint.udid = spec.device.deviceId.toStdString();
    config.iosEndpoint.connectionType
        = spec.device.connection == DeviceIdentity::Connection::Network
              ? ios::NativeConnectionType::Network
              : ios::NativeConnectionType::Usb;
    config.ansiOutputEnabled = spec.capture.ansiOutputEnabled;
    config.androidBuffers = spec.android.buffers;
    config.androidFilterSpec = spec.android.filterSpec;
    config.androidPriority = spec.android.priority;
    config.androidPid = spec.android.pid;
    config.iosLevel = spec.ios.level;
    config.iosCategories = spec.ios.categories;
    config.iosSubsystem = spec.ios.subsystem;
    config.iosJsonOutput = spec.ios.outputFormat == IosOptions::OutputFormat::Json;
    return config;
}

void LiveLogController::cancelScheduledRetry()
{
    if ( retryToken_.has_value() ) {
        scheduler_->cancel( *retryToken_ );
        retryToken_.reset();
    }
    if ( productionRuntime_ ) {
        productionRuntime_->setRetryCountdownActive( false );
    }
}

void LiveLogController::notifyChanged()
{
    if ( changedCallback_ ) {
        changedCallback_();
    }
}

std::optional<adb::AdbSmartSocketTransportConfig>
makeAdbSmartSocketTransportConfig( const LiveSourceTransportConfig& config )
{
    if ( config.sourceType != LiveLogSourceType::AdbLogcat
         || config.adbBackend != AdbTransportBackend::SmartSocket ) {
        return std::nullopt;
    }

    adb::AdbSmartSocketTransportConfig result;
    result.deviceSerial = config.deviceId;
    result.logcatOptions.ansiOutputEnabled = config.ansiOutputEnabled;
    for ( const auto& bufferName : config.androidBuffers ) {
        const auto buffer = androidBuffer( bufferName );
        if ( !buffer.has_value() ) {
            return std::nullopt;
        }
        result.logcatOptions.buffers.push_back( *buffer );
    }
    if ( config.androidPid.has_value() ) {
        if ( *config.androidPid < 0 ) {
            return std::nullopt;
        }
        result.logcatOptions.processId = static_cast<std::uint32_t>( *config.androidPid );
    }
    auto filters = androidFilters( config.androidFilterSpec, config.androidPriority );
    if ( !filters.has_value() ) {
        return std::nullopt;
    }
    result.logcatOptions.filters = std::move( *filters );
    if ( !adb::buildLogcatService( result.logcatOptions ).value.has_value() ) {
        return std::nullopt;
    }
    return result;
}

std::optional<ios::IosNativeStreamConfig>
makeIosNativeStreamConfig( const LiveSourceTransportConfig& config )
{
    if ( config.sourceType != LiveLogSourceType::IosLogStream
         || config.iosBackend != IosTransportBackend::Native ) {
        return std::nullopt;
    }

    ios::IosNativeStreamConfig result;
    result.endpoint = config.iosEndpoint;
    result.ansiOutputEnabled = config.ansiOutputEnabled;
    result.logOptions.level = config.iosLevel.toUtf8().toStdString();
    result.logOptions.categories = utf8Strings( config.iosCategories );
    result.logOptions.subsystem = config.iosSubsystem.toUtf8().toStdString();
    result.logOptions.outputFormat = config.iosJsonOutput ? ios::IosLogOutputFormat::Json
                                                         : ios::IosLogOutputFormat::Default;
    return result;
}

} // namespace klogg::livelog
