// Python.h must come before any standard header.
#include <pybind11/embed.h>

#include "chronos/gateway/python_control.hpp"

#include <nlohmann/json.hpp>

#include <mutex>
#include <optional>
#include <utility>

namespace chronos::gateway {
namespace {

namespace py = pybind11;
using Json = nlohmann::json;

Json store_json(const std::string &data_url, const std::string &metadata_url) {
    Json store = {{"data_url", data_url}};
    if (!metadata_url.empty()) store["metadata_url"] = metadata_url;
    return store;
}

Json error_body(const std::string &message) { return {{"error", message}}; }

} // namespace

std::string python_control_config(const std::unordered_map<std::string, WorkspaceConfig> &workspaces) {
    Json result = {{"workspaces", Json::object()}};
    for (const auto &[name, workspace] : workspaces) {
        Json stores = Json::object();
        for (const auto &[alias, store] : workspace.postgres_stores) {
            stores[alias] = store_json(store.data_url, store.metadata_url);
        }
        Json entry = {{"stores", std::move(stores)}, {"filesystem", nullptr}};
        if (workspace.filesystem) {
            entry["filesystem"] = store_json(workspace.filesystem->data_url, workspace.filesystem->metadata_url);
        }
        result["workspaces"][name] = std::move(entry);
    }
    return result.dump();
}

class PythonControl::Impl {
  public:
    explicit Impl(std::string config) : config(std::move(config)) {}

    PythonControlResponse call(const std::string &request) {
        if (!start()) return {501, error_body(unavailable).dump()};
        try {
            py::gil_scoped_acquire gil;
            const auto text = py::reinterpret_borrow<py::object>(control)
                .attr("handle")(request).cast<std::string>();
            auto response = Json::parse(text);
            return {response.at("status").get<int>(), response.at("body").dump()};
        } catch (const std::exception &error) {
            // GatewayControl.handle reports operation errors itself; this is a
            // failure of the bridge, and its message names no configured URL.
            return {500, error_body(std::string("Python control failed: ") + error.what()).dump()};
        }
    }

  private:
    // Starts the interpreter and imports the control module once. The
    // interpreter is never finalized: client threads may still hold Python
    // objects at exit, and native extension modules do not support it.
    bool start() {
        std::lock_guard lock(start_mutex);
        if (control) return true;
        if (!unavailable.empty()) return false;
        try {
            std::optional<py::gil_scoped_acquire> gil;
            const bool owner = !Py_IsInitialized();
            if (owner) {
                // The gateway keeps its own SIGINT and SIGTERM handlers.
                py::initialize_interpreter(false);
            } else {
                gil.emplace();
            }
            try {
                auto module = py::module_::import("chronos_core.control.gateway");
                // Leaked deliberately; see above.
                control = module.attr("GatewayControl")(config).release().ptr();
            } catch (const py::error_already_set &error) {
                unavailable = std::string("Python control is unavailable: ") + error.what();
            }
            if (owner) PyEval_SaveThread();  // release the GIL for client threads
        } catch (const std::exception &error) {
            unavailable = std::string("Python control is unavailable: ") + error.what();
        }
        return control != nullptr;
    }

    std::string config;
    std::mutex start_mutex;
    // A raw reference: pybind11 types have hidden visibility.
    PyObject *control = nullptr;
    std::string unavailable;
};

PythonControl::PythonControl(const std::unordered_map<std::string, WorkspaceConfig> &workspaces)
    : impl_(std::make_unique<Impl>(python_control_config(workspaces))) {}

PythonControl::~PythonControl() = default;

PythonControlResponse PythonControl::call(
    const std::string &workspace,
    const std::string &operation,
    const std::string &arguments_json) {
    const Json request = {
        {"workspace", workspace},
        {"operation", operation},
        {"args", Json::parse(arguments_json)},
    };
    return impl_->call(request.dump());
}

} // namespace chronos::gateway
