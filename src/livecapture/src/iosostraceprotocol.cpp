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

#include "iosostraceprotocol.h"

#include <algorithm>
#include <utility>

namespace klogg::livecapture::ios {
namespace {

// The offsets below mirror the public packed header contract in the pinned
// libimobiledevice 1.4.0 API. Decode bytes explicitly instead of importing the
// vendor struct so host packing, alignment, and endianness never affect parsing.
constexpr std::size_t OsTraceHeaderSize = 0x81u;
constexpr std::size_t MarkerOffset = 0u;
constexpr std::size_t TypeOffset = 1u;
constexpr std::size_t HeaderSizeOffset = 5u;
constexpr std::size_t PidOffset = 9u;
constexpr std::size_t ProcessIdOffset = 13u;
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

std::uint16_t readLe16( const ByteBuffer& bytes, std::size_t offset ) noexcept
{
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>( bytes[ offset ] )
        | static_cast<std::uint16_t>( static_cast<std::uint16_t>( bytes[ offset + 1u ] ) << 8u ) );
}

std::uint32_t readLe32( const ByteBuffer& bytes, std::size_t offset ) noexcept
{
    std::uint32_t result = 0u;
    for ( std::size_t index = 0u; index < 4u; ++index ) {
        result |= static_cast<std::uint32_t>( bytes[ offset + index ] )
                  << static_cast<unsigned>( index * 8u );
    }
    return result;
}

std::uint64_t readLe64( const ByteBuffer& bytes, std::size_t offset ) noexcept
{
    std::uint64_t result = 0u;
    for ( std::size_t index = 0u; index < 8u; ++index ) {
        result |= static_cast<std::uint64_t>( bytes[ offset + index ] )
                  << static_cast<unsigned>( index * 8u );
    }
    return result;
}

bool containsSpan( const ByteBuffer& bytes, std::size_t offset, std::size_t size ) noexcept
{
    return offset <= bytes.size() && size <= bytes.size() - offset;
}

OsTracePacketStructure inspectPacketStructure( const ByteBuffer& bytes ) noexcept
{
    OsTracePacketStructure structure;
    structure.packetByteCount = bytes.size();
    if ( containsSpan( bytes, MarkerOffset, 1u ) ) {
        structure.marker = bytes[ MarkerOffset ];
    }
    if ( containsSpan( bytes, TypeOffset, 4u ) ) {
        structure.wirePacketType = readLe32( bytes, TypeOffset );
    }
    if ( containsSpan( bytes, HeaderSizeOffset, 4u ) ) {
        structure.declaredHeaderByteCount = readLe32( bytes, HeaderSizeOffset );
    }
    if ( containsSpan( bytes, ProcessPathLengthOffset, 2u ) ) {
        structure.fieldLengths[ 0u ] = readLe16( bytes, ProcessPathLengthOffset );
    }
    if ( containsSpan( bytes, ImagePathLengthOffset, 2u ) ) {
        structure.fieldLengths[ 1u ] = readLe16( bytes, ImagePathLengthOffset );
    }
    if ( containsSpan( bytes, MessageLengthOffset, 4u ) ) {
        structure.fieldLengths[ 2u ] = readLe32( bytes, MessageLengthOffset );
    }
    if ( containsSpan( bytes, SubsystemLengthOffset, 2u ) ) {
        structure.fieldLengths[ 3u ] = readLe16( bytes, SubsystemLengthOffset );
    }
    if ( containsSpan( bytes, CategoryLengthOffset, 2u ) ) {
        structure.fieldLengths[ 4u ] = readLe16( bytes, CategoryLengthOffset );
    }
    for ( const auto length : structure.fieldLengths ) {
        structure.declaredSpanByteCount += length;
    }
    structure.availableVariableByteCount
        = bytes.size() > OsTraceHeaderSize ? bytes.size() - OsTraceHeaderSize : 0u;
    return structure;
}

OsTraceDecodeResult decodeFailure( OsTraceDecodeErrorCode code, OsTraceField field,
                                   const ByteBuffer& bytes )
{
    return { std::nullopt, OsTraceDecodeError{ code, field, inspectPacketStructure( bytes ) } };
}

bool isContinuationByte( std::uint8_t byte ) noexcept
{
    return byte >= 0x80u && byte <= 0xbfu;
}

bool isValidThreeByteSecondByte( std::uint8_t first, std::uint8_t second ) noexcept
{
    if ( first == 0xe0u ) {
        return second >= 0xa0u && second <= 0xbfu;
    }
    if ( first == 0xedu ) {
        return second >= 0x80u && second <= 0x9fu;
    }
    return isContinuationByte( second );
}

bool isValidFourByteSecondByte( std::uint8_t first, std::uint8_t second ) noexcept
{
    if ( first == 0xf0u ) {
        return second >= 0x90u && second <= 0xbfu;
    }
    if ( first == 0xf4u ) {
        return second >= 0x80u && second <= 0x8fu;
    }
    return isContinuationByte( second );
}

void appendReplacementCharacter( std::string& output )
{
    output.append( "\xef\xbf\xbd" );
}

std::string sanitizeUtf8( const std::uint8_t* input, std::size_t size )
{
    std::string output;
    output.reserve( size );

    std::size_t index = 0u;
    while ( index < size ) {
        const auto first = input[ index ];
        std::size_t sequenceLength = 0u;
        bool valid = false;

        if ( first <= 0x7fu ) {
            sequenceLength = 1u;
            valid = true;
        }
        else if ( first >= 0xc2u && first <= 0xdfu && size - index >= 2u ) {
            sequenceLength = 2u;
            valid = isContinuationByte( input[ index + 1u ] );
        }
        else if ( first >= 0xe0u && first <= 0xefu && size - index >= 3u ) {
            const auto second = input[ index + 1u ];
            const auto third = input[ index + 2u ];
            sequenceLength = 3u;
            valid = isValidThreeByteSecondByte( first, second ) && isContinuationByte( third );
        }
        else if ( first >= 0xf0u && first <= 0xf4u && size - index >= 4u ) {
            const auto second = input[ index + 1u ];
            sequenceLength = 4u;
            valid = isValidFourByteSecondByte( first, second )
                    && isContinuationByte( input[ index + 2u ] )
                    && isContinuationByte( input[ index + 3u ] );
        }

        if ( !valid ) {
            appendReplacementCharacter( output );
            ++index;
            continue;
        }

        for ( std::size_t byteIndex = 0u; byteIndex < sequenceLength; ++byteIndex ) {
            output.push_back( static_cast<char>( input[ index + byteIndex ] ) );
        }
        index += sequenceLength;
    }

    return output;
}

char lowercaseHexDigit( std::uint8_t nibble ) noexcept
{
    constexpr char Digits[] = "0123456789abcdef";
    return Digits[ nibble & 0x0fu ];
}

void appendHexEscape( std::string& output, const char* prefix, std::uint8_t value )
{
    output += prefix;
    output.push_back( lowercaseHexDigit( static_cast<std::uint8_t>( value >> 4u ) ) );
    output.push_back( lowercaseHexDigit( value ) );
}

std::string escapeDisplayText( const std::string& text )
{
    const auto* input = reinterpret_cast<const std::uint8_t*>( text.data() );
    const auto sanitized = sanitizeUtf8( input, text.size() );

    std::string output;
    output.reserve( sanitized.size() );
    for ( std::size_t index = 0u; index < sanitized.size(); ++index ) {
        const auto byte = static_cast<std::uint8_t>( sanitized[ index ] );
        switch ( byte ) {
        case 0x07u:
            output += "\\a";
            break;
        case 0x08u:
            output += "\\b";
            break;
        case 0x09u:
            output += "\\t";
            break;
        case 0x0au:
            output += "\\n";
            break;
        case 0x0bu:
            output += "\\v";
            break;
        case 0x0cu:
            output += "\\f";
            break;
        case 0x0du:
            output += "\\r";
            break;
        case 0x5cu:
            output += "\\\\";
            break;
        default:
            if ( byte < 0x20u || byte == 0x7fu ) {
                appendHexEscape( output, "\\x", byte );
            }
            else if ( byte == 0xc2u && index + 1u < sanitized.size() ) {
                const auto second = static_cast<std::uint8_t>( sanitized[ index + 1u ] );
                if ( second >= 0x80u && second <= 0x9fu ) {
                    appendHexEscape( output, "\\u00", second );
                    ++index;
                }
                else {
                    output.push_back( static_cast<char>( byte ) );
                }
            }
            else {
                output.push_back( static_cast<char>( byte ) );
            }
            break;
        }
    }
    return output;
}

std::optional<OsTraceDecodeError> decodeTextField( const ByteBuffer& bytes, std::size_t& cursor,
                                                   std::size_t length, OsTraceField field,
                                                   std::optional<std::string>& destination )
{
    if ( length == 0u ) {
        destination.reset();
        return std::nullopt;
    }
    if ( cursor > bytes.size() || length > bytes.size() - cursor ) {
        return OsTraceDecodeError{ OsTraceDecodeErrorCode::SpanOutOfBounds, field, std::nullopt };
    }

    const auto* first = bytes.data() + cursor;
    if ( first[ length - 1u ] != 0u ) {
        return OsTraceDecodeError{ OsTraceDecodeErrorCode::MissingNulTerminator, field,
                                   std::nullopt };
    }
    if ( std::find( first, first + length - 1u, std::uint8_t{ 0u } ) != first + length - 1u ) {
        return OsTraceDecodeError{ OsTraceDecodeErrorCode::EmbeddedNul, field, std::nullopt };
    }

    destination = sanitizeUtf8( first, length - 1u );
    cursor += length;
    return std::nullopt;
}

std::optional<OsTracePacketType> packetTypeFromWire( std::uint32_t value ) noexcept
{
    switch ( value ) {
    case 2u:
        return OsTracePacketType::Activity;
    case 8u:
        return OsTracePacketType::LogMessage;
    default:
        return std::nullopt;
    }
}

std::optional<OsTraceLevel> levelFromWire( std::uint8_t value ) noexcept
{
    switch ( value ) {
    case 0x00u:
        return OsTraceLevel::Notice;
    case 0x01u:
        return OsTraceLevel::Info;
    case 0x02u:
        return OsTraceLevel::Debug;
    case 0x03u:
        return OsTraceLevel::UserAction;
    case 0x10u:
        return OsTraceLevel::Error;
    case 0x11u:
        return OsTraceLevel::Fault;
    default:
        return std::nullopt;
    }
}

const char* levelName( OsTraceLevel level ) noexcept
{
    switch ( level ) {
    case OsTraceLevel::Notice:
        return "NOTICE";
    case OsTraceLevel::Info:
        return "INFO";
    case OsTraceLevel::Debug:
        return "DEBUG";
    case OsTraceLevel::UserAction:
        return "USER_ACTION";
    case OsTraceLevel::Error:
        return "ERROR";
    case OsTraceLevel::Fault:
        return "FAULT";
    }
    return "NOTICE";
}

const char* severityColor( OsTraceLevel level ) noexcept
{
    switch ( level ) {
    case OsTraceLevel::Debug:
        return "\x1b[32m";
    case OsTraceLevel::Error:
    case OsTraceLevel::Fault:
        return "\x1b[31m";
    case OsTraceLevel::Notice:
    case OsTraceLevel::Info:
    case OsTraceLevel::UserAction:
        return "\x1b[37m";
    }
    return "\x1b[37m";
}

std::string baseName( const std::optional<std::string>& path )
{
    if ( !path ) {
        return {};
    }
    const auto separator = path->find_last_of( "/\\" );
    return separator == std::string::npos ? *path : path->substr( separator + 1u );
}

std::string paddedUnsigned( std::uint64_t value, std::size_t width )
{
    auto text = std::to_string( value );
    if ( text.size() < width ) {
        text.insert( 0u, width - text.size(), '0' );
    }
    return text;
}

std::string lowercaseHex( std::uint32_t value )
{
    constexpr char Digits[] = "0123456789abcdef";
    if ( value == 0u ) {
        return "0";
    }

    std::string result;
    while ( value != 0u ) {
        result.push_back( Digits[ value & 0x0fu ] );
        value >>= 4u;
    }
    std::reverse( result.begin(), result.end() );
    return result;
}

std::int64_t floorDivide( std::int64_t value, std::int64_t divisor ) noexcept
{
    const auto quotient = value / divisor;
    const auto remainder = value % divisor;
    return remainder < 0 ? quotient - 1 : quotient;
}

std::string formatLocalSecond( std::uint64_t seconds, std::int32_t utcOffsetSeconds )
{
    constexpr std::uint64_t UnsignedSecondsPerDay = std::uint64_t{ 24u } * 60u * 60u;
    constexpr std::int64_t SecondsPerDay = std::int64_t{ 24 } * 60 * 60;

    // Even UINT64_MAX seconds is only about 2.1e14 days, so the day count and
    // all calendar intermediates fit comfortably in int64_t. Keeping the
    // sub-day value signed lets historical negative offsets cross the epoch
    // boundary without unsigned underflow.
    auto daysSinceEpoch = static_cast<std::int64_t>( seconds / UnsignedSecondsPerDay );
    const auto shiftedSeconds
        = static_cast<std::int64_t>( seconds % UnsignedSecondsPerDay )
          + static_cast<std::int64_t>( utcOffsetSeconds );
    const auto dayAdjustment = floorDivide( shiftedSeconds, SecondsPerDay );
    daysSinceEpoch += dayAdjustment;
    const auto secondsWithinDay = shiftedSeconds - dayAdjustment * SecondsPerDay;

    // Civil date conversion adapted from Howard Hinnant's public-domain
    // days-from-civil inverse. It is purely arithmetic and works beyond the
    // framework date-time ranges while preserving an exact signed UTC offset.
    const auto shiftedDays = daysSinceEpoch + 719468;
    const auto era = ( shiftedDays >= 0 ? shiftedDays : shiftedDays - 146096 ) / 146097;
    const auto dayOfEra
        = static_cast<std::uint64_t>( shiftedDays - era * 146097 );
    const auto yearOfEra
        = ( dayOfEra - dayOfEra / 1460u + dayOfEra / 36524u - dayOfEra / 146096u ) / 365u;
    const auto year = static_cast<std::int64_t>( yearOfEra ) + era * 400;
    const auto dayOfYear = dayOfEra - ( 365u * yearOfEra + yearOfEra / 4u - yearOfEra / 100u );
    const auto monthPrime = ( 5u * dayOfYear + 2u ) / 153u;
    const auto day = dayOfYear - ( 153u * monthPrime + 2u ) / 5u + 1u;
    const auto month = monthPrime < 10u ? monthPrime + 3u : monthPrime - 9u;
    const auto calendarYear = year + ( month <= 2u ? 1 : 0 );

    const auto hour = static_cast<std::uint64_t>( secondsWithinDay / 3600 );
    const auto minute = static_cast<std::uint64_t>( ( secondsWithinDay % 3600 ) / 60 );
    const auto second = static_cast<std::uint64_t>( secondsWithinDay % 60 );

    return paddedUnsigned( static_cast<std::uint64_t>( calendarYear ), 4u ) + "-"
           + paddedUnsigned( month, 2u ) + "-" + paddedUnsigned( day, 2u ) + " "
           + paddedUnsigned( hour, 2u ) + ":" + paddedUnsigned( minute, 2u ) + ":"
           + paddedUnsigned( second, 2u );
}

std::string normalizedUtcOffset( std::int32_t offsetSeconds )
{
    const auto signedOffset = static_cast<std::int64_t>( offsetSeconds );
    const auto magnitude
        = static_cast<std::uint64_t>( signedOffset < 0 ? -signedOffset : signedOffset );
    const auto hours = magnitude / 3600u;
    const auto minutes = ( magnitude % 3600u ) / 60u;
    const auto seconds = magnitude % 60u;

    auto result = std::string( 1u, signedOffset < 0 ? '-' : '+' )
                  + paddedUnsigned( hours, 2u ) + ":" + paddedUnsigned( minutes, 2u );
    if ( seconds != 0u ) {
        result += ":" + paddedUnsigned( seconds, 2u );
    }
    return result;
}

class FormatBuilder {
public:
    explicit FormatBuilder( bool ansi )
        : ansi_( ansi )
    {
    }

