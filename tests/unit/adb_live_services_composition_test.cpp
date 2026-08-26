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

#include <catch2/catch.hpp>

#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QTcpSocket>
#include <QTemporaryDir>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "adbliveservices.h"
#include "adblogcatdialog.h"
#include "adbtrackeddeviceprovider.h"
#include "livesourcetransport.h"
#include "livestate.h"
#include "mainwindow.h"
#include "session.h"

namespace {

using namespace std::chrono_literals;
using klogg::livecapture::ErrorCategory;
using klogg::livecapture::ErrorScope;
using klogg::livecapture::Generation;
using klogg::livecapture::InfrastructureStatus;
using klogg::livecapture::LiveSourceError;
using klogg::livecapture::RetryPolicy;
using namespace klogg::livecapture::adb;

static_assert( std::is_constructible_v<Session, const LiveSourceTransportFactory&>,
               "KloggApp must inject its ADB-capable transport factory into Session" );
static_assert( std::is_constructible_v<MainWindow, WindowSession, AdbLiveServices&>,
               "KloggApp must inject its owned ADB live services into every MainWindow" );

constexpr std::uint32_t SupportedProtocolVersion = 0x29u;

class ScopedEnvironment final {
public:
    explicit ScopedEnvironment( QByteArray name )
        : name_( std::move( name ) )
        , wasSet_( qEnvironmentVariableIsSet( name_.constData() ) )
        , oldValue_( qgetenv( name_.constData() ) )
    {
    }

    ~ScopedEnvironment()
    {
        if ( wasSet_ ) {
            qputenv( name_.constData(), oldValue_ );
        }
        else {
            qunsetenv( name_.constData() );
        }
    }

private:
    QByteArray name_;
    bool wasSet_{ false };
    QByteArray oldValue_;
};

class ManualProbe final : public AdbServerProbe {
public:
    struct Request {
        AdbServerToken token{ 0 };
        AdbServerEndpoint endpoint;
        Callback callback;
        bool active{ true };
    };

    AdbServerToken probe( const AdbServerEndpoint& endpoint, Callback callback ) override
    {
        const auto token = ++nextToken_;
        requests.push_back( Request{ token, endpoint, std::move( callback ), true } );
        return token;
    }

    void cancel( AdbServerToken token ) override
    {
        for ( auto& request : requests ) {
            if ( request.token == token ) {
                request.active = false;
            }
        }
    }

    void completeReady( std::size_t index, std::string identity = "adb-server:shared" )
    {
        REQUIRE( index < requests.size() );
        REQUIRE( requests.at( index ).active );
        requests.at( index ).active = false;
        requests.at( index ).callback( AdbServerProbeResult{ AdbServerProbeState::Ready,
                                                             SupportedProtocolVersion,
                                                             { "shell_v2", "cmd" },
                                                             std::move( identity ),
                                                             {} } );
    }

    void completeAbsent( std::size_t index )
    {
        REQUIRE( index < requests.size() );
        REQUIRE( requests.at( index ).active );
        requests.at( index ).active = false;
        requests.at( index ).callback(
            AdbServerProbeResult{ AdbServerProbeState::Absent, 0u, {}, {}, "server is absent" } );
    }

    void completeFailed( std::size_t index )
    {
        REQUIRE( index < requests.size() );
        REQUIRE( requests.at( index ).active );
        requests.at( index ).active = false;
        requests.at( index ).callback(
            AdbServerProbeResult{ AdbServerProbeState::Failed, 0u, {}, {}, "probe failed" } );
    }

    void repeatReadyEvenIfCancelled( std::size_t index )
    {
        REQUIRE( index < requests.size() );
        requests.at( index ).callback( AdbServerProbeResult{ AdbServerProbeState::Ready,
                                                             SupportedProtocolVersion,
                                                             { "shell_v2", "cmd" },
                                                             "adb-server:stale",
                                                             {} } );
    }

    void repeatAbsentEvenIfCancelled( std::size_t index )
    {
        REQUIRE( index < requests.size() );
        requests.at( index ).callback(
            AdbServerProbeResult{ AdbServerProbeState::Absent, 0u, {}, {}, "server is absent" } );
    }

    std::vector<Request> requests;

private:
    AdbServerToken nextToken_{ 0 };
};

class RecordingLauncher final : public AdbServerLauncher {
public:
    struct Request {
        AdbServerToken token{ 0 };
        AdbServerLaunchRequest request;
        Callback callback;
    };

    AdbServerToken launch( const AdbServerLaunchRequest& request, Callback callback ) override
    {
        const auto token = ++nextToken_;
        requests.push_back( Request{ token, request, std::move( callback ) } );
        return token;
    }

    void cleanup( AdbServerToken token ) override
    {
        cleanupTokens.push_back( token );
    }

    void release( AdbServerToken token ) override
    {
        releasedTokens.push_back( token );
    }

    std::vector<Request> requests;
    std::vector<AdbServerToken> cleanupTokens;
    std::vector<AdbServerToken> releasedTokens;

private:
    AdbServerToken nextToken_{ 0 };
};

class ManualStartupLock final : public AdbServerStartupLock {
public:
    struct Request {
        AdbServerToken token{ 0 };
        QString path;
        Callback callback;
        bool active{ true };
    };

    AdbServerToken acquire( const QString& path, Callback callback ) override
    {
        const auto token = ++nextToken_;
        requests.push_back( Request{ token, path, std::move( callback ), true } );
        return token;
    }

    void cancel( AdbServerToken token ) override
    {
        for ( auto& request : requests ) {
            if ( request.token == token ) {
                request.active = false;
            }
        }
    }

    void release( AdbServerToken token ) override
    {
        releasedTokens.push_back( token );
    }

    void completeAcquired( std::size_t index )
    {
        REQUIRE( index < requests.size() );
        REQUIRE( requests.at( index ).active );
        requests.at( index ).active = false;
        requests.at( index ).callback(
            AdbServerStartupLockResult{ AdbServerStartupLockState::Acquired, {} } );
    }

    std::vector<Request> requests;
    std::vector<AdbServerToken> releasedTokens;

private:
    AdbServerToken nextToken_{ 0 };
};

class FakeKeyStore final : public AdbKeyStore {
public:
    AdbServerKeyInspection inspectStandardKey() override
    {
        ++inspectionCount;
        return inspection;
    }

    AdbServerKeyGenerationResult generateStandardKey() override
    {
        ++generationCount;
        return generationResult;
    }

    AdbServerKeyInspection inspection{ AdbServerStandardKeyState::Present, {} };
    AdbServerKeyGenerationResult generationResult{ true, {} };
    int inspectionCount{ 0 };
    int generationCount{ 0 };
};

class ManualServerScheduler final : public AdbServerScheduler {
public:
    struct Entry {
        AdbServerToken token{ 0 };
        AdbServerScheduleKind kind{ AdbServerScheduleKind::ReadinessProbe };
        std::chrono::milliseconds delay{ 0 };
        Callback callback;
        bool active{ true };
    };

    AdbServerToken schedule( AdbServerScheduleKind kind, std::chrono::milliseconds delay,
                             Callback callback ) override
    {
        const auto token = ++nextToken_;
        entries.push_back( Entry{ token, kind, delay, std::move( callback ), true } );
        return token;
    }

    void cancel( AdbServerToken token ) override
    {
        for ( auto& entry : entries ) {
            if ( entry.token == token ) {
                entry.active = false;
            }
        }
    }

    void fire( AdbServerScheduleKind kind )
    {
        const auto found
            = std::find_if( entries.rbegin(), entries.rend(), [ kind ]( const Entry& entry ) {
                  return entry.active && entry.kind == kind;
              } );
        REQUIRE( found != entries.rend() );
        found->active = false;
        found->callback();
    }

