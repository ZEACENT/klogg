#pragma once

#include <cstdint>
#include <optional>
#include <utility>

#include <QJsonObject>
#include <QString>
#include <QStringList>

#include "livesourcetransport.h"
#include "streaminglogdata.h"

struct AdbLogcatSessionData {
    QString adbExecutable;
    QString deviceSerial;
    QString deviceDescription;
    QString extraArgs;
    QString captureId;
    QString boundOutputFile;
    LiveLogSourceType sourceType = LiveLogSourceType::AdbLogcat;
    bool ansiOutputEnabled = false;
    bool autoReconnectEnabled = false;
    int maxReconnectAttempts = 0;  // 0 = unlimited
    qint64 captureMaxFileSize = 0; // bytes, 0 = unlimited
    int captureBackupCount = 0;    // 0 = keep all rotated files
    LiveLogSaveAnsiMode outputAnsiMode = LiveLogSaveAnsiMode::Strip;
    AdbTransportBackend adbBackend = AdbTransportBackend::SmartSocket;
    IosTransportBackend iosBackend = IosTransportBackend::Native;
    klogg::livecapture::ios::IosEndpointKey iosEndpoint;
    bool migratedFromLegacySession = false;

    klogg::livecapture::RunIntent runIntent{ klogg::livecapture::RunIntent::Stopped };
    QStringList androidBuffers;
    QString androidFilterSpec;
    QString androidPriority;
    std::optional<int> androidPid;
    QString iosLevel;
    QStringList iosCategories;
    QString iosSubsystem;
    bool iosJsonOutput = false;
    bool readOnlyCompatibility = false;

    AdbLogcatSessionData() = default;
    AdbLogcatSessionData( QString executable, QString serial, QString description,
                          QString arguments, QString capture, QString outputFile,
                          LiveLogSourceType liveSourceType = LiveLogSourceType::AdbLogcat,
                          bool ansiEnabled = false, bool reconnectEnabled = false,
                          int reconnectAttempts = 0, qint64 maxFileSize = 0, int backupCount = 0,
                          LiveLogSaveAnsiMode saveAnsiMode = LiveLogSaveAnsiMode::Strip,
                          AdbTransportBackend adbTransportBackend = AdbTransportBackend::Process,
                          IosTransportBackend iosTransportBackend = IosTransportBackend::Native,
                          klogg::livecapture::ios::IosEndpointKey endpoint = {} )
        : adbExecutable( std::move( executable ) )
        , deviceSerial( std::move( serial ) )
        , deviceDescription( std::move( description ) )
        , extraArgs( std::move( arguments ) )
        , captureId( std::move( capture ) )
        , boundOutputFile( std::move( outputFile ) )
        , sourceType( liveSourceType )
        , ansiOutputEnabled( ansiEnabled )
        , autoReconnectEnabled( reconnectEnabled )
        , maxReconnectAttempts( reconnectAttempts )
        , captureMaxFileSize( maxFileSize )
        , captureBackupCount( backupCount )
        , outputAnsiMode( saveAnsiMode )
        , adbBackend( adbTransportBackend )
        , iosBackend( iosTransportBackend )
        , iosEndpoint( std::move( endpoint ) )
    {
    }

    QString displayName() const;
    QString documentId() const;
    QString associatedPath() const;
    QString persistedSourceType() const;
    bool isValid() const;

    QJsonObject toJson() const;
    static QString persistedSourceType( LiveLogSourceType sourceType );
    static bool isPersistedSourceType( const QString& sourceType );
    static AdbLogcatSessionData fromJson( const QString& json );
};
