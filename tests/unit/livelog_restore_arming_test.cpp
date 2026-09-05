/*
 * Copyright (C) 2026
 *
 * This file is part of klogg.
 *
 * klogg is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * klogg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with klogg.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * RED contracts for inert persisted live-session restoration.
 *
 * Parsing remains backward compatible with saved Running intent, but restore
 * must always recreate Android and iOS tabs in Stopped state. No transport,
 * ADB infrastructure observation, or iOS catalog subscription may begin until
 * an explicit reconnect. Saving snapshots a stopped persistence copy without
 * mutating the live controller, regardless of its runtime state.
 *
 * Everything here is deterministic: fixed capture ids, recording service
 * doubles, no waits, no wall-clock assertions, and no real devices/processes.
 */

#include <catch2/catch.hpp>

#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <QCoreApplication>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QJsonDocument>
#include <QHostAddress>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QTcpSocket>
#include <QTemporaryDir>

#include "adbinfrastructuremanager.h"
#include "adblogcatsource.h"
#include "adbserversupervisor.h"
#include "adbsmartsocketclient.h"
#include "livelogcontroller.h"
#include "livelogsession.h"
#include "livesourcetransport.h"
#include "livestate.h"
#include "session.h"
#include "sessioninfo.h"
#include "streaminglogdata.h"
#include "viewinterface.h"

namespace {

namespace live = klogg::livecapture;

using klogg::livelog::AndroidBackend;
using klogg::livelog::DeviceIdentity;
using klogg::livelog::Diagnostic;
using klogg::livelog::IosBackend;
using klogg::livelog::LiveLogSessionSpec;
using klogg::livelog::SourceKind;
using IosOptions = klogg::livelog::IosOptions;

// Fixed identifiers keep every assertion deterministic (no generators).
constexpr auto AndroidCaptureId = "2f4b6d8e-1a3c-45e7-9b0d-7f2a4c6e8d10";
constexpr auto IosCaptureId = "6c8a0e2b-4d6f-4a1c-8e3b-9d1f5a7c9e11";

LiveLogSessionSpec makeAndroidSpec( const char* captureId = AndroidCaptureId )
{
    LiveLogSessionSpec spec;
    spec.captureId = QLatin1String( captureId );
    spec.sourceKind = SourceKind::AndroidLogcat;
    spec.androidBackend = AndroidBackend::SmartSocket;
    spec.device.deviceId = QStringLiteral( "R58NC123ABC" );
    spec.device.displayName = QStringLiteral( "Pixel 8 Pro" );
    spec.device.connection = DeviceIdentity::Connection::Usb;
    spec.runIntent = live::RunIntent::Running;
    spec.android.buffers = QStringList{ QStringLiteral( "main" ), QStringLiteral( "system" ) };
    spec.android.filterSpec = QStringLiteral( "TagA:D" );
    spec.android.priority = QStringLiteral( "INFO" );
    spec.android.pid = std::optional<int>{ 12345 };
    spec.capture.ansiOutputEnabled = true;
    spec.capture.preserveAnsiOnSave = true;
    spec.capture.autoReconnectEnabled = true;
    spec.capture.maxReconnectAttempts = 7;
    spec.capture.captureMaxFileSize = static_cast<qint64>( 32 ) * 1024 * 1024;
    spec.capture.captureBackupCount = 5;
    return spec;
}

LiveLogSessionSpec makeIosSpec( const char* captureId = IosCaptureId )
{
    LiveLogSessionSpec spec;
    spec.captureId = QLatin1String( captureId );
    spec.sourceKind = SourceKind::IosSyslog;
    spec.iosBackend = IosBackend::Native;
    spec.device.deviceId = QStringLiteral( "00008101-001A2B3C4D5E" );
    spec.device.displayName = QStringLiteral( "iPhone 15 Pro" );
    spec.device.connection = DeviceIdentity::Connection::Network;
    spec.runIntent = live::RunIntent::Running;
    spec.ios.level = QStringLiteral( "debug" );
    spec.ios.categories = QStringList{ QStringLiteral( "network" ) };
    spec.ios.subsystem = QStringLiteral( "com.apple.network" );
    spec.ios.outputFormat = IosOptions::OutputFormat::Json;
    return spec;
}

LiveLogSessionSpec makeSupportedIosSpec( const char* captureId = IosCaptureId )
{
    auto spec = makeIosSpec( captureId );
    spec.ios = {};
    return spec;
}

bool hasFatalDiagnostic( const std::vector<Diagnostic>& diagnostics, const char* expectedCode )
{
    return std::any_of( diagnostics.cbegin(), diagnostics.cend(),
                        [ expectedCode ]( const Diagnostic& diagnostic ) {
                            return diagnostic.severity == Diagnostic::Severity::Fatal
                                   && diagnostic.code == QLatin1String( expectedCode );
                        } );
}

// --- Restore harness ---------------------------------------------------------

class RecordingLiveSourceTransport final : public LiveSourceTransport {
public:
    void start( Generation generation ) override
    {
        startGenerations.push_back( generation );
    }

    void stop( Generation generation ) override
    {
        stopGenerations.push_back( generation );
    }

    void clearRemoteAsync( Generation, ClearRequestId ) override {}

    QString lastError() const override
    {
        return lastError_;
    }

    std::optional<live::LiveSourceError> lastStructuredError() const override
    {
        return structuredError_;
    }

    void publishState( Generation generation, State state )
    {
        Q_EMIT stateChanged( generation, state );
    }

    void publishBytes( Generation generation, const QByteArray& bytes )
    {
        Q_EMIT bytesReceived( generation, bytes );
    }

    void publishTerminalError( Generation generation, live::LiveSourceError error )
    {
        structuredError_ = std::move( error );
        lastError_ = QString::fromStdString( structuredError_->message ) + QLatin1Char( '\n' )
                     + QString::fromStdString( structuredError_->nativeDetail );
        Q_EMIT stateChanged( generation, State::Error );
        stopObservedBeforeTerminalText = std::find( stopGenerations.cbegin(), stopGenerations.cend(),
                                                    generation )
                                         != stopGenerations.cend();
        ++terminalTextEmissions;
        Q_EMIT errorOccurred( generation, lastError_ );
    }

    void publishTextError( Generation generation )
    {
        ++terminalTextEmissions;
        Q_EMIT errorOccurred( generation, lastError_ );
    }

    std::vector<Generation> startGenerations;
    std::vector<Generation> stopGenerations;
    bool stopObservedBeforeTerminalText{ false };
    int terminalTextEmissions{ 0 };

private:
    QString lastError_;
    std::optional<live::LiveSourceError> structuredError_;
};

class UnavailableLiveSourceTransportFactory final : public LiveSourceTransportFactory {
public:
    std::unique_ptr<LiveSourceTransport> create( const LiveSourceTransportConfig& ) const override
    {
        ++createCalls;
        return nullptr;
    }

    mutable int createCalls = 0;
};

class PassiveAdbSocketFactory final : public live::adb::AdbSmartSocketFactory {
public:
    QTcpSocket* createSocket( QObject* parent ) override
    {
        // Ownership transfers to the Qt parent.
        // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
        return new QTcpSocket( parent );
    }
};

class PassiveAdbDeadlineScheduler final : public live::adb::AdbSmartSocketDeadlineScheduler {
public:
    live::adb::DeadlineToken armDeadline( live::adb::AdbSmartSocketDeadlineKind, int, QObject*,
                                          std::function<void()> ) override
    {
        return ++nextToken_;
    }

    void cancelDeadline( live::adb::DeadlineToken ) override {}

private:
    live::adb::DeadlineToken nextToken_{ 0 };
};

class RecordingAdbProbe final : public live::adb::AdbServerProbe {
public:
    live::adb::AdbServerToken probe( const live::adb::AdbServerEndpoint&, Callback ) override
    {
        ++calls;
        return static_cast<live::adb::AdbServerToken>( calls );
    }
    void cancel( live::adb::AdbServerToken ) override {}
    int calls{ 0 };
};

class PassiveAdbLauncher final : public live::adb::AdbServerLauncher {
public:
    live::adb::AdbServerToken launch( const live::adb::AdbServerLaunchRequest&, Callback ) override
    {
        return 1u;
    }
    void cleanup( live::adb::AdbServerToken ) override {}
    void release( live::adb::AdbServerToken ) override {}
};

class PassiveAdbStartupLock final : public live::adb::AdbServerStartupLock {
public:
    live::adb::AdbServerToken acquire( const QString&, Callback ) override
    {
        return 1u;
    }
    void cancel( live::adb::AdbServerToken ) override {}
    void release( live::adb::AdbServerToken ) override {}
};

class PassiveAdbKeyStore final : public live::adb::AdbKeyStore {
public:
    live::adb::AdbServerKeyInspection inspectStandardKey() override
    {
        return { live::adb::AdbServerStandardKeyState::Present, {} };
    }
    live::adb::AdbServerKeyGenerationResult generateStandardKey() override
    {
        return { true, {} };
    }
};

class PassiveAdbServerScheduler final : public live::adb::AdbServerScheduler {
public:
    live::adb::AdbServerToken schedule( live::adb::AdbServerScheduleKind,
                                        std::chrono::milliseconds, Callback ) override
    {
        return ++nextToken_;
    }
    void cancel( live::adb::AdbServerToken ) override {}

private:
    live::adb::AdbServerToken nextToken_{ 0 };
};

struct RecordingAdbInfrastructure {
    PassiveAdbSocketFactory socketFactory;
    PassiveAdbDeadlineScheduler deadlines;
    live::adb::AdbSmartSocketClient client;
    RecordingAdbProbe probe;
    PassiveAdbLauncher launcher;
    PassiveAdbStartupLock startupLock;
    PassiveAdbKeyStore keyStore;
    PassiveAdbServerScheduler supervisorScheduler;
    PassiveAdbServerScheduler trackerScheduler;
    live::adb::AdbInfrastructureManager manager;

