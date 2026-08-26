#include "adblogcatsource.h"

#include <limits>
#include <utility>

#include <QTimer>

#include "adbprocesstransport.h"
#include "adbsmartsockettransport.h"
#include "capturestore.h"
#include "ioslogprocesstransport.h"
#include "livelogcontroller.h"
#include "livesourcetransport.h"
#include "log.h"
#include "streaminglogdata.h"

std::unique_ptr<LiveSourceTransport>
DefaultLiveSourceTransportFactory::create( const LiveSourceTransportConfig& config ) const
{
    if ( config.sourceType == LiveLogSourceType::IosLogStream ) {
        if ( config.iosBackend != IosTransportBackend::LegacyProcess
             || config.executable.trimmed().isEmpty() ) {
            return nullptr;
        }
        return std::make_unique<IosLogProcessTransport>(
            config.executable, config.deviceId, config.extraArgs, config.ansiOutputEnabled );
    }

    if ( config.adbBackend == AdbTransportBackend::SmartSocket ) {
        auto smartSocketConfig = klogg::livelog::makeAdbSmartSocketTransportConfig( config );
        if ( !smartSocketConfig.has_value() ) {
            return nullptr;
        }
        return std::make_unique<klogg::livecapture::adb::AdbSmartSocketTransport>(
            std::move( *smartSocketConfig ) );
    }

    if ( config.executable.trimmed().isEmpty() ) {
        return nullptr;
    }
    return std::make_unique<AdbProcessTransport>( config.executable, config.deviceId,
                                                  config.extraArgs, config.ansiOutputEnabled );
}

namespace {

LiveSourceTransportConfig transportConfigFromSessionData( const AdbLogcatSessionData& sessionData )
{
    LiveSourceTransportConfig config;
    config.sourceType = sessionData.sourceType;
    config.adbBackend = sessionData.adbBackend;
    config.iosBackend = sessionData.iosBackend;
    config.iosEndpoint = sessionData.iosEndpoint;
    config.executable = sessionData.adbExecutable;
    config.deviceId = sessionData.deviceSerial;
    config.extraArgs = sessionData.extraArgs;
    config.ansiOutputEnabled = sessionData.ansiOutputEnabled;
    config.androidBuffers = sessionData.androidBuffers;
    config.androidFilterSpec = sessionData.androidFilterSpec;
    config.androidPriority = sessionData.androidPriority;
    config.androidPid = sessionData.androidPid;
    config.iosLevel = sessionData.iosLevel;
    config.iosCategories = sessionData.iosCategories;
    config.iosSubsystem = sessionData.iosSubsystem;
    config.iosJsonOutput = sessionData.iosJsonOutput;
    return config;
}

const LiveSourceTransportFactory& defaultTransportFactory()
{
    static const DefaultLiveSourceTransportFactory factory;
    return factory;
}

} // namespace

AdbLogcatSource::AdbLogcatSource( AdbLogcatSessionData sessionData,
                                  std::shared_ptr<StreamingLogData> logData, QObject* parent )
    : AdbLogcatSource( std::move( sessionData ), std::move( logData ), defaultTransportFactory(),
                       parent )
{
}

AdbLogcatSource::AdbLogcatSource( AdbLogcatSessionData sessionData,
                                  std::shared_ptr<StreamingLogData> logData,
                                  const LiveSourceTransportFactory& transportFactory,
                                  QObject* parent )
    : QObject( parent )
    , sessionData_( std::move( sessionData ) )
    , logData_( std::move( logData ) )
    , transportFactory_( &transportFactory )
{
    if ( logData_ ) {
        connect( logData_.get(), &StreamingLogData::captureOutputChanged, this,
                 &AdbLogcatSource::captureOutputChanged );
    }
}

AdbLogcatSource::~AdbLogcatSource()
{
    const auto generation = activeGeneration_;
    activeGeneration_.reset();
    if ( transport_ && generation.has_value() ) {
        transport_->stop( *generation );
    }
}

AdbLogcatSource::Generation AdbLogcatSource::nextGeneration()
{
    if ( generationCounter_ == std::numeric_limits<Generation>::max() ) {
        generationCounter_ = 0;
    }
    return ++generationCounter_;
}

