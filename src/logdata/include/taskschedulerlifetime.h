#ifndef KLOGG_TASKSCHEDULERLIFETIME_H
#define KLOGG_TASKSCHEDULERLIFETIME_H

#include <tbb/global_control.h>

namespace klogg {

// One owner per process, declared before QApplication and all task producers.
// Producers must stop and release their graphs/arenas before this owner dies.
// Non-main TBB callers must be native-joined, as the indexing/search workers are;
// Qt5 pool drainage alone does not wait for their native TLS destructors.
// Completing graph tasks is not sufficient: allocator TLS belongs to the TBB
// worker threads and must be destroyed before process-wide allocator teardown.
class TaskSchedulerLifetime {
  public:
    TaskSchedulerLifetime() = default;
    ~TaskSchedulerLifetime() noexcept;

    TaskSchedulerLifetime( const TaskSchedulerLifetime& ) = delete;
    TaskSchedulerLifetime& operator=( const TaskSchedulerLifetime& ) = delete;
    TaskSchedulerLifetime( TaskSchedulerLifetime&& ) = delete;
    TaskSchedulerLifetime& operator=( TaskSchedulerLifetime&& ) = delete;

  private:
    tbb::task_scheduler_handle scheduler_{ tbb::attach{} };
};

} // namespace klogg

#endif
