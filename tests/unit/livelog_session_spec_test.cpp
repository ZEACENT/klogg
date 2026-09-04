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
 * Task 6 cycle 1 RED contracts for the versioned, typed, source-neutral
 * live-log session spec that replaces Android/iOS executable and free-form
 * argument persistence (the flat AdbLogcatSessionData JSON written by
 * adblogcatsource.cpp today).
 *
 * Every function under contract here is pure data plumbing: parse, serialize,
 * validate, and map-to-live-state-events must never touch PATH, the Android
 * SDK, a Python environment, processes, devices, or wall-clock time. That
 * property is asserted directly by the environment-scrubbing test case below.
 */

#include <catch2/catch.hpp>

#include <algorithm>
#include <chrono>
#include <limits>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

#include <QtGlobal>

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include "livelogsession.h"
#include "livestate.h"

namespace {

namespace live = klogg::livecapture;

using klogg::livelog::AndroidBackend;
using klogg::livelog::DeviceIdentity;
using klogg::livelog::Diagnostic;
using klogg::livelog::IosBackend;
using klogg::livelog::LiveLogSessionSpec;
using klogg::livelog::ParseResult;
using klogg::livelog::SourceKind;
using IosOptions = klogg::livelog::IosOptions;

// Fixed identifiers keep every assertion below deterministic (no generator,
// no clock, no randomness anywhere in this file).
QString androidCaptureId()
{
    return QStringLiteral( "3f6b1d5e-9c24-4a17-8f2e-6d1a2b3c4d50" );
}

QString iosCaptureId()
{
    return QStringLiteral( "7a2c9e81-4b3d-4f60-9a15-2c7d8e9f0a12" );
}

LiveLogSessionSpec makeAndroidSpec()
{
    LiveLogSessionSpec spec;
    spec.captureId = androidCaptureId();
    spec.sourceKind = SourceKind::AndroidLogcat;
    spec.androidBackend = AndroidBackend::SmartSocket;
    spec.device.deviceId = QStringLiteral( "R58NC123ABC" );
    spec.device.displayName = QStringLiteral( "Pixel 8 Pro" );
    spec.device.connection = DeviceIdentity::Connection::Usb;
    spec.runIntent = live::RunIntent::Running;
    spec.android.buffers = QStringList{ QStringLiteral( "main" ), QStringLiteral( "system" ),
                                        QStringLiteral( "crash" ) };
    spec.android.filterSpec = QStringLiteral( "TagA:D TagB:S" );
    spec.android.priority = QStringLiteral( "INFO" );
    spec.android.pid = std::optional<int>{ 12345 };
    spec.capture.ansiOutputEnabled = true;
    spec.capture.preserveAnsiOnSave = true;
    spec.capture.autoReconnectEnabled = true;
    spec.capture.maxReconnectAttempts = 7;
    spec.capture.captureMaxFileSize = static_cast<qint64>( 32 ) * 1024 * 1024;
    spec.capture.captureBackupCount = 5;
    spec.boundOutputFile = QStringLiteral( "/tmp/klogg-capture.log" );
    return spec;
}

LiveLogSessionSpec makeIosSpec()
{
    LiveLogSessionSpec spec;
    spec.captureId = iosCaptureId();
    spec.sourceKind = SourceKind::IosSyslog;
    spec.iosBackend = IosBackend::Native;
    spec.device.deviceId = QStringLiteral( "00008101-001A2B3C4D5E" );
    spec.device.displayName = QStringLiteral( "iPhone 15 Pro" );
    spec.device.connection = DeviceIdentity::Connection::Network;
    spec.runIntent = live::RunIntent::Stopped;
    spec.ios.level = QStringLiteral( "debug" );
    spec.ios.categories = QStringList{ QStringLiteral( "network" ), QStringLiteral( "sqlite" ) };
    spec.ios.subsystem = QStringLiteral( "com.apple.network" );
    spec.ios.outputFormat = IosOptions::OutputFormat::Json;
    spec.capture.ansiOutputEnabled = false;
    spec.capture.preserveAnsiOnSave = false;
    spec.capture.autoReconnectEnabled = false;
    spec.capture.maxReconnectAttempts = 0;
    spec.capture.captureMaxFileSize = static_cast<qint64>( 16 ) * 1024 * 1024;
    spec.capture.captureBackupCount = 3;
    spec.boundOutputFile = QStringLiteral( "/tmp/ios-capture.log" );
    return spec;
}

LiveLogSessionSpec makeSupportedIosSpec()
{
    auto spec = makeIosSpec();
    spec.ios = {};
    return spec;
}

ParseResult parseSpec( const QString& json )
{
    return klogg::livelog::parsePersistedSpec( json );
}

QString serialized( const LiveLogSessionSpec& spec )
{
    return klogg::livelog::serializeSpec( spec );
}

QJsonObject objectFrom( const QString& json )
{
    return QJsonDocument::fromJson( json.toUtf8() ).object();
}

bool hasDiagnostic( const ParseResult& result, const char* expectedCode, bool fatal )
{
    return std::any_of(
        result.diagnostics.cbegin(), result.diagnostics.cend(),
        [ expectedCode, fatal ]( const Diagnostic& diagnostic ) {
            const auto severityMatches = fatal ? diagnostic.severity == Diagnostic::Severity::Fatal
                                               : diagnostic.severity != Diagnostic::Severity::Fatal;
            return severityMatches && diagnostic.code == QLatin1String( expectedCode );
        } );
}

bool hasFatalDiagnostic( const ParseResult& result, const char* expectedCode )
{
    return hasDiagnostic( result, expectedCode, true );
}

bool hasInfoDiagnostic( const ParseResult& result, const char* expectedCode )
{
    return hasDiagnostic( result, expectedCode, false );
}

// Scrubs resolver-adjacent environment variables for the duration of one test
// case and restores the original values afterwards. Any new-session path that
// consults PATH / the SDK / Python would observe the empty values and fail.
class ScopedScrubbedEnvironment {
public:
    explicit ScopedScrubbedEnvironment( std::initializer_list<const char*> names )
    {
        for ( const auto* name : names ) {
            Saved saved;
            saved.name = name;
            saved.existed = qEnvironmentVariableIsSet( name );
            saved.value = qgetenv( name );
            saved_.push_back( saved );
            qputenv( name, QByteArray{} );
        }
    }

    ~ScopedScrubbedEnvironment()
    {
        for ( const auto& saved : saved_ ) {
            if ( saved.existed ) {
                qputenv( saved.name, saved.value );
            }
            else {
                qunsetenv( saved.name );
            }
        }
    }

    ScopedScrubbedEnvironment( const ScopedScrubbedEnvironment& ) = delete;
    ScopedScrubbedEnvironment& operator=( const ScopedScrubbedEnvironment& ) = delete;

private:
    struct Saved {
        const char* name{};
        bool existed{};
        QByteArray value;
    };

    std::vector<Saved> saved_;
};

} // namespace

