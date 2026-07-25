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

#ifndef KLOGG_PLATFORM_STYLE_H
#define KLOGG_PLATFORM_STYLE_H

#include <QFont>
#include <QString>

namespace klogg::platform {

// Default Qt style key for each platform.
inline QString defaultStyleKey()
{
#ifdef Q_OS_WIN
    return QStringLiteral( "Vista" );
#elif defined( Q_OS_MACOS )
    return QStringLiteral( "Macintosh" );
#else
    return QStringLiteral( "Fusion" );
#endif
}

// Auto-theme dark style key.
inline QString darkStyleKey()
{
#ifdef Q_OS_WIN
    return QStringLiteral( "DarkWindows" );
#else
    return QStringLiteral( "Dark" );
#endif
}

// Default main font. macOS uses a larger point size (72 DPI reference) to
// match the visual size on Windows/Linux (96 DPI reference).
inline QFont defaultMainFont()
{
#ifdef Q_OS_MACOS
    return QFont( QStringLiteral( "DejaVu Sans Mono" ), 13 );
#else
    return QFont( QStringLiteral( "DejaVu Sans Mono" ), 10 );
#endif
}

} // namespace klogg::platform

#endif // KLOGG_PLATFORM_STYLE_H
