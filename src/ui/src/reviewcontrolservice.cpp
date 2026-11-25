#include "reviewcontrolservice.h"

#include <utility>

ReviewControlService::ReviewControlService( ReviewControlAdapter::WindowProvider windows,
    klogg::control::RuntimeOptions runtime, QObject* parent )
    : QObject( parent ), adapter_( std::move( windows ) ), runtime_( std::move( runtime ) )
{
}

ReviewControlService::~ReviewControlService()
{
    setEnabled( false );
}

bool ReviewControlService::setEnabled( bool enabled )
{
    if ( !enabled ) {
        // Stop admission before discarding candidates. No queued socket callback
        // can publish work after the global switch is disabled.
        server_.stop();
        adapter_.reset();
        lastError_.clear();
        return true;
    }
    if ( server_.isRunning() ) return true;
    return server_.start( runtime_, [ this ]( const klogg::control::RpcRequest& request ) {
        return adapter_.dispatch( request );
    }, &lastError_ );
}
bool ReviewControlService::isRunning() const { return server_.isRunning(); }
QString ReviewControlService::instanceId() const { return server_.descriptor().instanceId; }
QString ReviewControlService::lastError() const { return lastError_; }