TEST_CASE( "Fresh Android session spec serializes typed fields without raw command data",
           "[livelog-session-spec]" )
{
    const auto spec = makeAndroidSpec();
    const auto json = serialized( spec );
    const auto object = objectFrom( json );

    // Versioned: the current schema version is stamped explicitly.
    REQUIRE( object.value( QStringLiteral( "schemaVersion" ) ).toInt()
             == klogg::livelog::kCurrentSpecVersion );

    // Typed discriminators replace the boolean/string soup.
    REQUIRE( object.value( QStringLiteral( "sourceKind" ) ).toString()
             == QStringLiteral( "android_logcat" ) );
    REQUIRE( object.value( QStringLiteral( "androidBackend" ) ).toString()
             == QStringLiteral( "smart_socket" ) );

    // No executable path and no free-form argument blob may be persisted ever
    // again — the whole point of the spec is to retire raw command lines.
    REQUIRE_FALSE( object.contains( QStringLiteral( "adbExecutable" ) ) );
    REQUIRE_FALSE( object.contains( QStringLiteral( "extraArgs" ) ) );
    REQUIRE_FALSE( object.contains( QStringLiteral( "executable" ) ) );

    const auto parsed = parseSpec( json );
    REQUIRE( parsed.ok() );
    REQUIRE( parsed.spec.has_value() );
    const auto restored = *parsed.spec;

    REQUIRE( restored.schemaVersion == klogg::livelog::kCurrentSpecVersion );
    REQUIRE( restored.sourceKind == SourceKind::AndroidLogcat );
    REQUIRE( restored.androidBackend == AndroidBackend::SmartSocket );
    REQUIRE( restored.captureId == androidCaptureId() );
    REQUIRE( restored.device.deviceId == QStringLiteral( "R58NC123ABC" ) );
    REQUIRE( restored.device.displayName == QStringLiteral( "Pixel 8 Pro" ) );
    REQUIRE( restored.device.connection == DeviceIdentity::Connection::Usb );
    REQUIRE( restored.runIntent == live::RunIntent::Running );
    REQUIRE( restored.android.buffers
             == QStringList{ QStringLiteral( "main" ), QStringLiteral( "system" ),
                             QStringLiteral( "crash" ) } );
    REQUIRE( restored.android.filterSpec == QStringLiteral( "TagA:D TagB:S" ) );
    REQUIRE( restored.android.priority == QStringLiteral( "INFO" ) );
    REQUIRE( restored.android.pid == std::optional<int>{ 12345 } );
    REQUIRE( restored.capture.ansiOutputEnabled );
    REQUIRE( restored.capture.preserveAnsiOnSave );
    REQUIRE( restored.capture.autoReconnectEnabled );
    REQUIRE( restored.capture.maxReconnectAttempts == 7 );
    REQUIRE( restored.capture.captureMaxFileSize == static_cast<qint64>( 32 ) * 1024 * 1024 );
    REQUIRE( restored.capture.captureBackupCount == 5 );
    REQUIRE( restored.boundOutputFile == QStringLiteral( "/tmp/klogg-capture.log" ) );

    // A fresh spec is not a migration and carries no diagnostics.
    REQUIRE_FALSE( restored.migratedFromLegacySession );
    REQUIRE( parsed.diagnostics.empty() );
}

TEST_CASE( "Fresh iOS session spec serializes typed fields without raw command data",
           "[livelog-session-spec]" )
{
    const auto spec = makeIosSpec();
    const auto json = serialized( spec );
    const auto object = objectFrom( json );

    REQUIRE( object.value( QStringLiteral( "schemaVersion" ) ).toInt()
             == klogg::livelog::kCurrentSpecVersion );
    REQUIRE( object.value( QStringLiteral( "sourceKind" ) ).toString()
             == QStringLiteral( "ios_syslog" ) );
    REQUIRE( object.value( QStringLiteral( "iosBackend" ) ).toString()
             == QStringLiteral( "native" ) );
    REQUIRE_FALSE( object.contains( QStringLiteral( "adbExecutable" ) ) );
    REQUIRE_FALSE( object.contains( QStringLiteral( "extraArgs" ) ) );

    const auto parsed = parseSpec( json );
    REQUIRE( parsed.ok() );
    const auto restored = *parsed.spec;

    REQUIRE( restored.sourceKind == SourceKind::IosSyslog );
    REQUIRE( restored.iosBackend == IosBackend::Native );
    REQUIRE( restored.captureId == iosCaptureId() );
    REQUIRE( restored.device.deviceId == QStringLiteral( "00008101-001A2B3C4D5E" ) );
    REQUIRE( restored.device.connection == DeviceIdentity::Connection::Network );
    REQUIRE( restored.runIntent == live::RunIntent::Stopped );
    REQUIRE( restored.ios.level == QStringLiteral( "debug" ) );
    REQUIRE( restored.ios.categories
             == QStringList{ QStringLiteral( "network" ), QStringLiteral( "sqlite" ) } );
    REQUIRE( restored.ios.subsystem == QStringLiteral( "com.apple.network" ) );
    REQUIRE( restored.ios.outputFormat == IosOptions::OutputFormat::Json );
    REQUIRE( restored.capture.captureMaxFileSize == static_cast<qint64>( 16 ) * 1024 * 1024 );
    REQUIRE( restored.capture.captureBackupCount == 3 );
    REQUIRE( restored.boundOutputFile == QStringLiteral( "/tmp/ios-capture.log" ) );
    REQUIRE( parsed.diagnostics.empty() );
}

TEST_CASE( "Legacy and current running intent parse compatibly for restore normalization",
           "[livelog-session-spec][inert-restore][compatibility]" )
{
    auto currentAndroid = makeAndroidSpec();
    currentAndroid.runIntent = live::RunIntent::Running;
    const auto currentAndroidParsed = parseSpec( serialized( currentAndroid ) );
    REQUIRE( currentAndroidParsed.ok() );
    CHECK( currentAndroidParsed.spec->runIntent == live::RunIntent::Running );

    auto currentIos = makeSupportedIosSpec();
    currentIos.runIntent = live::RunIntent::Running;
    const auto currentIosParsed = parseSpec( serialized( currentIos ) );
    REQUIRE( currentIosParsed.ok() );
    CHECK( currentIosParsed.spec->runIntent == live::RunIntent::Running );

    const auto legacyAndroid = parseSpec( QStringLiteral(
        R"json({"schemaVersion":0,"sourceType":"adb_logcat","adbBackend":"smart_socket","captureId":"3f6b1d5e-9c24-4a17-8f2e-6d1a2b3c4d50","deviceSerial":"R58NC123ABC","runIntent":"running"})json" ) );
    REQUIRE( legacyAndroid.ok() );
    CHECK( legacyAndroid.spec->schemaVersion == klogg::livelog::kCurrentSpecVersion );
    CHECK( legacyAndroid.spec->runIntent == live::RunIntent::Running );

    const auto legacyIos = parseSpec( QStringLiteral(
        R"json({"schemaVersion":0,"sourceType":"ios_log_stream","iosBackend":"native","captureId":"7a2c9e81-4b3d-4f60-9a15-2c7d8e9f0a12","deviceSerial":"00008101-001A2B3C4D5E","runIntent":"running"})json" ) );
    REQUIRE( legacyIos.ok() );
    CHECK( legacyIos.spec->schemaVersion == klogg::livelog::kCurrentSpecVersion );
    CHECK( legacyIos.spec->runIntent == live::RunIntent::Running );
}

TEST_CASE( "Stopped session copies preserve every non-runtime field",
           "[livelog-session-spec][inert-restore][policy]" )
{
    const auto assertStoppedCopy = []( const LiveLogSessionSpec& source ) {
        auto expected = source;
        expected.runIntent = live::RunIntent::Stopped;
        const auto stopped = klogg::livelog::withStoppedRunIntent( source );
        CHECK( source.runIntent == live::RunIntent::Running );
        CHECK( stopped.runIntent == live::RunIntent::Stopped );
        CHECK( serialized( stopped ) == serialized( expected ) );
    };

    assertStoppedCopy( makeAndroidSpec() );

    auto parsedRestoreSpec = makeSupportedIosSpec();
    parsedRestoreSpec.runIntent = live::RunIntent::Running;
    parsedRestoreSpec.legacyMigrationMarker = true;
    assertStoppedCopy( parsedRestoreSpec );
}

TEST_CASE( "Session spec keeps source-neutral identity helpers", "[livelog-session-spec]" )
{
    // One type, both sources: the per-source URI schemes match the historical
    // document ids so restore keeps dedup/tab identity stable.
    REQUIRE( makeAndroidSpec().documentId()
             == QStringLiteral( "adb://%1" ).arg( androidCaptureId() ) );
    REQUIRE( makeIosSpec().documentId() == QStringLiteral( "ios-log://%1" ).arg( iosCaptureId() ) );

    REQUIRE( makeAndroidSpec().displayName() == QStringLiteral( "Pixel 8 Pro" ) );
    REQUIRE( makeIosSpec().displayName() == QStringLiteral( "iPhone 15 Pro" ) );
}

TEST_CASE( "Transitional legacy_process discriminators persist and restore unchanged",
           "[livelog-session-spec]" )
{
    // LegacyProcess remains legal in the persisted schema ONLY as a
    // transitional marker for sessions created while the compatibility
    // transports still exist. Parsing must preserve it exactly — neither
    // upgrading nor downgrading behind the user's back.
    auto androidSpec = makeAndroidSpec();
    androidSpec.androidBackend = AndroidBackend::LegacyProcess;
    const auto androidParsed = parseSpec( serialized( androidSpec ) );
    REQUIRE( androidParsed.ok() );
    REQUIRE( androidParsed.spec->androidBackend == AndroidBackend::LegacyProcess );
    REQUIRE_FALSE( androidParsed.hasFatalDiagnostic() );

    auto iosSpec = makeIosSpec();
    iosSpec.iosBackend = IosBackend::LegacyProcess;
    const auto iosParsed = parseSpec( serialized( iosSpec ) );
    REQUIRE( iosParsed.ok() );
    REQUIRE( iosParsed.spec->iosBackend == IosBackend::LegacyProcess );
    REQUIRE_FALSE( iosParsed.hasFatalDiagnostic() );
}

