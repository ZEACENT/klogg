#include "adblogcatsource.h"

#include <limits>
#include <algorithm>
#include <utility>

#include <QPointer>
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
    persistenceRetryTimer_.setParent( this );
    persistenceRetryTimer_.setObjectName( QStringLiteral( "liveCapturePersistenceRetry" ) );
    persistenceRetryTimer_.setTimerType( Qt::PreciseTimer );
    persistenceRetryTimer_.setSingleShot( true );
    connect( &persistenceRetryTimer_, &QTimer::timeout, this, [ this ] {
        if ( logData_ && persistenceSchedulingArmed_ ) {
            logData_->retryPersistence( 8 );
            schedulePersistenceRetry();
        }
    } );
    if ( logData_ ) {
        connect( logData_.get(), &StreamingLogData::captureOutputChanged, this,
                 &AdbLogcatSource::captureOutputChanged );
        connect( logData_.get(), &StreamingLogData::capturePersistenceChanged, this,
            [ this ]( bool healthy, CaptureStore::PersistenceFailure error ) {
                schedulePersistenceRetry();
                Q_EMIT capturePersistenceChanged( healthy, error );
            } );
    }
}

void AdbLogcatSource::schedulePersistenceRetry()
{
    if ( !logData_ || !persistenceSchedulingArmed_ ) { return; }
    const auto state = logData_->persistenceState();
    if ( state.retryableSegments == 0 ) {
        // A normal partial record and the appendable segment tail are not ready
        // to persist. EOF, segment sealing, memory pressure, or an actual failed
        // spill makes work retryable without creating one file per quiet append.
        persistenceRetryTimer_.stop();
        return;
    }
    const auto delay = std::clamp<qint64>( state.retryAfterMs.value_or( 25 ), 1,
                                         std::numeric_limits<int>::max() );
    persistenceRetryTimer_.start( static_cast<int>( delay ) );
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
                 if ( activeGeneration_ != generation
                      && !( retiringGeneration_ == generation
                            && retiringDisposition_ == klogg::livecapture::StopDisposition::SettleAccepted ) ) {
                     return;
                 }
                 if ( controllerBytes_ ) {
                     if ( !deliverySettlement_
                          || deliverySettlement_->generation != generation
                          || deliverySettlement_->producerStopped ) {
                         return;
                     }
                     const auto sequence = ++deliverySettlement_->lastOfferedSequence;
                     const QPointer<AdbLogcatSource> guard( this );
                     auto settled = [ guard, generation, sequence ] {
                         if ( guard ) { guard->settleOfferedDelivery( generation, sequence ); }
                     };
                     try { controllerBytes_( generation, data, settled ); }
                     catch ( ... ) {
                         settled();
                     }
                 }
                 else if ( logData_ ) {
                     appendTransportBytes( generation, data );
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
                 if ( activeGeneration_ != generation || error.isEmpty()
                      || reportedErrorGeneration_ == generation ) {
                     return;
                 }
                 reportedErrorGeneration_ = generation;
                 lastError_ = error;
                 LOG_WARNING << "live log transport error " << error;
                 Q_EMIT errorOccurred( lastError_ );
             } );
    connect( transport_.get(), &LiveSourceTransport::stopped, this,
        [ this ]( Generation generation, quint64 discarded ) {
            if ( activeGeneration_ == generation ) {
                // A transport can finish its own terminal retirement before the
                // controller or a later user action requests Stop. Adopt that real
                // producer acknowledgement instead of losing the only completion.
                activeGeneration_.reset();
                retiringGeneration_ = generation;
                retiringDisposition_ = klogg::livecapture::StopDisposition::SettleAccepted;
            }
            if ( retiringGeneration_ != generation || !deliverySettlement_
                 || deliverySettlement_->generation != generation ) {
                return;
            }
            deliverySettlement_->producerStopped = true;
            deliverySettlement_->discardedBytes = discarded;
            completeRetirementIfSettled( generation );
        } );
    connect( transport_.get(), &LiveSourceTransport::clearRemoteFinished, this,
             &AdbLogcatSource::finishClear );
}

