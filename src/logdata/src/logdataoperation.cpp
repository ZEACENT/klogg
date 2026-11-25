/*
 * Copyright (C) 2009, 2010, 2013, 2014, 2015 Nicolas Bonnefon and other contributors
 *
 * This file is part of glogg.
 *
 * glogg is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * glogg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with glogg.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Copyright (C) 2021 Anton Filimonov and other contributors
 *
 * This file is part of klogg.
 *
 * klogg is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * klogg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with klogg.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "logdataoperation.h"

#include "configuration.h"
#include "log.h"
#include "overload_visitor.h"
#include "synchronization.h"

void AttachOperation::doStart( LogDataWorker& workerThread ) const
{
    const auto defaultEncodingMib = Configuration::get().defaultEncodingMib();
    LOG_INFO << "Attaching " << filename_ << ", encoding " << defaultEncodingMib;
    workerThread.attachFile( filename_ );
    workerThread.indexAll();
}

void FullReindexOperation::doStart( LogDataWorker& workerThread ) const
{
    LOG_INFO << "Reindexing (full)";
    workerThread.indexAll( forcedEncoding_ );
}

void PartialReindexOperation::doStart( LogDataWorker& workerThread ) const
{
    LOG_INFO << "Reindexing (partial)";
    workerThread.indexAdditionalLines();
}

void CheckDataChangesOperation::doStart( LogDataWorker& workerThread ) const
{
    LOG_INFO << "Checking file changes";
    workerThread.checkFileChanges();
}

OperationQueue::OperationQueue( std::function<void()> beforeOperationStart )
    : beforeOperationStart_( std::move( beforeOperationStart ) )
{
}

void OperationQueue::setWorker( std::unique_ptr<LogDataWorker>&& worker )
{
    ScopedLock guard( mutex_ );
    shuttingDown_ = false;
    worker_ = std::move( worker );
}

void OperationQueue::interrupt()
{
    ScopedLock guard( mutex_ );
    if ( worker_ ) {
        worker_->interrupt();
    }
}

bool OperationQueue::beginRetirement()
{
    ScopedLock guard( mutex_ );
    shuttingDown_ = true;
    pendingOperation_ = {};
    if ( worker_ ) {
        worker_->interrupt();
    }
    return executingOperation_.index() == 0;
}

void OperationQueue::shutdown()
{
    std::unique_ptr<LogDataWorker> worker;
    {
        ScopedLock guard( mutex_ );
        shuttingDown_ = true;
        pendingOperation_ = {};
        executingOperation_ = {};
        worker = std::move( worker_ );
    }

    if ( worker ) {
        worker->interrupt();
    }

    LOG_INFO << "Operation queue shutdown";
}

void OperationQueue::waitForWorkerDone()
{
    LogDataWorker* worker = nullptr;
    {
        ScopedLock guard( mutex_ );
        worker = worker_.get();
    }

    if ( worker ) {
        worker->waitForDone();
    }
}

void OperationQueue::tryStartPendingOperation()
{
    if ( shuttingDown_ ) {
        LOG_INFO << "Skip starting operation: queue is shutting down";
        pendingOperation_ = {};
        executingOperation_ = {};
        return;
    }

    executingOperation_ = std::exchange( pendingOperation_, {} );
    if ( !worker_ ) {
        LOG_WARNING << "No worker for operation";
        executingOperation_ = {};
        return;
    }

    std::visit( makeOverloadVisitor(
                    [ this ]( const LogDataOperation& logDataOperation ) {
                        beforeOperationStart_();
                        logDataOperation.start( worker_.get() );
                        LOG_INFO << "Started operation " << executingOperation_.index();
                    },
                    []( std::monostate ) { LOG_INFO << "no operation to start"; } ),
                executingOperation_ );
}

void OperationQueue::enqueueOperation( OperationVariant&& operation )
{
    ScopedLock guard( mutex_ );

    if ( shuttingDown_ ) {
        LOG_INFO << "Skip enqueue operation during shutdown";
        return;
    }

    LOG_INFO << "Enqueue operation " << operation.index() << ", now executing "
             << executingOperation_.index();

    pendingOperation_ = std::move(operation);

    if ( executingOperation_.index() == 0 ) {
        tryStartPendingOperation();
    }
}

void OperationQueue::finishOperationAndStartNext()
{
    ScopedLock guard( mutex_ );
    LOG_INFO << "Finished operation " << executingOperation_.index() << ", next operation "
             << pendingOperation_.index();

    tryStartPendingOperation();
}