    void append( const std::string& text )
    {
        plain_ += text;
        output_ += text;
    }

    void appendStyled( const std::string& text, const char* color )
    {
        plain_ += text;
        if ( ansi_ && !text.empty() ) {
            output_ += color;
            output_ += text;
            output_ += "\x1b[0m";
        }
        else {
            output_ += text;
        }
    }

    const std::string& plain() const noexcept
    {
        return plain_;
    }
    std::string takeOutput()
    {
        return std::move( output_ );
    }

private:
    bool ansi_;
    std::string plain_;
    std::string output_;
};

} // namespace

OsTraceCallbackPayloadKind classifyOsTraceCallbackPayload( const ByteBuffer& bytes ) noexcept
{
    if ( !bytes.empty() && bytes.front() == 2u ) {
        return OsTraceCallbackPayloadKind::TracePacket;
    }
    constexpr std::array<std::uint8_t, 8u> BinaryPlistMagic{
        'b', 'p', 'l', 'i', 's', 't', '0', '0'
    };
    if ( bytes.size() >= BinaryPlistMagic.size()
         && std::equal( BinaryPlistMagic.cbegin(), BinaryPlistMagic.cend(), bytes.cbegin() ) ) {
        return OsTraceCallbackPayloadKind::ControlPlist;
    }
    constexpr std::array<std::uint8_t, 5u> XmlPrefix{ '<', '?', 'x', 'm', 'l' };
    constexpr std::array<std::uint8_t, 6u> DirectXmlPlistPrefix{ '<', 'p', 'l', 'i', 's', 't' };
    if ( ( bytes.size() >= XmlPrefix.size()
           && std::equal( XmlPrefix.cbegin(), XmlPrefix.cend(), bytes.cbegin() ) )
         || ( bytes.size() >= DirectXmlPlistPrefix.size()
              && std::equal( DirectXmlPlistPrefix.cbegin(), DirectXmlPlistPrefix.cend(),
                             bytes.cbegin() ) ) ) {
        return OsTraceCallbackPayloadKind::ControlPlist;
    }
    return OsTraceCallbackPayloadKind::Unknown;
}

