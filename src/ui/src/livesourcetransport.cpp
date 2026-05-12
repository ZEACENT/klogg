#include "livesourcetransport.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QMetaType>
#include <QPointer>
#include <QProcess>
#include <QTimer>

#include "log.h"

namespace {
constexpr int StartupFailureGracePeriodMs = 250;
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

    connect( process_.get(), &QProcess::readyReadStandardOutput, this, [ this ] {
        auto data = process_->readAllStandardOutput();
        if ( !data.isEmpty() ) {
            filterReceivedBytes( data );
            if ( !data.isEmpty() ) {
                Q_EMIT bytesReceived( data );
            }
        }
    } );

    connect( process_.get(), &QProcess::readyReadStandardError, this, [ this ] {
        const auto stdErr = QString::fromUtf8( process_->readAllStandardError() ).trimmed();
        if ( !stdErr.isEmpty() ) {
            lastError_ = stdErr;
            LOG_WARNING << "live source stderr " << stdErr;
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
                     if ( exitStatus != QProcess::NormalExit || exitCode != 0 || !lastError_.isEmpty() ) {
                         if ( lastError_.isEmpty() ) {
                             lastError_ = tr( "Live source exited unexpectedly (%1)" ).arg( exitCode );
                         }
                         setState( State::Error );
                         Q_EMIT errorOccurred( lastError_ );
                     }
                     else {
                         setState( State::Disconnected );
                     }
                 }
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
            const auto stdErr = QString::fromUtf8( process_->readAllStandardError() ).trimmed();
            lastError_ = stdErr.isEmpty() ? tr( "Live source terminated during startup" ) : stdErr;
            setState( State::Error );
            Q_EMIT errorOccurred( lastError_ );
        }
        return false;
    }

    setState( State::Connected );
    return true;
}

void ProcessLiveSourceTransport::disconnectTransport()
{
    if ( !process_ || process_->state() == QProcess::NotRunning ) {
        disconnectRequested_ = false;
        setState( State::Disconnected );
        return;
    }

    // Detach old process and cut all signal connections
    auto* dying = process_.release();
    dying->disconnect( this );

    disconnectRequested_ = false;

    // Terminate the old process
    if ( destroyed_ ) {
        // Destructor path: synchronous cleanup, no need to create a new process
        setState( State::Disconnected );
        dying->terminate();
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

        // Async cleanup, non-blocking
        dying->terminate();
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
