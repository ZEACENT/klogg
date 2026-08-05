#include "capturestore.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QLockFile>
#include <QSaveFile>
#include <QUuid>

#if defined( Q_OS_WIN )
#include <windows.h>
#else
#include <sys/types.h>
#endif

#include "log.h"
#include "readablesize.h"
#include "securecapturedirectory.h"

namespace {
QString makeSegmentFileName( qint64 segmentId )
{
    return QString( "segment_%1.log" ).arg( segmentId, 6, 10,
                                             QLatin1Char( '0' ) );
}

std::optional<qint64> segmentIdFromFileName( const QString& fileName )
{
    bool isValid = false;
    const auto numericId
        = QFileInfo( fileName ).baseName().mid( QString( "segment_" ).size() );
    const auto segmentId = numericId.toLongLong( &isValid );
    if ( !isValid || segmentId < 0
         || segmentId == std::numeric_limits<qint64>::max()
         || fileName != makeSegmentFileName( segmentId ) ) {
        return std::nullopt;
    }
    return segmentId;
}

QString decodeUtf8Line( const QByteArray& utf8Line, QTextCodec* codec,
                        const QRegularExpression& prefilterPattern )
{
    auto line = codec ? codec->toUnicode( utf8Line ) : QString::fromUtf8( utf8Line );
    if ( !prefilterPattern.pattern().isEmpty() ) {
        line.remove( prefilterPattern );
    }
    return line;
}

void reserveSegmentMemory( QByteArray& data, qint64 targetBytes, qint64 budgetBytes )
{
    const auto reserveTarget = std::min( targetBytes, budgetBytes );
    if ( reserveTarget <= 0 ) {
        return;
    }

    const auto cappedTarget = std::min<qint64>( reserveTarget, std::numeric_limits<int>::max() );
    data.reserve( type_safe::narrow_cast<int>( cappedTarget ) );
}

// Appended lines always live at the tail of the store. Their first line number
// must be derived from the CURRENT total (after commit + trim) so it stays
// correct when trimming shifts every absolute line number down. Using the
// pre-commit total leaves firstLine stale (too large by the trimmed count).
LineNumber tailFirstLine( qint64 totalLines, LinesCount lineCount )
{
    const auto count = static_cast<qint64>( lineCount.get() );
    auto firstLine = LineNumber{ static_cast<LineNumber::UnderlyingType>(
        totalLines >= count ? totalLines - count : 0 ) };
    return firstLine;
}

// Clamp untrusted limit inputs (session-restore JSON, hand-edited .ini) so the
// rolling window math (rollingMaxFileSize * rollingBackupCount) and the backup
// cleanup loop stay within their integer ranges.
CaptureStore::Limits sanitizeLimits( CaptureStore::Limits limits )
{
    constexpr int kMaxRollingBackupCount = 100000;
    limits.rollingBackupCount
        = std::clamp( limits.rollingBackupCount, 0, kMaxRollingBackupCount );
    return limits;
}

QString canonicalCaptureRoot( const QString& rootPath )
{
    const QDir rootDirectory( rootPath );
    const auto canonicalRoot = rootDirectory.canonicalPath();
    return QDir::cleanPath( canonicalRoot.isEmpty() ? rootDirectory.absolutePath() : canonicalRoot );
}

bool isSafeCaptureId( const QString& captureId )
{
    if ( captureId.isEmpty() || captureId == QStringLiteral( "." )
         || captureId == QStringLiteral( ".." )
         || QDir::isAbsolutePath( captureId )
         || captureId.endsWith( QLatin1Char( '.' ) )
         || captureId.endsWith( QLatin1Char( ' ' ) ) ) {
        return false;
    }

    for ( const auto character : captureId ) {
        if ( character.unicode() < 0x20
             || QStringLiteral( "/\\:*?\"<>|" ).contains( character ) ) {
            return false;
        }
    }

    const auto deviceName = captureId.section( QLatin1Char( '.' ), 0, 0 ).toUpper();
    static const QSet<QString> reservedDeviceNames{
        QStringLiteral( "CON" ), QStringLiteral( "PRN" ), QStringLiteral( "AUX" ),
        QStringLiteral( "NUL" ), QStringLiteral( "COM1" ), QStringLiteral( "COM2" ),
        QStringLiteral( "COM3" ), QStringLiteral( "COM4" ), QStringLiteral( "COM5" ),
        QStringLiteral( "COM6" ), QStringLiteral( "COM7" ), QStringLiteral( "COM8" ),
        QStringLiteral( "COM9" ), QStringLiteral( "LPT1" ), QStringLiteral( "LPT2" ),
        QStringLiteral( "LPT3" ), QStringLiteral( "LPT4" ), QStringLiteral( "LPT5" ),
        QStringLiteral( "LPT6" ), QStringLiteral( "LPT7" ), QStringLiteral( "LPT8" ),
        QStringLiteral( "LPT9" ),
    };
    return !reservedDeviceNames.contains( deviceName );
}

QString capturePathForId( const QString& rootPath, const QString& captureId )
{
    if ( !isSafeCaptureId( captureId ) ) {
        throw std::invalid_argument( "Capture ID must be a single relative path component" );
    }

    const auto canonicalRoot = canonicalCaptureRoot( rootPath );
    const auto capturePath = QDir::cleanPath( QDir( canonicalRoot ).absoluteFilePath( captureId ) );
    if ( QFileInfo( capturePath ).dir().absolutePath() != canonicalRoot
         || QFileInfo( capturePath ).isSymLink() ) {
        throw std::invalid_argument( "Capture ID escapes the configured capture root" );
    }
    return capturePath;
}

QString lexicalCapturePath( const QString& path )
{
    return QDir::cleanPath( QFileInfo( path ).absoluteFilePath() );
}

QString captureCoordinationRoot()
{
    const auto root = QDir( QDir::tempPath() ).filePath(
        QStringLiteral( "klogg_capture_coordination" ) );
    if ( !QDir{}.mkpath( root ) ) {
        return {};
    }
    const QFileInfo rootInfo( root );
    if ( rootInfo.isSymLink() || !rootInfo.isDir() ) {
        return {};
    }
#if !defined( Q_OS_WIN )
    if ( !QFile::setPermissions( root, QFileDevice::ReadOwner
                                          | QFileDevice::WriteOwner
                                          | QFileDevice::ExeOwner ) ) {
        return {};
    }
#endif
    return QDir::cleanPath( rootInfo.absoluteFilePath() );
}

QString captureCoordinationStem( const QString& directoryIdentity )
{
    const auto coordinationRoot = captureCoordinationRoot();
    if ( coordinationRoot.isEmpty() ) {
        return {};
    }
    const auto digest = QCryptographicHash::hash( directoryIdentity.toUtf8(),
                                                   QCryptographicHash::Sha256 )
                            .toHex();
    return QDir( coordinationRoot ).filePath(
        QStringLiteral( "capture_%1" ).arg( QString::fromLatin1( digest ) ) );
}

QString directChildName( const QString& filePath )
{
    auto fileName = QFileInfo( filePath ).fileName();
    if ( fileName.isEmpty() || fileName == QStringLiteral( "." )
         || fileName == QStringLiteral( ".." )
         || fileName.contains( QLatin1Char( '/' ) )
         || fileName.contains( QLatin1Char( '\\' ) ) ) {
        return {};
    }
    return fileName;
}

bool isProcessRunning( qint64 processId )
{
    if ( processId <= 0 ) {
        return false;
    }
#if defined( Q_OS_WIN )
    const auto process = OpenProcess(
        SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
        static_cast<DWORD>( processId ) );
    if ( process == nullptr ) {
        return GetLastError() == ERROR_ACCESS_DENIED;
    }
    const auto result = WaitForSingleObject( process, 0 );
    CloseHandle( process );
    return result == WAIT_TIMEOUT;
#else
    const auto result = ::kill( static_cast<pid_t>( processId ), 0 );
    const auto error = errno;
    return result == 0 || error == EPERM;
#endif
}

std::atomic<int> capturePathGateTimeoutMs{ 5000 };
constexpr int CaptureRetryAttemptLimit = 8;
constexpr auto CaptureRetryInitialDelay = std::chrono::milliseconds( 25 );
constexpr auto CaptureRetryMaximumDelay = std::chrono::milliseconds( 400 );

// activate() reports nullopt only while a competing cleanup is still tearing
// down the previous generation (a permanently unusable capture path already
// throws from acquire()). Retrying forever on a wedged teardown would hang the
// streaming worker thread, so the retry is bounded and escalates to an
// exception instead.
constexpr int CaptureActivationMaxAttempts = 100;
constexpr auto CaptureActivationRetryDelay = std::chrono::milliseconds( 10 );

class CapturePathGate {
  public:
    explicit CapturePathGate( QString gatePath )
        : lock_( gatePath )
    {
        lock_.setStaleLockTime( 0 );
    }

    bool lock( int timeoutOverrideMs = -1 )
    {
        const auto timeout
            = timeoutOverrideMs >= 0
                  ? timeoutOverrideMs
                  : capturePathGateTimeoutMs.load( std::memory_order_acquire );
        if ( lock_.tryLock( timeout ) ) {
            return true;
        }
        if ( lock_.error() != QLockFile::LockFailedError ) {
            return false;
        }

        qint64 processId = 0;
        QString hostname;
        QString applicationName;
        if ( lock_.getLockInfo( &processId, &hostname,
                                &applicationName )
             && processId != QCoreApplication::applicationPid()
             && !isProcessRunning( processId )
             && lock_.removeStaleLockFile() ) {
            return lock_.tryLock( timeout );
        }
        return false;
    }

  private:
    QLockFile lock_;
};

struct CaptureBackgroundThreadTracker {
    std::mutex mutex;
    std::condition_variable stopped;
    bool stopping = false;
    size_t activeThreads = 0;
};

CaptureBackgroundThreadTracker& captureBackgroundThreadTracker()
{
    static CaptureBackgroundThreadTracker tracker;
    return tracker;
}

void stopCaptureBackgroundThreads()
{
    auto& tracker = captureBackgroundThreadTracker();
    std::unique_lock<std::mutex> lock( tracker.mutex );
    tracker.stopping = true;
    tracker.stopped.wait( lock, [ &tracker ] {
        return tracker.activeThreads == 0;
    } );
}

bool registerCaptureBackgroundThread()
{
    if ( QCoreApplication::instance() == nullptr ) {
        return false;
    }

    static std::once_flag cleanupRegistered;
    std::call_once( cleanupRegistered, [] {
        qAddPostRoutine( stopCaptureBackgroundThreads );
    } );

    auto& tracker = captureBackgroundThreadTracker();
    const std::lock_guard<std::mutex> lock( tracker.mutex );
    if ( tracker.stopping ) {
        return false;
    }
    ++tracker.activeThreads;
    return true;
}

void unregisterCaptureBackgroundThread()
{
    auto& tracker = captureBackgroundThreadTracker();
    const std::lock_guard<std::mutex> lock( tracker.mutex );
    --tracker.activeThreads;
    if ( tracker.activeThreads == 0 ) {
        tracker.stopped.notify_all();
    }
}

bool captureBackgroundThreadsStopping()
{
    auto& tracker = captureBackgroundThreadTracker();
    const std::lock_guard<std::mutex> lock( tracker.mutex );
    return tracker.stopping;
}

class CaptureBackgroundThreadRegistration {
  public:
    CaptureBackgroundThreadRegistration() = default;

    ~CaptureBackgroundThreadRegistration()
    {
        unregisterCaptureBackgroundThread();
    }

    CaptureBackgroundThreadRegistration(
        const CaptureBackgroundThreadRegistration& ) = delete;
    CaptureBackgroundThreadRegistration& operator=(
        const CaptureBackgroundThreadRegistration& ) = delete;
};

} // namespace

bool CaptureStore::isValidCaptureId( const QString& captureId )
{
    return isSafeCaptureId( captureId );
}

struct CaptureSegmentIdState {
    std::atomic<qint64> nextSegmentId{ 0 };
};

struct CaptureProcessFileOwnership {
    QHash<QString, QString> ownedFiles;
};

