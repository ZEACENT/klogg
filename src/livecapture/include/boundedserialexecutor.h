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
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

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

class BoundedConcurrentExecutor final {
public:
    using Task = std::function<void()>;

    BoundedConcurrentExecutor( std::size_t maxConcurrency,
                               std::chrono::milliseconds shutdownDeadline )
        : state_( std::make_shared<State>() )
        , shutdownDeadline_( shutdownDeadline )
    {
        if ( maxConcurrency == 0u ) {
            throw std::invalid_argument( "bounded executor concurrency must be positive" );
        }
        state_->workerCount = maxConcurrency;
        state_->runningKeys.resize( maxConcurrency );
        threads_.reserve( maxConcurrency );
        try {
            for ( std::size_t worker = 0u; worker < maxConcurrency; ++worker ) {
                threads_.emplace_back( [ state = state_, worker ] { run( state, worker ); } );
            }
        } catch ( ... ) {
            requestStop();
            for ( auto& thread : threads_ ) {
                if ( thread.joinable() ) {
                    thread.join();
                }
            }
            throw;
        }
    }

    ~BoundedConcurrentExecutor()
    {
        requestStop();
        if ( std::none_of( threads_.cbegin(), threads_.cend(),
                           []( const auto& thread ) { return thread.joinable(); } ) ) {
            return;
        }
        const auto currentThread = std::this_thread::get_id();
        const bool calledFromWorker
            = std::any_of( threads_.cbegin(), threads_.cend(), [ currentThread ]( const auto& thread ) {
                  return thread.get_id() == currentThread;
              } );
        if ( calledFromWorker ) {
            detachWorkers();
            return;
        }

        bool finished = false;
        {
            std::unique_lock<std::mutex> lock( state_->mutex );
            const auto deadline
                = std::max( shutdownDeadline_, std::chrono::milliseconds::zero() );
            finished = state_->changed.wait_for( lock, deadline, [ state = state_ ] {
                return state->finishedWorkers == state->workerCount;
            } );
        }
        if ( finished ) {
            for ( auto& thread : threads_ ) {
                if ( thread.joinable() ) {
                    thread.join();
                }
            }
        }
        else {
            detachWorkers();
        }
    }

    BoundedConcurrentExecutor( const BoundedConcurrentExecutor& ) = delete;
    BoundedConcurrentExecutor& operator=( const BoundedConcurrentExecutor& ) = delete;

    bool post( Task task )
    {
        {
            std::lock_guard<std::mutex> lock( state_->mutex );
            if ( state_->stopping ) {
                return false;
            }
            state_->tasks.push_back( QueuedTask{ std::move( task ), std::nullopt } );
        }
        state_->changed.notify_one();
        return true;
    }

    // At most one replacement per key can wait while another task for that key
    // is running. Re-submission replaces the pending task in place, preserving
    // FIFO position relative to unrelated keys.
    bool submitLatest( std::string key, Task task )
    {
        {
            std::lock_guard<std::mutex> lock( state_->mutex );
            if ( state_->stopping ) {
                return false;
            }
            const auto pending = std::find_if(
                state_->tasks.begin(), state_->tasks.end(), [ &key ]( const QueuedTask& queued ) {
                    return queued.key.has_value() && *queued.key == key;
                } );
            if ( pending != state_->tasks.end() ) {
                pending->task = std::move( task );
                return true;
            }
            state_->tasks.push_back(
                QueuedTask{ std::move( task ), std::make_optional( std::move( key ) ) } );
        }
        state_->changed.notify_one();
        return true;
    }

    // Cancellation removes only a queued replacement. A running task remains
    // responsible for its ownership cleanup and must use its caller's stale gate.
    bool cancelLatest( const std::string& key )
    {
        std::lock_guard<std::mutex> lock( state_->mutex );
        const auto pending = std::find_if(
            state_->tasks.begin(), state_->tasks.end(), [ &key ]( const QueuedTask& queued ) {
                return queued.key.has_value() && *queued.key == key;
            } );
        if ( pending == state_->tasks.end() ) {
            return false;
        }
        state_->tasks.erase( pending );
        return true;
    }

