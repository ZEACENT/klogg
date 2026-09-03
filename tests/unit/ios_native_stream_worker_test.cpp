/*
 * Copyright (C) 2026
 *
 * This file is part of klogg.
 *
 * RED contract for the passive native iOS stream worker. Every native call is
 * injected; the tests use no vendor headers, daemon, framework, device, timer,
 * subprocess, Python, or real device content.
 */

#include <catch2/catch.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "iosnativeapi.h"
#include "iosnativestream.h"
#include "iosostraceprotocol.h"

namespace {
using namespace std::chrono_literals;
using namespace std::string_literals;
using namespace klogg::livecapture;
using namespace klogg::livecapture::ios;

const auto DeviceHandle = reinterpret_cast<NativeIdevice>( 0x101 );
const auto LockdownHandle = reinterpret_cast<NativeLockdownClient>( 0x202 );
const auto ServiceHandle = reinterpret_cast<NativeServiceDescriptor>( 0x303 );
const auto OsTraceHandle = reinterpret_cast<NativeOsTraceClient>( 0x404 );
const auto SyslogHandle = reinterpret_cast<NativeSyslogRelayClient>( 0x505 );

struct FakeNative {
    NativePairRecordResult pairRecordResult{ NativePairRecordResult::Present };
    std::int32_t deviceResult{ 0 };
    std::int32_t handshakeResult{ 0 };
    std::int32_t productVersionResult{ 0 };
    std::int32_t timeZoneResult{ 0 };
    std::int32_t startServiceResult{ 0 };
    std::int32_t osTraceNewResult{ 0 };
    std::int32_t osTraceStartResult{ 0 };
    std::int32_t syslogNewResult{ 0 };
    std::int32_t syslogStartResult{ 0 };
    std::string productVersion{ "17.6.1" };
    std::string timeZone{ "America/New_York" };
    bool allocateDeviceOnError{ false };
    bool allocateLockdownOnError{ false };
    bool allocateServiceOnError{ false };
    bool allocateOsTraceOnError{ false };
    bool allocateSyslogOnError{ false };
    bool nullDeviceOnSuccess{ false };
    bool nullLockdownOnSuccess{ false };
    bool nullServiceOnSuccess{ false };
    bool nullTimeZoneOnSuccess{ false };
    bool nullOsTraceOnSuccess{ false };
    bool nullSyslogOnSuccess{ false };
    bool emitOsTraceErrorDuringStart{ false };
    bool throwDuringDeviceNew{ false };
    std::atomic_bool stopOnCallbackThread{ false };
    std::atomic_bool freeInsideCallback{ false };
    std::atomic_bool abiViolation{ false };
    std::vector<std::string> calls;
    std::vector<std::thread::id> nativeThreads;
    std::mutex recordMutex;
    std::mutex callbackMutex;
    std::condition_variable callbackChanged;
    bool callbackActive{ false };
    bool blockCallbackReturn{ false };
    bool callbackBodyReturned{ false };
    bool callbackReturnReleased{ false };
    bool stopEntered{ false };
    std::thread::id callbackThread;
    NativeOsTraceActivityCallback osTraceActivityCallback{ nullptr };
    NativeOsTraceErrorCallback osTraceErrorCallback{ nullptr };
    NativeSyslogRelayCallback syslogCallback{ nullptr };
    NativeSyslogRelayErrorCallback syslogErrorCallback{ nullptr };
    void* osTraceContext{ nullptr };
    void* syslogContext{ nullptr };
    bool blockOsTraceReceive{ false };
    bool receiveEntered{ false };
    bool receiveInterrupted{ false };
    bool receiveExited{ false };
    std::mutex receiveMutex;
    std::condition_variable receiveChanged;
    std::thread receiveThread;
    bool blockHandshake{ false };
    bool handshakeEntered{ false };
    bool handshakeReleased{ false };
    std::mutex handshakeMutex;
    std::condition_variable handshakeChanged;
    bool blockTimeZone{ false };
    bool timeZoneEntered{ false };
    bool timeZoneReleased{ false };
    std::mutex timeZoneMutex;
    std::condition_variable timeZoneChanged;

    ~FakeNative()
    {
        releaseHandshake();
        releaseTimeZone();
        interruptBlockedReceive();
    }

    void startBlockedReceive()
    {
        if ( !blockOsTraceReceive ) {
            return;
        }
        receiveThread = std::thread( [ this ] {
            std::unique_lock<std::mutex> lock( receiveMutex );
            receiveEntered = true;
            receiveChanged.notify_all();
            receiveChanged.wait( lock, [ this ] { return receiveInterrupted; } );
            receiveExited = true;
            receiveChanged.notify_all();
        } );
    }

    void waitUntilReceiveEntered()
    {
        std::unique_lock<std::mutex> lock( receiveMutex );
        receiveChanged.wait( lock, [ this ] { return receiveEntered; } );
    }

    void interruptBlockedReceive()
    {
        {
            std::lock_guard<std::mutex> lock( receiveMutex );
            receiveInterrupted = true;
            receiveChanged.notify_all();
        }
        if ( receiveThread.joinable() ) {
            receiveThread.join();
        }
    }

    void waitAtHandshake()
    {
        if ( !blockHandshake ) {
            return;
        }
        std::unique_lock<std::mutex> lock( handshakeMutex );
        handshakeEntered = true;
        handshakeChanged.notify_all();
        if ( !handshakeChanged.wait_for( lock, 2s,
                                         [ this ] { return handshakeReleased; } ) ) {
            abiViolation = true;
        }
    }

    bool waitUntilHandshakeEntered()
    {
        std::unique_lock<std::mutex> lock( handshakeMutex );
        return handshakeChanged.wait_for( lock, 2s, [ this ] { return handshakeEntered; } );
    }

    void releaseHandshake()
    {
        std::lock_guard<std::mutex> lock( handshakeMutex );
        handshakeReleased = true;
        handshakeChanged.notify_all();
    }

    void waitAtTimeZone()
    {
        if ( !blockTimeZone ) {
            return;
        }
        std::unique_lock<std::mutex> lock( timeZoneMutex );
        timeZoneEntered = true;
        timeZoneChanged.notify_all();
        if ( !timeZoneChanged.wait_for( lock, 2s, [ this ] { return timeZoneReleased; } ) ) {
            abiViolation = true;
        }
    }

    bool waitUntilTimeZoneEntered()
    {
        std::unique_lock<std::mutex> lock( timeZoneMutex );
        return timeZoneChanged.wait_for( lock, 2s, [ this ] { return timeZoneEntered; } );
    }

    void releaseTimeZone()
    {
        std::lock_guard<std::mutex> lock( timeZoneMutex );
        timeZoneReleased = true;
        timeZoneChanged.notify_all();
    }

    void beginCallback()
    {
        std::lock_guard<std::mutex> lock( callbackMutex );
        callbackActive = true;
        callbackBodyReturned = false;
        callbackReturnReleased = false;
        callbackThread = std::this_thread::get_id();
    }

    void finishCallback()
    {
        std::unique_lock<std::mutex> lock( callbackMutex );
        callbackBodyReturned = true;
        callbackChanged.notify_all();
        callbackChanged.wait( lock, [ this ] {
            return !blockCallbackReturn || callbackReturnReleased;
        } );
        callbackActive = false;
        callbackThread = {};
        lock.unlock();
        callbackChanged.notify_all();
    }

    bool waitUntilCallbackBodyReturned()
    {
        std::unique_lock<std::mutex> lock( callbackMutex );
        return callbackChanged.wait_for( lock, 2s,
                                         [ this ] { return callbackBodyReturned; } );
    }

    bool waitUntilStopEntered()
    {
        std::unique_lock<std::mutex> lock( callbackMutex );
        return callbackChanged.wait_for( lock, 2s, [ this ] { return stopEntered; } );
    }

    void releaseCallbackReturn()
    {
        std::lock_guard<std::mutex> lock( callbackMutex );
        callbackReturnReleased = true;
        callbackChanged.notify_all();
    }

    void waitUntilCallbackExited()
    {
        std::unique_lock<std::mutex> lock( callbackMutex );
        callbackChanged.wait( lock, [ this ] { return !callbackActive; } );
    }

    bool callbackTeardownViolation() const
    {
        return stopOnCallbackThread.load() || freeInsideCallback.load();
    }

    void record( std::string call )
    {
        bool notifyCallback = false;
        {
            std::lock_guard<std::mutex> lock( callbackMutex );
            if ( call.find( "stop" ) != std::string::npos ) {
                stopEntered = true;
                notifyCallback = true;
                if ( callbackActive && callbackThread == std::this_thread::get_id() ) {
                    stopOnCallbackThread = true;
                }
            }
            if ( call.find( "free" ) != std::string::npos && callbackActive ) {
                freeInsideCallback = true;
            }
        }
        if ( notifyCallback ) {
            callbackChanged.notify_all();
        }
        std::lock_guard<std::mutex> lock( recordMutex );
        calls.push_back( std::move( call ) );
        nativeThreads.push_back( std::this_thread::get_id() );
    }

    void emitOsTrace( const std::vector<std::uint8_t>& bytes )
    {
        if ( osTraceActivityCallback == nullptr ) {
            abiViolation = true;
            return;
        }
        beginCallback();
        osTraceActivityCallback( bytes.data(), static_cast<std::uint32_t>( bytes.size() ),
                                 osTraceContext );
        finishCallback();
    }

    void emitOsTraceError( std::int32_t code )
    {
        if ( osTraceErrorCallback == nullptr ) {
            abiViolation = true;
            return;
        }
        beginCallback();
        osTraceErrorCallback( code, osTraceContext );
        finishCallback();
    }

    void emitSyslog( const std::string& bytes )
    {
        if ( syslogCallback == nullptr ) {
            abiViolation = true;
            return;
        }
        beginCallback();
        for ( const auto byte : bytes ) {
            syslogCallback( byte, syslogContext );
        }
        finishCallback();
    }

