#include <catch2/catch.hpp>
#include <memory>
#include <variant>

#include <QTemporaryFile>
#include <QTextCodec>

#include "configuration.h"
#include "logdataworker.h"

namespace {
class RestoreDefaultEncoding {
public:
    RestoreDefaultEncoding() : original_( Configuration::get().defaultEncodingMib() ) {}
    ~RestoreDefaultEncoding() { Configuration::get().setDefaultEncodingMib( original_ ); }
    RestoreDefaultEncoding( const RestoreDefaultEncoding& ) = delete;
    RestoreDefaultEncoding& operator=( const RestoreDefaultEncoding& ) = delete;
private:
    int original_;
};
}

TEST_CASE( "Native indexing uses captured defaults without overwriting later GUI preferences",
           "[indexing-preparation]" )
{
    RestoreDefaultEncoding restore;
    auto& config = Configuration::get();
    const auto* originalCodec = QTextCodec::codecForName( "ISO-8859-1" );
    const auto* laterCodec = QTextCodec::codecForName( "UTF-8" );
    REQUIRE( originalCodec );
    REQUIRE( laterCodec );
    config.setDefaultEncodingMib( originalCodec->mibEnum() );
    const auto settings = IndexingSettings::capture();
    QTemporaryFile file;
    REQUIRE( file.open() );
    auto data = std::make_shared<IndexingData>();
    AtomicFlag interrupted;
    FullIndexOperation operation{ file.fileName(), data, interrupted, nullptr, settings };
    config.setDefaultEncodingMib( laterCodec->mibEnum() );
    REQUIRE( std::get<bool>( operation.run() ) );
    IndexingData::ConstAccessor indexed{ data.get() };
    REQUIRE( indexed.getEncodingGuess() );
    CHECK( indexed.getEncodingGuess()->mibEnum() == originalCodec->mibEnum() );
    REQUIRE( indexed.getContentSnapshot() );
    CHECK( indexed.getContentSnapshot()->lineCount() == 0_lcount );
    CHECK( config.defaultEncodingMib() == laterCodec->mibEnum() );
}

TEST_CASE( "Encoding detection honors an explicit fallback without consulting current preferences",
           "[indexing-preparation]" )
{
    RestoreDefaultEncoding restore;
    const auto* fallback = QTextCodec::codecForName( "ISO-8859-1" );
    const auto* current = QTextCodec::codecForName( "UTF-8" );
    REQUIRE( fallback );
    REQUIRE( current );
    Configuration::get().setDefaultEncodingMib( current->mibEnum() );
    const klogg::vector<char> empty;
    const auto* detected = EncodingDetector::getInstance().detectEncoding( empty, fallback->mibEnum() );
    REQUIRE( detected );
    CHECK( detected->mibEnum() == fallback->mibEnum() );
    CHECK( Configuration::get().defaultEncodingMib() == current->mibEnum() );
}
