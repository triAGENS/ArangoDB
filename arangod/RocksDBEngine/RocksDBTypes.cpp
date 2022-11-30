////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2022 ArangoDB GmbH, Cologne, Germany
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
/// @author Jan Christoph Uhde
////////////////////////////////////////////////////////////////////////////////

#include "RocksDBTypes.h"

using namespace arangodb;

/// @brief rocksdb format version
char arangodb::rocksDBFormatVersion() { return '1'; }

namespace {

static RocksDBEntryType placeholder = arangodb::RocksDBEntryType::Placeholder;
static rocksdb::Slice Placeholder(
    reinterpret_cast<std::underlying_type<RocksDBEntryType>::type*>(
        &placeholder),
    1);

static RocksDBEntryType database = arangodb::RocksDBEntryType::Database;
static rocksdb::Slice Database(
    reinterpret_cast<std::underlying_type<RocksDBEntryType>::type*>(&database),
    1);

static RocksDBEntryType collection = RocksDBEntryType::Collection;
static rocksdb::Slice Collection(
    reinterpret_cast<std::underlying_type<RocksDBEntryType>::type*>(
        &collection),
    1);

static RocksDBEntryType counterVal = RocksDBEntryType::CounterValue;
static rocksdb::Slice CounterValue(
    reinterpret_cast<std::underlying_type<RocksDBEntryType>::type*>(
        &counterVal),
    1);

static RocksDBEntryType document = RocksDBEntryType::Document;
static rocksdb::Slice Document(
    reinterpret_cast<std::underlying_type<RocksDBEntryType>::type*>(&document),
    1);

static RocksDBEntryType primaryIndexValue = RocksDBEntryType::PrimaryIndexValue;
static rocksdb::Slice PrimaryIndexValue(
    reinterpret_cast<std::underlying_type<RocksDBEntryType>::type*>(
        &primaryIndexValue),
    1);

static RocksDBEntryType edgeIndexValue = RocksDBEntryType::EdgeIndexValue;
static rocksdb::Slice EdgeIndexValue(
    reinterpret_cast<std::underlying_type<RocksDBEntryType>::type*>(
        &edgeIndexValue),
    1);

static RocksDBEntryType vpackIndexValue = RocksDBEntryType::VPackIndexValue;
static rocksdb::Slice VPackIndexValue(
    reinterpret_cast<std::underlying_type<RocksDBEntryType>::type*>(
        &vpackIndexValue),
    1);

static RocksDBEntryType uniqueVPIndex = RocksDBEntryType::UniqueVPackIndexValue;
static rocksdb::Slice UniqueVPackIndexValue(
    reinterpret_cast<std::underlying_type<RocksDBEntryType>::type*>(
        &uniqueVPIndex),
    1);

static RocksDBEntryType fulltextIndexValue =
    RocksDBEntryType::FulltextIndexValue;
static rocksdb::Slice FulltextIndexValue(
    reinterpret_cast<std::underlying_type<RocksDBEntryType>::type*>(
        &fulltextIndexValue),
    1);

static RocksDBEntryType geoIndexValue = RocksDBEntryType::LegacyGeoIndexValue;
static rocksdb::Slice LegacyGeoIndexValue(
    reinterpret_cast<std::underlying_type<RocksDBEntryType>::type*>(
        &geoIndexValue),
    1);

static RocksDBEntryType s2IndexValue = RocksDBEntryType::GeoIndexValue;
static rocksdb::Slice GeoIndexValue(
    reinterpret_cast<std::underlying_type<RocksDBEntryType>::type*>(
        &s2IndexValue),
    1);

static RocksDBEntryType view = RocksDBEntryType::View;
static rocksdb::Slice View(
    reinterpret_cast<std::underlying_type<RocksDBEntryType>::type*>(&view), 1);

static RocksDBEntryType settingsValue = RocksDBEntryType::SettingsValue;
static rocksdb::Slice SettingsValue(
    reinterpret_cast<std::underlying_type<RocksDBEntryType>::type*>(
        &settingsValue),
    1);

static RocksDBEntryType replicationApplierConfig =
    RocksDBEntryType::ReplicationApplierConfig;
static rocksdb::Slice ReplicationApplierConfig(
    reinterpret_cast<std::underlying_type<RocksDBEntryType>::type*>(
        &replicationApplierConfig),
    1);

static RocksDBEntryType indexEstimateValue =
    RocksDBEntryType::IndexEstimateValue;
static rocksdb::Slice IndexEstimateValue(
    reinterpret_cast<std::underlying_type<RocksDBEntryType>::type*>(
        &indexEstimateValue),
    1);

static RocksDBEntryType keyGeneratorValue = RocksDBEntryType::KeyGeneratorValue;
static rocksdb::Slice KeyGeneratorValue(
    reinterpret_cast<std::underlying_type<RocksDBEntryType>::type*>(
        &keyGeneratorValue),
    1);

static RocksDBEntryType logEntry = RocksDBEntryType::LogEntry;
static rocksdb::Slice LogEntry(
    reinterpret_cast<std::underlying_type<RocksDBEntryType>::type*>(&logEntry),
    1);

static RocksDBEntryType replicatedLog = RocksDBEntryType::ReplicatedLog;
static rocksdb::Slice ReplicatedLog(
    reinterpret_cast<std::underlying_type<RocksDBEntryType>::type*>(
        &replicatedLog),
    1);

static RocksDBEntryType replicatedState = RocksDBEntryType::ReplicatedState;
static rocksdb::Slice ReplicatedState(
    reinterpret_cast<std::underlying_type<RocksDBEntryType>::type*>(
        &replicatedState),
    1);

static RocksDBEntryType revisionTreeValue = RocksDBEntryType::RevisionTreeValue;
static rocksdb::Slice RevisionTreeValue(
    reinterpret_cast<std::underlying_type<RocksDBEntryType>::type*>(
        &revisionTreeValue),
    1);

static RocksDBEntryType zkdIndexValue = RocksDBEntryType::ZkdIndexValue;
static rocksdb::Slice ZdkIndexValue(
    reinterpret_cast<std::underlying_type<RocksDBEntryType>::type*>(
        &zkdIndexValue),
    1);

static RocksDBEntryType uniqueZkdIndexValue =
    RocksDBEntryType::UniqueZkdIndexValue;
static rocksdb::Slice UniqueZdkIndexValue(
    reinterpret_cast<std::underlying_type<RocksDBEntryType>::type*>(
        &uniqueZkdIndexValue),
    1);
}  // namespace

