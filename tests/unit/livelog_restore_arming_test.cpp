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
 * Task 6 cycle 2 RED contracts: restore ARMING.
 *
 * Cycle 1 pinned what a persisted live-log session IS (typed spec, parse
 * diagnostics, accept gate). This file pins what restore DOES with it:
 *
 *   1. Restored tabs route through validateForAccept before anything arms:
 *      every spec the gate rejects fatally must also refuse to arm a run,
 *      and Session-level restore must surface structured rejections through
 *      the existing lastRestoreRejections() path with actionable text
 *      (including reopen guidance for raw-CLI sessions).
 *   2. Valid Running-intent specs drive initialLiveStateEvents through the
 *      Task 2 state reducer so the tab starts WaitingForInfrastructure /
 *      WaitingForDevice instead of Stopped — observable at session level as
 *      an armed transport on the recording factory.
 *   3. Transitional legacy_process sessions stay loadable (options intact)
 *      but never arm, and never surface as errors.
 *
 * Everything here is deterministic: fixed capture ids, a recording transport
 * factory instead of real processes/devices, no waits, no wall-clock reads.
 * (The once-per-capture-id migration notice lives in
 * livelog_migration_notice_test.cpp: its surfacing channel does not exist
 * yet, so that file intentionally does not compile until the production side
 * lands.)
 */

#include <catch2/catch.hpp>

#include <algorithm>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QTemporaryDir>
#include <QStringList>

