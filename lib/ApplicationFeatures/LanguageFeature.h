////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2016 ArangoDB GmbH, Cologne, Germany
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
/// @author Dr. Frank Celler
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGODB_APPLICATION_FEATURES_LANGUAGE_FEATURE_H
#define ARANGODB_APPLICATION_FEATURES_LANGUAGE_FEATURE_H 1

#include <unicode/locid.h>
#include "ApplicationFeatures/ApplicationFeature.h"

namespace arangodb {

class LanguageFeature final : public application_features::ApplicationFeature {
 public:
  explicit LanguageFeature(application_features::ApplicationServer& server);
  ~LanguageFeature();

  void collectOptions(std::shared_ptr<options::ProgramOptions>) override final;
  void prepare() override final;
  void start() override final;
  static void* prepareIcu(std::string const& binaryPath, std::string const& binaryExecutionPath, std::string& path, std::string const& binaryName);
  static LanguageFeature* instance();
  Locale& getLocale() {return _locale; }

private:
  Locale _locale;
  std::string _language;
  char const* _binaryPath;
  void* _icuDataPtr;
};

}

#endif