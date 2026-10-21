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

#include "crash_trace.h"

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <dbghelp.h>

#include "crash_collector_protocol.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <new>

namespace {
using namespace klogg::testing::collector;

constexpr std::uint32_t nativePointerBits = static_cast<std::uint32_t>( sizeof( void* ) * 8 );
constexpr std::uint32_t startupTimeoutMs = 5000;
constexpr std::uint32_t captureTimeoutMs = 10000;
constexpr std::uint32_t stopTimeoutMs = 5000;
constexpr std::uint32_t exitGraceMs = 1000;
constexpr UINT failStopExitCode = 0xe0000042U;

struct Channel {
    HANDLE output{};
    HANDLE mapping{};
    HANDLE requestEvent{};
    HANDLE acknowledgementEvent{};
    HANDLE process{};
    SharedPage* page{};
    SharedEnvelope* diagnosticView{};
    DiagnosticData* diagnostics{};
};

Channel channel;
ClientState clientState;
LONG installationStarted = 0;
LONG closing = 0;
LONG crashTraceActive = 0;
LONG dumpCaptureStarted = 0;
SRWLOCK lifetimeLock = SRWLOCK_INIT;
PVOID vectoredHandler{};
using StackLimitsFunction = void ( WINAPI* )( PULONG_PTR, PULONG_PTR );
StackLimitsFunction currentThreadStackLimits{};
std::array<DiagnosticModuleRecord, diagnosticModulesPerProcess> targetModuleObservations{};
// crashTraceActive serializes rendering; this does not consume the faulting stack.
std::array<char, 4096> diagnosticLine{};

void prepareDiagnosticIdentity() noexcept
{
    const auto kernel = GetModuleHandleW( L"kernel32.dll" );
    if ( kernel != nullptr ) {
        const auto procedure = GetProcAddress( kernel, "GetCurrentThreadStackLimits" );
        static_assert( sizeof( procedure ) == sizeof( currentThreadStackLimits ) );
        std::memcpy( &currentThreadStackLimits, &procedure, sizeof( procedure ) );
    }
#ifdef _WIN64
    constexpr const wchar_t* asanName = L"clang_rt.asan_dynamic-x86_64.dll";
#else
    constexpr const wchar_t* asanName = L"clang_rt.asan_dynamic-i386.dll";
#endif
    const std::array<const wchar_t*, diagnosticModulesPerProcess> names{
        L"dbghelp.dll", L"dbgcore.dll", asanName };
    for ( std::size_t index = 0; index < names.size(); ++index ) {
        auto& record = targetModuleObservations[ index ];
        record = {};
        record.process = static_cast<std::uint32_t>( DiagnosticProcess::Target );
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

const char* diagnosticStatusName( std::uint32_t status ) noexcept
{
    switch ( static_cast<DiagnosticStatus>( status ) ) {
    case DiagnosticStatus::Loaded: return "loaded";
    case DiagnosticStatus::Absent: return "absent";
    case DiagnosticStatus::Truncated: return "truncated";
    case DiagnosticStatus::Error: return "error";
    case DiagnosticStatus::Complete: return "complete";
    case DiagnosticStatus::Partial: return "partial";
    case DiagnosticStatus::Skipped: return "skipped";
    case DiagnosticStatus::Budget: return "budget";
    case DiagnosticStatus::Cycle: return "cycle";
    case DiagnosticStatus::InvalidRange: return "invalid_range";
    default: return "unavailable";
    }
}

bool appendDiagnosticText( std::size_t& used, std::string_view text ) noexcept
{
    if ( used >= diagnosticLine.size() || text.size() >= diagnosticLine.size() - used ) {
        return false;
    }
    for ( const auto character : text ) {
        diagnosticLine[ used++ ] = character;
    }
    diagnosticLine[ used ] = '\0';
    return true;
}

void reportDiagnosticModule( const DiagnosticModuleRecord& module ) noexcept
{
#ifdef _WIN64
    constexpr const char* asanName = "clang_rt.asan_dynamic-x86_64.dll";
#else
    constexpr const char* asanName = "clang_rt.asan_dynamic-i386.dll";
#endif
    const std::array<const char*, diagnosticModulesPerProcess> names{
        "dbghelp.dll", "dbgcore.dll", asanName };
    const auto target = module.process == static_cast<std::uint32_t>( DiagnosticProcess::Target );
    const auto written = std::snprintf(
        diagnosticLine.data(), diagnosticLine.size(),
        "Test minidump diagnostic {\"kind\":\"module\",\"process\":\"%s\",\"phase\":\"%s\","
        "\"lookup\":\"basename\",\"name\":\"%s\",\"status\":\"%s\",\"error\":%lu,\"path\":\"",
        target ? "target" : "collector", target ? "startup" : "postfailure",
        names[ module.name - 1 ], diagnosticStatusName( module.status ),
        static_cast<unsigned long>( module.error ) );
    bool complete = written >= 0 && static_cast<std::size_t>( written ) < diagnosticLine.size();
    auto used = complete ? static_cast<std::size_t>( written ) : diagnosticLine.size();
    for ( std::size_t index = 0; complete && index < module.pathLength; ++index ) {
        const auto character = module.path[ index ];
        if ( character >= 0x20 && character <= 0x7e && character != '\\' && character != '"' ) {
            const char literal[]{ static_cast<char>( character ) };
            complete = appendDiagnosticText( used, std::string_view{ literal, 1 } );
        }
        else if ( character == '\\' || character == '"' ) {
            const char escaped[]{ '\\', static_cast<char>( character ) };
            complete = appendDiagnosticText( used, std::string_view{ escaped, 2 } );
        }
        else {
            char escaped[ 7 ]{};
            const auto count = std::snprintf( escaped, sizeof( escaped ), "\\u%04x",
                                               static_cast<unsigned int>( character ) );
            complete = count == 6 && appendDiagnosticText( used, std::string_view{ escaped, 6 } );
        }
    }
    if ( complete && appendDiagnosticText( used, "\"}\n" ) ) {
        std::fputs( diagnosticLine.data(), stderr );
    }
    else {
        std::fputs( "Test minidump diagnostic {\"kind\":\"summary\",\"status\":\"render_truncated\"}\n",
                    stderr );
    }
}

void publishDiagnosticInput( EXCEPTION_POINTERS* info ) noexcept
{
    if ( channel.diagnostics == nullptr ) {
        return;
    }
    auto& data = *channel.diagnostics;
    data.input = {};
    data.input.sequence = captureSequence;
    data.input.threadId = static_cast<std::uint32_t>( GetCurrentThreadId() );
    data.input.contextFlags = static_cast<std::uint32_t>( info->ContextRecord->ContextFlags );
    data.input.exceptionPointers = static_cast<std::uint64_t>( reinterpret_cast<std::uintptr_t>( info ) );
    data.input.exceptionRecord = static_cast<std::uint64_t>( reinterpret_cast<std::uintptr_t>( info->ExceptionRecord ) );
    data.input.contextRecord = static_cast<std::uint64_t>( reinterpret_cast<std::uintptr_t>( info->ContextRecord ) );
#ifdef _WIN64
    data.input.pc = info->ContextRecord->Rip;
    data.input.sp = info->ContextRecord->Rsp;
    data.input.fp = info->ContextRecord->Rbp;
#else
    data.input.pc = info->ContextRecord->Eip;
    data.input.sp = info->ContextRecord->Esp;
    data.input.fp = info->ContextRecord->Ebp;
#endif
    if ( currentThreadStackLimits != nullptr ) {
        ULONG_PTR low = 0;
        ULONG_PTR high = 0;
        currentThreadStackLimits( &low, &high );
        if ( low < high ) {
            data.input.flags = stackLimitsAvailable;
            data.input.stackLow = static_cast<std::uint64_t>( low );
            data.input.stackHigh = static_cast<std::uint64_t>( high );
        }
    }
    InterlockedExchange( reinterpret_cast<volatile LONG*>( &data.inputPublication ),
                         static_cast<LONG>( captureSequence ) );
}

void reportCollectorDiagnostics() noexcept
{
    if ( !clientState.captureRequested || !clientState.collectorReaped ) {
        return;
    }
    if ( channel.diagnostics == nullptr
         || !diagnosticReportingAllowed( clientState, *channel.diagnostics, nativePointerBits ) ) {
        std::fputs( "Test minidump diagnostic {\"kind\":\"summary\",\"status\":\"unavailable\"}\n",
                    stderr );
        return;
    }
    const auto& data = *channel.diagnostics;
    const auto& input = data.input;
    std::fprintf(
        stderr,
        "Test minidump diagnostic {\"kind\":\"input\",\"sequence\":%lu,\"thread\":%lu,"
        "\"pointerBits\":%lu,\"exceptionPointers\":%llu,\"exceptionRecord\":%llu,"
        "\"contextRecord\":%llu,\"contextFlags\":%lu,\"pc\":%llu,\"sp\":%llu,\"fp\":%llu,"
        "\"stackLimitsAvailable\":%lu,\"stackLow\":%llu,\"stackHigh\":%llu}\n",
        static_cast<unsigned long>( input.sequence ), static_cast<unsigned long>( input.threadId ),
        static_cast<unsigned long>( nativePointerBits ),
        static_cast<unsigned long long>( input.exceptionPointers ),
        static_cast<unsigned long long>( input.exceptionRecord ),
        static_cast<unsigned long long>( input.contextRecord ),
        static_cast<unsigned long>( input.contextFlags ), static_cast<unsigned long long>( input.pc ),
        static_cast<unsigned long long>( input.sp ), static_cast<unsigned long long>( input.fp ),
        static_cast<unsigned long>( input.flags & stackLimitsAvailable ),
        static_cast<unsigned long long>( input.stackLow ), static_cast<unsigned long long>( input.stackHigh ) );
    const auto stack = summarizeDiagnosticStack( data );
    std::fprintf(
        stderr,
        "Test minidump diagnostic {\"kind\":\"stack-span\",\"boundsKnown\":%u,"
        "\"address\":%llu,\"desiredBytes\":%llu,\"requestedBytes\":%lu,\"transferredBytes\":%lu,"
        "\"recordsValid\":%u,\"complete\":%u,\"diagnosticTruncated\":%u,\"extendedXstate\":\"unknown\"}\n",
        stack.boundsKnown ? 1U : 0U, static_cast<unsigned long long>( stack.address ),
        static_cast<unsigned long long>( stack.desiredBytes ),
        static_cast<unsigned long>( stack.requestedBytes ), static_cast<unsigned long>( stack.transferredBytes ),
        stack.recordsValid ? 1U : 0U, stack.complete ? 1U : 0U,
        ( data.result.flags & diagnosticTruncated ) != 0 ? 1U : 0U );
    for ( std::size_t index = 0; index < data.result.recordCount; ++index ) {
        const auto& record = data.records[ index ];
        std::fprintf(
            stderr,
            "Test minidump diagnostic {\"kind\":\"%s\",\"order\":%lu,\"object\":%lu,\"status\":\"%s\","
            "\"address\":%llu,\"requested\":%llu,\"transferred\":%llu,\"error\":%lu,"
            "\"regionBase\":%llu,\"regionBytes\":%llu,\"state\":%lu,\"protection\":%lu,\"type\":%lu}\n",
            record.kind == static_cast<std::uint32_t>( DiagnosticKind::Read ) ? "read" : "region",
            static_cast<unsigned long>( record.order ), static_cast<unsigned long>( record.object ),
            diagnosticStatusName( record.status ), static_cast<unsigned long long>( record.address ),
            static_cast<unsigned long long>( record.requested ), static_cast<unsigned long long>( record.transferred ),
            static_cast<unsigned long>( record.error ), static_cast<unsigned long long>( record.regionBase ),
            static_cast<unsigned long long>( record.regionBytes ), static_cast<unsigned long>( record.state ),
            static_cast<unsigned long>( record.protection ), static_cast<unsigned long>( record.type ) );
    }
    for ( const auto& module : data.targetModules ) {
        reportDiagnosticModule( module );
    }
    for ( const auto& module : data.collectorModules ) {
        reportDiagnosticModule( module );
    }
    const auto& result = data.result;
    std::fprintf(
        stderr,
        "Test minidump diagnostic {\"kind\":\"summary\",\"sequence\":%lu,\"dumpSucceeded\":%lu,"
        "\"dumpDomain\":%lu,\"dumpError\":%lu,\"closeSucceeded\":%lu,\"closeError\":%lu,"
        "\"readCalls\":%lu,\"queryCalls\":%lu,\"requestedBytes\":%lu,\"flags\":%lu,\"decoded\":%lu,"
        "\"contextFlags\":%lu,\"exceptionRecord\":%llu,\"contextRecord\":%llu,"
        "\"pc\":%llu,\"sp\":%llu,\"fp\":%llu}\n",
        static_cast<unsigned long>( result.sequence ), static_cast<unsigned long>( result.dumpSucceeded ),
        static_cast<unsigned long>( result.dumpDomain ), static_cast<unsigned long>( result.dumpError ),
        static_cast<unsigned long>( result.closeSucceeded ), static_cast<unsigned long>( result.closeError ),
        static_cast<unsigned long>( result.readCalls ), static_cast<unsigned long>( result.queryCalls ),
        static_cast<unsigned long>( result.requestedBytes ), static_cast<unsigned long>( result.flags ),
        static_cast<unsigned long>( result.decoded ), static_cast<unsigned long>( result.contextFlags ),
        static_cast<unsigned long long>( result.exceptionRecord ), static_cast<unsigned long long>( result.contextRecord ),
        static_cast<unsigned long long>( result.pc ), static_cast<unsigned long long>( result.sp ),
        static_cast<unsigned long long>( result.fp ) );
    std::fflush( stderr );
}

void closeHandle( HANDLE& handle ) noexcept
{
    if ( handle != nullptr && handle != INVALID_HANDLE_VALUE ) {
        CloseHandle( handle );
    }
    handle = nullptr;
}

void reportFailure( const char* action, const Outcome& outcome ) noexcept
{
    std::fprintf( stderr, "Test minidump %s failed stage %lu domain %lu raw error 0x%08lx\n",
                  action, static_cast<unsigned long>( outcome.stage ),
                  static_cast<unsigned long>( outcome.domain ),
                  static_cast<unsigned long>( outcome.error ) );
    std::fflush( stderr );
}

class NativeOps {
public:
    ChannelWait waitAckOrExit( std::uint32_t timeout ) noexcept
    {
        const HANDLE handles[]{ channel.acknowledgementEvent, channel.process };
        // Ack has priority when completion and process exit are both already signaled.
        const auto result
            = WaitForMultipleObjects( 2, handles, FALSE, static_cast<DWORD>( timeout ) );
        if ( result == WAIT_OBJECT_0 ) {
            return ChannelWait::Acknowledged;
        }
        if ( result == WAIT_OBJECT_0 + 1 ) {
            return ChannelWait::Exited;
        }
        if ( result == WAIT_TIMEOUT ) {
            return ChannelWait::TimedOut;
        }
        error_ = static_cast<std::uint32_t>( GetLastError() );
        return ChannelWait::Failed;
    }

    Snapshot readAcknowledgement() noexcept
    {
        const auto published = InterlockedCompareExchange(
            reinterpret_cast<volatile LONG*>( &channel.page->acknowledgementPublication ), 0, 0 );
        const auto ack = channel.page->acknowledgement;
        return { channel.page->header, ack,
                 published == static_cast<LONG>( ack.phase ) && published != 0 };
    }

    bool publishRequest( const Request& request ) noexcept
    {
        channel.page->request = request;
        InterlockedExchange( reinterpret_cast<volatile LONG*>( &channel.page->requestPublication ),
                             static_cast<LONG>( request.sequence ) );
        if ( SetEvent( channel.requestEvent ) == FALSE ) {
            error_ = static_cast<std::uint32_t>( GetLastError() );
            return false;
        }
        return true;
    }

    ProcessWait waitProcess( std::uint32_t timeout ) noexcept
    {
        const auto result = WaitForSingleObject( channel.process, static_cast<DWORD>( timeout ) );
        if ( result == WAIT_OBJECT_0 ) {
            return ProcessWait::Signaled;
        }
        if ( result == WAIT_TIMEOUT ) {
            return ProcessWait::TimedOut;
        }
        error_ = static_cast<std::uint32_t>( GetLastError() );
        return ProcessWait::Failed;
    }

    bool terminateCollector() noexcept
    {
        const auto terminated = TerminateProcess( channel.process, ERROR_TIMEOUT ) != FALSE;
        if ( !terminated ) {
            error_ = static_cast<std::uint32_t>( GetLastError() );
        }
        return terminated;
    }

    std::uint32_t lastError() const noexcept
    {
        return error_;
    }

    [[noreturn]] void failStop( Stage stage, std::uint32_t error ) noexcept
    {
        reportFailure( "lifetime invariant", { stage, ErrorDomain::Win32, error, false, false } );
        // Never resume a handler whose original pointers might still have a live reader.
        TerminateProcess( GetCurrentProcess(), failStopExitCode );
        std::abort();
    }

    void reportReap( const ReapResult& result ) noexcept
    {
        if ( result.terminationAttempted ) {
            std::fprintf( stderr,
                          "Test minidump collector exit grace timed out; termination attempted "
                          "success %d raw error 0x%08lx\n",
                          result.terminationSucceeded,
                          static_cast<unsigned long>( result.terminationError ) );
        }
        DWORD exitCode = 0;
        if ( GetExitCodeProcess( channel.process, &exitCode ) != FALSE ) {
            std::fprintf( stderr, "Test minidump collector exited status 0x%08lx\n",
                          static_cast<unsigned long>( exitCode ) );
        }
        else {
            const auto error = GetLastError();
            std::fprintf( stderr, "Test minidump collector exit-status query failed error %lu\n",
                          static_cast<unsigned long>( error ) );
        }
        std::fflush( stderr );
    }

    void removeUnusedOutput() noexcept
    {
        FILE_DISPOSITION_INFO disposition{ static_cast<BOOLEAN>( TRUE ) };
        if ( SetFileInformationByHandle( channel.output, FileDispositionInfo, &disposition,
                                         static_cast<DWORD>( sizeof( disposition ) ) )
             == FALSE ) {
            const auto error = GetLastError();
            std::fprintf( stderr, "Test minidump unused startup output removal failed error %lu\n",
                          static_cast<unsigned long>( error ) );
            std::fflush( stderr );
        }
    }

    void closeResources() noexcept
    {
        if ( channel.diagnosticView != nullptr ) {
            UnmapViewOfFile( channel.diagnosticView );
            channel.diagnosticView = nullptr;
            channel.diagnostics = nullptr;
        }
        if ( channel.page != nullptr ) {
            UnmapViewOfFile( channel.page );
            channel.page = nullptr;
        }
        closeHandle( channel.mapping );
        closeHandle( channel.requestEvent );
        closeHandle( channel.acknowledgementEvent );
        closeHandle( channel.process );
        closeHandle( channel.output );
    }

private:
    std::uint32_t error_{};
};

void unwindStartup( const char* stage, DWORD error ) noexcept
{
    std::fprintf( stderr, "Test minidump collector startup failed stage %s error %lu\n", stage,
                  static_cast<unsigned long>( error ) );
    NativeOps ops;
    static_cast<void>(
        shutdownCollector( clientState, ops, nativePointerBits, stopTimeoutMs, exitGraceMs ) );
    std::fflush( stderr );
}

bool createCollectorProcess( const wchar_t* executable, DWORD& error ) noexcept
{
    std::array<HANDLE, 6> inherited{};
    const auto nul
        = CreateFileW( L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                       nullptr, OPEN_EXISTING, 0, nullptr );
    if ( nul == INVALID_HANDLE_VALUE ) {
        error = GetLastError();
        return false;
    }
    const std::array<HANDLE, 6> sources{ GetCurrentProcess(),
                                         channel.output,
                                         channel.mapping,
                                         channel.requestEvent,
                                         channel.acknowledgementEvent,
                                         nul };
    constexpr std::array<DWORD, 6> rights{ PROCESS_QUERY_INFORMATION | PROCESS_VM_READ
                                               | SYNCHRONIZE,
                                           GENERIC_WRITE,
                                           FILE_MAP_READ | FILE_MAP_WRITE,
                                           SYNCHRONIZE,
                                           EVENT_MODIFY_STATE,
                                           GENERIC_READ | GENERIC_WRITE };
    auto prepared = true;
    for ( std::size_t index = 0; index < inherited.size(); ++index ) {
        if ( DuplicateHandle( GetCurrentProcess(), sources[ index ], GetCurrentProcess(),
                              &inherited[ index ], rights[ index ], TRUE, 0 )
             == FALSE ) {
            error = GetLastError();
            prepared = false;
            break;
        }
    }
    CloseHandle( nul );

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = static_cast<DWORD>( sizeof( startup ) );
    SIZE_T attributeBytes = 0;
    bool attributesInitialized = false;
    if ( prepared ) {
        const auto sizing = InitializeProcThreadAttributeList( nullptr, 1, 0, &attributeBytes );
        error = sizing == FALSE ? GetLastError() : ERROR_INVALID_PARAMETER;
        prepared = sizing == FALSE && error == ERROR_INSUFFICIENT_BUFFER && attributeBytes != 0;
    }
    if ( prepared ) {
        startup.lpAttributeList = static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
            HeapAlloc( GetProcessHeap(), 0, attributeBytes ) );
        if ( startup.lpAttributeList == nullptr ) {
            error = ERROR_NOT_ENOUGH_MEMORY;
            prepared = false;
        }
    }
    if ( prepared ) {
        attributesInitialized
            = InitializeProcThreadAttributeList( startup.lpAttributeList, 1, 0, &attributeBytes )
              != FALSE;
        if ( !attributesInitialized ) {
            error = GetLastError();
            prepared = false;
        }
    }
    if ( prepared
         && UpdateProcThreadAttribute( startup.lpAttributeList, 0,
                                       PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited.data(),
                                       inherited.size() * sizeof( HANDLE ), nullptr, nullptr )
                == FALSE ) {
        error = GetLastError();
        prepared = false;
    }

    wchar_t command[ MAX_PATH + 200 ]{};
    if ( prepared ) {
        const auto written = std::swprintf(
            command, sizeof( command ) / sizeof( command[ 0 ] ),
            L"\"%ls\" %llx %llx %llx %llx %llx", executable,
            static_cast<unsigned long long>( reinterpret_cast<std::uintptr_t>( inherited[ 0 ] ) ),
            static_cast<unsigned long long>( reinterpret_cast<std::uintptr_t>( inherited[ 1 ] ) ),
            static_cast<unsigned long long>( reinterpret_cast<std::uintptr_t>( inherited[ 2 ] ) ),
            static_cast<unsigned long long>( reinterpret_cast<std::uintptr_t>( inherited[ 3 ] ) ),
            static_cast<unsigned long long>( reinterpret_cast<std::uintptr_t>( inherited[ 4 ] ) ) );
        if ( written < 0
             || static_cast<std::size_t>( written )
                    >= sizeof( command ) / sizeof( command[ 0 ] ) ) {
            error = ERROR_FILENAME_EXCED_RANGE;
            prepared = false;
        }
    }

    bool created = false;
    if ( prepared ) {
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = inherited[ 5 ];
        startup.StartupInfo.hStdOutput = inherited[ 5 ];
        startup.StartupInfo.hStdError = inherited[ 5 ];
        PROCESS_INFORMATION process{};
        created = CreateProcessW( executable, command, nullptr, nullptr, TRUE,
                                  EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, nullptr, nullptr,
                                  &startup.StartupInfo, &process )
                  != FALSE;
        if ( created ) {
            channel.process = process.hProcess;
            clientState.collectorStarted = true;
            CloseHandle( process.hThread );
        }
        else {
            error = GetLastError();
        }
    }
    if ( attributesInitialized ) {
        DeleteProcThreadAttributeList( startup.lpAttributeList );
    }
    if ( startup.lpAttributeList != nullptr ) {
        HeapFree( GetProcessHeap(), 0, startup.lpAttributeList );
    }
    for ( auto& handle : inherited ) {
        closeHandle( handle );
    }
    return created;
}

void prepareCollector() noexcept
{
    wchar_t directory[ MAX_PATH ]{};
    const auto length = GetEnvironmentVariableW( L"KLOGG_TEST_MINIDUMP_DIR", directory, MAX_PATH );
    if ( length == 0 ) {
        return;
    }
    if ( length >= MAX_PATH ) {
        unwindStartup( "directory-length", ERROR_FILENAME_EXCED_RANGE );
        return;
    }
    const auto attributes = GetFileAttributesW( directory );
    if ( attributes == INVALID_FILE_ATTRIBUTES || ( attributes & FILE_ATTRIBUTE_DIRECTORY ) == 0 ) {
        unwindStartup( "directory",
                       attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_DIRECTORY );
        return;
    }

    wchar_t absoluteDirectory[ MAX_PATH ]{};
    const auto absoluteLength = GetFullPathNameW( directory, MAX_PATH, absoluteDirectory, nullptr );
    if ( absoluteLength == 0 || absoluteLength >= MAX_PATH ) {
        unwindStartup( "absolute-directory",
                       absoluteLength == 0 ? GetLastError() : ERROR_FILENAME_EXCED_RANGE );
        return;
    }
    wchar_t outputPath[ MAX_PATH ]{};
    const auto written
        = std::swprintf( outputPath, MAX_PATH, L"%ls\\klogg-test-%lu.dmp", absoluteDirectory,
                         static_cast<unsigned long>( GetCurrentProcessId() ) );
    if ( written < 0 || static_cast<std::size_t>( written ) >= MAX_PATH ) {
        unwindStartup( "output-length", ERROR_FILENAME_EXCED_RANGE );
        return;
    }
    channel.output = CreateFileW( outputPath, GENERIC_WRITE | DELETE, 0, nullptr, CREATE_NEW,
                                  FILE_ATTRIBUTE_NORMAL, nullptr );
    if ( channel.output == INVALID_HANDLE_VALUE ) {
        const auto error = GetLastError();
        channel.output = nullptr;
        unwindStartup( "output-create-new", error );
        return;
    }
    clientState.outputOwned = true;

    channel.mapping = CreateFileMappingW( INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                          static_cast<DWORD>( sizeof( SharedEnvelope ) ), nullptr );
    if ( channel.mapping == nullptr ) {
        // Optional allocation failure does not change the original functional protocol.
        channel.mapping = CreateFileMappingW( INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                              static_cast<DWORD>( sizeof( SharedPage ) ), nullptr );
    }
    if ( channel.mapping == nullptr ) {
        unwindStartup( "mapping", GetLastError() );
        return;
    }
    const auto view = MapViewOfFile( channel.mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0,
                                     sizeof( SharedPage ) );
    if ( view == nullptr ) {
        unwindStartup( "mapping-view", GetLastError() );
        return;
    }
    channel.page = ::new ( view ) SharedPage{};
    channel.page->header = makeHeader( nativePointerBits );
    channel.diagnosticView = static_cast<SharedEnvelope*>( MapViewOfFile(
        channel.mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof( SharedEnvelope ) ) );
    if ( channel.diagnosticView != nullptr ) {
        // A full known-length view establishes extent independently of the trailer header.
        channel.diagnostics = ::new ( &channel.diagnosticView->diagnostics.data ) DiagnosticData{};
        channel.diagnostics->header = makeDiagnosticHeader( nativePointerBits );
        prepareDiagnosticIdentity();
        channel.diagnostics->targetModules = targetModuleObservations;
    }
    channel.requestEvent = CreateEventW( nullptr, FALSE, FALSE, nullptr );
    if ( channel.requestEvent == nullptr ) {
        unwindStartup( "request-event", GetLastError() );
        return;
    }
    channel.acknowledgementEvent = CreateEventW( nullptr, FALSE, FALSE, nullptr );
    if ( channel.acknowledgementEvent == nullptr ) {
        unwindStartup( "ack-event", GetLastError() );
        return;
    }

    wchar_t executable[ MAX_PATH ]{};
    const auto executableLength = GetModuleFileNameW( nullptr, executable, MAX_PATH );
    if ( executableLength == 0 || executableLength >= MAX_PATH ) {
        unwindStartup( "executable-path",
                       executableLength == 0 ? GetLastError() : ERROR_FILENAME_EXCED_RANGE );
        return;
    }
    auto* separator = std::wcsrchr( executable, L'\\' );
    if ( separator == nullptr ) {
        unwindStartup( "executable-directory", ERROR_BAD_PATHNAME );
        return;
    }
    ++separator;
    constexpr wchar_t name[] = L"crash_trace_collector.exe";
    const auto remaining
        = static_cast<std::size_t>( MAX_PATH ) - static_cast<std::size_t>( separator - executable );
    if ( sizeof( name ) / sizeof( name[ 0 ] ) > remaining ) {
        unwindStartup( "collector-path-length", ERROR_FILENAME_EXCED_RANGE );
        return;
    }
    std::wmemcpy( separator, name, sizeof( name ) / sizeof( name[ 0 ] ) );
    DWORD error = ERROR_SUCCESS;
    if ( !createCollectorProcess( executable, error ) ) {
        unwindStartup( "create-process", error );
        return;
    }

    NativeOps ops;
    const auto ready
        = awaitAcknowledgement( ops, Phase::Ready, 0, nativePointerBits, startupTimeoutMs );
    if ( !ready.succeeded ) {
        reportFailure( "collector readiness", ready );
        static_cast<void>(
            shutdownCollector( clientState, ops, nativePointerBits, stopTimeoutMs, exitGraceMs ) );
        return;
    }
    clientState.ready = true;
    std::fprintf( stderr, "Test minidump collector ready\n" );
    std::fflush( stderr );
}

void reportFirstChance( EXCEPTION_POINTERS* info ) noexcept
{
    const auto code = info->ExceptionRecord->ExceptionCode;
    std::fprintf( stderr, "\n=== first-chance crash trace (exception 0x%08lx) ===\n",
                  static_cast<unsigned long>( code ) );
    std::fprintf( stderr, "Exception address %p thread %lu\n",
                  info->ExceptionRecord->ExceptionAddress,
                  static_cast<unsigned long>( GetCurrentThreadId() ) );
#ifdef _WIN64
    std::fprintf( stderr, "Original PC 0x%llx SP 0x%llx FP 0x%llx\n",
                  static_cast<unsigned long long>( info->ContextRecord->Rip ),
                  static_cast<unsigned long long>( info->ContextRecord->Rsp ),
                  static_cast<unsigned long long>( info->ContextRecord->Rbp ) );
#else
    std::fprintf( stderr, "Original PC 0x%llx SP 0x%llx FP 0x%llx\n",
                  static_cast<unsigned long long>( info->ContextRecord->Eip ),
                  static_cast<unsigned long long>( info->ContextRecord->Esp ),
                  static_cast<unsigned long long>( info->ContextRecord->Ebp ) );
#endif
    if ( code == EXCEPTION_ACCESS_VIOLATION && info->ExceptionRecord->NumberParameters >= 2 ) {
        std::fprintf(
            stderr, "Access operation %llu address 0x%llx\n",
            static_cast<unsigned long long>( info->ExceptionRecord->ExceptionInformation[ 0 ] ),
            static_cast<unsigned long long>( info->ExceptionRecord->ExceptionInformation[ 1 ] ) );
    }
    std::fflush( stderr );

    if ( clientState.ready && InterlockedCompareExchange( &dumpCaptureStarted, 1, 0 ) == 0 ) {
        publishDiagnosticInput( info );
        const Request request{ static_cast<std::uint32_t>( Operation::Capture ), captureSequence,
                               static_cast<std::uint32_t>( GetCurrentThreadId() ), 0,
                               static_cast<std::uint64_t>(
                                   reinterpret_cast<std::uintptr_t>( info ) ) };
        NativeOps ops;
        const auto outcome = captureOnce( clientState, ops, request, nativePointerBits,
                                          captureTimeoutMs, exitGraceMs );
        reportCollectorDiagnostics();
        if ( outcome.succeeded ) {
            std::fprintf( stderr, "Test minidump captured 1 error 0\n" );
        }
        else {
            reportFailure( "capture", outcome );
        }
        std::fflush( stderr );
    }

    // Parent DbgHelp remains serialized by crashTraceActive. Dumping occurs only
    // in the collector, which has already exited before symbolization begins.
    const auto process = GetCurrentProcess();
    SymInitialize( process, nullptr, TRUE );
    alignas( SYMBOL_INFO ) char symbolStorage[ sizeof( SYMBOL_INFO ) + 256 ]{};
    auto* const symbol = reinterpret_cast<SYMBOL_INFO*>( symbolStorage );
    symbol->SizeOfStruct = static_cast<ULONG>( sizeof( SYMBOL_INFO ) );
    symbol->MaxNameLen = 255;
    auto context = *info->ContextRecord;
    STACKFRAME64 frame{};
#ifdef _WIN64
    constexpr DWORD machineType = IMAGE_FILE_MACHINE_AMD64;
    frame.AddrPC.Offset = context.Rip;
    frame.AddrStack.Offset = context.Rsp;
    frame.AddrFrame.Offset = context.Rbp;
#else
    constexpr DWORD machineType = IMAGE_FILE_MACHINE_I386;
    frame.AddrPC.Offset = context.Eip;
    frame.AddrStack.Offset = context.Esp;
    frame.AddrFrame.Offset = context.Ebp;
#endif
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrStack.Mode = AddrModeFlat;
    frame.AddrFrame.Mode = AddrModeFlat;
    const auto thread = GetCurrentThread();
    for ( int index = 0; index < 64; ++index ) {
        if ( !StackWalk64( machineType, process, thread, &frame, &context, nullptr,
                           SymFunctionTableAccess64, SymGetModuleBase64, nullptr )
             || frame.AddrPC.Offset == 0 ) {
            break;
        }
        DWORD64 displacement = 0;
        if ( SymFromAddr( process, frame.AddrPC.Offset, &displacement, symbol ) ) {
            std::fprintf( stderr, "  #%02d %s+0x%llx\n", index, symbol->Name,
                          static_cast<unsigned long long>( displacement ) );
        }
        else {
            std::fprintf( stderr, "  #%02d 0x%llx\n", index,
                          static_cast<unsigned long long>( frame.AddrPC.Offset ) );
        }
    }
    std::fflush( stderr );
}

LONG CALLBACK firstChanceCrashTrace( EXCEPTION_POINTERS* info )
{
    switch ( info->ExceptionRecord->ExceptionCode ) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case static_cast<DWORD>( 0xc0000374U ):
        break;
    default:
        return EXCEPTION_CONTINUE_SEARCH;
    }
    // Closing is checked before lock acquisition to avoid same-thread teardown
    // faults deadlocking against the exclusive lock held by normal-exit cleanup.
    if ( InterlockedCompareExchange( &closing, 0, 0 ) != 0
         || InterlockedCompareExchange( &crashTraceActive, 1, 0 ) != 0 ) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    bool acquired = false;
    __try {
        AcquireSRWLockShared( &lifetimeLock );
        acquired = true;
        if ( InterlockedCompareExchange( &closing, 0, 0 ) == 0 ) {
            reportFirstChance( info );
        }
    } __finally {
        if ( acquired ) {
            // A nested diagnostic SEH unwind must not release original pointers
            // if request publication happened before normal reap was reached.
            NativeOps ops;
            quiesceRequestedCollector( clientState, ops, exitGraceMs );
            ReleaseSRWLockShared( &lifetimeLock );
        }
        InterlockedExchange( &crashTraceActive, 0 );
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void shutdownFirstChanceCrashTrace() noexcept
{
    if ( InterlockedExchange( &closing, 1 ) != 0 ) {
        return;
    }
    if ( vectoredHandler != nullptr ) {
        RemoveVectoredExceptionHandler( vectoredHandler );
        vectoredHandler = nullptr;
    }
    // RemoveVEH alone does not wait for callbacks already in flight. The exclusive
    // lock is the actual quiescence barrier for IPC handles and mapped memory.
    AcquireSRWLockExclusive( &lifetimeLock );
    __try {
        NativeOps ops;
        const auto stopped
            = shutdownCollector( clientState, ops, nativePointerBits, stopTimeoutMs, exitGraceMs );
        if ( stopped.succeeded ) {
            std::fprintf( stderr, "Test minidump collector stopped\n" );
            std::fflush( stderr );
        }
        else if ( stopped.stage != Stage::None ) {
            reportFailure( "collector shutdown", stopped );
        }
    } __finally {
        ReleaseSRWLockExclusive( &lifetimeLock );
    }
}

} // namespace
#endif // _WIN32

namespace klogg::testing {

void installFirstChanceCrashTrace()
{
#ifdef _WIN32
    if ( InterlockedCompareExchange( &installationStarted, 1, 0 ) != 0 ) {
        return;
    }
    if ( std::atexit( shutdownFirstChanceCrashTrace ) == 0 ) {
        prepareCollector();
    }
    else {
        std::fprintf( stderr,
                      "Test minidump collector startup failed stage atexit-registration\n" );
        std::fflush( stderr );
    }
    vectoredHandler = AddVectoredExceptionHandler( 1, firstChanceCrashTrace );
    if ( vectoredHandler == nullptr ) {
        const auto error = GetLastError();
        std::fprintf( stderr, "Test crash trace registration failed error %lu\n",
                      static_cast<unsigned long>( error ) );
        shutdownFirstChanceCrashTrace();
    }
#endif
}

} // namespace klogg::testing