    RecordingAdbInfrastructure()
        : client( live::adb::AdbSmartSocketClientConfig{}, socketFactory, deadlines )
        , manager( live::adb::AdbInfrastructureManagerConfig{},
                   live::adb::AdbInfrastructureManagerDependencies{
                       probe, launcher, startupLock, keyStore, supervisorScheduler, client,
                       trackerScheduler } )
    {
    }
};

class RecordingLiveSourceTransportFactory final : public LiveSourceTransportFactory {
public:
    std::unique_ptr<LiveSourceTransport>
    create( const LiveSourceTransportConfig& config ) const override
    {
        requestedConfigs.push_back( config );
        auto transport = std::make_unique<RecordingLiveSourceTransport>();
        createdTransports.push_back( transport.get() );
        return std::unique_ptr<LiveSourceTransport>( std::move( transport ) );
    }

    // create() is const by contract; recording state is therefore mutable.
    mutable std::vector<LiveSourceTransportConfig> requestedConfigs;
    mutable std::vector<RecordingLiveSourceTransport*> createdTransports;

    std::size_t totalStarts() const
    {
        std::size_t total = 0;
        for ( const auto* transport : createdTransports ) {
            total += transport->startGenerations.size();
        }
        return total;
    }
};

class RecordingIosCatalog final : public klogg::livecapture::ios::IosCatalogSnapshotProvider,
                                  public klogg::livecapture::ios::IosCatalogMetadataRequester {
public:
    klogg::livecapture::ios::IosCatalogSnapshot snapshot() const override
    {
        return snapshot_;
    }

    SubscriptionId subscribe( SnapshotCallback callback ) override
    {
        ++subscribeCalls;
        callback_ = std::move( callback );
        return 1u;
    }

    void unsubscribe( SubscriptionId subscription ) override
    {
        ++unsubscribeCalls;
        if ( subscription == 1u ) {
            callback_ = {};
        }
    }

    std::optional<live::LiveSourceError> startupError() const override
    {
        return startupError_;
    }

    void requestMetadata( klogg::livecapture::ios::IosEndpointKey endpoint ) override
    {
        metadataRequests.push_back( std::move( endpoint ) );
    }

    void publish( klogg::livecapture::ios::IosCatalogSnapshot snapshot )
    {
        snapshot_ = std::move( snapshot );
        if ( callback_ ) {
            callback_( snapshot_ );
        }
    }

    void replaceSnapshot( klogg::livecapture::ios::IosCatalogSnapshot snapshot )
    {
        snapshot_ = std::move( snapshot );
    }

    SnapshotCallback callbackCopy() const
    {
        return callback_;
    }

    void setStartupError( live::LiveSourceError error )
    {
        startupError_ = std::move( error );
    }

    std::vector<klogg::livecapture::ios::IosEndpointKey> metadataRequests;
    int subscribeCalls{ 0 };
    int unsubscribeCalls{ 0 };

private:
    klogg::livecapture::ios::IosCatalogSnapshot snapshot_;
    SnapshotCallback callback_;
    std::optional<live::LiveSourceError> startupError_;
};

// Minimal view stand-in: restore wires data objects through ViewInterface's
// NVI surface; no widget is needed to observe arming decisions.
class NullView final : public ViewInterface {
public:
    std::shared_ptr<SearchableLogData> data() const
    {
        return data_;
    }

protected:
    void doSetData( std::shared_ptr<SearchableLogData> data,
                    std::shared_ptr<LogFilteredData> ) override
    {
        data_ = std::move( data );
    }
    void doSetQuickFindPattern( std::shared_ptr<QuickFindPattern> ) override {}
    void doSetSavedSearches( SavedSearches* ) override {}
    void doSetViewContext( const QString& ) override {}
    std::shared_ptr<const ViewContextInterface> doGetViewContext() const override
    {
        return nullptr;
    }

private:
    std::shared_ptr<SearchableLogData> data_;
};

// Removes every persisted window for the duration of one test case so the
// seeded SessionInfo entries cannot leak into sibling tests.
class ScopedSessionWindows {
public:
    ScopedSessionWindows()
    {
        clearAll();
    }

    ~ScopedSessionWindows()
    {
        clearAll();
    }

    ScopedSessionWindows( const ScopedSessionWindows& ) = delete;
    ScopedSessionWindows& operator=( const ScopedSessionWindows& ) = delete;

private:
    static void clearAll()
    {
        auto& sessionInfo = SessionInfo::getSynced();
        for ( const auto& windowId : sessionInfo.windows() ) {
            sessionInfo.remove( windowId );
        }
        sessionInfo.save();
    }
};

struct SeededSessionFile {
    QString displayName;
    QString sourceType;
    QString sourceSpec;
};

// Seeds one window whose only open files are the given live-session payloads.
void seedWindowFiles( const QString& windowId, const std::vector<SeededSessionFile>& files,
                      int currentIndex = 0 )
{
    auto& sessionInfo = SessionInfo::getSynced();
    sessionInfo.add( windowId );

    std::vector<SessionInfo::OpenFile> openFiles;
    openFiles.reserve( files.size() );
    for ( const auto& file : files ) {
        openFiles.emplace_back( file.displayName, 0, QString{}, file.sourceType, file.displayName,
                                file.sourceSpec );
    }
    sessionInfo.setOpenFiles( windowId, openFiles );
    sessionInfo.setCurrentFileIndex( windowId, currentIndex );
    sessionInfo.save();
}

OpenedDocumentsList restoreSession( Session& appSession, const QString& windowId,
                                    int* restoredCurrentIndex = nullptr )
{
    WindowSession windowSession{ std::shared_ptr<Session>( &appSession, []( Session* ) {} ),
                                 windowId, 0 };
    int currentIndex = -1;
    auto restored = windowSession.restore(
        []() -> ViewInterface* {
            // Ownership transfers to the session.
            // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
            return new NullView();
        },
        &currentIndex );
    if ( restoredCurrentIndex != nullptr ) {
        *restoredCurrentIndex = currentIndex;
    }
    return restored;
}

LiveLogSessionSpec saveAndParseLiveTab( Session& appSession, const QString& windowId,
                                        const ViewInterface* view )
{
    WindowSession windowSession{ std::shared_ptr<Session>( &appSession, []( Session* ) {} ),
                                 windowId, 0 };
    const std::vector<SaveFileInfo> files{ { view, 0u, nullptr } };
    windowSession.save( files, {}, 0 );

    const auto savedFiles = SessionInfo::getSynced().openFiles( windowId );
    REQUIRE( savedFiles.size() == 1 );
    const auto parsed = klogg::livelog::parsePersistedSpec( savedFiles.front().sourceSpec );
    REQUIRE( parsed.ok() );
    REQUIRE( parsed.spec.has_value() );
    return *parsed.spec;
}

void closeAndDeleteViews( Session& appSession, OpenedDocumentsList& opened )
{
    for ( auto& entry : opened ) {
        appSession.close( entry.second );
        delete entry.second;
    }
    opened.clear();
}

} // namespace

TEST_CASE( "Restore arming refuses every spec that validateForAccept rejects",
           "[livelog-restore-arming]" )
{
    // Explicit runtime start mapping must remain fail-closed in exactly the
    // same cases as the accept gate, including compatibility-only backends.
    const auto armEvents = []( const LiveLogSessionSpec& spec ) {
        return klogg::livelog::initialLiveStateEvents( spec, live::Timestamp{ 1500 } );
    };

    auto legacyAndroid = makeAndroidSpec();
    legacyAndroid.androidBackend = AndroidBackend::LegacyProcess;
    const auto androidGate = klogg::livelog::validateForAccept( legacyAndroid );
    REQUIRE( hasFatalDiagnostic( androidGate, "transitional-backend-not-creatable" ) );
    REQUIRE( armEvents( legacyAndroid ).empty() );

    auto legacyIos = makeIosSpec();
    legacyIos.iosBackend = IosBackend::LegacyProcess;
    const auto iosGate = klogg::livelog::validateForAccept( legacyIos );
    REQUIRE( hasFatalDiagnostic( iosGate, "transitional-backend-not-creatable" ) );
    REQUIRE( armEvents( legacyIos ).empty() );

    // Anchors: the fail-closed cases cycle 1 already covered keep refusing.
    auto undeviced = makeAndroidSpec();
    undeviced.device.deviceId.clear();
    REQUIRE( armEvents( undeviced ).empty() );

    auto blankDevice = makeAndroidSpec();
    blankDevice.device.deviceId = QStringLiteral( "   " );
    REQUIRE( armEvents( blankDevice ).empty() );

    auto escapedCapture = makeAndroidSpec();
    escapedCapture.captureId = QStringLiteral( "../escape" );
    REQUIRE( armEvents( escapedCapture ).empty() );

    // A valid Running spec still arms with the timestamp passed verbatim.
    const auto validEvents = armEvents( makeAndroidSpec() );
    REQUIRE( validEvents.size() == 1 );
    REQUIRE( std::get<live::StartRequested>( validEvents.front() ).at == live::Timestamp{ 1500 } );
}

