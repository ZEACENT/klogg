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

#include "livedatastatistics.h"

#include <algorithm>
#include <limits>

namespace klogg::livecapture {
namespace {

void addSaturating( std::size_t& value, std::size_t increment ) noexcept
{
    const auto maximum = std::numeric_limits<std::size_t>::max();
    value = increment > maximum - value ? maximum : value + increment;
}

} // namespace

void recordLiveDataReceived( LiveDataStatistics& statistics, std::size_t byteCount,
                             std::size_t chunkCount ) noexcept
{
    addSaturating( statistics.receivedBytes, byteCount );
    addSaturating( statistics.receivedChunks, chunkCount );
}

void recordLiveDataDelivered( LiveDataStatistics& statistics, std::size_t byteCount,
                              std::size_t chunkCount ) noexcept
{
    addSaturating( statistics.deliveredBytes, byteCount );
    addSaturating( statistics.deliveredChunks, chunkCount );
}

void recordLiveDataBackpressure( LiveDataStatistics& statistics, std::size_t byteCount,
                                 std::size_t chunkCount ) noexcept
{
    addSaturating( statistics.backpressuredBytes, byteCount );
    addSaturating( statistics.backpressuredChunks, chunkCount );
}

void accumulateLiveDataStatistics( LiveDataStatistics& total,
                                   const LiveDataStatistics& increment ) noexcept
{
    if ( total.generation == 0u ) {
        total.generation = increment.generation;
    }
    addSaturating( total.receivedBytes, increment.receivedBytes );
    addSaturating( total.receivedChunks, increment.receivedChunks );
    addSaturating( total.queuedBytes, increment.queuedBytes );
    addSaturating( total.queuedChunks, increment.queuedChunks );
    addSaturating( total.deliveredBytes, increment.deliveredBytes );
    addSaturating( total.deliveredChunks, increment.deliveredChunks );
    addSaturating( total.backpressuredBytes, increment.backpressuredBytes );
    addSaturating( total.backpressuredChunks, increment.backpressuredChunks );
    total.highWaterQueuedBytes
        = std::max( total.highWaterQueuedBytes, increment.highWaterQueuedBytes );
    total.highWaterQueuedChunks
        = std::max( total.highWaterQueuedChunks, increment.highWaterQueuedChunks );
}

} // namespace klogg::livecapture
