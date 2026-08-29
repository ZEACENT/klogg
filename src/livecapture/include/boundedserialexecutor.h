/*
 * Copyright (C) 2026 ZEACENT and other contributors
 *
 * This file is part of klogg.
 *
 * klogg is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace klogg::livecapture {

class BoundedSerialExecutor final {
public:
    using Task = std::function<void()>;

    explicit BoundedSerialExecutor( std::chrono::milliseconds shutdownDeadline )
        : state_( std::make_shared<State>() )
        , shutdownDeadline_( shutdownDeadline )
        , thread_( [ state = state_ ] { run( state ); } )
    {
    }

    ~BoundedSerialExecutor()
    {
        {
            std::lock_guard<std::mutex> lock( state_->mutex );
            state_->stopping = true;
        }
        state_->changed.notify_all();
        if ( !thread_.joinable() ) {
            return;
        }
        if ( thread_.get_id() == std::this_thread::get_id() ) {
            thread_.detach();
            return;
        }

        bool finished = false;
        {
            std::unique_lock<std::mutex> lock( state_->mutex );
            const auto deadline
                = std::max( shutdownDeadline_, std::chrono::milliseconds::zero() );
            finished = state_->changed.wait_for(
                lock, deadline, [ state = state_ ] { return state->finished; } );
        }
        if ( finished ) {
            thread_.join();
        }
        else {
            // The worker and queued tasks retain State independently. A blocked
            // native call may finish later without retaining this executor.
            thread_.detach();
        }
    }

    BoundedSerialExecutor( const BoundedSerialExecutor& ) = delete;
    BoundedSerialExecutor& operator=( const BoundedSerialExecutor& ) = delete;

    void shutdownAsync() noexcept
    {
        try {
            {
                std::lock_guard<std::mutex> lock( state_->mutex );
                state_->stopping = true;
            }
            state_->changed.notify_all();
            if ( thread_.joinable() ) {
                // The worker retains State and drains already queued tasks before
                // exiting. Detaching keeps a caller-owned teardown path nonblocking.
                thread_.detach();
            }
        } catch ( ... ) { // NOLINT(bugprone-empty-catch)
            // Teardown must remain noexcept. The destructor retains its bounded
            // wait fallback if the platform rejects detaching unexpectedly.
        }
    }

    bool post( Task task )
    {
        return enqueue( std::move( task ), false );
    }

    bool postBeforeFinished( Task task )
    {
        return enqueue( std::move( task ), true );
    }

private:
    bool enqueue( Task task, bool allowStopping )
    {
        {
            std::lock_guard<std::mutex> lock( state_->mutex );
            if ( state_->finished || ( state_->stopping && !allowStopping ) ) {
                return false;
            }
            state_->tasks.push_back( std::move( task ) );
        }
        state_->changed.notify_one();
        return true;
    }

    struct State {
        std::mutex mutex;
        std::condition_variable changed;
        std::deque<Task> tasks;
        bool stopping{ false };
        bool finished{ false };
    };

    static void run( const std::shared_ptr<State>& state ) noexcept
    {
        for ( ;; ) {
            Task task;
            {
                std::unique_lock<std::mutex> lock( state->mutex );
                state->changed.wait(
                    lock, [ & ] { return state->stopping || !state->tasks.empty(); } );
                if ( state->tasks.empty() ) {
                    if ( state->stopping ) {
                        state->finished = true;
                        break;
                    }
                    continue;
                }
                task = std::move( state->tasks.front() );
                state->tasks.pop_front();
            }
            try {
                task();
            } catch ( ... ) { // NOLINT(bugprone-empty-catch)
                // Native worker tasks are an exception boundary.
            }
        }
        {
            std::lock_guard<std::mutex> lock( state->mutex );
            state->finished = true;
        }
        state->changed.notify_all();
    }

    std::shared_ptr<State> state_;
    std::chrono::milliseconds shutdownDeadline_;
    std::thread thread_;
};

} // namespace klogg::livecapture
