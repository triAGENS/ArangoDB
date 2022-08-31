////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2020 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
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
/// @author Markus Pfeiffer
/// @author Michael Hackstein
////////////////////////////////////////////////////////////////////////////////

// Macro defined to avoid duplicate symbols when linking
#define ARANGODB_INCLUDED_FROM_GTESTS
#include "Aql/ExecutionBlockImpl/ExecutionBlockImpl.tpp"
#undef ARANGODB_INCLUDED_FROM_GTESTS

#include "TestEmptyExecutorHelper.h"
#include "TestLambdaExecutor.h"

template class ::arangodb::aql::ExecutionBlockImpl<TestLambdaExecutor>;
template class ::arangodb::aql::ExecutionBlockImpl<TestLambdaSkipExecutor>;