TEST_CASE( "Compatibility status depends only on the selected source backend",
           "[livelog-session-spec][source-neutral]" )
{
    auto android = makeAndroidSpec();
    android.iosBackend = IosBackend::LegacyProcess;
    CHECK_FALSE( klogg::livelog::usesCompatibilityTransport( android ) );
    CHECK( klogg::livelog::validateForAccept( android ).empty() );
    CHECK_FALSE( klogg::livelog::sessionDataFromSpec( android ).readOnlyCompatibility );

    auto ios = makeSupportedIosSpec();
    ios.androidBackend = AndroidBackend::LegacyProcess;
    CHECK_FALSE( klogg::livelog::usesCompatibilityTransport( ios ) );
    CHECK( klogg::livelog::validateForAccept( ios ).empty() );
    CHECK_FALSE( klogg::livelog::sessionDataFromSpec( ios ).readOnlyCompatibility );

    android.androidBackend = AndroidBackend::LegacyProcess;
    ios.iosBackend = IosBackend::LegacyProcess;
    CHECK( klogg::livelog::usesCompatibilityTransport( android ) );
    CHECK( klogg::livelog::usesCompatibilityTransport( ios ) );
    CHECK( klogg::livelog::validateForRestore( android ).empty() );
    CHECK( klogg::livelog::validateForRestore( ios ).empty() );
}

TEST_CASE( "Tampered Android discriminator fails closed to smart_socket, never legacy_process",
           "[livelog-session-spec]" )
{
    auto object = objectFrom( serialized( makeAndroidSpec() ) );
    object.insert( QStringLiteral( "androidBackend" ),
                   QJsonValue{ QStringLiteral( "root_shell_popen" ) } );

    const auto parsed = parseSpec(
        QString::fromUtf8( QJsonDocument( object ).toJson( QJsonDocument::Compact ) ) );

    // Structured, non-fatal diagnostic plus a defined modern default.
    REQUIRE( parsed.ok() );
    REQUIRE( parsed.spec.has_value() );
    REQUIRE( parsed.spec->androidBackend == AndroidBackend::SmartSocket );
    REQUIRE( hasInfoDiagnostic( parsed, "unknown-android-backend" ) );
}

TEST_CASE( "Tampered iOS discriminator fails closed to native, never legacy_process",
           "[livelog-session-spec]" )
{
    auto object = objectFrom( serialized( makeIosSpec() ) );
    object.insert( QStringLiteral( "iosBackend" ),
                   QJsonValue{ QStringLiteral( "python_sidecar_v9" ) } );

    const auto parsed = parseSpec(
        QString::fromUtf8( QJsonDocument( object ).toJson( QJsonDocument::Compact ) ) );

    REQUIRE( parsed.ok() );
    REQUIRE( parsed.spec.has_value() );
    REQUIRE( parsed.spec->iosBackend == IosBackend::Native );
    REQUIRE( hasInfoDiagnostic( parsed, "unknown-ios-backend" ) );
}

TEST_CASE( "Unknown source kind fails closed instead of guessing", "[livelog-session-spec]" )
{
    auto object = objectFrom( serialized( makeAndroidSpec() ) );
    object.insert( QStringLiteral( "sourceKind" ),
                   QJsonValue{ QStringLiteral( "windows_eventlog" ) } );

    const auto parsed = parseSpec(
        QString::fromUtf8( QJsonDocument( object ).toJson( QJsonDocument::Compact ) ) );

    // Without a known source kind the typed option blocks cannot be trusted,
    // so restore refuses rather than mislabeling the session.
    REQUIRE_FALSE( parsed.ok() );
    REQUIRE_FALSE( parsed.spec.has_value() );
    REQUIRE( hasFatalDiagnostic( parsed, "unknown-source-kind" ) );
}

TEST_CASE( "Pre-discriminator sessions migrate once to the native backend and stamp the marker "
           "on save",
           "[livelog-session-spec]" )
{
    // Payloads from before any backend discriminator existed carry neither
    // androidBackend nor iosBackend. They must migrate exactly once, to the
    // modern application-owned backends, with the migration recorded so the
    // next save stamps it into the persisted payload (a retrieve-only stamp
    // would erase fresh-install explicit choices on every reload).
    const auto preDiscriminatorIos = QStringLiteral(
        R"json({
            "sourceType": "ios_log_stream",
            "deviceSerial": "00008101-001A2B3C4D5E",
            "deviceDescription": "iPhone 15 Pro",
            "captureId": "7a2c9e81-4b3d-4f60-9a15-2c7d8e9f0a12"
        })json" );

    const auto iosParsed = parseSpec( preDiscriminatorIos );
    REQUIRE( iosParsed.ok() );
    REQUIRE( iosParsed.spec.has_value() );
    REQUIRE( iosParsed.spec->sourceKind == SourceKind::IosSyslog );
    // Fails closed to NATIVE — never to the compatibility process backend.
    REQUIRE( iosParsed.spec->iosBackend == IosBackend::Native );
    REQUIRE( iosParsed.spec->migratedFromLegacySession );
    REQUIRE( hasInfoDiagnostic( iosParsed, "migrated-pre-discriminator-session" ) );

    const auto iosSaved = objectFrom( serialized( *iosParsed.spec ) );
    REQUIRE( iosSaved.value( QStringLiteral( "schemaVersion" ) ).toInt()
             == klogg::livelog::kCurrentSpecVersion );
    REQUIRE( iosSaved.value( QStringLiteral( "migratedFromLegacySession" ) ).toBool() );

    // Reparsing the saved payload must NOT migrate a second time...
    const auto iosReparsed = parseSpec( serialized( *iosParsed.spec ) );
    REQUIRE( iosReparsed.ok() );
    REQUIRE( iosReparsed.spec->iosBackend == IosBackend::Native );
    REQUIRE_FALSE( iosReparsed.spec->migratedFromLegacySession );

    // ...while repeated saves of the migrated spec keep the marker stamped.
    const auto iosResaved = objectFrom( serialized( *iosReparsed.spec ) );
    REQUIRE( iosResaved.value( QStringLiteral( "migratedFromLegacySession" ) ).toBool() );

    const auto preDiscriminatorAndroid = QStringLiteral(
        R"json({
            "sourceType": "adb_logcat",
            "deviceSerial": "R58NC123ABC",
            "deviceDescription": "Pixel 8 Pro",
            "captureId": "3f6b1d5e-9c24-4a17-8f2e-6d1a2b3c4d50"
        })json" );

    const auto androidParsed = parseSpec( preDiscriminatorAndroid );
    REQUIRE( androidParsed.ok() );
    REQUIRE( androidParsed.spec.has_value() );
    REQUIRE( androidParsed.spec->androidBackend == AndroidBackend::SmartSocket );
    REQUIRE( androidParsed.spec->migratedFromLegacySession );
    REQUIRE( hasInfoDiagnostic( androidParsed, "migrated-pre-discriminator-session" ) );
    REQUIRE( objectFrom( serialized( *androidParsed.spec ) )
                 .value( QStringLiteral( "migratedFromLegacySession" ) )
                 .toBool() );
}

