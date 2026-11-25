#include <catch2/catch.hpp>

#include <QByteArray>
#include <utility>
#include <vector>

#include "qtcontainerindex.h"
#include "controlframing.h"

using klogg::control::FrameDecoder;
using klogg::control::FrameError;
using klogg::control::encodeFrame;

TEST_CASE( "Control frames use a four-byte network-order length", "[control-framing]" )
{
    const auto frame = encodeFrame( QByteArray( "{}" ) );
    REQUIRE( frame );
    CHECK( *frame == QByteArray::fromHex( "000000027b7d" ) );
    CHECK_FALSE( encodeFrame( {} ) );
    CHECK_FALSE( encodeFrame( QByteArray( static_cast<int>( klogg::control::MaxFrameBytes ) + 1,
                                         'x' ) ) );
}

TEST_CASE( "Control frames survive arbitrary header and UTF-8 payload fragmentation",
           "[control-framing]" )
{
    const QByteArray body = QByteArray( "{\"text\":\"" )
                            + QByteArray::fromHex( "e4b8ade69687" ) + "\"}";
    const auto encoded = encodeFrame( body );
    REQUIRE( encoded );
    for ( klogg::ContainerIndex split = 0; split <= encoded->size(); ++split ) {
        FrameDecoder decoder;
        auto first = decoder.feed( encoded->left( split ) );
        auto second = decoder.feed( encoded->mid( split ) );
        REQUIRE( first.error == FrameError::None );
        REQUIRE( second.error == FrameError::None );
        std::vector<QByteArray> messages = std::move( first.messages );
        messages.insert( messages.end(), second.messages.begin(), second.messages.end() );
        REQUIRE( messages.size() == 1 );
        CHECK( messages.front() == body );
        CHECK( decoder.finish() == FrameError::None );
    }
}

TEST_CASE( "Control framing preserves coalesced messages and an incomplete tail",
           "[control-framing]" )
{
    const auto first = encodeFrame( "one" );
    const auto second = encodeFrame( "two" );
    REQUIRE( first );
    REQUIRE( second );
    FrameDecoder decoder;
    const auto complete = decoder.feed( *first + *second + second->left( 3 ) );
    REQUIRE( complete.error == FrameError::None );
    REQUIRE( complete.messages.size() == 2 );
    CHECK( complete.messages[ 0 ] == "one" );
    CHECK( complete.messages[ 1 ] == "two" );
    const auto tail = decoder.feed( second->mid( 3 ) );
    REQUIRE( tail.messages.size() == 1 );
    CHECK( tail.messages.front() == "two" );
    CHECK( decoder.finish() == FrameError::None );
}

TEST_CASE( "Control framing rejects invalid lengths before receiving payloads",
           "[control-framing]" )
{
    const auto length = GENERATE( QByteArray::fromHex( "00000000" ),
                                 QByteArray::fromHex( "00100001" ),
                                 QByteArray::fromHex( "ffffffff" ) );
    FrameDecoder decoder;
    const auto rejected = decoder.feed( length );
    CHECK( rejected.error != FrameError::None );
    CHECK( rejected.messages.empty() );
    CHECK( decoder.feed( *encodeFrame( "valid" ) ).error == rejected.error );
    CHECK( decoder.finish() == rejected.error );
}

TEST_CASE( "Control framing detects truncated EOF and rejects data after EOF",
           "[control-framing]" )
{
    const auto encoded = encodeFrame( "payload" );
    REQUIRE( encoded );
    for ( klogg::ContainerIndex size = 1; size < encoded->size(); ++size ) {
        FrameDecoder decoder;
        CHECK( decoder.feed( encoded->left( size ) ).error == FrameError::None );
        CHECK( decoder.finish() == FrameError::Truncated );
        CHECK( decoder.feed( encoded->mid( size ) ).messages.empty() );
    }
    FrameDecoder empty;
    CHECK( empty.finish() == FrameError::None );
    CHECK( empty.feed( *encoded ).error == FrameError::Closed );
}

TEST_CASE( "Control framing accepts the bounded maximum and limits queued frames",
           "[control-framing]" )
{
    const QByteArray payload( static_cast<int>( klogg::control::MaxFrameBytes ), 'x' );
    const auto encoded = encodeFrame( payload );
    REQUIRE( encoded );
    FrameDecoder decoder;
    const auto maximum = decoder.feed( *encoded );
    REQUIRE( maximum.error == FrameError::None );
    REQUIRE( maximum.messages.size() == 1 );
    CHECK( maximum.messages.front() == payload );

    QByteArray flood;
    for ( size_t i = 0; i <= FrameDecoder::MaxMessagesPerFeed; ++i ) {
        flood.append( *encodeFrame( "x" ) );
    }
    const auto rejected = decoder.feed( flood );
    CHECK( rejected.error == FrameError::TooManyFrames );
    CHECK( rejected.messages.empty() );
}
