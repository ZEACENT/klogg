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

#ifndef KLOGG_PLATFORM_INPUT_H
#define KLOGG_PLATFORM_INPUT_H

#include <Qt>

#include <QWheelEvent>

namespace klogg::platform {

// The platform-appropriate keyboard modifier for application-level shortcuts
// (tab switching, scratchpad navigation, etc.).
// macOS: MetaModifier (Cmd, HIG-conformant), others: ControlModifier.
#ifdef Q_OS_MACOS
constexpr auto PrimaryMod = Qt::MetaModifier;
#else
constexpr auto PrimaryMod = Qt::ControlModifier;
#endif

// The modifier key for font size changes via mouse wheel.
#ifdef Q_OS_MACOS
constexpr auto FontSizeMod = Qt::MetaModifier;
#else
constexpr auto FontSizeMod = Qt::ControlModifier;
#endif

// Human-readable shortcut modifier name for UI display.
#ifdef Q_OS_MACOS
constexpr auto shortcutModifierName = "Meta";
#else
constexpr auto shortcutModifierName = "Ctrl";
#endif

// Construct a wheel event, hiding the Qt version constructor split:
// Qt < 5.14 only has the qt4Delta/qt4Orientation overloads (not yet marked
// deprecated there), Qt >= 5.15 deprecates those (fatal with
// WARNINGS_AS_ERRORS) and Qt 6 removes them.
inline QWheelEvent makeWheelEvent( const QPointF& position, const QPointF& globalPosition,
                                   const QPoint& pixelDelta, const QPoint& angleDelta,
                                   Qt::ScrollPhase phase )
{
#if QT_VERSION >= QT_VERSION_CHECK( 5, 14, 0 )
    return { position, globalPosition, pixelDelta, angleDelta, Qt::NoButton, Qt::NoModifier,
             phase, false, Qt::MouseEventNotSynthesized };
#else
    return { position, globalPosition, pixelDelta, angleDelta, angleDelta.y(), Qt::Vertical,
             Qt::NoButton, Qt::NoModifier, phase };
#endif
}

} // namespace klogg::platform

#endif // KLOGG_PLATFORM_INPUT_H
