#include <catch2/catch.hpp>

#include <QFile>
#include <QObject>
#include <QTemporaryDir>
#include <memory>

#include "logdata.h"
#include "logdataoperation.h"
#include "logdataworker.h"
#include "test_utils.h"

TEST_CASE( "Retiring an indexing queue never joins a blocked worker or starts pending work",
           "[source-retirement]" )
{
    QTemporaryDir root;
    REQUIRE( root.isValid() );
    const auto path = root.filePath( "retire.log" );
    QFile file{ path };
    REQUIRE( file.open( QIODevice::WriteOnly ) );
    REQUIRE( file.write( "one\ntwo\n" ) == 8 );
    file.close();

    auto data = std::make_shared<IndexingData>();
    OperationQueue queue{ [] {} };
    auto worker = std::make_unique<LogDataWorker>( data );
    QObject observer;
    int completions = 0;
    bool retiredAfterFirst = false;
    LoadingStatus firstStatus = LoadingStatus::Successful;
    QObject::connect( worker.get(), &LogDataWorker::indexingFinished, &observer,
                      [&]( LoadingStatus status ) {
                          queue.waitForWorkerDone();
                          queue.finishOperationAndStartNext();
                          if ( completions++ == 0 ) {
                              firstStatus = status;
                              retiredAfterFirst = queue.beginRetirement();
                          }
                      }, Qt::QueuedConnection );
    queue.setWorker( std::move( worker ) );
    {
        // The real worker must acquire this lock before completing its index.
        // Retirement must return while this scope still holds the lock.
        IndexingData::MutateAccessor blockedIndex{ data.get() };
        queue.enqueueOperation<AttachOperation>( path );
        queue.enqueueOperation<FullReindexOperation>();
        CHECK_FALSE( queue.beginRetirement() );
        queue.enqueueOperation<FullReindexOperation>(); // Admission must stay closed.
        CHECK( completions == 0 );
    }
    REQUIRE( waitUiState( [&] { return completions > 0; } ) );
    CHECK( firstStatus == LoadingStatus::Interrupted );
    CHECK( retiredAfterFirst );
    CHECK( queue.beginRetirement() );
    queue.shutdown();
}

TEST_CASE( "Source retirement completes after owner cleanup and suppresses late publication",
           "[source-retirement]" )
{
    QTemporaryDir root;
    REQUIRE( root.isValid() );
    const auto path = root.filePath( "source.log" );
    QFile file{ path };
    REQUIRE( file.open( QIODevice::WriteOnly ) );
    REQUIRE( file.write( "one\ntwo\n" ) == 8 );
    file.close();
    LogData source;
    SafeQSignalSpy loaded{ &source, &LogData::loadingFinished };
    SafeQSignalSpy progress{ &source, &LogData::loadingProgressed };
    SafeQSignalSpy changed{ &source, &LogData::fileChanged };
    source.attachFile( path );
    SECTION( "initial attachment" ) {}
    SECTION( "queued reload" ) { source.reload(); }
    SECTION( "file check" ) {
        REQUIRE( loaded.safeWait( 10000 ) );
        REQUIRE( QMetaObject::invokeMethod( &source, "fileChangedOnDisk",
                                            Qt::DirectConnection, Q_ARG( QString, path ) ) );
    }
    const auto loadCount = loaded.count();
    const auto progressCount = progress.count();
    const auto changeCount = changed.count();
    // No event processing since starting the operation: owner cleanup is pending
    // even if the worker has already finished its tiny input.
    const auto retired = source.retire();
    CHECK_FALSE( retired.isFinished() );
    source.reload();
    REQUIRE( QMetaObject::invokeMethod( &source, "fileChangedOnDisk",
                                        Qt::DirectConnection, Q_ARG( QString, path ) ) );
    REQUIRE( waitUiState( [&] { return retired.isFinished(); } ) );
    CHECK( source.retire().isFinished() );
    CHECK( loaded.count() == loadCount );
    CHECK( progress.count() == progressCount );
    CHECK( changed.count() == changeCount );
}

TEST_CASE( "Retirement requested by a loading observer completes after queue cleanup",
           "[source-retirement]" )
{
    QTemporaryDir root;
    REQUIRE( root.isValid() );
    const auto path = root.filePath( "observer.log" );
    QFile file{ path };
    REQUIRE( file.open( QIODevice::WriteOnly ) );
    file.close();
    LogData source;
    QFuture<void> retired;
    bool observed = false;
    bool finishedInsideObserver = true;
    QObject::connect( &source, &LogData::loadingFinished, &source, [&] {
        retired = source.retire();
        finishedInsideObserver = retired.isFinished();
        observed = true;
    } );
    source.attachFile( path );
    REQUIRE( waitUiState( [&] { return observed && retired.isFinished(); } ) );
    CHECK_FALSE( finishedInsideObserver );
}

TEST_CASE( "Unattached source retirement is immediate and terminal", "[source-retirement]" )
{
    LogData source;
    CHECK( source.retire().isFinished() );
    CHECK( source.retire().isFinished() );
}

TEST_CASE( "An idle indexing queue retires immediately and refuses new work",
           "[source-retirement]" )
{
    int starts = 0;
    OperationQueue queue{ [&] { ++starts; } };
    queue.setWorker( std::make_unique<LogDataWorker>( std::make_shared<IndexingData>() ) );
    CHECK( queue.beginRetirement() );
    queue.enqueueOperation<FullReindexOperation>();
    CHECK( starts == 0 );
    CHECK( queue.beginRetirement() );
    queue.shutdown();
}
