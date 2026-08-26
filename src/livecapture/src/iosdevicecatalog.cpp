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

#include "iosdevicecatalog.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

namespace klogg::livecapture::ios {
namespace {

bool isSupportedConnection( NativeConnectionType connection ) noexcept
{
    return connection == NativeConnectionType::Usb || connection == NativeConnectionType::Network;
}

std::string endpointIdentity( const IosEndpointKey& endpoint )
{
    return endpoint.udid
           + ( endpoint.connectionType == NativeConnectionType::Usb ? "@usb" : "@network" );
}

NativeConnectionOption connectionOption( NativeConnectionType connection )
{
    return connection == NativeConnectionType::Usb ? NativeConnectionOption::Usb
                                                   : NativeConnectionOption::Network;
}

} // namespace

struct IosDeviceCatalog::State final : std::enable_shared_from_this<State> {
    enum class Lifecycle : std::uint8_t { Stopped, Starting, Running };

    struct MetadataRequest {
        IosEndpointKey endpoint;
        Generation catalogGeneration{ 0 };
        Generation endpointEpoch{ 0 };
        Generation requestGeneration{ 0 };
    };

    struct CopiedNativeEvent {
        NativeEventType type{ NativeEventType::Add };
        IosEndpointKey endpoint;
    };

    struct NativeCallbackContext {
        std::weak_ptr<State> state;
    };

    State( IosNativeApi nativeApi, IosCatalogExecutor catalogExecutor )
        : api( nativeApi )
        , executor( std::move( catalogExecutor ) )
    {
    }

    static auto findEntry( IosCatalogSnapshot& snapshot, const IosEndpointKey& endpoint )
    {
        return std::find_if(
            snapshot.entries.begin(), snapshot.entries.end(),
            [ &endpoint ]( const IosCatalogEntry& entry ) { return entry.endpoint == endpoint; } );
    }

    static void addEndpointToSnapshot( IosCatalogSnapshot& snapshot, IosEndpointKey endpoint,
                                       Generation& nextEpoch )
    {
        if ( endpoint.udid.empty() || !isSupportedConnection( endpoint.connectionType )
             || findEntry( snapshot, endpoint ) != snapshot.entries.end() ) {
            return;
        }
        snapshot.entries.push_back(
            IosCatalogEntry{ std::move( endpoint ), ++nextEpoch, std::nullopt, std::nullopt } );
    }

    void applyEventToSnapshot( const CopiedNativeEvent& event )
    {
        switch ( event.type ) {
        case NativeEventType::Add:
        case NativeEventType::Paired:
            addEndpointToSnapshot( current, event.endpoint, nextEpoch );
            break;
        case NativeEventType::Remove: {
            const auto found = findEntry( current, event.endpoint );
            if ( found != current.entries.end() ) {
                current.entries.erase( found );
            }
            latestMetadataRequest.erase( endpointIdentity( event.endpoint ) );
            break;
        }
        }
    }

    void notify()
    {
        const auto keepAlive = shared_from_this();
        std::lock_guard<std::recursive_mutex> notificationLock( notificationMutex );
        IosCatalogSnapshot value;
        std::vector<SnapshotCallback> listeners;
        {
            std::lock_guard<std::mutex> lock( mutex );
            value = current;
            listeners.reserve( callbacks.size() );
            for ( const auto& callback : callbacks ) {
                listeners.push_back( callback.second );
            }
        }
        for ( const auto& callback : listeners ) {
            if ( !callback ) {
                continue;
            }
            try {
                callback( value );
            } catch ( ... ) { // NOLINT(bugprone-empty-catch)
                // Snapshot observers are an application boundary. One observer must not
                // prevent peers from receiving updates or unwind through a native C callback.
            }
        }
    }

    void notifyGeneration( Generation generation )
    {
        {
            std::lock_guard<std::mutex> lock( mutex );
            if ( lifecycle != Lifecycle::Running || current.generation != generation ) {
                return;
            }
        }
        notify();
    }

