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

#pragma once

#include <cstddef>
#include <cstdint>

namespace klogg::livecapture::ios {

// The pinned C ABI represents each of these enums as a 32-bit int.
enum class NativeConnectionType : std::int32_t { // NOLINT(performance-enum-size)
    Usb = 1,
    Network = 2
};
enum class NativeConnectionOption : std::int32_t { // NOLINT(performance-enum-size)
    Usb = 1,
    Network = 2
};
enum class NativeEventType : std::int32_t { // NOLINT(performance-enum-size)
    Add = 1,
    Remove = 2,
    Paired = 3
};

struct NativeDeviceInfo {
    const char* udid;
    NativeConnectionType connectionType;
    void* connectionData;
};

struct NativeDeviceEvent {
    NativeEventType event;
    const char* udid;
    NativeConnectionType connectionType;
};

using NativeIdevice = void*;
using NativeLockdownClient = void*;
using NativeServiceDescriptor = void*;
using NativeOsTraceClient = void*;
using NativeSyslogRelayClient = void*;
using NativeEventContext = void*;
using NativeEventCallback = void ( * )( const NativeDeviceEvent*, void* );
using NativeOsTraceActivityCallback = void ( * )( const void*, std::size_t, void* );
using NativeOsTraceErrorCallback = void ( * )( std::int32_t, void* );
using NativeSyslogRelayCallback = void ( * )( char, void* );
using NativeSyslogRelayErrorCallback = void ( * )( std::int32_t, void* );

// The base type is part of the patched vendor helper's C ABI and must stay
// int32_t; do not shrink it.
enum class NativePairRecordResult : std::int32_t { // NOLINT(performance-enum-size)
    Present = 0,
    Missing = 1,
    Error = 2
};

// Source-neutral mirror of the pinned libimobiledevice C ABI used by klogg.
// The table is injected so domain code never includes vendor headers and tests
// never load a daemon, framework, dylib, or real device.
struct IosNativeApi {
    std::int32_t ( *getDeviceListExtended )( NativeDeviceInfo***, std::int32_t* );
    std::int32_t ( *deviceListExtendedFree )( NativeDeviceInfo** );
    std::int32_t ( *eventSubscribe )( NativeEventContext*, NativeEventCallback, void* );
    std::int32_t ( *eventUnsubscribe )( NativeEventContext );
    std::int32_t ( *deviceNewWithOptions )( NativeIdevice*, const char*, NativeConnectionOption );
    std::int32_t ( *deviceFree )( NativeIdevice );
    std::int32_t ( *lockdownClientNew )( NativeIdevice, NativeLockdownClient*, const char* );
    std::int32_t ( *lockdownClientFree )( NativeLockdownClient );
    std::int32_t ( *lockdownStartService )( NativeLockdownClient, const char*,
                                            NativeServiceDescriptor* );
    std::int32_t ( *serviceDescriptorFree )( NativeServiceDescriptor );
    std::int32_t ( *lockdownGetStringValue )( NativeLockdownClient, const char*, const char*,
                                              char** );
    void ( *nativeStringFree )( char* );
    NativePairRecordResult ( *readPairRecord )( const char*, char**, std::uint32_t* );
    void ( *pairRecordFree )( char* );
    std::int32_t ( *lockdownClientNewWithExistingPair )( NativeIdevice, NativeLockdownClient*,
                                                         const char* );
    std::int32_t ( *osTraceClientNew )( NativeIdevice, NativeServiceDescriptor,
                                        NativeOsTraceClient* );
    std::int32_t ( *osTraceStart )( NativeOsTraceClient, NativeOsTraceActivityCallback,
                                    NativeOsTraceErrorCallback, void* );
    std::int32_t ( *osTraceStop )( NativeOsTraceClient );
    std::int32_t ( *osTraceClientFree )( NativeOsTraceClient );
    std::int32_t ( *syslogRelayClientNew )( NativeIdevice, NativeServiceDescriptor,
                                            NativeSyslogRelayClient* );
    std::int32_t ( *syslogRelayStart )( NativeSyslogRelayClient, NativeSyslogRelayCallback,
                                        NativeSyslogRelayErrorCallback, void* );
    std::int32_t ( *syslogRelayStop )( NativeSyslogRelayClient );
    std::int32_t ( *syslogRelayClientFree )( NativeSyslogRelayClient );
};

class NativeDeviceListOwner {
public:
    NativeDeviceListOwner() noexcept = default;
    NativeDeviceListOwner( IosNativeApi api, NativeDeviceInfo** value, std::int32_t size ) noexcept;
    ~NativeDeviceListOwner();

    NativeDeviceListOwner( const NativeDeviceListOwner& ) = delete;
    NativeDeviceListOwner& operator=( const NativeDeviceListOwner& ) = delete;
    NativeDeviceListOwner( NativeDeviceListOwner&& other ) noexcept;
    NativeDeviceListOwner& operator=( NativeDeviceListOwner&& other ) noexcept;

    NativeDeviceInfo** get() const noexcept;
    std::int32_t size() const noexcept;
    void reset() noexcept;

private:
    IosNativeApi api_{};
    NativeDeviceInfo** value_{ nullptr };
    std::int32_t size_{ 0 };
};

class NativeEventSubscription {
public:
    NativeEventSubscription() noexcept = default;
    NativeEventSubscription( IosNativeApi api, NativeEventCallback callback, void* context );
    ~NativeEventSubscription();

    NativeEventSubscription( const NativeEventSubscription& ) = delete;
    NativeEventSubscription& operator=( const NativeEventSubscription& ) = delete;
    NativeEventSubscription( NativeEventSubscription&& other ) noexcept;
    NativeEventSubscription& operator=( NativeEventSubscription&& other ) noexcept;