OsTraceDecodeResult decodeOsTracePacket( const ByteBuffer& bytes,
                                         const OsTraceDecodeLimits& limits )
{
    if ( bytes.size() > limits.maximumPacketSize ) {
        return decodeFailure( OsTraceDecodeErrorCode::PacketTooLarge, OsTraceField::Packet, bytes );
    }
    if ( bytes.size() < OsTraceHeaderSize ) {
        return decodeFailure( OsTraceDecodeErrorCode::TruncatedHeader, OsTraceField::Header,
                              bytes );
    }
    if ( bytes[ MarkerOffset ] != 2u ) {
        return decodeFailure( OsTraceDecodeErrorCode::InvalidMarker, OsTraceField::Marker, bytes );
    }

    const auto packetType = packetTypeFromWire( readLe32( bytes, TypeOffset ) );
    if ( !packetType ) {
        return decodeFailure( OsTraceDecodeErrorCode::UnknownPacketType, OsTraceField::PacketType,
                              bytes );
    }
    if ( readLe32( bytes, HeaderSizeOffset ) != OsTraceHeaderSize ) {
        return decodeFailure( OsTraceDecodeErrorCode::InvalidHeaderSize, OsTraceField::HeaderSize,
                              bytes );
    }

    const auto microseconds = readLe32( bytes, MicrosecondsOffset );
    if ( microseconds >= 1'000'000u ) {
        return decodeFailure( OsTraceDecodeErrorCode::InvalidTimestamp, OsTraceField::Microseconds,
                              bytes );
    }

    const auto level = levelFromWire( bytes[ LevelOffset ] );
    if ( !level ) {
        return decodeFailure( OsTraceDecodeErrorCode::UnknownLogLevel, OsTraceField::Level, bytes );
    }

    DecodedOsTraceRecord record;
    record.packetType = *packetType;
    record.pid = readLe32( bytes, PidOffset );
    record.processId = readLe64( bytes, ProcessIdOffset );
    for ( std::size_t index = 0u; index < record.processUuid.bytes.size(); ++index ) {
        record.processUuid.bytes[ index ] = bytes[ ProcessUuidOffset + index ];
        record.imageUuid.bytes[ index ] = bytes[ ImageUuidOffset + index ];
    }
    record.activityId = readLe64( bytes, ActivityIdOffset );
    record.parentActivityId = readLe64( bytes, ParentActivityIdOffset );
    record.seconds = readLe64( bytes, SecondsOffset );
    record.microseconds = microseconds;
    record.level = *level;
    record.machTimestamp = readLe64( bytes, MachTimestampOffset );
    record.threadId = readLe32( bytes, ThreadIdOffset );
    record.imageOffset = readLe32( bytes, ImageOffsetOffset );

    // Type 2 activity packets share the fixed 0x81-byte header but their
    // variable body is an opaque activity payload, not the five type 8 text
    // spans. Applying log-message lengths to that body misclassifies valid
    // activity records as SpanOutOfBounds. The global 16 MiB bound and complete
    // fixed-header validation still apply before this point.
    if ( *packetType == OsTracePacketType::Activity ) {
        return { std::move( record ), std::nullopt };
    }

    const std::array<std::size_t, 5> lengths{ readLe16( bytes, ProcessPathLengthOffset ),
                                              readLe16( bytes, ImagePathLengthOffset ),
                                              readLe32( bytes, MessageLengthOffset ),
                                              readLe16( bytes, SubsystemLengthOffset ),
                                              readLe16( bytes, CategoryLengthOffset ) };
    const std::array<OsTraceField, 5> fields{ OsTraceField::ProcessPath, OsTraceField::ImagePath,
                                              OsTraceField::Message, OsTraceField::Subsystem,
                                              OsTraceField::Category };
    std::array<std::optional<std::string>*, 5> destinations{ &record.processPath, &record.imagePath,
                                                             &record.message, &record.subsystem,
                                                             &record.category };

    std::size_t cursor = OsTraceHeaderSize;
    for ( std::size_t index = 0u; index < lengths.size(); ++index ) {
        if ( cursor > bytes.size() || lengths[ index ] > bytes.size() - cursor ) {
            return decodeFailure( OsTraceDecodeErrorCode::SpanOutOfBounds, fields[ index ], bytes );
        }
        cursor += lengths[ index ];
    }
    if ( cursor != bytes.size() ) {
        return decodeFailure( OsTraceDecodeErrorCode::UnexpectedTrailingData, OsTraceField::Packet,
                              bytes );
    }

    cursor = OsTraceHeaderSize;
    for ( std::size_t index = 0u; index < lengths.size(); ++index ) {
        auto error = decodeTextField( bytes, cursor, lengths[ index ], fields[ index ],
                                      *destinations[ index ] );
        if ( error ) {
            error->structure = inspectPacketStructure( bytes );
            return { std::nullopt, error };
        }
    }

    return { std::move( record ), std::nullopt };
}

