#include <catch2/catch.hpp>

#include <atomic>
#include <memory>
#include <variant>
#include <QFile>
#include <QTemporaryFile>

#include "logdataworker.h"

TEST_CASE( "Index change-detection hashes describe retained bytes rather than a later source rewrite",
           "[indexedcontent][index-hash-snapshot]" )
{
    QTemporaryFile file;
    REQUIRE( file.open() );
    REQUIRE( file.write( "first\n" ) == 6 );
    REQUIRE( file.flush() );
    const auto path = file.fileName();
    file.close();
    auto data = std::make_shared<IndexingData>();
    AtomicFlag cancelled;
    FullIndexOperation operation{ path, data, cancelled };
    std::atomic_bool attempted{ false };
    std::atomic_bool rewritten{ false };
    // Direct callback: complete the rewrite after the parser retains its last
    // block, but before the coordinator finishes the indexing operation.
    QObject::connect( &operation, &IndexOperation::indexingProgressed, [&]( int progress ) {
        if ( progress != 100 || attempted.exchange( true ) ) return;
        QFile changed{ path };
        if ( changed.open( QIODevice::WriteOnly ) ) {
            rewritten.store( changed.write( "other\n" ) == 6 && changed.flush() );
        }
    } );
    REQUIRE( std::get<bool>( operation.run() ) );
    REQUIRE( rewritten.load() );
    IndexingData::ConstAccessor indexed{ data.get() };
    const auto snapshot = indexed.getContentSnapshot();
    REQUIRE( snapshot );
    REQUIRE( snapshot->readBytes( 0, 6 ) == QByteArrayLiteral( "first\n" ) );
    FileDigest expected;
    expected.addData( "first\n", 6 );
    CHECK( indexed.getHash().headerDigest == expected.digest() );
    CHECK( indexed.getHash().tailDigest == expected.digest() );
}
