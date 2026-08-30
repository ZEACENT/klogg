/*
 * Copyright (C) 2026
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

#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "livestate.h"

namespace klogg::livecapture::ios {

enum class IosNativeErrorDomain : std::uint8_t { Idevice, Lockdown, Service, OsTrace, SyslogRelay };

struct IosNativeError {
    IosNativeErrorDomain domain{ IosNativeErrorDomain::Idevice };
    std::int32_t code{ 0 };
    std::string detail;
};

struct ClassifiedIosNativeError {
    LiveSourceError error;
    std::optional<AwaitingUserReason> awaitingUserReason;
};

ClassifiedIosNativeError classifyIosNativeError( const IosNativeError& nativeError );

} // namespace klogg::livecapture::ios
