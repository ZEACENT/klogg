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

#ifndef KLOGG_PLATFORM_PROCESS_H
#define KLOGG_PLATFORM_PROCESS_H

#include <chrono>

namespace klogg::platform {

// Startup failure grace period: Windows processes need longer to start.
// Returns the interval in milliseconds before retrying after a failed start.
constexpr std::chrono::milliseconds startupFailureGracePeriod()
{
#ifdef Q_OS_WIN
    return std::chrono::milliseconds{ 1000 };
#else
    return std::chrono::milliseconds{ 250 };
#endif
}

} // namespace klogg::platform

#endif // KLOGG_PLATFORM_PROCESS_H