TEST_CASE( "Persisted raw command fields are rejected with an actionable error at restore",
           "[livelog-session-spec]" )
{
    // Executable paths and free-form argument blobs are dead persistence
    // formats. Restoring them must fail loudly with guidance — never fall
    // back silently to defaults, never honor the recorded binary, and never
    // re-resolve anything through PATH or the SDK.
    const QStringList legacyPayloads{
        // Current-era Android payload still carrying executable + args.
        QStringLiteral(
            R"json({
                "schemaVersion": 0,
                "sourceType": "adb_logcat",
                "adbBackend": "process",
                "adbExecutable": "/opt/android-sdk/platform-tools/adb",
                "extraArgs": "-v threadtime",
                "deviceSerial": "R58NC123ABC",
                "captureId": "3f6b1d5e-9c24-4a17-8f2e-6d1a2b3c4d50"
            })json" ),
        // Current-era iOS legacy-process payload.
        QStringLiteral(
            R"json({
                "schemaVersion": 0,
                "sourceType": "ios_log_stream",
                "iosBackend": "legacy_process",
                "adbExecutable": "/usr/local/bin/pymobiledevice3",
                "extraArgs": "syslog live --debug",
                "deviceSerial": "00008101-001A2B3C4D5E",
                "captureId": "7a2c9e81-4b3d-4f60-9a15-2c7d8e9f0a12"
            })json" ),
        // Even the bare free-form argument blob alone is poison, at any
        // schema version.
        QStringLiteral(
            R"json({
                "schemaVersion": %1,
                "sourceKind": "android_logcat",
                "androidBackend": "smart_socket",
                "extraArgs": "--whatever",
                "captureId": "3f6b1d5e-9c24-4a17-8f2e-6d1a2b3c4d50"
            })json" )
            .arg( klogg::livelog::kCurrentSpecVersion )
    };

    for ( const auto& payload : legacyPayloads ) {
        INFO( "payload: " << payload.toStdString() );
        const auto parsed = parseSpec( payload );
        REQUIRE_FALSE( parsed.ok() );
        REQUIRE_FALSE( parsed.spec.has_value() );
        REQUIRE( hasFatalDiagnostic( parsed, "legacy-raw-cli-options-unsupported" ) );

        // Actionable: the diagnostic names a concrete recovery action.
        bool actionableMessage = false;
        for ( const auto& diagnostic : parsed.diagnostics ) {
            if ( diagnostic.severity == Diagnostic::Severity::Fatal
                 && diagnostic.message.contains( QStringLiteral( "reopen" ),
                                                 Qt::CaseInsensitive ) ) {
                actionableMessage = true;
            }
        }
        REQUIRE( actionableMessage );
    }
}

TEST_CASE( "Accept gate rejects transitional backends and undeviced running intents",
           "[livelog-session-spec]" )
{
    // Freshly composed sessions may never select the transitional backends:
    // they exist solely so previously-saved sessions survive the migration.
    auto freshAndroidLegacy = makeAndroidSpec();
    freshAndroidLegacy.androidBackend = AndroidBackend::LegacyProcess;
    auto freshLegacyDiagnostics = klogg::livelog::validateForAccept( freshAndroidLegacy );
    REQUIRE_FALSE( freshLegacyDiagnostics.empty() );
    REQUIRE( std::any_of(
        freshLegacyDiagnostics.cbegin(), freshLegacyDiagnostics.cend(),
        []( const Diagnostic& diagnostic ) {
            return diagnostic.severity == Diagnostic::Severity::Fatal
                   && diagnostic.code == QLatin1String( "transitional-backend-not-creatable" );
        } ) );

    auto freshIosLegacy = makeIosSpec();
    freshIosLegacy.iosBackend = IosBackend::LegacyProcess;
    freshLegacyDiagnostics = klogg::livelog::validateForAccept( freshIosLegacy );
    REQUIRE( std::any_of(
        freshLegacyDiagnostics.cbegin(), freshLegacyDiagnostics.cend(),
        []( const Diagnostic& diagnostic ) {
            return diagnostic.severity == Diagnostic::Severity::Fatal
                   && diagnostic.code == QLatin1String( "transitional-backend-not-creatable" );
        } ) );

    // Auto-starting on load requires knowing which device to start on.
    auto runningWithoutDevice = makeIosSpec();
    runningWithoutDevice.device.deviceId.clear();
    const auto undevicedDiagnostics = klogg::livelog::validateForAccept( runningWithoutDevice );
    REQUIRE( std::any_of( undevicedDiagnostics.cbegin(), undevicedDiagnostics.cend(),
                          []( const Diagnostic& diagnostic ) {
                              return diagnostic.severity == Diagnostic::Severity::Fatal
                                     && diagnostic.code
                                            == QLatin1String( "running-intent-requires-device" );
                          } ) );

    // Well-formed fresh specs pass the accept gate cleanly.
    REQUIRE( klogg::livelog::validateForAccept( makeAndroidSpec() ).empty() );
    REQUIRE( klogg::livelog::validateForAccept( makeSupportedIosSpec() ).empty() );

    auto negativePid = makeAndroidSpec();
    negativePid.android.pid = -1;
    REQUIRE( hasFatalDiagnostic(
        ParseResult{ negativePid, klogg::livelog::validateForAccept( negativePid ) },
        "invalid-android-pid" ) );

    auto invalidBuffer = makeAndroidSpec();
    invalidBuffer.android.buffers = QStringList{ QStringLiteral( "kernel" ) };
    REQUIRE( hasFatalDiagnostic(
        ParseResult{ invalidBuffer, klogg::livelog::validateForAccept( invalidBuffer ) },
        "invalid-android-log-options" ) );

    const auto unsupportedIos = klogg::livelog::validateForAccept( makeIosSpec() );
    REQUIRE( std::any_of( unsupportedIos.cbegin(), unsupportedIos.cend(),
                          []( const Diagnostic& diagnostic ) {
                              return diagnostic.severity == Diagnostic::Severity::Fatal
                                     && diagnostic.code
                                            == QLatin1String( "unsupported-ios-log-options" );
                          } ) );
}

TEST_CASE( "Explicit running intent drives the live state reducer StartRequested event",
           "[livelog-session-spec][runtime-start]" )
{
    const auto runtimeStartEvents = []( const LiveLogSessionSpec& spec ) {
        return klogg::livelog::initialLiveStateEvents( spec, live::Timestamp{ 1500 } );
    };

    // Running intent maps onto the Task 2 reducer's StartRequested input.
    const auto runningEvents = runtimeStartEvents( makeAndroidSpec() );
    REQUIRE_FALSE( runningEvents.empty() );
    for ( const auto& event : runningEvents ) {
        REQUIRE( std::holds_alternative<live::StartRequested>( event ) );
    }

    auto snapshot = live::initialLiveState();
    const live::LiveStateConfig config{ 5u, std::chrono::seconds{ 10 } };
    for ( const auto& event : runningEvents ) {
        snapshot = live::reduce( snapshot, event, config ).snapshot;
    }
    REQUIRE( snapshot.runIntent == live::RunIntent::Running );

    // Stopped tabs restore inert: no synthetic start is injected.
    auto stoppedSpec = makeAndroidSpec();
    stoppedSpec.runIntent = live::RunIntent::Stopped;
    REQUIRE( runtimeStartEvents( stoppedSpec ).empty() );

    // Source-neutral: iOS sessions drive the same reducer entry point.
    auto runningIos = makeSupportedIosSpec();
    runningIos.runIntent = live::RunIntent::Running;
    const auto iosEvents = runtimeStartEvents( runningIos );
    REQUIRE_FALSE( iosEvents.empty() );
    for ( const auto& event : iosEvents ) {
        REQUIRE( std::holds_alternative<live::StartRequested>( event ) );
    }
}

TEST_CASE( "New-session paths ignore PATH, SDK, and Python environment", "[livelog-session-spec]" )
{
    // Building, saving, restoring, validating, and arming a live session must
    // not depend on any external toolchain being resolvable. With PATH and
    // every SDK/Python variable scrubbed, all new-session paths still behave
    // identically.
    const ScopedScrubbedEnvironment scrub{
        "PATH",       "ANDROID_HOME", "ANDROID_SDK_ROOT", "ANDROID_SDK_ROOT_WINDOWS",
        "PYTHONHOME", "PYTHONPATH",
    };

    const auto androidJson = serialized( makeAndroidSpec() );
    const auto iosJson = serialized( makeSupportedIosSpec() );

    const auto androidParsed = parseSpec( androidJson );
    REQUIRE( androidParsed.ok() );
    REQUIRE( androidParsed.spec->androidBackend == AndroidBackend::SmartSocket );
    REQUIRE( androidParsed.spec->device.deviceId == QStringLiteral( "R58NC123ABC" ) );

    const auto iosParsed = parseSpec( iosJson );
    REQUIRE( iosParsed.ok() );
    REQUIRE( iosParsed.spec->iosBackend == IosBackend::Native );
    REQUIRE( iosParsed.spec->device.deviceId == QStringLiteral( "00008101-001A2B3C4D5E" ) );

    REQUIRE( klogg::livelog::validateForAccept( *androidParsed.spec ).empty() );
    REQUIRE( klogg::livelog::validateForAccept( *iosParsed.spec ).empty() );

    auto runningIos = *iosParsed.spec;
    runningIos.runIntent = live::RunIntent::Running;
    REQUIRE_FALSE(
        klogg::livelog::initialLiveStateEvents( runningIos, live::Timestamp{ 42 } ).empty() );
}