TEST_CASE( "Explicit runtime start intent still drives the reducer",
           "[livelog-restore-arming][runtime-start]" )
{
    const live::LiveStateConfig config{ 5u, std::chrono::seconds{ 10 } };
    auto snapshot = live::initialLiveState();
    const auto events
        = klogg::livelog::initialLiveStateEvents( makeAndroidSpec(), live::Timestamp{ 1500 } );
    REQUIRE( events.size() == 1 );
    REQUIRE( std::holds_alternative<live::StartRequested>( events.front() ) );
    snapshot = live::reduce( snapshot, events.front(), config ).snapshot;
    CHECK( snapshot.runIntent == live::RunIntent::Running );
    CHECK( snapshot.source.status == live::SourceStatus::WaitingForInfrastructure );
}

TEST_CASE( "Persisted Android running intent restores inert and reconnects once",
           "[livelog-restore-arming][session][inert-restore]" )
{
    ScopedSessionWindows windowsGuard;
    QTemporaryDir outputDir;
    REQUIRE( outputDir.isValid() );
    RecordingLiveSourceTransportFactory factory;
    auto appSession = std::make_shared<Session>( factory );
    const auto windowId = QStringLiteral( "livelog-inert-android-window" );
    auto spec = makeAndroidSpec();
    spec.boundOutputFile = outputDir.filePath( QStringLiteral( "android-restored.log" ) );
    seedWindowFiles( windowId, { { spec.displayName(), QStringLiteral( "adb_logcat" ),
                                   klogg::livelog::serializeSpec( spec ) } } );

    auto opened = restoreSession( *appSession, windowId );

    REQUIRE( opened.size() == 1 );
    REQUIRE( appSession->getDocumentKind( opened.front().second ) == DocumentKind::AdbLogcat );
    auto* controller = appSession->getLiveLogController( opened.front().second );
    auto* source = appSession->getAdbLogcatSource( opened.front().second );
    REQUIRE( controller != nullptr );
    REQUIRE( source != nullptr );
    CHECK( factory.createdTransports.empty() );
    CHECK( factory.totalStarts() == 0 );
    CHECK( controller->snapshot().runIntent == live::RunIntent::Stopped );
    CHECK( controller->snapshot().source.status == live::SourceStatus::Stopped );
    CHECK( controller->spec().runIntent == live::RunIntent::Stopped );
    CHECK( source->sessionData().runIntent == live::RunIntent::Stopped );
    CHECK( source->sessionData().deviceSerial == spec.device.deviceId );
    CHECK( source->sessionData().captureId == spec.captureId );
    CHECK( source->sessionData().androidBuffers == spec.android.buffers );
    CHECK( source->sessionData().androidFilterSpec == spec.android.filterSpec );
    CHECK( source->sessionData().androidPriority == spec.android.priority );
    CHECK( source->sessionData().androidPid == spec.android.pid );
    CHECK( source->sessionData().boundOutputFile == spec.boundOutputFile );
    CHECK( QFile::exists( spec.boundOutputFile ) );
    CHECK( appSession->lastRestoreRejections().isEmpty() );

    REQUIRE( source->reconnectSource() );
    REQUIRE( factory.requestedConfigs.size() == 1 );
    REQUIRE( factory.createdTransports.size() == 1 );
    CHECK( factory.totalStarts() == 1 );
    const auto& config = factory.requestedConfigs.front();
    CHECK( config.deviceId == spec.device.deviceId );
    CHECK( config.androidBuffers == spec.android.buffers );
    CHECK( config.androidFilterSpec == spec.android.filterSpec );
    CHECK( config.androidPriority == spec.android.priority );
    CHECK( config.androidPid == spec.android.pid );
    CHECK( controller->snapshot().runIntent == live::RunIntent::Running );

    closeAndDeleteViews( *appSession, opened );
}

TEST_CASE( "Persisted Android running intent does not acquire ADB infrastructure",
           "[livelog-restore-arming][session][inert-restore][adb-infrastructure]" )
{
    ScopedSessionWindows windowsGuard;
    RecordingLiveSourceTransportFactory factory;
    RecordingAdbInfrastructure infrastructure;
    auto appSession = std::make_shared<Session>( factory, &infrastructure.manager, nullptr );
    const auto windowId = QStringLiteral( "livelog-inert-adb-infrastructure-window" );
    const auto spec = makeAndroidSpec();
    seedWindowFiles( windowId, { { spec.displayName(), QStringLiteral( "adb_logcat" ),
                                   klogg::livelog::serializeSpec( spec ) } } );

    auto opened = restoreSession( *appSession, windowId );

    REQUIRE( opened.size() == 1 );
    CHECK( infrastructure.manager.activeLeaseCount() == 0u );
    CHECK( infrastructure.probe.calls == 0 );
    CHECK( factory.createdTransports.empty() );
    auto* controller = appSession->getLiveLogController( opened.front().second );
    REQUIRE( controller != nullptr );
    CHECK( controller->snapshot().runIntent == live::RunIntent::Stopped );
    CHECK( controller->snapshot().source.status == live::SourceStatus::Stopped );
    closeAndDeleteViews( *appSession, opened );
}

TEST_CASE( "Inactive live-session composition canonicalizes runtime run intent",
           "[livelog-restore-arming][session][inert]" )
{
    RecordingLiveSourceTransportFactory factory;
    auto appSession = std::make_shared<Session>( factory );
    auto sessionData = klogg::livelog::sessionDataFromSpec( makeAndroidSpec() );
    REQUIRE( sessionData.runIntent == live::RunIntent::Running );

    auto* view = appSession->openAdbLogcat(
        sessionData,
        []() -> ViewInterface* {
            // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
            return new NullView();
        },
        false );

    REQUIRE( view != nullptr );
    auto* controller = appSession->getLiveLogController( view );
    auto* source = appSession->getAdbLogcatSource( view );
    REQUIRE( controller != nullptr );
    REQUIRE( source != nullptr );
    CHECK( controller->spec().runIntent == live::RunIntent::Stopped );
    CHECK( controller->snapshot().runIntent == live::RunIntent::Stopped );
    CHECK( source->sessionData().runIntent == live::RunIntent::Stopped );
    CHECK( factory.createdTransports.empty() );

    appSession->close( view );
    delete view;
}

TEST_CASE( "Fresh explicitly-started live sessions still start immediately",
           "[livelog-restore-arming][session][fresh-start]" )
{
    RecordingLiveSourceTransportFactory factory;
    auto appSession = std::make_shared<Session>( factory );
    auto sessionData = klogg::livelog::sessionDataFromSpec( makeAndroidSpec() );

    auto* view = appSession->openAdbLogcat(
        sessionData,
        []() -> ViewInterface* {
            // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
            return new NullView();
        },
        true );

    REQUIRE( view != nullptr );
    REQUIRE( factory.createdTransports.size() == 1 );
    CHECK( factory.totalStarts() == 1 );
    auto* controller = appSession->getLiveLogController( view );
    REQUIRE( controller != nullptr );
    CHECK( controller->snapshot().runIntent == live::RunIntent::Running );
    appSession->close( view );
    delete view;
}

TEST_CASE( "Clean and unclean restore both normalize old running JSON on the next save",
           "[livelog-restore-arming][session][inert-restore][persistence]" )
{
    for ( const bool uncleanShutdown : { false, true } ) {
        DYNAMIC_SECTION( "uncleanShutdown=" << uncleanShutdown )
        {
            ScopedSessionWindows windowsGuard;
            RecordingLiveSourceTransportFactory factory;
            auto appSession = std::make_shared<Session>( factory );
            const auto windowId
                = uncleanShutdown ? QStringLiteral( "livelog-unclean-running-window" )
                                  : QStringLiteral( "livelog-clean-running-window" );
            const auto spec = makeAndroidSpec();
            seedWindowFiles( windowId, { { spec.displayName(), QStringLiteral( "adb_logcat" ),
                                           klogg::livelog::serializeSpec( spec ) } } );
            auto& sessionInfo = SessionInfo::getSynced();
            sessionInfo.setDirtyShutdown( uncleanShutdown );
            sessionInfo.save();

            auto opened = restoreSession( *appSession, windowId );
            REQUIRE( opened.size() == 1 );
            auto* controller = appSession->getLiveLogController( opened.front().second );
            REQUIRE( controller != nullptr );
            CHECK( controller->snapshot().runIntent == live::RunIntent::Stopped );
            CHECK( controller->snapshot().source.status == live::SourceStatus::Stopped );
            CHECK( factory.totalStarts() == 0 );

            const auto saved = saveAndParseLiveTab( *appSession, windowId, opened.front().second );
            CHECK( saved.schemaVersion == klogg::livelog::kCurrentSpecVersion );
            CHECK( saved.runIntent == live::RunIntent::Stopped );
            CHECK( saved.captureId == spec.captureId );
            CHECK( saved.device.deviceId == spec.device.deviceId );
            closeAndDeleteViews( *appSession, opened );
        }
    }
}

