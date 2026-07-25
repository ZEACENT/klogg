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

#include "predefinedfilterscombobox.h"

#include <QStandardItemModel>
#include <qabstractitemview.h>

#include "log.h"
#include "qtcompat/qtcompat.h"

constexpr int PatternRole = Qt::UserRole + 1;
constexpr int RegexRole = PatternRole + 1;

PredefinedFiltersComboBox::PredefinedFiltersComboBox( QWidget* parent )
    : QComboBox( parent )
    , model_( new QStandardItemModel(this) )
{
    setFocusPolicy( Qt::ClickFocus );
    populatePredefinedFilters();

    connect( this, QOverload<int>::of( &QComboBox::activated ), this,
             [ this ]( int ) { collectFilters(); } );

    QPalette palette = this->palette();
    palette.setColor( QPalette::Base, palette.color( QPalette::Window ) );
    view()->setPalette( palette );

    view()->setTextElideMode( Qt::ElideNone );
    setSizeAdjustPolicy( QComboBox::AdjustToContents );

klogg::qtcompat::setPlaceholderText( this, tr( "Filter favorites" ) );
}

void PredefinedFiltersComboBox::populatePredefinedFilters()
{
    model_->clear();
    const auto filters = filtersCollection_.getSyncedFilters();

    insertFilters( filters );

    this->setModel( model_ );
    setCurrentIndex( -1 );
}

void PredefinedFiltersComboBox::updateSearchPattern( const QString newSearchPattern, bool useLogicalCombining )
{
    Q_UNUSED( newSearchPattern );
    Q_UNUSED( useLogicalCombining );
    QSignalBlocker blocker( this );
    setCurrentIndex( -1 );
}

void PredefinedFiltersComboBox::insertFilters(
    const PredefinedFiltersCollection::Collection& filters )
{
    for ( const auto& filter : filters ) {
        auto* item = new QStandardItem( filter.name );

        item->setData( filter.pattern, PatternRole );
        item->setData( filter.useRegex, RegexRole );

        model_->insertRow( model_->rowCount(), item );
    }
}

void PredefinedFiltersComboBox::collectFilters()
{
    if ( ignoreCollecting_ ) {
        return;
    }

    QList<PredefinedFilter> selectedPatterns;
    const auto item = model_->item( currentIndex() );
    if ( !item || currentIndex() < 0 ) {
        return;
    }

    selectedPatterns.append(
        { item->text(), item->data( PatternRole ).toString(), item->data( RegexRole ).toBool() } );

    Q_EMIT filterChanged( selectedPatterns );

    QSignalBlocker blocker( this );
    setCurrentIndex( -1 );
}
