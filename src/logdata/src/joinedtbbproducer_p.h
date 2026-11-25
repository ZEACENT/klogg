#ifndef KLOGG_JOINEDTBBPRODUCER_P_H
#define KLOGG_JOINEDTBBPRODUCER_P_H

#include <future>
#include <thread>
#include <type_traits>
#include <utility>

namespace klogg {

// Private preparations are synchronous operations dispatched off the GUI thread.
// A Qt or Windows pool can keep that calling thread alive after the result is
// delivered. TBB's external producer state belongs to its native thread, so
// drain its TLS before returning rather than relying on the pool to retire it.
template <typename Function>
auto runOnJoinedTbbProducer( Function function ) -> std::invoke_result_t<Function&>
{
    std::packaged_task<std::invoke_result_t<Function&>()> task{ std::move( function ) };
    auto result = task.get_future();
    std::thread producer{ std::move( task ) };
    producer.join();
    return result.get();
}

} // namespace klogg

#endif
