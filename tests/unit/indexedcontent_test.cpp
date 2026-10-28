#include <catch2/catch.hpp>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include "indexedcontent.h"
#include "logdataworker.h"
#include "logdata.h"
#include "test_utils.h"

namespace {
void writeContent( const QString& path, const QByteArray& bytes,
                   QIODevice::OpenMode mode = QIODevice::WriteOnly | QIODevice::Truncate )
{
    QFile file( path );
    REQUIRE( file.open( mode ) );
    REQUIRE( file.write( bytes ) == bytes.size() );
}

IndexedContentSnapshot::Ptr indexedSnapshot( const std::shared_ptr<IndexingData>& data )
{
    return IndexingData::ConstAccessor{ data.get() }.getContentSnapshot();
}
}

TEST_CASE( "Snapshot decoding rejects corrupt bounds instead of publishing warning text",
           "[indexedcontent][source-snapshot]" )
{
    SearchableLogData::RawLines lines;
    lines.startLine = 0_lnum;
    lines.buffer = { 'a' };
    lines.endOfLines = { 5 };
    lines.textDecoder = TextCodecHolder{ QTextCodec::codecForName( "UTF-8" ) }.makeDecoder();
    REQUIRE_THROWS_AS( lines.decodeLinesStrict(), klogg::SnapshotError );
}

TEST_CASE( "File row snapshots own indexed text and its interpretation", "[indexedcontent][source-snapshot]" )
{
    QTemporaryDir temporary;
    REQUIRE( temporary.isValid() );
    const auto path = temporary.filePath( QStringLiteral( "source.log" ) );
    writeContent( path, "\x1b[31mERROR\x1b[0m\r\nprefix value" );
    klogg::LogSnapshot::Ptr snapshot;
    {
        LogData data;
        SafeQSignalSpy loaded{ &data, &SearchableLogData::loadingFinished };
        data.attachFile( path );
        REQUIRE( loaded.safeWait() );
        data.setDisplayEncoding( "UTF-8" );
        data.setAnsiProcessingMode( AnsiProcessingMode::Strip );
        data.setPrefilter( QStringLiteral( "prefix " ) );
        snapshot = data.createReadSnapshot( klogg::SnapshotScope::PresentedRows );
        REQUIRE( snapshot );
        REQUIRE( snapshot->lineCount() == 2_lcount );
        REQUIRE( snapshot->lineString( 0_lnum ) == QStringLiteral( "ERROR" ) );
        REQUIRE( snapshot->lineString( 1_lnum ) == QStringLiteral( "value" ) );
        REQUIRE( snapshot->row( 0_lnum ).sourceLineNumber == 1_lnum );
        REQUIRE( data.rowForIdentity( snapshot->row( 1_lnum ).identity ) == 1_lnum );
        writeContent( path, "other\nchanged" );
        data.setAnsiProcessingMode( AnsiProcessingMode::Plain );
        data.setPrefilter( {} );
        REQUIRE( snapshot->lineString( 0_lnum ) == QStringLiteral( "ERROR" ) );
        REQUIRE( snapshot->lineString( 1_lnum ) == QStringLiteral( "value" ) );
    }
    REQUIRE( QFile::remove( path ) );
    REQUIRE( snapshot->lineString( 1_lnum ) == QStringLiteral( "value" ) );
}

TEST_CASE( "File presentation reads the indexed version until a replacement is indexed",
           "[indexedcontent][indexed-presentation]" )
{
    QTemporaryDir temporary;
    REQUIRE( temporary.isValid() );
    const auto path = temporary.filePath( QStringLiteral( "source.log" ) );
    writeContent( path, "original\n" );
    LogData data;
    SafeQSignalSpy loaded{ &data, &SearchableLogData::loadingFinished };
    data.attachFile( path );
    REQUIRE( loaded.safeWait() );
    writeContent( path, "replaced\n" );
    REQUIRE( data.getLineString( 0_lnum ) == QStringLiteral( "original" ) );
    REQUIRE( data.getLinesRaw( 0_lnum, 1_lcount ).decodeLinesStrict().front()
             == QStringLiteral( "original" ) );
}