AdbLogcatSource::ClearRequestId AdbLogcatSource::nextClearRequestId()
{
    if ( clearRequestCounter_ == std::numeric_limits<ClearRequestId>::max() ) {
        clearRequestCounter_ = 0;
    }
    return ++clearRequestCounter_;
}

void AdbLogcatSource::wireTransport()
{
    if ( !transport_ ) {
        return;
    }
    connect( transport_.get(), &LiveSourceTransport::bytesReceived, this,
             [ this ]( Generation generation, const QByteArray& data ) {
                 if ( activeGeneration_ != generation ) {
                     return;
                 }
                 if ( controllerBytes_ ) {
                     controllerBytes_( generation, data );
                 }
                 else if ( logData_ ) {
                     logData_->appendUtf8( data );
                 }
             } );
    connect( transport_.get(), &LiveSourceTransport::stateChanged, this,
             [ this ]( Generation generation, LiveSourceTransport::State state ) {
                 if ( activeGeneration_ == generation ) {
                     setStateFromTransport( generation, state );
                 }
             } );
    connect( transport_.get(), &LiveSourceTransport::errorOccurred, this,
             [ this ]( Generation generation, const QString& error ) {
                 if ( activeGeneration_ != generation || error.isEmpty() ) {
                     return;
                 }
                 lastError_ = error;
                 LOG_WARNING << "live log transport error " << error;
                 Q_EMIT errorOccurred( lastError_ );
             } );
    connect( transport_.get(), &LiveSourceTransport::clearRemoteFinished, this,
             &AdbLogcatSource::finishClear );
}

void AdbLogcatSource::retireTransport()
{
    if ( !transport_ ) {
        return;
    }
    QObject::disconnect( transport_.get(), nullptr, this, nullptr );
    retiredTransports_.push_back( std::move( transport_ ) );
    if ( retiredCleanupScheduled_ ) {
        return;
    }
    retiredCleanupScheduled_ = true;
    QTimer::singleShot( 0, this, [ this ] {
        retiredCleanupScheduled_ = false;
        retiredTransports_.clear();
    } );
}

void AdbLogcatSource::startTransport()
{
    const auto generation = nextGeneration();
    activeGeneration_ = generation;
    connecting_ = true;
    transport_->start( generation );
}

bool AdbLogcatSource::connectSource()
{
    if ( sessionData_.readOnlyCompatibility ) {
        lastError_ = tr( "This compatibility session is read-only." );
        setState( State::Error );
        return false;
    }
    if ( state_ == State::Connected || connecting_ ) {
        return true;
    }
    if ( pendingClearGeneration_.has_value() && pendingClearRequestId_.has_value() ) {
        restartAfterClear_ = true;
        lastError_.clear();
        return true;
    }
    restartAfterClear_ = false;
    lastError_.clear();
    if ( !transport_ && transportFactory_ != nullptr ) {
        transport_ = transportFactory_->create( transportConfigFromSessionData( sessionData_ ) );
        wireTransport();
    }
    if ( !transport_ ) {
        lastError_ = tr( "Live log transport is unavailable." );
        setState( State::Error );
        return false;
    }
    sessionData_.runIntent = klogg::livecapture::RunIntent::Running;
    startTransport();
    return true;
}

void AdbLogcatSource::disconnectSource()
{
    if ( !sessionData_.readOnlyCompatibility ) {
        sessionData_.runIntent = klogg::livecapture::RunIntent::Stopped;
    }
    connecting_ = false;
    restartAfterClear_ = false;
    const auto generation = activeGeneration_;
    activeGeneration_.reset();
    if ( transport_ && generation.has_value() ) {
        transport_->stop( *generation );
    }
    if ( logData_ ) {
        logData_->finishInput();
    }
    setState( State::Disconnected );
}

bool AdbLogcatSource::reconnectSource()
{
    if ( sessionData_.readOnlyCompatibility ) {
        lastError_ = tr( "This compatibility session is read-only." );
        return false;
    }
    if ( controllerRestart_ ) {
        lastError_.clear();
        if ( pendingClearGeneration_.has_value() && pendingClearRequestId_.has_value() ) {
            restartAfterClear_ = true;
            return true;
        }
        controllerRestart_();
        return true;
    }
    disconnectSource();
    return connectSource();
}

