////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2023 ArangoDB GmbH, Cologne, Germany
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
////////////////////////////////////////////////////////////////////////////////

#include "ModificationExecutorHelpers.h"

#include "Aql/AqlValue.h"
#include "Aql/ModificationExecutorInfos.h"
#include "Basics/Result.h"
#include "Basics/StaticStrings.h"
#include "Basics/cpu-relax.h"
#include "Logger/LogLevel.h"
#include "Logger/LogMacros.h"
#include "Random/RandomGenerator.h"
#include "Scheduler/SchedulerFeature.h"
#include "Utils/CollectionNameResolver.h"
#include "Utils/OperationResult.h"

#include <velocypack/Builder.h>
#include <velocypack/Iterator.h>
#include <velocypack/Slice.h>

#include <chrono>
#include <string>

using namespace arangodb;
using namespace arangodb::aql;
using namespace arangodb::basics;

Result ModificationExecutorHelpers::getKey(
    CollectionNameResolver const& resolver, AqlValue const& value,
    std::string& key) {
  TRI_ASSERT(key.empty());

  // If `value` is a string, this is our _key entry, so we use that.
  if (value.isString()) {
    key.assign(value.slice().copyString());
    return Result{};
  }

  if (!value.isObject()) {
    return Result(TRI_ERROR_ARANGO_DOCUMENT_TYPE_INVALID,
                  std::string{"Expected object or string, but got "} +
                      value.slice().typeName());
  }

  // not necessary to check if key exists in object, since AqlValue::get() will
  // return a null-result below in case key does not exist.

  // Extract key from `value`, and make sure it is a string
  bool mustDestroyKey;
  AqlValue keyEntry =
      value.get(resolver, StaticStrings::KeyString, mustDestroyKey, false);
  AqlValueGuard keyGuard(keyEntry, mustDestroyKey);

  if (!keyEntry.isString()) {
    return Result{
        TRI_ERROR_ARANGO_DOCUMENT_KEY_MISSING,
        std::string{"Expected _key to be a string attribute in document."}};
  }

  // Key found and assigned, note rev is empty by assertion
  key.assign(keyEntry.slice().copyString());

  return Result{};
}

Result ModificationExecutorHelpers::getRevision(
    CollectionNameResolver const& resolver, AqlValue const& value,
    std::string& rev) {
  TRI_ASSERT(rev.empty());

  if (!value.isObject()) {
    return Result(
        TRI_ERROR_ARANGO_DOCUMENT_TYPE_INVALID,
        std::string{"Expected object, but got "} + value.slice().typeName());
  }

  if (value.hasKey(StaticStrings::RevString)) {
    bool mustDestroyRev;
    AqlValue revEntry =
        value.get(resolver, StaticStrings::RevString, mustDestroyRev, false);
    AqlValueGuard revGuard(revEntry, mustDestroyRev);

    if (!revEntry.isString()) {
      return Result(TRI_ERROR_ARANGO_DOCUMENT_TYPE_INVALID,
                    std::string{"Expected _rev as string, but got "} +
                        value.slice().typeName());
    }

    // Rev found and assigned
    rev.assign(revEntry.slice().copyString());
  }  // else we leave rev empty
  return Result{};
}

Result ModificationExecutorHelpers::getKeyAndRevision(
    CollectionNameResolver const& resolver, AqlValue const& value,
    std::string& key, std::string& rev) {
  Result result = getKey(resolver, value, key);
  // The key can either be a string, or contained in an object
  // If it is passed in as a string, then there is no revision
  // and there is no point in extracting it further on.
  if (!result.ok() || value.isString()) {
    return result;
  }
  return getRevision(resolver, value, rev);
}

void ModificationExecutorHelpers::buildKeyDocument(velocypack::Builder& builder,
                                                   std::string const& key) {
  builder.openObject();
  builder.add(StaticStrings::KeyString, VPackValue(key));
  builder.close();
}

void ModificationExecutorHelpers::buildKeyAndRevDocument(
    velocypack::Builder& builder, std::string const& key,
    std::string const& rev) {
  builder.openObject();
  builder.add(StaticStrings::KeyString, VPackValue(key));

  if (rev.empty()) {
    // Necessary to sometimes remove _rev entries
    builder.add(StaticStrings::RevString, VPackValue(VPackValueType::Null));
  } else {
    builder.add(StaticStrings::RevString, VPackValue(rev));
  }
  builder.close();
}

bool ModificationExecutorHelpers::writeRequired(
    ModificationExecutorInfos const& infos, velocypack::Slice doc,
    std::string const& key) {
  return (!infos._consultAqlWriteFilter ||
          !infos._aqlCollection->getCollection()->skipForAqlWrite(doc, key));
}