void AdbLogcatSource::retireTransport()
{
    reportedErrorGeneration_.reset();
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
    persistenceSchedulingArmed_ = true;
    const auto generation = nextGeneration();
    reportedErrorGeneration_.reset();
    beginDeliveryGeneration( generation );
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
    if ( retiringGeneration_ ) {
        restartAfterStop_ = true;
        return true;
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
    restartAfterStop_ = false;
    reportedErrorGeneration_.reset();
    const auto generation = activeGeneration_ ? activeGeneration_ : retiringGeneration_;
    if ( generation ) { cancelTransport( *generation ); }
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
    const auto shouldRestart = state_ == State::Connected || connecting_ || retiringGeneration_.has_value();
    if ( activeGeneration_ || retiringGeneration_ ) {
        clearAfterStop_ = true;
        restartAfterStop_ = shouldRestart;
        if ( controllerStop_ ) { controllerStop_(); }
        else {
            const auto generation = activeGeneration_ ? *activeGeneration_ : *retiringGeneration_;
            cancelTransport( generation );
        }
        return true;
    }
    return performClear( shouldRestart );
}

bool AdbLogcatSource::performClear( bool shouldRestart )
{
    const auto isIosLogStream = sessionData_.sourceType == LiveLogSourceType::IosLogStream;
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
    sessionData_.boundOutputFile = logData_->boundOutputFile();
    sessionData_.outputAnsiMode = ansiMode;
    return true;
}

bool AdbLogcatSource::hasActiveOutputBinding( const QString& outputPath,
                                              LiveLogSaveAnsiMode ansiMode ) const
{
    return logData_ && logData_->hasActiveOutputBinding( outputPath, ansiMode );
}

bool AdbLogcatSource::synchronizeOutputBinding( LiveLogSaveAnsiMode ansiMode )
{
    if ( !logData_ || logData_->boundOutputFile().isEmpty() ) {
        return false;
    }
    sessionData_.boundOutputFile = logData_->boundOutputFile();
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
    if ( activeGeneration_ && activeGeneration_ != generation ) {
        retiringGeneration_ = activeGeneration_;
        activeGeneration_.reset();
        retiringDisposition_ = klogg::livecapture::StopDisposition::SettleAccepted;
    }
}

void AdbLogcatSource::setStoppedCallback( StoppedCallback callback )
{
    stoppedCallback_ = std::move( callback );
}

void AdbLogcatSource::cancelTransport(
    Generation generation, klogg::livecapture::StopDisposition disposition )
{
    if ( activeGeneration_ == generation ) {
        retiringGeneration_ = generation;
        activeGeneration_.reset();
    }
    if ( retiringGeneration_ != generation ) {
        // Startup may have no transport at all: this owner has nothing to release.
        if ( !retiringGeneration_ && stoppedCallback_ ) { stoppedCallback_( generation, 0u ); }
        return;
    }
    const auto effectiveDisposition
        = stopRequested_
                  && retiringDisposition_ == klogg::livecapture::StopDisposition::DiscardPending
              ? klogg::livecapture::StopDisposition::DiscardPending
              : disposition;
    const auto dispositionChanged = effectiveDisposition != retiringDisposition_;
    retiringDisposition_ = effectiveDisposition;
    if ( stopRequested_ ) {
        if ( dispositionChanged && transport_ ) {
            transport_->requestStop( generation, effectiveDisposition );
        }
        return;
    }
    stopRequested_ = true;
    reportedErrorGeneration_.reset();
    connecting_ = false;
    if ( transport_ ) { transport_->requestStop( generation, disposition ); }
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
    reportedErrorGeneration_.reset();
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
    persistenceSchedulingArmed_ = true;
    beginDeliveryGeneration( generation );
    activeGeneration_ = generation;
    connecting_ = true;
    lastError_.clear();
    transport_->start( generation );
}

klogg::livecapture::CaptureDeliveryResult AdbLogcatSource::appendTransportBytes(
    Generation generation, const QByteArray& bytes )
{
    using namespace klogg::livecapture;
    CaptureDeliveryResult result;
    if ( !logData_ || ( activeGeneration_ != generation && retiringGeneration_ != generation ) ) {
        result.disposition = DeliveryDisposition::Rejected;
        return result;
    }
    try {
        result = mapCaptureOutcome( logData_->appendUtf8( bytes ) );
    }
    catch ( ... ) {
        result.disposition = DeliveryDisposition::PartialUnknown;
        result.outputBytes.reset();
        result.failureCode = "capture-outcome-unknown";
    }
    schedulePersistenceRetry();
    if ( !controllerBytes_ && ( result.failureCode || result.notificationFailed
                               || result.disposition != DeliveryDisposition::Complete ) ) {
        const LiveSourceError error{ ErrorCategory::Capture,
            result.failureCode.value_or( "capture-notification-failed" ), ErrorScope::Capture,
            RetryPolicy::Never, "The live capture could not accept all input safely.", {} };
        lastError_ = QString::fromStdString( error.message );
        setState( State::Error );
        if ( controllerFailure_ ) { controllerFailure_( generation, error ); }
    }
    return result;
}

klogg::livecapture::CaptureDeliveryResult AdbLogcatSource::mapCaptureOutcome(
    const CaptureStore::AppendResult& outcome )
{
    using namespace klogg::livecapture;
    CaptureDeliveryResult result;
        switch ( outcome.disposition ) {
        case CaptureStore::AppendDisposition::Complete:
            result.disposition = DeliveryDisposition::Complete; break;
        case CaptureStore::AppendDisposition::RejectedUnchanged:
            result.disposition = DeliveryDisposition::Rejected; break;
        case CaptureStore::AppendDisposition::PartialKnown:
            result.disposition = DeliveryDisposition::PartialKnown; break;
        case CaptureStore::AppendDisposition::PartialUnknown:
            result.disposition = DeliveryDisposition::PartialUnknown; break;
        }
        result.acceptedBytes = static_cast<std::uint64_t>( outcome.acceptedBytes );
        result.committedBytes = static_cast<std::uint64_t>( outcome.committedBytes );
        result.committedLines = outcome.committedLines.get();
        result.outputBytes = outcome.outputBytes
            ? std::optional<std::uint64_t>{ static_cast<std::uint64_t>( *outcome.outputBytes ) }
            : std::nullopt;
        result.notificationFailed = outcome.notificationFailed;
        if ( outcome.failure ) {
            switch ( *outcome.failure ) {
            case CaptureStore::CaptureFailure::Capacity: result.failureCode = "capture-capacity"; break;
            case CaptureStore::CaptureFailure::Directory: result.failureCode = "capture-directory"; break;
            case CaptureStore::CaptureFailure::SegmentIds: result.failureCode = "capture-segment-ids"; break;
            case CaptureStore::CaptureFailure::Allocation: result.failureCode = "capture-allocation"; break;
            case CaptureStore::CaptureFailure::Unexpected: result.failureCode = "capture-unexpected"; break;
            }
        }
    return result;
}

void AdbLogcatSource::setFinalizedCallback( FinalizedCallback callback )
{
    finalizedCallback_ = std::move( callback );
}

void AdbLogcatSource::beginDeliveryGeneration( Generation generation )
{
    deliverySettlement_ = DeliverySettlementToken{};
    deliverySettlement_->generation = generation;
}

void AdbLogcatSource::settleOfferedDelivery( Generation generation, DeliverySequence sequence )
{
    if ( !deliverySettlement_ || deliverySettlement_->generation != generation
         || sequence <= deliverySettlement_->settledThroughSequence
         || sequence > deliverySettlement_->lastOfferedSequence
         || !deliverySettlement_->settledOutOfOrder.insert( sequence ).second ) {
        return;
    }
    while ( deliverySettlement_->settledThroughSequence
                < deliverySettlement_->lastOfferedSequence
            && deliverySettlement_->settledOutOfOrder.erase(
                   deliverySettlement_->settledThroughSequence + 1u )
                   != 0u ) {
        ++deliverySettlement_->settledThroughSequence;
    }
    completeRetirementIfSettled( generation );
}

void AdbLogcatSource::completeRetirementIfSettled( Generation generation )
{
    if ( retiringGeneration_ != generation || !deliverySettlement_
         || deliverySettlement_->generation != generation || deliverySettlement_->completing
         || !deliverySettlement_->producerStopped
         || deliverySettlement_->settledThroughSequence
                != deliverySettlement_->lastOfferedSequence ) {
        return;
    }

    deliverySettlement_->completing = true;
    const auto discarded = deliverySettlement_->discardedBytes;
    const bool preserveTerminalError
        = state_ == State::Error
          && retiringDisposition_ == klogg::livecapture::StopDisposition::SettleAccepted;
    const QPointer<AdbLogcatSource> guard( this );
    // Only real producer completion plus settlement of every registered delivery
    // seals partial input. Arbitrary stale callbacks cannot register after stopped.
    finalizeInput( generation );
    if ( !guard ) { return; }

    guard->retiringGeneration_.reset();
    guard->stopRequested_ = false;
    guard->deliverySettlement_.reset();
    const auto callback = guard->stoppedCallback_;
    const bool clear = std::exchange( guard->clearAfterStop_, false );
    const bool restart = std::exchange( guard->restartAfterStop_, false );
    if ( !preserveTerminalError ) { guard->setState( State::Disconnected ); }
    if ( !guard ) { return; }
    if ( callback ) { callback( generation, discarded ); }
    if ( !guard ) { return; }
    if ( clear ) { guard->performClear( restart ); }
    else if ( restart ) {
        if ( guard->controllerRestart_ ) { guard->controllerRestart_(); }
        else { guard->connectSource(); }
    }
}

void AdbLogcatSource::finalizeInput( Generation generation )
{
    if ( !logData_ ) { return; }
    klogg::livecapture::CaptureDeliveryResult result;
    try { result = mapCaptureOutcome( logData_->finishInput() ); }
    catch ( ... ) {
        result.disposition = klogg::livecapture::DeliveryDisposition::PartialUnknown;
        result.outputBytes.reset();
        result.failureCode = "capture-finalization-unknown";
    }
    schedulePersistenceRetry();
    if ( finalizedCallback_ ) { finalizedCallback_( generation, result ); }
}

bool AdbLogcatSource::isInputTerminated() const
{
    return !activeGeneration_ && !retiringGeneration_;
}

std::optional<CaptureStore::PersistenceResult> AdbLogcatSource::persistForClose( int maxSegments )
{
    if ( !isInputTerminated() ) { return std::nullopt; }
    if ( !logData_ ) { return CaptureStore::PersistenceResult{}; }
    // This is one bounded preparation turn, not an async export or a durability promise.
    return logData_->persistCapture( maxSegments );
}

std::optional<CaptureOutputError> AdbLogcatSource::flushOutputForClose()
{
    if ( !isInputTerminated() || !logData_ ) {
        return CaptureOutputError::Flush;
    }
    return logData_->flushOutputForClose();
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
    if ( state == LiveSourceTransport::State::Error ) {
        const auto structured = transport_ ? transport_->lastStructuredError() : std::nullopt;
        const auto diagnostic = transport_ ? transport_->lastError() : QString{};
        const auto failure = structured.value_or( klogg::livecapture::LiveSourceError{
            klogg::livecapture::ErrorCategory::Stream, "live-stream-failed",
            klogg::livecapture::ErrorScope::Stream, klogg::livecapture::RetryPolicy::Backoff,
            diagnostic.isEmpty() ? "The live stream failed." : diagnostic.toStdString(),
            diagnostic.toStdString() } );
        const auto terminalText
            = diagnostic.isEmpty() ? QString::fromStdString( failure.message ) : diagnostic;
        QPointer<AdbLogcatSource> guard( this );

        connecting_ = false;
        if ( !guard ) { return; }

        guard->lastError_ = terminalText;
        guard->setState( State::Error );
        if ( !guard ) { return; }

        if ( guard->reportedErrorGeneration_ != generation && !terminalText.isEmpty() ) {
            guard->reportedErrorGeneration_ = generation;
            LOG_WARNING << "live log transport error " << terminalText;
            Q_EMIT guard->errorOccurred( terminalText );
        }
        if ( !guard || guard->activeGeneration_ != generation ) { return; }

        const auto stateCallback = guard->controllerState_;
        if ( stateCallback ) { stateCallback( generation, state ); }
        if ( !guard || guard->activeGeneration_ != generation ) { return; }

        const auto failureCallback = guard->controllerFailure_;
        if ( failureCallback ) { failureCallback( generation, failure ); }
        return;
    }

    if ( controllerState_ ) { controllerState_( generation, state ); }
    switch ( state ) {
    case LiveSourceTransport::State::Connected:
        connecting_ = false;
        setState( State::Connected );
        break;
    case LiveSourceTransport::State::Error:
        break;
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
