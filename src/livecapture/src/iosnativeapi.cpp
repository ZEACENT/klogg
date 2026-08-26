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

#include "iosnativeapi.h"

#include <utility>

namespace klogg::livecapture::ios {

NativeDeviceListOwner::NativeDeviceListOwner( IosNativeApi api, NativeDeviceInfo** value,
                                              std::int32_t size ) noexcept
    : api_( api )
    , value_( value )
    , size_( size )
{
}

NativeDeviceListOwner::~NativeDeviceListOwner()
{
    reset();
}

NativeDeviceListOwner::NativeDeviceListOwner( NativeDeviceListOwner&& other ) noexcept
    : api_( other.api_ )
    , value_( std::exchange( other.value_, nullptr ) )
    , size_( std::exchange( other.size_, 0 ) )
{
}

NativeDeviceListOwner& NativeDeviceListOwner::operator=( NativeDeviceListOwner&& other ) noexcept
{
    if ( this != &other ) {
        reset();
        api_ = other.api_;
        value_ = std::exchange( other.value_, nullptr );
        size_ = std::exchange( other.size_, 0 );
    }
    return *this;
}

NativeDeviceInfo** NativeDeviceListOwner::get() const noexcept
{
    return value_;
}

std::int32_t NativeDeviceListOwner::size() const noexcept
{
    return size_;
}

void NativeDeviceListOwner::reset() noexcept
{
    if ( value_ != nullptr && api_.deviceListExtendedFree != nullptr ) {
        api_.deviceListExtendedFree( value_ );
    }
    value_ = nullptr;
    size_ = 0;
}

NativeEventSubscription::NativeEventSubscription( IosNativeApi api, NativeEventCallback callback,
                                                  void* context )
    : api_( api )
{
    if ( api_.eventSubscribe == nullptr ) {
        return;
    }
    const auto result = api_.eventSubscribe( &context_, callback, context );
    ownsContext_ = context_ != nullptr;
    if ( result == 0 && ownsContext_ ) {
        active_ = true;
        return;
    }
    // A defensive adapter can return a partial context on failure. Keep it
    // owned until unsubscribe confirms that no callback can use it, without
    // reporting the failed subscription as active.
    static_cast<void>( reset() );
}

NativeEventSubscription::~NativeEventSubscription()
{
    reset();
}

NativeEventSubscription::NativeEventSubscription( NativeEventSubscription&& other ) noexcept
    : api_( other.api_ )
    , context_( std::exchange( other.context_, nullptr ) )
    , active_( std::exchange( other.active_, false ) )
    , ownsContext_( std::exchange( other.ownsContext_, false ) )
{
}

NativeEventSubscription&
NativeEventSubscription::operator=( NativeEventSubscription&& other ) noexcept
{
    if ( this != &other ) {
        if ( !reset() ) {
            return *this;
        }
        api_ = other.api_;
        context_ = std::exchange( other.context_, nullptr );
        active_ = std::exchange( other.active_, false );
        ownsContext_ = std::exchange( other.ownsContext_, false );
    }
    return *this;
}

bool NativeEventSubscription::active() const noexcept
{
    return active_;
}

bool NativeEventSubscription::reset() noexcept
{
    if ( !ownsContext_ ) {
        active_ = false;
        context_ = nullptr;
        return true;
    }
    if ( api_.eventUnsubscribe == nullptr || api_.eventUnsubscribe( context_ ) != 0 ) {
        return false;
    }
    active_ = false;
    ownsContext_ = false;
    context_ = nullptr;
    return true;
}

NativeDeviceOwner::NativeDeviceOwner( IosNativeApi api, NativeIdevice value ) noexcept
    : api_( api )
    , value_( value )
{
}

NativeDeviceOwner::~NativeDeviceOwner()
{
    reset();
}

NativeDeviceOwner::NativeDeviceOwner( NativeDeviceOwner&& other ) noexcept
    : api_( other.api_ )
    , value_( std::exchange( other.value_, nullptr ) )
{
}

NativeDeviceOwner& NativeDeviceOwner::operator=( NativeDeviceOwner&& other ) noexcept
{
    if ( this != &other ) {
        reset();
        api_ = other.api_;
        value_ = std::exchange( other.value_, nullptr );
    }
    return *this;
}

NativeIdevice NativeDeviceOwner::get() const noexcept
{
    return value_;
}

void NativeDeviceOwner::reset() noexcept
{
    if ( value_ != nullptr && api_.deviceFree != nullptr ) {
        api_.deviceFree( value_ );
    }
    value_ = nullptr;
}

NativeLockdownOwner::NativeLockdownOwner( IosNativeApi api, NativeLockdownClient value ) noexcept
    : api_( api )
    , value_( value )
{
}

NativeLockdownOwner::~NativeLockdownOwner()
{
    reset();
}

NativeLockdownOwner::NativeLockdownOwner( NativeLockdownOwner&& other ) noexcept
    : api_( other.api_ )
    , value_( std::exchange( other.value_, nullptr ) )
{
}

NativeLockdownOwner& NativeLockdownOwner::operator=( NativeLockdownOwner&& other ) noexcept
{
    if ( this != &other ) {
        reset();
        api_ = other.api_;
        value_ = std::exchange( other.value_, nullptr );
    }
    return *this;
}

NativeLockdownClient NativeLockdownOwner::get() const noexcept
{
    return value_;
}

void NativeLockdownOwner::reset() noexcept
{
    if ( value_ != nullptr && api_.lockdownClientFree != nullptr ) {
        api_.lockdownClientFree( value_ );
    }
    value_ = nullptr;
}

NativeServiceOwner::NativeServiceOwner( IosNativeApi api, NativeServiceDescriptor value ) noexcept
    : api_( api )
    , value_( value )
{
}

NativeServiceOwner::~NativeServiceOwner()
{
    reset();
}

NativeServiceOwner::NativeServiceOwner( NativeServiceOwner&& other ) noexcept
    : api_( other.api_ )
    , value_( std::exchange( other.value_, nullptr ) )
{
}

NativeServiceOwner& NativeServiceOwner::operator=( NativeServiceOwner&& other ) noexcept
{
    if ( this != &other ) {
        reset();
        api_ = other.api_;
        value_ = std::exchange( other.value_, nullptr );
    }
    return *this;
}

NativeServiceDescriptor NativeServiceOwner::get() const noexcept
{
    return value_;
}

void NativeServiceOwner::reset() noexcept
{
    if ( value_ != nullptr && api_.serviceDescriptorFree != nullptr ) {
        api_.serviceDescriptorFree( value_ );
    }
    value_ = nullptr;
}

NativeStringOwner::NativeStringOwner( IosNativeApi api, char* value ) noexcept
    : api_( api )
    , value_( value )
{
}

NativeStringOwner::~NativeStringOwner()
{
    reset();
}

NativeStringOwner::NativeStringOwner( NativeStringOwner&& other ) noexcept
    : api_( other.api_ )
    , value_( std::exchange( other.value_, nullptr ) )
{
}

NativeStringOwner& NativeStringOwner::operator=( NativeStringOwner&& other ) noexcept
{
    if ( this != &other ) {
        reset();
        api_ = other.api_;
        value_ = std::exchange( other.value_, nullptr );
    }
    return *this;
}

char* NativeStringOwner::get() const noexcept
{
    return value_;
}

void NativeStringOwner::reset() noexcept
{
    if ( value_ != nullptr && api_.nativeStringFree != nullptr ) {
        api_.nativeStringFree( value_ );
    }
    value_ = nullptr;
}

NativePairRecordOwner::NativePairRecordOwner( IosNativeApi api, char* value ) noexcept
    : api_( api )
    , value_( value )
{
}

NativePairRecordOwner::~NativePairRecordOwner()
{
    reset();
}

NativePairRecordOwner::NativePairRecordOwner( NativePairRecordOwner&& other ) noexcept
    : api_( other.api_ )
    , value_( std::exchange( other.value_, nullptr ) )
{
}

NativePairRecordOwner& NativePairRecordOwner::operator=( NativePairRecordOwner&& other ) noexcept
{
    if ( this != &other ) {
        reset();
        api_ = other.api_;
        value_ = std::exchange( other.value_, nullptr );
    }
    return *this;
}

char* NativePairRecordOwner::get() const noexcept
{
    return value_;
}

void NativePairRecordOwner::reset() noexcept
{
    if ( value_ != nullptr && api_.pairRecordFree != nullptr ) {
        api_.pairRecordFree( value_ );
    }
    value_ = nullptr;
}

NativeOsTraceOwner::NativeOsTraceOwner( IosNativeApi api, NativeOsTraceClient value ) noexcept
    : api_( api )
    , value_( value )
{
}

NativeOsTraceOwner::~NativeOsTraceOwner()
{
    reset();
}

NativeOsTraceOwner::NativeOsTraceOwner( NativeOsTraceOwner&& other ) noexcept
    : api_( other.api_ )
    , value_( std::exchange( other.value_, nullptr ) )
{
}

NativeOsTraceOwner& NativeOsTraceOwner::operator=( NativeOsTraceOwner&& other ) noexcept
{
    if ( this != &other ) {
        reset();
        api_ = other.api_;
        value_ = std::exchange( other.value_, nullptr );
    }
    return *this;
}

NativeOsTraceClient NativeOsTraceOwner::get() const noexcept
{
    return value_;
}

void NativeOsTraceOwner::reset() noexcept
{
    if ( value_ != nullptr && api_.osTraceClientFree != nullptr ) {
        api_.osTraceClientFree( value_ );
    }
    value_ = nullptr;
}

NativeSyslogRelayOwner::NativeSyslogRelayOwner( IosNativeApi api,
                                                NativeSyslogRelayClient value ) noexcept
    : api_( api )
    , value_( value )
{
}

NativeSyslogRelayOwner::~NativeSyslogRelayOwner()
{
    reset();
}

NativeSyslogRelayOwner::NativeSyslogRelayOwner( NativeSyslogRelayOwner&& other ) noexcept
    : api_( other.api_ )
    , value_( std::exchange( other.value_, nullptr ) )
{
}

NativeSyslogRelayOwner&
NativeSyslogRelayOwner::operator=( NativeSyslogRelayOwner&& other ) noexcept
{
    if ( this != &other ) {
        reset();
        api_ = other.api_;
        value_ = std::exchange( other.value_, nullptr );
    }
    return *this;
}

NativeSyslogRelayClient NativeSyslogRelayOwner::get() const noexcept
{
    return value_;
}

void NativeSyslogRelayOwner::reset() noexcept
{
    if ( value_ != nullptr && api_.syslogRelayClientFree != nullptr ) {
        api_.syslogRelayClientFree( value_ );
    }
    value_ = nullptr;
}

} // namespace klogg::livecapture::ios
