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

#include "adbliveservices.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QPointer>
#include <QTcpSocket>
#include <QTimer>

#include <algorithm>
#include <optional>
#include <utility>

#include "adbsmartsockettransport.h"
#include "adbtrackeddeviceprovider.h"
#include "livelogcontroller.h"

namespace {

using klogg::livecapture::ErrorCategory;
using klogg::livecapture::ErrorScope;
using klogg::livecapture::Generation;
using klogg::livecapture::InfrastructureStatus;
using klogg::livecapture::LiveSourceError;
using klogg::livecapture::RetryPolicy;
using namespace klogg::livecapture::adb;

class QtSocketFactory final : public AdbSmartSocketFactory {
public:
    QTcpSocket* createSocket( QObject* parent ) override
    {
        // Ownership is transferred to the supplied Qt object tree.
        // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
        return new QTcpSocket( parent );
    }
};

class QtDeadlineScheduler final : public AdbSmartSocketDeadlineScheduler {
public:
    ~QtDeadlineScheduler() override
    {
        for ( const auto& timer : timers_ ) {
            if ( timer != nullptr ) {
                timer->stop();
            }
        }
    }

    DeadlineToken armDeadline( AdbSmartSocketDeadlineKind, int timeoutMs, QObject* context,
                               std::function<void()> callback ) override
    {
        const auto token = ++nextToken_;
        // Ownership is transferred to the supplied deadline context.
        // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
        auto* const timer = new QTimer( context );
        timer->setSingleShot( true );
        timer->setTimerType( Qt::PreciseTimer );
        timers_.insert( token, timer );
        QObject::connect( timer, &QTimer::timeout, context,
                          [ this, token, callback = std::move( callback ) ]() mutable {
                              auto expired = timers_.take( token );
                              if ( expired != nullptr ) {
                                  expired->deleteLater();
                              }
                              callback();
                          } );
        timer->start( std::max( timeoutMs, 0 ) );
        return token;
    }

