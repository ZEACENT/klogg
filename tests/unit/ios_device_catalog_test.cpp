/*
 * Copyright (C) 2026
 *
 * This file is part of klogg.
 *
 * RED contract for native iOS discovery. All native behavior is injected; no
 * vendor library, daemon, framework, network, USB device, Qt event loop, timer,
 * or wall clock participates in these tests.
 */

#include <catch2/catch.hpp>

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "ioscatalogprovider.h"
#include "iosdevicecatalog.h"
#include "iosnativeapi.h"

namespace {

using namespace klogg::livecapture;
using namespace klogg::livecapture::ios;

std::string endpointId( const std::string& udid, NativeConnectionType connection )
{
    return udid + ( connection == NativeConnectionType::Usb ? "@usb" : "@network" );
}

struct FakeDevice {
    std::string udid;
    NativeConnectionType connection{ NativeConnectionType::Usb };
};

struct FakeLockdown {
    FakeDevice* device{ nullptr };
};

struct FakeNative {
    struct ListedEndpoint {
        std::string udid;
        NativeConnectionType connection{ NativeConnectionType::Usb };
    };

    struct MetadataResult {
        std::int32_t error{ 0 };
        std::string name;
        std::string productType;
        std::string productVersion;
    };

    std::vector<ListedEndpoint> listed;
    std::vector<NativeDeviceInfo> nativeInfos;
    std::vector<NativeDeviceInfo*> nativeInfoPointers;
    std::vector<NativeDeviceEvent> synchronousSubscribeEvents;
    std::optional<NativeDeviceEvent> unsubscribeEvent;
    std::int32_t listResult{ 0 };
    std::int32_t subscribeResult{ 0 };
    std::int32_t deviceNewResult{ 0 };
    std::int32_t lockdownNewResult{ 0 };
    std::optional<int> stringFailureCall;
    bool nullListOnSuccess{ false };
    bool nullDeviceOnSuccess{ false };
    bool nullLockdownOnSuccess{ false };
    bool allocateDeviceOnError{ false };
    bool allocateLockdownOnError{ false };
    bool allocateStringOnError{ false };
    bool insideNativeCallback{ false };
    std::function<void( int )> onStringCall;
    NativeEventCallback callback{ nullptr };
    void* callbackContext{ nullptr };
    NativeEventContext subscriptionContext{ reinterpret_cast<NativeEventContext>( 0x707 ) };
    std::map<std::string, MetadataResult> metadata;
    std::vector<NativeConnectionOption> connectionOptions;
    std::vector<std::thread::id> metadataThreads;
    int listFreeCalls{ 0 };
    int subscribeCalls{ 0 };
    int unsubscribeCalls{ 0 };
    int deviceNewCalls{ 0 };
    int deviceFreeCalls{ 0 };
    int lockdownNewCalls{ 0 };
    int lockdownFreeCalls{ 0 };
    int stringCalls{ 0 };
    int stringFreeCalls{ 0 };
    int serviceFreeCalls{ 0 };

