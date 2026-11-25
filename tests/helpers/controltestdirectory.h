#ifndef KLOGG_CONTROLTESTDIRECTORY_H
#define KLOGG_CONTROLTESTDIRECTORY_H

#include <QDir>
#include <QFileInfo>
#include <QSysInfo>

namespace klogg::test {
// GUI tests redirect TMPDIR into the checkout, whose ancestors can be owned
// by a different user in CI. Give control files a private random directory
// under the trusted system temp root, also keeping Unix socket paths short.
inline QString controlDirectoryTemplate()
{
    const auto parent = QSysInfo::kernelType() == QStringLiteral( "winnt" )
        ? QDir::tempPath() : QFileInfo( QStringLiteral( "/tmp" ) ).canonicalFilePath();
    return QDir( parent ).filePath( QStringLiteral( "kt-XXXXXX" ) );
}
} // namespace klogg::test

#endif
