////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2019 ArangoDB GmbH, Cologne, Germany
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
/// @author Tobias Gödderz
////////////////////////////////////////////////////////////////////////////////

#include "AqlHelper.h"

#include "Aql/ExecutionStats.h"

using namespace arangodb;
using namespace arangodb::aql;

std::ostream& arangodb::aql::operator<<(std::ostream& stream, ExecutionStats const& stats) {
  VPackBuilder builder{};
  stats.toVelocyPack(builder, true);
  return stream << builder.toJson();
}

std::ostream& arangodb::aql::operator<<(std::ostream& stream, AqlItemBlock const& block) {
  stream << "[";
  for (size_t row = 0; row < block.size(); row++) {
    if (row > 0) {
      stream << ",";
    }
    stream << " ";
    VPackBuilder builder{};
    builder.openArray();
    for (RegisterId reg = 0; reg < block.getNrRegs(); reg++) {
      if (reg > 0) {
        stream << ",";
      }
      // will not work for docvecs or ranges
      builder.add(block.getValueReference(row, reg).slice());
    }
    builder.close();
    stream << builder.toJson();
  }
  stream << " ]";
  return stream;
}

bool arangodb::aql::operator==(arangodb::aql::ExecutionStats const& left,
                               arangodb::aql::ExecutionStats const& right) {
  TRI_ASSERT(left.nodes.empty());
  TRI_ASSERT(right.nodes.empty());
  TRI_ASSERT(left.executionTime == 0.0);
  TRI_ASSERT(right.executionTime == 0.0);
  TRI_ASSERT(left.peakMemoryUsage == 0);
  TRI_ASSERT(right.peakMemoryUsage == 0);
  // clang-format off
  return left.writesExecuted == right.writesExecuted
      && left.writesIgnored == right.writesIgnored
      && left.scannedFull == right.scannedFull
      && left.scannedIndex == right.scannedIndex
      && left.filtered == right.filtered
      && left.requests == right.requests
      && left.fullCount == right.fullCount
      && left.count == right.count;
  // clang-format on
}
bool arangodb::aql::operator==(arangodb::aql::AqlItemBlock const& left,
                               arangodb::aql::AqlItemBlock const& right) {
  if (left.size() != right.size()) {
    return false;
  }
  if (left.getNrRegs() != right.getNrRegs()) {
    return false;
  }
  size_t const rows = left.size();
  RegisterCount const regs = left.getNrRegs();
  for (size_t row = 0; row < rows; row++) {
    for (RegisterId reg = 0; reg < regs; reg++) {
      AqlValue const& l = left.getValueReference(row, reg);
      AqlValue const& r = right.getValueReference(row, reg);
      // Doesn't work for docvecs or ranges
      if (l.slice() != r.slice()) {
        return false;
      }
    }
  }

  return true;
}