TEST_CASE( "Persistence stops only the saved copy of every active runtime state",
           "[livelog-restore-arming][session][persistence-normalization]" )
{
    const auto openFresh = []( Session& appSession, LiveLogSessionSpec spec ) {
        spec.boundOutputFile.clear();
        auto data = klogg::livelog::sessionDataFromSpec( spec );
        return appSession.openAdbLogcat(
            data,
            []() -> ViewInterface* {
                // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
                return new NullView();
            },
            true );
    };

    SECTION( "failed but still running" )
    {
        ScopedSessionWindows windowsGuard;
        RecordingLiveSourceTransportFactory factory;
        auto appSession = std::make_shared<Session>( factory );
        const auto windowId = QStringLiteral( "livelog-save-failed-window" );
        seedWindowFiles( windowId, {} );
        auto spec = makeAndroidSpec();
        spec.capture.autoReconnectEnabled = false;
        auto* view = openFresh( *appSession, spec );
        REQUIRE( view != nullptr );
        auto* controller = appSession->getLiveLogController( view );
        REQUIRE( controller != nullptr );
        controller->streamFailed(
            controller->snapshot().generation,
            live::LiveSourceError{ live::ErrorCategory::Stream, "terminal", live::ErrorScope::Stream,
                                   live::RetryPolicy::Never, "terminal", "test" } );
        REQUIRE( controller->snapshot().source.status == live::SourceStatus::Failed );
        const auto saved = saveAndParseLiveTab( *appSession, windowId, view );
        CHECK( saved.runIntent == live::RunIntent::Stopped );
        CHECK( controller->snapshot().runIntent == live::RunIntent::Running );
        CHECK( controller->snapshot().source.status == live::SourceStatus::Failed );
        appSession->close( view );
        delete view;
    }

    SECTION( "retry wait" )
    {
        ScopedSessionWindows windowsGuard;
        RecordingLiveSourceTransportFactory factory;
        auto appSession = std::make_shared<Session>( factory );
        const auto windowId = QStringLiteral( "livelog-save-retrying-window" );
        seedWindowFiles( windowId, {} );
        auto* view = openFresh( *appSession, makeAndroidSpec() );
        REQUIRE( view != nullptr );
        auto* controller = appSession->getLiveLogController( view );
        REQUIRE( controller != nullptr );
        controller->streamFailed(
            controller->snapshot().generation,
            live::LiveSourceError{ live::ErrorCategory::Stream, "retry", live::ErrorScope::Stream,
                                   live::RetryPolicy::Backoff, "retry", "test" } );
        REQUIRE( controller->snapshot().source.status == live::SourceStatus::RetryWait );
        const auto saved = saveAndParseLiveTab( *appSession, windowId, view );
        CHECK( saved.runIntent == live::RunIntent::Stopped );
        CHECK( controller->snapshot().runIntent == live::RunIntent::Running );
        CHECK( controller->snapshot().source.status == live::SourceStatus::RetryWait );
        appSession->close( view );
        delete view;
    }

    SECTION( "waiting for device" )
    {
        ScopedSessionWindows windowsGuard;
        RecordingLiveSourceTransportFactory factory;
        RecordingIosCatalog catalog;
        auto appSession = std::make_shared<Session>( factory, nullptr, &catalog );
        const auto windowId = QStringLiteral( "livelog-save-waiting-window" );
        seedWindowFiles( windowId, {} );
        auto* view = openFresh( *appSession, makeSupportedIosSpec() );
        REQUIRE( view != nullptr );
        auto* controller = appSession->getLiveLogController( view );
        REQUIRE( controller != nullptr );
        REQUIRE( controller->snapshot().source.status == live::SourceStatus::WaitingForDevice );
        const auto saved = saveAndParseLiveTab( *appSession, windowId, view );
        CHECK( saved.runIntent == live::RunIntent::Stopped );
        CHECK( controller->snapshot().runIntent == live::RunIntent::Running );
        CHECK( controller->snapshot().source.status == live::SourceStatus::WaitingForDevice );
        appSession->close( view );
        delete view;
    }

    SECTION( "streaming" )
    {
        ScopedSessionWindows windowsGuard;
        RecordingLiveSourceTransportFactory factory;
        auto appSession = std::make_shared<Session>( factory );
        const auto windowId = QStringLiteral( "livelog-save-streaming-window" );
        seedWindowFiles( windowId, {} );
        auto* view = openFresh( *appSession, makeAndroidSpec() );
        REQUIRE( view != nullptr );
        auto* controller = appSession->getLiveLogController( view );
        REQUIRE( controller != nullptr );
        REQUIRE( factory.createdTransports.size() == 1 );
        factory.createdTransports.front()->publishState( controller->snapshot().generation,
                                                         LiveSourceTransport::State::Connected );
        REQUIRE( controller->snapshot().source.status == live::SourceStatus::Streaming );
        const auto saved = saveAndParseLiveTab( *appSession, windowId, view );
        CHECK( saved.runIntent == live::RunIntent::Stopped );
        CHECK( controller->snapshot().runIntent == live::RunIntent::Running );
        CHECK( controller->snapshot().source.status == live::SourceStatus::Streaming );
        appSession->close( view );
        delete view;
    }
}

TEST_CASE( "Persisted iOS running intent restores without catalog activation and reconnects once",
           "[livelog-restore-arming][session][inert-restore][ios-catalog]" )
{
    ScopedSessionWindows windowsGuard;
    RecordingLiveSourceTransportFactory factory;
    RecordingIosCatalog catalog;
    auto appSession = std::make_shared<Session>( factory, nullptr, &catalog );
    const auto windowId = QStringLiteral( "livelog-inert-ios-window" );
    const auto spec = makeSupportedIosSpec();
    seedWindowFiles( windowId, { { spec.displayName(), QStringLiteral( "ios_log_stream" ),
                                   klogg::livelog::serializeSpec( spec ) } } );

    auto opened = restoreSession( *appSession, windowId );

    REQUIRE( opened.size() == 1 );
    auto* controller = appSession->getLiveLogController( opened.front().second );
    auto* source = appSession->getAdbLogcatSource( opened.front().second );
    REQUIRE( controller != nullptr );
    REQUIRE( source != nullptr );
    CHECK( catalog.subscribeCalls == 0 );
    CHECK( catalog.metadataRequests.empty() );
    CHECK( factory.createdTransports.empty() );
    CHECK( factory.totalStarts() == 0 );
    CHECK( controller->snapshot().runIntent == live::RunIntent::Stopped );
    CHECK( controller->snapshot().source.status == live::SourceStatus::Stopped );
    CHECK( source->sessionData().runIntent == live::RunIntent::Stopped );
    CHECK( source->sessionData().deviceSerial == spec.device.deviceId );
    CHECK( source->sessionData().captureId == spec.captureId );
    CHECK( source->sessionData().iosEndpoint.udid == spec.device.deviceId.toStdString() );
    CHECK( source->sessionData().iosEndpoint.connectionType
           == klogg::livecapture::ios::NativeConnectionType::Network );

    REQUIRE( source->reconnectSource() );
    CHECK( catalog.subscribeCalls == 1 );
    CHECK( factory.totalStarts() == 0 );

    klogg::livecapture::ios::IosCatalogEntry entry;
    entry.endpoint.udid = spec.device.deviceId.toStdString();
    entry.endpoint.connectionType = klogg::livecapture::ios::NativeConnectionType::Network;
    catalog.publish( { 1u, { entry } } );

    REQUIRE( factory.requestedConfigs.size() == 1 );
    CHECK( factory.totalStarts() == 1 );
    const auto& config = factory.requestedConfigs.front();
    CHECK( config.deviceId == spec.device.deviceId );
    CHECK( config.iosEndpoint == entry.endpoint );
    CHECK( controller->snapshot().runIntent == live::RunIntent::Running );

    closeAndDeleteViews( *appSession, opened );
}

SCENARIO( "Rejected restored specs never arm and surface structured rejections",
          "[livelog-restore-arming][session]" )
{
    struct RejectionCase {
        QString description;
        QString displayName;
        QString payload;
        QString expectedFragment;
    };

    // Every case carries a fatal rejection that must reach the existing
    // surfacing path with an actionable message — and none of them may ever
    // arm a transport.
    auto runningWithoutDevice = makeAndroidSpec();
    runningWithoutDevice.device.deviceId.clear();
    runningWithoutDevice.device.displayName = QStringLiteral( "Undeviced Pixel" );

    const QJsonObject unknownKindObject{
        { QStringLiteral( "schemaVersion" ), 1 },
        { QStringLiteral( "sourceKind" ), QStringLiteral( "windows_eventlog" ) },
        { QStringLiteral( "captureId" ), QLatin1String{ AndroidCaptureId } },
    };

    auto hostileCapture = makeAndroidSpec();
    hostileCapture.captureId = QStringLiteral( "../escape" );
    hostileCapture.device.displayName = QStringLiteral( "Escaped Pixel" );

    const std::vector<RejectionCase> rejectionCases{
        { QStringLiteral( "running intent without a device" ),
          runningWithoutDevice.device.displayName,
          klogg::livelog::serializeSpec( runningWithoutDevice ),
          QStringLiteral( "detected device is required" ) },
        { QStringLiteral( "raw command-line options" ), QStringLiteral( "Legacy CLI Pixel" ),
          QStringLiteral(
              R"json({"schemaVersion":1,"sourceKind":"android_logcat","androidBackend":"smart_socket","adbExecutable":"/opt/android-sdk/platform-tools/adb","extraArgs":"-v threadtime","deviceSerial":"R58NC123ABC","deviceDescription":"Legacy CLI Pixel","captureId":"%1"})json" )
              .arg( QLatin1String{ AndroidCaptureId } ),
          QStringLiteral( "reopen" ) },
        { QStringLiteral( "unknown source kind" ), QStringLiteral( "Mystery Source" ),
          QString::fromUtf8( QJsonDocument( unknownKindObject ).toJson( QJsonDocument::Compact ) ),
          QStringLiteral( "unrecognized source type" ) },
        { QStringLiteral( "hostile capture id" ), hostileCapture.device.displayName,
          klogg::livelog::serializeSpec( hostileCapture ),
          QStringLiteral( "usable capture identifier" ) },
    };

    for ( const auto& rejectionCase : rejectionCases ) {
        DYNAMIC_SECTION( rejectionCase.description.toStdString() )
        {
            ScopedSessionWindows windowsGuard;

            RecordingLiveSourceTransportFactory factory;
            auto appSession = std::make_shared<Session>( factory );

            const auto windowId = QStringLiteral( "livelog-rejected-window" );
            seedWindowFiles( windowId,
                             { { rejectionCase.displayName, QStringLiteral( "adb_logcat" ),
                                 rejectionCase.payload } } );

            auto opened = restoreSession( *appSession, windowId );

            // Structured rejection surfaces through the existing path, naming
            // the document and carrying the actionable fatal message.
            const auto rejections = appSession->lastRestoreRejections();
            REQUIRE_FALSE( rejections.isEmpty() );
            const auto joined = rejections.join( QStringLiteral( "\n" ) );
            INFO( "rejections: " << joined.toStdString() );
            REQUIRE( joined.contains( rejectionCase.displayName ) );
            REQUIRE( joined.contains( rejectionCase.expectedFragment ) );

            // Fatal rejection is transactional: it creates neither a tab nor a
            // transport and can never arm a run.
            REQUIRE( opened.empty() );
            REQUIRE( factory.createdTransports.empty() );
            REQUIRE( factory.totalStarts() == 0 );

            closeAndDeleteViews( *appSession, opened );
        }
    }
}

