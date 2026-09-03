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

#include <QByteArray>
#include <QHostAddress>
#include <QMetaType>
#include <QObject>
#include <QString>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "adbprotocol.h"
#include "livestate.h"

class QTcpSocket;

namespace klogg::livecapture::adb {

using DeadlineToken = std::uint64_t;

enum class AdbSmartSocketDeadlineKind : std::uint8_t { Connect, Write, Read };

class AdbSmartSocketFactory {
public:
    virtual ~AdbSmartSocketFactory() = default;
    virtual QTcpSocket* createSocket( QObject* parent ) = 0;
    virtual qint64 readSocket( QTcpSocket& socket, char* data, qint64 maxSize );
    virtual qint64 writeSocket( QTcpSocket& socket, const char* data, qint64 maxSize );
};

class AdbSmartSocketDeadlineScheduler {
public:
    virtual ~AdbSmartSocketDeadlineScheduler() = default;

    virtual DeadlineToken armDeadline( AdbSmartSocketDeadlineKind kind, int timeoutMs,
                                       QObject* context, std::function<void()> callback ) = 0;
    virtual void cancelDeadline( DeadlineToken token ) = 0;
};

struct AdbSmartSocketClientConfig {
    QHostAddress serverAddress{ QHostAddress::LocalHost };
    quint16 serverPort{ 5037 };
    qint64 maxReadChunkBytes{ qint64{ 64 } * 1024 };
    qint64 maxWriteChunkBytes{ qint64{ 64 } * 1024 };
    std::size_t maxHostReplyBytes{ 0xffffu };
    std::size_t maxShellFrameBytes{ std::size_t{ 16u } * 1024u * 1024u };
    int connectTimeoutMs{ 5000 };
    int writeTimeoutMs{ 5000 };
    int readTimeoutMs{ 5000 };
};

enum class AdbSmartSocketErrorCode : std::uint8_t {
    Connection,
    Protocol,
    RemoteFailure,
    UnexpectedEof,
    ConnectTimeout,
    WriteTimeout,
    ReadTimeout
};

class AdbSmartSocketClient final : public QObject {
    Q_OBJECT

public:
    using OperationId = std::uint64_t;

    explicit AdbSmartSocketClient( AdbSmartSocketClientConfig config, QObject* parent = nullptr );
    // Injected dependencies are non-owning and must outlive the client.
    AdbSmartSocketClient( AdbSmartSocketClientConfig config, AdbSmartSocketFactory& socketFactory,
                          AdbSmartSocketDeadlineScheduler& deadlineScheduler,
                          QObject* parent = nullptr );
    ~AdbSmartSocketClient() override;

    AdbSmartSocketClient( const AdbSmartSocketClient& ) = delete;
    AdbSmartSocketClient& operator=( const AdbSmartSocketClient& ) = delete;

    void requestHostService( Generation generation, OperationId operationId, HostService service );
    void requestTransportHostService( Generation generation, OperationId operationId,
                                      const TransportSelection& transport,
                                      TransportHostService service );
    void startShellService( Generation generation, OperationId operationId,
                            const TransportSelection& transport, const std::string& service );
    void cancelGeneration( Generation generation );

Q_SIGNALS:
    void operationConnected( klogg::livecapture::Generation generation,
                             AdbSmartSocketClient::OperationId operationId );
    void hostReplyReceived( klogg::livecapture::Generation generation,
                            AdbSmartSocketClient::OperationId operationId,
                            const QByteArray& reply );
    void shellServiceStarted( klogg::livecapture::Generation generation,
                              AdbSmartSocketClient::OperationId operationId );
    void shellStdoutReceived( klogg::livecapture::Generation generation,
                              AdbSmartSocketClient::OperationId operationId,
                              const QByteArray& bytes );
    void shellStderrReceived( klogg::livecapture::Generation generation,
                              AdbSmartSocketClient::OperationId operationId,
                              const QByteArray& bytes );
    void shellExited( klogg::livecapture::Generation generation,
                      AdbSmartSocketClient::OperationId operationId, std::uint8_t exitCode );
    void errorOccurred( klogg::livecapture::Generation generation,
                        AdbSmartSocketClient::OperationId operationId, AdbSmartSocketErrorCode code,
                        const QString& diagnostic );

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace klogg::livecapture::adb

Q_DECLARE_METATYPE( klogg::livecapture::adb::AdbSmartSocketErrorCode )
