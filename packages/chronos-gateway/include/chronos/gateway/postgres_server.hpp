#pragma once

#include "chronos/gateway/config.hpp"
#include "chronos/gateway/runtime.hpp"

#include <memory>

namespace chronos::gateway {

class PythonControl;

// PostgreSQL wire-protocol frontend for a branch grant.  The startup database
// is a configured store alias, the startup user is the grant id, and the
// cleartext password is the per-grant secret.  Each accepted connection owns
// one branch-bound NativeBranchSession.
class PostgresServer {
  public:
    PostgresServer(GatewayRuntime &runtime, ListenAddress listen);
    ~PostgresServer();
    PostgresServer(const PostgresServer &) = delete;
    PostgresServer &operator=(const PostgresServer &) = delete;

    void run();
    void stop();
    // Lets SQL clients of shared-structure workspaces create and register
    // tables and indexes through the embedded Python bridge. Set before run().
    void set_python_control(PythonControl *python);

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace chronos::gateway
