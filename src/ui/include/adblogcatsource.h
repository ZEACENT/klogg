#ifndef ADBLOGCATSOURCE_H
#define ADBLOGCATSOURCE_H

#include <memory>

#include <QJsonObject>
#include <QObject>
#include <QTimer>

#include "livesourcetransport.h"
#include "streaminglogdata.h"

class StreamingLogData;

enum class LiveLogSourceType {
    AdbLogcat,
    IosLogStream,
};

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
    int maxReconnectAttempts = 0; // 0 = unlimited
    qint64 captureMaxFileSize = 0; // bytes, 0 = unlimited
    int captureBackupCount = 0; // 0 = keep all rotated files

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

class AdbLogcatSource : public QObject {
    Q_OBJECT

  public:
    enum class State { Disconnected, Connected, Error };
    Q_ENUM( State )

    AdbLogcatSource( AdbLogcatSessionData sessionData, std::shared_ptr<StreamingLogData> logData,
                     QObject* parent = nullptr );
    ~AdbLogcatSource() override;

    bool connectSource();
    void disconnectSource();
    bool reconnectSource();
    bool clearAndRestart();
    bool bindOutputFile( const QString& outputPath );
    bool bindOutputFile( const QString& outputPath, LiveLogSaveAnsiMode ansiMode );
    void deleteCaptureFiles();

    const AdbLogcatSessionData& sessionData() const;
    State state() const;
    QString lastError() const;

  Q_SIGNALS:
    void stateChanged( AdbLogcatSource::State state );
    void errorOccurred( const QString& error );
    void reconnectAttemptStarted( int attempt );

  public:
    static constexpr int InitialReconnectDelayMs = 1000;
    static constexpr int MaxReconnectDelayMs = 30000;

    void setAutoReconnectEnabled( bool enabled );
    void setAutoReconnectMaxAttempts( int maxAttempts );
    bool isAutoReconnectActive() const;
    int reconnectAttempt() const;
    void cancelAutoReconnect();
    void setCaptureLimits( qint64 rollingMaxFileSize, int rollingBackupCount,
                           qint64 maxTotalLines = 0 );

  private:
    void setState( State state );
    void setStateFromTransport( LiveSourceTransport::State state );
    void scheduleReconnect();
    void attemptReconnect();

  private:
    AdbLogcatSessionData sessionData_;
    std::shared_ptr<StreamingLogData> logData_;
    std::unique_ptr<LiveSourceTransport> transport_;
    State state_{ State::Disconnected };
    QString lastError_;
    bool manualDisconnect_ = false;

    // Auto-reconnect state. Defaults to disabled: the user's setting is applied
    // via setAutoReconnectEnabled() before the first connect. A true default
    // armed the reconnect timer during the construction-to-registration window
    // with the wrong (unlimited) setting.
    bool autoReconnectEnabled_ = false;
    int autoReconnectMaxAttempts_ = 0; // 0 = unlimited
    int reconnectAttempt_ = 0;
    bool reconnectionProven_ = false; // set when first stdout data arrives
    QTimer reconnectTimer_;
};

#endif
