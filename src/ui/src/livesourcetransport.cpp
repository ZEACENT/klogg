#include "livesourcetransport.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QMetaType>
#include <QPointer>
#include <QProcess>
#include <QTemporaryFile>
#include <QTimer>

#include "log.h"
#include "platform/platform_process.h"

namespace {
constexpr int StartupFailureGracePeriodMs
    = klogg::platform::startupFailureGracePeriod().count();
constexpr int StartupFailurePollIntervalMs = 10;
}

LiveSourceTransport::LiveSourceTransport( QObject* parent ) : QObject( parent )
{
    static const auto registered = qRegisterMetaType<LiveSourceTransport::State>(
        "LiveSourceTransport::State" );
    Q_UNUSED( registered );
}

ProcessLiveSourceTransport::ProcessLiveSourceTransport( QObject* parent )
    : LiveSourceTransport( parent )
{
    createProcess();
}

void ProcessLiveSourceTransport::createProcess()
{
    process_ = std::make_unique<QProcess>();
    process_->setProcessChannelMode( QProcess::SeparateChannels );
    auto* const currentProcess = process_.get();
    const QPointer<QProcess> processGuard( currentProcess );

    // Create the capture path exclusively before exposing it to a subprocess
    // command. QTemporaryFile keeps the randomized, private file owned by this
    // transport until the process has finished.
    prepareStderrCapture();

    connect( currentProcess, &QProcess::readyReadStandardOutput, this,
             [ this, processGuard ] {
        if ( !processGuard || process_.get() != processGuard.data() ) {
            return;
        }

        auto data = processGuard->readAllStandardOutput();
        if ( !data.isEmpty() ) {
            filterReceivedBytes( data );
            if ( !data.isEmpty() ) {
                Q_EMIT bytesReceived( data );
            }
        }
    } );

    connect( currentProcess, &QProcess::started, this, [ this, processGuard ] {
        if ( destroyed_ || !processGuard || process_.get() != processGuard.data()
             || state_ != State::Connecting
             || asyncStartupPhase_ != AsyncStartupPhase::Starting ) {
            return;
        }

        const auto timing = asyncStartupTiming();
        cancelStartupTimer();
        asyncStartupPhase_ = AsyncStartupPhase::PostStartGrace;
        armStartupTimer( AsyncStartupPhase::PostStartGrace, timing.postStartGraceMs,
                         processGuard.data() );
    } );

    connect( currentProcess, &QProcess::errorOccurred, this,
             [ this, processGuard ]( QProcess::ProcessError error ) {
        if ( destroyed_ || !processGuard || process_.get() != processGuard.data()
             || ( state_ != State::Connecting && state_ != State::Connected ) ) {
            return;
        }

        lastError_ = processGuard->errorString();
        // Crashed is followed by finished(), which can recover redirected stderr.
        if ( error != QProcess::Crashed ) {
            failCurrentProcess( lastError_ );
        }
    } );

    connect( currentProcess,
             qOverload<int, QProcess::ExitStatus>( &QProcess::finished ), this,
             [ this, processGuard ]( int exitCode, QProcess::ExitStatus exitStatus ) {
        if ( destroyed_ || !processGuard || process_.get() != processGuard.data() ) {
            return;
        }

        const auto finishedStderrFilePath = stderrFilePath_;
        cancelStartupTimer();
        asyncStartupPhase_ = AsyncStartupPhase::Idle;
        if ( state_ == State::Connected || state_ == State::Connecting ) {
            const auto fallback = exitStatus == QProcess::NormalExit
                                      ? tr( "Live source exited unexpectedly (%1)" ).arg( exitCode )
                                      : tr( "Live source crashed" );
            failCurrentProcess( fallback );
        }

        QFile::remove( finishedStderrFilePath );
    } );
}

bool ProcessLiveSourceTransport::prepareStderrCapture()
{
    auto stderrFile = std::make_unique<QTemporaryFile>(
        QDir( QDir::tempPath() ).filePath(
            QStringLiteral( "klogg_stderr_XXXXXX.log" ) ) );
    if ( !stderrFile->open() ) {
        lastError_ = tr( "Failed to create a private stderr capture file: %1" )
                         .arg( stderrFile->errorString() );
        stderrFile_.reset();
        stderrFilePath_.clear();
        return false;
    }

    stderrFilePath_ = stderrFile->fileName();
    stderrFile->close();
    stderrFile_ = std::move( stderrFile );
    return true;
}

ProcessLiveSourceTransport::~ProcessLiveSourceTransport()
{
    destroyed_ = true;
    disconnectTransport();
}

