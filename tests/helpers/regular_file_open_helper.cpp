#include <QCoreApplication>
#include <QFile>
#include <QTextStream>
#include <QtGlobal>
#include <cstdio>

#if !defined( Q_OS_WIN )
#include <sys/stat.h>
#endif

#include "platform/platform_files.h"

int main( int argc, char** argv )
{
    QCoreApplication app{ argc, argv };
    if ( app.arguments().size() != 2 ) return 2;
#if defined( Q_OS_WIN )
    QFile source{ QStringLiteral( "\\\\.\\NUL" ) };
#else
    const auto path = app.arguments().at( 1 );
    const auto nativePath = QFile::encodeName( path );
    if ( ::mkfifo( nativePath.constData(), 0600 ) != 0 ) return 2;
    QFile source{ path };
#endif
    const auto opened = klogg::platform::openRegularFileReadOnly( source );
    const bool rejected = opened.status == klogg::platform::ReadOnlyFileOpenStatus::NotRegular
        && !source.isOpen() && !opened.metadata;
    if ( !rejected ) {
        QTextStream{ stderr } << "status=" << static_cast<int>( opened.status )
                              << ", open=" << source.isOpen()
                              << ", metadata=" << opened.metadata.has_value()
                              << ", error=" << opened.error << '\n';
    }
    return rejected ? 0 : 1;
}
