/*
 * Copyright (C) 2026
 *
 * This file is part of klogg.
 *
 * klogg is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include <catch2/catch.hpp>

#include <efsw/DirectoryIoLifetime.hpp>

namespace {

using efsw::DirectoryIoLifetime;

class FakeIocpAdapter {
  public:
    explicit FakeIocpAdapter( void* storage ) : token_( lifetime_.registerWatch( storage ) ) {}

    bool submit( bool accepted = true )
    {
        if ( !lifetime_.beginSubmission( token_ ) )
            return false;
        ++submissionAttempts_;
        if ( accepted )
            return true;

        lifetime_.submissionFailed( token_ );
        return false;
    }

    bool submitInitial( bool accepted )
    {
        if ( submit( accepted ) )
            return true;
        retireRejectedSubmission();
        return false;
    }

    void cancel()
    {
        retiredOwner_ = true;
        publicOwner_ = false;
        lifetime_.retire( token_ );
    }

    void shutdownAfterWorkerStartFailed()
    {
        cancel();
    }

    void dequeueNonNullPacket( bool succeeded, unsigned long bytesTransferred,
                               bool acceptRearm = true )
    {
        (void)bytesTransferred;
        const auto completion = lifetime_.completionDequeued( token_, succeeded );
        if ( completion == DirectoryIoLifetime::Completion::Failed ) {
            publicOwner_ = false;
            retiredOwner_ = true;
        } else if ( completion == DirectoryIoLifetime::Completion::Dispatch &&
                    !submit( acceptRearm ) ) {
            retireRejectedSubmission();
        }

        if ( lifetime_.finishDispatch( token_ ) )
            releaseIfReady();
    }

    int destructions() const { return destructions_; }
    bool publicOwner() const { return publicOwner_; }
    bool retiredOwner() const { return retiredOwner_; }
    int submissionAttempts() const { return submissionAttempts_; }

  private:
    void retireRejectedSubmission()
    {
        publicOwner_ = false;
        retiredOwner_ = true;
        lifetime_.retire( token_ );
        releaseIfReady();
    }

    void releaseIfReady()
    {
        if ( lifetime_.readyToDestroy( token_ ) ) {
            if ( lifetime_.destroy( token_ ) ) {
                retiredOwner_ = false;
                ++destructions_;
            }
        }
    }

    DirectoryIoLifetime lifetime_;
    DirectoryIoLifetime::Token token_;
    bool publicOwner_ = true;
    bool retiredOwner_ = false;
    int submissionAttempts_ = 0;
    int destructions_ = 0;
};

TEST_CASE( "A zero-byte completion rearms exactly once through the IOCP adapter" )
{
    int storage = 0;
    FakeIocpAdapter adapter( &storage );

    REQUIRE( adapter.submit() );
    adapter.dequeueNonNullPacket( true, 0 );
    REQUIRE( adapter.submissionAttempts() == 2 );
    REQUIRE( adapter.publicOwner() );
    REQUIRE_FALSE( adapter.retiredOwner() );

    adapter.cancel();
    adapter.dequeueNonNullPacket( false, 0 );
    REQUIRE( adapter.destructions() == 1 );
}

TEST_CASE( "A FALSE non-null completion retires an active IOCP adapter owner" )
{
    int storage = 0;
    FakeIocpAdapter adapter( &storage );

    REQUIRE( adapter.submit() );
    adapter.dequeueNonNullPacket( false, 4096 );
    REQUIRE_FALSE( adapter.publicOwner() );
    REQUIRE_FALSE( adapter.retiredOwner() );
    REQUIRE( adapter.submissionAttempts() == 1 );
    REQUIRE( adapter.destructions() == 1 );
}

TEST_CASE( "A rejected native submission retires the logical owner before teardown" )
{
    DirectoryIoLifetime lifetime;
    int storage = 0;
    const auto token = lifetime.registerWatch( &storage );

    REQUIRE( lifetime.beginSubmission( token ) );
    REQUIRE( lifetime.submissionFailed( token ) );
    REQUIRE( lifetime.retiredCount() == 1 );
    REQUIRE( lifetime.readyToDestroy( token ) );
    REQUIRE( lifetime.destroy( token ) );
}

TEST_CASE( "An initial native submission rejection releases an unqueued public owner" )
{
    int storage = 0;
    FakeIocpAdapter adapter( &storage );

    REQUIRE_FALSE( adapter.submitInitial( false ) );
    REQUIRE_FALSE( adapter.publicOwner() );
    REQUIRE_FALSE( adapter.retiredOwner() );
    REQUIRE( adapter.submissionAttempts() == 1 );
    REQUIRE( adapter.destructions() == 1 );
}

TEST_CASE( "A rejected zero-byte rearm retires its public owner after callback dispatch" )
{
    int storage = 0;
    FakeIocpAdapter adapter( &storage );

    REQUIRE( adapter.submitInitial( true ) );
    adapter.dequeueNonNullPacket( true, 0, false );
    REQUIRE_FALSE( adapter.publicOwner() );
    REQUIRE_FALSE( adapter.retiredOwner() );
    REQUIRE( adapter.submissionAttempts() == 2 );
    REQUIRE( adapter.destructions() == 1 );
}

TEST_CASE( "A rejected nonzero rearm retires its public owner after callback dispatch" )
{
    int storage = 0;
    FakeIocpAdapter adapter( &storage );

    REQUIRE( adapter.submitInitial( true ) );
    adapter.dequeueNonNullPacket( true, 4096, false );
    REQUIRE_FALSE( adapter.publicOwner() );
    REQUIRE_FALSE( adapter.retiredOwner() );
    REQUIRE( adapter.submissionAttempts() == 2 );
    REQUIRE( adapter.destructions() == 1 );
}

TEST_CASE( "A failed worker startup leaves cancellation storage for caller-side draining" )
{
    int storage = 0;
    FakeIocpAdapter adapter( &storage );

    REQUIRE( adapter.submitInitial( true ) );
    adapter.shutdownAfterWorkerStartFailed();
    REQUIRE_FALSE( adapter.publicOwner() );
    REQUIRE( adapter.retiredOwner() );
    REQUIRE( adapter.destructions() == 0 );

    adapter.dequeueNonNullPacket( false, 0 );
    REQUIRE_FALSE( adapter.retiredOwner() );
    REQUIRE( adapter.destructions() == 1 );
}

TEST_CASE( "A pending Win32 directory request owns its storage until its cancellation packet dequeues" )
{
    DirectoryIoLifetime lifetime;
    int storage = 0;
    const auto token = lifetime.registerWatch( &storage );

    REQUIRE( lifetime.beginSubmission( token ) );
    REQUIRE( lifetime.retire( token ) );
    CHECK_FALSE( lifetime.readyToDestroy( token ) );

    // GetQueuedCompletionStatus(FALSE, ..., nonnullOverlapped) is still the
    // native acknowledgement that permits the retirement to progress.
    REQUIRE( lifetime.completionDequeued( token, false ) ==
             DirectoryIoLifetime::Completion::Retired );
    CHECK_FALSE( lifetime.dispatchAllowed( token ) );
    REQUIRE( lifetime.finishDispatch( token ) );
    REQUIRE( lifetime.destroy( token ) );
}

TEST_CASE( "A queued completion cannot be released by a GetOverlappedResult-only path" )
{
    DirectoryIoLifetime lifetime;
    int storage = 0;
    const auto token = lifetime.registerWatch( &storage );

    REQUIRE( lifetime.beginSubmission( token ) );
    REQUIRE( lifetime.retire( token ) );
    CHECK_FALSE( lifetime.readyToDestroy( token ) );

    REQUIRE( lifetime.completionDequeued( token, false ) ==
             DirectoryIoLifetime::Completion::Retired );
    REQUIRE( lifetime.finishDispatch( token ) );
    REQUIRE( lifetime.destroy( token ) );
}

TEST_CASE( "An active failed IOCP completion becomes retired and cannot strand a watch" )
{
    DirectoryIoLifetime lifetime;
    int storage = 0;
    const auto token = lifetime.registerWatch( &storage );

    REQUIRE( lifetime.beginSubmission( token ) );
    REQUIRE( lifetime.completionDequeued( token, false ) ==
             DirectoryIoLifetime::Completion::Failed );
    REQUIRE( lifetime.retiredCount() == 1 );
    CHECK_FALSE( lifetime.dispatchAllowed( token ) );
    CHECK_FALSE( lifetime.readyToDestroy( token ) );
    REQUIRE( lifetime.finishDispatch( token ) );
    REQUIRE( lifetime.destroy( token ) );
}

TEST_CASE( "Reentrant remove or stop suppresses later batched callback notifications" )
{
    DirectoryIoLifetime lifetime;
    int storage = 0;
    const auto token = lifetime.registerWatch( &storage );

    REQUIRE( lifetime.beginSubmission( token ) );
    REQUIRE( lifetime.completionDequeued( token, true ) ==
             DirectoryIoLifetime::Completion::Dispatch );
    REQUIRE( lifetime.dispatchAllowed( token ) );

    // This models a listener removing its watch during the first notification
    // of a batched callback. The callback frame retains storage until it exits.
    CHECK_FALSE( lifetime.retire( token ) );
    CHECK_FALSE( lifetime.dispatchAllowed( token ) );
    CHECK_FALSE( lifetime.readyToDestroy( token ) );
    REQUIRE( lifetime.finishDispatch( token ) );
    REQUIRE( lifetime.destroy( token ) );
}

TEST_CASE( "A reentrant rename-old listener suppresses its paired rename-new notification" )
{
    DirectoryIoLifetime lifetime;
    int storage = 0;
    const auto token = lifetime.registerWatch( &storage );
    int deliveredNotifications = 0;

    REQUIRE( lifetime.beginSubmission( token ) );
    REQUIRE( lifetime.completionDequeued( token, true ) ==
             DirectoryIoLifetime::Completion::Dispatch );

    if ( lifetime.dispatchAllowed( token ) )
        ++deliveredNotifications; // rename-old
    CHECK_FALSE( lifetime.retire( token ) );
    if ( lifetime.dispatchAllowed( token ) )
        ++deliveredNotifications; // paired rename-new

    REQUIRE( deliveredNotifications == 1 );
    REQUIRE( lifetime.finishDispatch( token ) );
    REQUIRE( lifetime.destroy( token ) );
}

TEST_CASE( "A failed cancellation with no pending request releases exactly once" )
{
    DirectoryIoLifetime lifetime;
    int storage = 0;
    const auto token = lifetime.registerWatch( &storage );

    CHECK_FALSE( lifetime.retire( token ) );
    REQUIRE( lifetime.readyToDestroy( token ) );
    REQUIRE( lifetime.destroy( token ) );
    CHECK_FALSE( lifetime.destroy( token ) );
}

TEST_CASE( "Tracking precedes immediate completion and rejects duplicate submissions" )
{
    DirectoryIoLifetime lifetime;
    int storage = 0;
    const auto token = lifetime.registerWatch( &storage );

    REQUIRE( lifetime.beginSubmission( token ) );
    CHECK_FALSE( lifetime.beginSubmission( token ) );
    REQUIRE( lifetime.completionDequeued( token, true ) ==
             DirectoryIoLifetime::Completion::Dispatch );
    CHECK_FALSE( lifetime.finishDispatch( token ) );
    CHECK_FALSE( lifetime.retire( token ) );
    REQUIRE( lifetime.destroy( token ) );
}

TEST_CASE( "Replacement tokens reject stale packets and shutdown drains every retirement" )
{
    DirectoryIoLifetime lifetime;
    int firstStorage = 0;
    int secondStorage = 0;

    const auto first = lifetime.registerWatch( &firstStorage );
    REQUIRE( lifetime.beginSubmission( first ) );
    REQUIRE( lifetime.retire( first ) );
    REQUIRE( lifetime.completionDequeued( first, false ) ==
             DirectoryIoLifetime::Completion::Retired );
    REQUIRE( lifetime.finishDispatch( first ) );
    REQUIRE( lifetime.destroy( first ) );

    const auto replacement = lifetime.registerWatch( &firstStorage );
    REQUIRE( replacement.generation != first.generation );
    CHECK_FALSE( lifetime.beginSubmission( first ) );
    REQUIRE( lifetime.beginSubmission( replacement ) );

    const auto second = lifetime.registerWatch( &secondStorage );
    REQUIRE( lifetime.beginSubmission( second ) );
    REQUIRE( lifetime.retire( replacement ) );
    REQUIRE( lifetime.retire( second ) );
    REQUIRE( lifetime.retiredCount() == 2 );

    REQUIRE( lifetime.completionDequeued( replacement, false ) ==
             DirectoryIoLifetime::Completion::Retired );
    REQUIRE( lifetime.finishDispatch( replacement ) );
    REQUIRE( lifetime.destroy( replacement ) );
    REQUIRE( lifetime.retiredCount() == 1 );

    REQUIRE( lifetime.completionDequeued( second, false ) ==
             DirectoryIoLifetime::Completion::Retired );
    REQUIRE( lifetime.finishDispatch( second ) );
    REQUIRE( lifetime.destroy( second ) );
    REQUIRE( lifetime.retiredCount() == 0 );
}

} // namespace