TEST_CASE( "Schema versions outside the supported range are rejected without guessing",
           "[livelog-session-spec]" )
{
    // Explicit forward migration: versioned payloads older than the current
    // schema migrate deterministically; payloads from a NEWER schema must be
    // refused rather than interpreted with wrong-field guesses.
    auto futureObject = objectFrom( serialized( makeAndroidSpec() ) );
    futureObject.insert( QStringLiteral( "schemaVersion" ),
                         QJsonValue{ klogg::livelog::kCurrentSpecVersion + 1 } );
    const auto futureParsed = parseSpec(
        QString::fromUtf8( QJsonDocument( futureObject ).toJson( QJsonDocument::Compact ) ) );
    REQUIRE_FALSE( futureParsed.ok() );
    REQUIRE_FALSE( futureParsed.spec.has_value() );
    REQUIRE( hasFatalDiagnostic( futureParsed, "unsupported-schema-version" ) );

    auto farFutureObject = futureObject;
    farFutureObject.insert( QStringLiteral( "schemaVersion" ), QJsonValue{ 999 } );
    const auto farFutureParsed = parseSpec(
        QString::fromUtf8( QJsonDocument( farFutureObject ).toJson( QJsonDocument::Compact ) ) );
    REQUIRE_FALSE( farFutureParsed.ok() );
    REQUIRE( hasFatalDiagnostic( farFutureParsed, "unsupported-schema-version" ) );

    // Tampered, non-numeric, or nonsense versions also fail closed.
    const QStringList malformedVersions{ QStringLiteral( "version one" ), QStringLiteral( "-1" ),
                                         QStringLiteral( "0" ) };
    for ( const auto& broken : malformedVersions ) {
        auto tampered = futureObject;
        tampered.insert( QStringLiteral( "schemaVersion" ), QJsonValue{ broken } );
        INFO( "schemaVersion: " << broken.toStdString() );
        const auto parsed = parseSpec(
            QString::fromUtf8( QJsonDocument( tampered ).toJson( QJsonDocument::Compact ) ) );
        REQUIRE_FALSE( parsed.ok() );
        REQUIRE_FALSE( parsed.spec.has_value() );
        REQUIRE( hasFatalDiagnostic( parsed, "malformed-schema-version" ) );
    }
}

TEST_CASE( "Schema versions beyond the int range are refused instead of wrapping into range",
           "[livelog-session-spec]" )
{
    // A hostile schemaVersion of 2^31 or beyond used to be narrowed with a
    // plain static_cast<int>, which is undefined for out-of-range doubles
    // (UBSan float-cast-overflow) and platform-dependently wraps to INT_MIN —
    // slipping past both the unsupported-version gate and the legacy-era
    // check so the payload restored as the CURRENT schema. The comparison
    // must happen in the double domain.
    const std::vector<double> hostileVersions{ 2147483648.0, 9007199254740992.0, 9.3e18 };
    for ( const auto version : hostileVersions ) {
        auto tampered = objectFrom( serialized( makeAndroidSpec() ) );
        tampered.insert( QStringLiteral( "schemaVersion" ), QJsonValue{ version } );
        INFO( "schemaVersion: " << version );
        const auto parsed = parseSpec(
            QString::fromUtf8( QJsonDocument( tampered ).toJson( QJsonDocument::Compact ) ) );
        REQUIRE_FALSE( parsed.ok() );
        REQUIRE_FALSE( parsed.spec.has_value() );
        REQUIRE( hasFatalDiagnostic( parsed, "unsupported-schema-version" ) );
    }

    // Anchor: an int-range version above the supported maximum is still
    // refused through the same gate.
    auto intRangeFuture = objectFrom( serialized( makeAndroidSpec() ) );
    intRangeFuture.insert( QStringLiteral( "schemaVersion" ), QJsonValue{ 2147483647.0 } );
    const auto intRangeParsed = parseSpec(
        QString::fromUtf8( QJsonDocument( intRangeFuture ).toJson( QJsonDocument::Compact ) ) );
    REQUIRE_FALSE( intRangeParsed.ok() );
    REQUIRE( hasFatalDiagnostic( intRangeParsed, "unsupported-schema-version" ) );
}

namespace {

// Builds a minimal valid current-era Android payload whose android.pid is the
// raw JSON text given (so tests can express values Qt's writer would never
// emit: out-of-int-range magnitudes and fractions).
QString pidPayload( const char* pidJsonText )
{
    return QStringLiteral(
               R"json({"schemaVersion":1,"sourceKind":"android_logcat","androidBackend":"smart_socket","captureId":"3f6b1d5e-9c24-4a17-8f2e-6d1a2b3c4d50","device":{"deviceId":"R58NC123ABC"},"android":{"pid":%1}})json" )
        .arg( QLatin1String( pidJsonText ) );
}

// Same idea for the capture output policy numbers.
QString captureNumbersPayload( const char* attempts, const char* maxFileSize,
                               const char* backupCount )
{
    return QStringLiteral(
               R"json({"schemaVersion":1,"sourceKind":"ios_syslog","iosBackend":"native","captureId":"7a2c9e81-4b3d-4f60-9a15-2c7d8e9f0a12","capture":{"maxReconnectAttempts":%1,"captureMaxFileSize":%2,"captureBackupCount":%3}})json" )
        .arg( QLatin1String( attempts ), QLatin1String( maxFileSize ),
              QLatin1String( backupCount ) );
}

} // namespace

TEST_CASE( "Out-of-range numeric option values convert deterministically instead of wrapping",
           "[livelog-session-spec]" )
{
    // JSON numbers arrive as doubles. Narrowing casts that ignore range are
    // undefined (UBSan float-cast-overflow) and platform-dependent otherwise;
    // QJsonValue::toInt()'s out-of-range fallback of 0 silently rewrites
    // capacity-like options into their "unlimited" sentinel and identity-like
    // options into a wrong value.

    // Identity-like: the Android pid filter must never be silently rewritten
    // to a different process. Values exactly representable as an int survive;
    // anything else drops the filter entirely.
    REQUIRE( parseSpec( pidPayload( "12345" ) ).spec->android.pid == std::optional<int>{ 12345 } );
    REQUIRE( parseSpec( pidPayload( "-5" ) ).spec->android.pid == std::optional<int>{ -5 } );
    REQUIRE( parseSpec( pidPayload( "2147483647" ) ).spec->android.pid
             == std::optional<int>{ 2147483647 } );
    REQUIRE_FALSE( parseSpec( pidPayload( "2147483648" ) ).spec->android.pid.has_value() );
    REQUIRE_FALSE( parseSpec( pidPayload( "-2147483649" ) ).spec->android.pid.has_value() );
    REQUIRE_FALSE( parseSpec( pidPayload( "1e20" ) ).spec->android.pid.has_value() );
    REQUIRE_FALSE( parseSpec( pidPayload( "2.5" ) ).spec->android.pid.has_value() );
    REQUIRE_FALSE( parseSpec( pidPayload( "null" ) ).spec->android.pid.has_value() );

    // Capacity-like: saturate into the destination range instead of wrapping
    // into garbage-negative sentinels; negative magnitudes have no meaning and
    // clamp to zero. (NaN/Infinity cannot occur: Qt's parser rejects those
    // literals for the whole document.)
    auto hugeAttempts = parseSpec( captureNumbersPayload( "1000000000000000000", "1048576", "5" ) );
    REQUIRE( hugeAttempts.ok() );
    REQUIRE( hugeAttempts.spec->capture.maxReconnectAttempts == std::numeric_limits<int>::max() );

    auto negativeAttempts = parseSpec( captureNumbersPayload( "-3", "1048576", "5" ) );
    REQUIRE( negativeAttempts.ok() );
    REQUIRE( negativeAttempts.spec->capture.maxReconnectAttempts == 0 );

    auto fractionalAttempts = parseSpec( captureNumbersPayload( "2.5", "1048576", "5" ) );
    REQUIRE( fractionalAttempts.ok() );
    REQUIRE( fractionalAttempts.spec->capture.maxReconnectAttempts == 2 ); // truncates

    auto hugeFileSize = parseSpec( captureNumbersPayload( "7", "1e300", "5" ) );
    REQUIRE( hugeFileSize.ok() );
    REQUIRE( hugeFileSize.spec->capture.captureMaxFileSize == std::numeric_limits<qint64>::max() );

    auto negativeFileSize = parseSpec( captureNumbersPayload( "7", "-1e300", "5" ) );
    REQUIRE( negativeFileSize.ok() );
    REQUIRE( negativeFileSize.spec->capture.captureMaxFileSize == 0 );

    auto hugeBackup = parseSpec( captureNumbersPayload( "7", "1048576", "1000000000000000" ) );
    REQUIRE( hugeBackup.ok() );
    REQUIRE( hugeBackup.spec->capture.captureBackupCount == std::numeric_limits<int>::max() );

    // Anchors: in-range canonical values round-trip unchanged.
    auto canonical = parseSpec( captureNumbersPayload( "7", "1048576", "5" ) );
    REQUIRE( canonical.ok() );
    REQUIRE( canonical.spec->capture.maxReconnectAttempts == 7 );
    REQUIRE( canonical.spec->capture.captureMaxFileSize == 1048576 );
    REQUIRE( canonical.spec->capture.captureBackupCount == 5 );
}

