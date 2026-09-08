#ifndef ADBLOGCATSOURCE_H
#define ADBLOGCATSOURCE_H

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <vector>

#include <QObject>
#include <QTimer>

#include "capturestore.h"

#include "adblogcatsessiondata.h"
#include "livesourcetransport.h"

class StreamingLogData;

class AdbLogcatSource : public QObject {
    Q_OBJECT

public:
    enum class State { Disconnected, Connected, Error };
    Q_ENUM( State )

    using DeliverySequence = std::uint64_t;
    using DeliverySettledCallback = std::function<void()>;
    using BytesCallback = std::function<void( LiveSourceTransport::Generation, const QByteArray&,
                                              DeliverySettledCallback )>;
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
    bool hasActiveOutputBinding( const QString& outputPath,
                                 LiveLogSaveAnsiMode ansiMode ) const;
    bool synchronizeOutputBinding( LiveLogSaveAnsiMode ansiMode );
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
    void cancelTransport( LiveSourceTransport::Generation generation,
                          klogg::livecapture::StopDisposition disposition
                              = klogg::livecapture::StopDisposition::DiscardPending );
    using StoppedCallback = std::function<void( LiveSourceTransport::Generation, std::uint64_t )>;
    void setStoppedCallback( StoppedCallback callback );
    using FinalizedCallback = std::function<void( LiveSourceTransport::Generation,
        const klogg::livecapture::CaptureDeliveryResult& )>;
    void setFinalizedCallback( FinalizedCallback callback );
    bool isInputTerminated() const;
    // nullopt until real stopped; a value is one bounded persistence turn, not fsync.
    std::optional<CaptureStore::PersistenceResult> persistForClose( int maxSegments = 32 );
    std::optional<CaptureOutputError> flushOutputForClose();
    static klogg::livecapture::CaptureDeliveryResult mapCaptureOutcome(
        const CaptureStore::AppendResult& outcome );
    void openTransport( LiveSourceTransport::Generation generation,
                        const LiveSourceTransportConfig& config );
    klogg::livecapture::CaptureDeliveryResult appendTransportBytes(
        LiveSourceTransport::Generation generation, const QByteArray& bytes );

    void setCaptureLimits( qint64 rollingMaxFileSize, int rollingBackupCount,
                           qint64 maxTotalLines = 0 );

Q_SIGNALS:
    void stateChanged( AdbLogcatSource::State state );
    void errorOccurred( const QString& error );
    void clearFailed( const QString& error );
    void captureOutputChanged( bool healthy, CaptureOutputError error );
    void capturePersistenceChanged( bool healthy, CaptureStore::PersistenceFailure error );

private:
    using Generation = LiveSourceTransport::Generation;
    using ClearRequestId = LiveSourceTransport::ClearRequestId;

    Generation nextGeneration();
    ClearRequestId nextClearRequestId();
    void startTransport();
    void wireTransport();
    void retireTransport();
    void schedulePersistenceRetry();
    QTimer persistenceRetryTimer_;
    bool persistenceSchedulingArmed_{ false };
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
    std::optional<Generation> retiringGeneration_;
    klogg::livecapture::StopDisposition retiringDisposition_{
        klogg::livecapture::StopDisposition::DiscardPending };
    bool stopRequested_{ false };
    StoppedCallback stoppedCallback_;
    FinalizedCallback finalizedCallback_;
    void finalizeInput( Generation generation );
    void beginDeliveryGeneration( Generation generation );
    void settleOfferedDelivery( Generation generation, DeliverySequence sequence );
    void completeRetirementIfSettled( Generation generation );
    struct DeliverySettlementToken {
        Generation generation{ 0 };
        DeliverySequence lastOfferedSequence{ 0 };
        DeliverySequence settledThroughSequence{ 0 };
        std::set<DeliverySequence> settledOutOfOrder;
        bool producerStopped{ false };
        bool completing{ false };
        quint64 discardedBytes{ 0u };
    };
    std::optional<DeliverySettlementToken> deliverySettlement_;
    std::optional<Generation> reportedErrorGeneration_;
    ClearRequestId clearRequestCounter_{ 0 };
    std::optional<Generation> pendingClearGeneration_;
    std::optional<ClearRequestId> pendingClearRequestId_;
    bool restartAfterClear_ = false;
    bool clearAfterStop_ = false;
    bool restartAfterStop_ = false;
    bool performClear( bool restart );
    BytesCallback controllerBytes_;
    StateCallback controllerState_;
    FailureCallback controllerFailure_;
    ControlCallback controllerStop_;
    ControlCallback controllerRestart_;
    bool retiredCleanupScheduled_{ false };
};

#endif
