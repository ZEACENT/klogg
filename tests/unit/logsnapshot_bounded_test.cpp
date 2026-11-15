#include <catch2/catch.hpp>

#include "logsnapshot.h"

namespace {
class ObservedSnapshot : public klogg::LogSnapshot {
  public:
    std::optional<uint64_t> bytes;
    QString text = QStringLiteral( "abc" );
    mutable int payloadReads = 0;
    LinesCount lineCount() const override { return 1_lcount; }
    klogg::SnapshotRow row( LineNumber ) const override
    {
        return { { klogg::LogRowId::Origin::Memory, 1, 0, 0 }, 1_lnum, true, false };
    }
    std::optional<uint64_t> sourceBytesForLine( LineNumber ) const override { return bytes; }
    QString lineString( LineNumber ) const override { ++payloadReads; return text; }
};
}

TEST_CASE( "Bounded snapshot reads reject source extents before materializing payload", "[snapshot-bounded]" )
{
    ObservedSnapshot snapshot;
    snapshot.bytes = uint64_t{ 1 } << 40;
    CHECK_THROWS_AS( snapshot.readLineBounded( 0_lnum, 1024, 1024 ), klogg::SnapshotLimitError );
    CHECK( snapshot.payloadReads == 0 );
    snapshot.bytes = {};
    CHECK_THROWS_AS( snapshot.readLineBounded( 0_lnum, 1024, 1024 ), klogg::SnapshotReadUnsupported );
    CHECK( snapshot.payloadReads == 0 );
    snapshot.bytes = 4;
    CHECK_THROWS_AS( snapshot.readLineBounded( 1_lnum, 1024, 1024 ), klogg::SnapshotError );
    CHECK( snapshot.payloadReads == 0 );
}

TEST_CASE( "Bounded snapshot reads report charged source bytes and enforce normalized text limits", "[snapshot-bounded]" )
{
    ObservedSnapshot snapshot;
    snapshot.bytes = 4;
    const auto read = snapshot.readLineBounded( 0_lnum, 4, 3 );
    CHECK( read.text == QStringLiteral( "abc" ) );
    CHECK( read.sourceBytes == 4 );
    CHECK( snapshot.payloadReads == 1 );
    CHECK_THROWS_AS( snapshot.readLineBounded( 0_lnum, 4, 2 ), klogg::SnapshotLimitError );
    CHECK( snapshot.payloadReads == 2 );
    snapshot.bytes = 0;
    snapshot.text.clear();
    const auto empty = snapshot.readLineBounded( 0_lnum, 0, 0 );
    CHECK( empty.text.isEmpty() );
    CHECK( empty.sourceBytes == 0 );
}
