/*
 * Copyright (C) 2026
 *
 * This file is part of klogg.
 */

#include "iosnativeadapter.h"

#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <utility>
#include <vector>

#ifdef __APPLE__
#include <dlfcn.h>
#endif

namespace klogg::livecapture::ios {
namespace {

#ifdef __APPLE__
struct NativeSymbols {
    std::int32_t ( *deviceListExtendedFree )( NativeDeviceInfo** ){ nullptr };
    std::int32_t ( *deviceNewWithOptions )( NativeIdevice*, const char*, std::int32_t ){ nullptr };
    std::int32_t ( *lockdownGetValue )( NativeLockdownClient, const char*, const char*,
                                        void** ){ nullptr };
    void ( *plistGetStringValue )( void*, char** ){ nullptr };
    void ( *plistFree )( void* ){ nullptr };
    void ( *plistMemFree )( void* ){ nullptr };
    std::int32_t ( *readPairRecord )( const char*, char**, std::uint32_t* ){ nullptr };
    std::int32_t ( *osTraceStart )( NativeOsTraceClient, void*, NativeOsTraceActivityCallback,
                                    NativeOsTraceErrorCallback, void* ){ nullptr };
    std::int32_t ( *syslogRelayStart )( NativeSyslogRelayClient, NativeSyslogRelayCallback,
                                        NativeSyslogRelayErrorCallback, void* ){ nullptr };
};

NativeSymbols symbols;
std::vector<void*> moduleHandles;
std::string loadedRoot;
std::mutex loadMutex;

std::int32_t deviceListExtendedFree( NativeDeviceInfo** devices )
{
    return symbols.deviceListExtendedFree( devices );
}

std::int32_t deviceNewMapped( NativeIdevice* device, const char* udid,
                              NativeConnectionOption option )
{
    // Pinned libimobiledevice 1.4.0: IDEVICE_LOOKUP_USBMUX=1<<1,
    // IDEVICE_LOOKUP_NETWORK=1<<2. Keep vendor values behind this adapter.
    const auto nativeOption = option == NativeConnectionOption::Usb ? 2 : 4;
    return symbols.deviceNewWithOptions( device, udid, nativeOption );
}

std::int32_t lockdownGetStringValue( NativeLockdownClient client, const char* domain,
                                     const char* key, char** value )
{
    if ( value == nullptr ) {
        return -1;
    }
    *value = nullptr;
    void* plist = nullptr;
    const auto result = symbols.lockdownGetValue( client, domain, key, &plist );
    if ( result != 0 ) {
        if ( plist != nullptr ) {
            symbols.plistFree( plist );
        }
        return result;
    }
    if ( plist == nullptr ) {
        return -1;
    }
    symbols.plistGetStringValue( plist, value );
    symbols.plistFree( plist );
    return *value == nullptr ? -1 : 0;
}

void freeNativeString( char* value )
{
    symbols.plistMemFree( value );
}

NativePairRecordResult readPairRecord( const char* udid, char** record, std::uint32_t* recordSize )
{
    if ( udid == nullptr || record == nullptr || recordSize == nullptr
         || symbols.readPairRecord == nullptr ) {
        return NativePairRecordResult::Error;
    }
    *record = nullptr;
    *recordSize = 0u;
    const auto result = symbols.readPairRecord( udid, record, recordSize );
    if ( result == 0 ) {
        return *record != nullptr && *recordSize != 0u ? NativePairRecordResult::Present
                                                       : NativePairRecordResult::Error;
    }
    // libusbmuxd reports a missing pair record as -ENOENT on the supported
    // macOS stack. Other failures remain distinguishable from absence.
    return result == -2 ? NativePairRecordResult::Missing : NativePairRecordResult::Error;
}

// Releases the vendor-owned pair-record buffer handed out by the patched
// helper through the C ABI; the allocation ownership lives in libusbmuxd.
void freePairRecord( char* record )
{
    std::free( record ); // NOLINT(cppcoreguidelines-no-malloc, cppcoreguidelines-owning-memory)
}

std::int32_t osTraceStart( NativeOsTraceClient client, NativeOsTraceActivityCallback callback,
                           NativeOsTraceErrorCallback errorCallback, void* context )
{
    return symbols.osTraceStart( client, nullptr, callback, errorCallback, context );
}

std::int32_t syslogRelayStart( NativeSyslogRelayClient client, NativeSyslogRelayCallback callback,
                               NativeSyslogRelayErrorCallback errorCallback, void* context )
{
    return symbols.syslogRelayStart( client, callback, errorCallback, context );
}

template <typename Function>
Function requiredSymbol( void* handle, const char* name )
{
    dlerror();
    auto* value = dlsym( handle, name );
    return reinterpret_cast<Function>( value );
}

IosNativeApi resolvedApi( void* mobile )
{
    IosNativeApi api{};
    api.getDeviceListExtended = requiredSymbol<decltype( api.getDeviceListExtended )>(
        mobile, "idevice_get_device_list_extended" );
    api.deviceListExtendedFree = &deviceListExtendedFree;
    api.eventSubscribe
        = requiredSymbol<decltype( api.eventSubscribe )>( mobile, "idevice_events_subscribe" );
    api.eventUnsubscribe
        = requiredSymbol<decltype( api.eventUnsubscribe )>( mobile, "idevice_events_unsubscribe" );
    api.deviceNewWithOptions = &deviceNewMapped;
    api.deviceFree = requiredSymbol<decltype( api.deviceFree )>( mobile, "idevice_free" );
    api.lockdownClientNew
        = requiredSymbol<decltype( api.lockdownClientNew )>( mobile, "lockdownd_client_new" );
    api.lockdownClientFree
        = requiredSymbol<decltype( api.lockdownClientFree )>( mobile, "lockdownd_client_free" );
    api.lockdownStartService
        = requiredSymbol<decltype( api.lockdownStartService )>( mobile, "lockdownd_start_service" );
    api.serviceDescriptorFree = requiredSymbol<decltype( api.serviceDescriptorFree )>(
        mobile, "lockdownd_service_descriptor_free" );
    api.lockdownGetStringValue = &lockdownGetStringValue;
    api.nativeStringFree = &freeNativeString;
    api.readPairRecord = &readPairRecord;
    api.pairRecordFree = &freePairRecord;
    api.lockdownClientNewWithExistingPair
        = requiredSymbol<decltype( api.lockdownClientNewWithExistingPair )>(
            mobile, "lockdownd_client_new_with_existing_pair" );
    api.osTraceClientNew
        = requiredSymbol<decltype( api.osTraceClientNew )>( mobile, "ostrace_client_new" );
    api.osTraceStart = &osTraceStart;
    api.osTraceStop
        = requiredSymbol<decltype( api.osTraceStop )>( mobile, "ostrace_stop_activity" );
    api.osTraceClientFree
        = requiredSymbol<decltype( api.osTraceClientFree )>( mobile, "ostrace_client_free" );
    api.syslogRelayClientNew
        = requiredSymbol<decltype( api.syslogRelayClientNew )>( mobile, "syslog_relay_client_new" );
    api.syslogRelayStart = &syslogRelayStart;
    api.syslogRelayStop
        = requiredSymbol<decltype( api.syslogRelayStop )>( mobile, "syslog_relay_stop_capture" );
    api.syslogRelayClientFree = requiredSymbol<decltype( api.syslogRelayClientFree )>(
        mobile, "syslog_relay_client_free" );
    return api;
}

std::string canonicalStackRoot( const std::string& root, std::string* error )
{
    try {
        const std::filesystem::path requested( root );
        if ( root.empty() || !requested.is_absolute() ) {
            if ( error != nullptr ) {
                *error = "bundled iOS native stack root must be an absolute path";
            }
            return {};
        }
        const auto canonical = std::filesystem::canonical( requested );
        if ( !std::filesystem::is_directory( canonical / "lib" ) ) {
            if ( error != nullptr ) {
                *error = "bundled iOS native stack root has no lib directory";
            }
            return {};
        }
        return canonical.string();
    } catch ( const std::filesystem::filesystem_error& exception ) {
        if ( error != nullptr ) {
            *error = exception.what();
        }
        return {};
    }
}

std::string joinPath( const std::string& root, const char* name )
{
    return ( std::filesystem::path( root ) / "lib" / name ).string();
}

void closeModules( std::vector<void*>& handles ) noexcept
{
    for ( auto iterator = handles.rbegin(); iterator != handles.rend(); ++iterator ) {
        dlclose( *iterator );
    }
    handles.clear();
}
#endif

} // namespace

IosNativeApi loadIosNativeApiFromBundle( const std::string& stackRoot, std::string* error )
{
#ifndef __APPLE__
    static_cast<void>( stackRoot );
    if ( error != nullptr ) {
        *error = "the native iOS ABI adapter is available only on macOS";
    }
    return {};
#else
    const auto canonicalRoot = canonicalStackRoot( stackRoot, error );
    if ( canonicalRoot.empty() ) {
        return {};
    }

    std::lock_guard<std::mutex> lock( loadMutex );
    if ( !moduleHandles.empty() ) {
        if ( canonicalRoot != loadedRoot ) {
            if ( error != nullptr ) {
                *error = "the native iOS ABI is already bound to a different bundle root";
            }
            return {};
        }
        if ( error != nullptr ) {
            error->clear();
        }
    }
    else {
        const char* libraries[]{ "libcrypto.3.dylib",    "libssl.3.dylib",
                                 "libcurl.4.dylib",      "libplist-2.0.dylib",
                                 "libtatsu.0.dylib",     "libimobiledevice-glue-1.0.dylib",
                                 "libusbmuxd-2.0.dylib", "libimobiledevice-1.0.dylib" };
        std::vector<void*> candidateHandles;
        const auto canonicalLib = std::filesystem::path( canonicalRoot ) / "lib";
        for ( const auto* library : libraries ) {
            const auto path = joinPath( canonicalRoot, library );
            try {
                const auto resolved = std::filesystem::canonical( path );
                if ( !std::filesystem::is_regular_file( resolved )
                     || resolved.parent_path() != canonicalLib ) {
                    if ( error != nullptr ) {
                        *error = "bundled iOS native dylib escapes the verified closure";
                    }
                    closeModules( candidateHandles );
                    return {};
                }
            } catch ( const std::filesystem::filesystem_error& exception ) {
                if ( error != nullptr ) {
                    *error = exception.what();
                }
                closeModules( candidateHandles );
                return {};
            }
            void* handle = dlopen( path.c_str(), RTLD_NOW | RTLD_LOCAL );
            if ( handle == nullptr ) {
                if ( error != nullptr ) {
                    const auto* detail = dlerror();
                    *error = detail != nullptr ? detail : "failed to load bundled iOS native dylib";
                }
                closeModules( candidateHandles );
                return {};
            }
            candidateHandles.push_back( handle );
        }

        auto* mobile = candidateHandles.back();
        auto* plist = candidateHandles[ 3 ];
        NativeSymbols candidateSymbols;
        candidateSymbols.deviceListExtendedFree
            = requiredSymbol<decltype( candidateSymbols.deviceListExtendedFree )>(
                mobile, "idevice_device_list_extended_free" );
        candidateSymbols.deviceNewWithOptions
            = requiredSymbol<decltype( candidateSymbols.deviceNewWithOptions )>(
                mobile, "idevice_new_with_options" );
        candidateSymbols.lockdownGetValue
            = requiredSymbol<decltype( candidateSymbols.lockdownGetValue )>(
                mobile, "lockdownd_get_value" );
        candidateSymbols.plistGetStringValue
            = requiredSymbol<decltype( candidateSymbols.plistGetStringValue )>(
                plist, "plist_get_string_val" );
        candidateSymbols.plistFree
            = requiredSymbol<decltype( candidateSymbols.plistFree )>( plist, "plist_free" );
        candidateSymbols.plistMemFree
            = requiredSymbol<decltype( candidateSymbols.plistMemFree )>( plist, "plist_mem_free" );
        auto* usbmux = candidateHandles[ 6 ];
        candidateSymbols.readPairRecord
            = requiredSymbol<decltype( candidateSymbols.readPairRecord )>(
                usbmux, "usbmuxd_read_pair_record" );
        candidateSymbols.osTraceStart = requiredSymbol<decltype( candidateSymbols.osTraceStart )>(
            mobile, "ostrace_start_activity_with_error" );
        candidateSymbols.syslogRelayStart
            = requiredSymbol<decltype( candidateSymbols.syslogRelayStart )>(
                mobile, "syslog_relay_start_capture_raw_with_error" );

        const auto candidateApi = resolvedApi( mobile );

        const bool complete
            = candidateApi.getDeviceListExtended != nullptr
              && candidateSymbols.deviceListExtendedFree != nullptr
              && candidateApi.eventSubscribe != nullptr && candidateApi.eventUnsubscribe != nullptr
              && candidateSymbols.deviceNewWithOptions != nullptr
              && candidateApi.deviceFree != nullptr && candidateApi.lockdownClientNew != nullptr
              && candidateApi.lockdownClientFree != nullptr
              && candidateApi.lockdownStartService != nullptr
              && candidateApi.serviceDescriptorFree != nullptr
              && candidateSymbols.lockdownGetValue != nullptr
              && candidateSymbols.plistGetStringValue != nullptr
              && candidateSymbols.plistFree != nullptr && candidateSymbols.plistMemFree != nullptr
              && candidateSymbols.readPairRecord != nullptr
              && candidateApi.lockdownClientNewWithExistingPair != nullptr
              && candidateApi.osTraceClientNew != nullptr
              && candidateSymbols.osTraceStart != nullptr && candidateApi.osTraceStop != nullptr
              && candidateApi.osTraceClientFree != nullptr
              && candidateApi.syslogRelayClientNew != nullptr
              && candidateSymbols.syslogRelayStart != nullptr
              && candidateApi.syslogRelayStop != nullptr
              && candidateApi.syslogRelayClientFree != nullptr;
        if ( !complete ) {
            closeModules( candidateHandles );
            if ( error != nullptr ) {
                *error = "bundled iOS native dylib closure is missing a pinned C ABI symbol";
            }
            return {};
        }

        symbols = candidateSymbols;
        moduleHandles = std::move( candidateHandles );
        loadedRoot = canonicalRoot;
    }

    auto* mobile = moduleHandles.back();
    const auto api = resolvedApi( mobile );
    if ( error != nullptr ) {
        error->clear();
    }
    return api;
#endif
}

} // namespace klogg::livecapture::ios
