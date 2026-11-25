#define CATCH_CONFIG_RUNNER
#include <catch2/catch.hpp>

#include "taskschedulerlifetime.h"
#include "ownedtaskfuture.h"
#include "configuration.h"
#include "logdata.h"
#include "preparedfilesource.h"
#include "searchpreparation.h"

#include <QCoreApplication>
#include <QProcess>
#include <QTemporaryFile>

#include <mimalloc.h>
#include <tbb/task_arena.h>

#include <atomic>
#include <cstdlib>
#include <future>
#include <memory>
#include <string>
#include <thread>

namespace {
struct WorkerState {
    std::atomic<int> destroyed{ 0 };
};

struct WorkerExitProbe {
    explicit WorkerExitProbe( std::shared_ptr<WorkerState> workerState )
        : state( std::move( workerState ) )
        , allocation( mi_malloc( 4096 ) )
    {
    }

    ~WorkerExitProbe()
    {
        mi_free( allocation );
        state->destroyed.fetch_add( 1 );
    }

    std::shared_ptr<WorkerState> state;
    void* allocation;
};

bool runOnWorker( const std::shared_ptr<WorkerState>& state )
{
    // enqueue never executes the body on this waiting application thread.
    // No reserved master slot is needed, even on a one-core runner.
    tbb::task_arena arena{ 2, 0 };
    auto ran = std::make_shared<std::promise<bool>>();
    auto completed = ran->get_future();
    arena.enqueue( [ state, ran ] {
        thread_local auto probe = std::make_unique<WorkerExitProbe>( state );
        ran->set_value( probe->allocation != nullptr );
    } );
    return completed.get();
}

int retainedHandleChild()
{
    // A second live handle makes finalization fail. Silent release would let
    // this child return success without having joined any worker.
    tbb::task_scheduler_handle retained{ tbb::attach{} };
    {
        klogg::TaskSchedulerLifetime scheduler;
        if ( !runOnWorker( std::make_shared<WorkerState>() ) ) {
            return 2;
        }
    }
    tbb::finalize( retained );
    return EXIT_SUCCESS;
}

int activeProducerChild()
{
    std::promise<void> release;
    auto released = release.get_future();
    std::thread producer;
    {
        klogg::TaskSchedulerLifetime scheduler;
        auto ready = std::make_shared<std::promise<bool>>();
        auto started = ready->get_future();
        producer = std::thread( [ ready, &released ] {
            ready->set_value( runOnWorker( std::make_shared<WorkerState>() ) );
            released.wait();
        } );
        if ( !started.get() ) {
            release.set_value();
            producer.join();
            return 2;
        }
        // Task/arena completion is insufficient while an external TBB caller
        // remains alive. Finalization must reject this ownership violation.
    }
    release.set_value();
    producer.join();
    return EXIT_SUCCESS;
}

int applicationLifetimeChild( int argc, char* argv[] )
{
    auto state = std::make_shared<WorkerState>();
    auto producerState = std::make_shared<WorkerState>();
    bool allocated = false;
    {
        klogg::TaskSchedulerLifetime scheduler;
        {
            QCoreApplication app( argc, argv );
            // Match indexing/search ownership: Qt dispatches work, but every
            // external TBB caller is native-joined before reporting completion.
            // Qt5 QThread::wait alone does not wait for native TLS destruction.
            QThreadPool::globalInstance()->setExpiryTimeout( -1 );
            auto operation = klogg::runOwnedTask( [ state, producerState ] {
                bool result = false;
                std::thread producer( [ &result, state, producerState ] {
                    thread_local auto probe = std::make_unique<WorkerExitProbe>( producerState );
                    result = runOnWorker( state ) && probe->allocation != nullptr;
                } );
                producer.join();
                return result;
            } );
            allocated = operation.result();
        }
    }
    return allocated && producerState->destroyed.load() == 1 && state->destroyed.load() == 1
        ? EXIT_SUCCESS : EXIT_FAILURE;
}

int privatePreparationChild( bool search )
{
    QTemporaryFile input;
    if ( !input.open() || input.write( "hit\nmiss\nhit\n" ) != 13 ) return 2;
    const auto path = input.fileName();
    input.close();
    AtomicFlag cancelled;
    std::thread dispatcher;
    std::promise<void> release;
    auto released = release.get_future();
    bool ready = false;
    {
        klogg::TaskSchedulerLifetime scheduler;
        SearchableLogData::ReadSnapshot::Ptr snapshot;
        if ( search ) {
            // Seed the parallel search independently of the borrowed dispatch thread.
            Configuration::getDefaultForTests();
            auto prepared = prepareFileSource( path, {}, cancelled );
            auto model = LogData::fromPrepared( prepared.source );
            if ( !model ) return 2;
            snapshot = model->searchableSnapshot();
        }
        std::promise<bool> finished;
        auto result = finished.get_future();
        dispatcher = std::thread( [&, snapshot, completion = std::move( finished )]() mutable {
            if ( search ) {
                const auto outcome = prepareFullSearch( { snapshot, RegularExpressionPattern{ QStringLiteral( "hit" ) },
                    { 1, 2, false, RegexpEngine::QRegularExpression }, 0_lnum, {} }, cancelled );
                completion.set_value( outcome.status == SearchPreparation::Status::Ready );
            } else {
                const auto outcome = prepareFileSource( path, {}, cancelled );
                completion.set_value( outcome.status == FileSourcePreparation::Status::Ready );
            }
            // Reusable dispatcher threads outlive the call (Qt pool / MSVC std::async).
            // They are deliberately still alive while the scheduler finalizes.
            released.wait();
        } );
        ready = result.get();
    }
    release.set_value();
    dispatcher.join();
    return ready ? EXIT_SUCCESS : 2;
}

struct ChildResult {
    int exitCode;
    QByteArray diagnostic;
};

ChildResult runChild( const QString& mode )
{
    QProcess child;
    child.start( QCoreApplication::applicationFilePath(), { mode } );
    const auto finished = child.waitForFinished( 30000 );
    if ( !finished ) {
        child.kill();
        child.waitForFinished();
    }
    const auto diagnostic = child.readAllStandardError();
    INFO( diagnostic.constData() );
    REQUIRE( finished );
    return { child.exitCode(), diagnostic };
}
} // namespace

