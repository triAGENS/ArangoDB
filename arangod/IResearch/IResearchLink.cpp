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
/// @author Andrey Abramov
/// @author Vasiliy Nabatchikov
////////////////////////////////////////////////////////////////////////////////

#include <index/column_info.hpp>
#include <store/mmap_directory.hpp>
#include <utils/encryption.hpp>
#include <utils/file_utils.hpp>
#include <utils/singleton.hpp>

#include "IResearchDocument.h"
#include "IResearchLink.h"

#include "ApplicationFeatures/ApplicationServer.h"
#include "Aql/QueryCache.h"
#include "Basics/StaticStrings.h"
#include "Cluster/ClusterFeature.h"
#include "Cluster/ClusterInfo.h"
#ifdef USE_ENTERPRISE
#include "Cluster/ClusterMethods.h"
#endif
#include "IResearch/IResearchCommon.h"
#include "IResearch/IResearchCompression.h"
#include "IResearch/IResearchFeature.h"
#include "IResearch/IResearchLinkHelper.h"
#include "IResearch/IResearchPrimaryKeyFilter.h"
#include "IResearch/IResearchView.h"
#include "IResearch/IResearchViewCoordinator.h"
#include "IResearch/VelocyPackHelper.h"
#include "Metrics/BatchBuilder.h"
#include "Metrics/GaugeBuilder.h"
#include "Metrics/MetricsFeature.h"
#include "RestServer/DatabaseFeature.h"
#include "RestServer/DatabasePathFeature.h"
#include "RestServer/FlushFeature.h"
#include "StorageEngine/EngineSelectorFeature.h"
#include "StorageEngine/StorageEngine.h"
#include "StorageEngine/TransactionState.h"
#include "Transaction/Helpers.h"
#include "Transaction/Methods.h"
#include "VocBase/LogicalCollection.h"

using namespace std::literals;