    void cancelDeadline( DeadlineToken token ) override
    {
        auto timer = timers_.take( token );
        if ( timer != nullptr ) {
            timer->stop();
            timer->deleteLater();
        }
    }

private:
    DeadlineToken nextToken_{ 0 };
    QHash<DeadlineToken, QPointer<QTimer>> timers_;
};

struct OwnedDependencies {
    AdbSmartSocketServerProbe probe;
    QtAdbServerLauncher launcher;
    QtAdbServerStartupLock startupLock;
    StandardAdbKeyStore keyStore;
    QtAdbServerScheduler supervisorScheduler;
    QtSocketFactory trackerSocketFactory;
    QtDeadlineScheduler trackerDeadlineScheduler;
    QtAdbServerScheduler trackerScheduler;
    QtSocketFactory transportSocketFactory;
    QtDeadlineScheduler transportDeadlineScheduler;
};

AdbSmartSocketClientConfig clientConfig( const AdbLiveServicesConfig& config )
{
    AdbSmartSocketClientConfig result;
    result.serverAddress = config.endpoint.address;
    result.serverPort = config.endpoint.port;
    return result;
}

QString packagedHelperFileName()
{
#ifdef Q_OS_WIN
    return QStringLiteral( "adb.exe" );
#else
    return QStringLiteral( "adb" );
#endif
}

LiveSourceError missingHelperError( const QString& helperPath )
{
    const auto detail
        = QStringLiteral(
              "The packaged ADB helper is missing, not executable, or escapes its package: %1" )
              .arg( helperPath )
              .toStdString();
    return LiveSourceError{ ErrorCategory::Configuration,
                            "adb-packaged-helper-missing",
                            ErrorScope::Infrastructure,
                            RetryPolicy::Never,
                            "The packaged ADB helper is unavailable.",
                            detail };
}

bool isValidPackagedHelper( const QString& applicationDirPath, const QString& helperPath )
{
    const QFileInfo helper( helperPath );
    if ( !helper.exists() || !helper.isFile() || !helper.isExecutable() || helper.isSymLink() ) {
        return false;
    }

    const auto canonicalApplicationDir = QFileInfo( applicationDirPath ).canonicalFilePath();
    const auto canonicalHelper = helper.canonicalFilePath();
    if ( canonicalApplicationDir.isEmpty() || canonicalHelper.isEmpty() ) {
        return false;
    }

    const auto packageRelative
        = QDir( canonicalApplicationDir ).relativeFilePath( canonicalHelper );
    return QDir::cleanPath( packageRelative )
           == QDir::cleanPath(
               QDir( QStringLiteral( "helpers" ) ).filePath( packagedHelperFileName() ) );
}

QString diagnosticText( const LiveSourceError& error )
{
    auto result = QString::fromStdString( error.message );
    const auto detail = QString::fromStdString( error.nativeDetail );
    if ( !detail.isEmpty() && detail != result ) {
        if ( !result.isEmpty() ) {
            result.append( QLatin1Char( '\n' ) );
        }
        result.append( detail );
    }
    return result;
}

AdbInfrastructureManagerConfig managerConfig( const AdbLiveServicesConfig& config,
                                              const QString& helperPath, const QString& lockPath )
{
    AdbInfrastructureManagerConfig result;
    result.server.endpoint = config.endpoint;
    result.server.packagedServerPath = helperPath;
    result.server.lockPath = lockPath;
    result.server.minimumProtocolVersion = config.minimumProtocolVersion;
    result.server.requiredFeatures = config.requiredFeatures;
    result.server.readinessProbeInterval = config.readinessProbeInterval;
    result.server.startupTimeout = config.startupTimeout;
    result.server.healthProbeInterval = config.healthProbeInterval;
    result.server.reconnectBackoff = config.serverReconnectBackoff;
    if ( !isValidPackagedHelper( config.applicationDirPath, helperPath ) ) {
        result.server.configurationError = missingHelperError( helperPath );
    }
    result.tracker.reconnectBackoff = config.trackerReconnectBackoff;
    return result;
}

struct ManagedTransportState;
class ManagedAdbSmartSocketTransport;

struct ManagedTransportState {
    AdbInfrastructureManager* manager{ nullptr };
    AdbSmartSocketFactory* socketFactory{ nullptr };
    AdbSmartSocketDeadlineScheduler* deadlineScheduler{ nullptr };
    const LiveSourceTransportFactory* managedTransportFactory{ nullptr };
    AdbServerEndpoint endpoint;
    QObject* retirementOwner{ nullptr };
    std::vector<ManagedAdbSmartSocketTransport*> transports;
    bool shuttingDown{ false };
};

class ManagedAdbSmartSocketTransport final : public LiveSourceTransport {
public:
    ManagedAdbSmartSocketTransport( LiveSourceTransportConfig config,
                                    const std::shared_ptr<ManagedTransportState>& state )
        : config_( std::move( config ) )
        , sharedState_( state )
    {
        if ( const auto shared = sharedState_.lock(); shared != nullptr ) {
            shared->transports.push_back( this );
            if ( shared->manager != nullptr ) {
                managerConnection_ = QObject::connect(
                    shared->manager, &AdbInfrastructureManager::snapshotChanged, this,
                    [ this ]( const AdbInfrastructureSnapshot& snapshot ) {
                        managerSnapshotChanged( snapshot );
                    } );
            }
        }
    }

    ~ManagedAdbSmartSocketTransport() override
    {
        // Destructors must be signal-silent: observers commonly have shorter
        // lexical lifetimes than the transport owner. Explicit service shutdown
        // publishes Disconnected, while ordinary destruction only retires work.
        QObject::disconnect( managerConnection_ );
        abandonActiveRun();
        if ( const auto shared = sharedState_.lock(); shared != nullptr ) {
            const auto found
                = std::find( shared->transports.begin(), shared->transports.end(), this );
            if ( found != shared->transports.end() ) {
                shared->transports.erase( found );
            }
        }
    }

