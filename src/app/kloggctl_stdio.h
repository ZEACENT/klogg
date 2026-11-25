#ifndef KLOGG_CTL_STDIO_H
#define KLOGG_CTL_STDIO_H

#include <cstdio>
#include <QFile>

#ifdef Q_OS_WIN
#include <fcntl.h>
#include <io.h>
#endif

namespace klogg::platform {

// Borrow the descriptor, not FILE*: on Windows Qt's sequential FILE* engine
// reports a read error when a short fread already set the pipe's EOF flag.
// No bytes are consumed unless the requested command actually reads stdin.
inline void openControlStdin( QFile& input )
{
#ifdef Q_OS_WIN
    const int descriptor = ::_fileno( stdin );
    // Do not translate CRLF or treat a Ctrl-Z byte inside invalid JSON as EOF.
    if ( descriptor < 0 || ::_setmode( descriptor, _O_BINARY ) == -1 ) return;
#else
    const int descriptor = ::fileno( stdin );
    if ( descriptor < 0 ) return;
#endif
    static_cast<void>( input.open( descriptor, QIODevice::ReadOnly | QIODevice::Unbuffered,
                                   QFileDevice::DontCloseHandle ) );
}

} // namespace klogg::platform

#endif
