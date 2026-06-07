/*
 * Copyright (C) 2014 Nicolas Bonnefon and other contributors
 *
 * This file is part of glogg.
 *
 * glogg is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * glogg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with glogg.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Copyright (C) 2019 Anton Filimonov and other contributors
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

#include "versionchecker.h"
#include "configuration.h"
#include "log.h"

#include "klogg_version.h"
#include "dispatch_to.h"

#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QSysInfo>
#include <QUrl>
#include <QtConcurrent>

namespace {

const auto kReleaseApiUrl
    = QLatin1String( "https://api.github.com/repos/ZEACENT/klogg/releases/latest", 58 );
static constexpr std::time_t CHECK_INTERVAL_S = 3600 * 24 * 7; /* 7 days */

bool isVersionNewer( const QString& current_version, const QString& new_version )
{
#if ( QT_VERSION >= QT_VERSION_CHECK( 6, 4, 0 ) )
    const auto parseVersion = []( const QString& version_string ) {
        qsizetype tweak_index = 0;
        auto version = QVersionNumber::fromString( QAnyStringView(version_string), &tweak_index );
        return std::make_pair( version, version_string.right( tweak_index + 1 ).toUInt() );
    };
#else
    const auto parseVersion = []( const QString& version_string ) {
        int tweak_index = 0;
        auto version = QVersionNumber::fromString( version_string, &tweak_index );
        return std::make_pair( version, version_string.right( tweak_index + 1 ).toUInt() );
    };
#endif

    const auto old = parseVersion( current_version );
    const auto next = parseVersion( new_version );

    return next > old;
}

QString selectAssetDownloadUrl( const QVariantList& assets )
{
    if ( assets.isEmpty() ) {
        return {};
    }

    // Platform-specific extension(s)
#if defined( Q_OS_MAC )
    const QStringList platformExtensions{ QStringLiteral( ".dmg" ) };
#elif defined( Q_OS_WIN )
    const QStringList platformExtensions{ QStringLiteral( ".exe" ), QStringLiteral( ".msi" ) };
#else
    const QStringList platformExtensions{ QStringLiteral( ".appimage" ), QStringLiteral( ".deb" ) };
#endif

    // Architecture aliases for the current CPU
    const auto currentArch = QSysInfo::currentCpuArchitecture().toLower();
    QStringList archAliases;
    if ( currentArch == QStringLiteral( "arm64" ) || currentArch == QStringLiteral( "aarch64" ) ) {
        archAliases = QStringList{ QStringLiteral( "arm64" ), QStringLiteral( "aarch64" ) };
    }
    else if ( currentArch == QStringLiteral( "x86_64" )
              || currentArch == QStringLiteral( "amd64" ) ) {
        archAliases
            = QStringList{ QStringLiteral( "x86_64" ), QStringLiteral( "x64" ), QStringLiteral( "amd64" ) };
    }
    else {
        archAliases = QStringList{ currentArch };
    }

    auto assetMatchesExt = [ & ]( const QVariantMap& asset ) {
        const auto name = asset.value( QStringLiteral( "name" ) ).toString().toLower();
        return std::any_of( platformExtensions.begin(), platformExtensions.end(),
                            [ & ]( const QString& ext ) { return name.endsWith( ext ); } );
    };

    // Pass 1: exact arch + platform match
    for ( const auto& a : assets ) {
        const auto assetMap = a.toMap();
        if ( !assetMatchesExt( assetMap ) ) {
            continue;
        }
        const auto name = assetMap.value( QStringLiteral( "name" ) ).toString().toLower();
        for ( const auto& alias : archAliases ) {
            if ( name.contains( alias ) ) {
                return assetMap.value( QStringLiteral( "browser_download_url" ) ).toString();
            }
        }
    }

    // Pass 2: platform match only (fallback for un-arch'd assets)
    for ( const auto& a : assets ) {
        const auto assetMap = a.toMap();
        if ( assetMatchesExt( assetMap ) ) {
            return assetMap.value( QStringLiteral( "browser_download_url" ) ).toString();
        }
    }

    return {};
}

} // namespace

void VersionCheckerConfig::retrieveFromStorage( QSettings& settings )
{
    LOG_DEBUG << "VersionCheckerConfig::retrieveFromStorage";

    if ( settings.contains( "VersionChecker/nextDeadline" ) )
        next_deadline_ = settings.value( "VersionChecker/nextDeadline" ).toLongLong();
    if ( settings.contains( "VersionChecker/ignoredVersion" ) )
        ignored_version_ = settings.value( "VersionChecker/ignoredVersion" ).toString();
}

void VersionCheckerConfig::saveToStorage( QSettings& settings ) const
{
    LOG_DEBUG << "VersionCheckerConfig::saveToStorage";

    settings.setValue( "VersionChecker/nextDeadline", static_cast<long long>( next_deadline_ ) );
    settings.setValue( "VersionChecker/ignoredVersion", ignored_version_ );
}

VersionChecker::VersionChecker()
    : QObject()
{
}

