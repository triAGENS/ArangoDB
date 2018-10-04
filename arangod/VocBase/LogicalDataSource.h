////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2018 ArangoDB GmbH, Cologne, Germany
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

#ifndef ARANGOD__VOCBASE__LOGICAL_DATA_SOURCE_H
#define ARANGOD__VOCBASE__LOGICAL_DATA_SOURCE_H 1

#include "voc-types.h"
#include "Basics/Result.h"
#include "VocBase/LogicalDataSourceCategory.h"

struct TRI_vocbase_t; // forward declaration

namespace arangodb {

namespace velocypack {

class Builder; // forward declaration
class StringRef; // forward declaration

}

////////////////////////////////////////////////////////////////////////////////
/// @brief a common ancestor to all database objects proving access to documents
///        e.g. LogicalCollection / LoigcalView
////////////////////////////////////////////////////////////////////////////////
class LogicalDataSource {
 public:

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
    static Type const& emplace(arangodb::velocypack::StringRef const& name);
    std::string const& name() const noexcept { return _name; }

   private:
    std::string _name; // type name for e.g. log messages

    Type() = default;
    Type(Type const&) = delete;
    Type& operator=(Type const&) = delete;
    Type& operator=(Type&&) noexcept = delete;
  };

  //////////////////////////////////////////////////////////////////////////////
  /// @brief constructor for a logical data-source taking configuration values
  ///        from 'definition'
  //////////////////////////////////////////////////////////////////////////////
  LogicalDataSource(
    Type const& type,
    TRI_vocbase_t& vocbase,
    velocypack::Slice const& definition,
    uint64_t planVersion
  );

  //////////////////////////////////////////////////////////////////////////////
  /// @brief constructor for a logical data-source
  /// @note 'id' autogenerated IFF 'id' == 0
  /// @note 'planId' taken from evaluated value of 'id' IFF 'planId' == 0
  /// @note 'guid' autogenerated IFF 'guid'.empty()
  //////////////////////////////////////////////////////////////////////////////
  LogicalDataSource(
    Type const& type,
    TRI_vocbase_t& vocbase,
    TRI_voc_cid_t id,
    std::string&& guid,
    TRI_voc_cid_t planId,
    std::string&& name,
    uint64_t planVersion,
    bool system,
    bool deleted
  );

  LogicalDataSource(LogicalDataSource const& other)
    : _name(other._name),
      _type(other._type),
      _vocbase(other._vocbase),
      _id(other._id),
      _planId(other._planId),
      _planVersion(other._planVersion),
      _guid(other._guid),
      _deleted(other._deleted),
      _system(other._system) {
  }

  virtual ~LogicalDataSource() = default;

  virtual LogicalDataSourceCategory category() const = 0;
  bool deleted() const noexcept { return _deleted; }
  virtual Result drop() = 0;
  std::string const& guid() const noexcept { return _guid; }
  TRI_voc_cid_t const& id() const noexcept { return _id; } // reference required for ShardDistributionReporterTest
  std::string const& name() const noexcept { return _name; }
  TRI_voc_cid_t planId() const noexcept { return _planId; }
  uint64_t planVersion() const noexcept { return _planVersion; }
  virtual Result rename(std::string&& newName, bool doSync) = 0;
  bool system() const noexcept { return _system; }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief append a jSON definition of the data-source to the 'builder'
  /// @param the buffer to append to, must be an open object
  /// @param detailed make output more detailed, e.g.
  ///        for collections this will resolve CIDs for 'distributeShardsLike'
  ///        for views this will add view-specific properties
  /// @param forPersistence this definition is meant to be persisted
  /// @return success
  //////////////////////////////////////////////////////////////////////////////
  Result toVelocyPack(
    velocypack::Builder& builder,
    bool detailed,
    bool forPersistence
  ) const /*final*/;

  Type const& type() const noexcept { return _type; }
  TRI_vocbase_t& vocbase() const noexcept { return _vocbase; }

 protected:

  //////////////////////////////////////////////////////////////////////////////
  /// @brief append implementation-specific values to the data-source definition
  //////////////////////////////////////////////////////////////////////////////
  virtual Result appendVelocyPack(
    velocypack::Builder&,
      bool /*detailed*/,
      bool /*forPersistence*/
  ) const {
    return Result(); // NOOP by default
  }

  void deleted(bool deleted) noexcept { _deleted = deleted; }
  void name(std::string&& name) noexcept { _name = std::move(name); }

 private:
  // members ordered by sizeof(decltype(..)) except for '_guid'
  std::string _name; // data-source name
  Type const& _type; // the type of the underlying data-source implementation
  TRI_vocbase_t& _vocbase; // the database where the data-source resides
  TRI_voc_cid_t const _id; // local data-source id (current database node)
  TRI_voc_cid_t const _planId; // global data-source id (cluster-wide)
  uint64_t const _planVersion; // Only set if setPlanVersion was called. This only
                           // happens in ClusterInfo when this object is used
                           // to represent a cluster wide collection. This is
                           // then the version in the agency Plan that underpins
                           // the information in this object. Otherwise 0.
  std::string const _guid; // globally unique data-source id (cluster-wide) for proper initialization must be positioned after '_name' and '_planId' since they are autogenerated
  bool _deleted; // data-source marked as deleted
  bool const _system; // this instance represents a system data-source
};

} // arangodb

#endif // ARANGOD__VOCBASE__LOGICAL_DATA_SOURCE_H