    void emit( NativeEventType event, const std::string& udid, NativeConnectionType connection )
    {
        const auto activeCallback = callback;
        const auto activeContext = callbackContext;
        if ( activeCallback == nullptr ) {
            return;
        }
        NativeDeviceEvent value{ event, udid.c_str(), connection };
        insideNativeCallback = true;
        activeCallback( &value, activeContext );
        insideNativeCallback = false;
    }
};

FakeNative* fake = nullptr;

std::int32_t fakeGetDeviceList( NativeDeviceInfo*** devices, std::int32_t* count )
{
    fake->nativeInfos.clear();
    fake->nativeInfos.reserve( fake->listed.size() );
    for ( const auto& endpoint : fake->listed ) {
        fake->nativeInfos.push_back(
            NativeDeviceInfo{ endpoint.udid.c_str(), endpoint.connection, nullptr } );
    }
    fake->nativeInfoPointers.clear();
    for ( auto& info : fake->nativeInfos ) {
        fake->nativeInfoPointers.push_back( &info );
    }
    fake->nativeInfoPointers.push_back( nullptr );
    *devices = fake->nullListOnSuccess ? nullptr : fake->nativeInfoPointers.data();
    *count = static_cast<std::int32_t>( fake->nativeInfos.size() );
    return fake->listResult;
}

std::int32_t fakeFreeDeviceList( NativeDeviceInfo** )
{
    ++fake->listFreeCalls;
    return 0;
}

std::int32_t fakeSubscribe( NativeEventContext* subscriptionContext, NativeEventCallback callback,
                            void* callbackContext )
{
    ++fake->subscribeCalls;
    fake->callback = callback;
    fake->callbackContext = callbackContext;
    *subscriptionContext = fake->subscriptionContext;
    for ( const auto& event : fake->synchronousSubscribeEvents ) {
        fake->insideNativeCallback = true;
        callback( &event, callbackContext );
        fake->insideNativeCallback = false;
    }
    return fake->subscribeResult;
}

std::int32_t fakeUnsubscribe( NativeEventContext subscriptionContext )
{
    ++fake->unsubscribeCalls;
    REQUIRE( subscriptionContext == fake->subscriptionContext );
    if ( fake->unsubscribeEvent ) {
        fake->callback( &*fake->unsubscribeEvent, fake->callbackContext );
    }
    fake->callback = nullptr;
    fake->callbackContext = nullptr;
    return 0;
}

std::int32_t fakeNewDevice( NativeIdevice* raw, const char* udid, NativeConnectionOption option )
{
    const auto connection = option == NativeConnectionOption::Usb ? NativeConnectionType::Usb
                                                                  : NativeConnectionType::Network;
    ++fake->deviceNewCalls;
    fake->connectionOptions.push_back( option );
    if ( !fake->nullDeviceOnSuccess
         && ( fake->deviceNewResult == 0 || fake->allocateDeviceOnError ) ) {
        *raw = new FakeDevice{ udid, connection };
    }
    return fake->deviceNewResult;
}

std::int32_t fakeFreeDevice( NativeIdevice raw )
{
    delete static_cast<FakeDevice*>( raw );
    ++fake->deviceFreeCalls;
    return 0;
}

std::int32_t fakeNewLockdown( NativeIdevice raw, NativeLockdownClient* client, const char* )
{
    ++fake->lockdownNewCalls;
    if ( !fake->nullLockdownOnSuccess
         && ( fake->lockdownNewResult == 0 || fake->allocateLockdownOnError ) ) {
        *client = new FakeLockdown{ static_cast<FakeDevice*>( raw ) };
    }
    return fake->lockdownNewResult;
}

std::int32_t fakeFreeLockdown( NativeLockdownClient client )
{
    delete static_cast<FakeLockdown*>( client );
    ++fake->lockdownFreeCalls;
    return 0;
}

std::int32_t fakeStartService( NativeLockdownClient, const char*, NativeServiceDescriptor* service )
{
    *service = reinterpret_cast<NativeServiceDescriptor>( 0x404 );
    return 0;
}

std::int32_t fakeFreeService( NativeServiceDescriptor )
{
    ++fake->serviceFreeCalls;
    return 0;
}

std::int32_t fakeGetStringValue( NativeLockdownClient client, const char*, const char* key,
                                 char** value )
{
    fake->metadataThreads.push_back( std::this_thread::get_id() );
    const auto call = ++fake->stringCalls;
    if ( fake->onStringCall ) {
        fake->onStringCall( call );
    }
    const auto* lockdown = static_cast<FakeLockdown*>( client );
    const auto id = endpointId( lockdown->device->udid, lockdown->device->connection );
    const auto result = fake->metadata.at( id );
    if ( result.error != 0 || fake->stringFailureCall == call ) {
        if ( fake->allocateStringOnError ) {
            auto owned = std::make_unique<char[]>( 8 );
            std::memcpy( owned.get(), "partial", 8 );
            *value = owned.release();
        }
        return result.error != 0 ? result.error : -3;
    }

    std::string selected;
    if ( std::strcmp( key, "DeviceName" ) == 0 ) {
        selected = result.name;
    }
    else if ( std::strcmp( key, "ProductType" ) == 0 ) {
        selected = result.productType;
    }
    else if ( std::strcmp( key, "ProductVersion" ) == 0 ) {
        selected = result.productVersion;
    }
    else {
        return -1;
    }

    auto owned = std::make_unique<char[]>( selected.size() + 1 );
    std::memcpy( owned.get(), selected.c_str(), selected.size() + 1 );
    *value = owned.release();
    return 0;
}

void fakeFreeString( char* value )
{
    delete[] value;
    ++fake->stringFreeCalls;
}

IosNativeApi makeApi()
{
    IosNativeApi api{};
    api.getDeviceListExtended = &fakeGetDeviceList;
    api.deviceListExtendedFree = &fakeFreeDeviceList;
    api.eventSubscribe = &fakeSubscribe;
    api.eventUnsubscribe = &fakeUnsubscribe;
    api.deviceNewWithOptions = &fakeNewDevice;
    api.deviceFree = &fakeFreeDevice;
    api.lockdownClientNew = &fakeNewLockdown;
    api.lockdownClientFree = &fakeFreeLockdown;
    api.lockdownStartService = &fakeStartService;
    api.serviceDescriptorFree = &fakeFreeService;
    api.lockdownGetStringValue = &fakeGetStringValue;
    api.nativeStringFree = &fakeFreeString;
    return api;
}

class ManualExecutor {
public:
    IosCatalogExecutor executor()
    {
        return [ this ]( IosCatalogTask task ) {
            std::lock_guard<std::mutex> lock( mutex_ );
            tasks_.push_back( std::move( task ) );
        };
    }

