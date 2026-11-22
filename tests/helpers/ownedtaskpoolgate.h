#ifndef KLOGG_TEST_OWNEDTASKPOOLGATE_H
#define KLOGG_TEST_OWNEDTASKPOOLGATE_H

#include <QSemaphore>
#include <QThreadPool>

#include "ownedtaskfuture.h"

// Hold the only shared-pool worker while a test dispatches owned tasks. Declare
// this after the objects under test so failed assertions unblock before teardown.
struct ScopedOwnedTaskPoolGate {
    QThreadPool* pool = QThreadPool::globalInstance();
    int previousThreads = pool->maxThreadCount();
    QSemaphore entered;
    QSemaphore release;
    klogg::OwnedTaskFuture<void> worker;

    ScopedOwnedTaskPoolGate()
    {
        pool->setMaxThreadCount( 1 );
        worker = klogg::runOwnedTask( [ this ] {
            entered.release();
            release.acquire();
        } );
    }
    ~ScopedOwnedTaskPoolGate()
    {
        release.release();
        worker.waitForFinished();
        pool->waitForDone();
        pool->setMaxThreadCount( previousThreads );
    }
    ScopedOwnedTaskPoolGate( const ScopedOwnedTaskPoolGate& ) = delete;
    ScopedOwnedTaskPoolGate& operator=( const ScopedOwnedTaskPoolGate& ) = delete;
};

#endif