TEST_CASE( "initialLiveStateEvents refuses invalid specs and passes timestamps through verbatim",
           "[livelog-session-spec]" )
{
    // validateForAccept refuses to accept undeviced or unidentifiable running
    // sessions; arming restore events directly must be equally fail-closed so
    // a future wiring path cannot bypass the gate by forgetting it.
    auto undevicedRunning = makeAndroidSpec();
    undevicedRunning.device.deviceId.clear();
    REQUIRE( klogg::livelog::initialLiveStateEvents( undevicedRunning, live::Timestamp{ 1500 } )
                 .empty() );

    auto blankDeviceRunning = makeAndroidSpec();
    blankDeviceRunning.device.deviceId = QStringLiteral( "   " );
    REQUIRE( klogg::livelog::initialLiveStateEvents( blankDeviceRunning, live::Timestamp{ 1500 } )
                 .empty() );

    auto escapedCapture = makeAndroidSpec();
    escapedCapture.captureId = QStringLiteral( "../escape" );
    REQUIRE(
        klogg::livelog::initialLiveStateEvents( escapedCapture, live::Timestamp{ 1500 } ).empty() );

    auto emptyCapture = makeAndroidSpec();
    emptyCapture.captureId.clear();
    REQUIRE(
        klogg::livelog::initialLiveStateEvents( emptyCapture, live::Timestamp{ 1500 } ).empty() );

    // Timestamps flow into StartRequested verbatim — the reducer owns any
    // monotonic-now validation, this mapping stays a pure pass-through.
    const auto zeroEvents
        = klogg::livelog::initialLiveStateEvents( makeAndroidSpec(), live::Timestamp{ 0 } );
    REQUIRE( zeroEvents.size() == 1 );
    REQUIRE( std::get<live::StartRequested>( zeroEvents.front() ).at == live::Timestamp{ 0 } );

    auto runningIosNegative = makeSupportedIosSpec();
    runningIosNegative.runIntent = live::RunIntent::Running;
    const auto negativeEvents
        = klogg::livelog::initialLiveStateEvents( runningIosNegative, live::Timestamp{ -1000 } );
    REQUIRE( negativeEvents.size() == 1 );
    REQUIRE( std::get<live::StartRequested>( negativeEvents.front() ).at
             == live::Timestamp{ -1000 } );

    // A zero-epoch start still drives the Task 2 reducer into Running without
    // misbehaving.
    auto snapshot = live::initialLiveState();
    const live::LiveStateConfig config{ 5u, std::chrono::seconds{ 10 } };
    for ( const auto& event : zeroEvents ) {
        snapshot = live::reduce( snapshot, event, config ).snapshot;
    }
    REQUIRE( snapshot.runIntent == live::RunIntent::Running );
}

TEST_CASE( "Malformed and hostile documents fail closed without crashing",
           "[livelog-session-spec]" )
{
    // Document-level garbage: every one of these must yield the fatal
    // malformed-session-spec diagnostic with no spec — no crash, no hang.
    QString deeplyNested;
    deeplyNested.reserve( 10000 );
    for ( int i = 0; i < 5000; ++i ) {
        deeplyNested += QLatin1Char( '[' );
    }
    for ( int i = 0; i < 5000; ++i ) {
        deeplyNested += QLatin1Char( ']' );
    }

    const QStringList hostileDocuments{
        QString{},
        QStringLiteral( "   \n\t " ),
        QStringLiteral( "null" ),
        QStringLiteral( "\"just a string\"" ),
        QStringLiteral( "42" ),
        QStringLiteral( "[1,2,3]" ),
        QStringLiteral( "{" ),
        QStringLiteral( R"({"schemaVersion":1,)" ),
        QStringLiteral( R"({"captureId":"x"}})" ), // extra closing brace
        QStringLiteral( R"({"captureId":"x"} trailing garbage)" ),
        QStringLiteral( R"({"a":1}{"b":2})" ), // two concatenated documents
        deeplyNested,
    };
    for ( const auto& document : hostileDocuments ) {
        INFO( "document: " << document.left( 60 ).toStdString() );
        const auto parsed = parseSpec( document );
        REQUIRE_FALSE( parsed.ok() );
        REQUIRE_FALSE( parsed.spec.has_value() );
        REQUIRE( hasFatalDiagnostic( parsed, "malformed-session-spec" ) );
    }

    // A UTF-8 byte-order mark in front of an otherwise valid payload parses
    // identically (Qt's JSON reader skips it on every supported version).
    const auto bomPrefixed
        = parseSpec( QStringLiteral( "\uFEFF" ) + serialized( makeAndroidSpec() ) );
    REQUIRE( bomPrefixed.ok() );
    REQUIRE( bomPrefixed.spec->captureId == androidCaptureId() );
    REQUIRE( bomPrefixed.spec->androidBackend == AndroidBackend::SmartSocket );
    REQUIRE( bomPrefixed.diagnostics.empty() );

    // Duplicate keys: last one wins, deterministically.
    const auto duplicateKeys = parseSpec( QStringLiteral(
        R"json({"schemaVersion":1,"sourceKind":"android_logcat","androidBackend":"smart_socket","captureId":"11111111-1111-4111-8111-111111111111","captureId":"22222222-2222-4222-8222-222222222222"})json" ) );
    REQUIRE( duplicateKeys.ok() );
    REQUIRE( duplicateKeys.spec->captureId
             == QStringLiteral( "22222222-2222-4222-8222-222222222222" ) );

    // Wrong-typed scalar fields fail closed.
    const QStringList wrongTypedVersions{
        QStringLiteral( "true" ),
        QStringLiteral( "null" ),
        QStringLiteral( "\"1\"" ),
    };
    for ( const auto& version : wrongTypedVersions ) {
        auto tampered = objectFrom( serialized( makeAndroidSpec() ) );
        tampered.insert( QStringLiteral( "schemaVersion" ), QJsonValue{ version } );
        INFO( "schemaVersion type: " << version.toStdString() );
        const auto parsed = parseSpec(
            QString::fromUtf8( QJsonDocument( tampered ).toJson( QJsonDocument::Compact ) ) );
        REQUIRE_FALSE( parsed.ok() );
        REQUIRE( hasFatalDiagnostic( parsed, "malformed-schema-version" ) );
    }

    // Wrong-typed containers fall back to defined defaults instead of crashing.
    auto arrayDevice = objectFrom( serialized( makeAndroidSpec() ) );
    arrayDevice.insert( QStringLiteral( "device" ), QJsonValue{ QJsonArray{} } );
    const auto arrayDeviceParsed = parseSpec(
        QString::fromUtf8( QJsonDocument( arrayDevice ).toJson( QJsonDocument::Compact ) ) );
    REQUIRE( arrayDeviceParsed.ok() );
    REQUIRE( arrayDeviceParsed.spec->device.deviceId.isEmpty() );

    auto stringAndroidBlock = objectFrom( serialized( makeAndroidSpec() ) );
    stringAndroidBlock.insert( QStringLiteral( "android" ),
                               QJsonValue{ QStringLiteral( "nope" ) } );
    const auto stringBlockParsed = parseSpec(
        QString::fromUtf8( QJsonDocument( stringAndroidBlock ).toJson( QJsonDocument::Compact ) ) );
    REQUIRE( stringBlockParsed.ok() );
    REQUIRE( stringBlockParsed.spec->android.buffers.isEmpty() );
    REQUIRE_FALSE( stringBlockParsed.spec->android.pid.has_value() );

    auto objectBuffers = objectFrom( serialized( makeAndroidSpec() ) );
    auto androidBlock = objectFrom( serialized( makeAndroidSpec() ) )
                            .value( QStringLiteral( "android" ) )
                            .toObject();
    androidBlock.insert( QStringLiteral( "buffers" ),
                         QJsonValue{ QJsonObject{ { QStringLiteral( "main" ), 1 } } } );
    objectBuffers.insert( QStringLiteral( "android" ), androidBlock );
    const auto objectBuffersParsed = parseSpec(
        QString::fromUtf8( QJsonDocument( objectBuffers ).toJson( QJsonDocument::Compact ) ) );
    REQUIRE( objectBuffersParsed.ok() );
    REQUIRE( objectBuffersParsed.spec->android.buffers.isEmpty() );

    auto arrayPid = objectFrom( serialized( makeAndroidSpec() ) );
    auto pidBlock = arrayPid.value( QStringLiteral( "android" ) ).toObject();
    pidBlock.insert( QStringLiteral( "pid" ), QJsonValue{ QJsonArray{} } );
    arrayPid.insert( QStringLiteral( "android" ), pidBlock );
    const auto arrayPidParsed = parseSpec(
        QString::fromUtf8( QJsonDocument( arrayPid ).toJson( QJsonDocument::Compact ) ) );
    REQUIRE( arrayPidParsed.ok() );
    REQUIRE_FALSE( arrayPidParsed.spec->android.pid.has_value() );

    // Numeric source kind / backend scalars take the same paths as strings:
    // unknown kinds refuse, unknown backends fall back to modern defaults.
    auto numericSourceKind = objectFrom( serialized( makeAndroidSpec() ) );
    numericSourceKind.insert( QStringLiteral( "sourceKind" ), QJsonValue{ 42.0 } );
    const auto numericKindParsed = parseSpec(
        QString::fromUtf8( QJsonDocument( numericSourceKind ).toJson( QJsonDocument::Compact ) ) );
    REQUIRE_FALSE( numericKindParsed.ok() );
    REQUIRE( hasFatalDiagnostic( numericKindParsed, "unknown-source-kind" ) );

    auto numericBackend = objectFrom( serialized( makeAndroidSpec() ) );
    numericBackend.insert( QStringLiteral( "androidBackend" ), QJsonValue{ 7.0 } );
    const auto numericBackendParsed = parseSpec(
        QString::fromUtf8( QJsonDocument( numericBackend ).toJson( QJsonDocument::Compact ) ) );
    REQUIRE( numericBackendParsed.ok() );
    REQUIRE( numericBackendParsed.spec->androidBackend == AndroidBackend::SmartSocket );
    REQUIRE( hasInfoDiagnostic( numericBackendParsed, "unknown-android-backend" ) );
}

