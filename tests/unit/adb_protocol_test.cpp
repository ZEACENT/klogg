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

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

#include "adbprotocol.h"

namespace {
using namespace klogg::livecapture::adb;

ByteVector bytes( const std::string& text )
{
    return { text.begin(), text.end() };
}

std::string text( const ByteVector& value )
{
    return { value.begin(), value.end() };
}

ByteVector concatenate( std::initializer_list<ByteVector> parts )
{
    ByteVector result;
    for ( const auto& part : parts ) {
        result.insert( result.end(), part.begin(), part.end() );
    }
    return result;
}

ByteVector hostFrame( const std::string& payload )
{
    REQUIRE( payload.size() <= 0xffffu );
    static constexpr std::array<char, 16> hexDigits{
        '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F',
    };
    const auto length = static_cast<std::uint16_t>( payload.size() );
    ByteVector frame{
        static_cast<std::uint8_t>( hexDigits.at( ( length >> 12u ) & 0x0fu ) ),
        static_cast<std::uint8_t>( hexDigits.at( ( length >> 8u ) & 0x0fu ) ),
        static_cast<std::uint8_t>( hexDigits.at( ( length >> 4u ) & 0x0fu ) ),
        static_cast<std::uint8_t>( hexDigits.at( length & 0x0fu ) ),
    };
    for ( const auto byte : payload ) {
        frame.push_back( static_cast<std::uint8_t>( byte ) );
    }
    return frame;
}

ByteVector shellFrame( std::uint8_t wireId, const ByteVector& payload )
{
    const auto length = static_cast<std::uint32_t>( payload.size() );
    ByteVector frame{
        wireId,
        static_cast<std::uint8_t>( length & 0xffu ),
        static_cast<std::uint8_t>( ( length >> 8u ) & 0xffu ),
        static_cast<std::uint8_t>( ( length >> 16u ) & 0xffu ),
        static_cast<std::uint8_t>( ( length >> 24u ) & 0xffu ),
    };
    frame.insert( frame.end(), payload.begin(), payload.end() );
    return frame;
}

} // namespace

TEST_CASE( "ADB smart-socket requests use an uppercase four-hex byte length",
           "[livecapture][adb][protocol]" )
{
    const auto shortRequest = encodeSmartSocketRequest( bytes( "host:version" ) );
    REQUIRE( shortRequest.value.has_value() );
    REQUIRE_FALSE( shortRequest.error.has_value() );
    CHECK( text( *shortRequest.value ) == "000Chost:version" );

    ByteVector payload( 0xabu, static_cast<std::uint8_t>( 'x' ) );
    payload.at( 1 ) = 0u;
    const auto binaryRequest = encodeSmartSocketRequest( payload );
    REQUIRE( binaryRequest.value.has_value() );
    REQUIRE_FALSE( binaryRequest.error.has_value() );
    REQUIRE( binaryRequest.value->size() == payload.size() + 4u );
    CHECK( text( ByteVector( binaryRequest.value->begin(), binaryRequest.value->begin() + 4 ) )
           == "00AB" );
    CHECK( ByteVector( binaryRequest.value->begin() + 4, binaryRequest.value->end() ) == payload );
}

TEST_CASE( "ADB smart-socket request lengths reject overflow and invalid hex",
           "[livecapture][adb][protocol][bounds]" )
{
    const auto largest = encodeSmartSocketRequest( ByteVector( 0xffffu, 0x5au ) );
    REQUIRE( largest.value.has_value() );
    REQUIRE_FALSE( largest.error.has_value() );
    CHECK( text( ByteVector( largest.value->begin(), largest.value->begin() + 4 ) ) == "FFFF" );

    const auto empty = encodeSmartSocketRequest( {} );
    REQUIRE_FALSE( empty.value.has_value() );
    REQUIRE( empty.error.has_value() );
    CHECK( empty.error->code == ProtocolErrorCode::EmptyPayload );

    const auto tooLarge = encodeSmartSocketRequest( ByteVector( 0x10000u, 0x5au ) );
    REQUIRE_FALSE( tooLarge.value.has_value() );
    REQUIRE( tooLarge.error.has_value() );
    CHECK( tooLarge.error->code == ProtocolErrorCode::PayloadTooLarge );

    const auto lowercase
        = parseSmartSocketHexLength( std::array<std::uint8_t, 4>{ '0', '0', 'a', 'f' } );
    REQUIRE( lowercase.value.has_value() );
    CHECK( *lowercase.value == 0xafu );

    const auto invalid
        = parseSmartSocketHexLength( std::array<std::uint8_t, 4>{ '0', '0', 'G', '1' } );
    REQUIRE_FALSE( invalid.value.has_value() );
    REQUIRE( invalid.error.has_value() );
    CHECK( invalid.error->code == ProtocolErrorCode::InvalidHexLength );
}

