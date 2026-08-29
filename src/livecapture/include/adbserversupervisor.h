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

#include <QHostAddress>
#include <QObject>
#include <QString>
#include <QStringList>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "livestate.h"

namespace klogg::livecapture::adb {

using AdbServerToken = std::uint64_t;

struct AdbServerEndpoint {
    QHostAddress address{ QHostAddress::LocalHost };
    std::uint16_t port{ 5037 };
};

enum class AdbServerProbeState : std::uint8_t { Absent, Ready, Failed };

struct AdbServerProbeResult {
    AdbServerProbeState state{ AdbServerProbeState::Failed };
    std::uint32_t protocolVersion{ 0 };
    std::vector<std::string> features;
    std::string serverIdentity;
    std::string diagnostic;
};

class AdbServerProbe {
public:
    using Callback = std::function<void( AdbServerProbeResult )>;

    virtual ~AdbServerProbe() = default;
    virtual AdbServerToken probe( const AdbServerEndpoint& endpoint, Callback callback ) = 0;
    virtual void cancel( AdbServerToken token ) = 0;
};

struct AdbServerLaunchRequest {
    QString executable;
    QStringList arguments;
    bool allowPathLookup{ false };
    AdbServerEndpoint endpoint;
};

enum class AdbServerLaunchState : std::uint8_t { Started, Failed, Exited };

struct AdbServerLaunchResult {
    AdbServerLaunchState state{ AdbServerLaunchState::Failed };
    bool cleanupPermitted{ false };
    std::string diagnostic;
};

class AdbServerLauncher {
public:
    using Callback = std::function<void( AdbServerLaunchResult )>;

    virtual ~AdbServerLauncher() = default;
    virtual AdbServerToken launch( const AdbServerLaunchRequest& request, Callback callback ) = 0;
    virtual void cleanup( AdbServerToken token ) = 0;
    // Relinquishes lifecycle control after the server is published as shared;
    // the launched process must continue independently of this launcher.
    virtual void release( AdbServerToken token ) = 0;
};

enum class AdbServerStartupLockState : std::uint8_t { Acquired, Contended, Failed };

struct AdbServerStartupLockResult {
    AdbServerStartupLockState state{ AdbServerStartupLockState::Failed };
    std::string diagnostic;
};

class AdbServerStartupLock {
public:
    using Callback = std::function<void( AdbServerStartupLockResult )>;

    virtual ~AdbServerStartupLock() = default;
    virtual AdbServerToken acquire( const QString& lockPath, Callback callback ) = 0;
    virtual void cancel( AdbServerToken token ) = 0;
    virtual void release( AdbServerToken token ) = 0;
};

enum class AdbServerStandardKeyState : std::uint8_t { Present, Absent, Failed };

struct AdbServerKeyInspection {
    AdbServerStandardKeyState state{ AdbServerStandardKeyState::Failed };
    std::string diagnostic;
};

struct AdbServerKeyGenerationResult {
    bool generated{ false };
    std::string diagnostic;
};

class AdbKeyStore {
public:
    virtual ~AdbKeyStore() = default;
    virtual AdbServerKeyInspection inspectStandardKey() = 0;
    virtual AdbServerKeyGenerationResult generateStandardKey() = 0;
};

enum class AdbServerScheduleKind : std::uint8_t {
    ReadinessProbe,
    StartupTimeout,
    HealthProbe,
    LockRetry,
    StartupRetry,
    ReconnectBackoff
};

class AdbServerScheduler {
public:
    using Callback = std::function<void()>;

