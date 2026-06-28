#include "adblogcatsource.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QRandomGenerator>

#include "adbprocesstransport.h"
#include "capturestore.h"
#include "ioslogprocesstransport.h"
#include "log.h"
#include "livesourcetransport.h"
#include "streaminglogdata.h"

namespace {
LiveLogSourceType sourceTypeFromString( const QString& sourceType )
{
    if ( sourceType == AdbLogcatSessionData::persistedSourceType(
                           LiveLogSourceType::IosLogStream ) ) {
        return LiveLogSourceType::IosLogStream;
    }

    return LiveLogSourceType::AdbLogcat;
}

std::unique_ptr<LiveSourceTransport> makeTransport( const AdbLogcatSessionData& sessionData )
{
    if ( sessionData.sourceType == LiveLogSourceType::IosLogStream ) {
        return std::make_unique<IosLogProcessTransport>( sessionData.adbExecutable,
                                                         sessionData.deviceSerial,
                                                         sessionData.extraArgs,
                                                         sessionData.ansiOutputEnabled );
    }

    return std::make_unique<AdbProcessTransport>( sessionData.adbExecutable,
                                                  sessionData.deviceSerial,
                                                  sessionData.extraArgs,
                                                  sessionData.ansiOutputEnabled );
}

QString iosDeviceNameOnly( const QString& deviceDescription, const QString& deviceSerial )
{
    const auto label = deviceDescription.trimmed();
    if ( label.isEmpty() ) {
        return deviceSerial;
    }

    const auto serialOffset = deviceSerial.isEmpty() ? -1 : label.indexOf( deviceSerial );
    if ( serialOffset > 0 ) {
        return label.left( serialOffset ).trimmed();
    }

    return label;
}
} // namespace

QString AdbLogcatSessionData::displayName() const
{
    if ( sourceType == LiveLogSourceType::IosLogStream ) {
        return iosDeviceNameOnly( deviceDescription, deviceSerial );
    }

    return deviceDescription.isEmpty() ? deviceSerial : deviceDescription;
}

QString AdbLogcatSessionData::documentId() const
{
    const auto scheme = sourceType == LiveLogSourceType::IosLogStream ? QStringLiteral( "ios-log" )
                                                                      : QStringLiteral( "adb" );
    return QStringLiteral( "%1://%2" ).arg( scheme, captureId );
}

QString AdbLogcatSessionData::associatedPath() const
{
    return boundOutputFile;
}

QString AdbLogcatSessionData::persistedSourceType() const
{
    return persistedSourceType( sourceType );
}

bool AdbLogcatSessionData::isValid() const
{
    return !captureId.isEmpty();
}

QString AdbLogcatSessionData::persistedSourceType( LiveLogSourceType sourceType )
{
    switch ( sourceType ) {
    case LiveLogSourceType::IosLogStream:
        return QStringLiteral( "ios_log_stream" );
    case LiveLogSourceType::AdbLogcat:
        return QStringLiteral( "adb_logcat" );
    }

    return QStringLiteral( "adb_logcat" );
}

bool AdbLogcatSessionData::isPersistedSourceType( const QString& sourceType )
{
    return sourceType == persistedSourceType( LiveLogSourceType::AdbLogcat )
           || sourceType == persistedSourceType( LiveLogSourceType::IosLogStream );
}

QJsonObject AdbLogcatSessionData::toJson() const
{
    return QJsonObject{
        { QStringLiteral( "sourceType" ), persistedSourceType() },
        { QStringLiteral( "adbExecutable" ), adbExecutable },
        { QStringLiteral( "deviceSerial" ), deviceSerial },
        { QStringLiteral( "deviceDescription" ), deviceDescription },
        { QStringLiteral( "extraArgs" ), extraArgs },
        { QStringLiteral( "captureId" ), captureId },
        { QStringLiteral( "boundOutputFile" ), boundOutputFile },
        { QStringLiteral( "ansiOutputEnabled" ), ansiOutputEnabled },
        { QStringLiteral( "autoReconnectEnabled" ), autoReconnectEnabled },
        { QStringLiteral( "maxReconnectAttempts" ), maxReconnectAttempts },
        { QStringLiteral( "captureMaxFileSize" ), static_cast<qint64>( captureMaxFileSize ) },
        { QStringLiteral( "captureBackupCount" ), captureBackupCount },
    };
}

