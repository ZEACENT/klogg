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

void FilterFavoritesModel::replaceFavorites( const Collection& favorites )
{
    if ( favorites_ != favorites ) {
        beginResetModel();
        favorites_ = favorites;
        endResetModel();
    }

    // The caller's collection is authoritative. Do not synchronize the model
    // away from it when another process changed storage; only inspect storage
    // and repair it when needed. An identical model+store remains a no-op.
    const auto storedFavorites = PredefinedFiltersCollection::getSynced().getFilters();
    if ( storedFavorites != favorites ) {
        PredefinedFiltersCollection::get().saveToStorage( favorites );
    }
}

void FilterFavoritesModel::synchronizeFromStorage()
{
    const auto storedFavorites = PredefinedFiltersCollection::getSynced().getFilters();
    if ( favorites_ == storedFavorites ) {
        return;
    }

    beginResetModel();
    favorites_ = storedFavorites;
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
