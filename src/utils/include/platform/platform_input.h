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

} // namespace klogg::platform

#endif // KLOGG_PLATFORM_INPUT_H
