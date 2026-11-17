#pragma once

#include "chronos/gateway/runtime.hpp"

#include <memory>
#include <string>
#include <unordered_map>

namespace chronos::gateway {

// A control operation's HTTP status and JSON body text.
struct PythonControlResponse {
    int status = 500;
    std::string body;
};

// Runs Chronos versioning operations through chronos_core.control.gateway in
// an embedded Python interpreter, so the gateway behaves as the CLI does.
// The interpreter starts on the first call; when Python or the chronos_core
// package is unavailable every call answers 501 and the rest of the gateway
// keeps working.
class PythonControl {
  public:
    explicit PythonControl(const std::unordered_map<std::string, WorkspaceConfig> &workspaces);
    ~PythonControl();
    PythonControl(const PythonControl &) = delete;
    PythonControl &operator=(const PythonControl &) = delete;

    // operation names a function of chronos_core.control.operations;
    // arguments_json is a JSON object of its arguments.
    PythonControlResponse call(
        const std::string &workspace,
        const std::string &operation,
        const std::string &arguments_json);

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// The workspace configuration passed to chronos_core.control.gateway.GatewayControl.
std::string python_control_config(const std::unordered_map<std::string, WorkspaceConfig> &workspaces);

} // namespace chronos::gateway