struct CaptureStore::CapturePathState
    : public std::enable_shared_from_this<CapturePathState> {
    struct ActivationResult {
        QSet<QString> inheritedCaptureFiles;
        QByteArray activationToken;
    };

    struct CleanupSnapshot {
        qint64 activityEpoch = 0;
    };

    struct TrackedFile {
        QString name;
        QString identity;
    };

    static QString trackedFileKey( const QString& name,
                                   const QString& identity )
    {
        return QString::number( name.size() ) + QLatin1Char( ':' ) + name
               + identity;
    }

    enum class RetireResult : std::uint8_t {
        Retired,
        Deferred,
        Rejected,
    };

    struct Registry;

    CapturePathState( SecureCaptureDirectory directory, QString registryKey,
                      qint64 registrySlotEpoch,
                      std::shared_ptr<CaptureSegmentIdState> segmentIds,
                      std::shared_ptr<CaptureProcessFileOwnership> processFileOwnership )
        : path_( directory.path() )
        , registryKey_( std::move( registryKey ) )
        , registrySlotEpoch_( registrySlotEpoch )
        , coordinationStem_( captureCoordinationStem( registryKey_ ) )
        , directory_( std::move( directory ) )
        , segmentIds_( std::move( segmentIds ) )
        , processFileOwnership_( std::move( processFileOwnership ) )
    {
        if ( coordinationStem_.isEmpty() ) {
            throw std::runtime_error( "Failed to create capture coordination root" );
        }
    }
    ~CapturePathState();

    static std::shared_ptr<CapturePathState> acquire( const QString& path,
                                                      bool createIfMissing = true );
    static Registry& registry();

    QByteArray processGeneration() const;
    QByteArray advanceProcessGeneration();
    void removeProcessGeneration();

    std::optional<ActivationResult> activate()
    {
        std::function<void()> beforeActivationCallback;
        {
            const std::lock_guard<std::recursive_mutex> lock( mutex_ );
            beforeActivationCallback
                = std::move( beforeActivationCallbackForTesting_ );
            beforeActivationCallbackForTesting_ = {};
        }
        if ( beforeActivationCallback ) {
            beforeActivationCallback();
        }

        CapturePathGate gate( coordinationStem_ + QStringLiteral( ".gate" ) );
        if ( !gate.lock() ) {
            throw std::runtime_error( "Failed to acquire capture activation gate" );
        }
        {
            const std::lock_guard<std::recursive_mutex> lock( mutex_ );
            if ( terminallyRemoved_.load( std::memory_order_acquire ) ) {
                return std::nullopt;
            }
        }
        if ( !directory_.ensureExists() ) {
            return std::nullopt;
        }
        const auto hasSiblingProcess = hasActiveProcessMarker();
        const auto activationGeneration = advanceProcessGeneration();
        if ( activationGeneration.isEmpty() ) {
            throw std::runtime_error( "Failed to publish capture generation" );
        }

        ActivationResult result;
        result.activationToken
            = QUuid::createUuid().toByteArray( QUuid::WithoutBraces );
        {
            const std::lock_guard<std::recursive_mutex> lock( mutex_ );
            const auto mayAdoptExistingFiles
                = activeStoreTokens_.isEmpty() && !hasSiblingProcess;
            if ( activeStoreTokens_.isEmpty() ) {
                if ( QFileInfo::exists( activeMarkerPath() )
                     && !QFile::remove( activeMarkerPath() ) ) {
                    throw std::runtime_error(
                        "Failed to remove stale local capture marker" );
                }
                processMarker_ = std::make_unique<QLockFile>( activeMarkerPath() );
                processMarker_->setStaleLockTime( 0 );
                if ( !processMarker_->tryLock( 0 ) ) {
                    processMarker_.reset();
                    throw std::runtime_error( "Failed to publish active capture marker" );
                }
            }
            if ( mayAdoptExistingFiles ) {
                QHash<QString, QString> currentCaptureFiles;
                const auto captureFiles = directory_.entryList(
                    QDir::Files | QDir::Hidden, QDir::NoSort );
                for ( const auto& fileName : captureFiles ) {
                    const auto fileIdentity = directory_.fileIdentity( fileName );
                    if ( fileIdentity.isEmpty() ) {
                        continue;
                    }
                    currentCaptureFiles.insert( fileName, fileIdentity );
                    result.inheritedCaptureFiles.insert(
                        QDir( path_ ).filePath( fileName ) );
                }
                processFileOwnership_->ownedFiles
                    = std::move( currentCaptureFiles );
            }
            localProcessGeneration_ = activationGeneration;
            activeStoreTokens_.insert( result.activationToken );
            ++activityEpoch_;
            // A replacement is active from construction, even if it never
            // appends a complete line. Do not let a prior generation rmdir it
            // or retire files that the new store may load.
            cancelDirectoryDeletionLocked();
            pendingRetirementRequesters_.clear();
            pendingRetirementFiles_.clear();
        }
        retryRetiredFilesAndReleaseRegistryGateHeld();
        if ( hasRetryableMaintenance() ) {
            scheduleRetry();
        }
        return result;
    }

    void deactivate( const QByteArray& activationToken )
    {
        {
            const std::lock_guard<std::recursive_mutex> lock( mutex_ );
            if ( activeStoreTokens_.remove( activationToken ) ) {
                ++activityEpoch_;
            }
            if ( activeStoreTokens_.isEmpty() ) {
                processMarker_.reset();
                if ( retiredFiles_.isEmpty() && !QDir( path_ ).exists() ) {
                    segmentIds_->nextSegmentId.store( 0,
                                                      std::memory_order_release );
                }
            }
        }

        CapturePathGate gate( coordinationStem_ + QStringLiteral( ".gate" ) );
        if ( !gate.lock() ) {
            scheduleRetry( true );
            return;
        }
        retryRetiredFilesAndReleaseRegistryGateHeld();
        if ( hasRetryableMaintenance() ) {
            scheduleRetry();
        }
    }

    bool isTerminallyRemoved() const
    {
        return terminallyRemoved_.load( std::memory_order_acquire );
    }

    void ensureDirectory( bool startsReplacement )
    {
        const std::lock_guard<std::recursive_mutex> lock( mutex_ );
        if ( startsReplacement ) {
            cancelDirectoryDeletionLocked();
            pendingRetirementRequesters_.clear();
            pendingRetirementFiles_.clear();
        }
        if ( !directory_.ensureExists() ) {
            throw std::runtime_error( "Capture directory identity changed" );
        }
    }

    void requestDirectoryDeletion( const QByteArray& activationToken )
    {
        {
            const std::lock_guard<std::recursive_mutex> lock( mutex_ );
            if ( localProcessGeneration_.isEmpty()
                 || !activeStoreTokens_.contains( activationToken ) ) {
                return;
            }
            directoryDeletionRequested_ = true;
            directoryDeletionRequesterToken_ = activationToken;
            directoryDeletionGeneration_ = localProcessGeneration_;
        }

        CapturePathGate gate( gatePath() );
        if ( !gate.lock() ) {
            scheduleRetry( true );
            return;
        }
        retryRetiredFilesAndReleaseRegistryGateHeld();
        if ( hasRetryableMaintenance() ) {
            scheduleRetry();
        }
    }

    std::shared_ptr<SpilledSegmentFile> leaseFor( const QString& filePath );
    void registerCreatedFile( const QString& filePath );
    void transferCreatedFile( const QString& oldPath, const QString& newPath );
    RetireResult tryRetireOwnedFile(
        const QString& filePath, const QString& fileIdentity,
        const QByteArray& activationToken );
    void retireFile( const QString& filePath );
    bool isTombstoned( const QString& filePath ) const;
    QString physicalPath( const QString& filePath ) const;
    std::unique_ptr<QFile> openReadFile(
        const QString& filePath, const QString& fileIdentity ) const;
    void releaseRetiredFile( const QString& filePath,
                             const QString& fileIdentity );

    QString gatePath() const
    {
        return coordinationStem_ + QStringLiteral( ".gate" );
    }

    bool hasActiveProcessMarker() const
    {
        const QFileInfo coordinationInfo( coordinationStem_ );
        const auto markerFiles = coordinationInfo.dir().entryList(
            QStringList{ coordinationInfo.fileName() + QStringLiteral( ".active.*" ) },
            QDir::Files | QDir::Hidden, QDir::Name );
        for ( const auto& markerFile : markerFiles ) {
            const auto markerPath = coordinationInfo.dir().filePath( markerFile );
            bool validProcessId = false;
            const auto processId
                = markerFile.section( QLatin1Char( '.' ), -1 )
                      .toLongLong( &validProcessId );
            if ( !validProcessId ) {
                QFile::remove( markerPath );
                continue;
            }
            if ( processId == QCoreApplication::applicationPid() ) {
                continue;
            }

            QLockFile markerLock( markerPath );
            markerLock.setStaleLockTime( 0 );
            if ( markerLock.tryLock( 0 ) ) {
                markerLock.unlock();
                continue;
            }

            const auto lockError = markerLock.error();
            qint64 lockProcessId = 0;
            QString hostname;
            QString applicationName;
            auto lockInfoAvailable
                = markerLock.getLockInfo( &lockProcessId, &hostname,
                                          &applicationName );
            if ( lockError == QLockFile::LockFailedError
                 && ( !lockInfoAvailable || lockProcessId != processId ) ) {
                // QLockFile creates the marker with O_EXCL and writes the
                // record in a separate step, so a live sibling can briefly
                // own a marker whose record is not readable yet. Re-read
                // through a bounded grace window so that creation window can
                // close; a marker that stays incoherent is an abandoned or
                // forged file and must still be removed — a live filename
                // pid alone is not proof of ownership, since pid reuse is
                // exactly how such leftovers appear.
                constexpr auto MarkerRecordGraceMs = 100;
                constexpr auto MarkerRecordPollMs = 10;
                for ( auto waited = 0;
                      waited < MarkerRecordGraceMs
                      && ( !lockInfoAvailable
                           || lockProcessId != processId );
                      waited += MarkerRecordPollMs ) {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds{ MarkerRecordPollMs } );
                    lockProcessId = 0;
                    lockInfoAvailable = markerLock.getLockInfo(
                        &lockProcessId, &hostname, &applicationName );
                }
                if ( !lockInfoAvailable || lockProcessId != processId ) {
                    QFile::remove( markerPath );
                    continue;
                }
            }
            if ( lockError != QLockFile::LockFailedError
                 || !lockInfoAvailable
                 || lockProcessId != processId
                 || !isProcessRunning( lockProcessId ) ) {
                QFile::remove( markerPath );
                continue;
            }
            // A renamed executable or another compatible klogg build can use a
            // different application name while still owning this exact locked
            // marker. Once the lock metadata PID is coherent and live, fail
            // closed rather than deleting a capture that process still uses.
            return true;
        }
        return false;
    }

    void retryRetiredFilesAndReleaseRegistry();
    void retryRetiredFilesAndReleaseRegistryGateHeld();
    void scheduleRetry( bool force = false );
    bool hasRetryableMaintenance() const;
    bool hasPendingGateRetry() const;
    void failNextRetiredFileRemovalForTesting()
    {
        const std::lock_guard<std::recursive_mutex> lock( mutex_ );
        failNextRetiredFileRemovalForTesting_ = true;
    }

    void failNextCaptureDirectoryRemovalForTesting()
    {
        const std::lock_guard<std::recursive_mutex> lock( mutex_ );
        failNextCaptureDirectoryRemovalForTesting_ = true;
    }

    void failNextRecursiveDirectoryRemovalForTesting()
    {
        const std::lock_guard<std::recursive_mutex> lock( mutex_ );
        directory_.failNextRecursiveRemovalForTesting();
    }

    void setBeforeActivationCallbackForTesting(
        std::function<void()> callback )
    {
        const std::lock_guard<std::recursive_mutex> lock( mutex_ );
        beforeActivationCallbackForTesting_ = std::move( callback );
    }

    void setAfterRecursiveRemovalQuarantineCallbackForTesting(
        std::function<void()> callback )
    {
        const std::lock_guard<std::recursive_mutex> lock( mutex_ );
        directory_.setAfterRecursiveRemovalQuarantineCallbackForTesting(
            std::move( callback ) );
    }

    bool hasLocalCoordinationOwnershipForTesting() const
    {
        const std::lock_guard<std::recursive_mutex> lock( mutex_ );
        return processMarker_ || QFileInfo::exists( processGenerationPath() );
    }

    QString activeMarkerPathForTesting() const
    {
        return activeMarkerPath();
    }

    std::vector<qint64> reserveSegmentIds( qint64 count )
    {
        const std::lock_guard<std::recursive_mutex> lock( mutex_ );
        if ( count <= 0 ) {
            return {};
        }

        const auto maximumId = std::numeric_limits<qint64>::max();
        auto nextId = segmentIds_->nextSegmentId.load( std::memory_order_acquire );
        while ( true ) {
            if ( nextId < 0 || count > maximumId - nextId ) {
                throw std::overflow_error( "Capture segment id space exhausted" );
            }
            if ( segmentIds_->nextSegmentId.compare_exchange_weak(
                    nextId, nextId + count, std::memory_order_acq_rel,
                    std::memory_order_acquire ) ) {
                std::vector<qint64> segmentIds;
                segmentIds.reserve( static_cast<size_t>( count ) );
                for ( qint64 segmentId = nextId; segmentId < nextId + count;
                      ++segmentId ) {
                    segmentIds.push_back( segmentId );
                }
                return segmentIds;
            }
        }
    }

    void observeSegmentId( qint64 segmentId )
    {
        const std::lock_guard<std::recursive_mutex> lock( mutex_ );
        if ( segmentId < 0
             || segmentId == std::numeric_limits<qint64>::max() ) {
            return;
        }

        const auto afterId = segmentId + 1;
        auto nextId = segmentIds_->nextSegmentId.load( std::memory_order_acquire );
        while ( nextId < afterId
               && !segmentIds_->nextSegmentId.compare_exchange_weak(
                   nextId, afterId, std::memory_order_acq_rel,
                   std::memory_order_acquire ) ) {
        }
    }

    std::optional<CleanupSnapshot> cleanupSnapshot()
    {
        const std::lock_guard<std::recursive_mutex> lock( mutex_ );
        pruneExpiredLeases();
        if ( !activeStoreTokens_.isEmpty() || hasProtectedFiles() ) {
            return std::nullopt;
        }
        return CleanupSnapshot{ activityEpoch_ };
    }

    bool mayCleanup( qint64 expectedActivityEpoch )
    {
        pruneExpiredLeases();
        return activeStoreTokens_.isEmpty() && activityEpoch_ == expectedActivityEpoch
               && !hasProtectedFiles();
    }

    mutable std::recursive_mutex mutex_;
    QString path_;
    QString registryKey_;
    qint64 registrySlotEpoch_ = 0;
    QString coordinationStem_;
    SecureCaptureDirectory directory_;

    void finalizeRemovedGenerationGateHeld()
    {
        const std::lock_guard<std::recursive_mutex> lock( mutex_ );
        finalizeRemovedGenerationGateHeldLocked();
    }

  private:
    static void retainTombstones( const QString& registryKey,
                                  qint64 registrySlotEpoch,
                                  const std::shared_ptr<CapturePathState>& state,
                                  qint64 tombstoneEpoch );
    static void releaseTombstones( const QString& registryKey,
                                   qint64 registrySlotEpoch,
                                   const std::shared_ptr<CapturePathState>& state,
                                   qint64 tombstoneEpoch );

    void pruneExpiredLeases()
    {
        auto lease = fileLeases_.begin();
        while ( lease != fileLeases_.end() ) {
            if ( lease.value().expired() ) {
                lease = fileLeases_.erase( lease );
            } else {
                ++lease;
            }
        }
    }

    qint64 promotePendingRetirementsLocked()
    {
        const auto soleActiveToken
            = activeStoreTokens_.size() == 1
                  ? *activeStoreTokens_.cbegin()
                  : QByteArray{};
        qint64 retainEpoch = 0;
        auto pending = pendingRetirementRequesters_.begin();
        while ( pending != pendingRetirementRequesters_.end() ) {
            const auto mayRetire
                = activeStoreTokens_.isEmpty()
                  || ( !soleActiveToken.isEmpty()
                       && pending.value().contains( soleActiveToken ) );
            if ( !mayRetire ) {
                ++pending;
                continue;
            }
            const auto pendingFile
                = pendingRetirementFiles_.value( pending.key() );
            if ( !pendingFile.identity.isEmpty()
                 && !retiredFiles_.contains( pending.key() ) ) {
                retiredFiles_.insert( pending.key(), pendingFile );
                retainEpoch = ++tombstoneEpoch_;
            }
            pendingRetirementFiles_.remove( pending.key() );
            pending = pendingRetirementRequesters_.erase( pending );
        }
        return retainEpoch;
    }

    bool hasProtectedFiles() const
    {
        if ( !retiredFiles_.isEmpty() ) {
            return true;
        }
        for ( auto lease = fileLeases_.cbegin(); lease != fileLeases_.cend(); ++lease ) {
            if ( !lease.value().expired() ) {
                return true;
            }
        }
        return false;
    }

    QString activeMarkerPath() const
    {
        return coordinationStem_ + QStringLiteral( ".active.%1" )
                                   .arg( QCoreApplication::applicationPid() );
    }

    QString processGenerationPath() const
    {
        return coordinationStem_ + QStringLiteral( ".generation" );
    }

    bool removeRetiredFile( const TrackedFile& file )
    {
        if ( failNextRetiredFileRemovalForTesting_ ) {
            failNextRetiredFileRemovalForTesting_ = false;
            return false;
        }
        return directory_.removeFile( file.name, file.identity );
    }

    void cancelDirectoryDeletionLocked()
    {
        directoryDeletionRequested_ = false;
        directoryDeletionRequesterToken_.clear();
        directoryDeletionGeneration_.clear();
    }

    void finalizeRemovedGenerationGateHeldLocked()
    {
        processMarker_.reset();
        removeProcessGeneration();
        processFileOwnership_->ownedFiles.clear();
        pendingRetirementRequesters_.clear();
        pendingRetirementFiles_.clear();
        retiredFiles_.clear();
        segmentIds_->nextSegmentId.store( 0, std::memory_order_release );
        localProcessGeneration_.clear();
        cancelDirectoryDeletionLocked();
        terminallyRemoved_.store( true, std::memory_order_release );
    }

    void removeDirectoryIfRequestedAndEmptyGateHeld()
    {
        if ( !directoryDeletionRequested_ || !retiredFiles_.isEmpty() ) {
            return;
        }
        if ( directoryDeletionRequesterToken_.isEmpty() ) {
            cancelDirectoryDeletionLocked();
            return;
        }
        const auto requesterIsActive = activeStoreTokens_.contains(
            directoryDeletionRequesterToken_ );
        if ( ( requesterIsActive && activeStoreTokens_.size() != 1 )
             || ( !requesterIsActive && !activeStoreTokens_.isEmpty() )
             || hasActiveProcessMarker() ) {
            return;
        }
        if ( directoryDeletionGeneration_.isEmpty() ) {
            cancelDirectoryDeletionLocked();
            return;
        }
        if ( processGeneration() != directoryDeletionGeneration_ ) {
            // The gate excludes activation and no foreign marker remains, so a
            // different sidecar can only belong to an exited process. Publish a
            // fresh local generation instead of abandoning deletion forever.
            const auto refreshedGeneration = advanceProcessGeneration();
            if ( refreshedGeneration.isEmpty() ) {
                return;
            }
            localProcessGeneration_ = refreshedGeneration;
            directoryDeletionGeneration_ = refreshedGeneration;
        }

        if ( directory_.isRemoved() ) {
            finalizeRemovedGenerationGateHeldLocked();
            return;
        }
        if ( directory_.hasEntries() ) {
            if ( activeStoreTokens_.isEmpty() ) {
                cancelDirectoryDeletionLocked();
            }
            return;
        }
        if ( !failNextCaptureDirectoryRemovalForTesting_ ) {
            const auto directoryRemoved = directory_.removeIfEmpty();
            if ( directoryRemoved || directory_.isRemoved() ) {
                finalizeRemovedGenerationGateHeldLocked();
            }
        }
        failNextCaptureDirectoryRemovalForTesting_ = false;
    }

    qint64 takeTombstoneRegistryReleaseEpochLocked()
    {
        if ( !retiredFiles_.isEmpty() ) {
            return 0;
        }

        const auto releaseEpoch
            = qMax( pendingTombstoneRegistryReleaseEpoch_, tombstoneEpoch_ );
        pendingTombstoneRegistryReleaseEpoch_ = 0;
        return releaseEpoch;
    }

    QSet<QByteArray> activeStoreTokens_;
    qint64 activityEpoch_ = 0;
    bool directoryDeletionRequested_ = false;
    QByteArray localProcessGeneration_;
    QByteArray directoryDeletionRequesterToken_;
    QByteArray directoryDeletionGeneration_;
    std::atomic<bool> terminallyRemoved_{ false };
    std::atomic<bool> retryScheduled_{ false };
    std::atomic<std::uint64_t> gateRetryRequestEpoch_{ 0 };
    std::atomic<std::uint64_t> gateRetryCompletedEpoch_{ 0 };
    qint64 tombstoneEpoch_ = 0;
    qint64 pendingTombstoneRegistryReleaseEpoch_ = 0;
    bool failNextRetiredFileRemovalForTesting_ = false;
    bool failNextCaptureDirectoryRemovalForTesting_ = false;
    std::function<void()> beforeActivationCallbackForTesting_;
    std::unique_ptr<QLockFile> processMarker_;
    std::shared_ptr<CaptureSegmentIdState> segmentIds_;
    QHash<QString, std::weak_ptr<SpilledSegmentFile>> fileLeases_;
    std::shared_ptr<CaptureProcessFileOwnership> processFileOwnership_;
    QHash<QString, QSet<QByteArray>> pendingRetirementRequesters_;
    QHash<QString, TrackedFile> pendingRetirementFiles_;
    QHash<QString, TrackedFile> retiredFiles_;
};