TEST_CASE( "Hostile capture identifiers are refused at parse and at the accept gate",
           "[livelog-session-spec]" )
{
    // The capture id names files under the capture root: traversal, absolute
    // paths, separators, control bytes (embedded NUL), reserved device names,
    // and Windows-hostile trailing characters must never reach the filesystem.
    const QStringList hostileCaptureIds{
        QString{},
        QStringLiteral( "." ),
        QStringLiteral( ".." ),
        QStringLiteral( "/etc/passwd" ),
        QStringLiteral( "C:\\temp\\capture" ),
        QStringLiteral( "a/b" ),
        QStringLiteral( "a\\b" ),
        QStringLiteral( "../escape" ),
        QStringLiteral( "con" ),
        QStringLiteral( "CON" ),
        QStringLiteral( "trailingdot." ),
        QStringLiteral( "trailingspace " ),
        QStringLiteral( "nul\0byte" ),
    };

    for ( const auto& captureId : hostileCaptureIds ) {
        auto tampered = objectFrom( serialized( makeAndroidSpec() ) );
        tampered.insert( QStringLiteral( "captureId" ), QJsonValue{ captureId } );
        INFO( "captureId: [" << captureId.toStdString() << "]" );
        const auto parsed = parseSpec(
            QString::fromUtf8( QJsonDocument( tampered ).toJson( QJsonDocument::Compact ) ) );
        REQUIRE_FALSE( parsed.ok() );
        REQUIRE_FALSE( parsed.spec.has_value() );
        REQUIRE( hasFatalDiagnostic( parsed, "malformed-capture-id" ) );

        // Composed specs carrying the same ids cannot pass the accept gate
        // either.
        auto composed = makeAndroidSpec();
        composed.captureId = captureId;
        const auto acceptDiagnostics = klogg::livelog::validateForAccept( composed );
        REQUIRE( std::any_of( acceptDiagnostics.cbegin(), acceptDiagnostics.cend(),
                              []( const Diagnostic& diagnostic ) {
                                  return diagnostic.severity == Diagnostic::Severity::Fatal
                                         && diagnostic.code
                                                == QLatin1String( "invalid-capture-id" );
                              } ) );
    }

    // A payload without any capture id is equally unusable.
    const auto missingId = parseSpec( QStringLiteral(
        R"json({"schemaVersion":1,"sourceKind":"android_logcat","androidBackend":"smart_socket","device":{"deviceId":"R58NC123ABC"}})json" ) );
    REQUIRE_FALSE( missingId.ok() );
    REQUIRE( hasFatalDiagnostic( missingId, "malformed-capture-id" ) );
}

TEST_CASE( "Forged migration markers cannot fake migrations or launder transitional backends",
           "[livelog-session-spec]" )
{
    // An externally edited marker=true on a payload that already carries
    // explicit discriminators must not report a migration event, must not
    // flip migratedFromLegacySession, and must leave the recorded backend
    // untouched.
    auto forgedObject = objectFrom( serialized( makeAndroidSpec() ) );
    forgedObject.insert( QStringLiteral( "migratedFromLegacySession" ), QJsonValue{ true } );
    const auto forged = parseSpec(
        QString::fromUtf8( QJsonDocument( forgedObject ).toJson( QJsonDocument::Compact ) ) );
    REQUIRE( forged.ok() );
    REQUIRE( forged.spec->androidBackend == AndroidBackend::SmartSocket );
    REQUIRE_FALSE( forged.spec->migratedFromLegacySession );
    REQUIRE( forged.spec->legacyMigrationMarker );
    REQUIRE_FALSE( hasInfoDiagnostic( forged, "migrated-pre-discriminator-session" ) );
    // The durable marker does not interfere with accepting a modern session.
    REQUIRE( klogg::livelog::validateForAccept( *forged.spec ).empty() );

    // The marker cannot launder a compatibility backend past the fresh-session
    // gate: LegacyProcess stays restorable from payloads but remains
    // un-creatable, marker or not.
    auto launderAttempt = objectFrom( serialized( makeAndroidSpec() ) );
    launderAttempt.insert( QStringLiteral( "migratedFromLegacySession" ), QJsonValue{ true } );
    launderAttempt.insert( QStringLiteral( "androidBackend" ),
                           QJsonValue{ QStringLiteral( "legacy_process" ) } );
    const auto laundered = parseSpec(
        QString::fromUtf8( QJsonDocument( launderAttempt ).toJson( QJsonDocument::Compact ) ) );
    REQUIRE( laundered.ok() );
    REQUIRE( laundered.spec->androidBackend == AndroidBackend::LegacyProcess );
    const auto launderDiagnostics = klogg::livelog::validateForAccept( *laundered.spec );
    REQUIRE( std::any_of(
        launderDiagnostics.cbegin(), launderDiagnostics.cend(), []( const Diagnostic& diagnostic ) {
            return diagnostic.severity == Diagnostic::Severity::Fatal
                   && diagnostic.code == QLatin1String( "transitional-backend-not-creatable" );
        } ) );

    // iOS mirror: marker + native backend survives untouched.
    auto forgedIos = objectFrom( serialized( makeIosSpec() ) );
    forgedIos.insert( QStringLiteral( "migratedFromLegacySession" ), QJsonValue{ true } );
    const auto iosParsed = parseSpec(
        QString::fromUtf8( QJsonDocument( forgedIos ).toJson( QJsonDocument::Compact ) ) );
    REQUIRE( iosParsed.ok() );
    REQUIRE( iosParsed.spec->iosBackend == IosBackend::Native );
    REQUIRE_FALSE( iosParsed.spec->migratedFromLegacySession );
    REQUIRE( iosParsed.spec->legacyMigrationMarker );
}

TEST_CASE( "Migration marker is byte-stable across repeated save/load cycles",
           "[livelog-session-spec]" )
{
    // save -> load -> save -> load -> save: once stamped, every subsequent
    // save keeps the marker and no cycle reports a new migration event.
    auto migrated = makeIosSpec();
    migrated.migratedFromLegacySession = true;

    const auto json1 = serialized( migrated );
    const auto parsed1 = parseSpec( json1 );
    REQUIRE( parsed1.ok() );
    REQUIRE( parsed1.spec->legacyMigrationMarker );
    REQUIRE_FALSE( parsed1.spec->migratedFromLegacySession );

    const auto json2 = serialized( *parsed1.spec );
    const auto parsed2 = parseSpec( json2 );
    REQUIRE( parsed2.ok() );
    REQUIRE_FALSE( parsed2.spec->migratedFromLegacySession );
    REQUIRE( parsed2.spec->legacyMigrationMarker );

    const auto json3 = serialized( *parsed2.spec );
    REQUIRE( json3 == json2 ); // byte-for-byte stable from the second save on
}

