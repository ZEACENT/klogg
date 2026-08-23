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

#include <QAbstractItemView>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QScreen>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QStyleOptionComboBox>
#include <QStyleOptionViewItem>

#include <algorithm>

#include "filterfavoritesmodel.h"
#include "qtcompat/qtcompat.h"

namespace {
constexpr int MaximumClosedWidthInEms = 24;
constexpr int PopupTextHorizontalPadding = 12;

class FullTextItemDelegate final : public QStyledItemDelegate {
  public:
    explicit FullTextItemDelegate( QObject* parent )
        : QStyledItemDelegate( parent )
    {
    }

    QSize sizeHint( const QStyleOptionViewItem& option,
                    const QModelIndex& index ) const override
    {
        QStyleOptionViewItem textOption( option );
        initStyleOption( &textOption, index );

        auto hint = QStyledItemDelegate::sizeHint( option, index );
        const int fullTextWidth
            = QFontMetrics( textOption.font ).horizontalAdvance( textOption.text );
        hint.setWidth( std::max( hint.width(), fullTextWidth + PopupTextHorizontalPadding ) );
        return hint;
    }
};
}

PredefinedFiltersComboBox::PredefinedFiltersComboBox( QWidget* parent )
    : QComboBox( parent )
{
    setFocusPolicy( Qt::ClickFocus );
    setSizeAdjustPolicy( QComboBox::AdjustToMinimumContentsLengthWithIcon );
    setSizePolicy( QSizePolicy::Fixed, QSizePolicy::Minimum );

    auto& favoritesModel = FilterFavoritesModel::instance();
    favoritesModel.synchronizeFromStorage();
    setModel( &favoritesModel );
    // QAbstractItemView takes ownership of the delegate.
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
    view()->setItemDelegate( new FullTextItemDelegate( view() ) );

    connect( this, QOverload<int>::of( &QComboBox::activated ), this,
             &PredefinedFiltersComboBox::collectFilter );
    connect( model(), &QAbstractItemModel::modelReset, this,
             &PredefinedFiltersComboBox::resetSelection );

    QPalette popupPalette = palette();
    popupPalette.setColor( QPalette::Base, popupPalette.color( QPalette::Window ) );
    view()->setPalette( popupPalette );
    view()->setTextElideMode( Qt::ElideNone );
    view()->setHorizontalScrollBarPolicy( Qt::ScrollBarAsNeeded );
    view()->setHorizontalScrollMode( QAbstractItemView::ScrollPerPixel );

    klogg::qtcompat::setPlaceholderText( this, tr( "Filter favorites" ) );
    resetSelection();
}

void PredefinedFiltersComboBox::updateSearchPattern( const QString newSearchPattern,
                                                     bool useLogicalCombining )
{
    Q_UNUSED( newSearchPattern );
    Q_UNUSED( useLogicalCombining );
    resetSelection();
}

QSize PredefinedFiltersComboBox::sizeHint() const
{
    return closedSizeHint();
}

QSize PredefinedFiltersComboBox::minimumSizeHint() const
{
    return closedSizeHint();
}

void PredefinedFiltersComboBox::showPopup()
{
    FilterFavoritesModel::instance().synchronizeFromStorage();

    view()->setTextElideMode( Qt::ElideNone );
    view()->setHorizontalScrollBarPolicy( Qt::ScrollBarAsNeeded );
    view()->setHorizontalScrollMode( QAbstractItemView::ScrollPerPixel );

    QStyleOptionViewItem itemOption;
    itemOption.initFrom( view() );
    itemOption.font = view()->font();

    int contentWidth = 0;
    for ( int row = 0; row < model()->rowCount( rootModelIndex() ); ++row ) {
        const auto index = model()->index( row, modelColumn(), rootModelIndex() );
        if ( index.isValid() ) {
            contentWidth
                = std::max( contentWidth,
                            view()->itemDelegate()->sizeHint( itemOption, index ).width() );
        }
    }

    const int frameWidth = style()->pixelMetric( QStyle::PM_DefaultFrameWidth, nullptr, this );
    const int scrollbarWidth
        = count() > maxVisibleItems()
            ? style()->pixelMetric( QStyle::PM_ScrollBarExtent, nullptr, view() )
            : 0;
    const int desiredWidth
        = std::max( width(), contentWidth + scrollbarWidth + 2 * frameWidth );

    const QPoint popupAnchor = mapToGlobal( rect().center() );
    QScreen* screen = QGuiApplication::screenAt( popupAnchor );
    if ( screen == nullptr ) {
        screen = QGuiApplication::primaryScreen();
    }

    QComboBox::showPopup();

    QWidget* const popup = view()->window();
    if ( popup == nullptr ) {
        return;
    }

    const auto updateHorizontalScrollRange = [ this, contentWidth ] {
        view()->doItemsLayout();
        auto* const scrollBar = view()->horizontalScrollBar();
        const int viewportWidth = view()->viewport()->width();
        scrollBar->setPageStep( viewportWidth );
        scrollBar->setRange( 0, std::max( 0, contentWidth - viewportWidth ) );
    };

    if ( screen == nullptr ) {
        popup->resize( desiredWidth, popup->height() );
        updateHorizontalScrollRange();
        return;
    }

    const QRect available = screen->availableGeometry();
    const int popupWidth = std::min( desiredWidth, available.width() );
    const int popupHeight = std::min( popup->height(), available.height() );
    QRect popupGeometry = popup->geometry();
    popupGeometry.setSize( QSize( popupWidth, popupHeight ) );
    popupGeometry.moveLeft( std::max(
        available.left(),
        std::min( popupGeometry.left(), available.right() - popupWidth + 1 ) ) );
    popupGeometry.moveTop( std::max(
        available.top(),
        std::min( popupGeometry.top(), available.bottom() - popupHeight + 1 ) ) );
    popup->setGeometry( popupGeometry );
    updateHorizontalScrollRange();
}

QSize PredefinedFiltersComboBox::closedSizeHint() const
{
    const QString placeholder = tr( "Filter favorites" );
    const QFontMetrics metrics( font() );
    const int maximumTextWidth
        = metrics.horizontalAdvance( QString( MaximumClosedWidthInEms, QLatin1Char( 'M' ) ) );
    const QSize textSize( std::min( metrics.horizontalAdvance( placeholder ), maximumTextWidth ),
                          metrics.height() );

    QStyleOptionComboBox option;
    initStyleOption( &option );
    option.currentText = placeholder;
    option.currentIcon = {};
    return style()->sizeFromContents( QStyle::CT_ComboBox, &option, textSize, this );
}

void PredefinedFiltersComboBox::collectFilter( int index )
{
    const QModelIndex modelIndex = model()->index( index, modelColumn(), rootModelIndex() );
    if ( modelIndex.isValid() ) {
        QList<PredefinedFilter> selectedFilters;
        selectedFilters.append(
            { model()->data( modelIndex, FilterFavoritesModel::NameRole ).toString(),
              model()->data( modelIndex, FilterFavoritesModel::PatternRole ).toString(),
              model()->data( modelIndex, FilterFavoritesModel::RegexRole ).toBool() } );
        Q_EMIT filterChanged( selectedFilters );
    }

    resetSelection();
}

void PredefinedFiltersComboBox::resetSelection()
{
    QSignalBlocker blocker( this );
    setCurrentIndex( -1 );
}
