#ifndef LIVESOURCETRANSPORT_H
#define LIVESOURCETRANSPORT_H

#include <memory>

#include <QByteArray>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QStringList>

class QProcess;

class LiveSourceTransport : public QObject {
    Q_OBJECT

  public:
    enum class State { Disconnected, Connecting, Connected, Error };
    Q_ENUM( State )

    explicit LiveSourceTransport( QObject* parent = nullptr );
    ~LiveSourceTransport() override = default;

    virtual bool connectTransport() = 0;
    // Non-blocking variant: starts the subprocess and sets up signal-driven
    // startup detection (grace timer + error/finished handlers) instead of
    // blocking the calling thread.  Intended for the auto-reconnect path
    // where the caller does not need a synchronous success/failure result.
    virtual void connectTransportAsync() = 0;
    virtual void disconnectTransport() = 0;
    virtual bool clearRemote( QString* error ) = 0;
    virtual QString lastError() const = 0;

  Q_SIGNALS:
    void bytesReceived( const QByteArray& data );
    void stateChanged( LiveSourceTransport::State state );
    void errorOccurred( const QString& error );
};

class ProcessLiveSourceTransport : public LiveSourceTransport {
    Q_OBJECT

  public:
    struct Command {
        QString program;
        QStringList arguments;
    };

    explicit ProcessLiveSourceTransport( QObject* parent = nullptr );
    ~ProcessLiveSourceTransport() override;

    bool connectTransport() override;
    void connectTransportAsync() override;
    void disconnectTransport() override;
    bool clearRemote( QString* error ) override;
    QString lastError() const override;

  protected:
    virtual Command streamingCommand() const = 0;
    virtual Command clearCommand() const = 0;
    virtual void filterReceivedBytes( QByteArray& data );
    bool runBlockingCommand( const Command& command, QByteArray* stdErr ) const;

    // Path of the temp file that captures the subprocess stderr (it never
    // reaches the log view).  Exposed so a transport that wraps the command in
    // a PTY can redirect the inner command's stderr to this file *outside* the
    // PTY — a PTY otherwise merges stderr into stdout.
    QString stderrFilePath() const
    {
        return stderrFilePath_;
    }

  private:
    void setState( State state );
    void createProcess();

  private:
    std::unique_ptr<QProcess> process_;
    State state_{ State::Disconnected };
    QString lastError_;
    QString stderrFilePath_;
    bool destroyed_ = false;
    bool disconnectRequested_ = false;
};

Q_DECLARE_METATYPE( LiveSourceTransport::State )

#endif