    void emitSyslogError( std::int32_t code )
    {
        if ( syslogErrorCallback == nullptr ) {
            abiViolation = true;
            return;
        }
        beginCallback();
        syslogErrorCallback( code, syslogContext );
        finishCallback();
    }
};

FakeNative* fake = nullptr;

NativePairRecordResult readPairRecord( const char* udid, char** record, std::uint32_t* recordSize )
{
    fake->record( std::string{ "pair-record:" } + udid );
    if ( fake->pairRecordResult == NativePairRecordResult::Present ) {
        auto bytes = std::make_unique<char[]>( 4u );
        std::memcpy( bytes.get(), "pair", 4u );
        *record = bytes.release();
        *recordSize = 4u;
    }
    return fake->pairRecordResult;
}

void freePairRecord( char* record )
{
    fake->record( "pair-record-free" );
    delete[] record;
}

std::int32_t newDevice( NativeIdevice* device, const char* udid, NativeConnectionOption option )
{
    if ( fake->throwDuringDeviceNew ) {
        throw std::runtime_error( "injected native adapter exception" );
    }
    fake->record( std::string{ "device-new:" } + udid
                  + ( option == NativeConnectionOption::Usb ? ":usb" : ":network" ) );
    if ( ( fake->deviceResult == 0 && !fake->nullDeviceOnSuccess )
         || fake->allocateDeviceOnError ) {
        *device = DeviceHandle;
    }
    return fake->deviceResult;
}

std::int32_t freeDevice( NativeIdevice device )
{
    if ( device != DeviceHandle ) {
        fake->abiViolation = true;
    }
    fake->record( "device-free" );
    return 0;
}

std::int32_t newHandshake( NativeIdevice device, NativeLockdownClient* client, const char* label )
{
    if ( device != DeviceHandle || label == nullptr || std::string{ label } != "klogg" ) {
        fake->abiViolation = true;
    }
    fake->record( "lockdown-existing-pair" );
    fake->waitAtHandshake();
    if ( ( fake->handshakeResult == 0 && !fake->nullLockdownOnSuccess )
         || fake->allocateLockdownOnError ) {
        *client = LockdownHandle;
    }
    return fake->handshakeResult;
}

std::int32_t freeLockdown( NativeLockdownClient client )
{
    if ( client != LockdownHandle ) {
        fake->abiViolation = true;
    }
    fake->record( "lockdown-free" );
    return 0;
}

std::int32_t getString( NativeLockdownClient client, const char* domain, const char* key,
                        char** value )
{
    if ( client != LockdownHandle || domain != nullptr || key == nullptr ) {
        fake->abiViolation = true;
        return -1;
    }

    const std::string requestedKey{ key };
    fake->record( "get:" + requestedKey );
    const std::string* returnedValue = nullptr;
    std::int32_t result = 0;
    if ( requestedKey == "ProductVersion" ) {
        returnedValue = &fake->productVersion;
        result = fake->productVersionResult;
    }
    else if ( requestedKey == "TimeZone" ) {
        fake->waitAtTimeZone();
        returnedValue = &fake->timeZone;
        result = fake->timeZoneResult;
    }
    else {
        fake->abiViolation = true;
        return -1;
    }

    if ( result != 0 ) {
        return result;
    }
    if ( requestedKey == "TimeZone" && fake->nullTimeZoneOnSuccess ) {
        return 0;
    }
    auto bytes = std::make_unique<char[]>( returnedValue->size() + 1u );
    std::memcpy( bytes.get(), returnedValue->c_str(), returnedValue->size() + 1u );
    *value = bytes.release();
    return 0;
}

void freeString( char* value )
{
    fake->record( "string-free" );
    delete[] value;
}

std::int32_t startService( NativeLockdownClient client, const char* name,
                           NativeServiceDescriptor* service )
{
    if ( client != LockdownHandle || name == nullptr ) {
        fake->abiViolation = true;
    }
    fake->record( std::string{ "service-start:" } + ( name != nullptr ? name : "<null>" ) );
    if ( ( fake->startServiceResult == 0 && !fake->nullServiceOnSuccess )
         || fake->allocateServiceOnError ) {
        *service = ServiceHandle;
    }
    return fake->startServiceResult;
}

std::int32_t freeService( NativeServiceDescriptor service )
{
    if ( service != ServiceHandle ) {
        fake->abiViolation = true;
    }
    fake->record( "service-free" );
    return 0;
}

std::int32_t newOsTrace( NativeIdevice device, NativeServiceDescriptor service,
                         NativeOsTraceClient* client )
{
    if ( device != DeviceHandle || service != ServiceHandle ) {
        fake->abiViolation = true;
    }
    fake->record( "ostrace-new" );
    if ( ( fake->osTraceNewResult == 0 && !fake->nullOsTraceOnSuccess )
         || fake->allocateOsTraceOnError ) {
        *client = OsTraceHandle;
    }
    return fake->osTraceNewResult;
}

std::int32_t startOsTrace( NativeOsTraceClient client, NativeOsTraceActivityCallback activity,
                           NativeOsTraceErrorCallback error, void* context )
{
    if ( client != OsTraceHandle || activity == nullptr || error == nullptr ) {
        fake->abiViolation = true;
    }
    fake->record( "ostrace-start" );
    fake->osTraceActivityCallback = activity;
    fake->osTraceErrorCallback = error;
    fake->osTraceContext = context;
    if ( fake->osTraceStartResult == 0 ) {
        fake->startBlockedReceive();
        if ( fake->emitOsTraceErrorDuringStart ) {
            error( -2, context );
        }
    }
    return fake->osTraceStartResult;
}

std::int32_t stopOsTrace( NativeOsTraceClient client )
{
    if ( client != OsTraceHandle ) {
        fake->abiViolation = true;
    }
    fake->record( "ostrace-stop" );
    fake->interruptBlockedReceive();
    fake->waitUntilCallbackExited();
    return 0;
}

std::int32_t freeOsTrace( NativeOsTraceClient client )
{
    if ( client != OsTraceHandle ) {
        fake->abiViolation = true;
    }
    fake->record( "ostrace-free" );
    return 0;
}

std::int32_t newSyslog( NativeIdevice device, NativeServiceDescriptor service,
                        NativeSyslogRelayClient* client )
{
    if ( device != DeviceHandle || service != ServiceHandle ) {
        fake->abiViolation = true;
    }
    fake->record( "syslog-new" );
    if ( ( fake->syslogNewResult == 0 && !fake->nullSyslogOnSuccess )
         || fake->allocateSyslogOnError ) {
        *client = SyslogHandle;
    }
    return fake->syslogNewResult;
}

std::int32_t startSyslog( NativeSyslogRelayClient client, NativeSyslogRelayCallback callback,
                          NativeSyslogRelayErrorCallback error, void* context )
{
    if ( client != SyslogHandle || callback == nullptr || error == nullptr ) {
        fake->abiViolation = true;
    }
    fake->record( "syslog-start" );
    fake->syslogCallback = callback;
    fake->syslogErrorCallback = error;
    fake->syslogContext = context;
    return fake->syslogStartResult;
}

std::int32_t stopSyslog( NativeSyslogRelayClient client )
{
    if ( client != SyslogHandle ) {
        fake->abiViolation = true;
    }
    fake->record( "syslog-stop" );
    fake->waitUntilCallbackExited();
    return 0;
}

std::int32_t freeSyslog( NativeSyslogRelayClient client )
{
    if ( client != SyslogHandle ) {
        fake->abiViolation = true;
    }
    fake->record( "syslog-free" );
    return 0;
}

IosNativeApi makeApi()
{
    IosNativeApi api{};
    api.readPairRecord = &readPairRecord;
    api.pairRecordFree = &freePairRecord;
    api.deviceNewWithOptions = &newDevice;
    api.deviceFree = &freeDevice;
    api.lockdownClientNewWithExistingPair = &newHandshake;
    api.lockdownClientFree = &freeLockdown;
    api.lockdownStartService = &startService;
    api.serviceDescriptorFree = &freeService;
    api.lockdownGetStringValue = &getString;
    api.nativeStringFree = &freeString;
    api.osTraceClientNew = &newOsTrace;
    api.osTraceStart = &startOsTrace;
    api.osTraceStop = &stopOsTrace;
    api.osTraceClientFree = &freeOsTrace;
    api.syslogRelayClientNew = &newSyslog;
    api.syslogRelayStart = &startSyslog;
    api.syslogRelayStop = &stopSyslog;
    api.syslogRelayClientFree = &freeSyslog;
    return api;
}

class ManualExecutor {
public:
    IosNativeStreamExecutor executor()
    {
        return [ this ]( IosNativeStreamTask task ) { tasks_.push_back( std::move( task ) ); };
    }

    std::size_t pending() const
    {
        return tasks_.size();
    }

    std::thread startNextOnWorker()
    {
        REQUIRE_FALSE( tasks_.empty() );
        auto task = std::move( tasks_.front() );
        tasks_.erase( tasks_.begin() );
        return std::thread( std::move( task ) );
    }

    void runNextOnWorker()
    {
        auto worker = startNextOnWorker();
        worker.join();
    }

    void runAllOnWorker()
    {
        while ( !tasks_.empty() ) {
            runNextOnWorker();
        }
    }

private:
    std::vector<IosNativeStreamTask> tasks_;
};

struct ObservedCallbacks {
    std::vector<Generation> ready;
    std::vector<Generation> bytesAvailable;
    std::vector<std::pair<Generation, ClassifiedIosNativeError>> errors;
    std::vector<Generation> stopped;
    bool throwFromBytesAvailable{ false };

