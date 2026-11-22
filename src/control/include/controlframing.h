#ifndef KLOGG_CONTROLFRAMING_H
#define KLOGG_CONTROLFRAMING_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include <QByteArray>

namespace klogg::control {

inline constexpr uint32_t MaxFrameBytes = 1024U * 1024U;

enum class FrameError : uint8_t {
    None, InvalidLength, TooLarge, TooManyFrames, Truncated, Closed
};

// Framing only: JSON validation, protocol negotiation, authorization, and
// application dispatch belong to later stages. A frame carries uninterpreted
// bytes, prefixed by an unsigned, four-byte network-order payload length.
std::optional<QByteArray> encodeFrame( const QByteArray& payload );

class FrameDecoder {
  public:
    static constexpr size_t MaxMessagesPerFeed = 64;
    struct Result {
        FrameError error = FrameError::None;
        std::vector<QByteArray> messages;
    };

    // Connections must use bounded socket reads and drain returned frames.
    // On error, no prefix from this feed is returned for dispatch; earlier
    // successful feeds are independent requests, not a rollback boundary.
    Result feed( const QByteArray& bytes );
    FrameError finish();
    FrameError error() const { return error_; }

  private:
    Result fail( FrameError reason );

    QByteArray header_;
    QByteArray payload_;
    uint32_t expected_ = 0;
    FrameError error_ = FrameError::None;
    bool closed_ = false;
};

} // namespace klogg::control

#endif
