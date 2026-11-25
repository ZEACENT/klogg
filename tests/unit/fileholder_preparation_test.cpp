#include <catch2/catch.hpp>
#include <future>
#include <memory>

#include <QFile>
#include <QTemporaryDir>
#include <QThread>

#include "fileholder.h"
#include "platform/platform_files.h"

namespace {
struct FileHolderFixture {
    QTemporaryDir root;
    QString path;
    FileHolderFixture()
    {
        REQUIRE( root.isValid() );
        path = root.filePath( "source.log" );
        write( path, "original\n" );
    }
    static void write( const QString& path, const QByteArray& bytes )
    {
        QFile file{ path };
        REQUIRE( file.open( QIODevice::WriteOnly ) );
        REQUIRE( file.write( bytes ) == bytes.size() );
    }
    std::unique_ptr<QFile> acquire()
    {
        auto file = std::make_unique<QFile>( path );
        REQUIRE( klogg::platform::openRegularFileReadOnly( *file ).metadata );
        return file;
    }
};
}

TEST_CASE( "A prepared holder retains the acquired object until its reader is released",
           "[fileholder-preparation]" )
{
    FileHolderFixture fixture;
    const bool keepClosed = GENERATE( false, true );
    auto source = fixture.acquire();
    const auto identity = klogg::platform::fileIdentity( *source );
    REQUIRE( identity );
    const FileId originalId{ identity->file, identity->device };
    const auto retainedPath = fixture.root.filePath( "retained.log" );
    REQUIRE( QFile::rename( fixture.path, retainedPath ) );
    FileHolderFixture::write( fixture.path, "replacement is longer\n" );

    FileHolder holder{ keepClosed };
    REQUIRE( holder.adoptForReader( fixture.path, source, originalId ) );
    CHECK_FALSE( source );
    CHECK( holder.isOpen() );
    CHECK_FALSE( holder.getFileId() != originalId );
    CHECK( holder.size() == 9 );
    {
        QFile append{ retainedPath };
        REQUIRE( append.open( QIODevice::WriteOnly | QIODevice::Append ) );
        REQUIRE( append.write( "tail\n" ) == 5 );
    }
    CHECK( holder.size() == 14 );
    holder.attachReader();
    holder.detachReader();
    CHECK( holder.isOpen() );
    CHECK_FALSE( holder.getFileId() != originalId );
    holder.detachReader();
    CHECK( holder.isOpen() == !keepClosed );

    // Subsequent native readers still follow the pathname under keep-closed.
    holder.attachReader();
    CHECK( holder.isOpen() );
    CHECK( holder.size() == ( keepClosed ? 22 : 14 ) );
    CHECK( ( holder.getFileId() != originalId ) == keepClosed );
    holder.detachReader();
}

TEST_CASE( "Prepared holder rejection leaves ownership and existing readers unchanged",
           "[fileholder-preparation]" )
{
    FileHolderFixture fixture;
    FileHolder holder{ true };
    auto source = fixture.acquire();
    const auto* original = source.get();
    SECTION( "closed input" ) { source->close(); }
    SECTION( "a competing Qt parent" ) { source->setParent( QThread::currentThread() ); }
    SECTION( "an already configured holder" ) { holder.open( fixture.path ); }
    SECTION( "no input" ) { source.reset(); original = nullptr; }
    CHECK_FALSE( holder.adoptForReader( fixture.path, source, FileId{} ) );
    CHECK( source.get() == original );
    CHECK_FALSE( holder.isOpen() );
    if ( source ) source->setParent( nullptr );
}

TEST_CASE( "A detached worker acquisition can transfer to the holder owner thread",
           "[fileholder-preparation]" )
{
    FileHolderFixture fixture;
    auto acquisition = std::async( std::launch::async, [path = fixture.path] {
        auto source = std::make_unique<QFile>( path );
        const auto result = klogg::platform::openRegularFileReadOnly( *source );
        if ( !result.metadata ) return std::unique_ptr<QFile>{};
        source->moveToThread( nullptr );
        return source;
    } );
    auto source = acquisition.get();
    REQUIRE( source );
    REQUIRE( source->thread() == nullptr );
    auto* observed = source.get();
    FileHolder holder{ true };
    REQUIRE( holder.adoptForReader( fixture.path, source, FileId{} ) );
    CHECK( observed->thread() == QThread::currentThread() );
    CHECK( holder.size() == 9 );
    holder.detachReader();
    CHECK_FALSE( holder.isOpen() );
}