OsTraceRelayFrameDecoder::OsTraceRelayFrameDecoder( std::size_t maximumRecordSize )
    : maximumRecordSize_( maximumRecordSize )
{
}

OsTraceRelayFeedResult OsTraceRelayFrameDecoder::feed( const ByteBuffer& bytes )
{
    OsTraceRelayFeedResult result;
    if ( error_ ) {
        result.error = error_;
        result.bufferedByteCount = bufferedByteCount();
        return result;
    }

    std::size_t inputOffset = 0u;
    while ( inputOffset < bytes.size() ) {
        if ( !expectedPayloadSize_ ) {
            const auto headerRemaining = header_.size() - headerByteCount_;
            const auto copyCount = std::min( headerRemaining, bytes.size() - inputOffset );
            for ( std::size_t index = 0u; index < copyCount; ++index ) {
                header_[ headerByteCount_ + index ] = bytes[ inputOffset + index ];
            }
            headerByteCount_ += copyCount;
            inputOffset += copyCount;
            if ( headerByteCount_ != header_.size() ) {
                break;
            }

            if ( header_[ 0 ] == 1u ) {
                currentType_ = OsTraceRelayRecordType::ControlPlist;
            }
            else if ( header_[ 0 ] == 2u ) {
                currentType_ = OsTraceRelayRecordType::Activity;
            }
            else {
                setError( OsTraceRelayErrorCode::UnknownRecordType );
                break;
            }

            std::uint32_t wireLength = 0u;
            if ( currentType_ == OsTraceRelayRecordType::ControlPlist ) {
                for ( std::size_t index = 1u; index < header_.size(); ++index ) {
                    // The explicit shift documents the wire-endian fold.
                    // cppcheck-suppress useStlAlgorithm
                    wireLength = static_cast<std::uint32_t>( wireLength << 8u )
                                 | static_cast<std::uint32_t>( header_[ index ] );
                }
            }
            else {
                for ( std::size_t index = 1u; index < header_.size(); ++index ) {
                    wireLength |= static_cast<std::uint32_t>( header_[ index ] )
                                  << static_cast<unsigned>( ( index - 1u ) * 8u );
                }
            }

            const auto payloadSize = static_cast<std::size_t>( wireLength );
            if ( payloadSize > maximumRecordSize_ ) {
                setError( OsTraceRelayErrorCode::RecordTooLarge );
                break;
            }
            expectedPayloadSize_ = payloadSize;
            payload_.clear();
            payload_.reserve( payloadSize );

            if ( payloadSize == 0u ) {
                result.records.push_back( OsTraceRelayRecord{ currentType_, {} } );
                headerByteCount_ = 0u;
                expectedPayloadSize_.reset();
            }
        }

        if ( expectedPayloadSize_ ) {
            const auto remaining = *expectedPayloadSize_ - payload_.size();
            const auto copyCount = std::min( remaining, bytes.size() - inputOffset );
            payload_.insert(
                payload_.end(), bytes.begin() + static_cast<std::ptrdiff_t>( inputOffset ),
                bytes.begin() + static_cast<std::ptrdiff_t>( inputOffset + copyCount ) );
            inputOffset += copyCount;

            if ( payload_.size() == *expectedPayloadSize_ ) {
                result.records.push_back(
                    OsTraceRelayRecord{ currentType_, std::move( payload_ ) } );
                payload_.clear();
                headerByteCount_ = 0u;
                expectedPayloadSize_.reset();
            }
        }
    }

    result.error = error_;
    result.bufferedByteCount = bufferedByteCount();
    return result;
}