namespace arangodb::iresearch {
namespace {

DECLARE_GAUGE(arangodb_search_num_docs, uint64_t, "Number of documents");
DECLARE_GAUGE(arangodb_search_num_live_docs, uint64_t,
              "Number of live documents");
DECLARE_GAUGE(arangodb_search_num_segments, uint64_t, "Number of segments");
DECLARE_GAUGE(arangodb_search_num_files, uint64_t, "Number of files");
DECLARE_GAUGE(arangodb_search_index_size, uint64_t,
              "Size of the index in bytes");
DECLARE_GAUGE(arangodb_search_num_failed_commits, uint64_t,
              "Number of failed commits");
DECLARE_GAUGE(arangodb_search_num_failed_cleanups, uint64_t,
              "Number of failed cleanups");
DECLARE_GAUGE(arangodb_search_num_failed_consolidations, uint64_t,
              "Number of failed consolidations");
DECLARE_GAUGE(arangodb_search_commit_time, uint64_t,
              "Average time of few last commits");
DECLARE_GAUGE(arangodb_search_cleanup_time, uint64_t,
              "Average time of few last cleanups");
DECLARE_GAUGE(arangodb_search_consolidation_time, uint64_t,
              "Average time of few last consolidations");

// Ensures that all referenced analyzer features are consistent.
[[maybe_unused]] void checkAnalyzerFeatures(IResearchLinkMeta const& meta) {
  auto assertAnalyzerFeatures =
      [version = LinkVersion{meta._version}](auto const& analyzers) {
        for (auto& analyzer : analyzers) {
          irs::type_info::type_id invalidNorm;
          if (version < LinkVersion::MAX) {
            invalidNorm = irs::type<irs::Norm2>::id();
          } else {
            invalidNorm = irs::type<irs::Norm>::id();
          }

          const auto features = analyzer->fieldFeatures();

          TRI_ASSERT(std::end(features) == std::find(std::begin(features),
                                                     std::end(features),
                                                     invalidNorm));
        }
      };

  auto checkFieldFeatures = [&assertAnalyzerFeatures](auto const& fieldMeta,
                                                      auto&& self) -> void {
    assertAnalyzerFeatures(fieldMeta._analyzers);
    for (auto const& entry : fieldMeta._fields) {
      self(*entry.value(), self);
    }
  };
  assertAnalyzerFeatures(meta._analyzerDefinitions);
  checkFieldFeatures(meta, checkFieldFeatures);
}

constexpr std::string_view arangosearch_link_stats_name =
    "arangosearch_link_stats";

template<typename T>
T getMetric(const IResearchLink& link) {
  T metric;
  metric.addLabel("view", link.getViewId());
  metric.addLabel("collection", link.getCollectionName());
  metric.addLabel("shard", link.getShardName());
  metric.addLabel("db", link.getDbName());
  return metric;
}

void initCollectionName(LogicalCollection const& collection, ClusterInfo* ci,
                        IResearchLinkMeta& meta, uint64_t linkId) {
  // Upgrade step for old link definition without collection name
  // could be received from agency while shard of the collection was moved
  // or added to the server. New links already has collection name set,
  // but here we must get this name on our own.
  auto& name = meta._collectionName;
  if (name.empty()) {
    name = ci ? ci->getCollectionNameForShard(collection.name())
              : collection.name();
    LOG_TOPIC("86ece", TRACE, TOPIC) << "Setting collection name '" << name
                                     << "' for new link '" << linkId << "'";
    if (ADB_UNLIKELY(name.empty())) {
      LOG_TOPIC_IF("67da6", WARN, TOPIC, meta.willIndexIdAttribute())
          << "Failed to init collection name for the link '" << linkId
          << "'. Link will not index '_id' attribute."
             "Please recreate the link if this is necessary!";
    }
#ifdef USE_ENTERPRISE
    // enterprise name is not used in _id so should not be here!
    if (ADB_LIKELY(!name.empty())) {
      ClusterMethods::realNameFromSmartName(name);
    }
#endif
  }
}

Result linkWideCluster(LogicalCollection const& logical, IResearchView* view) {
  if (!view) {
    return {};
  }
  auto shardIds = logical.shardIds();
  // go through all shard IDs of the collection and
  // try to link any links missing links will be populated
  // when they are created in the per-shard collection
  if (!shardIds) {
    return {};
  }
  for (auto& entry : *shardIds) {  // per-shard collection is always in vocbase
    auto collection = logical.vocbase().lookupCollection(entry.first);
    if (!collection) {
      // missing collection should be created after Plan becomes Current
      continue;
    }
    if (auto link = IResearchLinkHelper::find(*collection, *view); link) {
      if (auto r = view->link(link->self()); !r.ok()) {
        return r;
      }
    }
  }
  return {};
}

}  // namespace

template<typename T>
Result IResearchLink::getView(LogicalView* logical, T*& view) {
  if (!logical) {
    return {};
  }
  if (logical->type() != ViewType::kSearch) {
    return {TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND,
            "error finding view: '" + _viewGuid + "' for link '" +
                std::to_string(_id.id()) + "' : no such view"};
  }
  view = LogicalView::cast<T>(logical);
  if (!view) {  // TODO(MBkkt) Should be assert?
    return {TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND,
            "error finding view: '" + _viewGuid + "' for link '" +
                std::to_string(_id.id()) + "'"};
  }
  // TODO(MBkkt) Now its workaround for unit tests that expected this behavior
  _viewGuid = view->guid();
  // TRI_ASSERT(_viewGuid == view->guid());
  return {};
}

Result IResearchLink::initAndLink(InitCallback const& init,
                                  IResearchView* view) {
  auto r = initDataStore(init, _meta._version, !_meta._sort.empty(),
                         _meta._storedValues.columns(), _meta._sortCompression);
  if (r.ok() && view) {
    r = view->link(_asyncSelf);
    // TODO(MBkkt) Should we remove directory if we create it?
  }
  return r;
}

Result IResearchLink::initSingleServer(InitCallback const& init) {
  auto logical = _collection.vocbase().lookupView(_viewGuid);
  IResearchView* view = nullptr;
  if (auto r = getView(logical.get(), view); !r.ok()) {
    return r;
  }
  return initAndLink(init, view);
}

Result IResearchLink::initCoordinator(InitCallback const& init) {
  auto& vocbase = _collection.vocbase();
  auto& ci = vocbase.server().getFeature<ClusterFeature>().clusterInfo();
  auto logical = ci.getView(vocbase.name(), _viewGuid);
  IResearchViewCoordinator* view = nullptr;
  if (auto r = getView(logical.get(), view); !view) {
    return r;
  }
  return view->link(*this);
}

Result IResearchLink::initDBServer(InitCallback const& init) {
  auto& vocbase = _collection.vocbase();
  auto& server = vocbase.server();
  bool const clusterEnabled = server.getFeature<ClusterFeature>().isEnabled();
  bool wide = _collection.id() == _collection.planId() && _collection.isAStub();
  std::shared_ptr<LogicalView> logical;
  IResearchView* view = nullptr;
  if (clusterEnabled) {
    auto& ci = server.getFeature<ClusterFeature>().clusterInfo();
    initCollectionName(_collection, wide ? nullptr : &ci, _meta, id().id());
    logical = ci.getView(vocbase.name(), _viewGuid);
    if (auto r = getView(logical.get(), view); !r.ok()) {
      return r;
    }
  } else {
    LOG_TOPIC("67dd6", DEBUG, TOPIC)
        << "Skipped link '" << id().id()
        << "' maybe due to disabled cluster features.";
  }
  if (wide) {
    return linkWideCluster(_collection, view);
  }
  if (_meta._collectionName.empty() && !clusterEnabled &&
      server.getFeature<EngineSelectorFeature>().engine().inRecovery() &&
      _meta.willIndexIdAttribute()) {
    LOG_TOPIC("f25ce", FATAL, TOPIC)
        << "Upgrade conflicts with recovering ArangoSearch link '" << id().id()
        << "' Please rollback the updated arangodb binary and"
           " finish the recovery first.";
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_INTERNAL,
        "Upgrade conflicts with recovering ArangoSearch link."
        " Please rollback the updated arangodb binary and"
        " finish the recovery first.");
  }
  return initAndLink(init, view);
}