    void start( Generation generation ) override
    {
        if ( activeGeneration_ == generation ) {
            return;
        }

        stopActiveRun();
        accumulatedStatistics_ = {};
        accumulatedStatistics_.generation = generation;
        activeGeneration_ = generation;
        expectedManagerGeneration_ = 0;
        activeInfrastructureEpoch_ = 0;
        lastError_.clear();

        const auto shared = sharedState_.lock();
        if ( shared == nullptr || shared->shuttingDown || shared->manager == nullptr ) {
            fail( generation, QStringLiteral( "Shared ADB services are unavailable." ) );
            return;
        }

        QPointer<ManagedAdbSmartSocketTransport> guard( this );
        emitState( generation, State::Connecting );
        if ( guard.isNull() || activeGeneration_ != generation ) {
            return;
        }

        auto lease = shared->manager->acquireLease();
        if ( guard.isNull() || activeGeneration_ != generation ) {
            return;
        }
        if ( !lease ) {
            fail( generation, QStringLiteral( "Shared ADB services are unavailable." ) );
            return;
        }

        lease_ = std::move( lease );
        expectedManagerGeneration_ = shared->manager->snapshot().generation;
        managerSnapshotChanged( shared->manager->snapshot() );
    }

    void stop( Generation generation ) override
    {
        if ( activeGeneration_ != generation ) {
            return;
        }

        activeGeneration_.reset();
        lease_.reset();
        expectedManagerGeneration_ = 0;
        activeInfrastructureEpoch_ = 0;
        if ( transport_ != nullptr ) {
            transport_->stop( generation );
        }
        emitState( generation, State::Disconnected );
    }

    void clearRemoteAsync( Generation generation, ClearRequestId requestId ) override
    {
        if ( transport_ != nullptr ) {
            transport_->clearRemoteAsync( generation, requestId );
            return;
        }
        Q_EMIT clearRemoteFinished( generation, requestId, false,
                                    QStringLiteral( "Shared ADB infrastructure is not ready." ) );
    }

    QString lastError() const override
    {
        return lastError_;
    }

    klogg::livecapture::LiveDataStatistics statistics() const override
    {
        auto result = accumulatedStatistics_;
        if ( transport_ != nullptr ) {
            klogg::livecapture::accumulateLiveDataStatistics( result, transport_->statistics() );
        }
        return result;
    }

    void serviceShutdown()
    {
        QObject::disconnect( managerConnection_ );
        const auto generation = activeGeneration_;
        destroyActiveRun();
        if ( generation.has_value() ) {
            emitState( *generation, State::Disconnected );
        }
    }

private:
    void managerSnapshotChanged( const AdbInfrastructureSnapshot& snapshot )
    {
        if ( !activeGeneration_.has_value() || expectedManagerGeneration_ == 0u
             || snapshot.generation != expectedManagerGeneration_ ) {
            return;
        }
        const auto generation = activeGeneration_.value();

        if ( snapshot.infrastructure.status == InfrastructureStatus::Ready ) {
            if ( transport_ == nullptr
                 || activeInfrastructureEpoch_ != snapshot.infrastructureEpoch ) {
                startInfrastructureEpoch( snapshot.infrastructureEpoch );
            }
            return;
        }

        if ( transport_ != nullptr ) {
            retireTransport();
            activeInfrastructureEpoch_ = 0;
        }

        if ( snapshot.error.has_value() && snapshot.error->retryPolicy == RetryPolicy::Never ) {
            fail( generation, diagnosticText( *snapshot.error ) );
            return;
        }

        emitState( generation, State::Connecting );
    }

