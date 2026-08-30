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

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include "livedatastatistics.h"
#include "livestate.h"

namespace klogg::livecapture {

struct LiveDataChunk {
    Generation generation{ 0 };
    std::vector<std::uint8_t> bytes;
};

struct LiveDataQueueLimits {
    std::size_t maxQueuedBytes{ 0 };
    std::size_t maxQueuedChunks{ 0 };
};

enum class LiveDataEnqueueResult : std::uint8_t { Accepted, Backpressure, StaleGeneration, Closed };

struct LiveDataBatch {
    Generation generation{ 0 };
    std::vector<std::uint8_t> bytes;
    std::size_t sourceChunks{ 0 };
};

struct LiveDataQueueReset {
    Generation retiredGeneration{ 0 };
    std::size_t retiredBytes{ 0 };
    std::size_t retiredChunks{ 0 };
};

class LiveDataQueue {
public:
    using DrainNotification = std::function<void()>;

    LiveDataQueue( LiveDataQueueLimits limits, Generation generation,
                   DrainNotification drainNotification );

    LiveDataEnqueueResult tryEnqueue( const LiveDataChunk& chunk );
    LiveDataEnqueueResult enqueueWait( const LiveDataChunk& chunk );
    std::optional<LiveDataBatch> drain();
    LiveDataQueueReset reset( Generation generation );
    void close();
    bool isClosed() const;
    std::size_t waitingProducerCount() const;
    LiveDataStatistics statistics() const;

private:
    struct ResetEpoch {};

    bool canEverFit( std::size_t byteCount ) const;
    bool hasCapacityFor( std::size_t byteCount ) const;
    void accept( const LiveDataChunk& chunk );
    void recordBackpressure( const LiveDataChunk& chunk );

private:
    const LiveDataQueueLimits limits_;
    const DrainNotification drainNotification_;
    mutable std::mutex mutex_;
    std::condition_variable capacityChanged_;
    Generation generation_;
    std::vector<std::uint8_t> queuedBytes_;
    std::size_t queuedChunks_{ 0 };
    std::size_t waitingProducers_{ 0 };
    LiveDataStatistics statistics_;
    std::shared_ptr<const ResetEpoch> resetEpoch_;
    bool closed_{ false };
};

} // namespace klogg::livecapture
