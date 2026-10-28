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
#include <cstring>
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
    SharedEnvelope* diagnosticView{};
    DiagnosticData* diagnostics{};
    bool diagnosticRequested{};
};

void observeDiagnosticModules(
    std::array<DiagnosticModuleRecord, diagnosticModulesPerProcess>& modules ) noexcept
{
    constexpr const wchar_t* asanName = L"clang_rt.asan_dynamic-x86_64.dll";
    const std::array<const wchar_t*, diagnosticModulesPerProcess> names{
        L"dbghelp.dll", L"dbgcore.dll", asanName };
    for ( std::size_t index = 0; index < modules.size(); ++index ) {
        auto& record = modules[ index ];
        record = {};
        record.process = static_cast<std::uint32_t>( DiagnosticProcess::Collector );
        record.name = static_cast<std::uint32_t>( index + 1 );
        // Basename-selected loaded image, not proof of the executing API's owner.
        const auto module = GetModuleHandleW( names[ index ] );
        if ( module == nullptr ) {
            record.status = static_cast<std::uint32_t>( DiagnosticStatus::Absent );
            record.error = static_cast<std::uint32_t>( GetLastError() );
            continue;
        }
        std::array<wchar_t, maximumDiagnosticPath> path{};
        const auto length = GetModuleFileNameW( module, path.data(),
                                               static_cast<DWORD>( path.size() ) );
        if ( length == 0 ) {
            record.status = static_cast<std::uint32_t>( DiagnosticStatus::Error );
            record.error = static_cast<std::uint32_t>( GetLastError() );
            continue;
        }
        record.status = static_cast<std::uint32_t>( length >= path.size()
                                                       ? DiagnosticStatus::Truncated
                                                       : DiagnosticStatus::Loaded );
        while ( record.pathLength < path.size() && path[ record.pathLength ] != L'\0' ) {
            ++record.pathLength;
        }
        for ( std::size_t offset = 0; offset < record.pathLength; ++offset ) {
            record.path[ offset ] = static_cast<std::uint16_t>( path[ offset ] );
        }
    }
}

bool queryDiagnosticRegion( Capabilities& capabilities, std::uint64_t address,
                            DiagnosticObject object, MEMORY_BASIC_INFORMATION& region ) noexcept
{
    auto& data = *capabilities.diagnostics;
    DiagnosticRecord record{};
    record.kind = static_cast<std::uint32_t>( DiagnosticKind::Region );
    record.object = static_cast<std::uint32_t>( object );
    record.address = address;
    if ( !validDiagnosticRange( address, 1, nativePointerBits ) ) {
        record.status = static_cast<std::uint32_t>( DiagnosticStatus::InvalidRange );
        data.result.flags |= diagnosticUnavailable;
        static_cast<void>( appendDiagnosticRecord( data, record ) );
        return false;
    }
    if ( !reserveDiagnosticQuery( data.result ) ) {
        record.status = static_cast<std::uint32_t>( DiagnosticStatus::Budget );
        static_cast<void>( appendDiagnosticRecord( data, record ) );
        return false;
    }
    const auto queried = VirtualQueryEx(
        capabilities.parent, reinterpret_cast<const void*>( static_cast<std::uintptr_t>( address ) ),
        &region, sizeof( region ) );
    const auto error = queried != 0 ? ERROR_SUCCESS : GetLastError();
    record.requested = sizeof( region );
    record.transferred = static_cast<std::uint64_t>( queried );
    record.error = static_cast<std::uint32_t>( error );
    record.status = static_cast<std::uint32_t>( queried == sizeof( region )
                                                   ? DiagnosticStatus::Complete
                                                   : ( queried != 0 ? DiagnosticStatus::Partial
                                                                    : DiagnosticStatus::Error ) );
    if ( queried == sizeof( region ) ) {
        record.regionBase = static_cast<std::uint64_t>( reinterpret_cast<std::uintptr_t>( region.BaseAddress ) );
        record.regionBytes = static_cast<std::uint64_t>( region.RegionSize );
        record.state = static_cast<std::uint32_t>( region.State );
        record.protection = static_cast<std::uint32_t>( region.Protect );
        record.type = static_cast<std::uint32_t>( region.Type );
    }
    static_cast<void>( appendDiagnosticRecord( data, record ) );
    if ( queried != sizeof( region ) ) {
        data.result.flags |= diagnosticUnavailable;
        return false;
    }
    return true;
}