AdbLogcatSessionData AdbLogcatSessionData::fromJson( const QString& json )
{
    const auto jsonObject = QJsonDocument::fromJson( json.toUtf8() ).object();
    AdbLogcatSessionData data;
    data.adbExecutable = jsonObject.value( QStringLiteral( "adbExecutable" ) ).toString();
    data.deviceSerial = jsonObject.value( QStringLiteral( "deviceSerial" ) ).toString();
    data.deviceDescription = jsonObject.value( QStringLiteral( "deviceDescription" ) ).toString();
    data.extraArgs = jsonObject.value( QStringLiteral( "extraArgs" ) ).toString();
    data.captureId = jsonObject.value( QStringLiteral( "captureId" ) ).toString();
    data.boundOutputFile = jsonObject.value( QStringLiteral( "boundOutputFile" ) ).toString();
    data.sourceType
        = sourceTypeFromString( jsonObject.value( QStringLiteral( "sourceType" ) ).toString() );
    data.ansiOutputEnabled
        = jsonObject.value( QStringLiteral( "ansiOutputEnabled" ) ).toBool( false );
    data.autoReconnectEnabled
        = jsonObject.value( QStringLiteral( "autoReconnectEnabled" ) ).toBool( false );
    data.maxReconnectAttempts
        = jsonObject.value( QStringLiteral( "maxReconnectAttempts" ) ).toInt( 0 );
    data.captureMaxFileSize
        = jsonObject.value( QStringLiteral( "captureMaxFileSize" ) ).toVariant().toLongLong();
    data.captureBackupCount
        = jsonObject.value( QStringLiteral( "captureBackupCount" ) ).toInt( 0 );
    return data;
}

AdbLogcatSource::AdbLogcatSource( AdbLogcatSessionData sessionData,
                                  std::shared_ptr<StreamingLogData> logData, QObject* parent )
    : QObject( parent )
    , sessionData_( std::move( sessionData ) )
    , logData_( std::move( logData ) )
    , transport_( makeTransport( sessionData_ ) )
{
    reconnectTimer_.setSingleShot( true );
    connect( &reconnectTimer_, &QTimer::timeout, this, &AdbLogcatSource::attemptReconnect );

    connect( transport_.get(), &LiveSourceTransport::bytesReceived, this,
             [ this ]( const QByteArray& data ) {
                 if ( logData_ ) {
                     logData_->appendUtf8( data );
                 }
                 // First stdout data after a reconnect proves the connection
                 // is truly working — reset the backoff counter.  Reconnect
                 // progress is surfaced via the status bar only; nothing is
                 // written to the log view (fully silent retries).
                 if ( !reconnectionProven_ && reconnectAttempt_ > 0 ) {
                     reconnectionProven_ = true;
                     LOG_INFO << "Auto-reconnect succeeded after " << reconnectAttempt_
                              << " attempt(s)";
                     reconnectAttempt_ = 0;
                 }
             } );
    connect( transport_.get(), &LiveSourceTransport::stateChanged, this,
             [ this ]( LiveSourceTransport::State state ) { setStateFromTransport( state ); } );
    connect( transport_.get(), &LiveSourceTransport::errorOccurred, this, [ this ]( const QString& error ) {
        if ( error.isEmpty() ) {
            return;
        }
        lastError_ = error;
        LOG_WARNING << "adb logcat transport error " << error;
        Q_EMIT errorOccurred( lastError_ );
    } );
}

AdbLogcatSource::~AdbLogcatSource()
{
    reconnectTimer_.stop();
    disconnectSource();
}