    virtual ~AdbServerScheduler() = default;
    virtual AdbServerToken schedule( AdbServerScheduleKind kind, std::chrono::milliseconds delay,
                                     Callback callback ) = 0;
    virtual void cancel( AdbServerToken token ) = 0;
};

struct AdbServerSupervisorConfig {
    AdbServerEndpoint endpoint;
    QString packagedServerPath;
    QString lockPath;
    std::uint32_t minimumProtocolVersion{ 0 };
    std::vector<std::string> requiredFeatures;
    std::chrono::milliseconds readinessProbeInterval{ 100 };
    std::chrono::milliseconds startupTimeout{ 5000 };
    std::chrono::milliseconds healthProbeInterval{ 1000 };
    std::vector<std::chrono::milliseconds> reconnectBackoff{ std::chrono::milliseconds{ 250 } };
    // Optional app-layer preflight failure (for example, a missing packaged
    // helper). The lower layer surfaces it without knowing package layout.
    std::optional<LiveSourceError> configurationError;
};

enum class AdbServerSupervisorStatus : std::uint8_t {
    Stopped,
    InvalidConfiguration,
    Probing,
    WaitingForStartupLock,
    AwaitingKeyGenerationConsent,
    Starting,
    Ready,
    RetryWait,
    Incompatible,
    Failed
};

enum class AdbServerKeyConsentState : std::uint8_t { NotRequired, Required, Granted, Denied };

struct AdbServerSupervisorSnapshot {
    Generation generation{ 0 };
    std::uint64_t epoch{ 0 };
    AdbServerSupervisorStatus status{ AdbServerSupervisorStatus::Stopped };
    InfrastructureState infrastructure;
    AdbServerKeyConsentState keyConsent{ AdbServerKeyConsentState::NotRequired };
    std::string serverIdentity;
    std::uint32_t protocolVersion{ 0 };
    std::optional<LiveSourceError> error;
};

// Coordinates one shared ADB server endpoint. Dependencies are non-owning and
// must outlive the supervisor; all callbacks are guarded against stale runs.
// Qt production adapters. They only use the explicit endpoint/path supplied by
// the caller; no PATH or Android SDK discovery is performed.
class AdbSmartSocketServerProbe final : public QObject, public AdbServerProbe {
public:
    explicit AdbSmartSocketServerProbe( QObject* parent = nullptr );
    ~AdbSmartSocketServerProbe() override;

    AdbServerToken probe( const AdbServerEndpoint& endpoint, Callback callback ) override;
    void cancel( AdbServerToken token ) override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

class QtAdbServerLauncher final : public QObject, public AdbServerLauncher {
public:
    explicit QtAdbServerLauncher( QObject* parent = nullptr );
    ~QtAdbServerLauncher() override;

    AdbServerToken launch( const AdbServerLaunchRequest& request, Callback callback ) override;
    void cleanup( AdbServerToken token ) override;
    void release( AdbServerToken token ) override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

class QtAdbServerStartupLock final : public QObject, public AdbServerStartupLock {
public:
    explicit QtAdbServerStartupLock( QObject* parent = nullptr );
    ~QtAdbServerStartupLock() override;

    AdbServerToken acquire( const QString& lockPath, Callback callback ) override;
    void cancel( AdbServerToken token ) override;
    void release( AdbServerToken token ) override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

class StandardAdbKeyStore final : public AdbKeyStore {
public:
    explicit StandardAdbKeyStore( QString privateKeyPath = {} );

    AdbServerKeyInspection inspectStandardKey() override;
    AdbServerKeyGenerationResult generateStandardKey() override;

private:
    QString privateKeyPath_;
};

class QtAdbServerScheduler final : public QObject, public AdbServerScheduler {
public:
    explicit QtAdbServerScheduler( QObject* parent = nullptr );
    ~QtAdbServerScheduler() override;

    AdbServerToken schedule( AdbServerScheduleKind kind, std::chrono::milliseconds delay,
                             Callback callback ) override;
    void cancel( AdbServerToken token ) override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

class AdbServerSupervisor final : public QObject {
    Q_OBJECT

public:
    AdbServerSupervisor( AdbServerSupervisorConfig config, AdbServerProbe& probe,
                         AdbServerLauncher& launcher, AdbServerStartupLock& startupLock,
                         AdbKeyStore& keyStore, AdbServerScheduler& scheduler,
                         QObject* parent = nullptr );
    ~AdbServerSupervisor() override;

    AdbServerSupervisor( const AdbServerSupervisor& ) = delete;
    AdbServerSupervisor& operator=( const AdbServerSupervisor& ) = delete;

    void start( Generation generation );
    void stop( Generation generation );
    void grantKeyGenerationConsent( Generation generation, bool granted );

    const AdbServerSupervisorSnapshot& snapshot() const noexcept;

Q_SIGNALS:
    void stateChanged( klogg::livecapture::Generation generation, std::uint64_t epoch,
                       const klogg::livecapture::adb::AdbServerSupervisorSnapshot& snapshot );
    void consentRequired( klogg::livecapture::Generation generation, std::uint64_t epoch );
    void errorOccurred( klogg::livecapture::Generation generation, std::uint64_t epoch,
                        const klogg::livecapture::LiveSourceError& error );

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace klogg::livecapture::adb
