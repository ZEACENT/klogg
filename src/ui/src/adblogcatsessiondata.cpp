#include "adblogcatsessiondata.h"

#include <QJsonDocument>

#include "capturestore.h"

namespace {

LiveLogSourceType sourceTypeFromString( const QString& sourceType )
{
    if ( sourceType
         == AdbLogcatSessionData::persistedSourceType( LiveLogSourceType::IosLogStream ) ) {
        return LiveLogSourceType::IosLogStream;
    }
    return LiveLogSourceType::AdbLogcat;
}

QString iosDeviceNameOnly( const QString& deviceDescription, const QString& deviceSerial )
{
    const auto label = deviceDescription.trimmed();
    if ( label.isEmpty() ) {
        return deviceSerial;
    }
    const auto serialOffset = deviceSerial.isEmpty() ? -1 : label.indexOf( deviceSerial );
    return serialOffset > 0 ? label.left( serialOffset ).trimmed() : label;
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
    return CaptureStore::isValidCaptureId( captureId );
}

QString AdbLogcatSessionData::persistedSourceType( LiveLogSourceType sourceType )
{
    return sourceType == LiveLogSourceType::IosLogStream ? QStringLiteral( "ios_log_stream" )
                                                          : QStringLiteral( "adb_logcat" );
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
        { QStringLiteral( "adbBackend" ), adbBackend == AdbTransportBackend::SmartSocket
                                              ? QStringLiteral( "smart_socket" )
                                              : QStringLiteral( "process" ) },
        { QStringLiteral( "iosBackend" ), iosBackend == IosTransportBackend::Native
                                              ? QStringLiteral( "native" )
                                              : QStringLiteral( "legacy_process" ) },
        { QStringLiteral( "iosUdid" ), QString::fromStdString( iosEndpoint.udid ) },
        { QStringLiteral( "iosConnectionType" ),
          iosEndpoint.connectionType == klogg::livecapture::ios::NativeConnectionType::Network
              ? QStringLiteral( "network" )
              : QStringLiteral( "usb" ) },
        { QStringLiteral( "deviceSerial" ), deviceSerial },
        { QStringLiteral( "deviceDescription" ), deviceDescription },
        { QStringLiteral( "captureId" ), captureId },
        { QStringLiteral( "boundOutputFile" ), boundOutputFile },
        { QStringLiteral( "outputPreserveAnsi" ), outputAnsiMode == LiveLogSaveAnsiMode::Preserve },
        { QStringLiteral( "ansiOutputEnabled" ), ansiOutputEnabled },
        { QStringLiteral( "autoReconnectEnabled" ), autoReconnectEnabled },
        { QStringLiteral( "maxReconnectAttempts" ), maxReconnectAttempts },
        { QStringLiteral( "captureMaxFileSize" ), static_cast<qint64>( captureMaxFileSize ) },
        { QStringLiteral( "captureBackupCount" ), captureBackupCount },
    };
}

AdbLogcatSessionData AdbLogcatSessionData::fromJson( const QString& json )
{
    const auto object = QJsonDocument::fromJson( json.toUtf8() ).object();
    AdbLogcatSessionData data;
    data.adbExecutable = object.value( QStringLiteral( "adbExecutable" ) ).toString();
    data.deviceSerial = object.value( QStringLiteral( "deviceSerial" ) ).toString();
    data.deviceDescription = object.value( QStringLiteral( "deviceDescription" ) ).toString();
    data.extraArgs = object.value( QStringLiteral( "extraArgs" ) ).toString();
    data.captureId = object.value( QStringLiteral( "captureId" ) ).toString();
    data.boundOutputFile = object.value( QStringLiteral( "boundOutputFile" ) ).toString();
    data.outputAnsiMode = object.value( QStringLiteral( "outputPreserveAnsi" ) ).toBool( false )
                              ? LiveLogSaveAnsiMode::Preserve
                              : LiveLogSaveAnsiMode::Strip;
    data.adbBackend = object.value( QStringLiteral( "adbBackend" ) ).toString()
                              == QStringLiteral( "process" )
                          ? AdbTransportBackend::Process
                          : AdbTransportBackend::SmartSocket;
    data.sourceType = sourceTypeFromString( object.value( QStringLiteral( "sourceType" ) ).toString() );
    const auto iosBackend = object.value( QStringLiteral( "iosBackend" ) ).toString();
    if ( iosBackend == QStringLiteral( "native" ) ) {
        data.iosBackend = IosTransportBackend::Native;
    }
    else if ( iosBackend == QStringLiteral( "legacy_process" ) ) {
        data.iosBackend = IosTransportBackend::LegacyProcess;
    }
    data.iosEndpoint.udid
        = object.value( QStringLiteral( "iosUdid" ) ).toString( data.deviceSerial ).toStdString();
    data.iosEndpoint.connectionType
        = object.value( QStringLiteral( "iosConnectionType" ) ).toString()
                  == QStringLiteral( "network" )
              ? klogg::livecapture::ios::NativeConnectionType::Network
              : klogg::livecapture::ios::NativeConnectionType::Usb;
    data.ansiOutputEnabled
        = object.value( QStringLiteral( "ansiOutputEnabled" ) ).toBool( false );
    data.autoReconnectEnabled
        = object.value( QStringLiteral( "autoReconnectEnabled" ) ).toBool( false );
    data.maxReconnectAttempts
        = object.value( QStringLiteral( "maxReconnectAttempts" ) ).toInt( 0 );
    data.captureMaxFileSize
        = object.value( QStringLiteral( "captureMaxFileSize" ) ).toVariant().toLongLong();
    data.captureBackupCount
        = object.value( QStringLiteral( "captureBackupCount" ) ).toInt( 0 );
    return data;
}