bool readDiagnosticObject( Capabilities& capabilities, std::uint64_t address, void* scratch,
                           std::size_t bytes, DiagnosticObject object ) noexcept
{
    auto& data = *capabilities.diagnostics;
    if ( !validDiagnosticRange( address, static_cast<std::uint64_t>( bytes ), nativePointerBits ) ) {
        DiagnosticRecord record{};
        record.kind = static_cast<std::uint32_t>( DiagnosticKind::Read );
        record.object = static_cast<std::uint32_t>( object );
        record.address = address;
        record.requested = static_cast<std::uint64_t>( bytes );
        record.status = static_cast<std::uint32_t>( DiagnosticStatus::InvalidRange );
        data.result.flags |= diagnosticUnavailable;
        static_cast<void>( appendDiagnosticRecord( data, record ) );
        return false;
    }
    std::size_t completed = 0;
    while ( completed < bytes ) {
        const auto current = address + static_cast<std::uint64_t>( completed );
        MEMORY_BASIC_INFORMATION region{};
        if ( !queryDiagnosticRegion( capabilities, current, object, region ) ) {
            return false;
        }
        const auto base = static_cast<std::uint64_t>( reinterpret_cast<std::uintptr_t>( region.BaseAddress ) );
        const auto regionBytes = static_cast<std::uint64_t>( region.RegionSize );
        DiagnosticRecord record{};
        record.kind = static_cast<std::uint32_t>( DiagnosticKind::Read );
        record.object = static_cast<std::uint32_t>( object );
        record.address = current;
        record.regionBase = base;
        record.regionBytes = regionBytes;
        record.state = static_cast<std::uint32_t>( region.State );
        record.protection = static_cast<std::uint32_t>( region.Protect );
        record.type = static_cast<std::uint32_t>( region.Type );
        if ( !validDiagnosticRange( base, regionBytes, nativePointerBits ) || current < base
             || current - base >= regionBytes ) {
            record.status = static_cast<std::uint32_t>( DiagnosticStatus::InvalidRange );
            data.result.flags |= diagnosticUnavailable;
            static_cast<void>( appendDiagnosticRecord( data, record ) );
            return false;
        }
        const auto remaining = static_cast<std::uint64_t>( bytes - completed );
        const auto available = regionBytes - ( current - base );
        const auto segment = remaining < available ? remaining : available;
        record.requested = segment;
        if ( !readableDiagnosticRegion( record.state, record.protection ) ) {
            record.status = static_cast<std::uint32_t>( DiagnosticStatus::Skipped );
            data.result.flags |= diagnosticUnavailable;
            static_cast<void>( appendDiagnosticRecord( data, record ) );
            return false;
        }
        if ( segment > maximumDiagnosticReadBytes
             || !reserveDiagnosticRead( data.result, static_cast<std::uint32_t>( segment ) ) ) {
            record.status = static_cast<std::uint32_t>( DiagnosticStatus::Budget );
            data.result.flags |= diagnosticTruncated;
            static_cast<void>( appendDiagnosticRecord( data, record ) );
            return false;
        }
        SIZE_T transferred = 0;
        const auto read = ReadProcessMemory(
            capabilities.parent, reinterpret_cast<const void*>( static_cast<std::uintptr_t>( current ) ),
            static_cast<std::uint8_t*>( scratch ) + completed, static_cast<SIZE_T>( segment ),
            &transferred );
        const auto error = read != FALSE ? ERROR_SUCCESS : GetLastError();
        record.transferred = static_cast<std::uint64_t>( transferred );
        record.error = static_cast<std::uint32_t>( error );
        record.status = static_cast<std::uint32_t>(
            read != FALSE && record.transferred == segment
                ? DiagnosticStatus::Complete
                : ( transferred != 0 ? DiagnosticStatus::Partial : DiagnosticStatus::Error ) );
        static_cast<void>( appendDiagnosticRecord( data, record ) );
        if ( read == FALSE || record.transferred != segment ) {
            data.result.flags |= diagnosticUnavailable;
            return false;
        }
        completed += static_cast<std::size_t>( segment );
    }
    return true;
}

class NativeDiagnosticProbes {
public:
    explicit NativeDiagnosticProbes( Capabilities& capabilities ) noexcept
        : capabilities_( capabilities )
    {
    }

    bool readPointers( std::uint64_t address, std::uint64_t& exceptionRecord,
                       std::uint64_t& contextRecord ) noexcept
    {
        EXCEPTION_POINTERS observedPointers{};
        if ( !readDiagnosticObject( capabilities_, address, &observedPointers,
                                    sizeof( observedPointers ), DiagnosticObject::Pointers ) ) {
            return false;
        }
        exceptionRecord = static_cast<std::uint64_t>(
            reinterpret_cast<std::uintptr_t>( observedPointers.ExceptionRecord ) );
        contextRecord = static_cast<std::uint64_t>(
            reinterpret_cast<std::uintptr_t>( observedPointers.ContextRecord ) );
        return true;
    }

    bool readException( std::uint64_t address, std::uint64_t& next ) noexcept
    {
        EXCEPTION_RECORD observedException{};
        if ( !readDiagnosticObject( capabilities_, address, &observedException,
                                    sizeof( observedException ), DiagnosticObject::Exception ) ) {
            return false;
        }
        next = static_cast<std::uint64_t>(
            reinterpret_cast<std::uintptr_t>( observedException.ExceptionRecord ) );
        return true;
    }