TEST_CASE( "File display and snapshots remove exactly one framing carriage return",
           "[indexedcontent][indexed-line-ending]" )
{
    QTemporaryDir temporary;
    REQUIRE( temporary.isValid() );
    const auto path = temporary.filePath( QStringLiteral( "source.log" ) );
    writeContent( path, "value\r\r\n" );
    LogData data;
    SafeQSignalSpy loaded{ &data, &SearchableLogData::loadingFinished };
    data.attachFile( path );
    REQUIRE( loaded.safeWait() );
    CHECK( data.getLineString( 0_lnum ) == QStringLiteral( "value\r" ) );
    CHECK( data.getExpandedLineString( 0_lnum ) == QStringLiteral( "value\r" ) );
    CHECK( data.searchableSnapshot()->lineString( 0_lnum ) == QStringLiteral( "value\r" ) );
}

TEST_CASE( "File interpretation changes notify readers once outside the index lock",
           "[indexedcontent][file-interpretation]" )
{
    QTemporaryDir temporary;
    REQUIRE( temporary.isValid() );
    const auto path = temporary.filePath( QStringLiteral( "source.log" ) );
    writeContent( path, "prefix value\n" );
    LogData data;
    SafeQSignalSpy loaded{ &data, &SearchableLogData::loadingFinished };
    data.attachFile( path );
    REQUIRE( loaded.safeWait() );
    int notifications = 0;
    QObject::connect( &data, &AbstractLogData::interpretationChanged, &data, [ & ] {
        REQUIRE( data.searchableSnapshot() );
        ++notifications;
    } );
    SECTION( "ANSI" ) {
        data.setAnsiProcessingMode( AnsiProcessingMode::Strip );
        data.setAnsiProcessingMode( AnsiProcessingMode::Strip );
    }
    SECTION( "Prefilter" ) {
        data.setPrefilter( QStringLiteral( "^prefix " ) );
        data.setPrefilter( QStringLiteral( "^prefix " ) );
    }
    REQUIRE( notifications == 1 );
}

TEST_CASE( "Indexed content leases retain exact bytes with bounded resident storage",
           "[indexedcontent]" )
{
    QTemporaryDir temporary;
    REQUIRE( temporary.isValid() );
    IndexedContentBuilder::Options options;
    options.byteMemoryBudget = 4;
    options.boundaryMemoryBudget = 4;
    options.temporaryDirectory = temporary.path();
    IndexedContentSnapshot::Ptr first;
    IndexedContentSnapshot::Ptr second;
    const QByteArray bytes( "a\r\n\0z", 5 );
    {
        IndexedContentBuilder builder( options );
        builder.appendBlock( 0, bytes.constData(), static_cast<size_t>( bytes.size() ),
                             { 3_offset } );
        first = builder.snapshot( "UTF-8", true );
        REQUIRE( first->byteSize() == 5 );
        REQUIRE( first->lineCount() == 2_lcount );
        REQUIRE( first->lineEnd( 1_lnum ) == 6_offset );
        REQUIRE( first->readBytes( 0, 5 ) == bytes );
        REQUIRE( first->residentBytes() <= 8 );
        const auto spillNames = first->spillFileNamesForTesting();
        REQUIRE( spillNames.size() == 2 );
        REQUIRE_FALSE( QDir( temporary.path() ).entryList( QDir::Files ).isEmpty() );

        builder.appendBlock( 5, "\nnext", 5, { 6_offset } );
        second = builder.snapshot( "UTF-8", true );
        REQUIRE( second->version() != first->version() );
        REQUIRE( second->incarnation() == first->incarnation() );
        REQUIRE( second->byteSize() == 10 );
        REQUIRE( second->lineCount() == 3_lcount );
        REQUIRE( second->lineEnd( 1_lnum ) == 6_offset );
        REQUIRE( second->lineEnd( 2_lnum ) == 11_offset );
        REQUIRE( second->readBytes( 0, 10 ) == bytes + "\nnext" );
        REQUIRE( first->readBytes( 0, 5 ) == bytes );
        REQUIRE( first->lineCount() == 2_lcount );
        REQUIRE( first->lineEnds( 0_lnum, 2_lcount )
                 == klogg::vector<OffsetInFile>{ 3_offset, 6_offset } );
        REQUIRE( second->lineEnds( 1_lnum, 2_lcount )
                 == klogg::vector<OffsetInFile>{ 6_offset, 11_offset } );
        REQUIRE( second->lineEnds( 3_lnum, 0_lcount ).empty() );
        REQUIRE_THROWS_AS( second->lineEnds( 2_lnum, 2_lcount ), IndexedContentError );
        REQUIRE( second->spillFileNamesForTesting() == spillNames );
        qint64 spillBytes = 0;
        for ( const auto& spillName : spillNames ) {
            // NTFS directory entries can retain stale sizes while the writer is
            // open. Query an open handle instead of directory enumeration metadata.
            QFile spill( spillName );
            REQUIRE( spill.open( QIODevice::ReadOnly ) );
            spillBytes += spill.size();
        }
        // Ten byte-exact payload bytes plus two eight-byte real boundaries.
        REQUIRE( spillBytes == 26 );
        REQUIRE_THROWS_AS( first->readBytes( 0, 6 ), IndexedContentError );
        REQUIRE_THROWS_AS( first->readBytes( -1, 1 ), IndexedContentError );
        REQUIRE_THROWS_AS( first->lineEnd( 2_lnum ), IndexedContentError );
    }
    first.reset();
    REQUIRE( second->readBytes( 5, 5 ) == "\nnext" );
    second.reset();
    REQUIRE( QDir( temporary.path() ).entryList( QDir::Files ).isEmpty() );
}