    std::size_t pending() const
    {
        std::lock_guard<std::mutex> lock( mutex_ );
        return tasks_.size();
    }

    void runNextOnWorker()
    {
        IosCatalogTask task;
        {
            std::lock_guard<std::mutex> lock( mutex_ );
            REQUIRE_FALSE( tasks_.empty() );
            task = std::move( tasks_.front() );
            tasks_.erase( tasks_.begin() );
        }
        std::thread worker( std::move( task ) );
        worker.join();
    }

    void runAllOnWorker()
    {
        while ( pending() != 0 ) {
            runNextOnWorker();
        }
    }

private:
    mutable std::mutex mutex_;
    std::vector<IosCatalogTask> tasks_;
};

const IosCatalogEntry* findEntry( const IosCatalogSnapshot& snapshot, const std::string& udid,
                                  NativeConnectionType connection )
{
    const auto found = std::find_if(
        snapshot.entries.cbegin(), snapshot.entries.cend(), [ & ]( const auto& entry ) {
            return entry.endpoint.udid == udid && entry.endpoint.connectionType == connection;
        } );
    return found == snapshot.entries.cend() ? nullptr : &*found;
}

class MemorySnapshotProvider final : public IosCatalogSnapshotProvider {
public:
    IosCatalogSnapshot snapshot() const override
    {
        return snapshot_;
    }

    SubscriptionId subscribe( SnapshotCallback callback ) override
    {
        callback_ = std::move( callback );
        return 1;
    }

    void unsubscribe( SubscriptionId subscription ) override
    {
        REQUIRE( subscription == 1 );
        callback_ = {};
    }

    void publish( IosCatalogSnapshot snapshot )
    {
        snapshot_ = std::move( snapshot );
        if ( callback_ ) {
            callback_( snapshot_ );
        }
    }

private:
    IosCatalogSnapshot snapshot_;
    SnapshotCallback callback_;
};

} // namespace

TEST_CASE( "catalog snapshot provider is a thin source-neutral value contract",
           "[ios][catalog][provider][contract]" )
{
    static_assert( std::has_virtual_destructor_v<IosCatalogSnapshotProvider> );
    static_assert( std::is_copy_constructible_v<IosCatalogSnapshot> );
    static_assert( std::is_move_constructible_v<IosCatalogSnapshot> );

    MemorySnapshotProvider provider;
    int notifications = 0;
    const auto subscription = provider.subscribe( [ & ]( const IosCatalogSnapshot& snapshot ) {
        ++notifications;
        CHECK( snapshot.generation == Generation{ 8 } );
    } );
    provider.publish( IosCatalogSnapshot{ Generation{ 8 }, {} } );
    CHECK( notifications == 1 );
    CHECK( provider.snapshot().generation == Generation{ 8 } );
    provider.unsubscribe( subscription );
}

TEST_CASE( "catalog retains extended USB and network endpoints sharing one UDID",
           "[ios][catalog][extended-list][duplicate-udid]" )
{
    FakeNative state;
    fake = &state;
    state.listed = { { "same-udid", NativeConnectionType::Usb },
                     { "same-udid", NativeConnectionType::Network } };
    ManualExecutor executor;
    IosDeviceCatalog catalog( makeApi(), executor.executor() );

    REQUIRE( catalog.start() );
    const auto snapshot = catalog.snapshot();
    REQUIRE( snapshot.entries.size() == 2 );
    CHECK( findEntry( snapshot, "same-udid", NativeConnectionType::Usb ) != nullptr );
    CHECK( findEntry( snapshot, "same-udid", NativeConnectionType::Network ) != nullptr );
    CHECK( state.listFreeCalls == 1 );

    catalog.stop();
}