    IosNativeStreamCallbacks callbacks()
    {
        IosNativeStreamCallbacks result;
        result.ready = [ this ]( Generation generation ) { ready.push_back( generation ); };
        result.bytesAvailable = [ this ]( Generation generation ) {
            bytesAvailable.push_back( generation );
            if ( throwFromBytesAvailable ) {
                throw std::runtime_error( "observer failure" );
            }
        };
        result.failed = [ this ]( Generation generation, const ClassifiedIosNativeError& error ) {
            errors.emplace_back( generation, error );
        };
        result.stopped = [ this ]( Generation generation ) { stopped.push_back( generation ); };
        return result;
    }
};

IosNativeStreamConfig config( Generation generation, std::string version = "17.6.1" )
{
    fake->productVersion = std::move( version );
    IosNativeStreamConfig result;
    result.endpoint = IosEndpointKey{ "device-udid", NativeConnectionType::Usb };
    result.generation = generation;
    result.ansiOutputEnabled = false;
    result.queueLimits = LiveDataQueueLimits{ 64u * 1024u, 32u };
    result.servicePolicy = IosNativeServicePolicy::AutomaticByProductVersion;
    result.timeZoneResolverFactory
        = []( const std::string& timeZoneId ) -> std::optional<OsTraceUtcOffsetResolver> {
        std::optional<std::int32_t> fixedOffset;
        if ( timeZoneId == "America/New_York" ) {
            fixedOffset = -5 * 60 * 60;
        }
        else if ( timeZoneId == "Asia/Kolkata" ) {
            fixedOffset = 5 * 60 * 60 + 30 * 60;
        }
        if ( !fixedOffset ) {
            return std::nullopt;
        }
        return OsTraceUtcOffsetResolver{
            [ offset = *fixedOffset ]( std::uint64_t epochSeconds )
                -> std::optional<std::int32_t> {
                if ( epochSeconds
                     > static_cast<std::uint64_t>( std::numeric_limits<std::int64_t>::max() ) ) {
                    return std::nullopt;
                }
                return offset;
            }
        };
    };
    result.cleanupDeadline = 75ms;
    return result;
}

void putLe16( std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint16_t value )
{
    bytes.at( offset ) = static_cast<std::uint8_t>( value & 0xffu );
    bytes.at( offset + 1u ) = static_cast<std::uint8_t>( value >> 8u );
}

void putLe32( std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value )
{
    for ( std::size_t index = 0u; index < 4u; ++index ) {
        bytes.at( offset + index )
            = static_cast<std::uint8_t>( ( value >> ( index * 8u ) ) & 0xffu );
    }
}

void putLe64( std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint64_t value )
{
    for ( std::size_t index = 0u; index < 8u; ++index ) {
        bytes.at( offset + index )
            = static_cast<std::uint8_t>( ( value >> ( index * 8u ) ) & 0xffu );
    }
}

std::vector<std::uint8_t> osTracePacket( const std::string& message = "native log" )
{
    constexpr std::size_t headerSize = 0x81u;
    const std::string process = "/Applications/Test.app/Test";
    std::vector<std::uint8_t> packet( headerSize, 0u );
    packet.at( 0u ) = 2u;
    putLe32( packet, 1u, 8u );
    putLe32( packet, 5u, static_cast<std::uint32_t>( headerSize ) );
    putLe32( packet, 9u, 42u );
    putLe16( packet, 37u, static_cast<std::uint16_t>( process.size() + 1u ) );
    putLe64( packet, 55u, 1700000000ull );
    putLe32( packet, 63u, 123456u );
    packet.at( 68u ) = 0x10u;
    putLe32( packet, 109u, static_cast<std::uint32_t>( message.size() + 1u ) );
    packet.insert( packet.end(), process.begin(), process.end() );
    packet.push_back( 0u );
    packet.insert( packet.end(), message.begin(), message.end() );
    packet.push_back( 0u );
    return packet;
}

std::size_t indexOf( const std::vector<std::string>& calls, const std::string& value )
{
    const auto found = std::find( calls.cbegin(), calls.cend(), value );
    REQUIRE( found != calls.cend() );
    return static_cast<std::size_t>( std::distance( calls.cbegin(), found ) );
}

} // namespace

TEST_CASE( "native iOS worker preflights an existing pair record before passive handshake",
           "[ios][native][stream][pairing][order]" )
{
    FakeNative state;
    fake = &state;
    ManualExecutor executor;
    ObservedCallbacks observed;
    const auto callerThread = std::this_thread::get_id();
    IosNativeStreamWorker worker( makeApi(), executor.executor(), config( 11u ),
                                  observed.callbacks() );

    REQUIRE( worker.start() );
    REQUIRE( executor.pending() == 1u );
    CHECK( state.calls.empty() );
    executor.runNextOnWorker();

    const std::vector<std::string> expectedPrefix{ "device-new:device-udid:usb",
                                                   "pair-record:device-udid",
                                                   "pair-record-free",
                                                   "lockdown-existing-pair",
                                                   "get:ProductVersion",
                                                   "string-free",
                                                   "get:TimeZone",
                                                   "string-free",
                                                   "service-start:com.apple.os_trace_relay",
                                                   "ostrace-new",
                                                   "ostrace-start" };
    REQUIRE( state.calls.size() >= expectedPrefix.size() );
    CHECK( std::equal( expectedPrefix.cbegin(), expectedPrefix.cend(), state.calls.cbegin() ) );
    CHECK( observed.ready == std::vector<Generation>{ 11u } );
    CHECK( std::none_of( state.nativeThreads.cbegin(), state.nativeThreads.cend(),
                         [ callerThread ]( auto thread ) { return thread == callerThread; } ) );

    worker.stop( 11u );
    executor.runAllOnWorker();
}

TEST_CASE( "stop during in-flight startup cancels readiness and retires partial owners",
           "[ios][native][stream][startup][stop][barrier]" )
{
    FakeNative state;
    fake = &state;
    state.blockHandshake = true;
    ManualExecutor executor;
    ObservedCallbacks observed;
    IosNativeStreamWorker worker( makeApi(), executor.executor(), config( 111u ),
                                  observed.callbacks() );
    REQUIRE( worker.start() );
    auto startup = executor.startNextOnWorker();
    if ( !state.waitUntilHandshakeEntered() ) {
        state.releaseHandshake();
        startup.join();
        FAIL( "native startup did not reach the handshake barrier" );
    }

    worker.stop( 111u );
    REQUIRE( executor.pending() == 1u );
    state.releaseHandshake();
    startup.join();
    executor.runAllOnWorker();

    CHECK( observed.ready.empty() );
    CHECK( observed.bytesAvailable.empty() );
    CHECK( observed.stopped == std::vector<Generation>{ 111u } );
    CHECK( std::find( state.calls.cbegin(), state.calls.cend(), "ostrace-start" )
           == state.calls.cend() );
    CHECK( std::count( state.calls.cbegin(), state.calls.cend(), "lockdown-free" ) == 1 );
    CHECK( std::count( state.calls.cbegin(), state.calls.cend(), "device-free" ) == 1 );
    CHECK_FALSE( state.abiViolation );
}

TEST_CASE( "missing native pair record awaits explicit user pairing without handshake or mutation",
           "[ios][native][stream][pairing][passive]" )
{
    FakeNative state;
    fake = &state;
    state.pairRecordResult = NativePairRecordResult::Missing;
    ManualExecutor executor;
    ObservedCallbacks observed;
    IosNativeStreamWorker worker( makeApi(), executor.executor(), config( 12u ),
                                  observed.callbacks() );

    REQUIRE( worker.start() );
    executor.runAllOnWorker();

    REQUIRE( observed.errors.size() == 1u );
    const auto& classified = observed.errors.front().second;
    CHECK( classified.error.code == "ios-pair-required" );
    CHECK( classified.error.retryPolicy == RetryPolicy::AwaitUser );
    CHECK( classified.awaitingUserReason == AwaitingUserReason::Pair );
    CHECK( std::find( state.calls.cbegin(), state.calls.cend(), "lockdown-existing-pair" )
           == state.calls.cend() );
    REQUIRE_FALSE( state.calls.empty() );
    CHECK( state.calls.back() == "device-free" );
}

TEST_CASE( "native iOS worker selects os_trace for iOS 9 and newer and syslog only below iOS 9",
           "[ios][native][stream][service][version]" )
{
    struct VersionCase {
        const char* version;
        const char* service;
        const char* client;
        bool queriesTimeZone;
    };
    const std::vector<VersionCase> cases{
        { "8.4.1", "service-start:com.apple.syslog_relay", "syslog-new", false },
        { "9.0", "service-start:com.apple.os_trace_relay", "ostrace-new", true },
        { "17.6.1", "service-start:com.apple.os_trace_relay", "ostrace-new", true },
    };

    for ( const auto& value : cases ) {
        INFO( value.version );
        FakeNative state;
        fake = &state;
        ManualExecutor executor;
        ObservedCallbacks observed;
        IosNativeStreamWorker worker( makeApi(), executor.executor(), config( 20u, value.version ),
                                      observed.callbacks() );
        REQUIRE( worker.start() );
        executor.runAllOnWorker();
        CHECK( std::find( state.calls.cbegin(), state.calls.cend(), value.service )
               != state.calls.cend() );
        CHECK( std::find( state.calls.cbegin(), state.calls.cend(), value.client )
               != state.calls.cend() );
        if ( value.queriesTimeZone ) {
            CHECK( indexOf( state.calls, "get:ProductVersion" )
                   < indexOf( state.calls, "get:TimeZone" ) );
            CHECK( indexOf( state.calls, "get:TimeZone" ) < indexOf( state.calls, value.service ) );
        }
        else {
            CHECK( std::find( state.calls.cbegin(), state.calls.cend(), "get:TimeZone" )
                   == state.calls.cend() );
        }
        CHECK( observed.ready == std::vector<Generation>{ 20u } );
        worker.stop( 20u );
        executor.runAllOnWorker();
    }
}

TEST_CASE( "TimeZone native errors retain the existing Lockdown classification",
           "[ios][native][stream][startup][timezone][error][mapping]" )
{
    FakeNative state;
    fake = &state;
    state.timeZoneResult = -8;
    ManualExecutor executor;
    ObservedCallbacks observed;
    IosNativeStreamWorker worker( makeApi(), executor.executor(), config( 201u ),
                                  observed.callbacks() );

    REQUIRE( worker.start() );
    executor.runAllOnWorker();

    CHECK( observed.ready.empty() );
    REQUIRE( observed.errors.size() == 1u );
    const auto& classified = observed.errors.front().second;
    CHECK( classified.error.code == "ios-device-disconnected" );
    CHECK( classified.error.category == ErrorCategory::Device );
    CHECK( classified.error.scope == ErrorScope::Device );
    CHECK( classified.error.retryPolicy == RetryPolicy::WaitForDevice );
    CHECK( classified.error.code != "ios-device-time-zone-invalid" );
    CHECK( classified.error.nativeDetail.find( "read TimeZone" ) != std::string::npos );
    CHECK( classified.error.nativeDetail.find( "-8" ) != std::string::npos );
    CHECK_FALSE( classified.awaitingUserReason.has_value() );
    CHECK( std::find( state.calls.cbegin(), state.calls.cend(),
                      "service-start:com.apple.os_trace_relay" )
           == state.calls.cend() );
    CHECK_FALSE( state.abiViolation );
}