char const* arangodb::rocksDBEntryTypeName(arangodb::RocksDBEntryType type) {
  switch (type) {
    case arangodb::RocksDBEntryType::Placeholder:
      return "Placeholder";
    case arangodb::RocksDBEntryType::Database:
      return "Database";
    case arangodb::RocksDBEntryType::Collection:
      return "Collection";
    case arangodb::RocksDBEntryType::CounterValue:
      return "CounterValue";
    case arangodb::RocksDBEntryType::Document:
      return "Document";
    case arangodb::RocksDBEntryType::PrimaryIndexValue:
      return "PrimaryIndexValue";
    case arangodb::RocksDBEntryType::EdgeIndexValue:
      return "EdgeIndexValue";
    case arangodb::RocksDBEntryType::VPackIndexValue:
      return "VPackIndexValue";
    case arangodb::RocksDBEntryType::UniqueVPackIndexValue:
      return "UniqueVPackIndexValue";
    case arangodb::RocksDBEntryType::View:
      return "View";
    case arangodb::RocksDBEntryType::SettingsValue:
      return "SettingsValue";
    case arangodb::RocksDBEntryType::ReplicationApplierConfig:
      return "ReplicationApplierConfig";
    case arangodb::RocksDBEntryType::FulltextIndexValue:
      return "FulltextIndexValue";
    case arangodb::RocksDBEntryType::LegacyGeoIndexValue:
      return "LegacyGeoIndexValue";
    case arangodb::RocksDBEntryType::GeoIndexValue:
      return "SphericalIndexValue";
    case arangodb::RocksDBEntryType::IndexEstimateValue:
      return "IndexEstimateValue";
    case arangodb::RocksDBEntryType::KeyGeneratorValue:
      return "KeyGeneratorValue";
    case arangodb::RocksDBEntryType::RevisionTreeValue:
      return "RevisionTreeValue";
    case arangodb::RocksDBEntryType::ZkdIndexValue:
      return "ZkdIndexValue";
    case arangodb::RocksDBEntryType::UniqueZkdIndexValue:
      return "UniqueZkdIndexValue";
    case RocksDBEntryType::LogEntry:
      return "ReplicatedLogEntry";
    case RocksDBEntryType::ReplicatedLog:
      return "ReplicatedLog";
    case RocksDBEntryType::ReplicatedState:
      return "ReplicatedState";
  }
  return "Invalid";
}