    void startInfrastructureEpoch( std::uint64_t infrastructureEpoch )
    {
        const auto shared = sharedState_.lock();
        if ( shared == nullptr || shared->shuttingDown || !activeGeneration_.has_value()
             || ( shared->managedTransportFactory == nullptr
                  && ( shared->socketFactory == nullptr
                       || shared->deadlineScheduler == nullptr ) ) ) {
            return;
        }
        const auto generation = activeGeneration_.value();

        retireTransport();
        activeInfrastructureEpoch_ = infrastructureEpoch;
        if ( shared->managedTransportFactory != nullptr ) {
            transport_ = shared->managedTransportFactory->create( config_ );
        }
        else {
            auto transportConfig
                = klogg::livelog::makeAdbSmartSocketTransportConfig( config_ );
            if ( !transportConfig.has_value() ) {
                fail( generation, QStringLiteral( "Invalid typed Android logcat options." ) );
                return;
            }
            transportConfig->clientConfig.serverAddress = shared->endpoint.address;
            transportConfig->clientConfig.serverPort = shared->endpoint.port;
            transport_ = std::make_unique<AdbSmartSocketTransport>(
                std::move( *transportConfig ), *shared->socketFactory,
                *shared->deadlineScheduler );
        }
        if ( transport_ == nullptr ) {
            fail( generation, QStringLiteral( "Unable to create the ADB transport." ) );
            return;
        }
        connectTransport();
        // Opening a newly-ready infrastructure epoch is a distinct connection
        // attempt even when the managed wrapper was already waiting in
        // Connecting. Re-publish the source-correlated state for observers that
        // attached while readiness was gated.
        stateGeneration_.reset();
        transport_->start( generation );
    }

    void connectTransport()
    {
        QObject::connect( transport_.get(), &LiveSourceTransport::bytesReceived, this,
                          [ this ]( Generation generation, const QByteArray& bytes ) {
                              if ( activeGeneration_ == generation ) {
                                  Q_EMIT bytesReceived( generation, bytes );
                              }
                          } );
        QObject::connect( transport_.get(), &LiveSourceTransport::clearRemoteFinished, this,
                          &LiveSourceTransport::clearRemoteFinished );
        QObject::connect( transport_.get(), &LiveSourceTransport::stateChanged, this,
                          [ this ]( Generation generation, State state ) {
                              if ( activeGeneration_ != generation ) {
                                  return;
                              }
                              if ( state == State::Error ) {
                                  lastError_ = transport_->lastError();
                              }
                              emitState( generation, state );
                          } );
        QObject::connect( transport_.get(), &LiveSourceTransport::errorOccurred, this,
                          [ this ]( Generation generation, const QString& error ) {
                              if ( activeGeneration_ != generation ) {
                                  return;
                              }
                              lastError_ = error;
                              Q_EMIT errorOccurred( generation, error );
                          } );
    }

    void retireTransport()
    {
        if ( transport_ == nullptr ) {
            return;
        }
        klogg::livecapture::accumulateLiveDataStatistics( accumulatedStatistics_,
                                                          transport_->statistics() );
        QObject::disconnect( transport_.get(), nullptr, this, nullptr );
        if ( activeGeneration_.has_value() ) {
            transport_->stop( *activeGeneration_ );
        }
        // Infrastructure replacement is delivered synchronously from the manager.
        // Keep the stopped QObject tree alive until this wrapper itself retires so
        // Qt never destroys a socket/client subtree while its manager signal stack
        // is still unwinding.
        retiredTransports_.push_back( std::move( transport_ ) );
        if ( !retiredCleanupScheduled_ ) {
            retiredCleanupScheduled_ = true;
            QTimer::singleShot( 0, this, [ this ] {
                retiredCleanupScheduled_ = false;
                retiredTransports_.clear();
            } );
        }
    }

    void stopActiveRun()
    {
        retireTransport();
        lease_.reset();
        activeGeneration_.reset();
        expectedManagerGeneration_ = 0;
        activeInfrastructureEpoch_ = 0;
    }

