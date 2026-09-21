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
// (including Catch2's), so the captured stack is the fault stack. Everything
// here is last-gasp diagnostics: no heap allocation after SymInitialize,
// plain stdio, and we always continue the search so Catch2/WER still run.
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

    void* frames[ 64 ];
    const auto frameCount = CaptureStackBackTrace( 2, 64, frames, nullptr );

    std::fprintf( stderr, "\n=== first-chance crash trace (exception 0x%08lx) ===\n",
                  static_cast<unsigned long>( code ) );

    const auto process = GetCurrentProcess();
    // Fails harmlessly if the symbols subsystem is already initialized.
    SymInitialize( process, nullptr, TRUE );

    alignas( SYMBOL_INFO ) char symbolStorage[ sizeof( SYMBOL_INFO ) + 256 ];
    auto* const symbol = reinterpret_cast<SYMBOL_INFO*>( symbolStorage );
    symbol->SizeOfStruct = sizeof( SYMBOL_INFO );
    symbol->MaxNameLen = 255;

    for ( DWORD frame = 0; frame < frameCount; ++frame ) {
        const auto address = reinterpret_cast<DWORD64>( frames[ frame ] );
        DWORD64 displacement = 0;
        if ( SymFromAddr( process, address, &displacement, symbol ) ) {
            std::fprintf( stderr, "  #%02lu %s+0x%llx\n", static_cast<unsigned long>( frame ),
                          symbol->Name, static_cast<unsigned long long>( displacement ) );
        }
        else {
            std::fprintf( stderr, "  #%02lu 0x%llx\n", static_cast<unsigned long>( frame ),
                          static_cast<unsigned long long>( address ) );
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