IResearchLink::IResearchLink(IndexId iid, LogicalCollection& collection)
    : IResearchDataStore(iid, collection), _linkStats{nullptr} {}

IResearchLink::~IResearchLink() {
  Result res;
  try {
    res = unload();  // disassociate from view if it has not been done yet
  } catch (...) {
  }

  if (!res.ok()) {
    LOG_TOPIC("2b41f", ERR, TOPIC)
        << "failed to unload arangodb_search link in link destructor: "
        << res.errorNumber() << " " << res.errorMessage();
  }
}

bool IResearchLink::operator==(LogicalView const& view) const noexcept {
  return _viewGuid == view.guid();
}

bool IResearchLink::operator==(IResearchLinkMeta const& meta) const noexcept {
  return _meta == meta;
}

Result IResearchLink::drop() {
  // the lookup and unlink is valid for single-server only (that is the only
  // scenario where links are persisted) on coordinator and db-server the
  // IResearchView is immutable and lives in ClusterInfo therefore on
  // coordinator and db-server a new plan will already have an IResearchView
  // without the link this avoids deadlocks with ClusterInfo::loadPlan() during
  // lookup in ClusterInfo
  if (ServerState::instance()->isSingleServer()) {
    auto logicalView = collection().vocbase().lookupView(_viewGuid);
    auto* view = LogicalView::cast<IResearchView>(logicalView.get());

    // may occur if the link was already unlinked from the view via another
    // instance this behavior was seen
    // user-access-right-drop-view-arangosearch-spec.js where the collection
    // drop was called through REST, the link was dropped as a result of the
    // collection drop call then the view was dropped via a separate REST call
    // then the vocbase was destroyed calling
    // collection close()-> link unload() -> link drop() due to collection
    // marked as dropped thus returning an error here will cause
    // ~TRI_vocbase_t() on RocksDB to receive an exception which is not handled
    // in the destructor the reverse happens during drop of a collection with
    // MMFiles i.e. collection drop() -> collection close()-> link unload(),
    // then link drop()
    if (!view) {
      LOG_TOPIC("f4e2c", WARN, iresearch::TOPIC)
          << "unable to find arangosearch view '" << _viewGuid
          << "' while dropping arangosearch link '" << _id.id() << "'";
    } else {
      view->unlink(
          collection()
              .id());  // unlink before reset() to release lock in view (if any)
    }
  }

  return deleteDataStore();
}