TEST_CASE( "catalog accepts synchronous ADD during native subscription and copies callback UDID",
           "[ios][catalog][subscribe][callback-lifetime]" )
{
    FakeNative state;
    fake = &state;
    char callbackUdid[] = "sync-device";
    state.synchronousSubscribeEvents
        = { NativeDeviceEvent{ NativeEventType::Add, callbackUdid, NativeConnectionType::Usb } };
    ManualExecutor executor;
    IosDeviceCatalog catalog( makeApi(), executor.executor() );

    REQUIRE( catalog.start() );
    std::fill( std::begin( callbackUdid ), std::end( callbackUdid ) - 1, 'x' );

    const auto snapshot = catalog.snapshot();
    REQUIRE( snapshot.entries.size() == 1 );
    CHECK( snapshot.entries.front().endpoint.udid == "sync-device" );
    catalog.stop();
}

TEST_CASE( "catalog gates synthetic REMOVE delivered synchronously by unsubscribe",
           "[ios][catalog][unsubscribe][shutdown]" )
{
    FakeNative state;
    fake = &state;
    state.listed = { { "survivor", NativeConnectionType::Usb } };
    const char udid[] = "survivor";
    state.unsubscribeEvent
        = NativeDeviceEvent{ NativeEventType::Remove, udid, NativeConnectionType::Usb };
    ManualExecutor executor;
    IosDeviceCatalog catalog( makeApi(), executor.executor() );

    REQUIRE( catalog.start() );
    const auto beforeStop = catalog.snapshot();
    REQUIRE( beforeStop.entries.size() == 1 );
    catalog.stop();

    CHECK( state.unsubscribeCalls == 1 );
    CHECK( catalog.snapshot().generation == beforeStop.generation );
    CHECK( catalog.snapshot().entries.size() == 1 );
}

TEST_CASE( "metadata query uses endpoint connection option and never runs on caller thread",
           "[ios][catalog][metadata][thread][connection-option]" )
{
    FakeNative state;
    fake = &state;
    state.listed
        = { { "usb", NativeConnectionType::Usb }, { "wifi", NativeConnectionType::Network } };
    state.metadata[ "usb@usb" ] = { 0, "USB phone", "iPhone17,1", "20.0" };
    state.metadata[ "wifi@network" ] = { 0, "WiFi phone", "iPhone17,2", "20.1" };
    ManualExecutor executor;
    IosDeviceCatalog catalog( makeApi(), executor.executor() );
    REQUIRE( catalog.start() );

    const auto callerThread = std::this_thread::get_id();
    catalog.requestMetadata( IosEndpointKey{ "usb", NativeConnectionType::Usb } );
    catalog.requestMetadata( IosEndpointKey{ "wifi", NativeConnectionType::Network } );
    REQUIRE( executor.pending() == 2 );
    executor.runAllOnWorker();

    REQUIRE( state.connectionOptions.size() == 2 );
    CHECK( state.connectionOptions[ 0 ] == NativeConnectionOption::Usb );
    CHECK( state.connectionOptions[ 1 ] == NativeConnectionOption::Network );
    REQUIRE_FALSE( state.metadataThreads.empty() );
    CHECK( std::none_of( state.metadataThreads.cbegin(), state.metadataThreads.cend(),
                         [ & ]( auto id ) { return id == callerThread; } ) );

    const auto snapshot = catalog.snapshot();
    REQUIRE( findEntry( snapshot, "usb", NativeConnectionType::Usb )->metadata.has_value() );
    CHECK( findEntry( snapshot, "usb", NativeConnectionType::Usb )->metadata->displayName
           == "USB phone" );
    REQUIRE( findEntry( snapshot, "wifi", NativeConnectionType::Network )->metadata.has_value() );
    CHECK( findEntry( snapshot, "wifi", NativeConnectionType::Network )->metadata->displayName
           == "WiFi phone" );
    catalog.stop();
}

