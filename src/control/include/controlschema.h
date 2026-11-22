#ifndef KLOGG_CONTROLSCHEMA_H
#define KLOGG_CONTROLSCHEMA_H

#include "controlrpc.h"

namespace klogg::control {
// Describe one advertised method without resolving IDs or accessing application state.
DecodedResponse methodSchema( const RpcRequest& request );
} // namespace klogg::control

#endif
