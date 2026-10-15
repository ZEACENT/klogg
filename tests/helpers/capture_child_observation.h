#ifndef CAPTURE_CHILD_OBSERVATION_H
#define CAPTURE_CHILD_OBSERVATION_H

#include <array>
#include <cstddef>
#include <cstdint>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QStringList>

namespace klogg::testing {

enum class CaptureChildPhase : std::uint8_t {
    MainEnter,
    ApplicationReady,
    CatchDispatch,
    BeforeStore,
    StoreReady,
    AppendBegin,
    SpillBegin,
    SpillComplete,
    ReadyPublishBegin,
    ReadyCommitBegin,
    ReadyCommitted
};

inline constexpr std::array<const char*, 11> CaptureChildPhaseNames{
    "main-enter", "qapplication-constructed", "catch-dispatch", "pre-store",
    "store-constructed", "append-begin", "spill-begin", "spill-complete",
    "ready-publish-begin", "ready-commit-begin", "ready-committed"
};
static_assert( CaptureChildPhaseNames.size()
               == static_cast<std::size_t>( CaptureChildPhase::ReadyCommitted ) + 1 );

// Test-child evidence only: no stderr, retries, directory creation or replacement.
// Missing records are unavailable observations, not proof that execution stopped.
inline void recordCaptureChildPhase( CaptureChildPhase phase ) noexcept
{
    try {
        const auto index = static_cast<std::size_t>( phase );
        if ( qgetenv( "KLOGG_CAPTURESTORE_CHILD_PHASES" ) != "1"
             || index >= CaptureChildPhaseNames.size() ) {
            return;
        }
        const auto rootPath = qEnvironmentVariable( "KLOGG_CAPTURESTORE_CHILD_ROOT" );
        const QFileInfo rootInfo{ rootPath };
        if ( !QDir::isAbsolutePath( rootPath ) || !rootInfo.isDir() || rootInfo.isSymLink() ) {
            return;
        }
        const QByteArray token{ CaptureChildPhaseNames[ index ] };
        QFile record( QDir( rootPath ).filePath(
            QStringLiteral( ".child-phase-" ) + QString::fromLatin1( token ) ) );
        if ( record.open( QIODevice::WriteOnly | QIODevice::NewOnly ) ) {
            (void)record.write( token + '\n' );
        }
    }
    catch ( ... ) {
        // Observation failure cannot replace the child's functional result.
    }
}

inline QString captureChildPhaseSnapshot( const QString& rootPath )
{
    QStringList observed;
    for ( const auto* phase : CaptureChildPhaseNames ) {
        const auto token = QString::fromLatin1( phase );
        QFile record( QDir( rootPath ).filePath( QStringLiteral( ".child-phase-" ) + token ) );
        const QFileInfo info{ record };
        if ( !info.isFile() || info.isSymLink() || info.size() > 64 ) {
            continue;
        }
        if ( record.open( QIODevice::ReadOnly ) && record.read( 65 ) == token.toLatin1() + '\n' ) {
            observed.append( token );
        }
    }
    return QStringLiteral( "observed_child_phases=" )
        + ( observed.isEmpty() ? QStringLiteral( "none" ) : observed.join( QLatin1Char( ',' ) ) );
}

} // namespace klogg::testing

#endif