TEST_CASE( "catalog rejects metadata from a stale endpoint epoch after remove and re-add",
           "[ios][catalog][generation][epoch][stale]" )
{
    FakeNative state;
    fake = &state;
    state.listed = { { "reused", NativeConnectionType::Usb } };
    state.metadata[ "reused@usb" ] = { 0, "old incarnation", "old", "1" };
    ManualExecutor executor;
    IosDeviceCatalog catalog( makeApi(), executor.executor() );
    REQUIRE( catalog.start() );

    const auto oldEpoch
        = findEntry( catalog.snapshot(), "reused", NativeConnectionType::Usb )->epoch;
    catalog.requestMetadata( IosEndpointKey{ "reused", NativeConnectionType::Usb } );
    REQUIRE( executor.pending() == 1 );

    state.emit( NativeEventType::Remove, "reused", NativeConnectionType::Usb );
    state.emit( NativeEventType::Add, "reused", NativeConnectionType::Usb );
    const auto newEpoch
        = findEntry( catalog.snapshot(), "reused", NativeConnectionType::Usb )->epoch;
    REQUIRE( newEpoch > oldEpoch );
    state.metadata[ "reused@usb" ] = { 0, "new incarnation", "new", "2" };
    catalog.requestMetadata( IosEndpointKey{ "reused", NativeConnectionType::Usb } );
    REQUIRE( executor.pending() == 2 );

    executor.runNextOnWorker();
    CHECK_FALSE( findEntry( catalog.snapshot(), "reused", NativeConnectionType::Usb )
                     ->metadata.has_value() );
    executor.runNextOnWorker();
    REQUIRE( findEntry( catalog.snapshot(), "reused", NativeConnectionType::Usb )
                 ->metadata.has_value() );
    CHECK(
        findEntry( catalog.snapshot(), "reused", NativeConnectionType::Usb )->metadata->displayName
        == "new incarnation" );
    catalog.stop();
}

TEST_CASE( "newer metadata request generation rejects an older result for the same epoch",
           "[ios][catalog][generation][stale]" )
{
    FakeNative state;
    fake = &state;
    state.listed = { { "overlap", NativeConnectionType::Usb } };
    state.metadata[ "overlap@usb" ] = { 0, "current", "model", "1" };
    ManualExecutor executor;
    IosDeviceCatalog catalog( makeApi(), executor.executor() );
    REQUIRE( catalog.start() );

    const IosEndpointKey endpoint{ "overlap", NativeConnectionType::Usb };
    catalog.requestMetadata( endpoint );
    catalog.requestMetadata( endpoint );
    REQUIRE( executor.pending() == 2 );

    executor.runNextOnWorker();
    CHECK_FALSE( findEntry( catalog.snapshot(), "overlap", NativeConnectionType::Usb )
                     ->metadata.has_value() );
    executor.runNextOnWorker();
    REQUIRE( findEntry( catalog.snapshot(), "overlap", NativeConnectionType::Usb )
                 ->metadata.has_value() );
    CHECK(
        findEntry( catalog.snapshot(), "overlap", NativeConnectionType::Usb )->metadata->displayName
        == "current" );
    catalog.stop();
}

TEST_CASE(
    "ADD PAIRED REMOVE callbacks from different threads converge without pairing side effects",
    "[ios][catalog][events][race][pairing-policy]" )
{
    FakeNative state;
    fake = &state;
    ManualExecutor executor;
    IosDeviceCatalog catalog( makeApi(), executor.executor() );
    REQUIRE( catalog.start() );

    std::mutex mutex;
    std::condition_variable changed;
    int stage = 0;
    auto waitFor = [ & ]( int target ) {
        std::unique_lock<std::mutex> lock( mutex );
        changed.wait( lock, [ & ] { return stage == target; } );
    };
    auto advance = [ & ]( int target ) {
        {
            std::lock_guard<std::mutex> lock( mutex );
            stage = target;
        }
        changed.notify_all();
    };

    std::thread add( [ & ] {
        state.emit( NativeEventType::Add, "racy", NativeConnectionType::Usb );
        advance( 1 );
    } );
    std::thread paired( [ & ] {
        waitFor( 1 );
        state.emit( NativeEventType::Paired, "racy", NativeConnectionType::Usb );
        advance( 2 );
    } );
    std::thread remove( [ & ] {
        waitFor( 2 );
        state.emit( NativeEventType::Remove, "racy", NativeConnectionType::Usb );
    } );
    add.join();
    paired.join();
    remove.join();

    CHECK( findEntry( catalog.snapshot(), "racy", NativeConnectionType::Usb ) == nullptr );
    catalog.stop();
}

