#ifndef KLOGG_HIGHLIGHTERSTATEPUBLISHER_H
#define KLOGG_HIGHLIGHTERSTATEPUBLISHER_H

#include <QObject>

// Application-owned publication only. HighlighterSetCollection remains the
// sole state/revision owner; this object stores no duplicate highlighter data.
class HighlighterStatePublisher final : public QObject {
    Q_OBJECT
  public:
    static HighlighterStatePublisher& instance();
    bool publish();
  Q_SIGNALS:
    void changed();
  private:
    explicit HighlighterStatePublisher( QObject* parent ) : QObject( parent ) {}
};

#endif