TEST_CASE( "os_trace startup rejects null empty or unrecognized device time zones before readiness",
           "[ios][native][stream][startup][timezone][configuration]" )
{
    struct TimeZoneCase {
        const char* name;
        std::function<void( FakeNative& )> arrange;
        const char* diagnostic;
    };
    const std::vector<TimeZoneCase> cases{
        { "missing null TimeZone", []( FakeNative& value ) { value.nullTimeZoneOnSuccess = true; },
          "missing" },
        { "empty TimeZone", []( FakeNative& value ) { value.timeZone.clear(); }, "unrecognized" },
        { "unrecognized TimeZone",
          []( FakeNative& value ) { value.timeZone = "Mars/Olympus_Mons"; }, "unrecognized" },
    };

    Generation generation = 210u;
    for ( const auto& value : cases ) {
        INFO( value.name );
        FakeNative state;
        fake = &state;
        value.arrange( state );
        ManualExecutor executor;
        ObservedCallbacks observed;
        IosNativeStreamWorker worker( makeApi(), executor.executor(), config( ++generation ),
                                      observed.callbacks() );

        REQUIRE( worker.start() );
        executor.runAllOnWorker();

        CHECK( observed.ready.empty() );
        REQUIRE( observed.errors.size() == 1u );
        const auto& error = observed.errors.front().second.error;
        CHECK( error.code == "ios-device-time-zone-invalid" );
        CHECK( error.category == ErrorCategory::Configuration );
        CHECK( error.scope == ErrorScope::Device );
        CHECK( error.retryPolicy == RetryPolicy::Never );
        CHECK( error.message.find( "time zone" ) != std::string::npos );
        CHECK( error.nativeDetail.find( "TimeZone" ) != std::string::npos );
        CHECK( error.nativeDetail.find( value.diagnostic ) != std::string::npos );
        CHECK( indexOf( state.calls, "get:ProductVersion" )
               < indexOf( state.calls, "get:TimeZone" ) );
        CHECK( std::find( state.calls.cbegin(), state.calls.cend(),
                          "service-start:com.apple.os_trace_relay" )
               == state.calls.cend() );
        CHECK_FALSE( state.abiViolation );
    }
}

TEST_CASE( "stop during a blocked TimeZone query prevents all startup publication and service work",
           "[ios][native][stream][startup][timezone][stop][barrier]" )
{
    FakeNative state;
    fake = &state;
    state.blockTimeZone = true;
    ManualExecutor executor;
    ObservedCallbacks observed;
    IosNativeStreamWorker worker( makeApi(), executor.executor(), config( 220u ),
                                  observed.callbacks() );
    REQUIRE( worker.start() );
    auto startup = executor.startNextOnWorker();
    if ( !state.waitUntilTimeZoneEntered() ) {
        state.releaseTimeZone();
        startup.join();
        FAIL( "native startup did not reach the TimeZone query barrier" );
    }

    worker.stop( 220u );
    REQUIRE( executor.pending() == 1u );
    state.releaseTimeZone();
    startup.join();
    executor.runAllOnWorker();

    CHECK( observed.ready.empty() );
    CHECK( observed.bytesAvailable.empty() );
    CHECK( observed.errors.empty() );
    CHECK( observed.stopped == std::vector<Generation>{ 220u } );
    CHECK( std::find( state.calls.cbegin(), state.calls.cend(),
                      "service-start:com.apple.os_trace_relay" )
           == state.calls.cend() );
    CHECK( std::find( state.calls.cbegin(), state.calls.cend(), "ostrace-new" )
           == state.calls.cend() );
    CHECK( std::find( state.calls.cbegin(), state.calls.cend(), "ostrace-start" )
           == state.calls.cend() );
    CHECK( std::count( state.calls.cbegin(), state.calls.cend(), "lockdown-free" ) == 1 );
    CHECK( std::count( state.calls.cbegin(), state.calls.cend(), "device-free" ) == 1 );
    CHECK_FALSE( state.abiViolation );
}

TEST_CASE( "native iOS startup validates the selected stream ABI before device metadata I/O",
           "[ios][native][stream][startup][abi][ordering]" )
{
    FakeNative state;
    fake = &state;
    ManualExecutor executor;
    ObservedCallbacks observed;
    auto api = makeApi();
    api.osTraceStart = nullptr;
    IosNativeStreamWorker worker( std::move( api ), executor.executor(), config( 221u ),
                                  observed.callbacks() );

    REQUIRE( worker.start() );
    executor.runAllOnWorker();

    REQUIRE( observed.errors.size() == 1u );
    CHECK( observed.errors.front().second.error.code == "ios-native-abi-incomplete" );
    CHECK( std::find( state.calls.cbegin(), state.calls.cend(), "get:ProductVersion" )
           != state.calls.cend() );
    CHECK( std::find( state.calls.cbegin(), state.calls.cend(), "get:TimeZone" )
           == state.calls.cend() );
    CHECK( std::find( state.calls.cbegin(), state.calls.cend(),
                      "service-start:com.apple.os_trace_relay" )
           == state.calls.cend() );
}

TEST_CASE( "native iOS readiness requires service descriptor stream handle and armed read",
           "[ios][native][stream][readiness]" )
{
    FakeNative state;
    fake = &state;
    state.osTraceStartResult = -5;
    ManualExecutor executor;
    ObservedCallbacks observed;
    IosNativeStreamWorker worker( makeApi(), executor.executor(), config( 21u ),
                                  observed.callbacks() );

    REQUIRE( worker.start() );
    executor.runAllOnWorker();

    CHECK( observed.ready.empty() );
    REQUIRE( observed.errors.size() == 1u );
    CHECK( observed.errors.front().second.error.code == "ios-timeout" );
    CHECK( indexOf( state.calls, "service-start:com.apple.os_trace_relay" )
           < indexOf( state.calls, "ostrace-new" ) );
    CHECK( indexOf( state.calls, "ostrace-new" ) < indexOf( state.calls, "ostrace-start" ) );
}

TEST_CASE( "synchronous terminal callback during start prevents readiness commit",
           "[ios][native][stream][startup][terminal][reentrant]" )
{
    FakeNative state;
    fake = &state;
    state.emitOsTraceErrorDuringStart = true;
    ManualExecutor executor;
    ObservedCallbacks observed;
    IosNativeStreamWorker worker( makeApi(), executor.executor(), config( 22u ),
                                  observed.callbacks() );
    REQUIRE( worker.start() );
    executor.runAllOnWorker();

    CHECK( observed.ready.empty() );
    REQUIRE( observed.errors.size() == 1u );
    CHECK( observed.errors.front().second.error.code == "ios-device-disconnected" );
    worker.stop( 22u );
    executor.runAllOnWorker();
}

TEST_CASE( "native iOS worker maps startup failures precisely and preserves native diagnostics",
           "[ios][native][stream][error][mapping]" )
{
    struct ErrorCase {
        const char* name;
        std::function<void( FakeNative& )> arrange;
        const char* code;
        RetryPolicy retry;
        std::optional<AwaitingUserReason> reason;
    };
    const std::vector<ErrorCase> cases{
        { "trust pending", []( auto& value ) { value.handshakeResult = -19; }, "ios-trust-pending",
          RetryPolicy::AwaitUser, AwaitingUserReason::Trust },
        { "trust denied", []( auto& value ) { value.handshakeResult = -18; }, "ios-trust-denied",
          RetryPolicy::AwaitUser, AwaitingUserReason::Trust },
        { "unlock", []( auto& value ) { value.handshakeResult = -17; }, "ios-device-locked",
          RetryPolicy::AwaitUser, AwaitingUserReason::Unlock },
        { "stale pair", []( auto& value ) { value.handshakeResult = -31; }, "ios-stale-pair",
          RetryPolicy::AwaitUser, AwaitingUserReason::Pair },
        { "policy", []( auto& value ) { value.startServiceResult = -34; }, "ios-service-prohibited",
          RetryPolicy::Never, std::nullopt },
        { "disconnect", []( auto& value ) { value.deviceResult = -3; }, "ios-device-disconnected",
          RetryPolicy::WaitForDevice, std::nullopt },
        { "service missing", []( auto& value ) { value.startServiceResult = -26; },
          "ios-service-missing", RetryPolicy::Never, std::nullopt },
    };

    for ( const auto& value : cases ) {
        INFO( value.name );
        FakeNative state;
        fake = &state;
        value.arrange( state );
        ManualExecutor executor;
        ObservedCallbacks observed;
        IosNativeStreamWorker worker( makeApi(), executor.executor(), config( 30u ),
                                      observed.callbacks() );
        REQUIRE( worker.start() );
        executor.runAllOnWorker();
        REQUIRE( observed.errors.size() == 1u );
        const auto& error = observed.errors.front().second;
        CHECK( error.error.code == value.code );
        CHECK( error.error.retryPolicy == value.retry );
        CHECK( error.awaitingUserReason == value.reason );
        CHECK_FALSE( error.error.nativeDetail.empty() );
    }
}

TEST_CASE( "native startup contains C++ exceptions without terminating the executor",
           "[ios][native][stream][startup][exception-boundary]" )
{
    FakeNative state;
    fake = &state;
    state.throwDuringDeviceNew = true;
    ManualExecutor executor;
    ObservedCallbacks observed;
    IosNativeStreamWorker worker( makeApi(), executor.executor(), config( 301u ),
                                  observed.callbacks() );

    REQUIRE( worker.start() );
    CHECK_NOTHROW( executor.runAllOnWorker() );
    REQUIRE( observed.errors.size() == 1u );
    CHECK( observed.errors.front().second.error.code == "ios-native-startup-exception" );
    CHECK( observed.stopped == std::vector<Generation>{ 301u } );
}