TEST_CASE(
    "catalog exposes structured trust locked and stale-pair metadata errors without auto pair",
    "[ios][catalog][metadata][error][pairing-policy]" )
{
    struct Case {
        const char* udid;
        std::int32_t nativeCode;
        const char* stableCode;
        AwaitingUserReason reason;
    };
    const std::vector<Case> cases{
        { "trust", -19, "ios-trust-pending", AwaitingUserReason::Trust },
        { "locked", -17, "ios-device-locked", AwaitingUserReason::Unlock },
        { "pair", -29, "ios-stale-pair", AwaitingUserReason::Pair },
    };

    FakeNative state;
    fake = &state;
    for ( const auto& value : cases ) {
        state.listed.push_back( { value.udid, NativeConnectionType::Usb } );
        state.metadata[ endpointId( value.udid, NativeConnectionType::Usb ) ]
            = { value.nativeCode, {}, {}, {} };
    }
    ManualExecutor executor;
    IosDeviceCatalog catalog( makeApi(), executor.executor() );
    REQUIRE( catalog.start() );
    for ( const auto& value : cases ) {
        catalog.requestMetadata( IosEndpointKey{ value.udid, NativeConnectionType::Usb } );
    }
    executor.runAllOnWorker();

    const auto snapshot = catalog.snapshot();
    for ( const auto& value : cases ) {
        const auto* entry = findEntry( snapshot, value.udid, NativeConnectionType::Usb );
        REQUIRE( entry != nullptr );
        REQUIRE( entry->error.has_value() );
        CHECK( entry->error->error.code == value.stableCode );
        CHECK( entry->error->awaitingUserReason == value.reason );
    }
    catalog.stop();
}

TEST_CASE( "catalog restart rejects metadata queued by a stale catalog generation",
           "[ios][catalog][generation][stale]" )
{
    FakeNative state;
    fake = &state;
    state.listed = { { "generation-device", NativeConnectionType::Usb } };
    state.metadata[ "generation-device@usb" ] = { 0, "stale generation", "old", "1" };
    ManualExecutor executor;
    IosDeviceCatalog catalog( makeApi(), executor.executor() );
    REQUIRE( catalog.start() );
    const auto firstGeneration = catalog.snapshot().generation;
    catalog.requestMetadata( IosEndpointKey{ "generation-device", NativeConnectionType::Usb } );
    REQUIRE( executor.pending() == 1 );

    catalog.stop();
    REQUIRE( catalog.start() );
    REQUIRE( catalog.snapshot().generation > firstGeneration );
    executor.runNextOnWorker();

    const auto currentSnapshot = catalog.snapshot();
    const auto* current
        = findEntry( currentSnapshot, "generation-device", NativeConnectionType::Usb );
    REQUIRE( current != nullptr );
    CHECK_FALSE( current->metadata.has_value() );
    catalog.stop();
}

TEST_CASE( "shutdown invalidates queued metadata work and releases every opened owner",
           "[ios][catalog][shutdown][lifetime]" )
{
    FakeNative state;
    fake = &state;
    state.listed = { { "late", NativeConnectionType::Usb } };
    state.metadata[ "late@usb" ] = { 0, "must not publish", "late", "1" };
    ManualExecutor executor;
    auto catalog = std::make_unique<IosDeviceCatalog>( makeApi(), executor.executor() );
    REQUIRE( catalog->start() );
    catalog->requestMetadata( IosEndpointKey{ "late", NativeConnectionType::Usb } );
    REQUIRE( executor.pending() == 1 );
    const auto generationBeforeStop = catalog->snapshot().generation;

    catalog->stop();
    executor.runAllOnWorker();

    CHECK( catalog->snapshot().generation == generationBeforeStop );
    CHECK_FALSE(
        findEntry( catalog->snapshot(), "late", NativeConnectionType::Usb )->metadata.has_value() );
    CHECK( state.deviceFreeCalls == state.lockdownFreeCalls );
    CHECK( state.deviceFreeCalls <= 1 );
    catalog.reset();
}

TEST_CASE( "catalog start is atomic when an initial notification stops it reentrantly",
           "[ios][catalog][start][reentrant][shutdown]" )
{
    FakeNative state;
    fake = &state;
    state.listed = { { "reentrant", NativeConnectionType::Usb } };
    ManualExecutor executor;
    IosDeviceCatalog catalog( makeApi(), executor.executor() );
    catalog.subscribe( [ & ]( const IosCatalogSnapshot& snapshot ) {
        if ( !snapshot.entries.empty() ) {
            catalog.stop();
        }
    } );

    CHECK_FALSE( catalog.start() );
    CHECK( state.subscribeCalls == 1 );
    CHECK( state.unsubscribeCalls == 1 );
}

