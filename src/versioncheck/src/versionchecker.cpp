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

} // namespace

void VersionCheckerConfig::retrieveFromStorage( QSettings& settings )
{
    LOG_DEBUG << "VersionCheckerConfig::retrieveFromStorage";

    if ( settings.contains( "VersionChecker/nextDeadline" ) )
        next_deadline_ = settings.value( "VersionChecker/nextDeadline" ).toLongLong();
}

void VersionCheckerConfig::saveToStorage( QSettings& settings ) const
{
    LOG_DEBUG << "VersionCheckerConfig::saveToStorage";

    settings.setValue( "VersionChecker/nextDeadline", static_cast<long long>( next_deadline_ ) );
}

VersionChecker::VersionChecker()
    : QObject()
    , manager_( new QNetworkAccessManager( this ) )
{
    manager_->setRedirectPolicy( QNetworkRequest::NoLessSafeRedirectPolicy );
}

void VersionChecker::startCheck()
{
    LOG_DEBUG << "VersionChecker::startCheck()";

    const auto& deadlineConfig = VersionCheckerConfig::getSynced();
    const auto& appConfig = Configuration::get();

    if ( appConfig.versionCheckingEnabled() ) {
        // Check the deadline has been reached
        if ( deadlineConfig.nextDeadline() < std::time( nullptr ) ) {
            connect( manager_, &QNetworkAccessManager::finished, this,
                     &VersionChecker::downloadFinished );

            LOG_DEBUG << "Requesting new version info from " << kReleaseApiUrl;

            QNetworkRequest request;
            request.setUrl( QUrl( kReleaseApiUrl ) );
            manager_->get( request );
        }
        else {
            LOG_DEBUG << "Deadline not reached yet, next check in "
                      << std::difftime( deadlineConfig.nextDeadline(), std::time( nullptr ) );
        }
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

    isManualCheck_ = true;

    connect( manager_, &QNetworkAccessManager::finished, this,
             &VersionChecker::downloadFinished );

    LOG_DEBUG << "Requesting new version info from " << kReleaseApiUrl;

    QNetworkRequest request;
    request.setUrl( QUrl( kReleaseApiUrl ) );
    manager_->get( request );
}

void VersionChecker::downloadFinished( QNetworkReply* reply )
{
    LOG_DEBUG << "VersionChecker::downloadFinished()";

    const bool wasManual = isManualCheck_;
    isManualCheck_ = false;

    if ( reply->error() == QNetworkReply::NoError ) {
        const auto rawReply = reply->readAll();
        const bool foundNewer = checkVersionData( rawReply );

        if ( !foundNewer && wasManual ) {
            Q_EMIT checkCompleted( false );
        }
    }
    else {
        LOG_WARNING << "Download failed: err " << reply->error();
        if ( wasManual ) {
            Q_EMIT checkCompleted( false );
        }
    }

    reply->deleteLater();

    // Extend the deadline
    auto& config = VersionCheckerConfig::get();

    config.setNextDeadline( std::time( nullptr ) + CHECK_INTERVAL_S );

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

    const auto currentVersion = kloggVersion();

    // Use the release body as a single changelog entry
    QStringList changes;
    if ( !changelogBody.isEmpty() ) {
        changes << changelogBody;
    }

    LOG_DEBUG << "Current version: " << currentVersion << ". Latest version is " << latestVersion
              << ", url " << url;
    if ( isVersionNewer( currentVersion, latestVersion ) ) {
        LOG_INFO << "Sending new version notification";

        Q_EMIT newVersionFound( latestVersion, url, changes );
        return true;
    }
    return false;
}