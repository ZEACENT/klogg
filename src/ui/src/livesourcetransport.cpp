#include "livesourcetransport.h"

#include <utility>

#include <QDir>
#include <QFile>
#include <QMetaType>
#include <QPointer>
#include <QProcess>
#include <QTemporaryFile>
#include <QTimer>

#include "log.h"
#include "platform/platform_process.h"

namespace {
constexpr int StartupFailureGracePeriodMs = klogg::platform::startupFailureGracePeriod().count();
constexpr int ClearTimeoutMs = 5000;

QString processErrorText( QProcess& process, const QString& fallback )
{
    auto stdErr = QString::fromUtf8( process.readAllStandardError() ).trimmed();
    if ( !stdErr.isEmpty() ) {
        return stdErr;
    }
    if ( !process.errorString().isEmpty() ) {
        return process.errorString();
    }
    return fallback;
}
} // namespace

struct ProcessLiveSourceTransport::ProcessContext {
    Generation generation{ 0 };
};

LiveSourceTransport::LiveSourceTransport( QObject* parent )
    : QObject( parent )
{
    static const auto registeredState
        = qRegisterMetaType<LiveSourceTransport::State>( "LiveSourceTransport::State" );
    static const auto registeredGeneration
        = qRegisterMetaType<LiveSourceTransport::Generation>( "LiveSourceTransport::Generation" );
    static const auto registeredRequest = qRegisterMetaType<LiveSourceTransport::ClearRequestId>(
        "LiveSourceTransport::ClearRequestId" );
    Q_UNUSED( registeredState );
    Q_UNUSED( registeredGeneration );
    Q_UNUSED( registeredRequest );
}

std::optional<klogg::livecapture::LiveSourceError> LiveSourceTransport::lastStructuredError() const
{
    return std::nullopt;
}

klogg::livecapture::LiveDataStatistics LiveSourceTransport::statistics() const
{
    std::lock_guard<std::mutex> lock( statisticsMutex_ );
    return statistics_;
}

void LiveSourceTransport::resetStatistics( Generation generation )
{
    std::lock_guard<std::mutex> lock( statisticsMutex_ );
    statistics_ = klogg::livecapture::LiveDataStatistics{};
    statistics_.generation = generation;
}

void LiveSourceTransport::recordDeliveredChunk( Generation generation, std::size_t byteCount )
{
    std::lock_guard<std::mutex> lock( statisticsMutex_ );
    if ( statistics_.generation != generation ) {
        return;
    }

    klogg::livecapture::recordLiveDataReceived( statistics_, byteCount );
    klogg::livecapture::recordLiveDataDelivered( statistics_, byteCount );
}

ProcessLiveSourceTransport::ProcessLiveSourceTransport( QObject* parent )
    : LiveSourceTransport( parent )
{
    createProcess();
}

