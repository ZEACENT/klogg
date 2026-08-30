/*
 * Copyright (C) 2026
 *
 * This file is part of klogg.
 *
 * RED contract: this translation unit deliberately names the source-neutral
 * native C-ABI seam and its ownership wrappers before production implements
 * them. It uses no vendor headers and no real device.
 */

#include <catch2/catch.hpp>

#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>

#include "iosnativeadapter.h"
#include "iosnativeapi.h"

namespace {

using namespace klogg::livecapture::ios;

struct NativeApiProbe {
    int listFrees{ 0 };
    std::int32_t subscribeResult{ 0 };
    int subscribeCalls{ 0 };
    int unsubscribeFailuresRemaining{ 0 };
    int unsubscribeCalls{ 0 };
    int deviceFrees{ 0 };
    int lockdownFrees{ 0 };
    int serviceFrees{ 0 };
    int stringFrees{ 0 };
    int osTraceFrees{ 0 };
    int syslogFrees{ 0 };
    bool abiViolation{ false };
    NativeEventCallback callback{ nullptr };
    void* callbackContext{ nullptr };
    NativeEventContext subscriptionContext{ reinterpret_cast<NativeEventContext>( 0x707 ) };
};

NativeApiProbe* probe = nullptr;

std::int32_t getDeviceList( NativeDeviceInfo*** devices, std::int32_t* count )
{
    static NativeDeviceInfo usb{ "usb-udid", NativeConnectionType::Usb, nullptr };
    static NativeDeviceInfo network{ "network-udid", NativeConnectionType::Network, nullptr };
    static NativeDeviceInfo* entries[]{ &usb, &network, nullptr };
    *devices = entries;
    *count = 2;
    return 0;
}

std::int32_t freeDeviceList( NativeDeviceInfo** )
{
    ++probe->listFrees;
    return 0;
}

std::int32_t subscribe( NativeEventContext* subscriptionContext, NativeEventCallback callback,
                        void* callbackContext )
{
    ++probe->subscribeCalls;
    probe->callback = callback;
    probe->callbackContext = callbackContext;
    *subscriptionContext = probe->subscriptionContext;
    return probe->subscribeResult;
}

std::int32_t unsubscribe( NativeEventContext subscriptionContext )
{
    if ( subscriptionContext != probe->subscriptionContext ) {
        probe->abiViolation = true;
    }
    ++probe->unsubscribeCalls;
    if ( probe->unsubscribeFailuresRemaining > 0 ) {
        --probe->unsubscribeFailuresRemaining;
        return -1;
    }
    return 0;
}

std::int32_t newDevice( NativeIdevice* device, const char*, NativeConnectionOption )
{
    *device = reinterpret_cast<NativeIdevice>( 0x101 );
    return 0;
}

std::int32_t freeDevice( NativeIdevice device )
{
    if ( device != reinterpret_cast<NativeIdevice>( 0x101 ) ) {
        probe->abiViolation = true;
    }
    ++probe->deviceFrees;
    return 0;
}

std::int32_t newLockdown( NativeIdevice, NativeLockdownClient* client, const char* )
{
    *client = reinterpret_cast<NativeLockdownClient>( 0x202 );
    return 0;
}

std::int32_t freeLockdown( NativeLockdownClient client )
{
    if ( client != reinterpret_cast<NativeLockdownClient>( 0x202 ) ) {
        probe->abiViolation = true;
    }
    ++probe->lockdownFrees;
    return 0;
}

std::int32_t startService( NativeLockdownClient, const char*, NativeServiceDescriptor* service )
{
    *service = reinterpret_cast<NativeServiceDescriptor>( 0x303 );
    return 0;
}

std::int32_t freeService( NativeServiceDescriptor service )
{
    if ( service != reinterpret_cast<NativeServiceDescriptor>( 0x303 ) ) {
        probe->abiViolation = true;
    }
    ++probe->serviceFrees;
    return 0;
}

std::int32_t getStringValue( NativeLockdownClient, const char*, const char*, char** )
{
    return 0;
}

void freeNativeString( char* value )
{
    if ( value != reinterpret_cast<char*>( 0x404 ) ) {
        probe->abiViolation = true;
    }
    ++probe->stringFrees;
}

std::int32_t freeOsTrace( NativeOsTraceClient client )
{
    if ( client != reinterpret_cast<NativeOsTraceClient>( 0x505 ) ) {
        probe->abiViolation = true;
    }
    ++probe->osTraceFrees;
    return 0;
}

std::int32_t freeSyslog( NativeSyslogRelayClient client )
{
    if ( client != reinterpret_cast<NativeSyslogRelayClient>( 0x606 ) ) {
        probe->abiViolation = true;
    }
    ++probe->syslogFrees;
    return 0;
}

IosNativeApi makeApi()
{
    IosNativeApi api{};
    api.getDeviceListExtended = &getDeviceList;
    api.deviceListExtendedFree = &freeDeviceList;
    api.eventSubscribe = &subscribe;
    api.eventUnsubscribe = &unsubscribe;
    api.deviceNewWithOptions = &newDevice;
    api.deviceFree = &freeDevice;
    api.lockdownClientNew = &newLockdown;
    api.lockdownClientFree = &freeLockdown;
    api.lockdownStartService = &startService;
    api.serviceDescriptorFree = &freeService;
    api.lockdownGetStringValue = &getStringValue;
    api.nativeStringFree = &freeNativeString;
    api.osTraceClientFree = &freeOsTrace;
    api.syslogRelayClientFree = &freeSyslog;
    return api;
}

void ignoreEvent( const NativeDeviceEvent*, void* ) {}

using ExpectedDeviceListFree = std::int32_t ( * )( NativeDeviceInfo** );
using ExpectedReadPairRecord = NativePairRecordResult ( * )( const char*, char**, std::uint32_t* );
using ExpectedPairRecordFree = void ( * )( char* );
using ExpectedHandshakeNew
    = std::int32_t ( * )( NativeIdevice, NativeLockdownClient*, const char* );
using ExpectedOsTraceNew
    = std::int32_t ( * )( NativeIdevice, NativeServiceDescriptor, NativeOsTraceClient* );
using ExpectedOsTraceStart = std::int32_t ( * )( NativeOsTraceClient, NativeOsTraceActivityCallback,
                                                 NativeOsTraceErrorCallback, void* );
using ExpectedOsTraceStop = std::int32_t ( * )( NativeOsTraceClient );
using ExpectedSyslogNew
    = std::int32_t ( * )( NativeIdevice, NativeServiceDescriptor, NativeSyslogRelayClient* );
using ExpectedSyslogStart = std::int32_t ( * )( NativeSyslogRelayClient, NativeSyslogRelayCallback,
                                                NativeSyslogRelayErrorCallback, void* );
using ExpectedSyslogStop = std::int32_t ( * )( NativeSyslogRelayClient );

template <typename T, typename = void>
struct HasPairMutationApi : std::false_type {};

template <typename T>
struct HasPairMutationApi<T, std::void_t<decltype( T::pairDevice ), decltype( T::unpairDevice )>>
    : std::true_type {};

} // namespace