    std::vector<Entry> entries;

private:
    AdbServerToken nextToken_{ 0 };
};

class RecordingSocketFactory final : public AdbSmartSocketFactory {
public:
    QTcpSocket* createSocket( QObject* parent ) override
    {
        ++creationCount;
        parents.push_back( parent );
        // Ownership is transferred to the supplied Qt parent.
        // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
        return new QTcpSocket( parent );
    }

    int creationCount{ 0 };
    std::vector<QPointer<QObject>> parents;
};

class PassiveDeadlineScheduler final : public AdbSmartSocketDeadlineScheduler {
public:
    DeadlineToken armDeadline( AdbSmartSocketDeadlineKind kind, int timeoutMs, QObject* owner,
                               std::function<void()> callback ) override
    {
        const auto token = ++nextToken_;
        entries.push_back( Entry{ token, kind, timeoutMs, owner, std::move( callback ), true } );
        return token;
    }

    void cancelDeadline( DeadlineToken token ) override
    {
        for ( auto& entry : entries ) {
            if ( entry.token == token ) {
                entry.active = false;
            }
        }
    }

private:
    struct Entry {
        DeadlineToken token{ 0 };
        AdbSmartSocketDeadlineKind kind{ AdbSmartSocketDeadlineKind::Connect };
        int timeoutMs{ 0 };
        QObject* owner{ nullptr };
        std::function<void()> callback;
        bool active{ true };
    };

    DeadlineToken nextToken_{ 0 };
    std::vector<Entry> entries;
};

class ScriptedManagedTransport final : public LiveSourceTransport {
public:
    void start( Generation generation ) override
    {
        activeGeneration = generation;
        statistics_.generation = generation;
        Q_EMIT stateChanged( generation, State::Connecting );
    }

    void stop( Generation generation ) override
    {
        if ( activeGeneration == generation ) {
            activeGeneration.reset();
            Q_EMIT stateChanged( generation, State::Disconnected );
        }
    }

    void clearRemoteAsync( Generation generation, ClearRequestId requestId ) override
    {
        clearRequests.emplace_back( generation, requestId );
    }

    QString lastError() const override
    {
        return lastError_;
    }

    klogg::livecapture::LiveDataStatistics statistics() const override
    {
        return statistics_;
    }

    void publishState( State state )
    {
        REQUIRE( activeGeneration.has_value() );
        Q_EMIT stateChanged( *activeGeneration, state );
    }

    void setStatistics( klogg::livecapture::LiveDataStatistics statistics )
    {
        statistics_ = statistics;
    }

    std::optional<Generation> activeGeneration;
    std::vector<std::pair<Generation, ClearRequestId>> clearRequests;

private:
    QString lastError_;
    klogg::livecapture::LiveDataStatistics statistics_;
};

class ScriptedManagedTransportFactory final : public LiveSourceTransportFactory {
public:
    std::unique_ptr<LiveSourceTransport>
    create( const LiveSourceTransportConfig& config ) const override
    {
        configs.push_back( config );
        auto transport = std::make_unique<ScriptedManagedTransport>();
        created.push_back( transport.get() );
        return transport;
    }

    mutable std::vector<LiveSourceTransportConfig> configs;
    mutable std::vector<QPointer<ScriptedManagedTransport>> created;
};

struct ServicesDependencies {
    ManualProbe probe;
    RecordingLauncher launcher;
    ManualStartupLock startupLock;
    FakeKeyStore keyStore;
    ManualServerScheduler supervisorScheduler;
    RecordingSocketFactory trackerSocketFactory;
    PassiveDeadlineScheduler trackerDeadlines;
    ManualServerScheduler trackerScheduler;
    RecordingSocketFactory transportSocketFactory;
    PassiveDeadlineScheduler transportDeadlines;

    AdbLiveServicesDependencies refs( const LiveSourceTransportFactory* managedTransportFactory
                                      = nullptr )
    {
        return AdbLiveServicesDependencies{ probe,
                                            launcher,
                                            startupLock,
                                            keyStore,
                                            supervisorScheduler,
                                            trackerSocketFactory,
                                            trackerDeadlines,
                                            trackerScheduler,
                                            transportSocketFactory,
                                            transportDeadlines,
                                            managedTransportFactory };
    }
};

AdbLiveServicesConfig servicesConfig( const QString& applicationDir,
                                      const QString& perUserRuntimeDir )
{
    AdbLiveServicesConfig config;
    config.applicationDirPath = applicationDir;
    config.perUserRuntimeDir = perUserRuntimeDir;
    config.endpoint = AdbServerEndpoint{ QHostAddress::LocalHost, 5037 };
    config.minimumProtocolVersion = SupportedProtocolVersion;
    config.requiredFeatures = { "shell_v2" };
    config.readinessProbeInterval = 1ms;
    config.startupTimeout = 10s;
    config.healthProbeInterval = 10s;
    config.serverReconnectBackoff = { 1ms, 2ms };
    config.trackerReconnectBackoff = { 1ms, 2ms };
    return config;
}

void createPackagedHelper( const QString& applicationDir )
{
    const auto helperPath = AdbLiveServices::packagedHelperPath( applicationDir );
    REQUIRE( QDir().mkpath( QFileInfo( helperPath ).absolutePath() ) );
    QFile helper( helperPath );
    REQUIRE( helper.open( QIODevice::WriteOnly ) );
    REQUIRE( helper.write( "test helper" ) > 0 );
    helper.close();
    REQUIRE( helper.setPermissions( QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                    | QFileDevice::ExeOwner ) );
}

void requireRejectedPackagedHelper( const QString& applicationDir, const QString& runtimeDir )
{
    ServicesDependencies dependencies;
    AdbLiveServices services( servicesConfig( applicationDir, runtimeDir ), dependencies.refs() );
    CHECK_FALSE( services.isPackagedHelperAvailable() );

    auto lease = services.trackedDeviceProvider().acquireLease();
    const auto& snapshot = services.manager().snapshot();
    REQUIRE( snapshot.error.has_value() );
    CHECK( snapshot.error->category == ErrorCategory::Configuration );
    CHECK( snapshot.error->code == "adb-packaged-helper-missing" );
    CHECK( snapshot.error->retryPolicy == RetryPolicy::Never );
    CHECK( dependencies.probe.requests.empty() );
    CHECK( dependencies.launcher.requests.empty() );
}

LiveSourceTransportConfig smartSocketConfig( const QString& serial )
{
    LiveSourceTransportConfig config;
    config.sourceType = LiveLogSourceType::AdbLogcat;
    config.adbBackend = AdbTransportBackend::SmartSocket;
    config.deviceId = serial;
    return config;
}

class FakeTrackedDeviceProvider final : public AdbTrackedDeviceProvider {
public:
    explicit FakeTrackedDeviceProvider( QObject* parent = nullptr )
        : AdbTrackedDeviceProvider( parent )
    {
    }

    AdbInfrastructureLease acquireLease() override
    {
        ++leaseRequestCount;
        return {};
    }

    DeviceDiscoveryResult<::AdbDeviceInfo> currentSnapshot() const override
    {
        return snapshot;
    }

    void refresh() override
    {
        ++refreshRequestCount;
        Q_EMIT snapshotChanged( snapshot );
    }

    void push( DeviceDiscoveryResult<::AdbDeviceInfo> next )
    {
        snapshot = std::move( next );
        Q_EMIT snapshotChanged( snapshot );
    }

