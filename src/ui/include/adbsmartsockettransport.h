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

#include <cstddef>
#include <memory>

#include <QString>

#include "adbprotocol.h"
#include "adbsmartsocketclient.h"
#include "livedataqueue.h"
#include "livesourcetransport.h"

namespace klogg::livecapture::adb {

struct AdbSmartSocketTransportConfig {
    AdbSmartSocketClientConfig clientConfig;
    QString deviceSerial;
    LogcatCommandOptions logcatOptions;
    LiveDataQueueLimits queueLimits{ std::size_t{ 4u } * 1024u * 1024u, 256u };
};

class AdbSmartSocketTransport final : public LiveSourceTransport {
    Q_OBJECT

public:
    explicit AdbSmartSocketTransport( AdbSmartSocketTransportConfig config,
                                      QObject* parent = nullptr );
    // Injected dependencies are non-owning and must outlive the transport.
    AdbSmartSocketTransport( AdbSmartSocketTransportConfig config,
                             AdbSmartSocketFactory& socketFactory,
                             AdbSmartSocketDeadlineScheduler& deadlineScheduler,
                             QObject* parent = nullptr );
    ~AdbSmartSocketTransport() override;

    AdbSmartSocketTransport( const AdbSmartSocketTransport& ) = delete;
    AdbSmartSocketTransport& operator=( const AdbSmartSocketTransport& ) = delete;

    void start( Generation generation ) override;
    void stop( Generation generation ) override;
    void clearRemoteAsync( Generation generation, ClearRequestId requestId ) override;
    QString lastError() const override;
    LiveDataStatistics statistics() const override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace klogg::livecapture::adb