    void destroyActiveRun()
    {
        const auto generation = activeGeneration_;
        activeGeneration_.reset();
        lease_.reset();
        expectedManagerGeneration_ = 0;
        activeInfrastructureEpoch_ = 0;
        if ( transport_ != nullptr ) {
            klogg::livecapture::accumulateLiveDataStatistics( accumulatedStatistics_,
                                                              transport_->statistics() );
            QObject::disconnect( transport_.get(), nullptr, this, nullptr );
            if ( generation.has_value() ) {
                transport_->stop( *generation );
            }
            transport_.reset();
        }
        retiredTransports_.clear();
    }

    void abandonActiveRun() noexcept
    {
        activeGeneration_.reset();
        lease_.reset();
        expectedManagerGeneration_ = 0;
        activeInfrastructureEpoch_ = 0;
        if ( transport_ != nullptr ) {
            QObject::disconnect( transport_.get(), nullptr, this, nullptr );
            deferDestruction( std::move( transport_ ) );
        }
        for ( auto& retired : retiredTransports_ ) {
            deferDestruction( std::move( retired ) );
        }
        retiredTransports_.clear();
    }

    void deferDestruction( std::unique_ptr<LiveSourceTransport> transport ) noexcept
    {
        if ( transport == nullptr ) {
            return;
        }

        auto* const retired = transport.release();
        const auto shared = sharedState_.lock();
        if ( shared == nullptr || shared->shuttingDown || shared->retirementOwner == nullptr ) {
            // Ownership was released above for deferred QObject lifetime management.
            // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
            delete retired;
            return;
        }

        retired->setParent( shared->retirementOwner );
        retired->deleteLater();
    }

    void fail( Generation generation, QString error )
    {
        if ( activeGeneration_ != generation ) {
            return;
        }
        lastError_ = std::move( error );
        const auto terminalError = lastError_;
        QPointer<ManagedAdbSmartSocketTransport> guard( this );
        emitState( generation, State::Error );
        if ( !guard.isNull() && activeGeneration_ == generation && stateGeneration_ == generation
             && state_ == State::Error ) {
            Q_EMIT errorOccurred( generation, terminalError );
        }
    }

    void emitState( Generation generation, State state )
    {
        if ( stateGeneration_ == generation && state_ == state ) {
            return;
        }
        stateGeneration_ = generation;
        state_ = state;
        Q_EMIT stateChanged( generation, state );
    }

    LiveSourceTransportConfig config_;
    std::weak_ptr<ManagedTransportState> sharedState_;
    QMetaObject::Connection managerConnection_;
    AdbInfrastructureLease lease_;
    std::unique_ptr<LiveSourceTransport> transport_;
    std::vector<std::unique_ptr<LiveSourceTransport>> retiredTransports_;
    klogg::livecapture::LiveDataStatistics accumulatedStatistics_;
    std::optional<Generation> activeGeneration_;
    std::optional<Generation> stateGeneration_;
    Generation expectedManagerGeneration_{ 0 };
    std::uint64_t activeInfrastructureEpoch_{ 0 };
    State state_{ State::Disconnected };
    QString lastError_;
    bool retiredCleanupScheduled_{ false };
};

} // namespace

class AdbLiveServices::Impl final {
public:
    Impl( AdbLiveServices& services, AdbLiveServicesConfig config )
        : services_( services )
        , config_( std::move( config ) )
        , helperPath_( AdbLiveServices::packagedHelperPath( config_.applicationDirPath ) )
        , lockPath_( AdbLiveServices::perUserLockPath( config_.perUserRuntimeDir ) )
        , ownedDependencies_( std::make_unique<OwnedDependencies>() )
        , trackerClient_( clientConfig( config_ ), ownedDependencies_->trackerSocketFactory,
                          ownedDependencies_->trackerDeadlineScheduler )
        , manager_( managerConfig( config_, helperPath_, lockPath_ ),
                    AdbInfrastructureManagerDependencies{
                        ownedDependencies_->probe, ownedDependencies_->launcher,
                        ownedDependencies_->startupLock, ownedDependencies_->keyStore,
                        ownedDependencies_->supervisorScheduler, trackerClient_,
                        ownedDependencies_->trackerScheduler } )
        , provider_( manager_ )
        , transportState_( std::make_shared<ManagedTransportState>() )
        , fallbackFactory_( std::make_unique<DefaultLiveSourceTransportFactory>() )
    {
        initialize( ownedDependencies_->transportSocketFactory,
                    ownedDependencies_->transportDeadlineScheduler, nullptr );
    }