TEST_CASE( "Serialization roundtrip is byte-stable across the full enum matrix",
           "[livelog-session-spec]" )
{
    // serialize(parse(serialize(x))) == serialize(x), byte-for-byte, for every
    // enum combination, every marker state, and hostile-but-legal strings.
    const auto makeVariant = []( SourceKind kind, bool legacyBackend,
                                 DeviceIdentity::Connection connection, bool jsonOutputFormat,
                                 bool running, bool eventFlag, bool durableMarker ) {
        LiveLogSessionSpec spec
            = kind == SourceKind::AndroidLogcat ? makeAndroidSpec() : makeIosSpec();
        spec.sourceKind = kind;
        if ( kind == SourceKind::AndroidLogcat ) {
            spec.androidBackend
                = legacyBackend ? AndroidBackend::LegacyProcess : AndroidBackend::SmartSocket;
        }
        else {
            spec.iosBackend = legacyBackend ? IosBackend::LegacyProcess : IosBackend::Native;
        }
        spec.device.connection = connection;
        if ( jsonOutputFormat ) {
            spec.ios.outputFormat = IosOptions::OutputFormat::Json;
        }
        else {
            spec.ios.outputFormat = IosOptions::OutputFormat::Default;
        }
        spec.runIntent = running ? live::RunIntent::Running : live::RunIntent::Stopped;
        spec.migratedFromLegacySession = eventFlag;
        spec.legacyMigrationMarker = durableMarker;

        // Hostile-but-legal opaque strings: unicode, embedded NUL, quotes,
        // backslashes — everything a hand-edited session file could carry in
        // free-form label/path fields.
        spec.device.displayName = QString::fromUtf8( "\xc3\xbc"
                                                     "ber \xf0\x9f\x9a\x80 \"quote\" \\ slash" );
        spec.device.deviceId = QStringLiteral( "id\u0000with-nul" );
        spec.boundOutputFile = QStringLiteral( "/tmp/\u00f6utput \"q\" \\n.log" );
        return spec;
    };

    for ( const auto kind : { SourceKind::AndroidLogcat, SourceKind::IosSyslog } ) {
        for ( const auto legacyBackend : { false, true } ) {
            for ( const auto connection :
                  { DeviceIdentity::Connection::Usb, DeviceIdentity::Connection::Network } ) {
                for ( const auto jsonOutputFormat : { false, true } ) {
                    for ( const auto running : { false, true } ) {
                        for ( const auto eventFlag : { false, true } ) {
                            for ( const auto durableMarker : { false, true } ) {
                                const auto spec = makeVariant( kind, legacyBackend, connection,
                                                               jsonOutputFormat, running, eventFlag,
                                                               durableMarker );
                                INFO( "matrix cell kind="
                                      << static_cast<int>( kind ) << " legacy=" << legacyBackend
                                      << " conn=" << static_cast<int>( connection )
                                      << " jsonFmt=" << jsonOutputFormat << " running=" << running
                                      << " event=" << eventFlag << " marker=" << durableMarker );

                                const auto json1 = serialized( spec );
                                const auto parsed = parseSpec( json1 );
                                REQUIRE( parsed.ok() );
                                REQUIRE_FALSE( parsed.hasFatalDiagnostic() );
                                REQUIRE( parsed.diagnostics.empty() );

                                const auto json2 = serialized( *parsed.spec );
                                REQUIRE( json2 == json1 );

                                const auto reparsed = parseSpec( json2 );
                                REQUIRE( reparsed.ok() );
                                REQUIRE( serialized( *reparsed.spec ) == json2 );

                                // Roundtrip preserves every discriminating field.
                                REQUIRE( reparsed.spec->sourceKind == spec.sourceKind );
                                REQUIRE( reparsed.spec->androidBackend == spec.androidBackend );
                                REQUIRE( reparsed.spec->iosBackend == spec.iosBackend );
                                REQUIRE( reparsed.spec->device.connection
                                         == spec.device.connection );
                                REQUIRE( reparsed.spec->runIntent == spec.runIntent );
                                REQUIRE( reparsed.spec->captureId == spec.captureId );
                            }
                        }
                    }
                }
            }
        }
    }
}

TEST_CASE( "Runtime bridge preserves run intent and typed source options",
           "[livelog-session-spec][bridge]" )
{
    AdbLogcatSessionData android;
    android.captureId = QStringLiteral( "bridge-android" );
    android.deviceSerial = QStringLiteral( "SERIAL" );
    android.runIntent = live::RunIntent::Running;
    android.androidBuffers
        = QStringList{ QStringLiteral( "main" ), QStringLiteral( "system" ) };
    android.androidFilterSpec = QStringLiteral( "ActivityManager:I *:S" );
    android.androidPriority = QStringLiteral( "debug" );
    android.androidPid = 4242;

    const auto androidSpec = klogg::livelog::sessionSpecFromSessionData( android );
    CHECK( androidSpec.runIntent == live::RunIntent::Running );
    CHECK( androidSpec.android.buffers == android.androidBuffers );
    CHECK( androidSpec.android.filterSpec == android.androidFilterSpec );
    CHECK( androidSpec.android.priority == android.androidPriority );
    CHECK( androidSpec.android.pid == android.androidPid );

    AdbLogcatSessionData ios;
    ios.captureId = QStringLiteral( "bridge-ios" );
    ios.sourceType = LiveLogSourceType::IosLogStream;
    ios.deviceSerial = QStringLiteral( "UDID" );
    ios.runIntent = live::RunIntent::Running;
    ios.iosLevel = QStringLiteral( "debug" );
    ios.iosCategories
        = QStringList{ QStringLiteral( "network" ), QStringLiteral( "signpost" ) };
    ios.iosSubsystem = QStringLiteral( "com.example.app" );
    ios.iosJsonOutput = true;

    const auto iosSpec = klogg::livelog::sessionSpecFromSessionData( ios );
    CHECK( iosSpec.runIntent == live::RunIntent::Running );
    CHECK( iosSpec.ios.level == ios.iosLevel );
    CHECK( iosSpec.ios.categories == ios.iosCategories );
    CHECK( iosSpec.ios.subsystem == ios.iosSubsystem );
    CHECK( iosSpec.ios.outputFormat == IosOptions::OutputFormat::Json );

    const auto restoredAndroid = klogg::livelog::sessionDataFromSpec( androidSpec );
    CHECK( restoredAndroid.runIntent == android.runIntent );
    CHECK( restoredAndroid.androidBuffers == android.androidBuffers );
    CHECK( restoredAndroid.androidFilterSpec == android.androidFilterSpec );
    CHECK( restoredAndroid.androidPriority == android.androidPriority );
    CHECK( restoredAndroid.androidPid == android.androidPid );

    const auto restoredIos = klogg::livelog::sessionDataFromSpec( iosSpec );
    CHECK( restoredIos.runIntent == ios.runIntent );
    CHECK( restoredIos.iosLevel == ios.iosLevel );
    CHECK( restoredIos.iosCategories == ios.iosCategories );
    CHECK( restoredIos.iosSubsystem == ios.iosSubsystem );
    CHECK( restoredIos.iosJsonOutput );
}

TEST_CASE( "Document identity helpers keep schemes disjoint across sources",
           "[livelog-session-spec]" )
{
    const auto androidA = makeAndroidSpec();
    auto androidB = makeAndroidSpec();
    androidB.captureId = QStringLiteral( "11111111-2222-4333-8444-555555555555" );
    REQUIRE( androidA.documentId() != androidB.documentId() );

    // Same capture id under different source kinds yields different document
    // ids: adb://X and ios-log://X never collide.
    auto iosSameId = makeIosSpec();
    iosSameId.captureId = androidA.captureId;
    REQUIRE( iosSameId.documentId() != androidA.documentId() );

    // displayName falls back to deviceId, then to empty — never crashes.
    auto unlabeled = makeAndroidSpec();
    unlabeled.device.displayName.clear();
    REQUIRE( unlabeled.displayName() == unlabeled.device.deviceId );
    unlabeled.device.deviceId.clear();
    REQUIRE( unlabeled.displayName().isEmpty() );
}
