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

#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include <QtGlobal>

#include <QString>
#include <QStringList>

#include "adblogcatsessiondata.h"
#include "livestate.h"

/*
 * The versioned, typed, source-neutral live-log session spec.
 *
 * This module replaces the flat AdbLogcatSessionData JSON (executable paths
 * and free-form argument blobs) as the persisted form of a live log session.
 * Everything here is pure data plumbing: parse, serialize, validate, and the
 * run-intent to live-state event mapping never touch PATH, an SDK, a Python
 * environment, processes, devices, or wall-clock time.
 */

namespace klogg::livelog {

// Bump on incompatible payload changes; older versions migrate in
// parsePersistedSpec(), newer versions are refused without guessing.
inline constexpr int kCurrentSpecVersion = 1;

enum class SourceKind : std::uint8_t { AndroidLogcat, IosSyslog };

// Android transport selection. LegacyProcess exists ONLY so sessions saved
// while the compatibility transports existed keep restoring; fresh sessions
// must never select it (see validateForAccept).
enum class AndroidBackend : std::uint8_t { SmartSocket, LegacyProcess };

// iOS transport selection, same transitional rule for LegacyProcess.
enum class IosBackend : std::uint8_t { Native, LegacyProcess };

struct DeviceIdentity {
    enum class Connection : std::uint8_t { Usb, Network };

    QString deviceId;
    QString displayName;
    Connection connection{ Connection::Usb };
};

struct AndroidOptions {
    QStringList buffers;
    QString filterSpec;
    QString priority;
    std::optional<int> pid;
};

struct IosOptions {
    enum class OutputFormat : std::uint8_t { Default, Json };

    QString level;
    QStringList categories;
    QString subsystem;
    OutputFormat outputFormat{ OutputFormat::Default };
};

struct CaptureOutputOptions {
    bool ansiOutputEnabled{ false };
    bool preserveAnsiOnSave{ false };
    bool autoReconnectEnabled{ false };
    int maxReconnectAttempts{ 0 };  // 0 = unlimited
    qint64 captureMaxFileSize{ 0 }; // bytes, 0 = unlimited
    int captureBackupCount{ 0 };    // 0 = keep all rotated files
};

struct LiveLogSessionSpec {
    // Always normalized to kCurrentSpecVersion by parsePersistedSpec().
    int schemaVersion{ kCurrentSpecVersion };
    QString captureId;
    SourceKind sourceKind{ SourceKind::AndroidLogcat };
    AndroidBackend androidBackend{ AndroidBackend::SmartSocket };
    IosBackend iosBackend{ IosBackend::Native };
    DeviceIdentity device;
    livecapture::RunIntent runIntent{ livecapture::RunIntent::Stopped };
    AndroidOptions android;
    IosOptions ios;
    CaptureOutputOptions capture;
    QString boundOutputFile;

    // Event flag: true only when THIS restore performed the one-time
    // pre-discriminator migration. Never persisted as such — consumers surface
    // the migration notice once; reparsing the saved payload reads false again.
    bool migratedFromLegacySession{ false };

    // Durable marker: this session descends from a pre-discriminator legacy
    // session. Read from the payload on restore and re-stamped into every save
    // by serializeSpec() — stamping on retrieve only would erase the marker on
    // the next reload cycle and re-migrate over explicit choices.
    bool legacyMigrationMarker{ false };

    // adb://<captureId> / ios-log://<captureId>: matches the historical
    // document ids so restore keeps dedup and tab identity stable.
    QString documentId() const;
    QString displayName() const;
};

struct Diagnostic {
    enum class Severity : std::uint8_t { Info, Fatal };

    Severity severity{ Severity::Info };
    QString code;
    QString message;
};

struct ParseResult {
    std::optional<LiveLogSessionSpec> spec;
    std::vector<Diagnostic> diagnostics;

    // A spec is produced unless a fatal diagnostic fired; info diagnostics
    // (fail-closed fallbacks, one-time migrations) still yield a spec.
    bool ok() const
    {
        return spec.has_value();
    }

    bool hasFatalDiagnostic() const;
};

// User-facing live-log session texts. The i18n catalogs carry this exact
// context name (klogg::livelog::messages); the texts stay beside the
// parse/validate logic that emits them.
namespace messages {
// Non-error note for sessions persisted on a transitional compatibility
// backend: they restore loadable but never arm, presented read-only.
QString compatibilityTransportReadOnly();
QString captureIdentifierAlreadyInUse();
} // namespace messages

QString serializeSpec( const LiveLogSessionSpec& spec );
ParseResult parsePersistedSpec( const QString& json );
bool usesCompatibilityTransport( const LiveLogSessionSpec& spec ) noexcept;
std::vector<Diagnostic> validateForAccept( const LiveLogSessionSpec& spec );
std::vector<Diagnostic> validateForRestore( const LiveLogSessionSpec& spec );

// Persistence snapshots an inactive copy without mutating the runtime session;
// every non-runtime field remains unchanged.
LiveLogSessionSpec withStoppedRunIntent( LiveLogSessionSpec spec );

// Explicit runtime run intent expressed through the reducer's inputs: Running
// maps to a single StartRequested event and Stopped maps to no events.
// Fail-closed like validateForAccept: sessions without a device target or with
// an unusable capture id never arm a start, and the timestamp is passed through
// verbatim (monotonic-now validation belongs to the reducer). Persistence and
// session restoration stay inert without using this mapping.
std::vector<livecapture::LiveStateEvent> initialLiveStateEvents( const LiveLogSessionSpec& spec,
                                                                 livecapture::Timestamp now );

// --- Transitional bridge (Task 6 migration) ---------------------------------
//
// AdbLogcatSessionData remains the runtime session object carried by open
// tabs and transports. These two conversions move it in and out of the typed
// spec at the persistence boundary only. Raw CLI fields are dropped in both
// directions: the spec format cannot carry them, ever.

LiveLogSessionSpec sessionSpecFromSessionData( const AdbLogcatSessionData& sessionData );
AdbLogcatSessionData sessionDataFromSpec( const LiveLogSessionSpec& spec );

} // namespace klogg::livelog