TEST_CASE( "catalog subscription failure publishes no non-live endpoints",
           "[ios][catalog][start][atomic][failure]" )
{
    FakeNative state;
    fake = &state;
    state.listed = { { "not-live", NativeConnectionType::Usb } };
    state.subscribeResult = -1;
    ManualExecutor executor;
    IosDeviceCatalog catalog( makeApi(), executor.executor() );
    int notifications = 0;
    catalog.subscribe( [ & ]( const IosCatalogSnapshot& ) { ++notifications; } );

    CHECK_FALSE( catalog.start() );
    CHECK( catalog.snapshot().entries.empty() );
    CHECK( notifications == 0 );
    CHECK( state.listFreeCalls == 0 );
}

TEST_CASE( "catalog rejects malformed list outputs and unknown endpoint transports",
           "[ios][catalog][abi][malformed][endpoint]" )
{
    SECTION( "positive count requires a list" )
    {
        FakeNative state;
        fake = &state;
        state.listed = { { "missing-list", NativeConnectionType::Usb } };
        state.nullListOnSuccess = true;
        ManualExecutor executor;
        IosDeviceCatalog catalog( makeApi(), executor.executor() );

        CHECK_FALSE( catalog.start() );
        CHECK( catalog.snapshot().entries.empty() );
    }

    SECTION( "unknown connection types are not coerced to network" )
    {
        FakeNative state;
        fake = &state;
        state.listed = { { "unknown", static_cast<NativeConnectionType>( 99 ) } };
        ManualExecutor executor;
        IosDeviceCatalog catalog( makeApi(), executor.executor() );

        REQUIRE( catalog.start() );
        CHECK( catalog.snapshot().entries.empty() );
        catalog.stop();
    }
}

TEST_CASE( "metadata cancellation prevents native effects after stop",
           "[ios][catalog][metadata][shutdown][cancellation]" )
{
    FakeNative state;
    fake = &state;
    state.listed = { { "cancelled", NativeConnectionType::Usb } };
    state.metadata[ "cancelled@usb" ] = { 0, "must not open", "none", "0" };
    ManualExecutor executor;
    IosDeviceCatalog catalog( makeApi(), executor.executor() );
    REQUIRE( catalog.start() );
    catalog.requestMetadata( IosEndpointKey{ "cancelled", NativeConnectionType::Usb } );
    REQUIRE( executor.pending() == 1 );

    catalog.stop();
    executor.runAllOnWorker();

    CHECK( state.deviceNewCalls == 0 );
    CHECK( state.lockdownNewCalls == 0 );
    CHECK( state.stringCalls == 0 );
}

TEST_CASE( "metadata cancellation stops remaining RPCs and releases opened owners",
           "[ios][catalog][metadata][shutdown][cancellation][ownership]" )
{
    FakeNative state;
    fake = &state;
    state.listed = { { "mid-flight", NativeConnectionType::Usb } };
    state.metadata[ "mid-flight@usb" ] = { 0, "one", "two", "three" };
    ManualExecutor executor;
    IosDeviceCatalog catalog( makeApi(), executor.executor() );
    REQUIRE( catalog.start() );
    state.onStringCall = [ & ]( int call ) {
        if ( call == 1 ) {
            catalog.stop();
        }
    };
    catalog.requestMetadata( IosEndpointKey{ "mid-flight", NativeConnectionType::Usb } );
    executor.runAllOnWorker();

    CHECK( state.stringCalls == 1 );
    CHECK( state.stringFreeCalls == 1 );
    CHECK( state.lockdownFreeCalls == 1 );
    CHECK( state.deviceFreeCalls == 1 );
}