struct CaptureStore::SpilledSegmentFile {
    SpilledSegmentFile( QString path, QString identity,
                        std::shared_ptr<CapturePathState> capturePathState )
        : path_( std::move( path ) )
        , identity_( std::move( identity ) )
        , capturePathState_( std::move( capturePathState ) )
    {
    }

    ~SpilledSegmentFile()
    {
        if ( retired_.load( std::memory_order_acquire )
             || notifyOnRelease_.load( std::memory_order_acquire ) ) {
            try {
                capturePathState_->releaseRetiredFile( path_, identity_ );
            } catch ( ... ) {
                LOG_WARNING << "Failed to release retired capture file";
            }
        }
    }

    void retire( const QByteArray& activationToken )
    {
        bool expected = false;
        if ( !retired_.compare_exchange_strong( expected, true,
                                                std::memory_order_acq_rel ) ) {
            return;
        }
        const auto result = capturePathState_->tryRetireOwnedFile(
            path_, identity_, activationToken );
        if ( result == CapturePathState::RetireResult::Deferred ) {
            notifyOnRelease_.store( true, std::memory_order_release );
        }
        if ( result != CapturePathState::RetireResult::Retired ) {
            retired_.store( false, std::memory_order_release );
        }
    }

    QString path() const
    {
        return capturePathState_->physicalPath( path_ );
    }

    bool isRetired() const
    {
        return retired_.load( std::memory_order_acquire );
    }

    QString identity() const
    {
        return identity_;
    }

    std::unique_ptr<QFile> openForRead() const
    {
        return capturePathState_->openReadFile( path_, identity_ );
    }

  private:
    QString path_;
    QString identity_;
    std::shared_ptr<CapturePathState> capturePathState_;
    std::atomic<bool> retired_{ false };
    std::atomic<bool> notifyOnRelease_{ false };
};

struct CaptureStore::CapturePathState::Registry {
    struct Entry {
        std::weak_ptr<CapturePathState> state;
        std::shared_ptr<CaptureSegmentIdState> segmentIds;
        std::shared_ptr<CaptureProcessFileOwnership> processFileOwnership;
        std::shared_ptr<CapturePathState> tombstoneState;
        qint64 tombstoneEpoch = 0;
        qint64 slotEpoch = 0;
    };

    std::mutex mutex;
    QHash<QString, Entry> entries;
    qint64 nextSlotEpoch = 0;
};

CaptureStore::CapturePathState::Registry& CaptureStore::CapturePathState::registry()
{
    // The registry intentionally outlives Qt and static teardown: path-state
    // destructors may run during process shutdown and still need this mutex.
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
    static auto* const registry = new Registry;
    return *registry;
}

CaptureStore::CapturePathState::~CapturePathState()
{
    auto& pathRegistry = registry();
    const std::lock_guard<std::mutex> lock( pathRegistry.mutex );
    const auto entry = pathRegistry.entries.find( registryKey_ );
    if ( entry != pathRegistry.entries.end()
         && entry->slotEpoch == registrySlotEpoch_ && entry->state.expired()
         && !entry->tombstoneState
         && ( !entry->processFileOwnership
              || entry->processFileOwnership->ownedFiles.isEmpty() ) ) {
        pathRegistry.entries.erase( entry );
    }
}

QByteArray CaptureStore::CapturePathState::processGeneration() const
{
    QFile generationFile( processGenerationPath() );
    if ( !generationFile.open( QIODevice::ReadOnly ) ) {
        return {};
    }
    return generationFile.readAll();
}

QByteArray CaptureStore::CapturePathState::advanceProcessGeneration()
{
    QSaveFile generationFile( processGenerationPath() );
    generationFile.setDirectWriteFallback( false );
    if ( !generationFile.open( QIODevice::WriteOnly ) ) {
        return {};
    }

    auto generation
        = QUuid::createUuid().toByteArray( QUuid::WithoutBraces );
    if ( generationFile.write( generation ) != generation.size()
         || !generationFile.commit() ) {
        return {};
    }
    return generation;
}

void CaptureStore::CapturePathState::removeProcessGeneration()
{
    QFile::remove( processGenerationPath() );
}

std::shared_ptr<CaptureStore::CapturePathState>
CaptureStore::CapturePathState::acquire( const QString& path, bool createIfMissing )
{
#if !defined( Q_OS_WIN )
    const auto captureRoot = QFileInfo( path ).dir().absolutePath();
    if ( createIfMissing && !QDir{}.mkpath( captureRoot ) ) {
        throw std::runtime_error( "Failed to create capture root" );
    }
#endif

    SecureCaptureDirectory directory( path );
    const auto directoryReady = createIfMissing ? directory.ensureExists()
                                                : directory.bindExisting();
    if ( !directoryReady ) {
        if ( createIfMissing ) {
            throw std::runtime_error( "Failed to bind capture directory" );
        }
        return {};
    }
    const auto registryKey = directory.identityKey();
    if ( registryKey.isEmpty() ) {
        if ( createIfMissing ) {
            throw std::runtime_error( "Failed to identify capture directory" );
        }
        return {};
    }

    auto& pathRegistry = registry();
    const std::lock_guard<std::mutex> lock( pathRegistry.mutex );
    auto entry = pathRegistry.entries.find( registryKey );
    if ( entry != pathRegistry.entries.end() ) {
        if ( const auto state = entry->state.lock() ) {
            if ( !state->terminallyRemoved_.load( std::memory_order_acquire ) ) {
                return state;
            }
        }
        if ( entry->tombstoneState
             && !entry->tombstoneState->terminallyRemoved_.load(
                 std::memory_order_acquire ) ) {
            entry->state = entry->tombstoneState;
            return entry->tombstoneState;
        }
        if ( !entry->tombstoneState && entry->segmentIds
             && entry->processFileOwnership ) {
            auto state = std::make_shared<CapturePathState>(
                std::move( directory ), registryKey, entry->slotEpoch,
                entry->segmentIds, entry->processFileOwnership );
            entry->state = state;
            return state;
        }
    }

    Registry::Entry newEntry;
    newEntry.segmentIds = std::make_shared<CaptureSegmentIdState>();
    newEntry.processFileOwnership
        = std::make_shared<CaptureProcessFileOwnership>();
    newEntry.slotEpoch = ++pathRegistry.nextSlotEpoch;
    auto state = std::make_shared<CapturePathState>(
        std::move( directory ), registryKey, newEntry.slotEpoch,
        newEntry.segmentIds, newEntry.processFileOwnership );
    newEntry.state = state;
    pathRegistry.entries.insert( registryKey, newEntry );
    return state;
}

void CaptureStore::CapturePathState::retainTombstones(
    const QString& registryKey, qint64 registrySlotEpoch,
    const std::shared_ptr<CapturePathState>& state, qint64 tombstoneEpoch )
{
    auto& pathRegistry = registry();
    const std::lock_guard<std::mutex> lock( pathRegistry.mutex );
    const auto entry = pathRegistry.entries.find( registryKey );
    if ( entry != pathRegistry.entries.end()
         && entry->slotEpoch == registrySlotEpoch
         && ( !entry->tombstoneState
              || entry->tombstoneEpoch <= tombstoneEpoch ) ) {
        entry->tombstoneState = state;
        entry->tombstoneEpoch = tombstoneEpoch;
    }
}

void CaptureStore::CapturePathState::releaseTombstones(
    const QString& registryKey, qint64 registrySlotEpoch,
    const std::shared_ptr<CapturePathState>& state, qint64 tombstoneEpoch )
{
    auto& pathRegistry = registry();
    const std::lock_guard<std::mutex> lock( pathRegistry.mutex );
    const auto entry = pathRegistry.entries.find( registryKey );
    if ( entry != pathRegistry.entries.end()
         && entry->slotEpoch == registrySlotEpoch
         && entry->tombstoneState == state
         && entry->tombstoneEpoch <= tombstoneEpoch ) {
        entry->tombstoneState.reset();
        entry->tombstoneEpoch = 0;
    }
}

std::shared_ptr<CaptureStore::SpilledSegmentFile>
CaptureStore::CapturePathState::leaseFor( const QString& filePath )
{
    const auto fileName = directChildName( filePath );
    if ( fileName.isEmpty() ) {
        throw std::invalid_argument( "Capture file must be a direct child" );
    }

    const std::lock_guard<std::recursive_mutex> lock( mutex_ );
    const auto fileIdentity = directory_.fileIdentity( fileName );
    if ( fileIdentity.isEmpty() ) {
        return {};
    }
    const auto fileKey = trackedFileKey( fileName, fileIdentity );
    if ( const auto retained = fileLeases_.value( fileKey ).lock() ) {
        return retained;
    }

    auto spilledFile = std::make_shared<SpilledSegmentFile>(
        fileName, fileIdentity, shared_from_this() );
    fileLeases_.insert( fileKey, spilledFile );
    return spilledFile;
}

void CaptureStore::CapturePathState::registerCreatedFile( const QString& filePath )
{
    const auto fileName = directChildName( filePath );
    if ( fileName.isEmpty() ) {
        return;
    }
    const std::lock_guard<std::recursive_mutex> lock( mutex_ );
    const auto fileIdentity = directory_.fileIdentity( fileName );
    if ( !fileIdentity.isEmpty() ) {
        processFileOwnership_->ownedFiles.insert( fileName, fileIdentity );
    }
}

void CaptureStore::CapturePathState::transferCreatedFile( const QString& oldPath,
                                                          const QString& newPath )
{
    const auto oldName = directChildName( oldPath );
    const auto newName = directChildName( newPath );
    if ( oldName.isEmpty() || newName.isEmpty() ) {
        return;
    }
    const std::lock_guard<std::recursive_mutex> lock( mutex_ );
    const auto fileIdentity
        = processFileOwnership_->ownedFiles.take( oldName );
    if ( !fileIdentity.isEmpty() ) {
        processFileOwnership_->ownedFiles.insert( newName, fileIdentity );
    }
}

CaptureStore::CapturePathState::RetireResult
CaptureStore::CapturePathState::tryRetireOwnedFile(
    const QString& filePath, const QString& fileIdentity,
    const QByteArray& activationToken )
{
    const auto fileName = directChildName( filePath );
    if ( fileName.isEmpty() || fileIdentity.isEmpty()
         || activationToken.isEmpty() ) {
        return RetireResult::Rejected;
    }

    const auto fileKey = trackedFileKey( fileName, fileIdentity );
    qint64 tombstoneEpoch = 0;
    {
        const std::lock_guard<std::recursive_mutex> lock( mutex_ );
        if ( processFileOwnership_->ownedFiles.value( fileName )
                 != fileIdentity
             || !activeStoreTokens_.contains( activationToken ) ) {
            return RetireResult::Rejected;
        }
        if ( activeStoreTokens_.size() != 1 ) {
            pendingRetirementRequesters_[ fileKey ].insert(
                activationToken );
            pendingRetirementFiles_.insert(
                fileKey, TrackedFile{ fileName, fileIdentity } );
            return RetireResult::Deferred;
        }
        retiredFiles_.insert(
            fileKey, TrackedFile{ fileName, fileIdentity } );
        tombstoneEpoch = ++tombstoneEpoch_;
    }
    retainTombstones( registryKey_, registrySlotEpoch_, shared_from_this(),
                      tombstoneEpoch );
    return RetireResult::Retired;
}

void CaptureStore::CapturePathState::retireFile( const QString& filePath )
{
    const auto fileName = directChildName( filePath );
    if ( fileName.isEmpty() ) {
        return;
    }

    const auto fileIdentity = directory_.fileIdentity( fileName );
    if ( fileIdentity.isEmpty() ) {
        return;
    }

    const auto fileKey = trackedFileKey( fileName, fileIdentity );
    qint64 tombstoneEpoch = 0;
    {
        const std::lock_guard<std::recursive_mutex> lock( mutex_ );
        retiredFiles_.insert(
            fileKey, TrackedFile{ fileName, fileIdentity } );
        tombstoneEpoch = ++tombstoneEpoch_;
    }
    retainTombstones( registryKey_, registrySlotEpoch_, shared_from_this(),
                      tombstoneEpoch );
}

