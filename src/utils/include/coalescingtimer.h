/*
 * Copyright (C) 2026 Anton Filimonov and other contributors
 *
 * This file is part of klogg.
 *
 * klogg is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef COALESCINGTIMER_H
#define COALESCINGTIMER_H

#include <functional>
#include <utility>

#include <QTimer>

namespace klogg {

inline constexpr int kIncrementalPresentationIntervalMs = 33;

class CoalescingTimer final {
public:
    CoalescingTimer( int intervalMs, std::function<void()> callback )
        : callback_( std::move( callback ) )
    {
        timer_.setSingleShot( true );
        timer_.setTimerType( Qt::PreciseTimer );
        timer_.setInterval( intervalMs );
        QObject::connect( &timer_, &QTimer::timeout, [ this ] {
            if ( !pending_ ) {
                return;
            }
            pending_ = false;
            callback_();
        } );
    }

    ~CoalescingTimer()
    {
        cancel();
    }

    CoalescingTimer( const CoalescingTimer& ) = delete;
    CoalescingTimer& operator=( const CoalescingTimer& ) = delete;
    CoalescingTimer( CoalescingTimer&& ) = delete;
    CoalescingTimer& operator=( CoalescingTimer&& ) = delete;

    void request()
    {
        pending_ = true;
        if ( !timer_.isActive() ) {
            timer_.start();
        }
    }

    bool flushPending()
    {
        if ( !pending_ ) {
            return false;
        }

        timer_.stop();
        pending_ = false;
        callback_();
        return true;
    }

    void cancel()
    {
        timer_.stop();
        pending_ = false;
    }

    bool isPending() const
    {
        return pending_;
    }
    bool isActive() const
    {
        return timer_.isActive();
    }

private:
    QTimer timer_;
    std::function<void()> callback_;
    bool pending_ = false;
};

} // namespace klogg

#endif // COALESCINGTIMER_H