void ModificationExecutorHelpers::throwOperationResultException(
    ModificationExecutorInfos const& infos,
    OperationResult const& operationResult) {
  // A "higher level error" happened (such as the transaction being aborted,
  // replication being refused, etc ), and we do not have errorCounter or
  // similar so we throw.
  if (!operationResult.ok()) {
    // inside OperationResult hides a small result.
    THROW_ARANGO_EXCEPTION(operationResult.result);
  }

  auto const& errorCounter = operationResult.countErrorCodes;

  // Early escape if we are ignoring errors.
  if (infos._ignoreErrors == true || errorCounter.empty()) {
    return;
  }

  // Find the first relevant error for which we want to throw.
  // If _ignoreDocumentNotFound is true, then this is any error other than
  // TRI_ERROR_ARANGO_DOCUMENT_NOT_FOUND, otherwise it is just any error.
  //
  // Find the first error with a message and throw that
  // This mirrors previous behaviour and might not be entirely ideal.
  for (auto const& p : errorCounter) {
    auto const errorCode = ErrorCode{p.first};
    if (!(infos._ignoreDocumentNotFound &&
          errorCode == TRI_ERROR_ARANGO_DOCUMENT_NOT_FOUND)) {
      // Find the first error and throw with message.
      for (auto doc : VPackArrayIterator(operationResult.slice())) {
        if (doc.isObject() && doc.hasKey(StaticStrings::ErrorNum) &&
            ErrorCode{doc.get(StaticStrings::ErrorNum).getNumber<int>()} ==
                errorCode) {
          VPackSlice s = doc.get(StaticStrings::ErrorMessage);
          if (s.isString()) {
            THROW_ARANGO_EXCEPTION_MESSAGE(errorCode, s.copyString());
          }
        }
      }
      // if we did not find a message, we still throw something, because we
      // know that a relevant error has happened
      THROW_ARANGO_EXCEPTION(errorCode);
    }
  }
}

// Convert ModificationOptions to OperationOptions struct
OperationOptions ModificationExecutorHelpers::convertOptions(
    ModificationOptions const& in, Variable const* outVariableNew,
    Variable const* outVariableOld) {
  OperationOptions out;

  // commented out OperationOptions attributes are not provided
  // by the ModificationOptions or the information given by the
  // Variable pointer.

  // in.ignoreErrors;
  out.waitForSync = in.waitForSync;
  out.validate = in.validate;
  out.keepNull = in.keepNull;
  out.mergeObjects = in.mergeObjects;
  // in.ignoreDocumentNotFound;
  out.isRestore = in.isRestore;
  // in.consultAqlWriteFilter;
  // in.exclusive;
  out.overwriteMode = in.overwriteMode;
  out.ignoreRevs = in.ignoreRevs;
  out.refillIndexCaches = in.refillIndexCaches;

  out.returnNew = (outVariableNew != nullptr);
  out.returnOld = (outVariableOld != nullptr);
  out.silent = !(out.returnNew || out.returnOld);

  return out;
}

AqlValue ModificationExecutorHelpers::getDocumentOrNull(
    velocypack::Slice elm, std::string const& key) {
  if (velocypack::Slice s = elm.get(key); !s.isNone()) {
    return AqlValue{s};
  }
  return AqlValue(AqlValueHintNull());
}

// If we simply wait, it can happen that we get into a blockage in which
// all threads wait in the same place here and none can make progress,
// since the scheduler is full. This means we must detach the thread
// after some time. To avoid that all are detaching at the same time,
// we choose a random timeout for the detaching. But first we spin a
// while to avoid delays:
void ModificationExecutorHelpers::waitAndDetach(
    futures::Future<OperationResult>& future) {
    if (!future.isReady()) {
        {
          auto const spinTime = std::chrono::milliseconds(10);
          auto const start = std::chrono::steady_clock::now();
          while (!future.isReady() &&
                 std::chrono::steady_clock::now() - start < spinTime) {
            basics::cpu_relax();
          }
        }
        if (!future.isReady()) {
            auto const detachTime = std::chrono::milliseconds(
                    1000 + RandomGenerator::interval(uint32_t(100)) * 100);
            auto start = std::chrono::steady_clock::now();
            while (!future.isReady() &&
                   std::chrono::steady_clock::now() - start < detachTime) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (!future.isReady()) {
                LOG_TOPIC("afe32", INFO, Logger::THREADS)
                        << "Did not get replication response within " << detachTime.count()
                        << " milliseconds, detaching scheduler thread.";
                uint64_t currentNumberDetached = 0;
                uint64_t maximumNumberDetached = 0;
                auto res = SchedulerFeature::SCHEDULER->detachThread(
                        &currentNumberDetached, &maximumNumberDetached);
                if (res.is(TRI_ERROR_TOO_MANY_DETACHED_THREADS)) {
                    LOG_TOPIC("afe33", WARN, Logger::THREADS)
                            << "Could not detach scheduler thread (currently detached "
                               "threads: "
                            << currentNumberDetached
                            << ", maximal number of detached threads: "
                            << maximumNumberDetached
                            << "), will continue to wait for replication in scheduler "
                               "thread, this can potentially lead to blockages!";
                }
                future.wait();
            }
        }
    }
}