SCENARIO( "Transitional legacy_process sessions restore loadable and inert",
          "[livelog-restore-arming][session]" )
{
    GIVEN( "A saved running-intent session recorded on the transitional backend" )
    {
        auto legacySpec = makeAndroidSpec();
        legacySpec.androidBackend = AndroidBackend::LegacyProcess;

        // Loadable: the persisted options survive parsing untouched — the
        // compatibility marker retires the TRANSPORT, not the user's data.
        const auto parsed
            = klogg::livelog::parsePersistedSpec( klogg::livelog::serializeSpec( legacySpec ) );
        REQUIRE( parsed.ok() );
        REQUIRE( parsed.spec->androidBackend == AndroidBackend::LegacyProcess );
        REQUIRE( parsed.spec->android.buffers == legacySpec.android.buffers );
        REQUIRE( parsed.spec->android.filterSpec == legacySpec.android.filterSpec );
        REQUIRE( parsed.spec->android.priority == legacySpec.android.priority );
        REQUIRE( parsed.spec->android.pid == legacySpec.android.pid );
        REQUIRE( parsed.spec->device.deviceId == legacySpec.device.deviceId );
        REQUIRE( parsed.spec->device.displayName == legacySpec.device.displayName );
        REQUIRE( parsed.spec->device.connection == legacySpec.device.connection );
        REQUIRE( parsed.spec->runIntent == legacySpec.runIntent );
        REQUIRE( parsed.spec->capture.autoReconnectEnabled
                 == legacySpec.capture.autoReconnectEnabled );
        REQUIRE( parsed.spec->capture.maxReconnectAttempts
                 == legacySpec.capture.maxReconnectAttempts );
        REQUIRE( parsed.spec->capture.captureMaxFileSize == legacySpec.capture.captureMaxFileSize );
        REQUIRE( parsed.spec->capture.captureBackupCount == legacySpec.capture.captureBackupCount );
        REQUIRE_FALSE( parsed.hasFatalDiagnostic() );

        // iOS mirror: typed options survive the transitional discriminator.
        auto legacyIos = makeIosSpec();
        legacyIos.iosBackend = IosBackend::LegacyProcess;
        const auto parsedIos
            = klogg::livelog::parsePersistedSpec( klogg::livelog::serializeSpec( legacyIos ) );
        REQUIRE( parsedIos.ok() );
        REQUIRE( parsedIos.spec->ios.level == legacyIos.ios.level );
        REQUIRE( parsedIos.spec->ios.categories == legacyIos.ios.categories );
        REQUIRE( parsedIos.spec->ios.subsystem == legacyIos.ios.subsystem );
        REQUIRE( parsedIos.spec->ios.outputFormat == legacyIos.ios.outputFormat );
        REQUIRE_FALSE( parsedIos.hasFatalDiagnostic() );

        WHEN( "Such a session is restored" )
        {
            ScopedSessionWindows windowsGuard;

            RecordingLiveSourceTransportFactory factory;
            auto appSession = std::make_shared<Session>( factory );

            const auto windowId = QStringLiteral( "livelog-legacy-window" );
            seedWindowFiles( windowId, { { legacySpec.displayName(), QStringLiteral( "adb_logcat" ),
                                           klogg::livelog::serializeSpec( legacySpec ) } } );

            auto opened = restoreSession( *appSession, windowId );

            THEN( "The tab loads read-only inert: no error surfaced, nothing armed" )
            {
                REQUIRE( opened.size() == 1 );
                REQUIRE( appSession->getDisplayName( opened.front().second )
                         == legacySpec.displayName() );

                // Even with a Running intent the compatibility backend never
                // starts: the user must reconnect through the built-in
                // services instead of a silently revived process transport.
                REQUIRE( factory.totalStarts() == 0 );
                REQUIRE( appSession->lastRestoreRejections().isEmpty() );
                auto* source = appSession->getAdbLogcatSource( opened.front().second );
                REQUIRE( source != nullptr );
                CHECK( source->isReadOnlyCompatibility() );
                CHECK_FALSE( source->connectSource() );
                CHECK_FALSE(
                    source->lastError().contains( QStringLiteral( "ADB" ), Qt::CaseInsensitive ) );
            }

            closeAndDeleteViews( *appSession, opened );
        }
    }
}

TEST_CASE( "Stopped restore creates one inert tab and zero transport starts",
           "[livelog-restore-arming][session]" )
{
    ScopedSessionWindows windowsGuard;
    RecordingLiveSourceTransportFactory factory;
    auto appSession = std::make_shared<Session>( factory );

    auto spec = makeAndroidSpec();
    spec.runIntent = live::RunIntent::Stopped;
    const auto windowId = QStringLiteral( "livelog-stopped-window" );
    seedWindowFiles( windowId, { { spec.displayName(), QStringLiteral( "adb_logcat" ),
                                   klogg::livelog::serializeSpec( spec ) } } );

    auto opened = restoreSession( *appSession, windowId );
    REQUIRE( opened.size() == 1 );
    CHECK( factory.createdTransports.empty() );
    CHECK( factory.totalStarts() == 0 );
    closeAndDeleteViews( *appSession, opened );
}

TEST_CASE( "restored output binding applies rolling limits before opening the file",
           "[livelog-restore-arming][session][capture]" )
{
    ScopedSessionWindows windowsGuard;
    QTemporaryDir outputDir;
    REQUIRE( outputDir.isValid() );
    RecordingLiveSourceTransportFactory factory;
    auto appSession = std::make_shared<Session>( factory );

    auto spec = makeAndroidSpec();
    spec.runIntent = live::RunIntent::Stopped;
    spec.boundOutputFile = outputDir.filePath( QStringLiteral( "restored.log" ) );
    spec.capture.captureMaxFileSize = 32;
    spec.capture.captureBackupCount = 2;
    const auto windowId = QStringLiteral( "livelog-rolling-output-window" );
    seedWindowFiles( windowId, { { spec.displayName(), QStringLiteral( "adb_logcat" ),
                                   klogg::livelog::serializeSpec( spec ) } } );

    auto opened = restoreSession( *appSession, windowId );
    REQUIRE( opened.size() == 1 );
    auto* const view = dynamic_cast<NullView*>( opened.front().second );
    REQUIRE( view != nullptr );
    auto logData = std::dynamic_pointer_cast<StreamingLogData>( view->data() );
    REQUIRE( logData != nullptr );

    for ( int line = 0; line < 10; ++line ) {
        logData->appendUtf8( QByteArrayLiteral( "0123456789\n" ) );
    }
    logData->finishInput();

    CHECK( QFile::exists( spec.boundOutputFile + QStringLiteral( ".0" ) ) );
    closeAndDeleteViews( *appSession, opened );
}

TEST_CASE( "Restore remaps the selected index after rejecting earlier entries",
           "[livelog-restore-arming][session]" )
{
    ScopedSessionWindows windowsGuard;
    RecordingLiveSourceTransportFactory factory;
    auto appSession = std::make_shared<Session>( factory );
    const auto windowId = QStringLiteral( "livelog-index-remap-window" );

    auto rejectedSpec = makeAndroidSpec();
    rejectedSpec.device.deviceId.clear();
    auto acceptedSpec = makeAndroidSpec( IosCaptureId );
    acceptedSpec.runIntent = live::RunIntent::Stopped;
    seedWindowFiles( windowId,
                     { { QStringLiteral( "Rejected" ), QStringLiteral( "adb_logcat" ),
                         klogg::livelog::serializeSpec( rejectedSpec ) },
                       { QStringLiteral( "Accepted" ), QStringLiteral( "adb_logcat" ),
                         klogg::livelog::serializeSpec( acceptedSpec ) } },
                     1 );

    int restoredCurrentIndex = -1;
    auto opened = restoreSession( *appSession, windowId, &restoredCurrentIndex );
    REQUIRE( opened.size() == 1 );
    CHECK( restoredCurrentIndex == 0 );
    closeAndDeleteViews( *appSession, opened );
}

