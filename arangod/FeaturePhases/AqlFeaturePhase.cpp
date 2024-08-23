////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2024 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Business Source License 1.1 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     https://github.com/arangodb/arangodb/blob/devel/LICENSE
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Michael Hackstein
////////////////////////////////////////////////////////////////////////////////

#include "AqlFeaturePhase.h"
#include "ApplicationFeatures/ApplicationServer.h"

namespace arangodb::application_features {

AqlFeaturePhase::AqlFeaturePhase(ArangodServer& server)
    : ApplicationFeaturePhase{server, *this} {
  setOptional(false);
  startsAfter<CommunicationFeaturePhase, ArangodServer>();
#ifdef USE_V8
  startsAfter<V8FeaturePhase, ArangodServer>();
#endif

  startsAfter<AqlFeature, ArangodServer>();
  startsAfter<aql::AqlFunctionFeature, ArangodServer>();
  startsAfter<iresearch::IResearchAnalyzerFeature, ArangodServer>();
  startsAfter<iresearch::IResearchFeature, ArangodServer>();
  startsAfter<aql::OptimizerRulesFeature, ArangodServer>();
  startsAfter<QueryRegistryFeature, ArangodServer>();
  startsAfter<SystemDatabaseFeature, ArangodServer>();
}

}  // namespace arangodb::application_features
