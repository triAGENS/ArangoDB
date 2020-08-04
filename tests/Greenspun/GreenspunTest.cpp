#include <iostream>

#include "Pregel/Algos/AIR/Greenspun/Interpreter.h"
#include "Pregel/Algos/AIR/Greenspun/Primitives.h"
#include "velocypack/Builder.h"
#include "velocypack/Parser.h"
#include "velocypack/velocypack-aliases.h"
#include "./structs/EvalContext.h"

/* YOLO */

namespace arangodb::basics::VelocyPackHelper {

int compare(arangodb::velocypack::Slice, arangodb::velocypack::Slice, bool,
            arangodb::velocypack::Options const*,
            arangodb::velocypack::Slice const*, arangodb::velocypack::Slice const*) {
  std::cerr << "WARNING! YOU ARE ABOUT TO SHOOT YOURSELF IN THE FOOT!" << std::endl;
  return 0;
}

}  // namespace arangodb::basics::VelocyPackHelper


int main(int argc, char** argv) {
  InitInterpreter();

  MyEvalContext ctx;
  VPackBuilder result;

  auto v = arangodb::velocypack::Parser::fromJson(R"aql("aNodeId")aql");
  auto S = arangodb::velocypack::Parser::fromJson(R"aql("anotherNodeId")aql");

  auto program = arangodb::velocypack::Parser::fromJson(R"aql(
  ["+", 3,
    ["if",
      [["eq?", ["+", 12, 2], 2], 3],
      [true, 1]
    ]
  ]
  )aql");

  std::cout << "ArangoLISP Interpreter Executing" << std::endl;
  std::cout << " " << program->toJson() << std::endl;

  auto res = Evaluate(ctx, program->slice(), result).wrapError([](EvalError& err) { err.wrapMessage("at top-level"); });
  if (res.fail()) {
    std::cerr << "Evaluate failed: " << res.error().toString() << std::endl;
  } else {
    std::cout << " ArangoLISP executed, result " << result.toJson() << std::endl;
  }

  return 0;
}
