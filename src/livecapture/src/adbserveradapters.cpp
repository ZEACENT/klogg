/*
 * Copyright (C) 2026 ZEACENT and other contributors
 *
 * This file is part of klogg.
 *
 * klogg is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "adbserversupervisor.h"

#include "adbsmartsocketclient.h"
#include "platform/platform_files.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTimer>

#include <algorithm>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <utility>

namespace klogg::livecapture::adb {
namespace {

constexpr auto versionOperation = AdbSmartSocketClient::OperationId{ 1 };
constexpr auto featuresOperation = AdbSmartSocketClient::OperationId{ 2 };
constexpr int adbServerStderrTailCapacityBytes = 16 * 1024;

std::string stringFromQString( const QString& value )
{
    return value.toStdString();
}

QString standardPrivateKeyPath()
{
    return QDir( QStandardPaths::writableLocation( QStandardPaths::HomeLocation ) )
        .filePath( QStringLiteral( ".android/adbkey" ) );
}

QString endpointSocket( const AdbServerEndpoint& endpoint )
{
    // launch() has already rejected every endpoint except the official loopback
    // listener. ADB's server socket grammar represents that listener as
    // tcp:<port>; tcp:<numeric-host>:<port> is not accepted by the native server.
    return QStringLiteral( "tcp:%1" ).arg( endpoint.port );
}

void retainStderrTail( QByteArray& tail, const QByteArray& output )
{
    if ( output.isEmpty() ) {
        return;
    }
    if ( output.size() >= adbServerStderrTailCapacityBytes ) {
        tail = output.right( adbServerStderrTailCapacityBytes );
        return;
    }

    tail.append( output );
    const auto overflow = tail.size() - adbServerStderrTailCapacityBytes;
    if ( overflow > 0 ) {
        tail.remove( 0, overflow );
    }
}

void drainStderr( QProcess& process, QByteArray& tail )
{
    retainStderrTail( tail, process.readAllStandardError() );
}

std::string appendStderrDiagnostic( std::string diagnostic, const QByteArray& tail )
{
    const auto stderrText = tail.trimmed();
    if ( stderrText.isEmpty() ) {
        return diagnostic;
    }
    if ( !diagnostic.empty() ) {
        diagnostic.push_back( '\n' );
    }
    diagnostic += "Packaged ADB server stderr:\n";
    diagnostic += stderrText.toStdString();
    return diagnostic;
}

std::set<QProcess*>& publishedProcesses()
{
    // Published ADB servers intentionally outlive the launcher and application.
    // Keep their QProcess wrappers reachable until each process exits so Qt does
    // not destroy the wrapper and terminate the shared server during shutdown.
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
    static auto* const processes = new std::set<QProcess*>;
    return *processes;
}

} // namespace

class AdbSmartSocketServerProbe::Impl final {
public:
    explicit Impl( AdbSmartSocketServerProbe& owner )
        : owner_( owner )
    {
    }

    ~Impl()
    {
        retireClient( activeToken_ );
    }

    AdbServerToken probe( const AdbServerEndpoint& endpoint, Callback callback )
    {
        const auto token = ++nextToken_;
        if ( activeToken_ != 0u ) {
            QTimer::singleShot( 0, &owner_, [ callback = std::move( callback ) ]() mutable {
                callback( AdbServerProbeResult{
                    AdbServerProbeState::Failed,
                    0u,
                    {},
                    {},
                    "An ADB server probe is already active.",
                } );
            } );
            return token;
        }

        activeToken_ = token;
        callback_ = std::move( callback );
        phase_ = Phase::Version;
        version_ = 0u;

        AdbSmartSocketClientConfig config;
        config.serverAddress = endpoint.address;
        config.serverPort = endpoint.port;
        // Ownership is transferred to the adapter's Qt object tree.
        // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
        client_ = new AdbSmartSocketClient( config, &owner_ );
        QObject::connect( client_, &AdbSmartSocketClient::hostReplyReceived, &owner_,
                          [ this ]( Generation generation,
                                    AdbSmartSocketClient::OperationId operationId,
                                    const QByteArray& reply ) {
                              hostReplyReceived( generation, operationId, reply );
                          } );
        QObject::connect( client_, &AdbSmartSocketClient::errorOccurred, &owner_,
                          [ this ]( Generation generation, AdbSmartSocketClient::OperationId,
                                    AdbSmartSocketErrorCode code, const QString& diagnostic ) {
                              probeFailed( generation, code, diagnostic );
                          } );
        client_->requestHostService( token, versionOperation, HostService::Version );
        return token;
    }

    void cancel( AdbServerToken token )
    {
        if ( activeToken_ != token ) {
            return;
        }
        callback_ = {};
        const auto generation = activeToken_;
        activeToken_ = 0u;
        retireClient( generation );
    }

private:
    enum class Phase : std::uint8_t { Idle, Version, Features };

    void hostReplyReceived( Generation generation, AdbSmartSocketClient::OperationId operationId,
                            const QByteArray& reply )
    {
        if ( generation != activeToken_ ) {
            return;
        }

        if ( phase_ == Phase::Version && operationId == versionOperation ) {
            bool parsed = false;
            const auto value = reply.trimmed().toULongLong( &parsed, 16 );
            if ( !parsed || value > std::numeric_limits<std::uint32_t>::max() ) {
                complete( AdbServerProbeResult{ AdbServerProbeState::Failed,
                                                0u,
                                                {},
                                                {},
                                                "ADB host:version returned an invalid value." } );
                return;
            }
            version_ = static_cast<std::uint32_t>( value );
            phase_ = Phase::Features;
            client_->requestHostService( activeToken_, featuresOperation,
                                         HostService::ServerFeatures );
            return;
        }

        if ( phase_ != Phase::Features || operationId != featuresOperation ) {
            complete( AdbServerProbeResult{ AdbServerProbeState::Failed,
                                            0u,
                                            {},
                                            {},
                                            "ADB server probe received an unexpected reply." } );
            return;
        }

        std::vector<std::string> features;
        const auto encodedFeatures = reply.trimmed().split( ',' );
        features.reserve( static_cast<std::size_t>( encodedFeatures.size() ) );
        for ( const auto& feature : encodedFeatures ) {
            if ( !feature.isEmpty() ) {
                features.push_back( feature.toStdString() );
            }
        }
        std::sort( features.begin(), features.end() );

        std::string identity = "adb:" + std::to_string( version_ ) + ':';
        for ( const auto& feature : features ) {
            identity += feature;
            identity.push_back( ',' );
        }
        complete( AdbServerProbeResult{ AdbServerProbeState::Ready,
                                        version_,
                                        std::move( features ),
                                        std::move( identity ),
                                        {} } );
    }

    void probeFailed( Generation generation, AdbSmartSocketErrorCode code,
                      const QString& diagnostic )
    {
        if ( generation != activeToken_ ) {
            return;
        }
        const auto state = code == AdbSmartSocketErrorCode::Connection
                                   || code == AdbSmartSocketErrorCode::ConnectTimeout
                               ? AdbServerProbeState::Absent
                               : AdbServerProbeState::Failed;
        complete( AdbServerProbeResult{ state, 0u, {}, {}, stringFromQString( diagnostic ) } );
    }

    void complete( AdbServerProbeResult result )
    {
        auto callback = std::move( callback_ );
        callback_ = {};
        const auto generation = activeToken_;
        activeToken_ = 0u;
        phase_ = Phase::Idle;
        retireClient( generation );
        if ( callback ) {
            callback( std::move( result ) );
        }
    }

    void retireClient( Generation generation )
    {
        if ( client_ == nullptr ) {
            return;
        }
        auto* const retired = client_.data();
        client_ = nullptr;
        QObject::disconnect( retired, nullptr, &owner_, nullptr );
        retired->cancelGeneration( generation );
        retired->deleteLater();
    }

private:
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members)
    AdbSmartSocketServerProbe& owner_;
    QPointer<AdbSmartSocketClient> client_;
    Callback callback_;
    Phase phase_{ Phase::Idle };
    AdbServerToken nextToken_{ 0 };
    AdbServerToken activeToken_{ 0 };
    std::uint32_t version_{ 0 };
};

AdbSmartSocketServerProbe::AdbSmartSocketServerProbe( QObject* parent )
    : QObject( parent )
    , impl_( std::make_unique<Impl>( *this ) )
{
}

AdbSmartSocketServerProbe::~AdbSmartSocketServerProbe() = default;

AdbServerToken AdbSmartSocketServerProbe::probe( const AdbServerEndpoint& endpoint,
                                                 Callback callback )
{
    return impl_->probe( endpoint, std::move( callback ) );
}

void AdbSmartSocketServerProbe::cancel( AdbServerToken token )
{
    impl_->cancel( token );
}

class QtAdbServerLauncher::Impl final {
public:
    explicit Impl( QtAdbServerLauncher& owner )
        : owner_( owner )
    {
    }

    ~Impl()
    {
        for ( auto& entry : launches_ ) {
            if ( entry.second.process != nullptr ) {
                entry.second.process->kill();
            }
        }
    }

    AdbServerToken launch( const AdbServerLaunchRequest& request, Callback callback )
    {
        const auto token = ++nextToken_;
        const QFileInfo executableInfo( request.executable );
        const auto resolvedExecutable = executableInfo.canonicalFilePath();
        const QStringList officialArguments{ QStringLiteral( "server" ),
                                             QStringLiteral( "nodaemon" ) };
        if ( request.allowPathLookup || !executableInfo.isAbsolute() || resolvedExecutable.isEmpty()
             || !executableInfo.isFile() || !executableInfo.isExecutable()
             || request.endpoint.address != QHostAddress::LocalHost
             || request.endpoint.port != 5037u || request.arguments != officialArguments ) {
            QTimer::singleShot( 0, &owner_, [ callback = std::move( callback ) ]() mutable {
                callback( AdbServerLaunchResult{
                    AdbServerLaunchState::Failed,
                    false,
                    "Packaged ADB launch requires an explicit executable and official "
                    "loopback server mode.",
                } );
            } );
            return token;
        }

        // Ownership is transferred to the launcher's Qt object tree.
        // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
        auto* const process = new QProcess( &owner_ );
        Launch launch;
        launch.process = process;
        launch.stderrTail = std::make_shared<QByteArray>();
        launch.callback = std::move( callback );
        const auto stderrTail = launch.stderrTail;
        launches_.emplace( token, std::move( launch ) );

        auto environment = QProcessEnvironment::systemEnvironment();
        environment.remove( QStringLiteral( "ADB_VENDOR_KEYS" ) );
        environment.remove( QStringLiteral( "ADB_SERVER_PORT" ) );
        environment.remove( QStringLiteral( "ANDROID_ADB_SERVER_PORT" ) );
        environment.remove( QStringLiteral( "ANDROID_ADB_SERVER_ADDRESS" ) );
        environment.insert( QStringLiteral( "ADB_SERVER_SOCKET" ),
                            endpointSocket( request.endpoint ) );
        process->setProcessEnvironment( environment );
        process->setProgram( resolvedExecutable );
        process->setArguments( officialArguments );
        process->setStandardOutputFile( QProcess::nullDevice() );
        process->setProcessChannelMode( QProcess::SeparateChannels );

        // The process is the connection context so drainage survives release() and
        // launcher destruction. Retaining only the bounded tail preserves fatal
        // startup diagnostics while continuing to empty the pipe indefinitely.
        QObject::connect( process, &QProcess::readyReadStandardError, process,
                          [ process, stderrTail ] { drainStderr( *process, *stderrTail ); } );
        QObject::connect( process, &QProcess::started, &owner_, [ this, token ] {
            const auto found = launches_.find( token );
            if ( found == launches_.end() ) {
                return;
            }
            found->second.started = true;
            found->second.callback(
                AdbServerLaunchResult{ AdbServerLaunchState::Started, true, {} } );
        } );
        QObject::connect( process, &QProcess::errorOccurred, &owner_,
                          [ this, token ]( QProcess::ProcessError error ) {
                              if ( error == QProcess::FailedToStart ) {
                                  finish( token, AdbServerLaunchState::Failed, false );
                              }
                          } );
        QObject::connect(
            process, qOverload<int, QProcess::ExitStatus>( &QProcess::finished ), &owner_,
            [ this, token ]( int exitCode, QProcess::ExitStatus exitStatus ) {
                const auto found = launches_.find( token );
                if ( found == launches_.end() ) {
                    return;
                }
                const auto detail
                    = QStringLiteral( "Packaged ADB server exited with code %1 (%2)." )
                          .arg( exitCode )
                          .arg( exitStatus == QProcess::NormalExit ? QStringLiteral( "normal" )
                                                                   : QStringLiteral( "crashed" ) )
                          .toStdString();
                finish( token,
                        found->second.started ? AdbServerLaunchState::Exited
                                              : AdbServerLaunchState::Failed,
                        found->second.started, detail );
            } );
        process->start( QIODevice::ReadOnly );
        return token;
    }

    void cleanup( AdbServerToken token )
    {
        const auto found = launches_.find( token );
        if ( found == launches_.end() ) {
            return;
        }
        auto* const process = found->second.process.data();
        launches_.erase( found );
        if ( process != nullptr ) {
            QObject::disconnect( process, nullptr, &owner_, nullptr );
            process->kill();
            process->deleteLater();
        }
    }

    void release( AdbServerToken token )
    {
        const auto found = launches_.find( token );
        if ( found == launches_.end() ) {
            return;
        }
        auto* const process = found->second.process.data();
        const auto stderrTail = found->second.stderrTail;
        launches_.erase( found );
        if ( process == nullptr ) {
            return;
        }

        QObject::disconnect( process, nullptr, &owner_, nullptr );
        process->setParent( nullptr );
        publishedProcesses().insert( process );
        QObject::connect( process, qOverload<int, QProcess::ExitStatus>( &QProcess::finished ),
                          process, [ process, stderrTail ]( int, QProcess::ExitStatus ) {
                              drainStderr( *process, *stderrTail );
                              publishedProcesses().erase( process );
                              process->deleteLater();
                          } );
        if ( process->state() == QProcess::NotRunning ) {
            publishedProcesses().erase( process );
            process->deleteLater();
        }
    }

private:
    struct Launch {
        QPointer<QProcess> process;
        std::shared_ptr<QByteArray> stderrTail;
        Callback callback;
        bool started{ false };
    };

    void finish( AdbServerToken token, AdbServerLaunchState state, bool cleanupPermitted,
                 std::string diagnostic = {} )
    {
        const auto found = launches_.find( token );
        if ( found == launches_.end() ) {
            return;
        }
        auto callback = std::move( found->second.callback );
        auto* const process = found->second.process.data();
        if ( process != nullptr ) {
            drainStderr( *process, *found->second.stderrTail );
        }
        diagnostic = appendStderrDiagnostic( std::move( diagnostic ), *found->second.stderrTail );
        launches_.erase( found );
        if ( process != nullptr ) {
            QObject::disconnect( process, nullptr, &owner_, nullptr );
            process->deleteLater();
        }
        callback( AdbServerLaunchResult{ state, cleanupPermitted, std::move( diagnostic ) } );
    }

private:
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members)
    QtAdbServerLauncher& owner_;
    std::map<AdbServerToken, Launch> launches_;
    AdbServerToken nextToken_{ 0 };
};

QtAdbServerLauncher::QtAdbServerLauncher( QObject* parent )
    : QObject( parent )
    , impl_( std::make_unique<Impl>( *this ) )
{
}

QtAdbServerLauncher::~QtAdbServerLauncher() = default;

AdbServerToken QtAdbServerLauncher::launch( const AdbServerLaunchRequest& request,
                                            Callback callback )
{
    return impl_->launch( request, std::move( callback ) );
}

void QtAdbServerLauncher::cleanup( AdbServerToken token )
{
    impl_->cleanup( token );
}

void QtAdbServerLauncher::release( AdbServerToken token )
{
    impl_->release( token );
}

class QtAdbServerStartupLock::Impl final {
public:
    explicit Impl( QtAdbServerStartupLock& owner )
        : owner_( owner )
    {
    }

    AdbServerToken acquire( const QString& lockPath, Callback callback )
    {
        const auto token = ++nextToken_;
        requests_.emplace( token, Request{ lockPath, std::move( callback ), nullptr } );
        QTimer::singleShot( 0, &owner_, [ this, token ] { tryAcquire( token ); } );
        return token;
    }

    void cancel( AdbServerToken token )
    {
        requests_.erase( token );
    }

    void release( AdbServerToken token )
    {
        const auto found = requests_.find( token );
        if ( found == requests_.end() ) {
            return;
        }
        if ( found->second.lock != nullptr ) {
            found->second.lock->unlock();
        }
        requests_.erase( found );
    }

private:
    struct Request {
        QString path;
        Callback callback;
        std::unique_ptr<QLockFile> lock;
    };

    void tryAcquire( AdbServerToken token )
    {
        const auto found = requests_.find( token );
        if ( found == requests_.end() ) {
            return;
        }

        const QFileInfo lockInfo( found->second.path );
        if ( !lockInfo.isAbsolute() ) {
            auto callback = std::move( found->second.callback );
            requests_.erase( found );
            callback( AdbServerStartupLockResult{
                AdbServerStartupLockState::Failed,
                "ADB startup lock path must be absolute.",
            } );
            return;
        }

        const auto parentPath = lockInfo.absolutePath();
        if ( !QDir().mkpath( parentPath ) ) {
            auto callback = std::move( found->second.callback );
            requests_.erase( found );
            callback( AdbServerStartupLockResult{
                AdbServerStartupLockState::Failed,
                "Unable to create the ADB startup lock directory.",
            } );
            return;
        }

        auto lock = std::make_unique<QLockFile>( lockInfo.absoluteFilePath() );
        // Consent can keep the startup lease held for an arbitrary duration.
        // Never expire a live owner's lock by age; QLockFile still removes locks
        // whose owning process no longer exists.
        lock->setStaleLockTime( 0 );
        const auto acquired = lock->tryLock( 0 );
        auto callback = std::move( found->second.callback );
        if ( acquired ) {
            found->second.lock = std::move( lock );
            callback( AdbServerStartupLockResult{ AdbServerStartupLockState::Acquired, {} } );
        }
        else {
            const auto lockError = lock->error();
            requests_.erase( found );
            if ( lockError == QLockFile::LockFailedError ) {
                callback( AdbServerStartupLockResult{
                    AdbServerStartupLockState::Contended,
                    "Another process holds the ADB startup lock.",
                } );
            }
            else {
                callback( AdbServerStartupLockResult{
                    AdbServerStartupLockState::Failed,
                    lockError == QLockFile::PermissionError
                        ? "Permission denied while acquiring the ADB startup lock."
                        : "Unable to acquire the ADB startup lock.",
                } );
            }
        }
    }

private:
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members)
    QtAdbServerStartupLock& owner_;
    std::map<AdbServerToken, Request> requests_;
    AdbServerToken nextToken_{ 0 };
};

QtAdbServerStartupLock::QtAdbServerStartupLock( QObject* parent )
    : QObject( parent )
    , impl_( std::make_unique<Impl>( *this ) )
{
}

QtAdbServerStartupLock::~QtAdbServerStartupLock() = default;

AdbServerToken QtAdbServerStartupLock::acquire( const QString& lockPath, Callback callback )
{
    return impl_->acquire( lockPath, std::move( callback ) );
}

void QtAdbServerStartupLock::cancel( AdbServerToken token )
{
    impl_->cancel( token );
}

void QtAdbServerStartupLock::release( AdbServerToken token )
{
    impl_->release( token );
}

StandardAdbKeyStore::StandardAdbKeyStore( QString privateKeyPath )
    : privateKeyPath_( privateKeyPath.isEmpty() ? standardPrivateKeyPath()
                                                : std::move( privateKeyPath ) )
{
}

AdbServerKeyInspection StandardAdbKeyStore::inspectStandardKey()
{
    const QFileInfo keyInfo( privateKeyPath_ );
    if ( !keyInfo.isAbsolute() ) {
        return AdbServerKeyInspection{ AdbServerStandardKeyState::Failed,
                                       "The standard ADB private key path is not absolute." };
    }
    if ( keyInfo.exists() ) {
        if ( !keyInfo.isFile() || !keyInfo.isReadable() ) {
            return AdbServerKeyInspection{ AdbServerStandardKeyState::Failed,
                                           "The standard ADB private key is not a readable file." };
        }
        if ( !klogg::platform::ensureOwnerOnlyDirectory( keyInfo.absolutePath() )
             || !klogg::platform::restrictRegularFileToOwner( privateKeyPath_ ) ) {
            return AdbServerKeyInspection{
                AdbServerStandardKeyState::Failed,
                "Unable to restrict the standard ADB private key to owner-only access.",
            };
        }
        return AdbServerKeyInspection{ AdbServerStandardKeyState::Present, {} };
    }
    return AdbServerKeyInspection{ AdbServerStandardKeyState::Absent, {} };
}

AdbServerKeyGenerationResult StandardAdbKeyStore::generateStandardKey()
{
    const auto inspection = inspectStandardKey();
    if ( inspection.state == AdbServerStandardKeyState::Present ) {
        return AdbServerKeyGenerationResult{ true, {} };
    }
    if ( inspection.state == AdbServerStandardKeyState::Failed ) {
        return AdbServerKeyGenerationResult{ false, inspection.diagnostic };
    }

    const QFileInfo keyInfo( privateKeyPath_ );
    if ( !klogg::platform::ensureOwnerOnlyDirectory( keyInfo.absolutePath() ) ) {
        return AdbServerKeyGenerationResult{ false,
                                             "Unable to secure the standard ADB key directory." };
    }

    // The official server creates ~/.android/adbkey when started. Reaching
    // this point records that the caller explicitly consented and that the
    // standard directory is ready; no alternate key path or helper command is
    // introduced.
    return AdbServerKeyGenerationResult{ true, {} };
}

class QtAdbServerScheduler::Impl final {
public:
    explicit Impl( QtAdbServerScheduler& owner )
        : owner_( owner )
    {
    }

    ~Impl()
    {
        for ( const auto& entry : timers_ ) {
            entry.second->stop();
        }
    }

    AdbServerToken schedule( AdbServerScheduleKind, std::chrono::milliseconds delay,
                             Callback callback )
    {
        const auto token = ++nextToken_;
        // Ownership is transferred to the scheduler's Qt object tree.
        // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
        auto* const timer = new QTimer( &owner_ );
        timer->setSingleShot( true );
        timer->setTimerType( Qt::PreciseTimer );
        timers_.emplace( token, timer );
        QObject::connect( timer, &QTimer::timeout, &owner_,
                          [ this, token, callback = std::move( callback ) ]() mutable {
                              const auto found = timers_.find( token );
                              if ( found == timers_.end() ) {
                                  return;
                              }
                              auto* const expired = found->second.data();
                              timers_.erase( found );
                              if ( expired != nullptr ) {
                                  expired->deleteLater();
                              }
                              callback();
                          } );
        const auto boundedDelay
            = std::clamp<std::int64_t>( delay.count(), 0, std::numeric_limits<int>::max() );
        timer->start( static_cast<int>( boundedDelay ) );
        return token;
    }

    void cancel( AdbServerToken token )
    {
        const auto found = timers_.find( token );
        if ( found == timers_.end() ) {
            return;
        }
        auto* const timer = found->second.data();
        timers_.erase( found );
        if ( timer != nullptr ) {
            timer->stop();
            timer->deleteLater();
        }
    }

private:
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members)
    QtAdbServerScheduler& owner_;
    std::map<AdbServerToken, QPointer<QTimer>> timers_;
    AdbServerToken nextToken_{ 0 };
};

QtAdbServerScheduler::QtAdbServerScheduler( QObject* parent )
    : QObject( parent )
    , impl_( std::make_unique<Impl>( *this ) )
{
}

QtAdbServerScheduler::~QtAdbServerScheduler() = default;

AdbServerToken QtAdbServerScheduler::schedule( AdbServerScheduleKind kind,
                                               std::chrono::milliseconds delay, Callback callback )
{
    return impl_->schedule( kind, delay, std::move( callback ) );
}

void QtAdbServerScheduler::cancel( AdbServerToken token )
{
    impl_->cancel( token );
}

} // namespace klogg::livecapture::adb
