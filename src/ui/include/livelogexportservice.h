/*
 * Copyright (C) 2026
 *
 * This file is part of klogg.
 */

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

#include <QEventLoop>
#include <QObject>
#include <QString>

#include "streaminglogdata.h"

namespace klogg::livelog {

enum class LiveLogExportResult : std::uint8_t {
    Succeeded,
    Cancelled,
    Busy,
    TailOverflow,
    PartialUnknown,
    SnapshotReadFailed,
    WriteFailed,
    PublishFailed,
    PublishedReopenFailed,
    PublishedCutoverFailed,
};

class LiveLogExportJob final : public QObject,
                               public std::enable_shared_from_this<LiveLogExportJob> {
    Q_OBJECT

public:
    ~LiveLogExportJob() override;

    void cancel();
    void waitForFinished();
    void onFinished( QObject* context,
                     std::function<void( LiveLogExportResult )> callback );
    bool isFinished() const;
    std::optional<LiveLogExportResult> result() const;
    QString outputPath() const;
    std::thread::id workerThreadId() const;

Q_SIGNALS:
    void progressChanged( qint64 bytesWritten );
    void finished( klogg::livelog::LiveLogExportResult result );

private:
    friend class LiveLogExportService;
    friend struct LiveLogExportServiceTestAccess;

    LiveLogExportJob( std::shared_ptr<StreamingLogData> data,
                      StreamingLogData::OutputExportCandidate candidate,
                      QString outputPath, std::function<void()> beforeSnapshotWrite,
                      std::function<void()> beforePublication,
                      std::function<void()> afterPublish );
    void start();
    void run();
    void complete( LiveLogExportResult result );
    void cancelCandidate();
    StreamingLogData::OutputExportTail takeCandidateTail();
    static LiveLogExportResult mapFailure( StreamingLogData::OutputExportFailure failure );

    enum class PublicationDecision : std::uint8_t {
        Writing,
        Cancelled,
        Publishing,
    };

    std::shared_ptr<StreamingLogData> data_;
    StreamingLogData::OutputExportCandidate candidate_;
    QString outputPath_;
    std::thread worker_;
    std::function<void()> beforeSnapshotWrite_;
    std::function<void()> beforePublication_;
    std::function<void()> afterPublish_;
    std::function<void( QEventLoop::ProcessEventsFlags, int )> ownerEventPumpForTesting_;
    std::atomic<PublicationDecision> publicationDecision_{ PublicationDecision::Writing };
    mutable std::mutex stateMutex_;
    std::condition_variable finishedCondition_;
    std::optional<LiveLogExportResult> result_;
    std::thread::id workerThreadId_;
    std::thread::id dataAccessThreadId_;
};

class LiveLogExportService final {
public:
    explicit LiveLogExportService( std::shared_ptr<StreamingLogData> data );
    ~LiveLogExportService();

    std::shared_ptr<LiveLogExportJob>
    start( const QString& outputPath, LiveLogSaveAnsiMode ansiMode,
           qint64 maximumTailBytes = 16LL * 1024 * 1024 );
    std::shared_ptr<LiveLogExportJob> activeJob() const;
    void cancelAndWait() const;

private:
    friend struct LiveLogExportServiceTestAccess;
    std::shared_ptr<StreamingLogData> data_;
    mutable std::mutex mutex_;
    std::shared_ptr<LiveLogExportJob> activeJob_;
    std::function<void()> beforeSnapshotWriteForTesting_;
    std::function<void()> beforePublicationForTesting_;
    std::function<void()> afterPublishForTesting_;
};

} // namespace klogg::livelog

Q_DECLARE_METATYPE( klogg::livelog::LiveLogExportResult )
