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
            failCurrentProcess( lastError_, true );
        }
    } );

    connect( currentProcess,
             qOverload<int, QProcess::ExitStatus>( &QProcess::finished ), this,
             [ this, processGuard ]( int exitCode, QProcess::ExitStatus exitStatus ) {
        if ( destroyed_ || !processGuard || process_.get() != processGuard.data() ) {
            return;
        }

        cancelStartupTimer();
        asyncStartupPhase_ = AsyncStartupPhase::Idle;
        if ( state_ == State::Connected || state_ == State::Connecting ) {
            const auto fallback = exitStatus == QProcess::NormalExit
                                      ? tr( "Live source exited unexpectedly (%1)" ).arg( exitCode )
                                      : tr( "Live source crashed" );
            failCurrentProcess( fallback, true );
        }
        else {
            // Even after finished(), QProcess can retain redirected Windows
            // handles until destruction. Retire it before releasing the capture.
            retireCurrentProcess();
        }
    } );
}

void ProcessLiveSourceTransport::retireCurrentProcess()
{
    if ( !process_ ) {
        return;
    }

    auto* const dying = process_.release();
    dying->disconnect( this );

    // QProcess can retain a redirected Windows handle until its destructor,
    // even after state() becomes NotRunning. Keep the QTemporaryFile owner and
    // path attached to that exact process lifetime.
    const auto detachedStderrFilePath = stderrFilePath_;
    std::shared_ptr<QTemporaryFile> detachedStderrFile( std::move( stderrFile_ ) );

    if ( destroyed_ ) {
        if ( dying->state() != QProcess::NotRunning ) {
            if ( dying->processId() > 0 ) {
                dying->terminate();
            }
            if ( !dying->waitForFinished( 1500 ) ) {
                dying->kill();
                dying->waitForFinished( 1500 );
            }
        }
        delete dying;
        QFile::remove( detachedStderrFilePath );
        detachedStderrFile.reset();
        return;
    }

    // Future connects must never reuse the process whose finished/error signal
    // is currently unwinding. A reentrant error handler therefore sees a fresh
    // process and capture pair.
    createProcess();

    QObject::connect(
        dying, &QObject::destroyed,
        [ detachedStderrFile,
          detachedStderrFilePath ]( QObject* ) mutable {
        QFile::remove( detachedStderrFilePath );
        detachedStderrFile.reset();
    } );

    if ( dying->state() == QProcess::NotRunning ) {
        dying->deleteLater();
        return;
    }

    QObject::connect( dying, qOverload<int, QProcess::ExitStatus>( &QProcess::finished ), dying,
                      &QObject::deleteLater );

    // On macOS, QProcess::terminate() followed by deleteLater can cause
    // ~QProcess() to re-send SIGTERM with a stale PID if the child was already
    // reaped and the OS reused the PID. Use kill() to force immediate exit.
    dying->kill();
    QPointer<QProcess> guard( dying );
    QTimer::singleShot( 1500, dying, [ guard ] {
        if ( guard && guard->state() != QProcess::NotRunning ) {
            guard->kill();
        }
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
    auto* const currentProcess = process_.get();
    currentProcess->setStandardErrorFile( stderrFilePath_ );
    currentProcess->setProgram( command.program );
    currentProcess->setArguments( command.arguments );
    setState( State::Connecting );
    currentProcess->start();

    const auto started = currentProcess->waitForStarted( 3000 );
    if ( process_.get() != currentProcess ) {
        return false;
    }
    if ( !started ) {
        lastError_ = currentProcess->errorString();
        failCurrentProcess( lastError_, true );
        return false;
    }

    QElapsedTimer startupTimer;
    startupTimer.start();
    while ( process_.get() == currentProcess && state_ != State::Error
            && currentProcess->state() != QProcess::NotRunning
            && startupTimer.elapsed() < StartupFailureGracePeriodMs ) {
        currentProcess->waitForFinished( StartupFailurePollIntervalMs );
        QCoreApplication::processEvents();

        // A disconnect or failure may have been processed during processEvents(),
        // swapping process_ with a fresh idle instance. Bail out without touching
        // the retired QProcess again.
        if ( process_.get() != currentProcess || state_ == State::Disconnected ) {
            return false;
        }
    }

    if ( process_.get() != currentProcess ) {
        return false;
    }
    if ( state_ == State::Error || currentProcess->state() == QProcess::NotRunning ) {
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

    if ( !process_ ) {
        setState( State::Disconnected );
        return;
    }
    if ( !destroyed_ && state_ == State::Disconnected
         && process_->state() == QProcess::NotRunning ) {
        return;
    }

    retireCurrentProcess();
    setState( State::Disconnected );
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
            failCurrentProcess( tr( "Timed out waiting for the live source process to start" ),
                                true );
        }
        else if ( processGuard->state() == QProcess::Running ) {
            setState( State::Connected );
        }
        else {
            failCurrentProcess( tr( "Live source terminated during startup" ), true );
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

void ProcessLiveSourceTransport::failCurrentProcess( const QString& fallback, bool retireProcess )
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

    if ( retireProcess ) {
        retireCurrentProcess();
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
