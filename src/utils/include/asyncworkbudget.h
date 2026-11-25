#ifndef KLOGG_ASYNCWORKBUDGET_H
#define KLOGG_ASYNCWORKBUDGET_H

#include <atomic>
#include <cstddef>
#include <memory>
#include <utility>

namespace klogg {

// A permit follows the actual work, not the UI operation record. Resetting a
// controller cannot free admission while its detached worker still owns a lease.
class AsyncWorkBudget {
    struct State {
        explicit State( std::size_t maximum ) : limit( maximum ) {}
        const std::size_t limit;
        std::atomic<std::size_t> active{ 0 };
    };
    class Permit {
      public:
        explicit Permit( std::shared_ptr<State> state ) : state_( std::move( state ) ) {}
        ~Permit() { if ( acquired_ ) --state_->active; }
        Permit( const Permit& ) = delete;
        Permit& operator=( const Permit& ) = delete;
      private:
        friend class AsyncWorkBudget;
        std::shared_ptr<State> state_;
        bool acquired_ = false;
    };
  public:
    using Lease = std::shared_ptr<Permit>;
    explicit AsyncWorkBudget( std::size_t maximum ) : state_( std::make_shared<State>( maximum ) ) {}

    Lease tryAcquire() const
    {
        auto count = state_->active.load();
        if ( count >= state_->limit ) return {};
        // Allocate before incrementing; allocation failure must not leak a slot.
        auto permit = std::make_shared<Permit>( state_ );
        while ( count < state_->limit ) {
            if ( state_->active.compare_exchange_weak( count, count + 1 ) ) {
                permit->acquired_ = true;
                return permit;
            }
        }
        return {};
    }
  private:
    std::shared_ptr<State> state_;
};

} // namespace klogg
#endif