TEST_CASE( "ADB status decoder returns bytes coalesced after one fragmented OKAY",
           "[livecapture][adb][protocol][status]" )
{
    SmartSocketStatusDecoder decoder;

    auto result = decoder.feed( bytes( "O" ) );
    CHECK( result.frames.empty() );
    CHECK_FALSE( result.error.has_value() );
    CHECK( result.bufferedByteCount == 1u );
    CHECK( result.unconsumedBytes.empty() );

    result = decoder.feed( bytes( "KA" ) );
    CHECK( result.frames.empty() );
    CHECK_FALSE( result.error.has_value() );
    CHECK( result.bufferedByteCount == 3u );
    CHECK( result.unconsumedBytes.empty() );

    const auto hostReply = hostFrame( "0029" );
    result = decoder.feed( concatenate( { bytes( "Y" ), hostReply } ) );
    REQUIRE_FALSE( result.error.has_value() );
    REQUIRE( result.frames.size() == 1u );
    CHECK( result.frames.front().kind == SmartSocketStatusKind::Okay );
    CHECK( result.frames.front().message.empty() );
    CHECK( result.unconsumedBytes == hostReply );
    CHECK( result.bufferedByteCount == 0u );

    LengthPrefixedHostReplyDecoder hostDecoder;
    const auto hostResult = hostDecoder.feed( result.unconsumedBytes );
    REQUIRE_FALSE( hostResult.error.has_value() );
    REQUIRE( hostResult.frames.size() == 1u );
    CHECK( text( hostResult.frames.front() ) == "0029" );
}

TEST_CASE( "ADB status decoder hands coalesced shell-v2 bytes to the frame decoder",
           "[livecapture][adb][protocol][status][shell-v2]" )
{
    SmartSocketStatusDecoder statusDecoder;
    const auto shellPacket = shellFrame( 1u, bytes( "line\n" ) );
    const auto status = statusDecoder.feed( concatenate( { bytes( "OKAY" ), shellPacket } ) );

    REQUIRE_FALSE( status.error.has_value() );
    REQUIRE( status.frames.size() == 1u );
    CHECK( status.unconsumedBytes == shellPacket );

    ShellV2FrameDecoder shellDecoder;
    const auto shell = shellDecoder.feed( status.unconsumedBytes );
    REQUIRE_FALSE( shell.error.has_value() );
    REQUIRE( shell.frames.size() == 1u );
    CHECK( shell.frames.front().channel == ShellV2Channel::Stdout );
    CHECK( text( shell.frames.front().payload ) == "line\n" );
}

TEST_CASE( "ADB status decoder waits for a complete fragmented FAIL message",
           "[livecapture][adb][protocol][status]" )
{
    SmartSocketStatusDecoder decoder;

    auto result = decoder.feed( bytes( "FA" ) );
    CHECK( result.frames.empty() );
    CHECK_FALSE( result.error.has_value() );

    result = decoder.feed( bytes( "IL000Bdevice " ) );
    CHECK( result.frames.empty() );
    CHECK_FALSE( result.error.has_value() );
    CHECK( result.bufferedByteCount == 15u );

    result = decoder.feed( bytes( "busytrailing" ) );
    REQUIRE_FALSE( result.error.has_value() );
    REQUIRE( result.frames.size() == 1u );
    CHECK( result.frames.front().kind == SmartSocketStatusKind::Fail );
    CHECK( text( result.frames.front().message ) == "device busy" );
    CHECK( text( result.unconsumedBytes ) == "trailing" );
    CHECK( result.bufferedByteCount == 0u );
}

