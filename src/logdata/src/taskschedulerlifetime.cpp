#include "taskschedulerlifetime.h"

#include <cstdio>
#include <cstdlib>
#include <new>

namespace klogg {

TaskSchedulerLifetime::~TaskSchedulerLifetime() noexcept
{
    // Application-owned TBB producer threads have been native-joined. Qt5's
    // pool shutdown is not a substitute for that join. Do not recreate Qt pools.
    // The raw TBB handle destructor only releases its reference; finalize also
    // joins worker threads, including their allocator's pthread TLS cleanup.
    if ( !tbb::finalize( scheduler_, std::nothrow ) ) {
        std::fputs( "Cannot join the process task scheduler: task producers or scheduler "
                    "handles are still alive\n", stderr );
        std::fflush( stderr );
        // Continuing into allocator atexit callbacks would race live workers.
        std::_Exit( EXIT_FAILURE );
    }
}

} // namespace klogg