TEST_CASE( "native iOS ABI is injectable and contains every owned C resource",
           "[ios][native][abi][contract]" )
{
    static_assert( std::is_aggregate_v<IosNativeApi> );
    static_assert( !HasPairMutationApi<IosNativeApi>::value,
                   "native capture ABI must make pair/unpair unreachable" );
    static_assert( std::is_pointer_v<decltype( IosNativeApi::getDeviceListExtended )> );
    static_assert(
        std::is_same_v<decltype( IosNativeApi::deviceListExtendedFree ), ExpectedDeviceListFree> );
    static_assert( std::is_pointer_v<decltype( IosNativeApi::eventSubscribe )> );
    static_assert( std::is_pointer_v<decltype( IosNativeApi::eventUnsubscribe )> );
    static_assert( std::is_pointer_v<decltype( IosNativeApi::deviceNewWithOptions )> );
    static_assert( std::is_pointer_v<decltype( IosNativeApi::deviceFree )> );
    static_assert( std::is_pointer_v<decltype( IosNativeApi::lockdownClientNew )> );
    static_assert( std::is_pointer_v<decltype( IosNativeApi::lockdownClientFree )> );
    static_assert( std::is_pointer_v<decltype( IosNativeApi::lockdownStartService )> );
    static_assert( std::is_pointer_v<decltype( IosNativeApi::serviceDescriptorFree )> );
    static_assert( std::is_pointer_v<decltype( IosNativeApi::lockdownGetStringValue )> );
    static_assert( std::is_pointer_v<decltype( IosNativeApi::nativeStringFree )> );
    static_assert(
        std::is_same_v<decltype( IosNativeApi::readPairRecord ), ExpectedReadPairRecord> );
    static_assert(
        std::is_same_v<decltype( IosNativeApi::pairRecordFree ), ExpectedPairRecordFree> );
    static_assert( std::is_same_v<decltype( IosNativeApi::lockdownClientNewWithExistingPair ),
                                  ExpectedHandshakeNew> );
    static_assert( std::is_same_v<decltype( IosNativeApi::osTraceClientNew ), ExpectedOsTraceNew> );
    static_assert( std::is_same_v<decltype( IosNativeApi::osTraceStart ), ExpectedOsTraceStart> );
    static_assert( std::is_same_v<decltype( IosNativeApi::osTraceStop ), ExpectedOsTraceStop> );
    static_assert(
        std::is_same_v<decltype( IosNativeApi::osTraceClientFree ), ExpectedOsTraceStop> );
    static_assert(
        std::is_same_v<decltype( IosNativeApi::syslogRelayClientNew ), ExpectedSyslogNew> );
    static_assert(
        std::is_same_v<decltype( IosNativeApi::syslogRelayStart ), ExpectedSyslogStart> );
    static_assert( std::is_same_v<decltype( IosNativeApi::syslogRelayStop ), ExpectedSyslogStop> );
    static_assert(
        std::is_same_v<decltype( IosNativeApi::syslogRelayClientFree ), ExpectedSyslogStop> );
    static_assert( std::is_same_v<std::underlying_type_t<NativeConnectionType>, std::int32_t> );
    static_assert( std::is_same_v<std::underlying_type_t<NativeConnectionOption>, std::int32_t> );
    static_assert( std::is_same_v<std::underlying_type_t<NativeEventType>, std::int32_t> );
    static_assert( std::is_same_v<std::underlying_type_t<NativePairRecordResult>, std::int32_t> );
    static_assert( static_cast<std::int32_t>( NativeConnectionType::Usb ) == 1 );
    static_assert( static_cast<std::int32_t>( NativeConnectionType::Network ) == 2 );
    static_assert( static_cast<std::int32_t>( NativeConnectionOption::Usb ) == 1 );
    static_assert( static_cast<std::int32_t>( NativeConnectionOption::Network ) == 2 );
    static_assert( static_cast<std::int32_t>( NativeEventType::Add ) == 1 );
    static_assert( static_cast<std::int32_t>( NativeEventType::Remove ) == 2 );
    static_assert( static_cast<std::int32_t>( NativeEventType::Paired ) == 3 );

    const auto api = makeApi();
    CHECK( api.getDeviceListExtended != nullptr );
    CHECK( api.deviceNewWithOptions != nullptr );
    CHECK( api.lockdownStartService != nullptr );
}

