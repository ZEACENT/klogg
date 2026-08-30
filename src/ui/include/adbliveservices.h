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

#ifndef KLOGG_ADBLIVESERVICES_H
#define KLOGG_ADBLIVESERVICES_H

#include <QObject>
#include <QString>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "adbinfrastructuremanager.h"
#include "adbserversupervisor.h"
#include "adbsmartsocketclient.h"
#include "livesourcetransport.h"

class AdbTrackedDeviceProvider;

struct AdbLiveServicesConfig {
    QString applicationDirPath;
    QString perUserRuntimeDir;
    klogg::livecapture::adb::AdbServerEndpoint endpoint;
    std::uint32_t minimumProtocolVersion{ 0 };
    std::vector<std::string> requiredFeatures;
    std::chrono::milliseconds readinessProbeInterval{ 100 };
    std::chrono::milliseconds startupTimeout{ 5000 };
    std::chrono::milliseconds healthProbeInterval{ 1000 };
    std::vector<std::chrono::milliseconds> serverReconnectBackoff{ std::chrono::milliseconds{
        250 } };
    std::vector<std::chrono::milliseconds> trackerReconnectBackoff{ std::chrono::milliseconds{
        250 } };
};

struct AdbLiveServicesDependencies {
    // The application composition root owns these dependencies for longer than
    // AdbLiveServices. Tests inject deterministic implementations here.
    // NOLINTBEGIN(cppcoreguidelines-avoid-const-or-ref-data-members)
    klogg::livecapture::adb::AdbServerProbe& probe;
    klogg::livecapture::adb::AdbServerLauncher& launcher;
    klogg::livecapture::adb::AdbServerStartupLock& startupLock;
    klogg::livecapture::adb::AdbKeyStore& keyStore;
    klogg::livecapture::adb::AdbServerScheduler& supervisorScheduler;
    klogg::livecapture::adb::AdbSmartSocketFactory& trackerSocketFactory;
    klogg::livecapture::adb::AdbSmartSocketDeadlineScheduler& trackerDeadlineScheduler;
    klogg::livecapture::adb::AdbServerScheduler& trackerScheduler;
    klogg::livecapture::adb::AdbSmartSocketFactory& transportSocketFactory;
    klogg::livecapture::adb::AdbSmartSocketDeadlineScheduler& transportDeadlineScheduler;
    const LiveSourceTransportFactory* managedTransportFactory{ nullptr };
    // NOLINTEND(cppcoreguidelines-avoid-const-or-ref-data-members)
};

// Application/UI composition root for all ADB live services. The manager and
// tracker remain domain components in livecapture; this class owns and wires one
// shared instance of each into dialogs and managed SmartSocket transports.
class AdbLiveServices final : public QObject, public LiveSourceTransportFactory {
    Q_OBJECT

public:
    explicit AdbLiveServices( AdbLiveServicesConfig config, QObject* parent = nullptr );
    AdbLiveServices( AdbLiveServicesConfig config, AdbLiveServicesDependencies dependencies,
                     QObject* parent = nullptr );
    ~AdbLiveServices() override;

    AdbLiveServices( const AdbLiveServices& ) = delete;
    AdbLiveServices& operator=( const AdbLiveServices& ) = delete;

    static QString packagedHelperPath( const QString& applicationDirPath );
    static QString perUserLockPath( const QString& perUserRuntimeDir );

    bool isPackagedHelperAvailable() const noexcept;
    klogg::livecapture::adb::AdbInfrastructureManager& manager() noexcept;
    const klogg::livecapture::adb::AdbInfrastructureManager& manager() const noexcept;
    AdbTrackedDeviceProvider& trackedDeviceProvider() noexcept;

    std::unique_ptr<LiveSourceTransport>
    create( const LiveSourceTransportConfig& config ) const override;

    void answerKeyGenerationConsent( klogg::livecapture::Generation generation,
                                     std::uint64_t infrastructureEpoch, bool granted );
    void shutdown();

Q_SIGNALS:
    void keyGenerationConsentRequested( klogg::livecapture::Generation generation,
                                        std::uint64_t infrastructureEpoch );

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

#endif // KLOGG_ADBLIVESERVICES_H