Result IResearchLink::init(velocypack::Slice definition,
                           InitCallback const& init) {
  auto& vocbase = _collection.vocbase();
  auto& server = vocbase.server();
  bool const isSingleServer = ServerState::instance()->isSingleServer();
  if (!isSingleServer && !server.hasFeature<ClusterFeature>()) {
    return {
        TRI_ERROR_INTERNAL,
        "failure to get cluster info while initializing arangosearch link '" +
            std::to_string(_id.id()) + "'"};
  }
  std::string error;
  // definition should already be normalized and analyzers created if required
  if (!_meta.init(server, definition, error, vocbase.name())) {
    return {TRI_ERROR_BAD_PARAMETER,
            "error parsing view link parameters from json: " + error};
  }
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  checkAnalyzerFeatures(_meta);
#endif
  if (!definition.isObject() ||
      !definition.get(StaticStrings::ViewIdField).isString()) {
    return {TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND,
            "error finding view for link '" + std::to_string(_id.id()) + "'"};
  }
  TRI_ASSERT(_meta._sortCompression);
  _viewGuid = definition.get(StaticStrings::ViewIdField).stringView();
  Result r;
  if (isSingleServer) {
    r = initSingleServer(init);
  } else if (ServerState::instance()->isCoordinator()) {
    r = initCoordinator(init);
  } else if (ServerState::instance()->isDBServer()) {
    r = initDBServer(init);
  } else {
    TRI_ASSERT(false);
    return r;
  }
  if (r.ok()) {  // TODO(MBkkt) Do we really need this check?
    _comparer.reset(_meta._sort);
  }
  return r;
}

Result IResearchLink::insert(transaction::Methods& trx,
                             LocalDocumentId const documentId,
                             velocypack::Slice const doc) {
  return IResearchDataStore::insert<FieldIterator, IResearchLinkMeta>(
      trx, documentId, doc, _meta);
}

bool IResearchLink::isHidden() {
  // hide links unless we are on a DBServer
  return !ServerState::instance()->isDBServer();
}

bool IResearchLink::isSorted() {
  return false;  // IResearch does not provide a fixed default sort order
}

void IResearchLink::load() {
  // Note: this function is only used by RocksDB
}

bool IResearchLink::matchesDefinition(velocypack::Slice slice) const {
  if (!slice.isObject() || !slice.hasKey(StaticStrings::ViewIdField)) {
    return false;  // slice has no view identifier field
  }
  auto viewId = slice.get(StaticStrings::ViewIdField);
  // NOTE: below will not match if 'viewId' is 'id' or 'name',
  //       but ViewIdField should always contain GUID
  if (!viewId.isString() || !viewId.isEqualString(_viewGuid)) {
    // IResearch View identifiers of current object and slice do not match
    return false;
  }
  IResearchLinkMeta other;
  std::string errorField;
  // for db-server analyzer validation should have already passed on coordinator
  // (missing analyzer == no match)
  auto& vocbase = _collection.vocbase();
  return other.init(vocbase.server(), slice, errorField, vocbase.name()) &&
         _meta == other;
}