    void acceptNativeEvent( CopiedNativeEvent event )
    {
        IosCatalogExecutor dispatch;
        Generation generation{ 0 };
        {
            std::lock_guard<std::mutex> lock( mutex );
            if ( lifecycle == Lifecycle::Starting ) {
                startupEvents.push_back( std::move( event ) );
                return;
            }
            if ( lifecycle != Lifecycle::Running ) {
                return;
            }
            applyEventToSnapshot( event );
            generation = current.generation;
            if ( !callbacks.empty() ) {
                dispatch = executor;
            }
        }

        if ( !dispatch ) {
            return;
        }
        const std::weak_ptr<State> weakState = weak_from_this();
        try {
            dispatch( [ weakState, generation ] {
                if ( const auto state = weakState.lock() ) {
                    state->notifyGeneration( generation );
                }
            } );
        } catch ( ... ) { // NOLINT(bugprone-empty-catch)
            // Never allow an executor failure to cross the libimobiledevice callback ABI.
        }
    }

    bool start()
    {
        std::lock_guard<std::recursive_mutex> lifecycleLock( lifecycleMutex );
        {
            std::lock_guard<std::mutex> lock( mutex );
            if ( lifecycle == Lifecycle::Running ) {
                return true;
            }
            if ( lifecycle == Lifecycle::Starting ) {
                return false;
            }
        }
        if ( !subscription.reset() ) {
            return false;
        }
        callbackContext.reset();
        {
            std::lock_guard<std::mutex> lock( mutex );
            lifecycle = Lifecycle::Starting;
            ++current.generation;
            current.entries.clear();
            latestMetadataRequest.clear();
            startupEvents.clear();
        }

        auto failStart = [ this ] {
            std::lock_guard<std::mutex> lock( mutex );
            lifecycle = Lifecycle::Stopped;
            current.entries.clear();
            latestMetadataRequest.clear();
            startupEvents.clear();
        };

        if ( api.getDeviceListExtended == nullptr || api.deviceListExtendedFree == nullptr
             || api.eventSubscribe == nullptr || api.eventUnsubscribe == nullptr ) {
            failStart();
            return false;
        }

        auto newCallbackContext
            = std::make_unique<NativeCallbackContext>( NativeCallbackContext{ weak_from_this() } );
        NativeEventSubscription newSubscription( api, &IosDeviceCatalog::nativeEventCallback,
                                                 newCallbackContext.get() );
        if ( !newSubscription.active() ) {
            failStart();
            if ( !newSubscription.reset() ) {
                // The native side still owns the user-data pointer. Leak only this
                // inert weak gate rather than allowing a callback-after-free.
                static_cast<void>(
                    newCallbackContext.release() ); // NOLINT(bugprone-unused-return-value)
            }
            return false;
        }

        NativeDeviceInfo** rawList = nullptr;
        std::int32_t count = 0;
        const auto listResult = api.getDeviceListExtended( &rawList, &count );
        NativeDeviceListOwner devices( api, rawList, count );
        if ( listResult != 0 || count < 0 || ( count > 0 && rawList == nullptr ) ) {
            failStart();
            if ( !newSubscription.reset() ) {
                static_cast<void>(
                    newCallbackContext.release() ); // NOLINT(bugprone-unused-return-value)
            }
            return false;
        }

        {
            std::lock_guard<std::mutex> lock( mutex );
            current.entries.clear();
            for ( std::int32_t index = 0; index < devices.size(); ++index ) {
                const auto* entry = devices.get()[ index ];
                if ( entry == nullptr || entry->udid == nullptr ) {
                    continue;
                }
                addEndpointToSnapshot(
                    current, IosEndpointKey{ entry->udid, entry->connectionType }, nextEpoch );
            }
            for ( const auto& event : startupEvents ) {
                applyEventToSnapshot( event );
            }
            startupEvents.clear();
            subscription = std::move( newSubscription );
            callbackContext = std::move( newCallbackContext );
            lifecycle = Lifecycle::Running;
        }

        notify();
        std::lock_guard<std::mutex> lock( mutex );
        return lifecycle == Lifecycle::Running;
    }

    void stop()
    {
        std::lock_guard<std::recursive_mutex> lifecycleLock( lifecycleMutex );
        {
            std::lock_guard<std::mutex> lock( mutex );
            lifecycle = Lifecycle::Stopped;
            latestMetadataRequest.clear();
            startupEvents.clear();
        }
        // The pinned libimobiledevice/libusbmuxd unsubscribe path removes this
        // callback context under its listener lock before returning. Keeping State
        // alive through this call also covers synchronous REMOVE callbacks.
        if ( subscription.reset() ) {
            callbackContext.reset();
        }
        else {
            // Preserve native user_data if unsubscription fails. Its weak_ptr makes
            // future callbacks inert after State expires, avoiding a shutdown UAF.
            static_cast<void>( callbackContext.release() ); // NOLINT(bugprone-unused-return-value)
        }
    }

