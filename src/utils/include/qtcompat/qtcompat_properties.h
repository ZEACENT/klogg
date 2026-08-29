/*
 * Copyright (C) 2024 Anton Filimonov and other contributors
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

#ifndef KLOGG_QTCOMPAT_PROPERTIES_H
#define KLOGG_QTCOMPAT_PROPERTIES_H

#include <QApplication>
#include <QColor>
#include <QComboBox>
#include <QFontDatabase>
#include <QScreen>
#include <QStyleHints>
#include <QTabBar>
#include <QWidget>
#include <QWindow>

// Qt API compat wrappers.  Each function encapsulates a Qt-version branch;
// call sites include this header and use the wrapper instead of writing an
// inline #if QT_VERSION block.

namespace klogg::qtcompat {

// --- QFontDatabase ----------------------------------------------------------

inline QStringList fontFamilies()
{
#if QT_VERSION < QT_VERSION_CHECK( 6, 0, 0 )
    return QFontDatabase().families();
#else
    return QFontDatabase::families();
#endif
}

inline bool isFixedPitch( const QString& family )
{
#if QT_VERSION < QT_VERSION_CHECK( 6, 0, 0 )
    return QFontDatabase().isFixedPitch( family );
#else
    return QFontDatabase::isFixedPitch( family );
#endif
}

inline QList<int> pointSizes( const QString& family, const QString& style = {} )
{
#if QT_VERSION < QT_VERSION_CHECK( 6, 0, 0 )
    return QFontDatabase().pointSizes( family, style );
#else
    return QFontDatabase::pointSizes( family, style );
#endif
}

// --- QWidget::screen --------------------------------------------------------

inline QScreen* widgetScreen( QWidget* widget )
{
    if ( !widget )
        return nullptr;
#if QT_VERSION >= QT_VERSION_CHECK( 5, 14, 0 )
    return widget->screen();
#else
    (void)widget->winId(); // force native window creation
    QWindow* window = widget->windowHandle();
    return window ? window->screen() : nullptr;
#endif
}

// --- QColor -----------------------------------------------------------------

inline QColor colorFromString( const QString& name )
{
#if QT_VERSION <= QT_VERSION_CHECK( 6, 4, 0 )
    QColor color;
    color.setNamedColor( name );
    return color;
#else
    return QColor::fromString( name );
#endif
}

// --- QPalette / Dark mode ---------------------------------------------------

inline bool isDarkColorScheme()
{
#if QT_VERSION >= QT_VERSION_CHECK( 6, 5, 0 )
    return QApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
#else
    return false;
#endif
}

// --- QComboBox --------------------------------------------------------------

inline void setPlaceholderText( QComboBox* combo, const QString& text )
{
#if QT_VERSION >= QT_VERSION_CHECK( 5, 15, 0 )
    combo->setPlaceholderText( text );
#else
    Q_UNUSED( combo );
    Q_UNUSED( text );
#endif
}

// --- QTabBar ----------------------------------------------------------------

inline int tabBarTabAt( const QTabBar* bar, const QPoint& pos )
{
#if QT_VERSION >= QT_VERSION_CHECK( 5, 15, 0 )
    return bar->tabAt( pos );
#else
    for ( int i = 0; i < bar->count(); ++i ) {
        if ( bar->tabRect( i ).contains( pos ) )
            return i;
    }
    return -1;
#endif
}

inline void setTabVisible( QTabBar* bar, int index, bool visible )
{
#if QT_VERSION >= QT_VERSION_CHECK( 5, 15, 0 )
    bar->setTabVisible( index, visible );
#else
    bar->setTabEnabled( index, visible );
#endif
}

inline bool isTabVisible( const QTabBar* bar, int index )
{
#if QT_VERSION >= QT_VERSION_CHECK( 5, 15, 0 )
    return bar->isTabVisible( index );
#else
    return bar->isTabEnabled( index );
#endif
}

// --- QString::split ---------------------------------------------------------

#if QT_VERSION >= QT_VERSION_CHECK( 5, 14, 0 )
inline Qt::SplitBehaviorFlags skipEmptyParts()
#else
inline QString::SplitBehavior skipEmptyParts()
#endif
{
#if QT_VERSION >= QT_VERSION_CHECK( 5, 15, 0 )
    return Qt::SkipEmptyParts; // lint-allow: platform-fragile
#else
    return QString::SkipEmptyParts;
#endif
}

} // namespace klogg::qtcompat

#endif // KLOGG_QTCOMPAT_PROPERTIES_H
