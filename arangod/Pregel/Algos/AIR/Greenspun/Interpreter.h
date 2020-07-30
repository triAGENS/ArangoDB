////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2020 ArangoDB GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Heiko Kernbach
/// @author Lars Maier
/// @author Markus Pfeiffer
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGODB_PREGEL_ALGOS_VERTEX_ACCUMULATORS_EVALUATOR_H
#define ARANGODB_PREGEL_ALGOS_VERTEX_ACCUMULATORS_EVALUATOR_H 1

#include <functional>
#include <map>
#include <variant>

#include <velocypack/Builder.h>
#include <velocypack/Iterator.h>
#include <velocypack/Slice.h>
#include <velocypack/velocypack-aliases.h>
#include "template-stuff.h"

#include "EvalResult.h"

namespace arangodb {
namespace greenspun {

struct EvalContext {
  EvalContext() noexcept;
  virtual ~EvalContext() = default;
  // Variables go here.
  void pushStack();
  void popStack();
  EvalResult setVariable(std::string name, VPackSlice value);
  EvalResult getVariable(std::string const& name, VPackBuilder& result);


  size_t depth{0};

  using function_type =
      std::function<EvalResult(EvalContext& ctx, VPackSlice const slice, VPackBuilder& result)>;

  EvalResult setFunction(std::string name, function_type&& f);
  EvalResult unsetFunction(std::string name);
 private:
  std::vector<std::unordered_map<std::string, VPackSlice>> variables;
  std::unordered_map<std::string, function_type> functions;
};

template <bool isNewScope>
struct StackFrameGuard {
  StackFrameGuard(EvalContext& ctx) : _ctx(ctx) {
    ctx.depth += 1;
    if constexpr (isNewScope) {
      ctx.pushStack();
    }
  }

  ~StackFrameGuard() {
    _ctx.depth -= 1;
    if constexpr (isNewScope) {
      _ctx.popStack();
    }
  }

  EvalContext& _ctx;
};

EvalResult Evaluate(EvalContext& ctx, VPackSlice slice, VPackBuilder& result);
void InitInterpreter();

bool ValueConsideredTrue(VPackSlice const value);
bool ValueConsideredFalse(VPackSlice const value);

}  // namespace greenspun
}  // namespace arangodb

#endif