    std::optional<MetadataRequest> beginMetadataRequest( const IosEndpointKey& endpoint )
    {
        std::lock_guard<std::mutex> lock( mutex );
        if ( lifecycle != Lifecycle::Running ) {
            return std::nullopt;
        }
        const auto found = findEntry( current, endpoint );
        if ( found == current.entries.end() ) {
            return std::nullopt;
        }
        const auto requestGeneration = ++nextRequestGeneration;
        latestMetadataRequest[ endpointIdentity( endpoint ) ] = requestGeneration;
        return MetadataRequest{ endpoint, current.generation, found->epoch, requestGeneration };
    }

    bool isMetadataRequestCurrent( const MetadataRequest& request ) const
    {
        std::lock_guard<std::mutex> lock( mutex );
        if ( lifecycle != Lifecycle::Running || current.generation != request.catalogGeneration ) {
            return false;
        }
        const auto found = std::find_if( current.entries.cbegin(), current.entries.cend(),
                                         [ &request ]( const IosCatalogEntry& entry ) {
                                             return entry.endpoint == request.endpoint;
                                         } );
        const auto latest = latestMetadataRequest.find( endpointIdentity( request.endpoint ) );
        return found != current.entries.cend() && found->epoch == request.endpointEpoch
               && latest != latestMetadataRequest.end()
               && latest->second == request.requestGeneration;
    }

    void completeMetadataRequest( const MetadataRequest& request,
                                  std::optional<IosDeviceMetadata> metadata,
                                  std::optional<IosCatalogError> error )
    {
        {
            std::lock_guard<std::mutex> lock( mutex );
            if ( lifecycle != Lifecycle::Running
                 || current.generation != request.catalogGeneration ) {
                return;
            }
            const auto found = findEntry( current, request.endpoint );
            const auto latest = latestMetadataRequest.find( endpointIdentity( request.endpoint ) );
            if ( found == current.entries.end() || found->epoch != request.endpointEpoch
                 || latest == latestMetadataRequest.end()
                 || latest->second != request.requestGeneration ) {
                return;
            }
            found->metadata = std::move( metadata );
            found->error = std::move( error );
            latestMetadataRequest.erase( latest );
        }
        notify();
    }