TEST_CASE( "ADB status decoder rejects malformed status and FAIL lengths without over-reading",
           "[livecapture][adb][protocol][status][bounds]" )
{
    SECTION( "unknown status" )
    {
        SmartSocketStatusDecoder decoder;
        const auto result = decoder.feed( bytes( "NOPE" ) );
        REQUIRE( result.frames.empty() );
        REQUIRE( result.error.has_value() );
        CHECK( result.error->code == ProtocolErrorCode::InvalidStatus );
    }

    SECTION( "invalid FAIL hex" )
    {
        SmartSocketStatusDecoder decoder;
        const auto result = decoder.feed( bytes( "FAIL00G1" ) );
        REQUIRE( result.frames.empty() );
        REQUIRE( result.error.has_value() );
        CHECK( result.error->code == ProtocolErrorCode::InvalidHexLength );
    }

    SECTION( "advertised FAIL message exceeds configured bound" )
    {
        SmartSocketStatusDecoder decoder( 8u );
        const auto result = decoder.feed( bytes( "FAIL0009" ) );
        REQUIRE( result.frames.empty() );
        REQUIRE( result.error.has_value() );
        CHECK( result.error->code == ProtocolErrorCode::FrameTooLarge );
        CHECK( result.bufferedByteCount <= 8u );
    }
}

TEST_CASE( "length-prefixed host reply decoder handles fragmented host:version replies",
           "[livecapture][adb][protocol][host]" )
{
    LengthPrefixedHostReplyDecoder decoder;
    const auto wire = hostFrame( "0029" );

    for ( std::size_t i = 0; i + 1u < wire.size(); ++i ) {
        const auto result = decoder.feed( ByteVector{ wire.at( i ) } );
        CHECK( result.frames.empty() );
        CHECK_FALSE( result.error.has_value() );
        CHECK( result.bufferedByteCount == i + 1u );
    }

    const auto result = decoder.feed( ByteVector{ wire.back() } );
    REQUIRE_FALSE( result.error.has_value() );
    REQUIRE( result.frames.size() == 1u );
    CHECK( text( result.frames.front() ) == "0029" );
    CHECK( result.bufferedByteCount == 0u );
}

TEST_CASE( "length-prefixed host reply decoder preserves host:features payload bytes",
           "[livecapture][adb][protocol][host]" )
{
    LengthPrefixedHostReplyDecoder decoder;
    const auto result = decoder.feed( hostFrame( "shell_v2,cmd,stat_v2,apex,fixed_push_mkdir" ) );

    REQUIRE_FALSE( result.error.has_value() );
    REQUIRE( result.frames.size() == 1u );
    CHECK( text( result.frames.front() ) == "shell_v2,cmd,stat_v2,apex,fixed_push_mkdir" );
    CHECK( result.bufferedByteCount == 0u );
}

TEST_CASE( "length-prefixed host reply decoder preserves devices-l snapshots",
           "[livecapture][adb][protocol][host][devices]" )
{
    LengthPrefixedHostReplyDecoder decoder;
    const std::string snapshot
        = "emulator-5554\tdevice product:sdk_gphone64_arm64 model:sdk_gphone64 "
          "device:emu64a transport_id:1\n"
          "R58M123ABCD\toffline transport_id:2\n";
    const auto result = decoder.feed( hostFrame( snapshot ) );

    REQUIRE_FALSE( result.error.has_value() );
    REQUIRE( result.frames.size() == 1u );
    CHECK( text( result.frames.front() ) == snapshot );
}