void OsTraceRelayFrameDecoder::reset() noexcept
{
    header_.fill( 0u );
    headerByteCount_ = 0u;
    expectedPayloadSize_.reset();
    payload_.clear();
    error_.reset();
}

std::size_t OsTraceRelayFrameDecoder::bufferedByteCount() const noexcept
{
    return headerByteCount_ + payload_.size();
}

void OsTraceRelayFrameDecoder::setError( OsTraceRelayErrorCode code ) noexcept
{
    error_ = OsTraceRelayError{ code };
    header_.fill( 0u );
    headerByteCount_ = 0u;
    expectedPayloadSize_.reset();
    payload_.clear();
}

OsTraceFormatOptions::OsTraceFormatOptions( bool ansi, bool imageMetadata, bool labels,
                                            OsTraceUtcOffsetResolver utcOffsetResolver )
    : ansiColors( ansi )
    , includeImageMetadata( imageMetadata )
    , includeLabels( labels )
    , utcOffsetSecondsAt( std::move( utcOffsetResolver ) )
{
}

OsTraceRecordFormatter::OsTraceRecordFormatter( OsTraceFormatOptions options )
    : options_( std::move( options ) )
{
}

FormattedOsTraceRecord
OsTraceRecordFormatter::format( const DecodedOsTraceRecord& record,
                                OsTraceFormatStatisticsHook statisticsHook )
{
    constexpr const char* Green = "\x1b[32m";
    constexpr const char* Magenta = "\x1b[35m";
    constexpr const char* Cyan = "\x1b[36m";

    if ( !cachedEpochSecond_ || *cachedEpochSecond_ != record.seconds ) {
        const auto utcOffsetSeconds
            = options_.utcOffsetSecondsAt
                  ? options_.utcOffsetSecondsAt( record.seconds )
                  : std::optional<std::int32_t>{ 0 };
        if ( !utcOffsetSeconds ) {
            FormattedOsTraceRecord result;
            result.utcOffsetResolved = false;
            return result;
        }
        cachedLocalSecond_ = formatLocalSecond( record.seconds, *utcOffsetSeconds );
        cachedUtcOffset_ = normalizedUtcOffset( *utcOffsetSeconds );
        cachedEpochSecond_ = record.seconds;
    }
    const auto timestamp = cachedLocalSecond_ + "." + paddedUnsigned( record.microseconds, 6u )
                           + cachedUtcOffset_;

    FormatBuilder builder( options_.ansiColors );
    builder.appendStyled( timestamp, Green );
    builder.append( " " );
    builder.appendStyled( escapeDisplayText( baseName( record.processPath ) ), Magenta );
    builder.append( "{" );
    if ( options_.includeImageMetadata && record.imagePath ) {
        constexpr const char* Blue = "\x1b[34m";
        builder.appendStyled( escapeDisplayText( baseName( record.imagePath ) ), Magenta );
        builder.appendStyled( "+0x" + lowercaseHex( record.imageOffset ), Blue );
    }
    builder.append( "}[" );
    builder.appendStyled( std::to_string( record.pid ), Cyan );
    builder.append( "] <" );
    builder.appendStyled( levelName( record.level ), severityColor( record.level ) );
    builder.append( ">: " );
    builder.appendStyled( escapeDisplayText( record.message.value_or( std::string{} ) ),
                          severityColor( record.level ) );

    if ( options_.includeLabels && ( record.subsystem || record.category ) ) {
        std::string labels;
        if ( record.subsystem ) {
            labels += "[" + escapeDisplayText( *record.subsystem ) + "]";
        }
        if ( record.category ) {
            labels += "[" + escapeDisplayText( *record.category ) + "]";
        }
        builder.append( " " );
        builder.appendStyled( labels, Cyan );
    }

    FormattedOsTraceRecord result;
    result.statistics.plainByteCount = builder.plain().size();
    result.bytes = builder.takeOutput();
    result.statistics.outputByteCount = result.bytes.size();
    result.statistics.ansiEscapeByteCount
        = result.statistics.outputByteCount - result.statistics.plainByteCount;
    result.statistics.ansiExpansionByteCount = result.statistics.ansiEscapeByteCount;

    if ( statisticsHook ) {
        statisticsHook( result.statistics );
    }
    return result;
}