    bool active() const noexcept;
    bool reset() noexcept;

private:
    IosNativeApi api_{};
    NativeEventContext context_{ nullptr };
    bool active_{ false };
    bool ownsContext_{ false };
};

class NativeDeviceOwner {
public:
    NativeDeviceOwner() noexcept = default;
    NativeDeviceOwner( IosNativeApi api, NativeIdevice value ) noexcept;
    ~NativeDeviceOwner();

    NativeDeviceOwner( const NativeDeviceOwner& ) = delete;
    NativeDeviceOwner& operator=( const NativeDeviceOwner& ) = delete;
    NativeDeviceOwner( NativeDeviceOwner&& other ) noexcept;
    NativeDeviceOwner& operator=( NativeDeviceOwner&& other ) noexcept;

    NativeIdevice get() const noexcept;
    void reset() noexcept;

private:
    IosNativeApi api_{};
    NativeIdevice value_{ nullptr };
};

class NativeLockdownOwner {
public:
    NativeLockdownOwner() noexcept = default;
    NativeLockdownOwner( IosNativeApi api, NativeLockdownClient value ) noexcept;
    ~NativeLockdownOwner();

    NativeLockdownOwner( const NativeLockdownOwner& ) = delete;
    NativeLockdownOwner& operator=( const NativeLockdownOwner& ) = delete;
    NativeLockdownOwner( NativeLockdownOwner&& other ) noexcept;
    NativeLockdownOwner& operator=( NativeLockdownOwner&& other ) noexcept;

    NativeLockdownClient get() const noexcept;
    void reset() noexcept;

private:
    IosNativeApi api_{};
    NativeLockdownClient value_{ nullptr };
};

class NativeServiceOwner {
public:
    NativeServiceOwner() noexcept = default;
    NativeServiceOwner( IosNativeApi api, NativeServiceDescriptor value ) noexcept;
    ~NativeServiceOwner();

    NativeServiceOwner( const NativeServiceOwner& ) = delete;
    NativeServiceOwner& operator=( const NativeServiceOwner& ) = delete;
    NativeServiceOwner( NativeServiceOwner&& other ) noexcept;
    NativeServiceOwner& operator=( NativeServiceOwner&& other ) noexcept;

    NativeServiceDescriptor get() const noexcept;
    void reset() noexcept;

private:
    IosNativeApi api_{};
    NativeServiceDescriptor value_{ nullptr };
};

class NativeStringOwner {
public:
    NativeStringOwner() noexcept = default;
    NativeStringOwner( IosNativeApi api, char* value ) noexcept;
    ~NativeStringOwner();

    NativeStringOwner( const NativeStringOwner& ) = delete;
    NativeStringOwner& operator=( const NativeStringOwner& ) = delete;
    NativeStringOwner( NativeStringOwner&& other ) noexcept;
    NativeStringOwner& operator=( NativeStringOwner&& other ) noexcept;

    char* get() const noexcept;
    void reset() noexcept;

private:
    IosNativeApi api_{};
    char* value_{ nullptr };
};

class NativePairRecordOwner {
public:
    NativePairRecordOwner() noexcept = default;
    NativePairRecordOwner( IosNativeApi api, char* value ) noexcept;
    ~NativePairRecordOwner();

    NativePairRecordOwner( const NativePairRecordOwner& ) = delete;
    NativePairRecordOwner& operator=( const NativePairRecordOwner& ) = delete;
    NativePairRecordOwner( NativePairRecordOwner&& other ) noexcept;
    NativePairRecordOwner& operator=( NativePairRecordOwner&& other ) noexcept;

    char* get() const noexcept;
    void reset() noexcept;

private:
    IosNativeApi api_{};
    char* value_{ nullptr };
};

class NativeOsTraceOwner {
public:
    NativeOsTraceOwner() noexcept = default;
    NativeOsTraceOwner( IosNativeApi api, NativeOsTraceClient value ) noexcept;
    ~NativeOsTraceOwner();

    NativeOsTraceOwner( const NativeOsTraceOwner& ) = delete;
    NativeOsTraceOwner& operator=( const NativeOsTraceOwner& ) = delete;
    NativeOsTraceOwner( NativeOsTraceOwner&& other ) noexcept;
    NativeOsTraceOwner& operator=( NativeOsTraceOwner&& other ) noexcept;

    NativeOsTraceClient get() const noexcept;
    void reset() noexcept;

private:
    IosNativeApi api_{};
    NativeOsTraceClient value_{ nullptr };
};

class NativeSyslogRelayOwner {
public:
    NativeSyslogRelayOwner() noexcept = default;
    NativeSyslogRelayOwner( IosNativeApi api, NativeSyslogRelayClient value ) noexcept;
    ~NativeSyslogRelayOwner();

    NativeSyslogRelayOwner( const NativeSyslogRelayOwner& ) = delete;
    NativeSyslogRelayOwner& operator=( const NativeSyslogRelayOwner& ) = delete;
    NativeSyslogRelayOwner( NativeSyslogRelayOwner&& other ) noexcept;
    NativeSyslogRelayOwner& operator=( NativeSyslogRelayOwner&& other ) noexcept;

    NativeSyslogRelayClient get() const noexcept;
    void reset() noexcept;

private:
    IosNativeApi api_{};
    NativeSyslogRelayClient value_{ nullptr };
};

using NativeOsTraceClientOwner = NativeOsTraceOwner;
using NativeSyslogRelayClientOwner = NativeSyslogRelayOwner;

} // namespace klogg::livecapture::ios
