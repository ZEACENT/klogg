#ifndef ADBLOGCATSOURCE_H
#define ADBLOGCATSOURCE_H

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include <QObject>

#include "adblogcatsessiondata.h"
#include "livesourcetransport.h"

class StreamingLogData;

class AdbLogcatSource : public QObject {
    Q_OBJECT

public:
    enum class State { Disconnected, Connected, Error };
    Q_ENUM( State )

    using BytesCallback = std::function<void( LiveSourceTransport::Generation, const QByteArray& )>;
    using StateCallback
        = std::function<void( LiveSourceTransport::Generation, LiveSourceTransport::State )>;
    using FailureCallback = std::function<void( LiveSourceTransport::Generation,
                                                klogg::livecapture::LiveSourceError )>;
    using ControlCallback = std::function<void()>;

    AdbLogcatSource( AdbLogcatSessionData sessionData, std::shared_ptr<StreamingLogData> logData,
                     QObject* parent = nullptr );
    AdbLogcatSource( AdbLogcatSessionData sessionData, std::shared_ptr<StreamingLogData> logData,
                     const LiveSourceTransportFactory& transportFactory,
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
    bool isTransportAvailable() const;
    bool isReadOnlyCompatibility() const;

    void setControllerCallbacks( BytesCallback bytes, StateCallback state,
                                 FailureCallback failure, ControlCallback stop = {},
                                 ControlCallback restart = {} );
    void invalidateTransportGeneration( LiveSourceTransport::Generation generation );
    void cancelTransport( LiveSourceTransport::Generation generation );
    void openTransport( LiveSourceTransport::Generation generation,
                        const LiveSourceTransportConfig& config );
    void appendTransportBytes( LiveSourceTransport::Generation generation,
                               const QByteArray& bytes );

    void setCaptureLimits( qint64 rollingMaxFileSize, int rollingBackupCount,
                           qint64 maxTotalLines = 0 );

Q_SIGNALS:
    void stateChanged( AdbLogcatSource::State state );
    void errorOccurred( const QString& error );
    void clearFailed( const QString& error );
    void captureOutputChanged( bool healthy, const QString& detail );

private:
    using Generation = LiveSourceTransport::Generation;
    using ClearRequestId = LiveSourceTransport::ClearRequestId;

    Generation nextGeneration();
    ClearRequestId nextClearRequestId();
    void startTransport();
    void wireTransport();
    void retireTransport();
    void setState( State state );
    void setStateFromTransport( Generation generation, LiveSourceTransport::State state );
    void finishClear( Generation generation, ClearRequestId requestId, bool succeeded,
                      const QString& error );

    AdbLogcatSessionData sessionData_;
    std::shared_ptr<StreamingLogData> logData_;
    const LiveSourceTransportFactory* transportFactory_{ nullptr };
    std::unique_ptr<LiveSourceTransport> transport_;
    std::vector<std::unique_ptr<LiveSourceTransport>> retiredTransports_;
    State state_{ State::Disconnected };
    QString lastError_;
    bool connecting_ = false;
    Generation generationCounter_{ 0 };
    std::optional<Generation> activeGeneration_;
    ClearRequestId clearRequestCounter_{ 0 };
    std::optional<Generation> pendingClearGeneration_;
    std::optional<ClearRequestId> pendingClearRequestId_;
    bool restartAfterClear_ = false;
    BytesCallback controllerBytes_;
    StateCallback controllerState_;
    FailureCallback controllerFailure_;
    ControlCallback controllerStop_;
    ControlCallback controllerRestart_;
    bool retiredCleanupScheduled_{ false };
};

#endif