TEST_CASE( "Indexed content migrates resident bytes without changing earlier leases",
           "[indexedcontent]" )
{
    QTemporaryDir temporary;
    REQUIRE( temporary.isValid() );
    IndexedContentBuilder::Options options;
    options.byteMemoryBudget = 4;
    options.boundaryMemoryBudget = 8;
    options.temporaryDirectory = temporary.path();
    IndexedContentSnapshot::Ptr first;
    {
        IndexedContentBuilder builder( options );
        builder.appendBlock( 0, "a\n", 2, { 2_offset } );
        first = builder.snapshot( "UTF-8" );
        REQUIRE( first->spillFileNamesForTesting().isEmpty() );
        REQUIRE( QDir( temporary.path() ).entryList( QDir::Files ).isEmpty() );
        REQUIRE( first->residentBytes() <= 12 );
    }
    IndexedContentBuilder resumed( first );
    resumed.appendBlock( 2, "b\nc\n", 4, { 4_offset, 6_offset } );
    const auto second = resumed.snapshot( "UTF-8" );
    REQUIRE( second->readBytes( 0, 6 ) == "a\nb\nc\n" );
    REQUIRE( second->lineEnd( 2_lnum ) == 6_offset );
    REQUIRE( first->readBytes( 0, 2 ) == "a\n" );
    REQUIRE( first->lineCount() == 1_lcount );
    REQUIRE( second->residentBytes() <= 12 );
    IndexedContentBuilder stale( first );
    REQUIRE_THROWS_AS( stale.appendBlock( 2, "x", 1, {} ), IndexedContentError );
    REQUIRE_THROWS_AS( resumed.appendBlock( 5, "x", 1, {} ), IndexedContentError );
    REQUIRE_THROWS_AS( resumed.appendBlock( 6, "x", 1, { 8_offset } ), IndexedContentError );
}

TEST_CASE( "Indexed content spill failures do not publish partial content or leak files",
           "[indexedcontent]" )
{
    QTemporaryDir temporary;
    REQUIRE( temporary.isValid() );
    const auto blocker = temporary.filePath( "not-a-directory" );
    writeContent( blocker, "blocker" );
    IndexedContentBuilder::Options options;
    options.byteMemoryBudget = 0;
    options.temporaryDirectory = blocker;
    IndexedContentBuilder builder( options );
    REQUIRE_THROWS_AS( builder.appendBlock( 0, "a", 1, {} ), IndexedContentError );
    const auto snapshot = builder.snapshot( "UTF-8" );
    REQUIRE( snapshot->byteSize() == 0 );
    REQUIRE( snapshot->readBytes( 0, 0 ).isEmpty() );
    REQUIRE( snapshot->spillFileNamesForTesting().isEmpty() );
    REQUIRE( QDir( temporary.path() ).entryList( QDir::Files ).size() == 1 );
}