void ProcessLiveSourceTransport::createProcess()
{
    process_ = std::make_unique<QProcess>();
    processContext_ = std::make_shared<ProcessContext>();
    process_->setProcessChannelMode( QProcess::SeparateChannels );
    auto* const currentProcess = process_.get();
    const QPointer<QProcess> processGuard( currentProcess );
    const auto context = processContext_;

    // Every process context owns a distinct private stderr capture from birth.
    // This keeps retirement ownership exact even before the process is started.
    prepareStderrCapture();

    connect( currentProcess, &QProcess::readyReadStandardOutput, this,
             [ this, processGuard, context ] {
                 if ( !processGuard || !isCurrentProcess( processGuard.data(), context ) ) {
                     return;
                 }

                 auto data = processGuard->readAllStandardOutput();
                 if ( !data.isEmpty() ) {
                     filterReceivedBytes( data );
                     if ( !processGuard || !isCurrentProcess( processGuard.data(), context ) ) {
                         return;
                     }
                     if ( !data.isEmpty() ) {
                         recordDeliveredChunk( context->generation,
                                               static_cast<std::size_t>( data.size() ) );
                         Q_EMIT bytesReceived( context->generation, data );
                     }
                 }
             } );

    connect( currentProcess, &QProcess::started, this, [ this, processGuard, context ] {
        if ( !processGuard || !isCurrentProcess( processGuard.data(), context )
             || state_ != State::Connecting || asyncStartupPhase_ != AsyncStartupPhase::Starting ) {
            return;
        }

        const auto timing = asyncStartupTiming();
        cancelStartupTimer();
        asyncStartupPhase_ = AsyncStartupPhase::PostStartGrace;
        armStartupTimer( AsyncStartupPhase::PostStartGrace, timing.postStartGraceMs,
                         processGuard.data(), context );
    } );

    connect( currentProcess, &QProcess::errorOccurred, this,
             [ this, processGuard, context ]( QProcess::ProcessError error ) {
                 if ( !processGuard || !isCurrentProcess( processGuard.data(), context )
                      || ( state_ != State::Connecting && state_ != State::Connected ) ) {
                     return;
                 }

                 lastError_ = processGuard->errorString();
                 // Crashed is normally followed by finished(), which can recover the
                 // redirected stderr. Other errors need immediate completion.
                 if ( error != QProcess::Crashed ) {
                     failCurrentProcess( context->generation, lastError_ );
                 }
             } );

    connect( currentProcess, qOverload<int, QProcess::ExitStatus>( &QProcess::finished ), this,
             [ this, processGuard, context ]( int exitCode, QProcess::ExitStatus exitStatus ) {
                 if ( !processGuard || !isCurrentProcess( processGuard.data(), context ) ) {
                     return;
                 }

                 cancelStartupTimer();
                 asyncStartupPhase_ = AsyncStartupPhase::Idle;
                 if ( state_ == State::Connected || state_ == State::Connecting ) {
                     const auto fallback
                         = exitStatus == QProcess::NormalExit
                               ? tr( "Live source exited unexpectedly (%1)" ).arg( exitCode )
                               : tr( "Live source crashed" );
                     failCurrentProcess( context->generation, fallback );
                 }
                 else {
                     retireCurrentProcess();
                 }
             } );
}

bool ProcessLiveSourceTransport::isCurrentProcess(
    const QProcess* process, const std::shared_ptr<ProcessContext>& context ) const
{
    return !destroyed_ && process != nullptr && process_.get() == process
           && processContext_ == context && activeGeneration_.has_value()
           && *activeGeneration_ == context->generation;
}