    DeviceDiscoveryResult<::AdbDeviceInfo> snapshot;
    int leaseRequestCount{ 0 };
    int refreshRequestCount{ 0 };
};

DeviceDiscoveryResult<::AdbDeviceInfo>
deviceSnapshot( Generation generation, QList<::AdbDeviceInfo> devices,
                std::optional<LiveSourceError> error = std::nullopt )
{
    return DeviceDiscoveryResult<::AdbDeviceInfo>{ generation, std::move( devices ),
                                                   std::move( error ) };
}

::AdbDeviceInfo onlineDevice( QString serial, QString description )
{
    return ::AdbDeviceInfo{ serial, QStringLiteral( "%1 (%2)" ).arg( description, serial ),
                            std::move( description ), ::AdbDeviceState::Online,
                            QStringLiteral( "device" ) };
}

::AdbDeviceInfo unavailableDevice( QString serial, ::AdbDeviceState state, QString stateText )
{
    return ::AdbDeviceInfo{ serial,
                            QStringLiteral( "%1 [%2]" ).arg( serial, stateText ),
                            {},
                            state,
                            std::move( stateText ) };
}

} // namespace

TEST_CASE( "ADB live services derive only fixed package and per-user paths",
           "[livecapture][adb][composition][paths]" )
{
    QTemporaryDir root;
    REQUIRE( root.isValid() );
    const auto applicationDir = root.filePath( QStringLiteral( "package/bin" ) );
    const auto runtimeDir = root.filePath( QStringLiteral( "user/runtime" ) );
    REQUIRE( QDir().mkpath( applicationDir ) );
    REQUIRE( QDir().mkpath( runtimeDir ) );

    ScopedEnvironment pathGuard( QByteArrayLiteral( "PATH" ) );
    ScopedEnvironment homeGuard( QByteArrayLiteral( "ANDROID_HOME" ) );
    ScopedEnvironment sdkGuard( QByteArrayLiteral( "ANDROID_SDK_ROOT" ) );
    qputenv( "PATH", QByteArrayLiteral( "/attacker/path" ) );
    qputenv( "ANDROID_HOME", QByteArrayLiteral( "/attacker/android-home" ) );
    qputenv( "ANDROID_SDK_ROOT", QByteArrayLiteral( "/attacker/android-sdk" ) );

    const auto firstHelper = AdbLiveServices::packagedHelperPath( applicationDir );
    const auto firstLock = AdbLiveServices::perUserLockPath( runtimeDir );
    const auto traversedApplicationDir
        = QDir( applicationDir ).filePath( QStringLiteral( "ignored/../" ) );
    const auto traversedRuntimeDir = QDir( runtimeDir ).filePath( QStringLiteral( "ignored/../" ) );
    CHECK( AdbLiveServices::packagedHelperPath( traversedApplicationDir ) == firstHelper );
    CHECK( AdbLiveServices::perUserLockPath( traversedRuntimeDir ) == firstLock );
    qputenv( "PATH", QByteArrayLiteral( "/different/path" ) );
    qputenv( "ANDROID_HOME", QByteArrayLiteral( "/different/android-home" ) );
    qputenv( "ANDROID_SDK_ROOT", QByteArrayLiteral( "/different/android-sdk" ) );

    CHECK( AdbLiveServices::packagedHelperPath( applicationDir ) == firstHelper );
    CHECK( AdbLiveServices::perUserLockPath( runtimeDir ) == firstLock );
    CHECK( QFileInfo( firstHelper ).isAbsolute() );
    CHECK( QFileInfo( firstLock ).isAbsolute() );
    CHECK( QDir::cleanPath( firstHelper ) == firstHelper );
    CHECK( QDir::cleanPath( firstLock ) == firstLock );
    const auto packageRelative
        = QDir( QFileInfo( applicationDir ).absolutePath() ).relativeFilePath( firstHelper );
    const auto userRelative = QDir( runtimeDir ).relativeFilePath( firstLock );
    CHECK_FALSE( QDir::isAbsolutePath( packageRelative ) );
    CHECK_FALSE( ( packageRelative == QStringLiteral( ".." )
                   || packageRelative.startsWith( QStringLiteral( "../" ) ) ) );
    CHECK_FALSE( QDir::isAbsolutePath( userRelative ) );
    CHECK_FALSE( ( userRelative == QStringLiteral( ".." )
                   || userRelative.startsWith( QStringLiteral( "../" ) ) ) );
#ifdef Q_OS_WIN
    CHECK( QFileInfo( firstHelper ).fileName() == QStringLiteral( "adb.exe" ) );
#else
    CHECK( QFileInfo( firstHelper ).fileName() == QStringLiteral( "adb" ) );
#endif
    CHECK( QFileInfo( firstLock ).fileName() == QStringLiteral( "adb-server-5037.lock" ) );
}

TEST_CASE( "packaged ADB helper validation rejects non-files non-executables and symlink escapes",
           "[livecapture][adb][composition][paths][security]" )
{
    QTemporaryDir root;
    REQUIRE( root.isValid() );
    const auto runtimeDir = root.filePath( QStringLiteral( "user/runtime" ) );
    REQUIRE( QDir().mkpath( runtimeDir ) );

    SECTION( "a directory cannot impersonate the helper" )
    {
        const auto applicationDir = root.filePath( QStringLiteral( "directory/package/bin" ) );
        const auto helperPath = AdbLiveServices::packagedHelperPath( applicationDir );
        REQUIRE( QDir().mkpath( helperPath ) );
        requireRejectedPackagedHelper( applicationDir, runtimeDir );
    }

    SECTION( "a regular file without execute permission is rejected" )
    {
        const auto applicationDir = root.filePath( QStringLiteral( "nonexec/package/bin" ) );
        const auto helperPath = AdbLiveServices::packagedHelperPath( applicationDir );
        REQUIRE( QDir().mkpath( QFileInfo( helperPath ).absolutePath() ) );
        QFile helper( helperPath );
        REQUIRE( helper.open( QIODevice::WriteOnly ) );
        REQUIRE( helper.write( "not executable" ) > 0 );
        helper.close();
        REQUIRE( helper.setPermissions( QFileDevice::ReadOwner | QFileDevice::WriteOwner ) );
        requireRejectedPackagedHelper( applicationDir, runtimeDir );
    }

    SECTION( "the helper itself cannot be a symlink" )
    {
        const auto applicationDir = root.filePath( QStringLiteral( "file-link/package/bin" ) );
        const auto helperPath = AdbLiveServices::packagedHelperPath( applicationDir );
        const auto externalHelper = root.filePath( QStringLiteral( "external/adb-file" ) );
        REQUIRE( QDir().mkpath( QFileInfo( helperPath ).absolutePath() ) );
        REQUIRE( QDir().mkpath( QFileInfo( externalHelper ).absolutePath() ) );
        QFile helper( externalHelper );
        REQUIRE( helper.open( QIODevice::WriteOnly ) );
        REQUIRE( helper.write( "external helper" ) > 0 );
        helper.close();
        REQUIRE( helper.setPermissions( QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                        | QFileDevice::ExeOwner ) );
        if ( !QFile::link( externalHelper, helperPath ) ) {
            WARN( "Filesystem does not support creating the helper symlink" );
        }
        else {
            requireRejectedPackagedHelper( applicationDir, runtimeDir );
        }
    }

    SECTION( "the helpers directory cannot redirect outside the application package" )
    {
        const auto applicationDir = root.filePath( QStringLiteral( "directory-link/package/bin" ) );
        const auto helpersDir = QDir( applicationDir ).filePath( QStringLiteral( "helpers" ) );
        const auto externalHelpersDir = root.filePath( QStringLiteral( "external/helpers" ) );
        REQUIRE( QDir().mkpath( applicationDir ) );
        REQUIRE( QDir().mkpath( externalHelpersDir ) );
        const auto externalHelper
            = QDir( externalHelpersDir )
                  .filePath( QFileInfo( AdbLiveServices::packagedHelperPath( applicationDir ) )
                                 .fileName() );
        QFile helper( externalHelper );
        REQUIRE( helper.open( QIODevice::WriteOnly ) );
        REQUIRE( helper.write( "external helper" ) > 0 );
        helper.close();
        REQUIRE( helper.setPermissions( QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                        | QFileDevice::ExeOwner ) );
        if ( !QFile::link( externalHelpersDir, helpersDir ) ) {
            WARN( "Filesystem does not support creating the helpers-directory symlink" );
        }
        else {
            requireRejectedPackagedHelper( applicationDir, runtimeDir );
        }
    }
}