bool CaptureStore::CapturePathState::isTombstoned( const QString& filePath ) const
{
    const auto fileName = directChildName( filePath );
    const std::lock_guard<std::recursive_mutex> lock( mutex_ );
    if ( fileName.isEmpty() ) {
        return false;
    }
    const auto currentIdentity = directory_.fileIdentity( fileName );
    return !currentIdentity.isEmpty()
           && retiredFiles_.contains(
               trackedFileKey( fileName, currentIdentity ) );
}

QString CaptureStore::CapturePathState::physicalPath( const QString& filePath ) const
{
    const auto fileName = directChildName( filePath );
    return fileName.isEmpty() ? QString{} : QDir( path_ ).filePath( fileName );
}

std::unique_ptr<QFile> CaptureStore::CapturePathState::openReadFile(
    const QString& filePath, const QString& fileIdentity ) const
{
    const auto fileName = directChildName( filePath );
    return fileName.isEmpty()
               ? nullptr
               : directory_.openReadFile( fileName, fileIdentity );
}

void CaptureStore::CapturePathState::releaseRetiredFile(
    const QString& filePath, const QString& fileIdentity )
{
    CapturePathGate gate( gatePath() );
    if ( !gate.lock() ) {
        scheduleRetry( true );
        return;
    }
    const auto foreignProcessActive = hasActiveProcessMarker();

    qint64 retainEpoch = 0;
    qint64 releaseEpoch = 0;
    {
        const std::lock_guard<std::recursive_mutex> lock( mutex_ );
        pruneExpiredLeases();
        retainEpoch = promotePendingRetirementsLocked();
        const auto fileKey = trackedFileKey( filePath, fileIdentity );
        const auto retired = retiredFiles_.find( fileKey );
        if ( retired != retiredFiles_.end() && !foreignProcessActive ) {
            auto removed = removeRetiredFile( retired.value() );
            if ( !removed ) {
                std::this_thread::yield();
                removed = removeRetiredFile( retired.value() );
            }
            if ( removed ) {
                if ( processFileOwnership_->ownedFiles.value( filePath )
                     == fileIdentity ) {
                    processFileOwnership_->ownedFiles.remove( filePath );
                }
                retiredFiles_.erase( retired );
                ++tombstoneEpoch_;
                removeDirectoryIfRequestedAndEmptyGateHeld();
                releaseEpoch = takeTombstoneRegistryReleaseEpochLocked();
            }
        }
    }

    if ( retainEpoch > 0 ) {
        retainTombstones( registryKey_, registrySlotEpoch_, shared_from_this(),
                          retainEpoch );
    }
    if ( releaseEpoch > 0 ) {
        releaseTombstones( registryKey_, registrySlotEpoch_, shared_from_this(),
                           releaseEpoch );
    }
    if ( hasRetryableMaintenance() ) {
        scheduleRetry();
    }
}

void CaptureStore::CapturePathState::retryRetiredFilesAndReleaseRegistry()
{
    CapturePathGate gate( gatePath() );
    if ( !gate.lock() ) {
        scheduleRetry( true );
        return;
    }
    retryRetiredFilesAndReleaseRegistryGateHeld();
    if ( hasRetryableMaintenance() ) {
        scheduleRetry();
    }
}

void CaptureStore::CapturePathState::retryRetiredFilesAndReleaseRegistryGateHeld()
{
    const auto foreignProcessActive = hasActiveProcessMarker();
    qint64 retainEpoch = 0;
    qint64 releaseEpoch = 0;
    {
        const std::lock_guard<std::recursive_mutex> lock( mutex_ );
        pruneExpiredLeases();
        retainEpoch = promotePendingRetirementsLocked();
        if ( !foreignProcessActive ) {
            for ( auto retired = retiredFiles_.begin();
                  retired != retiredFiles_.end(); ) {
                if ( !fileLeases_.value( retired.key() ).expired() ) {
                    ++retired;
                    continue;
                }
                if ( removeRetiredFile( retired.value() ) ) {
                    const auto& retiredFile = retired.value();
                    if ( processFileOwnership_->ownedFiles.value(
                             retiredFile.name )
                         == retiredFile.identity ) {
                        processFileOwnership_->ownedFiles.remove(
                            retiredFile.name );
                    }
                    retired = retiredFiles_.erase( retired );
                    ++tombstoneEpoch_;
                } else {
                    ++retired;
                }
            }
        }
        if ( retiredFiles_.isEmpty() ) {
            removeDirectoryIfRequestedAndEmptyGateHeld();
            releaseEpoch = takeTombstoneRegistryReleaseEpochLocked();
        }
    }

    if ( retainEpoch > 0 ) {
        retainTombstones( registryKey_, registrySlotEpoch_, shared_from_this(),
                          retainEpoch );
    }
    if ( releaseEpoch > 0 ) {
        releaseTombstones( registryKey_, registrySlotEpoch_, shared_from_this(),
                           releaseEpoch );
    }
}

bool CaptureStore::CapturePathState::hasRetryableMaintenance() const
{
    const std::lock_guard<std::recursive_mutex> lock( mutex_ );
    for ( auto retired = retiredFiles_.cbegin();
          retired != retiredFiles_.cend(); ++retired ) {
        if ( fileLeases_.value( retired.key() ).expired() ) {
            return true;
        }
    }
    if ( !pendingRetirementRequesters_.isEmpty()
         && activeStoreTokens_.isEmpty() ) {
        return true;
    }
    return directoryDeletionRequested_ && activeStoreTokens_.isEmpty()
           && retiredFiles_.isEmpty();
}

bool CaptureStore::CapturePathState::hasPendingGateRetry() const
{
    return gateRetryCompletedEpoch_.load( std::memory_order_acquire )
           != gateRetryRequestEpoch_.load( std::memory_order_acquire );
}

void CaptureStore::CapturePathState::scheduleRetry( bool force )
{
    if ( !force && !hasPendingGateRetry() && !hasRetryableMaintenance() ) {
        return;
    }
    gateRetryRequestEpoch_.fetch_add( 1, std::memory_order_acq_rel );

    bool expected = false;
    if ( !retryScheduled_.compare_exchange_strong(
             expected, true, std::memory_order_acq_rel ) ) {
        return;
    }

    if ( !registerCaptureBackgroundThread() ) {
        retryScheduled_.store( false, std::memory_order_release );
        return;
    }

    auto state = shared_from_this();
    try {
        std::thread( [ state = std::move( state ) ]() noexcept {
            CaptureBackgroundThreadRegistration registration;
            auto lastAttemptRequestEpoch = std::uint64_t{ 0 };
            try {
                auto retryDelay = CaptureRetryInitialDelay;
                for ( int attempt = 0;
                      attempt < CaptureRetryAttemptLimit
                      && !captureBackgroundThreadsStopping();
                      ++attempt ) {
                    lastAttemptRequestEpoch
                        = state->gateRetryRequestEpoch_.load(
                            std::memory_order_acquire );
                    {
                        CapturePathGate gate( state->gatePath() );
                        if ( gate.lock( 0 ) ) {
                            state->retryRetiredFilesAndReleaseRegistryGateHeld();
                            state->gateRetryCompletedEpoch_.store(
                                lastAttemptRequestEpoch,
                                std::memory_order_release );
                        }
                    }
                    if ( !state->hasPendingGateRetry()
                         && !state->hasRetryableMaintenance() ) {
                        state->retryScheduled_.store(
                            false, std::memory_order_release );
                        if ( state->hasPendingGateRetry()
                             || state->hasRetryableMaintenance() ) {
                            state->scheduleRetry();
                        }
                        return;
                    }
                    if ( attempt + 1 < CaptureRetryAttemptLimit ) {
                        std::this_thread::sleep_for( retryDelay );
                        retryDelay = std::min( retryDelay * 2,
                                               CaptureRetryMaximumDelay );
                    }
                }
            } catch ( const std::exception& error ) {
                LOG_ERROR << "Capture gate retry worker failed: " << error.what();
            } catch ( ... ) {
                LOG_ERROR << "Capture gate retry worker failed with an unknown error";
            }

            // The epilogue runs outside the retry loop's guard but must still
            // never let an exception escape this detached noexcept thread:
            // scheduleRetry() allocates and can re-enter the registry.
            try {
                state->retryScheduled_.store( false, std::memory_order_release );
                const auto requestEpoch
                    = state->gateRetryRequestEpoch_.load(
                        std::memory_order_acquire );
                const auto newRetryRequestArrived
                    = requestEpoch != lastAttemptRequestEpoch;
                if ( !captureBackgroundThreadsStopping()
                     && newRetryRequestArrived ) {
                    state->scheduleRetry();
                }
            } catch ( const std::exception& error ) {
                LOG_ERROR << "Failed to reschedule capture gate retry worker: "
                          << error.what();
            } catch ( ... ) {
                LOG_ERROR << "Failed to reschedule capture gate retry worker";
            }
        } ).detach();
    } catch ( ... ) {
        unregisterCaptureBackgroundThread();
        retryScheduled_.store( false, std::memory_order_release );
    }
}

QString CaptureStore::defaultRootPath()
{
    return QDir( QDir::tempPath() ).filePath( "klogg_live" );
}

std::vector<CaptureStore::CleanupCandidate> CaptureStore::collectUnusedCaptureCandidates(
    const QSet<QString>& retainCaptureIds, const QString& rootPath,
    int gateTimeoutMs, const std::function<bool()>& shouldStop )
{
    QDir capturesRoot( canonicalCaptureRoot( rootPath.isEmpty() ? defaultRootPath() : rootPath ) );
    if ( !capturesRoot.exists() ) {
        return {};
    }

    QSet<QString> retainedRegistryKeys;
    for ( const auto& retainCaptureId : retainCaptureIds ) {
        if ( !isSafeCaptureId( retainCaptureId ) ) {
            continue;
        }
        const auto retainedPath
            = capturePathForId( capturesRoot.absolutePath(), retainCaptureId );
        if ( const auto retainedState
             = CapturePathState::acquire( retainedPath, false ) ) {
            retainedRegistryKeys.insert( retainedState->registryKey_ );
        }
    }

    const auto entries = capturesRoot.entryInfoList(
        QDir::Dirs | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot );
    std::vector<CleanupCandidate> candidates;
    candidates.reserve( static_cast<size_t>( entries.size() ) );
    for ( const auto& entry : entries ) {
        if ( shouldStop && shouldStop() ) {
            break;
        }
        // Never resolve capture-directory symlinks: their target may be outside
        // the configured root or may alias a retained real capture.
        if ( entry.isSymLink() || retainCaptureIds.contains( entry.fileName() ) ) {
            continue;
        }

        const auto capturePath = lexicalCapturePath( entry.absoluteFilePath() );
        const auto capturePathState = CapturePathState::acquire( capturePath, false );
        if ( !capturePathState
             || retainedRegistryKeys.contains( capturePathState->registryKey_ ) ) {
            continue;
        }
        CapturePathGate gate( capturePathState->gatePath() );
        if ( !gate.lock( gateTimeoutMs )
             || capturePathState->hasActiveProcessMarker() ) {
            continue;
        }
        capturePathState->retryRetiredFilesAndReleaseRegistryGateHeld();
        const auto snapshot = capturePathState->cleanupSnapshot();
        if ( snapshot ) {
            candidates.push_back( CleanupCandidate{
                capturePath, capturePathState, snapshot->activityEpoch,
                capturePathState->processGeneration() } );
        }
    }
    return candidates;
}

QStringList CaptureStore::collectUnusedCapturePaths( const QSet<QString>& retainCaptureIds,
                                                      const QString& rootPath )
{
    const auto candidates = collectUnusedCaptureCandidates( retainCaptureIds, rootPath );
    QStringList capturePaths;
    capturePaths.reserve( static_cast<int>( candidates.size() ) );
    for ( const auto& candidate : candidates ) {
        capturePaths.append( candidate.capturePath );
    }
    return capturePaths;
}

void CaptureStore::cleanupCaptureCandidates(
    const std::vector<CleanupCandidate>& candidates,
    const QDateTime& preserveModifiedAfter,
    const std::function<void( const QString& )>& beforeRemoval,
    int gateTimeoutMs, const std::function<bool()>& shouldStop )
{
    const auto cutoff = preserveModifiedAfter.toUTC();
    for ( const auto& candidate : candidates ) {
        if ( shouldStop && shouldStop() ) {
            break;
        }
        CapturePathGate gate( candidate.capturePathState->gatePath() );
        if ( !gate.lock( gateTimeoutMs )
             || candidate.capturePathState->hasActiveProcessMarker()
             || candidate.capturePathState->processGeneration()
                    != candidate.processGeneration ) {
            continue;
        }

        candidate.capturePathState->retryRetiredFilesAndReleaseRegistryGateHeld();
        // Cleanup is intentionally isolated to the per-path mutex after the
        // cross-process gate has made activation-vs-cleanup deterministic.
        const std::lock_guard<std::recursive_mutex> lock( candidate.capturePathState->mutex_ );
        if ( !candidate.capturePathState->mayCleanup( candidate.activityEpoch ) ) {
            continue;
        }

        if ( !candidate.capturePathState->directory_.isCurrentPath() ) {
            continue;
        }
        const auto latestModification
            = candidate.capturePathState->directory_.latestModificationTime();
        if ( cutoff.isValid() && latestModification > cutoff ) {
            continue;
        }

        if ( beforeRemoval ) {
            beforeRemoval( candidate.capturePath );
        }
        const auto directoryRemoved
            = candidate.capturePathState->directory_.removeRecursively();
        if ( directoryRemoved
             || candidate.capturePathState->directory_.isRemoved() ) {
            candidate.capturePathState->finalizeRemovedGenerationGateHeld();
        }
    }
}

void CaptureStore::cleanupCapturePaths( const QStringList& capturePaths,
                                        const QDateTime& preserveModifiedAfter )
{
    std::vector<CleanupCandidate> candidates;
    candidates.reserve( static_cast<size_t>( capturePaths.size() ) );
    for ( const auto& capturePath : capturePaths ) {
        const QFileInfo entry( capturePath );
        if ( entry.isSymLink() ) {
            continue;
        }
        const auto normalizedPath = lexicalCapturePath( capturePath );
        const auto capturePathState = CapturePathState::acquire( normalizedPath, false );
        if ( !capturePathState ) {
            continue;
        }
        CapturePathGate gate( capturePathState->gatePath() );
        if ( !gate.lock() || capturePathState->hasActiveProcessMarker() ) {
            continue;
        }
        capturePathState->retryRetiredFilesAndReleaseRegistryGateHeld();
        const auto snapshot = capturePathState->cleanupSnapshot();
        if ( snapshot ) {
            candidates.push_back( CleanupCandidate{
                normalizedPath, capturePathState, snapshot->activityEpoch,
                capturePathState->processGeneration() } );
        }
    }
    cleanupCaptureCandidates( candidates, preserveModifiedAfter );
}

void CaptureStore::cleanupUnusedCaptures( const QSet<QString>& retainCaptureIds,
                                          const QString& rootPath,
                                          const QDateTime& preserveModifiedAfter )
{
    cleanupCaptureCandidates( collectUnusedCaptureCandidates( retainCaptureIds, rootPath ),
                              preserveModifiedAfter );
}

void CaptureStore::cleanupUnusedCapturesAsync( const QSet<QString>& retainCaptureIds,
                                               const QString& rootPath,
                                               const QDateTime& preserveModifiedAfter )
{
    const auto cutoff = preserveModifiedAfter.isValid() ? preserveModifiedAfter
                                                        : QDateTime::currentDateTimeUtc();
    if ( QCoreApplication::instance() == nullptr ) {
        cleanupUnusedCaptures( retainCaptureIds, rootPath, cutoff );
        return;
    }
    scheduleCleanupUnusedCaptures( retainCaptureIds, rootPath, cutoff );
}

