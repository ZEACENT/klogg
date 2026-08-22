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

#include <QFileInfo>
#include <QSettings>

#include "log.h"

void PredefinedFiltersCollection::retrieveFromStorage( QSettings& settings )
{
    LOG_DEBUG << "PredefinedFiltersCollection::retrieveFromStorage";

    filters_.clear();
    if ( !settings.contains( "PredefinedFiltersCollection/version" ) ) {
        return;
    }

    settings.beginGroup( "PredefinedFiltersCollection" );
    bool versionIsValid = false;
    const int version = settings.value( "version" ).toInt( &versionIsValid );
    if ( !versionIsValid || version < 1 ) {
        LOG_ERROR << "Invalid version of PredefinedFiltersCollection, ignoring it...";
        settings.endGroup();
        return;
    }
    if ( version > PredefinedFiltersCollection_VERSION ) {
        LOG_ERROR << "Unknown version of PredefinedFiltersCollection, ignoring it...";
        settings.endGroup();
        return;
    }

    const int size = settings.beginReadArray( "filters" );
    filters_.reserve( size );
    for ( int i = 0; i < size; ++i ) {
        settings.setArrayIndex( i );

        filters_.push_back( { settings.value( "name" ).toString(),
                              settings.value( "filter" ).toString(),
                              settings.value( "regex", true ).toBool() } );
    }
    settings.endArray();
    settings.endGroup();
}

void PredefinedFiltersCollection::saveToStorage( QSettings& settings ) const
{
    LOG_DEBUG << "PredefinedFiltersCollection::saveToStorage";

    settings.beginGroup( "PredefinedFiltersCollection" );
    settings.setValue( "version", PredefinedFiltersCollection_VERSION );

    settings.remove( "filters" );

    settings.beginWriteArray( "filters" );
    int arrayIndex = 0;
    for ( const auto& filter : filters_ ) {
        settings.setArrayIndex( arrayIndex );
        settings.setValue( "name", filter.name );
        settings.setValue( "filter", filter.pattern );
        settings.setValue( "regex", filter.useRegex );

        arrayIndex++;
    }
    settings.endArray();
    settings.endGroup();
}

void PredefinedFiltersCollection::saveToStorage(
    const PredefinedFiltersCollection::Collection& filters )
{
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

    settings.beginGroup( QStringLiteral( "PredefinedFiltersCollection" ) );
    if ( !settings.contains( QStringLiteral( "version" ) ) ) {
        settings.endGroup();
        return { LoadStatus::MalformedFile, {} };
    }

    bool versionValid = false;
    const int version = settings.value( QStringLiteral( "version" ) ).toInt( &versionValid );
    if ( !versionValid || version <= 0 ) {
        settings.endGroup();
        return { LoadStatus::MalformedFile, {} };
    }
    if ( version > PredefinedFiltersCollection_VERSION ) {
        settings.endGroup();
        return { LoadStatus::UnsupportedVersion, {} };
    }

    bool sizeValid = false;
    const int declaredSize
        = settings.value( QStringLiteral( "filters/size" ) ).toInt( &sizeValid );
    if ( !settings.contains( QStringLiteral( "filters/size" ) ) || !sizeValid
         || declaredSize < 0 ) {
        settings.endGroup();
        return { LoadStatus::MalformedFile, {} };
    }

    Collection filters;
    const int size = settings.beginReadArray( QStringLiteral( "filters" ) );
    if ( size != declaredSize ) {
        settings.endArray();
        settings.endGroup();
        return { LoadStatus::MalformedFile, {} };
    }

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

PredefinedFiltersCollection::Collection PredefinedFiltersCollection::loadFromFile(
    const QString& file )
{
    const auto result = tryLoadFromFile( file );
    return result.status == LoadStatus::Success ? result.filters : Collection{};
}

bool PredefinedFiltersCollection::saveToFile(
    const QString& file, const PredefinedFiltersCollection::Collection& filters )
{
    QSettings settings{ file, QSettings::IniFormat };
    PredefinedFiltersCollection collection;
    collection.setFilters( filters );
    collection.saveToStorage( settings );
    settings.sync();
    return settings.status() == QSettings::NoError;
}