TEST_CASE( "native iOS RAII owners are move-only and release exactly once",
           "[ios][native][raii][ownership]" )
{
    static_assert( !std::is_copy_constructible_v<NativeDeviceListOwner> );
    static_assert( std::is_nothrow_move_constructible_v<NativeDeviceListOwner> );
    static_assert( !std::is_copy_constructible_v<NativeEventSubscription> );
    static_assert( std::is_nothrow_move_constructible_v<NativeEventSubscription> );
    static_assert( !std::is_copy_constructible_v<NativeDeviceOwner> );
    static_assert( std::is_nothrow_move_constructible_v<NativeDeviceOwner> );
    static_assert( !std::is_copy_constructible_v<NativeLockdownOwner> );
    static_assert( std::is_nothrow_move_constructible_v<NativeLockdownOwner> );
    static_assert( !std::is_copy_constructible_v<NativeServiceOwner> );
    static_assert( std::is_nothrow_move_constructible_v<NativeServiceOwner> );
    static_assert( !std::is_copy_constructible_v<NativeStringOwner> );
    static_assert( std::is_nothrow_move_constructible_v<NativeStringOwner> );
    static_assert( !std::is_copy_constructible_v<NativeOsTraceClientOwner> );
    static_assert( std::is_nothrow_move_constructible_v<NativeOsTraceClientOwner> );
    static_assert( !std::is_copy_constructible_v<NativeSyslogRelayClientOwner> );
    static_assert( std::is_nothrow_move_constructible_v<NativeSyslogRelayClientOwner> );

    NativeApiProbe state;
    probe = &state;
    const auto api = makeApi();

    NativeDeviceInfo** rawList = nullptr;
    std::int32_t count = 0;
    REQUIRE( api.getDeviceListExtended( &rawList, &count ) == 0 );
    {
        NativeDeviceListOwner first( api, rawList, count );
        REQUIRE( first.size() == 2 );
        CHECK( first.get()[ 0 ]->connectionType == NativeConnectionType::Usb );
        CHECK( first.get()[ 1 ]->connectionType == NativeConnectionType::Network );
        NativeDeviceListOwner second( std::move( first ) );
        CHECK( first.get() == nullptr ); // NOLINT(bugprone-use-after-move)
        CHECK( second.get() == rawList );
    }
    CHECK( state.listFrees == 1 );

    {
        NativeEventSubscription first( api, &ignoreEvent, &state );
        REQUIRE( first.active() );
        NativeEventSubscription second( std::move( first ) );
        CHECK_FALSE( first.active() ); // NOLINT(bugprone-use-after-move)
        CHECK( second.active() );
    }
    CHECK( state.subscribeCalls == 1 );
    CHECK( state.unsubscribeCalls == 1 );

    {
        NativeDeviceOwner first( api, reinterpret_cast<NativeIdevice>( 0x101 ) );
        NativeDeviceOwner second( std::move( first ) );
        CHECK( first.get() == nullptr ); // NOLINT(bugprone-use-after-move)
        CHECK( second.get() == reinterpret_cast<NativeIdevice>( 0x101 ) );
    }
    CHECK( state.deviceFrees == 1 );

    {
        NativeLockdownOwner first( api, reinterpret_cast<NativeLockdownClient>( 0x202 ) );
        NativeLockdownOwner second( std::move( first ) );
        CHECK( first.get() == nullptr ); // NOLINT(bugprone-use-after-move)
        CHECK( second.get() == reinterpret_cast<NativeLockdownClient>( 0x202 ) );
    }
    CHECK( state.lockdownFrees == 1 );

    {
        NativeServiceOwner first( api, reinterpret_cast<NativeServiceDescriptor>( 0x303 ) );
        NativeServiceOwner second( std::move( first ) );
        CHECK( first.get() == nullptr ); // NOLINT(bugprone-use-after-move)
        CHECK( second.get() == reinterpret_cast<NativeServiceDescriptor>( 0x303 ) );
    }
    CHECK( state.serviceFrees == 1 );

    {
        NativeStringOwner first( api, reinterpret_cast<char*>( 0x404 ) );
        NativeStringOwner second( std::move( first ) );
        CHECK( first.get() == nullptr ); // NOLINT(bugprone-use-after-move)
        CHECK( second.get() == reinterpret_cast<char*>( 0x404 ) );
        second.reset();
        CHECK( second.get() == nullptr );
    }
    CHECK( state.stringFrees == 1 );

    {
        NativeOsTraceClientOwner first( api, reinterpret_cast<NativeOsTraceClient>( 0x505 ) );
        NativeOsTraceClientOwner second( std::move( first ) );
        CHECK( first.get() == nullptr ); // NOLINT(bugprone-use-after-move)
        CHECK( second.get() == reinterpret_cast<NativeOsTraceClient>( 0x505 ) );
    }
    CHECK( state.osTraceFrees == 1 );

    {
        NativeSyslogRelayClientOwner first( api,
                                            reinterpret_cast<NativeSyslogRelayClient>( 0x606 ) );
        NativeSyslogRelayClientOwner second( std::move( first ) );
        CHECK( first.get() == nullptr ); // NOLINT(bugprone-use-after-move)
        CHECK( second.get() == reinterpret_cast<NativeSyslogRelayClient>( 0x606 ) );
    }
    CHECK( state.syslogFrees == 1 );
    CHECK_FALSE( state.abiViolation );
}