FormattedOsTraceRecord formatOsTraceRecord( const DecodedOsTraceRecord& record,
                                            const OsTraceFormatOptions& options,
                                            OsTraceFormatStatisticsHook statisticsHook )
{
    OsTraceRecordFormatter formatter( options );
    return formatter.format( record, std::move( statisticsHook ) );
}

BorrowedActivityCallbackBridge::BorrowedActivityCallbackBridge( OwnedBytesCallback callback,
                                                                std::size_t maximumByteCount )
    : callback_( std::move( callback ) )
    , maximumByteCount_( maximumByteCount )
{
}

BorrowedActivityCopyResult
BorrowedActivityCallbackBridge::operator()( const void* borrowedBytes,
                                            std::size_t byteCount ) const noexcept
{
    if ( !callback_ ) {
        return BorrowedActivityCopyResult::CallbackUnavailable;
    }
    if ( borrowedBytes == nullptr && byteCount != 0u ) {
        return BorrowedActivityCopyResult::InvalidBorrowedBuffer;
    }
    if ( byteCount > maximumByteCount_ ) {
        return BorrowedActivityCopyResult::BufferTooLarge;
    }

    ByteBuffer owned;
    try {
        if ( byteCount != 0u ) {
            const auto* first = static_cast<const std::uint8_t*>( borrowedBytes );
            owned.assign( first, first + byteCount );
        }
    } catch ( ... ) {
        return BorrowedActivityCopyResult::AllocationFailed;
    }

    try {
        callback_( std::move( owned ) );
    } catch ( ... ) {
        return BorrowedActivityCopyResult::CallbackFailed;
    }
    return BorrowedActivityCopyResult::Delivered;
}

} // namespace klogg::livecapture::ios