TEST_CASE( "startup owns partial native handles and rejects success with null handles",
           "[ios][native][stream][startup][ownership][malformed-abi]" )
{
    struct HandleCase {
        const char* name;
        std::function<void( FakeNative& )> arrange;
        const char* expectedFree;
        const char* forbiddenFollowUp;
        const char* version;
    };
    const std::vector<HandleCase> cases{
        { "device on error",
          []( auto& value ) {
              value.deviceResult = -3;
              value.allocateDeviceOnError = true;
          },
          "device-free", "pair-record:device-udid", "17.6" },
        { "device success null", []( auto& value ) { value.nullDeviceOnSuccess = true; }, "",
          "pair-record:device-udid", "17.6" },
        { "lockdown on error",
          []( auto& value ) {
              value.handshakeResult = -19;
              value.allocateLockdownOnError = true;
          },
          "lockdown-free", "get:ProductVersion", "17.6" },
        { "lockdown success null", []( auto& value ) { value.nullLockdownOnSuccess = true; },
          "device-free", "get:ProductVersion", "17.6" },
        { "service on error",
          []( auto& value ) {
              value.startServiceResult = -26;
              value.allocateServiceOnError = true;
          },
          "service-free", "ostrace-new", "17.6" },
        { "service success null", []( auto& value ) { value.nullServiceOnSuccess = true; },
          "lockdown-free", "ostrace-new", "17.6" },
        { "ostrace on error",
          []( auto& value ) {
              value.osTraceNewResult = -2;
              value.allocateOsTraceOnError = true;
          },
          "ostrace-free", "ostrace-start", "17.6" },
        { "ostrace success null", []( auto& value ) { value.nullOsTraceOnSuccess = true; },
          "service-free", "ostrace-start", "17.6" },
        { "syslog on error",
          []( auto& value ) {
              value.syslogNewResult = -2;
              value.allocateSyslogOnError = true;
          },
          "syslog-free", "syslog-start", "8.4" },
        { "syslog success null", []( auto& value ) { value.nullSyslogOnSuccess = true; },
          "service-free", "syslog-start", "8.4" },
    };

    for ( const auto& value : cases ) {
        INFO( value.name );
        FakeNative state;
        fake = &state;
        value.arrange( state );
        ManualExecutor executor;
        ObservedCallbacks observed;
        IosNativeStreamWorker worker( makeApi(), executor.executor(), config( 40u, value.version ),
                                      observed.callbacks() );
        REQUIRE( worker.start() );
        executor.runAllOnWorker();

        REQUIRE( observed.errors.size() == 1u );
        CHECK( observed.ready.empty() );
        if ( std::string{ value.expectedFree }.empty() ) {
            CHECK( std::none_of( state.calls.cbegin(), state.calls.cend(), []( const auto& call ) {
                return call.find( "-free" ) != std::string::npos;
            } ) );
        }
        else {
            CHECK( std::count( state.calls.cbegin(), state.calls.cend(), value.expectedFree )
                   == 1 );
        }
        CHECK( std::find( state.calls.cbegin(), state.calls.cend(), value.forbiddenFollowUp )
               == state.calls.cend() );
        CHECK_FALSE( state.abiViolation );
    }
}

TEST_CASE( "os_trace packets are emitted in the fake source device time zone",
           "[ios][native][stream][callback][format][timezone]" )
{
    FakeNative state;
    fake = &state;
    state.timeZone = "Asia/Kolkata";
    ManualExecutor executor;
    ObservedCallbacks observed;
    IosNativeStreamWorker worker( makeApi(), executor.executor(), config( 401u ),
                                  observed.callbacks() );
    REQUIRE( worker.start() );
    executor.runAllOnWorker();
    REQUIRE( observed.ready == std::vector<Generation>{ 401u } );

    state.emitOsTrace( osTracePacket() );

    REQUIRE( observed.bytesAvailable == std::vector<Generation>{ 401u } );
    const auto drained = worker.drain();
    REQUIRE( drained.has_value() );
    const std::string output( drained->bytes.begin(), drained->bytes.end() );
    CHECK( output
           == "2023-11-15 03:43:20.123456+05:30 Test{}[42] <ERROR>: native log\n" );
    CHECK( output.find( "2023-11-14 22:13:20" ) == std::string::npos );
    CHECK_FALSE( state.abiViolation );

    worker.stop( 401u );
    executor.runAllOnWorker();
}

TEST_CASE( "os_trace rejects epochs the device time zone cannot represent",
           "[ios][native][stream][callback][format][timezone][malformed]" )
{
    FakeNative state;
    fake = &state;
    ManualExecutor executor;
    ObservedCallbacks observed;
    IosNativeStreamWorker worker( makeApi(), executor.executor(), config( 402u ),
                                  observed.callbacks() );
    REQUIRE( worker.start() );
    executor.runAllOnWorker();
    REQUIRE( observed.ready == std::vector<Generation>{ 402u } );

    auto packet = osTracePacket();
    putLe64( packet, 55u, std::numeric_limits<std::uint64_t>::max() );
    state.emitOsTrace( packet );

    REQUIRE( observed.errors.size() == 1u );
    CHECK( observed.errors.front().second.error.code == "ios-ostrace-malformed-packet" );
    CHECK( observed.errors.front().second.error.nativeDetail.find( "timestamp" )
           != std::string::npos );
    CHECK_FALSE( worker.drain().has_value() );
    CHECK_FALSE( state.abiViolation );

    worker.stop( 402u );
    executor.runAllOnWorker();
}

TEST_CASE( "os_trace callback copies decodes formats and queues bytes without unwinding into C",
           "[ios][native][stream][callback][ownership][format]" )
{
    FakeNative state;
    fake = &state;
    ManualExecutor executor;
    ObservedCallbacks observed;
    auto options = config( 41u );
    options.ansiOutputEnabled = true;
    IosNativeStreamWorker worker( makeApi(), executor.executor(), options, observed.callbacks() );
    REQUIRE( worker.start() );
    executor.runAllOnWorker();

    auto borrowed = osTracePacket( "copy me" );
    CHECK_NOTHROW( state.emitOsTrace( borrowed ) );
    std::fill( borrowed.begin(), borrowed.end(), std::uint8_t{ 0xa5 } );

    REQUIRE( observed.bytesAvailable == std::vector<Generation>{ 41u } );
    const auto drained = worker.drain();
    REQUIRE( drained.has_value() );
    const std::string output( drained->bytes.begin(), drained->bytes.end() );
    CHECK( output.find( "copy me" ) != std::string::npos );
    CHECK( output.find( "\x1b[" ) != std::string::npos );
    CHECK( output.back() == '\n' );
    CHECK_FALSE( state.callbackTeardownViolation() );

    auto activity = osTracePacket( "not-an-activity-text-span" );
    putLe32( activity, 1u, 2u );
    activity.resize( 0x81u + 12u );
    putLe16( activity, 37u, std::numeric_limits<std::uint16_t>::max() );
    putLe32( activity, 109u, std::numeric_limits<std::uint32_t>::max() );
    CHECK_NOTHROW( state.emitOsTrace( activity ) );
    CHECK( observed.errors.empty() );
    const auto activityBatch = worker.drain();
    REQUIRE( activityBatch.has_value() );
    const std::string activityOutput( activityBatch->bytes.begin(), activityBatch->bytes.end() );
    CHECK( activityOutput.find( "not-an-activity-text-span" ) == std::string::npos );
    CHECK( activityOutput.back() == '\n' );

    const auto notificationsBeforeControl = observed.bytesAvailable.size();
    CHECK_NOTHROW( state.emitOsTrace(
        std::vector<std::uint8_t>{ 'b', 'p', 'l', 'i', 's', 't', '0', '0', 0xd1u, 0x01u } ) );
    CHECK( observed.errors.empty() );
    CHECK( observed.bytesAvailable.size() == notificationsBeforeControl );
    CHECK_FALSE( worker.drain().has_value() );

    observed.throwFromBytesAvailable = true;
    CHECK_NOTHROW( state.emitOsTrace( osTracePacket( "observer throws" ) ) );
    CHECK_FALSE( state.callbackTeardownViolation() );
    worker.stop( 41u );
    executor.runAllOnWorker();
}

TEST_CASE( "oversized os_trace callbacks are rejected before copying borrowed bytes",
           "[ios][native][stream][ostrace][bounded][ownership]" )
{
    FakeNative state;
    fake = &state;
    ManualExecutor executor;
    ObservedCallbacks observed;
    IosNativeStreamWorker worker( makeApi(), executor.executor(), config( 411u ),
                                  observed.callbacks() );
    REQUIRE( worker.start() );
    executor.runAllOnWorker();

    std::vector<std::uint8_t> oversized( DefaultMaximumOsTraceRecordSize + 1u, 0u );
    state.emitOsTrace( oversized );

    REQUIRE( observed.errors.size() == 1u );
    CHECK( observed.errors.front().second.error.code == "ios-ostrace-malformed-packet" );
    CHECK( observed.errors.front().second.error.nativeDetail.find( "before copying" )
           != std::string::npos );
    CHECK_FALSE( worker.drain().has_value() );
    worker.stop( 411u );
    executor.runAllOnWorker();
}