TEST_CASE( "missing packaged ADB helper is a structured non-retryable configuration failure",
           "[livecapture][adb][composition][configuration]" )
{
    QTemporaryDir root;
    REQUIRE( root.isValid() );
    const auto applicationDir = root.filePath( QStringLiteral( "package/bin" ) );
    const auto runtimeDir = root.filePath( QStringLiteral( "user/runtime" ) );
    REQUIRE( QDir().mkpath( applicationDir ) );
    REQUIRE( QDir().mkpath( runtimeDir ) );

    ServicesDependencies dependencies;
    AdbLiveServices services( servicesConfig( applicationDir, runtimeDir ), dependencies.refs() );
    auto lease = services.trackedDeviceProvider().acquireLease();

    const auto& snapshot = services.manager().snapshot();
    REQUIRE( snapshot.error.has_value() );
    CHECK( snapshot.error->category == ErrorCategory::Configuration );
    CHECK( snapshot.error->scope == ErrorScope::Infrastructure );
    CHECK( snapshot.error->retryPolicy == RetryPolicy::Never );
    CHECK( snapshot.error->code == "adb-packaged-helper-missing" );
    CHECK( snapshot.error->nativeDetail.find(
               AdbLiveServices::packagedHelperPath( applicationDir ).toStdString() )
           != std::string::npos );
    CHECK( dependencies.probe.requests.empty() );
    CHECK( dependencies.launcher.requests.empty() );

    auto transport = services.create( smartSocketConfig( QStringLiteral( "restored-device" ) ) );
    REQUIRE( transport != nullptr );
    std::vector<std::pair<Generation, QString>> errors;
    QObject::connect( transport.get(), &LiveSourceTransport::errorOccurred,
                      [ &errors ]( Generation generation, const QString& error ) {
                          errors.emplace_back( generation, error );
                      } );
    transport->start( 91u );
    REQUIRE( errors.size() == 1u );
    CHECK( errors.front().first == 91u );
    CHECK(
        errors.front().second.contains( AdbLiveServices::packagedHelperPath( applicationDir ) ) );
}

TEST_CASE( "one explicit application root shares manager tracker provider and transport leases",
           "[livecapture][adb][composition][ownership][lease]" )
{
    QTemporaryDir root;
    REQUIRE( root.isValid() );
    const auto applicationDir = root.filePath( QStringLiteral( "package/bin" ) );
    const auto runtimeDir = root.filePath( QStringLiteral( "user/runtime" ) );
    REQUIRE( QDir().mkpath( runtimeDir ) );
    createPackagedHelper( applicationDir );

    ServicesDependencies dependencies;
    AdbLiveServices services( servicesConfig( applicationDir, runtimeDir ), dependencies.refs() );
    auto* const managerAddress = &services.manager();
    auto* const providerAddress = &services.trackedDeviceProvider();

    auto firstWindowDialogLease = providerAddress->acquireLease();
    auto secondWindowDialogLease = services.trackedDeviceProvider().acquireLease();
    auto firstTab = services.create( smartSocketConfig( QStringLiteral( "serial-one" ) ) );
    auto secondTab = services.create( smartSocketConfig( QStringLiteral( "serial-two" ) ) );
    REQUIRE( firstTab != nullptr );
    REQUIRE( secondTab != nullptr );
    firstTab->start( 101u );
    secondTab->start( 202u );

    CHECK( &services.manager() == managerAddress );
    CHECK( &services.trackedDeviceProvider() == providerAddress );
    CHECK( services.manager().activeLeaseCount() == 4u );
    REQUIRE( dependencies.probe.requests.size() == 1u );
    CHECK( dependencies.trackerSocketFactory.creationCount == 0 );
    CHECK( dependencies.transportSocketFactory.creationCount == 0 );

    std::vector<std::pair<Generation, LiveSourceTransport::State>> firstStates;
    std::vector<std::pair<Generation, LiveSourceTransport::State>> secondStates;
    QObject::connect( firstTab.get(), &LiveSourceTransport::stateChanged,
                      [ &firstStates ]( Generation generation, LiveSourceTransport::State state ) {
                          firstStates.emplace_back( generation, state );
                      } );
    QObject::connect( secondTab.get(), &LiveSourceTransport::stateChanged,
                      [ &secondStates ]( Generation generation, LiveSourceTransport::State state ) {
                          secondStates.emplace_back( generation, state );
                      } );

    dependencies.probe.completeReady( 0 );
    CHECK( services.manager().snapshot().infrastructure.status == InfrastructureStatus::Ready );
    CHECK( dependencies.trackerSocketFactory.creationCount == 1 );
    CHECK( dependencies.transportSocketFactory.creationCount == 2 );
    REQUIRE_FALSE( firstStates.empty() );
    REQUIRE_FALSE( secondStates.empty() );
    CHECK( firstStates.front().first == 101u );
    CHECK( secondStates.front().first == 202u );

    firstTab.reset();
    CHECK( services.manager().activeLeaseCount() == 3u );
    CHECK( dependencies.launcher.cleanupTokens.empty() );
    secondTab->stop( 202u );
    CHECK( services.manager().activeLeaseCount() == 2u );
    CHECK( services.manager().snapshot().infrastructure.status == InfrastructureStatus::Ready );

    firstWindowDialogLease.reset();
    CHECK( services.manager().activeLeaseCount() == 1u );
    secondWindowDialogLease.reset();
    CHECK( services.manager().activeLeaseCount() == 0u );
}

TEST_CASE( "managed transport synchronous stop from Connecting cannot acquire an orphan lease",
           "[livecapture][adb][composition][transport][reentrant][lease]" )
{
    QTemporaryDir root;
    REQUIRE( root.isValid() );
    const auto applicationDir = root.filePath( QStringLiteral( "package/bin" ) );
    const auto runtimeDir = root.filePath( QStringLiteral( "user/runtime" ) );
    REQUIRE( QDir().mkpath( runtimeDir ) );
    createPackagedHelper( applicationDir );

    ServicesDependencies dependencies;
    AdbLiveServices services( servicesConfig( applicationDir, runtimeDir ), dependencies.refs() );
    auto transport = services.create( smartSocketConfig( QStringLiteral( "serial-current" ) ) );
    REQUIRE( transport != nullptr );
    QObject::connect( transport.get(), &LiveSourceTransport::stateChanged, transport.get(),
                      [ &transport ]( Generation generation, LiveSourceTransport::State state ) {
                          if ( state == LiveSourceTransport::State::Connecting ) {
                              transport->stop( generation );
                          }
                      } );

    transport->start( 81u );

    CHECK( services.manager().activeLeaseCount() == 0u );
    CHECK( dependencies.probe.requests.empty() );
    CHECK( dependencies.trackerSocketFactory.creationCount == 0 );
    CHECK( dependencies.transportSocketFactory.creationCount == 0 );
}