TEST_CASE( "Failed spill migration preserves the already published resident prefix",
           "[indexedcontent]" )
{
    QTemporaryDir temporary;
    REQUIRE( temporary.isValid() );
    const auto blocker = temporary.filePath( "not-a-directory" );
    writeContent( blocker, "blocker" );
    IndexedContentBuilder::Options options;
    options.byteMemoryBudget = 4;
    options.temporaryDirectory = blocker;
    IndexedContentBuilder builder( options );
    builder.appendBlock( 0, "old", 3, {} );
    const auto previous = builder.snapshot( "UTF-8", true );
    REQUIRE_THROWS_AS( builder.appendBlock( 3, "xx", 2, {} ), IndexedContentError );
    REQUIRE( previous->readBytes( 0, 3 ) == "old" );
    REQUIRE( previous->lineEnd( 0_lnum ) == 4_offset );
    REQUIRE( builder.snapshot( "UTF-8", true )->byteSize() == 3 );
    REQUIRE( previous->spillFileNamesForTesting().isEmpty() );
    REQUIRE( QDir( temporary.path() ).entryList( QDir::Files ).size() == 1 );
}

TEST_CASE( "A failed spill read is explicit and releases the private file with its last lease",
           "[indexedcontent]" )
{
    QTemporaryDir temporary;
    REQUIRE( temporary.isValid() );
    IndexedContentBuilder::Options options;
    options.byteMemoryBudget = 0;
    options.temporaryDirectory = temporary.path();
    IndexedContentSnapshot::Ptr snapshot;
    {
        IndexedContentBuilder builder( options );
        builder.appendBlock( 0, "retained", 8, {} );
        snapshot = builder.snapshot( "UTF-8", true );
    }
    const auto spillNames = snapshot->spillFileNamesForTesting();
    REQUIRE( spillNames.size() == 1 );
    const auto files = QDir( temporary.path() ).entryList( QDir::Files );
    REQUIRE( files.size() == 1 );
    REQUIRE( QFile::exists( spillNames.front() ) );
    // Deliberately damage private storage, not the original source. A reader
    // must report the failure, never silently reread an unrelated source path.
    writeContent( spillNames.front(), {} );
    REQUIRE_THROWS_AS( snapshot->readBytes( 0, 8 ), IndexedContentError );
    snapshot.reset();
    REQUIRE_FALSE( QFile::exists( spillNames.front() ) );
    REQUIRE( QDir( temporary.path() ).entryList( QDir::Files ).isEmpty() );
}

TEST_CASE( "Indexer retains actual blocks across CRLF boundaries and a spilled unterminated tail",
           "[indexedcontent][indexing]" )
{
    QTemporaryDir temporary;
    REQUIRE( temporary.isValid() );
    const auto path = temporary.filePath( "large-source.log" );
    constexpr int firstBlockSize = 5 * 1024 * 1024;
    constexpr int contentSize = 9 * 1024 * 1024;
    QByteArray bytes( contentSize, 'x' );
    bytes[ firstBlockSize - 1 ] = '\r';
    bytes[ firstBlockSize ] = '\n';
    writeContent( path, bytes );
    auto data = std::make_shared<IndexingData>();
    AtomicFlag interrupted;
    interrupted.clear();
    {
        FullIndexOperation full( path, data, interrupted,
                                 QTextCodec::codecForName( "UTF-8" ) );
        REQUIRE( std::get<bool>( full.run() ) );
    }
    const auto snapshot = indexedSnapshot( data );
    REQUIRE( snapshot );
    REQUIRE( snapshot->byteSize() == contentSize );
    REQUIRE( snapshot->lineCount() == 2_lcount );
    REQUIRE( snapshot->lineEnd( 0_lnum ) == OffsetInFile( firstBlockSize + 1 ) );
    REQUIRE( snapshot->lineEnd( 1_lnum ) == OffsetInFile( contentSize + 1 ) );
    REQUIRE( snapshot->residentBytes() <= 9 * 1024 * 1024 );
    REQUIRE( snapshot->readBytes( firstBlockSize - 4, 8 ) == bytes.mid( firstBlockSize - 4, 8 ) );
    writeContent( path, "replacement" );
    data.reset();
    REQUIRE( snapshot->readBytes( contentSize - 8, 8 ) == "xxxxxxxx" );
    REQUIRE( snapshot->readBytes( contentSize, 0 ).isEmpty() );
    REQUIRE_THROWS_AS( snapshot->readBytes( contentSize, 1 ), IndexedContentError );
}