bool ProcessLiveSourceTransport::connectTransport()
{
    if ( state_ == State::Connected ) {
        return true;
    }

    lastError_.clear();
    if ( !prepareStderrCapture() ) {
        setState( State::Error );
        Q_EMIT errorOccurred( lastError_ );
        return false;
    }

    // streamingCommand() may embed stderrFilePath_ in a PTY wrapper, so the
    // fresh private path must exist before the command is assembled.
    const auto command = streamingCommand();
    process_->setStandardErrorFile( stderrFilePath_ );
    process_->setProgram( command.program );
    process_->setArguments( command.arguments );
    setState( State::Connecting );
    process_->start();

    if ( !process_->waitForStarted( 3000 ) ) {
        lastError_ = process_->errorString();
        setState( State::Error );
        Q_EMIT errorOccurred( lastError_ );
        return false;
    }

    QElapsedTimer startupTimer;
    startupTimer.start();
    while ( state_ != State::Error && process_->state() != QProcess::NotRunning
            && startupTimer.elapsed() < StartupFailureGracePeriodMs ) {
        process_->waitForFinished( StartupFailurePollIntervalMs );
        QCoreApplication::processEvents();

        // A disconnect may have been processed during processEvents(),
        // swapping process_ with a fresh idle instance.  Bail out cleanly
        // instead of misdiagnosing the new process as a startup failure.
        if ( state_ == State::Disconnected ) {
            return false;
        }
    }

    if ( state_ == State::Error || process_->state() == QProcess::NotRunning ) {
        if ( lastError_.isEmpty() ) {
            // stderr is redirected to stderrFilePath_ via setStandardErrorFile(),
            // so readAllStandardError() is always empty here. Read the temp file
            // instead so the real startup error surfaces (matches the finished
            // handler).
            QString stdErr;
            QFile stderrFile( stderrFilePath_ );
            if ( stderrFile.open( QIODevice::ReadOnly ) ) {
                stdErr = QString::fromUtf8( stderrFile.readAll() ).trimmed();
                stderrFile.close();
            }
            lastError_ = stdErr.isEmpty() ? tr( "Live source terminated during startup" ) : stdErr;
            setState( State::Error );
            Q_EMIT errorOccurred( lastError_ );
        }
        return false;
    }

    setState( State::Connected );
    return true;
}

void ProcessLiveSourceTransport::connectTransportAsync()
{
    if ( state_ == State::Connected || state_ == State::Connecting ) {
        return;
    }

    cancelStartupTimer();
    asyncStartupPhase_ = AsyncStartupPhase::Idle;

    lastError_.clear();
    if ( !prepareStderrCapture() ) {
        setState( State::Error );
        Q_EMIT errorOccurred( lastError_ );
        return;
    }

    // streamingCommand() may embed stderrFilePath_ in a PTY wrapper, so the
    // fresh private path must exist before the command is assembled.
    const auto command = streamingCommand();
    process_->setStandardErrorFile( stderrFilePath_ );
    process_->setProgram( command.program );
    process_->setArguments( command.arguments );
    setState( State::Connecting );

    const auto timing = asyncStartupTiming();
    asyncStartupPhase_ = AsyncStartupPhase::Starting;
    armStartupTimer( AsyncStartupPhase::Starting, timing.startTimeoutMs, process_.get() );
    startProcessAsync( *process_ );
}

void ProcessLiveSourceTransport::disconnectTransport()
{
    // Cancel pending startup work before replacing or terminating the process.
    cancelStartupTimer();
    asyncStartupPhase_ = AsyncStartupPhase::Idle;

    if ( !process_ || process_->state() == QProcess::NotRunning ) {
        setState( State::Disconnected );
        return;
    }

    // Detach old process and cut all signal connections
    auto* dying = process_.release();
    dying->disconnect( this );

    // Terminate the old process
    if ( destroyed_ ) {
        // Destructor path: synchronous cleanup, no need to create a new process
        setState( State::Disconnected );
        // Guard against pid == 0: if the child already exited and Qt cleared
        // the pid, kill(0, SIGTERM) would send SIGTERM to our process group.
        if ( dying->processId() > 0 ) {
            dying->terminate();
        }
        if ( !dying->waitForFinished( 1500 ) ) {
            dying->kill();
            dying->waitForFinished( 1500 );
        }
        delete dying;
    }
    else {
        // Keep the old capture owner alive until the detached process closes
        // its redirected stderr handle. Windows cannot unlink an open redirect.
        const auto detachedStderrFilePath = stderrFilePath_;
        std::shared_ptr<QTemporaryFile> detachedStderrFile(
            std::move( stderrFile_ ) );

        // Create fresh process and capture resources for future connections.
        createProcess();
        setState( State::Disconnected );

        // QProcess on Windows keeps redirected handles open until its destructor,
        // which is later than finished(). Retain the old capture through QObject
        // destruction, then remove it after the QProcess-specific teardown.
        QObject::connect(
            dying, &QObject::destroyed,
            [ detachedStderrFile,
              detachedStderrFilePath ]( QObject* ) mutable {
            QFile::remove( detachedStderrFilePath );
            detachedStderrFile.reset();
        } );
        QObject::connect(
            dying, qOverload<int, QProcess::ExitStatus>( &QProcess::finished ),
            dying, &QObject::deleteLater );

        // On macOS, QProcess::terminate() followed by deleteLater can cause
        // ~QProcess() to re-send SIGTERM with a stale PID if the child was
        // already reaped and the OS reused the PID. Use kill() (SIGKILL), which
        // forces immediate exit and leaves the destructor with NotRunning.
        dying->kill();
        QPointer<QProcess> guard( dying );
        QTimer::singleShot( 1500, dying, [ guard ] {
            if ( guard && guard->state() != QProcess::NotRunning ) {
                guard->kill();
            }
        } );
    }
}