TEST_CASE( "Restore rejects duplicate active capture storage ids across source kinds",
           "[livelog-restore-arming][session]" )
{
    ScopedSessionWindows windowsGuard;
    RecordingLiveSourceTransportFactory factory;
    auto appSession = std::make_shared<Session>( factory );
    const auto windowId = QStringLiteral( "livelog-capture-collision-window" );

    auto android = makeAndroidSpec();
    android.runIntent = live::RunIntent::Stopped;
    auto ios = makeSupportedIosSpec( AndroidCaptureId );
    ios.runIntent = live::RunIntent::Stopped;
    seedWindowFiles( windowId, { { android.displayName(), QStringLiteral( "adb_logcat" ),
                                   klogg::livelog::serializeSpec( android ) },
                                 { ios.displayName(), QStringLiteral( "ios_log_stream" ),
                                   klogg::livelog::serializeSpec( ios ) } } );

    auto opened = restoreSession( *appSession, windowId );
    REQUIRE( opened.size() == 1 );
    REQUIRE( appSession->lastRestoreRejections().size() == 1 );
    CHECK( appSession->lastRestoreRejections().front().contains( QStringLiteral( "capture" ),
                                                                 Qt::CaseInsensitive ) );
    closeAndDeleteViews( *appSession, opened );
}

TEST_CASE( "Unavailable transport does not prevent inert restoration",
           "[livelog-restore-arming][session][inert-restore]" )
{
    ScopedSessionWindows windowsGuard;
    UnavailableLiveSourceTransportFactory factory;
    auto appSession = std::make_shared<Session>( factory );
    const auto windowId = QStringLiteral( "livelog-unavailable-factory-window" );
    const auto spec = makeSupportedIosSpec();
    seedWindowFiles( windowId, { { spec.displayName(), QStringLiteral( "ios_log_stream" ),
                                   klogg::livelog::serializeSpec( spec ) } } );

    auto opened = restoreSession( *appSession, windowId );
    REQUIRE( opened.size() == 1 );
    CHECK( factory.createCalls == 0 );
    CHECK( appSession->lastRestoreRejections().isEmpty() );
    auto* controller = appSession->getLiveLogController( opened.front().second );
    REQUIRE( controller != nullptr );
    CHECK( controller->snapshot().source.status == live::SourceStatus::Stopped );
    closeAndDeleteViews( *appSession, opened );
}

TEST_CASE( "Each restore pass replaces stale notices and rejections",
           "[livelog-restore-arming][session]" )
{
    ScopedSessionWindows windowsGuard;
    RecordingLiveSourceTransportFactory factory;
    auto appSession = std::make_shared<Session>( factory );
    const auto windowId = QStringLiteral( "livelog-repeat-restore-window" );

    auto rejectedSpec = makeAndroidSpec();
    rejectedSpec.device.deviceId.clear();
    seedWindowFiles( windowId, { { rejectedSpec.displayName(), QStringLiteral( "adb_logcat" ),
                                   klogg::livelog::serializeSpec( rejectedSpec ) } } );
    auto rejected = restoreSession( *appSession, windowId );
    REQUIRE( rejected.empty() );
    REQUIRE_FALSE( appSession->lastRestoreRejections().isEmpty() );
    REQUIRE( appSession->lastRestoreNotices().isEmpty() );

    auto acceptedSpec = makeAndroidSpec();
    acceptedSpec.runIntent = live::RunIntent::Stopped;
    seedWindowFiles( windowId, { { acceptedSpec.displayName(), QStringLiteral( "adb_logcat" ),
                                   klogg::livelog::serializeSpec( acceptedSpec ) } } );
    auto accepted = restoreSession( *appSession, windowId );
    REQUIRE( accepted.size() == 1 );
    CHECK( appSession->lastRestoreRejections().isEmpty() );
    CHECK( appSession->lastRestoreNotices().isEmpty() );
    CHECK( factory.totalStarts() == 0 );
    closeAndDeleteViews( *appSession, accepted );
}

TEST_CASE( "Shutdown requested reentrantly during restore cannot arm a late transport",
           "[livelog-restore-arming][session]" )
{
    ScopedSessionWindows windowsGuard;
    RecordingLiveSourceTransportFactory factory;
    auto appSession = std::make_shared<Session>( factory );

    const auto spec = makeAndroidSpec();
    const auto windowId = QStringLiteral( "livelog-shutdown-during-restore-window" );
    seedWindowFiles( windowId, { { spec.displayName(), QStringLiteral( "adb_logcat" ),
                                   klogg::livelog::serializeSpec( spec ) } } );

    WindowSession windowSession{ appSession, windowId, 0 };
    int currentIndex = -1;
    auto opened = windowSession.restore(
        [ &appSession ]() -> ViewInterface* {
            appSession->setExitRequested( true );
            // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
            return new NullView();
        },
        &currentIndex );

    REQUIRE( opened.size() == 1 );
    CHECK( factory.createdTransports.empty() );
    CHECK( factory.totalStarts() == 0 );

    appSession->setExitRequested( false );
    closeAndDeleteViews( *appSession, opened );
}

TEST_CASE( "One configured reconnect attempt permits one retry after the initial run",
           "[livelog-restore-arming][session][retry-budget]" )
{
    ScopedSessionWindows windowsGuard;
    RecordingLiveSourceTransportFactory factory;
    auto appSession = std::make_shared<Session>( factory );
    auto spec = makeAndroidSpec();
    spec.capture.maxReconnectAttempts = 1;
    const auto windowId = QStringLiteral( "livelog-one-retry-window" );
    seedWindowFiles( windowId, { { spec.displayName(), QStringLiteral( "adb_logcat" ),
                                   klogg::livelog::serializeSpec( spec ) } } );

    auto opened = restoreSession( *appSession, windowId );
    REQUIRE( opened.size() == 1 );
    auto* controller = appSession->getLiveLogController( opened.front().second );
    auto* source = appSession->getAdbLogcatSource( opened.front().second );
    REQUIRE( controller != nullptr );
    REQUIRE( source != nullptr );
    REQUIRE( source->reconnectSource() );
    CHECK( controller->snapshot().infrastructure.ownership
           == live::InfrastructureOwnership::AppShared );

    controller->streamFailed(
        controller->snapshot().generation,
        live::LiveSourceError{ live::ErrorCategory::Stream, "lost", live::ErrorScope::Stream,
                               live::RetryPolicy::Backoff, "lost", "test" } );

    CHECK( controller->snapshot().source.status == live::SourceStatus::RetryWait );
    CHECK( controller->snapshot().source.retry.has_value() );
    CHECK( controller->snapshot().source.retry->attempt == 1u );
    closeAndDeleteViews( *appSession, opened );
}

TEST_CASE( "iOS catalog startup failure stays dormant until manual reconnect",
           "[livelog-restore-arming][session][ios-catalog][error][inert-restore]" )
{
    ScopedSessionWindows windowsGuard;
    RecordingLiveSourceTransportFactory factory;
    RecordingIosCatalog catalog;
    catalog.setStartupError( live::LiveSourceError{ live::ErrorCategory::Configuration,
                                                    "ios-catalog-api-incomplete",
                                                    live::ErrorScope::Infrastructure,
                                                    live::RetryPolicy::Never,
                                                    "The bundled native iOS catalog is unavailable.",
                                                    "missing symbols" } );
    auto appSession = std::make_shared<Session>( factory, nullptr, &catalog );
    const auto spec = makeSupportedIosSpec();
    const auto windowId = QStringLiteral( "livelog-ios-catalog-failure-window" );
    seedWindowFiles( windowId, { { spec.displayName(), QStringLiteral( "ios_log_stream" ),
                                   klogg::livelog::serializeSpec( spec ) } } );

    auto opened = restoreSession( *appSession, windowId );
    REQUIRE( opened.size() == 1 );
    auto* controller = appSession->getLiveLogController( opened.front().second );
    auto* source = appSession->getAdbLogcatSource( opened.front().second );
    REQUIRE( controller != nullptr );
    REQUIRE( source != nullptr );
    CHECK( controller->snapshot().source.status == live::SourceStatus::Stopped );
    CHECK( catalog.subscribeCalls == 0 );
    REQUIRE( source->reconnectSource() );
    CHECK( controller->snapshot().source.status == live::SourceStatus::Failed );
    REQUIRE( controller->snapshot().source.failure.has_value() );
    CHECK( controller->snapshot().source.failure->code == "ios-catalog-api-incomplete" );
    CHECK( factory.totalStarts() == 0u );
    closeAndDeleteViews( *appSession, opened );
}

TEST_CASE( "manual reconnect retries recoverable iOS metadata for the selected endpoint",
           "[livelog-restore-arming][session][ios-catalog][metadata][retry]" )
{
    ScopedSessionWindows windowsGuard;
    RecordingLiveSourceTransportFactory factory;
    RecordingIosCatalog catalog;
    auto appSession = std::make_shared<Session>( factory, nullptr, &catalog );
    const auto spec = makeSupportedIosSpec();
    const auto windowId = QStringLiteral( "livelog-ios-metadata-retry-window" );
    seedWindowFiles( windowId, { { spec.displayName(), QStringLiteral( "ios_log_stream" ),
                                   klogg::livelog::serializeSpec( spec ) } } );

    auto opened = restoreSession( *appSession, windowId );
    REQUIRE( opened.size() == 1 );
    auto* controller = appSession->getLiveLogController( opened.front().second );
    auto* source = appSession->getAdbLogcatSource( opened.front().second );
    REQUIRE( controller != nullptr );
    REQUIRE( source != nullptr );
    REQUIRE( source->reconnectSource() );

    klogg::livecapture::ios::IosCatalogEntry entry;
    entry.endpoint.udid = spec.device.deviceId.toStdString();
    entry.endpoint.connectionType = klogg::livecapture::ios::NativeConnectionType::Network;
    entry.epoch = 1u;
    entry.error = klogg::livecapture::ios::IosCatalogError{
        live::LiveSourceError{ live::ErrorCategory::Device, "ios-trust-pending",
                               live::ErrorScope::Device, live::RetryPolicy::AwaitUser,
                               "Trust this computer on the iOS device.", "trust pending" },
        live::AwaitingUserReason::Trust
    };
    catalog.publish( { 1u, { entry } } );

    REQUIRE( controller->snapshot().source.status == live::SourceStatus::AwaitingUser );
    REQUIRE( klogg::livecapture::projectLiveState( controller->snapshot() ).reconnectEnabled );
    REQUIRE( catalog.metadataRequests.empty() );

    controller->startRequested();

    REQUIRE( catalog.metadataRequests.size() == 1u );
    CHECK( catalog.metadataRequests.front() == entry.endpoint );
    closeAndDeleteViews( *appSession, opened );
}

