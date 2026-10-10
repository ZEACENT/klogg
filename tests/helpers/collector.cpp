/*
 * Copyright (C) 2026 ZEACENT and other contributors
 *
 * This file is part of klogg.
 *
 * klogg is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * klogg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with klogg.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <dbghelp.h>

#include "crash_collector_protocol.h"

#include <array>
#include <limits>
#include <string_view>

namespace {
using namespace klogg::testing::collector;
constexpr std::uint32_t nativePointerBits = static_cast<std::uint32_t>( sizeof( void* ) * 8 );

struct Capabilities {
    HANDLE parent{};
    HANDLE output{};
    HANDLE mapping{};
    HANDLE requestEvent{};
    HANDLE acknowledgementEvent{};
    SharedPage* page{};
};

void closeHandle( HANDLE& handle ) noexcept
{
    if ( handle != nullptr && handle != INVALID_HANDLE_VALUE ) {
        CloseHandle( handle );
    }
    handle = nullptr;
}

[[noreturn]] void exitCollector( Capabilities& capabilities, DWORD code ) noexcept
{
    if ( capabilities.page != nullptr ) {
        UnmapViewOfFile( capabilities.page );
        capabilities.page = nullptr;
    }
    closeHandle( capabilities.output );
    closeHandle( capabilities.mapping );
    closeHandle( capabilities.requestEvent );
    closeHandle( capabilities.acknowledgementEvent );
    closeHandle( capabilities.parent );
    ExitProcess( code );
}

bool publishAcknowledgement( Capabilities& capabilities, Phase phase, std::uint32_t sequence,
                             bool succeeded, ErrorDomain domain, DWORD error ) noexcept
{
    capabilities.page->acknowledgement = { static_cast<std::uint32_t>( phase ),
                                           sequence,
                                           succeeded ? 1U : 0U,
                                           static_cast<std::uint32_t>( domain ),
                                           static_cast<std::uint32_t>( error ),
                                           0 };
    InterlockedExchange(
        reinterpret_cast<volatile LONG*>( &capabilities.page->acknowledgementPublication ),
        static_cast<LONG>( phase ) );
    return SetEvent( capabilities.acknowledgementEvent ) != FALSE;
}

[[noreturn]] void finishRequest( Capabilities& capabilities, Phase phase, bool succeeded,
                                 ErrorDomain domain, DWORD error ) noexcept
{
    // No output writes or remote reads occur after completion publication.
    if ( CloseHandle( capabilities.output ) == FALSE ) {
        error = GetLastError();
        domain = ErrorDomain::Win32;
        succeeded = false;
    }
    capabilities.output = nullptr;
    if ( !publishAcknowledgement( capabilities, phase, captureSequence, succeeded, domain,
                                  error ) ) {
        exitCollector( capabilities, GetLastError() );
    }
    exitCollector( capabilities,
                   succeeded ? ERROR_SUCCESS : ( error != 0 ? error : ERROR_GEN_FAILURE ) );
}

bool parseCapabilities( int argc, wchar_t* argv[], Capabilities& capabilities ) noexcept
{
    // Arguments are only inherited handle tokens: parent, output, mapping, request, ack.
    // There is no PID, path, endpoint, dump-type argument, or arbitrary-target service.
    if ( argc != 6 ) {
        return false;
    }
    std::array<HANDLE, 5> handles{};
    for ( std::size_t index = 0; index < handles.size(); ++index ) {
        std::uint64_t value = 0;
        if ( !parseHandleValue(
                 std::wstring_view{ argv[ index + 1 ] },
                 static_cast<std::uint64_t>( ( std::numeric_limits<std::uintptr_t>::max )() ),
                 value ) ) {
            return false;
        }
        handles[ index ] = reinterpret_cast<HANDLE>( static_cast<std::uintptr_t>( value ) );
        DWORD flags = 0;
        if ( handles[ index ] == INVALID_HANDLE_VALUE
             || GetHandleInformation( handles[ index ], &flags ) == FALSE ) {
            return false;
        }
    }
    capabilities.parent = handles[ 0 ];
    capabilities.output = handles[ 1 ];
    capabilities.mapping = handles[ 2 ];
    capabilities.requestEvent = handles[ 3 ];
    capabilities.acknowledgementEvent = handles[ 4 ];
    for ( const auto handle : handles ) {
        if ( SetHandleInformation( handle, HANDLE_FLAG_INHERIT, 0 ) == FALSE ) {
            return false;
        }
    }
    return true;
}
} // namespace

int wmain( int argc, wchar_t* argv[] )
{
    SetErrorMode( SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX );
    Capabilities capabilities;
    if ( !parseCapabilities( argc, argv, capabilities ) ) {
        exitCollector( capabilities, ERROR_INVALID_PARAMETER );
    }
    capabilities.page = static_cast<SharedPage*>( MapViewOfFile(
        capabilities.mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof( SharedPage ) ) );
    if ( capabilities.page == nullptr ) {
        exitCollector( capabilities, GetLastError() );
    }
    if ( !validHeader( capabilities.page->header, nativePointerBits ) ) {
        static_cast<void>( publishAcknowledgement( capabilities, Phase::Ready, 0, false,
                                                   ErrorDomain::Protocol, invalidProtocol ) );
        exitCollector( capabilities, ERROR_BAD_FORMAT );
    }
    const auto targetPid = GetProcessId( capabilities.parent );
    if ( targetPid == 0 || targetPid == GetCurrentProcessId() ) {
        const auto error = targetPid == 0 ? GetLastError() : ERROR_INVALID_PARAMETER;
        static_cast<void>( publishAcknowledgement( capabilities, Phase::Ready, 0, false,
                                                   ErrorDomain::Win32, error ) );
        exitCollector( capabilities, error );
    }
    if ( !publishAcknowledgement( capabilities, Phase::Ready, 0, true, ErrorDomain::None,
                                  ERROR_SUCCESS ) ) {
        exitCollector( capabilities, GetLastError() );
    }

    const HANDLE waits[]{ capabilities.parent, capabilities.requestEvent };
    const auto wait = WaitForMultipleObjects( 2, waits, FALSE, INFINITE );
    if ( wait == WAIT_OBJECT_0 ) {
        // Parent death is also an idle shutdown signal. We own no path or DELETE capability.
        exitCollector( capabilities, ERROR_PROCESS_ABORTED );
    }
    if ( wait != WAIT_OBJECT_0 + 1 ) {
        exitCollector( capabilities,
                       wait == WAIT_FAILED ? GetLastError() : ERROR_INVALID_FUNCTION );
    }
    const auto publication = InterlockedCompareExchange(
        reinterpret_cast<volatile LONG*>( &capabilities.page->requestPublication ), 0, 0 );
    const auto request = capabilities.page->request;
    if ( publication != static_cast<LONG>( captureSequence )
         || !validRequest( capabilities.page->header, request, nativePointerBits ) ) {
        finishRequest( capabilities, Phase::Complete, false, ErrorDomain::Protocol,
                       invalidCaptureRequest );
    }
    if ( request.operation == static_cast<std::uint32_t>( Operation::Stop ) ) {
        finishRequest( capabilities, Phase::Stopped, true, ErrorDomain::None, ERROR_SUCCESS );
    }

    // This main thread is the collector's sole DbgHelp caller. The parent's VEH
    // remains blocked until this process is signaled, keeping the original remote
    // EXCEPTION_POINTERS, EXCEPTION_RECORD chain, CONTEXT and XSTATE storage alive.
    MINIDUMP_EXCEPTION_INFORMATION exception{};
    exception.ThreadId = static_cast<DWORD>( request.threadId );
    exception.ExceptionPointers = reinterpret_cast<EXCEPTION_POINTERS*>(
        static_cast<std::uintptr_t>( request.exceptionPointers ) );
    exception.ClientPointers = TRUE;
    const auto captured = MiniDumpWriteDump( capabilities.parent, targetPid, capabilities.output,
                                             MiniDumpNormal, &exception, nullptr, nullptr );
    const auto error = captured != FALSE ? ERROR_SUCCESS : GetLastError();
    finishRequest( capabilities, Phase::Complete, captured != FALSE,
                   captured != FALSE ? ErrorDomain::None : ErrorDomain::DumpHResult, error );
    return 0;
}

#endif // _WIN32