void VersionChecker::startCheck()
{
    LOG_DEBUG << "VersionChecker::startCheck()";

    const auto& deadlineConfig = VersionCheckerConfig::getSynced();
    const auto& appConfig = Configuration::get();

    if ( !appConfig.versionCheckingEnabled() ) {
        return;
    }

    // Check the deadline has been reached
    if ( deadlineConfig.nextDeadline() < std::time( nullptr ) ) {
        LOG_DEBUG << "Requesting new version info from " << kReleaseApiUrl;

        QPointer<VersionChecker> guard( this );
        [[maybe_unused]] const auto versionCheckFuture = QtConcurrent::run( [ guard ] {
            QNetworkAccessManager mgr;
            mgr.setRedirectPolicy( QNetworkRequest::NoLessSafeRedirectPolicy );

            QNetworkRequest request;
            request.setUrl( QUrl( kReleaseApiUrl ) );

            QNetworkReply* reply = mgr.get( request );

            QEventLoop loop;
            QObject::connect( reply, &QNetworkReply::finished, &loop, &QEventLoop::quit );
            loop.exec();

            const bool hadError = ( reply->error() != QNetworkReply::NoError );
            const QByteArray data = hadError ? QByteArray() : reply->readAll();

            if ( hadError ) {
                LOG_WARNING << "Version check download failed: err " << reply->error();
            }

            reply->deleteLater();

            dispatchToMainThread( [ guard, data, hadError ] {
                if ( guard ) {
                    guard->processResponse( data, hadError, false );
                }
            } );
        } );
    }
    else {
        LOG_DEBUG << "Deadline not reached yet, next check in "
                  << std::difftime( deadlineConfig.nextDeadline(), std::time( nullptr ) );
    }
}

void VersionChecker::forceCheck()
{
    LOG_DEBUG << "VersionChecker::forceCheck()";

    const auto& appConfig = Configuration::get();

    if ( !appConfig.versionCheckingEnabled() ) {
        LOG_DEBUG << "Version checking is disabled";
        Q_EMIT checkCompleted( false );
        return;
    }

    QPointer<VersionChecker> guard( this );
    [[maybe_unused]] const auto versionCheckFuture = QtConcurrent::run( [ guard ] {
        QNetworkAccessManager mgr;
        mgr.setRedirectPolicy( QNetworkRequest::NoLessSafeRedirectPolicy );

        QNetworkRequest request;
        request.setUrl( QUrl( kReleaseApiUrl ) );

        QNetworkReply* reply = mgr.get( request );

        QEventLoop loop;
        QObject::connect( reply, &QNetworkReply::finished, &loop, &QEventLoop::quit );
        loop.exec();

        const bool hadError = ( reply->error() != QNetworkReply::NoError );
        const QByteArray data = hadError ? QByteArray() : reply->readAll();

        if ( hadError ) {
            LOG_WARNING << "Version check download failed: err " << reply->error();
        }

        reply->deleteLater();

        dispatchToMainThread( [ guard, data, hadError ] {
            if ( guard ) {
                guard->processResponse( data, hadError, true );
            }
        } );
    } );
}

void VersionChecker::processResponse( QByteArray data, bool hadError, bool wasManual )
{
    LOG_DEBUG << "VersionChecker::processResponse()";

    if ( !hadError ) {
        const bool foundNewer = checkVersionData( data );

        if ( !foundNewer && wasManual ) {
            Q_EMIT checkCompleted( false );
        }
    }
    else {
        if ( wasManual ) {
            Q_EMIT checkCompleted( false );
        }
    }

    // Extend the deadline, but only if no user-set reminder is still active
    auto& config = VersionCheckerConfig::get();

    const auto now = std::time( nullptr );
    if ( config.nextDeadline() <= now ) {
        config.setNextDeadline( now + CHECK_INTERVAL_S );
    }

    config.save();
}

bool VersionChecker::checkVersionData( QByteArray versionData )
{
    LOG_DEBUG << "Version reply: " << QString::fromUtf8( versionData );

    const auto releaseJson = QJsonDocument::fromJson( versionData );
    const auto releaseMap = releaseJson.toVariant().toMap();

    // The /releases/latest endpoint returns the latest non-prerelease, non-draft release.
    // tag_name is e.g. "v26.05.27.958" — strip the "v" prefix for version comparison.
    auto latestVersion = releaseMap.value( "tag_name" ).toString();
    if ( latestVersion.startsWith( QLatin1Char( 'v' ) ) ) {
        latestVersion = latestVersion.mid( 1 );
    }
    const auto url = releaseMap.value( "html_url" ).toString();
    const auto changelogBody = releaseMap.value( "body" ).toString();

    // Parse assets for the platform + architecture matched download URL
    const auto assetsList = releaseMap.value( "assets" ).toList();
    const auto downloadUrl = selectAssetDownloadUrl( assetsList );

    const auto currentVersion = kloggVersion();

    // Check if this version has been explicitly ignored
    const auto& deadlineConfig = VersionCheckerConfig::getSynced();
    if ( !deadlineConfig.ignoredVersion().isEmpty()
         && latestVersion == deadlineConfig.ignoredVersion() ) {
        LOG_DEBUG << "Version " << latestVersion << " is ignored, skipping notification";
        return false;
    }

    // Use the release body as a single changelog entry
    QStringList changes;
    if ( !changelogBody.isEmpty() ) {
        changes << changelogBody;
    }

    LOG_DEBUG << "Current version: " << currentVersion << ". Latest version is " << latestVersion
              << ", url " << url;
    if ( isVersionNewer( currentVersion, latestVersion ) ) {
        LOG_INFO << "Sending new version notification";

        Q_EMIT newVersionFound( latestVersion, url, downloadUrl, changes );
        return true;
    }
    return false;
}