#ifndef STAGEDOUTPUTFILE_H
#define STAGEDOUTPUTFILE_H

#include <cstdint>
#include <functional>
#include <optional>

#include <QDir>
#include <QFileInfo>
#include <QIODevice>
#include <QString>
#include <QTemporaryFile>

#include "platform/platform_files.h"

namespace klogg::stagedoutput {

enum class Result : std::uint8_t {
    Published,
    DestinationExists,
    OpenFailure,
    WriteFailure,
    FlushFailure,
    PublishFailure,
};

struct Publication {
    Result result = Result::OpenFailure;
    std::optional<klogg::platform::FileIdentity> identity;
};

inline Publication publishSibling( const QString& destination,
                                   const std::function<bool( QIODevice* )>& write )
{
    const auto directory = QFileInfo( destination ).absoluteDir();
    QTemporaryFile staged( directory.filePath( QStringLiteral( ".klogg-output-XXXXXX" ) ) );
    if ( !staged.open() ) {
        return { Result::OpenFailure, std::nullopt };
    }
    if ( !write( &staged ) ) {
        return { Result::WriteFailure, std::nullopt };
    }
    if ( !staged.flush() ) {
        return { Result::FlushFailure, std::nullopt };
    }
    const auto stagedIdentity = klogg::platform::fileIdentity( staged );
    if ( !stagedIdentity.has_value() ) {
        return { Result::PublishFailure, std::nullopt };
    }
    if ( staged.rename( destination ) ) {
        staged.setAutoRemove( false );
        return { Result::Published, stagedIdentity };
    }
    if ( QFileInfo::exists( destination ) ) {
        return { Result::DestinationExists, std::nullopt };
    }
    return { Result::PublishFailure, std::nullopt };
}

} // namespace klogg::stagedoutput

#endif // STAGEDOUTPUTFILE_H