TEST_CASE( "transient iOS metadata failure remains actionable and retries on reconnect",
           "[livelog-restore-arming][session][ios-catalog][metadata][backoff]" )
{
    ScopedSessionWindows windowsGuard;
    RecordingLiveSourceTransportFactory factory;
    RecordingIosCatalog catalog;
    auto appSession = std::make_shared<Session>( factory, nullptr, &catalog );
    const auto spec = makeSupportedIosSpec();
    const auto windowId = QStringLiteral( "livelog-ios-metadata-backoff-window" );
    seedWindowFiles( windowId, { { spec.displayName(), QStringLiteral( "ios_log_stream" ),
                                   klogg::livelog::serializeSpec( spec ) } } );

    auto opened = restoreSession( *appSession, windowId );
    REQUIRE( opened.size() == 1 );
    auto* controller = appSession->getLiveLogController( opened.front().second );
    auto* source = appSession->getAdbLogcatSource( opened.front().second );
    REQUIRE( controller != nullptr );
    REQUIRE( source != nullptr );
    REQUIRE( source->reconnectSource() );

    klogg::livecapture::ios::IosCatalogEntry entry;
    entry.endpoint.udid = spec.device.deviceId.toStdString();
    entry.endpoint.connectionType = klogg::livecapture::ios::NativeConnectionType::Network;
    entry.epoch = 1u;
    entry.error = klogg::livecapture::ios::IosCatalogError{
        live::LiveSourceError{ live::ErrorCategory::Device, "ios-lockdown-timeout",
                               live::ErrorScope::Device, live::RetryPolicy::Backoff,
                               "The iOS device metadata request timed out.", "lockdown timeout" },
        std::nullopt
    };
    catalog.publish( { 1u, { entry } } );

    REQUIRE( controller->snapshot().source.status == live::SourceStatus::Failed );
    REQUIRE( controller->snapshot().source.failure.has_value() );
    CHECK( controller->snapshot().source.failure->code == "ios-lockdown-timeout" );
    REQUIRE( catalog.metadataRequests.empty() );

    REQUIRE( source->reconnectSource() );

    REQUIRE( catalog.metadataRequests.size() == 1u );
    CHECK( catalog.metadataRequests.front() == entry.endpoint );
    CHECK( controller->snapshot().source.status == live::SourceStatus::WaitingForDevice );
    CHECK_FALSE( controller->snapshot().source.failure.has_value() );

    entry.epoch = 2u;
    entry.error.reset();
    catalog.publish( { 2u, { entry } } );

    CHECK( factory.totalStarts() == 1u );
    CHECK( controller->snapshot().source.status == live::SourceStatus::OpeningStream );
    closeAndDeleteViews( *appSession, opened );
}

TEST_CASE( "iOS catalog snapshots arm only after reconnect and retire the matching live tab",
           "[livelog-restore-arming][session][ios-catalog][inert-restore]" )
{
    ScopedSessionWindows windowsGuard;
    RecordingLiveSourceTransportFactory factory;
    RecordingIosCatalog catalog;
    auto appSession = std::make_shared<Session>( factory, nullptr, &catalog );
    const auto spec = makeSupportedIosSpec();
    const auto windowId = QStringLiteral( "livelog-ios-catalog-window" );
    seedWindowFiles( windowId, { { spec.displayName(), QStringLiteral( "ios_log_stream" ),
                                   klogg::livelog::serializeSpec( spec ) } } );

    auto opened = restoreSession( *appSession, windowId );
    REQUIRE( opened.size() == 1 );
    auto* controller = appSession->getLiveLogController( opened.front().second );
    auto* source = appSession->getAdbLogcatSource( opened.front().second );
    REQUIRE( controller != nullptr );
    REQUIRE( source != nullptr );
    CHECK( controller->snapshot().source.status == live::SourceStatus::Stopped );
    REQUIRE( source->reconnectSource() );
    CHECK( controller->snapshot().source.status == live::SourceStatus::WaitingForDevice );
    CHECK( factory.totalStarts() == 0u );

    klogg::livecapture::ios::IosCatalogEntry entry;
    entry.endpoint.udid = spec.device.deviceId.toStdString();
    entry.endpoint.connectionType = klogg::livecapture::ios::NativeConnectionType::Network;
    catalog.publish( { 1u, { entry } } );

    REQUIRE( factory.totalStarts() == 1u );
    const auto generation = controller->snapshot().generation;
    factory.createdTransports.back()->publishState(
        generation, LiveSourceTransport::State::Connected );
    REQUIRE( controller->snapshot().source.status == live::SourceStatus::Streaming );

    catalog.publish( { 2u, {} } );
    CHECK( controller->snapshot().source.status == live::SourceStatus::WaitingForDevice );
    CHECK_FALSE( factory.createdTransports.front()->stopGenerations.empty() );
    closeAndDeleteViews( *appSession, opened );
}

TEST_CASE( "iOS terminal diagnostics survive controller cancellation exactly once",
           "[livelog-restore-arming][session][ios][error][ordering][reentrant]" )
{
    ScopedSessionWindows windowsGuard;
    RecordingLiveSourceTransportFactory factory;
    RecordingIosCatalog catalog;
    auto appSession = std::make_shared<Session>( factory, nullptr, &catalog );
    const auto spec = makeSupportedIosSpec();
    const auto windowId = QStringLiteral( "livelog-ios-terminal-diagnostic-window" );
    seedWindowFiles( windowId, { { spec.displayName(), QStringLiteral( "ios_log_stream" ),
                                   klogg::livelog::serializeSpec( spec ) } } );

    auto opened = restoreSession( *appSession, windowId );
    REQUIRE( opened.size() == 1 );
    auto* controller = appSession->getLiveLogController( opened.front().second );
    auto* source = appSession->getAdbLogcatSource( opened.front().second );
    REQUIRE( controller != nullptr );
    REQUIRE( source != nullptr );
    REQUIRE( source->reconnectSource() );

    klogg::livecapture::ios::IosCatalogEntry entry;
    entry.endpoint.udid = spec.device.deviceId.toStdString();
    entry.endpoint.connectionType = klogg::livecapture::ios::NativeConnectionType::Network;
    catalog.publish( { 1u, { entry } } );
    REQUIRE( factory.createdTransports.size() == 1u );
    auto* transport = factory.createdTransports.front();
    REQUIRE( transport != nullptr );
    const auto generation = controller->snapshot().generation;
    transport->publishState( generation, LiveSourceTransport::State::Connected );
    REQUIRE( controller->snapshot().source.status == live::SourceStatus::Streaming );

    const live::LiveSourceError terminalError{
        live::ErrorCategory::Backend,
        "ios-native-terminal-test",
        live::ErrorScope::Stream,
        live::RetryPolicy::Never,
        "The native iOS stream terminated.",
        "lockdown receive failed: connection reset"
    };
    const auto expectedText = QStringLiteral(
        "The native iOS stream terminated.\nlockdown receive failed: connection reset" );
    std::vector<QString> sourceDiagnostics;
    bool diagnosticObservedBeforeStop = false;
    QObject::connect( source, &AdbLogcatSource::errorOccurred, source,
                      [ & ]( const QString& diagnostic ) {
                          sourceDiagnostics.push_back( diagnostic );
                          diagnosticObservedBeforeStop = transport->stopGenerations.empty();
                      } );

    transport->publishTerminalError( generation, terminalError );

    CHECK( transport->stopObservedBeforeTerminalText );
    CHECK( transport->stopGenerations == std::vector<live::Generation>{ generation } );
    CHECK( sourceDiagnostics == std::vector<QString>{ expectedText } );
    CHECK( diagnosticObservedBeforeStop );
    CHECK( source->lastError() == expectedText );
    CHECK( controller->snapshot().source.status == live::SourceStatus::Failed );
    REQUIRE( controller->snapshot().source.failure.has_value() );
    CHECK( controller->snapshot().source.failure->code == "ios-native-terminal-test" );
    CHECK( controller->snapshot().source.failure->retryPolicy == live::RetryPolicy::Never );
    CHECK( controller->snapshot().source.failure->nativeDetail
           == "lockdown receive failed: connection reset" );
    CHECK( controller->snapshot().source.failure->code != "live-stream-failed" );

    transport->publishTextError( generation );
    QCoreApplication::sendPostedEvents( source, QEvent::MetaCall );
    CHECK( transport->terminalTextEmissions == 2 );
    CHECK( sourceDiagnostics == std::vector<QString>{ expectedText } );
    CHECK( source->lastError() == expectedText );

    closeAndDeleteViews( *appSession, opened );
}

