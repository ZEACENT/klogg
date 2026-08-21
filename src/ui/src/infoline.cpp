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
 * Copyright (C) 2016 -- 2019 Anton Filimonov and other contributors
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

#include "containers.h"

#include "infoline.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QMenu>
#include <QPainter>

#include "clipboard.h"

InfoLine::InfoLine()
{
    setSizePolicy( QSizePolicy::Expanding, QSizePolicy::Fixed );
    setTextInteractionFlags( Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard );
}

// A QLabel without wordWrap reports minimumSizeHint == sizeHint -- the full
// text width. Hosted in a QToolBar (the main window's path line), any text
// wider than the toolbar's free space then overflows the whole label into the
// toolbar's extension popup: the label is HIDDEN and the path bar renders
// blank (field repro: long non-ASCII paths from folder-search result clicks).
// Only the height is load-bearing for the toolbar row; the width may shrink
// freely so QToolBarLayout always keeps the label on-screen, clipping the
// text instead of hiding it. Same physics applies to the CrawlerWidget search
// info line hosted in an QHBoxLayout: a full-width minimum there forces the
// search line's minimum width to the text width.
QSize InfoLine::minimumSizeHint() const
{
    QSize hint = QLabel::minimumSizeHint();
    hint.setWidth( 0 );
    return hint;
}

void InfoLine::displayGauge( int completion )
{
    if ( !origPalette_ ) {
        origPalette_ = palette();
    }

    int changeoverX = width() * completion / 100;

    // Create a gradient for the progress bar
    QLinearGradient linearGrad( changeoverX - 1, 0, changeoverX + 1, 0 );
    linearGrad.setColorAt( 0, origPalette_->color( QPalette::Highlight ) );
    linearGrad.setColorAt( 1, origPalette_->color( QPalette::Window ) );

    // Apply the gradient to the current palette (background)
    QPalette newPalette = *origPalette_;
    newPalette.setBrush( backgroundRole(), QBrush( linearGrad ) );
    setPalette( newPalette );
}

void InfoLine::hideGauge()
{
    if ( origPalette_ ) {
        setPalette( *origPalette_ );
    }
    origPalette_.reset();
}

// Custom painter: draw the background then call QLabel's painter
void InfoLine::paintEvent( QPaintEvent* paintEvent )
{
    // Fill the widget background
    {
        QPainter painter( this );
        painter.fillRect( 0, 0, this->width(), this->height(),
                          palette().brush( backgroundRole() ) );
    }

    // Call the parent's painter
    QLabel::paintEvent( paintEvent );
}

void InfoLine::contextMenuEvent( QContextMenuEvent* event )
{
    QMenu menu( this );

    auto copySelection = menu.addAction( "Copy" );
    menu.addSeparator();
    auto selectAll = menu.addAction( "Select all" );

    copySelection->setEnabled( this->hasSelectedText() );
    connect( copySelection, &QAction::triggered, this,
             [ this ]( auto ) { sendTextToClipboard( this->selectedText() ); } );

    connect( selectAll, &QAction::triggered, this,
             [ this ]( auto ) { setSelection( 0, klogg::isize( this->text() ) ); } );

    menu.exec( event->globalPos() );
}
