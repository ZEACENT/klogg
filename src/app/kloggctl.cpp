#include <cstdio>

#include <QCoreApplication>
#include <QFile>

#include "controlcli.h"
#include "klogg_version.h"
#include "kloggctl_stdio.h"

int main( int argc, char* argv[] )
{
    QCoreApplication application( argc, argv );
    QCoreApplication::setApplicationName( QStringLiteral( "kloggctl" ) );
    QFile standardInput;
    klogg::platform::openControlStdin( standardInput );
    const auto result = klogg::control::runControlCli(
        QCoreApplication::arguments().mid( 1 ), standardInput, kloggVersion() );
    const auto write = []( FILE* stream, const QByteArray& bytes ) {
        const auto size = static_cast<size_t>( bytes.size() );
        return std::fwrite( bytes.constData(), 1, size, stream ) == size
               && std::fflush( stream ) == 0;
    };
    if ( !write( stdout, result.standardOutput ) || !write( stderr, result.standardError ) )
        return static_cast<int>( klogg::control::CliExit::Unavailable );
    return static_cast<int>( result.exit );
}