    Impl( AdbLiveServices& services, AdbLiveServicesConfig config,
          AdbLiveServicesDependencies dependencies )
        : services_( services )
        , config_( std::move( config ) )
        , helperPath_( AdbLiveServices::packagedHelperPath( config_.applicationDirPath ) )
        , lockPath_( AdbLiveServices::perUserLockPath( config_.perUserRuntimeDir ) )
        , trackerClient_( clientConfig( config_ ), dependencies.trackerSocketFactory,
                          dependencies.trackerDeadlineScheduler )
        , manager_( managerConfig( config_, helperPath_, lockPath_ ),
                    AdbInfrastructureManagerDependencies{
                        dependencies.probe, dependencies.launcher, dependencies.startupLock,
                        dependencies.keyStore, dependencies.supervisorScheduler, trackerClient_,
                        dependencies.trackerScheduler } )
        , provider_( manager_ )
        , transportState_( std::make_shared<ManagedTransportState>() )
        , fallbackFactory_( std::make_unique<DefaultLiveSourceTransportFactory>() )
    {
        initialize( dependencies.transportSocketFactory, dependencies.transportDeadlineScheduler,
                    dependencies.managedTransportFactory );
    }

    ~Impl()
    {
        shutdown();
    }

    std::unique_ptr<LiveSourceTransport> create( const LiveSourceTransportConfig& config ) const
    {
        if ( config.sourceType == LiveLogSourceType::AdbLogcat
             && config.adbBackend == AdbTransportBackend::SmartSocket ) {
            // Managed sessions never fall back to the legacy process backend. A
            // missing or unhealthy packaged helper remains a visible manager error.
            return std::make_unique<ManagedAdbSmartSocketTransport>( config, transportState_ );
        }
        return fallbackFactory_->create( config );
    }

    void answerKeyGenerationConsent( Generation generation, std::uint64_t epoch, bool granted )
    {
        if ( transportState_->shuttingDown || !pendingConsent_.has_value()
             || pendingConsent_->first != generation || pendingConsent_->second != epoch ) {
            return;
        }

        // Consume before calling down: a synchronous supervisor transition or UI
        // callback can re-enter without answering the same prompt twice.
        pendingConsent_.reset();
        manager_.grantKeyGenerationConsent( granted );
    }

    void shutdown()
    {
        if ( transportState_->shuttingDown ) {
            return;
        }
        transportState_->shuttingDown = true;
        pendingConsent_.reset();
        while ( !transportState_->transports.empty() ) {
            auto* const transport = transportState_->transports.back();
            transportState_->transports.pop_back();
            if ( transport != nullptr ) {
                transport->serviceShutdown();
            }
        }
        const auto deferredTransports = retiredTransportOwner_.children();
        for ( auto* const transport : deferredTransports ) {
            // The dedicated QObject parent owns every deferred transport.
            // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
            delete transport;
        }
        manager_.shutdown();
        transportState_->manager = nullptr;
        transportState_->socketFactory = nullptr;
        transportState_->deadlineScheduler = nullptr;
        transportState_->managedTransportFactory = nullptr;
        transportState_->retirementOwner = nullptr;
    }

    bool isPackagedHelperAvailable() const noexcept
    {
        return isValidPackagedHelper( config_.applicationDirPath, helperPath_ );
    }