void CaptureStore::shutdownBackgroundWorkers()
{
    stopCaptureBackgroundThreads();
}

void CaptureStore::scheduleCleanupUnusedCaptures( const QSet<QString>& retainCaptureIds,
                                                  const QString& rootPath,
                                                  const QDateTime& preserveModifiedAfter )
{
    if ( !registerCaptureBackgroundThread() ) {
        return;
    }

    try {
        std::thread( [ retainCaptureIds, rootPath, preserveModifiedAfter ] {
            CaptureBackgroundThreadRegistration registration;
            try {
                const auto shouldStop = [] {
                    return captureBackgroundThreadsStopping();
                };
                const auto captureCandidates
                    = CaptureStore::collectUnusedCaptureCandidates(
                        retainCaptureIds, rootPath, 0, shouldStop );
                if ( !shouldStop() ) {
                    CaptureStore::cleanupCaptureCandidates(
                        captureCandidates, preserveModifiedAfter, {}, 0,
                        shouldStop );
                }
            } catch ( const std::exception& error ) {
                LOG_WARNING << "Asynchronous capture cleanup failed: "
                            << error.what();
            } catch ( ... ) {
                LOG_WARNING << "Asynchronous capture cleanup failed";
            }
        } ).detach();
    } catch ( const std::exception& error ) {
        unregisterCaptureBackgroundThread();
        LOG_WARNING << "Failed to schedule asynchronous capture cleanup: "
                    << error.what();
    } catch ( ... ) {
        unregisterCaptureBackgroundThread();
        LOG_WARNING << "Failed to schedule asynchronous capture cleanup";
    }
}

CaptureStore::CaptureStore( QString captureId, QString rootPath )
    : CaptureStore( std::move( captureId ), std::move( rootPath ), Limits{} )
{
}

CaptureStore::CaptureStore( QString captureId, QString rootPath, Limits limits )
    : captureId_( std::move( captureId ) )
    , rootPath_( rootPath.isEmpty() ? defaultRootPath() : std::move( rootPath ) )
    , capturePath_( capturePathForId( rootPath_, captureId_ ) )
    , limits_( sanitizeLimits( limits ) )
{
    // Activation precedes every directory operation so cleanup can never remove
    // a just-constructed capture between path resolution and first use. If a
    // competing cleanup wins after acquisition, retry (bounded) against the
    // successor generation.
    auto activePath = activateCapturePathState();
    capturePathState_ = std::move( activePath.state );
    capturePathActivationToken_ = std::move( activePath.activationToken );
    inheritedCaptureFiles_ = std::move( activePath.inheritedCaptureFiles );
    try {
        ensureCaptureDir( false );
        synchronizeSegmentIdsWithDisk();
    } catch ( ... ) {
        capturePathState_->deactivate( capturePathActivationToken_ );
        throw;
    }
}

CaptureStore::~CaptureStore()
{
    const std::lock_guard<std::recursive_mutex> lock( mutex_ );
    if ( persistBufferedSegmentsOnDestroy_ ) {
        try {
            finishInput();
        } catch ( const std::exception& error ) {
            LOG_ERROR << "Failed to finish capture input during destruction: " << error.what();
        } catch ( ... ) {
            LOG_ERROR << "Failed to finish capture input during destruction";
        }

        try {
            persistBufferedSegments();
        } catch ( const std::exception& error ) {
            LOG_ERROR << "Failed to persist capture segments during destruction: " << error.what();
        } catch ( ... ) {
            LOG_ERROR << "Failed to persist capture segments during destruction";
        }
    }

    try {
        flush();
    } catch ( const std::exception& error ) {
        LOG_ERROR << "Failed to flush capture output during destruction: " << error.what();
    } catch ( ... ) {
        LOG_ERROR << "Failed to flush capture output during destruction";
    }

    // Release store-owned leases before deactivation. deactivate() records the
    // lifecycle transition before its single bounded gate attempt, then retries
    // tombstones under that same gate when acquisition succeeds.
    segments_.clear();
    inheritedCaptureFiles_.clear();

    // Deactivate only after all persistence attempts, preserving the path's
    // active epoch for asynchronous cleanup that was scheduled concurrently.
    // deactivate() performs bounded gate and tombstone filesystem work that can
    // throw; a destructor must never let that escape. A failed deactivation
    // leaves the active marker behind, but it carries this process's pid, so a
    // later activation or cleanup round treats it as stale and removes it.
    try {
        capturePathState_->deactivate( capturePathActivationToken_ );
    } catch ( const std::exception& error ) {
        LOG_ERROR << "Failed to deactivate capture path during destruction: " << error.what();
    } catch ( ... ) {
        LOG_ERROR << "Failed to deactivate capture path during destruction";
    }
}

std::shared_ptr<CaptureStore::SpilledSegmentFile>
CaptureStore::spilledFileLease( const QString& filePath ) const
{
    return capturePathState_->leaseFor( filePath );
}

void CaptureStore::failNextRetiredFileRemovalForTesting()
{
    const std::lock_guard<std::recursive_mutex> lock( mutex_ );
    capturePathState_->failNextRetiredFileRemovalForTesting();
}

void CaptureStore::failNextCaptureDirectoryRemovalForTesting()
{
    const std::lock_guard<std::recursive_mutex> lock( mutex_ );
    capturePathState_->failNextCaptureDirectoryRemovalForTesting();
}

void CaptureStore::failNextCandidateRecursiveRemovalForTesting(
    const CleanupCandidate& candidate )
{
    candidate.capturePathState->failNextRecursiveDirectoryRemovalForTesting();
}

void CaptureStore::setBeforeCandidateActivationCallbackForTesting(
    const CleanupCandidate& candidate, std::function<void()> callback )
{
    candidate.capturePathState->setBeforeActivationCallbackForTesting(
        std::move( callback ) );
}

void CaptureStore::setAfterCandidateRecursiveRemovalQuarantineCallbackForTesting(
    const CleanupCandidate& candidate, std::function<void()> callback )
{
    candidate.capturePathState
        ->setAfterRecursiveRemovalQuarantineCallbackForTesting(
            std::move( callback ) );
}

void CaptureStore::failNextSegmentWriteForTesting()
{
    const std::lock_guard<std::recursive_mutex> lock( mutex_ );
    failNextSegmentWriteForTesting_ = true;
}

void CaptureStore::setAfterCaptureFilesRetiredCallbackForTesting(
    std::function<void()> callback )
{
    const std::lock_guard<std::recursive_mutex> lock( mutex_ );
    afterCaptureFilesRetiredCallbackForTesting_ = std::move( callback );
}

int CaptureStore::setCapturePathGateTimeoutForTesting( int timeoutMs )
{
    return capturePathGateTimeoutMs.exchange( timeoutMs,
                                              std::memory_order_acq_rel );
}

bool CaptureStore::hasCapturePathCoordinationOwnershipForTesting() const
{
    const std::lock_guard<std::recursive_mutex> lock( mutex_ );
    return capturePathState_->hasLocalCoordinationOwnershipForTesting();
}

QString CaptureStore::capturePathActiveMarkerPathForTesting() const
{
    const std::lock_guard<std::recursive_mutex> lock( mutex_ );
    return capturePathState_->activeMarkerPathForTesting();
}

bool CaptureStore::holdCapturePathGateForTesting(
    std::function<void()> gateAcquired,
    std::function<void()> waitForRelease )
{
    std::shared_ptr<CapturePathState> capturePathState;
    {
        const std::lock_guard<std::recursive_mutex> lock( mutex_ );
        capturePathState = capturePathState_;
    }
    CapturePathGate gate( capturePathState->gatePath() );
    if ( !gate.lock() ) {
        return false;
    }
    if ( gateAcquired ) {
        gateAcquired();
    }
    if ( waitForRelease ) {
        waitForRelease();
    }
    return true;
}

bool CaptureStore::contendForCapturePathAfterGateForTesting(
    std::function<void()> gateAcquired )
{
    std::shared_ptr<CapturePathState> capturePathState;
    {
        const std::lock_guard<std::recursive_mutex> lock( mutex_ );
        capturePathState = capturePathState_;
    }
    CapturePathGate gate( capturePathState->gatePath() );
    if ( !gate.lock() ) {
        return false;
    }
    if ( gateAcquired ) {
        gateAcquired();
    }
    const std::lock_guard<std::recursive_mutex> pathLock(
        capturePathState->mutex_ );
    return true;
}

void CaptureStore::retireSpilledSegment( Segment& segment )
{
    if ( !segment.spilled || segment.filePath.isEmpty() ) {
        return;
    }

    if ( !segment.spilledFile ) {
        segment.spilledFile = spilledFileLease( segment.filePath );
    }
    if ( segment.spilledFile ) {
        segment.spilledFile->retire( capturePathActivationToken_ );
    }
}

void CaptureStore::synchronizeSegmentIdsWithDisk()
{
    const std::lock_guard<std::recursive_mutex> pathLock( capturePathState_->mutex_ );
    const auto segmentFiles = capturePathState_->directory_.entryList(
        QStringList{ "segment_*.log" }, QDir::Files,
        QDir::Name | QDir::IgnoreCase );
    for ( const auto& fileName : segmentFiles ) {
        if ( const auto segmentId = segmentIdFromFileName( fileName ) ) {
            capturePathState_->observeSegmentId( *segmentId );
        }
    }
}

std::vector<std::shared_ptr<CaptureStore::SpilledSegmentFile>>
CaptureStore::retireCaptureFiles()
{
    std::vector<std::shared_ptr<SpilledSegmentFile>> retiredLeases;
    const std::lock_guard<std::recursive_mutex> pathLock( capturePathState_->mutex_ );
    for ( auto& segment : segments_ ) {
        retireSpilledSegment( segment );
        if ( segment.spilledFile ) {
            retiredLeases.push_back( segment.spilledFile );
        }
    }

    for ( const auto& filePath : inheritedCaptureFiles_ ) {
        auto spilledFile = spilledFileLease( filePath );
        if ( spilledFile ) {
            spilledFile->retire( capturePathActivationToken_ );
            retiredLeases.push_back( std::move( spilledFile ) );
        }
    }
    inheritedCaptureFiles_.clear();

    auto callback = std::move( afterCaptureFilesRetiredCallbackForTesting_ );
    afterCaptureFilesRetiredCallbackForTesting_ = {};
    if ( callback ) {
        callback();
    }
    return retiredLeases;
}

bool CaptureStore::loadFromDisk()
{
    const std::lock_guard<std::recursive_mutex> lock( mutex_ );
    capturePathState_->retryRetiredFilesAndReleaseRegistry();
    decltype( segments_ ) oldSegments;
    {
        const std::lock_guard<std::recursive_mutex> pathLock(
            capturePathState_->mutex_ );
        ensureCaptureDir( false );

        oldSegments = std::move( segments_ );
        partialLine_.clear();
        reservedSegmentIds_.clear();
        fileSize_ = 0;
        memoryBytes_ = 0;
        totalLines_ = 0;
        maxLineLength_ = 0;
        lastModified_ = QDateTime{};

        std::vector<std::pair<qint64, QString>> segmentFiles;
        auto fileNames = capturePathState_->directory_.entryList(
            QStringList{ "segment_*.log" }, QDir::Files,
            QDir::NoSort );
        std::sort( fileNames.begin(), fileNames.end() );
        segmentFiles.reserve( static_cast<size_t>( fileNames.size() ) );
        QSet<qint64> observedSegmentIds;
        for ( const auto& fileName : fileNames ) {
            const auto segmentId = segmentIdFromFileName( fileName );
            if ( !segmentId ) {
                LOG_WARNING << "Ignoring capture segment with noncanonical name "
                            << fileName;
                continue;
            }
            if ( observedSegmentIds.contains( *segmentId ) ) {
                LOG_WARNING << "Ignoring capture segment with duplicate id "
                            << fileName;
                continue;
            }
            observedSegmentIds.insert( *segmentId );
            capturePathState_->observeSegmentId( *segmentId );
            segmentFiles.emplace_back( *segmentId, fileName );
        }
        std::sort(
            segmentFiles.begin(), segmentFiles.end(),
            []( const auto& lhs, const auto& rhs ) {
                return lhs.first < rhs.first;
            } );

        for ( const auto& [ segmentId, fileName ] : segmentFiles ) {
            Segment segment;
            segment.id = segmentId;
            segment.filePath = QDir( capturePath_ ).filePath( fileName );
            if ( !capturePathState_->isTombstoned( segment.filePath )
                 && scanSegment( segment ) ) {
                segments_.push_back( std::move( segment ) );
            }
        }

        rebuildCumulativeLineCounts();
        enforceMemoryBudget();
    }
    oldSegments.clear();
    return true;
}

CaptureStore::AppendResult CaptureStore::appendUtf8( const QByteArray& data )
{
    const std::lock_guard<std::recursive_mutex> lock( mutex_ );
    AppendResult appendResult;
    appendResult.firstLine = LineNumber( static_cast<LineNumber::UnderlyingType>( totalLines_ ) );
    if ( data.isEmpty() ) {
        return appendResult;
    }

    appendResult.rawUtf8Lines.reserve( partialLine_.size() + data.size() );
    appendResult.endOfLines.reserve( static_cast<size_t>( qMax<qsizetype>( 1, data.size() / 32 ) ) );

    const auto originalLineCount = totalLines_;

    const auto processBuffer = [ &appendResult ]( const QByteArray& buffer ) -> qsizetype {
        qsizetype lineStart = 0;
        const auto* const bufferData = buffer.constData();
        const auto bufferSize = buffer.size();
        klogg::vector<qint64> lineEnds;
        lineEnds.reserve( static_cast<size_t>( qMax<qsizetype>( 1, bufferSize / 32 ) ) );
        bool needsNormalization = false;

        while ( lineStart < bufferSize ) {
            const auto remaining = bufferSize - lineStart;
            const auto* newline = static_cast<const char*>(
                std::memchr( bufferData + lineStart, '\n', static_cast<size_t>( remaining ) ) );
            if ( newline == nullptr ) {
                break;
            }

            auto lineLength = static_cast<qsizetype>( newline - ( bufferData + lineStart ) );
            if ( lineLength > 0 && bufferData[ lineStart + lineLength - 1 ] == '\r' ) {
                needsNormalization = true;
            }
            lineEnds.push_back( static_cast<qint64>( newline - bufferData ) + 1 );
            lineStart = static_cast<qsizetype>( newline - bufferData ) + 1;
        }

        if ( lineEnds.empty() ) {
            return lineStart;
        }

        if ( !needsNormalization ) {
            const auto outputStart = appendResult.rawUtf8Lines.size();
            appendResult.rawUtf8Lines.append( bufferData, type_safe::narrow_cast<int>( lineStart ) );
            for ( const auto lineEnd : lineEnds ) {
                appendResult.endOfLines.push_back( outputStart + lineEnd );
            }
            return lineStart;
        }

        qsizetype normalizedLineStart = 0;
        for ( const auto lineEnd : lineEnds ) {
            auto lineLength = static_cast<qsizetype>( lineEnd ) - normalizedLineStart - 1;
            if ( lineLength > 0 && bufferData[ normalizedLineStart + lineLength - 1 ] == '\r' ) {
                --lineLength;
            }

            appendResult.rawUtf8Lines.append( bufferData + normalizedLineStart,
                                               type_safe::narrow_cast<int>( lineLength ) );
            appendResult.rawUtf8Lines.append( '\n' );
            appendResult.endOfLines.push_back( appendResult.rawUtf8Lines.size() );
            normalizedLineStart = static_cast<qsizetype>( lineEnd );
        }

        return lineStart;
    };

    QByteArray pendingPartialLine;
    if ( partialLine_.isEmpty() ) {
        const auto consumed = processBuffer( data );
        if ( consumed < data.size() ) {
            pendingPartialLine = data.mid( type_safe::narrow_cast<int>( consumed ) );
        }
    } else {
        auto combinedData = partialLine_;
        combinedData.append( data );
        const auto consumed = processBuffer( combinedData );
        if ( consumed < combinedData.size() ) {
            pendingPartialLine
                = combinedData.mid( type_safe::narrow_cast<int>( consumed ) );
        }
    }

    appendResult.lineCount
        = LinesCount( static_cast<LinesCount::UnderlyingType>( appendResult.endOfLines.size() ) );
    ensureSegmentIdsAvailable( appendResult,
                               static_cast<qint64>( pendingPartialLine.size() ) );

    persistBufferedSegmentsOnDestroy_ = true;
    partialLine_ = std::move( pendingPartialLine );
    commitLines( appendResult );
    appendResult.firstLine = tailFirstLine( totalLines_, appendResult.lineCount );
    if ( totalLines_ != originalLineCount ) {
        lastModified_ = QDateTime::currentDateTime();
    }
    return appendResult;
}