TEST_CASE( "Process scheduler shutdown completes worker allocator TLS destruction" )
{
    auto state = std::make_shared<WorkerState>();
    {
        klogg::TaskSchedulerLifetime scheduler;
        REQUIRE( runOnWorker( state ) );
        // Task completion is not thread completion. The process owner keeps
        // the worker alive until the final join below.
        REQUIRE( state->destroyed.load() == 0 );
    }
    CHECK( state->destroyed.load() == 1 );
}

TEST_CASE( "Process scheduler shutdown also supports a process without tasks" )
{
    klogg::TaskSchedulerLifetime scheduler;
}

TEST_CASE( "Process scheduler shutdown reports an unjoinable scheduler" )
{
    const auto child = runChild( QStringLiteral( "--retained-scheduler-handle" ) );
    CHECK( child.exitCode != EXIT_SUCCESS );
    CHECK( child.diagnostic.contains( "Cannot join the process task scheduler" ) );
}

TEST_CASE( "Process scheduler shutdown rejects an unjoined external producer" )
{
    const auto child = runChild( QStringLiteral( "--active-scheduler-producer" ) );
    CHECK( child.exitCode != EXIT_SUCCESS );
    CHECK( child.diagnostic.contains( "Cannot join the process task scheduler" ) );
}

TEST_CASE( "Application teardown follows native producer joins before scheduler finalization" )
{
    const auto child = runChild( QStringLiteral( "--application-lifetime" ) );
    INFO( child.diagnostic.constData() );
    CHECK( child.exitCode == EXIT_SUCCESS );
}

TEST_CASE( "Private preparations release scheduler ownership before the dispatcher exits", "[private-producer]" )
{
    const auto mode = GENERATE( QStringLiteral( "--private-index-caller" ), QStringLiteral( "--private-search-caller" ) );
    CAPTURE( mode.toStdString() );
    const auto child = runChild( mode );
    INFO( child.diagnostic.constData() );
    REQUIRE( child.exitCode == EXIT_SUCCESS );
}

int main( int argc, char* argv[] )
{
    if ( argc == 2 && std::string{ argv[1] } == "--application-lifetime" ) {
        return applicationLifetimeChild( argc, argv );
    }
    QCoreApplication app( argc, argv );
    if ( argc == 2 && std::string{ argv[1] } == "--retained-scheduler-handle" ) {
        return retainedHandleChild();
    }
    if ( argc == 2 && std::string{ argv[1] } == "--active-scheduler-producer" ) {
        return activeProducerChild();
    }
    if ( argc == 2 && std::string{ argv[1] } == "--private-index-caller" ) return privatePreparationChild( false );
    if ( argc == 2 && std::string{ argv[1] } == "--private-search-caller" ) return privatePreparationChild( true );
    return Catch::Session().run( argc, argv );
}
