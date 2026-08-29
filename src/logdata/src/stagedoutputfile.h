#ifndef STAGEDOUTPUTFILE_H
#define STAGEDOUTPUTFILE_H

#include <cstdint>
#include <functional>

#include <QDir>
#include <QFileInfo>
#include <QIODevice>
#include <QString>
#include <QTemporaryFile>

namespace klogg::stagedoutput {

enum class Result : std::uint8_t {
    Published,
    DestinationExists,
    OpenFailure,
    WriteFailure,
    FlushFailure,
    PublishFailure,
};

inline Result publishSibling( const QString& destination,
                              const std::function<bool( QIODevice* )>& write )
{
    const auto directory = QFileInfo( destination ).absoluteDir();
    QTemporaryFile staged( directory.filePath( QStringLiteral( ".klogg-output-XXXXXX" ) ) );
    if ( !staged.open() ) {
        return Result::OpenFailure;
    }
    if ( !write( &staged ) ) {
        return Result::WriteFailure;
    }
    if ( !staged.flush() ) {
        return Result::FlushFailure;
    }
    if ( staged.rename( destination ) ) {
        staged.setAutoRemove( false );
        return Result::Published;
    }
    if ( QFileInfo::exists( destination ) ) {
        return Result::DestinationExists;
    }
    return Result::PublishFailure;
}

} // namespace klogg::stagedoutput

#endif // STAGEDOUTPUTFILE_H