Result IResearchLink::properties(velocypack::Builder& builder,
                                 bool forPersistence) const {
  if (!builder.isOpenObject()  // not an open object
      || !_meta.json(_collection.vocbase().server(), builder, forPersistence,
                     nullptr, &(_collection.vocbase()))) {
    return {TRI_ERROR_BAD_PARAMETER};
  }

  builder.add(arangodb::StaticStrings::IndexId,
              velocypack::Value(std::to_string(_id.id())));
  builder.add(
      arangodb::StaticStrings::IndexType,
      velocypack::Value(arangodb::iresearch::StaticStrings::DataSourceType));
  builder.add(StaticStrings::ViewIdField, velocypack::Value(_viewGuid));

  return {};
}

Index::IndexType IResearchLink::type() {
  // TODO: don't use enum
  return Index::TRI_IDX_TYPE_IRESEARCH_LINK;
}

char const* IResearchLink::typeName() {
  return StaticStrings::DataSourceType.data();
}

bool IResearchLink::setCollectionName(irs::string_ref name) noexcept {
  TRI_ASSERT(!name.empty());
  if (_meta._collectionName.empty()) {
    _meta._collectionName = name;
    return true;
  }
  LOG_TOPIC_IF("5573c", ERR, TOPIC, name != _meta._collectionName)
      << "Collection name mismatch for arangosearch link '" << id() << "'."
      << " Meta name '" << _meta._collectionName << "' setting name '" << name
      << "'";
  TRI_ASSERT(name == _meta._collectionName);
  return false;
}

Result IResearchLink::unload() {
  // this code is used by the MMFilesEngine
  // if the collection is in the process of being removed then drop it from the
  // view
  // FIXME TODO remove once LogicalCollection::drop(...) will drop its indexes
  // explicitly
  if (_collection.deleted()  // collection deleted
      || TRI_vocbase_col_status_e::TRI_VOC_COL_STATUS_DELETED ==
             _collection.status()) {
    return drop();
  }

  return shutdownDataStore();
}

////////////////////////////////////////////////////////////////////////////////
/// @brief lookup referenced analyzer
////////////////////////////////////////////////////////////////////////////////
AnalyzerPool::ptr IResearchLink::findAnalyzer(
    AnalyzerPool const& analyzer) const {
  auto const it =
      _meta._analyzerDefinitions.find(irs::string_ref(analyzer.name()));

  if (it == _meta._analyzerDefinitions.end()) {
    return nullptr;
  }

  auto pool = *it;

  if (pool && analyzer == *pool) {
    return pool;
  }

  return nullptr;
}

std::string_view IResearchLink::format() const noexcept {
  return getFormat(LinkVersion{_meta._version});
}

IResearchViewStoredValues const& IResearchLink::storedValues() const noexcept {
  return _meta._storedValues;
}

std::string const& IResearchLink::getViewId() const noexcept {
  return _viewGuid;
}

std::string const& IResearchLink::getDbName() const {
  return _collection.vocbase().name();
}

std::string const& IResearchLink::getShardName() const noexcept {
  if (ServerState::instance()->isDBServer()) {
    return _collection.name();
  }
  return arangodb::StaticStrings::Empty;
}

std::string IResearchLink::getCollectionName() const {
  if (ServerState::instance()->isDBServer()) {
    return _meta._collectionName;
  }
  if (ServerState::instance()->isSingleServer()) {
    return std::to_string(_collection.id().id());
  }
  TRI_ASSERT(false);
  return {};
}

IResearchLink::LinkStats IResearchLink::stats() const {
  return LinkStats(statsSynced());
}

void IResearchLink::updateStats(IResearchDataStore::Stats const& stats) {
  _linkStats->store(IResearchLink::LinkStats(stats));
}

