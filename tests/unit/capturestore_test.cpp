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

#include <catch2/catch.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTextCodec>
#include <QUuid>

#include "capturestore.h"
#include "rollingfilemanager.h"

class CaptureStoreTestAccess {
  public:
    static void setBeforeRawSnapshotCopyCallback( CaptureStore& store,
                                                  std::function<void()> callback )
    {
        const std::lock_guard<std::recursive_mutex> lock( store.mutex_ );
        store.beforeRawSnapshotCopyCallbackForTesting_ = std::move( callback );
    }

    static void forceActiveStorageRelocation( CaptureStore& store )
    {
        const std::lock_guard<std::recursive_mutex> lock( store.mutex_ );
        auto& activeData = *store.segments_.back().memoryData;
        QByteArray relocated = activeData;
        relocated.detach();
        activeData.swap( relocated );
        relocated.fill( 'z' );
    }

    static void setBeforeSpilledSegmentReadCallback( CaptureStore& store,
                                                     std::function<void()> callback )
    {
        const std::lock_guard<std::recursive_mutex> lock( store.mutex_ );
        store.beforeSpilledSegmentReadCallbackForTesting_ = std::move( callback );
    }

    static bool canLockStoreMutex( CaptureStore& store )
    {
        if ( !store.mutex_.try_lock() ) {
            return false;
        }
        store.mutex_.unlock();
        return true;
    }

    static void failNextRetiredFileRemoval( CaptureStore& store )
    {
        store.failNextRetiredFileRemovalForTesting();
    }

    static void failNextCaptureDirectoryRemoval( CaptureStore& store )
    {
        store.failNextCaptureDirectoryRemovalForTesting();
    }

    static void failNextSegmentWrite( CaptureStore& store )
    {
        store.failNextSegmentWriteForTesting();
    }

    static void setAfterCaptureFilesRetiredCallback(
        CaptureStore& store, std::function<void()> callback )
    {
        store.setAfterCaptureFilesRetiredCallbackForTesting(
            std::move( callback ) );
    }

    static bool contendForCapturePathAfterGate(
        CaptureStore& store, std::function<void()> gateAcquired )
    {
        return store.contendForCapturePathAfterGateForTesting(
            std::move( gateAcquired ) );
    }

    static bool holdCapturePathGate(
        CaptureStore& store, std::function<void()> gateAcquired,
        std::function<void()> waitForRelease )
    {
        return store.holdCapturePathGateForTesting(
            std::move( gateAcquired ), std::move( waitForRelease ) );
    }

    static int setCapturePathGateTimeout( int timeoutMs )
    {
        return CaptureStore::setCapturePathGateTimeoutForTesting(
            timeoutMs );
    }

    static bool spillFirstSegment( CaptureStore& store )
    {
        const std::lock_guard<std::recursive_mutex> lock( store.mutex_ );
        return !store.segments_.empty()
               && store.spillSegmentToDisk( store.segments_.front() );
    }

    static bool spillLastSegment( CaptureStore& store )
    {
        const std::lock_guard<std::recursive_mutex> lock( store.mutex_ );
        return !store.segments_.empty()
               && store.spillSegmentToDisk( store.segments_.back() );
    }

    static std::weak_ptr<void> capturePathStateLifetime( const CaptureStore& store )
    {
        return store.capturePathState_;
    }

    static std::shared_ptr<void> pinFirstSpilledSegment( const CaptureStore& store )
    {
        const std::lock_guard<std::recursive_mutex> lock( store.mutex_ );
        if ( store.segments_.empty() ) {
            return {};
        }
        return store.segments_.front().spilledFile;
    }

    static bool sharesCapturePathState( const CaptureStore& lhs,
                                        const CaptureStore& rhs )
    {
        return lhs.capturePathState_ == rhs.capturePathState_;
    }

    static QString capturePathIdentity( const CaptureStore& store )
    {
        return store.capturePathIdentity();
    }

    static void scheduleCleanupUnusedCaptures( const QSet<QString>& retainCaptureIds,
                                               const QString& rootPath,
                                               const QDateTime& preserveModifiedAfter )
    {
        CaptureStore::scheduleCleanupUnusedCaptures( retainCaptureIds, rootPath,
                                                     preserveModifiedAfter );
    }

    static QStringList collectUnusedCapturePaths( const QSet<QString>& retainCaptureIds,
                                                  const QString& rootPath )
    {
        return CaptureStore::collectUnusedCapturePaths( retainCaptureIds, rootPath );
    }

    static std::vector<CaptureStore::CleanupCandidate>
    collectUnusedCaptureCandidates( const QSet<QString>& retainCaptureIds,
                                    const QString& rootPath )
    {
        return CaptureStore::collectUnusedCaptureCandidates( retainCaptureIds,
                                                              rootPath );
    }

    static long capturePathStateUseCount(
        const CaptureStore::CleanupCandidate& candidate )
    {
        return candidate.capturePathState.use_count();
    }

    static void cleanupCapturePaths( const QStringList& capturePaths,
                                     const QDateTime& preserveModifiedAfter )
    {
        CaptureStore::cleanupCapturePaths( capturePaths, preserveModifiedAfter );
    }

    static void cleanupCaptureCandidates(
        const std::vector<CaptureStore::CleanupCandidate>& candidates,
        const QDateTime& preserveModifiedAfter,
        std::function<void( const QString& )> beforeRemoval = {} )
    {
        CaptureStore::cleanupCaptureCandidates( candidates, preserveModifiedAfter,
                                                beforeRemoval );
    }
};

namespace {
QString makeTestDir( const QString& prefix )
{
    const auto dirPath = QDir::cleanPath( QDir::currentPath() + QDir::separator()
                                          + QLatin1String( "test_tmp" ) + QDir::separator()
                                          + prefix + QLatin1Char( '_' )
                                          + QUuid::createUuid().toString( QUuid::WithoutBraces ) );
    QDir{}.mkpath( dirPath );
    return dirPath;
}

QString makeCaptureId()
{
    return QUuid::createUuid().toString( QUuid::WithoutBraces );
}

QStringList segmentFiles( const QString& capturePath )
{
    return QDir( capturePath ).entryList( QStringList{ "segment_*.log" }, QDir::Files,
                                          QDir::Name | QDir::IgnoreCase );
}

QString readUtf8File( const QString& filePath )
{
    QFile file( filePath );
    if ( !file.open( QIODevice::ReadOnly ) ) {
        return {};
    }
    return QString::fromUtf8( file.readAll() );
}

bool createSignalFile( const QString& filePath )
{
    QFile file( filePath );
    return file.open( QIODevice::WriteOnly | QIODevice::Truncate );
}

bool createDirectoryAlias( const QString& targetPath, const QString& aliasPath )
{
#if defined( Q_OS_WIN )
    return QProcess::execute(
               QStringLiteral( "cmd.exe" ),
               QStringList{ QStringLiteral( "/d" ), QStringLiteral( "/c" ),
                            QStringLiteral( "mklink" ), QStringLiteral( "/J" ),
                            QDir::toNativeSeparators( aliasPath ),
                            QDir::toNativeSeparators( targetPath ) } )
           == 0;
#else
    return QFile( targetPath ).link( aliasPath );
#endif
}

bool removeDirectoryAlias( const QString& aliasPath )
{
#if defined( Q_OS_WIN )
    const QFileInfo aliasInfo( aliasPath );
    return aliasInfo.dir().rmdir( aliasInfo.fileName() );
#else
    return QFile::remove( aliasPath );
#endif
}

bool waitForFile( const QString& filePath, int timeoutMs = 5000 )
{
    QElapsedTimer deadline;
    deadline.start();
    while ( !QFileInfo::exists( filePath ) && deadline.elapsed() < timeoutMs ) {
        std::this_thread::yield();
    }
    return QFileInfo::exists( filePath );
}

class ChildProcessReleaseGuard {
  public:
    ChildProcessReleaseGuard( QProcess& process, QString releasePath )
        : process_( process )
        , releasePath_( std::move( releasePath ) )
    {
    }

    ~ChildProcessReleaseGuard()
    {
        createSignalFile( releasePath_ );
        if ( process_.state() != QProcess::NotRunning && !process_.waitForFinished( 5000 ) ) {
            process_.kill();
            process_.waitForFinished( 5000 );
        }
    }

  private:
    QProcess& process_;
    QString releasePath_;
};

class ActiveCaptureChild {
  public:
    ActiveCaptureChild( QString rootPath, QString captureId, bool spillSegment )
        : readyPath_( QDir( rootPath ).filePath( QStringLiteral( "child-ready" ) ) )
        , releasePath_( QDir( rootPath ).filePath( QStringLiteral( "child-release" ) ) )
    {
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert( QStringLiteral( "KLOGG_TEST_PRESERVE_TEMP_DIR" ),
                            QStringLiteral( "1" ) );
        environment.insert( QStringLiteral( "KLOGG_CAPTURESTORE_CHILD_ROOT" ), rootPath );
        environment.insert( QStringLiteral( "KLOGG_CAPTURESTORE_CHILD_ID" ), captureId );
        environment.insert( QStringLiteral( "KLOGG_CAPTURESTORE_CHILD_READY" ), readyPath_ );
        environment.insert( QStringLiteral( "KLOGG_CAPTURESTORE_CHILD_RELEASE" ), releasePath_ );
        if ( spillSegment ) {
            environment.insert( QStringLiteral( "KLOGG_CAPTURESTORE_CHILD_SPILL" ),
                                QStringLiteral( "1" ) );
        }
        process_.setProcessEnvironment( environment );
        process_.setProgram( QCoreApplication::applicationFilePath() );
        process_.setArguments(
            { QStringLiteral( "-platform" ), QStringLiteral( "offscreen" ),
              QStringLiteral( "CaptureStore child holds active process marker" ) } );
    }

    ~ActiveCaptureChild()
    {
        createSignalFile( releasePath_ );
        if ( process_.state() != QProcess::NotRunning && !process_.waitForFinished( 5000 ) ) {
            process_.kill();
            process_.waitForFinished( 5000 );
        }
    }

    bool startAndWaitUntilReady()
    {
        process_.start();
        return process_.waitForStarted( 5000 ) && waitForFile( readyPath_ );
    }

    QString captureIdentity() const
    {
        return readUtf8File( readyPath_ );
    }

    bool releaseAndWait()
    {
        return createSignalFile( releasePath_ ) && process_.waitForFinished( 5000 )
               && process_.exitStatus() == QProcess::NormalExit && process_.exitCode() == 0;
    }

  private:
    QProcess process_;
    QString readyPath_;
    QString releasePath_;
};
} // namespace

TEST_CASE( "CaptureStore default spill limits prefer memory over temp files" )
{
    CaptureStore::Limits limits;

    REQUIRE( limits.segmentTargetBytes == 1024 * 1024 );
    REQUIRE( limits.memoryBudgetBytes == 256 * 1024 * 1024 );
}

TEST_CASE( "CaptureStore spills old segments only after memory budget is exceeded" )
{
    CaptureStore::Limits limits;
    limits.segmentTargetBytes = 8;
    limits.memoryBudgetBytes = 16;

    const auto rootPath = makeTestDir( "capturestore_spill" );
    CaptureStore store( makeCaptureId(), rootPath, limits );

    store.appendUtf8( QByteArrayLiteral( "aaa\nbbb\nccc\n" ) );
    REQUIRE( segmentFiles( store.capturePath() ).empty() );

    store.appendUtf8( QByteArrayLiteral( "ddd\neee\nfff\n" ) );
    REQUIRE_FALSE( segmentFiles( store.capturePath() ).empty() );
    REQUIRE( store.stats().memoryBytes <= limits.memoryBudgetBytes );
    REQUIRE( store.lineCount().get() == 6 );
    REQUIRE( store.lineAt( LineNumber( 0 ), QTextCodec::codecForName( "UTF-8" ),
                           QRegularExpression{} )
             == QStringLiteral( "aaa" ) );
    REQUIRE( store.lineAt( LineNumber( 5 ), QTextCodec::codecForName( "UTF-8" ),
                           QRegularExpression{} )
             == QStringLiteral( "fff" ) );
}

TEST_CASE( "CaptureStore persists buffered segments on destruction for session restore" )
{
    CaptureStore::Limits limits;
    limits.segmentTargetBytes = 64;
    limits.memoryBudgetBytes = 4096;

    const auto rootPath = makeTestDir( "capturestore_restore" );
    const auto captureId = makeCaptureId();
    const auto capturePath = QDir( rootPath ).filePath( captureId );

    {
        CaptureStore store( captureId, rootPath, limits );
        store.appendUtf8( QByteArrayLiteral( "one\ntwo\nthree\n" ) );
        REQUIRE( segmentFiles( capturePath ).empty() );
    }

    REQUIRE_FALSE( segmentFiles( capturePath ).empty() );

    CaptureStore restored( captureId, rootPath, limits );
    REQUIRE( restored.loadFromDisk() );
    REQUIRE( restored.lineCount().get() == 3 );
    REQUIRE( restored.lineAt( LineNumber( 2 ), QTextCodec::codecForName( "UTF-8" ),
                              QRegularExpression{} )
             == QStringLiteral( "three" ) );
}

TEST_CASE( "CaptureStore deleteCaptureFiles suppresses destructor persistence" )
{
    CaptureStore::Limits limits;
    limits.segmentTargetBytes = 64;
    limits.memoryBudgetBytes = 4096;

    const auto rootPath = makeTestDir( "capturestore_delete" );
    const auto captureId = makeCaptureId();
    const auto capturePath = QDir( rootPath ).filePath( captureId );

    {
        CaptureStore store( captureId, rootPath, limits );
        store.appendUtf8( QByteArrayLiteral( "alpha\nbeta\n" ) );
        store.deleteCaptureFiles();
        REQUIRE_FALSE( QFileInfo::exists( capturePath ) );
    }

    REQUIRE_FALSE( QFileInfo::exists( capturePath ) );
}

