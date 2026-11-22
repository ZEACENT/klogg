#include <catch2/catch.hpp>

#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>

#include "platform/platform_files.h"
#include "test_utils.h"

using klogg::platform::ReadOnlyFileOpenStatus;

TEST_CASE( "Strict file acquisition distinguishes empty regular files from unavailable sources",
           "[platform-file-preparation]" )
{
    QTemporaryDir root;
    REQUIRE( root.isValid() );
    const auto path = root.filePath( "source with spaces.log" );
    QFile source{ path };
    const auto missing = klogg::platform::openRegularFileReadOnly( source );
    CHECK( missing.status == ReadOnlyFileOpenStatus::Missing );
    CHECK_FALSE( missing.error.isEmpty() );
    CHECK_FALSE( missing.metadata );
    CHECK_FALSE( source.isOpen() );
    CHECK_FALSE( QFileInfo::exists( path ) );

    const auto bytes = GENERATE( QByteArray{}, QByteArrayLiteral( "one\ntwo\n" ) );
    QFile writer{ path };
    REQUIRE( writer.open( QIODevice::WriteOnly ) );
    REQUIRE( writer.write( bytes ) == bytes.size() );
    writer.close();
    const auto opened = klogg::platform::openRegularFileReadOnly( source );
    REQUIRE( opened.status == ReadOnlyFileOpenStatus::Opened );
    REQUIRE( opened.metadata );
    CHECK( opened.error.isEmpty() );
    CHECK( opened.metadata->size == bytes.size() );
    CHECK( opened.metadata->modified == QFileInfo( path ).lastModified() );
    const auto identity = klogg::platform::fileIdentity( source );
    REQUIRE( identity );
    CHECK( opened.metadata->identity == *identity );
    CHECK( source.readAll() == bytes );
    const auto descriptor = source.handle();
    const auto repeated = klogg::platform::openRegularFileReadOnly( source );
    CHECK( repeated.status == ReadOnlyFileOpenStatus::Failed );
    CHECK_FALSE( repeated.metadata );
    CHECK( source.handle() == descriptor );
    CHECK( source.isOpen() );
}

TEST_CASE( "Strict acquired source follows its handle rather than a replaced pathname",
           "[platform-file-preparation]" )
{
    QTemporaryDir root;
    REQUIRE( root.isValid() );
    const auto path = root.filePath( "current.log" );
    QFile writer{ path };
    REQUIRE( writer.open( QIODevice::WriteOnly ) );
    REQUIRE( writer.write( "original\n" ) == 9 );
    writer.close();
    QFile source{ path };
    const auto opened = klogg::platform::openRegularFileReadOnly( source );
    REQUIRE( opened.status == ReadOnlyFileOpenStatus::Opened );
    REQUIRE( opened.metadata );
    REQUIRE( QFile::rename( path, root.filePath( "retained.log" ) ) );
    REQUIRE( writer.open( QIODevice::WriteOnly ) );
    REQUIRE( writer.write( "replacement\n" ) == 12 );
    writer.close();
    CHECK( source.readAll() == QByteArrayLiteral( "original\n" ) );
    const auto retainedIdentity = klogg::platform::fileIdentity( source );
    const auto replacementIdentity = klogg::platform::fileIdentity( path );
    REQUIRE( retainedIdentity );
    REQUIRE( replacementIdentity );
    CHECK( opened.metadata->identity == *retainedIdentity );
    CHECK( opened.metadata->identity != *replacementIdentity );
    CHECK( opened.metadata->size == 9 );
}

TEST_CASE( "Strict file acquisition rejects directories and invalid input without ownership",
           "[platform-file-preparation]" )
{
    QTemporaryDir root;
    REQUIRE( root.isValid() );
    QFile directory{ root.path() };
    const auto unsupported = klogg::platform::openRegularFileReadOnly( directory );
    // Some kernels refuse directory acquisition before handle-based validation.
    CHECK( ( unsupported.status == ReadOnlyFileOpenStatus::NotRegular
             || unsupported.status == ReadOnlyFileOpenStatus::Failed ) );
    CHECK_FALSE( unsupported.metadata );
    CHECK_FALSE( unsupported.error.isEmpty() );
    CHECK_FALSE( directory.isOpen() );
    QFile unnamed;
    const auto invalid = klogg::platform::openRegularFileReadOnly( unnamed );
    CHECK( invalid.status == ReadOnlyFileOpenStatus::Failed );
    CHECK_FALSE( invalid.metadata );
    CHECK_FALSE( unnamed.isOpen() );
}

TEST_CASE( "Strict acquisition rejects a special source without waiting for a peer",
           "[platform-file-preparation]" )
{
    QTemporaryDir root;
    REQUIRE( root.isValid() );
    QProcess helper;
    SafeQSignalSpy finished{ &helper, qOverload<int, QProcess::ExitStatus>( &QProcess::finished ) };
    helper.start( QString::fromUtf8( KLOGG_REGULAR_FILE_HELPER_PATH ),
                  { root.filePath( "special-source" ) } );
    REQUIRE( helper.waitForStarted( 10000 ) );
    const bool completed = finished.safeWait( 10000 );
    if ( !completed ) {
        helper.kill();
        helper.waitForFinished( 10000 );
    }
    INFO( helper.readAllStandardError().constData() );
    REQUIRE( completed );
    CHECK( helper.exitStatus() == QProcess::NormalExit );
    CHECK( helper.exitCode() == 0 );
}
