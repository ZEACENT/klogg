#include <catch2/catch.hpp>
#include <future>

#include <QFile>
#include <QTemporaryDir>

#include "preparedfilesource.h"
#include "logdata.h"
#include "test_utils.h"

namespace {
struct PreparedSourceFixture {
    QTemporaryDir root;
    QString path;
    IndexingSettings settings = IndexingSettings::capture();
    AtomicFlag cancelled;
    PreparedSourceFixture()
    {
        REQUIRE( root.isValid() );
        path = root.filePath( "input.log" );
        QFile file{ path };
        REQUIRE( file.open( QIODevice::WriteOnly ) );
        REQUIRE( file.write( "first\nsecond\n" ) == 13 );
    }
    FileSourcePreparation prepare( std::optional<LineLength> maximum = std::nullopt )
    {
        return std::async( std::launch::async, [this, maximum] {
            return prepareFileSource( path, settings, cancelled, nullptr, maximum );
        } ).get();
    }
};
}

TEST_CASE( "Private source preparation completes without GUI event processing",
           "[prepared-file-source]" )
{
    PreparedSourceFixture fixture;
    auto prepared = fixture.prepare();
    REQUIRE( prepared.status == FileSourcePreparation::Status::Ready );
    REQUIRE( prepared.source );
    CHECK( prepared.source->path() == fixture.path );
    CHECK( prepared.source->metadata().size == 13 );
    CHECK( prepared.source->metadata().modified.isValid() );
    const auto snapshot = prepared.source->content();
    REQUIRE( snapshot );
    CHECK( snapshot->lineCount() == 2_lcount );
    CHECK( snapshot->readBytes( 0, 13 ) == QByteArrayLiteral( "first\nsecond\n" ) );
    // Discarding an unpublished result on another worker needs no deleteLater.
    std::async( std::launch::async, [source = std::move( prepared.source )]() mutable {
        source.reset();
    } ).get();
    CHECK( snapshot->readBytes( 0, 6 ) == QByteArrayLiteral( "first\n" ) );
}

TEST_CASE( "Private source preparation never converts failure into an empty ready source",
           "[prepared-file-source]" )
{
    using Status = FileSourcePreparation::Status;
    PreparedSourceFixture fixture;
    Status expected = Status::InvalidInput;
    std::optional<LineLength> maximum;
    SECTION( "missing" ) {
        fixture.path = fixture.root.filePath( "absent.log" );
        expected = Status::Missing;
    }
    SECTION( "cancelled before acquisition" ) {
        fixture.cancelled.set();
        fixture.path = fixture.root.filePath( "absent.log" );
        expected = Status::Cancelled;
    }
    SECTION( "relative path" ) { fixture.path = "relative.log"; }
    SECTION( "line limit" ) { maximum = 3_length; expected = Status::ResourceLimit; }
    const auto prepared = fixture.prepare( maximum );
    CHECK( prepared.status == expected );
    CHECK_FALSE( prepared.source );
}

TEST_CASE( "A GUI source adopts the complete prepared index without reopening its pathname",
           "[prepared-file-source][prepared-file-model]" )
{
    PreparedSourceFixture fixture;
    auto prepared = fixture.prepare();
    REQUIRE( prepared.source );
    const auto original = prepared.source->content();
    const auto modified = prepared.source->metadata().modified;
    REQUIRE( QFile::rename( fixture.path, fixture.root.filePath( "retained.log" ) ) );
    {
        QFile replacement{ fixture.path };
        REQUIRE( replacement.open( QIODevice::WriteOnly ) );
        const QByteArray bytes{ "replacement\nnext\nlast\n" };
        REQUIRE( replacement.write( bytes ) == bytes.size() );
    }
    auto model = LogData::fromPrepared( prepared.source );
    REQUIRE( model );
    CHECK_FALSE( prepared.source );
    CHECK( model->indexedContentSnapshot() == original );
    CHECK( model->getNbLine() == 2_lcount );
    CHECK( model->getLineString( 0_lnum ) == QStringLiteral( "first" ) );
    CHECK( model->getLastModifiedDate() == modified );
    CHECK_THROWS_AS( model->attachFile( fixture.path ), CantReattachErr );
    CHECK_FALSE( LogData::fromPrepared( prepared.source ) );
    REQUIRE( model->completePreparedLoading() );
    REQUIRE( model->activatePrepared() );

    // Native future work must update the very same index the model reads.
    SafeQSignalSpy loaded{ model.get(), &LogData::loadingFinished };
    model->reload();
    REQUIRE( loaded.safeWait( 10000 ) );
    CHECK( model->getNbLine() == 3_lcount );
    CHECK( model->getLineString( 0_lnum ) == QStringLiteral( "replacement" ) );
    CHECK( original->readBytes( 0, 6 ) == QByteArrayLiteral( "first\n" ) );
}

TEST_CASE( "An acquired zero-byte source is ready, unlike a directory",
           "[prepared-file-source]" )
{
    PreparedSourceFixture fixture;
    SECTION( "zero bytes" ) {
        QFile file{ fixture.path };
        REQUIRE( file.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
        file.close();
        const auto prepared = fixture.prepare();
        REQUIRE( prepared.status == FileSourcePreparation::Status::Ready );
        REQUIRE( prepared.source );
        CHECK( prepared.source->metadata().size == 0 );
        REQUIRE( prepared.source->content() );
        CHECK( prepared.source->content()->lineCount() == 0_lcount );
    }
    SECTION( "directory" ) {
        fixture.path = fixture.root.path();
        const auto prepared = fixture.prepare();
        CHECK( ( prepared.status == FileSourcePreparation::Status::NotRegular
                 || prepared.status == FileSourcePreparation::Status::OpenFailure ) );
        CHECK_FALSE( prepared.source );
    }
}
