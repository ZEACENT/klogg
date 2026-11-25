#ifndef KLOGG_OWNEDTASKFUTURE_H
#define KLOGG_OWNEDTASKFUTURE_H

#include <QFuture>
#include <QFutureInterface>
#include <QRunnable>
#include <QThreadPool>

#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <type_traits>
#include <utility>

namespace klogg {

// Qt carries only a completion event. Task and result ownership cross threads
// through the standard future, including when the host Qt is not instrumented.
template <typename Result>
class OwnedTaskFuture {
public:
    OwnedTaskFuture() = default;
    OwnedTaskFuture( QFuture<void> completion, std::shared_future<Result> result )
        : completion_( std::move( completion ) ), result_( std::move( result ) ) {}

    bool isFinished() const
    {
        return !result_.valid()
            || result_.wait_for( std::chrono::seconds{ 0 } ) == std::future_status::ready;
    }
    void waitForFinished() const
    {
        if ( result_.valid() ) result_.wait();
    }
    Result result() const { return result_.get(); }
    QFuture<void> completion() const { return completion_; }

private:
    QFuture<void> completion_;
    std::shared_future<Result> result_;
};

namespace detail {
// A fixture may redirect submissions from its own thread without occupying the
// Qt global pool (which some Qt versions also use for synchronous raster work).
inline QThreadPool*& ownedTaskPoolOverride()
{
    static thread_local QThreadPool* pool = nullptr;
    return pool;
}

inline QThreadPool* ownedTaskPool()
{
    auto* pool = ownedTaskPoolOverride();
    return pool != nullptr ? pool : QThreadPool::globalInstance();
}

inline std::mutex& ownedTaskPublicationMutex()
{
    static std::mutex mutex;
    return mutex;
}
} // namespace detail

// Completion observers never own a result. A single consumer can move it out
// on publication; a polling registry may instead share it for repeatable reads.
// This future comes from a packaged task, never from std::async: dropping it
// on abort does not wait for the worker.
template <typename Result>
struct SingleOwnerTaskFuture {
    QFuture<void> finished;
    std::future<Result> result;
};

template <typename Function>
auto runSingleOwnerTask( Function function ) -> SingleOwnerTaskFuture<std::invoke_result_t<Function>>
{
    using Result = std::invoke_result_t<Function>;
    struct Task final : QRunnable {
        Task( Function taskFunction, QFutureInterface<void> notification )
            : function( [ callable = std::optional<Function>( std::move( taskFunction ) ) ]() mutable
                            -> Result {
                  // Capture teardown belongs to completion, including on failure.
                  // The shared future may outlive both the work and its receiver.
                  struct ReleaseCapture {
                      std::optional<Function>* callable;
                      ~ReleaseCapture() { callable->reset(); }
                  } release{ &callable };
                  return ( *callable )();
              } )
            , completion( std::move( notification ) ) {}

        void run() override
        {
            // Publish construction through an instrumented C++ boundary before
            // reading task state. Never hold this mutex while executing work.
            {
                const std::lock_guard<std::mutex> lock( detail::ownedTaskPublicationMutex() );
            }
            function(); // packaged_task stores exceptions as well as results.
            completion.reportFinished();
        }

        std::packaged_task<Result()> function;
        QFutureInterface<void> completion;
    };

    QFutureInterface<void> completion;
    completion.reportStarted();
    auto task = std::make_unique<Task>( std::move( function ), completion );
    auto result = task->function.get_future();
    {
        const std::lock_guard<std::mutex> lock( detail::ownedTaskPublicationMutex() );
        // Qt takes ownership; production submissions retain the global pool bound.
        detail::ownedTaskPool()->start( task.release() );
    }
    return { completion.future(), std::move( result ) };
}

template <typename Function>
auto runOwnedTask( Function function ) -> OwnedTaskFuture<std::invoke_result_t<Function>>
{
    auto task = runSingleOwnerTask( std::move( function ) );
    return { std::move( task.finished ), task.result.share() };
}

} // namespace klogg

#endif
