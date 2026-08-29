/*
 * Copyright (C) 2026
 *
 * This file is part of klogg.
 *
 * klogg is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * klogg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with klogg.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <catch2/catch.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "iosostraceprotocol.h"

namespace {
using namespace klogg::livecapture::ios;

constexpr std::size_t HeaderSize = 0x81u;
constexpr std::size_t MarkerOffset = 0u;
constexpr std::size_t TypeOffset = 1u;
constexpr std::size_t HeaderSizeOffset = 5u;
constexpr std::size_t PidOffset = 9u;
constexpr std::size_t ProcIdOffset = 13u;
constexpr std::size_t ProcessUuidOffset = 21u;
constexpr std::size_t ProcessPathLengthOffset = 37u;
constexpr std::size_t ActivityIdOffset = 39u;
constexpr std::size_t ParentActivityIdOffset = 47u;
constexpr std::size_t SecondsOffset = 55u;
constexpr std::size_t MicrosecondsOffset = 63u;
constexpr std::size_t LevelOffset = 68u;
constexpr std::size_t MachTimestampOffset = 75u;
constexpr std::size_t ThreadIdOffset = 83u;
constexpr std::size_t ImageUuidOffset = 91u;
constexpr std::size_t ImagePathLengthOffset = 107u;
constexpr std::size_t MessageLengthOffset = 109u;
constexpr std::size_t ImageOffsetOffset = 113u;
constexpr std::size_t SubsystemLengthOffset = 117u;
constexpr std::size_t CategoryLengthOffset = 121u;

void putLe16( ByteBuffer& bytes, std::size_t offset, std::uint16_t value )
{
    REQUIRE( offset + 2u <= bytes.size() );
    bytes.at( offset ) = static_cast<std::uint8_t>( value & 0xffu );
    bytes.at( offset + 1u ) = static_cast<std::uint8_t>( value >> 8u );
}

void putLe32( ByteBuffer& bytes, std::size_t offset, std::uint32_t value )
{
    REQUIRE( offset + 4u <= bytes.size() );
    for ( std::size_t index = 0; index < 4u; ++index ) {
        bytes.at( offset + index )
            = static_cast<std::uint8_t>( ( value >> ( index * 8u ) ) & 0xffu );
    }
}

void putLe64( ByteBuffer& bytes, std::size_t offset, std::uint64_t value )
{
    REQUIRE( offset + 8u <= bytes.size() );
    for ( std::size_t index = 0; index < 8u; ++index ) {
        bytes.at( offset + index )
            = static_cast<std::uint8_t>( ( value >> ( index * 8u ) ) & 0xffu );
    }
}

ByteBuffer nulTerminated( const std::string& value )
{
    ByteBuffer result( value.begin(), value.end() );
    result.push_back( 0u );
    return result;
}

ByteBuffer concatenate( const std::vector<ByteBuffer>& parts )
{
    ByteBuffer result;
    for ( const auto& part : parts ) {
        result.insert( result.end(), part.begin(), part.end() );
    }
    return result;
}

struct PacketFixture {
    ByteBuffer processPath = nulTerminated( "/Applications/Runner.app/Runner" );
    ByteBuffer imagePath
        = nulTerminated( "/System/Library/Frameworks/Framework.framework/Framework" );
    ByteBuffer message = nulTerminated( "network ready" );
    ByteBuffer subsystem = nulTerminated( "com.example.runner" );
    ByteBuffer category = nulTerminated( "network" );
    std::uint8_t marker = 2u;
    std::uint32_t type = 8u;
    std::uint32_t headerSize = static_cast<std::uint32_t>( HeaderSize );
    std::uint8_t level = 0x10u;

    ByteBuffer wire() const
    {
        REQUIRE( processPath.size() <= std::numeric_limits<std::uint16_t>::max() );
        REQUIRE( imagePath.size() <= std::numeric_limits<std::uint16_t>::max() );
        REQUIRE( message.size() <= std::numeric_limits<std::uint32_t>::max() );
        REQUIRE( subsystem.size() <= std::numeric_limits<std::uint16_t>::max() );
        REQUIRE( category.size() <= std::numeric_limits<std::uint16_t>::max() );

        ByteBuffer result( HeaderSize, 0u );
        result.at( MarkerOffset ) = marker;
        putLe32( result, TypeOffset, type );
        putLe32( result, HeaderSizeOffset, headerSize );
        putLe32( result, PidOffset, 0x01020304u );
        putLe64( result, ProcIdOffset, 0x0102030405060708ull );
        for ( std::size_t index = 0; index < 16u; ++index ) {
            result.at( ProcessUuidOffset + index ) = static_cast<std::uint8_t>( 0x10u + index );
            result.at( ImageUuidOffset + index ) = static_cast<std::uint8_t>( 0xa0u + index );
        }
        putLe16( result, ProcessPathLengthOffset,
                 static_cast<std::uint16_t>( processPath.size() ) );
        putLe64( result, ActivityIdOffset, 0x1112131415161718ull );
        putLe64( result, ParentActivityIdOffset, 0x2122232425262728ull );
        putLe64( result, SecondsOffset, 1700000000ull );
        putLe32( result, MicrosecondsOffset, 123456u );
        result.at( LevelOffset ) = level;
        putLe64( result, MachTimestampOffset, 0x3132333435363738ull );
        putLe32( result, ThreadIdOffset, 0x41424344u );
        putLe16( result, ImagePathLengthOffset, static_cast<std::uint16_t>( imagePath.size() ) );
        putLe32( result, MessageLengthOffset, static_cast<std::uint32_t>( message.size() ) );
        putLe32( result, ImageOffsetOffset, 0x1234u );
        putLe16( result, SubsystemLengthOffset, static_cast<std::uint16_t>( subsystem.size() ) );
        putLe16( result, CategoryLengthOffset, static_cast<std::uint16_t>( category.size() ) );

        result.insert( result.end(), processPath.begin(), processPath.end() );
        result.insert( result.end(), imagePath.begin(), imagePath.end() );
        result.insert( result.end(), message.begin(), message.end() );
        result.insert( result.end(), subsystem.begin(), subsystem.end() );
        result.insert( result.end(), category.begin(), category.end() );
        return result;
    }
};

void requireError( const OsTraceDecodeResult& result, OsTraceDecodeErrorCode code,
                   OsTraceField field )
{
    REQUIRE_FALSE( result.record.has_value() );
    REQUIRE( result.error.has_value() );
    const auto& error = result.error.value();
    CHECK( error.code == code );
    CHECK( error.field == field );
}

ByteBuffer relayFrame( std::uint8_t type, const ByteBuffer& payload )
{
    REQUIRE( ( type == 1u || type == 2u ) );
    REQUIRE( payload.size() <= std::numeric_limits<std::uint32_t>::max() );
    const auto length = static_cast<std::uint32_t>( payload.size() );
    ByteBuffer frame{ type, 0u, 0u, 0u, 0u };
    if ( type == 1u ) {
        frame.at( 1 ) = static_cast<std::uint8_t>( ( length >> 24u ) & 0xffu );
        frame.at( 2 ) = static_cast<std::uint8_t>( ( length >> 16u ) & 0xffu );
        frame.at( 3 ) = static_cast<std::uint8_t>( ( length >> 8u ) & 0xffu );
        frame.at( 4 ) = static_cast<std::uint8_t>( length & 0xffu );
    }
    else {
        frame.at( 1 ) = static_cast<std::uint8_t>( length & 0xffu );
        frame.at( 2 ) = static_cast<std::uint8_t>( ( length >> 8u ) & 0xffu );
        frame.at( 3 ) = static_cast<std::uint8_t>( ( length >> 16u ) & 0xffu );
        frame.at( 4 ) = static_cast<std::uint8_t>( ( length >> 24u ) & 0xffu );
    }
    frame.insert( frame.end(), payload.begin(), payload.end() );
    return frame;
}

std::string byteString( const ByteBuffer& bytes )
{
    return { bytes.begin(), bytes.end() };
}

std::uint32_t nextDeterministic( std::uint32_t& state ) noexcept
{
    state = state * 1664525u + 1013904223u;
    return state;
}

bool isWellFormedUtf8( const std::string& text )
{
    const auto* bytes = reinterpret_cast<const unsigned char*>( text.data() );
    std::size_t index = 0u;
    while ( index < text.size() ) {
        const auto first = bytes[ index ];
        if ( first <= 0x7fu ) {
            ++index;
            continue;
        }

        std::size_t length = 0u;
        if ( first >= 0xc2u && first <= 0xdfu ) {
            length = 2u;
        }
        else if ( first >= 0xe0u && first <= 0xefu ) {
            length = 3u;
        }
        else if ( first >= 0xf0u && first <= 0xf4u ) {
            length = 4u;
        }
        else {
            return false;
        }
        if ( length > text.size() - index ) {
            return false;
        }
        for ( std::size_t continuation = 1u; continuation < length; ++continuation ) {
            if ( bytes[ index + continuation ] < 0x80u || bytes[ index + continuation ] > 0xbfu ) {
                return false;
            }
        }
        if ( length == 3u
             && ( ( first == 0xe0u && bytes[ index + 1u ] < 0xa0u )
                  || ( first == 0xedu && bytes[ index + 1u ] > 0x9fu ) ) ) {
            return false;
        }
        if ( length == 4u
             && ( ( first == 0xf0u && bytes[ index + 1u ] < 0x90u )
                  || ( first == 0xf4u && bytes[ index + 1u ] > 0x8fu ) ) ) {
            return false;
        }
        index += length;
    }
    return true;
}

bool containsAsciiTerminalControl( const std::string& text )
{
    return std::any_of( text.begin(), text.end(), []( char value ) {
        const auto byte = static_cast<unsigned char>( value );
        return byte < 0x20u || byte == 0x7fu;
    } );
}

std::string stripGeneratedAnsi( const std::string& text )
{
    std::string plain;
    for ( std::size_t index = 0u; index < text.size(); ) {
        if ( static_cast<unsigned char>( text[ index ] ) == 0x1bu && index + 2u < text.size()
             && text[ index + 1u ] == '[' ) {
            const auto end = text.find( 'm', index + 2u );
            REQUIRE( end != std::string::npos );
            index = end + 1u;
            continue;
        }
        plain.push_back( text[ index ] );
        ++index;
    }
    return plain;
}

DecodedOsTraceRecord formattingRecord()
{
    DecodedOsTraceRecord record;
    record.pid = 4242u;
    record.seconds = 1700000000ull;
    record.microseconds = 123456u;
    record.level = OsTraceLevel::Error;
    record.processPath = std::string{ "/Applications/Runner.app/Runner" };
    record.imagePath = std::string{ "/System/Library/Frameworks/Framework.framework/Framework" };
    record.message = std::string{ "bad \xef\xbf\xbd payload" };
    record.subsystem = std::string{ "com.example" };
    record.category = std::string{ "network" };
    record.imageOffset = 0x1234u;
    return record;
}
} // namespace

TEST_CASE( "iOS os_trace decoder reads the public 1.4.0 packed header explicitly as little endian",
           "[livecapture][ios][ostrace][protocol]" )
{
    const auto result = decodeOsTracePacket( PacketFixture{}.wire() );

    REQUIRE_FALSE( result.error.has_value() );
    REQUIRE( result.record.has_value() );
    const auto& record = result.record.value();
    CHECK( record.packetType == OsTracePacketType::LogMessage );
    CHECK( record.pid == 0x01020304u );
    CHECK( record.processId == 0x0102030405060708ull );
    CHECK( record.activityId == 0x1112131415161718ull );
    CHECK( record.parentActivityId == 0x2122232425262728ull );
    CHECK( record.seconds == 1700000000ull );
    CHECK( record.microseconds == 123456u );
    CHECK( record.level == OsTraceLevel::Error );
    CHECK( record.machTimestamp == 0x3132333435363738ull );
    CHECK( record.threadId == 0x41424344u );
    CHECK( record.imageOffset == 0x1234u );
    CHECK( record.processUuid.bytes.front() == 0x10u );
    CHECK( record.processUuid.bytes.back() == 0x1fu );
    CHECK( record.imageUuid.bytes.front() == 0xa0u );
    CHECK( record.imageUuid.bytes.back() == 0xafu );
    CHECK( record.processPath == std::optional<std::string>{ "/Applications/Runner.app/Runner" } );
    CHECK( record.imagePath
           == std::optional<std::string>{
               "/System/Library/Frameworks/Framework.framework/Framework" } );
    CHECK( record.message == std::optional<std::string>{ "network ready" } );
    CHECK( record.subsystem == std::optional<std::string>{ "com.example.runner" } );
    CHECK( record.category == std::optional<std::string>{ "network" } );
}

TEST_CASE( "iOS os_trace decoder accepts only the documented marker packet types and levels",
           "[livecapture][ios][ostrace][protocol][validation]" )
{
    const std::array<std::pair<std::uint8_t, OsTraceLevel>, 6> levels{
        std::pair<std::uint8_t, OsTraceLevel>{ std::uint8_t{ 0x00 }, OsTraceLevel::Notice },
        std::pair<std::uint8_t, OsTraceLevel>{ std::uint8_t{ 0x01 }, OsTraceLevel::Info },
        std::pair<std::uint8_t, OsTraceLevel>{ std::uint8_t{ 0x02 }, OsTraceLevel::Debug },
        std::pair<std::uint8_t, OsTraceLevel>{ std::uint8_t{ 0x03 }, OsTraceLevel::UserAction },
        std::pair<std::uint8_t, OsTraceLevel>{ std::uint8_t{ 0x10 }, OsTraceLevel::Error },
        std::pair<std::uint8_t, OsTraceLevel>{ std::uint8_t{ 0x11 }, OsTraceLevel::Fault },
    };
    for ( const auto& level : levels ) {
        PacketFixture fixture;
        fixture.type = 8u;
        fixture.level = level.first;
        const auto result = decodeOsTracePacket( fixture.wire() );
        INFO( "type=8 level=" << static_cast<unsigned>( level.first ) );
        REQUIRE_FALSE( result.error.has_value() );
        REQUIRE( result.record.has_value() );
        CHECK( result.record.value().level == level.second );
    }

    SECTION( "activity packets carry an opaque bounded body, not log-message spans" )
    {
        PacketFixture fixture;
        fixture.type = 2u;
        auto wire = fixture.wire();
        wire.resize( HeaderSize + 12u );
        putLe16( wire, ProcessPathLengthOffset, std::numeric_limits<std::uint16_t>::max() );
        putLe16( wire, ImagePathLengthOffset, std::numeric_limits<std::uint16_t>::max() );
        putLe32( wire, MessageLengthOffset, std::numeric_limits<std::uint32_t>::max() );
        const auto result = decodeOsTracePacket( wire );
        REQUIRE_FALSE( result.error.has_value() );
        REQUIRE( result.record.has_value() );
        CHECK( result.record->packetType == OsTracePacketType::Activity );
        CHECK_FALSE( result.record->processPath.has_value() );
        CHECK_FALSE( result.record->imagePath.has_value() );
        CHECK_FALSE( result.record->message.has_value() );
        CHECK_FALSE( result.record->subsystem.has_value() );
        CHECK_FALSE( result.record->category.has_value() );
    }

    PacketFixture badMarker;
    badMarker.marker = 3u;
    requireError( decodeOsTracePacket( badMarker.wire() ), OsTraceDecodeErrorCode::InvalidMarker,
                  OsTraceField::Marker );

    for ( const auto type : { 0u, 1u, 3u, 7u, 9u, 0xffffffffu } ) {
        PacketFixture fixture;
        fixture.type = type;
        INFO( "type=" << type );
        requireError( decodeOsTracePacket( fixture.wire() ),
                      OsTraceDecodeErrorCode::UnknownPacketType, OsTraceField::PacketType );
    }

    for ( const auto level : { 0x04u, 0x0fu, 0x12u, 0xffu } ) {
        PacketFixture fixture;
        fixture.level = static_cast<std::uint8_t>( level );
        INFO( "level=" << level );
        requireError( decodeOsTracePacket( fixture.wire() ),
                      OsTraceDecodeErrorCode::UnknownLogLevel, OsTraceField::Level );
    }
}

TEST_CASE( "iOS os_trace decoder rejects every truncation before the complete 0x81 header",
           "[livecapture][ios][ostrace][protocol][bounds]" )
{
    const auto complete = PacketFixture{}.wire();
    for ( std::size_t size = 0u; size < HeaderSize; ++size ) {
        INFO( "truncated size=" << size );
        const ByteBuffer truncated(
            complete.begin(), complete.begin() + static_cast<ByteBuffer::difference_type>( size ) );
        requireError( decodeOsTracePacket( truncated ), OsTraceDecodeErrorCode::TruncatedHeader,
                      OsTraceField::Header );
    }
}

TEST_CASE( "iOS os_trace decoder requires the exact public packed 0x81 header size",
           "[livecapture][ios][ostrace][protocol][bounds]" )
{
    for ( const auto invalidSize : { 0u, 1u, 0x80u, 0x82u, 0xffffffffu } ) {
        PacketFixture fixture;
        fixture.headerSize = invalidSize;
        INFO( "header_size=" << invalidSize );
        requireError( decodeOsTracePacket( fixture.wire() ),
                      OsTraceDecodeErrorCode::InvalidHeaderSize, OsTraceField::HeaderSize );
    }
}

TEST_CASE( "iOS os_trace decoder bounds-checks every variable field before slicing",
           "[livecapture][ios][ostrace][protocol][bounds]" )
{
    struct OverflowCase {
        std::size_t lengthOffset;
        bool lengthIs32Bit;
        OsTraceField field;
    };
    const std::array cases{
        OverflowCase{ ProcessPathLengthOffset, false, OsTraceField::ProcessPath },
        OverflowCase{ ImagePathLengthOffset, false, OsTraceField::ImagePath },
        OverflowCase{ MessageLengthOffset, true, OsTraceField::Message },
        OverflowCase{ SubsystemLengthOffset, false, OsTraceField::Subsystem },
        OverflowCase{ CategoryLengthOffset, false, OsTraceField::Category },
    };

    const PacketFixture fixture;
    for ( const auto& overflow : cases ) {
        auto wire = fixture.wire();
        if ( overflow.lengthIs32Bit ) {
            putLe32( wire, overflow.lengthOffset, std::numeric_limits<std::uint32_t>::max() );
        }
        else {
            putLe16( wire, overflow.lengthOffset, std::numeric_limits<std::uint16_t>::max() );
        }
        INFO( "field=" << static_cast<unsigned>( overflow.field ) );
        const auto result = decodeOsTracePacket( wire );
        requireError( result, OsTraceDecodeErrorCode::SpanOutOfBounds, overflow.field );
        REQUIRE( result.error->structure.has_value() );
        const auto& structure = *result.error->structure;
        CHECK( structure.packetByteCount == wire.size() );
        CHECK( structure.marker == 2u );
        CHECK( structure.wirePacketType == 8u );
        CHECK( structure.declaredHeaderByteCount == HeaderSize );
        CHECK( structure.availableVariableByteCount == wire.size() - HeaderSize );
        CHECK( structure.declaredSpanByteCount > structure.availableVariableByteCount );
        CHECK( structure.fieldLengths.at( 2u )
               == ( overflow.lengthIs32Bit ? std::numeric_limits<std::uint32_t>::max()
                                           : fixture.message.size() ) );
    }
}

TEST_CASE( "iOS os_trace decoder enforces its configured packet allocation boundary",
           "[livecapture][ios][ostrace][protocol][bounds]" )
{
    const auto wire = PacketFixture{}.wire();
    const auto result
        = decodeOsTracePacket( wire, OsTraceDecodeLimits{ wire.size() - std::size_t{ 1u } } );

    requireError( result, OsTraceDecodeErrorCode::PacketTooLarge, OsTraceField::Packet );
}

TEST_CASE( "iOS os_trace decoder rejects cumulative variable-span overflow",
           "[livecapture][ios][ostrace][protocol][bounds]" )
{
    PacketFixture fixture;
    auto wire = fixture.wire();
    const auto variableByteCount = wire.size() - HeaderSize;
    REQUIRE( variableByteCount <= std::numeric_limits<std::uint16_t>::max() );

    putLe16( wire, ProcessPathLengthOffset, static_cast<std::uint16_t>( variableByteCount ) );
    putLe16( wire, ImagePathLengthOffset, 1u );
    putLe32( wire, MessageLengthOffset, 0u );
    putLe16( wire, SubsystemLengthOffset, 0u );
    putLe16( wire, CategoryLengthOffset, 0u );

    requireError( decodeOsTracePacket( wire ), OsTraceDecodeErrorCode::SpanOutOfBounds,
                  OsTraceField::ImagePath );
}

TEST_CASE( "iOS os_trace decoder rejects invalid timestamp fractions and trailing packet bytes",
           "[livecapture][ios][ostrace][protocol][validation]" )
{
    SECTION( "microseconds must be a normalized timestamp fraction" )
    {
        auto wire = PacketFixture{}.wire();
        putLe32( wire, MicrosecondsOffset, 1'000'000u );

        requireError( decodeOsTracePacket( wire ), OsTraceDecodeErrorCode::InvalidTimestamp,
                      OsTraceField::Microseconds );
    }

    SECTION( "declared variable spans must consume the complete callback packet" )
    {
        auto wire = PacketFixture{}.wire();
        wire.push_back( 0xa5u );

        requireError( decodeOsTracePacket( wire ), OsTraceDecodeErrorCode::UnexpectedTrailingData,
                      OsTraceField::Packet );
    }
}

TEST_CASE( "iOS os_trace decoder defines missing optional fields independently",
           "[livecapture][ios][ostrace][protocol][optional]" )
{
    PacketFixture fixture;
    fixture.processPath.clear();
    fixture.imagePath.clear();
    fixture.message.clear();
    fixture.subsystem.clear();
    fixture.category.clear();

    const auto result = decodeOsTracePacket( fixture.wire() );
    REQUIRE_FALSE( result.error.has_value() );
    REQUIRE( result.record.has_value() );
    const auto& record = result.record.value();
    CHECK_FALSE( record.processPath.has_value() );
    CHECK_FALSE( record.imagePath.has_value() );
    CHECK_FALSE( record.message.has_value() );
    CHECK_FALSE( record.subsystem.has_value() );
    CHECK_FALSE( record.category.has_value() );

    fixture.subsystem = nulTerminated( "subsystem-only" );
    const auto partialLabel = decodeOsTracePacket( fixture.wire() );
    REQUIRE_FALSE( partialLabel.error.has_value() );
    REQUIRE( partialLabel.record.has_value() );
    const auto& partialRecord = partialLabel.record.value();
    CHECK( partialRecord.subsystem == std::optional<std::string>{ "subsystem-only" } );
    CHECK_FALSE( partialRecord.category.has_value() );
}

TEST_CASE( "iOS os_trace decoder rejects missing or embedded NUL within declared text spans",
           "[livecapture][ios][ostrace][protocol][text]" )
{
    SECTION( "missing final terminator" )
    {
        PacketFixture fixture;
        fixture.message = ByteBuffer{ 'n', 'o', 'n', 'u', 'l' };
        requireError( decodeOsTracePacket( fixture.wire() ),
                      OsTraceDecodeErrorCode::MissingNulTerminator, OsTraceField::Message );
    }

    SECTION( "embedded terminator" )
    {
        PacketFixture fixture;
        fixture.category = ByteBuffer{ 'n', 'e', 't', 0u, 'x', 0u };
        requireError( decodeOsTracePacket( fixture.wire() ), OsTraceDecodeErrorCode::EmbeddedNul,
                      OsTraceField::Category );
    }
}

TEST_CASE( "iOS os_trace decoder replaces malformed UTF-8 without dropping surrounding bytes",
           "[livecapture][ios][ostrace][protocol][text]" )
{
    PacketFixture fixture;
    fixture.message = ByteBuffer{ 'b', 'a', 'd', ' ', 0xffu, ' ', 't', 'e', 'x', 't', 0u };

    const auto result = decodeOsTracePacket( fixture.wire() );
    REQUIRE_FALSE( result.error.has_value() );
    REQUIRE( result.record.has_value() );
    const auto& record = result.record.value();
    REQUIRE( record.message.has_value() );
    CHECK( record.message.value() == std::string{ "bad \xef\xbf\xbd text" } );
}

TEST_CASE( "iOS os_trace decoder sanitizes a deterministic malformed UTF-8 corpus",
           "[livecapture][ios][ostrace][protocol][text][property]" )
{
    std::uint32_t state = 0x13579bdfu;
    for ( std::size_t corpusIndex = 0u; corpusIndex < 512u; ++corpusIndex ) {
        PacketFixture fixture;
        fixture.message.clear();
        const auto byteCount = static_cast<std::size_t>( nextDeterministic( state ) % 128u ) + 1u;
        fixture.message.reserve( byteCount + 1u );
        for ( std::size_t index = 0u; index < byteCount; ++index ) {
            auto byte = static_cast<std::uint8_t>( nextDeterministic( state ) & 0xffu );
            if ( byte == 0u ) {
                byte = 1u;
            }
            fixture.message.push_back( byte );
        }
        fixture.message.push_back( 0u );

        const auto decoded = decodeOsTracePacket( fixture.wire() );
        INFO( "corpusIndex=" << corpusIndex );
        REQUIRE_FALSE( decoded.error.has_value() );
        REQUIRE( decoded.record.has_value() );
        const auto& record = decoded.record.value();
        REQUIRE( record.message.has_value() );
        CHECK( isWellFormedUtf8( record.message.value() ) );
        CHECK( record.message.value().find( '\0' ) == std::string::npos );

        const auto plain
            = formatOsTraceRecord( record, OsTraceFormatOptions{ false, false, false } );
        CHECK_FALSE( containsAsciiTerminalControl( plain.bytes ) );
    }
}

TEST_CASE( "iOS os_trace decoder returns exactly one outcome for deterministic packet mutations",
           "[livecapture][ios][ostrace][protocol][property]" )
{
    const auto baseline = PacketFixture{}.wire();
    std::uint32_t state = 0x2468ace0u;
    for ( std::size_t corpusIndex = 0u; corpusIndex < 1024u; ++corpusIndex ) {
        auto mutation = baseline;
        const auto mutationCount = static_cast<std::size_t>( nextDeterministic( state ) % 4u ) + 1u;
        for ( std::size_t index = 0u; index < mutationCount; ++index ) {
            const auto offset
                = static_cast<std::size_t>( nextDeterministic( state ) % mutation.size() );
            mutation.at( offset ) = static_cast<std::uint8_t>( nextDeterministic( state ) & 0xffu );
        }
        if ( nextDeterministic( state ) % 5u == 0u ) {
            mutation.resize(
                static_cast<std::size_t>( nextDeterministic( state ) % ( mutation.size() + 1u ) ) );
        }

        const auto result = decodeOsTracePacket( mutation );
        INFO( "corpusIndex=" << corpusIndex );
        CHECK( result.record.has_value() != result.error.has_value() );
        if ( result.record ) {
            CHECK( result.record->microseconds < 1'000'000u );
        }
    }
}

TEST_CASE( "iOS os_trace callback boundary distinguishes control plist from trace packets",
           "[livecapture][ios][ostrace][protocol][callback-boundary]" )
{
    const ByteBuffer binaryPlist{ 'b', 'p', 'l', 'i', 's', 't', '0', '0', 0xd1u, 0x01u };
    const ByteBuffer xmlPlist{ '<', '?', 'x', 'm', 'l', ' ', 'v', 'e', 'r', 's', 'i', 'o', 'n' };
    const ByteBuffer directXmlPlist{ '<', 'p', 'l', 'i', 's', 't', ' ', 'v', 'e', 'r' };
    const auto trace = PacketFixture{}.wire();
    const ByteBuffer unknown{ 0x7fu, 0x00u, 0x01u };

    CHECK( classifyOsTraceCallbackPayload( binaryPlist )
           == OsTraceCallbackPayloadKind::ControlPlist );
    CHECK( classifyOsTraceCallbackPayload( xmlPlist ) == OsTraceCallbackPayloadKind::ControlPlist );
    CHECK( classifyOsTraceCallbackPayload( directXmlPlist )
           == OsTraceCallbackPayloadKind::ControlPlist );
    CHECK( classifyOsTraceCallbackPayload( trace ) == OsTraceCallbackPayloadKind::TracePacket );
    CHECK( classifyOsTraceCallbackPayload( unknown ) == OsTraceCallbackPayloadKind::Unknown );
}

TEST_CASE( "iOS relay framing decodes type 1 big-endian and type 2 little-endian lengths",
           "[livecapture][ios][ostrace][relay]" )
{
    OsTraceRelayFrameDecoder decoder;
    const auto result
        = decoder.feed( concatenate( { relayFrame( 1u, ByteBuffer{ 'p', 'l', 'i', 's', 't' } ),
                                       relayFrame( 2u, ByteBuffer{ 0u, 1u, 2u } ) } ) );

    REQUIRE_FALSE( result.error.has_value() );
    REQUIRE( result.records.size() == 2u );
    CHECK( result.records.at( 0 ).type == OsTraceRelayRecordType::ControlPlist );
    CHECK( byteString( result.records.at( 0 ).payload ) == "plist" );
    CHECK( result.records.at( 1 ).type == OsTraceRelayRecordType::Activity );
    CHECK( result.records.at( 1 ).payload == ByteBuffer{ 0u, 1u, 2u } );
    CHECK( result.bufferedByteCount == 0u );
}

TEST_CASE( "iOS relay framing handles every fragmentation boundary and coalesced records",
           "[livecapture][ios][ostrace][relay][fragmentation]" )
{
    const auto first = relayFrame( 1u, ByteBuffer{ 'o', 'n', 'e' } );
    const auto second = relayFrame( 2u, ByteBuffer{ 't', 'w', 'o' } );
    const auto wire = concatenate( { first, second } );

    OsTraceRelayFrameDecoder bytewise;
    std::vector<OsTraceRelayRecord> decoded;
    for ( const auto byte : wire ) {
        const auto result = bytewise.feed( ByteBuffer{ byte } );
        REQUIRE_FALSE( result.error.has_value() );
        decoded.insert( decoded.end(), result.records.begin(), result.records.end() );
    }
    REQUIRE( decoded.size() == 2u );
    CHECK( byteString( decoded.at( 0 ).payload ) == "one" );
    CHECK( byteString( decoded.at( 1 ).payload ) == "two" );

    for ( std::size_t split = 0u; split <= wire.size(); ++split ) {
        INFO( "split=" << split );
        OsTraceRelayFrameDecoder decoder;
        const auto splitOffset = static_cast<ByteBuffer::difference_type>( split );
        const auto firstResult
            = decoder.feed( ByteBuffer( wire.begin(), wire.begin() + splitOffset ) );
        REQUIRE_FALSE( firstResult.error.has_value() );
        const auto secondResult
            = decoder.feed( ByteBuffer( wire.begin() + splitOffset, wire.end() ) );
        REQUIRE_FALSE( secondResult.error.has_value() );
        CHECK( firstResult.records.size() + secondResult.records.size() == 2u );
        CHECK( secondResult.bufferedByteCount == 0u );
    }
}

TEST_CASE( "iOS relay framing preserves a deterministic corpus across arbitrary chunking",
           "[livecapture][ios][ostrace][relay][fragmentation][property]" )
{
    std::uint32_t state = 0x10203040u;
    std::vector<OsTraceRelayRecord> expected;
    ByteBuffer wire;
    for ( std::size_t recordIndex = 0u; recordIndex < 128u; ++recordIndex ) {
        const auto type = recordIndex % 2u == 0u ? OsTraceRelayRecordType::ControlPlist
                                                 : OsTraceRelayRecordType::Activity;
        ByteBuffer payload( static_cast<std::size_t>( nextDeterministic( state ) % 257u ) );
        for ( auto& byte : payload ) {
            byte = static_cast<std::uint8_t>( nextDeterministic( state ) & 0xffu );
        }
        expected.push_back( OsTraceRelayRecord{ type, payload } );
        const auto encoded
            = relayFrame( type == OsTraceRelayRecordType::ControlPlist ? 1u : 2u, payload );
        wire.insert( wire.end(), encoded.begin(), encoded.end() );
    }

    OsTraceRelayFrameDecoder decoder( 256u );
    std::vector<OsTraceRelayRecord> actual;
    for ( std::size_t offset = 0u; offset < wire.size(); ) {
        const auto chunkSize
            = std::min( static_cast<std::size_t>( nextDeterministic( state ) % 31u ) + 1u,
                        wire.size() - offset );
        const auto first = wire.begin() + static_cast<ByteBuffer::difference_type>( offset );
        const auto result = decoder.feed(
            ByteBuffer( first, first + static_cast<ByteBuffer::difference_type>( chunkSize ) ) );
        REQUIRE_FALSE( result.error.has_value() );
        actual.insert( actual.end(), result.records.begin(), result.records.end() );
        CHECK( result.bufferedByteCount <= 261u );
        offset += chunkSize;
    }

    REQUIRE( actual.size() == expected.size() );
    for ( std::size_t index = 0u; index < expected.size(); ++index ) {
        INFO( "recordIndex=" << index );
        CHECK( actual.at( index ).type == expected.at( index ).type );
        CHECK( actual.at( index ).payload == expected.at( index ).payload );
    }
    CHECK( decoder.feed( {} ).bufferedByteCount == 0u );
}

TEST_CASE( "iOS relay framing enforces a maximum record before allocation",
           "[livecapture][ios][ostrace][relay][bounds]" )
{
    OsTraceRelayFrameDecoder decoder( 4u );
    const auto result = decoder.feed( relayFrame( 2u, ByteBuffer( 5u, 0x5au ) ) );

    REQUIRE( result.records.empty() );
    REQUIRE( result.error.has_value() );
    CHECK( result.error.value().code == OsTraceRelayErrorCode::RecordTooLarge );
    CHECK( result.bufferedByteCount == 0u );

    OsTraceRelayFrameDecoder overflowDecoder( 1024u );
    const auto overflow = overflowDecoder.feed( ByteBuffer{ 2u, 0xffu, 0xffu, 0xffu, 0xffu } );
    REQUIRE( overflow.records.empty() );
    REQUIRE( overflow.error.has_value() );
    CHECK( overflow.error.value().code == OsTraceRelayErrorCode::RecordTooLarge );
}

TEST_CASE( "iOS relay framing errors stay sticky until reset starts a clean stream",
           "[livecapture][ios][ostrace][relay][reset]" )
{
    OsTraceRelayFrameDecoder decoder;
    const auto malformed = decoder.feed( ByteBuffer{ 3u, 0u, 0u, 0u, 0u } );
    REQUIRE( malformed.error.has_value() );
    CHECK( malformed.error.value().code == OsTraceRelayErrorCode::UnknownRecordType );

    const auto sticky
        = decoder.feed( relayFrame( 2u, ByteBuffer{ 'i', 'g', 'n', 'o', 'r', 'e' } ) );
    REQUIRE( sticky.records.empty() );
    REQUIRE( sticky.error.has_value() );
    CHECK( sticky.error.value().code == OsTraceRelayErrorCode::UnknownRecordType );

    decoder.reset();
    CHECK( decoder.feed( ByteBuffer{ 2u, 3u } ).bufferedByteCount == 2u );
    decoder.reset();
    const auto recovered = decoder.feed( relayFrame( 2u, ByteBuffer{ 'o', 'k' } ) );
    REQUIRE_FALSE( recovered.error.has_value() );
    REQUIRE( recovered.records.size() == 1u ); // NOLINT(readability-container-size-empty)
    CHECK( byteString( recovered.records.front().payload ) == "ok" );
}

TEST_CASE( "iOS formatter covers the supported unsigned timestamp range deterministically",
           "[livecapture][ios][ostrace][format][timestamp]" )
{
    auto record = formattingRecord();
    record.seconds = 0u;
    record.microseconds = 0u;
    CHECK( formatOsTraceRecord( record ).bytes.find( "1970-01-01 00:00:00.000000" ) == 0u );

    record.seconds = 253402300799ull;
    record.microseconds = 999999u;
    CHECK( formatOsTraceRecord( record ).bytes.find( "9999-12-31 23:59:59.999999" ) == 0u );

    record.seconds = std::numeric_limits<std::uint64_t>::max();
    const auto first = formatOsTraceRecord( record );
    const auto second = formatOsTraceRecord( record );
    CHECK( first.bytes == second.bytes );
    CHECK( first.statistics == second.statistics );
    CHECK( first.bytes.find( ".999999 " ) != std::string::npos );
}

TEST_CASE(
    "source-neutral iOS formatter emits fixed plain and pymobiledevice3-compatible ANSI bytes",
    "[livecapture][ios][ostrace][format]" )
{
    const auto record = formattingRecord();
    const std::string expectedPlain
        = "2023-11-14 22:13:20.123456 Runner{Framework+0x1234}[4242] <ERROR>: "
          "bad \xef\xbf\xbd payload [com.example][network]";
    const std::string expectedAnsi
        = "\x1b[32m2023-11-14 22:13:20.123456\x1b[0m "
          "\x1b[35mRunner\x1b[0m{\x1b[35mFramework\x1b[0m\x1b[34m+0x1234\x1b[0m}"
          "[\x1b[36m4242\x1b[0m] <\x1b[31mERROR\x1b[0m>: "
          "\x1b[31mbad \xef\xbf\xbd payload\x1b[0m "
          "\x1b[36m[com.example][network]\x1b[0m";

    std::optional<OsTraceFormatStatistics> plainHook;
    const auto plain = formatOsTraceRecord(
        record, OsTraceFormatOptions{ false, true, true },
        [ &plainHook ]( const OsTraceFormatStatistics& statistics ) { plainHook = statistics; } );
    CHECK( plain.bytes == expectedPlain );
    CHECK( plain.statistics.plainByteCount == expectedPlain.size() );
    CHECK( plain.statistics.outputByteCount == expectedPlain.size() );
    CHECK( plain.statistics.ansiEscapeByteCount == 0u );
    CHECK( plain.statistics.ansiExpansionByteCount == 0u );
    REQUIRE( plainHook.has_value() );
    CHECK( plainHook.value() == plain.statistics );

    std::optional<OsTraceFormatStatistics> ansiHook;
    const auto ansi = formatOsTraceRecord(
        record, OsTraceFormatOptions{ true, true, true },
        [ &ansiHook ]( const OsTraceFormatStatistics& statistics ) { ansiHook = statistics; } );
    CHECK( ansi.bytes == expectedAnsi );
    CHECK( ansi.statistics.plainByteCount == expectedPlain.size() );
    CHECK( ansi.statistics.outputByteCount == expectedAnsi.size() );
    CHECK( ansi.statistics.ansiEscapeByteCount == expectedAnsi.size() - expectedPlain.size() );
    CHECK( ansi.statistics.ansiExpansionByteCount == expectedAnsi.size() - expectedPlain.size() );
    REQUIRE( ansiHook.has_value() );
    CHECK( ansiHook.value() == ansi.statistics );
}

TEST_CASE( "iOS formatter keeps plain and ANSI output semantically identical across a corpus",
           "[livecapture][ios][ostrace][format][ansi][property]" )
{
    std::uint32_t state = 0x89abcdefu;
    for ( std::size_t corpusIndex = 0u; corpusIndex < 256u; ++corpusIndex ) {
        auto record = formattingRecord();
        std::string message;
        const auto byteCount = static_cast<std::size_t>( nextDeterministic( state ) % 96u ) + 1u;
        for ( std::size_t index = 0u; index < byteCount; ++index ) {
            message.push_back( static_cast<char>( nextDeterministic( state ) & 0xffu ) );
        }
        record.message = std::move( message );
        record.microseconds = static_cast<std::uint32_t>( nextDeterministic( state ) % 1'000'000u );

        const auto plain = formatOsTraceRecord( record, OsTraceFormatOptions{ false, true, true } );
        const auto ansi = formatOsTraceRecord( record, OsTraceFormatOptions{ true, true, true } );
        INFO( "corpusIndex=" << corpusIndex );
        CHECK( stripGeneratedAnsi( ansi.bytes ) == plain.bytes );
        CHECK( plain.statistics.outputByteCount == plain.bytes.size() );
        CHECK( plain.statistics.plainByteCount == plain.bytes.size() );
        CHECK( ansi.statistics.outputByteCount == ansi.bytes.size() );
        CHECK( ansi.statistics.plainByteCount == plain.bytes.size() );
        CHECK( ansi.statistics.ansiEscapeByteCount
               == ansi.statistics.outputByteCount - ansi.statistics.plainByteCount );
        CHECK_FALSE( containsAsciiTerminalControl( plain.bytes ) );
        CHECK( isWellFormedUtf8( plain.bytes ) );
    }
}

TEST_CASE( "iOS formatter applies the compatible ANSI severity policy to every accepted level",
           "[livecapture][ios][ostrace][format][ansi]" )
{
    struct LevelPolicy {
        OsTraceLevel level;
        const char* name;
        const char* color;
    };
    const std::array policies{
        LevelPolicy{ OsTraceLevel::Notice, "NOTICE", "\x1b[37m" },
        LevelPolicy{ OsTraceLevel::Info, "INFO", "\x1b[37m" },
        LevelPolicy{ OsTraceLevel::Debug, "DEBUG", "\x1b[32m" },
        LevelPolicy{ OsTraceLevel::UserAction, "USER_ACTION", "\x1b[37m" },
        LevelPolicy{ OsTraceLevel::Error, "ERROR", "\x1b[31m" },
        LevelPolicy{ OsTraceLevel::Fault, "FAULT", "\x1b[31m" },
    };

    for ( const auto& policy : policies ) {
        auto record = formattingRecord();
        record.level = policy.level;
        const auto formatted
            = formatOsTraceRecord( record, OsTraceFormatOptions{ true, false, false } );
        const auto styledLevel = std::string{ "<" } + policy.color + policy.name + "\x1b[0m>";
        const auto styledMessage = std::string{ policy.color } + "bad \xef\xbf\xbd payload\x1b[0m";
        INFO( policy.name );
        CHECK( formatted.bytes.find( styledLevel ) != std::string::npos );
        CHECK( formatted.bytes.find( styledMessage ) != std::string::npos );
    }
}

TEST_CASE( "iOS formatter escapes terminal controls from every untrusted text field",
           "[livecapture][ios][ostrace][format][ansi][injection]" )
{
    auto record = formattingRecord();
    record.processPath = std::string{ "/tmp/proc\x1b[2J" };
    record.imagePath = std::string{ "/tmp/img\n" };
    record.message = std::string{ "line1\nline2 \x1b[31m " } + "\xc2\x9b" + "31m";
    record.subsystem = std::string{ "sub\r" };
    record.category = std::string{ "cat\t" };

    for ( const auto ansi : { false, true } ) {
        const auto formatted
            = formatOsTraceRecord( record, OsTraceFormatOptions{ ansi, true, true } );
        INFO( "ansi=" << ansi );
        CHECK( formatted.bytes.find( "proc\\x1b[2J" ) != std::string::npos );
        CHECK( formatted.bytes.find( "img\\n" ) != std::string::npos );
        CHECK( formatted.bytes.find( "line1\\nline2 \\x1b[31m \\u009b31m" ) != std::string::npos );
        CHECK( formatted.bytes.find( "[sub\\r][cat\\t]" ) != std::string::npos );
        CHECK( formatted.bytes.find( '\n' ) == std::string::npos );
        CHECK( formatted.bytes.find( '\r' ) == std::string::npos );
        CHECK( formatted.bytes.find( '\t' ) == std::string::npos );

        if ( !ansi ) {
            CHECK( formatted.bytes.find( '\x1b' ) == std::string::npos );
        }
    }
}

TEST_CASE( "iOS formatter keeps labels and image metadata optional without punctuation ghosts",
           "[livecapture][ios][ostrace][format][optional]" )
{
    auto record = formattingRecord();
    record.imagePath.reset();
    record.subsystem.reset();
    record.category.reset();

    const auto formatted = formatOsTraceRecord( record, OsTraceFormatOptions{ false, true, true } );
    CHECK( formatted.bytes
           == "2023-11-14 22:13:20.123456 Runner{}[4242] <ERROR>: bad \xef\xbf\xbd payload" );

    record.processPath.reset();
    record.message.reset();
    const auto missing = formatOsTraceRecord( record, OsTraceFormatOptions{ false, true, true } );
    CHECK( missing.bytes == "2023-11-14 22:13:20.123456 {}[4242] <ERROR>: " );
}

TEST_CASE(
    "iOS ANSI formatter follows the compatible severity color policy for every accepted level",
    "[livecapture][ios][ostrace][format][ansi][levels]" )
{
    struct LevelColor {
        OsTraceLevel level;
        const char* levelName;
        const char* ansiColor;
    };
    const std::array levels{
        LevelColor{ OsTraceLevel::Notice, "NOTICE", "\x1b[37m" },
        LevelColor{ OsTraceLevel::Info, "INFO", "\x1b[37m" },
        LevelColor{ OsTraceLevel::Debug, "DEBUG", "\x1b[32m" },
        LevelColor{ OsTraceLevel::Error, "ERROR", "\x1b[31m" },
        LevelColor{ OsTraceLevel::Fault, "FAULT", "\x1b[31m" },
    };

    for ( const auto& expected : levels ) {
        auto record = formattingRecord();
        record.level = expected.level;
        const auto formatted
            = formatOsTraceRecord( record, OsTraceFormatOptions{ true, true, true } );
        INFO( expected.levelName );
        CHECK( formatted.bytes.find( std::string{ expected.ansiColor } + expected.levelName
                                     + "\x1b[0m" )
               != std::string::npos );
        CHECK( formatted.bytes.find( std::string{ expected.ansiColor } + "bad \xef\xbf\xbd payload"
                                     + "\x1b[0m" )
               != std::string::npos );
    }
}

TEST_CASE( "libimobiledevice callback copy boundary is bounded and never unwinds into C",
           "[livecapture][ios][ostrace][ownership][bounds]" )
{
    std::vector<ByteBuffer> delivered;
    BorrowedActivityCallbackBridge bridge(
        [ &delivered ]( ByteBuffer owned ) { delivered.push_back( std::move( owned ) ); }, 4u );

    ByteBuffer exact{ 1u, 2u, 3u, 4u };
    CHECK( bridge( exact.data(), exact.size() ) == BorrowedActivityCopyResult::Delivered );
    CHECK( bridge( nullptr, 1u ) == BorrowedActivityCopyResult::InvalidBorrowedBuffer );
    CHECK( bridge( exact.data(), exact.size() + 1u )
           == BorrowedActivityCopyResult::BufferTooLarge );
    REQUIRE( delivered.size() == 1u );
    CHECK( delivered.front() == exact );

    BorrowedActivityCallbackBridge throwing(
        []( ByteBuffer ) { throw std::runtime_error( "callback failure" ); } );
    CHECK( throwing( exact.data(), exact.size() ) == BorrowedActivityCopyResult::CallbackFailed );
}

TEST_CASE(
    "libimobiledevice activity callback bytes are copied before the borrowed buffer is reused",
    "[livecapture][ios][ostrace][ownership]" )
{
    std::vector<ByteBuffer> delivered;
    BorrowedActivityCallbackBridge bridge(
        [ &delivered ]( ByteBuffer owned ) { delivered.push_back( std::move( owned ) ); } );

    ByteBuffer source{ 'f', 'i', 'r', 's', 't' };
    bridge( source.data(), source.size() );
    std::fill( source.begin(), source.end(), std::uint8_t{ 0xa5 } );
    source.assign( { 's', 'e', 'c', 'o', 'n', 'd' } );
    bridge( source.data(), source.size() );
    std::fill( source.begin(), source.end(), std::uint8_t{ 0x5a } );

    REQUIRE( delivered.size() == 2u );
    CHECK( byteString( delivered.at( 0 ) ) == "first" );
    CHECK( byteString( delivered.at( 1 ) ) == "second" );
}