TEST_CASE( "CaptureStore rejects malformed capture ids before touching disk" )
{
    const auto rootPath = makeTestDir( "capturestore_invalid_id_root" );
    const auto externalPath = makeTestDir( "capturestore_invalid_id_external" );
    const auto sentinelPath = QDir( externalPath ).filePath( QStringLiteral( "segment_000000.log" ) );
    QFile sentinel( sentinelPath );
    REQUIRE( sentinel.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
    REQUIRE( sentinel.write( QByteArrayLiteral( "external\n" ) ) > 0 );
    sentinel.close();

    const QStringList malformedIds = {
        QString{}, QStringLiteral( "." ), QStringLiteral( ".." ),
        QDir( externalPath ).absolutePath(), QStringLiteral( "../external" ),
        QStringLiteral( "nested/capture" ), QStringLiteral( "nested\\capture" ),
        QStringLiteral( "capture:stream" ), QStringLiteral( "CON" ),
        QStringLiteral( "nul.txt" ), QStringLiteral( "COM1" ),
        QStringLiteral( "LPT9.log" ), QStringLiteral( "capture." ),
        QStringLiteral( "capture " ),
    };
    for ( const auto& captureId : malformedIds ) {
        INFO( "capture id: " << captureId.toStdString() );
        REQUIRE_THROWS_AS( CaptureStore( captureId, rootPath ), std::invalid_argument );
    }

    REQUIRE( QFileInfo::exists( sentinelPath ) );
    REQUIRE( readUtf8File( sentinelPath ) == QStringLiteral( "external\n" ) );
    REQUIRE( QDir( rootPath ).entryList( QDir::Dirs | QDir::NoDotAndDotDot ).isEmpty() );
}

TEST_CASE( "CaptureStore rejects a regular file at the capture path" )
{
    const auto rootPath = makeTestDir( "capturestore_regular_capture_path" );
    const auto captureId = makeCaptureId();
    const auto capturePath = QDir( rootPath ).filePath( captureId );
    QFile captureFile( capturePath );
    REQUIRE( captureFile.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
    REQUIRE( captureFile.write( QByteArrayLiteral( "external\n" ) ) > 0 );
    captureFile.close();

    REQUIRE_THROWS_AS( CaptureStore( captureId, rootPath ), std::runtime_error );
    REQUIRE( QFileInfo( capturePath ).isFile() );
    REQUIRE( readUtf8File( capturePath ) == QStringLiteral( "external\n" ) );
}

#if defined( Q_OS_WIN )
TEST_CASE( "CaptureStore supports extended-length Windows drive roots" )
{
    CaptureStore::Limits limits;
    limits.segmentTargetBytes = 64;
    limits.memoryBudgetBytes = 4096;

    const auto rootPath = makeTestDir( "capturestore_extended_drive_root" );
    const auto extendedRoot = QStringLiteral( "\\\\?\\" )
                              + QDir::toNativeSeparators( rootPath );
    CaptureStore store( makeCaptureId(), extendedRoot, limits );
    store.appendUtf8( QByteArrayLiteral( "extended-a\nextended-b\n" ) );

    REQUIRE( CaptureStoreTestAccess::spillFirstSegment( store ) );
    REQUIRE( segmentFiles( store.capturePath() ).size() == 1 );
}
#endif

TEST_CASE( "CaptureStore cleanup never follows capture symlinks" )
{
#if defined( Q_OS_WIN )
    // Creating a real directory symlink requires elevated privileges or
    // Developer Mode on Windows; QFile::link creates a shell shortcut instead.
    SUCCEED( "Directory symlink creation is not portable on Windows" );
#else
    const auto rootPath = makeTestDir( "capturestore_cleanup_symlink_root" );
    const auto retainedPath = QDir( rootPath ).filePath( QStringLiteral( "retained" ) );
    const auto retainedSentinel = QDir( retainedPath ).filePath( QStringLiteral( "sentinel.log" ) );
    const auto externalPath = makeTestDir( "capturestore_cleanup_symlink_external" );
    const auto externalSentinel = QDir( externalPath ).filePath( QStringLiteral( "sentinel.log" ) );
    const auto retainedAlias = QDir( rootPath ).filePath( QStringLiteral( "retained-alias" ) );
    const auto externalAlias = QDir( rootPath ).filePath( QStringLiteral( "external-alias" ) );

    REQUIRE( QDir{}.mkpath( retainedPath ) );
    REQUIRE( createSignalFile( retainedSentinel ) );
    REQUIRE( createSignalFile( externalSentinel ) );
    REQUIRE( QFile( retainedPath ).link( retainedAlias ) );
    REQUIRE( QFile( externalPath ).link( externalAlias ) );
    REQUIRE( QFileInfo( retainedAlias ).isSymLink() );
    REQUIRE( QFileInfo( externalAlias ).isSymLink() );

    CaptureStore::cleanupUnusedCaptures(
        QSet<QString>{ QStringLiteral( "retained" ) }, rootPath,
        QDateTime::currentDateTimeUtc().addSecs( 5 ) );

    REQUIRE( QFileInfo::exists( retainedSentinel ) );
    REQUIRE( QFileInfo::exists( externalSentinel ) );
    REQUIRE( QFileInfo::exists( retainedAlias ) );
    REQUIRE( QFileInfo::exists( externalAlias ) );
    REQUIRE_THROWS_AS( CaptureStore( QStringLiteral( "external-alias" ), rootPath ),
                       std::invalid_argument );
#endif
}

TEST_CASE( "CaptureStore cleanup removes nested trees without following child symlinks" )
{
    const auto rootPath = makeTestDir( "capturestore_cleanup_nested_root" );
    const auto captureId = makeCaptureId();
    const auto capturePath = QDir( rootPath ).filePath( captureId );
    const auto nestedPath
        = QDir( capturePath ).filePath( QStringLiteral( "nested/deep" ) );
    const auto nestedSentinel
        = QDir( nestedPath ).filePath( QStringLiteral( "capture.log" ) );
    const auto externalPath = makeTestDir( "capturestore_cleanup_nested_external" );
    const auto externalSentinel
        = QDir( externalPath ).filePath( QStringLiteral( "external.log" ) );
    const auto externalAlias
        = QDir( nestedPath ).filePath( QStringLiteral( "external-alias" ) );

    REQUIRE( QDir{}.mkpath( nestedPath ) );
    REQUIRE( createSignalFile( nestedSentinel ) );
    REQUIRE( createSignalFile( externalSentinel ) );
    REQUIRE( createDirectoryAlias( externalPath, externalAlias ) );
    REQUIRE( QFileInfo::exists( externalAlias ) );

    CaptureStore::cleanupUnusedCaptures(
        {}, rootPath, QDateTime::currentDateTimeUtc().addSecs( 5 ) );

    REQUIRE_FALSE( QFileInfo::exists( capturePath ) );
    REQUIRE( QFileInfo::exists( externalSentinel ) );
}

TEST_CASE( "CaptureStore cleanup restores a quarantine after a transient recursive failure" )
{
#if defined( Q_OS_WIN )
    SUCCEED( "POSIX directory permissions drive this regression" );
#else
    const auto rootPath = makeTestDir( "capturestore_cleanup_restore_quarantine" );
    const auto captureId = makeCaptureId();
    const auto capturePath = QDir( rootPath ).filePath( captureId );
    const auto blockedPath
        = QDir( capturePath ).filePath( QStringLiteral( "blocked" ) );
    REQUIRE( QDir{}.mkpath( blockedPath ) );
    QFile blockedFile(
        QDir( blockedPath ).filePath( QStringLiteral( "segment.log" ) ) );
    REQUIRE( blockedFile.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
    REQUIRE( blockedFile.write( QByteArrayLiteral( "blocked\n" ) ) > 0 );
    blockedFile.close();
    REQUIRE( QFile::setPermissions( blockedPath, {} ) );

    CaptureStore::cleanupUnusedCaptures(
        {}, rootPath, QDateTime::currentDateTimeUtc().addSecs( 5 ) );

    const auto quarantines = QDir( rootPath ).entryList(
        QStringList{ QStringLiteral( ".klogg-capture-delete-*" ) },
        QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot,
        QDir::NoSort );
    REQUIRE( QFileInfo::exists( capturePath ) );
    REQUIRE( quarantines.isEmpty() );

    REQUIRE( QFile::setPermissions(
        blockedPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                         | QFileDevice::ExeOwner ) );
    CaptureStore::cleanupUnusedCaptures(
        {}, rootPath, QDateTime::currentDateTimeUtc().addSecs( 5 ) );
    REQUIRE_FALSE( QFileInfo::exists( capturePath ) );
#endif
}

TEST_CASE( "CaptureStore cleanup retries stranded hidden quarantines" )
{
    const auto rootPath = makeTestDir( "capturestore_cleanup_hidden_quarantine" );
    const auto quarantinePath = QDir( rootPath ).filePath(
        QStringLiteral( ".klogg-capture-delete-%1" ).arg( makeCaptureId() ) );
    const auto nestedPath
        = QDir( quarantinePath ).filePath( QStringLiteral( "nested" ) );
    REQUIRE( QDir{}.mkpath( nestedPath ) );
    REQUIRE( createSignalFile(
        QDir( nestedPath ).filePath( QStringLiteral( "segment.log" ) ) ) );

    CaptureStore::cleanupUnusedCaptures(
        {}, rootPath, QDateTime::currentDateTimeUtc().addSecs( 5 ) );

    REQUIRE_FALSE( QFileInfo::exists( quarantinePath ) );
}

TEST_CASE( "CaptureStore cleanup does not follow a capture directory replaced after validation" )
{
    const auto rootPath = makeTestDir( "capturestore_cleanup_swap_root" );
    const auto captureId = makeCaptureId();
    const auto capturePath = QDir( rootPath ).filePath( captureId );
    const auto displacedName = captureId + QStringLiteral( ".displaced" );
    const auto displacedPath = QDir( rootPath ).filePath( displacedName );
    const auto captureSentinel
        = QDir( capturePath ).filePath( QStringLiteral( "capture-sentinel.log" ) );
    const auto externalPath = makeTestDir( "capturestore_cleanup_swap_external" );
    const auto externalSentinel
        = QDir( externalPath ).filePath( QStringLiteral( "external-sentinel.log" ) );

    REQUIRE( QDir{}.mkpath( capturePath ) );
    REQUIRE( createSignalFile( captureSentinel ) );
    REQUIRE( createSignalFile( externalSentinel ) );
    const auto candidates
        = CaptureStoreTestAccess::collectUnusedCaptureCandidates( {}, rootPath );
    REQUIRE( candidates.size() == 1 );

    bool swapped = false;
    CaptureStoreTestAccess::cleanupCaptureCandidates(
        candidates, QDateTime::currentDateTimeUtc().addSecs( 5 ),
        [ & ]( const QString& candidatePath ) {
            REQUIRE( candidatePath == capturePath );
            REQUIRE( QDir( rootPath ).rename( captureId, displacedName ) );
            REQUIRE( createDirectoryAlias( externalPath, capturePath ) );
            swapped = true;
        } );

    REQUIRE( swapped );
    REQUIRE( QFileInfo::exists( externalSentinel ) );
    REQUIRE( QFileInfo::exists( QDir( displacedPath ).filePath(
        QStringLiteral( "capture-sentinel.log" ) ) ) );
    REQUIRE( QFileInfo::exists( capturePath ) );
    REQUIRE( removeDirectoryAlias( capturePath ) );
    REQUIRE( QDir( rootPath ).rename( displacedName, captureId ) );
}

TEST_CASE( "CaptureStore spill fails closed when its capture directory is replaced" )
{
    CaptureStore::Limits limits;
    limits.segmentTargetBytes = 64;
    limits.memoryBudgetBytes = 4096;

    const auto rootPath = makeTestDir( "capturestore_spill_swap_root" );
    const auto captureId = makeCaptureId();
    CaptureStore store( captureId, rootPath, limits );
    store.appendUtf8( QByteArrayLiteral( "aaa\nbbb\n" ) );

    const auto capturePath = store.capturePath();
    const auto displacedName = captureId + QStringLiteral( ".displaced" );
    const auto externalPath = makeTestDir( "capturestore_spill_swap_external" );
    const auto externalSentinel
        = QDir( externalPath ).filePath( QStringLiteral( "sentinel.log" ) );
    QFile sentinel( externalSentinel );
    REQUIRE( sentinel.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
    REQUIRE( sentinel.write( QByteArrayLiteral( "external\n" ) ) > 0 );
    sentinel.close();
    REQUIRE( QDir( rootPath ).rename( captureId, displacedName ) );
    REQUIRE( createDirectoryAlias( externalPath, capturePath ) );

    const auto spillSucceeded = CaptureStoreTestAccess::spillFirstSegment( store );
    const auto externalCaptureFiles = QDir( externalPath ).entryList(
        QStringList{ QStringLiteral( "segment_*.log" ),
                     QStringLiteral( ".klogg-segment-*.tmp" ) },
        QDir::Files | QDir::Hidden, QDir::NoSort );
    const auto sentinelContents = readUtf8File( externalSentinel );
    for ( const auto& fileName : externalCaptureFiles ) {
        REQUIRE( QFile::remove( QDir( externalPath ).filePath( fileName ) ) );
    }
    REQUIRE( removeDirectoryAlias( capturePath ) );
    REQUIRE( QDir( rootPath ).rename( displacedName, captureId ) );

    REQUIRE_FALSE( spillSucceeded );
    REQUIRE( externalCaptureFiles.isEmpty() );
    REQUIRE( sentinelContents == QStringLiteral( "external\n" ) );
    REQUIRE( CaptureStoreTestAccess::spillFirstSegment( store ) );
    REQUIRE( segmentFiles( capturePath ).size() == 1 );
}

TEST_CASE( "CaptureStore spill stays anchored when the capture root is replaced" )
{
    CaptureStore::Limits limits;
    limits.segmentTargetBytes = 64;
    limits.memoryBudgetBytes = 4096;

    const auto containerPath = makeTestDir( "capturestore_root_swap_container" );
    const auto rootName = QStringLiteral( "capture-root" );
    const auto displacedName = QStringLiteral( "capture-root.displaced" );
    const auto rootPath = QDir( containerPath ).filePath( rootName );
    const auto displacedRootPath
        = QDir( containerPath ).filePath( displacedName );
    REQUIRE( QDir{}.mkpath( rootPath ) );
    const auto captureId = makeCaptureId();
    CaptureStore store( captureId, rootPath, limits );
    store.appendUtf8( QByteArrayLiteral( "anchored-a\nanchored-b\n" ) );

    const auto externalPath = makeTestDir( "capturestore_root_swap_external" );
    const auto externalSentinel
        = QDir( externalPath ).filePath( QStringLiteral( "sentinel.log" ) );
    REQUIRE( createSignalFile( externalSentinel ) );
    REQUIRE( QDir( containerPath ).rename( rootName, displacedName ) );
    REQUIRE( createDirectoryAlias( externalPath, rootPath ) );

    const auto spillSucceeded = CaptureStoreTestAccess::spillFirstSegment( store );
    const auto externalSegments = QDir( externalPath ).entryList(
        QStringList{ QStringLiteral( "segment_*.log" ),
                     QStringLiteral( ".klogg-segment-*.tmp" ),
                     QStringLiteral( "*.gate" ),
                     QStringLiteral( "*.generation" ),
                     QStringLiteral( "*.active.*" ) },
        QDir::Files | QDir::Hidden, QDir::NoSort );

    REQUIRE( spillSucceeded );
    REQUIRE( externalSegments.isEmpty() );
    REQUIRE( QFileInfo::exists( externalSentinel ) );
    REQUIRE( segmentFiles( QDir( displacedRootPath ).filePath( captureId ) ).size()
             == 1 );
    REQUIRE( removeDirectoryAlias( rootPath ) );
    REQUIRE( QDir( containerPath ).rename( displacedName, rootName ) );
}

TEST_CASE( "CaptureStore tombstones stay bound to the displaced directory generation" )
{
    CaptureStore::Limits limits;
    limits.segmentTargetBytes = 64;
    limits.memoryBudgetBytes = 4096;

    const auto rootPath = makeTestDir( "capturestore_tombstone_generation" );
    const auto captureId = makeCaptureId();
    const auto capturePath = QDir( rootPath ).filePath( captureId );
    const auto displacedName = captureId + QStringLiteral( ".displaced" );
    const auto displacedPath = QDir( rootPath ).filePath( displacedName );

    auto original = std::make_unique<CaptureStore>( captureId, rootPath, limits );
    original->appendUtf8( QByteArrayLiteral( "original-a\noriginal-b\n" ) );
    REQUIRE( CaptureStoreTestAccess::spillFirstSegment( *original ) );
    const auto originalFiles = segmentFiles( capturePath );
    REQUIRE( originalFiles.size() == 1 );
    auto pinnedSegment = CaptureStoreTestAccess::pinFirstSpilledSegment( *original );
    REQUIRE( pinnedSegment );
    original->deleteCaptureFiles();

    REQUIRE( QDir( rootPath ).rename( captureId, displacedName ) );
    REQUIRE( QDir{}.mkpath( capturePath ) );
    const auto replacementPath
        = QDir( capturePath ).filePath( originalFiles.front() );
    QFile replacementFile( replacementPath );
    REQUIRE( replacementFile.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
    REQUIRE( replacementFile.write( QByteArrayLiteral( "replacement\n" ) ) > 0 );
    replacementFile.close();

    CaptureStore replacement( captureId, rootPath, limits );
    REQUIRE_FALSE( CaptureStoreTestAccess::sharesCapturePathState(
        *original, replacement ) );
    REQUIRE( replacement.loadFromDisk() );

    original.reset();
    pinnedSegment.reset();

    REQUIRE( QFileInfo::exists( replacementPath ) );
    REQUIRE( readUtf8File( replacementPath ) == QStringLiteral( "replacement\n" ) );
    REQUIRE_FALSE( QFileInfo::exists(
        QDir( displacedPath ).filePath( originalFiles.front() ) ) );
}

TEST_CASE( "CaptureStore deferred deletion preserves a newer cross-process generation" )
{
    CaptureStore::Limits limits;
    limits.segmentTargetBytes = 64;
    limits.memoryBudgetBytes = 4096;

    const auto rootPath = makeTestDir( "capturestore_deferred_generation" );
    const auto captureId = makeCaptureId();
    const auto capturePath = QDir( rootPath ).filePath( captureId );
    const auto coordinationRoot = QDir( QDir::tempPath() ).filePath(
        QStringLiteral( "klogg_capture_coordination" ) );
    const auto activeMarkerFiles = [ &coordinationRoot ] {
        return QDir( coordinationRoot ).entryList(
            QStringList{ QStringLiteral( "*.active.*" ) },
            QDir::Files | QDir::Hidden, QDir::NoSort );
    };
    const auto baselineMarkers = activeMarkerFiles();

    CaptureStore original( captureId, rootPath, limits );
    auto originalMarkers = activeMarkerFiles();
    for ( const auto& marker : baselineMarkers ) {
        originalMarkers.removeAll( marker );
    }
    REQUIRE( originalMarkers.size() == 1 );
    const auto originalMarker = originalMarkers.front();
    const auto coordinationPrefix
        = originalMarker.section( QStringLiteral( ".active." ), 0, 0 );

    original.appendUtf8( QByteArrayLiteral( "original-a\noriginal-b\n" ) );
    REQUIRE( CaptureStoreTestAccess::spillFirstSegment( original ) );
    auto pinnedSegment = CaptureStoreTestAccess::pinFirstSpilledSegment( original );
    REQUIRE( pinnedSegment );
    original.deleteCaptureFiles();
    REQUIRE( QFileInfo::exists( capturePath ) );
    REQUIRE( QFileInfo::exists(
        QDir( coordinationRoot ).filePath( originalMarker ) ) );

    ActiveCaptureChild child( rootPath, captureId, false );
    REQUIRE( child.startAndWaitUntilReady() );
    const auto parentIdentity
        = CaptureStoreTestAccess::capturePathIdentity( original );
    const auto childIdentity = child.captureIdentity();
    INFO( "parent capture identity: " << parentIdentity.toStdString() );
    INFO( "child capture identity: " << childIdentity.toStdString() );
    REQUIRE( childIdentity == parentIdentity );
    const auto concurrentMarkers = QDir( coordinationRoot ).entryList(
        QStringList{ coordinationPrefix + QStringLiteral( ".active.*" ) },
        QDir::Files | QDir::Hidden, QDir::NoSort );
    REQUIRE( concurrentMarkers.size() == 2 );
    pinnedSegment.reset();

    REQUIRE( QFileInfo::exists( capturePath ) );
    REQUIRE( child.releaseAndWait() );
}

TEST_CASE( "CaptureStore deletion preserves an active same-process sibling" )
{
    CaptureStore::Limits limits;
    limits.segmentTargetBytes = 64;
    limits.memoryBudgetBytes = 4096;

    const auto rootPath = makeTestDir( "capturestore_same_process_sibling" );
    const auto captureId = makeCaptureId();
    const auto capturePath = QDir( rootPath ).filePath( captureId );

    CaptureStore activeSibling( captureId, rootPath, limits );
    const auto originalIdentity
        = CaptureStoreTestAccess::capturePathIdentity( activeSibling );
    CaptureStore deletingStore( captureId, rootPath, limits );

    deletingStore.deleteCaptureFiles();

    REQUIRE( QFileInfo::exists( capturePath ) );
    REQUIRE( CaptureStoreTestAccess::capturePathIdentity( activeSibling )
             == originalIdentity );
    activeSibling.appendUtf8( QByteArrayLiteral( "active-a\nactive-b\n" ) );
    REQUIRE( CaptureStoreTestAccess::spillFirstSegment( activeSibling ) );
    REQUIRE( QFileInfo::exists( capturePath ) );
}

TEST_CASE( "CaptureStore deletion preserves files loaded by a local sibling" )
{
    CaptureStore::Limits limits;
    limits.segmentTargetBytes = 64;
    limits.memoryBudgetBytes = 4096;

    const auto rootPath = makeTestDir( "capturestore_local_loaded_sibling" );
    const auto captureId = makeCaptureId();
    const auto capturePath = QDir( rootPath ).filePath( captureId );

    CaptureStore activeSibling( captureId, rootPath, limits );
    activeSibling.appendUtf8( QByteArrayLiteral( "active-a\nactive-b\n" ) );
    REQUIRE( CaptureStoreTestAccess::spillFirstSegment( activeSibling ) );
    const auto originalFiles = segmentFiles( capturePath );
    REQUIRE( originalFiles.size() == 1 );

    CaptureStore deletingStore( captureId, rootPath, limits );
    REQUIRE( deletingStore.loadFromDisk() );
    deletingStore.deleteCaptureFiles();

    REQUIRE( QFileInfo::exists(
        QDir( capturePath ).filePath( originalFiles.front() ) ) );
    REQUIRE( activeSibling.loadFromDisk() );
    REQUIRE( activeSibling.lineCount() == 2_lcount );
    REQUIRE( activeSibling.lineAt( 0_lnum,
                                   QTextCodec::codecForName( "UTF-8" ),
                                   QRegularExpression{} )
             == QStringLiteral( "active-a" ) );
}

TEST_CASE( "CaptureStore deferred file retirement completes after its local sibling exits" )
{
    CaptureStore::Limits limits;
    limits.segmentTargetBytes = 64;
    limits.memoryBudgetBytes = 4096;

    const auto rootPath = makeTestDir( "capturestore_local_file_sibling_exit" );
    const auto captureId = makeCaptureId();
    const auto capturePath = QDir( rootPath ).filePath( captureId );

    auto activeSibling
        = std::make_unique<CaptureStore>( captureId, rootPath, limits );
    activeSibling->appendUtf8( QByteArrayLiteral( "active-a\nactive-b\n" ) );
    REQUIRE( CaptureStoreTestAccess::spillFirstSegment( *activeSibling ) );

    CaptureStore deletingStore( captureId, rootPath, limits );
    REQUIRE( deletingStore.loadFromDisk() );
    deletingStore.deleteCaptureFiles();
    REQUIRE( QFileInfo::exists( capturePath ) );

    activeSibling.reset();

    REQUIRE_FALSE( QFileInfo::exists( capturePath ) );
}

TEST_CASE( "CaptureStore deferred local retirement follows the final pinned lease" )
{
    CaptureStore::Limits limits;
    limits.segmentTargetBytes = 64;
    limits.memoryBudgetBytes = 4096;

    const auto rootPath = makeTestDir( "capturestore_local_pinned_retirement" );
    const auto captureId = makeCaptureId();
    const auto capturePath = QDir( rootPath ).filePath( captureId );

    auto activeSibling
        = std::make_unique<CaptureStore>( captureId, rootPath, limits );
    activeSibling->appendUtf8( QByteArrayLiteral( "active-a\nactive-b\n" ) );
    REQUIRE( CaptureStoreTestAccess::spillFirstSegment( *activeSibling ) );
    auto pinnedSegment
        = CaptureStoreTestAccess::pinFirstSpilledSegment( *activeSibling );
    REQUIRE( pinnedSegment );

    CaptureStore deletingStore( captureId, rootPath, limits );
    REQUIRE( deletingStore.loadFromDisk() );
    deletingStore.deleteCaptureFiles();
    activeSibling.reset();
    REQUIRE( QFileInfo::exists( capturePath ) );

    pinnedSegment.reset();

    REQUIRE_FALSE( QFileInfo::exists( capturePath ) );
}

TEST_CASE( "CaptureStore deferred deletion completes after its local sibling exits" )
{
    const auto rootPath = makeTestDir( "capturestore_local_sibling_exit" );
    const auto captureId = makeCaptureId();
    const auto capturePath = QDir( rootPath ).filePath( captureId );

    auto activeSibling = std::make_unique<CaptureStore>( captureId, rootPath );
    CaptureStore deletingStore( captureId, rootPath );
    deletingStore.deleteCaptureFiles();
    REQUIRE( QFileInfo::exists( capturePath ) );

    activeSibling.reset();

    REQUIRE_FALSE( QFileInfo::exists( capturePath ) );
}

TEST_CASE( "CaptureStore deletion never adopts another process generation" )
{
    CaptureStore::Limits limits;
    limits.segmentTargetBytes = 64;
    limits.memoryBudgetBytes = 4096;

    const auto rootPath = makeTestDir( "capturestore_foreign_generation" );
    const auto captureId = makeCaptureId();
    const auto capturePath = QDir( rootPath ).filePath( captureId );

    CaptureStore original( captureId, rootPath, limits );
    original.appendUtf8( QByteArrayLiteral( "original-a\noriginal-b\n" ) );
    REQUIRE( CaptureStoreTestAccess::spillFirstSegment( original ) );
    auto pinnedSegment = CaptureStoreTestAccess::pinFirstSpilledSegment( original );
    REQUIRE( pinnedSegment );

    ActiveCaptureChild child( rootPath, captureId, false );
    REQUIRE( child.startAndWaitUntilReady() );
    REQUIRE( child.captureIdentity()
             == CaptureStoreTestAccess::capturePathIdentity( original ) );

    original.deleteCaptureFiles();
    pinnedSegment.reset();

    REQUIRE( QFileInfo::exists( capturePath ) );
    REQUIRE( child.releaseAndWait() );
}

TEST_CASE( "CaptureStore deletion refreshes a generation left by an exited process" )
{
    const auto rootPath = makeTestDir( "capturestore_exited_process_generation" );
    const auto captureId = makeCaptureId();
    const auto capturePath = QDir( rootPath ).filePath( captureId );

    CaptureStore survivingStore( captureId, rootPath );
    ActiveCaptureChild child( rootPath, captureId, false );
    REQUIRE( child.startAndWaitUntilReady() );
    REQUIRE( child.releaseAndWait() );

    survivingStore.deleteCaptureFiles();

    REQUIRE_FALSE( QFileInfo::exists( capturePath ) );
}

TEST_CASE( "CaptureStore cleanupUnusedCapturesAsync removes orphan captures off the startup path" )
{
    const auto rootPath = makeTestDir( "capturestore_async_cleanup" );
    const auto retainedCaptureId = makeCaptureId();
    const auto orphanCaptureId = makeCaptureId();
    const auto retainedPath = QDir( rootPath ).filePath( retainedCaptureId );
    const auto orphanPath = QDir( rootPath ).filePath( orphanCaptureId );

    REQUIRE( QDir{}.mkpath( retainedPath ) );
    REQUIRE( QDir{}.mkpath( orphanPath ) );

    QFile orphanSegment( QDir( orphanPath ).filePath( QStringLiteral( "segment_000000.log" ) ) );
    REQUIRE( orphanSegment.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
    orphanSegment.write( QByteArray( 1024 * 1024, 'x' ) );
    orphanSegment.close();

    // The async behavior is independent of timestamp ordering. Inject a future
    // cutoff so bind-mounted filesystem timestamp skew cannot turn this into a
    // preservation test; that contract is covered separately below.
    const auto cleanupCutoff = QDateTime::currentDateTimeUtc().addSecs( 5 );

    QElapsedTimer timer;
    timer.start();
    CaptureStore::cleanupUnusedCapturesAsync(
        QSet<QString>{ retainedCaptureId }, rootPath, cleanupCutoff );
    const auto elapsedMs = timer.elapsed();

    INFO( "cleanup scheduling elapsed ms: " << elapsedMs );
    CHECK( elapsedMs < 200 );
    REQUIRE( QDir{ retainedPath }.exists() );

    QElapsedTimer deadline;
    deadline.start();
    while ( QDir{ orphanPath }.exists() && deadline.elapsed() < 5000 ) {
        std::this_thread::sleep_for( std::chrono::milliseconds( 20 ) );
    }

    REQUIRE_FALSE( QDir{ orphanPath }.exists() );
    REQUIRE( QDir{ retainedPath }.exists() );
}

TEST_CASE( "CaptureStore cleanup snapshot excludes captures created after scheduling" )
{
    const auto rootPath = makeTestDir( "capturestore_cleanup_snapshot" );
    const auto orphanCaptureId = makeCaptureId();
    const auto orphanPath = QDir( rootPath ).filePath( orphanCaptureId );

    REQUIRE( QDir{}.mkpath( orphanPath ) );
    QFile orphanSegment( QDir( orphanPath ).filePath( QStringLiteral( "segment_000000.log" ) ) );
    REQUIRE( orphanSegment.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
    orphanSegment.write( QByteArrayLiteral( "orphan\n" ) );
    orphanSegment.close();

    const auto cleanupCandidates
        = CaptureStoreTestAccess::collectUnusedCapturePaths( {}, rootPath );

    const auto activeCaptureId = makeCaptureId();
    CaptureStore activeStore( activeCaptureId, rootPath );
    activeStore.appendUtf8( QByteArrayLiteral( "active\n" ) );

    CaptureStoreTestAccess::cleanupCapturePaths(
        cleanupCandidates, QDateTime::currentDateTimeUtc().addSecs( 5 ) );

    REQUIRE_FALSE( QDir{ orphanPath }.exists() );
    REQUIRE( QDir{ activeStore.capturePath() }.exists() );
    REQUIRE( activeStore.lineCount() == 1_lcount );
}

TEST_CASE( "CaptureStore cleanup snapshot preserves a capture activated with the same id" )
{
    const auto rootPath = makeTestDir( "capturestore_cleanup_same_id" );
    const auto captureId = makeCaptureId();
    const auto capturePath = QDir( rootPath ).filePath( captureId );

    REQUIRE( QDir{}.mkpath( capturePath ) );
    QFile orphanSegment(
        QDir( capturePath ).filePath( QStringLiteral( "segment_000000.log" ) ) );
    REQUIRE( orphanSegment.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
    REQUIRE( orphanSegment.write( QByteArrayLiteral( "orphan\n" ) ) > 0 );
    orphanSegment.close();

    const auto cleanupCandidates
        = CaptureStoreTestAccess::collectUnusedCaptureCandidates( {}, rootPath );

    CaptureStore activeStore( captureId, rootPath );
    activeStore.appendUtf8( QByteArrayLiteral( "active\n" ) );

    CaptureStoreTestAccess::cleanupCaptureCandidates(
        cleanupCandidates, QDateTime::currentDateTimeUtc().addSecs( 5 ) );

    REQUIRE( QFileInfo::exists( capturePath ) );
    REQUIRE( activeStore.lineCount() == 1_lcount );
    REQUIRE( activeStore.lineAt( 0_lnum, QTextCodec::codecForName( "UTF-8" ),
                                 QRegularExpression{} )
             == QStringLiteral( "active" ) );
}

TEST_CASE( "CaptureStore cleanupUnusedCaptures preserves captures modified after cutoff" )
{
    const auto rootPath = makeTestDir( "capturestore_cleanup_cutoff" );
    const auto orphanCaptureId = makeCaptureId();
    const auto activeCaptureId = makeCaptureId();
    const auto orphanPath = QDir( rootPath ).filePath( orphanCaptureId );
    const auto activePath = QDir( rootPath ).filePath( activeCaptureId );

    REQUIRE( QDir{}.mkpath( orphanPath ) );
    QFile orphanSegment( QDir( orphanPath ).filePath( QStringLiteral( "segment_000000.log" ) ) );
    REQUIRE( orphanSegment.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
    orphanSegment.write( QByteArrayLiteral( "old\n" ) );
    orphanSegment.close();

    // Keep the cutoff clear of filesystem timestamp rounding, then assign the
    // active segment an explicit post-cutoff modification time. Bind-mounted
    // filesystems used by sanitizer containers do not guarantee that a 20 ms
    // sleep produces distinct directory/file timestamps.
    const auto cleanupCutoff = QDateTime::currentDateTimeUtc().addSecs( 5 );

    REQUIRE( QDir{}.mkpath( activePath ) );
    QFile activeSegment( QDir( activePath ).filePath( QStringLiteral( "segment_000000.log" ) ) );
    REQUIRE( activeSegment.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
    activeSegment.write( QByteArrayLiteral( "new\n" ) );
    REQUIRE( activeSegment.flush() );
    REQUIRE( activeSegment.setFileTime( cleanupCutoff.addSecs( 5 ),
                                        QFileDevice::FileModificationTime ) );
    activeSegment.close();

    CaptureStore::cleanupUnusedCaptures( {}, rootPath, cleanupCutoff );

    REQUIRE_FALSE( QDir{ orphanPath }.exists() );
    REQUIRE( QDir{ activePath }.exists() );
}

TEST_CASE( "CaptureStore retries retired file deletion immediately after a transient failure" )
{
    CaptureStore::Limits limits;
    limits.segmentTargetBytes = 8;
    limits.memoryBudgetBytes = 16;

    const auto rootPath = makeTestDir( "capturestore_retry_retired_delete" );
    CaptureStore store( makeCaptureId(), rootPath, limits );
    store.appendUtf8( QByteArrayLiteral( "aaa\nbbb\nccc\nddd\neee\nfff\n" ) );
    REQUIRE_FALSE( segmentFiles( store.capturePath() ).isEmpty() );

    CaptureStoreTestAccess::failNextRetiredFileRemoval( store );
    store.clear();

    REQUIRE( segmentFiles( store.capturePath() ).isEmpty() );
}

TEST_CASE( "CaptureStore retries a transient capture directory removal failure" )
{
    const auto rootPath = makeTestDir( "capturestore_retry_rmdir" );
    CaptureStore store( makeCaptureId(), rootPath );
    const auto capturePath = store.capturePath();

    CaptureStoreTestAccess::failNextCaptureDirectoryRemoval( store );
    store.deleteCaptureFiles();
    REQUIRE( QFileInfo::exists( capturePath ) );

    store.clear();
    REQUIRE_FALSE( QFileInfo::exists( capturePath ) );
}

TEST_CASE( "CaptureStore releases retired leases after dropping the path mutex" )
{
    CaptureStore::Limits limits;
    limits.segmentTargetBytes = 64;
    limits.memoryBudgetBytes = 4096;

    const auto rootPath = makeTestDir( "capturestore_release_lock_order" );
    const auto captureId = makeCaptureId();

    CaptureStore owner( captureId, rootPath, limits );
    owner.appendUtf8( QByteArrayLiteral( "owner-a\nowner-b\n" ) );
    REQUIRE( CaptureStoreTestAccess::spillFirstSegment( owner ) );
    auto sibling
        = std::make_unique<CaptureStore>( captureId, rootPath, limits );

    std::thread siblingThread;
    std::atomic<bool> siblingHoldsGate{ false };
    bool siblingCompleted = false;
    CaptureStoreTestAccess::setAfterCaptureFilesRetiredCallback(
        owner, [ & ] {
            siblingThread = std::thread( [ & ] {
                siblingCompleted
                    = CaptureStoreTestAccess::contendForCapturePathAfterGate(
                        *sibling, [ & ] {
                            siblingHoldsGate.store(
                                true, std::memory_order_release );
                        } );
            } );
            QElapsedTimer waitForGate;
            waitForGate.start();
            while ( !siblingHoldsGate.load( std::memory_order_acquire )
                    && waitForGate.elapsed() < 5000 ) {
                std::this_thread::yield();
            }
        } );

    QElapsedTimer clearTimer;
    clearTimer.start();
    owner.clear();
    const auto clearElapsed = clearTimer.elapsed();
    if ( siblingThread.joinable() ) {
        siblingThread.join();
    }

    REQUIRE( siblingHoldsGate.load( std::memory_order_acquire ) );
    REQUIRE( siblingCompleted );
    REQUIRE( clearElapsed < 1000 );
    sibling.reset();
    REQUIRE( segmentFiles( owner.capturePath() ).isEmpty() );
}

TEST_CASE( "CaptureStore lifecycle transitions survive a gate timeout" )
{
    const auto rootPath = makeTestDir( "capturestore_gate_timeout" );
    const auto captureId = makeCaptureId();
    const auto capturePath = QDir( rootPath ).filePath( captureId );
    CaptureStore store( captureId, rootPath );

    const auto previousTimeout
        = CaptureStoreTestAccess::setCapturePathGateTimeout( 20 );
    std::atomic<bool> gateHeld{ false };
    std::atomic<bool> releaseGate{ false };
    std::atomic<bool> deletionFinished{ false };
    bool holderCompleted = false;
    std::thread holder( [ & ] {
        holderCompleted = CaptureStoreTestAccess::holdCapturePathGate(
            store,
            [ & ] {
                gateHeld.store( true, std::memory_order_release );
            },
            [ & ] {
                while ( !releaseGate.load( std::memory_order_acquire ) ) {
                    std::this_thread::yield();
                }
            } );
    } );

    QElapsedTimer waitForGate;
    waitForGate.start();
    while ( !gateHeld.load( std::memory_order_acquire )
            && waitForGate.elapsed() < 5000 ) {
        std::this_thread::yield();
    }

    std::thread deletion( [ & ] {
        store.deleteCaptureFiles();
        deletionFinished.store( true, std::memory_order_release );
    } );
    std::this_thread::sleep_for( std::chrono::milliseconds( 80 ) );
    const auto deletionFinishedBeforeRelease
        = deletionFinished.load( std::memory_order_acquire );
    releaseGate.store( true, std::memory_order_release );

    holder.join();
    deletion.join();
    store.clear();
    CaptureStoreTestAccess::setCapturePathGateTimeout( previousTimeout );

    REQUIRE( gateHeld.load( std::memory_order_acquire ) );
    REQUIRE( holderCompleted );
    REQUIRE( deletionFinishedBeforeRelease );
    REQUIRE( deletionFinished.load( std::memory_order_acquire ) );
    REQUIRE_FALSE( QFileInfo::exists( capturePath ) );
}

TEST_CASE( "CaptureStore deactivation survives a gate timeout" )
{
    const auto rootPath = makeTestDir( "capturestore_deactivate_timeout" );
    const auto captureId = makeCaptureId();
    const auto capturePath = QDir( rootPath ).filePath( captureId );
    auto exitingStore = std::make_unique<CaptureStore>( captureId, rootPath );
    CaptureStore survivingStore( captureId, rootPath );

    const auto previousTimeout
        = CaptureStoreTestAccess::setCapturePathGateTimeout( 20 );
    std::atomic<bool> gateHeld{ false };
    std::atomic<bool> releaseGate{ false };
    std::atomic<bool> destructionFinished{ false };
    bool holderCompleted = false;
    std::thread holder( [ & ] {
        holderCompleted = CaptureStoreTestAccess::holdCapturePathGate(
            survivingStore,
            [ & ] {
                gateHeld.store( true, std::memory_order_release );
            },
            [ & ] {
                while ( !releaseGate.load( std::memory_order_acquire ) ) {
                    std::this_thread::yield();
                }
            } );
    } );

    QElapsedTimer waitForGate;
    waitForGate.start();
    while ( !gateHeld.load( std::memory_order_acquire )
            && waitForGate.elapsed() < 5000 ) {
        std::this_thread::yield();
    }

    std::thread destruction( [ & ] {
        exitingStore.reset();
        destructionFinished.store( true, std::memory_order_release );
    } );
    std::this_thread::sleep_for( std::chrono::milliseconds( 80 ) );
    const auto destructionFinishedBeforeRelease
        = destructionFinished.load( std::memory_order_acquire );
    releaseGate.store( true, std::memory_order_release );

    holder.join();
    destruction.join();
    survivingStore.deleteCaptureFiles();
    CaptureStoreTestAccess::setCapturePathGateTimeout( previousTimeout );

    REQUIRE( gateHeld.load( std::memory_order_acquire ) );
    REQUIRE( holderCompleted );
    REQUIRE( destructionFinishedBeforeRelease );
    REQUIRE( destructionFinished.load( std::memory_order_acquire ) );
    REQUIRE_FALSE( QFileInfo::exists( capturePath ) );
}

TEST_CASE( "CaptureStore child holds active process marker" )
{
    const auto rootPath = qEnvironmentVariable( "KLOGG_CAPTURESTORE_CHILD_ROOT" );
    if ( rootPath.isEmpty() ) {
        SUCCEED();
        return;
    }

    const auto captureId = qEnvironmentVariable( "KLOGG_CAPTURESTORE_CHILD_ID" );
    const auto readyPath = qEnvironmentVariable( "KLOGG_CAPTURESTORE_CHILD_READY" );
    const auto releasePath = qEnvironmentVariable( "KLOGG_CAPTURESTORE_CHILD_RELEASE" );
    CaptureStore store( captureId, rootPath );
    const auto spillsSegment
        = qEnvironmentVariableIsSet( "KLOGG_CAPTURESTORE_CHILD_SPILL" );
    if ( spillsSegment ) {
        store.appendUtf8( QByteArrayLiteral( "external-a\nexternal-b\n" ) );
        REQUIRE( CaptureStoreTestAccess::spillFirstSegment( store ) );
    } else if ( qEnvironmentVariableIsSet( "KLOGG_CAPTURESTORE_CHILD_WRITE" ) ) {
        store.appendUtf8( QByteArrayLiteral( "replacement\n" ) );
    }
    QSaveFile readyFile( readyPath );
    readyFile.setDirectWriteFallback( false );
    REQUIRE( readyFile.open( QIODevice::WriteOnly ) );
    const auto captureIdentity
        = CaptureStoreTestAccess::capturePathIdentity( store ).toUtf8();
    REQUIRE( readyFile.write( captureIdentity ) == captureIdentity.size() );
    REQUIRE( readyFile.commit() );
    REQUIRE( waitForFile( releasePath ) );
    if ( spillsSegment ) {
        REQUIRE( store.lineAt( 0_lnum, QTextCodec::codecForName( "UTF-8" ),
                               QRegularExpression{} )
                 == QStringLiteral( "external-a" ) );
    }
}

TEST_CASE( "CaptureStore maintenance retires only locally owned files while an external process is active" )
{
    CaptureStore::Limits limits;
    limits.segmentTargetBytes = 64;
    limits.memoryBudgetBytes = 4096;

    const auto rootPath = makeTestDir( "capturestore_external_owner_maintenance" );
    const auto captureId = makeCaptureId();
    const auto capturePath = QDir( rootPath ).filePath( captureId );
    ActiveCaptureChild child( rootPath, captureId, true );
    REQUIRE( child.startAndWaitUntilReady() );

    const auto externalFiles = segmentFiles( capturePath );
    REQUIRE( externalFiles.size() == 1 );
    const auto externalPath = QDir( capturePath ).filePath( externalFiles.front() );

    CaptureStore observer( captureId, rootPath, limits );
    REQUIRE( observer.loadFromDisk() );
    REQUIRE( observer.lineAt( 0_lnum, QTextCodec::codecForName( "UTF-8" ),
                              QRegularExpression{} )
             == QStringLiteral( "external-a" ) );
    observer.appendUtf8( QByteArrayLiteral( "parent-a\nparent-b\n" ) );
    REQUIRE( CaptureStoreTestAccess::spillLastSegment( observer ) );

    const auto allFiles = segmentFiles( capturePath );
    REQUIRE( allFiles.size() == 2 );
    auto parentFiles = allFiles;
    parentFiles.removeAll( externalFiles.front() );
    REQUIRE( parentFiles.size() == 1 );
    const auto parentPath = QDir( capturePath ).filePath( parentFiles.front() );

    SECTION( "clear" )
    {
        observer.clear();
    }
    SECTION( "delete capture files" )
    {
        observer.deleteCaptureFiles();
    }

    REQUIRE( QFileInfo::exists( externalPath ) );
    REQUIRE_FALSE( QFileInfo::exists( parentPath ) );
    REQUIRE( QFileInfo::exists( capturePath ) );
    REQUIRE( child.releaseAndWait() );
}

TEST_CASE( "CaptureStore case aliases share physical capture coordination" )
{
    const auto rootPath = makeTestDir( "capturestore_case_alias" );
    const auto primaryId
        = QStringLiteral( "CaptureAlias-%1" ).arg( makeCaptureId() );
    const auto aliasId = primaryId.toLower();
    const auto primaryPath = QDir( rootPath ).filePath( primaryId );
    const auto aliasPath = QDir( rootPath ).filePath( aliasId );
    REQUIRE( QDir{}.mkpath( primaryPath ) );
    if ( !QFileInfo( aliasPath ).isDir() ) {
        SUCCEED( "Filesystem is case-sensitive" );
        return;
    }

    ActiveCaptureChild child( rootPath, primaryId, true );
    REQUIRE( child.startAndWaitUntilReady() );
    const auto externalFiles = segmentFiles( aliasPath );
    REQUIRE( externalFiles.size() == 1 );
    const auto externalPath = QDir( aliasPath ).filePath( externalFiles.front() );

    CaptureStore observer( aliasId, rootPath );
    REQUIRE( observer.loadFromDisk() );
    observer.deleteCaptureFiles();

    REQUIRE( QFileInfo::exists( externalPath ) );
    REQUIRE( child.releaseAndWait() );
}

TEST_CASE( "CaptureStore root aliases share physical capture coordination" )
{
    const auto rootPath = makeTestDir( "capturestore_root_alias" );
    const QFileInfo rootInfo( rootPath );
    const auto aliasRootPath = rootInfo.dir().filePath(
        rootInfo.fileName().toUpper() );
    if ( aliasRootPath == rootPath || !QFileInfo( aliasRootPath ).isDir() ) {
        SUCCEED( "Filesystem is case-sensitive" );
        return;
    }

    const auto captureId = makeCaptureId();
    ActiveCaptureChild child( rootPath, captureId, true );
    REQUIRE( child.startAndWaitUntilReady() );
    const auto aliasCapturePath = QDir( aliasRootPath ).filePath( captureId );
    const auto externalFiles = segmentFiles( aliasCapturePath );
    REQUIRE( externalFiles.size() == 1 );
    const auto externalPath
        = QDir( aliasCapturePath ).filePath( externalFiles.front() );

    CaptureStore observer( captureId, aliasRootPath );
    REQUIRE( observer.loadFromDisk() );
    observer.deleteCaptureFiles();

    REQUIRE( QFileInfo::exists( externalPath ) );
    REQUIRE( child.releaseAndWait() );
}

TEST_CASE( "CaptureStore normalization aliases share physical capture coordination" )
{
    const auto rootPath = makeTestDir( "capturestore_normalization_alias" );
    const auto suffix = QStringLiteral( "-%1" ).arg( makeCaptureId() );
    const auto primaryId
        = ( QStringLiteral( "Café" ) + suffix ).normalized( QString::NormalizationForm_C );
    const auto aliasId = primaryId.normalized( QString::NormalizationForm_D );
    REQUIRE( primaryId != aliasId );
    const auto primaryPath = QDir( rootPath ).filePath( primaryId );
    const auto aliasPath = QDir( rootPath ).filePath( aliasId );
    REQUIRE( QDir{}.mkpath( primaryPath ) );
    if ( !QFileInfo( aliasPath ).isDir() ) {
        SUCCEED( "Filesystem distinguishes Unicode normalization forms" );
        return;
    }

    ActiveCaptureChild child( rootPath, primaryId, true );
    REQUIRE( child.startAndWaitUntilReady() );
    const auto externalFiles = segmentFiles( aliasPath );
    REQUIRE( externalFiles.size() == 1 );
    const auto externalPath = QDir( aliasPath ).filePath( externalFiles.front() );

    CaptureStore observer( aliasId, rootPath );
    REQUIRE( observer.loadFromDisk() );
    observer.deleteCaptureFiles();

    REQUIRE( QFileInfo::exists( externalPath ) );
    REQUIRE( child.releaseAndWait() );
}

TEST_CASE( "CaptureStore cleanup retention matches physical capture aliases" )
{
    const auto rootPath = makeTestDir( "capturestore_retained_alias" );

    SECTION( "case alias" )
    {
        const auto primaryId
            = QStringLiteral( "RetainedAlias-%1" ).arg( makeCaptureId() );
        const auto aliasId = primaryId.toLower();
        const auto primaryPath = QDir( rootPath ).filePath( primaryId );
        REQUIRE( QDir{}.mkpath( primaryPath ) );
        if ( !QFileInfo( QDir( rootPath ).filePath( aliasId ) ).isDir() ) {
            SUCCEED( "Filesystem is case-sensitive" );
            return;
        }
        REQUIRE( createSignalFile(
            QDir( primaryPath ).filePath( QStringLiteral( "sentinel.log" ) ) ) );

        CaptureStore::cleanupUnusedCaptures(
            QSet<QString>{ aliasId }, rootPath,
            QDateTime::currentDateTimeUtc().addSecs( 5 ) );

        REQUIRE( QFileInfo::exists( primaryPath ) );
    }

    SECTION( "normalization alias" )
    {
        const auto suffix = QStringLiteral( "-%1" ).arg( makeCaptureId() );
        const auto primaryId
            = ( QStringLiteral( "Retainé" ) + suffix )
                  .normalized( QString::NormalizationForm_C );
        const auto aliasId = primaryId.normalized( QString::NormalizationForm_D );
        const auto primaryPath = QDir( rootPath ).filePath( primaryId );
        REQUIRE( QDir{}.mkpath( primaryPath ) );
        if ( !QFileInfo( QDir( rootPath ).filePath( aliasId ) ).isDir() ) {
            SUCCEED( "Filesystem distinguishes Unicode normalization forms" );
            return;
        }
        REQUIRE( createSignalFile(
            QDir( primaryPath ).filePath( QStringLiteral( "sentinel.log" ) ) ) );

        CaptureStore::cleanupUnusedCaptures(
            QSet<QString>{ aliasId }, rootPath,
            QDateTime::currentDateTimeUtc().addSecs( 5 ) );

        REQUIRE( QFileInfo::exists( primaryPath ) );
    }
}

TEST_CASE( "CaptureStore process ownership survives destruction of the creating store" )
{
    CaptureStore::Limits limits;
    limits.segmentTargetBytes = 64;
    limits.memoryBudgetBytes = 4096;

    const auto rootPath = makeTestDir( "capturestore_process_owned_successor" );
    const auto captureId = makeCaptureId();
    const auto capturePath = QDir( rootPath ).filePath( captureId );

    auto creator = std::make_unique<CaptureStore>( captureId, rootPath, limits );
    creator->appendUtf8( QByteArrayLiteral( "owned-a\nowned-b\n" ) );
    REQUIRE( CaptureStoreTestAccess::spillFirstSegment( *creator ) );
    const auto ownedFiles = segmentFiles( capturePath );
    REQUIRE( ownedFiles.size() == 1 );
    const auto ownedPath = QDir( capturePath ).filePath( ownedFiles.front() );

    CaptureStore successor( captureId, rootPath, limits );
    creator.reset();
    REQUIRE( successor.loadFromDisk() );

    successor.clear();
    REQUIRE_FALSE( QFileInfo::exists( ownedPath ) );
    REQUIRE( QFileInfo::exists( capturePath ) );

    successor.deleteCaptureFiles();
    REQUIRE_FALSE( QFileInfo::exists( capturePath ) );
}

TEST_CASE( "CaptureStore process ownership survives an inactive local-store gap" )
{
    CaptureStore::Limits limits;
    limits.segmentTargetBytes = 64;
    limits.memoryBudgetBytes = 4096;

    const auto rootPath = makeTestDir( "capturestore_process_owned_inactive_gap" );
    const auto captureId = makeCaptureId();
    const auto capturePath = QDir( rootPath ).filePath( captureId );
    ActiveCaptureChild child( rootPath, captureId, true );
    REQUIRE( child.startAndWaitUntilReady() );

    const auto externalFiles = segmentFiles( capturePath );
    REQUIRE( externalFiles.size() == 1 );
    const auto externalPath = QDir( capturePath ).filePath( externalFiles.front() );

    QString locallyOwnedPath;
    {
        CaptureStore creator( captureId, rootPath, limits );
        REQUIRE( creator.loadFromDisk() );
        creator.appendUtf8( QByteArrayLiteral( "local-a\nlocal-b\n" ) );
        REQUIRE( CaptureStoreTestAccess::spillLastSegment( creator ) );
        auto localFiles = segmentFiles( capturePath );
        localFiles.removeAll( externalFiles.front() );
        REQUIRE( localFiles.size() == 1 );
        locallyOwnedPath = QDir( capturePath ).filePath( localFiles.front() );
    }

    REQUIRE( QFileInfo::exists( externalPath ) );
    REQUIRE( QFileInfo::exists( locallyOwnedPath ) );

    CaptureStore successor( captureId, rootPath, limits );
    REQUIRE( successor.loadFromDisk() );
    successor.deleteCaptureFiles();

    REQUIRE( QFileInfo::exists( externalPath ) );
    REQUIRE_FALSE( QFileInfo::exists( locallyOwnedPath ) );
    REQUIRE( child.releaseAndWait() );
}

TEST_CASE( "CaptureStore cleanup retires ownership with the deleted directory generation" )
{
    CaptureStore::Limits limits;
    limits.segmentTargetBytes = 64;
    limits.memoryBudgetBytes = 4096;

    const auto rootPath = makeTestDir( "capturestore_cleanup_generation_ownership" );
    const auto captureId = makeCaptureId();
    const auto capturePath = QDir( rootPath ).filePath( captureId );

    {
        CaptureStore original( captureId, rootPath, limits );
        original.appendUtf8( QByteArrayLiteral( "original-a\noriginal-b\n" ) );
        REQUIRE( CaptureStoreTestAccess::spillFirstSegment( original ) );
        REQUIRE( segmentFiles( capturePath )
                     == QStringList{ QStringLiteral( "segment_000000.log" ) } );
    }

    CaptureStore::cleanupUnusedCaptures(
        {}, rootPath, QDateTime::currentDateTimeUtc().addSecs( 5 ) );
    REQUIRE_FALSE( QFileInfo::exists( capturePath ) );

    ActiveCaptureChild child( rootPath, captureId, true );
    REQUIRE( child.startAndWaitUntilReady() );
    REQUIRE( segmentFiles( capturePath )
                 == QStringList{ QStringLiteral( "segment_000000.log" ) } );
    const auto externalPath
        = QDir( capturePath ).filePath( QStringLiteral( "segment_000000.log" ) );

    CaptureStore observer( captureId, rootPath, limits );
    REQUIRE( observer.loadFromDisk() );
    observer.deleteCaptureFiles();

    REQUIRE( QFileInfo::exists( externalPath ) );
    REQUIRE( child.releaseAndWait() );
}

TEST_CASE( "CaptureStore does not acquire loaded-file ownership when the external owner exits" )
{
    const auto rootPath = makeTestDir( "capturestore_external_owner_exit" );
    const auto captureId = makeCaptureId();
    const auto capturePath = QDir( rootPath ).filePath( captureId );
    ActiveCaptureChild child( rootPath, captureId, true );
    REQUIRE( child.startAndWaitUntilReady() );

    const auto externalFiles = segmentFiles( capturePath );
    REQUIRE( externalFiles.size() == 1 );
    const auto externalPath = QDir( capturePath ).filePath( externalFiles.front() );

    auto observer = std::make_unique<CaptureStore>( captureId, rootPath );
    REQUIRE( observer->loadFromDisk() );
    REQUIRE( child.releaseAndWait() );

    SECTION( "clear" )
    {
        observer->clear();
    }
    SECTION( "delete capture files" )
    {
        observer->deleteCaptureFiles();
    }

    REQUIRE( QFileInfo::exists( externalPath ) );
    observer.reset();
    REQUIRE( QFileInfo::exists( externalPath ) );

    CaptureStore adoptingStore( captureId, rootPath );
    REQUIRE( adoptingStore.loadFromDisk() );
    adoptingStore.deleteCaptureFiles();
    REQUIRE_FALSE( QFileInfo::exists( externalPath ) );
    REQUIRE_FALSE( QFileInfo::exists( capturePath ) );
}

TEST_CASE( "CaptureStore cleanup skips a capture active in another process" )
{
    const auto rootPath = makeTestDir( "capturestore_cross_process_cleanup" );
    const auto captureId = makeCaptureId();
    const auto capturePath = QDir( rootPath ).filePath( captureId );
    const auto readyPath = QDir( rootPath ).filePath( QStringLiteral( "child-ready" ) );
    const auto releasePath = QDir( rootPath ).filePath( QStringLiteral( "child-release" ) );

    QProcess child;
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert( QStringLiteral( "KLOGG_TEST_PRESERVE_TEMP_DIR" ),
                        QStringLiteral( "1" ) );
    environment.insert( QStringLiteral( "KLOGG_CAPTURESTORE_CHILD_ROOT" ), rootPath );
    environment.insert( QStringLiteral( "KLOGG_CAPTURESTORE_CHILD_ID" ), captureId );
    environment.insert( QStringLiteral( "KLOGG_CAPTURESTORE_CHILD_READY" ), readyPath );
    environment.insert( QStringLiteral( "KLOGG_CAPTURESTORE_CHILD_RELEASE" ), releasePath );
    child.setProcessEnvironment( environment );
    child.setProgram( QCoreApplication::applicationFilePath() );
    child.setArguments( { QStringLiteral( "-platform" ), QStringLiteral( "offscreen" ),
                          QStringLiteral( "CaptureStore child holds active process marker" ) } );
    child.start();
    REQUIRE( child.waitForStarted( 5000 ) );

    bool childExited = false;
    {
        ChildProcessReleaseGuard releaseChild( child, releasePath );
        REQUIRE( waitForFile( readyPath ) );
        CaptureStore::cleanupUnusedCaptures(
            {}, rootPath, QDateTime::currentDateTimeUtc().addSecs( 5 ) );
        REQUIRE( QFileInfo::exists( capturePath ) );

        REQUIRE( createSignalFile( releasePath ) );
        childExited = child.waitForFinished( 5000 ) && child.exitStatus() == QProcess::NormalExit
                      && child.exitCode() == 0;
    }

    REQUIRE( childExited );
    CaptureStore::cleanupUnusedCaptures(
        {}, rootPath, QDateTime::currentDateTimeUtc().addSecs( 5 ) );
    REQUIRE_FALSE( QFileInfo::exists( capturePath ) );
}

TEST_CASE( "CaptureStore cleanup snapshot rejects a completed cross-process replacement" )
{
    const auto rootPath = makeTestDir( "capturestore_cross_process_generation" );
    const auto captureId = makeCaptureId();
    const auto capturePath = QDir( rootPath ).filePath( captureId );
    const auto readyPath = QDir( rootPath ).filePath( QStringLiteral( "child-ready" ) );
    const auto releasePath = QDir( rootPath ).filePath( QStringLiteral( "child-release" ) );

    REQUIRE( QDir{}.mkpath( capturePath ) );
    QFile oldSegment(
        QDir( capturePath ).filePath( QStringLiteral( "segment_000000.log" ) ) );
    REQUIRE( oldSegment.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
    REQUIRE( oldSegment.write( QByteArrayLiteral( "orphan\n" ) ) > 0 );
    oldSegment.close();

    const auto cleanupCandidates
        = CaptureStoreTestAccess::collectUnusedCaptureCandidates( {}, rootPath );

    QProcess child;
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert( QStringLiteral( "KLOGG_TEST_PRESERVE_TEMP_DIR" ),
                        QStringLiteral( "1" ) );
    environment.insert( QStringLiteral( "KLOGG_CAPTURESTORE_CHILD_ROOT" ), rootPath );
    environment.insert( QStringLiteral( "KLOGG_CAPTURESTORE_CHILD_ID" ), captureId );
    environment.insert( QStringLiteral( "KLOGG_CAPTURESTORE_CHILD_READY" ), readyPath );
    environment.insert( QStringLiteral( "KLOGG_CAPTURESTORE_CHILD_RELEASE" ), releasePath );
    environment.insert( QStringLiteral( "KLOGG_CAPTURESTORE_CHILD_WRITE" ),
                        QStringLiteral( "1" ) );
    child.setProcessEnvironment( environment );
    child.setProgram( QCoreApplication::applicationFilePath() );
    child.setArguments( { QStringLiteral( "-platform" ), QStringLiteral( "offscreen" ),
                          QStringLiteral( "CaptureStore child holds active process marker" ) } );
    child.start();
    REQUIRE( child.waitForStarted( 5000 ) );

    bool childExited = false;
    {
        ChildProcessReleaseGuard releaseChild( child, releasePath );
        REQUIRE( waitForFile( readyPath ) );
        REQUIRE( createSignalFile( releasePath ) );
        childExited = child.waitForFinished( 5000 )
                      && child.exitStatus() == QProcess::NormalExit
                      && child.exitCode() == 0;
    }
    REQUIRE( childExited );

    CaptureStoreTestAccess::cleanupCaptureCandidates(
        cleanupCandidates, QDateTime::currentDateTimeUtc().addSecs( 5 ) );

    REQUIRE( QFileInfo::exists( capturePath ) );
    CaptureStore restored( captureId, rootPath );
    REQUIRE( restored.loadFromDisk() );
    REQUIRE( restored.lineCount() >= 1_lcount );
    REQUIRE( restored.lineAt( LineNumber( restored.lineCount().get() - 1 ),
                              QTextCodec::codecForName( "UTF-8" ),
                              QRegularExpression{} )
             == QStringLiteral( "replacement" ) );
}

TEST_CASE( "CaptureStore retries activation after cleanup removes the acquired state" )
{
    const auto rootPath = makeTestDir( "capturestore_activation_cleanup_race" );
    const auto captureId = makeCaptureId();
    const auto capturePath = QDir( rootPath ).filePath( captureId );
    REQUIRE( QDir{}.mkpath( capturePath ) );

    const auto cleanupCandidates
        = CaptureStoreTestAccess::collectUnusedCaptureCandidates( {}, rootPath );
    REQUIRE( cleanupCandidates.size() == 1 );
    const auto baselineUseCount
        = CaptureStoreTestAccess::capturePathStateUseCount(
            cleanupCandidates.front() );

    std::unique_ptr<CaptureStore> activatedStore;
    std::exception_ptr constructionError;
    std::thread constructorThread;
    bool acquiredBeforeRemoval = false;
    CaptureStoreTestAccess::cleanupCaptureCandidates(
        cleanupCandidates, QDateTime::currentDateTimeUtc().addSecs( 5 ),
        [ & ]( const QString& ) {
            constructorThread = std::thread( [ & ] {
                try {
                    activatedStore
                        = std::make_unique<CaptureStore>( captureId, rootPath );
                } catch ( ... ) {
                    constructionError = std::current_exception();
                }
            } );

            QElapsedTimer timer;
            timer.start();
            while ( CaptureStoreTestAccess::capturePathStateUseCount(
                        cleanupCandidates.front() )
                        <= baselineUseCount
                    && timer.elapsed() < 5000 ) {
                std::this_thread::yield();
            }
            acquiredBeforeRemoval
                = CaptureStoreTestAccess::capturePathStateUseCount(
                      cleanupCandidates.front() )
                  > baselineUseCount;
        } );

    if ( constructorThread.joinable() ) {
        constructorThread.join();
    }

    REQUIRE( acquiredBeforeRemoval );
    REQUIRE_FALSE( constructionError );
    REQUIRE( activatedStore );
    activatedStore->appendUtf8( QByteArrayLiteral( "replacement\n" ) );
    REQUIRE( CaptureStoreTestAccess::spillFirstSegment( *activatedStore ) );
    REQUIRE( QFileInfo::exists( capturePath ) );
}

TEST_CASE( "CaptureStore bindOutputFile overwrites existing files and replays spilled segments" )
{
    CaptureStore::Limits limits;
    limits.segmentTargetBytes = 8;
    limits.memoryBudgetBytes = 8;

    const auto rootPath = makeTestDir( "capturestore_bind_output" );
    const auto outputPath = QDir( rootPath ).filePath( QStringLiteral( "saved.log" ) );

    QFile staleOutput( outputPath );
    REQUIRE( staleOutput.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
    REQUIRE( staleOutput.write( QByteArrayLiteral( "stale-data\n" ) ) > 0 );
    staleOutput.close();

    CaptureStore store( makeCaptureId(), rootPath, limits );
    store.appendUtf8( QByteArrayLiteral( "alpha\nbeta\ngamma\ndelta\n" ) );
    REQUIRE_FALSE( segmentFiles( store.capturePath() ).empty() );

    REQUIRE( store.bindOutputFile( outputPath ) );
    store.appendUtf8( QByteArrayLiteral( "epsilon\n" ) );
    REQUIRE( store.bindOutputFile( QString{} ) );

    REQUIRE( QFileInfo::exists( outputPath ) );
    REQUIRE( readUtf8File( outputPath )
             == QStringLiteral( "alpha\nbeta\ngamma\ndelta\nepsilon\n" ) );
}

TEST_CASE( "RollingFileManager resyncSize reads actual file size after direct writes" )
{
    const auto rootPath = makeTestDir( "rolling_resync" );
    const auto filePath = QDir( rootPath ).filePath( QStringLiteral( "output.log" ) );

    RollingFileManager manager( filePath, 64, 2 );
    REQUIRE( manager.open() );

    // Write directly to the underlying QFile (bypassing write()).
    // This is what bindOutputFile does when replaying segments.
    auto* file = manager.currentFile();
    REQUIRE( file != nullptr );
    file->write( QByteArray( 50, 'A' ) );
    file->flush();

    // Without resyncSize(), currentBytes_ is 0 even though the file has 50 bytes.
    CHECK( manager.currentFileSize() == 0 );
    CHECK_FALSE( manager.needsRotation() );

    // After resyncSize(), the size is correct.
    manager.resyncSize();
    CHECK( manager.currentFileSize() == 50 );
    // 50 < 64, so no rotation needed yet
    CHECK_FALSE( manager.needsRotation() );

    manager.deleteAll();
}

TEST_CASE( "RollingFileManager resyncSize enables rotation after direct writes" )
{
    const auto rootPath = makeTestDir( "rolling_resync_rotate" );
    const auto filePath = QDir( rootPath ).filePath( QStringLiteral( "output.log" ) );

    RollingFileManager manager( filePath, 32, 2 );
    REQUIRE( manager.open() );

    // Write 50 bytes directly to QFile (bypassing write()).
    auto* file = manager.currentFile();
    REQUIRE( file != nullptr );
    file->write( QByteArray( 50, 'B' ) );
    file->flush();

    // Sync the size so needsRotation() works correctly.
    manager.resyncSize();
    CHECK( manager.currentFileSize() == 50 );
    CHECK( manager.needsRotation() ); // 50 >= 32

    // The next write() call should trigger rotation because we're over the limit.
    manager.write( QByteArray( 10, 'C' ) );

    // After rotation, the current file should only contain the new data.
    manager.flush();
    QFile output( filePath );
    REQUIRE( output.open( QIODevice::ReadOnly ) );
    CHECK( output.size() == 10 );

    // Backup should contain the old 50-byte file.
    const auto backups = manager.backupFiles();
    CHECK( backups.size() >= 1 );

    manager.deleteAll();
}

TEST_CASE( "CaptureStore bindOutputFile syncs rolling file size after replay" )
{
    // Use small limits so replayed data exceeds rollingMaxFileSize.
    CaptureStore::Limits limits;
    limits.segmentTargetBytes = 16;
    limits.memoryBudgetBytes = 4096;
    limits.rollingMaxFileSize = 32;
    limits.rollingBackupCount = 2;

    const auto rootPath = makeTestDir( "capturestore_bind_tracking" );
    const auto outputPath = QDir( rootPath ).filePath( QStringLiteral( "tracked.log" ) );

    CaptureStore store( makeCaptureId(), rootPath, limits );

    // Append data that exceeds rollingMaxFileSize (32 bytes).
    for ( int i = 0; i < 10; ++i ) {
        store.appendUtf8( QByteArrayLiteral( "abcdefgh\n" ) );
    }

    // Bind output file — replays all data into the rolling file
    // via writeSegmentToDevice() which bypasses RollingFileManager::write().
    REQUIRE( store.bindOutputFile( outputPath ) );

    // Append more data — rotation should trigger because resyncSize()
    // updated currentBytes_ to reflect the replayed data.
    for ( int i = 0; i < 5; ++i ) {
        store.appendUtf8( QByteArrayLiteral( "new\n" ) );
    }

    // At least one rotation should have occurred, producing backup files.
    const auto backups = QDir( rootPath ).entryList(
        { QFileInfo( outputPath ).fileName() + ".*" }, QDir::Files );
    CHECK( backups.size() >= 1 );
}

TEST_CASE( "CaptureStore finishInput commits a trailing partial line without adding a newline" )
{
    CaptureStore::Limits limits;
    limits.segmentTargetBytes = 64;
    limits.memoryBudgetBytes = 4096;

    const auto rootPath = makeTestDir( "capturestore_partial_finish" );
    const auto outputPath = QDir( rootPath ).filePath( QStringLiteral( "saved.log" ) );
    auto* codec = QTextCodec::codecForName( "UTF-8" );

    CaptureStore store( makeCaptureId(), rootPath, limits );
    REQUIRE( store.bindOutputFile( outputPath ) );

    store.appendUtf8( QByteArrayLiteral( "partial-line" ) );
    REQUIRE( store.lineCount().get() == 0 );

    store.finishInput();

    REQUIRE( store.lineCount().get() == 1 );
    REQUIRE( store.lineAt( LineNumber( 0 ), codec, QRegularExpression{} )
             == QStringLiteral( "partial-line" ) );
    REQUIRE( readUtf8File( outputPath ) == QStringLiteral( "partial-line" ) );
}

TEST_CASE( "CaptureStore keeps a finished partial line when rolling output rotates" )
{
    CaptureStore::Limits limits;
    limits.segmentTargetBytes = 4;
    limits.memoryBudgetBytes = 4096;
    limits.rollingMaxFileSize = 64;
    limits.rollingBackupCount = 1;

    const auto rootPath = makeTestDir( "capturestore_partial_finish_rotation" );
    const auto outputPath = QDir( rootPath ).filePath( QStringLiteral( "saved.log" ) );
    auto* codec = QTextCodec::codecForName( "UTF-8" );

    CaptureStore store( makeCaptureId(), rootPath, limits );
    store.appendUtf8( QByteArrayLiteral( "old-a\nold-b\n" ) );

    // Rebind with a window smaller than the replayed segments. Finishing the
    // partial line rotates output and front-trims segments_, invalidating any
    // reference retained across appendOutputBytes().
    limits.rollingMaxFileSize = 4;
    store.setLimits( limits );
    REQUIRE( store.bindOutputFile( outputPath ) );
    store.appendUtf8( QByteArrayLiteral( "abcdef" ) );

    const auto result = store.finishInput();

    REQUIRE( result.lineCount == 1_lcount );
    REQUIRE( store.lineCount() == 1_lcount );
    REQUIRE( store.lineAt( 0_lnum, codec, QRegularExpression{} )
             == QStringLiteral( "abcdef" ) );
    REQUIRE( store.stats().fileSize == 6 );
    REQUIRE( readUtf8File( outputPath ) == QStringLiteral( "abcdef" ) );
}

TEST_CASE( "CaptureStore persists a trailing partial line on destruction" )
{
    CaptureStore::Limits limits;
    limits.segmentTargetBytes = 64;
    limits.memoryBudgetBytes = 4096;

    const auto rootPath = makeTestDir( "capturestore_partial_restore" );
    const auto captureId = makeCaptureId();

    {
        CaptureStore store( captureId, rootPath, limits );
        store.appendUtf8( QByteArrayLiteral( "tail-fragment" ) );
    }

    CaptureStore restored( captureId, rootPath, limits );
    REQUIRE( restored.loadFromDisk() );
    REQUIRE( restored.lineCount().get() == 1 );
    REQUIRE( restored.lineAt( LineNumber( 0 ), QTextCodec::codecForName( "UTF-8" ),
                              QRegularExpression{} )
             == QStringLiteral( "tail-fragment" ) );
}

TEST_CASE( "CaptureStore snapshots active bytes before concurrent append" )
{
    CaptureStore::Limits limits;
    limits.segmentTargetBytes = 4 * 1024 * 1024;
    limits.memoryBudgetBytes = 4 * 1024 * 1024;

    const auto rootPath = makeTestDir( "capturestore_concurrent" );
    CaptureStore store( makeCaptureId(), rootPath, limits );
    auto* codec = QTextCodec::codecForName( "UTF-8" );

    constexpr int snapshotLines = 256;
    QByteArray expectedSnapshot;
    for ( int i = 0; i < snapshotLines; ++i ) {
        expectedSnapshot.append(
            ( QStringLiteral( "line-%1-%2" )
                  .arg( i, 3, 10, QLatin1Char( '0' ) )
                  .arg( QString( 4096, QLatin1Char( 'x' ) ) )
              + QLatin1Char( '\n' ) )
                .toUtf8() );
    }
    store.appendUtf8( expectedSnapshot );

    std::atomic<bool> snapshotReady{ false };
    std::atomic<bool> appendComplete{ false };
    CaptureStoreTestAccess::setBeforeRawSnapshotCopyCallback(
        store, [ &snapshotReady, &appendComplete ] {
            snapshotReady.store( true, std::memory_order_release );
            while ( !appendComplete.load( std::memory_order_acquire ) ) {
                std::this_thread::yield();
            }
        } );

    std::unique_ptr<SearchableLogData::RawLines> rawLines;
    std::thread reader( [ &store, codec, &rawLines ] {
        rawLines = std::make_unique<SearchableLogData::RawLines>(
            store.buildRawLines( 0_lnum, LinesCount( snapshotLines ), codec,
                                 QRegularExpression{} ) );
    } );

    QElapsedTimer waitForSnapshot;
    waitForSnapshot.start();
    while ( !snapshotReady.load( std::memory_order_acquire )
            && waitForSnapshot.elapsed() < 5000 ) {
        std::this_thread::yield();
    }
    const auto reachedSnapshot = snapshotReady.load( std::memory_order_acquire );
    if ( reachedSnapshot ) {
        store.appendUtf8( QByteArrayLiteral( "appended-after-snapshot\n" ) );
        // Reproduce QByteArray's reallocation boundary deterministically: the
        // active object gets equivalent new storage and the retired allocation
        // is overwritten before buildRawLines consumes its captured pointer.
        CaptureStoreTestAccess::forceActiveStorageRelocation( store );
    }
    appendComplete.store( true, std::memory_order_release );
    reader.join();

    REQUIRE( reachedSnapshot );
    REQUIRE( rawLines );
    REQUIRE( rawLines->buffer.size()
             == static_cast<size_t>( expectedSnapshot.size() ) );
    REQUIRE( std::equal( rawLines->buffer.cbegin(), rawLines->buffer.cend(),
                         expectedSnapshot.cbegin() ) );
    REQUIRE( store.lineCount().get() == snapshotLines + 1 );
}

TEST_CASE( "CaptureStore buildRawLines snapshot consistency under concurrent append" )
{
    CaptureStore::Limits limits;
    limits.segmentTargetBytes = 32;
    limits.memoryBudgetBytes = 256;

    const auto rootPath = makeTestDir( "capturestore_snapshot_consistency" );
    CaptureStore store( makeCaptureId(), rootPath, limits );
    auto* codec = QTextCodec::codecForName( "UTF-8" );

    // Pre-populate some data so buildRawLines has something to read from the start.
    for ( int i = 0; i < 50; ++i ) {
        store.appendUtf8( QStringLiteral( "seed-%1\n" ).arg( i ).toUtf8() );
    }

    std::atomic<bool> writerDone{ false };
    std::atomic<int> readerIterations{ 0 };
    bool readerHadError = false;

    std::thread writer( [ &store, &writerDone ] {
        for ( int i = 50; i < 300; ++i ) {
            store.appendUtf8( QStringLiteral( "concurrent-%1\n" ).arg( i ).toUtf8() );
        }
        writerDone = true;
    } );

    // Reader thread: repeatedly call buildRawLines and verify internal consistency.
    std::thread reader( [ &store, &writerDone, &readerIterations, &readerHadError, codec ] {
        while ( !writerDone.load() || readerIterations.load() < 10 ) {
            const auto lines = store.lineCount();
            if ( lines <= 0_lcount ) {
                continue;
            }

            // Request a subset of lines from the middle of the store.
            const auto startLine = LineNumber( lines.get() / 2 );
            const auto count = LinesCount(
                qMin( static_cast<LinesCount::UnderlyingType>( 10 ), lines.get() - startLine.get() ) );
            if ( count <= 0_lcount ) {
                continue;
            }

            const auto rawLines
                = store.buildRawLines( startLine, count, codec, QRegularExpression{} );

            // Verify snapshot consistency: the number of endOfLines entries
            // must not exceed the requested count, and must match the buffer's
            // newline-terminated structure.
            if ( static_cast<LinesCount::UnderlyingType>( rawLines.endOfLines.size() ) > count.get() ) {
                readerHadError = true;
                break;
            }

            // Verify that each endOfLines offset is within the buffer.
            const auto bufferSize = static_cast<qint64>( rawLines.buffer.size() );
            for ( const auto& eol : rawLines.endOfLines ) {
                if ( eol > bufferSize ) {
                    readerHadError = true;
                    break;
                }
            }
            if ( readerHadError ) {
                break;
            }

            readerIterations.fetch_add( 1 );
        }
    } );

    writer.join();
    reader.join();

    REQUIRE_FALSE( readerHadError );
    REQUIRE( readerIterations.load() >= 10 );
    REQUIRE( store.lineCount().get() == 300 );
}

TEST_CASE( "CaptureStore incremental rebuildCumulativeLineCounts stays correct across many segments" )
{
    CaptureStore::Limits limits;
    limits.segmentTargetBytes = 8;
    limits.memoryBudgetBytes = 4096;

    const auto rootPath = makeTestDir( "capturestore_incremental_cumulative" );
    CaptureStore store( makeCaptureId(), rootPath, limits );
    auto* codec = QTextCodec::codecForName( "UTF-8" );

    // Append lines one at a time to force many segment rotations.
    constexpr int totalLines = 60;
    for ( int i = 0; i < totalLines; ++i ) {
        store.appendUtf8( QStringLiteral( "line-%1\n" ).arg( i, 3, 10, QLatin1Char( '0' ) ).toUtf8() );

        // Verify cumulative line count is correct after every append.
        REQUIRE( store.lineCount().get() == static_cast<LinesCount::UnderlyingType>( i + 1 ) );
    }

    // Verify every single line is addressable and contains the expected content.
    for ( int i = 0; i < totalLines; ++i ) {
        INFO( "Checking line " << i );
        const auto expected
            = QStringLiteral( "line-%1" ).arg( i, 3, 10, QLatin1Char( '0' ) );
        REQUIRE( store.lineAt( LineNumber( static_cast<LineNumber::UnderlyingType>( i ) ), codec,
                              QRegularExpression{} )
                 == expected );
    }

    // Also verify buildRawLines covers the full range correctly.
    const auto rawLines = store.buildRawLines( 0_lnum, LinesCount( totalLines ), codec,
                                                QRegularExpression{} );
    REQUIRE( rawLines.endOfLines.size() == static_cast<size_t>( totalLines ) );
}

TEST_CASE( "CaptureStore buildRawLines snapshot spans in-memory and spilled segments" )
{
    CaptureStore::Limits limits;
    limits.segmentTargetBytes = 8;
    limits.memoryBudgetBytes = 24;

    const auto rootPath = makeTestDir( "capturestore_snapshot_spill" );
    CaptureStore store( makeCaptureId(), rootPath, limits );
    auto* codec = QTextCodec::codecForName( "UTF-8" );

    // Append enough data to ensure some segments get spilled to disk.
    const QStringList expectedLines = {
        QStringLiteral( "alpha" ),   QStringLiteral( "bravo" ),
        QStringLiteral( "charlie" ), QStringLiteral( "delta" ),
        QStringLiteral( "echo" ),    QStringLiteral( "foxtrot" ),
        QStringLiteral( "golf" ),    QStringLiteral( "hotel" ),
        QStringLiteral( "india" ),   QStringLiteral( "juliet" ),
    };

    for ( const auto& line : expectedLines ) {
        store.appendUtf8( ( line + QLatin1Char( '\n' ) ).toUtf8() );
    }

    // Confirm spilling actually happened.
    REQUIRE_FALSE( segmentFiles( store.capturePath() ).empty() );
    REQUIRE( store.lineCount().get() == 10 );

    // Build raw lines spanning the entire range (both spilled and in-memory).
    const auto rawLines = store.buildRawLines( 0_lnum, LinesCount( static_cast<uint64_t>( expectedLines.size() ) ),
                                                codec, QRegularExpression{} );
    REQUIRE( rawLines.endOfLines.size() == 10 );

    // Decode and verify every line matches the original.
    const auto decodedLines = rawLines.decodeLines();
    REQUIRE( decodedLines.size() == 10 );
    for ( size_t i = 0; i < decodedLines.size(); ++i ) {
        INFO( "Verifying decoded line " << i );
        REQUIRE( decodedLines[ i ] == expectedLines[ static_cast<int>( i ) ] );
    }

    // Also verify individual lineAt access for a few spilled lines.
    REQUIRE( store.lineAt( LineNumber( 0 ), codec, QRegularExpression{} )
             == QStringLiteral( "alpha" ) );
    REQUIRE( store.lineAt( LineNumber( 4 ), codec, QRegularExpression{} )
             == QStringLiteral( "echo" ) );
    REQUIRE( store.lineAt( LineNumber( 9 ), codec, QRegularExpression{} )
             == QStringLiteral( "juliet" ) );
}

TEST_CASE( "CaptureStore buildRawLines converts non UTF-8 input before search views" )
{
    const auto* latin1Codec = QTextCodec::codecForName( "ISO-8859-1" );
    REQUIRE( latin1Codec != nullptr );

    const auto rootPath = makeTestDir( "capturestore_non_utf8_rawlines" );
    CaptureStore store( makeCaptureId(), rootPath );
    store.appendUtf8( QByteArray::fromHex( "636166e90a" ) ); // cafe acute in ISO-8859-1

    const auto rawLines = store.buildRawLines( 0_lnum, 1_lcount, const_cast<QTextCodec*>( latin1Codec ),
                                               QRegularExpression{} );
    const auto decoded = rawLines.decodeLines();
    REQUIRE( decoded.size() == 1 );
    REQUIRE( decoded[ 0 ] == QString::fromLatin1( "caf\xe9" ) );

    const auto utf8View = rawLines.buildUtf8View();
    REQUIRE( utf8View.size() == 1 );
    REQUIRE( utf8View[ 0 ] == std::string_view{ "caf\xc3\xa9" } );
}

TEST_CASE( "CaptureStore appends large UTF-8 batches within a linear-time budget" )
{
    const auto rootPath = makeTestDir( "capturestore_large_append_budget" );
    CaptureStore store( makeCaptureId(), rootPath );

    constexpr int lineCount = 1000000;
    QByteArray data;
    data.reserve( lineCount * 32 );
    for ( int i = 0; i < lineCount; ++i ) {
        data.append( "line-" );
        data.append( QByteArray::number( i ) );
        data.append( "\r\n" );
    }

    QElapsedTimer timer;
    timer.start();
    store.appendUtf8( data );
    const auto elapsedMs = timer.elapsed();

    REQUIRE( store.lineCount().get() == lineCount );
    REQUIRE( store.lineAt( 0_lnum, QTextCodec::codecForName( "UTF-8" ), QRegularExpression{} )
             == QStringLiteral( "line-0" ) );
    REQUIRE( store.lineAt( LineNumber( lineCount - 1 ), QTextCodec::codecForName( "UTF-8" ),
                           QRegularExpression{} )
             == QStringLiteral( "line-999999" ) );
    CHECK( elapsedMs < 2000 );
}

TEST_CASE( "CaptureStore appends large UTF-8 batches with low per-line metadata overhead" )
{
    const auto rootPath = makeTestDir( "capturestore_large_append_metadata_budget" );
    CaptureStore store( makeCaptureId(), rootPath );

    constexpr int lineCount = 1000000;
    QByteArray data;
    data.reserve( lineCount * 16 );
    for ( int i = 0; i < lineCount; ++i ) {
        data.append( "m-" );
        data.append( QByteArray::number( i ) );
        data.append( '\n' );
    }

    QElapsedTimer timer;
    timer.start();
    store.appendUtf8( data );
    const auto elapsedMs = timer.elapsed();

    REQUIRE( store.lineCount().get() == lineCount );
    REQUIRE( store.lineAt( 0_lnum, QTextCodec::codecForName( "UTF-8" ), QRegularExpression{} )
             == QStringLiteral( "m-0" ) );
    REQUIRE( store.lineAt( LineNumber( lineCount - 1 ), QTextCodec::codecForName( "UTF-8" ),
                           QRegularExpression{} )
             == QStringLiteral( "m-999999" ) );
    REQUIRE( store.stats().memoryBytes == data.size() );
    // The per-line metadata budget is calibrated for optimised builds
    // (RelWithDebInfo/Release legs, where NDEBUG is defined and -O2 is in
    // effect).  The coverage leg builds with -O0 + gcov instrumentation,
    // which runs the 1M-line append many times slower; give it the same
    // generous budget the linear-time sibling test uses so the assertion
    // stays a release-only regression guard rather than an instrumentation
    // wall-clock trip wire. Sanitizer legs instrument allocator dependencies
    // as well; Debug does not define NDEBUG, while RelWithDebInfo does.
#if defined( KLOGG_SANITIZER_BUILD ) || !defined( NDEBUG )
    constexpr int MetadataOverheadBudgetMs = 2000;
#else
    constexpr int MetadataOverheadBudgetMs = 200;
#endif
    CHECK( elapsedMs < MetadataOverheadBudgetMs );
}

TEST_CASE( "CaptureStore batched output defers flush below threshold" )
{
    const auto rootPath = makeTestDir( "capturestore_batched_flush" );
    const auto outputPath = QDir( rootPath ).filePath( QStringLiteral( "batched.log" ) );

    CaptureStore store( makeCaptureId(), rootPath );
    REQUIRE( store.bindOutputFile( outputPath ) );

    // Append a small amount of data (well below 1MB and 1000 lines)
    for ( int i = 0; i < 10; ++i ) {
        store.appendUtf8( QStringLiteral( "short-line-%1\n" ).arg( i ).toUtf8() );
    }

    // Before explicit flush, data may not yet be on disk (deferred)
    {
        QFile preFlush( outputPath );
        REQUIRE( preFlush.open( QIODevice::ReadOnly ) );
        // The file exists (bindOutputFile wrote existing data), but the 10 new
        // lines should not have been flushed yet since we are below all thresholds.
        const auto preContent = QString::fromUtf8( preFlush.readAll() );
        CHECK_FALSE( preContent.contains( QStringLiteral( "short-line-9" ) ) );
    }

    // After explicit flush, all data should be on disk
    store.flush();
    const auto content = readUtf8File( outputPath );
    REQUIRE( content.contains( QStringLiteral( "short-line-9" ) ) );
}

TEST_CASE( "CaptureStore batched output auto-flushes after byte threshold" )
{
    const auto rootPath = makeTestDir( "capturestore_byte_threshold" );
    const auto outputPath = QDir( rootPath ).filePath( QStringLiteral( "big.log" ) );

    CaptureStore store( makeCaptureId(), rootPath );
    REQUIRE( store.bindOutputFile( outputPath ) );

    // Append more than 1MB of data to trigger auto-flush
    const QByteArray bigLine = QByteArray( 1024, 'X' ) + "\n";
    for ( int i = 0; i < 1040; ++i ) {
        store.appendUtf8( bigLine );
    }

    // Data should have been auto-flushed (1040KB > 1MB threshold)
    QFile output( outputPath );
    REQUIRE( output.open( QIODevice::ReadOnly ) );
    REQUIRE( output.size() > 1024 * 1024 );
}

TEST_CASE( "CaptureStore batched output auto-flushes after line threshold" )
{
    const auto rootPath = makeTestDir( "capturestore_line_threshold" );
    const auto outputPath = QDir( rootPath ).filePath( QStringLiteral( "lines.log" ) );

    CaptureStore store( makeCaptureId(), rootPath );
    REQUIRE( store.bindOutputFile( outputPath ) );

    // Append more than 1000 lines (each small enough to stay under 1MB total)
    for ( int i = 0; i < 1010; ++i ) {
        store.appendUtf8( QStringLiteral( "ln-%1\n" ).arg( i ).toUtf8() );
    }

    // Data should have been auto-flushed (1010 lines > 1000 line threshold)
    const auto content = readUtf8File( outputPath );
    REQUIRE( content.contains( QStringLiteral( "ln-0" ) ) );
}

TEST_CASE( "CaptureStore finishInput flushes pending output data" )
{
    const auto rootPath = makeTestDir( "capturestore_finish_flush" );
    const auto outputPath = QDir( rootPath ).filePath( QStringLiteral( "finish.log" ) );

    CaptureStore store( makeCaptureId(), rootPath );
    REQUIRE( store.bindOutputFile( outputPath ) );

    // Append small amount (under all thresholds)
    store.appendUtf8( QByteArrayLiteral( "pending-data\n" ) );

    // finishInput should flush remaining data
    store.finishInput();

    const auto content = readUtf8File( outputPath );
    REQUIRE( content.contains( QStringLiteral( "pending-data" ) ) );
}

TEST_CASE( "CaptureStore buildRawLines bulk-read spans multiple segments" )
{
    CaptureStore::Limits limits;
    limits.segmentTargetBytes = 16;
    limits.memoryBudgetBytes = 4096;

    const auto rootPath = makeTestDir( "capturestore_bulk_multi_segment" );
    CaptureStore store( makeCaptureId(), rootPath, limits );
    auto* codec = QTextCodec::codecForName( "UTF-8" );

    // Each line is short enough that several lines fit per segment,
    // but with segmentTargetBytes=16, many segments will be created.
    const QStringList expectedLines = {
        QStringLiteral( "aa" ), QStringLiteral( "bb" ), QStringLiteral( "cc" ),
        QStringLiteral( "dd" ), QStringLiteral( "ee" ), QStringLiteral( "ff" ),
        QStringLiteral( "gg" ), QStringLiteral( "hh" ), QStringLiteral( "ii" ),
    };

    for ( const auto& line : expectedLines ) {
        store.appendUtf8( ( line + QLatin1Char( '\n' ) ).toUtf8() );
    }

    REQUIRE( store.lineCount().get() == 9 );

    // Request all lines — this forces the bulk read to traverse multiple segments.
    const auto rawLines = store.buildRawLines( 0_lnum, LinesCount( 9 ), codec, QRegularExpression{} );
    REQUIRE( rawLines.endOfLines.size() == 9 );

    const auto decoded = rawLines.decodeLines();
    REQUIRE( decoded.size() == 9 );
    for ( size_t i = 0; i < decoded.size(); ++i ) {
        INFO( "Multi-segment line " << i );
        REQUIRE( decoded[ i ] == expectedLines[ static_cast<int>( i ) ] );
    }

    // Also verify buildUtf8View produces the same content.
    const auto views = rawLines.buildUtf8View();
    REQUIRE( views.size() == 9 );
    for ( size_t i = 0; i < views.size(); ++i ) {
        INFO( "Multi-segment utf8 view " << i );
        REQUIRE( views[ i ] == expectedLines[ static_cast<int>( i ) ].toStdString() );
    }
}

TEST_CASE( "CaptureStore buildRawLines handles unterminated last line" )
{
    CaptureStore::Limits limits;
    limits.segmentTargetBytes = 64;
    limits.memoryBudgetBytes = 4096;

    const auto rootPath = makeTestDir( "capturestore_unterminated" );
    CaptureStore store( makeCaptureId(), rootPath, limits );
    auto* codec = QTextCodec::codecForName( "UTF-8" );

    // Append data where the last line has no trailing newline.
    store.appendUtf8( QByteArrayLiteral( "alpha\nbeta\nno-newline" ) );
    // finishInput commits the trailing partial line.
    store.finishInput();

    REQUIRE( store.lineCount().get() == 3 );

    const auto rawLines = store.buildRawLines( 0_lnum, LinesCount( 3 ), codec, QRegularExpression{} );
    REQUIRE( rawLines.endOfLines.size() == 3 );

    const auto decoded = rawLines.decodeLines();
    REQUIRE( decoded.size() == 3 );
    REQUIRE( decoded[ 0 ] == QStringLiteral( "alpha" ) );
    REQUIRE( decoded[ 1 ] == QStringLiteral( "beta" ) );
    REQUIRE( decoded[ 2 ] == QStringLiteral( "no-newline" ) );

    // Verify the unterminated line gets a synthetic \n appended in the buffer.
    REQUIRE( rawLines.buffer.back() == '\n' );
}

TEST_CASE( "CaptureStore buildRawLines CRLF normalization in slow path" )
{
    const auto* latin1Codec = QTextCodec::codecForName( "ISO-8859-1" );
    REQUIRE( latin1Codec != nullptr );

    const auto rootPath = makeTestDir( "capturestore_crlf_slow_path" );
    CaptureStore store( makeCaptureId(), rootPath );

    // CRLF line endings with a non-UTF8 codec forces the slow path.
    store.appendUtf8( QByteArrayLiteral( "line1\r\nline2\r\nline3\r\n" ) );
    REQUIRE( store.lineCount().get() == 3 );

    const auto rawLines = store.buildRawLines( 0_lnum, LinesCount( 3 ), const_cast<QTextCodec*>( latin1Codec ), QRegularExpression{} );
    REQUIRE( rawLines.endOfLines.size() == 3 );

    const auto decoded = rawLines.decodeLines();
    REQUIRE( decoded.size() == 3 );
    // \r should be stripped in the slow path.
    REQUIRE( decoded[ 0 ] == QStringLiteral( "line1" ) );
    REQUIRE( decoded[ 1 ] == QStringLiteral( "line2" ) );
    REQUIRE( decoded[ 2 ] == QStringLiteral( "line3" ) );
}

TEST_CASE( "CaptureStore buildRawLines applies prefilter pattern in slow path" )
{
    const auto* latin1Codec = QTextCodec::codecForName( "ISO-8859-1" );
    REQUIRE( latin1Codec != nullptr );

    const auto rootPath = makeTestDir( "capturestore_prefilter" );
    CaptureStore store( makeCaptureId(), rootPath );

    store.appendUtf8( QByteArrayLiteral( "[INFO] message-one\n[WARN] message-two\n" ) );
    REQUIRE( store.lineCount().get() == 2 );

    // A prefilter pattern forces the slow path even with UTF-8 data,
    // but we also use a non-UTF8 codec here to ensure the slow path.
    QRegularExpression prefilter( QStringLiteral( "\\[\\w+\\]\\s*" ) );
    const auto rawLines = store.buildRawLines( 0_lnum, LinesCount( 2 ), const_cast<QTextCodec*>( latin1Codec ), prefilter );
    REQUIRE( rawLines.endOfLines.size() == 2 );

    const auto decoded = rawLines.decodeLines();
    REQUIRE( decoded.size() == 2 );
    REQUIRE( decoded[ 0 ] == QStringLiteral( "message-one" ) );
    REQUIRE( decoded[ 1 ] == QStringLiteral( "message-two" ) );
}

TEST_CASE( "CaptureStore buildRawLines reads from spilled disk segments" )
{
    CaptureStore::Limits limits;
    limits.segmentTargetBytes = 8;
    limits.memoryBudgetBytes = 16;

    const auto rootPath = makeTestDir( "capturestore_disk_read" );
    CaptureStore store( makeCaptureId(), rootPath, limits );
    auto* codec = QTextCodec::codecForName( "UTF-8" );

    const QStringList expectedLines = {
        QStringLiteral( "aaa" ), QStringLiteral( "bbb" ),
        QStringLiteral( "ccc" ), QStringLiteral( "ddd" ),
        QStringLiteral( "eee" ), QStringLiteral( "fff" ),
    };

    for ( const auto& line : expectedLines ) {
        store.appendUtf8( ( line + QLatin1Char( '\n' ) ).toUtf8() );
    }

    REQUIRE( store.lineCount().get() == 6 );
    REQUIRE_FALSE( segmentFiles( store.capturePath() ).empty() );

    // Read all lines — at least some must come from disk (spilled segments).
    const auto rawLines = store.buildRawLines( 0_lnum, LinesCount( 6 ), codec, QRegularExpression{} );
    REQUIRE( rawLines.endOfLines.size() == 6 );

    const auto decoded = rawLines.decodeLines();
    REQUIRE( decoded.size() == 6 );
    for ( size_t i = 0; i < decoded.size(); ++i ) {
        INFO( "Disk-read line " << i );
        REQUIRE( decoded[ i ] == expectedLines[ static_cast<int>( i ) ] );
    }

    // Read a subset from the middle — spanning a spilled segment boundary.
    const auto midLines = store.buildRawLines( LineNumber( 2 ), LinesCount( 3 ), codec, QRegularExpression{} );
    REQUIRE( midLines.endOfLines.size() == 3 );
    const auto midDecoded = midLines.decodeLines();
    REQUIRE( midDecoded[ 0 ] == QStringLiteral( "ccc" ) );
    REQUIRE( midDecoded[ 1 ] == QStringLiteral( "ddd" ) );
    REQUIRE( midDecoded[ 2 ] == QStringLiteral( "eee" ) );
}

TEST_CASE( "CaptureStore publishes spilled segments only after a complete write" )
{
    CaptureStore::Limits limits;
    limits.segmentTargetBytes = 8;
    limits.memoryBudgetBytes = 4096;

    const auto rootPath = makeTestDir( "capturestore_atomic_spill_publish" );
    const auto captureId = makeCaptureId();
    QString capturePath;
    {
        CaptureStore store( captureId, rootPath, limits );
        capturePath = store.capturePath();
        store.appendUtf8( QByteArrayLiteral( "aaa\nbbb\n" ) );
        REQUIRE( segmentFiles( capturePath ).isEmpty() );

        CaptureStoreTestAccess::failNextRetiredFileRemoval( store );
        CaptureStoreTestAccess::failNextSegmentWrite( store );
        REQUIRE_FALSE( CaptureStoreTestAccess::spillFirstSegment( store ) );
        REQUIRE( segmentFiles( capturePath ).isEmpty() );

        REQUIRE( CaptureStoreTestAccess::spillFirstSegment( store ) );
        REQUIRE( segmentFiles( capturePath ).size() == 1 );
    }

    CaptureStore restored( captureId, rootPath, limits );
    REQUIRE( restored.loadFromDisk() );
    REQUIRE( restored.lineCount() == 2_lcount );
    REQUIRE( restored.lineAt( 0_lnum, QTextCodec::codecForName( "UTF-8" ),
                              QRegularExpression{} )
             == QStringLiteral( "aaa" ) );
    REQUIRE( restored.lineAt( 1_lnum, QTextCodec::codecForName( "UTF-8" ),
                              QRegularExpression{} )
             == QStringLiteral( "bbb" ) );
}

TEST_CASE( "CaptureStore final pinned spill release drops its path registry tombstone" )
{
    CaptureStore::Limits limits;
    limits.segmentTargetBytes = 64;
    limits.memoryBudgetBytes = 4096;

    const auto rootPath = makeTestDir( "capturestore_final_pinned_release" );
    const auto captureId = makeCaptureId();
    const auto capturePath = QDir( rootPath ).filePath( captureId );

    std::weak_ptr<void> capturePathStateLifetime;
    std::shared_ptr<void> pinnedSpilledSegment;
    QString spilledPath;
    {
        auto store = std::make_unique<CaptureStore>( captureId, rootPath, limits );
        store->appendUtf8( QByteArrayLiteral( "aaa\nbbb\n" ) );
        REQUIRE( CaptureStoreTestAccess::spillFirstSegment( *store ) );

        const auto spilledFiles = segmentFiles( capturePath );
        REQUIRE( spilledFiles.size() == 1 );
        spilledPath = QDir( capturePath ).filePath( spilledFiles.front() );
        capturePathStateLifetime
            = CaptureStoreTestAccess::capturePathStateLifetime( *store );
        pinnedSpilledSegment
            = CaptureStoreTestAccess::pinFirstSpilledSegment( *store );
        REQUIRE( pinnedSpilledSegment );

        store->deleteCaptureFiles();
        REQUIRE( QFileInfo::exists( spilledPath ) );
        REQUIRE( QFileInfo::exists( capturePath ) );
        REQUIRE_FALSE( capturePathStateLifetime.expired() );
    }

    REQUIRE( QFileInfo::exists( spilledPath ) );
    REQUIRE( QFileInfo::exists( capturePath ) );
    REQUIRE_FALSE( capturePathStateLifetime.expired() );

    pinnedSpilledSegment.reset();

    REQUIRE_FALSE( QFileInfo::exists( spilledPath ) );
    REQUIRE_FALSE( QFileInfo::exists( capturePath ) );
    REQUIRE( capturePathStateLifetime.expired() );
}

TEST_CASE( "CaptureStore retries transient deletion on final pinned spill release" )
{
    CaptureStore::Limits limits;
    limits.segmentTargetBytes = 64;
    limits.memoryBudgetBytes = 4096;

    const auto rootPath = makeTestDir( "capturestore_final_pinned_retry" );
    const auto captureId = makeCaptureId();
    const auto capturePath = QDir( rootPath ).filePath( captureId );

    std::weak_ptr<void> capturePathStateLifetime;
    std::shared_ptr<void> pinnedSpilledSegment;
    QString spilledPath;
    {
        auto store = std::make_unique<CaptureStore>( captureId, rootPath, limits );
        store->appendUtf8( QByteArrayLiteral( "aaa\nbbb\n" ) );
        REQUIRE( CaptureStoreTestAccess::spillFirstSegment( *store ) );
        const auto spilledFiles = segmentFiles( capturePath );
        REQUIRE( spilledFiles.size() == 1 );
        spilledPath = QDir( capturePath ).filePath( spilledFiles.front() );
        capturePathStateLifetime
            = CaptureStoreTestAccess::capturePathStateLifetime( *store );
        pinnedSpilledSegment
            = CaptureStoreTestAccess::pinFirstSpilledSegment( *store );
        REQUIRE( pinnedSpilledSegment );

        store->deleteCaptureFiles();
        CaptureStoreTestAccess::failNextRetiredFileRemoval( *store );
    }

    pinnedSpilledSegment.reset();

    REQUIRE_FALSE( QFileInfo::exists( spilledPath ) );
    REQUIRE_FALSE( QFileInfo::exists( capturePath ) );
    REQUIRE( capturePathStateLifetime.expired() );
}

TEST_CASE( "CaptureStore pins spilled raw reads while maintenance proceeds" )
{
    CaptureStore::Limits limits;
    limits.segmentTargetBytes = 8;
    limits.memoryBudgetBytes = 16;

    const auto rootPath = makeTestDir( "capturestore_pinned_spill_read" );
    CaptureStore store( makeCaptureId(), rootPath, limits );
    auto* codec = QTextCodec::codecForName( "UTF-8" );

    store.appendUtf8( QByteArrayLiteral( "aaa\nbbb\nccc\nddd\neee\nfff\n" ) );
    const auto capturePath = store.capturePath();
    const auto spilledFiles = segmentFiles( capturePath );
    REQUIRE_FALSE( spilledFiles.isEmpty() );
    const auto selectedSegmentPath = QDir( capturePath ).filePath( spilledFiles.front() );

    QStringList replacementLines;
    QByteArray replacementData;
    for ( int i = 0; i < 12; ++i ) {
        const auto line = QStringLiteral( "new-%1" ).arg( i, 2, 10, QLatin1Char( '0' ) );
        replacementLines.append( line );
        replacementData.append( line.toUtf8() ).append( '\n' );
    }

    std::atomic<bool> readPaused{ false };
    std::atomic<bool> releaseRead{ false };
    CaptureStoreTestAccess::setBeforeSpilledSegmentReadCallback(
        store, [ &readPaused, &releaseRead ] {
            readPaused.store( true, std::memory_order_release );
            while ( !releaseRead.load( std::memory_order_acquire ) ) {
                std::this_thread::yield();
            }
        } );

    std::unique_ptr<SearchableLogData::RawLines> rawLines;
    std::thread reader( [ &store, codec, &rawLines ] {
        rawLines = std::make_unique<SearchableLogData::RawLines>(
            store.buildRawLines( 0_lnum, 2_lcount, codec, QRegularExpression{} ) );
    } );

    QElapsedTimer waitForRead;
    waitForRead.start();
    while ( !readPaused.load( std::memory_order_acquire ) && waitForRead.elapsed() < 5000 ) {
        std::this_thread::yield();
    }

    const auto reachedReadBoundary = readPaused.load( std::memory_order_acquire );
    const auto storeLockAvailable
        = reachedReadBoundary && CaptureStoreTestAccess::canLockStoreMutex( store );

    bool expectCaptureDirectoryRemoval = false;
    bool appendReplacementGeneration = false;
    bool expectEmptyStoreAfterMaintenance = false;
    bool reloadSucceeded = true;
    std::function<void()> maintenance;
    SECTION( "trim" )
    {
        maintenance = [ &store, limits ] {
            auto trimmedLimits = limits;
            trimmedLimits.maxTotalLines = 1;
            store.setLimits( trimmedLimits );
            store.trimToLimits();
        };
    }
    SECTION( "clear and append a replacement generation" )
    {
        appendReplacementGeneration = true;
        maintenance = [ &store ] { store.clear(); };
    }
    SECTION( "delete capture files" )
    {
        expectCaptureDirectoryRemoval = true;
        maintenance = [ &store ] { store.deleteCaptureFiles(); };
    }
    SECTION( "delete and append a replacement generation" )
    {
        appendReplacementGeneration = true;
        maintenance = [ &store ] { store.deleteCaptureFiles(); };
    }
    SECTION( "reload and clear" )
    {
        expectEmptyStoreAfterMaintenance = true;
        maintenance = [ &store, &reloadSucceeded ] {
            reloadSucceeded = store.loadFromDisk();
            store.clear();
        };
    }
    SECTION( "clear twice" )
    {
        expectEmptyStoreAfterMaintenance = true;
        maintenance = [ &store ] {
            store.clear();
            store.clear();
        };
    }
    SECTION( "delete and reload" )
    {
        expectCaptureDirectoryRemoval = true;
        expectEmptyStoreAfterMaintenance = true;
        maintenance = [ &store, &reloadSucceeded ] {
            store.deleteCaptureFiles();
            reloadSucceeded = store.loadFromDisk();
        };
    }

    QStringList replacementSegmentPaths;
    if ( storeLockAvailable ) {
        maintenance();
        if ( appendReplacementGeneration ) {
            store.appendUtf8( replacementData );
            for ( const auto& fileName : segmentFiles( capturePath ) ) {
                if ( !spilledFiles.contains( fileName ) ) {
                    replacementSegmentPaths.append( QDir( capturePath ).filePath( fileName ) );
                }
            }
        }
    }
    const auto selectedSourceRemainedPinned
        = storeLockAvailable && QFileInfo::exists( selectedSegmentPath );
    const auto replacementGenerationSpilled
        = !appendReplacementGeneration || !replacementSegmentPaths.isEmpty();

    releaseRead.store( true, std::memory_order_release );
    reader.join();

    REQUIRE( reachedReadBoundary );
    REQUIRE( storeLockAvailable );
    REQUIRE( reloadSucceeded );
    REQUIRE( selectedSourceRemainedPinned );
    REQUIRE( replacementGenerationSpilled );
    REQUIRE( rawLines );
    const auto decodedLines = rawLines->decodeLines();
    REQUIRE( decodedLines.size() == 2 );
    REQUIRE( decodedLines[ 0 ] == QStringLiteral( "aaa" ) );
    REQUIRE( decodedLines[ 1 ] == QStringLiteral( "bbb" ) );
    REQUIRE_FALSE( QFileInfo::exists( selectedSegmentPath ) );
    if ( expectEmptyStoreAfterMaintenance ) {
        REQUIRE( store.lineCount() == 0_lcount );
    }

    if ( appendReplacementGeneration ) {
        REQUIRE( QFileInfo::exists( capturePath ) );
        for ( const auto& replacementPath : replacementSegmentPaths ) {
            REQUIRE( QFileInfo::exists( replacementPath ) );
        }
        const auto replacementRawLines
            = store.buildRawLines( 0_lnum, 12_lcount, codec, QRegularExpression{} );
        const auto decodedReplacementLines = replacementRawLines.decodeLines();
        REQUIRE( decodedReplacementLines.size()
                 == static_cast<size_t>( replacementLines.size() ) );
        for ( auto i = 0; i < replacementLines.size(); ++i ) {
            REQUIRE( decodedReplacementLines[ static_cast<size_t>( i ) ]
                     == replacementLines[ i ] );
        }
    } else {
        REQUIRE( QFileInfo::exists( capturePath ) != expectCaptureDirectoryRemoval );
    }
}

TEST_CASE( "CaptureStore keeps same-path replacement files separate from pinned readers" )
{
    CaptureStore::Limits limits;
    limits.segmentTargetBytes = 8;
    limits.memoryBudgetBytes = 16;

    const auto rootPath = makeTestDir( "capturestore_cross_store_generation" );
    const auto captureId = makeCaptureId();
    CaptureStore oldStore( captureId, rootPath, limits );
    auto* codec = QTextCodec::codecForName( "UTF-8" );

    oldStore.appendUtf8( QByteArrayLiteral( "aaa\nbbb\nccc\nddd\neee\nfff\n" ) );
    const auto capturePath = oldStore.capturePath();
    const auto oldFiles = segmentFiles( capturePath );
    REQUIRE_FALSE( oldFiles.isEmpty() );
    const auto pinnedPath = QDir( capturePath ).filePath( oldFiles.front() );

    std::atomic<bool> readPaused{ false };
    std::atomic<bool> releaseRead{ false };
    CaptureStoreTestAccess::setBeforeSpilledSegmentReadCallback(
        oldStore, [ &readPaused, &releaseRead ] {
            readPaused.store( true, std::memory_order_release );
            while ( !releaseRead.load( std::memory_order_acquire ) ) {
                std::this_thread::yield();
            }
        } );

    std::unique_ptr<SearchableLogData::RawLines> oldRawLines;
    std::thread reader( [ &oldStore, codec, &oldRawLines ] {
        oldRawLines = std::make_unique<SearchableLogData::RawLines>(
            oldStore.buildRawLines( 0_lnum, 2_lcount, codec, QRegularExpression{} ) );
    } );

    QElapsedTimer waitForRead;
    waitForRead.start();
    while ( !readPaused.load( std::memory_order_acquire )
            && waitForRead.elapsed() < 5000 ) {
        std::this_thread::yield();
    }
    const auto reachedReadBoundary
        = readPaused.load( std::memory_order_acquire );

    QStringList replacementFiles;
    bool replacementPathsAreDistinct = false;
    std::unique_ptr<CaptureStore> replacementStore;
    if ( reachedReadBoundary ) {
        oldStore.deleteCaptureFiles();

        replacementStore
            = std::make_unique<CaptureStore>( captureId, rootPath, limits );
        replacementStore->appendUtf8(
            QByteArrayLiteral( "new-a\nnew-b\nnew-c\nnew-d\nnew-e\nnew-f\n" ) );
        for ( const auto& segmentFile : segmentFiles( capturePath ) ) {
            if ( !oldFiles.contains( segmentFile ) ) {
                replacementFiles.append( segmentFile );
            }
        }
        replacementPathsAreDistinct = !replacementFiles.isEmpty();
        for ( const auto& replacementFile : replacementFiles ) {
            replacementPathsAreDistinct
                = replacementPathsAreDistinct
                  && QDir( capturePath ).filePath( replacementFile ) != pinnedPath;
        }
    }

    releaseRead.store( true, std::memory_order_release );
    reader.join();

    REQUIRE( reachedReadBoundary );
    REQUIRE( replacementStore );
    REQUIRE( replacementPathsAreDistinct );
    REQUIRE( oldRawLines );
    const auto decodedOldLines = oldRawLines->decodeLines();
    REQUIRE( decodedOldLines.size() == 2 );
    REQUIRE( decodedOldLines[ 0 ] == QStringLiteral( "aaa" ) );
    REQUIRE( decodedOldLines[ 1 ] == QStringLiteral( "bbb" ) );
    REQUIRE_FALSE( QFileInfo::exists( pinnedPath ) );

    const auto replacementRawLines = replacementStore->buildRawLines(
        0_lnum, 6_lcount, codec, QRegularExpression{} );
    const auto decodedReplacementLines = replacementRawLines.decodeLines();
    REQUIRE( decodedReplacementLines.size() == 6 );
    REQUIRE( decodedReplacementLines[ 0 ] == QStringLiteral( "new-a" ) );
    REQUIRE( decodedReplacementLines[ 5 ] == QStringLiteral( "new-f" ) );
    for ( const auto& replacementFile : replacementFiles ) {
        REQUIRE( QFileInfo::exists( QDir( capturePath ).filePath( replacementFile ) ) );
    }
}

TEST_CASE( "CaptureStore replacement construction preserves directory before any segment" )
{
    CaptureStore::Limits limits;
    limits.segmentTargetBytes = 8;
    limits.memoryBudgetBytes = 16;

    const auto rootPath = makeTestDir( "capturestore_replacement_partial" );
    const auto captureId = makeCaptureId();
    CaptureStore oldStore( captureId, rootPath, limits );
    oldStore.appendUtf8( QByteArrayLiteral( "aaa\nbbb\nccc\nddd\neee\nfff\n" ) );

    std::atomic<bool> readPaused{ false };
    std::atomic<bool> releaseRead{ false };
    CaptureStoreTestAccess::setBeforeSpilledSegmentReadCallback(
        oldStore, [ &readPaused, &releaseRead ] {
            readPaused.store( true, std::memory_order_release );
            while ( !releaseRead.load( std::memory_order_acquire ) ) {
                std::this_thread::yield();
            }
        } );
    std::thread reader( [ &oldStore ] {
        oldStore.buildRawLines( 0_lnum, 2_lcount, QTextCodec::codecForName( "UTF-8" ),
                                QRegularExpression{} );
    } );
    QElapsedTimer deadline;
    deadline.start();
    while ( !readPaused.load( std::memory_order_acquire ) && deadline.elapsed() < 5000 ) {
        std::this_thread::yield();
    }
    const auto reachedReadBoundary = readPaused.load( std::memory_order_acquire );
    if ( reachedReadBoundary ) {
        oldStore.deleteCaptureFiles();
        CaptureStore replacementStore( captureId, rootPath, limits );
        replacementStore.appendUtf8( QByteArrayLiteral( "partial" ) );
        releaseRead.store( true, std::memory_order_release );
        reader.join();
        REQUIRE( QFileInfo::exists( replacementStore.capturePath() ) );
        REQUIRE( replacementStore.lineCount() == 0_lcount );
    } else {
        releaseRead.store( true, std::memory_order_release );
        reader.join();
        FAIL( "spilled reader did not reach synchronization boundary" );
    }
}

TEST_CASE( "CaptureStore sibling deletion does not retire an active store generation" )
{
    CaptureStore::Limits limits;
    limits.segmentTargetBytes = 8;
    limits.memoryBudgetBytes = 16;

    const auto rootPath = makeTestDir( "capturestore_active_sibling_delete" );
    const auto captureId = makeCaptureId();
    QString capturePath;
    {
        CaptureStore activeStore( captureId, rootPath, limits );
        capturePath = activeStore.capturePath();
        activeStore.appendUtf8(
            QByteArrayLiteral( "aaa\nbbb\nccc\nddd\neee\nfff\n" ) );
        const auto activeFiles = segmentFiles( capturePath );
        REQUIRE_FALSE( activeFiles.isEmpty() );

        CaptureStore siblingStore( captureId, rootPath, limits );
        siblingStore.deleteCaptureFiles();

        REQUIRE( segmentFiles( capturePath ) == activeFiles );
        const auto activeLines = activeStore.buildRawLines(
            0_lnum, 6_lcount, QTextCodec::codecForName( "UTF-8" ),
            QRegularExpression{} );
        REQUIRE( activeLines.decodeLines().size() == 6 );
    }

    CaptureStore restored( captureId, rootPath, limits );
    REQUIRE( restored.loadFromDisk() );
    REQUIRE( restored.lineCount() == 6_lcount );
}

TEST_CASE( "CaptureStore old maintenance does not retire sibling replacement segments" )
{
    CaptureStore::Limits limits;
    limits.segmentTargetBytes = 8;
    limits.memoryBudgetBytes = 16;

    const auto rootPath = makeTestDir( "capturestore_sibling_replacement" );
    const auto captureId = makeCaptureId();
    {
        CaptureStore writer( captureId, rootPath, limits );
        writer.appendUtf8( QByteArrayLiteral( "old-a\nold-b\nold-c\n" ) );
    }

    CaptureStore oldStore( captureId, rootPath, limits );
    oldStore.deleteCaptureFiles();

    CaptureStore replacementStore( captureId, rootPath, limits );
    replacementStore.appendUtf8(
        QByteArrayLiteral( "new-a\nnew-b\nnew-c\nnew-d\nnew-e\nnew-f\n" ) );
    const auto replacementFiles = segmentFiles( replacementStore.capturePath() );
    REQUIRE_FALSE( replacementFiles.isEmpty() );

    oldStore.clear();
    oldStore.deleteCaptureFiles();

    REQUIRE( segmentFiles( replacementStore.capturePath() ) == replacementFiles );
    const auto lines = replacementStore.buildRawLines(
        0_lnum, 6_lcount, QTextCodec::codecForName( "UTF-8" ), QRegularExpression{} );
    const auto decodedLines = lines.decodeLines();
    REQUIRE( decodedLines.size() == 6 );
    REQUIRE( decodedLines.front() == QStringLiteral( "new-a" ) );
    REQUIRE( decodedLines.back() == QStringLiteral( "new-f" ) );
}

TEST_CASE( "CaptureStore maintenance retires persisted segments before loading" )
{
    CaptureStore::Limits limits;
    limits.segmentTargetBytes = 8;
    limits.memoryBudgetBytes = 16;

    const auto rootPath = makeTestDir( "capturestore_unloaded_maintenance" );
    const auto captureId = makeCaptureId();
    QString capturePath;
    {
        CaptureStore writer( captureId, rootPath, limits );
        capturePath = writer.capturePath();
        writer.appendUtf8( QByteArrayLiteral( "aaa\nbbb\nccc\nddd\neee\nfff\n" ) );
    }
    REQUIRE_FALSE( segmentFiles( capturePath ).isEmpty() );

    CaptureStore store( captureId, rootPath, limits );
    SECTION( "clear" )
    {
        store.clear();
        REQUIRE( segmentFiles( capturePath ).isEmpty() );
        REQUIRE( QFileInfo::exists( capturePath ) );
    }
    SECTION( "delete capture files" )
    {
        store.deleteCaptureFiles();
        REQUIRE_FALSE( QFileInfo::exists( capturePath ) );
    }
}

TEST_CASE( "CaptureStore deletes inherited malformed capture files" )
{
    const auto rootPath = makeTestDir( "capturestore_delete_malformed_files" );
    const auto captureId = makeCaptureId();
    const auto capturePath = QDir( rootPath ).filePath( captureId );
    REQUIRE( QDir{}.mkpath( capturePath ) );

    for ( const auto& fileName :
          QStringList{ QStringLiteral( "segment_000000.log" ),
                       QStringLiteral( "segment_1.log" ),
                       QStringLiteral( "orphan.tmp" ) } ) {
        QFile file( QDir( capturePath ).filePath( fileName ) );
        REQUIRE( file.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
        REQUIRE( file.write( QByteArrayLiteral( "data\n" ) ) > 0 );
    }

    CaptureStore store( captureId, rootPath );
    store.deleteCaptureFiles();
    REQUIRE_FALSE( QFileInfo::exists( capturePath ) );
}

TEST_CASE( "CaptureStore persists a replacement generation after deletion" )
{
    const auto rootPath = makeTestDir( "capturestore_delete_reuse" );
    const auto captureId = makeCaptureId();
    {
        CaptureStore store( captureId, rootPath );
        store.deleteCaptureFiles();
        store.appendUtf8( QByteArrayLiteral( "replacement-tail" ) );
    }

    CaptureStore restored( captureId, rootPath );
    REQUIRE( restored.loadFromDisk() );
    REQUIRE( restored.lineCount() == 1_lcount );
    REQUIRE( restored.lineAt( 0_lnum, QTextCodec::codecForName( "UTF-8" ),
                              QRegularExpression{} )
             == QStringLiteral( "replacement-tail" ) );
}

TEST_CASE( "CaptureStore loads segment files in numeric id order" )
{
    const auto rootPath = makeTestDir( "capturestore_numeric_segment_order" );
    const auto captureId = makeCaptureId();
    const auto capturePath = QDir( rootPath ).filePath( captureId );
    REQUIRE( QDir{}.mkpath( capturePath ) );

    const auto writeSegment = [ &capturePath ]( const QString& fileName,
                                                const QByteArray& data ) {
        QFile file( QDir( capturePath ).filePath( fileName ) );
        if ( !file.open( QIODevice::WriteOnly | QIODevice::Truncate ) ) {
            return false;
        }
        return file.write( data ) == data.size();
    };
    REQUIRE( writeSegment( QStringLiteral( "segment_999999.log" ),
                           QByteArrayLiteral( "older\n" ) ) );
    REQUIRE( writeSegment( QStringLiteral( "segment_1000000.log" ),
                           QByteArrayLiteral( "newer\n" ) ) );

    CaptureStore store( captureId, rootPath );
    REQUIRE( store.loadFromDisk() );
    const auto rawLines = store.buildRawLines( 0_lnum, 2_lcount,
                                               QTextCodec::codecForName( "UTF-8" ),
                                               QRegularExpression{} );
    const auto decodedLines = rawLines.decodeLines();
    REQUIRE( decodedLines.size() == 2 );
    REQUIRE( decodedLines[ 0 ] == QStringLiteral( "older" ) );
    REQUIRE( decodedLines[ 1 ] == QStringLiteral( "newer" ) );
}

TEST_CASE( "CaptureStore ignores noncanonical duplicate segment ids" )
{
    const auto rootPath = makeTestDir( "capturestore_duplicate_segment_id" );
    const auto captureId = makeCaptureId();
    const auto capturePath = QDir( rootPath ).filePath( captureId );
    REQUIRE( QDir{}.mkpath( capturePath ) );

    const auto writeSegment = [ &capturePath ]( const QString& fileName,
                                                const QByteArray& data ) {
        QFile file( QDir( capturePath ).filePath( fileName ) );
        if ( !file.open( QIODevice::WriteOnly | QIODevice::Truncate ) ) {
            return false;
        }
        return file.write( data ) == data.size();
    };
    REQUIRE( writeSegment( QStringLiteral( "segment_000001.log" ),
                           QByteArrayLiteral( "canonical\n" ) ) );
    REQUIRE( writeSegment( QStringLiteral( "segment_1.log" ),
                           QByteArrayLiteral( "duplicate\n" ) ) );

    CaptureStore store( captureId, rootPath );
    REQUIRE( store.loadFromDisk() );
    REQUIRE( store.lineCount() == 1_lcount );
    REQUIRE( store.lineAt( 0_lnum, QTextCodec::codecForName( "UTF-8" ),
                           QRegularExpression{} )
             == QStringLiteral( "canonical" ) );
}

TEST_CASE( "CaptureStore rejects segment id exhaustion before increment" )
{
    const auto rootPath = makeTestDir( "capturestore_segment_id_exhaustion" );
    const auto captureId = makeCaptureId();
    const auto capturePath = QDir( rootPath ).filePath( captureId );
    REQUIRE( QDir{}.mkpath( capturePath ) );

    const auto nearLimitId = std::numeric_limits<qint64>::max() - 1;
    QFile segmentFile( QDir( capturePath ).filePath(
        QStringLiteral( "segment_%1.log" ).arg( nearLimitId ) ) );
    REQUIRE( segmentFile.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
    REQUIRE( segmentFile.write( QByteArrayLiteral( "existing\n" ) ) > 0 );
    segmentFile.close();

    CaptureStore store( captureId, rootPath );
    REQUIRE( store.loadFromDisk() );
    SECTION( "terminated input" )
    {
        REQUIRE_THROWS_AS( store.appendUtf8( QByteArrayLiteral( "new\n" ) ),
                           std::overflow_error );
    }
    SECTION( "unterminated input is rejected before destruction" )
    {
        bool rejected = false;
        try {
            store.appendUtf8( QByteArrayLiteral( "new-tail" ) );
        } catch ( const std::overflow_error& ) {
            rejected = true;
        }
        if ( !rejected ) {
            // Keep the RED path exception-safe: old code buffered the fragment and
            // would otherwise throw from the implicitly noexcept destructor.
            store.deleteCaptureFiles();
        }
        REQUIRE( rejected );
    }
}

TEST_CASE( "CaptureStore resets exhausted ids after deleting the final generation" )
{
    const auto rootPath = makeTestDir( "capturestore_reset_exhausted_ids" );
    const auto captureId = makeCaptureId();
    const auto capturePath = QDir( rootPath ).filePath( captureId );
    REQUIRE( QDir{}.mkpath( capturePath ) );

    const auto nearLimitId = std::numeric_limits<qint64>::max() - 1;
    QFile segmentFile( QDir( capturePath ).filePath(
        QStringLiteral( "segment_%1.log" ).arg( nearLimitId ) ) );
    REQUIRE( segmentFile.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
    REQUIRE( segmentFile.write( QByteArrayLiteral( "existing\n" ) ) > 0 );
    segmentFile.close();

    {
        CaptureStore exhaustedStore( captureId, rootPath );
        REQUIRE( exhaustedStore.loadFromDisk() );
        exhaustedStore.deleteCaptureFiles();
        REQUIRE_FALSE( QFileInfo::exists( capturePath ) );
    }

    CaptureStore replacementStore( captureId, rootPath );
    const auto appended = replacementStore.appendUtf8(
        QByteArrayLiteral( "replacement\n" ) );
    REQUIRE( appended.lineCount == 1_lcount );
    REQUIRE( replacementStore.lineCount() == 1_lcount );
}

TEST_CASE( "CaptureStore rejects appends that exceed remaining segment ids atomically" )
{
    CaptureStore::Limits limits;
    limits.segmentTargetBytes = 8;
    limits.memoryBudgetBytes = 16;

    const auto rootPath = makeTestDir( "capturestore_remaining_segment_ids" );
    const auto captureId = makeCaptureId();
    const auto capturePath = QDir( rootPath ).filePath( captureId );
    REQUIRE( QDir{}.mkpath( capturePath ) );

    const auto penultimateId = std::numeric_limits<qint64>::max() - 2;
    QFile segmentFile( QDir( capturePath ).filePath(
        QStringLiteral( "segment_%1.log" ).arg( penultimateId ) ) );
    REQUIRE( segmentFile.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
    REQUIRE( segmentFile.write( QByteArrayLiteral( "existing\n" ) ) > 0 );
    segmentFile.close();

    CaptureStore store( captureId, rootPath, limits );
    REQUIRE( store.loadFromDisk() );
    const auto originalFiles = segmentFiles( capturePath );

    QByteArray input;
    SECTION( "terminated line plus tail" )
    {
        input = QByteArrayLiteral( "1234567\nTAIL" );
    }
    SECTION( "two terminated lines" )
    {
        input = QByteArrayLiteral( "1234567\n7654321\n" );
    }

    bool rejected = false;
    try {
        store.appendUtf8( input );
    } catch ( const std::overflow_error& ) {
        rejected = true;
    }
    if ( !rejected ) {
        // Keep the RED path exception-safe when an unterminated tail was accepted.
        store.deleteCaptureFiles();
    }

    REQUIRE( rejected );
    REQUIRE( store.lineCount() == 1_lcount );
    REQUIRE( segmentFiles( capturePath ) == originalFiles );
}

TEST_CASE( "CaptureStore retains a partial line when changed limits exhaust ids" )
{
    CaptureStore::Limits limits;
    limits.segmentTargetBytes = 1024;
    limits.memoryBudgetBytes = 4096;

    const auto rootPath = makeTestDir( "capturestore_finish_id_exhaustion" );
    const auto captureId = makeCaptureId();
    const auto capturePath = QDir( rootPath ).filePath( captureId );
    REQUIRE( QDir{}.mkpath( capturePath ) );

    const auto penultimateId = std::numeric_limits<qint64>::max() - 2;
    QFile segmentFile( QDir( capturePath ).filePath(
        QStringLiteral( "segment_%1.log" ).arg( penultimateId ) ) );
    REQUIRE( segmentFile.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
    REQUIRE( segmentFile.write( QByteArrayLiteral( "existing\n" ) ) > 0 );
    segmentFile.close();

    CaptureStore store( captureId, rootPath, limits );
    REQUIRE( store.loadFromDisk() );
    store.appendUtf8( QByteArrayLiteral( "a\n" ) );
    store.appendUtf8( QByteArrayLiteral( "tail" ) );

    auto closedLimits = limits;
    closedLimits.segmentTargetBytes = 1;
    store.setLimits( closedLimits );
    REQUIRE_THROWS_AS( store.finishInput(), std::overflow_error );
    REQUIRE( store.lineCount() == 2_lcount );

    store.setLimits( limits );
    const auto finished = store.finishInput();
    REQUIRE( finished.lineCount == 1_lcount );
    REQUIRE( store.lineCount() == 3_lcount );
    REQUIRE( store.lineAt( 2_lnum, QTextCodec::codecForName( "UTF-8" ),
                           QRegularExpression{} )
             == QStringLiteral( "tail" ) );
}

TEST_CASE( "CaptureStore persists committed segments when destructor finalization fails" )
{
    CaptureStore::Limits limits;
    limits.segmentTargetBytes = 1024;
    limits.memoryBudgetBytes = 4096;

    const auto rootPath = makeTestDir( "capturestore_destructor_id_exhaustion" );
    const auto captureId = makeCaptureId();
    const auto capturePath = QDir( rootPath ).filePath( captureId );
    REQUIRE( QDir{}.mkpath( capturePath ) );

    const auto penultimateId = std::numeric_limits<qint64>::max() - 2;
    QFile segmentFile( QDir( capturePath ).filePath(
        QStringLiteral( "segment_%1.log" ).arg( penultimateId ) ) );
    REQUIRE( segmentFile.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
    REQUIRE( segmentFile.write( QByteArrayLiteral( "existing\n" ) ) > 0 );
    segmentFile.close();

    {
        CaptureStore store( captureId, rootPath, limits );
        REQUIRE( store.loadFromDisk() );
        store.appendUtf8( QByteArrayLiteral( "committed\n" ) );
        store.appendUtf8( QByteArrayLiteral( "tail" ) );
        auto closedLimits = limits;
        closedLimits.segmentTargetBytes = 1;
        store.setLimits( closedLimits );
    }

    CaptureStore restored( captureId, rootPath, limits );
    REQUIRE( restored.loadFromDisk() );
    REQUIRE( restored.lineCount() == 2_lcount );
    REQUIRE( restored.lineAt( 1_lnum, QTextCodec::codecForName( "UTF-8" ),
                              QRegularExpression{} )
             == QStringLiteral( "committed" ) );
}

TEST_CASE( "CaptureStore buildRawLines CRLF fast path strips carriage returns" )
{
    const auto rootPath = makeTestDir( "capturestore_crlf_fast_path" );
    CaptureStore store( makeCaptureId(), rootPath );
    auto* codec = QTextCodec::codecForName( "UTF-8" );

    // CRLF line endings stored in capture — the fast path scans for \n.
    // The \r before each \n should still be present in raw buffer,
    // but decodeLines should strip them.
    store.appendUtf8( QByteArrayLiteral( "row1\r\nrow2\r\nrow3\r\n" ) );
    REQUIRE( store.lineCount().get() == 3 );

    const auto rawLines = store.buildRawLines( 0_lnum, LinesCount( 3 ), codec, QRegularExpression{} );
    REQUIRE( rawLines.endOfLines.size() == 3 );

    const auto decoded = rawLines.decodeLines();
    REQUIRE( decoded.size() == 3 );
    REQUIRE( decoded[ 0 ] == QStringLiteral( "row1" ) );
    REQUIRE( decoded[ 1 ] == QStringLiteral( "row2" ) );
    REQUIRE( decoded[ 2 ] == QStringLiteral( "row3" ) );
}

TEST_CASE( "CaptureStore clear flushes and resets output" )
{
    const auto rootPath = makeTestDir( "capturestore_clear_flush" );
    const auto outputPath = QDir( rootPath ).filePath( QStringLiteral( "clear.log" ) );

    CaptureStore store( makeCaptureId(), rootPath );
    REQUIRE( store.bindOutputFile( outputPath ) );

    store.appendUtf8( QByteArrayLiteral( "before-clear\n" ) );
    store.clear();

    // After clear, the output file should have been truncated
    QFile output( outputPath );
    REQUIRE( output.open( QIODevice::ReadOnly ) );
    REQUIRE( output.size() == 0 );
}

TEST_CASE( "CaptureStore clear resets lastTrimResult" )
{
    CaptureStore::Limits limits;
    limits.segmentTargetBytes = 16;
    limits.memoryBudgetBytes = 4096;
    limits.rollingMaxFileSize = 16;
    limits.rollingBackupCount = 2;

    const auto rootPath = makeTestDir( "capturestore_clear_trim_result" );
    CaptureStore store( makeCaptureId(), rootPath, limits );

    // Append enough data to exceed the window and trigger trimming
    for ( int i = 0; i < 20; ++i ) {
        store.appendUtf8( QStringLiteral( "line-%1\n" ).arg( i, 3, 10, QLatin1Char( '0' ) ).toUtf8() );
    }

    // After auto-trim during append, lastTrimResult should be non-zero
    const auto trimResult = store.lastTrimResult();
    CHECK( trimResult.trimmedLines > 0_lcount );

    // Clear the store
    store.clear();

    // After clear, lastTrimResult should be reset to zero
    const auto afterClear = store.lastTrimResult();
    CHECK( afterClear.trimmedLines == 0_lcount );
    CHECK( afterClear.trimmedBytes == 0 );
}

TEST_CASE( "CaptureStore trims a closed sole segment at the exact boundary" )
{
    CaptureStore::Limits limits;
    limits.segmentTargetBytes = 8;
    limits.memoryBudgetBytes = 4096;
    limits.rollingMaxFileSize = 4;
    limits.rollingBackupCount = 1;
    limits.maxTotalLines = 1;

    const auto rootPath = makeTestDir( "capturestore_trim_closed_tail" );
    CaptureStore store( makeCaptureId(), rootPath, limits );
    store.appendUtf8( QByteArrayLiteral( "aaa\nbbb\n" ) );

    REQUIRE( store.lineCount() == 0_lcount );
    const auto trimResult = store.lastTrimResult();
    REQUIRE( trimResult.trimmedLines == 2_lcount );
    REQUIRE( trimResult.trimmedBytes == 8 );
}

TEST_CASE( "CaptureStore trimToLimits removes oldest segments and updates line count" )
{
    CaptureStore::Limits limits;
    limits.segmentTargetBytes = 16; // Very small segments
    limits.memoryBudgetBytes = 4096;
    limits.rollingMaxFileSize = 16;
    limits.rollingBackupCount = 3; // Window = 16 * 3 = 48

    const auto rootPath = makeTestDir( "capturestore_trim_limits" );
    CaptureStore store( makeCaptureId(), rootPath, limits );

    // Each line is ~5 bytes + newline = 6 bytes. With segmentTargetBytes=16,
    // each segment holds ~2-3 lines. With maxTotalBytes=48, we can fit ~3 segments.
    for ( int i = 0; i < 20; ++i ) {
        store.appendUtf8( QStringLiteral( "ln-%1\n" ).arg( i, 3, 10, QLatin1Char( '0' ) ).toUtf8() );
    }

    // After trimming, total file size should be within the limit
    const auto stats = store.stats();
    CHECK( stats.fileSize <= limits.rollingMaxFileSize * limits.rollingBackupCount );

    // Lines should still be addressable from the surviving segments
    const auto lineCount = store.lineCount();
    CHECK( lineCount.get() > 0 );

    // The last line should still be "ln-019"
    auto* codec = QTextCodec::codecForName( "UTF-8" );
    REQUIRE( store.lineAt( LineNumber( lineCount.get() - 1 ), codec, QRegularExpression{} )
             == QStringLiteral( "ln-019" ) );
}

TEST_CASE( "CaptureStore trimToLimits preserves surviving data in bound output file" )
{
    CaptureStore::Limits limits;
    limits.segmentTargetBytes = 16;
    limits.memoryBudgetBytes = 4096;
    limits.rollingMaxFileSize = 16;
    limits.rollingBackupCount = 3;

    const auto rootPath = makeTestDir( "capturestore_trim_output" );
    const auto outputPath = QDir( rootPath ).filePath( QStringLiteral( "trimmed.log" ) );

    CaptureStore store( makeCaptureId(), rootPath, limits );
    REQUIRE( store.bindOutputFile( outputPath ) );

    for ( int i = 0; i < 20; ++i ) {
        store.appendUtf8( QStringLiteral( "line-%1\n" ).arg( i ).toUtf8() );
    }
    store.flush();

    // Read from current file and all backup files
    QByteArray allContent;
    {
        QFile f( outputPath );
        if ( f.open( QIODevice::ReadOnly ) ) {
            allContent.append( f.readAll() );
        }
    }
    for ( int i = 0; i < 10; ++i ) {
        const auto bp = outputPath + QStringLiteral( ".%1" ).arg( i );
        QFile f( bp );
        if ( f.open( QIODevice::ReadOnly ) ) {
            allContent.append( f.readAll() );
        }
    }

    INFO( "All content: " << allContent.toStdString() );

    // The last line should be "line-19"
    REQUIRE( allContent.contains( QByteArrayLiteral( "line-19" ) ) );

    // The in-memory CaptureStore should be within limits
    CHECK( store.stats().fileSize <= limits.rollingMaxFileSize * limits.rollingBackupCount );
}

TEST_CASE( "CaptureStore trimToLimits returns correct trim result" )
{
    CaptureStore::Limits limits;
    limits.segmentTargetBytes = 16;
    limits.memoryBudgetBytes = 4096;
    limits.rollingMaxFileSize = 16;
    limits.rollingBackupCount = 3;

    const auto rootPath = makeTestDir( "capturestore_trim_result" );
    CaptureStore store( makeCaptureId(), rootPath, limits );

    for ( int i = 0; i < 20; ++i ) {
        store.appendUtf8( QStringLiteral( "x\n" ).toUtf8() );
    }

    // Now trigger a trim manually and check the result
    // First, set a very small limit and append more data
    limits.rollingMaxFileSize = 16;
    limits.rollingBackupCount = 2;
    store.setLimits( limits );
    store.appendUtf8( QByteArrayLiteral( "trigger\n" ) );

    // At least some lines should have been trimmed
    CHECK( store.lineCount().get() > 0 );

    // The remaining data should be within the new limit
    CHECK( store.stats().fileSize <= 32 );
}

TEST_CASE( "CaptureStore cumulative line counts are correct after front-trim" )
{
    CaptureStore::Limits limits;
    limits.segmentTargetBytes = 16;
    limits.memoryBudgetBytes = 4096;
    limits.rollingMaxFileSize = 16;
    limits.rollingBackupCount = 3;

    const auto rootPath = makeTestDir( "capturestore_trim_cumulative" );
    CaptureStore store( makeCaptureId(), rootPath, limits );
    auto* codec = QTextCodec::codecForName( "UTF-8" );

    for ( int i = 0; i < 20; ++i ) {
        store.appendUtf8( QStringLiteral( "L%1\n" ).arg( i, 3, 10, QLatin1Char( '0' ) ).toUtf8() );
    }

    // Verify every surviving line is addressable and contains the expected content.
    // After trim, line 0 is the first surviving line (not the original line 0).
    const auto lineCount = store.lineCount();
    for ( LinesCount::UnderlyingType i = 0; i < lineCount.get(); ++i ) {
        INFO( "Checking surviving line " << i );
        const auto line = store.lineAt( LineNumber( i ), codec, QRegularExpression{} );
        REQUIRE_FALSE( line.isEmpty() );
        // Each line should match the pattern "LXXX"
        REQUIRE( line.startsWith( QLatin1Char( 'L' ) ) );
    }

    // The first surviving line should NOT be "L000" (it was trimmed)
    const auto firstLine = store.lineAt( 0_lnum, codec, QRegularExpression{} );
    REQUIRE( firstLine != QStringLiteral( "L000" ) );

    // The last surviving line should be "L019"
    const auto lastLine = store.lineAt( LineNumber( lineCount.get() - 1 ), codec, QRegularExpression{} );
    REQUIRE( lastLine == QStringLiteral( "L019" ) );
}

TEST_CASE( "CaptureStore buildRawLines works correctly after front-trim" )
{
    CaptureStore::Limits limits;
    limits.segmentTargetBytes = 16;
    limits.memoryBudgetBytes = 4096;
    limits.rollingMaxFileSize = 16;
    limits.rollingBackupCount = 3;

    const auto rootPath = makeTestDir( "capturestore_trim_rawlines" );
    CaptureStore store( makeCaptureId(), rootPath, limits );
    auto* codec = QTextCodec::codecForName( "UTF-8" );

    for ( int i = 0; i < 20; ++i ) {
        store.appendUtf8( QStringLiteral( "row-%1\n" ).arg( i ).toUtf8() );
    }

    const auto lineCount = store.lineCount();
    const auto rawLines = store.buildRawLines( 0_lnum, lineCount, codec, QRegularExpression{} );

    REQUIRE( rawLines.endOfLines.size() == static_cast<size_t>( lineCount.get() ) );

    const auto decoded = rawLines.decodeLines();
    REQUIRE( decoded.size() == static_cast<size_t>( lineCount.get() ) );

    // Verify first and last surviving lines
    REQUIRE( decoded.front().startsWith( QLatin1String( "row-" ) ) );
    REQUIRE( decoded.back() == QStringLiteral( "row-19" ) );

    // Verify no duplicate or out-of-order lines
    for ( size_t i = 1; i < decoded.size(); ++i ) {
        INFO( "Comparing line " << i );
        REQUIRE( decoded[ i ] != decoded[ i - 1 ] );
    }
}

// === RollingFileManager Tests ===

TEST_CASE( "RollingFileManager writes to current file" )
{
    const auto rootPath = makeTestDir( "rolling_basic" );
    const auto filePath = QDir( rootPath ).filePath( QStringLiteral( "output.log" ) );

    RollingFileManager manager( filePath, 1024, 3 );
    REQUIRE( manager.open() );
    REQUIRE( manager.isValid() );

    manager.write( QByteArrayLiteral( "hello world\n" ) );
    manager.flush();

    REQUIRE( readUtf8File( filePath ) == QStringLiteral( "hello world\n" ) );
    REQUIRE( manager.currentFileSize() == 12 );

    manager.deleteAll();
}

TEST_CASE( "RollingFileManager rotates when file reaches maxFileSize" )
{
    const auto rootPath = makeTestDir( "rolling_rotate" );
    const auto filePath = QDir( rootPath ).filePath( QStringLiteral( "output.log" ) );

    RollingFileManager manager( filePath, 32, 3 );
    REQUIRE( manager.open() );

    // Write enough data to exceed 32 bytes
    const QByteArray data( 20, 'A' );
    manager.write( data + QByteArrayLiteral( "\n" ) ); // 21 bytes
    manager.write( data + QByteArrayLiteral( "\n" ) ); // 42 bytes total → triggers rotation

    // After rotation: backup[0] should exist, current file should have remaining data
    const auto backups = manager.backupFiles();
    INFO( "Backup files: " << backups.join( QLatin1String( ", " ) ).toStdString() );
    REQUIRE( backups.size() >= 1 );

    // Current file should exist and have data
    REQUIRE( QFile::exists( filePath ) );

    manager.deleteAll();
}

TEST_CASE( "RollingFileManager maintains backupCount limit" )
{
    const auto rootPath = makeTestDir( "rolling_backup_limit" );
    const auto filePath = QDir( rootPath ).filePath( QStringLiteral( "output.log" ) );

    constexpr int backupCount = 2;
    RollingFileManager manager( filePath, 16, backupCount );
    REQUIRE( manager.open() );

    // Write enough to trigger multiple rotations
    for ( int i = 0; i < 10; ++i ) {
        manager.write( QStringLiteral( "line-%1\n" ).arg( i ).toUtf8() );
    }

    // Should have at most backupCount backup files
    const auto backups = manager.backupFiles();
    INFO( "Backup count: " << backups.size() );
    REQUIRE( backups.size() <= backupCount + 1 ); // +1 for current file

    manager.deleteAll();
}

TEST_CASE( "RollingFileManager deleteAll removes all files" )
{
    const auto rootPath = makeTestDir( "rolling_delete_all" );
    const auto filePath = QDir( rootPath ).filePath( QStringLiteral( "output.log" ) );

    RollingFileManager manager( filePath, 16, 3 );
    REQUIRE( manager.open() );

    for ( int i = 0; i < 10; ++i ) {
        manager.write( QStringLiteral( "data-%1\n" ).arg( i ).toUtf8() );
    }

    manager.deleteAll();

    REQUIRE_FALSE( QFile::exists( filePath ) );
    for ( int i = 0; i < 5; ++i ) {
        REQUIRE_FALSE( QFile::exists( filePath + QStringLiteral( ".%1" ).arg( i ) ) );
    }
}

TEST_CASE( "RollingFileManager no data loss within window" )
{
    const auto rootPath = makeTestDir( "rolling_no_loss" );
    const auto filePath = QDir( rootPath ).filePath( QStringLiteral( "output.log" ) );

    // Window = maxFileSize(32) * backupCount(2) = 64 bytes.
    // Write 7 lines (56 bytes) — all fit within the window.
    constexpr int lineCount = 7;
    RollingFileManager manager( filePath, 32, 2 );
    REQUIRE( manager.open() );

    for ( int i = 0; i < lineCount; ++i ) {
        const auto line = QStringLiteral( "line-%1\n" ).arg( i, 3, 10, QLatin1Char( '0' ) );
        manager.write( line.toUtf8() );
    }

    manager.flush();

    // Collect all data from current file and backups
    QByteArray collected;
    {
        QFile f( filePath );
        if ( f.open( QIODevice::ReadOnly ) ) {
            collected.append( f.readAll() );
        }
    }
    const auto backups = manager.backupFiles();
    for ( const auto& path : backups ) {
        QFile f( path );
        if ( f.open( QIODevice::ReadOnly ) ) {
            collected.append( f.readAll() );
        }
    }

    INFO( "Collected " << collected.size() << " bytes from " << ( backups.size() + 1 ) << " files" );

    // All data within the window should be present
    for ( int i = 0; i < lineCount; ++i ) {
        const auto line = QStringLiteral( "line-%1" ).arg( i, 3, 10, QLatin1Char( '0' ) );
        INFO( "Checking line " << i );
        REQUIRE( collected.contains( line.toUtf8() ) );
    }

    manager.deleteAll();
}

TEST_CASE( "RollingFileManager deletes data outside window" )
{
    const auto rootPath = makeTestDir( "rolling_window_delete" );
    const auto filePath = QDir( rootPath ).filePath( QStringLiteral( "output.log" ) );

    // Window = 32 * 2 = 64 bytes. Write 10 lines (80 bytes).
    // The oldest lines (outside the window) should be deleted.
    RollingFileManager manager( filePath, 32, 2 );
    REQUIRE( manager.open() );

    for ( int i = 0; i < 10; ++i ) {
        manager.write( QStringLiteral( "line-%1\n" ).arg( i, 3, 10, QLatin1Char( '0' ) ).toUtf8() );
    }
    manager.flush();

    // Collect all data
    QByteArray collected;
    {
        QFile f( filePath );
        if ( f.open( QIODevice::ReadOnly ) ) {
            collected.append( f.readAll() );
        }
    }
    const auto backups = manager.backupFiles();
    for ( const auto& path : backups ) {
        QFile f( path );
        if ( f.open( QIODevice::ReadOnly ) ) {
            collected.append( f.readAll() );
        }
    }

    // Lines 000-001 (16 bytes) should be outside the window and deleted
    REQUIRE_FALSE( collected.contains( "line-000" ) );

    // Lines 002-009 should be within the window
    REQUIRE( collected.contains( "line-009" ) );

    manager.deleteAll();
}

TEST_CASE( "RollingFileManager zero backupCount keeps all rotated files" )
{
    const auto rootPath = makeTestDir( "rolling_zero_backup" );
    const auto filePath = QDir( rootPath ).filePath( QStringLiteral( "output.log" ) );

    // maxFileSize = 16 bytes, backupCount = 0 (keep all rotated files)
    RollingFileManager manager( filePath, 16, 0 );
    REQUIRE( manager.open() );

    // Write data in small chunks to trigger multiple rotations.
    // Each "line-NN\n" is 8 bytes; maxFileSize = 16 → rotation every 2 lines.
    for ( int i = 0; i < 20; ++i ) {
        const auto line = QStringLiteral( "line-%1\n" )
                              .arg( i, 2, 10, QLatin1Char( '0' ) )
                              .toUtf8();
        manager.write( line );
    }

    // Backup files should be retained (no cleanup when backupCount = 0)
    const auto backups = manager.backupFiles();
    REQUIRE( backups.size() >= 2 );

    // The current file should also exist
    REQUIRE( QFile::exists( filePath ) );

    manager.deleteAll();
}

TEST_CASE( "RollingFileManager reopen after rotation preserves data" )
{
    const auto rootPath = makeTestDir( "rolling_reopen" );
    const auto filePath = QDir( rootPath ).filePath( QStringLiteral( "output.log" ) );

    {
        RollingFileManager manager( filePath, 24, 2 );
        REQUIRE( manager.open() );
        for ( int i = 0; i < 5; ++i ) {
            manager.write( QStringLiteral( "line-%1\n" ).arg( i ).toUtf8() );
        }
    }

    // Verify files exist after close
    REQUIRE( QFile::exists( filePath ) );

    // Reopen and verify we can continue writing
    {
        RollingFileManager manager( filePath, 24, 2 );
        REQUIRE( manager.open() );
        manager.write( QByteArrayLiteral( "new-data\n" ) );
        REQUIRE( manager.currentFileSize() > 0 );
    }

    // Cleanup
    RollingFileManager cleanup( filePath, 24, 2 );
    cleanup.deleteAll();
}

TEST_CASE( "RollingFileManager writes every complete line across rotations", "[rolling]" )
{
    const auto dir = makeTestDir( "rolling_no_dataloss" );
    const auto filePath = QDir( dir ).filePath( "live.log" );

    // maxFileSize=30; a 3-line batch (33 bytes) must split. The last newline
    // that fits lands strictly before the capacity boundary, which previously
    // caused the trailing complete line to be dropped silently.
    RollingFileManager manager( filePath, 30, 5 );
    REQUIRE( manager.open( true ) );

    const QByteArray data = QByteArrayLiteral( "0123456789\n0123456789\n0123456789\n" );
    const auto written = manager.write( data );
    manager.flush();

    // Every input byte must be accounted for (old code returned 22, dropping 11).
    REQUIRE( written == data.size() );

    // Reassemble backups (oldest first) + current and confirm no line was lost.
    QByteArray all;
    for ( const auto& path : manager.backupFiles() ) {
        QFile f( path );
        if ( f.open( QIODevice::ReadOnly ) ) {
            all.append( f.readAll() );
        }
    }
    {
        QFile f( filePath );
        if ( f.open( QIODevice::ReadOnly ) ) {
            all.append( f.readAll() );
        }
    }
    REQUIRE( all.count( '\n' ) == data.count( '\n' ) );
    REQUIRE( all.contains( QByteArrayLiteral( "0123456789\n0123456789\n0123456789\n" ) ) );

    RollingFileManager( filePath, 30, 5 ).deleteAll();
}

TEST_CASE( "RollingFileManager reports rotation via rotated()", "[rolling]" )
{
    const auto dir = makeTestDir( "rolling_rotated_flag" );
    const auto filePath = QDir( dir ).filePath( "live.log" );

    RollingFileManager manager( filePath, 16, 3 );
    REQUIRE( manager.open( true ) );

    // A write that fits entirely must not report a rotation.
    manager.write( QByteArrayLiteral( "short\n" ) );
    REQUIRE_FALSE( manager.rotated() );

    // A write that overflows the file must rotate and report it.
    manager.write( QByteArrayLiteral( "0123456789ABCDEF0123456789ABCDEF\n" ) );
    REQUIRE( manager.rotated() );

    RollingFileManager( filePath, 16, 3 ).deleteAll();
}

TEST_CASE( "RollingFileManager clamps absurd backup counts", "[rolling]" )
{
    const auto dir = makeTestDir( "rolling_clamp" );
    const auto filePath = QDir( dir ).filePath( "live.log" );

    // A backup count within 100 of INT_MAX previously made cleanupOldBackups()
    // compute `backupCount_ + 100` with signed overflow (UB) and skip cleanup.
    RollingFileManager manager( filePath, 16, 2147483600 );
    REQUIRE( manager.backupCount() <= 100000 );
    REQUIRE( manager.backupCount() >= 0 );

    REQUIRE( manager.open( true ) );
    for ( int i = 0; i < 4; ++i ) {
        manager.write( QByteArrayLiteral( "0123456789ABCDEF\n" ) ); // 17 bytes > 16 -> rotate
    }
    manager.flush();
    // Cleanup must have run without overflow; only a bounded number of backups
    // survive (clamped count + the current file).
    REQUIRE( manager.backupFiles().size() <= manager.backupCount() + 1 );

    RollingFileManager( filePath, 16, 2147483600 ).deleteAll();
}

TEST_CASE( "CaptureStore AppendResult firstLine reflects post-trim position", "[capturestore]" )
{
    CaptureStore::Limits limits;
    limits.segmentTargetBytes = 8;
    limits.memoryBudgetBytes = 4096;
    limits.rollingMaxFileSize = 16;
    limits.rollingBackupCount = 1; // window = 16 bytes

    const auto rootPath = makeTestDir( "capture_firstline_trim" );
    CaptureStore store( makeCaptureId(), rootPath, limits );

    // Pre-fill well past the window so the store is already trimming (oldest
    // segments removed down to the window).
    store.appendUtf8( QByteArrayLiteral( "AAA\nAAA\nAAA\nAAA\nAAA\nAAA\nAAA\nAAA\nAAA\nAAA\n" ) );
    const auto preTotal = store.lineCount().get();

    // Small append that forces another trim of OLD data only; its single line
    // survives at the tail. Its AppendResult.firstLine must address the
    // post-trim tail position, not the stale pre-trim total.
    const auto result = store.appendUtf8( QByteArrayLiteral( "BBB\n" ) );

    const auto total = store.lineCount().get();
    // Guard: trimming must have removed older lines during this append.
    REQUIRE( total < preTotal + result.lineCount.get() );

    const auto expectedFirst = static_cast<LineNumber::UnderlyingType>(
        total > result.lineCount.get() ? total - result.lineCount.get() : 0 );
    REQUIRE( result.firstLine.get() == expectedFirst );
}