TEST_CASE( "metadata owns partial native outputs on every failure path",
           "[ios][catalog][metadata][ownership][partial-failure]" )
{
    SECTION( "device allocation returned with an error" )
    {
        FakeNative state;
        fake = &state;
        state.listed = { { "partial-device", NativeConnectionType::Usb } };
        state.deviceNewResult = -3;
        state.allocateDeviceOnError = true;
        ManualExecutor executor;
        IosDeviceCatalog catalog( makeApi(), executor.executor() );
        REQUIRE( catalog.start() );
        catalog.requestMetadata( IosEndpointKey{ "partial-device", NativeConnectionType::Usb } );
        executor.runAllOnWorker();
        CHECK( state.deviceFreeCalls == 1 );
        catalog.stop();
    }

    SECTION( "lockdown allocation returned with an error" )
    {
        FakeNative state;
        fake = &state;
        state.listed = { { "partial-lockdown", NativeConnectionType::Usb } };
        state.lockdownNewResult = -19;
        state.allocateLockdownOnError = true;
        ManualExecutor executor;
        IosDeviceCatalog catalog( makeApi(), executor.executor() );
        REQUIRE( catalog.start() );
        catalog.requestMetadata( IosEndpointKey{ "partial-lockdown", NativeConnectionType::Usb } );
        executor.runAllOnWorker();
        CHECK( state.lockdownFreeCalls == 1 );
        CHECK( state.deviceFreeCalls == 1 );
        catalog.stop();
    }

    SECTION( "string allocation returned with an error" )
    {
        FakeNative state;
        fake = &state;
        state.listed = { { "partial-string", NativeConnectionType::Usb } };
        state.metadata[ "partial-string@usb" ] = { 0, "", "", "" };
        state.stringFailureCall = 1;
        state.allocateStringOnError = true;
        ManualExecutor executor;
        IosDeviceCatalog catalog( makeApi(), executor.executor() );
        REQUIRE( catalog.start() );
        catalog.requestMetadata( IosEndpointKey{ "partial-string", NativeConnectionType::Usb } );
        executor.runAllOnWorker();
        CHECK( state.stringFreeCalls == 1 );
        CHECK( state.lockdownFreeCalls == 1 );
        CHECK( state.deviceFreeCalls == 1 );
        catalog.stop();
    }
}

TEST_CASE( "metadata rejects success with null native handles",
           "[ios][catalog][metadata][abi][malformed]" )
{
    SECTION( "null device" )
    {
        FakeNative state;
        fake = &state;
        state.listed = { { "null-device", NativeConnectionType::Usb } };
        state.nullDeviceOnSuccess = true;
        ManualExecutor executor;
        IosDeviceCatalog catalog( makeApi(), executor.executor() );
        REQUIRE( catalog.start() );
        catalog.requestMetadata( IosEndpointKey{ "null-device", NativeConnectionType::Usb } );
        executor.runAllOnWorker();
        const auto snapshot = catalog.snapshot();
        const auto* entry = findEntry( snapshot, "null-device", NativeConnectionType::Usb );
        REQUIRE( entry != nullptr );
        REQUIRE( entry->error.has_value() );
        CHECK( state.lockdownNewCalls == 0 );
        catalog.stop();
    }

    SECTION( "null lockdown client" )
    {
        FakeNative state;
        fake = &state;
        state.listed = { { "null-lockdown", NativeConnectionType::Usb } };
        state.nullLockdownOnSuccess = true;
        ManualExecutor executor;
        IosDeviceCatalog catalog( makeApi(), executor.executor() );
        REQUIRE( catalog.start() );
        catalog.requestMetadata( IosEndpointKey{ "null-lockdown", NativeConnectionType::Usb } );
        executor.runAllOnWorker();
        const auto snapshot = catalog.snapshot();
        const auto* entry = findEntry( snapshot, "null-lockdown", NativeConnectionType::Usb );
        REQUIRE( entry != nullptr );
        REQUIRE( entry->error.has_value() );
        CHECK( state.stringCalls == 0 );
        CHECK( state.deviceFreeCalls == 1 );
        catalog.stop();
    }
}

TEST_CASE( "snapshot callback failures are isolated from native callbacks and peers",
           "[ios][catalog][callback][exception][isolation]" )
{
    FakeNative state;
    fake = &state;
    ManualExecutor executor;
    IosDeviceCatalog catalog( makeApi(), executor.executor() );
    int healthyNotifications = 0;
    catalog.subscribe( {} );
    catalog.subscribe( [ & ]( const IosCatalogSnapshot& ) { throw std::runtime_error( "boom" ); } );
    catalog.subscribe( [ & ]( const IosCatalogSnapshot& ) { ++healthyNotifications; } );
    REQUIRE( catalog.start() );
    REQUIRE( healthyNotifications == 1 );

    CHECK_NOTHROW( state.emit( NativeEventType::Add, "safe", NativeConnectionType::Usb ) );
    CHECK( healthyNotifications == 1 );
    executor.runAllOnWorker();
    CHECK( healthyNotifications == 2 );
    catalog.stop();
}
