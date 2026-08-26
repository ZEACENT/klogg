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

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace klogg::livecapture::ios {

using ByteBuffer = std::vector<std::uint8_t>;

inline constexpr std::size_t DefaultMaximumOsTraceRecordSize = std::size_t{ 16u } * 1024u * 1024u;

struct OsTraceUuid {
    std::array<std::uint8_t, 16> bytes{};
};

enum class OsTracePacketType : std::uint8_t { Activity = 2, LogMessage = 8 };

enum class OsTraceLevel : std::uint8_t {
    Notice = 0x00,
    Info = 0x01,
    Debug = 0x02,
    UserAction = 0x03,
    Error = 0x10,
    Fault = 0x11
};

struct DecodedOsTraceRecord {
    OsTracePacketType packetType{ OsTracePacketType::LogMessage };
    std::uint32_t pid{ 0 };
    std::uint64_t processId{ 0 };
    OsTraceUuid processUuid;
    std::uint64_t activityId{ 0 };
    std::uint64_t parentActivityId{ 0 };
    std::uint64_t seconds{ 0 };
    std::uint32_t microseconds{ 0 };
    OsTraceLevel level{ OsTraceLevel::Notice };
    std::uint64_t machTimestamp{ 0 };
    std::uint32_t threadId{ 0 };
    OsTraceUuid imageUuid;
    std::uint32_t imageOffset{ 0 };
    std::optional<std::string> processPath;
    std::optional<std::string> imagePath;
    std::optional<std::string> message;
    std::optional<std::string> subsystem;
    std::optional<std::string> category;
};

enum class OsTraceField : std::uint8_t {
    Packet,
    Header,
    Marker,
    PacketType,
    HeaderSize,
    Microseconds,
    Level,
    ProcessPath,
    ImagePath,
    Message,
    Subsystem,
    Category
};

enum class OsTraceDecodeErrorCode : std::uint8_t {
    PacketTooLarge,
    TruncatedHeader,
    InvalidMarker,
    UnknownPacketType,
    InvalidHeaderSize,
    InvalidTimestamp,
    UnknownLogLevel,
    SpanOutOfBounds,
    MissingNulTerminator,
    EmbeddedNul,
    UnexpectedTrailingData
};

struct OsTracePacketStructure {
    std::size_t packetByteCount{ 0u };
    std::uint8_t marker{ 0u };
    std::uint32_t wirePacketType{ 0u };
    std::uint32_t declaredHeaderByteCount{ 0u };
    std::array<std::uint32_t, 5u> fieldLengths{};
    std::uint64_t declaredSpanByteCount{ 0u };
    std::size_t availableVariableByteCount{ 0u };
};

struct OsTraceDecodeError {
    OsTraceDecodeErrorCode code{ OsTraceDecodeErrorCode::TruncatedHeader };
    OsTraceField field{ OsTraceField::Header };
    std::optional<OsTracePacketStructure> structure;
};

struct OsTraceDecodeResult {
    std::optional<DecodedOsTraceRecord> record;
    std::optional<OsTraceDecodeError> error;
};

struct OsTraceDecodeLimits {
    std::size_t maximumPacketSize{ DefaultMaximumOsTraceRecordSize };
};

OsTraceDecodeResult decodeOsTracePacket( const ByteBuffer& bytes,
                                         const OsTraceDecodeLimits& limits = {} );

enum class OsTraceCallbackPayloadKind : std::uint8_t { TracePacket, ControlPlist, Unknown };
OsTraceCallbackPayloadKind classifyOsTraceCallbackPayload( const ByteBuffer& bytes ) noexcept;

enum class OsTraceRelayRecordType : std::uint8_t { ControlPlist = 1, Activity = 2 };

struct OsTraceRelayRecord {
    OsTraceRelayRecordType type{ OsTraceRelayRecordType::Activity };
    ByteBuffer payload;
};

enum class OsTraceRelayErrorCode : std::uint8_t { UnknownRecordType, RecordTooLarge };

struct OsTraceRelayError {
    OsTraceRelayErrorCode code{ OsTraceRelayErrorCode::UnknownRecordType };
};

struct OsTraceRelayFeedResult {
    std::vector<OsTraceRelayRecord> records;
    std::optional<OsTraceRelayError> error;
    std::size_t bufferedByteCount{ 0 };
};

class OsTraceRelayFrameDecoder {
public:
    explicit OsTraceRelayFrameDecoder( std::size_t maximumRecordSize
                                       = DefaultMaximumOsTraceRecordSize );

    OsTraceRelayFeedResult feed( const ByteBuffer& bytes );
    void reset() noexcept;

private:
    std::size_t bufferedByteCount() const noexcept;
    void setError( OsTraceRelayErrorCode code ) noexcept;

    std::size_t maximumRecordSize_;
    std::array<std::uint8_t, 5> header_{};
    std::size_t headerByteCount_{ 0 };
    std::optional<std::size_t> expectedPayloadSize_;
    OsTraceRelayRecordType currentType_{ OsTraceRelayRecordType::Activity };
    ByteBuffer payload_;
    std::optional<OsTraceRelayError> error_;
};

struct OsTraceFormatOptions {
    bool ansiColors{ false };
    bool includeImageMetadata{ true };
    bool includeLabels{ true };
};

struct OsTraceFormatStatistics {
    std::size_t plainByteCount{ 0 };
    std::size_t outputByteCount{ 0 };
    std::size_t ansiEscapeByteCount{ 0 };
    std::size_t ansiExpansionByteCount{ 0 };
};

inline bool operator==( const OsTraceFormatStatistics& lhs,
                        const OsTraceFormatStatistics& rhs ) noexcept
{
    return lhs.plainByteCount == rhs.plainByteCount && lhs.outputByteCount == rhs.outputByteCount
           && lhs.ansiEscapeByteCount == rhs.ansiEscapeByteCount
           && lhs.ansiExpansionByteCount == rhs.ansiExpansionByteCount;
}

inline bool operator!=( const OsTraceFormatStatistics& lhs,
                        const OsTraceFormatStatistics& rhs ) noexcept
{
    return !( lhs == rhs );
}

using OsTraceFormatStatisticsHook = std::function<void( const OsTraceFormatStatistics& )>;

struct FormattedOsTraceRecord {
    std::string bytes;
    OsTraceFormatStatistics statistics;
};

FormattedOsTraceRecord formatOsTraceRecord( const DecodedOsTraceRecord& record,
                                            const OsTraceFormatOptions& options
                                            = OsTraceFormatOptions{},
                                            OsTraceFormatStatisticsHook statisticsHook = {} );

enum class BorrowedActivityCopyResult : std::uint8_t {
    Delivered,
    CallbackUnavailable,
    InvalidBorrowedBuffer,
    BufferTooLarge,
    AllocationFailed,
    CallbackFailed
};

class BorrowedActivityCallbackBridge {
public:
    using OwnedBytesCallback = std::function<void( ByteBuffer )>;

    explicit BorrowedActivityCallbackBridge( OwnedBytesCallback callback,
                                             std::size_t maximumByteCount
                                             = DefaultMaximumOsTraceRecordSize );
    BorrowedActivityCopyResult operator()( const void* borrowedBytes,
                                           std::size_t byteCount ) const noexcept;

private:
    OwnedBytesCallback callback_;
    std::size_t maximumByteCount_;
};

} // namespace klogg::livecapture::ios
