/*
 * Copyright (C) 2009, 2010 Nicolas Bonnefon and other contributors
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

#include "predefinedfilters.h"

#include <memory>

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QLockFile>
#include <QSettings>
#include <QStandardPaths>

#include "log.h"
#include "persistentinfo.h"

namespace {
constexpr auto SettingsGroup = "PredefinedFiltersCollection";
constexpr auto FiltersArray = "filters";
constexpr auto VersionKey = "version";
constexpr int LockTimeoutMs = 100;

std::unique_ptr<QSettings> makeTransactionSettings( const QSettings& settings )
{
    std::unique_ptr<QSettings> transactionSettings;
    if ( settings.organizationName().isEmpty() ) {
        transactionSettings = std::make_unique<QSettings>( settings.fileName(), settings.format() );
    }
    else {
        transactionSettings = std::make_unique<QSettings>(
            settings.format(), settings.scope(), settings.organizationName(),
            settings.applicationName() );
    }
    transactionSettings->setFallbacksEnabled( settings.fallbacksEnabled() );
    transactionSettings->setAtomicSyncRequired( settings.isAtomicSyncRequired() );
    return transactionSettings;
}

QSettings::SettingsMap snapshotSettingsGroup( QSettings& settings )
{
    QSettings::SettingsMap values;
    settings.beginGroup( QLatin1String( SettingsGroup ) );
    for ( const auto& key : settings.allKeys() ) {
        values.insert( key, settings.value( key ) );
    }
    settings.endGroup();
    return values;
}

QSettings::SettingsMap snapshotWritableSettingsGroup( QSettings& settings )
{
    const bool fallbacksEnabled = settings.fallbacksEnabled();
    settings.setFallbacksEnabled( false );
    const auto values = snapshotSettingsGroup( settings );
    settings.setFallbacksEnabled( fallbacksEnabled );
    return values;
}

void restoreWritableSettingsGroup( QSettings& settings,
                                   const QSettings::SettingsMap& values )
{
    settings.setFallbacksEnabled( false );
    settings.beginGroup( QLatin1String( SettingsGroup ) );
    settings.remove( QString{} );
    for ( auto it = values.cbegin(); it != values.cend(); ++it ) {
        settings.setValue( it.key(), it.value() );
    }
    settings.endGroup();
    settings.sync();
}

bool writableSettingsGroupMatches( const QSettings& identity,
                                   const QSettings::SettingsMap& expected )
{
    auto verificationSettings = makeTransactionSettings( identity );
    verificationSettings->setFallbacksEnabled( false );
    verificationSettings->sync();
    return verificationSettings->status() == QSettings::NoError
           && snapshotSettingsGroup( *verificationSettings ) == expected;
}

bool commitResultHasStoredFilters( PredefinedFiltersCollection::CommitStatus status )
{
    using CommitStatus = PredefinedFiltersCollection::CommitStatus;
    return status == CommitStatus::Success || status == CommitStatus::Unchanged
           || status == CommitStatus::Conflict || status == CommitStatus::WriteError;
}
} // namespace

PredefinedFiltersCollection::LoadResult PredefinedFiltersCollection::readFromSettings(
    QSettings& settings, bool missingIsSuccess )
{
    if ( settings.status() != QSettings::NoError ) {
        return { LoadStatus::MalformedFile, {} };
    }

    settings.beginGroup( QLatin1String( SettingsGroup ) );
    if ( !settings.contains( QLatin1String( VersionKey ) ) ) {
        settings.endGroup();
        return { missingIsSuccess ? LoadStatus::Success : LoadStatus::MalformedFile, {} };
    }

    bool versionIsValid = false;
    const int version = settings.value( QLatin1String( VersionKey ) ).toInt( &versionIsValid );
    if ( !versionIsValid || version < 1 ) {
        settings.endGroup();
        return { LoadStatus::MalformedFile, {} };
    }
    if ( version > PredefinedFiltersCollection_VERSION ) {
        settings.endGroup();
        return { LoadStatus::UnsupportedVersion, {} };
    }

    bool sizeIsValid = false;
    const auto sizeKey = QStringLiteral( "filters/size" );
    const int declaredSize = settings.value( sizeKey ).toInt( &sizeIsValid );
    if ( !settings.contains( sizeKey ) || !sizeIsValid || declaredSize < 0
         || declaredSize > MaximumFilterCount ) {
        settings.endGroup();
        return { LoadStatus::MalformedFile, {} };
    }

    const int size = settings.beginReadArray( QLatin1String( FiltersArray ) );
    if ( size != declaredSize ) {
        settings.endArray();
        settings.endGroup();
        return { LoadStatus::MalformedFile, {} };
    }

    Collection filters;
    filters.reserve( size );
    for ( int index = 0; index < size; ++index ) {
        settings.setArrayIndex( index );
        if ( !settings.contains( QStringLiteral( "name" ) )
             || !settings.contains( QStringLiteral( "filter" ) ) ) {
            settings.endArray();
            settings.endGroup();
            return { LoadStatus::MalformedFile, {} };
        }

        filters.push_back( { settings.value( QStringLiteral( "name" ) ).toString(),
                             settings.value( QStringLiteral( "filter" ) ).toString(),
                             settings.value( QStringLiteral( "regex" ), true ).toBool() } );
    }
    settings.endArray();
    settings.endGroup();

    if ( settings.status() != QSettings::NoError ) {
        return { LoadStatus::MalformedFile, {} };
    }
    return { LoadStatus::Success, filters };
}

void PredefinedFiltersCollection::retrieveFromStorage( QSettings& settings )
{
    LOG_DEBUG << "PredefinedFiltersCollection::retrieveFromStorage";

    const auto result = readFromSettings( settings, true );
    if ( result.status != LoadStatus::Success ) {
        LOG_ERROR << "Invalid PredefinedFiltersCollection settings, keeping the last valid state";
        return;
    }
    filters_ = result.filters;
}

void PredefinedFiltersCollection::saveToStorage( QSettings& settings ) const
{
    LOG_DEBUG << "PredefinedFiltersCollection::saveToStorage";

    if ( filters_.size() > MaximumFilterCount ) {
        LOG_ERROR << "Too many filter favorites to persist";
        return;
    }

    settings.beginGroup( QLatin1String( SettingsGroup ) );
    settings.setValue( QLatin1String( VersionKey ), PredefinedFiltersCollection_VERSION );
    settings.remove( QLatin1String( FiltersArray ) );

    settings.beginWriteArray( QLatin1String( FiltersArray ) );
    int arrayIndex = 0;
    for ( const auto& filter : filters_ ) {
        settings.setArrayIndex( arrayIndex );
        settings.setValue( QStringLiteral( "name" ), filter.name );
        settings.setValue( QStringLiteral( "filter" ), filter.pattern );
        settings.setValue( QStringLiteral( "regex" ), filter.useRegex );
        ++arrayIndex;
    }
    settings.endArray();
    settings.endGroup();
}

void PredefinedFiltersCollection::saveToStorage(
    const PredefinedFiltersCollection::Collection& filters )
{
    if ( filters.size() > MaximumFilterCount ) {
        LOG_ERROR << "Too many filter favorites to persist";
        return;
    }

    filters_ = filters;
    this->save();
}

PredefinedFiltersCollection::Collection PredefinedFiltersCollection::getFilters() const
{
    return filters_;
}

PredefinedFiltersCollection::Collection PredefinedFiltersCollection::getSyncedFilters()
{
    filters_ = this->getSynced().getFilters();
    return filters_;
}

void PredefinedFiltersCollection::setFilters( const Collection& filters )
{
    filters_ = filters;
}

PredefinedFiltersCollection::LoadResult PredefinedFiltersCollection::tryLoadFromFile(
    const QString& file )
{
    const QFileInfo fileInfo( file );
    if ( !fileInfo.exists() || !fileInfo.isFile() ) {
        return { LoadStatus::MissingFile, {} };
    }

    QSettings settings{ file, QSettings::IniFormat };
    settings.sync();
    if ( settings.status() != QSettings::NoError ) {
        return { LoadStatus::MalformedFile, {} };
    }
    return readFromSettings( settings, false );
}

PredefinedFiltersCollection::Collection PredefinedFiltersCollection::loadFromFile(
    const QString& file )
{
    const auto result = tryLoadFromFile( file );
    return result.status == LoadStatus::Success ? result.filters : Collection{};
}

bool PredefinedFiltersCollection::saveToFile(
    const QString& file, const PredefinedFiltersCollection::Collection& filters )
{
    if ( filters.size() > MaximumFilterCount ) {
        return false;
    }

    QSettings settings{ file, QSettings::IniFormat };
    PredefinedFiltersCollection collection;
    collection.setFilters( filters );
    collection.saveToStorage( settings );
    settings.sync();
    return settings.status() == QSettings::NoError;
}

QString PredefinedFiltersCollection::storageLockFilePath( const QSettings& settings )
{
    if ( settings.format() == QSettings::IniFormat && !settings.fileName().isEmpty() ) {
        return settings.fileName() + QStringLiteral( ".filter-favorites.lock" );
    }

    const auto identity = QCryptographicHash::hash( settings.fileName().toUtf8(),
                                                    QCryptographicHash::Sha256 )
                              .toHex();
    auto coordinationRoot
        = QStandardPaths::writableLocation( QStandardPaths::AppConfigLocation );
    if ( coordinationRoot.isEmpty() ) {
        coordinationRoot = QStandardPaths::writableLocation( QStandardPaths::TempLocation );
    }
    const QDir configDirectory{ coordinationRoot };
    return configDirectory.filePath(
        QStringLiteral( "locks/filter-favorites-%1.lock" ).arg( QString::fromLatin1( identity ) ) );
}

PredefinedFiltersCollection::CommitResult PredefinedFiltersCollection::commitToSettings(
    const QSettings& settings, const QString& lockFile, const Collection& expected,
    const Collection& replacement )
{
    if ( replacement.size() > MaximumFilterCount ) {
        return { CommitStatus::InvalidReplacement, {} };
    }

    const QFileInfo lockInfo{ lockFile };
    if ( !QDir{}.mkpath( lockInfo.absolutePath() ) ) {
        return { CommitStatus::LockError, {} };
    }

    QLockFile lock{ lockFile };
    if ( !lock.tryLock( LockTimeoutMs ) ) {
        return { CommitStatus::LockError, {} };
    }

    auto transactionSettings = makeTransactionSettings( settings );
    transactionSettings->sync();
    if ( transactionSettings->status() != QSettings::NoError ) {
        return { CommitStatus::StorageError, {} };
    }

    const auto storedValues = snapshotWritableSettingsGroup( *transactionSettings );
    const auto current = readFromSettings( *transactionSettings, true );
    const bool repairingMalformedStorage = current.status == LoadStatus::MalformedFile;
    if ( current.status == LoadStatus::UnsupportedVersion ) {
        return { CommitStatus::StorageError, {} };
    }
    if ( !repairingMalformedStorage && current.status != LoadStatus::Success ) {
        return { CommitStatus::StorageError, {} };
    }
    if ( !repairingMalformedStorage && current.filters != expected ) {
        return { CommitStatus::Conflict, current.filters };
    }
    if ( !repairingMalformedStorage && current.filters == replacement ) {
        return { CommitStatus::Unchanged, current.filters };
    }

    PredefinedFiltersCollection replacementCollection;
    replacementCollection.setFilters( replacement );
    replacementCollection.saveToStorage( *transactionSettings );
    transactionSettings->sync();
    if ( transactionSettings->status() != QSettings::NoError ) {
        restoreWritableSettingsGroup( *transactionSettings, storedValues );
        transactionSettings.reset();
        if ( !writableSettingsGroupMatches( settings, storedValues )
             || repairingMalformedStorage ) {
            return { CommitStatus::StorageError, {} };
        }
        return { CommitStatus::WriteError, current.filters };
    }

    return { CommitStatus::Success, replacement };
}

PredefinedFiltersCollection::CommitResult PredefinedFiltersCollection::commitUsingSettings(
    QSettings& sharedSettings, const Collection& expected, const Collection& replacement )
{
    const bool sharedSettingsCanRefresh = sharedSettings.status() == QSettings::NoError;
    const auto result = commitToSettings( sharedSettings, storageLockFilePath( sharedSettings ),
                                          expected, replacement );
    if ( sharedSettingsCanRefresh && commitResultHasStoredFilters( result.status ) ) {
        sharedSettings.sync();
    }
    return result;
}

PredefinedFiltersCollection::CommitResult PredefinedFiltersCollection::commit(
    const Collection& expected, const Collection& replacement )
{
    const auto result = commitUsingSettings( PersistentInfo::getSettings( app_settings{} ), expected,
                                             replacement );
    if ( result.status == CommitStatus::Success || result.status == CommitStatus::Unchanged
         || result.status == CommitStatus::Conflict || result.status == CommitStatus::WriteError ) {
        PredefinedFiltersCollection::get().setFilters( result.storedFilters );
    }
    return result;
}
