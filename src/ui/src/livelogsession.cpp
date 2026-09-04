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

#include "livelogsession.h"

#include "adblogcatsource.h"
#include "livelogcontroller.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "capturestore.h"
#include "log.h"

namespace klogg::livelog {

namespace {

// JSON numbers arrive as doubles. Narrowing them with plain casts is
// undefined behavior for values beyond the destination range (UBSan
// float-cast-overflow) and platform-dependent garbage otherwise, while
// QJsonValue::toInt()'s out-of-range fallback silently rewrites hostile
// magnitudes into a chosen default. Capacity-like numbers instead saturate
// into [lowestAllowed, Target max]; non-numbers keep the documented fallback.
// All bounds are compared in the double domain: integer limits up to 2^63 are
// either exactly representable as doubles or round UP to a power of two, so
// any surviving candidate always fits the destination type.
template <typename Target>
Target saturatedNumber( const QJsonValue& value, Target fallback, Target lowestAllowed )
{
    if ( !value.isDouble() ) {
        return fallback;
    }
    // Qt's JSON parser rejects NaN and infinity before deserialization reaches
    // this helper, so every accepted numeric value is finite.
    const auto raw = value.toDouble();
    if ( raw <= static_cast<double>( lowestAllowed ) ) {
        return lowestAllowed;
    }
    if ( raw >= static_cast<double>( std::numeric_limits<Target>::max() ) ) {
        return std::numeric_limits<Target>::max();
    }
    // In-range cast: defined truncation toward zero.
    return static_cast<Target>( raw );
}

// Identity-like integers must never be silently rewritten to a different
// value: the Android pid filter either survives exactly representable or is
// dropped entirely, never clamped onto an unrelated process id.
std::optional<int> exactIntOrNullopt( const QJsonValue& value )
{
    if ( !value.isDouble() ) {
        return std::nullopt;
    }
    const double raw = value.toDouble();
    if ( raw != std::floor( raw ) || raw < static_cast<double>( std::numeric_limits<int>::min() )
         || raw > static_cast<double>( std::numeric_limits<int>::max() ) ) {
        return std::nullopt;
    }
    return static_cast<int>( raw );
}

QString sourceKindKey( SourceKind kind )
{
    return kind == SourceKind::IosSyslog ? QStringLiteral( "ios_syslog" )
                                         : QStringLiteral( "android_logcat" );
}

QString androidBackendKey( AndroidBackend backend )
{
    return backend == AndroidBackend::SmartSocket ? QStringLiteral( "smart_socket" )
                                                  : QStringLiteral( "legacy_process" );
}

QString iosBackendKey( IosBackend backend )
{
    return backend == IosBackend::Native ? QStringLiteral( "native" )
                                         : QStringLiteral( "legacy_process" );
}

QStringList stringArrayValues( const QJsonArray& array )
{
    QStringList values;
    values.reserve( static_cast<int>( array.size() ) );
    for ( const auto& value : array ) {
        const auto text = value.toString();
        if ( !text.isEmpty() ) {
            values.append( text );
        }
    }
    return values;
}

QString liveLogMessage( const char* sourceText )
{
    return QCoreApplication::translate( "klogg::livelog::messages", sourceText );
}

Diagnostic fatalDiagnostic( QString code, QString message )
{
    return Diagnostic{ Diagnostic::Severity::Fatal, std::move( code ), std::move( message ) };
}

Diagnostic infoDiagnostic( QString code, QString message )
{
    return Diagnostic{ Diagnostic::Severity::Info, std::move( code ), std::move( message ) };
}

// The persisted raw command fields are dead formats: any non-empty payload
// carrying them is refused outright, at every schema version. Empty keys left
// behind by the old writer are harmless and simply dropped.
bool carriesRawCliOptions( const QJsonObject& object )
{
    const auto executable = object.value( QStringLiteral( "adbExecutable" ) ).toString().trimmed();
    if ( !executable.isEmpty() ) {
        return true;
    }

    const auto arguments = object.value( QStringLiteral( "extraArgs" ) ).toString().trimmed();
    return !arguments.isEmpty();
}

// Payload eras: the current typed schema (schemaVersion >= 1) and the flat
// AdbLogcatSessionData JSON written before it (no or zero schemaVersion).
enum class PayloadEra : std::uint8_t { Current, LegacyFlat };

} // namespace

QString LiveLogSessionSpec::documentId() const
{
    const auto scheme = sourceKind == SourceKind::IosSyslog ? QStringLiteral( "ios-log" )
                                                            : QStringLiteral( "adb" );
    return QStringLiteral( "%1://%2" ).arg( scheme, captureId );
}

QString LiveLogSessionSpec::displayName() const
{
    return device.displayName.isEmpty() ? device.deviceId : device.displayName;
}

bool ParseResult::hasFatalDiagnostic() const
{
    return std::any_of( diagnostics.cbegin(), diagnostics.cend(),
                        []( const Diagnostic& diagnostic ) {
                            return diagnostic.severity == Diagnostic::Severity::Fatal;
                        } );
}

namespace messages {

QString compatibilityTransportReadOnly()
{
    return QCoreApplication::translate(
        "klogg::livelog::messages",
        "This session uses a compatibility transport and opens read-only; reconnect it through "
        "the built-in services to stream again." );
}

QString captureIdentifierAlreadyInUse()
{
    return QCoreApplication::translate(
        "klogg::livelog::messages",
        "Another live session already uses this capture identifier; the saved session cannot be "
        "restored without overwriting capture storage." );
}

} // namespace messages

QString serializeSpec( const LiveLogSessionSpec& spec )
{
    QJsonObject object;
    object.insert( QStringLiteral( "schemaVersion" ), spec.schemaVersion );
    object.insert( QStringLiteral( "sourceKind" ), sourceKindKey( spec.sourceKind ) );

    // One discriminator per source kind; unknown values fail closed to these
    // modern defaults on restore, never to the compatibility backends.
    if ( spec.sourceKind == SourceKind::AndroidLogcat ) {
        object.insert( QStringLiteral( "androidBackend" ),
                       androidBackendKey( spec.androidBackend ) );
    }
    else {
        object.insert( QStringLiteral( "iosBackend" ), iosBackendKey( spec.iosBackend ) );
    }

    object.insert( QStringLiteral( "captureId" ), spec.captureId );
    object.insert( QStringLiteral( "runIntent" ), spec.runIntent == livecapture::RunIntent::Running
                                                      ? QStringLiteral( "running" )
                                                      : QStringLiteral( "stopped" ) );

    QJsonObject device;
    device.insert( QStringLiteral( "deviceId" ), spec.device.deviceId );
    device.insert( QStringLiteral( "displayName" ), spec.device.displayName );
    device.insert( QStringLiteral( "connection" ),
                   spec.device.connection == DeviceIdentity::Connection::Network
                       ? QStringLiteral( "network" )
                       : QStringLiteral( "usb" ) );
    object.insert( QStringLiteral( "device" ), device );

    if ( spec.sourceKind == SourceKind::AndroidLogcat ) {
        QJsonObject android;
        android.insert( QStringLiteral( "buffers" ),
                        QJsonArray::fromStringList( spec.android.buffers ) );
        android.insert( QStringLiteral( "filterSpec" ), spec.android.filterSpec );
        android.insert( QStringLiteral( "priority" ), spec.android.priority );
        if ( spec.android.pid.has_value() ) {
            android.insert( QStringLiteral( "pid" ), *spec.android.pid );
        }
        object.insert( QStringLiteral( "android" ), android );
    }
    else {
        QJsonObject ios;
        ios.insert( QStringLiteral( "level" ), spec.ios.level );
        ios.insert( QStringLiteral( "categories" ),
                    QJsonArray::fromStringList( spec.ios.categories ) );
        ios.insert( QStringLiteral( "subsystem" ), spec.ios.subsystem );
        ios.insert( QStringLiteral( "outputFormat" ),
                    spec.ios.outputFormat == IosOptions::OutputFormat::Json
                        ? QStringLiteral( "json" )
                        : QStringLiteral( "default" ) );
        object.insert( QStringLiteral( "ios" ), ios );
    }

    QJsonObject capture;
    capture.insert( QStringLiteral( "ansiOutputEnabled" ), spec.capture.ansiOutputEnabled );
    capture.insert( QStringLiteral( "preserveAnsiOnSave" ), spec.capture.preserveAnsiOnSave );
    capture.insert( QStringLiteral( "autoReconnectEnabled" ), spec.capture.autoReconnectEnabled );
    capture.insert( QStringLiteral( "maxReconnectAttempts" ), spec.capture.maxReconnectAttempts );
    capture.insert( QStringLiteral( "captureMaxFileSize" ), spec.capture.captureMaxFileSize );
    capture.insert( QStringLiteral( "captureBackupCount" ), spec.capture.captureBackupCount );
    object.insert( QStringLiteral( "capture" ), capture );

    object.insert( QStringLiteral( "boundOutputFile" ), spec.boundOutputFile );

    // Stamp on save: a marker applied on retrieve only would be erased by the
    // next save of a freshly-loaded spec and re-migrate over explicit choices.
    if ( spec.migratedFromLegacySession || spec.legacyMigrationMarker ) {
        object.insert( QStringLiteral( "migratedFromLegacySession" ), true );
    }

    return QString::fromUtf8( QJsonDocument( object ).toJson( QJsonDocument::Compact ) );
}

ParseResult parsePersistedSpec( const QString& json )
{
    ParseResult result;

    const auto document = QJsonDocument::fromJson( json.toUtf8() );
    if ( !document.isObject() ) {
        result.diagnostics.push_back(
            fatalDiagnostic( QStringLiteral( "malformed-session-spec" ),
                             liveLogMessage( "The saved live-log session payload is not a valid "
                                             "session object." ) ) );
        return result;
    }

    const auto object = document.object();

    // Raw CLI options are an absolute veto, checked before anything else can
    // be inferred from the payload — at any schema version.
    if ( carriesRawCliOptions( object ) ) {
        LOG_WARNING << "Refusing saved live session with raw command-line options";
        result.diagnostics.push_back( fatalDiagnostic(
            QStringLiteral( "legacy-raw-cli-options-unsupported" ),
            liveLogMessage( "This live log session was saved with a custom executable or "
                            "free-form command line arguments, which this version of klogg no "
                            "longer restores for security reasons. Please reopen the source "
                            "(File > Open Android Logcat / Open iOS Log Stream) to recreate "
                            "the session with a built-in transport." ) ) );
        return result;
    }

    // Schema version gate. Numeric versions outside the supported range and
    // tampered/non-numeric versions both fail closed instead of guessing.
    PayloadEra era{ PayloadEra::Current };
    const auto versionValue = object.value( QStringLiteral( "schemaVersion" ) );
    if ( versionValue.isUndefined() ) {
        era = PayloadEra::LegacyFlat;
    }
    else if ( !versionValue.isDouble() ) {
        result.diagnostics.push_back( fatalDiagnostic(
            QStringLiteral( "malformed-schema-version" ),
            liveLogMessage( "The saved live-log session has a malformed schema version; it "
                            "cannot be restored safely." ) ) );
        return result;
    }
    else {
        const auto numericVersion = versionValue.toDouble();
        if ( numericVersion != std::floor( numericVersion ) || numericVersion < 0 ) {
            result.diagnostics.push_back( fatalDiagnostic(
                QStringLiteral( "malformed-schema-version" ),
                liveLogMessage( "The saved live-log session has a malformed schema version; it "
                                "cannot be restored safely." ) ) );
            return result;
        }

        // Compare in the double domain: narrowing a hostile huge version with
        // static_cast<int> is undefined behavior and platform-dependently
        // wraps INTO the supported range, letting the payload restore as the
        // current schema.
        if ( numericVersion > static_cast<double>( kCurrentSpecVersion ) ) {
            // QString::number keeps even absurd magnitudes (2^53, 1e300,
            // infinity) well-defined in the user-facing message.
            result.diagnostics.push_back( fatalDiagnostic(
                QStringLiteral( "unsupported-schema-version" ),
                liveLogMessage( "This session was written by a newer version of klogg "
                                "(schema %1); it cannot be restored by this version." )
                    .arg( QString::number( numericVersion, 'f', 0 ) ) ) );
            return result;
        }

        const auto schemaVersion = static_cast<int>( numericVersion );

        if ( schemaVersion == 0 ) {
            era = PayloadEra::LegacyFlat;
        }
    }

    // Source kind resolution. Without a known kind the typed option blocks
    // cannot be trusted, so restore refuses rather than mislabeling.
    SourceKind kind{ SourceKind::AndroidLogcat };
    bool knownSourceKind = false;
    if ( era == PayloadEra::LegacyFlat ) {
        const auto sourceType = object.value( QStringLiteral( "sourceType" ) ).toString();
        if ( sourceType == QLatin1String( "adb_logcat" ) ) {
            kind = SourceKind::AndroidLogcat;
            knownSourceKind = true;
        }
        else if ( sourceType == QLatin1String( "ios_log_stream" ) ) {
            kind = SourceKind::IosSyslog;
            knownSourceKind = true;
        }
    }
    else {
        const auto sourceKindText = object.value( QStringLiteral( "sourceKind" ) ).toString();
        if ( sourceKindText == QLatin1String( "android_logcat" ) ) {
            kind = SourceKind::AndroidLogcat;
            knownSourceKind = true;
        }
        else if ( sourceKindText == QLatin1String( "ios_syslog" ) ) {
            kind = SourceKind::IosSyslog;
            knownSourceKind = true;
        }
    }

    if ( !knownSourceKind ) {
        result.diagnostics.push_back( fatalDiagnostic(
            QStringLiteral( "unknown-source-kind" ),
            liveLogMessage( "The saved live-log session has an unrecognized source type; it "
                            "cannot be restored safely." ) ) );
        return result;
    }

    LiveLogSessionSpec spec;
    spec.schemaVersion = kCurrentSpecVersion;
    spec.sourceKind = kind;

    // Backend discriminator. Defaults fail closed to the application-owned
    // transports; the compatibility process backends survive only when the
    // payload explicitly recorded them.
    QStringList backendKeys;
    if ( kind == SourceKind::AndroidLogcat ) {
        backendKeys << QStringLiteral( "androidBackend" )
                    << QStringLiteral( "adbBackend" ); // legacy-era alias
    }
    else {
        backendKeys << QStringLiteral( "iosBackend" );
    }

    QString discriminatorText;
    bool hasDiscriminator = false;
    for ( const auto& key : backendKeys ) {
        if ( object.contains( key ) ) {
            discriminatorText = object.value( key ).toString();
            hasDiscriminator = true;
            break;
        }
    }

    if ( !hasDiscriminator ) {
        // One-time migration for payloads from before any backend key existed:
        // to the modern application-owned backend, never to LegacyProcess,
        // with the durable marker stamped into every subsequent save.
        spec.migratedFromLegacySession = true;
        spec.legacyMigrationMarker = true;
        result.diagnostics.push_back( infoDiagnostic(
            QStringLiteral( "migrated-pre-discriminator-session" ),
            liveLogMessage( "This session predates transport selection and was migrated once "
                            "to the built-in transport." ) ) );
        LOG_INFO << "Migrated pre-discriminator live session to the built-in backend";
    }
    else if ( kind == SourceKind::AndroidLogcat ) {
        if ( discriminatorText == QLatin1String( "smart_socket" ) ) {
            spec.androidBackend = AndroidBackend::SmartSocket;
        }
        else if ( discriminatorText == QLatin1String( "process" )
                  || discriminatorText == QLatin1String( "legacy_process" ) ) {
            spec.androidBackend = AndroidBackend::LegacyProcess;
        }
        else {
            // Tampered or future value: defined modern default, never guess.
            result.diagnostics.push_back( infoDiagnostic(
                QStringLiteral( "unknown-android-backend" ),
                liveLogMessage( "Saved Android transport '%1' is not recognized; using the "
                                "built-in smart socket transport." )
                    .arg( discriminatorText ) ) );
            LOG_WARNING << "Unknown Android backend in saved session, failing closed to "
                           "smart_socket: "
                        << discriminatorText.toStdString();
        }
    }
    else {
        if ( discriminatorText == QLatin1String( "native" ) ) {
            spec.iosBackend = IosBackend::Native;
        }
        else if ( discriminatorText == QLatin1String( "legacy_process" ) ) {
            spec.iosBackend = IosBackend::LegacyProcess;
        }
        else {
            result.diagnostics.push_back( infoDiagnostic(
                QStringLiteral( "unknown-ios-backend" ),
                liveLogMessage( "Saved iOS transport '%1' is not recognized; using the "
                                "built-in native transport." )
                    .arg( discriminatorText ) ) );
            LOG_WARNING << "Unknown iOS backend in saved session, failing closed to native: "
                        << discriminatorText.toStdString();
        }
    }

    // Device identity: nested block in the current schema, flat keys before.
    const auto deviceBlock = object.value( QStringLiteral( "device" ) );
    if ( era == PayloadEra::Current && deviceBlock.isObject() ) {
        const auto device = deviceBlock.toObject();
        spec.device.deviceId = device.value( QStringLiteral( "deviceId" ) ).toString();
        spec.device.displayName = device.value( QStringLiteral( "displayName" ) ).toString();
        spec.device.connection = device.value( QStringLiteral( "connection" ) ).toString()
                                         == QLatin1String( "network" )
                                     ? DeviceIdentity::Connection::Network
                                     : DeviceIdentity::Connection::Usb;
    }
    else {
        spec.device.deviceId = object.value( QStringLiteral( "deviceSerial" ) ).toString();
        spec.device.displayName = object.value( QStringLiteral( "deviceDescription" ) ).toString();
        if ( kind == SourceKind::IosSyslog ) {
            // The legacy writer preferred the native endpoint udid when present.
            const auto udid = object.value( QStringLiteral( "iosUdid" ) ).toString();
            if ( !udid.isEmpty() ) {
                spec.device.deviceId = udid;
            }
            spec.device.connection
                = object.value( QStringLiteral( "iosConnectionType" ) ).toString()
                          == QLatin1String( "network" )
                      ? DeviceIdentity::Connection::Network
                      : DeviceIdentity::Connection::Usb;
        }
    }

    // Typed option blocks (current schema only; the flat era had none).
    if ( kind == SourceKind::AndroidLogcat ) {
        const auto androidBlock = object.value( QStringLiteral( "android" ) ).toObject();
        spec.android.buffers
            = stringArrayValues( androidBlock.value( QStringLiteral( "buffers" ) ).toArray() );
        spec.android.filterSpec = androidBlock.value( QStringLiteral( "filterSpec" ) ).toString();
        spec.android.priority = androidBlock.value( QStringLiteral( "priority" ) ).toString();
        const auto pidValue = androidBlock.value( QStringLiteral( "pid" ) );
        spec.android.pid = exactIntOrNullopt( pidValue );
    }
    else {
        const auto iosBlock = object.value( QStringLiteral( "ios" ) ).toObject();
        spec.ios.level = iosBlock.value( QStringLiteral( "level" ) ).toString();
        spec.ios.categories
            = stringArrayValues( iosBlock.value( QStringLiteral( "categories" ) ).toArray() );
        spec.ios.subsystem = iosBlock.value( QStringLiteral( "subsystem" ) ).toString();
        spec.ios.outputFormat = iosBlock.value( QStringLiteral( "outputFormat" ) ).toString()
                                        == QLatin1String( "json" )
                                    ? IosOptions::OutputFormat::Json
                                    : IosOptions::OutputFormat::Default;
    }

    // Capture output policy shares key names between eras; the flat era kept
    // them at the top level, the current schema nests them under "capture".
    auto captureBlock = object.value( QStringLiteral( "capture" ) );
    if ( !captureBlock.isObject() ) {
        captureBlock = object;
    }
    const auto capture = captureBlock.toObject();
    spec.capture.ansiOutputEnabled
        = capture.value( QStringLiteral( "ansiOutputEnabled" ) ).toBool( false );
    const auto preserveValue = capture.value( QStringLiteral( "preserveAnsiOnSave" ) );
    spec.capture.preserveAnsiOnSave
        = preserveValue.isBool()
              ? preserveValue.toBool()
              : capture.value( QStringLiteral( "outputPreserveAnsi" ) ).toBool( false );
    spec.capture.autoReconnectEnabled
        = capture.value( QStringLiteral( "autoReconnectEnabled" ) ).toBool( false );
    // Capacity-like options saturate into range: a hostile huge magnitude must
    // not wrap into the "0 = unlimited" sentinel or garbage-negative values.
    spec.capture.maxReconnectAttempts
        = saturatedNumber( capture.value( QStringLiteral( "maxReconnectAttempts" ) ), 0, 0 );
    spec.capture.captureMaxFileSize
        = saturatedNumber<qint64>( capture.value( QStringLiteral( "captureMaxFileSize" ) ), 0, 0 );
    spec.capture.captureBackupCount
        = saturatedNumber( capture.value( QStringLiteral( "captureBackupCount" ) ), 0, 0 );

    spec.boundOutputFile = object.value( QStringLiteral( "boundOutputFile" ) ).toString();

    const auto intentText = object.value( QStringLiteral( "runIntent" ) ).toString();
    spec.runIntent = intentText == QLatin1String( "running" ) ? livecapture::RunIntent::Running
                                                              : livecapture::RunIntent::Stopped;

    // Durable migration marker from an earlier save cycle: carried on the
    // marker field so serialize keeps stamping it, without reporting a new
    // migration event.
    if ( object.value( QStringLiteral( "migratedFromLegacySession" ) ).toBool( false ) ) {
        spec.legacyMigrationMarker = true;
    }

    spec.captureId = object.value( QStringLiteral( "captureId" ) ).toString();
    if ( !CaptureStore::isValidCaptureId( spec.captureId ) ) {
        result.diagnostics.push_back( fatalDiagnostic(
            QStringLiteral( "malformed-capture-id" ),
            liveLogMessage( "The saved live-log session has no usable capture identifier; it "
                            "cannot be restored." ) ) );
        return result;
    }

    result.spec = std::move( spec );
    return result;
}

bool usesCompatibilityTransport( const LiveLogSessionSpec& spec ) noexcept
{
    return spec.sourceKind == SourceKind::AndroidLogcat
               ? spec.androidBackend == AndroidBackend::LegacyProcess
               : spec.iosBackend == IosBackend::LegacyProcess;
}

namespace {

std::vector<Diagnostic> validateSpec( const LiveLogSessionSpec& spec,
                                      bool allowCompatibilityTransport )
{
    std::vector<Diagnostic> diagnostics;

    // Transitional backends exist solely so previously-saved sessions survive
    // the migration. Fresh composition may not select them; restore keeps them
    // loadable and inert while applying every other validation rule unchanged.
    if ( !allowCompatibilityTransport && usesCompatibilityTransport( spec ) ) {
        diagnostics.push_back( fatalDiagnostic(
            QStringLiteral( "transitional-backend-not-creatable" ),
            liveLogMessage( "New sessions cannot use compatibility process transports. Choose "
                            "a device detected by the built-in services." ) ) );
    }

    // A live session always targets a device; without one there is nothing to
    // reconnect to and nothing to stream later.
    if ( spec.device.deviceId.trimmed().isEmpty() ) {
        diagnostics.push_back( fatalDiagnostic(
            QStringLiteral( "running-intent-requires-device" ),
            liveLogMessage(
                "A detected device is required before the live session can connect." ) ) );
    }

    if ( !CaptureStore::isValidCaptureId( spec.captureId ) ) {
        diagnostics.push_back(
            fatalDiagnostic( QStringLiteral( "invalid-capture-id" ),
                             liveLogMessage( "The session has no usable capture identifier." ) ) );
    }

    if ( spec.sourceKind == SourceKind::AndroidLogcat
         && spec.androidBackend == AndroidBackend::SmartSocket ) {
        const auto transportConfig = makeLiveSourceTransportConfig( spec );
        const auto backendConfig = makeAdbSmartSocketTransportConfig( transportConfig );
        if ( !backendConfig.has_value() ) {
            if ( spec.android.pid.has_value() && *spec.android.pid < 0 ) {
                diagnostics.push_back( fatalDiagnostic(
                    QStringLiteral( "invalid-android-pid" ),
                    liveLogMessage(
                        "The saved Android process filter must be a non-negative PID." ) ) );
            }
            else {
                diagnostics.push_back( fatalDiagnostic(
                    QStringLiteral( "invalid-android-log-options" ),
                    liveLogMessage( "The saved Android buffers, priority, or filter expression is "
                                    "invalid." ) ) );
            }
        }
    }

    if ( spec.sourceKind == SourceKind::IosSyslog
         && ( !spec.ios.level.trimmed().isEmpty() || !spec.ios.categories.isEmpty()
              || !spec.ios.subsystem.trimmed().isEmpty()
              || spec.ios.outputFormat != IosOptions::OutputFormat::Default ) ) {
        diagnostics.push_back( fatalDiagnostic(
            QStringLiteral( "unsupported-ios-log-options" ),
            liveLogMessage( "Saved iOS level, category, subsystem, and JSON options are not "
                            "supported by the native stream yet." ) ) );
    }

    return diagnostics;
}

} // namespace

std::vector<Diagnostic> validateForAccept( const LiveLogSessionSpec& spec )
{
    return validateSpec( spec, false );
}

std::vector<Diagnostic> validateForRestore( const LiveLogSessionSpec& spec )
{
    return validateSpec( spec, true );
}

LiveLogSessionSpec withStoppedRunIntent( LiveLogSessionSpec spec )
{
    spec.runIntent = livecapture::RunIntent::Stopped;
    return spec;
}

std::vector<livecapture::LiveStateEvent> initialLiveStateEvents( const LiveLogSessionSpec& spec,
                                                                 livecapture::Timestamp now )
{
    // Explicit runtime starts route through the same accept gate as fresh
    // composition, so EVERY fatally-rejected spec refuses to arm a run —
    // including the transitional compatibility backends that exist only for
    // persistence compatibility and can never be re-created here. Timestamps
    // stay a pure pass-through; monotonic-now validation belongs to the
    // reducer.
    for ( const auto& diagnostic : validateForAccept( spec ) ) {
        if ( diagnostic.severity == Diagnostic::Severity::Fatal ) {
            return {};
        }
    }

    if ( spec.runIntent != livecapture::RunIntent::Running ) {
        return {};
    }

    return { livecapture::StartRequested{ now } };
}

LiveLogSessionSpec sessionSpecFromSessionData( const AdbLogcatSessionData& sessionData )
{
    LiveLogSessionSpec spec;
    spec.captureId = sessionData.captureId;
    spec.sourceKind = sessionData.sourceType == LiveLogSourceType::IosLogStream
                          ? SourceKind::IosSyslog
                          : SourceKind::AndroidLogcat;
    spec.androidBackend = sessionData.adbBackend == AdbTransportBackend::SmartSocket
                              ? AndroidBackend::SmartSocket
                              : AndroidBackend::LegacyProcess;
    spec.iosBackend = sessionData.iosBackend == IosTransportBackend::Native
                          ? IosBackend::Native
                          : IosBackend::LegacyProcess;
    spec.device.deviceId = sessionData.deviceSerial;
    spec.device.displayName = sessionData.deviceDescription;
    if ( spec.sourceKind == SourceKind::IosSyslog && !sessionData.iosEndpoint.udid.empty() ) {
        // The native endpoint identity wins over the generic serial column.
        spec.device.deviceId = QString::fromStdString( sessionData.iosEndpoint.udid );
    }
    spec.device.connection = sessionData.iosEndpoint.connectionType
                                     == klogg::livecapture::ios::NativeConnectionType::Network
                                 ? DeviceIdentity::Connection::Network
                                 : DeviceIdentity::Connection::Usb;
    spec.runIntent = sessionData.runIntent;
    spec.android.buffers = sessionData.androidBuffers;
    spec.android.filterSpec = sessionData.androidFilterSpec;
    spec.android.priority = sessionData.androidPriority;
    spec.android.pid = sessionData.androidPid;
    spec.ios.level = sessionData.iosLevel;
    spec.ios.categories = sessionData.iosCategories;
    spec.ios.subsystem = sessionData.iosSubsystem;
    spec.ios.outputFormat = sessionData.iosJsonOutput ? IosOptions::OutputFormat::Json
                                                      : IosOptions::OutputFormat::Default;
    spec.capture.ansiOutputEnabled = sessionData.ansiOutputEnabled;
    spec.capture.preserveAnsiOnSave = sessionData.outputAnsiMode == LiveLogSaveAnsiMode::Preserve;
    spec.capture.autoReconnectEnabled = sessionData.autoReconnectEnabled;
    spec.capture.maxReconnectAttempts = sessionData.maxReconnectAttempts;
    spec.capture.captureMaxFileSize = sessionData.captureMaxFileSize;
    spec.capture.captureBackupCount = sessionData.captureBackupCount;
    spec.boundOutputFile = sessionData.boundOutputFile;
    spec.legacyMigrationMarker = sessionData.migratedFromLegacySession;

    // adbExecutable / extraArgs / extra free-form data are intentionally NOT
    // carried over: the typed format retires raw command lines.

    return spec;
}

AdbLogcatSessionData sessionDataFromSpec( const LiveLogSessionSpec& spec )
{
    AdbLogcatSessionData sessionData;
    // Raw CLI fields stay empty forever: the typed spec cannot carry them.
    sessionData.captureId = spec.captureId;
    sessionData.sourceType = spec.sourceKind == SourceKind::IosSyslog
                                 ? LiveLogSourceType::IosLogStream
                                 : LiveLogSourceType::AdbLogcat;
    sessionData.deviceSerial = spec.device.deviceId;
    sessionData.deviceDescription = spec.device.displayName;
    sessionData.adbBackend = spec.androidBackend == AndroidBackend::SmartSocket
                                 ? AdbTransportBackend::SmartSocket
                                 : AdbTransportBackend::Process;
    sessionData.iosBackend = spec.iosBackend == IosBackend::Native
                                 ? IosTransportBackend::Native
                                 : IosTransportBackend::LegacyProcess;
    sessionData.iosEndpoint.udid = spec.device.deviceId.toStdString();
    sessionData.iosEndpoint.connectionType
        = spec.device.connection == DeviceIdentity::Connection::Network
              ? klogg::livecapture::ios::NativeConnectionType::Network
              : klogg::livecapture::ios::NativeConnectionType::Usb;
    sessionData.runIntent = spec.runIntent;
    sessionData.androidBuffers = spec.android.buffers;
    sessionData.androidFilterSpec = spec.android.filterSpec;
    sessionData.androidPriority = spec.android.priority;
    sessionData.androidPid = spec.android.pid;
    sessionData.iosLevel = spec.ios.level;
    sessionData.iosCategories = spec.ios.categories;
    sessionData.iosSubsystem = spec.ios.subsystem;
    sessionData.iosJsonOutput = spec.ios.outputFormat == IosOptions::OutputFormat::Json;
    sessionData.readOnlyCompatibility = usesCompatibilityTransport( spec );
    sessionData.ansiOutputEnabled = spec.capture.ansiOutputEnabled;
    sessionData.outputAnsiMode = spec.capture.preserveAnsiOnSave ? LiveLogSaveAnsiMode::Preserve
                                                                 : LiveLogSaveAnsiMode::Strip;
    sessionData.autoReconnectEnabled = spec.capture.autoReconnectEnabled;
    sessionData.maxReconnectAttempts = spec.capture.maxReconnectAttempts;
    sessionData.captureMaxFileSize = spec.capture.captureMaxFileSize;
    sessionData.captureBackupCount = spec.capture.captureBackupCount;
    sessionData.boundOutputFile = spec.boundOutputFile;
    sessionData.migratedFromLegacySession
        = spec.migratedFromLegacySession || spec.legacyMigrationMarker;

    return sessionData;
}

} // namespace klogg::livelog
