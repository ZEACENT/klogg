#ifndef KLOGG_CONTROLCLI_H
#define KLOGG_CONTROLCLI_H

#include <cstdint>
#include <functional>

#include <QByteArray>
#include <QStringList>

#include "controltransport.h"

class QIODevice;

namespace klogg::control {

// Stable process exit statuses; domain failures remain in error.data.code.
enum class CliExit : std::uint8_t {
    Success = 0,
    InvalidArguments = 2,
    Unavailable = 3,
    Rejected = 4,
    OutcomeUnknown = 5
};

struct CliResult {
    CliExit exit = CliExit::Success;
    QByteArray standardOutput;
    QByteArray standardError;
};

// Only the two external effects are replaceable; parsing and bounded file/stdin
// reads use the same path in tests and the console entrypoint.
struct CliServices {
    std::function<DiscoveryResult( const RuntimeOptions& )> discover = discoverInstances;
    std::function<ClientResult( const InstanceDescriptor&, const RpcRequest&, int )> call = callLocal;
};

// Arguments exclude argv[0]. Help is text; every other command emits one JSON
// object. No input is read unless --params-file - explicitly selects stdin.
// timeout-ms bounds transport, not the producer of an explicitly selected stdin.
CliResult runControlCli( const QStringList& arguments, QIODevice& standardInput,
                         const QString& version, const CliServices& services = {},
                         const RuntimeOptions& runtime = {} );

} // namespace klogg::control
#endif
