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

#include "livedataqueue.h"

#include <algorithm>
#include <utility>

namespace klogg::livecapture {

LiveDataQueue::LiveDataQueue( LiveDataQueueLimits limits, Generation generation,
                              DrainNotification drainNotification )
    : limits_( limits )
    , drainNotification_( std::move( drainNotification ) )
    , generation_( generation )
    , resetEpoch_( std::make_shared<const ResetEpoch>() )
{
    statistics_.generation = generation_;
}

bool LiveDataQueue::canEverFit( std::size_t byteCount ) const
{
    return limits_.maxQueuedChunks > 0u && byteCount <= limits_.maxQueuedBytes;
}

bool LiveDataQueue::hasCapacityFor( std::size_t byteCount ) const
{
    return canEverFit( byteCount ) && queuedChunks_ < limits_.maxQueuedChunks
           && queuedBytes_.size() <= limits_.maxQueuedBytes - byteCount;
}

void LiveDataQueue::accept( const LiveDataChunk& chunk )
{
    const auto byteCount = chunk.bytes.size();
    queuedBytes_.insert( queuedBytes_.end(), chunk.bytes.begin(), chunk.bytes.end() );
    ++queuedChunks_;

    recordLiveDataReceived( statistics_, byteCount );
    statistics_.queuedBytes = queuedBytes_.size();
    statistics_.queuedChunks = queuedChunks_;
    statistics_.highWaterQueuedBytes
        = std::max( statistics_.highWaterQueuedBytes, statistics_.queuedBytes );
    statistics_.highWaterQueuedChunks
        = std::max( statistics_.highWaterQueuedChunks, statistics_.queuedChunks );
}

void LiveDataQueue::recordBackpressure( const LiveDataChunk& chunk )
{
    const auto byteCount = chunk.bytes.size();
    recordLiveDataReceived( statistics_, byteCount );
    recordLiveDataBackpressure( statistics_, byteCount );
}

LiveDataEnqueueResult LiveDataQueue::tryEnqueue( const LiveDataChunk& chunk )
{
    bool notifyDrain = false;
    {
        std::lock_guard<std::mutex> lock( mutex_ );
        if ( chunk.generation != generation_ ) {
            return LiveDataEnqueueResult::StaleGeneration;
        }
        if ( closed_ ) {
            return LiveDataEnqueueResult::Closed;
        }
        if ( !hasCapacityFor( chunk.bytes.size() ) ) {
            recordBackpressure( chunk );
            return LiveDataEnqueueResult::Backpressure;
        }

        notifyDrain = queuedChunks_ == 0u;
        accept( chunk );
    }

    if ( notifyDrain && drainNotification_ ) {
        drainNotification_();
    }
    return LiveDataEnqueueResult::Accepted;
}

LiveDataEnqueueResult LiveDataQueue::enqueueWait( const LiveDataChunk& chunk )
{
    bool notifyDrain = false;
    {
        std::unique_lock<std::mutex> lock( mutex_ );
        if ( chunk.generation != generation_ ) {
            return LiveDataEnqueueResult::StaleGeneration;
        }
        if ( closed_ ) {
            return LiveDataEnqueueResult::Closed;
        }
        if ( !canEverFit( chunk.bytes.size() ) ) {
            recordBackpressure( chunk );
            return LiveDataEnqueueResult::Backpressure;
        }

        const auto enqueueEpoch = resetEpoch_;
        ++waitingProducers_;
        capacityChanged_.wait( lock, [ this, &chunk, &enqueueEpoch ] {
            return chunk.generation != generation_ || enqueueEpoch != resetEpoch_ || closed_
                   || hasCapacityFor( chunk.bytes.size() );
        } );
        --waitingProducers_;

        if ( chunk.generation != generation_ || enqueueEpoch != resetEpoch_ ) {
            return LiveDataEnqueueResult::StaleGeneration;
        }
        if ( closed_ ) {
            return LiveDataEnqueueResult::Closed;
        }

        notifyDrain = queuedChunks_ == 0u;
        accept( chunk );
    }

    if ( notifyDrain && drainNotification_ ) {
        drainNotification_();
    }
    return LiveDataEnqueueResult::Accepted;
}

std::optional<LiveDataBatch> LiveDataQueue::drain()
{
    std::optional<LiveDataBatch> drained;
    {
        std::lock_guard<std::mutex> lock( mutex_ );
        if ( queuedChunks_ == 0u ) {
            return std::nullopt;
        }

        const auto deliveredBytes = queuedBytes_.size();
        const auto deliveredChunks = queuedChunks_;
        drained = LiveDataBatch{ generation_, std::move( queuedBytes_ ), deliveredChunks };
        queuedBytes_.clear();
        queuedChunks_ = 0u;

        statistics_.queuedBytes = 0u;
        statistics_.queuedChunks = 0u;
        recordLiveDataDelivered( statistics_, deliveredBytes, deliveredChunks );
    }

    capacityChanged_.notify_all();
    return drained;
}

LiveDataQueueReset LiveDataQueue::reset( Generation generation )
{
    auto nextResetEpoch = std::make_shared<const ResetEpoch>();
    LiveDataQueueReset result;
    {
        std::lock_guard<std::mutex> lock( mutex_ );
        result = LiveDataQueueReset{ generation_, queuedBytes_.size(), queuedChunks_ };
        generation_ = generation;
        resetEpoch_ = std::move( nextResetEpoch );
        queuedBytes_.clear();
        queuedChunks_ = 0u;
        statistics_ = LiveDataStatistics{};
        statistics_.generation = generation_;
    }

    capacityChanged_.notify_all();
    return result;
}

void LiveDataQueue::close()
{
    {
        std::lock_guard<std::mutex> lock( mutex_ );
        closed_ = true;
    }
    capacityChanged_.notify_all();
}

bool LiveDataQueue::isClosed() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return closed_;
}

std::size_t LiveDataQueue::waitingProducerCount() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return waitingProducers_;
}

LiveDataStatistics LiveDataQueue::statistics() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return statistics_;
}

} // namespace klogg::livecapture