CaptureStore::AppendResult CaptureStore::finishInput()
{
    const std::lock_guard<std::recursive_mutex> lock( mutex_ );
    AppendResult appendResult;
    appendResult.firstLine = LineNumber( static_cast<LineNumber::UnderlyingType>( totalLines_ ) );
    if ( !partialLine_.isEmpty() ) {
        auto lineBytes = partialLine_;
        if ( lineBytes.endsWith( '\r' ) ) {
            lineBytes.chop( 1 );
        }

        // Reserve before any mutation.  In particular, exhausting the shared
        // ID space leaves partialLine_ intact so a later limit change can retry.
        AppendResult reservation;
        reservation.rawUtf8Lines = lineBytes;
        reservation.endOfLines.push_back( reservation.rawUtf8Lines.size() );
        ensureSegmentIdsAvailable( reservation, 0 );

        commitLine( lineBytes, false );
        partialLine_.clear();
        appendResult.rawUtf8Lines = std::move( reservation.rawUtf8Lines );
        appendResult.rawUtf8Lines.append( '\n' );
        appendResult.endOfLines.clear();
        appendResult.endOfLines.push_back( appendResult.rawUtf8Lines.size() );
        appendResult.lineCount = 1_lcount;
        lastModified_ = QDateTime::currentDateTime();
    }
    appendResult.firstLine = tailFirstLine( totalLines_, appendResult.lineCount );

    // Flush any pending output data
    if ( rollingOutput_.isValid() && unflushedOutputBytes_ > 0 ) {
        if ( !rollingOutput_.flush() ) {
            LOG_WARNING << "Rolling output file flush failed in finishInput, unbinding: "
                        << boundOutputFile_;
            rollingOutput_.close();
            rollingOutput_ = RollingFileManager();
            boundOutputFile_.clear();
        }
        resetOutputFlushCounters();
    }
    return appendResult;
}

void CaptureStore::flush()
{
    const std::lock_guard<std::recursive_mutex> lock( mutex_ );
    if ( rollingOutput_.isValid() && unflushedOutputBytes_ > 0 ) {
        if ( !rollingOutput_.flush() ) {
            LOG_WARNING << "Rolling output file flush failed in flush(), unbinding: "
                        << boundOutputFile_;
            rollingOutput_.close();
            rollingOutput_ = RollingFileManager();
            boundOutputFile_.clear();
        }
        resetOutputFlushCounters();
    }
}

void CaptureStore::clear()
{
    const std::lock_guard<std::recursive_mutex> lock( mutex_ );
    flush();
    capturePathState_->retryRetiredFilesAndReleaseRegistry();

    partialLine_.clear();
    auto retiredLeases = retireCaptureFiles();
    segments_.clear();
    fileSize_ = 0;
    memoryBytes_ = 0;
    totalLines_ = 0;
    maxLineLength_ = 0;
    lastModified_ = QDateTime::currentDateTime();
    lastTrimResult_ = {};

    if ( !boundOutputFile_.isEmpty() ) {
        rollingOutput_.deleteAll();
        rollingOutput_ = RollingFileManager( boundOutputFile_, limits_.rollingMaxFileSize,
                                             limits_.rollingBackupCount );
        if ( !rollingOutput_.open( true ) ) {
            LOG_WARNING << "CaptureStore::clear: failed to reopen output file: "
                        << boundOutputFile_;
            boundOutputFile_.clear();
        }
    }
}

CaptureStore::TrimResult CaptureStore::trimToLimits()
{
    const std::lock_guard<std::recursive_mutex> lock( mutex_ );
    capturePathState_->retryRetiredFilesAndReleaseRegistry();
    std::vector<std::shared_ptr<SpilledSegmentFile>> retiredLeases;
    TrimResult result;

    if ( segments_.empty()
         || ( segments_.size() == 1
              && ( preserveTailDuringTrim_ || !needsNewSegment() ) ) ) {
        return result;
    }

    // Rolling window: rollingMaxFileSize * rollingBackupCount.
    // backupCount is the number of retained backups; the current file is
    // tracked separately via fileSize_.  Use checked multiplication to guard
    // against overflow from hand-edited or session-restored limits.
    qint64 windowBytes = 0;
    if ( limits_.rollingMaxFileSize > 0 && limits_.rollingBackupCount > 0 ) {
        const auto backupCount = static_cast<qint64>( limits_.rollingBackupCount );
        windowBytes
            = limits_.rollingMaxFileSize > std::numeric_limits<qint64>::max() / backupCount
                  ? std::numeric_limits<qint64>::max()
                  : limits_.rollingMaxFileSize * backupCount;
    }
    const auto exceedsBytes = windowBytes > 0 && fileSize_ > windowBytes;
    const auto exceedsLines = limits_.maxTotalLines > 0 && totalLines_ > limits_.maxTotalLines;

    if ( !exceedsBytes && !exceedsLines ) {
        return result;
    }

    LinesCount::UnderlyingType totalRemovedLines = 0;
    bool removedMaxLine = false;

    // Remove oldest segments (FIFO) until within limits. Preserve the last
    // segment only while it is still mutable; a full/spilled tail is closed and
    // can be trimmed even when it is the sole segment.
    while ( !segments_.empty() ) {
        if ( segments_.size() == 1
              && ( preserveTailDuringTrim_ || !needsNewSegment() ) ) {
            break;
        }
        const auto stillOverBytes = windowBytes > 0 && fileSize_ > windowBytes;
        const auto stillOverLines
            = limits_.maxTotalLines > 0 && totalLines_ > limits_.maxTotalLines;

        if ( !stillOverBytes && !stillOverLines ) {
            break;
        }

        auto& front = segments_.front();
        const auto segmentLines = klogg::ssize( front.lineOffsets );
        const auto segmentBytes = front.byteSize;

        // Track if we're removing the segment that held maxLineLength
        for ( const auto len : front.lineLengths ) {
            if ( len == maxLineLength_ ) {
                removedMaxLine = true;
            }
        }

        // Remove the segment from the logical window immediately. A raw-line
        // reader may still hold a lease, in which case physical deletion waits
        // until its unlocked file read completes.
        retireSpilledSegment( front );
        if ( front.spilledFile ) {
            retiredLeases.push_back( front.spilledFile );
        }

        fileSize_ -= segmentBytes;
        if ( front.memoryData ) {
            memoryBytes_ -= front.memoryData->size();
        }
        totalLines_ -= segmentLines;
        totalRemovedLines += static_cast<LinesCount::UnderlyingType>( segmentLines );

        result.trimmedLines = result.trimmedLines + LinesCount( static_cast<LinesCount::UnderlyingType>( segmentLines ) );
        result.trimmedBytes += segmentBytes;

        segments_.erase( segments_.begin() );
    }

    // O(1) cumulative line count fixup: subtract removed lines from all remaining segments
    if ( totalRemovedLines > 0 ) {
        for ( auto& segment : segments_ ) {
            segment.cumulativeEndLine
                -= static_cast<qint64>( totalRemovedLines );
        }
    }

    // Only recompute maxLineLength if we removed the segment that held it
    if ( removedMaxLine ) {
        maxLineLength_ = 0;
        for ( const auto& segment : segments_ ) {
            for ( const auto len : segment.lineLengths ) {
                maxLineLength_ = qMax( maxLineLength_, len );
            }
        }
    }

    if ( result.trimmedLines > 0_lcount ) {
        lastModified_ = QDateTime::currentDateTime();
        lastTrimResult_ = result;

        LOG_INFO << "CaptureStore trimmed " << result.trimmedLines.get() << " lines ("
                 << result.trimmedBytes << " bytes), remaining: " << totalLines_ << " lines, "
                 << fileSize_ << " bytes";
    }

    return result;
}

CaptureStore::TrimResult CaptureStore::lastTrimResult() const
{
    const std::lock_guard<std::recursive_mutex> lock( mutex_ );
    return lastTrimResult_;
}

void CaptureStore::clearTrimResult()
{
    const std::lock_guard<std::recursive_mutex> lock( mutex_ );
    lastTrimResult_ = {};
}

bool CaptureStore::bindOutputFile( const QString& outputPath, bool preserveExisting )
{
    const std::lock_guard<std::recursive_mutex> lock( mutex_ );
    rollingOutput_.close();
    rollingOutput_ = RollingFileManager();
    boundOutputFile_ = outputPath;
    if ( boundOutputFile_.isEmpty() ) {
        return true;
    }

    QDir().mkpath( QFileInfo( boundOutputFile_ ).absolutePath() );

    rollingOutput_ = RollingFileManager( boundOutputFile_, limits_.rollingMaxFileSize,
                                         limits_.rollingBackupCount );
    // FreshSave truncates and replays the capture; Restore opens append-only so
    // previously streamed content already on disk is never destroyed (the
    // capture is volatile — it lives in the OS temp dir and may be empty on
    // restart, which would otherwise empty the file).
    if ( !rollingOutput_.open( /*truncate=*/!preserveExisting ) ) {
        boundOutputFile_.clear();
        return false;
    }

    if ( !preserveExisting ) {
        // Write existing segments to the rolling file
        for ( const auto& segment : segments_ ) {
            if ( !writeSegmentToDevice( segment, rollingOutput_.currentFile() ) ) {
                boundOutputFile_.clear();
                rollingOutput_.close();
                return false;
            }
        }
    }
    // Replay writes directly to QFile, bypassing RollingFileManager::write().
    // Sync the byte counter so needsRotation() reflects the true file size.
    rollingOutput_.resyncSize();
    rollingOutput_.flush();

    resetOutputFlushCounters();
    return true;
}

void CaptureStore::setLimits( Limits limits )
{
    const std::lock_guard<std::recursive_mutex> lock( mutex_ );
    limits_ = sanitizeLimits( std::move( limits ) );
}

QString CaptureStore::boundOutputFile() const
{
    const std::lock_guard<std::recursive_mutex> lock( mutex_ );
    return boundOutputFile_;
}

QString CaptureStore::captureId() const
{
    return captureId_;
}

QString CaptureStore::capturePath() const
{
    return capturePath_;
}

QString CaptureStore::rootPath() const
{
    return rootPath_;
}

QString CaptureStore::capturePathIdentity() const
{
    const std::lock_guard<std::recursive_mutex> lock( mutex_ );
    return capturePathState_->registryKey_;
}

void CaptureStore::deleteCaptureFiles()
{
    const std::lock_guard<std::recursive_mutex> lock( mutex_ );
    flush();
    rollingOutput_.close();
    rollingOutput_ = RollingFileManager();
    // Bind delayed removal to the current cross-process generation before
    // taking the path mutex; every removal path follows gate -> mutex order.
    capturePathState_->requestDirectoryDeletion(
        capturePathActivationToken_ );
    persistBufferedSegmentsOnDestroy_ = false;
    partialLine_.clear();

    auto retiredLeases = retireCaptureFiles();
    segments_.clear();
    fileSize_ = 0;
    memoryBytes_ = 0;
    totalLines_ = 0;
    maxLineLength_ = 0;
    lastModified_ = QDateTime{};
}