TEST_CASE( "managed transport retains its stopped epoch adapter for source clear requests",
           "[livecapture][adb][composition][transport][clear][lifecycle]" )
{
    QTemporaryDir root;
    REQUIRE( root.isValid() );
    const auto applicationDir = root.filePath( QStringLiteral( "package/bin" ) );
    const auto runtimeDir = root.filePath( QStringLiteral( "user/runtime" ) );
    REQUIRE( QDir().mkpath( runtimeDir ) );
    createPackagedHelper( applicationDir );

    ServicesDependencies dependencies;
    AdbLiveServices services( servicesConfig( applicationDir, runtimeDir ), dependencies.refs() );
    auto transport = services.create( smartSocketConfig( QStringLiteral( "serial-current" ) ) );
    REQUIRE( transport != nullptr );
    std::vector<std::tuple<Generation, LiveSourceTransport::ClearRequestId, bool>> clearResults;
    QObject::connect( transport.get(), &LiveSourceTransport::clearRemoteFinished,
                      [ &clearResults ]( Generation generation,
                                         LiveSourceTransport::ClearRequestId requestId,
                                         bool succeeded, const QString& ) {
                          clearResults.emplace_back( generation, requestId, succeeded );
                      } );

    transport->start( 82u );
    dependencies.probe.completeReady( 0 );
    REQUIRE( dependencies.transportSocketFactory.creationCount == 1 );
    transport->stop( 82u );
    REQUIRE( services.manager().activeLeaseCount() == 0u );

    transport->clearRemoteAsync( 83u, 701u );

    CHECK( dependencies.transportSocketFactory.creationCount == 2 );
    CHECK( clearResults.empty() );
}

TEST_CASE( "managed transport publishes Connecting when ready infrastructure is lost",
           "[livecapture][adb][composition][transport][infrastructure][state]" )
{
    QTemporaryDir root;
    REQUIRE( root.isValid() );
    const auto applicationDir = root.filePath( QStringLiteral( "package/bin" ) );
    const auto runtimeDir = root.filePath( QStringLiteral( "user/runtime" ) );
    REQUIRE( QDir().mkpath( runtimeDir ) );
    createPackagedHelper( applicationDir );

    ServicesDependencies dependencies;
    ScriptedManagedTransportFactory innerFactory;
    AdbLiveServices services( servicesConfig( applicationDir, runtimeDir ),
                              dependencies.refs( &innerFactory ) );
    auto transport = services.create( smartSocketConfig( QStringLiteral( "serial-current" ) ) );
    REQUIRE( transport != nullptr );
    std::vector<LiveSourceTransport::State> states;
    QObject::connect( transport.get(), &LiveSourceTransport::stateChanged,
                      [ &states ]( Generation, LiveSourceTransport::State state ) {
                          states.push_back( state );
                      } );

    transport->start( 84u );
    dependencies.probe.completeReady( 0 );
    REQUIRE( innerFactory.created.size() == 1u );
    REQUIRE( innerFactory.created.front() != nullptr );
    innerFactory.created.front()->publishState( LiveSourceTransport::State::Connected );
    REQUIRE( states.back() == LiveSourceTransport::State::Connected );

    dependencies.supervisorScheduler.fire( AdbServerScheduleKind::HealthProbe );
    dependencies.probe.completeAbsent( 1 );

    CHECK( states.back() == LiveSourceTransport::State::Connecting );
}

TEST_CASE( "managed transport suppresses a terminal diagnostic made stale by Error reentrancy",
           "[livecapture][adb][composition][transport][error][reentrant]" )
{
    QTemporaryDir root;
    REQUIRE( root.isValid() );
    const auto applicationDir = root.filePath( QStringLiteral( "package/bin" ) );
    const auto runtimeDir = root.filePath( QStringLiteral( "user/runtime" ) );
    REQUIRE( QDir().mkpath( applicationDir ) );
    REQUIRE( QDir().mkpath( runtimeDir ) );

    ServicesDependencies dependencies;
    AdbLiveServices services( servicesConfig( applicationDir, runtimeDir ), dependencies.refs() );
    auto transport = services.create( smartSocketConfig( QStringLiteral( "restored-device" ) ) );
    REQUIRE( transport != nullptr );
    std::vector<std::pair<Generation, QString>> errors;
    QObject::connect( transport.get(), &LiveSourceTransport::stateChanged, transport.get(),
                      [ &transport ]( Generation generation, LiveSourceTransport::State state ) {
                          if ( state == LiveSourceTransport::State::Error ) {
                              transport->stop( generation );
                          }
                      } );
    QObject::connect( transport.get(), &LiveSourceTransport::errorOccurred,
                      [ &errors ]( Generation generation, const QString& error ) {
                          errors.emplace_back( generation, error );
                      } );

    transport->start( 85u );

    CHECK( errors.empty() );
    CHECK( services.manager().activeLeaseCount() == 0u );
}

TEST_CASE( "managed transport accumulates saturating statistics across infrastructure epochs",
           "[livecapture][adb][composition][transport][statistics][epoch]" )
{
    QTemporaryDir root;
    REQUIRE( root.isValid() );
    const auto applicationDir = root.filePath( QStringLiteral( "package/bin" ) );
    const auto runtimeDir = root.filePath( QStringLiteral( "user/runtime" ) );
    REQUIRE( QDir().mkpath( runtimeDir ) );
    createPackagedHelper( applicationDir );

    ServicesDependencies dependencies;
    ScriptedManagedTransportFactory innerFactory;
    AdbLiveServices services( servicesConfig( applicationDir, runtimeDir ),
                              dependencies.refs( &innerFactory ) );
    auto transport = services.create( smartSocketConfig( QStringLiteral( "serial-current" ) ) );
    REQUIRE( transport != nullptr );

    constexpr Generation generation = 86u;
    transport->start( generation );
    dependencies.probe.completeReady( 0, "adb-server:first" );
    REQUIRE( innerFactory.created.size() == 1u );
    klogg::livecapture::LiveDataStatistics first;
    first.generation = generation;
    first.receivedBytes = std::numeric_limits<std::size_t>::max() - 3u;
    first.receivedChunks = 4u;
    first.queuedBytes = 2u;
    first.queuedChunks = 1u;
    first.deliveredBytes = 9u;
    first.deliveredChunks = 3u;
    first.backpressuredBytes = 5u;
    first.backpressuredChunks = 1u;
    first.highWaterQueuedBytes = 7u;
    first.highWaterQueuedChunks = 2u;
    innerFactory.created.front()->setStatistics( first );

    dependencies.supervisorScheduler.fire( AdbServerScheduleKind::HealthProbe );
    dependencies.probe.completeReady( 1, "adb-server:replacement" );
    REQUIRE( innerFactory.created.size() == 2u );
    klogg::livecapture::LiveDataStatistics second;
    second.generation = generation;
    second.receivedBytes = 10u;
    second.receivedChunks = 6u;
    second.queuedBytes = 3u;
    second.queuedChunks = 2u;
    second.deliveredBytes = 11u;
    second.deliveredChunks = 5u;
    second.backpressuredBytes = 13u;
    second.backpressuredChunks = 4u;
    second.highWaterQueuedBytes = 5u;
    second.highWaterQueuedChunks = 3u;
    innerFactory.created.back()->setStatistics( second );

    const auto statistics = transport->statistics();
    CHECK( statistics.generation == generation );
    CHECK( statistics.receivedBytes == std::numeric_limits<std::size_t>::max() );
    CHECK( statistics.receivedChunks == 10u );
    CHECK( statistics.queuedBytes == 5u );
    CHECK( statistics.queuedChunks == 3u );
    CHECK( statistics.deliveredBytes == 20u );
    CHECK( statistics.deliveredChunks == 8u );
    CHECK( statistics.backpressuredBytes == 18u );
    CHECK( statistics.backpressuredChunks == 5u );
    CHECK( statistics.highWaterQueuedBytes == 7u );
    CHECK( statistics.highWaterQueuedChunks == 3u );
}

