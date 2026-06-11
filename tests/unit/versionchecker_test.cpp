/*
 * Copyright (C) 2026 ZEACENT and other contributors
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

#include <catch2/catch.hpp>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QSysInfo>
#include <QVariantList>
#include <QVariantMap>

#include "configuration.h"
#include "klogg_version.h"
#include "test_utils.h"
#include "versionchecker.h"

namespace {

// Build JSON in the GitHub Releases API format.
// https://docs.github.com/en/rest/releases/releases#get-the-latest-release
QByteArray makeReleaseJson( const QString& tagName, const QString& htmlUrl,
                            const QString& body = {},
                            const QVariantList& assets = {} )
{
    QJsonObject root;
    root[ "tag_name" ] = tagName;
    root[ "html_url" ] = htmlUrl;
    root[ "prerelease" ] = false;
    if ( !body.isEmpty() ) {
        root[ "body" ] = body;
    }
    if ( !assets.isEmpty() ) {
        root[ "assets" ] = QJsonArray::fromVariantList( assets );
    }
    return QJsonDocument( root ).toJson( QJsonDocument::Compact );
}

// Convenience: tag without "v" prefix gets "v" prepended automatically
QByteArray makeReleaseJsonForVersion( const QString& version, const QString& htmlUrl,
                                      const QString& body = {},
                                      const QVariantList& assets = {} )
{
    return makeReleaseJson( QStringLiteral( "v%1" ).arg( version ), htmlUrl, body, assets );
}

class ScopedVersionCheckConfigGuard {
  public:
    ScopedVersionCheckConfigGuard()
        : config_( Configuration::getSynced() )
        , originalValue_( config_.versionCheckingEnabled() )
    {
    }

    ~ScopedVersionCheckConfigGuard()
    {
        config_.setVersionCheckingEnabled( originalValue_ );
        config_.save();
    }

    void setVersionCheckingEnabled( bool enabled )
    {
        config_.setVersionCheckingEnabled( enabled );
        config_.save();
    }

  private:
    Configuration& config_;
    bool originalValue_;
};

} // namespace

TEST_CASE( "checkVersionData: newer tag_name emits newVersionFound", "[versionchecker]" )
{
    VersionChecker checker;
    SafeQSignalSpy versionSpy( &checker, SIGNAL( newVersionFound( QString, QString, QString, QStringList ) ) );

    const auto releaseUrl = QStringLiteral( "https://github.com/ZEACENT/klogg/releases/tag/v99.0.0.0" );
    const auto json = makeReleaseJsonForVersion( QStringLiteral( "99.0.0.0" ), releaseUrl );

    const bool foundNewer = checker.checkVersionData( json );
    REQUIRE( foundNewer );

    REQUIRE( versionSpy.count() == 1 );
    const auto args = versionSpy.at( 0 );
    // "v" prefix should be stripped
    REQUIRE( args.at( 0 ).toString() == QStringLiteral( "99.0.0.0" ) );
    REQUIRE( args.at( 1 ).toString() == releaseUrl );
}

TEST_CASE( "checkVersionData: older tag_name returns false, no signal", "[versionchecker]" )
{
    VersionChecker checker;
    SafeQSignalSpy versionSpy( &checker, SIGNAL( newVersionFound( QString, QString, QString, QStringList ) ) );
    SafeQSignalSpy completedSpy( &checker, SIGNAL( checkCompleted( bool, bool ) ) );

    const auto json = makeReleaseJsonForVersion( QStringLiteral( "1.0.0.0" ),
                                                  QStringLiteral( "https://example.com" ) );

    const bool foundNewer = checker.checkVersionData( json );
    REQUIRE_FALSE( foundNewer );

    CHECK( versionSpy.count() == 0 );
    CHECK( completedSpy.count() == 0 );
}

TEST_CASE( "checkVersionData: equal tag_name returns false, no signal", "[versionchecker]" )
{
    VersionChecker checker;
    SafeQSignalSpy versionSpy( &checker, SIGNAL( newVersionFound( QString, QString, QString, QStringList ) ) );
    SafeQSignalSpy completedSpy( &checker, SIGNAL( checkCompleted( bool, bool ) ) );

    const auto currentVersion = QString::fromLatin1( kloggVersion().data(), kloggVersion().size() );
    const auto json = makeReleaseJsonForVersion( currentVersion,
                                                  QStringLiteral( "https://example.com" ) );

    const bool foundNewer = checker.checkVersionData( json );
    REQUIRE_FALSE( foundNewer );

    CHECK( versionSpy.count() == 0 );
    CHECK( completedSpy.count() == 0 );
}

TEST_CASE( "checkVersionData: strips v prefix from tag_name", "[versionchecker]" )
{
    VersionChecker checker;
    SafeQSignalSpy versionSpy( &checker, SIGNAL( newVersionFound( QString, QString, QString, QStringList ) ) );

    // tag_name with "v" prefix
    const auto json = makeReleaseJson( QStringLiteral( "v99.0.0.0" ),
                                        QStringLiteral( "https://example.com/v99.0.0.0" ) );

    const bool foundNewer = checker.checkVersionData( json );
    REQUIRE( foundNewer );

    REQUIRE( versionSpy.count() == 1 );
    // Version emitted should NOT have the "v" prefix
    REQUIRE( versionSpy.at( 0 ).at( 0 ).toString() == QStringLiteral( "99.0.0.0" ) );
}

TEST_CASE( "checkVersionData: tag_name without v prefix still works", "[versionchecker]" )
{
    VersionChecker checker;
    SafeQSignalSpy versionSpy( &checker, SIGNAL( newVersionFound( QString, QString, QString, QStringList ) ) );

    // Some older GitHub releases might not have the "v" prefix
    const auto json = makeReleaseJson( QStringLiteral( "99.0.0.0" ),
                                        QStringLiteral( "https://example.com" ) );

    const bool foundNewer = checker.checkVersionData( json );
    REQUIRE( foundNewer );

    REQUIRE( versionSpy.at( 0 ).at( 0 ).toString() == QStringLiteral( "99.0.0.0" ) );
}

TEST_CASE( "checkVersionData: release body is passed as changelog", "[versionchecker]" )
{
    VersionChecker checker;
    SafeQSignalSpy versionSpy( &checker, SIGNAL( newVersionFound( QString, QString, QString, QStringList ) ) );

    const auto body = QStringLiteral( "## Changes\n- Feature A\n- Bug fix B" );
    const auto json = makeReleaseJsonForVersion( QStringLiteral( "99.0.0.0" ),
                                                  QStringLiteral( "https://example.com" ), body );

    checker.checkVersionData( json );

    REQUIRE( versionSpy.count() == 1 );
    const auto changes = versionSpy.at( 0 ).at( 3 ).toStringList();
    REQUIRE( changes.size() == 1 );
    CHECK( changes.at( 0 ) == body );
}

TEST_CASE( "checkVersionData: empty body produces empty changelog", "[versionchecker]" )
{
    VersionChecker checker;
    SafeQSignalSpy versionSpy( &checker, SIGNAL( newVersionFound( QString, QString, QString, QStringList ) ) );

    const auto json = makeReleaseJsonForVersion( QStringLiteral( "99.0.0.0" ),
                                                  QStringLiteral( "https://example.com" ),
                                                  QString{} );

    checker.checkVersionData( json );

    REQUIRE( versionSpy.count() == 1 );
    const auto changes = versionSpy.at( 0 ).at( 3 ).toStringList();
    CHECK( changes.isEmpty() );
}

TEST_CASE( "checkVersionData: prerelease field is ignored (API guarantees false)", "[versionchecker]" )
{
    // The /releases/latest endpoint already excludes prereleases. Even if the
    // JSON happens to have "prerelease": true, checkVersionData should still
    // use it — the filtering is done by the API endpoint, not by our parser.
    VersionChecker checker;
    SafeQSignalSpy versionSpy( &checker, SIGNAL( newVersionFound( QString, QString, QString, QStringList ) ) );

    QJsonObject root;
    root[ "tag_name" ] = QStringLiteral( "v99.0.0.0" );
    root[ "html_url" ] = QStringLiteral( "https://example.com" );
    root[ "prerelease" ] = true; // ignored by our parser

    const auto json = QJsonDocument( root ).toJson( QJsonDocument::Compact );
    const bool foundNewer = checker.checkVersionData( json );
    REQUIRE( foundNewer );
}

TEST_CASE( "checkVersionData: handles malformed JSON gracefully", "[versionchecker]" )
{
    VersionChecker checker;
    SafeQSignalSpy versionSpy( &checker, SIGNAL( newVersionFound( QString, QString, QString, QStringList ) ) );

    const QByteArray badJson = QByteArrayLiteral( "not valid json" );
    const bool foundNewer = checker.checkVersionData( badJson );

    // Empty/invalid JSON — tag_name will be empty, which is not newer
    REQUIRE_FALSE( foundNewer );
    CHECK( versionSpy.count() == 0 );
}

TEST_CASE( "checkVersionData: handles missing tag_name gracefully", "[versionchecker]" )
{
    VersionChecker checker;
    SafeQSignalSpy versionSpy( &checker, SIGNAL( newVersionFound( QString, QString, QString, QStringList ) ) );

    QJsonObject root;
    // Only html_url present, no tag_name
    root[ "html_url" ] = QStringLiteral( "https://example.com" );

    const auto json = QJsonDocument( root ).toJson( QJsonDocument::Compact );
    const bool foundNewer = checker.checkVersionData( json );

    // Empty version string is not newer than current
    REQUIRE_FALSE( foundNewer );
    CHECK( versionSpy.count() == 0 );
}

TEST_CASE( "checkVersionData: version with only major.minor segments", "[versionchecker]" )
{
    VersionChecker checker;
    SafeQSignalSpy versionSpy( &checker, SIGNAL( newVersionFound( QString, QString, QString, QStringList ) ) );

    const auto json = makeReleaseJsonForVersion( QStringLiteral( "99.0" ),
                                                  QStringLiteral( "https://example.com" ) );

    const bool foundNewer = checker.checkVersionData( json );
    REQUIRE( foundNewer );
    CHECK( versionSpy.at( 0 ).at( 0 ).toString() == QStringLiteral( "99.0" ) );
}

TEST_CASE( "checkVersionData: ignored version suppresses notification", "[versionchecker]" )
{
    VersionChecker checker;
    SafeQSignalSpy versionSpy(
        &checker, SIGNAL( newVersionFound( QString, QString, QString, QStringList ) ) );

    // Set the ignored version to the same version as the release
    auto& deadlineConfig = VersionCheckerConfig::getSynced();
    deadlineConfig.setIgnoredVersion( QStringLiteral( "99.0.0.0" ) );
    deadlineConfig.save();

    const auto json = makeReleaseJsonForVersion( QStringLiteral( "99.0.0.0" ),
                                                  QStringLiteral( "https://example.com" ) );

    const bool foundNewer = checker.checkVersionData( json );
    REQUIRE_FALSE( foundNewer );
    CHECK( versionSpy.count() == 0 );

    // Clean up
    deadlineConfig.setIgnoredVersion( {} );
    deadlineConfig.save();
}

TEST_CASE( "checkVersionData: different version not suppressed when ignored version differs",
           "[versionchecker]" )
{
    VersionChecker checker;
    SafeQSignalSpy versionSpy(
        &checker, SIGNAL( newVersionFound( QString, QString, QString, QStringList ) ) );

    auto& deadlineConfig = VersionCheckerConfig::getSynced();
    deadlineConfig.setIgnoredVersion( QStringLiteral( "1.0.0.0" ) );
    deadlineConfig.save();

    const auto json = makeReleaseJsonForVersion( QStringLiteral( "99.0.0.0" ),
                                                  QStringLiteral( "https://example.com" ) );

    const bool foundNewer = checker.checkVersionData( json );
    REQUIRE( foundNewer );
    CHECK( versionSpy.count() == 1 );

    // Clean up
    deadlineConfig.setIgnoredVersion( {} );
    deadlineConfig.save();
}

TEST_CASE( "checkVersionData: extracts download URL from assets matching current arch",
           "[versionchecker]" )
{
    VersionChecker checker;
    SafeQSignalSpy versionSpy(
        &checker, SIGNAL( newVersionFound( QString, QString, QString, QStringList ) ) );

    const auto currentArch = QSysInfo::currentCpuArchitecture();

    QVariantList assets;
    QVariantMap asset;
#if defined( Q_OS_MAC )
    asset[ "name" ] = QStringLiteral( "klogg-99.0.0.0-macOS-%1.dmg" ).arg( currentArch );
    asset[ "browser_download_url" ]
        = QStringLiteral( "https://example.com/download/klogg-99.0.0.0-macOS-%1.dmg" )
              .arg( currentArch );
#elif defined( Q_OS_WIN )
    asset[ "name" ] = QStringLiteral( "klogg-99.0.0.0-win64-%1.exe" ).arg( currentArch );
    asset[ "browser_download_url" ]
        = QStringLiteral( "https://example.com/download/klogg-99.0.0.0-win64-%1.exe" )
              .arg( currentArch );
#else
    asset[ "name" ] = QStringLiteral( "klogg-99.0.0.0-linux-%1.appimage" ).arg( currentArch );
    asset[ "browser_download_url" ]
        = QStringLiteral( "https://example.com/download/klogg-99.0.0.0-linux-%1.appimage" )
              .arg( currentArch );
#endif
    assets << asset;

    const auto json = makeReleaseJsonForVersion(
        QStringLiteral( "99.0.0.0" ), QStringLiteral( "https://example.com" ), {}, assets );

    const bool foundNewer = checker.checkVersionData( json );
    REQUIRE( foundNewer );

    REQUIRE( versionSpy.count() == 1 );
    // downloadUrl is at index 2, should contain the asset URL
    const auto downloadUrl = versionSpy.at( 0 ).at( 2 ).toString();
    CHECK( downloadUrl.contains( currentArch ) );
}

TEST_CASE( "checkVersionData: falls back to platform match when no arch-specific asset",
           "[versionchecker]" )
{
    VersionChecker checker;
    SafeQSignalSpy versionSpy(
        &checker, SIGNAL( newVersionFound( QString, QString, QString, QStringList ) ) );

    // Asset name without any architecture string
    QVariantList assets;
    QVariantMap asset;
#if defined( Q_OS_MAC )
    asset[ "name" ] = QStringLiteral( "klogg-99.0.0.0-macOS.dmg" );
#elif defined( Q_OS_WIN )
    asset[ "name" ] = QStringLiteral( "klogg-99.0.0.0-win64.exe" );
#else
    asset[ "name" ] = QStringLiteral( "klogg-99.0.0.0-linux.AppImage" );
#endif
    asset[ "browser_download_url" ]
        = QStringLiteral( "https://example.com/download/%1" ).arg( asset[ "name" ].toString() );
    assets << asset;

    const auto json = makeReleaseJsonForVersion(
        QStringLiteral( "99.0.0.0" ), QStringLiteral( "https://example.com" ), {}, assets );

    const bool foundNewer = checker.checkVersionData( json );
    REQUIRE( foundNewer );

    REQUIRE( versionSpy.count() == 1 );
    // downloadUrl should not be empty — fallback matched
    const auto downloadUrl = versionSpy.at( 0 ).at( 2 ).toString();
    CHECK_FALSE( downloadUrl.isEmpty() );
}

TEST_CASE( "checkVersionData: empty assets produces empty downloadUrl", "[versionchecker]" )
{
    VersionChecker checker;
    SafeQSignalSpy versionSpy(
        &checker, SIGNAL( newVersionFound( QString, QString, QString, QStringList ) ) );

    const auto json = makeReleaseJsonForVersion( QStringLiteral( "99.0.0.0" ),
                                                  QStringLiteral( "https://example.com" ),
                                                  {}, QVariantList{} );

    const bool foundNewer = checker.checkVersionData( json );
    REQUIRE( foundNewer );

    REQUIRE( versionSpy.count() == 1 );
    // downloadUrl should be empty — no assets
    const auto downloadUrl = versionSpy.at( 0 ).at( 2 ).toString();
    CHECK( downloadUrl.isEmpty() );
}

TEST_CASE( "checkVersionData: no matching platform asset produces empty downloadUrl",
           "[versionchecker]" )
{
    VersionChecker checker;
    SafeQSignalSpy versionSpy(
        &checker, SIGNAL( newVersionFound( QString, QString, QString, QStringList ) ) );

    // Assets only for a different platform (e.g., Windows .exe on macOS)
    QVariantList assets;
    QVariantMap asset;
    asset[ "name" ] = QStringLiteral( "klogg-99.0.0.0-win64.exe" );
    asset[ "browser_download_url" ]
        = QStringLiteral( "https://example.com/download/klogg-99.0.0.0-win64.exe" );
    assets << asset;

    const auto json = makeReleaseJsonForVersion(
        QStringLiteral( "99.0.0.0" ), QStringLiteral( "https://example.com" ), {}, assets );

    const bool foundNewer = checker.checkVersionData( json );
    REQUIRE( foundNewer );

    REQUIRE( versionSpy.count() == 1 );
    const auto downloadUrl = versionSpy.at( 0 ).at( 2 ).toString();

#if defined( Q_OS_WIN )
    // On Windows, the .exe should match
    CHECK_FALSE( downloadUrl.isEmpty() );
#else
    // On non-Windows, the .exe should NOT match
    CHECK( downloadUrl.isEmpty() );
#endif
}

TEST_CASE( "startCheck: does nothing when version checking is disabled",
           "[versionchecker]" )
{
    ScopedVersionCheckConfigGuard configGuard;
    configGuard.setVersionCheckingEnabled( false );

    VersionChecker checker;
    SafeQSignalSpy versionSpy(
        &checker, SIGNAL( newVersionFound( QString, QString, QString, QStringList ) ) );
    SafeQSignalSpy completedSpy( &checker, SIGNAL( checkCompleted( bool, bool ) ) );

    checker.startCheck();

    // startCheck returns immediately when version checking is disabled.
    // No signals should be emitted (no network request is made).
    CHECK( versionSpy.count() == 0 );
    CHECK( completedSpy.count() == 0 );
}

TEST_CASE( "startCheck: does nothing when deadline not reached", "[versionchecker]" )
{
    ScopedVersionCheckConfigGuard configGuard;
    configGuard.setVersionCheckingEnabled( true );

    auto& deadlineConfig = VersionCheckerConfig::get();
    // Set deadline far in the future
    const auto futureDeadline = std::time( nullptr ) + 86400 * 365;
    deadlineConfig.setNextDeadline( futureDeadline );
    deadlineConfig.save();

    VersionChecker checker;
    SafeQSignalSpy versionSpy(
        &checker, SIGNAL( newVersionFound( QString, QString, QString, QStringList ) ) );
    SafeQSignalSpy completedSpy( &checker, SIGNAL( checkCompleted( bool, bool ) ) );

    checker.startCheck();

    // No network request should be made — deadline hasn't passed
    CHECK( versionSpy.count() == 0 );
    CHECK( completedSpy.count() == 0 );

    // Reset
    deadlineConfig.setNextDeadline( 0 );
    deadlineConfig.save();
}

TEST_CASE( "forceCheck: emits checkCompleted with hadError when version checking disabled",
           "[versionchecker]" )
{
    ScopedVersionCheckConfigGuard configGuard;
    configGuard.setVersionCheckingEnabled( false );

    VersionChecker checker;
    SafeQSignalSpy completedSpy( &checker, SIGNAL( checkCompleted( bool, bool ) ) );

    checker.forceCheck();

    REQUIRE( completedSpy.count() == 1 );
    CHECK_FALSE( completedSpy.at( 0 ).at( 0 ).toBool() );   // newVersionFound = false
    CHECK( completedSpy.at( 0 ).at( 1 ).toBool() );          // hadError = true
}

TEST_CASE( "VersionChecker: lifecycle safety — destroy during pending check",
           "[versionchecker]" )
{
    // Verify that destroying a VersionChecker while a check might be
    // in-flight does not crash. The QNetworkAccessManager is a child
    // of VersionChecker and should be cleaned up automatically.
    ScopedVersionCheckConfigGuard configGuard;
    configGuard.setVersionCheckingEnabled( true );

    auto& deadlineConfig = VersionCheckerConfig::get();
    deadlineConfig.setNextDeadline( 0 ); // past deadline
    deadlineConfig.save();

    {
        VersionChecker checker;
        checker.startCheck();
        // checker is destroyed here — QNetworkAccessManager must be cleaned up
    }

    // Process any pending events to ensure cleanup
    QTest::qWait( 100 );

    // Reset
    deadlineConfig.setNextDeadline( std::time( nullptr ) + 86400 * 365 );
    deadlineConfig.save();
}
