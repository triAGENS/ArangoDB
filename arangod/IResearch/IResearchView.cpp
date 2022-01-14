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

#include "IResearchCommon.h"
#include "IResearchFeature.h"
#include "IResearchLink.h"
#include "IResearchLinkHelper.h"
#include "VelocyPackHelper.h"

#include "ApplicationFeatures/ApplicationServer.h"
#include "Aql/AstNode.h"
#include "Aql/PlanCache.h"
#include "Aql/QueryCache.h"
#include "Basics/StaticStrings.h"
#include "Basics/StringUtils.h"
#include "Basics/VelocyPackHelper.h"
#include "RestServer/DatabaseFeature.h"
#include "RestServer/ViewTypesFeature.h"
#include "StorageEngine/EngineSelectorFeature.h"
#include "StorageEngine/StorageEngine.h"
#include "StorageEngine/TransactionCollection.h"
#include "StorageEngine/TransactionState.h"
#include "Transaction/Methods.h"
#include "Transaction/StandaloneContext.h"
#include "Utils/Events.h"
#include "Utils/ExecContext.h"

#include "IResearchView.h"

namespace arangodb::iresearch {
namespace {

using kludge::read_write_mutex;

////////////////////////////////////////////////////////////////////////////////
/// @brief index reader implementation over multiple irs::index_reader
///        the container storing the view state for a given TransactionState
/// @note it is assumed that DBServer ViewState resides in the same
///       TransactionState as the IResearchView ViewState, therefore a separate
///       lock is not required to be held by the DBServer CompoundReader
////////////////////////////////////////////////////////////////////////////////
class ViewTrxState final : public TransactionState::Cookie,
                           public IResearchView::Snapshot {
 public:
  irs::sub_reader const& operator[](size_t subReaderId) const noexcept final {
    TRI_ASSERT(subReaderId < _subReaders.size());
    return *(_subReaders[subReaderId].second);
  }

  void add(DataSourceId cid, IResearchLink::Snapshot&& snapshot);

  DataSourceId cid(size_t offset) const noexcept final {
    return offset < _subReaders.size() ? _subReaders[offset].first
                                       : DataSourceId::none();
  }

  void clear() noexcept {
    _collections.clear();
    _subReaders.clear();
    _snapshots.clear();
    _live_docs_count = 0;
    _docs_count = 0;
  }

  bool equalCollections(
      containers::FlatHashSet<DataSourceId> const& collections) {
    return collections == _collections;
  }

  template<typename Collections>
  bool equalCollections(Collections const& collections) {
    size_t const size = _collections.size();
    if (collections.size() != size) {
      return false;
    }  // TODO better iterate for smaller capacity
    for (auto const& cid : _collections) {
      if (!collections.contains(cid)) {
        return false;
      }
    }
    return true;
  }

  [[nodiscard]] uint64_t docs_count() const noexcept final {
    return _docs_count;
  }
  [[nodiscard]] uint64_t live_docs_count() const noexcept final {
    return _live_docs_count;
  }
  [[nodiscard]] size_t size() const noexcept final {
    return _subReaders.size();
  }

 private:
  size_t _docs_count{};
  size_t _live_docs_count{};
  containers::FlatHashSet<DataSourceId> _collections;
  std::vector<IResearchLink::Snapshot> _snapshots;
  // prevent data-store deallocation (lock @ AsyncSelf)
  std::vector<std::pair<DataSourceId, irs::sub_reader const*>> _subReaders;
};

void ViewTrxState::add(DataSourceId cid, IResearchLink::Snapshot&& snapshot) {
  auto& reader = snapshot.getDirectoryReader();
  for (auto& entry : reader) {
    _subReaders.emplace_back(std::piecewise_construct,
                             std::forward_as_tuple(cid),
                             std::forward_as_tuple(&entry));
  }

  _docs_count += reader.docs_count();
  _live_docs_count += reader.live_docs_count();
  _collections.emplace(cid);
  _snapshots.emplace_back(std::move(snapshot));
}

void ensureImmutableProperties(IResearchViewMeta& dst,
                               IResearchViewMeta const& src) {
  dst._version = src._version;
  dst._writebufferActive = src._writebufferActive;
  dst._writebufferIdle = src._writebufferIdle;
  dst._writebufferSizeMax = src._writebufferSizeMax;
  dst._primarySort = src._primarySort;
  dst._storedValues = src._storedValues;
  dst._primarySortCompression = src._primarySortCompression;
}

}  // namespace

////////////////////////////////////////////////////////////////////////////////
/// @brief IResearchView-specific implementation of a ViewFactory
////////////////////////////////////////////////////////////////////////////////
struct IResearchView::ViewFactory : public arangodb::ViewFactory {
  Result create(LogicalView::ptr& view, TRI_vocbase_t& vocbase,
                VPackSlice definition, bool isUserRequest) const override {
    auto& engine =
        vocbase.server().getFeature<EngineSelectorFeature>().engine();
    auto properties = definition.isObject()
                          ? definition
                          : velocypack::Slice::emptyObjectSlice();
    // if no 'info' then assume defaults
    auto links = properties.hasKey(StaticStrings::LinksField)
                     ? properties.get(StaticStrings::LinksField)
                     : velocypack::Slice::emptyObjectSlice();
    auto res = engine.inRecovery()
                   ? Result()  // do not validate if in recovery
                   : IResearchLinkHelper::validateLinks(vocbase, links);

    if (!res.ok()) {
      std::string name;
      if (definition.isObject()) {
        name = basics::VelocyPackHelper::getStringValue(
            definition, arangodb::StaticStrings::DataSourceName, "");
      }
      events::CreateView(vocbase.name(), name, res.errorNumber());
      return res;
    }

    LogicalView::ptr impl;

    res = ServerState::instance()->isSingleServer()
              ? LogicalViewHelperStorageEngine::construct(impl, vocbase,
                                                          definition)
              : LogicalViewHelperClusterInfo::construct(impl, vocbase,
                                                        definition);

    if (!res.ok()) {
      std::string name;
      if (definition.isObject()) {
        name = basics::VelocyPackHelper::getStringValue(
            definition, arangodb::StaticStrings::DataSourceName, "");
      }
      events::CreateView(vocbase.name(), name, res.errorNumber());
      return res;
    }

    if (!impl) {
      std::string name;
      if (definition.isObject()) {
        name = basics::VelocyPackHelper::getStringValue(
            definition, arangodb::StaticStrings::DataSourceName, "");
      }
      events::CreateView(vocbase.name(), name, TRI_ERROR_INTERNAL);
      return {TRI_ERROR_INTERNAL,
              "failure during instantiation while creating arangosearch View "
              "in database '" +
                  vocbase.name() + "'"};
    }

    // create links on a best-effort basis
    // link creation failure does not cause view creation failure
    try {
      std::unordered_set<DataSourceId> collections;  // TODO remove
      res = IResearchLinkHelper::updateLinks(collections, *impl, links,
                                             getDefaultVersion(isUserRequest));
      if (!res.ok()) {
        LOG_TOPIC("d683b", WARN, TOPIC)
            << "failed to create links while creating arangosearch view '"
            << impl->name() << "': " << res.errorNumber() << " "
            << res.errorMessage();
      }
    } catch (basics::Exception const& e) {
      std::string name;
      if (definition.isObject()) {
        name = basics::VelocyPackHelper::getStringValue(
            definition, arangodb::StaticStrings::DataSourceName, "");
      }
      events::CreateView(vocbase.name(), name, e.code());
      LOG_TOPIC("eddb2", WARN, TOPIC)
          << "caught exception while creating links while creating "
             "arangosearch view '"
          << impl->name() << "': " << e.code() << " " << e.what();
    } catch (std::exception const& e) {
      std::string name;
      if (definition.isObject()) {
        name = basics::VelocyPackHelper::getStringValue(
            definition, arangodb::StaticStrings::DataSourceName, "");
      }
      events::CreateView(vocbase.name(), name, TRI_ERROR_INTERNAL);
      LOG_TOPIC("dc829", WARN, TOPIC)
          << "caught exception while creating links while creating "
             "arangosearch view '"
          << impl->name() << "': " << e.what();
    } catch (...) {
      std::string name;
      if (definition.isObject()) {
        name = basics::VelocyPackHelper::getStringValue(
            definition, arangodb::StaticStrings::DataSourceName, "");
      }
      events::CreateView(vocbase.name(), name, TRI_ERROR_INTERNAL);
      LOG_TOPIC("6491c", WARN, TOPIC)
          << "caught exception while creating links while creating "
             "arangosearch view '"
          << impl->name() << "'";
    }
    view = impl;
    return {};
  }