TEST_CASE( "managed SmartSocket transport rejects stale manager generations before opening",
           "[livecapture][adb][composition][transport][generation][stale]" )
{
    QTemporaryDir root;
    REQUIRE( root.isValid() );
    const auto applicationDir = root.filePath( QStringLiteral( "package/bin" ) );
    const auto runtimeDir = root.filePath( QStringLiteral( "user/runtime" ) );
    REQUIRE( QDir().mkpath( runtimeDir ) );
    createPackagedHelper( applicationDir );

    ServicesDependencies dependencies;
    AdbLiveServices services( servicesConfig( applicationDir, runtimeDir ), dependencies.refs() );
    auto transport = services.create( smartSocketConfig( QStringLiteral( "serial-current" ) ) );
    REQUIRE( transport != nullptr );

    transport->start( 7u );
    REQUIRE( dependencies.probe.requests.size() == 1u );
    const auto firstManagerGeneration = services.manager().snapshot().generation;
    CHECK( dependencies.transportSocketFactory.creationCount == 0 );

    dependencies.probe.completeFailed( 0 );
    transport->stop( 7u );
    REQUIRE( services.manager().activeLeaseCount() == 0u );

    transport->start( 8u );
    REQUIRE( services.manager().snapshot().generation == firstManagerGeneration + 1u );
    REQUIRE( dependencies.probe.requests.size() == 2u );
    dependencies.probe.repeatReadyEvenIfCancelled( 0 );
    CHECK( dependencies.transportSocketFactory.creationCount == 0 );

    dependencies.probe.completeReady( 1 );
    CHECK( dependencies.transportSocketFactory.creationCount == 1 );
}

TEST_CASE( "managed SmartSocket transport replaces old infrastructure epochs with correlated runs",
           "[livecapture][adb][composition][transport][epoch][correlation]" )
{
    QTemporaryDir root;
    REQUIRE( root.isValid() );
    const auto applicationDir = root.filePath( QStringLiteral( "package/bin" ) );
    const auto runtimeDir = root.filePath( QStringLiteral( "user/runtime" ) );
    REQUIRE( QDir().mkpath( runtimeDir ) );
    createPackagedHelper( applicationDir );

    ServicesDependencies dependencies;
    AdbLiveServices services( servicesConfig( applicationDir, runtimeDir ), dependencies.refs() );
    auto transport = services.create( smartSocketConfig( QStringLiteral( "serial-current" ) ) );
    REQUIRE( transport != nullptr );
    std::vector<Generation> stateGenerations;
    QObject::connect( transport.get(), &LiveSourceTransport::stateChanged,
                      [ &stateGenerations ]( Generation generation, LiveSourceTransport::State ) {
                          stateGenerations.push_back( generation );
                      } );

    transport->start( 17u );
    dependencies.probe.completeReady( 0, "adb-server:first" );
    REQUIRE( dependencies.transportSocketFactory.creationCount == 1 );
    const auto firstEpoch = services.manager().snapshot().infrastructureEpoch;

    dependencies.supervisorScheduler.fire( AdbServerScheduleKind::HealthProbe );
    REQUIRE( dependencies.probe.requests.size() == 2u );
    dependencies.probe.completeReady( 1, "adb-server:replacement" );

    CHECK( services.manager().snapshot().infrastructureEpoch == firstEpoch + 1u );
    CHECK( dependencies.trackerSocketFactory.creationCount == 2 );
    CHECK( dependencies.transportSocketFactory.creationCount == 2 );
    CHECK( std::all_of( stateGenerations.cbegin(), stateGenerations.cend(),
                        []( Generation generation ) { return generation == 17u; } ) );

    dependencies.probe.repeatReadyEvenIfCancelled( 0 );
    CHECK( dependencies.transportSocketFactory.creationCount == 2 );
}

TEST_CASE( "key consent is forwarded once and answers are generation and epoch correlated",
           "[livecapture][adb][composition][keys][consent]" )
{
    QTemporaryDir root;
    REQUIRE( root.isValid() );
    const auto applicationDir = root.filePath( QStringLiteral( "package/bin" ) );
    const auto runtimeDir = root.filePath( QStringLiteral( "user/runtime" ) );
    REQUIRE( QDir().mkpath( runtimeDir ) );
    createPackagedHelper( applicationDir );

    ServicesDependencies dependencies;
    dependencies.keyStore.inspection.state = AdbServerStandardKeyState::Absent;
    AdbLiveServices services( servicesConfig( applicationDir, runtimeDir ), dependencies.refs() );

    std::vector<std::pair<Generation, std::uint64_t>> prompts;
    QObject::connect( &services, &AdbLiveServices::keyGenerationConsentRequested, &services,
                      [ &prompts ]( Generation generation, std::uint64_t epoch ) {
                          prompts.emplace_back( generation, epoch );
                      } );
    auto firstDialogLease = services.trackedDeviceProvider().acquireLease();
    auto secondDialogLease = services.trackedDeviceProvider().acquireLease();
    auto firstTab = services.create( smartSocketConfig( QStringLiteral( "serial-one" ) ) );
    auto secondTab = services.create( smartSocketConfig( QStringLiteral( "serial-two" ) ) );
    firstTab->start( 11u );
    secondTab->start( 12u );

    REQUIRE( dependencies.probe.requests.size() == 1u );
    dependencies.probe.completeAbsent( 0 );
    REQUIRE( dependencies.startupLock.requests.size() == 1u );
    dependencies.startupLock.completeAcquired( 0 );
    REQUIRE( dependencies.probe.requests.size() == 2u );
    dependencies.probe.completeAbsent( 1 );

    REQUIRE( prompts.size() == 1u );
    const auto prompt = prompts.front();
    CHECK( dependencies.keyStore.generationCount == 0 );
    CHECK( dependencies.launcher.requests.empty() );

    services.answerKeyGenerationConsent( prompt.first + 1u, prompt.second, true );
    services.answerKeyGenerationConsent( prompt.first, prompt.second + 1u, true );
    CHECK( dependencies.keyStore.generationCount == 0 );
    CHECK( dependencies.launcher.requests.empty() );

    services.answerKeyGenerationConsent( prompt.first, prompt.second, true );
    CHECK( dependencies.keyStore.generationCount == 1 );
    REQUIRE( dependencies.launcher.requests.size() == 1u );
    CHECK( dependencies.launcher.requests.front().request.executable
           == AdbLiveServices::packagedHelperPath( applicationDir ) );
    CHECK_FALSE( dependencies.launcher.requests.front().request.allowPathLookup );

    services.answerKeyGenerationConsent( prompt.first, prompt.second, true );
    CHECK( dependencies.keyStore.generationCount == 1 );
    CHECK( dependencies.launcher.requests.size() == 1u );
}

