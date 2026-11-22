#include "highlighterstatepublisher.h"

#include <QCoreApplication>
#include <QPointer>
#include <QThread>

#include "highlighterset.h"
#include "log.h"

HighlighterStatePublisher& HighlighterStatePublisher::instance()
{
    auto* application = QCoreApplication::instance();
    Q_ASSERT( application && QThread::currentThread() == application->thread() );
    static QPointer<HighlighterStatePublisher> publisher;
    if ( !publisher ) publisher = new HighlighterStatePublisher( application );
    return *publisher;
}

bool HighlighterStatePublisher::publish()
{
    Q_ASSERT( QThread::currentThread() == thread() );
    bool stored = true;
    try {
        // Keep the normal QSettings contract, not a crash-durable transaction.
        // New windows may retrieve settings from observers, so store first.
        HighlighterSetCollection::get().save();
    }
    catch ( ... ) {
        stored = false;
        LOG_WARNING << "Could not persist committed highlighter state";
    }
    Q_EMIT changed();
    return stored;
}
