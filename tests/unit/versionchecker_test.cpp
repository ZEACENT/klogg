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

#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>

#include "configuration.h"
#include "klogg_version.h"
#include "test_utils.h"
#include "versionchecker.h"

namespace {

// Build JSON in the GitHub Releases API format.
// https://docs.github.com/en/rest/releases/releases#get-the-latest-release
QByteArray makeReleaseJson( const QString& tagName, const QString& htmlUrl,
                            const QString& body = {} )
{
    QJsonObject root;
    root[ "tag_name" ] = tagName;
    root[ "html_url" ] = htmlUrl;
    root[ "prerelease" ] = false;
    if ( !body.isEmpty() ) {
        root[ "body" ] = body;
    }
    return QJsonDocument( root ).toJson( QJsonDocument::Compact );
}

// Convenience: tag without "v" prefix gets "v" prepended automatically
QByteArray makeReleaseJsonForVersion( const QString& version, const QString& htmlUrl,
                                      const QString& body = {} )
{
    return makeReleaseJson( QStringLiteral( "v%1" ).arg( version ), htmlUrl, body );
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
    SafeQSignalSpy versionSpy( &checker, SIGNAL( newVersionFound( QString, QString, QStringList ) ) );

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
    SafeQSignalSpy versionSpy( &checker, SIGNAL( newVersionFound( QString, QString, QStringList ) ) );
    SafeQSignalSpy completedSpy( &checker, SIGNAL( checkCompleted( bool ) ) );

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
    SafeQSignalSpy versionSpy( &checker, SIGNAL( newVersionFound( QString, QString, QStringList ) ) );
    SafeQSignalSpy completedSpy( &checker, SIGNAL( checkCompleted( bool ) ) );

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
    SafeQSignalSpy versionSpy( &checker, SIGNAL( newVersionFound( QString, QString, QStringList ) ) );

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
    SafeQSignalSpy versionSpy( &checker, SIGNAL( newVersionFound( QString, QString, QStringList ) ) );

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
    SafeQSignalSpy versionSpy( &checker, SIGNAL( newVersionFound( QString, QString, QStringList ) ) );

    const auto body = QStringLiteral( "## Changes\n- Feature A\n- Bug fix B" );
    const auto json = makeReleaseJsonForVersion( QStringLiteral( "99.0.0.0" ),
                                                  QStringLiteral( "https://example.com" ), body );

    checker.checkVersionData( json );

    REQUIRE( versionSpy.count() == 1 );
    const auto changes = versionSpy.at( 0 ).at( 2 ).toStringList();
    REQUIRE( changes.size() == 1 );
    CHECK( changes.at( 0 ) == body );
}

TEST_CASE( "checkVersionData: empty body produces empty changelog", "[versionchecker]" )
{
    VersionChecker checker;
    SafeQSignalSpy versionSpy( &checker, SIGNAL( newVersionFound( QString, QString, QStringList ) ) );

    const auto json = makeReleaseJsonForVersion( QStringLiteral( "99.0.0.0" ),
                                                  QStringLiteral( "https://example.com" ),
                                                  QString{} );

    checker.checkVersionData( json );

    REQUIRE( versionSpy.count() == 1 );
    const auto changes = versionSpy.at( 0 ).at( 2 ).toStringList();
    CHECK( changes.isEmpty() );
}

TEST_CASE( "checkVersionData: prerelease field is ignored (API guarantees false)", "[versionchecker]" )
{
    // The /releases/latest endpoint already excludes prereleases. Even if the
    // JSON happens to have "prerelease": true, checkVersionData should still
    // use it — the filtering is done by the API endpoint, not by our parser.
    VersionChecker checker;
    SafeQSignalSpy versionSpy( &checker, SIGNAL( newVersionFound( QString, QString, QStringList ) ) );

    QJsonObject root;
    root[ "tag_name" ] = QStringLiteral( "v99.0.0.0" );
    root[ "html_url" ] = QStringLiteral( "https://example.com" );
    root[ "prerelease" ] = true; // ignored by our parser

    const auto json = QJsonDocument( root ).toJson( QJsonDocument::Compact );
    const bool foundNewer = checker.checkVersionData( json );
    REQUIRE( foundNewer );
}

TEST_CASE( "forceCheck: emits checkCompleted(false) when version checking is disabled",
           "[versionchecker]" )
{
    ScopedVersionCheckConfigGuard configGuard;
    configGuard.setVersionCheckingEnabled( false );

    VersionChecker checker;
    SafeQSignalSpy completedSpy( &checker, SIGNAL( checkCompleted( bool ) ) );

    checker.forceCheck();

    REQUIRE( completedSpy.count() == 1 );
    CHECK_FALSE( completedSpy.at( 0 ).at( 0 ).toBool() );
}

TEST_CASE( "checkVersionData: handles malformed JSON gracefully", "[versionchecker]" )
{
    VersionChecker checker;
    SafeQSignalSpy versionSpy( &checker, SIGNAL( newVersionFound( QString, QString, QStringList ) ) );

    const QByteArray badJson = QByteArrayLiteral( "not valid json" );
    const bool foundNewer = checker.checkVersionData( badJson );

    // Empty/invalid JSON — tag_name will be empty, which is not newer
    REQUIRE_FALSE( foundNewer );
    CHECK( versionSpy.count() == 0 );
}

TEST_CASE( "checkVersionData: handles missing tag_name gracefully", "[versionchecker]" )
{
    VersionChecker checker;
    SafeQSignalSpy versionSpy( &checker, SIGNAL( newVersionFound( QString, QString, QStringList ) ) );

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
    SafeQSignalSpy versionSpy( &checker, SIGNAL( newVersionFound( QString, QString, QStringList ) ) );

    const auto json = makeReleaseJsonForVersion( QStringLiteral( "99.0" ),
                                                  QStringLiteral( "https://example.com" ) );

    const bool foundNewer = checker.checkVersionData( json );
    REQUIRE( foundNewer );
    CHECK( versionSpy.at( 0 ).at( 0 ).toString() == QStringLiteral( "99.0" ) );
}