SearchableLogData::RawLines CaptureStore::buildRawLines( LineNumber first, LinesCount number,
                                                         QTextCodec* codec,
                                                         const QRegularExpression& prefilterPattern ) const
{
    // Segment-based batch read: identify contiguous byte ranges per segment
    // under the lock, then bulk-copy directly into the output buffer.
    // This avoids per-line QByteArray allocations that dominate the old path.

    struct SegmentRead {
        QByteArray data;
        std::shared_ptr<SpilledSegmentFile> spilledFile;
        qint64 byteStart = 0;
        qint64 byteLength = 0;
        qint64 lineCount = 0;
    };

    LinesCount::UnderlyingType totalRequestedLines = 0;
    std::vector<SegmentRead> segmentReads;
    std::function<void()> beforeRawSnapshotCopyCallback;
    std::function<void()> beforeSpilledSegmentReadCallback;

    {
        const std::lock_guard<std::recursive_mutex> lock( mutex_ );
        beforeRawSnapshotCopyCallback
            = std::move( beforeRawSnapshotCopyCallbackForTesting_ );
        beforeSpilledSegmentReadCallback
            = std::move( beforeSpilledSegmentReadCallbackForTesting_ );
        const auto totalLines = lineCount();
        const auto availableLines = qMax<LineNumber::UnderlyingType>(
            0, totalLines.get() - qMin( first.get(), totalLines.get() ) );
        totalRequestedLines
            = qMin( number.get(), static_cast<LinesCount::UnderlyingType>( availableLines ) );

        if ( totalRequestedLines == 0 ) {
            SearchableLogData::RawLines rawLines;
            rawLines.startLine = first;
            return rawLines;
        }

        segmentReads.reserve( 4 );

        auto segIt = std::lower_bound(
            segments_.cbegin(), segments_.cend(), first.get(),
            []( const Segment& segment, qint64 value ) {
                return segment.cumulativeEndLine <= value;
            } );

        if ( segIt == segments_.cend() ) {
            SearchableLogData::RawLines rawLines;
            rawLines.startLine = first;
            return rawLines;
        }

        LinesCount::UnderlyingType linesRemaining = totalRequestedLines;
        auto currentLine = first;

        while ( linesRemaining > 0 && segIt != segments_.cend() ) {
            const auto segIdx
                = static_cast<size_t>( std::distance( segments_.cbegin(), segIt ) );
            const qint64 prevEndLine
                = segIdx == 0 ? 0LL : segments_[ segIdx - 1 ].cumulativeEndLine;

            const auto localStart = static_cast<int>( currentLine.get<qint64>() - prevEndLine );
            const auto linesInThisSegment
                = qMin( linesRemaining,
                        static_cast<LinesCount::UnderlyingType>(
                            klogg::ssize( segIt->lineOffsets ) - localStart ) );

            if ( linesInThisSegment <= 0 || localStart < 0
                 || localStart >= klogg::ssize( segIt->lineOffsets ) ) {
                break;
            }

            SegmentRead read;
            read.lineCount = static_cast<qint64>( linesInThisSegment );

            const auto byteStart
                = segIt->lineOffsets[ static_cast<size_t>( localStart ) ];
            const auto lastLocalLine = localStart + static_cast<int>( linesInThisSegment ) - 1;

            qint64 byteLength;
            const auto nextLocalLine = static_cast<size_t>( lastLocalLine ) + 1U;
            if ( nextLocalLine < segIt->lineOffsets.size() ) {
                byteLength = segIt->lineOffsets[ nextLocalLine ] - byteStart;
            } else {
                byteLength = segIt->byteSize - byteStart;
            }

            read.byteStart = byteStart;
            read.byteLength = byteLength;

            // Snapshot mutable memory while protected by mutex_. A shared_ptr
            // copy preserves only the QByteArray object's lifetime; append()
            // could still reallocate its backing storage after unlock.
            if ( segIt->memoryData ) {
                read.data = segIt->memoryData->mid(
                    type_safe::narrow_cast<int>( read.byteStart ),
                    type_safe::narrow_cast<int>( read.byteLength ) );
                // QByteArray::mid() may retain the source allocation when the
                // whole array is selected. Force a deep copy before mutex_ is
                // released so later append() calls cannot mutate this snapshot.
                read.data.detach();
            } else {
                // Spilled files are immutable. Copying the lease pins this exact
                // path across trim/clear/delete while slow file I/O runs unlocked.
                read.spilledFile = segIt->spilledFile;
            }

            segmentReads.push_back( std::move( read ) );

            linesRemaining -= linesInThisSegment;
            currentLine = currentLine + LinesCount( linesInThisSegment );
            ++segIt;
        }
    }
    // mutex released

    for ( auto& read : segmentReads ) {
        if ( !read.spilledFile ) {
            continue;
        }

        if ( beforeSpilledSegmentReadCallback ) {
            auto callback = std::move( beforeSpilledSegmentReadCallback );
            beforeSpilledSegmentReadCallback = {};
            callback();
        }

        if ( auto file = read.spilledFile->openForRead() ) {
            file->seek( read.byteStart );
            read.data = file->read( read.byteLength );
        }
    }

    const auto effectiveCodec = codec ? codec : QTextCodec::codecForName( "UTF-8" );
    const auto sourceEncodingParams = EncodingParameters( effectiveCodec );
    const auto canUseRawUtf8
        = sourceEncodingParams.isUtf8Compatible && prefilterPattern.pattern().isEmpty();

    SearchableLogData::RawLines rawLines;
    rawLines.startLine = first;
    auto* utf8Codec = QTextCodec::codecForName( "UTF-8" );
    rawLines.textDecoder.decoder.reset( utf8Codec->makeDecoder() );
    rawLines.textDecoder.encodingParams = sourceEncodingParams;
    rawLines.textDecoder.encodingParams.isUtf8Compatible = true;
    rawLines.textDecoder.encodingParams.lineFeedWidth = 1;
    rawLines.prefilterPattern = prefilterPattern;

    if ( canUseRawUtf8 ) {
        // Fast path: bulk copy from segments, derive endOfLines from \n scanning.
        // Segment data stores each line as content+\n, so scanning for \n gives
        // exact line boundaries without per-line allocation.
        qint64 totalBytes = 0;
        for ( const auto& read : segmentReads ) {
            totalBytes += read.data.size();
        }
        rawLines.buffer.reserve( static_cast<size_t>( totalBytes ) );
        rawLines.endOfLines.reserve( static_cast<size_t>( totalRequestedLines ) );

        for ( const auto& read : segmentReads ) {
            const auto outputStart = klogg::ssize( rawLines.buffer );

            if ( !read.data.isEmpty() ) {
                const auto* const sourceBegin = read.data.constData();
                const auto* const sourceEnd = sourceBegin + read.data.size();
                if ( beforeRawSnapshotCopyCallback ) {
                    auto callback = std::move( beforeRawSnapshotCopyCallback );
                    beforeRawSnapshotCopyCallback = {};
                    callback();
                }
                rawLines.buffer.insert( rawLines.buffer.end(), sourceBegin, sourceEnd );
            }

            // Scan for \n to compute endOfLines
            const auto* scanStart = rawLines.buffer.data() + outputStart;
            const auto scanLength = klogg::ssize( rawLines.buffer ) - outputStart;
            qint64 linesFound = 0;
            for ( qint64 pos = 0; pos < scanLength && linesFound < read.lineCount; ++pos ) {
                if ( scanStart[ pos ] == '\n' ) {
                    rawLines.endOfLines.push_back( outputStart + pos + 1 );
                    ++linesFound;
                }
            }

            // Handle unterminated last line
            if ( linesFound < read.lineCount ) {
                rawLines.buffer.push_back( '\n' );
                rawLines.endOfLines.push_back( klogg::ssize( rawLines.buffer ) );
            }
        }
    } else {
        // Slow path: bulk read per segment, per-line codec conversion or prefilter
        for ( const auto& read : segmentReads ) {
            const auto& segmentData = read.data;

            qint64 lineStart = 0;
            for ( qint64 lineIdx = 0; lineIdx < read.lineCount; ++lineIdx ) {
                const auto remaining = segmentData.size() - static_cast<int>( lineStart );
                if ( remaining <= 0 ) {
                    break;
                }
                const auto* nl = static_cast<const char*>(
                    std::memchr( segmentData.constData() + static_cast<int>( lineStart ), '\n',
                                 static_cast<size_t>( remaining ) ) );
                qint64 lineEnd;
                if ( nl ) {
                    lineEnd = nl - segmentData.constData();
                } else {
                    lineEnd = segmentData.size();
                }

                auto lineLength = lineEnd - lineStart;
                if ( lineLength > 0 && segmentData[ static_cast<int>( lineStart + lineLength - 1 ) ] == '\r' ) {
                    --lineLength;
                }

                const auto utf8Line = segmentData.mid( static_cast<int>( lineStart ),
                                                       static_cast<int>( lineLength ) );
                const auto lineStr = decodeUtf8Line( utf8Line, effectiveCodec, prefilterPattern );
                const auto lineUtf8 = lineStr.toUtf8();
                rawLines.buffer.insert( rawLines.buffer.end(), lineUtf8.begin(), lineUtf8.end() );
                rawLines.buffer.push_back( '\n' );
                rawLines.endOfLines.push_back( klogg::ssize( rawLines.buffer ) );

                lineStart = lineEnd + 1;
            }
        }
    }

    return rawLines;
}

QString CaptureStore::lineAt( LineNumber line, QTextCodec* codec,
                              const QRegularExpression& prefilterPattern ) const
{
    const std::lock_guard<std::recursive_mutex> lock( mutex_ );
    if ( line < 0_lnum || line >= lineCount() ) {
        return {};
    }

    const auto segmentIt
        = std::lower_bound( segments_.cbegin(), segments_.cend(), line.get(),
                            []( const Segment& segment, qint64 value ) {
                                return segment.cumulativeEndLine <= value;
                            } );
    if ( segmentIt == segments_.cend() ) {
        return {};
    }

    const auto segmentIndex = static_cast<size_t>( std::distance( segments_.cbegin(), segmentIt ) );
    const qint64 previousEndLine
        = segmentIndex == 0 ? 0LL : segments_[ segmentIndex - 1 ].cumulativeEndLine;
    const auto localLine = static_cast<int>( line.get<qint64>() - previousEndLine );

    const auto utf8Line = readSegmentLine( *segmentIt, localLine );
    return decodeUtf8Line( utf8Line, codec, prefilterPattern );
}

LineLength CaptureStore::lineLength( LineNumber line ) const
{
    const std::lock_guard<std::recursive_mutex> lock( mutex_ );
    if ( line < 0_lnum || line >= lineCount() ) {
        return 0_length;
    }

    const auto segmentIt
        = std::lower_bound( segments_.cbegin(), segments_.cend(), line.get(),
                            []( const Segment& segment, qint64 value ) {
                                return segment.cumulativeEndLine <= value;
                            } );
    if ( segmentIt == segments_.cend() ) {
        return 0_length;
    }

    const auto segmentIndex = static_cast<size_t>( std::distance( segments_.cbegin(), segmentIt ) );
    const qint64 previousEndLine
        = segmentIndex == 0 ? 0LL : segments_[ segmentIndex - 1 ].cumulativeEndLine;
    const auto localLine = static_cast<int>( line.get<qint64>() - previousEndLine );
    if ( localLine < 0 || localLine >= klogg::isize( segmentIt->lineLengths ) ) {
        return 0_length;
    }
    return LineLength( segmentIt->lineLengths[ static_cast<size_t>( localLine ) ] );
}

LinesCount CaptureStore::lineCount() const
{
    const std::lock_guard<std::recursive_mutex> lock( mutex_ );
    return LinesCount( static_cast<LinesCount::UnderlyingType>( totalLines_ ) );
}

LineLength CaptureStore::maxLineLength() const
{
    const std::lock_guard<std::recursive_mutex> lock( mutex_ );
    return LineLength( maxLineLength_ );
}

CaptureStore::Stats CaptureStore::stats() const
{
    const std::lock_guard<std::recursive_mutex> lock( mutex_ );
    return Stats{ fileSize_, memoryBytes_, totalLines_, maxLineLength_, lastModified_ };
}

void CaptureStore::commitLine( const QByteArray& lineBytes, bool terminated )
{
    auto& segment = ensureActiveSegment();
    if ( !segment.memoryData ) {
        segment.memoryData = std::make_shared<QByteArray>();
        reserveSegmentMemory( *segment.memoryData, limits_.segmentTargetBytes,
                              limits_.memoryBudgetBytes );
    }

    const auto offset = segment.memoryData->size();
    segment.memoryData->append( lineBytes );
    if ( terminated ) {
        segment.memoryData->append( '\n' );
    }
    segment.lineOffsets.push_back( offset );
    segment.lineLengths.push_back( static_cast<int>( lineBytes.size() ) );
    segment.byteSize = segment.memoryData->size();
    segment.spilled = false;

    fileSize_ += lineBytes.size() + ( terminated ? 1 : 0 );
    memoryBytes_ += lineBytes.size() + ( terminated ? 1 : 0 );
    totalLines_ += 1;
    maxLineLength_ = qMax( maxLineLength_, static_cast<int>( lineBytes.size() ) );

    // Complete every access through the segment reference before operations
    // that can trim or grow segments_ and invalidate it. Keep the committed
    // segment active while rolling output trims the capture window; rotating it
    // first would make the just-committed line eligible for immediate removal.
    segment.cumulativeEndLine += 1;

    if ( rollingOutput_.isValid() ) {
        preserveTailDuringTrim_ = true;
        try {
            appendOutputBytes( terminated ? lineBytes + '\n' : lineBytes );
        } catch ( ... ) {
            preserveTailDuringTrim_ = false;
            throw;
        }
        preserveTailDuringTrim_ = false;
    }

    if ( memoryBytes_ > limits_.memoryBudgetBytes ) {
        enforceMemoryBudget();
    }
}

void CaptureStore::commitLines( const AppendResult& appendResult )
{
    if ( appendResult.endOfLines.empty() ) {
        return;
    }

    if ( rollingOutput_.isValid() ) {
        if ( appendResult.endOfLines.size()
             > static_cast<size_t>( std::numeric_limits<int>::max() ) ) {
            throw std::runtime_error( "Too many output lines while committing capture batch" );
        }
        appendOutputBytes( appendResult.rawUtf8Lines,
                           static_cast<int>( appendResult.endOfLines.size() ) );
    }

    size_t lineIndex = 0;
    qint64 rawLineStart = 0;
    const auto lineCount = appendResult.endOfLines.size();

    while ( lineIndex < lineCount ) {
        auto& segment = ensureActiveSegment();
        if ( !segment.memoryData ) {
            segment.memoryData = std::make_shared<QByteArray>();
            reserveSegmentMemory( *segment.memoryData, limits_.segmentTargetBytes,
                                  limits_.memoryBudgetBytes );
        }

        const auto segmentRawStart = rawLineStart;
        const auto firstLineInSegment = lineIndex;
        auto segmentBytes = qint64{ 0 };
        auto segmentMaxLineLength = 0;

        while ( lineIndex < lineCount ) {
            const auto lineEnd = appendResult.endOfLines[ lineIndex ];
            const auto lineBytes = lineEnd - rawLineStart;
            if ( lineIndex > firstLineInSegment
                 && segment.byteSize + segmentBytes + lineBytes > limits_.segmentTargetBytes ) {
                break;
            }

            if ( lineBytes <= 0 || lineBytes - 1 > std::numeric_limits<int>::max() ) {
                throw std::runtime_error( "Invalid append line length while committing capture batch" );
            }
            segmentMaxLineLength = qMax( segmentMaxLineLength,
                                         type_safe::narrow_cast<int>( lineBytes - 1 ) );
            segmentBytes += lineBytes;
            rawLineStart = lineEnd;
            ++lineIndex;

            if ( segment.byteSize + segmentBytes >= limits_.segmentTargetBytes ) {
                break;
            }
        }

        const auto segmentOffset = segment.memoryData->size();
        if ( segmentBytes < 0 || segmentBytes > std::numeric_limits<int>::max() ) {
            throw std::runtime_error( "Invalid segment byte count while committing capture batch" );
        }
        segment.memoryData->append( appendResult.rawUtf8Lines.constData() + segmentRawStart,
                                    type_safe::narrow_cast<int>( segmentBytes ) );

        qint64 localRawStart = segmentRawStart;
        qint64 localOffset = segmentOffset;
        for ( auto metadataIndex = firstLineInSegment; metadataIndex < lineIndex;
              ++metadataIndex ) {
            const auto lineEnd = appendResult.endOfLines[ metadataIndex ];
            const auto lineBytes = lineEnd - localRawStart;
            if ( lineBytes <= 0 || lineBytes - 1 > std::numeric_limits<int>::max() ) {
                throw std::runtime_error( "Invalid metadata line length while committing capture batch" );
            }
            segment.lineOffsets.push_back( localOffset );
            segment.lineLengths.push_back( type_safe::narrow_cast<int>( lineBytes - 1 ) );
            localOffset += lineBytes;
            localRawStart = lineEnd;
        }

        const auto committedLines
            = static_cast<qint64>( lineIndex - firstLineInSegment );
        segment.byteSize = segment.memoryData->size();
        segment.spilled = false;
        segment.cumulativeEndLine += committedLines;

        fileSize_ += segmentBytes;
        memoryBytes_ += segmentBytes;
        totalLines_ += committedLines;
        maxLineLength_ = qMax( maxLineLength_, segmentMaxLineLength );

        // Throttle spill operations to avoid frequent small spills under heavy load.
        // Emergency spill if memory exceeds 2x budget.
        if ( memoryBytes_ > limits_.memoryBudgetBytes ) {
            const auto now = QDateTime::currentMSecsSinceEpoch();
            if ( memoryBytes_ > limits_.memoryBudgetBytes * 2
                 || now - lastSpillTimeMs_ >= SpillThrottleMs ) {
                lastSpillTimeMs_ = now;
                enforceMemoryBudget();
            }
        }
    }

    // Trim oldest segments if total limits are exceeded
    if ( limits_.rollingMaxFileSize > 0 || limits_.maxTotalLines > 0 ) {
        trimToLimits();
    }
}