TEST_CASE( "malformed os_trace packets and native packet errors are terminal and diagnosable",
           "[ios][native][stream][ostrace][malformed][error]" )
{
    SECTION( "malformed packet" )
    {
        FakeNative state;
        fake = &state;
        ManualExecutor executor;
        ObservedCallbacks observed;
        IosNativeStreamWorker worker( makeApi(), executor.executor(), config( 42u ),
                                      observed.callbacks() );
        REQUIRE( worker.start() );
        executor.runAllOnWorker();

        CHECK_NOTHROW( state.emitOsTrace( std::vector<std::uint8_t>{ 2u, 8u, 0u } ) );
        REQUIRE( observed.errors.size() == 1u );
        CHECK( observed.errors.front().second.error.code == "ios-ostrace-malformed-packet" );
        CHECK_FALSE( observed.errors.front().second.error.nativeDetail.empty() );
        CHECK_FALSE( state.callbackTeardownViolation() );
        REQUIRE( executor.pending() == 1u );
        executor.runAllOnWorker();
        CHECK( std::count( state.calls.cbegin(), state.calls.cend(), "ostrace-stop" ) == 1 );
        CHECK( observed.stopped == std::vector<Generation>{ 42u } );
    }

    SECTION( "structural diagnostics contain no packet text or raw bytes" )
    {
        FakeNative state;
        fake = &state;
        ManualExecutor executor;
        ObservedCallbacks observed;
        IosNativeStreamWorker worker( makeApi(), executor.executor(), config( 44u ),
                                      observed.callbacks() );
        REQUIRE( worker.start() );
        executor.runAllOnWorker();

        auto packet = osTracePacket( "SENSITIVE-PACKET-TEXT" );
        putLe32( packet, 109u, std::numeric_limits<std::uint32_t>::max() );
        state.emitOsTrace( packet );

        REQUIRE( observed.errors.size() == 1u );
        const auto& detail = observed.errors.front().second.error.nativeDetail;
        CHECK( detail.find( "packet_bytes=" ) != std::string::npos );
        CHECK( detail.find( "packet_type=8" ) != std::string::npos );
        CHECK( detail.find( "header_bytes=129" ) != std::string::npos );
        CHECK( detail.find( "spans=" ) != std::string::npos );
        CHECK( detail.find( "declared_span_bytes=" ) != std::string::npos );
        CHECK( detail.find( "available_span_bytes=" ) != std::string::npos );
        CHECK( detail.find( "SENSITIVE-PACKET-TEXT" ) == std::string::npos );
        worker.stop( 44u );
        executor.runAllOnWorker();
    }

    SECTION( "native packet receive error" )
    {
        FakeNative state;
        fake = &state;
        ManualExecutor executor;
        ObservedCallbacks observed;
        IosNativeStreamWorker worker( makeApi(), executor.executor(), config( 43u ),
                                      observed.callbacks() );
        REQUIRE( worker.start() );
        executor.runAllOnWorker();

        CHECK_NOTHROW( state.emitOsTraceError( -2 ) );
        REQUIRE( observed.errors.size() == 1u );
        CHECK( observed.errors.front().second.error.code == "ios-device-disconnected" );
        CHECK_FALSE( state.callbackTeardownViolation() );
        executor.runAllOnWorker();
    }
}

TEST_CASE(
    "legacy syslog assembles NUL chunks and notifies once instead of locking UI per character",
    "[ios][native][stream][syslog][chunking][ansi]" )
{
    FakeNative state;
    fake = &state;
    state.timeZoneResult = -8;
    state.timeZone = "Not/A_TimeZone";
    ManualExecutor executor;
    ObservedCallbacks observed;
    auto options = config( 51u, "8.4" );
    options.ansiOutputEnabled = true;
    IosNativeStreamWorker worker( makeApi(), executor.executor(), options, observed.callbacks() );
    REQUIRE( worker.start() );
    executor.runAllOnWorker();
    CHECK( observed.ready == std::vector<Generation>{ 51u } );
    CHECK( std::find( state.calls.cbegin(), state.calls.cend(), "get:TimeZone" )
           == state.calls.cend() );

    CHECK_NOTHROW( state.emitSyslog( "alp" ) );
    CHECK( observed.bytesAvailable.empty() );
    CHECK_FALSE( worker.drain().has_value() );
    CHECK_NOTHROW( state.emitSyslog( "ha\0\x1b[31mbet"s ) );
    CHECK_NOTHROW( state.emitSyslog( "a\x1b[0m\0"s ) );
    REQUIRE( observed.bytesAvailable == std::vector<Generation>{ 51u } );
    const auto drained = worker.drain();
    REQUIRE( drained.has_value() );
    CHECK( drained->sourceChunks == 2u );
    const std::string output( drained->bytes.begin(), drained->bytes.end() );
    CHECK( output == "alpha\n\x1b[31mbeta\x1b[0m\n" );
    CHECK_FALSE( state.callbackTeardownViolation() );

    worker.stop( 51u );
    executor.runAllOnWorker();
}

TEST_CASE( "legacy syslog rejects an unterminated record at the configured bound",
           "[ios][native][stream][syslog][bounded-record]" )
{
    FakeNative state;
    fake = &state;
    ManualExecutor executor;
    ObservedCallbacks observed;
    auto options = config( 511u, "8.4" );
    options.maximumSyslogRecordBytes = 4u;
    IosNativeStreamWorker worker( makeApi(), executor.executor(), options, observed.callbacks() );
    REQUIRE( worker.start() );
    executor.runAllOnWorker();

    state.emitSyslog( "abcde" );
    REQUIRE( observed.errors.size() == 1u );
    CHECK( observed.errors.front().second.error.code == "ios-syslog-record-too-large" );
    CHECK_FALSE( worker.drain().has_value() );
    worker.stop( 511u );
    executor.runAllOnWorker();
}

TEST_CASE( "legacy syslog terminal errors use syslog relay ABI codes",
           "[ios][native][stream][syslog][error][mapping]" )
{
    struct ExpectedError {
        std::int32_t nativeCode;
        const char* stableCode;
        ErrorScope scope;
    };
    const ExpectedError cases[]{
        { -2, "ios-device-disconnected", ErrorScope::Device },
        { -3, "ios-ssl-error", ErrorScope::Service },
        { -4, "ios-syslog-truncated-record", ErrorScope::Stream },
        { -5, "ios-timeout", ErrorScope::Stream },
    };

    Generation generation = 520u;
    for ( const auto& expected : cases ) {
        ++generation;
        FakeNative state;
        fake = &state;
        ManualExecutor executor;
        ObservedCallbacks observed;
        IosNativeStreamWorker worker( makeApi(), executor.executor(), config( generation, "8.4" ),
                                      observed.callbacks() );
        REQUIRE( worker.start() );
        executor.runAllOnWorker();

        state.emitSyslogError( expected.nativeCode );
        REQUIRE( observed.errors.size() == 1u );
        CHECK( observed.errors.front().second.error.code == expected.stableCode );
        CHECK( observed.errors.front().second.error.scope == expected.scope );
        worker.stop( generation );
        executor.runAllOnWorker();
    }
}

TEST_CASE( "bounded native queue reports backpressure and statistics without silent drops",
           "[ios][native][stream][queue][backpressure][statistics]" )
{
    FakeNative state;
    fake = &state;
    ManualExecutor executor;
    ObservedCallbacks observed;
    auto options = config( 52u, "8.4" );
    options.queueLimits = LiveDataQueueLimits{ 2u, 1u };
    IosNativeStreamWorker worker( makeApi(), executor.executor(), options, observed.callbacks() );
    REQUIRE( worker.start() );
    executor.runAllOnWorker();

    state.emitSyslog( "a\0"s );
    state.emitSyslog( "b\0"s );

    REQUIRE( observed.errors.size() == 1u );
    CHECK( observed.errors.front().second.error.code == "ios-live-queue-saturated" );
    const auto statistics = worker.statistics();
    CHECK( statistics.receivedBytes == 4u );
    CHECK( statistics.receivedChunks == 2u );
    CHECK( statistics.queuedBytes == 2u );
    CHECK( statistics.queuedChunks == 1u );
    CHECK( statistics.backpressuredBytes == 2u );
    CHECK( statistics.backpressuredChunks == 1u );
    CHECK( statistics.highWaterQueuedBytes == 2u );
    CHECK_FALSE( state.callbackTeardownViolation() );
    executor.runAllOnWorker();
}

TEST_CASE( "native stop interrupts a blocked partial-frame receive before cleanup acknowledgement",
           "[ios][native][stream][stop][interrupt][partial-frame]" )
{
    FakeNative state;
    fake = &state;
    state.blockOsTraceReceive = true;
    ManualExecutor executor;
    ObservedCallbacks observed;
    IosNativeStreamWorker worker( makeApi(), executor.executor(), config( 60u ),
                                  observed.callbacks() );
    REQUIRE( worker.start() );
    executor.runAllOnWorker();
    state.waitUntilReceiveEntered();

    worker.stop( 60u );
    REQUIRE( executor.pending() == 1u );
    executor.runNextOnWorker();

    CHECK( state.receiveInterrupted );
    CHECK( state.receiveExited );
    CHECK( observed.stopped == std::vector<Generation>{ 60u } );
    CHECK( indexOf( state.calls, "ostrace-stop" ) < indexOf( state.calls, "ostrace-free" ) );
    CHECK_FALSE( state.abiViolation );
}

TEST_CASE( "native stop is asynchronous ordered and recreates one-shot clients for the next run",
           "[ios][native][stream][stop][cleanup][recreate]" )
{
    FakeNative state;
    fake = &state;
    ManualExecutor executor;
    ObservedCallbacks firstObserved;
    IosNativeStreamWorker first( makeApi(), executor.executor(), config( 61u ),
                                 firstObserved.callbacks() );
    REQUIRE( first.start() );
    executor.runAllOnWorker();
    state.emitOsTrace( osTracePacket( "before stop" ) );

    first.stop( 61u );
    REQUIRE_FALSE( state.calls.empty() );
    CHECK( state.calls.back() == "ostrace-start" );
    REQUIRE( executor.pending() == 1u );
    executor.runNextOnWorker();

    const auto stop = indexOf( state.calls, "ostrace-stop" );
    const auto clientFree = indexOf( state.calls, "ostrace-free" );
    const auto serviceFree = indexOf( state.calls, "service-free" );
    const auto lockdownFree = indexOf( state.calls, "lockdown-free" );
    const auto deviceFree = indexOf( state.calls, "device-free" );
    CHECK( stop < clientFree );
    CHECK( clientFree < serviceFree );
    CHECK( serviceFree < lockdownFree );
    CHECK( lockdownFree < deviceFree );
    CHECK( firstObserved.stopped == std::vector<Generation>{ 61u } );
    CHECK_FALSE( state.callbackTeardownViolation() );

    ObservedCallbacks secondObserved;
    IosNativeStreamWorker second( makeApi(), executor.executor(), config( 62u ),
                                  secondObserved.callbacks() );
    REQUIRE( second.start() );
    executor.runAllOnWorker();
    CHECK( std::count( state.calls.cbegin(), state.calls.cend(), "ostrace-new" ) == 2 );
    CHECK( std::count( state.calls.cbegin(), state.calls.cend(), "ostrace-start" ) == 2 );
    second.stop( 62u );
    executor.runAllOnWorker();
}

