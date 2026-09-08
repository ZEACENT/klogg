/*
 * Copyright (C) 2026
 *
 * This file is part of klogg.
 */

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

#include <QElapsedTimer>
#include <QObject>
#include <QTimer>

#include "adblogcatsource.h"
#include "livelogexportservice.h"

namespace klogg::livelog {

class LiveLogController;

class LiveLogCloseTransaction final : public QObject {
    Q_OBJECT

public:
    enum class Mode : std::uint8_t { Preserve, Discard };
    enum class Result : std::uint8_t { ReadyToRemove, Cancelled, CloseAnywayPossibleLoss };
    enum class FailureKind : std::uint8_t { StopTimeout, Persistence, OutputFlush };

    struct Config {
        int stopTimeoutMs = 30'000;
    };

    struct Failure {
        FailureKind kind = FailureKind::Persistence;
        CaptureStore::PersistenceResult persistence;
        std::optional<CaptureOutputError> outputError;
    };

    using FailureCallback = std::function<void( const Failure& )>;
    using FinishedCallback = std::function<void( Result )>;

    LiveLogCloseTransaction( LiveLogController& controller, AdbLogcatSource& source,
                             LiveLogExportService& exportService, Mode mode,
                             QObject* parent = nullptr );
    LiveLogCloseTransaction( LiveLogController& controller, AdbLogcatSource& source,
                             LiveLogExportService& exportService, Mode mode,
                             Config config, QObject* parent = nullptr );

    void setCallbacks( FailureCallback failure, FinishedCallback finished );
    void start();
    void retry();
    void cancel();
    void closeAnywayPossibleLoss();
    bool isRunning() const noexcept;

private:
    enum class Stage : std::uint8_t {
        Idle,
        AwaitingStop,
        AwaitingExport,
        Persisting,
        AwaitingDecision,
        Finished,
    };

    void scheduleAdvance( int delayMs = 1 );
    void advance();
    void persistTurn();
    void flushAndFinish();
    void finish( Result result );

    LiveLogController& controller_;
    AdbLogcatSource& source_;
    LiveLogExportService& exportService_;
    Mode mode_;
    Config config_;
    QTimer timer_;
    QElapsedTimer stopWait_;
    FailureCallback failureCallback_;
    FinishedCallback finishedCallback_;
    Stage stage_{ Stage::Idle };
    std::optional<FailureKind> lastFailureKind_;
    std::optional<CaptureStore::PersistenceResult> lastPersistence_;
};

} // namespace klogg::livelog
