#ifndef KLOGG_REVIEWCONTROLSERVICE_H
#define KLOGG_REVIEWCONTROLSERVICE_H

#include "controltransport.h"
#include "reviewcontroladapter.h"

// Application-owned composition of local transport and GUI review commands.
// Configuration owns the global policy; this service applies it without dialogs,
// focus changes or a second window/tab registry.
class ReviewControlService : public QObject {
  public:
    explicit ReviewControlService( ReviewControlAdapter::WindowProvider windows,
                                   klogg::control::RuntimeOptions runtime = {}, QObject* parent = nullptr );
    ~ReviewControlService() override;
    bool setEnabled( bool enabled );
    bool isRunning() const;
    QString instanceId() const;
    QString lastError() const;

  private:
    ReviewControlAdapter adapter_;
    klogg::control::ControlServer server_;
    klogg::control::RuntimeOptions runtime_;
    QString lastError_;
};

#endif