bool AdbLogcatSource::clearAndRestart()
{
    if ( sessionData_.readOnlyCompatibility ) {
        lastError_ = tr( "This compatibility session is read-only." );
        return false;
    }
    if ( pendingClearGeneration_.has_value() && pendingClearRequestId_.has_value() ) {
        restartAfterClear_ = true;
        lastError_.clear();
        if ( logData_ ) {
            logData_->clearCapture();
        }
        return true;
    }
    const auto shouldRestart = state_ == State::Connected || connecting_;
    const auto isIosLogStream = sessionData_.sourceType == LiveLogSourceType::IosLogStream;
    if ( controllerStop_ ) {
        controllerStop_();
    }
    else {
        disconnectSource();
    }
    if ( logData_ ) {
        logData_->clearCapture();
    }
    if ( !shouldRestart ) {
        lastError_.clear();
        return true;
    }
    if ( isIosLogStream ) {
        if ( controllerRestart_ ) {
            controllerRestart_();
            return true;
        }
        return connectSource();
    }
    if ( !transport_ ) {
        lastError_ = tr( "Failed to clear logcat buffer" );
        setState( State::Error );
        return false;
    }
    const auto generation = nextGeneration();
    const auto requestId = nextClearRequestId();
    pendingClearGeneration_ = generation;
    pendingClearRequestId_ = requestId;
    restartAfterClear_ = true;
    lastError_.clear();
    transport_->clearRemoteAsync( generation, requestId );
    return true;
}

void AdbLogcatSource::finishClear( Generation generation, ClearRequestId requestId, bool succeeded,
                                   const QString& error )
{
    if ( pendingClearGeneration_ != generation || pendingClearRequestId_ != requestId ) {
        return;
    }
    pendingClearGeneration_.reset();
    pendingClearRequestId_.reset();
    const auto shouldRestart = std::exchange( restartAfterClear_, false );
    if ( !succeeded ) {
        lastError_ = error.isEmpty() ? tr( "Failed to clear logcat buffer" ) : error;
        setState( State::Error );
        Q_EMIT errorOccurred( lastError_ );
        Q_EMIT clearFailed( lastError_ );
        return;
    }
    lastError_.clear();
    if ( shouldRestart ) {
        if ( controllerRestart_ ) {
            controllerRestart_();
        }
        else {
            connectSource();
        }
    }
}

bool AdbLogcatSource::bindOutputFile( const QString& outputPath )
{
    return bindOutputFile( outputPath, LiveLogSaveAnsiMode::Strip );
}

bool AdbLogcatSource::bindOutputFile( const QString& outputPath, LiveLogSaveAnsiMode ansiMode )
{
    if ( !logData_ || !logData_->bindOutputFile( outputPath, ansiMode ) ) {
        return false;
    }
    sessionData_.boundOutputFile = outputPath;
    sessionData_.outputAnsiMode = ansiMode;
    return true;
}

void AdbLogcatSource::deleteCaptureFiles()
{
    if ( logData_ ) {
        logData_->deleteCaptureFiles();
    }
}

const AdbLogcatSessionData& AdbLogcatSource::sessionData() const { return sessionData_; }
AdbLogcatSource::State AdbLogcatSource::state() const { return state_; }
QString AdbLogcatSource::lastError() const { return lastError_; }
bool AdbLogcatSource::isTransportAvailable() const { return transport_ != nullptr; }
bool AdbLogcatSource::isReadOnlyCompatibility() const { return sessionData_.readOnlyCompatibility; }

void AdbLogcatSource::setControllerCallbacks( BytesCallback bytes, StateCallback state,
                                              FailureCallback failure, ControlCallback stop,
                                              ControlCallback restart )
{
    controllerBytes_ = std::move( bytes );
    controllerState_ = std::move( state );
    controllerFailure_ = std::move( failure );
    controllerStop_ = std::move( stop );
    controllerRestart_ = std::move( restart );
}

void AdbLogcatSource::invalidateTransportGeneration( Generation generation )
{
    Q_UNUSED( generation );
}

