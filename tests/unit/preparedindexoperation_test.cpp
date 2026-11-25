#include <catch2/catch.hpp>
#include <memory>

#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "logdataworker.h"
#include "platform/platform_files.h"

namespace {
struct PreparedIndexFixture {
    QTemporaryDir root;
    QString path;
    std::shared_ptr<IndexingData> data = std::make_shared<IndexingData>();
    AtomicFlag cancelled;
    PreparedIndexFixture()
    {
        REQUIRE( root.isValid() );
        path = root.filePath( "input.log" );
        QFile writer{ path };
        REQUIRE( writer.open( QIODevice::WriteOnly ) );
        REQUIRE( writer.write( "first\n" ) == 6 );
    }
};

class FailHeaderSeek final : public QFile {
public:
    using QFile::QFile;
    bool seek( qint64 position ) override
    {
        if ( position == 0 && ++zeroSeeks_ == 2 ) {
            failed = true;
            return false;
        }
        return QFile::seek( position );
    }
    bool failed = false;
private:
    int zeroSeeks_ = 0;
};
}

TEST_CASE( "Prepared native indexing reads the acquired object and only its captured extent",
           "[indexing-preparation][prepared-index]" )
{
    PreparedIndexFixture fixture;
    QFile source{ fixture.path };
    const auto acquired = klogg::platform::openRegularFileReadOnly( source );
    REQUIRE( acquired.metadata );
    SECTION( "a replacement path cannot substitute another object" ) {
        REQUIRE( QFile::rename( fixture.path, fixture.root.filePath( "retained.log" ) ) );
        QFile replacement{ fixture.path };
        REQUIRE( replacement.open( QIODevice::WriteOnly ) );
        REQUIRE( replacement.write( "replacement\n" ) == 12 );
    }
    SECTION( "later append remains outside the finite preparation" ) {
        QFile append{ fixture.path };
        REQUIRE( append.open( QIODevice::WriteOnly | QIODevice::Append ) );
        REQUIRE( append.write( "later\n" ) == 6 );
    }
    FullIndexOperation operation{ fixture.path, fixture.data, fixture.cancelled };
    QSignalSpy legacyFinished{ &operation, &IndexOperation::indexingFinished };
    const auto result = operation.prepare( source, acquired.metadata->size );
    REQUIRE( result.status == PreparedIndexResult::Status::Ready );
    CHECK( legacyFinished.count() == 0 );
    IndexingData::ConstAccessor indexed{ fixture.data.get() };
    const auto snapshot = indexed.getContentSnapshot();
    REQUIRE( snapshot );
    CHECK( snapshot->byteSize() == 6 );
    CHECK( snapshot->lineCount() == 1_lcount );
    CHECK( snapshot->readBytes( 0, 6 ) == QByteArrayLiteral( "first\n" ) );
    CHECK( indexed.getHash().headerSize <= 6 );
    CHECK( indexed.getHash().tailOffset + indexed.getHash().tailSize <= 6 );
}

TEST_CASE( "Prepared native indexing treats empty input as ready and cancellation as terminal",
           "[indexing-preparation][prepared-index]" )
{
    PreparedIndexFixture fixture;
    QFile source{ fixture.path };
    REQUIRE( klogg::platform::openRegularFileReadOnly( source ).metadata );
    const bool cancel = GENERATE( false, true );
    if ( cancel ) fixture.cancelled.set();
    FullIndexOperation operation{ fixture.path, fixture.data, fixture.cancelled };
    const auto result = operation.prepare( source, 0 );
    CHECK( result.status == ( cancel ? PreparedIndexResult::Status::Cancelled
                                    : PreparedIndexResult::Status::Ready ) );
    IndexingData::ConstAccessor indexed{ fixture.data.get() };
    CHECK( indexed.getNbLines() == 0_lcount );
    if ( !cancel ) {
        REQUIRE( indexed.getContentSnapshot() );
        CHECK( indexed.getContentSnapshot()->byteSize() == 0 );
    }
}

TEST_CASE( "Prepared index hashes do not rewind and reread a mutable original source",
           "[indexing-preparation][prepared-index]" )
{
    PreparedIndexFixture fixture;
    FailHeaderSeek source{ fixture.path };
    REQUIRE( klogg::platform::openRegularFileReadOnly( source ).metadata );
    FullIndexOperation operation{ fixture.path, fixture.data, fixture.cancelled };
    const auto result = operation.prepare( source, 6 );
    CHECK( result.status == PreparedIndexResult::Status::Ready );
    CHECK_FALSE( source.failed );
}

TEST_CASE( "Prepared native indexing rejects read and resource failures without a partial prefix",
           "[indexing-preparation][prepared-index]" )
{
    PreparedIndexFixture fixture;
    FullIndexOperation operation{ fixture.path, fixture.data, fixture.cancelled };
    QSignalSpy legacyFinished{ &operation, &IndexOperation::indexingFinished };
    SECTION( "captured extent is no longer readable" ) {
        QFile source{ fixture.path };
        const auto acquired = klogg::platform::openRegularFileReadOnly( source );
        REQUIRE( acquired.metadata );
        QFile truncated{ fixture.path };
        REQUIRE( truncated.open( QIODevice::ReadWrite ) );
        REQUIRE( truncated.resize( 2 ) );
        truncated.close();
        const auto result = operation.prepare( source, acquired.metadata->size );
        CHECK( result.status == PreparedIndexResult::Status::ReadFailure );
    }
    SECTION( "line limit is enforced by the native parser" ) {
        QFile source{ fixture.path };
        REQUIRE( klogg::platform::openRegularFileReadOnly( source ).metadata );
        const auto result = operation.prepare( source, 6, 4_length );
        CHECK( result.status == PreparedIndexResult::Status::ResourceLimit );
    }
    SECTION( "closed input is not mistaken for an empty success" ) {
        QFile source{ fixture.path };
        const auto result = operation.prepare( source, 0 );
        CHECK( result.status == PreparedIndexResult::Status::InvalidInput );
    }
    CHECK( legacyFinished.count() == 0 );
    IndexingData::ConstAccessor indexed{ fixture.data.get() };
    CHECK( indexed.getNbLines() == 0_lcount );
    CHECK_FALSE( indexed.getContentSnapshot() );
}
