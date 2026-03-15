#include "adblogcatsource.h"

#include <QDateTime>
#include <QJsonDocument>

#include "adbprocesstransport.h"
#include "log.h"
#include "livesourcetransport.h"
#include "streaminglogdata.h"

QString AdbLogcatSessionData::displayName() const
{
    return deviceDescription.isEmpty() ? deviceSerial : deviceDescription;
}

QString AdbLogcatSessionData::documentId() const
{
    return QStringLiteral( "adb://%1" ).arg( captureId );
}

QString AdbLogcatSessionData::associatedPath() const
{
    return boundOutputFile;
}

QJsonObject AdbLogcatSessionData::toJson() const
{
    return QJsonObject{
        { QStringLiteral( "adbExecutable" ), adbExecutable },
        { QStringLiteral( "deviceSerial" ), deviceSerial },
        { QStringLiteral( "deviceDescription" ), deviceDescription },
        { QStringLiteral( "extraArgs" ), extraArgs },
        { QStringLiteral( "captureId" ), captureId },
        { QStringLiteral( "boundOutputFile" ), boundOutputFile },
    };
}

AdbLogcatSessionData AdbLogcatSessionData::fromJson( const QString& json )
{
    const auto jsonObject = QJsonDocument::fromJson( json.toUtf8() ).object();
    return AdbLogcatSessionData{
        jsonObject.value( QStringLiteral( "adbExecutable" ) ).toString(),
        jsonObject.value( QStringLiteral( "deviceSerial" ) ).toString(),
        jsonObject.value( QStringLiteral( "deviceDescription" ) ).toString(),
        jsonObject.value( QStringLiteral( "extraArgs" ) ).toString(),
        jsonObject.value( QStringLiteral( "captureId" ) ).toString(),
        jsonObject.value( QStringLiteral( "boundOutputFile" ) ).toString(),
    };
}

AdbLogcatSource::AdbLogcatSource( AdbLogcatSessionData sessionData,
                                  std::shared_ptr<StreamingLogData> logData, QObject* parent )
    : QObject( parent )
    , sessionData_( std::move( sessionData ) )
    , logData_( std::move( logData ) )
    , transport_( std::make_unique<AdbProcessTransport>( sessionData_.adbExecutable,
                                                         sessionData_.deviceSerial,
                                                         sessionData_.extraArgs ) )
{
    connect( transport_.get(), &LiveSourceTransport::bytesReceived, this,
             [ this ]( const QByteArray& data ) {
                 if ( logData_ ) {
                     logData_->appendUtf8( data );
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
    return connectSource();
}

bool AdbLogcatSource::clearAndRestart()
{
    const auto wasConnected = state_ == State::Connected;
    disconnectSource();

    QString error;
    if ( !transport_ || !transport_->clearRemote( &error ) ) {
        lastError_ = error.isEmpty() ? tr( "Failed to clear logcat buffer" ) : error;
        setState( State::Error );
        return false;
    }

    if ( logData_ ) {
        logData_->clearCapture();
    }

    if ( wasConnected ) {
        return connectSource();
    }
    lastError_.clear();
    return true;
}

bool AdbLogcatSource::bindOutputFile( const QString& outputPath )
{
    if ( !logData_ ) {
        return false;
    }

    if ( !logData_->bindOutputFile( outputPath ) ) {
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
        setState( State::Connected );
        break;
    case LiveSourceTransport::State::Error:
        if ( logData_ ) {
            logData_->finishInput();
        }
        if ( transport_ ) {
            lastError_ = transport_->lastError();
        }
        setState( State::Error );
        break;
    case LiveSourceTransport::State::Connecting:
        setState( State::Disconnected );
        break;
    case LiveSourceTransport::State::Disconnected:
        if ( logData_ ) {
            logData_->finishInput();
        }
        setState( State::Disconnected );
        break;
    }
}