TEST_CASE( "cleanup waits for an in-flight native callback before stop and free",
           "[ios][native][stream][callback][stop][barrier]" )
{
    FakeNative state;
    fake = &state;
    ManualExecutor executor;
    std::mutex callbackMutex;
    std::condition_variable callbackChanged;
    bool callbackEntered = false;
    bool releaseCallback = false;
    IosNativeStreamCallbacks callbacks;
    callbacks.ready = []( Generation ) {};
    callbacks.bytesAvailable = [ & ]( Generation ) {
        std::unique_lock<std::mutex> lock( callbackMutex );
        callbackEntered = true;
        callbackChanged.notify_all();
        callbackChanged.wait( lock, [ & ] { return releaseCallback; } );
    };
    callbacks.failed = []( Generation, const ClassifiedIosNativeError& ) {};
    callbacks.stopped = []( Generation ) {};
    IosNativeStreamWorker worker( makeApi(), executor.executor(), config( 73u ),
                                  std::move( callbacks ) );
    REQUIRE( worker.start() );
    executor.runAllOnWorker();

    std::thread callbackThread( [ & ] { state.emitOsTrace( osTracePacket( "in flight" ) ); } );
    {
        std::unique_lock<std::mutex> lock( callbackMutex );
        callbackChanged.wait( lock, [ & ] { return callbackEntered; } );
    }
    worker.stop( 73u );
    auto cleanupThread = executor.startNextOnWorker();
    std::this_thread::sleep_for( 20ms );
    CHECK_FALSE( state.callbackTeardownViolation() );
    {
        std::lock_guard<std::mutex> lock( callbackMutex );
        releaseCallback = true;
        callbackChanged.notify_all();
    }
    callbackThread.join();
    cleanupThread.join();

    CHECK_FALSE( state.abiViolation.load() );
}

TEST_CASE( "cleanup rejection falls back asynchronously and waits for active callbacks",
           "[ios][native][stream][callback][stop][executor-rejection]" )
{
    FakeNative state;
    fake = &state;
    std::optional<IosNativeStreamTask> startupTask;
    int dispatchCount = 0;
    const IosNativeStreamExecutor executor = [ & ]( IosNativeStreamTask task ) {
        if ( dispatchCount++ == 0 ) {
            startupTask = std::move( task );
            return;
        }
        throw std::runtime_error( "cleanup rejected" );
    };

    std::mutex callbackMutex;
    std::condition_variable callbackChanged;
    bool callbackEntered = false;
    bool releaseCallback = false;
    std::atomic<bool> stopped{ false };
    IosNativeStreamCallbacks callbacks;
    callbacks.ready = []( Generation ) {};
    callbacks.bytesAvailable = [ & ]( Generation ) {
        std::unique_lock<std::mutex> lock( callbackMutex );
        callbackEntered = true;
        callbackChanged.notify_all();
        callbackChanged.wait( lock, [ & ] { return releaseCallback; } );
    };
    callbacks.failed = []( Generation, const ClassifiedIosNativeError& ) {};
    callbacks.stopped = [ & ]( Generation ) { stopped = true; };

    IosNativeStreamWorker worker( makeApi(), executor, config( 74u ), std::move( callbacks ) );
    REQUIRE( worker.start() );
    REQUIRE( startupTask.has_value() );
    std::thread startup( std::move( *startupTask ) );
    startup.join();

    std::thread callbackThread(
        [ & ] { state.emitOsTrace( osTracePacket( "in flight" ) ); } );
    {
        std::unique_lock<std::mutex> lock( callbackMutex );
        REQUIRE( callbackChanged.wait_for( lock, 2s, [ & ] { return callbackEntered; } ) );
    }

    worker.stop( 74u );
    std::this_thread::sleep_for( 20ms );
    CHECK_FALSE( stopped.load() );
    CHECK_FALSE( state.callbackTeardownViolation() );

    {
        std::lock_guard<std::mutex> lock( callbackMutex );
        releaseCallback = true;
        callbackChanged.notify_all();
    }
    callbackThread.join();
    const auto stoppedDeadline = std::chrono::steady_clock::now() + 2s;
    while ( !stopped.load() && std::chrono::steady_clock::now() < stoppedDeadline ) {
        std::this_thread::sleep_for( 1ms );
    }

    CHECK( stopped.load() );
    CHECK_FALSE( state.callbackTeardownViolation() );
    CHECK( indexOf( state.calls, "ostrace-stop" ) < indexOf( state.calls, "ostrace-free" ) );
}

TEST_CASE( "worker ignores stale stop commands without retiring the current generation",
           "[ios][native][stream][generation][stale-command]" )
{
    FakeNative state;
    fake = &state;
    ManualExecutor executor;
    ObservedCallbacks observed;
    IosNativeStreamWorker worker( makeApi(), executor.executor(), config( 72u ),
                                  observed.callbacks() );
    REQUIRE( worker.start() );
    executor.runAllOnWorker();
    REQUIRE( observed.ready == std::vector<Generation>{ 72u } );

    worker.stop( 71u );
    CHECK( executor.pending() == 0u );
    CHECK( std::find( state.calls.cbegin(), state.calls.cend(), "ostrace-stop" )
           == state.calls.cend() );

    state.emitOsTrace( osTracePacket( "still current" ) );
    REQUIRE( observed.bytesAvailable == std::vector<Generation>{ 72u } );
    worker.stop( 72u );
    executor.runAllOnWorker();
    CHECK( observed.stopped == std::vector<Generation>{ 72u } );
}

TEST_CASE( "default worker factory bounds sessions while native startup remains blocked",
           "[ios][native][stream][factory][admission][blocked-startup]" )
{
    FakeNative state;
    fake = &state;
    state.blockHandshake = true;
    std::mutex mutex;
    std::condition_variable changed;
    bool stopped = false;
    IosNativeStreamCallbacks callbacks;
    callbacks.ready = []( Generation ) {};
    callbacks.bytesAvailable = []( Generation ) {};
    callbacks.failed = []( Generation, const ClassifiedIosNativeError& ) {};
    callbacks.stopped = [ & ]( Generation ) {
        {
            std::lock_guard<std::mutex> lock( mutex );
            stopped = true;
            changed.notify_all();
        }
    };

    DefaultIosNativeStreamWorkerFactory factory( makeApi(), 2u );
    auto creation = factory.create( config( 801u ), std::move( callbacks ) );
    REQUIRE( creation.session != nullptr );
    REQUIRE( creation.session->start() );
    if ( !state.waitUntilHandshakeEntered() ) {
        creation.session->stop( 801u );
        state.releaseHandshake();
        std::unique_lock<std::mutex> lock( mutex );
        if ( !changed.wait_for( lock, 2s, [ & ] { return stopped; } ) ) {
            FAIL( "native startup missed the handshake barrier and cleanup timed out" );
        }
        FAIL( "native startup did not reach the handshake barrier" );
    }
    struct HandshakeRelease final {
        FakeNative* state;
        ~HandshakeRelease()
        {
            if ( state != nullptr ) {
                state->releaseHandshake();
            }
        }
    } handshakeRelease{ &state };
    creation.session->stop( 801u );
    creation.session.reset();

    const auto duplicate = factory.create( config( 802u ), {} );
    CHECK( duplicate.session == nullptr );
    CHECK( duplicate.error.has_value() );
    if ( duplicate.error.has_value() ) {
        CHECK( duplicate.error->error.code == "ios-native-endpoint-busy" );
    }

    auto otherEndpoint = config( 803u );
    otherEndpoint.endpoint.udid = "other-device";
    auto other = factory.create( otherEndpoint, {} );
    CHECK( other.session != nullptr );
    auto thirdEndpoint = config( 804u );
    thirdEndpoint.endpoint.udid = "third-device";
    const auto capacity = factory.create( thirdEndpoint, {} );
    CHECK( capacity.session == nullptr );
    CHECK( capacity.error.has_value() );
    if ( capacity.error.has_value() ) {
        CHECK( capacity.error->error.code == "ios-native-session-capacity-exhausted" );
    }

    state.releaseHandshake();
    handshakeRelease.state = nullptr;
    std::unique_ptr<IosNativeStreamSession> replacement;
    const auto releaseDeadline = std::chrono::steady_clock::now() + 2s;
    while ( replacement == nullptr && std::chrono::steady_clock::now() < releaseDeadline ) {
        auto retry = factory.create( config( 805u ), {} );
        replacement = std::move( retry.session );
        if ( replacement == nullptr ) {
            std::this_thread::sleep_for( 1ms );
        }
    }
    CHECK( replacement != nullptr );
    {
        std::unique_lock<std::mutex> lock( mutex );
        CHECK( changed.wait_for( lock, 2s, [ & ] { return stopped; } ) );
    }
}

TEST_CASE( "default worker factory releases endpoint admission before stopped callback",
           "[ios][native][stream][factory][admission][reentrant]" )
{
    FakeNative state;
    fake = &state;
    std::mutex mutex;
    std::condition_variable changed;
    bool ready = false;
    bool stopped = false;
    std::unique_ptr<IosNativeStreamSession> replacement;
    std::optional<ClassifiedIosNativeError> replacementError;

    DefaultIosNativeStreamWorkerFactory factory( makeApi() );
    IosNativeStreamCallbacks callbacks;
    callbacks.ready = [ & ]( Generation ) {
        std::lock_guard<std::mutex> lock( mutex );
        ready = true;
        changed.notify_all();
    };
    callbacks.bytesAvailable = []( Generation ) {};
    callbacks.failed = []( Generation, const ClassifiedIosNativeError& ) {};
    callbacks.stopped = [ & ]( Generation ) {
        auto retried = factory.create( config( 806u ), {} );
        {
            std::lock_guard<std::mutex> lock( mutex );
            replacement = std::move( retried.session );
            replacementError = std::move( retried.error );
            stopped = true;
            changed.notify_all();
        }
    };

    auto creation = factory.create( config( 805u ), std::move( callbacks ) );
    REQUIRE( creation.session != nullptr );
    REQUIRE( creation.session->start() );
    {
        std::unique_lock<std::mutex> lock( mutex );
        REQUIRE( changed.wait_for( lock, 2s, [ & ] { return ready; } ) );
    }
    creation.session->stop( 805u );
    {
        std::unique_lock<std::mutex> lock( mutex );
        REQUIRE( changed.wait_for( lock, 2s, [ & ] { return stopped; } ) );
    }

    CHECK( replacement != nullptr );
    CHECK_FALSE( replacementError.has_value() );
}

