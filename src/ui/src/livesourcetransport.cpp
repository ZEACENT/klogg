#include "livesourcetransport.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QMetaType>
#include <QPointer>
#include <QProcess>
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

    // Redirect stderr to a temp file so it never reaches the log view.
    // We read the file in the finished handler for error detection only.
    stderrFilePath_ = QDir::tempPath() + QStringLiteral( "/klogg_stderr_%1.log" )
                          .arg( reinterpret_cast<quintptr>( this ), 0, 16 );

    connect( process_.get(), &QProcess::readyReadStandardOutput, this, [ this ] {
        auto data = process_->readAllStandardOutput();
        if ( !data.isEmpty() ) {
            filterReceivedBytes( data );
            if ( !data.isEmpty() ) {
                Q_EMIT bytesReceived( data );
            }
        }
    } );

    connect( process_.get(), &QProcess::errorOccurred, this, [ this ]( QProcess::ProcessError ) {
        if ( disconnectRequested_ ) {
            return;
        }
        lastError_ = process_->errorString();
        setState( State::Error );
        Q_EMIT errorOccurred( lastError_ );
    } );

    connect( process_.get(),
             qOverload<int, QProcess::ExitStatus>( &QProcess::finished ), this,
             [ this ]( int exitCode, QProcess::ExitStatus exitStatus ) {
                 if ( destroyed_ ) {
                     return;
                 }

                 const auto disconnectRequested = disconnectRequested_;
                 disconnectRequested_ = false;

                 if ( disconnectRequested ) {
                     setState( State::Disconnected );
                     return;
                 }

                 if ( state_ == State::Connected || state_ == State::Connecting ) {
                     // Read stderr from the temp file for error detection.
                     // Stderr never reaches the log view because it's
                     // redirected to this file.
                     if ( lastError_.isEmpty() ) {
                         QFile stderrFile( stderrFilePath_ );
                         if ( stderrFile.open( QIODevice::ReadOnly ) ) {
                             const auto stdErr
                                 = QString::fromUtf8( stderrFile.readAll() ).trimmed();
                             if ( !stdErr.isEmpty() ) {
                                 lastError_ = stdErr;
                                 LOG_WARNING << "live source stderr " << stdErr;
                             }
                             stderrFile.close();
                         }
                     }
                     if ( lastError_.isEmpty() ) {
                         lastError_ = exitStatus == QProcess::NormalExit
                                          ? tr( "Live source exited unexpectedly (%1)" ).arg( exitCode )
                                          : tr( "Live source crashed" );
                     }
                     setState( State::Error );
                     Q_EMIT errorOccurred( lastError_ );
                 }

                 // Clean up stderr temp file
                 QFile::remove( stderrFilePath_ );
             } );
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

    const auto command = streamingCommand();
    lastError_.clear();
    // Clean up any previous stderr temp file and redirect stderr so it never
    // appears in the log view. We read it in the finished handler for error
    // detection only.
    QFile::remove( stderrFilePath_ );
    process_->setStandardErrorFile( stderrFilePath_ );
    process_->setProgram( command.program );
    process_->setArguments( command.arguments );
    disconnectRequested_ = false;
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
    if ( state_ == State::Connected ) {
        return;
    }

    const auto command = streamingCommand();
    lastError_.clear();
    QFile::remove( stderrFilePath_ );
    process_->setStandardErrorFile( stderrFilePath_ );
    process_->setProgram( command.program );
    process_->setArguments( command.arguments );
    disconnectRequested_ = false;
    setState( State::Connecting );
    process_->start();

    // After the grace period, if we're still in Connecting state and the
    // process is running, we're connected.  If the process failed to start
    // (errorOccurred FailedToStart) or crashed during the grace (finished),
    // the existing handlers already transitioned to Error — the timer just
    // confirms the happy path.
    QPointer<ProcessLiveSourceTransport> self( this );
    // Capture the specific process instance so a reconnect that replaces
    // process_ before the timer fires cannot promote the wrong process to
    // Connected.
    auto* startedProcess = process_.get();

    // Cancel any stale grace timer from a previous connectTransportAsync()
    // that was interrupted by disconnectTransport() before it fired.
    cancelGraceTimer();

    graceTimer_ = new QTimer( this );
    graceTimer_->setSingleShot( true );
    connect( graceTimer_, &QTimer::timeout, this,
             [ this, self, startedProcess ]() {
        // Single-shot timer fired: drop the handle and free the QTimer. The
        // object was only stop()ed+null'd on cancel; on fire it would otherwise
        // linger parented to this until destruction (cancelGraceTimer deletes
        // it synchronously; here deleteLater is required since we are inside the
        // timer's own timeout signal).
        auto* const firedTimer = graceTimer_;
        graceTimer_ = nullptr;
        if ( firedTimer != nullptr ) {
            firedTimer->deleteLater();
        }

        if ( !self || destroyed_ || disconnectRequested_ ) {
            return;
        }
        // Another handler (errorOccurred / finished) already moved us past
        // Connecting — nothing to do.
        if ( state_ != State::Connecting ) {
            return;
        }

        // A reconnect replaced process_ — this timer is stale.
        if ( process_.get() != startedProcess ) {
            return;
        }

        if ( process_->state() == QProcess::Running ) {
            setState( State::Connected );
        }
        else {
            // Process exited during the grace without triggering the normal
            // errorOccurred/finished handlers — read stderr for diagnostics.
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
    } );
    graceTimer_->start( StartupFailureGracePeriodMs );
}

void ProcessLiveSourceTransport::disconnectTransport()
{
    // Cancel any pending grace timer before tearing down the process.
    // The timer captures process_ pointers that become stale after
    // createProcess() replaces the instance.
    cancelGraceTimer();

    if ( !process_ || process_->state() == QProcess::NotRunning ) {
        disconnectRequested_ = false;
        setState( State::Disconnected );
        return;
    }

    // Clean up the stderr temp file before detaching the process.
    // The finished lambda (which normally removes this file) will never
    // fire because we're about to disconnect it.
    QFile::remove( stderrFilePath_ );

    // Detach old process and cut all signal connections
    auto* dying = process_.release();
    dying->disconnect( this );

    disconnectRequested_ = false;

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
        // Create fresh process for future connections
        createProcess();
        setState( State::Disconnected );

        // Async cleanup, non-blocking.
        // On macOS, QProcess::terminate() followed by deleteLater can cause
        // ~QProcess() to re-send SIGTERM with a stale PID if the child was
        // already reaped and the OS reused the PID.  Use kill() (SIGKILL)
        // which forces immediate exit and is safe against PID reuse because
        // SIGKILL cannot be caught and the destructor's terminateProcess()
        // will see processState == NotRunning.
        dying->kill();
        QObject::connect( dying,
                          qOverload<int, QProcess::ExitStatus>( &QProcess::finished ),
                          dying, &QObject::deleteLater );
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

void ProcessLiveSourceTransport::cancelGraceTimer()
{
    if ( graceTimer_ ) {
        graceTimer_->stop();
        delete graceTimer_;
        graceTimer_ = nullptr;
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