#include "adblogcatsource.h"
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
        return {};
    }

    void publishState( Generation generation, State state )
    {
        Q_EMIT stateChanged( generation, state );
    }

    std::vector<Generation> startGenerations;
    std::vector<Generation> stopGenerations;
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
        callback_ = std::move( callback );
        return 1u;
    }

    void unsubscribe( SubscriptionId subscription ) override
    {
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

    SnapshotCallback callbackCopy() const
    {
        return callback_;
    }

    void setStartupError( live::LiveSourceError error )
    {
        startupError_ = std::move( error );
    }

    std::vector<klogg::livecapture::ios::IosEndpointKey> metadataRequests;

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
    // The arming mapping is the last defense before a restored tab starts
    // talking to hardware: it must be fail-closed in EXACTLY the same cases
    // as the accept gate, including the transitional backends that cycle 1
    // allowed into the schema for persistence compatibility only.
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

TEST_CASE( "Restored Running intent drives the reducer into a waiting state, never Stopped",
           "[livelog-restore-arming]" )
{
    const live::LiveStateConfig config{ 5u, std::chrono::seconds{ 10 } };

    // Infrastructure unknown at restore time (the common case): the restored
    // tab must present WaitingForInfrastructure, not a silent Stopped state.
    auto snapshot = live::initialLiveState();
    REQUIRE( snapshot.source.status == live::SourceStatus::Stopped );
    for ( const auto& event :
          klogg::livelog::initialLiveStateEvents( makeAndroidSpec(), live::Timestamp{ 1500 } ) ) {
        snapshot = live::reduce( snapshot, event, config ).snapshot;
    }
    REQUIRE( snapshot.runIntent == live::RunIntent::Running );
    REQUIRE( snapshot.source.status == live::SourceStatus::WaitingForInfrastructure );

    // Infrastructure already ready when the session comes back: straight to
    // waiting for the device, still never Stopped.
    auto readySnapshot = live::initialLiveState();
    live::InfrastructureChanged infrastructureReady;
    infrastructureReady.status = live::InfrastructureStatus::Ready;
    infrastructureReady.ownership = live::InfrastructureOwnership::AppShared;
    infrastructureReady.at = live::Timestamp{ 1000 };
    readySnapshot = live::reduce( readySnapshot, infrastructureReady, config ).snapshot;
    for ( const auto& event :
          klogg::livelog::initialLiveStateEvents( makeAndroidSpec(), live::Timestamp{ 1500 } ) ) {
        readySnapshot = live::reduce( readySnapshot, event, config ).snapshot;
    }
    REQUIRE( readySnapshot.runIntent == live::RunIntent::Running );
    REQUIRE( readySnapshot.source.status == live::SourceStatus::WaitingForDevice );

    // Source neutrality: iOS sessions drive the same waiting states.
    auto iosSnapshot = live::initialLiveState();
    for ( const auto& event :
          klogg::livelog::initialLiveStateEvents( makeSupportedIosSpec(), live::Timestamp{ 2500 } ) ) {
        iosSnapshot = live::reduce( iosSnapshot, event, config ).snapshot;
    }
    REQUIRE( iosSnapshot.runIntent == live::RunIntent::Running );
    REQUIRE( iosSnapshot.source.status != live::SourceStatus::Stopped );
    REQUIRE( iosSnapshot.source.status == live::SourceStatus::WaitingForInfrastructure );

    // Stopped tabs restore inert: no synthetic start exists and the state
    // machine stays Stopped.
    auto stoppedSpec = makeAndroidSpec();
    stoppedSpec.runIntent = live::RunIntent::Stopped;
    REQUIRE(
        klogg::livelog::initialLiveStateEvents( stoppedSpec, live::Timestamp{ 1500 } ).empty() );
    REQUIRE( live::initialLiveState().source.status == live::SourceStatus::Stopped );
    REQUIRE( live::initialLiveState().runIntent == live::RunIntent::Stopped );
}

SCENARIO( "Restore arms valid running sessions through the transport factory",
          "[livelog-restore-arming][session]" )
{
    ScopedSessionWindows windowsGuard;

    RecordingLiveSourceTransportFactory factory;
    auto appSession = std::make_shared<Session>( factory );

    const auto windowId = QStringLiteral( "livelog-arm-running-window" );
    auto spec = makeAndroidSpec();

    GIVEN( "A saved window with one valid running-intent live session" )
    {
        seedWindowFiles( windowId, { { spec.displayName(), QStringLiteral( "adb_logcat" ),
                                       klogg::livelog::serializeSpec( spec ) } } );

        WHEN( "The window session is restored" )
        {
            auto opened = restoreSession( *appSession, windowId );

            THEN( "The live tab comes back and its run is already armed" )
            {
                REQUIRE( opened.size() == 1 );
                REQUIRE( appSession->getDocumentKind( opened.front().second )
                         == DocumentKind::AdbLogcat );
                REQUIRE( appSession->getDisplayName( opened.front().second )
                         == spec.displayName() );

                // Exactly one transport was created for the exact device the
                // spec recorded, and restore started it: the tab resumes its
                // running intent without user interaction.
                REQUIRE( factory.createdTransports.size() == 1 );
                REQUIRE( factory.requestedConfigs.front().deviceId == spec.device.deviceId );
                REQUIRE( factory.totalStarts() == 1 );
                auto* source = appSession->getAdbLogcatSource( opened.front().second );
                REQUIRE( source != nullptr );
                CHECK( source->sessionData().runIntent == live::RunIntent::Running );
                source->disconnectSource();
                CHECK( source->sessionData().runIntent == live::RunIntent::Stopped );
                REQUIRE( appSession->lastRestoreRejections().isEmpty() );
            }

            closeAndDeleteViews( *appSession, opened );
        }
    }
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

TEST_CASE( "Running restore with unavailable transport factory is rejected transactionally",
           "[livelog-restore-arming][session]" )
{
    ScopedSessionWindows windowsGuard;
    UnavailableLiveSourceTransportFactory factory;
    auto appSession = std::make_shared<Session>( factory );
    const auto windowId = QStringLiteral( "livelog-unavailable-factory-window" );
    const auto spec = makeSupportedIosSpec();
    seedWindowFiles( windowId, { { spec.displayName(), QStringLiteral( "ios_log_stream" ),
                                   klogg::livelog::serializeSpec( spec ) } } );

    auto opened = restoreSession( *appSession, windowId );
    CHECK( opened.empty() );
    CHECK( factory.createCalls == 1 );
    REQUIRE( appSession->lastRestoreRejections().size() == 1 );
    CHECK( appSession->lastRestoreRejections().front().contains( QStringLiteral( "unavailable" ),
                                                                 Qt::CaseInsensitive ) );
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
    REQUIRE( controller != nullptr );
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

TEST_CASE( "iOS catalog startup failure surfaces as a failed restored tab",
           "[livelog-restore-arming][session][ios-catalog][error]" )
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
    REQUIRE( controller != nullptr );
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
    REQUIRE( controller != nullptr );

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

TEST_CASE( "iOS catalog snapshots arm and retire only the matching live tab",
           "[livelog-restore-arming][session][ios-catalog]" )
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
    REQUIRE( controller != nullptr );
    CHECK( controller->snapshot().source.status == live::SourceStatus::WaitingForDevice );
    CHECK( factory.totalStarts() == 0u );

    klogg::livecapture::ios::IosCatalogEntry entry;
    entry.endpoint.udid = spec.device.deviceId.toStdString();
    entry.endpoint.connectionType = klogg::livecapture::ios::NativeConnectionType::Network;
    catalog.publish( { 1u, { entry } } );

    REQUIRE( factory.totalStarts() == 1u );
    const auto generation = controller->snapshot().generation;
    factory.createdTransports.front()->publishState(
        generation, LiveSourceTransport::State::Connected );
    REQUIRE( controller->snapshot().source.status == live::SourceStatus::Streaming );

    catalog.publish( { 2u, {} } );
    CHECK( controller->snapshot().source.status == live::SourceStatus::WaitingForDevice );
    CHECK_FALSE( factory.createdTransports.front()->stopGenerations.empty() );
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
    REQUIRE( controller != nullptr );
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

TEST_CASE( "Capture output degradation reaches the per-tab controller without disconnecting",
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
    const auto generation = controller->snapshot().generation;
    factory.createdTransports.front()->publishState(
        generation, LiveSourceTransport::State::Connected );
    REQUIRE( controller->snapshot().source.status == live::SourceStatus::Streaming );

    Q_EMIT source->captureOutputChanged( false, QStringLiteral( "disk full" ) );
    CHECK( controller->snapshot().capture == live::CaptureState::OutputDegraded );
    CHECK( controller->snapshot().source.status == live::SourceStatus::Streaming );

    Q_EMIT source->captureOutputChanged( true, QString{} );
    CHECK( controller->snapshot().capture == live::CaptureState::OpenHealthy );

    controller->stopRequested();
    CHECK( controller->snapshot().capture == live::CaptureState::Finalized );
    closeAndDeleteViews( *appSession, opened );
}