TEST_CASE(
    "length-prefixed host reply decoder preserves partial and multiple track-devices-l frames",
    "[livecapture][adb][protocol][host][devices]" )
{
    LengthPrefixedHostReplyDecoder decoder;
    const auto first = hostFrame( "emulator-5554\tdevice transport_id:1\n" );
    const auto second = hostFrame( "emulator-5554\toffline transport_id:1\n" );
    const auto split = second.begin() + 7;

    auto result = decoder.feed( concatenate( { first, ByteVector( second.begin(), split ) } ) );
    REQUIRE_FALSE( result.error.has_value() );
    REQUIRE( result.frames.size() == 1u );
    CHECK( text( result.frames.front() ) == "emulator-5554\tdevice transport_id:1\n" );
    CHECK( result.bufferedByteCount == 7u );

    result = decoder.feed( concatenate( { ByteVector( split, second.end() ), hostFrame( "" ) } ) );
    REQUIRE_FALSE( result.error.has_value() );
    REQUIRE( result.frames.size() == 2u );
    CHECK( text( result.frames.at( 0 ) ) == "emulator-5554\toffline transport_id:1\n" );
    CHECK( result.frames.at( 1 ).empty() );
    CHECK( result.bufferedByteCount == 0u );
}

TEST_CASE( "length-prefixed host reply decoder rejects invalid and oversized frames",
           "[livecapture][adb][protocol][host][bounds]" )
{
    SECTION( "invalid hex" )
    {
        LengthPrefixedHostReplyDecoder decoder;
        const auto result = decoder.feed( bytes( "00Z1" ) );
        REQUIRE( result.frames.empty() );
        REQUIRE( result.error.has_value() );
        CHECK( result.error->code == ProtocolErrorCode::InvalidHexLength );
    }

    SECTION( "configured payload bound" )
    {
        LengthPrefixedHostReplyDecoder decoder( 8u );
        const auto result = decoder.feed( bytes( "0009" ) );
        REQUIRE( result.frames.empty() );
        REQUIRE( result.error.has_value() );
        CHECK( result.error->code == ProtocolErrorCode::FrameTooLarge );
        CHECK( result.bufferedByteCount <= 4u );
    }
}

TEST_CASE( "shell-v2 decoder uses one-byte channels and little-endian uint32 lengths",
           "[livecapture][adb][protocol][shell-v2]" )
{
    ShellV2FrameDecoder decoder;
    const ByteVector wire{ 1u, 3u, 0u, 0u, 0u, 'a', 0u, 'b' };

    for ( std::size_t i = 0; i + 1u < wire.size(); ++i ) {
        const auto result = decoder.feed( ByteVector{ wire.at( i ) } );
        CHECK( result.frames.empty() );
        CHECK_FALSE( result.error.has_value() );
    }

    const auto result = decoder.feed( ByteVector{ wire.back() } );
    REQUIRE_FALSE( result.error.has_value() );
    REQUIRE( result.frames.size() == 1u );
    CHECK( result.frames.front().channel == ShellV2Channel::Stdout );
    CHECK( result.frames.front().wireId == 1u );
    const ByteVector expectedPayload{ 'a', 0u, 'b' };
    CHECK( result.frames.front().payload == expectedPayload );
    CHECK( result.bufferedByteCount == 0u );
}

TEST_CASE( "shell-v2 decoder preserves empty and multiple stdout stderr exit and control frames",
           "[livecapture][adb][protocol][shell-v2]" )
{
    ShellV2FrameDecoder decoder;
    const auto wire = concatenate( {
        shellFrame( 1u, {} ),
        shellFrame( 1u, bytes( "out\n" ) ),
        shellFrame( 2u, bytes( "warning\n" ) ),
        shellFrame( 3u, ByteVector{ 23u } ),
        shellFrame( 4u, {} ),
        shellFrame( 5u, bytes( "80x24" ) ),
    } );
    const auto result = decoder.feed( wire );

    REQUIRE_FALSE( result.error.has_value() );
    REQUIRE( result.frames.size() == 6u );
    CHECK( result.frames.at( 0 ).channel == ShellV2Channel::Stdout );
    CHECK( result.frames.at( 0 ).payload.empty() );
    CHECK( result.frames.at( 1 ).channel == ShellV2Channel::Stdout );
    CHECK( text( result.frames.at( 1 ).payload ) == "out\n" );
    CHECK( result.frames.at( 2 ).channel == ShellV2Channel::Stderr );
    CHECK( text( result.frames.at( 2 ).payload ) == "warning\n" );
    CHECK( result.frames.at( 3 ).channel == ShellV2Channel::Exit );
    CHECK( result.frames.at( 3 ).payload == ByteVector{ 23u } );
    CHECK( result.frames.at( 4 ).channel == ShellV2Channel::CloseStdin );
    CHECK( result.frames.at( 4 ).payload.empty() );
    CHECK( result.frames.at( 5 ).channel == ShellV2Channel::WindowSizeChange );
    CHECK( text( result.frames.at( 5 ).payload ) == "80x24" );
    CHECK( result.bufferedByteCount == 0u );
}

