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

#include <windows.h>

#include <dbghelp.h>

#include <cstdio>
#include <cwchar>

namespace {

wchar_t testDumpPath[ MAX_PATH ]{};
LONG crashTraceActive = 0;
LONG dumpCaptureStarted = 0;

void prepareTestDumpPath()
{
    wchar_t directory[ MAX_PATH ]{};
    const auto length = GetEnvironmentVariableW( L"KLOGG_TEST_MINIDUMP_DIR", directory, MAX_PATH );
    if ( length == 0 || length >= MAX_PATH ) {
        return;
    }
    const auto attributes = GetFileAttributesW( directory );
    if ( attributes == INVALID_FILE_ATTRIBUTES || !( attributes & FILE_ATTRIBUTE_DIRECTORY ) ) {
        std::fprintf( stderr, "Test minidump directory is unavailable (error %lu)\n",
                      static_cast<unsigned long>( GetLastError() ) );
        return;
    }
    const auto written = std::swprintf( testDumpPath, MAX_PATH, L"%ls\\klogg-test-%lu.dmp",
                                       directory, static_cast<unsigned long>( GetCurrentProcessId() ) );
    if ( written < 0 || written >= MAX_PATH ) {
        testDumpPath[ 0 ] = L'\0';
    }
}

// Vectored handlers run on the faulting thread before any SEH frame handler
// (including Catch2's). Walk the stack from the EXCEPTION_POINTERS context
// record so the trace is rooted at the faulting instruction rather than at
// the handler's own exception-dispatch frames. Everything here is last-gasp
// diagnostics: plain stdio, one best-effort minimal test dump before symbol
// lookup, and continued exception search so Catch2/WER still run. DbgHelp may
// allocate internally; an unstable process cannot guarantee capture success.
LONG CALLBACK firstChanceCrashTrace( EXCEPTION_POINTERS* info )
{
    const auto code = info->ExceptionRecord->ExceptionCode;
    switch ( code ) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case static_cast<DWORD>( 0xC0000374L ): // STATUS_HEAP_CORRUPTION
        break;
    default:
        return EXCEPTION_CONTINUE_SEARCH;
    }

    if ( InterlockedCompareExchange( &crashTraceActive, 1, 0 ) != 0 ) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
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
        std::fprintf( stderr, "Access operation %llu address 0x%llx\n",
                      static_cast<unsigned long long>( info->ExceptionRecord->ExceptionInformation[ 0 ] ),
                      static_cast<unsigned long long>( info->ExceptionRecord->ExceptionInformation[ 1 ] ) );
    }
    std::fflush( stderr );

    const auto process = GetCurrentProcess();
    if ( testDumpPath[ 0 ] != L'\0'
         && InterlockedCompareExchange( &dumpCaptureStarted, 1, 0 ) == 0 ) {
        const auto file = CreateFileW( testDumpPath, GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                      FILE_ATTRIBUTE_NORMAL, nullptr );
        if ( file != INVALID_HANDLE_VALUE ) {
            MINIDUMP_EXCEPTION_INFORMATION exception{};
            exception.ThreadId = GetCurrentThreadId();
            exception.ExceptionPointers = info;
            exception.ClientPointers = FALSE;
            const auto captured = MiniDumpWriteDump( process, GetCurrentProcessId(), file,
                                                      MiniDumpNormal, &exception, nullptr, nullptr );
            const auto error = captured ? ERROR_SUCCESS : GetLastError();
            CloseHandle( file );
            std::fprintf( stderr, "Test minidump captured %d error %lu\n", captured != FALSE,
                          static_cast<unsigned long>( error ) );
        }
        else {
            std::fprintf( stderr, "Test minidump create failed error %lu\n",
                          static_cast<unsigned long>( GetLastError() ) );
        }
        std::fflush( stderr );
    }
    // Fails harmlessly if the symbols subsystem is already initialized.
    SymInitialize( process, nullptr, TRUE );

    alignas( SYMBOL_INFO ) char symbolStorage[ sizeof( SYMBOL_INFO ) + 256 ];
    auto* const symbol = reinterpret_cast<SYMBOL_INFO*>( symbolStorage );
    symbol->SizeOfStruct = sizeof( SYMBOL_INFO );
    symbol->MaxNameLen = 255;

    auto context = *info->ContextRecord;
    STACKFRAME64 frame{};
#ifdef _WIN64
    // IMAGE_FILE_MACHINE_NATIVE needs a newer Windows SDK than the oldest CI
    // toolchain ships; pick the machine type explicitly.
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
                           SymFunctionTableAccess64, SymGetModuleBase64, nullptr ) ) {
            break;
        }
        if ( frame.AddrPC.Offset == 0 ) {
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
    InterlockedExchange( &crashTraceActive, 0 );
    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

#endif // _WIN32

namespace klogg::testing {

void installFirstChanceCrashTrace()
{
#ifdef _WIN32
    prepareTestDumpPath();
    AddVectoredExceptionHandler( 1, firstChanceCrashTrace );
#endif
}

} // namespace klogg::testing