    void clearPendingLatest()
    {
        std::lock_guard<std::mutex> lock( state_->mutex );
        state_->tasks.erase(
            std::remove_if( state_->tasks.begin(), state_->tasks.end(), []( const QueuedTask& queued ) {
                return queued.key.has_value();
            } ),
            state_->tasks.end() );
    }

#ifdef KLOGG_TESTS
    std::size_t pendingLatestCountForTest() const
    {
        std::lock_guard<std::mutex> lock( state_->mutex );
        return static_cast<std::size_t>(
            std::count_if( state_->tasks.cbegin(), state_->tasks.cend(), []( const QueuedTask& queued ) {
                return queued.key.has_value();
            } ) );
    }

    std::size_t runningLatestCountForTest() const
    {
        std::lock_guard<std::mutex> lock( state_->mutex );
        return static_cast<std::size_t>( std::count_if(
            state_->runningKeys.cbegin(), state_->runningKeys.cend(),
            []( const auto& key ) { return key.has_value(); } ) );
    }
#endif

    void shutdownAsync() noexcept
    {
        try {
            requestStop();
            detachWorkers();
        } catch ( ... ) { // NOLINT(bugprone-empty-catch)
            // Teardown must remain noexcept. The destructor retains its bounded
            // wait fallback if detaching is rejected unexpectedly.
        }
    }

private:
    struct QueuedTask {
        Task task;
        std::optional<std::string> key;
    };

    struct State {
        std::mutex mutex;
        std::condition_variable changed;
        std::deque<QueuedTask> tasks;
        std::vector<std::optional<std::string>> runningKeys;
        std::size_t workerCount{ 0u };
        std::size_t finishedWorkers{ 0u };
        bool stopping{ false };
    };

    void requestStop() noexcept
    {
        {
            std::lock_guard<std::mutex> lock( state_->mutex );
            state_->stopping = true;
            state_->tasks.erase(
                std::remove_if( state_->tasks.begin(), state_->tasks.end(),
                                []( const QueuedTask& queued ) {
                                    return queued.key.has_value();
                                } ),
                state_->tasks.end() );
        }
        state_->changed.notify_all();
    }

    void detachWorkers()
    {
        for ( auto& thread : threads_ ) {
            if ( thread.joinable() ) {
                thread.detach();
            }
        }
    }

    static bool isRunning( const State& state, const std::string& key )
    {
        return std::any_of( state.runningKeys.cbegin(), state.runningKeys.cend(),
                            [ &key ]( const auto& running ) {
                                return running.has_value() && *running == key;
                            } );
    }

    static auto findRunnableTask( State& state )
    {
        return std::find_if( state.tasks.begin(), state.tasks.end(), [ &state ]( const auto& queued ) {
            return !queued.key.has_value() || !isRunning( state, *queued.key );
        } );
    }

    static void run( const std::shared_ptr<State>& state, std::size_t worker ) noexcept
    {
        for ( ;; ) {
            Task task;
            std::optional<std::string> key;
            {
                std::unique_lock<std::mutex> lock( state->mutex );
                state->changed.wait( lock, [ & ] {
                    return state->stopping || findRunnableTask( *state ) != state->tasks.end();
                } );
                const auto queued = findRunnableTask( *state );
                if ( queued == state->tasks.end() ) {
                    if ( state->stopping ) {
                        ++state->finishedWorkers;
                        break;
                    }
                    continue;
                }
                task = std::move( queued->task );
                key = std::move( queued->key );
                state->tasks.erase( queued );
                state->runningKeys[ worker ] = std::move( key );
            }
            try {
                task();
            } catch ( ... ) { // NOLINT(bugprone-empty-catch)
                // Native worker tasks are an exception boundary.
            }
            {
                std::lock_guard<std::mutex> lock( state->mutex );
                state->runningKeys[ worker ].reset();
            }
            state->changed.notify_all();
        }
        state->changed.notify_all();
    }

    std::shared_ptr<State> state_;
    std::chrono::milliseconds shutdownDeadline_;
    std::vector<std::thread> threads_;
};

} // namespace klogg::livecapture
