/*
 * Copyright (C) 2026 Anton Filimonov and other contributors
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

#include "filterfavoritesmodel.h"

#include "containers.h"

FilterFavoritesModel& FilterFavoritesModel::instance()
{
    static FilterFavoritesModel model;
    return model;
}

FilterFavoritesModel::FilterFavoritesModel( QObject* parent )
    : QAbstractListModel( parent )
    , favorites_( PredefinedFiltersCollection::getSynced().getFilters() )
{
}

FilterFavoritesModel::Collection FilterFavoritesModel::favorites() const
{
    return favorites_;
}

FilterFavoritesModel::CommitResult FilterFavoritesModel::replaceFavorites(
    const Collection& favorites )
{
    return replaceFavorites( favorites_, favorites );
}

FilterFavoritesModel::CommitResult FilterFavoritesModel::replaceFavorites(
    const Collection& expected, const Collection& favorites )
{
    const auto result = PredefinedFiltersCollection::commit( expected, favorites );
    switch ( result.status ) {
    case PredefinedFiltersCollection::CommitStatus::Success:
    case PredefinedFiltersCollection::CommitStatus::Unchanged:
    case PredefinedFiltersCollection::CommitStatus::Conflict:
    case PredefinedFiltersCollection::CommitStatus::WriteError:
        publishFavorites( result.storedFilters );
        break;
    case PredefinedFiltersCollection::CommitStatus::InvalidReplacement:
    case PredefinedFiltersCollection::CommitStatus::LockError:
    case PredefinedFiltersCollection::CommitStatus::StorageError:
        break;
    }
    return result;
}

void FilterFavoritesModel::synchronizeFromStorage()
{
    publishFavorites( PredefinedFiltersCollection::getSynced().getFilters() );
}

void FilterFavoritesModel::publishFavorites( const Collection& favorites )
{
    if ( favorites_ == favorites ) {
        return;
    }

    beginResetModel();
    favorites_ = favorites;
    endResetModel();
}

int FilterFavoritesModel::rowCount( const QModelIndex& parent ) const
{
    return parent.isValid() ? 0 : klogg::isize( favorites_ );
}

QVariant FilterFavoritesModel::data( const QModelIndex& index, int role ) const
{
    if ( !index.isValid() || index.column() != 0 || index.row() < 0
         || index.row() >= klogg::isize( favorites_ ) ) {
        return {};
    }

    const auto& favorite = favorites_.at( index.row() );
    switch ( role ) {
    case Qt::DisplayRole:
    case Qt::ToolTipRole:
    case NameRole:
        return favorite.name;
    case PatternRole:
        return favorite.pattern;
    case RegexRole:
        return favorite.useRegex;
    default:
        return {};
    }
}

QHash<int, QByteArray> FilterFavoritesModel::roleNames() const
{
    auto roles = QAbstractListModel::roleNames();
    roles.insert( NameRole, QByteArrayLiteral( "name" ) );
    roles.insert( PatternRole, QByteArrayLiteral( "pattern" ) );
    roles.insert( RegexRole, QByteArrayLiteral( "regex" ) );
    return roles;
}
