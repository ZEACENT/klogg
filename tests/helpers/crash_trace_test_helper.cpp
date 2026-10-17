#ifdef _WIN32

#include <windows.h>

#include <cstdio>
#include <cstring>

#include "crash_trace.h"

namespace {
void raiseSyntheticAccessViolation()
{
    const ULONG_PTR parameters[]{ 0, 0x1234 };
    __try {
        RaiseException( EXCEPTION_ACCESS_VIOLATION, 0, 2, parameters );
    }
    __except ( EXCEPTION_EXECUTE_HANDLER ) {
    }
}
} // namespace

int main( int argc, char* argv[] )
{
    const bool idle = argc == 2 && std::strcmp( argv[ 1 ], "--idle" ) == 0;
    if ( argc != 1 && !idle ) {
        std::fprintf( stderr, "Unknown synthetic crash probe mode\n" );
        return 64;
    }
    SetErrorMode( SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX );
    klogg::testing::installFirstChanceCrashTrace();
    if ( idle ) {
        return 0;
    }
    raiseSyntheticAccessViolation();
    raiseSyntheticAccessViolation();
    return 67;
}

#endif // _WIN32
