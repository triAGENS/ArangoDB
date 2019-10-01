////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2016 ArangoDB GmbH, Cologne, Germany
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
/// @author Jan Steemann
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGOD_UTILS_COLLECTION_KEYS_REPOSITORY_H
#define ARANGOD_UTILS_COLLECTION_KEYS_REPOSITORY_H 1

#include "Basics/Common.h"
#include "Basics/Mutex.h"
#include "Basics/MutexLocker.h"
#include "Utils/CollectionKeys.h"
#include "VocBase/voc-types.h"

namespace arangodb {

class CollectionKeys;

class CollectionKeysRepository {
 public:
  //////////////////////////////////////////////////////////////////////////////
  /// @brief create a collection keys repository
  //////////////////////////////////////////////////////////////////////////////

  CollectionKeysRepository();

  //////////////////////////////////////////////////////////////////////////////
  /// @brief destroy a collection keys repository
  //////////////////////////////////////////////////////////////////////////////

  ~CollectionKeysRepository();

 public:
  //////////////////////////////////////////////////////////////////////////////
  /// @brief stores collection keys in the repository
  //////////////////////////////////////////////////////////////////////////////

  void store(std::unique_ptr<CollectionKeys>);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief remove a keys entry by id
  //////////////////////////////////////////////////////////////////////////////

  bool remove(CollectionKeysId id);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief find an existing collection keys list by id
  /// if found, the keys will be returned with the usage flag set to true.
  /// it must be returned later using release()
  //////////////////////////////////////////////////////////////////////////////

  CollectionKeys* find(CollectionKeysId);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief return a cursor
  //////////////////////////////////////////////////////////////////////////////

  void release(CollectionKeys*);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief whether or not the repository contains used keys
  //////////////////////////////////////////////////////////////////////////////

  bool containsUsed();

  /// @brief return number of elements in the repository
  size_t count();

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief run a garbage collection on the data, returns if anything was left
  /// over.
  ////////////////////////////////////////////////////////////////////////////////

  bool garbageCollect(bool force);

  //////////////////////////////////////////////////////////////////////////
  /// @brief stop further stores, this is used on shutdown
  //////////////////////////////////////////////////////////////////////////

  void stopStores() {
    MUTEX_LOCKER(mutexLocker, _lock);
    _stopped = true;
  }

 private:
  //////////////////////////////////////////////////////////////////////////////
  /// @brief mutex for the repository
  //////////////////////////////////////////////////////////////////////////////

  Mutex _lock;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief list of current keys
  //////////////////////////////////////////////////////////////////////////////

  std::unordered_map<CollectionKeysId, std::unique_ptr<CollectionKeys>> _keys;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief maximum number of keys to garbage-collect in one go
  //////////////////////////////////////////////////////////////////////////////

  static size_t const MaxCollectCount;

  ////////////////////////////////////////////////////////////////////////////
  /// @brief stopped flag, indicating that no more CollectionKeys can be
  /// stored
  ////////////////////////////////////////////////////////////////////////////

  bool _stopped;
};
}  // namespace arangodb

#endif