TEST_CASE( "shell-v2 decoder emits complete frames while preserving a following partial frame",
           "[livecapture][adb][protocol][shell-v2]" )
{
    ShellV2FrameDecoder decoder;
    const auto first = shellFrame( 1u, bytes( "first" ) );
    const auto second = shellFrame( 2u, bytes( "second" ) );
    const auto split = second.begin() + 6;

    auto result = decoder.feed( concatenate( { first, ByteVector( second.begin(), split ) } ) );
    REQUIRE_FALSE( result.error.has_value() );
    REQUIRE( result.frames.size() == 1u );
    CHECK( text( result.frames.front().payload ) == "first" );
    CHECK( result.bufferedByteCount == 6u );

    result = decoder.feed( ByteVector( split, second.end() ) );
    REQUIRE_FALSE( result.error.has_value() );
    REQUIRE( result.frames.size() == 1u );
    CHECK( result.frames.front().channel == ShellV2Channel::Stderr );
    CHECK( text( result.frames.front().payload ) == "second" );
    CHECK( result.bufferedByteCount == 0u );
}

TEST_CASE( "shell-v2 decoder rejects unknown channels and oversized uint32 lengths safely",
           "[livecapture][adb][protocol][shell-v2][bounds]" )
{
    SECTION( "unknown channel" )
    {
        ShellV2FrameDecoder decoder;
        const auto result = decoder.feed( ByteVector{ 0x7fu, 0u, 0u, 0u, 0u } );
        REQUIRE( result.frames.empty() );
        REQUIRE( result.error.has_value() );
        CHECK( result.error->code == ProtocolErrorCode::UnknownShellV2Channel );
    }

    SECTION( "maximum wire length cannot overflow allocation arithmetic" )
    {
        ShellV2FrameDecoder decoder( 1024u );
        const auto result = decoder.feed( ByteVector{ 1u, 0xffu, 0xffu, 0xffu, 0xffu } );
        REQUIRE( result.frames.empty() );
        REQUIRE( result.error.has_value() );
        CHECK( result.error->code == ProtocolErrorCode::FrameTooLarge );
        CHECK( result.bufferedByteCount <= 5u );
    }

    SECTION( "exit packets contain exactly one status byte" )
    {
        for ( const auto& payload : { ByteVector{}, ByteVector{ 1u, 2u } } ) {
            ShellV2FrameDecoder decoder;
            const auto result = decoder.feed( shellFrame( 3u, payload ) );
            REQUIRE( result.frames.empty() );
            REQUIRE( result.error.has_value() );
            CHECK( result.error->code == ProtocolErrorCode::InvalidShellV2Frame );
        }
    }

    SECTION( "close-stdin packets contain no payload" )
    {
        ShellV2FrameDecoder decoder;
        const auto result = decoder.feed( shellFrame( 4u, ByteVector{ 1u } ) );
        REQUIRE( result.frames.empty() );
        REQUIRE( result.error.has_value() );
        CHECK( result.error->code == ProtocolErrorCode::InvalidShellV2Frame );
    }
}