CaptureStore::ActiveCapturePath CaptureStore::activateCapturePathState()
{
    for ( int attempt = 0; attempt < CaptureActivationMaxAttempts; ++attempt ) {
        auto candidateState = CapturePathState::acquire( capturePath_ );
        auto activationResult = candidateState->activate();
        if ( activationResult ) {
            return { std::move( candidateState ),
                     std::move( activationResult->activationToken ),
                     std::move( activationResult->inheritedCaptureFiles ) };
        }
        // A competing cleanup is still tearing down the previous generation;
        // give it time to finalize before retrying against its successor.
        std::this_thread::sleep_for( CaptureActivationRetryDelay );
    }
    throw std::runtime_error( "Capture directory generation was removed" );
}

void CaptureStore::ensureCaptureDir( bool startsReplacement )
{
    if ( capturePathState_->isTerminallyRemoved() ) {
        if ( !startsReplacement ) {
            throw std::runtime_error( "Capture directory generation was removed" );
        }

        auto activePath = activateCapturePathState();
        auto previousState = std::move( capturePathState_ );
        auto previousActivationToken
            = std::move( capturePathActivationToken_ );
        capturePathState_ = std::move( activePath.state );
        capturePathActivationToken_
            = std::move( activePath.activationToken );
        inheritedCaptureFiles_
            = std::move( activePath.inheritedCaptureFiles );
        reservedSegmentIds_.clear();
        previousState->deactivate( previousActivationToken );
    }
    capturePathState_->ensureDirectory( startsReplacement );
}

bool CaptureStore::needsNewSegment() const
{
    return segments_.empty() || segments_.back().byteSize >= limits_.segmentTargetBytes
           || segments_.back().spilled || !segments_.back().memoryData;
}

void CaptureStore::ensureSegmentIdsAvailable( const AppendResult& appendResult,
                                              qint64 pendingPartialBytes )
{
    if ( capturePathState_->isTerminallyRemoved() ) {
        ensureCaptureDir();
    }

    auto requiredIds = qint64{ 0 };
    auto hasActiveSegment = !needsNewSegment();
    auto activeBytes = hasActiveSegment ? segments_.back().byteSize : qint64{ 0 };

    const auto consumeLine = [ & ]( qint64 lineBytes ) {
        if ( !hasActiveSegment ) {
            ++requiredIds;
            hasActiveSegment = true;
            activeBytes = 0;
        }
        activeBytes = lineBytes > std::numeric_limits<qint64>::max() - activeBytes
                          ? std::numeric_limits<qint64>::max()
                          : activeBytes + lineBytes;
        if ( activeBytes >= limits_.segmentTargetBytes ) {
            hasActiveSegment = false;
        }
    };

    qint64 previousLineEnd = 0;
    for ( const auto lineEnd : appendResult.endOfLines ) {
        consumeLine( lineEnd - previousLineEnd );
        previousLineEnd = lineEnd;
    }
    if ( pendingPartialBytes > 0 ) {
        consumeLine( pendingPartialBytes );
    }

    const auto alreadyReserved = static_cast<qint64>( reservedSegmentIds_.size() );
    if ( requiredIds > alreadyReserved ) {
        const auto additionalIds = capturePathState_->reserveSegmentIds(
            requiredIds - alreadyReserved );
        reservedSegmentIds_.insert( reservedSegmentIds_.end(), additionalIds.cbegin(),
                                    additionalIds.cend() );
    }
}

qint64 CaptureStore::takeNextSegmentId()
{
    if ( reservedSegmentIds_.empty() ) {
        const auto ids = capturePathState_->reserveSegmentIds( 1 );
        reservedSegmentIds_.insert( reservedSegmentIds_.end(), ids.cbegin(), ids.cend() );
    }

    const auto segmentId = reservedSegmentIds_.front();
    reservedSegmentIds_.pop_front();
    return segmentId;
}

CaptureStore::Segment& CaptureStore::ensureActiveSegment()
{
    if ( needsNewSegment() ) {
        // Starting a replacement generation cancels any delayed directory
        // removal from deleteCaptureFiles() before a pinned reader can finish.
        ensureCaptureDir();
        const auto prevCumulative
            = segments_.empty() ? 0LL : segments_.back().cumulativeEndLine;
        Segment segment;
        segment.id = takeNextSegmentId();
        segment.filePath = QDir( capturePath_ ).filePath( makeSegmentFileName( segment.id ) );
        segment.memoryData = std::make_shared<QByteArray>();
        reserveSegmentMemory( *segment.memoryData, limits_.segmentTargetBytes,
                              limits_.memoryBudgetBytes );
        segment.cumulativeEndLine = prevCumulative;
        segments_.push_back( std::move( segment ) );
    }

    return segments_.back();
}

void CaptureStore::rebuildCumulativeLineCounts( bool onlyLast )
{
    if ( onlyLast && !segments_.empty() ) {
        // O(1) fast path: only update the active (last) segment.
        // commitLine() tracks memoryBytes_ incrementally, so we only need to
        // fix up cumulativeEndLine for the last segment.
        auto& last = segments_.back();
        const qint64 prevEnd = segments_.size() > 1
                                   ? segments_[ segments_.size() - 2 ].cumulativeEndLine
                                   : 0;
        last.cumulativeEndLine = prevEnd + klogg::ssize( last.lineOffsets );
        return;
    }

    // Full rebuild -- used after loadFromDisk(), clear(), etc.
    qint64 cumulative = 0;
    memoryBytes_ = 0;
    for ( auto& segment : segments_ ) {
        cumulative += klogg::ssize( segment.lineOffsets );
        segment.cumulativeEndLine = cumulative;
        if ( segment.memoryData ) {
            memoryBytes_ += segment.memoryData->size();
        }
    }
}

void CaptureStore::enforceMemoryBudget()
{
    for ( auto& segment : segments_ ) {
        if ( memoryBytes_ <= limits_.memoryBudgetBytes ) {
            break;
        }
        if ( &segment == &segments_.back() && !needsNewSegment() ) {
            break;
        }
        if ( segment.memoryData && spillSegmentToDisk( segment ) ) {
            memoryBytes_ -= segment.memoryData->size();
            segment.memoryData.reset();
        }
    }
}

bool CaptureStore::scanSegment( Segment& segment )
{
    auto spilledFile = spilledFileLease( segment.filePath );
    if ( !spilledFile || spilledFile->isRetired() ) {
        return false;
    }

    auto file = spilledFile->openForRead();
    if ( !file ) {
        return false;
    }

    segment.spilledFile = std::move( spilledFile );
    segment.byteSize = file->size();
    segment.spilled = true;
    lastModified_ = file->fileTime( QFileDevice::FileModificationTime );

    qint64 offset = 0;
    while ( !file->atEnd() ) {
        const auto lineBytes = file->readLine();
        if ( lineBytes.isEmpty() ) {
            break;
        }
        auto lineLength = lineBytes.endsWith( '\n' ) ? lineBytes.size() - 1 : lineBytes.size();
        if ( lineLength > 0 && lineBytes[ lineLength - 1 ] == '\r' ) {
            --lineLength;
        }
        segment.lineOffsets.push_back( offset );
        segment.lineLengths.push_back( type_safe::narrow_cast<int>( lineLength ) );
        fileSize_ += lineBytes.size();
        maxLineLength_ = qMax( maxLineLength_, type_safe::narrow_cast<int>( lineLength ) );
        offset += lineBytes.size();
    }
    totalLines_ += klogg::ssize( segment.lineOffsets );
    return true;
}

bool CaptureStore::spillSegmentToDisk( Segment& segment )
{
    if ( segment.spilled || !segment.memoryData ) {
        return true;
    }
    if ( segment.memoryData->isEmpty() ) {
        return true;
    }

    try {
        ensureCaptureDir();
    } catch ( const std::exception& error ) {
        LOG_WARNING << "Failed to prepare capture spill directory " << capturePath_ << ": "
                    << error.what();
        return false;
    }
    capturePathState_->retryRetiredFilesAndReleaseRegistry();
    CapturePathGate gate( capturePathState_->gatePath() );
    if ( !gate.lock() ) {
        LOG_WARNING << "Failed to acquire capture spill gate for " << capturePath_;
        return false;
    }
    const std::lock_guard<std::recursive_mutex> pathLock( capturePathState_->mutex_ );
    try {
        ensureCaptureDir();
    } catch ( const std::exception& error ) {
        LOG_WARNING << "Failed to validate capture spill directory " << capturePath_ << ": "
                    << error.what();
        return false;
    }

    QString temporaryPath;
    auto temporaryFile
        = capturePathState_->directory_.createTemporaryFile( temporaryPath );
    if ( !temporaryFile ) {
        LOG_WARNING << "Failed to create temporary capture segment in " << capturePath_;
        return false;
    }
    capturePathState_->registerCreatedFile( temporaryPath );

    const auto expectedBytes
        = static_cast<qint64>( segment.memoryData->size() );
    qint64 writtenBytes = 0;
    if ( failNextSegmentWriteForTesting_ ) {
        failNextSegmentWriteForTesting_ = false;
        const auto partialBytes = qMax<qint64>( 0, expectedBytes - 1 );
        writtenBytes = temporaryFile->write( segment.memoryData->constData(), partialBytes );
    } else {
        writtenBytes = temporaryFile->write( *segment.memoryData );
    }

    const auto writeSucceeded
        = writtenBytes == expectedBytes && temporaryFile->flush();
    temporaryFile->close();
    temporaryFile.reset();
    if ( !writeSucceeded ) {
        LOG_WARNING << "Failed to write temporary capture segment " << temporaryPath;
        capturePathState_->retireFile( temporaryPath );
        capturePathState_->retryRetiredFilesAndReleaseRegistryGateHeld();
        if ( capturePathState_->hasRetryableMaintenance() ) {
            capturePathState_->scheduleRetry();
        }
        return false;
    }

    while ( true ) {
        const auto publishResult = capturePathState_->directory_.publishTemporaryFile(
            temporaryPath, directChildName( segment.filePath ) );
        if ( publishResult == SecureCaptureDirectory::PublishResult::Success ) {
            break;
        }
        if ( publishResult == SecureCaptureDirectory::PublishResult::AlreadyExists ) {
            capturePathState_->observeSegmentId( segment.id );
            segment.id = takeNextSegmentId();
            segment.filePath
                = QDir( capturePath_ ).filePath( makeSegmentFileName( segment.id ) );
            continue;
        }

        LOG_WARNING << "Failed to publish capture segment " << segment.filePath;
        capturePathState_->retireFile( temporaryPath );
        capturePathState_->retryRetiredFilesAndReleaseRegistryGateHeld();
        if ( capturePathState_->hasRetryableMaintenance() ) {
            capturePathState_->scheduleRetry();
        }
        return false;
    }

    capturePathState_->transferCreatedFile(
        temporaryPath, directChildName( segment.filePath ) );
    segment.spilledFile = spilledFileLease( segment.filePath );
    if ( !segment.spilledFile ) {
        capturePathState_->retireFile( segment.filePath );
        capturePathState_->retryRetiredFilesAndReleaseRegistryGateHeld();
        if ( capturePathState_->hasRetryableMaintenance() ) {
            capturePathState_->scheduleRetry();
        }
        return false;
    }
    segment.spilled = true;
    return true;
}

void CaptureStore::persistBufferedSegments()
{
    for ( auto& segment : segments_ ) {
        spillSegmentToDisk( segment );
    }
}

QByteArray CaptureStore::readSegmentLine( const Segment& segment, int localLine ) const
{
    if ( localLine < 0 || localLine >= klogg::isize( segment.lineOffsets ) ) {
        return {};
    }

    if ( segment.memoryData ) {
        const auto lineOffset = segment.lineOffsets[ static_cast<size_t>( localLine ) ];
        const auto lineLength = segment.lineLengths[ static_cast<size_t>( localLine ) ];
        return segment.memoryData->mid( type_safe::narrow_cast<int>( lineOffset ), lineLength );
    }

    std::unique_ptr<QFile> file;
    if ( segment.spilledFile ) {
        file = segment.spilledFile->openForRead();
    } else {
        file = std::make_unique<QFile>( segment.filePath );
        if ( !file->open( QIODevice::ReadOnly ) ) {
            file.reset();
        }
    }
    if ( !file ) {
        return {};
    }
    const auto lineOffset = segment.lineOffsets[ static_cast<size_t>( localLine ) ];
    const auto lineLength = segment.lineLengths[ static_cast<size_t>( localLine ) ];
    file->seek( lineOffset );
    return file->read( lineLength );
}

bool CaptureStore::writeSegmentToDevice( const Segment& segment, QIODevice* device ) const
{
    if ( !device ) {
        return false;
    }

    if ( segment.memoryData ) {
        return device->write( *segment.memoryData ) == segment.memoryData->size();
    }

    std::unique_ptr<QFile> file;
    if ( segment.spilledFile ) {
        file = segment.spilledFile->openForRead();
    } else {
        file = std::make_unique<QFile>( segment.filePath );
        if ( !file->open( QIODevice::ReadOnly ) ) {
            file.reset();
        }
    }
    if ( !file ) {
        return false;
    }

    std::array<char, 64 * 1024> buffer{};
    while ( true ) {
        const auto bytesRead = file->read(
            buffer.data(), type_safe::narrow_cast<qint64>( buffer.size() ) );
        if ( bytesRead < 0 ) {
            return false;
        }
        if ( bytesRead == 0 ) {
            break;
        }
        if ( device->write( buffer.data(), bytesRead ) != bytesRead ) {
            return false;
        }
    }

    return true;
}

void CaptureStore::appendOutputBytes( const QByteArray& bytes, int lineCount )
{
    if ( !rollingOutput_.isValid() ) {
        return;
    }

    const auto written = rollingOutput_.write( bytes );
    if ( written <= 0 ) {
        LOG_WARNING << "Rolling output file write failed, unbinding: " << boundOutputFile_;
        rollingOutput_.close();
        rollingOutput_ = RollingFileManager();
        boundOutputFile_.clear();
        return;
    }

    // write() now writes the whole batch across rotations and reports whether it
    // rotated. The previous size-before/after heuristic missed rotations that
    // left the new file at least as large as the old one (e.g. a single write
    // that fills, rotates and writes a large remainder from an empty file),
    // skipping the in-memory window trim on the single-line commit path.
    if ( rollingOutput_.rotated() ) {
        trimToWindowSize();
    }

    unflushedOutputBytes_ += written;
    unflushedOutputLines_ += lineCount;
    flushOutputIfNeeded();
}

void CaptureStore::flushOutputIfNeeded()
{
    if ( !rollingOutput_.isValid() || unflushedOutputBytes_ == 0 ) {
        return;
    }

    if ( unflushedOutputBytes_ >= OutputFlushBytesThreshold
         || unflushedOutputLines_ >= OutputFlushLinesThreshold ) {
        if ( !rollingOutput_.flush() ) {
            LOG_WARNING << "Rolling output file flush failed, unbinding: "
                        << boundOutputFile_;
            rollingOutput_.close();
            rollingOutput_ = RollingFileManager();
            boundOutputFile_.clear();
            resetOutputFlushCounters();
            return;
        }
        resetOutputFlushCounters();
    }
}

void CaptureStore::resetOutputFlushCounters()
{
    unflushedOutputBytes_ = 0;
    unflushedOutputLines_ = 0;
}

void CaptureStore::trimToWindowSize()
{
    // Called when the rolling file rotates. Trim in-memory segments to match the window.
    // trimToLimits() already checks whether any limits are configured and
    // acquires the (recursive) mutex — safe because the caller chain
    // (appendOutputBytes → commitLines → appendUtf8) already holds it.
    trimToLimits();
}