  Result instantiate(LogicalView::ptr& view, TRI_vocbase_t& vocbase,
                     VPackSlice definition) const final {
    std::string error;
    IResearchViewMeta meta;
    IResearchViewMetaState metaState;

    if (!meta.init(definition, error)  // parse definition
        || meta._version > static_cast<uint32_t>(ViewVersion::MAX)
        // ensure version is valid
        || (ServerState::instance()->isSingleServer()
            // init metaState for SingleServer
            && !metaState.init(definition, error))) {
      return {TRI_ERROR_BAD_PARAMETER,
              error.empty()
                  ? (std::string("failed to initialize arangosearch View from "
                                 "definition: ") +
                     definition.toString())
                  : (std::string("failed to initialize arangosearch View from "
                                 "definition, error in attribute '") +
                     error + "': " + definition.toString())};
    }

    auto impl = std::shared_ptr<IResearchView>(
        new IResearchView(vocbase, definition, std::move(meta)));

    // NOTE: for single-server must have full list of collections to lock
    //       for cluster the shards to lock come from coordinator and are not in
    //       the definition
    for (auto cid : metaState._collections) {
      auto collection = vocbase.lookupCollection(cid);
      // always look up in vocbase
      // (single server or cluster per-shard collection)
      auto link =
          collection ? IResearchLinkHelper::find(*collection, *impl) : nullptr;
      // add placeholders to links,
      // when the collection comes up it'll bring up the link
      auto [it, was] = impl->_collections.try_emplace(cid, impl->_links.end());
      if (was && link) {
        auto [it2, was2] = impl->_links.try_emplace(link->id(), link->self());
        TRI_ASSERT(was2);
        it->second = it2;
      }
      // add placeholders to links, when the link comes up it'll call link(...)
    }

    view = impl;

    return {};
  }
};

IResearchView::IResearchView(TRI_vocbase_t& vocbase,
                             velocypack::Slice const& info,
                             IResearchViewMeta&& meta)
    : LogicalView(vocbase, info),
      _asyncSelf(std::make_shared<AsyncViewPtr::element_type>(this)),
      _meta(std::move(meta)),
      _inRecovery(false) {
  // set up in-recovery insertion hooks
  if (vocbase.server().hasFeature<DatabaseFeature>()) {
    auto& databaseFeature = vocbase.server().getFeature<DatabaseFeature>();
    databaseFeature.registerPostRecoveryCallback([view = _asyncSelf] {
      // ensure view does not get deallocated before call back finishes
      auto viewLock = view->lock();
      if (viewLock) {
        viewLock->verifyKnownCollections();
      }
      return Result{};
    });
  }
  auto self = _asyncSelf;
  // initialize transaction read callback
  _trxCallback = [self](transaction::Methods& trx,
                        transaction::Status status) -> void {
    if (transaction::Status::RUNNING != status) {
      return;  // NOOP
    }
    auto viewLock = self->lock();
    // populate snapshot when view is registred with a transaction on
    // single-server
    if (viewLock && ServerState::instance()->isSingleServer()) {
      viewLock->snapshot(trx, IResearchView::SnapshotMode::FindOrCreate);
    }
  };
}

IResearchView::~IResearchView() {
  _asyncSelf->reset();  // the view is being deallocated, its use is no longer
                        // valid (wait for all the view users to finish)
  if (ServerState::instance()->isSingleServer()) {
    LogicalViewHelperStorageEngine::destruct(*this);
    // cleanup of the storage engine
  }
}

Result IResearchView::appendVelocyPackImpl(velocypack::Builder& builder,
                                           Serialization context) const {
  if (Serialization::List == context) {
    return {};  // nothing more to output
  }
  std::function<bool(irs::string_ref)> const propertiesAcceptor{
      [](std::string_view key) {
        return key != StaticStrings::VersionField;  // ignored fields
      }};
  std::function<bool(irs::string_ref)> const persistenceAcceptor{
      [](irs::string_ref) { return true; }};
  auto* acceptor = &propertiesAcceptor;
  if (context == Serialization::Persistence ||
      context == Serialization::PersistenceWithInProgress) {
    acceptor = &persistenceAcceptor;
    if (ServerState::instance()->isSingleServer()) {
      auto res = LogicalViewHelperStorageEngine::properties(builder, *this);
      if (!res.ok()) {
        return res;
      }
    }
  }
  if (!builder.isOpenObject()) {
    return {TRI_ERROR_BAD_PARAMETER};
  }
  std::vector<std::string> collections;
  {
    read_write_mutex::read_mutex mutex{_mutex};
    std::lock_guard lock{mutex};
    velocypack::Builder sanitizedBuilder;
    sanitizedBuilder.openObject();
    IResearchViewMeta::Mask mask(true);
    if (!_meta.json(sanitizedBuilder, nullptr, &mask) ||
        !mergeSliceSkipKeys(builder, sanitizedBuilder.close().slice(),
                            *acceptor)) {
      return {TRI_ERROR_INTERNAL,
              "failure to generate definition while generating properties jSON "
              "for arangosearch View in database '" +
                  vocbase().name() + "'"};
    }
    if (context == Serialization::Persistence ||
        context == Serialization::PersistenceWithInProgress) {
      IResearchViewMetaState metaState;
      for (auto& entry : _collections) {
        metaState._collections.emplace(entry.first);
      }
      metaState.json(builder);
      // nothing more to output (persistent configuration does not need links)
      return {};
    }
    // add CIDs of known collections to list
    for (auto& entry : _collections) {
      // skip collections missing from vocbase or
      // UserTransaction constructor will throw an exception
      if (vocbase().lookupCollection(entry.first)) {
        collections.emplace_back(std::to_string(entry.first.id()));
      }
    }
  }
  // open up a read transaction and add all linked collections to verify that
  // the current user has access
  velocypack::Builder linksBuilder;
  std::vector<std::string> const EMPTY;
  // use default lock timeout
  transaction::Options options;
  options.waitForSync = false;
  options.allowImplicitCollectionsForRead = false;
  Result res;
  try {
    transaction::Methods trx(transaction::StandaloneContext::Create(vocbase()),
                             collections,  // readCollections
                             EMPTY,        // writeCollections
                             EMPTY,        // exclusiveCollections
                             options);
    res = trx.begin();
    if (!res.ok()) {
      return res;  // nothing more to output
    }
    auto* state = trx.state();
    if (!state) {
      return {TRI_ERROR_INTERNAL,
              "failed to get transaction state while generating json for "
              "arangosearch view '" +
                  name() + "'"};
    }
    auto visitor = [this, &linksBuilder, &res,
                    context](TransactionCollection& trxCollection) {
      auto const& collection = trxCollection.collection();
      if (!collection) {
        return true;  // skip missing collections
      }
      auto link = IResearchLinkHelper::find(*collection, *this);
      if (!link) {
        return true;  // no links for the current view
      }
      velocypack::Builder linkBuilder;
      linkBuilder.openObject();
      if (!link->properties(linkBuilder, Serialization::Inventory == context)
               .ok()) {  // link definitions are not output if forPersistence
        LOG_TOPIC("713ad", WARN, TOPIC)
            << "failed to generate json for arangosearch link '" << link->id()
            << "' while generating json for arangosearch view '" << name()
            << "'";
        return true;  // skip invalid link definitions
      }
      linkBuilder.close();

      auto const acceptor = [](std::string_view key) -> bool {
        return key != arangodb::StaticStrings::IndexId &&
               key != arangodb::StaticStrings::IndexType &&
               key != StaticStrings::ViewIdField;
      };
      linksBuilder.add(collection->name(),
                       velocypack::Value(velocypack::ValueType::Object));
      if (!mergeSliceSkipKeys(linksBuilder, linkBuilder.slice(), acceptor)) {
        res = {
            TRI_ERROR_INTERNAL,
            "failed to generate arangosearch link '" +
                std::to_string(link->id().id()) +
                "' definition while generating json for arangosearch view '" +
                name() + "'"};
        return false;  // terminate generation
      }
      linksBuilder.close();
      return true;  // done with this collection
    };
    linksBuilder.openObject();
    state->allCollections(visitor);
    linksBuilder.close();
    if (!res.ok()) {
      return res;
    }
    res = trx.commit();
  } catch (basics::Exception& e) {
    return {e.code(),
            "caught exception while generating json for arangosearch view '" +
                name() + "': " + e.what()};
  } catch (std::exception const& e) {
    return {TRI_ERROR_INTERNAL,
            "caught exception while generating json for arangosearch view '" +
                name() + "': " + e.what()};
  } catch (...) {
    return {TRI_ERROR_INTERNAL,
            "caught exception while generating json for arangosearch view '" +
                name() + "'"};
  }
  builder.add(StaticStrings::LinksField, linksBuilder.slice());
  return res;
}

bool IResearchView::apply(transaction::Methods& trx) {
  // called from IResearchView when this view is added to a transaction
  return trx.addStatusChangeCallback(&_trxCallback);  // add shapshot
}

Result IResearchView::dropImpl() {
  std::unordered_set<DataSourceId> collections;
  std::unordered_set<DataSourceId> stale;

  // drop all known links
  {
    read_write_mutex::read_mutex mutex{_mutex};
    std::lock_guard lock{mutex};
    for (auto& entry : _collections) {
      stale.emplace(entry.first);
    }
  }

  if (!stale.empty()) {
    // check link auth as per https://github.com/arangodb/backlog/issues/459
    if (!ExecContext::current().isSuperuser()) {
      for (auto& entry : stale) {
        auto collection = vocbase().lookupCollection(entry);

        if (collection &&
            !ExecContext::current().canUseCollection(
                vocbase().name(), collection->name(), auth::Level::RO)) {
          return {TRI_ERROR_FORBIDDEN};
        }
      }
    }

    Result res;
    {
      std::unique_lock lock{_updateLinksLock, std::try_to_lock};
      if (!lock.owns_lock()) {
        return {TRI_ERROR_FAILED,  // FIXME use specific error code
                "failed to remove arangosearch view '" + name()};
      }
      res = IResearchLinkHelper::updateLinks(
          collections, *this, velocypack::Slice::emptyObjectSlice(),
          LinkVersion::MAX,
          // we don't care of link version due to removal only request
          stale);
    }
    if (!res.ok()) {
      return {res.errorNumber(),
              "failed to remove links while removing arangosearch view '" +
                  name() + "': " + std::string{res.errorMessage()}};
    }
  }
  _asyncSelf->reset();
  // the view data-stores are being deallocated, view use is no longer valid
  // (wait for all the view users to finish)
  read_write_mutex::write_mutex mutex{_mutex};
  std::lock_guard lock{mutex};
  for (auto& entry : _collections) {
    collections.emplace(entry.first);
  }
  auto collectionsCount = collections.size();
  for (auto& entry : collections) {
    auto collection = vocbase().lookupCollection(entry);
    if (!collection || !IResearchLinkHelper::find(*collection, *this)) {
      --collectionsCount;
    }
  }
  // ArangoDB global consistency check, no known dangling links
  if (collectionsCount) {
    return {TRI_ERROR_INTERNAL,
            "links still present while removing arangosearch view '" +
                std::to_string(id().id()) + "'"};
  }
  return ServerState::instance()->isSingleServer()
             ? LogicalViewHelperStorageEngine::drop(*this)
             // single-server additionally requires removal from the
             // StorageEngine
             : Result();
}

arangodb::ViewFactory const& IResearchView::factory() {
  static const IResearchView::ViewFactory factory;
  return factory;
}

Result IResearchView::link(AsyncLinkPtr const& link) {
  if (!link) {
    return {TRI_ERROR_BAD_PARAMETER,
            "invalid link parameter while emplacing collection into "
            "arangosearch View '" +
                name() + "'"};
  }
  // prevent the link from being deallocated
  auto linkLock = link->lock();
  if (!linkLock) {
    return {TRI_ERROR_BAD_PARAMETER,
            "failed to acquire link while emplacing collection into "
            "arangosearch View '" +
                name() + "'"};
  }
  auto const cid = linkLock->collection().id();
  read_write_mutex::write_mutex mutex{_mutex};
  std::lock_guard lock{mutex};
  auto it = _collections.find(cid);
  if (it == _collections.end()) {
    auto [it_link, was] = _links.try_emplace(linkLock->id(), link);
    TRI_ASSERT(was);
    std::tie(it, was) = _collections.try_emplace(cid, it_link);
    TRI_ASSERT(was);
  } else if (ServerState::instance()->isSingleServer() &&
             it->second == _links.end()) {
    auto [it_link, was] = _links.try_emplace(linkLock->id(), link);
    TRI_ASSERT(was);
    it->second = it_link;
    IResearchLink::properties(std::move(linkLock), _meta);
    // single-server persisted cid placeholder substituted with actual link
    return {};
  } else if (it->second != _links.end() && it->second->second->empty()) {
    it->second->second = link;
    IResearchLink::properties(std::move(linkLock), _meta);
    // a previous link instance was unload()ed and a new instance is linking
    return {};
  } else {
    return {TRI_ERROR_ARANGO_DUPLICATE_IDENTIFIER,
            std::string("duplicate entry while emplacing collection '") +
                std::to_string(cid.id()) + "' into arangosearch View '" +
                name() + "'"};
  }
  Result res;
  if (ServerState::instance()->isSingleServer()) {
    res = LogicalViewHelperStorageEngine::properties(*this);
  }
  if (!res.ok()) {  // undo meta modification
    TRI_ASSERT(it != _collections.end());
    if (it->second != _links.end()) {
      _links.erase(it->second);
    }
    _collections.erase(it);
    return res;
  }
  IResearchLink::properties(std::move(linkLock), _meta);
  return {};
}

Result IResearchView::commitUnsafe() {
  // TODO Maybe we want sync all and skip errors, now we stop on first error?
  if (_collections.size() != _links.size()) {
    TRI_ASSERT(_collections.size() > _links.size());
    return {TRI_ERROR_ARANGO_INDEX_HANDLE_BAD,
            "Some collections without links while syncing arangosearch view '" +
                name() + "'"};
  }
  for (auto& entry : _links) {
    // ensure link is not deallocated for the duration of the operation
    auto linkLock = entry.second->lock();
    if (!linkLock) {
      return {TRI_ERROR_ARANGO_INDEX_HANDLE_BAD,
              "link" + std::to_string(entry.first.id()) +
                  "was removed while syncing arangosearch view '" + name() +
                  "'"};
    }
    auto res = IResearchLink::commit(std::move(linkLock), true);
    if (!res.ok()) {
      return res;
    }
  }
  return {};
}

void IResearchView::open() {
  auto& engine =
      vocbase().server().getFeature<EngineSelectorFeature>().engine();
  _inRecovery = engine.inRecovery();
}

Result IResearchView::properties(velocypack::Slice properties,
                                 bool isUserRequest, bool partialUpdate) {
  auto res = updateProperties(properties, isUserRequest, partialUpdate);
  if (!res.ok()) {
    return res;
  }
#if USE_PLAN_CACHE
  aql::PlanCache::instance()->invalidate(&vocbase());
#endif
  aql::QueryCache::instance()->invalidate(&vocbase());
  return ServerState::instance()->isSingleServer()
             ? LogicalViewHelperStorageEngine::properties(*this)
             : LogicalViewHelperClusterInfo::properties(*this);
}

Result IResearchView::renameImpl(std::string const& oldName) {
  return ServerState::instance()->isSingleServer()
             ? LogicalViewHelperStorageEngine::rename(*this, oldName)
             : LogicalViewHelperClusterInfo::rename(*this, oldName);
}

IResearchView::Snapshot const* IResearchView::snapshot(
    transaction::Methods& trx,
    IResearchView::SnapshotMode mode /*= IResearchView::SnapshotMode::Find*/,
    containers::FlatHashSet<DataSourceId> const* shards /*= nullptr*/,
    void const* key /*= nullptr*/) const {
  if (!trx.state()) {
    LOG_TOPIC("47098", WARN, TOPIC)
        << "failed to get transaction state while creating arangosearch view "
           "snapshot";
    return nullptr;
  }
  if (!key) {
    key = this;
  }
  auto& state = *(trx.state());
// TODO find a better way to look up a ViewState
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  auto* ctx = dynamic_cast<ViewTrxState*>(state.cookie(key));
#else
  auto* ctx = static_cast<ViewTrxState*>(state.cookie(key));
#endif
  read_write_mutex::read_mutex mutex{_mutex};
  std::lock_guard lock{mutex};
  switch (mode) {
      // We want to check ==, but not >= because ctx also is reader
    case SnapshotMode::Find:
      return ctx && (shards ? ctx->equalCollections(*shards)
                            : ctx->equalCollections(_collections))
                 ? ctx
                 : nullptr;  // ensure same collections
    case SnapshotMode::FindOrCreate:
      if (ctx) {
        if ((shards ? ctx->equalCollections(*shards)
                    : ctx->equalCollections(_collections))) {
          return ctx;  // ensure same collections
        }
        ctx->clear();  // reassemble snapshot
      }
      break;
    case SnapshotMode::SyncAndReplace: {
      if (ctx) {
        ctx->clear();  // ignore existing cookie, recreate snapshot
      }
      auto res = const_cast<IResearchView*>(this)->commitUnsafe();
      if (!res.ok()) {
        LOG_TOPIC("fd776", WARN, TOPIC)
            << "failed to sync while creating snapshot for arangosearch view '"
            << name() << "', previous snapshot will be used instead, error: '"
            << res.errorMessage() << "'";
      }
    } break;
    default:              // TODO unreachable
      TRI_ASSERT(false);  // all values of the enum should be covered
  }
  if (!ctx) {
    auto ptr = std::make_unique<ViewTrxState>();
    ctx = ptr.get();
    state.cookie(key, std::move(ptr));
  }
  try {
    auto cantFind = [&] {
      LOG_TOPIC("d63ff", ERR, TOPIC)
          << "Some link was removed in arangosearch view '" << name()
          << "', skipping it";
      state.cookie(key, nullptr);  // unset cookie
      return nullptr;              // skip missing links
    };
    // collect snapshots from all requested links
    auto shouldFind = shards ? shards->size() : _collections.size();
    auto canFind = _links.size();
    TRI_ASSERT(shards || shouldFind >= canFind);
    if (shouldFind == 0) {
      return ctx;
    }
    for (auto& entry : _links) {
      if (shouldFind > canFind) {
        return cantFind();
      }
      --canFind;
      auto linkLock = entry.second->lock();
      if (!linkLock) {
        continue;
      }
      auto cid = linkLock->collection().id();
      if (shards && !shards->contains(cid)) {
        continue;
      }
      TRI_ASSERT(_collections.contains(cid));
      auto snapshot = IResearchLink::snapshot(std::move(linkLock));
      if (!snapshot.getDirectoryReader()) {
        return cantFind();
      }
      ctx->add(cid, std::move(snapshot));
      if (--shouldFind == 0) {
        return ctx;
      }
    }
    return cantFind();
  } catch (basics::Exception& e) {
    LOG_TOPIC("29b30", WARN, TOPIC)
        << "caught exception while collecting readers for snapshot of "
           "arangosearch view '"
        << name() << "', tid '" << state.id() << "': " << e.code() << " "
        << e.what();
  } catch (std::exception const& e) {
    LOG_TOPIC("ffe73", WARN, TOPIC)
        << "caught exception while collecting readers for snapshot of "
           "arangosearch view '"
        << name() << "', tid '" << state.id() << "': " << e.what();
  } catch (...) {
    LOG_TOPIC("c54e8", WARN, TOPIC)
        << "caught exception while collecting readers for snapshot of "
           "arangosearch view '"
        << name() << "', tid '" << state.id() << "'";
  }
  return nullptr;
}

Result IResearchView::unlink(DataSourceId cid) noexcept {
  try {
    read_write_mutex::write_mutex mutex{_mutex};
    std::lock_guard lock{mutex};
    auto it = _collections.find(cid);
    if (it == _collections.end()) {
      return {};  // already unlinked
    }
    Links::node_type link;
    if (it->second != _links.end()) {
      link = _links.extract(it->second);  // invalidate it->second, noexcept
      it->second = _links.end();          // noexcept
    }
    auto res = ServerState::instance()->isSingleServer()
                   ? LogicalViewHelperStorageEngine::properties(*this)
                   : Result{};  // noexcept
    if (!res.ok()) {
      LOG_TOPIC("9d678", WARN, TOPIC)
          << "failed to persist logical view while unlinking collection '"
          << cid << "' from arangosearch view '" << name()
          << "': " << res.errorMessage();  // noexcept
      if (link) {
        auto r = _links.insert(move(link));  // noexcept
        TRI_ASSERT(r.inserted);
        it->second = r.position;  // noexcept
      }
      return res;
    }
    _collections.erase(it);  // if exception here we don't need to restore link,
    // because _collections anyway stay in the valid state:
    // without it or with it and it->second == _links.end()
  } catch (basics::Exception const& e) {
    return {e.code(), "caught exception while unlinking collection '" +
                          std::to_string(cid.id()) +
                          "' from arangosearch view '" + name() +
                          "': " + e.what()};
  } catch (std::exception const& e) {
    return {TRI_ERROR_INTERNAL,
            "caught exception while unlinking collection '" +
                std::to_string(cid.id()) + "' from arangosearch view '" +
                name() + "': " + e.what()};
  } catch (...) {
    return {TRI_ERROR_INTERNAL,
            "caught exception while unlinking collection '" +
                std::to_string(cid.id()) + "' from arangosearch view '" +
                name() + "'"};
  }
  return {};
}

Result IResearchView::updateProperties(velocypack::Slice slice,
                                       bool isUserRequest, bool partialUpdate) {
  try {
    auto links = slice.hasKey(StaticStrings::LinksField)
                     ? slice.get(StaticStrings::LinksField)
                     : velocypack::Slice::emptyObjectSlice();
    auto res = _inRecovery
                   ? Result()  // do not validate if in recovery
                   : IResearchLinkHelper::validateLinks(vocbase(), links);
    if (!res.ok()) {
      return res;
    }
    read_write_mutex::write_mutex mutex{_mutex};
    std::unique_lock mtx{mutex};
    // check link auth as per https://github.com/arangodb/backlog/issues/459
    if (!ExecContext::current().isSuperuser()) {
      // check existing _collections
      for (auto& entry : _collections) {
        auto collection = vocbase().lookupCollection(entry.first);
        if (collection &&
            !ExecContext::current().canUseCollection(
                vocbase().name(), collection->name(), auth::Level::RO)) {
          return {
              TRI_ERROR_FORBIDDEN,
              "while updating arangosearch definition, error: collection '" +
                  collection->name() + "' not authorized for read access"};
        }
      }
    }
    std::string error;
    IResearchViewMeta meta;
    auto& initialMeta = partialUpdate ? _meta : IResearchViewMeta::DEFAULT();
    if (!meta.init(slice, error, initialMeta)) {
      return {TRI_ERROR_BAD_PARAMETER,
              "failed to update arangosearch view '" + name() +
                  "' from definition" +
                  (error.empty() ? ": "
                                 : (", error in attribute '" + error + "': ")) +
                  slice.toString()};
    }
    // reset non-updatable values to match current meta
    ensureImmutableProperties(meta, _meta);
    _meta = std::move(meta);
    mutex.unlock(true);  // downgrade to a read-lock
    // update properties of links
    for (auto& entry : _links) {
      // prevent the link from being deallocated
      auto linkLock = entry.second->lock();
      if (linkLock) {
        IResearchLink::properties(std::move(linkLock), _meta);
      }
    }
    if (links.isEmptyObject() && (partialUpdate || _inRecovery.load())) {
      // ignore missing links coming from WAL (inRecovery)
      return res;
    }
    // ...........................................................................
    // update links if requested (on a best-effort basis)
    // indexing of collections is done in different threads
    // so no locks can be held and rollback is not possible as a result
    // it's also possible for links to be simultaneously modified via a
    // different callflow (e.g. from collections)
    // ...........................................................................
    std::unordered_set<DataSourceId> collections;
    std::unordered_set<DataSourceId> stale;
    if (!partialUpdate) {
      for (auto& entry : _collections) {
        stale.emplace(entry.first);
      }
    }
    mtx.unlock();
    std::lock_guard lock{_updateLinksLock};
    return IResearchLinkHelper::updateLinks(
        collections, *this, links, getDefaultVersion(isUserRequest), stale);
  } catch (basics::Exception& e) {
    LOG_TOPIC("74705", WARN, TOPIC)
        << "caught exception while updating properties for arangosearch view '"
        << name() << "': " << e.code() << " " << e.what();
    return {e.code(),
            "error updating properties for arangosearch view '" + name() + "'"};
  } catch (std::exception const& e) {
    LOG_TOPIC("27f54", WARN, TOPIC)
        << "caught exception while updating properties for arangosearch view '"
        << name() << "': " << e.what();

    return {TRI_ERROR_BAD_PARAMETER,
            "error updating properties for arangosearch view '" + name() + "'"};
  } catch (...) {
    LOG_TOPIC("99bbe", WARN, TOPIC)
        << "caught exception while updating properties for arangosearch view '"
        << name() << "'";
    return {TRI_ERROR_BAD_PARAMETER,
            "error updating properties for arangosearch view '" + name() + "'"};
  }
}

bool IResearchView::visitCollections(
    LogicalView::CollectionVisitor const& visitor) const {
  read_write_mutex::read_mutex mutex{_mutex};
  std::lock_guard lock{mutex};
  for (auto& entry : _collections) {
    if (!visitor(entry.first)) {
      return false;
    }
  }
  return true;
}

void IResearchView::verifyKnownCollections() {
  bool modified = false;
  read_write_mutex::write_mutex mutex{_mutex};
  std::lock_guard lock{mutex};
  for (auto it = _collections.begin(), end = _collections.end(); it != end;) {
    auto remove = [&] {
      if (it->second != _links.end()) {
        _links.erase(it->second);
      }
      auto erase_it = it++;
      _collections.erase(erase_it);
      modified = true;
    };
    auto cid = it->first;
    auto collection = vocbase().lookupCollection(cid);
    // always look up in vocbase (single server or cluster per-shard collection)
    if (!collection) {
      LOG_TOPIC("40976", TRACE, TOPIC)
          << "collection '" << cid
          << "' no longer exists! removing from arangosearch view '" << name()
          << "'";
      remove();
      continue;
    }
    auto link = IResearchLinkHelper::find(*collection, *this);
    if (!link) {
      LOG_TOPIC("d0509", TRACE, TOPIC)
          << "collection '" << collection->name()
          << "' no longer linked! removing from arangosearch view '" << name()
          << "'";
      remove();
      continue;
    }
    // all links must be valid even on single-server
    TRI_ASSERT(it->second != _links.end());
    ++it;
  }

  if (modified && ServerState::instance()->isSingleServer()) {
    LogicalViewHelperStorageEngine::properties(*this);
  }
}

}  // namespace arangodb::iresearch