TEST_CASE( "synchronous consent UI callback is safe and existing keys are never overwritten",
           "[livecapture][adb][composition][keys][reentrant]" )
{
    QTemporaryDir root;
    REQUIRE( root.isValid() );
    const auto applicationDir = root.filePath( QStringLiteral( "package/bin" ) );
    const auto runtimeDir = root.filePath( QStringLiteral( "user/runtime" ) );
    REQUIRE( QDir().mkpath( runtimeDir ) );
    createPackagedHelper( applicationDir );

    SECTION( "a synchronous UI answer is correlated to the active prompt" )
    {
        ServicesDependencies dependencies;
        dependencies.keyStore.inspection.state = AdbServerStandardKeyState::Absent;
        AdbLiveServices services( servicesConfig( applicationDir, runtimeDir ),
                                  dependencies.refs() );
        int promptCount = 0;
        QObject::connect(
            &services, &AdbLiveServices::keyGenerationConsentRequested, &services,
            [ &services, &promptCount ]( Generation generation, std::uint64_t epoch ) {
                ++promptCount;
                services.answerKeyGenerationConsent( generation, epoch, true );
            } );
        auto lease = services.trackedDeviceProvider().acquireLease();
        dependencies.probe.completeAbsent( 0 );
        dependencies.startupLock.completeAcquired( 0 );
        dependencies.probe.completeAbsent( 1 );

        CHECK( promptCount == 1 );
        CHECK( dependencies.keyStore.generationCount == 1 );
        CHECK( dependencies.launcher.requests.size() == 1u );
    }

    SECTION( "an existing standard key launches without prompting or generation" )
    {
        ServicesDependencies dependencies;
        dependencies.keyStore.inspection.state = AdbServerStandardKeyState::Present;
        AdbLiveServices services( servicesConfig( applicationDir, runtimeDir ),
                                  dependencies.refs() );
        int promptCount = 0;
        QObject::connect( &services, &AdbLiveServices::keyGenerationConsentRequested, &services,
                          [ &promptCount ]( Generation, std::uint64_t ) { ++promptCount; } );
        auto lease = services.trackedDeviceProvider().acquireLease();
        dependencies.probe.completeAbsent( 0 );
        dependencies.startupLock.completeAcquired( 0 );
        dependencies.probe.completeAbsent( 1 );

        CHECK( promptCount == 0 );
        CHECK( dependencies.keyStore.generationCount == 0 );
        CHECK( dependencies.launcher.requests.size() == 1u );
    }
}

TEST_CASE( "ADB dialog refresh consumes pushed snapshots and preserves explicit selection",
           "[livecapture][adb][composition][dialog][snapshot][selection]" )
{
    FakeTrackedDeviceProvider provider;
    provider.snapshot = deviceSnapshot(
        1u, { unavailableDevice( QStringLiteral( "offline-first" ), ::AdbDeviceState::Offline,
                                 QStringLiteral( "offline" ) ),
              unavailableDevice( QStringLiteral( "locked-second" ), ::AdbDeviceState::Unauthorized,
                                 QStringLiteral( "unauthorized" ) ),
              onlineDevice( QStringLiteral( "online-third" ), QStringLiteral( "Pixel 9" ) ) } );

    AdbLogcatDialog dialog( provider, QStringLiteral( "locked-second" ) );
    auto* const combo = dialog.findChild<QComboBox*>( QStringLiteral( "deviceCombo" ) );
    auto* const refresh
        = dialog.findChild<QPushButton*>( QStringLiteral( "refreshDevicesButton" ) );
    auto* const buttonBox = dialog.findChild<QDialogButtonBox*>( QStringLiteral( "buttonBox" ) );
    REQUIRE( combo != nullptr );
    REQUIRE( refresh != nullptr );
    REQUIRE( buttonBox != nullptr );
    REQUIRE( buttonBox->button( QDialogButtonBox::Ok ) != nullptr );

    CHECK( provider.leaseRequestCount == 1 );
    CHECK( provider.refreshRequestCount == 1 );
    CHECK( combo->currentData().toString() == QStringLiteral( "locked-second" ) );
    CHECK_FALSE( buttonBox->button( QDialogButtonBox::Ok )->isEnabled() );
    CHECK( combo->itemText( 0 ) == QStringLiteral( "offline-first [offline]" ) );
    CHECK( combo->itemText( 1 ) == QStringLiteral( "locked-second [unauthorized]" ) );

    provider.push( deviceSnapshot(
        2u, { onlineDevice( QStringLiteral( "online-third" ), QStringLiteral( "Pixel 9" ) ),
              unavailableDevice( QStringLiteral( "locked-second" ), ::AdbDeviceState::Unauthorized,
                                 QStringLiteral( "unauthorized" ) ),
              unavailableDevice( QStringLiteral( "offline-first" ), ::AdbDeviceState::Offline,
                                 QStringLiteral( "offline" ) ) } ) );
    CHECK( combo->currentData().toString() == QStringLiteral( "locked-second" ) );

    refresh->click();
    CHECK( provider.refreshRequestCount == 2 );
    CHECK( dialog.findChild<QObject*>( QStringLiteral( "adbExecutableEdit" ) ) == nullptr );
}

TEST_CASE( "ADB dialog defaults to the first online device only without a valid explicit serial",
           "[livecapture][adb][composition][dialog][default]" )
{
    FakeTrackedDeviceProvider provider;
    provider.snapshot = deviceSnapshot(
        1u, { unavailableDevice( QStringLiteral( "offline-first" ), ::AdbDeviceState::Offline,
                                 QStringLiteral( "offline" ) ),
              onlineDevice( QStringLiteral( "online-second" ), QStringLiteral( "Pixel 8" ) ),
              onlineDevice( QStringLiteral( "online-third" ), QStringLiteral( "Pixel 9" ) ) } );

    SECTION( "missing explicit serial falls back to first online" )
    {
        AdbLogcatDialog dialog( provider, QStringLiteral( "detached-explicit" ) );
        auto* const combo = dialog.findChild<QComboBox*>( QStringLiteral( "deviceCombo" ) );
        REQUIRE( combo != nullptr );
        CHECK( combo->currentData().toString() == QStringLiteral( "online-second" ) );
    }

    SECTION( "empty explicit serial falls back to first online" )
    {
        AdbLogcatDialog dialog( provider, {} );
        auto* const combo = dialog.findChild<QComboBox*>( QStringLiteral( "deviceCombo" ) );
        REQUIRE( combo != nullptr );
        CHECK( combo->currentData().toString() == QStringLiteral( "online-second" ) );
    }

    SECTION( "valid offline explicit serial is preserved and cannot be accepted" )
    {
        AdbLogcatDialog dialog( provider, QStringLiteral( "offline-first" ) );
        auto* const combo = dialog.findChild<QComboBox*>( QStringLiteral( "deviceCombo" ) );
        auto* const buttonBox
            = dialog.findChild<QDialogButtonBox*>( QStringLiteral( "buttonBox" ) );
        REQUIRE( combo != nullptr );
        REQUIRE( buttonBox != nullptr );
        CHECK( combo->currentData().toString() == QStringLiteral( "offline-first" ) );
        CHECK_FALSE( buttonBox->button( QDialogButtonBox::Ok )->isEnabled() );
    }
}

TEST_CASE( "ADB dialog surfaces manager diagnostics without withdrawing tracked devices",
           "[livecapture][adb][composition][dialog][error]" )
{
    FakeTrackedDeviceProvider provider;
    provider.snapshot = deviceSnapshot(
        9u, { onlineDevice( QStringLiteral( "known-online" ), QStringLiteral( "Pixel" ) ) },
        LiveSourceError{ ErrorCategory::Infrastructure, "adb-health-probe-failed",
                         ErrorScope::Infrastructure, RetryPolicy::WaitForInfrastructure,
                         "Shared ADB server health check failed.", "connection reset by peer" } );

    AdbLogcatDialog dialog( provider, QStringLiteral( "known-online" ) );
    auto* const combo = dialog.findChild<QComboBox*>( QStringLiteral( "deviceCombo" ) );
    auto* const status = dialog.findChild<QLabel*>( QStringLiteral( "adbStatusLabel" ) );
    REQUIRE( combo != nullptr );
    REQUIRE( status != nullptr );
    CHECK( combo->count() == 1 );
    CHECK( combo->currentData().toString() == QStringLiteral( "known-online" ) );
    CHECK( status->text().contains( QStringLiteral( "Shared ADB server health check failed" ) ) );
    CHECK( status->text().contains( QStringLiteral( "connection reset by peer" ) ) );
}

