////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2021 ArangoDB GmbH, Cologne, Germany
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
/// @author Andrey Abramov
/// @author Vasiliy Nabatchikov
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "Basics/Result.h"
#include "VocBase/Identifiers/DataSourceId.h"
#include "voc-types.h"

#include <atomic>
#include <string_view>

struct TRI_vocbase_t;  // forward declaration

namespace arangodb {

class LogicalCollection;
class LogicalView;

namespace velocypack {
class Builder;  // forward declaration
}  // namespace velocypack

////////////////////////////////////////////////////////////////////////////////
/// @brief a common ancestor to all database objects proving access to documents
///        e.g. LogicalCollection / LoigcalView
////////////////////////////////////////////////////////////////////////////////
class LogicalDataSource {
 public:
  enum class Category {
    kCollection = 1,
    kView = 2,
  };

  //////////////////////////////////////////////////////////////////////////////
  /// @brief singleton identifying the underlying implementation type
  ///        each implementation should have its own static instance
  ///        once a type is emplace(...)ed it cannot be removed
  //////////////////////////////////////////////////////////////////////////////
  class Type final {
   public:
    Type(Type&& other) noexcept = default;
    bool operator==(Type const& other) const noexcept { return this == &other; }
    bool operator!=(Type const& other) const noexcept { return this != &other; }
    operator Type const *() const noexcept { return this; }
    static Type const& emplace(std::string_view name);
    std::string const& name() const noexcept { return _name; }

   private:
    std::string _name;  // type name for e.g. log messages

    Type() = default;
    Type(Type const&) = delete;
    Type& operator=(Type const&) = delete;
    Type& operator=(Type&&) noexcept = delete;
  };

  LogicalDataSource(LogicalDataSource const& other) = delete;
  LogicalDataSource& operator=(LogicalDataSource const& other) = delete;

  virtual ~LogicalDataSource() = default;

  Category category() const noexcept;
  bool deleted() const noexcept {
    return _deleted.load(std::memory_order_relaxed);
  }
  virtual Result drop() = 0;
  std::string const& guid() const noexcept { return _guid; }
  DataSourceId id() const noexcept { return _id; }
  std::string const& name() const noexcept { return _name; }
  DataSourceId planId() const noexcept { return _planId; }

  enum class Serialization {
    // object properties will be shown in a list
    List = 0,
    // object properties will be shown
    Properties,
    // object will be saved in storage engine
    Persistence,
    // object will be saved in storage engine
    PersistenceWithInProgress,
    // object will be replicated or dumped/restored
    Inventory
  };

  //////////////////////////////////////////////////////////////////////////////
  /// @brief append a jSON definition of the data-source to the 'builder'
  /// @param the buffer to append to, must be an open object
  /// @param detailed make output more detailed, e.g.
  ///        for collections this will resolve CIDs for 'distributeShardsLike'
  ///        for views this will add view-specific properties
  /// @param Serialization defines which properties to serialize
  /// @return success
  //////////////////////////////////////////////////////////////////////////////
  Result properties(velocypack::Builder& builder, Serialization context) const;

  virtual Result rename(std::string&& newName) = 0;
  bool system() const noexcept { return _system; }
  Type const& type() const noexcept { return _type; }
  TRI_vocbase_t& vocbase() const noexcept { return _vocbase; }

 protected:
  template<typename DataSource, typename... Args>
  explicit LogicalDataSource(DataSource const& /*self*/, Args&&... args)
      : LogicalDataSource{DataSource::category(), std::forward<Args>(args)...} {
  }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief append implementation-specific values to the data-source definition
  //////////////////////////////////////////////////////////////////////////////
  virtual Result appendVelocyPack(velocypack::Builder&, Serialization) const {
    return {};  // NOOP by default
  }

  void deleted(bool deleted) noexcept {
    _deleted.store(deleted, std::memory_order_relaxed);
  }
  void name(std::string&& name) noexcept { _name = std::move(name); }

 private:
  //////////////////////////////////////////////////////////////////////////////
  /// @brief constructor for a logical data-source taking configuration values
  ///        from 'definition'
  //////////////////////////////////////////////////////////////////////////////
  LogicalDataSource(Category category, Type const& type, TRI_vocbase_t& vocbase,
                    velocypack::Slice definition);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief constructor for a logical data-source
  /// @note 'id' autogenerated IFF 'id' == 0
  /// @note 'planId' taken from evaluated value of 'id' IFF 'planId' == 0
  /// @note 'guid' autogenerated IFF 'guid'.empty()
  //////////////////////////////////////////////////////////////////////////////
  LogicalDataSource(Category category, Type const& type, TRI_vocbase_t& vocbase,
                    DataSourceId id, std::string&& guid, DataSourceId planId,
                    std::string&& name, bool system, bool deleted);

  // members ordered by sizeof(decltype(..)) except for '_guid'
  std::string _name;  // data-source name
  Type const& _type;  // the type of the underlying data-source implementation
  TRI_vocbase_t& _vocbase;     // the database where the data-source resides
  DataSourceId const _id;      // local data-source id (current database node)
  DataSourceId const _planId;  // global data-source id (cluster-wide)
  std::string const
      _guid;  // globally unique data-source id (cluster-wide) for proper
              // initialization must be positioned after '_name' and '_planId'
              // since they are autogenerated
  std::atomic<bool> _deleted;  // data-source marked as deleted
  Category const _category;    // the category of the logical data-source
  bool const _system;          // this instance represents a system data-source
};

}  // namespace arangodb
