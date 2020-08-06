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
/// @author Lars Maier
////////////////////////////////////////////////////////////////////////////////

#include "Reports.h"

using namespace arangodb::pregel;

ReportBuilder::~ReportBuilder() {
  manager.append(Report{ss.str(), level, std::move(annotations)});
}

ReportBuilder::ReportBuilder(ReportManager& manager, ReportLevel lvl)
    : level(lvl), manager(manager) {}

std::string arangodb::pregel::to_string(ReportLevel lvl) {
  switch(lvl) {
    case ReportLevel::INFO:
      return "info";
    case ReportLevel::WARNING:
      return "warn";
    case ReportLevel::ERROR:
      return "error";
  }
}

namespace {
ReportLevel levelFromString(std::string_view str) {
  if (str == "info") {
    return ReportLevel::INFO;
  } else if (str == "warn") {
    return ReportLevel::WARNING;
  } else {
    return ReportLevel::ERROR;
  }
}
}

Report Report::fromVelocyPack(VPackSlice slice) {
  std::string msg = slice.get("msg").copyString();
  auto level = levelFromString(slice.get("level").stringView());
  ReportAnnotations annotations;
  for (auto&& pair : VPackObjectIterator(slice.get("annotations"))) {
    VPackBuilder builder;
    builder.add(pair.value);
    annotations.emplace(pair.key.copyString(), std::move(builder));
  }

  return Report{msg, level,
                std::move(annotations)};
}

void Report::intoBuilder(VPackBuilder& builder) const {
  VPackObjectBuilder ob(&builder);
  builder.add("msg", VPackValue(message));
  builder.add("level", VPackValue(to_string(level)));
    VPackObjectBuilder pob(&builder, "annotations");
    for (auto&& pair : annotations) {
      builder.add(pair.first, pair.second.slice());
    }
}

auto ReportManager::report(ReportLevel level) -> ReportBuilder {
  return ReportBuilder{*this, level};
}

void ReportManager::append(Report report) {
  if (report.isError()) {
    if (_numErrors >= 20) {
      return;
    }
    _numErrors += 1;
  }
  _reports.emplace_back(std::move(report));
}

void ReportManager::clear() {
  _reports.clear();
  _numErrors = 0;
}

void ReportManager::appendFromSlice(VPackSlice slice) {
  for (auto&& reportSlice : VPackArrayIterator(slice)) {
    append(Report::fromVelocyPack(reportSlice));
  }
}

void ReportManager::intoBuilder(VPackBuilder& builder) const {
  VPackArrayBuilder ab(&builder);
  for (auto&& report : _reports) {
    report.intoBuilder(builder);
  }
}

void ReportManager::append(ReportManager other) {
  std::move(std::begin(other._reports), std::end(other._reports),
            std::back_inserter(_reports));
  _numErrors += other._numErrors;
}
