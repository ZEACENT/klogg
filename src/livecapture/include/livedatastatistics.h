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

#include <cstddef>

#include "livestate.h"

namespace klogg::livecapture {

struct LiveDataStatistics {
    Generation generation{ 0 };
    std::size_t receivedBytes{ 0 };
    std::size_t receivedChunks{ 0 };
    std::size_t queuedBytes{ 0 };
    std::size_t queuedChunks{ 0 };
    std::size_t deliveredBytes{ 0 };
    std::size_t deliveredChunks{ 0 };
    std::size_t backpressuredBytes{ 0 };
    std::size_t backpressuredChunks{ 0 };
    std::size_t highWaterQueuedBytes{ 0 };
    std::size_t highWaterQueuedChunks{ 0 };
};

void recordLiveDataReceived( LiveDataStatistics& statistics, std::size_t byteCount,
                             std::size_t chunkCount = 1u ) noexcept;
void recordLiveDataDelivered( LiveDataStatistics& statistics, std::size_t byteCount,
                              std::size_t chunkCount = 1u ) noexcept;
void recordLiveDataBackpressure( LiveDataStatistics& statistics, std::size_t byteCount,
                                 std::size_t chunkCount = 1u ) noexcept;
void accumulateLiveDataStatistics( LiveDataStatistics& total,
                                   const LiveDataStatistics& increment ) noexcept;

} // namespace klogg::livecapture