bool AdbLogcatSource::connectSource()
{
    if ( !transport_ ) {
        lastError_ = tr( "ADB transport is unavailable" );
        setState( State::Error );
        return false;
    }

    if ( state_ == State::Connected ) {
        return true;
    }

    manualDisconnect_ = false;

    // A fresh (re)connect starts a new reconnect cycle: reset the attempt
    // counter and cancel any pending auto-reconnect. Without this, a manual
    // Reconnect after auto-reconnect exhaustion left reconnectAttempt_ at its
    // stale high value, suppressing further retries.
    reconnectTimer_.stop();
    reconnectAttempt_ = 0;

    // connectTransport() handles device-not-found errors directly.
    // The isDeviceAvailable() pre-check was removed because it runs a
    // blocking subprocess (adb devices / pymobiledevice3 usbmux list)
    // on the UI thread for up to 8 seconds.
    if ( !transport_->connectTransport() ) {
        lastError_ = transport_->lastError();
        return false;
    }

    lastError_.clear();
    return true;
}

void AdbLogcatSource::disconnectSource()
{
    if ( !transport_ ) {
        setState( State::Disconnected );
        return;
    }

    manualDisconnect_ = true;
    reconnectTimer_.stop();
    transport_->disconnectTransport();
}

bool AdbLogcatSource::reconnectSource()
{
    disconnectSource();
    if ( logData_ ) {
        const auto marker = QStringLiteral( "----- reconnected %1 -----\n" )
                                .arg( QDateTime::currentDateTime().toString( Qt::ISODate ) );
        logData_->appendUtf8( marker.toUtf8() );
    }
    manualDisconnect_ = false;
    return connectSource();
}

bool AdbLogcatSource::clearAndRestart()
{
    const auto wasConnected = state_ == State::Connected;
    const auto isIosLogStream = sessionData_.sourceType == LiveLogSourceType::IosLogStream;
    disconnectSource();

    bool remoteClearFailed = false;
    if ( !isIosLogStream && wasConnected ) {
        QString error;
        if ( !transport_ || !transport_->clearRemote( &error ) ) {
            lastError_ = error.isEmpty() ? tr( "Failed to clear logcat buffer" ) : error;
            setState( State::Error );
            remoteClearFailed = true;
        }
    }

    if ( logData_ ) {
        logData_->clearCapture();
    }

    if ( remoteClearFailed ) {
        return false;
    }

    if ( wasConnected ) {
        const auto restarted = connectSource();
        return restarted || isIosLogStream;
    }
    lastError_.clear();
    return true;
}

bool AdbLogcatSource::bindOutputFile( const QString& outputPath )
{
    return bindOutputFile( outputPath, LiveLogSaveAnsiMode::Strip );
}

bool AdbLogcatSource::bindOutputFile( const QString& outputPath, LiveLogSaveAnsiMode ansiMode )
{
    if ( !logData_ ) {
        return false;
    }

    if ( !logData_->bindOutputFile( outputPath, ansiMode ) ) {
        return false;
    }

    sessionData_.boundOutputFile = outputPath;
    return true;
}

void AdbLogcatSource::deleteCaptureFiles()
{
    if ( logData_ ) {
        logData_->deleteCaptureFiles();
    }
}

const AdbLogcatSessionData& AdbLogcatSource::sessionData() const
{
    return sessionData_;
}

AdbLogcatSource::State AdbLogcatSource::state() const
{
    return state_;
}

QString AdbLogcatSource::lastError() const
{
    return lastError_;
}

void AdbLogcatSource::setState( State state )
{
    if ( state_ == state ) {
        return;
    }
    state_ = state;
    Q_EMIT stateChanged( state_ );
}

