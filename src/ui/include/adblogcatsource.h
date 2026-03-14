#ifndef ADBLOGCATSOURCE_H
#define ADBLOGCATSOURCE_H

#include <memory>

#include <QJsonObject>
#include <QObject>

#include "livesourcetransport.h"

class StreamingLogData;

struct AdbLogcatSessionData {
    QString adbExecutable;
    QString deviceSerial;
    QString deviceDescription;
    QString extraArgs;
    QString captureId;
    QString boundOutputFile;

    QString displayName() const;
    QString documentId() const;
    QString associatedPath() const;

    QJsonObject toJson() const;
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
    void deleteCaptureFiles();

    const AdbLogcatSessionData& sessionData() const;
    State state() const;
    QString lastError() const;

  Q_SIGNALS:
    void stateChanged( AdbLogcatSource::State state );
    void errorOccurred( const QString& error );

  private:
    void setState( State state );
    void setStateFromTransport( LiveSourceTransport::State state );

  private:
    AdbLogcatSessionData sessionData_;
    std::shared_ptr<StreamingLogData> logData_;
    std::unique_ptr<LiveSourceTransport> transport_;
    State state_{ State::Disconnected };
    QString lastError_;
};

#endif