void ProcessLiveSourceTransport::retireCurrentProcess()
{
    if ( !process_ ) {
        return;
    }

    auto dyingProcess = std::move( process_ );
    processContext_.reset();
    auto* const dying = dyingProcess.get();
    dying->disconnect( this );

    // QProcess can retain a redirected Windows handle until its destructor,
    // even after state() becomes NotRunning. Keep the QTemporaryFile owner and
    // path attached to that exact process lifetime.
    const auto detachedStderrFilePath = stderrFilePath_;

    if ( destroyed_ ) {
        auto detachedStderrFile = std::move( stderrFile_ );
        if ( dying->state() != QProcess::NotRunning ) {
            if ( dying->processId() > 0 ) {
                dying->terminate();
            }
            if ( !dying->waitForFinished( 1500 ) ) {
                dying->kill();
                dying->waitForFinished( 1500 );
            }
        }
        dyingProcess.reset();
        if ( !detachedStderrFilePath.isEmpty() ) {
            QFile::remove( detachedStderrFilePath );
        }
        detachedStderrFile.reset();
        return;
    }

    std::shared_ptr<QTemporaryFile> detachedStderrFile( std::move( stderrFile_ ) );

    // Keep asynchronous retirement owned by the transport. finished() normally
    // schedules prompt deletion, while QObject parentage closes the lifetime if
    // the event loop stops or the transport is destroyed first.
    dying->setParent( this );

    // A new generation must never reuse the QProcess whose callbacks are
    // currently unwinding. Create the fresh process before external callbacks
    // can request a reentrant start.
    createProcess();

    QObject::connect( dying, &QObject::destroyed,
                      [ detachedStderrFile, detachedStderrFilePath ]( QObject* ) mutable {
                          if ( !detachedStderrFilePath.isEmpty() ) {
                              QFile::remove( detachedStderrFilePath );
                          }
                          detachedStderrFile.reset();
                      } );

    if ( dying->state() == QProcess::NotRunning ) {
        auto* const qtOwnedProcess = dyingProcess.release();
        Q_ASSERT( qtOwnedProcess == dying );
        Q_UNUSED( qtOwnedProcess );
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
    auto* const qtOwnedProcess = dyingProcess.release();
    Q_ASSERT( qtOwnedProcess == dying );
    Q_UNUSED( qtOwnedProcess );
}

bool ProcessLiveSourceTransport::prepareStderrCapture()
{
    auto stderrFile = std::make_unique<QTemporaryFile>(
        QDir( QDir::tempPath() ).filePath( QStringLiteral( "klogg_stderr_XXXXXX.log" ) ) );
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

// Qt child teardown is explicitly contained below; clang-tidy still models
// QObject/QProcess destructors as potentially throwing.
// NOLINTNEXTLINE(bugprone-exception-escape)
ProcessLiveSourceTransport::~ProcessLiveSourceTransport()
{
    destroyed_ = true;
    cancelStartupTimer();
    activeGeneration_.reset();
    retireCurrentProcess();
    stopOwnedChildProcesses();
}

void ProcessLiveSourceTransport::stopOwnedChildProcesses()
{
    // Retired streams and asynchronous clear commands are QObject children.
    // Quiesce them before QObject tears down its child list so QProcess is never
    // destroyed while the operating-system process is still running.
    for ( auto* child : children() ) {
        auto* const childProcess = qobject_cast<QProcess*>( child );
        if ( !childProcess ) {
            continue;
        }

        childProcess->disconnect( this );
        if ( childProcess->state() != QProcess::NotRunning ) {
            childProcess->kill();
            childProcess->waitForFinished( 1500 );
        }
    }
}

void ProcessLiveSourceTransport::start( Generation generation )
{
    if ( activeGeneration_ == generation
         && ( state_ == State::Connecting || state_ == State::Connected ) ) {
        return;
    }

    resetStatistics( generation );

    // Invalidate the old generation before retiring its process. Any synchronous
    // callbacks caused by cancellation therefore fail isCurrentProcess().
    if ( activeGeneration_.has_value() && *activeGeneration_ != generation ) {
        activeGeneration_.reset();
        cancelStartupTimer();
        asyncStartupPhase_ = AsyncStartupPhase::Idle;
        retireCurrentProcess();
    }

    if ( !process_ ) {
        createProcess();
    }

    activeGeneration_ = generation;
    processContext_->generation = generation;
    const auto context = processContext_;
    auto* const currentProcess = process_.get();
    cancelStartupTimer();
    asyncStartupPhase_ = AsyncStartupPhase::Idle;
    lastError_.clear();
    prepareStreamingSession();

    if ( !prepareStderrCapture() ) {
        failCurrentProcess( generation, lastError_ );
        return;
    }

    // streamingCommand() may embed stderrFilePath_ in a PTY wrapper, so the
    // fresh private path must exist before the command is assembled.
    const auto command = streamingCommand();
    currentProcess->setStandardErrorFile( stderrFilePath_ );
    currentProcess->setProgram( command.program );
    currentProcess->setArguments( command.arguments );
    setState( generation, State::Connecting );

    // stateChanged is intentionally synchronous. A listener may replace or stop
    // this generation reentrantly, so never continue startup through whatever
    // process happens to be current after the signal returns.
    if ( !isCurrentProcess( currentProcess, context ) || state_ != State::Connecting ) {
        return;
    }

    const auto timing = asyncStartupTiming();
    asyncStartupPhase_ = AsyncStartupPhase::Starting;
    armStartupTimer( AsyncStartupPhase::Starting, timing.startTimeoutMs, currentProcess, context );
    startProcessAsync( *currentProcess );
}

void ProcessLiveSourceTransport::stop( Generation generation )
{
    if ( !activeGeneration_.has_value() || *activeGeneration_ != generation ) {
        return;
    }

    // Generation invalidation is deliberately the first mutation. QProcess
    // cancellation may synchronously dispatch platform callbacks.
    activeGeneration_.reset();
    cancelStartupTimer();
    asyncStartupPhase_ = AsyncStartupPhase::Idle;
    retireCurrentProcess();
    setState( generation, State::Disconnected );
}

void ProcessLiveSourceTransport::clearRemoteAsync( Generation generation, ClearRequestId requestId )
{
    const auto command = clearCommand();
    // QObject parentage owns the operation independently from the streaming
    // process. Completion remains correlated even when results arrive out of
    // order or the stream generation has already retired.
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
    auto* const process = new QProcess( this );
    process->setProcessChannelMode( QProcess::SeparateChannels );

    struct CompletionState {
        bool completed = false;
    };
    const auto completionState = std::make_shared<CompletionState>();
    const QPointer<ProcessLiveSourceTransport> self( this );
    const QPointer<QProcess> processGuard( process );

    const auto complete = [ self, processGuard, completionState, generation,
                            requestId ]( bool succeeded, QString error ) {
        if ( completionState->completed ) {
            return;
        }
        completionState->completed = true;

        if ( self ) {
            Q_EMIT self->clearRemoteFinished( generation, requestId, succeeded, error );
        }
        if ( processGuard ) {
            if ( processGuard->state() == QProcess::NotRunning ) {
                processGuard->deleteLater();
            }
            else {
                QObject::connect( processGuard.data(),
                                  qOverload<int, QProcess::ExitStatus>( &QProcess::finished ),
                                  processGuard.data(), &QObject::deleteLater );
            }
        }
    };

    connect( process, &QProcess::errorOccurred, this,
             [ processGuard, complete ]( QProcess::ProcessError ) {
                 if ( !processGuard ) {
                     return;
                 }
                 complete( false,
                           processErrorText( *processGuard,
                                             QObject::tr( "Failed to clear remote source" ) ) );
             } );
    connect( process, qOverload<int, QProcess::ExitStatus>( &QProcess::finished ), this,
             [ processGuard, complete ]( int exitCode, QProcess::ExitStatus exitStatus ) {
                 if ( !processGuard ) {
                     return;
                 }
                 const auto succeeded = exitStatus == QProcess::NormalExit && exitCode == 0;
                 auto error
                     = succeeded
                           ? QString{}
                           : processErrorText( *processGuard,
                                               QObject::tr( "Failed to clear remote source" ) );
                 complete( succeeded, std::move( error ) );
             } );
    QTimer::singleShot( ClearTimeoutMs, process, [ processGuard, complete, command ] {
        if ( !processGuard ) {
            return;
        }
        if ( processGuard->state() != QProcess::NotRunning ) {
            processGuard->kill();
        }
        complete( false, QObject::tr( "Timed out waiting for %1" ).arg( command.program ) );
    } );

    process->start( command.program, command.arguments );
}

QString ProcessLiveSourceTransport::lastError() const
{
    return lastError_;
}

void ProcessLiveSourceTransport::startProcessAsync( QProcess& process )
{
    process.start();
}

ProcessLiveSourceTransport::AsyncStartupTiming
ProcessLiveSourceTransport::asyncStartupTiming() const
{
    return { 3000, StartupFailureGracePeriodMs };
}

void ProcessLiveSourceTransport::armStartupTimer( AsyncStartupPhase phase, int timeoutMs,
                                                  QProcess* process,
                                                  std::shared_ptr<ProcessContext> context )
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
             [ this, self, processGuard, context = std::move( context ), timer, phase ] {
                 if ( startupTimer_ != timer ) {
                     timer->deleteLater();
                     return;
                 }

                 startupTimer_ = nullptr;
                 timer->deleteLater();
                 if ( !self || !processGuard || !isCurrentProcess( processGuard.data(), context )
                      || state_ != State::Connecting || asyncStartupPhase_ != phase ) {
                     return;
                 }

                 asyncStartupPhase_ = AsyncStartupPhase::Idle;
                 if ( phase == AsyncStartupPhase::Starting ) {
                     failCurrentProcess(
                         context->generation,
                         tr( "Timed out waiting for the live source process to start" ) );
                 }
                 else if ( processGuard->state() == QProcess::Running ) {
                     setState( context->generation, State::Connected );
                 }
                 else {
                     failCurrentProcess( context->generation,
                                         tr( "Live source terminated during startup" ) );
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

void ProcessLiveSourceTransport::failCurrentProcess( Generation generation,
                                                     const QString& fallback )
{
    if ( activeGeneration_ != generation ) {
        return;
    }

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

    // Failure retires this run just as decisively as an intentional stop.
    // Invalidate before process teardown so any synchronous or queued callback
    // from the failed process is stale by construction.
    const auto error = lastError_;
    activeGeneration_.reset();
    retireCurrentProcess();

    if ( stateGeneration_ != generation || state_ != State::Error ) {
        setState( generation, State::Error );
        Q_EMIT errorOccurred( generation, error );
    }
}

void ProcessLiveSourceTransport::setState( Generation generation, State state )
{
    if ( stateGeneration_ == generation && state_ == state ) {
        return;
    }

    stateGeneration_ = generation;
    state_ = state;
    Q_EMIT stateChanged( generation, state_ );
}

void ProcessLiveSourceTransport::prepareStreamingSession() {}

void ProcessLiveSourceTransport::filterReceivedBytes( QByteArray& data )
{
    Q_UNUSED( data );
}
