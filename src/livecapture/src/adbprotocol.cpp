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

#include "adbprotocol.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <string_view>
#include <utility>

namespace klogg::livecapture::adb {
namespace {

constexpr std::size_t SmartSocketLengthSize = 4u;
constexpr std::size_t ShellV2HeaderSize = 5u;
constexpr std::size_t MaxSmartSocketPayloadSize = 0xffffu;

ProtocolError makeError( ProtocolErrorCode code, std::string message )
{
    return ProtocolError{ code, std::move( message ) };
}

std::optional<std::uint8_t> hexNibble( std::uint8_t encoded )
{
    const auto zero = static_cast<std::uint8_t>( '0' );
    const auto nine = static_cast<std::uint8_t>( '9' );
    if ( encoded >= zero && encoded <= nine ) {
        return static_cast<std::uint8_t>( encoded - zero );
    }

    const auto upperA = static_cast<std::uint8_t>( 'A' );
    const auto upperF = static_cast<std::uint8_t>( 'F' );
    if ( encoded >= upperA && encoded <= upperF ) {
        return static_cast<std::uint8_t>( encoded - upperA + 10u );
    }

    const auto lowerA = static_cast<std::uint8_t>( 'a' );
    const auto lowerF = static_cast<std::uint8_t>( 'f' );
    if ( encoded >= lowerA && encoded <= lowerF ) {
        return static_cast<std::uint8_t>( encoded - lowerA + 10u );
    }

    return std::nullopt;
}

std::uint8_t upperHexDigit( std::uint8_t nibble )
{
    static constexpr std::array<std::uint8_t, 16> Digits{
        static_cast<std::uint8_t>( '0' ), static_cast<std::uint8_t>( '1' ),
        static_cast<std::uint8_t>( '2' ), static_cast<std::uint8_t>( '3' ),
        static_cast<std::uint8_t>( '4' ), static_cast<std::uint8_t>( '5' ),
        static_cast<std::uint8_t>( '6' ), static_cast<std::uint8_t>( '7' ),
        static_cast<std::uint8_t>( '8' ), static_cast<std::uint8_t>( '9' ),
        static_cast<std::uint8_t>( 'A' ), static_cast<std::uint8_t>( 'B' ),
        static_cast<std::uint8_t>( 'C' ), static_cast<std::uint8_t>( 'D' ),
        static_cast<std::uint8_t>( 'E' ), static_cast<std::uint8_t>( 'F' ),
    };
    return Digits.at( nibble );
}

bool appendInput( ByteVector& buffer, const ByteVector& bytes, std::optional<ProtocolError>& error )
{
    if ( bytes.size() > buffer.max_size() - buffer.size() ) {
        buffer.clear();
        error = makeError( ProtocolErrorCode::BufferOverflow,
                           "Protocol input exceeds the decoder buffer capacity." );
        return false;
    }

    buffer.insert( buffer.end(), bytes.begin(), bytes.end() );
    return true;
}

ByteVector copyRange( const ByteVector& bytes, std::size_t first, std::size_t last )
{
    const auto beginOffset = static_cast<ByteVector::difference_type>( first );
    const auto endOffset = static_cast<ByteVector::difference_type>( last );
    return { bytes.begin() + beginOffset, bytes.begin() + endOffset };
}

void discardPrefix( ByteVector& buffer, std::size_t byteCount )
{
    if ( byteCount == 0u ) {
        return;
    }

    const auto offset = static_cast<ByteVector::difference_type>( byteCount );
    buffer.erase( buffer.begin(), buffer.begin() + offset );
}

bool matchesStatus( const ByteVector& buffer, std::size_t offset, std::string_view status )
{
    for ( std::size_t index = 0; index < status.size(); ++index ) {
        if ( buffer.at( offset + index ) != static_cast<std::uint8_t>( status.at( index ) ) ) {
            return false;
        }
    }
    return true;
}

std::array<std::uint8_t, 4> lengthAt( const ByteVector& buffer, std::size_t offset )
{
    return { buffer.at( offset ), buffer.at( offset + 1u ), buffer.at( offset + 2u ),
             buffer.at( offset + 3u ) };
}

std::optional<ShellV2Channel> shellChannel( std::uint8_t wireId )
{
    switch ( wireId ) {
    case 0u:
        return ShellV2Channel::Stdin;
    case 1u:
        return ShellV2Channel::Stdout;
    case 2u:
        return ShellV2Channel::Stderr;
    case 3u:
        return ShellV2Channel::Exit;
    case 4u:
        return ShellV2Channel::CloseStdin;
    case 5u:
        return ShellV2Channel::WindowSizeChange;
    default:
        return std::nullopt;
    }
}

std::uint64_t shellPayloadLength( const ByteVector& buffer, std::size_t offset )
{
    const auto byte0 = static_cast<std::uint64_t>( buffer.at( offset + 1u ) );
    const auto byte1 = static_cast<std::uint64_t>( buffer.at( offset + 2u ) );
    const auto byte2 = static_cast<std::uint64_t>( buffer.at( offset + 3u ) );
    const auto byte3 = static_cast<std::uint64_t>( buffer.at( offset + 4u ) );
    return byte0 | ( byte1 << 8u ) | ( byte2 << 16u ) | ( byte3 << 24u );
}

bool hasValidShellPayloadSize( ShellV2Channel channel, std::uint64_t payloadLength )
{
    switch ( channel ) {
    case ShellV2Channel::Exit:
        return payloadLength == 1u;
    case ShellV2Channel::CloseStdin:
        return payloadLength == 0u;
    case ShellV2Channel::Stdin:
    case ShellV2Channel::Stdout:
    case ShellV2Channel::Stderr:
    case ShellV2Channel::WindowSizeChange:
        return true;
    }

    return false;
}

template <typename Frame>
DecoderFeedResult<Frame> failedFeed( const std::optional<ProtocolError>& error,
                                     std::size_t bufferedByteCount )
{
    DecoderFeedResult<Frame> result;
    result.error = error;
    result.bufferedByteCount = bufferedByteCount;
    return result;
}

template <typename Frame>
void failDecoder( DecoderFeedResult<Frame>& result, ByteVector& buffer,
                  std::optional<ProtocolError>& decoderError, ProtocolError error )
{
    decoderError = std::move( error );
    result.error = decoderError;
    buffer.clear();
    result.bufferedByteCount = 0u;
}

bool appendBounded( std::string& output, std::string_view fragment )
{
    if ( output.size() > MaxSmartSocketPayloadSize
         || fragment.size() > MaxSmartSocketPayloadSize - output.size() ) {
        return false;
    }
    output.append( fragment.data(), fragment.size() );
    return true;
}

bool hasUnrepresentableCommandByte( std::string_view value )
{
    return std::find( value.begin(), value.end(), '\0' ) != value.end();
}

bool appendShellQuoted( std::string& output, std::string_view value )
{
    if ( hasUnrepresentableCommandByte( value ) || !appendBounded( output, "'" ) ) {
        return false;
    }

    for ( const auto character : value ) {
        const auto fragment = character == '\'' ? std::string_view{ "'\"'\"'" }
                                                : std::string_view{ &character, 1u };
        if ( !appendBounded( output, fragment ) ) {
            return false;
        }
    }

    return appendBounded( output, "'" );
}

std::optional<std::string_view> logBufferName( LogBuffer buffer )
{
    switch ( buffer ) {
    case LogBuffer::Main:
        return "main";
    case LogBuffer::System:
        return "system";
    case LogBuffer::Radio:
        return "radio";
    case LogBuffer::Events:
        return "events";
    case LogBuffer::Crash:
        return "crash";
    }

    return std::nullopt;
}

std::optional<char> logPriorityLetter( LogPriority priority )
{
    switch ( priority ) {
    case LogPriority::Verbose:
        return 'V';
    case LogPriority::Debug:
        return 'D';
    case LogPriority::Info:
        return 'I';
    case LogPriority::Warn:
        return 'W';
    case LogPriority::Error:
        return 'E';
    case LogPriority::Fatal:
        return 'F';
    case LogPriority::Silent:
        return 'S';
    }

    return std::nullopt;
}

bool lineRejectsOwnedLogcatFormat( std::string_view line )
{
    std::string normalized( line );
    std::transform( normalized.begin(), normalized.end(), normalized.begin(), []( char value ) {
        return static_cast<char>( std::tolower( static_cast<unsigned char>( value ) ) );
    } );

    if ( normalized.find( "invalid parameter" ) == std::string::npos
         || normalized.find( "to -v" ) == std::string::npos ) {
        return false;
    }

    constexpr std::array<std::string_view, 4> OwnedModifiers{ "year", "zone", "usec", "color" };
    return std::any_of( OwnedModifiers.cbegin(), OwnedModifiers.cend(),
                        [ &normalized ]( std::string_view modifier ) {
                            return normalized.find( modifier ) != std::string::npos;
                        } );
}

ProtocolResult<std::string> commandError( ProtocolErrorCode code, std::string message )
{
    return ProtocolResult<std::string>{ std::nullopt, makeError( code, std::move( message ) ) };
}

} // namespace

ProtocolResult<ByteVector> encodeSmartSocketRequest( const ByteVector& payload )
{
    if ( payload.empty() ) {
        return ProtocolResult<ByteVector>{
            std::nullopt,
            makeError( ProtocolErrorCode::EmptyPayload,
                       "ADB smart-socket requests require a non-empty service payload." )
        };
    }
    if ( payload.size() > MaxSmartSocketPayloadSize ) {
        return ProtocolResult<ByteVector>{
            std::nullopt,
            makeError( ProtocolErrorCode::PayloadTooLarge,
                       "ADB smart-socket payload exceeds the four-hex-digit length limit." )
        };
    }

    const auto length = static_cast<std::uint16_t>( payload.size() );
    ByteVector encoded;
    encoded.reserve( SmartSocketLengthSize + payload.size() );
    encoded.push_back( upperHexDigit( static_cast<std::uint8_t>( ( length >> 12u ) & 0x0fu ) ) );
    encoded.push_back( upperHexDigit( static_cast<std::uint8_t>( ( length >> 8u ) & 0x0fu ) ) );
    encoded.push_back( upperHexDigit( static_cast<std::uint8_t>( ( length >> 4u ) & 0x0fu ) ) );
    encoded.push_back( upperHexDigit( static_cast<std::uint8_t>( length & 0x0fu ) ) );
    encoded.insert( encoded.end(), payload.begin(), payload.end() );
    return ProtocolResult<ByteVector>{ std::move( encoded ), std::nullopt };
}

ProtocolResult<std::uint16_t>
parseSmartSocketHexLength( const std::array<std::uint8_t, 4>& encodedLength )
{
    std::uint16_t value = 0u;
    for ( const auto encoded : encodedLength ) {
        const auto nibble = hexNibble( encoded );
        if ( !nibble.has_value() ) {
            return ProtocolResult<std::uint16_t>{
                std::nullopt,
                makeError( ProtocolErrorCode::InvalidHexLength,
                           "ADB smart-socket length contains a non-hexadecimal byte." )
            };
        }
        value = static_cast<std::uint16_t>( ( value << 4u ) | *nibble );
    }
    return ProtocolResult<std::uint16_t>{ value, std::nullopt };
}

SmartSocketStatusDecoder::SmartSocketStatusDecoder( std::size_t maxFailureMessageSize )
    : maxFailureMessageSize_( std::min( maxFailureMessageSize, MaxSmartSocketPayloadSize ) )
{
}

DecoderFeedResult<SmartSocketStatus> SmartSocketStatusDecoder::feed( const ByteVector& bytes )
{
    if ( error_.has_value() ) {
        return failedFeed<SmartSocketStatus>( error_, buffer_.size() );
    }

    DecoderFeedResult<SmartSocketStatus> result;
    if ( complete_ ) {
        result.unconsumedBytes = bytes;
        return result;
    }
    if ( !appendInput( buffer_, bytes, error_ ) ) {
        return failedFeed<SmartSocketStatus>( error_, buffer_.size() );
    }
    if ( buffer_.size() < SmartSocketLengthSize ) {
        result.bufferedByteCount = buffer_.size();
        return result;
    }

    std::size_t frameEnd = SmartSocketLengthSize;
    if ( matchesStatus( buffer_, 0u, "OKAY" ) ) {
        result.frames.push_back( SmartSocketStatus{ SmartSocketStatusKind::Okay, {} } );
    }
    else if ( matchesStatus( buffer_, 0u, "FAIL" ) ) {
        constexpr std::size_t FailureHeaderSize = 8u;
        if ( buffer_.size() < FailureHeaderSize ) {
            result.bufferedByteCount = buffer_.size();
            return result;
        }

        const auto parsedLength
            = parseSmartSocketHexLength( lengthAt( buffer_, SmartSocketLengthSize ) );
        if ( parsedLength.error.has_value() ) {
            failDecoder( result, buffer_, error_, *parsedLength.error );
            return result;
        }
        if ( !parsedLength.value.has_value() ) {
            failDecoder( result, buffer_, error_,
                         makeError( ProtocolErrorCode::InvalidHexLength,
                                    "ADB smart-socket length parser returned no value." ) );
            return result;
        }

        const auto payloadLength = static_cast<std::size_t>( *parsedLength.value );
        if ( payloadLength > maxFailureMessageSize_ ) {
            failDecoder( result, buffer_, error_,
                         makeError( ProtocolErrorCode::FrameTooLarge,
                                    "ADB FAIL message exceeds the configured decoder bound." ) );
            return result;
        }
        if ( buffer_.size() - FailureHeaderSize < payloadLength ) {
            result.bufferedByteCount = buffer_.size();
            return result;
        }

        const auto payloadStart = FailureHeaderSize;
        frameEnd = payloadStart + payloadLength;
        result.frames.push_back( SmartSocketStatus{
            SmartSocketStatusKind::Fail, copyRange( buffer_, payloadStart, frameEnd ) } );
    }
    else {
        failDecoder( result, buffer_, error_,
                     makeError( ProtocolErrorCode::InvalidStatus,
                                "ADB smart-socket returned an unknown status." ) );
        return result;
    }

    discardPrefix( buffer_, frameEnd );
    result.unconsumedBytes.swap( buffer_ );
    complete_ = true;
    return result;
}

void SmartSocketStatusDecoder::reset() noexcept
{
    buffer_.clear();
    error_.reset();
    complete_ = false;
}

LengthPrefixedHostReplyDecoder::LengthPrefixedHostReplyDecoder( std::size_t maxPayloadSize )
    : maxPayloadSize_( std::min( maxPayloadSize, MaxSmartSocketPayloadSize ) )
{
}

DecoderFeedResult<ByteVector> LengthPrefixedHostReplyDecoder::feed( const ByteVector& bytes )
{
    if ( error_.has_value() ) {
        return failedFeed<ByteVector>( error_, buffer_.size() );
    }

    DecoderFeedResult<ByteVector> result;
    if ( !appendInput( buffer_, bytes, error_ ) ) {
        return failedFeed<ByteVector>( error_, buffer_.size() );
    }

    std::size_t cursor = 0u;
    while ( buffer_.size() - cursor >= SmartSocketLengthSize ) {
        const auto parsedLength = parseSmartSocketHexLength( lengthAt( buffer_, cursor ) );
        if ( parsedLength.error.has_value() ) {
            failDecoder( result, buffer_, error_, *parsedLength.error );
            return result;
        }
        if ( !parsedLength.value.has_value() ) {
            failDecoder( result, buffer_, error_,
                         makeError( ProtocolErrorCode::InvalidHexLength,
                                    "ADB smart-socket length parser returned no value." ) );
            return result;
        }

        const auto payloadLength = static_cast<std::size_t>( *parsedLength.value );
        if ( payloadLength > maxPayloadSize_ ) {
            failDecoder( result, buffer_, error_,
                         makeError( ProtocolErrorCode::FrameTooLarge,
                                    "ADB host reply exceeds the configured decoder bound." ) );
            return result;
        }
        if ( buffer_.size() - cursor - SmartSocketLengthSize < payloadLength ) {
            break;
        }

        const auto payloadStart = cursor + SmartSocketLengthSize;
        result.frames.push_back( copyRange( buffer_, payloadStart, payloadStart + payloadLength ) );
        cursor = payloadStart + payloadLength;
    }

    discardPrefix( buffer_, cursor );
    result.bufferedByteCount = buffer_.size();
    return result;
}

void LengthPrefixedHostReplyDecoder::reset() noexcept
{
    buffer_.clear();
    error_.reset();
}

ShellV2FrameDecoder::ShellV2FrameDecoder( std::size_t maxPayloadSize )
    : maxPayloadSize_( maxPayloadSize )
{
}

DecoderFeedResult<ShellV2Frame> ShellV2FrameDecoder::feed( const ByteVector& bytes )
{
    if ( error_.has_value() ) {
        return failedFeed<ShellV2Frame>( error_, buffer_.size() );
    }

    DecoderFeedResult<ShellV2Frame> result;
    if ( !appendInput( buffer_, bytes, error_ ) ) {
        return failedFeed<ShellV2Frame>( error_, buffer_.size() );
    }

    std::size_t cursor = 0u;
    while ( buffer_.size() - cursor >= ShellV2HeaderSize ) {
        const auto wireId = buffer_.at( cursor );
        const auto channel = shellChannel( wireId );
        if ( !channel.has_value() ) {
            failDecoder( result, buffer_, error_,
                         makeError( ProtocolErrorCode::UnknownShellV2Channel,
                                    "ADB shell-v2 frame contains an unknown channel id." ) );
            return result;
        }

        const auto wirePayloadLength = shellPayloadLength( buffer_, cursor );
        if ( !hasValidShellPayloadSize( *channel, wirePayloadLength ) ) {
            failDecoder( result, buffer_, error_,
                         makeError( ProtocolErrorCode::InvalidShellV2Frame,
                                    "ADB shell-v2 control frame has an invalid payload size." ) );
            return result;
        }

        const auto maxSize = static_cast<std::uint64_t>( std::numeric_limits<std::size_t>::max() );
        if ( wirePayloadLength > static_cast<std::uint64_t>( maxPayloadSize_ )
             || wirePayloadLength > maxSize - ShellV2HeaderSize ) {
            failDecoder( result, buffer_, error_,
                         makeError( ProtocolErrorCode::FrameTooLarge,
                                    "ADB shell-v2 frame exceeds the configured decoder bound." ) );
            return result;
        }

        const auto payloadLength = static_cast<std::size_t>( wirePayloadLength );
        if ( buffer_.size() - cursor - ShellV2HeaderSize < payloadLength ) {
            break;
        }

        const auto payloadStart = cursor + ShellV2HeaderSize;
        result.frames.push_back( ShellV2Frame{
            *channel, wireId, copyRange( buffer_, payloadStart, payloadStart + payloadLength ) } );
        cursor = payloadStart + payloadLength;
    }

    discardPrefix( buffer_, cursor );
    result.bufferedByteCount = buffer_.size();
    return result;
}

void ShellV2FrameDecoder::reset() noexcept
{
    buffer_.clear();
    error_.reset();
}

ProtocolResult<std::string> buildHostService( HostService service )
{
    switch ( service ) {
    case HostService::Version:
        return ProtocolResult<std::string>{ "host:version", std::nullopt };
    case HostService::ServerFeatures:
        return ProtocolResult<std::string>{ "host:host-features", std::nullopt };
    case HostService::DevicesLong:
        return ProtocolResult<std::string>{ "host:devices-l", std::nullopt };
    case HostService::TrackDevicesLong:
        return ProtocolResult<std::string>{ "host:track-devices-l", std::nullopt };
    }

    return commandError( ProtocolErrorCode::InvalidCommandOption,
                         "ADB host service contains an unknown service kind." );
}

ProtocolResult<std::string> buildTransportHostService( TransportHostService service )
{
    switch ( service ) {
    case TransportHostService::Features:
        return ProtocolResult<std::string>{ "host:features", std::nullopt };
    }

    return commandError( ProtocolErrorCode::InvalidCommandOption,
                         "ADB transport-scoped host service contains an unknown service kind." );
}

ProtocolResult<std::string> buildTransportService( const TransportSelection& selection )
{
    switch ( selection.kind ) {
    case TransportKind::Any:
        return ProtocolResult<std::string>{ "host:transport-any", std::nullopt };
    case TransportKind::Usb:
        return ProtocolResult<std::string>{ "host:transport-usb", std::nullopt };
    case TransportKind::Local:
        return ProtocolResult<std::string>{ "host:transport-local", std::nullopt };
    case TransportKind::Serial:
        break;
    default:
        return commandError( ProtocolErrorCode::InvalidCommandOption,
                             "ADB transport selection contains an unknown transport kind." );
    }

    if ( selection.serial.empty() || hasUnrepresentableCommandByte( selection.serial ) ) {
        return commandError(
            ProtocolErrorCode::InvalidCommandOption,
            "ADB serial transport requires a non-empty serial without NUL bytes." );
    }

    std::string service{ "host:transport:" };
    if ( !appendBounded( service, selection.serial ) ) {
        return commandError( ProtocolErrorCode::PayloadTooLarge,
                             "ADB transport service exceeds the smart-socket payload limit." );
    }
    return ProtocolResult<std::string>{ std::move( service ), std::nullopt };
}

std::vector<std::string> buildLogcatFormatArguments( bool ansiOutputEnabled )
{
    std::vector<std::string> arguments{
        "-v", "threadtime", "-v", "year", "-v", "zone", "-v", "usec"
    };
    if ( ansiOutputEnabled ) {
        arguments.emplace_back( "-v" );
        arguments.emplace_back( "color" );
    }
    return arguments;
}

std::string normalizeLogcatStreamError( const std::string& diagnostic )
{
    std::string_view remaining( diagnostic );
    while ( !remaining.empty() ) {
        const auto end = remaining.find( '\n' );
        const auto line = remaining.substr( 0u, end );
        if ( lineRejectsOwnedLogcatFormat( line ) ) {
            return "This ADB device cannot provide unambiguous source-device wall time because "
                   "its logcat does not support the required year, zone, and microsecond format "
                   "modifiers (Android 7.0 or compatible is required). Original error: "
                   + diagnostic;
        }
        if ( end == std::string_view::npos ) {
            break;
        }
        remaining.remove_prefix( end + 1u );
    }
    return diagnostic;
}

ProtocolResult<std::string> buildLogcatService( const LogcatCommandOptions& options )
{
    std::string service{ "shell,v2,raw:logcat" };
    // appendBounded mutates the command and must stop at the first overflow.
    for ( const auto& argument : buildLogcatFormatArguments( options.ansiOutputEnabled ) ) {
        // cppcheck-suppress useStlAlgorithm
        if ( !appendBounded( service, " " ) || !appendBounded( service, argument ) ) {
            return commandError( ProtocolErrorCode::PayloadTooLarge,
                                 "ADB logcat service exceeds the smart-socket payload limit." );
        }
    }

    for ( const auto buffer : options.buffers ) {
        const auto bufferName = logBufferName( buffer );
        if ( !bufferName.has_value() ) {
            return commandError( ProtocolErrorCode::InvalidCommandOption,
                                 "ADB logcat options contain an unknown log buffer." );
        }
        if ( !appendBounded( service, " -b " ) || !appendBounded( service, *bufferName ) ) {
            return commandError( ProtocolErrorCode::PayloadTooLarge,
                                 "ADB logcat service exceeds the smart-socket payload limit." );
        }
    }

    if ( options.initialTailLineCount.has_value() ) {
        const auto count = std::to_string( *options.initialTailLineCount );
        if ( !appendBounded( service, " -T " ) || !appendBounded( service, count ) ) {
            return commandError( ProtocolErrorCode::PayloadTooLarge,
                                 "ADB logcat service exceeds the smart-socket payload limit." );
        }
    }

    if ( options.processId.has_value() ) {
        const auto processId = std::to_string( *options.processId );
        if ( !appendBounded( service, " --pid " ) || !appendBounded( service, processId ) ) {
            return commandError( ProtocolErrorCode::PayloadTooLarge,
                                 "ADB logcat service exceeds the smart-socket payload limit." );
        }
    }

    for ( const auto& filter : options.filters ) {
        if ( filter.tag.empty() || filter.tag.front() == '-'
             || hasUnrepresentableCommandByte( filter.tag ) ) {
            return commandError(
                ProtocolErrorCode::InvalidCommandOption,
                "ADB logcat filters require a non-empty, non-option tag without NUL bytes." );
        }

        const auto priority = logPriorityLetter( filter.priority );
        if ( !priority.has_value() ) {
            return commandError( ProtocolErrorCode::InvalidCommandOption,
                                 "ADB logcat filter contains an unknown priority." );
        }

        constexpr std::size_t FilterSuffixSize = 2u;
        if ( filter.tag.size() > MaxSmartSocketPayloadSize - FilterSuffixSize ) {
            return commandError( ProtocolErrorCode::PayloadTooLarge,
                                 "ADB logcat filter exceeds the smart-socket payload limit." );
        }
        std::string filterExpression = filter.tag;
        filterExpression.push_back( ':' );
        filterExpression.push_back( *priority );

        if ( !appendBounded( service, " " ) || !appendShellQuoted( service, filterExpression ) ) {
            return commandError( ProtocolErrorCode::PayloadTooLarge,
                                 "ADB logcat service exceeds the smart-socket payload limit." );
        }
    }

    return ProtocolResult<std::string>{ std::move( service ), std::nullopt };
}

std::string buildClearLogcatService()
{
    return "shell,v2,raw:logcat -c";
}

} // namespace klogg::livecapture::adb
