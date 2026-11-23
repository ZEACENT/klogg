#ifndef KLOGG_CONTROLRUNTIME_H
#define KLOGG_CONTROLRUNTIME_H

#include <cstddef>
#include <memory>
#include <vector>

#include <QString>

namespace klogg::control {

inline constexpr int ControlProtocolVersion = 1;

struct RuntimeOptions {
    // Empty roots select local per-user cache and the OS temporary directory.
    // KLOGG_CONTROL_DESCRIPTOR_ROOT can override discovery for isolated processes;
    // an explicit descriptorRoot takes precedence. All overrides undergo the same
    // private-directory checks. Roots must be absolute with non-symlink ancestors.
    QString descriptorRoot;
    QString socketRoot;
    QString installationId;
    QString profileId;
};

struct InstanceDescriptor {
    int descriptorVersion = 1;
    int protocolMin = ControlProtocolVersion;
    int protocolMax = ControlProtocolVersion;
    QString instanceId;
    QString installationId;
    QString profileId;
    QString endpoint;
    // Credential: never print this structure or pass its token in argv.
    QString token;
};

struct DiscoveryResult {
    std::vector<InstanceDescriptor> instances;
    QString error;
    std::size_t rejectedDescriptors = 0;
};

// Read-only kernel lease checks. No endpoint probes, deletion, or execution of descriptor contents.
DiscoveryResult discoverInstances( const RuntimeOptions& requested = {} );

// Owns one enable epoch. Internal transport resource owner; public so the
// transport's pimpl does not depend on platform-specific types.
class ControlRuntime {
  public:
    ControlRuntime();
    ~ControlRuntime();
    ControlRuntime( const ControlRuntime& ) = delete;
    ControlRuntime& operator=( const ControlRuntime& ) = delete;

    bool prepare( const RuntimeOptions& requested, QString* error );
    bool publish( QString* error );
    void clear();
    const InstanceDescriptor& descriptor() const;
    QString descriptorPath() const;
    // Unix: returns an already-bound listening fd, transferred to the PAL
    // listener on successful open(). Windows: Qt creates the protected pipe.
    qintptr createListener( QString* error );
    static void closeListener( qintptr descriptor );

  private:
    struct Private;
    std::unique_ptr<Private> d_;
};

} // namespace klogg::control
#endif