TEST_CASE( "default worker factory drains cleanup without executor self-join",
           "[ios][native][stream][factory][lifetime][shutdown]" )
{
    FakeNative state;
    fake = &state;
    std::mutex mutex;
    std::condition_variable changed;
    bool ready = false;
    bool stopped = false;
    IosNativeStreamCallbacks callbacks;
    callbacks.ready = [ & ]( Generation ) {
        std::lock_guard<std::mutex> lock( mutex );
        ready = true;
        changed.notify_all();
    };
    callbacks.bytesAvailable = []( Generation ) {};
    callbacks.failed = []( Generation, const ClassifiedIosNativeError& ) {};
    callbacks.stopped = [ & ]( Generation ) {
        {
            std::lock_guard<std::mutex> lock( mutex );
            stopped = true;
            changed.notify_all();
        }
    };

    DefaultIosNativeStreamWorkerFactory factory( makeApi() );
    auto creation = factory.create( config( 81u ), std::move( callbacks ) );
    REQUIRE( creation.session != nullptr );
    auto session = std::move( creation.session );
    REQUIRE( session->start() );
    {
        std::unique_lock<std::mutex> lock( mutex );
        REQUIRE( changed.wait_for( lock, 2s, [ & ] { return ready; } ) );
    }
    session->stop( 81u );
    {
        std::unique_lock<std::mutex> lock( mutex );
        REQUIRE( changed.wait_for( lock, 2s, [ & ] { return stopped; } ) );
    }
    CHECK_NOTHROW( session.reset() );
}

TEST_CASE( "default worker session destruction never waits for a blocked native callback",
           "[ios][native][stream][factory][lifetime][async-cleanup][callback]" )
{
    FakeNative state;
    fake = &state;
    std::mutex mutex;
    std::condition_variable changed;
    bool ready = false;
    bool callbackEntered = false;
    bool releaseCallback = false;
    bool stopped = false;
    bool destroyed = false;
    IosNativeStreamCallbacks callbacks;
    callbacks.ready = [ & ]( Generation ) {
        std::lock_guard<std::mutex> lock( mutex );
        ready = true;
        changed.notify_all();
    };
    callbacks.bytesAvailable = [ & ]( Generation ) {
        std::unique_lock<std::mutex> lock( mutex );
        callbackEntered = true;
        changed.notify_all();
        changed.wait( lock, [ & ] { return releaseCallback; } );
    };
    callbacks.failed = []( Generation, const ClassifiedIosNativeError& ) {};
    callbacks.stopped = [ & ]( Generation ) {
        {
            std::lock_guard<std::mutex> lock( mutex );
            stopped = true;
            changed.notify_all();
        }
    };

    auto options = config( 811u );
    options.cleanupDeadline = 2s;
    DefaultIosNativeStreamWorkerFactory factory( makeApi() );
    auto creation = factory.create( options, std::move( callbacks ) );
    REQUIRE( creation.session != nullptr );
    auto session = std::move( creation.session );
    REQUIRE( session->start() );
    {
        std::unique_lock<std::mutex> lock( mutex );
        REQUIRE( changed.wait_for( lock, 2s, [ & ] { return ready; } ) );
    }

    std::thread callbackThread(
        [ & ] { state.emitOsTrace( osTracePacket( "blocked callback" ) ); } );
    {
        std::unique_lock<std::mutex> lock( mutex );
        REQUIRE( changed.wait_for( lock, 2s, [ & ] { return callbackEntered; } ) );
    }
    session->stop( 811u );
    std::thread destroyThread( [ & ] {
        session.reset();
        {
            std::lock_guard<std::mutex> lock( mutex );
            destroyed = true;
            changed.notify_all();
        }
    } );

    bool destroyedPromptly = false;
    {
        std::unique_lock<std::mutex> lock( mutex );
        destroyedPromptly = changed.wait_for( lock, 250ms, [ & ] { return destroyed; } );
        releaseCallback = true;
        changed.notify_all();
    }
    CHECK( destroyedPromptly );

    callbackThread.join();
    {
        std::unique_lock<std::mutex> lock( mutex );
        REQUIRE( changed.wait_for( lock, 2s, [ & ] { return stopped; } ) );
    }
    destroyThread.join();
    CHECK_FALSE( state.abiViolation );
}

TEST_CASE( "default worker session may be destroyed from a native data callback",
           "[ios][native][stream][factory][lifetime][callback-destroy][deadline]" )
{
    FakeNative state;
    fake = &state;
    std::mutex mutex;
    std::condition_variable changed;
    bool ready = false;
    bool destroyed = false;
    bool stopped = false;
    std::unique_ptr<IosNativeStreamSession> session;
    IosNativeStreamCallbacks callbacks;
    callbacks.ready = [ & ]( Generation ) {
        std::lock_guard<std::mutex> lock( mutex );
        ready = true;
        changed.notify_all();
    };
    callbacks.bytesAvailable = [ & ]( Generation ) {
        session.reset();
        {
            std::lock_guard<std::mutex> lock( mutex );
            destroyed = true;
            changed.notify_all();
        }
    };
    callbacks.failed = []( Generation, const ClassifiedIosNativeError& ) {};
    callbacks.stopped = [ & ]( Generation ) {
        {
            std::lock_guard<std::mutex> lock( mutex );
            stopped = true;
            changed.notify_all();
        }
    };

    auto options = config( 812u );
    options.cleanupDeadline = 75ms;
    state.blockCallbackReturn = true;
    DefaultIosNativeStreamWorkerFactory factory( makeApi() );
    auto creation = factory.create( options, std::move( callbacks ) );
    REQUIRE( creation.session != nullptr );
    session = std::move( creation.session );
    REQUIRE( session->start() );
    {
        std::unique_lock<std::mutex> lock( mutex );
        REQUIRE( changed.wait_for( lock, 2s, [ & ] { return ready; } ) );
    }

    std::thread callbackThread(
        [ & ] { state.emitOsTrace( osTracePacket( "destroy session" ) ); } );
    {
        std::unique_lock<std::mutex> lock( mutex );
        REQUIRE( changed.wait_for( lock, 1s, [ & ] { return destroyed; } ) );
    }
    REQUIRE( state.waitUntilCallbackBodyReturned() );
    REQUIRE( state.waitUntilStopEntered() );
    CHECK_FALSE( state.freeInsideCallback.load() );
    state.releaseCallbackReturn();
    {
        std::unique_lock<std::mutex> lock( mutex );
        REQUIRE( changed.wait_for( lock, 2s, [ & ] { return stopped; } ) );
    }
    callbackThread.join();
    CHECK_FALSE( state.callbackTeardownViolation() );
    CHECK_FALSE( state.abiViolation );
}

TEST_CASE( "default worker session may be destroyed from its stopped callback",
           "[ios][native][stream][factory][lifetime][worker-thread-destroy]" )
{
    FakeNative state;
    fake = &state;
    std::mutex mutex;
    std::condition_variable changed;
    bool ready = false;
    bool allowDestroy = false;
    bool destroyed = false;
    std::unique_ptr<IosNativeStreamSession> session;
    IosNativeStreamCallbacks callbacks;
    callbacks.ready = [ & ]( Generation ) {
        std::lock_guard<std::mutex> lock( mutex );
        ready = true;
        changed.notify_all();
    };
    callbacks.bytesAvailable = []( Generation ) {};
    callbacks.failed = []( Generation, const ClassifiedIosNativeError& ) {};
    callbacks.stopped = [ & ]( Generation ) {
        {
            std::unique_lock<std::mutex> lock( mutex );
            changed.wait( lock, [ & ] { return allowDestroy; } );
        }
        session.reset();
        {
            std::lock_guard<std::mutex> lock( mutex );
            destroyed = true;
            changed.notify_all();
        }
    };

    DefaultIosNativeStreamWorkerFactory factory( makeApi() );
    auto creation = factory.create( config( 82u ), std::move( callbacks ) );
    REQUIRE( creation.session != nullptr );
    session = std::move( creation.session );
    REQUIRE( session->start() );
    {
        std::unique_lock<std::mutex> lock( mutex );
        REQUIRE( changed.wait_for( lock, 2s, [ & ] { return ready; } ) );
    }
    session->stop( 82u );
    {
        std::lock_guard<std::mutex> lock( mutex );
        allowDestroy = true;
        changed.notify_all();
    }
    {
        std::unique_lock<std::mutex> lock( mutex );
        REQUIRE( changed.wait_for( lock, 2s, [ & ] { return destroyed; } ) );
    }
}

TEST_CASE( "worker shutdown rejects late callbacks and completes cleanup on its worker executor",
           "[ios][native][stream][shutdown][stale]" )
{
    FakeNative state;
    fake = &state;
    ManualExecutor executor;
    ObservedCallbacks observed;
    IosNativeStreamWorker worker( makeApi(), executor.executor(), config( 71u ),
                                  observed.callbacks() );
    REQUIRE( worker.start() );
    executor.runAllOnWorker();
    const auto oldCallback = state.osTraceActivityCallback;
    const auto oldContext = state.osTraceContext;

    worker.shutdown();
    executor.runAllOnWorker();
    const auto notificationsBeforeLateCallback = observed.bytesAvailable.size();
    const auto packet = osTracePacket( "late" );
    CHECK_NOTHROW(
        oldCallback( packet.data(), static_cast<std::uint32_t>( packet.size() ), oldContext ) );

    CHECK( observed.bytesAvailable.size() == notificationsBeforeLateCallback );
    CHECK( observed.stopped == std::vector<Generation>{ 71u } );
    CHECK_FALSE( state.callbackTeardownViolation() );
}
