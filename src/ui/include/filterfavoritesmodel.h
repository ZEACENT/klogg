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

#ifndef FILTERFAVORITESMODEL_H
#define FILTERFAVORITESMODEL_H

#include <QAbstractListModel>
#include <QByteArray>
#include <QHash>
#include <QVariant>

#include "predefinedfilters.h"

class FilterFavoritesModel final : public QAbstractListModel {
    Q_OBJECT

  public:
    using Collection = PredefinedFiltersCollection::Collection;

    enum Roles {
        NameRole = Qt::UserRole + 1,
        PatternRole,
        RegexRole,
    };

    static FilterFavoritesModel& instance();

    Collection favorites() const;
    void replaceFavorites( const Collection& favorites );
    void synchronizeFromStorage();

    int rowCount( const QModelIndex& parent = QModelIndex{} ) const override;
    QVariant data( const QModelIndex& index, int role = Qt::DisplayRole ) const override;
    QHash<int, QByteArray> roleNames() const override;

  private:
    explicit FilterFavoritesModel( QObject* parent = nullptr );

    Collection favorites_;
};

#endif // FILTERFAVORITESMODEL_H