void AdbLogcatSource::setStateFromTransport( LiveSourceTransport::State state )
{
    switch ( state ) {
    case LiveSourceTransport::State::Connected:
        reconnectTimer_.stop();
        // The connection is not yet proven — we wait for the first stdout
        // data before declaring success (see bytesReceived handler above).
        // If the process dies without producing data, the failure is surfaced
        // via the status bar / lastError_ (see setStateFromTransport(Error)).
        reconnectionProven_ = false;
        setState( State::Connected );
        break;
    case LiveSourceTransport::State::Error:
        // Reconnect failures are surfaced via the status bar / lastError_
        // only — nothing is written to the log view (fully silent retries).
        if ( logData_ ) {
            logData_->finishInput();
        }
        reconnectionProven_ = false;
        if ( transport_ ) {
            lastError_ = transport_->lastError();
        }
        setState( State::Error );
        if ( !manualDisconnect_ && autoReconnectEnabled_ ) {
            scheduleReconnect();
        }
        break;
    case LiveSourceTransport::State::Connecting:
        setState( State::Disconnected );
        break;
    case LiveSourceTransport::State::Disconnected:
        reconnectionProven_ = false;
        if ( logData_ ) {
            logData_->finishInput();
        }
        setState( State::Disconnected );
        break;
    }
}

void AdbLogcatSource::setAutoReconnectEnabled( bool enabled )
{
    autoReconnectEnabled_ = enabled;
    if ( !enabled ) {
        reconnectTimer_.stop();
        reconnectAttempt_ = 0;
    }
}

void AdbLogcatSource::setAutoReconnectMaxAttempts( int maxAttempts )
{
    autoReconnectMaxAttempts_ = maxAttempts;
}

bool AdbLogcatSource::isAutoReconnectActive() const
{
    return reconnectTimer_.isActive();
}

int AdbLogcatSource::reconnectAttempt() const
{
    return reconnectAttempt_;
}

void AdbLogcatSource::cancelAutoReconnect()
{
    reconnectTimer_.stop();
    reconnectAttempt_ = 0;
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

void AdbLogcatSource::scheduleReconnect()
{
    if ( autoReconnectMaxAttempts_ > 0 && reconnectAttempt_ >= autoReconnectMaxAttempts_ ) {
        // Reconnect exhaustion is surfaced via the status bar (state becomes
        // Error with lastError_) — nothing is written to the log view.
        LOG_INFO << "Auto-reconnect max attempts (" << autoReconnectMaxAttempts_ << ") reached, giving up";
        return;
    }

    // Exponential backoff with ±20% jitter
    const auto baseDelay
        = qMin( InitialReconnectDelayMs * ( 1 << qMin( reconnectAttempt_, 15 ) ),
                MaxReconnectDelayMs );
    const auto jitter = QRandomGenerator::global()->bounded( baseDelay / 5 ); // ±20%
    const auto delay = baseDelay + jitter - ( baseDelay / 10 );

    LOG_INFO << "Scheduling auto-reconnect attempt " << ( reconnectAttempt_ + 1 ) << " in " << delay
             << "ms";
    reconnectTimer_.start( delay );
    Q_EMIT reconnectAttemptStarted( reconnectAttempt_ + 1 );
}

void AdbLogcatSource::attemptReconnect()
{
    if ( !autoReconnectEnabled_ ) {
        return;
    }

    ++reconnectAttempt_;
    LOG_INFO << "Auto-reconnect attempt " << reconnectAttempt_;

    if ( !transport_ ) {
        LOG_WARNING << "Auto-reconnect: transport unavailable";
        return;
    }

    // Reset manualDisconnect_ so setStateFromTransport() can trigger
    // another scheduleReconnect() on failure.
    manualDisconnect_ = false;

    // Use the non-blocking async path: connectTransportAsync() starts the
    // subprocess and sets up signal-driven startup detection (grace timer +
    // error/finished handlers) instead of blocking the GUI thread for up to
    // 3.25 seconds.  The result (Connected or Error) arrives via stateChanged
    // → setStateFromTransport, which handles the reconnect cycle.
    transport_->connectTransportAsync();
    LOG_INFO << "Auto-reconnect attempt " << reconnectAttempt_ << " started (async)";
}

