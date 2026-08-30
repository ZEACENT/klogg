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
#include <optional>
#include <string>
#include <vector>

namespace klogg::livecapture::adb {

using ByteVector = std::vector<std::uint8_t>;

enum class ProtocolErrorCode : std::uint8_t {
    EmptyPayload,
    PayloadTooLarge,
    InvalidHexLength,
    InvalidStatus,
    FrameTooLarge,
    UnknownShellV2Channel,
    InvalidShellV2Frame,
    InvalidCommandOption,
    BufferOverflow
};

struct ProtocolError {
    ProtocolErrorCode code{ ProtocolErrorCode::InvalidStatus };
    std::string message;
};

template <typename Value>
struct ProtocolResult {
    std::optional<Value> value;
    std::optional<ProtocolError> error;
};

ProtocolResult<ByteVector> encodeSmartSocketRequest( const ByteVector& payload );
ProtocolResult<std::uint16_t>
parseSmartSocketHexLength( const std::array<std::uint8_t, 4>& encodedLength );

enum class SmartSocketStatusKind : std::uint8_t { Okay, Fail };

struct SmartSocketStatus {
    SmartSocketStatusKind kind{ SmartSocketStatusKind::Okay };
    ByteVector message;
};

template <typename Frame>
struct DecoderFeedResult {
    std::vector<Frame> frames;
    // Bytes following a terminal frame that belong to the next protocol phase.
    ByteVector unconsumedBytes;
    std::optional<ProtocolError> error;
    std::size_t bufferedByteCount{ 0 };
};

// Decodes exactly one smart-socket status and returns any coalesced next-phase bytes.
class SmartSocketStatusDecoder {
public:
    explicit SmartSocketStatusDecoder( std::size_t maxFailureMessageSize = 0xffffu );

    DecoderFeedResult<SmartSocketStatus> feed( const ByteVector& bytes );
    void reset() noexcept;

private:
    std::size_t maxFailureMessageSize_;
    ByteVector buffer_;
    std::optional<ProtocolError> error_;
    bool complete_{ false };
};

class LengthPrefixedHostReplyDecoder {
public:
    explicit LengthPrefixedHostReplyDecoder( std::size_t maxPayloadSize = 0xffffu );

    DecoderFeedResult<ByteVector> feed( const ByteVector& bytes );
    void reset() noexcept;

private:
    std::size_t maxPayloadSize_;
    ByteVector buffer_;
    std::optional<ProtocolError> error_;
};

enum class ShellV2Channel : std::uint8_t {
    Stdin = 0,
    Stdout = 1,
    Stderr = 2,
    Exit = 3,
    CloseStdin = 4,
    WindowSizeChange = 5
};

struct ShellV2Frame {
    ShellV2Channel channel{ ShellV2Channel::Stdout };
    std::uint8_t wireId{ 1 };
    ByteVector payload;
};

class ShellV2FrameDecoder {
public:
    explicit ShellV2FrameDecoder( std::size_t maxPayloadSize = std::size_t{ 16u } * 1024u * 1024u );

    DecoderFeedResult<ShellV2Frame> feed( const ByteVector& bytes );
    void reset() noexcept;

private:
    std::size_t maxPayloadSize_;
    ByteVector buffer_;
    std::optional<ProtocolError> error_;
};

enum class HostService : std::uint8_t { Version, Features, DevicesLong, TrackDevicesLong };

ProtocolResult<std::string> buildHostService( HostService service );

enum class TransportKind : std::uint8_t { Any, Usb, Local, Serial };

struct TransportSelection {
    TransportKind kind{ TransportKind::Any };
    std::string serial;
};

ProtocolResult<std::string> buildTransportService( const TransportSelection& selection );

enum class LogBuffer : std::uint8_t { Main, System, Radio, Events, Crash };
enum class LogPriority : std::uint8_t { Verbose, Debug, Info, Warn, Error, Fatal, Silent };

struct LogcatFilter {
    std::string tag;
    LogPriority priority{ LogPriority::Verbose };
};

struct LogcatCommandOptions {
    bool ansiOutputEnabled{ false };
    std::vector<LogBuffer> buffers;
    std::optional<std::uint32_t> initialTailLineCount;
    std::optional<std::uint32_t> processId;
    std::vector<LogcatFilter> filters;
};

ProtocolResult<std::string> buildLogcatService( const LogcatCommandOptions& options );
std::string buildClearLogcatService();

} // namespace klogg::livecapture::adb