TEST_CASE( "ADB decoders keep fatal errors sticky until reset starts a new stream",
           "[livecapture][adb][protocol][reset]" )
{
    SECTION( "status stream" )
    {
        SmartSocketStatusDecoder decoder;
        const auto malformed = decoder.feed( bytes( "NOPE" ) );
        REQUIRE( malformed.error.has_value() );
        CHECK( malformed.error->code == ProtocolErrorCode::InvalidStatus );

        const auto sticky = decoder.feed( bytes( "OKAY" ) );
        CHECK( sticky.frames.empty() );
        REQUIRE( sticky.error.has_value() );
        CHECK( sticky.error->code == ProtocolErrorCode::InvalidStatus );

        decoder.reset();
        CHECK( decoder.feed( bytes( "O" ) ).bufferedByteCount == 1u );
        decoder.reset();
        const auto recovered = decoder.feed( bytes( "OKAY" ) );
        REQUIRE_FALSE( recovered.error.has_value() );
        REQUIRE( recovered.frames.size() == 1u );
        CHECK( recovered.frames.front().kind == SmartSocketStatusKind::Okay );
    }

    SECTION( "host reply stream" )
    {
        LengthPrefixedHostReplyDecoder decoder;
        const auto malformed = decoder.feed( bytes( "00Z1" ) );
        REQUIRE( malformed.error.has_value() );
        CHECK( malformed.error->code == ProtocolErrorCode::InvalidHexLength );

        const auto sticky = decoder.feed( hostFrame( "ignored" ) );
        CHECK( sticky.frames.empty() );
        REQUIRE( sticky.error.has_value() );
        CHECK( sticky.error->code == ProtocolErrorCode::InvalidHexLength );

        decoder.reset();
        CHECK( decoder.feed( bytes( "0004ab" ) ).bufferedByteCount == 6u );
        decoder.reset();
        const auto recovered = decoder.feed( hostFrame( "ready" ) );
        REQUIRE_FALSE( recovered.error.has_value() );
        REQUIRE( recovered.frames.size() == 1u );
        CHECK( text( recovered.frames.front() ) == "ready" );
    }

    SECTION( "shell-v2 stream" )
    {
        ShellV2FrameDecoder decoder;
        const auto malformed = decoder.feed( ByteVector{ 0x7fu, 0u, 0u, 0u, 0u } );
        REQUIRE( malformed.error.has_value() );
        CHECK( malformed.error->code == ProtocolErrorCode::UnknownShellV2Channel );

        const auto sticky = decoder.feed( shellFrame( 1u, bytes( "ignored" ) ) );
        CHECK( sticky.frames.empty() );
        REQUIRE( sticky.error.has_value() );
        CHECK( sticky.error->code == ProtocolErrorCode::UnknownShellV2Channel );

        decoder.reset();
        CHECK( decoder.feed( ByteVector{ 1u, 3u } ).bufferedByteCount == 2u );
        decoder.reset();
        const auto recovered = decoder.feed( shellFrame( 1u, bytes( "ready" ) ) );
        REQUIRE_FALSE( recovered.error.has_value() );
        REQUIRE( recovered.frames.size() == 1u );
        CHECK( text( recovered.frames.front().payload ) == "ready" );
    }
}