TEST_CASE( "queued iOS snapshot from a retired observation cannot arm a restarted run",
           "[livelog-restore-arming][session][ios-catalog]" )
{
    ScopedSessionWindows windowsGuard;
    RecordingLiveSourceTransportFactory factory;
    RecordingIosCatalog catalog;
    auto appSession = std::make_shared<Session>( factory, nullptr, &catalog );
    const auto spec = makeSupportedIosSpec();
    const auto windowId = QStringLiteral( "livelog-ios-stale-catalog-window" );
    seedWindowFiles( windowId, { { spec.displayName(), QStringLiteral( "ios_log_stream" ),
                                   klogg::livelog::serializeSpec( spec ) } } );

    auto opened = restoreSession( *appSession, windowId );
    REQUIRE( opened.size() == 1 );
    auto* controller = appSession->getLiveLogController( opened.front().second );
    auto* source = appSession->getAdbLogcatSource( opened.front().second );
    REQUIRE( controller != nullptr );
    REQUIRE( source != nullptr );
    REQUIRE( source->reconnectSource() );
    const auto staleCallback = catalog.callbackCopy();
    REQUIRE( static_cast<bool>( staleCallback ) );

    controller->stopRequested();
    REQUIRE( controller->snapshot().runIntent == live::RunIntent::Stopped );
    controller->startRequested();
    REQUIRE( controller->snapshot().source.status == live::SourceStatus::WaitingForDevice );

    klogg::livecapture::ios::IosCatalogEntry staleEntry;
    staleEntry.endpoint.udid = spec.device.deviceId.toStdString();
    staleEntry.endpoint.connectionType = klogg::livecapture::ios::NativeConnectionType::Network;
    staleCallback( { catalog.snapshot().generation, { staleEntry } } );

    CHECK( factory.totalStarts() == 0u );
    CHECK( controller->snapshot().source.status == live::SourceStatus::WaitingForDevice );
    closeAndDeleteViews( *appSession, opened );
}

TEST_CASE( "queued iOS catalog invalidation observes the latest same-generation snapshot",
           "[livelog-restore-arming][session][ios-catalog]" )
{
    ScopedSessionWindows windowsGuard;
    RecordingLiveSourceTransportFactory factory;
    RecordingIosCatalog catalog;
    auto appSession = std::make_shared<Session>( factory, nullptr, &catalog );
    const auto spec = makeSupportedIosSpec();
    const auto windowId = QStringLiteral( "livelog-ios-latest-catalog-window" );
    seedWindowFiles( windowId, { { spec.displayName(), QStringLiteral( "ios_log_stream" ),
                                   klogg::livelog::serializeSpec( spec ) } } );

    auto opened = restoreSession( *appSession, windowId );
    REQUIRE( opened.size() == 1 );
    auto* controller = appSession->getLiveLogController( opened.front().second );
    auto* source = appSession->getAdbLogcatSource( opened.front().second );
    REQUIRE( controller != nullptr );
    REQUIRE( source != nullptr );
    REQUIRE( source->reconnectSource() );

    klogg::livecapture::ios::IosCatalogEntry entry;
    entry.endpoint.udid = spec.device.deviceId.toStdString();
    entry.endpoint.connectionType = klogg::livecapture::ios::NativeConnectionType::Network;
    catalog.publish( { 1u, { entry } } );
    REQUIRE( factory.totalStarts() == 1u );
    const auto generation = controller->snapshot().generation;
    factory.createdTransports.front()->publishState( generation,
                                                     LiveSourceTransport::State::Connected );
    REQUIRE( controller->snapshot().source.status == live::SourceStatus::Streaming );

    const auto callback = catalog.callbackCopy();
    REQUIRE( static_cast<bool>( callback ) );
    std::thread observer( [ callback, entry ] { callback( { 1u, { entry } } ); } );
    observer.join();

    catalog.replaceSnapshot( { 1u, {} } );
    REQUIRE( source->reconnectSource() );
    REQUIRE( controller->snapshot().source.status == live::SourceStatus::WaitingForDevice );
    REQUIRE( factory.totalStarts() == 1u );

    QCoreApplication::sendPostedEvents( source, QEvent::MetaCall );

    CHECK( controller->snapshot().source.status == live::SourceStatus::WaitingForDevice );
    CHECK( factory.totalStarts() == 1u );
    closeAndDeleteViews( *appSession, opened );
}

TEST_CASE( "Failed output rebind keeps the rolled-back per-tab binding healthy",
           "[livelog-restore-arming][session][capture]" )
{
    ScopedSessionWindows windowsGuard;
    RecordingLiveSourceTransportFactory factory;
    auto appSession = std::make_shared<Session>( factory );
    const auto spec = makeAndroidSpec();
    const auto windowId = QStringLiteral( "livelog-capture-degradation-window" );
    seedWindowFiles( windowId, { { spec.displayName(), QStringLiteral( "adb_logcat" ),
                                   klogg::livelog::serializeSpec( spec ) } } );

    auto opened = restoreSession( *appSession, windowId );
    REQUIRE( opened.size() == 1 );
    auto* controller = appSession->getLiveLogController( opened.front().second );
    auto* source = appSession->getAdbLogcatSource( opened.front().second );
    REQUIRE( controller != nullptr );
    REQUIRE( source != nullptr );
    REQUIRE( source->reconnectSource() );
    const auto generation = controller->snapshot().generation;
    factory.createdTransports.back()->publishState(
        generation, LiveSourceTransport::State::Connected );
    REQUIRE( controller->snapshot().source.status == live::SourceStatus::Streaming );

    QTemporaryDir outputRoot;
    REQUIRE( outputRoot.isValid() );
    const auto outputPath = outputRoot.filePath( QStringLiteral( "capture.log" ) );
    REQUIRE( source->bindOutputFile( outputPath, LiveLogSaveAnsiMode::Strip ) );

    REQUIRE_FALSE( source->bindOutputFile( outputRoot.path(), LiveLogSaveAnsiMode::Strip ) );
    CHECK( controller->snapshot().outputBinding == live::OutputBindingState::Healthy );
    CHECK( controller->snapshot().source.status == live::SourceStatus::Streaming );
    CHECK_FALSE( controller->snapshot().outputBindingError.has_value() );

    REQUIRE( source->bindOutputFile( outputPath, LiveLogSaveAnsiMode::Strip ) );
    CHECK( controller->snapshot().outputBinding == live::OutputBindingState::Healthy );

    controller->stopRequested();
    CHECK( controller->snapshot().outputBinding == live::OutputBindingState::Healthy );
    closeAndDeleteViews( *appSession, opened );
}

TEST_CASE( "Capture output degradation survives stop and fresh start until rebind succeeds",
           "[livelog-restore-arming][session][capture]" )
{
    ScopedSessionWindows windowsGuard;
    RecordingLiveSourceTransportFactory factory;
    auto appSession = std::make_shared<Session>( factory );
    auto spec = makeAndroidSpec();
    spec.capture.captureMaxFileSize = 1;
    spec.capture.captureBackupCount = 1;
    const auto windowId = QStringLiteral( "livelog-capture-degradation-restart-window" );
    seedWindowFiles( windowId, { { spec.displayName(), QStringLiteral( "adb_logcat" ),
                                   klogg::livelog::serializeSpec( spec ) } } );

    auto opened = restoreSession( *appSession, windowId );
    REQUIRE( opened.size() == 1 );
    auto* controller = appSession->getLiveLogController( opened.front().second );
    auto* source = appSession->getAdbLogcatSource( opened.front().second );
    REQUIRE( controller != nullptr );
    REQUIRE( source != nullptr );
    REQUIRE( source->reconnectSource() );
    const auto generation = controller->snapshot().generation;
    factory.createdTransports.back()->publishState( generation,
                                                    LiveSourceTransport::State::Connected );
    REQUIRE( controller->snapshot().source.status == live::SourceStatus::Streaming );

    QTemporaryDir outputRoot;
    REQUIRE( outputRoot.isValid() );
    const auto outputPath = outputRoot.filePath( QStringLiteral( "capture.log" ) );
    REQUIRE( source->bindOutputFile( outputPath, LiveLogSaveAnsiMode::Strip ) );
    const auto rotationPath = outputPath + QStringLiteral( ".tmp_rotate" );
    REQUIRE( QDir().mkpath( rotationPath ) );

    factory.createdTransports.back()->publishBytes( generation, QByteArrayLiteral( "line\n" ) );
    REQUIRE( controller->snapshot().outputBinding == live::OutputBindingState::Degraded );

    controller->stopRequested();
    CHECK( controller->snapshot().outputBinding == live::OutputBindingState::Degraded );

    controller->startRequested();
    CHECK( controller->snapshot().outputBinding == live::OutputBindingState::Degraded );
    REQUIRE( controller->snapshot().outputBindingError.has_value() );
    CHECK( controller->snapshot().outputBindingError->code == "output-write-failed" );
    CHECK( controller->snapshot().outputBindingError->message
           == "The bound capture output could not be written." );
    CHECK( controller->snapshot().outputBindingError->nativeDetail.empty() );

    REQUIRE( QDir( rotationPath ).removeRecursively() );
    REQUIRE( source->bindOutputFile( outputPath, LiveLogSaveAnsiMode::Strip ) );
    CHECK( controller->snapshot().outputBinding == live::OutputBindingState::Healthy );
    CHECK_FALSE( controller->snapshot().outputBindingError.has_value() );
    closeAndDeleteViews( *appSession, opened );
}
