#include <catch2/catch.hpp>
#include <future>

#include <QCoreApplication>
#include <QEvent>
#include <QFile>
#include <QTemporaryDir>

#include "filewatcher.h"
#include "logdata.h"
#include "preparedfilesource.h"
#include "test_utils.h"

namespace {
struct PreparedLifecycleFixture {
    QTemporaryDir root;
    QString path;
    std::unique_ptr<LogData> model;
    PreparedLifecycleFixture()
    {
        REQUIRE( root.isValid() );
        path = root.filePath( "input.log" );
        QFile writer{ path };
        REQUIRE( writer.open( QIODevice::WriteOnly ) );
        REQUIRE( writer.write( "original\n" ) == 9 );
        writer.close();
        const auto settings = IndexingSettings::capture();
        AtomicFlag cancelled;
        auto result = std::async( std::launch::async, [&] {
            return prepareFileSource( path, settings, cancelled );
        } ).get();
        REQUIRE( result.source );
        model = LogData::fromPrepared( result.source );
        REQUIRE( model );
    }
};
}

TEST_CASE( "Unpublished source models ignore global watcher traffic",
           "[prepared-file-source][prepared-source-lifecycle]" )
{
    PreparedLifecycleFixture fixture;
    auto& watcher = FileWatcher::getFileWatcher();
    SafeQSignalSpy loaded{ fixture.model.get(), &LogData::loadingFinished };
    QFile append{ fixture.path };
    REQUIRE( append.open( QIODevice::WriteOnly | QIODevice::Append ) );
    REQUIRE( append.write( "tail\n" ) == 5 );
    append.close();
    // Deterministically deliver the same global signal another open owner of
    // this path would generate. Do not depend on filesystem watcher timing.
    SECTION( "global watcher delivery" ) {
        watcher.fileChanged( fixture.path );
        QCoreApplication::sendPostedEvents( fixture.model.get(), QEvent::MetaCall );
    }
    SECTION( "explicit reload while private" ) { fixture.model->reload(); }
    const auto retired = fixture.model->retire();
    CHECK( retired.isFinished() ); // No hidden native check/index job was started.
    CHECK( fixture.model->getNbLine() == 1_lcount );
    CHECK( loaded.count() == 0 );
    REQUIRE( waitUiState( [&] { return retired.isFinished(); } ) );
}

TEST_CASE( "Prepared source presentation and native watch activation are distinct one-shot steps",
           "[prepared-file-source][prepared-source-lifecycle]" )
{
    PreparedLifecycleFixture fixture;
    auto& watcher = FileWatcher::getFileWatcher();
    REQUIRE( watcher.waitForIdleForTest( 10000 ) );
    const auto before = watcher.watchedFileCountForTest();
    SafeQSignalSpy loaded{ fixture.model.get(), &LogData::loadingFinished };
    CHECK_FALSE( fixture.model->activatePrepared() );
    REQUIRE( fixture.model->completePreparedLoading() );
    CHECK( loaded.count() == 1 );
    CHECK_FALSE( fixture.model->completePreparedLoading() );
    REQUIRE( watcher.waitForIdleForTest( 10000 ) );
    CHECK( watcher.watchedFileCountForTest() == before );
    REQUIRE( fixture.model->activatePrepared() );
    CHECK_FALSE( fixture.model->activatePrepared() );
    REQUIRE( watcher.waitForIdleForTest( 10000 ) );
    CHECK( watcher.watchedFileCountForTest() == before + 1 );
    CHECK( loaded.count() == 1 );
    fixture.model.reset();
    REQUIRE( watcher.waitForIdleForTest( 10000 ) );
    CHECK( watcher.watchedFileCountForTest() == before );
}

TEST_CASE( "Prepared activation reconciles changes made before watcher registration",
           "[prepared-file-source][prepared-source-lifecycle][prepared-activation-catchup]" )
{
    PreparedLifecycleFixture fixture;
    REQUIRE( fixture.model->completePreparedLoading() );
    QFile append{ fixture.path };
    REQUIRE( append.open( QIODevice::WriteOnly | QIODevice::Append ) );
    REQUIRE( append.write( "tail\n" ) == 5 );
    append.close();
    SafeQSignalSpy updated{ fixture.model.get(), &LogData::loadingFinished };
    REQUIRE( fixture.model->activatePrepared() );
    SECTION( "a native check is queued before the next GUI event turn" ) {
        const auto retired = fixture.model->retire();
        CHECK_FALSE( retired.isFinished() );
        REQUIRE( waitUiState( [&] { return retired.isFinished(); } ) );
    }
    SECTION( "the prepared-to-active gap does not lose appended content" ) {
        REQUIRE( updated.safeWait( 10000 ) );
        CHECK( fixture.model->getNbLine() == 2_lcount );
        CHECK( fixture.model->getLineString( 1_lnum ) == QStringLiteral( "tail" ) );
    }
}

TEST_CASE( "Prepared loading observers may destroy the unpublished model",
           "[prepared-file-source][prepared-source-lifecycle]" )
{
    PreparedLifecycleFixture fixture;
    auto* source = fixture.model.get();
    QObject::connect( source, &LogData::loadingFinished, source, [&] { fixture.model.reset(); } );
    CHECK( source->completePreparedLoading() );
    CHECK_FALSE( fixture.model );
}

TEST_CASE( "Retired private source models cannot announce or activate",
           "[prepared-file-source][prepared-source-lifecycle]" )
{
    PreparedLifecycleFixture fixture;
    REQUIRE( fixture.model->retire().isFinished() );
    SafeQSignalSpy loaded{ fixture.model.get(), &LogData::loadingFinished };
    CHECK_FALSE( fixture.model->completePreparedLoading() );
    CHECK_FALSE( fixture.model->activatePrepared() );
    CHECK( loaded.count() == 0 );
}