char const* arangodb::rocksDBLogTypeName(arangodb::RocksDBLogType type) {
  switch (type) {
    case arangodb::RocksDBLogType::DatabaseCreate:
      return "DatabaseCreate";
    case arangodb::RocksDBLogType::DatabaseDrop:
      return "DatabaseDrop";
    case arangodb::RocksDBLogType::CollectionCreate:
      return "CollectionCreate";
    case arangodb::RocksDBLogType::CollectionDrop:
      return "CollectionDrop";
    case arangodb::RocksDBLogType::CollectionRename:
      return "CollectionRename";
    case arangodb::RocksDBLogType::CollectionChange:
      return "CollectionChange";
    case arangodb::RocksDBLogType::CollectionTruncate:
      return "CollectionTruncate";
    case arangodb::RocksDBLogType::IndexCreate:
      return "IndexCreate";
    case arangodb::RocksDBLogType::IndexChange:
      return "IndexChange";
    case arangodb::RocksDBLogType::IndexDrop:
      return "IndexDrop";
    case arangodb::RocksDBLogType::ViewCreate:
      return "ViewCreate";
    case arangodb::RocksDBLogType::ViewDrop:
      return "ViewDrop";
    case arangodb::RocksDBLogType::ViewChange:
      return "ViewChange";
    case arangodb::RocksDBLogType::BeginTransaction:
      return "BeginTransaction";
    case arangodb::RocksDBLogType::CommitTransaction:
      return "CommitTransaction";
    case arangodb::RocksDBLogType::DocumentOperationsPrologue:
      return "DocumentOperationsPrologue";
    case arangodb::RocksDBLogType::DocumentRemove:
      return "DocumentRemove";
    case arangodb::RocksDBLogType::DocumentRemoveV2:
      return "DocumentRemoveV2";
    case arangodb::RocksDBLogType::DocumentRemoveAsPartOfUpdate:
      return "IgnoreRemoveAsPartOfUpdate";
    case arangodb::RocksDBLogType::SinglePut:
      return "SinglePut";
    case arangodb::RocksDBLogType::SingleRemove:
      return "SingleRemove";
    case arangodb::RocksDBLogType::SingleRemoveV2:
      return "SingleRemoveV2";
    case arangodb::RocksDBLogType::FlushSync:
      return "FlushSync";
    case RocksDBLogType::TrackedDocumentInsert:
      return "TrackedDocumentInsert";
    case RocksDBLogType::TrackedDocumentRemove:
      return "TrackedDocumentRemove";
    case arangodb::RocksDBLogType::Invalid:
      return "Invalid";
  }
  return "Invalid";
}

rocksdb::Slice const& arangodb::rocksDBSlice(RocksDBEntryType const& type) {
  switch (type) {
    case RocksDBEntryType::Placeholder:
      return Placeholder;
    case RocksDBEntryType::Database:
      return Database;
    case RocksDBEntryType::Collection:
      return Collection;
    case RocksDBEntryType::CounterValue:
      return CounterValue;
    case RocksDBEntryType::Document:
      return Document;
    case RocksDBEntryType::PrimaryIndexValue:
      return PrimaryIndexValue;
    case RocksDBEntryType::EdgeIndexValue:
      return EdgeIndexValue;
    case RocksDBEntryType::VPackIndexValue:
      return VPackIndexValue;
    case RocksDBEntryType::UniqueVPackIndexValue:
      return UniqueVPackIndexValue;
    case RocksDBEntryType::FulltextIndexValue:
      return FulltextIndexValue;
    case RocksDBEntryType::LegacyGeoIndexValue:
      return LegacyGeoIndexValue;
    case RocksDBEntryType::GeoIndexValue:
      return GeoIndexValue;
    case RocksDBEntryType::View:
      return View;
    case RocksDBEntryType::SettingsValue:
      return SettingsValue;
    case RocksDBEntryType::ReplicationApplierConfig:
      return ReplicationApplierConfig;
    case RocksDBEntryType::IndexEstimateValue:
      return IndexEstimateValue;
    case RocksDBEntryType::KeyGeneratorValue:
      return KeyGeneratorValue;
    case RocksDBEntryType::RevisionTreeValue:
      return RevisionTreeValue;
    case RocksDBEntryType::ZkdIndexValue:
      return ZdkIndexValue;
    case RocksDBEntryType::UniqueZkdIndexValue:
      return UniqueZdkIndexValue;
    case RocksDBEntryType::LogEntry:
      return LogEntry;
    case RocksDBEntryType::ReplicatedLog:
      return ReplicatedLog;
    case RocksDBEntryType::ReplicatedState:
      return ReplicatedState;
  }

  return Placeholder;  // avoids warning - errorslice instead ?!
}
