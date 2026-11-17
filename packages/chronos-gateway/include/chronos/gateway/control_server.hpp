#pragma once

#include "chronos/gateway/config.hpp"
#include "chronos/gateway/runtime.hpp"

#include <atomic>
#include <memory>
#include <string>
#include <unordered_map>

namespace chronos::gateway {

class PythonControl;

class ControlServer {
  public:
    ControlServer(
        GatewayRuntime &runtime,
        ListenAddress listen,
        ListenAddress postgres,
        ListenAddress nfs,
        std::string controller_token,
        // Workspaces served by the versioning routes (see python_control.hpp).
        const std::unordered_map<std::string, WorkspaceConfig> &workspaces = {});
    ~ControlServer();
    ControlServer(const ControlServer &) = delete;
    ControlServer &operator=(const ControlServer &) = delete;

    void run();
    void stop();
    // The embedded Python bridge, shared with the PostgreSQL frontend.
    PythonControl &python_control();

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace chronos::gateway
