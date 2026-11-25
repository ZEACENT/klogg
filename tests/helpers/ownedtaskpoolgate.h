#ifndef KLOGG_TEST_OWNEDTASKPOOLGATE_H
#define KLOGG_TEST_OWNEDTASKPOOLGATE_H

#include <QScopedValueRollback>
#include <QSemaphore>
#include <QThreadPool>

#include "ownedtaskfuture.h"

// Park only owned tasks submitted on this thread, never Qt's unrelated work.
// Declare after the objects under test so unwinding opens the gate before teardown.
struct ScopedOwnedTaskPoolGate {
    QThreadPool pool;
    QScopedValueRollback<QThreadPool*> redirect{ klogg::detail::ownedTaskPoolOverride(), &pool };
    QSemaphore entered;
    QSemaphore release;
    klogg::OwnedTaskFuture<void> worker;

    ScopedOwnedTaskPoolGate()
    {
        pool.setMaxThreadCount( 1 );
        worker = klogg::runOwnedTask( [ this ] {
            entered.release();
            release.acquire();
        } );
    }
    ~ScopedOwnedTaskPoolGate()
    {
        release.release();
        worker.waitForFinished();
        pool.waitForDone();
    }
    ScopedOwnedTaskPoolGate( const ScopedOwnedTaskPoolGate& ) = delete;
    ScopedOwnedTaskPoolGate& operator=( const ScopedOwnedTaskPoolGate& ) = delete;
};

#endif