    mutable std::mutex mutex;
    std::recursive_mutex lifecycleMutex;
    std::recursive_mutex notificationMutex;
    IosNativeApi api;
    IosCatalogExecutor executor;
    IosCatalogSnapshot current;
    Lifecycle lifecycle{ Lifecycle::Stopped };
    Generation nextEpoch{ 0 };
    Generation nextRequestGeneration{ 0 };
    SubscriptionId nextSubscription{ 0 };
    std::vector<CopiedNativeEvent> startupEvents;
    std::map<std::string, Generation> latestMetadataRequest;
    std::map<SubscriptionId, SnapshotCallback> callbacks;
    std::unique_ptr<NativeCallbackContext> callbackContext;
    NativeEventSubscription subscription;
};

IosDeviceCatalog::IosDeviceCatalog( IosNativeApi api, IosCatalogExecutor executor )
    : state_( std::make_shared<State>( api, std::move( executor ) ) )
{
}

IosDeviceCatalog::~IosDeviceCatalog()
{
    stop();
}

bool IosDeviceCatalog::start()
{
    const auto state = state_;
    return state->start();
}

void IosDeviceCatalog::stop()
{
    const auto state = state_;
    state->stop();
}

void IosDeviceCatalog::requestMetadata( IosEndpointKey endpoint )
{
    const auto state = state_;
    if ( !state->executor ) {
        return;
    }
    const auto request = state->beginMetadataRequest( endpoint );
    if ( !request ) {
        return;
    }

    const auto api = state->api;
    const std::weak_ptr<State> weakState = state;
    try {
        state->executor( [ api, weakState, request = *request ] {
            const auto locked = weakState.lock();
            if ( !locked || !locked->isMetadataRequestCurrent( request ) ) {
                return;
            }

            auto fail
                = [ & ]( IosNativeErrorDomain domain, std::int32_t code, const char* detail ) {
                      locked->completeMetadataRequest(
                          request, std::nullopt,
                          classifyIosNativeError( IosNativeError{ domain, code, detail } ) );
                  };

            if ( api.deviceNewWithOptions == nullptr || api.deviceFree == nullptr
                 || api.lockdownClientNew == nullptr || api.lockdownClientFree == nullptr
                 || api.lockdownGetStringValue == nullptr || api.nativeStringFree == nullptr ) {
                fail( IosNativeErrorDomain::Idevice, -1, "native iOS ABI is incomplete" );
                return;
            }

            NativeIdevice rawDevice = nullptr;
            const auto deviceResult
                = api.deviceNewWithOptions( &rawDevice, request.endpoint.udid.c_str(),
                                            connectionOption( request.endpoint.connectionType ) );
            NativeDeviceOwner device( api, rawDevice );
            if ( deviceResult != 0 ) {
                fail( IosNativeErrorDomain::Idevice, deviceResult,
                      "idevice_new_with_options failed" );
                return;
            }
            if ( device.get() == nullptr ) {
                fail( IosNativeErrorDomain::Idevice, -1,
                      "idevice_new_with_options returned a null device" );
                return;
            }
            if ( !locked->isMetadataRequestCurrent( request ) ) {
                return;
            }

            NativeLockdownClient rawLockdown = nullptr;
            const auto lockdownResult
                = api.lockdownClientNew( device.get(), &rawLockdown, "klogg" );
            NativeLockdownOwner lockdown( api, rawLockdown );
            if ( lockdownResult != 0 ) {
                fail( IosNativeErrorDomain::Lockdown, lockdownResult,
                      "lockdownd client creation failed" );
                return;
            }
            if ( lockdown.get() == nullptr ) {
                fail( IosNativeErrorDomain::Lockdown, -1,
                      "lockdownd client creation returned a null client" );
                return;
            }

            IosDeviceMetadata metadata;
            const std::pair<const char*, std::string*> fields[]{
                { "DeviceName", &metadata.displayName },
                { "ProductType", &metadata.productType },
                { "ProductVersion", &metadata.productVersion },
            };
            for ( const auto& field : fields ) {
                if ( !locked->isMetadataRequestCurrent( request ) ) {
                    return;
                }
                char* rawValue = nullptr;
                const auto valueResult
                    = api.lockdownGetStringValue( lockdown.get(), nullptr, field.first, &rawValue );
                NativeStringOwner value( api, rawValue );
                if ( valueResult != 0 ) {
                    fail( IosNativeErrorDomain::Lockdown, valueResult,
                          "lockdownd metadata query failed" );
                    return;
                }
                if ( !locked->isMetadataRequestCurrent( request ) ) {
                    return;
                }
                if ( value.get() != nullptr ) {
                    *field.second = value.get();
                }
            }

            locked->completeMetadataRequest( request, std::move( metadata ), std::nullopt );
        } );
    } catch ( ... ) { // NOLINT(bugprone-empty-catch)
        state->completeMetadataRequest(
            *request, std::nullopt,
            classifyIosNativeError( IosNativeError{ IosNativeErrorDomain::Idevice, -1,
                                                    "metadata executor rejected the task" } ) );
    }
}

IosCatalogSnapshot IosDeviceCatalog::snapshot() const
{
    const auto state = state_;
    std::lock_guard<std::mutex> lock( state->mutex );
    return state->current;
}

IosCatalogSnapshotProvider::SubscriptionId IosDeviceCatalog::subscribe( SnapshotCallback callback )
{
    const auto state = state_;
    std::lock_guard<std::mutex> lock( state->mutex );
    const auto subscriptionId = ++state->nextSubscription;
    state->callbacks.emplace( subscriptionId, std::move( callback ) );
    return subscriptionId;
}

void IosDeviceCatalog::unsubscribe( SubscriptionId subscription )
{
    const auto state = state_;
    std::lock_guard<std::mutex> lock( state->mutex );
    state->callbacks.erase( subscription );
}

void IosDeviceCatalog::nativeEventCallback( const NativeDeviceEvent* event, void* context )
{
    if ( event == nullptr || event->udid == nullptr || context == nullptr
         || !isSupportedConnection( event->connectionType ) ) {
        return;
    }
    try {
        auto* callbackContext = static_cast<State::NativeCallbackContext*>( context );
        const auto state = callbackContext->state.lock();
        if ( !state ) {
            return;
        }
        state->acceptNativeEvent( State::CopiedNativeEvent{
            event->event, IosEndpointKey{ event->udid, event->connectionType } } );
    } catch ( ... ) { // NOLINT(bugprone-empty-catch)
        // No C++ exception may cross the injected native C callback boundary.
    }
}

} // namespace klogg::livecapture::ios
