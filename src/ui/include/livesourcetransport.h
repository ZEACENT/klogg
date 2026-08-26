#ifndef LIVESOURCETRANSPORT_H
#define LIVESOURCETRANSPORT_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>

#include <QByteArray>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QStringList>

#include "ioscatalogprovider.h"
#include "livedatastatistics.h"
#include "livestate.h"

class QProcess;
class QTemporaryFile;
class QTimer;

enum class LiveLogSourceType : std::uint8_t {
    AdbLogcat,
    IosLogStream,
};

enum class AdbTransportBackend : std::uint8_t {
    Process,
    SmartSocket,
};

enum class IosTransportBackend : std::uint8_t {
    Native,
    LegacyProcess,
};

struct LiveSourceTransportConfig {
    LiveLogSourceType sourceType = LiveLogSourceType::AdbLogcat;
    AdbTransportBackend adbBackend = AdbTransportBackend::SmartSocket;
    IosTransportBackend iosBackend = IosTransportBackend::Native;
    klogg::livecapture::ios::IosEndpointKey iosEndpoint;
    QString executable;
    QString deviceId;
    QString extraArgs;
    bool ansiOutputEnabled = false;
    QStringList androidBuffers;
    QString androidFilterSpec;
    QString androidPriority;
    std::optional<int> androidPid;
    QString iosLevel;
    QStringList iosCategories;
    QString iosSubsystem;
    bool iosJsonOutput = false;
};

class LiveSourceTransport : public QObject {
    Q_OBJECT

public:
    using Generation = klogg::livecapture::Generation;
    using ClearRequestId = std::uint64_t;

    enum class State : std::uint8_t { Disconnected, Connecting, Connected, Error };
    Q_ENUM( State )

    explicit LiveSourceTransport( QObject* parent = nullptr );
    ~LiveSourceTransport() override = default;

    virtual void start( Generation generation ) = 0;
    virtual void stop( Generation generation ) = 0;
    virtual void clearRemoteAsync( Generation generation, ClearRequestId requestId ) = 0;
    virtual QString lastError() const = 0;
    virtual std::optional<klogg::livecapture::LiveSourceError> lastStructuredError() const;
    virtual klogg::livecapture::LiveDataStatistics statistics() const;

protected:
    void resetStatistics( Generation generation );
    void recordDeliveredChunk( Generation generation, std::size_t byteCount );

Q_SIGNALS:
    void bytesReceived( LiveSourceTransport::Generation generation, const QByteArray& data );
    void stateChanged( LiveSourceTransport::Generation generation,
                       LiveSourceTransport::State state );
    void errorOccurred( LiveSourceTransport::Generation generation, const QString& error );
    void clearRemoteFinished( LiveSourceTransport::Generation generation,
                              LiveSourceTransport::ClearRequestId requestId, bool succeeded,
                              const QString& error );

private:
    mutable std::mutex statisticsMutex_;
    klogg::livecapture::LiveDataStatistics statistics_;
};

class LiveSourceTransportFactory {
public:
    virtual ~LiveSourceTransportFactory() = default;
    virtual std::unique_ptr<LiveSourceTransport>
    create( const LiveSourceTransportConfig& config ) const = 0;
};

class DefaultLiveSourceTransportFactory final : public LiveSourceTransportFactory {
public:
    std::unique_ptr<LiveSourceTransport>
    create( const LiveSourceTransportConfig& config ) const override;
};

class ProcessLiveSourceTransport : public LiveSourceTransport {
    Q_OBJECT

public:
    struct Command {
        QString program;
        QStringList arguments;
    };

    struct AsyncStartupTiming {
        int startTimeoutMs;
        int postStartGraceMs;
    };

    explicit ProcessLiveSourceTransport( QObject* parent = nullptr );
    ~ProcessLiveSourceTransport() override;

    void start( Generation generation ) override;
    void stop( Generation generation ) override;
    void clearRemoteAsync( Generation generation, ClearRequestId requestId ) override;
    QString lastError() const override;

protected:
    virtual Command streamingCommand() const = 0;
    virtual Command clearCommand() const = 0;
    virtual void prepareStreamingSession();
    virtual void filterReceivedBytes( QByteArray& data );
    virtual void startProcessAsync( QProcess& process );
    virtual AsyncStartupTiming asyncStartupTiming() const;

    // Path of the temp file that captures the subprocess stderr (it never
    // reaches the log view). Exposed so a transport that wraps the command in a
    // PTY can redirect the inner command's stderr outside the PTY.
    QString stderrFilePath() const
    {
        return stderrFilePath_;
    }

private:
    struct ProcessContext;
    enum class AsyncStartupPhase : std::uint8_t { Idle, Starting, PostStartGrace };

    void setState( Generation generation, State state );
    void createProcess();
    void retireCurrentProcess();
    void stopOwnedChildProcesses();
    bool prepareStderrCapture();
    void armStartupTimer( AsyncStartupPhase phase, int timeoutMs, QProcess* process,
                          std::shared_ptr<ProcessContext> context );
    void cancelStartupTimer();
    QString capturedStderr() const;
    void failCurrentProcess( Generation generation, const QString& fallback );
    bool isCurrentProcess( const QProcess* process,
                           const std::shared_ptr<ProcessContext>& context ) const;

private:
    std::unique_ptr<QProcess> process_;
    std::shared_ptr<ProcessContext> processContext_;
    std::unique_ptr<QTemporaryFile> stderrFile_;
    QTimer* startupTimer_{ nullptr };
    AsyncStartupPhase asyncStartupPhase_{ AsyncStartupPhase::Idle };
    std::optional<Generation> activeGeneration_;
    std::optional<Generation> stateGeneration_;
    State state_{ State::Disconnected };
    QString lastError_;
    QString stderrFilePath_;
    bool destroyed_ = false;
};

Q_DECLARE_METATYPE( LiveSourceTransport::State )

#endif