void AdbLogcatSource::cancelTransport( Generation generation )
{
    if ( activeGeneration_ != generation ) {
        return;
    }
    activeGeneration_.reset();
    connecting_ = false;
    if ( transport_ ) {
        transport_->stop( generation );
    }
    if ( logData_ ) {
        logData_->finishInput();
    }
    setState( State::Disconnected );
}

void AdbLogcatSource::openTransport( Generation generation,
                                     const LiveSourceTransportConfig& config )
{
    if ( sessionData_.readOnlyCompatibility || transportFactory_ == nullptr ) {
        const auto error = klogg::livecapture::LiveSourceError{
            klogg::livecapture::ErrorCategory::Configuration, "live-transport-unavailable",
            klogg::livecapture::ErrorScope::Stream, klogg::livecapture::RetryPolicy::Never,
            "The live log transport is unavailable.",
            "The session is read-only or has no transport factory." };
        lastError_ = QString::fromStdString( error.message );
        setState( State::Error );
        if ( controllerFailure_ ) {
            controllerFailure_( generation, error );
        }
        return;
    }

    activeGeneration_.reset();
    connecting_ = false;
    retireTransport();
    transport_ = transportFactory_->create( config );
    wireTransport();
    if ( !transport_ ) {
        const auto error = klogg::livecapture::LiveSourceError{
            klogg::livecapture::ErrorCategory::Configuration, "live-transport-create-failed",
            klogg::livecapture::ErrorScope::Stream, klogg::livecapture::RetryPolicy::Never,
            "The live log transport could not be created.",
            "The typed transport configuration was rejected." };
        lastError_ = QString::fromStdString( error.message );
        setState( State::Error );
        if ( controllerFailure_ ) {
            controllerFailure_( generation, error );
        }
        return;
    }
    activeGeneration_ = generation;
    connecting_ = true;
    lastError_.clear();
    transport_->start( generation );
}

void AdbLogcatSource::appendTransportBytes( Generation generation, const QByteArray& bytes )
{
    if ( activeGeneration_ == generation && logData_ ) {
        logData_->appendUtf8( bytes );
    }
}

void AdbLogcatSource::setState( State state )
{
    if ( state_ == state ) { return; }
    state_ = state;
    Q_EMIT stateChanged( state_ );
}

void AdbLogcatSource::setStateFromTransport( Generation generation,
                                             LiveSourceTransport::State state )
{
    if ( controllerState_ ) { controllerState_( generation, state ); }
    switch ( state ) {
    case LiveSourceTransport::State::Connected:
        connecting_ = false;
        setState( State::Connected );
        break;
    case LiveSourceTransport::State::Error: {
        connecting_ = false;
        if ( logData_ ) { logData_->finishInput(); }
        auto structured = transport_ ? transport_->lastStructuredError() : std::nullopt;
        if ( transport_ ) { lastError_ = transport_->lastError(); }
        setState( State::Error );
        if ( controllerFailure_ ) {
            controllerFailure_( generation, structured.value_or( klogg::livecapture::LiveSourceError{
                klogg::livecapture::ErrorCategory::Stream, "live-stream-failed",
                klogg::livecapture::ErrorScope::Stream, klogg::livecapture::RetryPolicy::Backoff,
                lastError_.isEmpty() ? "The live stream failed." : lastError_.toStdString(),
                lastError_.toStdString() } ) );
        }
        break;
    }
    case LiveSourceTransport::State::Connecting:
        connecting_ = true;
        setState( State::Disconnected );
        break;
    case LiveSourceTransport::State::Disconnected:
        connecting_ = false;
        if ( logData_ ) { logData_->finishInput(); }
        setState( State::Disconnected );
        break;
    }
}

void AdbLogcatSource::setCaptureLimits( qint64 rollingMaxFileSize, int rollingBackupCount,
                                        qint64 maxTotalLines )
{
    if ( logData_ ) {
        CaptureStore::Limits limits;
        limits.rollingMaxFileSize = rollingMaxFileSize;
        limits.rollingBackupCount = rollingBackupCount;
        limits.maxTotalLines = maxTotalLines;
        logData_->setCaptureLimits( std::move( limits ) );
    }
}