    bool readContext( std::uint64_t address ) noexcept
    {
        // sizeof(CONTEXT) is only the documented native base. No XSTATE extent is inferred.
        CONTEXT observedContext{};
        if ( !readDiagnosticObject( capabilities_, address, &observedContext,
                                    sizeof( observedContext ), DiagnosticObject::Context ) ) {
            return false;
        }
        auto& result = capabilities_.diagnostics->result;
        result.contextFlags = static_cast<std::uint32_t>( observedContext.ContextFlags );
        result.pc = observedContext.Rip;
        result.sp = observedContext.Rsp;
        result.fp = observedContext.Rbp;
        return true;
    }

    void readStack( std::uint64_t address ) noexcept
    {
        auto& data = *capabilities_.diagnostics;
        const auto plan = diagnosticStackPlan( data.input, address, data.result.requestedBytes );
        if ( !plan.boundsKnown ) {
            data.result.flags |= diagnosticUnavailable;
        }
        const auto wanted = plan.boundsKnown ? plan.desiredBytes : maximumStackSample;
        if ( plan.readBytes < wanted ) {
            data.result.flags |= diagnosticTruncated;
        }
        // Cover the active fault-thread stack only within the existing total read budget.
        // This says nothing about other threads or the original CONTEXT's XSTATE extent.
        std::array<std::uint8_t, maximumDiagnosticReadBytes> stackScratch{};
        if ( plan.readBytes != 0 ) {
            static_cast<void>( readDiagnosticObject( capabilities_, address, stackScratch.data(),
                                                     plan.readBytes, DiagnosticObject::Stack ) );
        }
    }

private:
    Capabilities& capabilities_;
};

void collectFailureDiagnostics( Capabilities& capabilities ) noexcept
{
    auto& data = *capabilities.diagnostics;
    NativeDiagnosticProbes probes{ capabilities };
    probeDiagnosticObjects( data, probes );
    observeDiagnosticModules( data.collectorModules );
}

void closeHandle( HANDLE& handle ) noexcept
{
    if ( handle != nullptr && handle != INVALID_HANDLE_VALUE ) {
        CloseHandle( handle );
    }
    handle = nullptr;
}

[[noreturn]] void exitCollector( Capabilities& capabilities, DWORD code ) noexcept
{
    if ( capabilities.diagnosticView != nullptr ) {
        UnmapViewOfFile( capabilities.diagnosticView );
        capabilities.diagnosticView = nullptr;
        capabilities.diagnostics = nullptr;
    }
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
    const auto closed = CloseHandle( capabilities.output ) != FALSE;
    const auto closeError = closed ? ERROR_SUCCESS : GetLastError();
    const auto finalized = includeOutputCloseFailure(
        { succeeded ? Stage::None : Stage::Dump, domain, static_cast<std::uint32_t>( error ), false,
          succeeded },
        closed, static_cast<std::uint32_t>( closeError ) );
    succeeded = finalized.succeeded;
    domain = finalized.domain;
    error = static_cast<DWORD>( finalized.error );
    capabilities.output = nullptr;
    if ( capabilities.diagnosticRequested ) {
        capabilities.diagnostics->result.closeSucceeded = closed ? 1U : 0U;
        capabilities.diagnostics->result.closeError = static_cast<std::uint32_t>( closeError );
        // All remote reads and diagnostic writes precede functional completion.
        InterlockedExchange( reinterpret_cast<volatile LONG*>( &capabilities.diagnostics->resultPublication ),
                             static_cast<LONG>( captureSequence ) );
    }
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
    // A successful full-length view, not page rounding or a claimed header size,
    // proves the optional trailer is addressable. Failure leaves v1 operation intact.
    capabilities.diagnosticView = static_cast<SharedEnvelope*>( MapViewOfFile(
        capabilities.mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof( SharedEnvelope ) ) );
    if ( capabilities.diagnosticView != nullptr
         && validDiagnosticHeader( capabilities.diagnosticView->diagnostics.data.header,
                                   nativePointerBits ) ) {
        capabilities.diagnostics = &capabilities.diagnosticView->diagnostics.data;
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
    const Outcome primary{ captured != FALSE ? Stage::None : Stage::Dump,
                           captured != FALSE ? ErrorDomain::None : ErrorDomain::DumpHResult,
                           static_cast<std::uint32_t>( error ), false, captured != FALSE };
    const auto inputPublished
        = capabilities.diagnostics != nullptr
              ? InterlockedCompareExchange(
                    reinterpret_cast<volatile LONG*>(
                        &capabilities.diagnostics->inputPublication ),
                    0, 0 )
              : 0;
    // Count/byte bounds do not bound syscall latency. The unchanged ACK deadline
    // may expire before this result is published; no sideband substitutes for an ACK.
    const auto observation = observeDumpOutcome(
        capabilities.diagnostics, request, nativePointerBits,
        inputPublished == static_cast<LONG>( captureSequence ), primary,
        [&capabilities]( DiagnosticData& ) noexcept { collectFailureDiagnostics( capabilities ); } );
    capabilities.diagnosticRequested = observation.requested;
    finishRequest( capabilities, Phase::Complete, observation.primary.succeeded,
                   observation.primary.domain, static_cast<DWORD>( observation.primary.error ) );
    return 0;
}

#endif // _WIN32