    AdbLiveServices& services_;
    AdbLiveServicesConfig config_;
    QString helperPath_;
    QString lockPath_;
    std::unique_ptr<OwnedDependencies> ownedDependencies_;
    QObject retiredTransportOwner_;
    AdbSmartSocketClient trackerClient_;
    AdbInfrastructureManager manager_;
    ManagerAdbTrackedDeviceProvider provider_;
    std::shared_ptr<ManagedTransportState> transportState_;
    std::unique_ptr<DefaultLiveSourceTransportFactory> fallbackFactory_;
    std::optional<std::pair<Generation, std::uint64_t>> pendingConsent_;

private:
    void initialize( AdbSmartSocketFactory& transportSocketFactory,
                     AdbSmartSocketDeadlineScheduler& transportDeadlineScheduler,
                     const LiveSourceTransportFactory* managedTransportFactory )
    {
        transportState_->manager = &manager_;
        transportState_->socketFactory = &transportSocketFactory;
        transportState_->deadlineScheduler = &transportDeadlineScheduler;
        transportState_->managedTransportFactory = managedTransportFactory;
        transportState_->endpoint = config_.endpoint;
        transportState_->retirementOwner = &retiredTransportOwner_;
        QObject::connect( &manager_, &AdbInfrastructureManager::keyConsentRequired, &services_,
                          [ this ]( Generation generation, std::uint64_t epoch ) {
                              if ( transportState_->shuttingDown ) {
                                  return;
                              }
                              const auto prompt = std::make_pair( generation, epoch );
                              if ( pendingConsent_ == prompt ) {
                                  return;
                              }
                              pendingConsent_ = prompt;
                              Q_EMIT services_.keyGenerationConsentRequested( generation, epoch );
                          } );
    }
};

AdbLiveServices::AdbLiveServices( AdbLiveServicesConfig config, QObject* parent )
    : QObject( parent )
    , impl_( std::make_unique<Impl>( *this, std::move( config ) ) )
{
}

AdbLiveServices::AdbLiveServices( AdbLiveServicesConfig config,
                                  AdbLiveServicesDependencies dependencies, QObject* parent )
    : QObject( parent )
    , impl_( std::make_unique<Impl>( *this, std::move( config ), dependencies ) )
{
}

AdbLiveServices::~AdbLiveServices() = default;

QString AdbLiveServices::packagedHelperPath( const QString& applicationDirPath )
{
    const auto applicationDir = QFileInfo( applicationDirPath ).absoluteFilePath();
    return QDir::cleanPath(
        QDir( applicationDir )
            .filePath( QStringLiteral( "helpers/%1" ).arg( packagedHelperFileName() ) ) );
}

QString AdbLiveServices::perUserLockPath( const QString& perUserRuntimeDir )
{
    const auto runtimeDir = QFileInfo( perUserRuntimeDir ).absoluteFilePath();
    return QDir::cleanPath(
        QDir( runtimeDir ).filePath( QStringLiteral( "klogg/adb-server-5037.lock" ) ) );
}

bool AdbLiveServices::isPackagedHelperAvailable() const noexcept
{
    return impl_->isPackagedHelperAvailable();
}

AdbInfrastructureManager& AdbLiveServices::manager() noexcept
{
    return impl_->manager_;
}

const AdbInfrastructureManager& AdbLiveServices::manager() const noexcept
{
    return impl_->manager_;
}

AdbTrackedDeviceProvider& AdbLiveServices::trackedDeviceProvider() noexcept
{
    return impl_->provider_;
}

std::unique_ptr<LiveSourceTransport>
AdbLiveServices::create( const LiveSourceTransportConfig& config ) const
{
    return impl_->create( config );
}

void AdbLiveServices::answerKeyGenerationConsent( Generation generation,
                                                  std::uint64_t infrastructureEpoch, bool granted )
{
    impl_->answerKeyGenerationConsent( generation, infrastructureEpoch, granted );
}

void AdbLiveServices::shutdown()
{
    impl_->shutdown();
}
