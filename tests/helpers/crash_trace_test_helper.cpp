#ifdef _WIN32

#include <windows.h>

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

int main()
{
    SetErrorMode( SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX );
    klogg::testing::installFirstChanceCrashTrace();
    raiseSyntheticAccessViolation();
    raiseSyntheticAccessViolation();
    return 67;
}

#endif // _WIN32
