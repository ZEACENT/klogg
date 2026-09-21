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

namespace {

// Vectored handlers run on the faulting thread before any SEH frame handler
// (including Catch2's). Walk the stack from the EXCEPTION_POINTERS context
// record so the trace is rooted at the faulting instruction rather than at
// the handler's own exception-dispatch frames. Everything here is last-gasp
// diagnostics: no heap allocation after SymInitialize, plain stdio, and we
// always continue the search so Catch2/WER still run.
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

    std::fprintf( stderr, "\n=== first-chance crash trace (exception 0x%08lx) ===\n",
                  static_cast<unsigned long>( code ) );

    const auto process = GetCurrentProcess();
    // Fails harmlessly if the symbols subsystem is already initialized.
    SymInitialize( process, nullptr, TRUE );

    alignas( SYMBOL_INFO ) char symbolStorage[ sizeof( SYMBOL_INFO ) + 256 ];
    auto* const symbol = reinterpret_cast<SYMBOL_INFO*>( symbolStorage );
    symbol->SizeOfStruct = sizeof( SYMBOL_INFO );
    symbol->MaxNameLen = 255;

    auto context = *info->ContextRecord;
    STACKFRAME64 frame{};
#ifdef _WIN64
    frame.AddrPC.Offset = context.Rip;
    frame.AddrStack.Offset = context.Rsp;
    frame.AddrFrame.Offset = context.Rbp;
#else
    frame.AddrPC.Offset = context.Eip;
    frame.AddrStack.Offset = context.Esp;
    frame.AddrFrame.Offset = context.Ebp;
#endif
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrStack.Mode = AddrModeFlat;
    frame.AddrFrame.Mode = AddrModeFlat;

    const auto thread = GetCurrentThread();
    for ( int index = 0; index < 64; ++index ) {
        if ( !StackWalk64( IMAGE_FILE_MACHINE_NATIVE, process, thread, &frame, &context,
                           nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr ) ) {
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
    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

#endif // _WIN32

namespace klogg::testing {

void installFirstChanceCrashTrace()
{
#ifdef _WIN32
    AddVectoredExceptionHandler( 1, firstChanceCrashTrace );
#endif
}

} // namespace klogg::testing