bool ProcessLiveSourceTransport::clearRemote( QString* error )
{
    QByteArray stdErr;
    const auto ok = runBlockingCommand( clearCommand(), &stdErr );
    if ( !ok ) {
        lastError_ = stdErr.isEmpty() ? tr( "Failed to clear remote source" )
                                      : QString::fromUtf8( stdErr ).trimmed();
        if ( error ) {
            *error = lastError_;
        }
        return false;
    }

    if ( error ) {
        error->clear();
    }
    lastError_.clear();
    return true;
}

QString ProcessLiveSourceTransport::lastError() const
{
    return lastError_;
}

bool ProcessLiveSourceTransport::runBlockingCommand( const Command& command, QByteArray* stdErr ) const
{
    QProcess process;
    process.start( command.program, command.arguments );
    if ( !process.waitForStarted( 3000 ) ) {
        if ( stdErr ) {
            *stdErr = process.errorString().toUtf8();
        }
        return false;
    }

    if ( !process.waitForFinished( 5000 ) ) {
        process.kill();
        process.waitForFinished( 1500 );
        if ( stdErr ) {
            *stdErr = QObject::tr( "Timed out waiting for %1" ).arg( command.program ).toUtf8();
        }
        return false;
    }
    if ( stdErr ) {
        *stdErr = process.readAllStandardError();
    }

    return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}

void ProcessLiveSourceTransport::startProcessAsync( QProcess& process )
{
    process.start();
}

ProcessLiveSourceTransport::AsyncStartupTiming ProcessLiveSourceTransport::asyncStartupTiming() const
{
    return { 3000, StartupFailureGracePeriodMs };
}

void ProcessLiveSourceTransport::armStartupTimer( AsyncStartupPhase phase, int timeoutMs,
                                                  QProcess* process )
{
    cancelStartupTimer();

    // QTimer is owned by QObject parentage; startupTimer_ is only an observer.
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
    startupTimer_ = new QTimer( this );
    auto* const timer = startupTimer_;
    const QPointer<ProcessLiveSourceTransport> self( this );
    const QPointer<QProcess> processGuard( process );
    timer->setSingleShot( true );
    timer->setTimerType( Qt::PreciseTimer );
    connect( timer, &QTimer::timeout, this,
             [ this, self, processGuard, timer, phase ] {
        if ( startupTimer_ != timer ) {
            timer->deleteLater();
            return;
        }

        startupTimer_ = nullptr;
        timer->deleteLater();
        if ( !self || destroyed_ || !processGuard || process_.get() != processGuard.data()
             || state_ != State::Connecting || asyncStartupPhase_ != phase ) {
            return;
        }

        asyncStartupPhase_ = AsyncStartupPhase::Idle;
        if ( phase == AsyncStartupPhase::Starting ) {
            failCurrentProcess( tr( "Timed out waiting for the live source process to start" ) );
            if ( processGuard->state() != QProcess::NotRunning ) {
                processGuard->kill();
            }
        }
        else if ( processGuard->state() == QProcess::Running ) {
            setState( State::Connected );
        }
        else {
            failCurrentProcess( tr( "Live source terminated during startup" ) );
        }
    } );
    timer->start( timeoutMs );
}

void ProcessLiveSourceTransport::cancelStartupTimer()
{
    if ( startupTimer_ ) {
        startupTimer_->stop();
        delete startupTimer_;
        startupTimer_ = nullptr;
    }
}

QString ProcessLiveSourceTransport::capturedStderr() const
{
    QFile stderrFile( stderrFilePath_ );
    if ( !stderrFile.open( QIODevice::ReadOnly ) ) {
        return {};
    }
    return QString::fromUtf8( stderrFile.readAll() ).trimmed();
}

void ProcessLiveSourceTransport::failCurrentProcess( const QString& fallback )
{
    cancelStartupTimer();
    asyncStartupPhase_ = AsyncStartupPhase::Idle;

    const auto stdErr = capturedStderr();
    if ( !stdErr.isEmpty() ) {
        lastError_ = stdErr;
        LOG_WARNING << "live source stderr " << stdErr;
    }
    else if ( lastError_.isEmpty() ) {
        lastError_ = fallback;
    }

    if ( state_ != State::Error ) {
        setState( State::Error );
        Q_EMIT errorOccurred( lastError_ );
    }
}

void ProcessLiveSourceTransport::setState( State state )
{
    if ( state_ == state ) {
        return;
    }

    state_ = state;
    Q_EMIT stateChanged( state_ );
}

void ProcessLiveSourceTransport::filterReceivedBytes( QByteArray& data )
{
    Q_UNUSED( data );
}
