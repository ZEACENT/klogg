#ifndef KLOGG_QTCONTAINERINDEX_H
#define KLOGG_QTCONTAINERINDEX_H

#include <QByteArray>

namespace klogg {

// Qt-version-adaptive container index: int on Qt 5, qsizetype on Qt 6.
// Kept independent of allocator/Widgets helpers so QtCore-only tools can use
// the same contract without pulling in the application's data/UI dependencies.
// This tracks QByteArray/QString, NOT QStringView (qsizetype on both Qt majors).
using ContainerIndex = decltype( QByteArray{}.size() );

} // namespace klogg

#endif