TEST_CASE( "application root teardown retires live tabs before shared ADB infrastructure",
           "[livecapture][adb][composition][shutdown][lifetime]" )
{
    QTemporaryDir root;
    REQUIRE( root.isValid() );
    const auto applicationDir = root.filePath( QStringLiteral( "package/bin" ) );
    const auto runtimeDir = root.filePath( QStringLiteral( "user/runtime" ) );
    REQUIRE( QDir().mkpath( runtimeDir ) );
    createPackagedHelper( applicationDir );

    ServicesDependencies dependencies;
    std::unique_ptr<LiveSourceTransport> survivingTab;
    std::vector<std::pair<Generation, LiveSourceTransport::State>> states;
    {
        AdbLiveServices services( servicesConfig( applicationDir, runtimeDir ),
                                  dependencies.refs() );
        survivingTab = services.create( smartSocketConfig( QStringLiteral( "serial-live" ) ) );
        REQUIRE( survivingTab != nullptr );
        QObject::connect( survivingTab.get(), &LiveSourceTransport::stateChanged,
                          [ &states ]( Generation generation, LiveSourceTransport::State state ) {
                              states.emplace_back( generation, state );
                          } );
        survivingTab->start( 44u );
        REQUIRE( services.manager().activeLeaseCount() == 1u );
        REQUIRE( dependencies.probe.requests.size() == 1u );

        services.shutdown();
        CHECK( services.manager().activeLeaseCount() == 0u );
        REQUIRE_FALSE( states.empty() );
        CHECK( states.back()
               == std::make_pair( Generation{ 44u }, LiveSourceTransport::State::Disconnected ) );

        dependencies.probe.repeatReadyEvenIfCancelled( 0 );
        CHECK( dependencies.trackerSocketFactory.creationCount == 0 );
        CHECK( dependencies.transportSocketFactory.creationCount == 0 );
        CHECK( services.manager().snapshot().infrastructure.status != InfrastructureStatus::Ready );
        CHECK_FALSE( services.trackedDeviceProvider().acquireLease() );
    }

    survivingTab.reset();
    QCoreApplication::sendPostedEvents( nullptr, QEvent::DeferredDelete );
    QCoreApplication::processEvents();
    CHECK( dependencies.launcher.cleanupTokens.empty() );
}

TEST_CASE( "application shutdown destroys managed transport trees before injected dependencies",
           "[livecapture][adb][composition][shutdown][ordering]" )
{
    QTemporaryDir root;
    REQUIRE( root.isValid() );
    const auto applicationDir = root.filePath( QStringLiteral( "package/bin" ) );
    const auto runtimeDir = root.filePath( QStringLiteral( "user/runtime" ) );
    REQUIRE( QDir().mkpath( runtimeDir ) );
    createPackagedHelper( applicationDir );

    ServicesDependencies dependencies;
    std::unique_ptr<LiveSourceTransport> survivingTab;
    {
        AdbLiveServices services( servicesConfig( applicationDir, runtimeDir ),
                                  dependencies.refs() );
        survivingTab = services.create( smartSocketConfig( QStringLiteral( "serial-live" ) ) );
        REQUIRE( survivingTab != nullptr );
        survivingTab->start( 51u );
        dependencies.probe.completeReady( 0 );
        REQUIRE( dependencies.transportSocketFactory.parents.size() == 1u );
        REQUIRE_FALSE( dependencies.transportSocketFactory.parents.front().isNull() );

        services.shutdown();

        CHECK( dependencies.transportSocketFactory.parents.front().isNull() );
    }
    survivingTab.reset();
}

TEST_CASE( "application shutdown drains transports deferred by earlier tab destruction",
           "[livecapture][adb][composition][shutdown][ordering][deferred]" )
{
    QTemporaryDir root;
    REQUIRE( root.isValid() );
    const auto applicationDir = root.filePath( QStringLiteral( "package/bin" ) );
    const auto runtimeDir = root.filePath( QStringLiteral( "user/runtime" ) );
    REQUIRE( QDir().mkpath( runtimeDir ) );
    createPackagedHelper( applicationDir );

    ServicesDependencies dependencies;
    AdbLiveServices services( servicesConfig( applicationDir, runtimeDir ), dependencies.refs() );
    auto tab = services.create( smartSocketConfig( QStringLiteral( "serial-live" ) ) );
    REQUIRE( tab != nullptr );
    tab->start( 52u );
    dependencies.probe.completeReady( 0 );
    REQUIRE( dependencies.transportSocketFactory.parents.size() == 1u );
    REQUIRE_FALSE( dependencies.transportSocketFactory.parents.front().isNull() );

    tab.reset();
    REQUIRE_FALSE( dependencies.transportSocketFactory.parents.front().isNull() );
    services.shutdown();

    CHECK( dependencies.transportSocketFactory.parents.front().isNull() );
}

TEST_CASE( "application shutdown tolerates a disconnected listener destroying another tab",
           "[livecapture][adb][composition][shutdown][reentrant]" )
{
    QTemporaryDir root;
    REQUIRE( root.isValid() );
    const auto applicationDir = root.filePath( QStringLiteral( "package/bin" ) );
    const auto runtimeDir = root.filePath( QStringLiteral( "user/runtime" ) );
    REQUIRE( QDir().mkpath( runtimeDir ) );
    createPackagedHelper( applicationDir );

    ServicesDependencies dependencies;
    AdbLiveServices services( servicesConfig( applicationDir, runtimeDir ), dependencies.refs() );
    auto firstTab = services.create( smartSocketConfig( QStringLiteral( "serial-first" ) ) );
    auto secondTab = services.create( smartSocketConfig( QStringLiteral( "serial-second" ) ) );
    REQUIRE( firstTab != nullptr );
    REQUIRE( secondTab != nullptr );
    firstTab->start( 61u );
    secondTab->start( 62u );
    REQUIRE( services.manager().activeLeaseCount() == 2u );
    QObject::connect( firstTab.get(), &LiveSourceTransport::stateChanged, firstTab.get(),
                      [ &secondTab ]( Generation, LiveSourceTransport::State state ) {
                          if ( state == LiveSourceTransport::State::Disconnected ) {
                              secondTab.reset();
                          }
                      } );

    services.shutdown();

    CHECK( secondTab == nullptr );
    CHECK( services.manager().activeLeaseCount() == 0u );
}

TEST_CASE( "application shutdown rejects stale key-consent startup callbacks",
           "[livecapture][adb][composition][shutdown][consent][stale]" )
{
    QTemporaryDir root;
    REQUIRE( root.isValid() );
    const auto applicationDir = root.filePath( QStringLiteral( "package/bin" ) );
    const auto runtimeDir = root.filePath( QStringLiteral( "user/runtime" ) );
    REQUIRE( QDir().mkpath( runtimeDir ) );
    createPackagedHelper( applicationDir );

    ServicesDependencies dependencies;
    dependencies.keyStore.inspection.state = AdbServerStandardKeyState::Absent;
    AdbLiveServices services( servicesConfig( applicationDir, runtimeDir ), dependencies.refs() );
    int promptCount = 0;
    QObject::connect( &services, &AdbLiveServices::keyGenerationConsentRequested, &services,
                      [ &promptCount ]( Generation, std::uint64_t ) { ++promptCount; } );
    auto lease = services.trackedDeviceProvider().acquireLease();
    REQUIRE( dependencies.probe.requests.size() == 1u );

    services.shutdown();
    dependencies.probe.repeatAbsentEvenIfCancelled( 0 );

    CHECK( promptCount == 0 );
    CHECK( dependencies.startupLock.requests.empty() );
    CHECK( dependencies.launcher.requests.empty() );
}