TEST_CASE( "ADB host and transport service builders produce exact smart-socket text",
           "[livecapture][adb][protocol][command]" )
{
    const auto version = buildHostService( HostService::Version );
    const auto serverFeatures = buildHostService( HostService::ServerFeatures );
    const auto devices = buildHostService( HostService::DevicesLong );
    const auto trackDevices = buildHostService( HostService::TrackDevicesLong );
    REQUIRE( version.value.has_value() );
    REQUIRE( serverFeatures.value.has_value() );
    REQUIRE( devices.value.has_value() );
    REQUIRE( trackDevices.value.has_value() );
    CHECK( *version.value == "host:version" );
    CHECK( *serverFeatures.value == "host:host-features" );
    CHECK( *devices.value == "host:devices-l" );
    CHECK( *trackDevices.value == "host:track-devices-l" );

    const auto invalidHost = buildHostService( static_cast<HostService>( 0xffu ) );
    REQUIRE_FALSE( invalidHost.value.has_value() );
    REQUIRE( invalidHost.error.has_value() );
    CHECK( invalidHost.error->code == ProtocolErrorCode::InvalidCommandOption );

    const auto transportFeatures = buildTransportHostService( TransportHostService::Features );
    REQUIRE( transportFeatures.value.has_value() );
    REQUIRE_FALSE( transportFeatures.error.has_value() );
    CHECK( *transportFeatures.value == "host:features" );

    const auto invalidTransportService
        = buildTransportHostService( static_cast<TransportHostService>( 0xffu ) );
    REQUIRE_FALSE( invalidTransportService.value.has_value() );
    REQUIRE( invalidTransportService.error.has_value() );
    CHECK( invalidTransportService.error->code == ProtocolErrorCode::InvalidCommandOption );

    const auto any = buildTransportService( TransportSelection{ TransportKind::Any, {} } );
    const auto usb = buildTransportService( TransportSelection{ TransportKind::Usb, {} } );
    const auto local = buildTransportService( TransportSelection{ TransportKind::Local, {} } );
    const auto serial
        = buildTransportService( TransportSelection{ TransportKind::Serial, "emulator-5554" } );
    const auto colonSerial
        = buildTransportService( TransportSelection{ TransportKind::Serial, "foo:bar" } );

    REQUIRE( any.value.has_value() );
    REQUIRE( usb.value.has_value() );
    REQUIRE( local.value.has_value() );
    REQUIRE( serial.value.has_value() );
    REQUIRE( colonSerial.value.has_value() );
    CHECK( *any.value == "host:transport-any" );
    CHECK( *usb.value == "host:transport-usb" );
    CHECK( *local.value == "host:transport-local" );
    CHECK( *serial.value == "host:transport:emulator-5554" );
    CHECK( *colonSerial.value == "host:transport:foo:bar" );

    const auto missingSerial
        = buildTransportService( TransportSelection{ TransportKind::Serial, {} } );
    REQUIRE_FALSE( missingSerial.value.has_value() );
    REQUIRE( missingSerial.error.has_value() );
    CHECK( missingSerial.error->code == ProtocolErrorCode::InvalidCommandOption );

    const auto embeddedNul = buildTransportService(
        TransportSelection{ TransportKind::Serial, std::string( "serial\0hidden", 13u ) } );
    REQUIRE_FALSE( embeddedNul.value.has_value() );
    REQUIRE( embeddedNul.error.has_value() );
    CHECK( embeddedNul.error->code == ProtocolErrorCode::InvalidCommandOption );

    const auto invalidKind = buildTransportService(
        TransportSelection{ static_cast<TransportKind>( 0xffu ), "emulator-5554" } );
    REQUIRE_FALSE( invalidKind.value.has_value() );
    REQUIRE( invalidKind.error.has_value() );
    CHECK( invalidKind.error->code == ProtocolErrorCode::InvalidCommandOption );

    constexpr std::size_t MaxRequestSize = 0xffffu;
    const std::string prefix{ "host:transport:" };
    const auto longestSerial = std::string( MaxRequestSize - prefix.size(), 's' );
    const auto largest
        = buildTransportService( TransportSelection{ TransportKind::Serial, longestSerial } );
    REQUIRE( largest.value.has_value() );
    CHECK( largest.value->size() == MaxRequestSize );

    const auto oversized
        = buildTransportService( TransportSelection{ TransportKind::Serial, longestSerial + "s" } );
    REQUIRE_FALSE( oversized.value.has_value() );
    REQUIRE( oversized.error.has_value() );
    CHECK( oversized.error->code == ProtocolErrorCode::PayloadTooLarge );
}

TEST_CASE( "typed ADB logcat command builder owns ordered source-device wall-time modifiers",
           "[livecapture][adb][protocol][command][wall-time]" )
{
    const auto defaults = buildLogcatService( LogcatCommandOptions{} );
    REQUIRE( defaults.value.has_value() );
    REQUIRE_FALSE( defaults.error.has_value() );
    CHECK( *defaults.value
           == "shell,v2,raw:logcat -v threadtime -v year -v zone -v usec" );

    LogcatCommandOptions options;
    options.ansiOutputEnabled = true;
    options.buffers = { LogBuffer::Main, LogBuffer::System, LogBuffer::Crash };
    options.initialTailLineCount = 25u;
    options.processId = 4242u;
    options.filters = {
        LogcatFilter{ "ActivityManager", LogPriority::Info },
        LogcatFilter{ "*", LogPriority::Silent },
    };
    const auto structured = buildLogcatService( options );
    REQUIRE( structured.value.has_value() );
    REQUIRE_FALSE( structured.error.has_value() );
    CHECK( *structured.value
           == "shell,v2,raw:logcat -v threadtime -v year -v zone -v usec -v color -b main "
              "-b system -b crash -T 25 --pid 4242 'ActivityManager:I' '*:S'" );

    CHECK( buildClearLogcatService() == "shell,v2,raw:logcat -c" );
}

