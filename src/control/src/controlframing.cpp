#include "controlframing.h"

#include <algorithm>
#include <utility>

#include <QtEndian>

#include "qtcontainerindex.h"

namespace klogg::control {

namespace {
constexpr ContainerIndex HeaderBytes = 4;
}

std::optional<QByteArray> encodeFrame( const QByteArray& payload )
{
    if ( payload.isEmpty() || static_cast<uint64_t>( payload.size() ) > MaxFrameBytes ) {
        return {};
    }
    QByteArray frame( HeaderBytes, '\0' );
    qToBigEndian<uint32_t>( static_cast<uint32_t>( payload.size() ), frame.data() );
    frame.append( payload );
    return frame;
}

FrameDecoder::Result FrameDecoder::fail( FrameError reason )
{
    error_ = reason;
    header_.clear();
    payload_.clear();
    expected_ = 0;
    return { error_, {} };
}

FrameDecoder::Result FrameDecoder::feed( const QByteArray& bytes )
{
    if ( error_ != FrameError::None ) {
        return { error_, {} };
    }
    if ( closed_ ) {
        return fail( FrameError::Closed );
    }

    Result result;
    ContainerIndex offset = 0;
    while ( offset < bytes.size() ) {
        if ( expected_ == 0 ) {
            const auto count = std::min( HeaderBytes - header_.size(), bytes.size() - offset );
            header_.append( bytes.constData() + offset, count );
            offset += count;
            if ( header_.size() != HeaderBytes ) {
                continue;
            }
            expected_ = qFromBigEndian<uint32_t>( header_.constData() );
            header_.clear();
            if ( expected_ == 0 ) {
                return fail( FrameError::InvalidLength );
            }
            if ( expected_ > MaxFrameBytes ) {
                return fail( FrameError::TooLarge );
            }
        }

        const auto remaining = static_cast<ContainerIndex>( expected_ ) - payload_.size();
        const auto count = std::min( remaining, bytes.size() - offset );
        payload_.append( bytes.constData() + offset, count );
        offset += count;
        if ( payload_.size() == static_cast<ContainerIndex>( expected_ ) ) {
            if ( result.messages.size() == MaxMessagesPerFeed ) {
                return fail( FrameError::TooManyFrames );
            }
            result.messages.push_back( std::move( payload_ ) );
            payload_.clear();
            expected_ = 0;
        }
    }
    return result;
}

FrameError FrameDecoder::finish()
{
    closed_ = true;
    if ( error_ != FrameError::None ) {
        return error_;
    }
    if ( !header_.isEmpty() || expected_ != 0 ) {
        return fail( FrameError::Truncated ).error;
    }
    return FrameError::None;
}

} // namespace klogg::control
