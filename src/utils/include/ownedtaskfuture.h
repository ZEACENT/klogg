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
inline std::mutex& ownedTaskPublicationMutex()
{
    static std::mutex mutex;
    return mutex;
}
} // namespace detail

template <typename Function>
auto runOwnedTask( Function function ) -> OwnedTaskFuture<std::invoke_result_t<Function>>
{
    using Result = std::invoke_result_t<Function>;
    struct Task final : QRunnable {
        Task( Function taskFunction, QFutureInterface<void> notification )
            : function( std::move( taskFunction ) ), completion( std::move( notification ) ) {}

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
    auto result = task->function.get_future().share();
    {
        const std::lock_guard<std::mutex> lock( detail::ownedTaskPublicationMutex() );
        // Preserve the shared pool's concurrency bound; Qt owns the runnable.
        QThreadPool::globalInstance()->start( task.release() );
    }
    return { completion.future(), std::move( result ) };
}

} // namespace klogg

#endif
