#include "WasmServerFeature.h"
#include <s2/base/integral_types.h>
#include <optional>
#include "Cluster/ServerState.h"
#include "ApplicationFeatures/ApplicationServer.h"
#include "WasmServer/Wasm3cpp.h"
#include "WasmServer/WasmCommon.h"

using namespace arangodb;

WasmServerFeature::WasmServerFeature(Server& server)
    : ArangodFeature{server, *this} {
  setOptional(true);
  // TODO: check when to start WasmServerFeature
  startsAfter<CommunicationFeaturePhase>();
  startsAfter<DatabaseFeaturePhase>();
}

void WasmServerFeature::prepare() {
  if (ServerState::instance()->isCoordinator() or
      ServerState::instance()->isDBServer()) {
    setEnabled(true);
  } else {
    setEnabled(false);
  }
}

void WasmServerFeature::collectOptions(
    std::shared_ptr<options::ProgramOptions>) {}
void WasmServerFeature::validateOptions(
    std::shared_ptr<options::ProgramOptions>) {}

void WasmServerFeature::addFunction(wasm::WasmFunction const& function) {
  _guardedFunctions.doUnderLock(
      [&function](GuardedFunctions& guardedFunctions) {
        guardedFunctions._functions.emplace(function.name(), function);
      });
}

auto WasmServerFeature::loadFunction(std::string const& name)
    -> std::optional<wasm3::module> {
  auto function =
      _guardedFunctions.doUnderLock([&name](GuardedFunctions& guardedFunctions)
                                        -> std::optional<wasm::WasmFunction> {
        auto maybef = guardedFunctions._functions.find(name);
        if (maybef == std::end(guardedFunctions._functions)) {
          return std::nullopt;
        } else {
          return maybef->second;
        }
      });
  if (function.has_value()) {
    return environment.parse_module(function->code.code, function->code.length);
  } else {
    // TODO
    return std::nullopt;
  }
}

auto WasmServerFeature::executeFunction(std::string const& name, uint64_t a,
                                        uint64_t b) -> std::optional<uint64_t> {
  auto module = loadFunction(name);
  auto runtime = environment.new_runtime(1024);
  if (module.has_value()) {
    runtime.load(module.value());
    auto function = runtime.find_function(name.c_str());
    return function.call<uint64_t>(a, b);
  } else {
    // TODO
  }
}

void WasmServerFeature::deleteFunction(std::string const& functionName) {
  _guardedFunctions.doUnderLock(
      [&functionName](GuardedFunctions& guardedFunctions) {
        guardedFunctions._functions.erase(functionName);
      });
}