TEST_CASE( "Indexer snapshots survive rewrites replacement append and clear",
           "[indexedcontent][indexing]" )
{
    QTemporaryDir temporary;
    REQUIRE( temporary.isValid() );
    const auto path = temporary.filePath( "source.log" );
    writeContent( path, "first\r\ntail" );
    auto data = std::make_shared<IndexingData>();
    AtomicFlag interrupted;
    interrupted.clear();
    FullIndexOperation full( path, data, interrupted, QTextCodec::codecForName( "UTF-8" ) );
    REQUIRE( std::get<bool>( full.run() ) );
    const auto first = indexedSnapshot( data );
    REQUIRE( first );
    REQUIRE( first->byteSize() == 11 );
    REQUIRE( first->lineCount() == 2_lcount );
    REQUIRE( first->lineEnd( 1_lnum ) == 12_offset );
    REQUIRE( first->codecName() == "UTF-8" );
    REQUIRE( first->readBytes( 0, 11 ) == "first\r\ntail" );

    SECTION( "same-size rewrite and path replacement never change retained content" )
    {
        writeContent( path, "other\r\ntext" );
        REQUIRE( first->readBytes( 0, 11 ) == "first\r\ntail" );
        REQUIRE( QFile::remove( path ) );
        writeContent( path, "new path" );
        REQUIRE( first->readBytes( 0, 11 ) == "first\r\ntail" );
    }
    SECTION( "append replaces only the new version's synthetic last boundary" )
    {
        writeContent( path, " end\n", QIODevice::WriteOnly | QIODevice::Append );
        PartialIndexOperation partial( path, data, interrupted );
        REQUIRE( std::get<bool>( partial.run() ) );
        const auto second = indexedSnapshot( data );
        REQUIRE( second );
        REQUIRE( second->version() != first->version() );
        REQUIRE( second->incarnation() == first->incarnation() );
        REQUIRE( second->byteSize() == 16 );
        REQUIRE( second->lineCount() == 2_lcount );
        REQUIRE( second->lineEnd( 1_lnum ) == 16_offset );
        REQUIRE( second->readBytes( 0, 16 ) == "first\r\ntail end\n" );
        REQUIRE( first->lineEnd( 1_lnum ) == 12_offset );
        REQUIRE( first->readBytes( 0, 11 ) == "first\r\ntail" );
    }
    SECTION( "full reindex replaces source incarnation without invalidating the old lease" )
    {
        writeContent( path, "new\n" );
        FullIndexOperation replacement( path, data, interrupted,
                                         QTextCodec::codecForName( "UTF-8" ) );
        REQUIRE( std::get<bool>( replacement.run() ) );
        const auto second = indexedSnapshot( data );
        REQUIRE( second );
        REQUIRE( second->incarnation() != first->incarnation() );
        REQUIRE( second->readBytes( 0, 4 ) == "new\n" );
    }
    IndexingData::MutateAccessor{ data.get() }.clear();
    REQUIRE_FALSE( indexedSnapshot( data ) );
    data.reset();
    REQUIRE( first->readBytes( 0, 11 ) == "first\r\ntail" );
}