void IResearchLink::insertStats() {
  auto& metric =
      _collection.vocbase().server().getFeature<metrics::MetricsFeature>();
  auto builder = getMetric<metrics::BatchBuilder<LinkStats>>(*this);
  builder.setName(arangosearch_link_stats_name);
  _linkStats = &metric.add(std::move(builder));
  _numFailedCommits =
      &metric.add(getMetric<arangodb_search_num_failed_commits>(*this));
  _numFailedCleanups =
      &metric.add(getMetric<arangodb_search_num_failed_cleanups>(*this));
  _numFailedConsolidations =
      &metric.add(getMetric<arangodb_search_num_failed_consolidations>(*this));
  _avgCommitTimeMs = &metric.add(getMetric<arangodb_search_commit_time>(*this));
  _avgCleanupTimeMs =
      &metric.add(getMetric<arangodb_search_cleanup_time>(*this));
  _avgConsolidationTimeMs =
      &metric.add(getMetric<arangodb_search_consolidation_time>(*this));
}

void IResearchLink::removeStats() {
  auto& metricFeature =
      _collection.vocbase().server().getFeature<metrics::MetricsFeature>();
  if (_linkStats) {
    _linkStats = nullptr;
    auto builder = getMetric<metrics::BatchBuilder<LinkStats>>(*this);
    builder.setName(arangosearch_link_stats_name);
    metricFeature.remove(std::move(builder));
  }
  if (_numFailedCommits) {
    _numFailedCommits = nullptr;
    metricFeature.remove(getMetric<arangodb_search_num_failed_commits>(*this));
  }
  if (_numFailedCleanups) {
    _numFailedCleanups = nullptr;
    metricFeature.remove(getMetric<arangodb_search_num_failed_cleanups>(*this));
  }
  if (_numFailedConsolidations) {
    _numFailedConsolidations = nullptr;
    metricFeature.remove(
        getMetric<arangodb_search_num_failed_consolidations>(*this));
  }
  if (_avgCommitTimeMs) {
    _avgCommitTimeMs = nullptr;
    metricFeature.remove(getMetric<arangodb_search_commit_time>(*this));
  }
  if (_avgCleanupTimeMs) {
    _avgCleanupTimeMs = nullptr;
    metricFeature.remove(getMetric<arangodb_search_cleanup_time>(*this));
  }
  if (_avgConsolidationTimeMs) {
    _avgConsolidationTimeMs = nullptr;
    metricFeature.remove(getMetric<arangodb_search_consolidation_time>(*this));
  }
}

void IResearchLink::invalidateQueryCache(TRI_vocbase_t* vocbase) {
  aql::QueryCache::instance()->invalidate(vocbase, _viewGuid);
}

void IResearchLink::LinkStats::toPrometheus(std::string& result, bool first,
                                            std::string_view globals,
                                            std::string_view labels) const {
  auto writeAnnotation = [&] {
    (result += '{') += globals;
    if (!labels.empty()) {
      if (!globals.empty()) {
        result += ',';
      }
      result += labels;
    }
    result += '}';
  };
  auto writeMetric = [&](std::string_view name, std::string_view help,
                         size_t value) {
    if (first) {
      (result.append("# HELP ").append(name) += ' ').append(help) += '\n';
      result.append("# TYPE ").append(name) += " gauge\n";
    }
    result.append(name);
    writeAnnotation();
    result.append(std::to_string(value)) += '\n';
  };
  writeMetric(arangodb_search_num_docs::kName, "Number of documents", numDocs);
  writeMetric(arangodb_search_num_live_docs::kName, "Number of live documents",
              numLiveDocs);
  writeMetric(arangodb_search_num_segments::kName, "Number of segments",
              numSegments);
  writeMetric(arangodb_search_num_files::kName, "Number of files", numFiles);
  writeMetric(arangodb_search_index_size::kName, "Size of the index in bytes",
              indexSize);
}

}  // namespace arangodb::iresearch