TEST_CASE( "ADB logcat diagnostics classify only rejected owned format modifiers",
           "[livecapture][adb][protocol][command][wall-time][diagnostic]" )
{
    const std::string unsupported{ "Invalid parameter year to -v\n" };
    const auto normalized = normalizeLogcatStreamError( unsupported );
    CHECK( normalized.find( "Android 7.0" ) != std::string::npos );
    CHECK( normalized.find( "source-device wall time" ) != std::string::npos );
    CHECK( normalized.find( unsupported ) != std::string::npos );

    const std::string unrelated{
        "Invalid parameter nope to -r\nusage: logcat [options]\n  -v <format>\n"
    };
    CHECK( normalizeLogcatStreamError( unrelated ) == unrelated );
}

TEST_CASE( "ADB logcat command builder shell-quotes filter values without interpolation",
           "[livecapture][adb][protocol][command][security]" )
{
    LogcatCommandOptions options;
    options.ansiOutputEnabled = true;
    options.processId = 4294967295u;
    options.filters = {
        LogcatFilter{ "Activity Manager'$(reboot)", LogPriority::Info },
    };

    const auto command = buildLogcatService( options );
    REQUIRE( command.value.has_value() );
    REQUIRE_FALSE( command.error.has_value() );
    CHECK( *command.value
           == "shell,v2,raw:logcat -v threadtime -v year -v zone -v usec -v color "
              "--pid 4294967295 'Activity Manager'\"'\"'$(reboot):I'" );
}

TEST_CASE( "ADB logcat command builder rejects options that cannot be represented safely",
           "[livecapture][adb][protocol][command][security]" )
{
    SECTION( "embedded NUL" )
    {
        LogcatCommandOptions options;
        options.filters = {
            LogcatFilter{ std::string( "tag\0hidden", 10u ), LogPriority::Info },
        };

        const auto command = buildLogcatService( options );
        REQUIRE_FALSE( command.value.has_value() );
        REQUIRE( command.error.has_value() );
        CHECK( command.error->code == ProtocolErrorCode::InvalidCommandOption );
    }

    SECTION( "option-shaped filter tag" )
    {
        for ( const auto& tag :
              { std::string{ "-b" }, std::string{ "--pid=1" }, std::string{ "--help" } } ) {
            LogcatCommandOptions options;
            options.filters = { LogcatFilter{ tag, LogPriority::Info } };

            const auto command = buildLogcatService( options );
            REQUIRE_FALSE( command.value.has_value() );
            REQUIRE( command.error.has_value() );
            CHECK( command.error->code == ProtocolErrorCode::InvalidCommandOption );
        }
    }

    SECTION( "unknown buffer" )
    {
        LogcatCommandOptions options;
        options.buffers = { static_cast<LogBuffer>( 0xffu ) };

        const auto command = buildLogcatService( options );
        REQUIRE_FALSE( command.value.has_value() );
        REQUIRE( command.error.has_value() );
        CHECK( command.error->code == ProtocolErrorCode::InvalidCommandOption );
    }

    SECTION( "unknown priority" )
    {
        LogcatCommandOptions options;
        options.filters = {
            LogcatFilter{ "ActivityManager", static_cast<LogPriority>( 0xffu ) },
        };

        const auto command = buildLogcatService( options );
        REQUIRE_FALSE( command.value.has_value() );
        REQUIRE( command.error.has_value() );
        CHECK( command.error->code == ProtocolErrorCode::InvalidCommandOption );
    }
}