TEST_CASE( "native event subscription releases a partial context when subscribe fails",
           "[ios][native][raii][partial-failure]" )
{
    NativeApiProbe state;
    state.subscribeResult = -1;
    probe = &state;

    NativeEventSubscription subscription( makeApi(), &ignoreEvent, &state );

    CHECK_FALSE( subscription.active() );
    CHECK( state.subscribeCalls == 1 );
    CHECK( state.unsubscribeCalls == 1 );
}

TEST_CASE( "native event subscription retains ownership when unsubscribe fails",
           "[ios][native][raii][unsubscribe][retry]" )
{
    NativeApiProbe state;
    state.unsubscribeFailuresRemaining = 1;
    probe = &state;
    NativeEventSubscription subscription( makeApi(), &ignoreEvent, &state );
    REQUIRE( subscription.active() );

    CHECK_FALSE( subscription.reset() );
    CHECK( subscription.active() );
    CHECK( state.unsubscribeCalls == 1 );

    CHECK( subscription.reset() );
    CHECK_FALSE( subscription.active() );
    CHECK( state.unsubscribeCalls == 2 );
}

TEST_CASE( "native event subscription can be reset before dependent state is destroyed",
           "[ios][native][raii][shutdown]" )
{
    NativeApiProbe state;
    probe = &state;
    NativeEventSubscription subscription( makeApi(), &ignoreEvent, &state );
    REQUIRE( subscription.active() );

    subscription.reset();
    CHECK_FALSE( subscription.active() );
    CHECK( state.unsubscribeCalls == 1 );

    subscription.reset();
    CHECK( state.unsubscribeCalls == 1 );
}

TEST_CASE( "native iOS bundle loader rejects empty and relative roots before loading dylibs",
           "[ios][native][adapter][bundle-identity]" )
{
    for ( const std::string& root : { std::string{}, std::string{ "relative-stack" } } ) {
        std::string error;
        const auto api = loadIosNativeApiFromBundle( root, &error );
        CHECK( api.getDeviceListExtended == nullptr );
#ifdef __APPLE__
        CHECK( error.find( "absolute path" ) != std::string::npos );
#else
        CHECK_FALSE( error.empty() );
#endif
    }
}
