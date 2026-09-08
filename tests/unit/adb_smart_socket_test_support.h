/*
 * Copyright (C) 2026 ZEACENT and other contributors
 *
 * This file is part of klogg.
 *
 * klogg is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <algorithm>
#include <functional>
#include <utility>

#include <QByteArray>
#include <QHostAddress>
#include <QIODevice>
#include <QPointer>
#include <QTcpSocket>
#include <QVector>

#include "adbsmartsocketclient.h"

namespace klogg::test {

class DeterministicAdbSocket final : public QTcpSocket {
public:
    explicit DeterministicAdbSocket( QObject* parent = nullptr )
        : QTcpSocket( parent )
    {
    }

    void connectToHost( const QString&, quint16, QIODevice::OpenMode mode = QIODevice::ReadWrite,
                        QAbstractSocket::NetworkLayerProtocol
                            = QAbstractSocket::AnyIPProtocol ) override
    {
        setOpenMode( mode );
        setSocketState( QAbstractSocket::ConnectedState );
        Q_EMIT connected();
    }

    void disconnectFromHost() override
    {
        closePeer();
    }

    qint64 bytesAvailable() const override
    {
        return static_cast<qint64>( incoming_.size() );
    }

    qint64 bytesToWrite() const override
    {
        return 0;
    }

    void pushIncoming( QByteArray bytes, bool notify = true )
    {
        incoming_.append( std::move( bytes ) );
        if ( notify ) {
            Q_EMIT readyRead();
        }
    }

    void closePeer()
    {
        if ( state() == QAbstractSocket::UnconnectedState ) {
            return;
        }
        setOpenMode( QIODevice::NotOpen );
        setSocketState( QAbstractSocket::UnconnectedState );
        Q_EMIT stateChanged( QAbstractSocket::UnconnectedState );
        Q_EMIT disconnected();
    }

    const QByteArray& writtenBytes() const noexcept
    {
        return written_;
    }

    const QVector<qint64>& requestedReadSizes() const noexcept
    {
        return requestedReadSizes_;
    }

    void setAfterRead( std::function<void()> callback )
    {
        afterRead_ = std::move( callback );
    }

protected:
    qint64 readData( char* data, qint64 maxSize ) override
    {
        requestedReadSizes_.append( maxSize );
        const auto available = static_cast<qint64>( incoming_.size() );
        const auto byteCount = std::min( maxSize, available );
        if ( byteCount <= 0 ) {
            return 0;
        }
        std::copy_n( incoming_.constData(), byteCount, data );
        incoming_.remove( 0, static_cast<int>( byteCount ) );
        if ( afterRead_ ) {
            afterRead_();
        }
        return byteCount;
    }

    qint64 writeData( const char* data, qint64 maxSize ) override
    {
        written_.append( data, static_cast<int>( maxSize ) );
        return maxSize;
    }

private:
    QByteArray incoming_;
    QByteArray written_;
    QVector<qint64> requestedReadSizes_;
    std::function<void()> afterRead_;
};

class DeterministicAdbSocketFactory final
    : public klogg::livecapture::adb::AdbSmartSocketFactory {
public:
    QTcpSocket* createSocket( QObject* parent ) override
    {
        // Ownership is transferred to the supplied Qt parent.
        // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
        auto* const socket = new DeterministicAdbSocket( parent );
        sockets_.append( socket );
        return socket;
    }

    DeterministicAdbSocket* socketAt( int index ) const
    {
        return sockets_.at( index ).data();
    }

    int socketCount() const noexcept
    {
        return static_cast<int>( sockets_.size() );
    }

private:
    QVector<QPointer<DeterministicAdbSocket>> sockets_;
};

} // namespace klogg::test
