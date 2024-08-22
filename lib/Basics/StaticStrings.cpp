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
/// @author Jan Steemann
////////////////////////////////////////////////////////////////////////////////

#include "StaticStrings.h"

using namespace arangodb;

// constants
std::string const StaticStrings::Base64("base64");
std::string const StaticStrings::Binary("binary");
std::string const StaticStrings::Empty("");
std::string const StaticStrings::N1800("1800");

// index lookup strings
std::string const StaticStrings::IndexEq("eq");
std::string const StaticStrings::IndexIn("in");
std::string const StaticStrings::IndexLe("le");
std::string const StaticStrings::IndexLt("lt");
std::string const StaticStrings::IndexGe("ge");
std::string const StaticStrings::IndexGt("gt");

// system attribute names
std::string const StaticStrings::AttachmentString("_attachment");
std::string const StaticStrings::IdString("_id");
std::string const StaticStrings::KeyString("_key");
std::string const StaticStrings::PrefixOfKeyString("_key:");
std::string const StaticStrings::PostfixOfKeyString(":_key");
std::string const StaticStrings::RevString("_rev");
std::string const StaticStrings::FromString("_from");
std::string const StaticStrings::ToString("_to");

// URL parameter names
std::string const StaticStrings::IgnoreRevsString("ignoreRevs");
std::string const StaticStrings::IsRestoreString("isRestore");
std::string const StaticStrings::KeepNullString("keepNull");
std::string const StaticStrings::MergeObjectsString("mergeObjects");
std::string const StaticStrings::ReturnNewString("returnNew");
std::string const StaticStrings::ReturnOldString("returnOld");
std::string const StaticStrings::SilentString("silent");
std::string const StaticStrings::WaitForSyncString("waitForSync");
std::string const StaticStrings::SkipDocumentValidation(
    "skipDocumentValidation");
std::string const StaticStrings::OverwriteCollectionPrefix(
    "overwriteCollectionPrefix");
std::string const StaticStrings::IsSynchronousReplicationString(
    "isSynchronousReplication");
std::string const StaticStrings::RefillIndexCachesString("refillIndexCaches");
std::string const StaticStrings::VersionAttributeString("versionAttribute");
std::string const StaticStrings::Group("group");
std::string const StaticStrings::Namespace("namespace");
std::string const StaticStrings::Prefix("prefix");
std::string const StaticStrings::Overwrite("overwrite");
std::string const StaticStrings::OverwriteMode("overwriteMode");
std::string const StaticStrings::Compact("compact");
std::string const StaticStrings::DontWaitForCommit("dontWaitForCommit");
std::string const StaticStrings::UserString("user");

// dump headers
std::string const StaticStrings::DumpAuthUser("x-arango-dump-auth-user");
std::string const StaticStrings::DumpBlockCounts("x-arango-dump-block-counts");
std::string const StaticStrings::DumpId("x-arango-dump-id");
std::string const StaticStrings::DumpShardId("x-arango-dump-shard-id");

// replication headers
std::string const StaticStrings::ReplicationHeaderCheckMore(
    "x-arango-replication-checkmore");
std::string const StaticStrings::ReplicationHeaderLastIncluded(
    "x-arango-replication-lastincluded");
std::string const StaticStrings::ReplicationHeaderLastScanned(
    "x-arango-replication-lastscanned");
std::string const StaticStrings::ReplicationHeaderLastTick(
    "x-arango-replication-lasttick");
std::string const StaticStrings::ReplicationHeaderFromPresent(
    "x-arango-replication-frompresent");
std::string const StaticStrings::ReplicationHeaderActive(
    "x-arango-replication-active");

// database names
std::string const StaticStrings::SystemDatabase("_system");

// collection names
std::string const StaticStrings::AnalyzersCollection("_analyzers");
std::string const StaticStrings::LegacyAnalyzersCollection(
    "_iresearch_analyzers");
std::string const StaticStrings::UsersCollection("_users");
std::string const StaticStrings::GraphsCollection("_graphs");
std::string const StaticStrings::AqlFunctionsCollection("_aqlfunctions");
std::string const StaticStrings::QueuesCollection("_queues");
std::string const StaticStrings::JobsCollection("_jobs");
std::string const StaticStrings::AppsCollection("_apps");
std::string const StaticStrings::AppBundlesCollection("_appbundles");
std::string const StaticStrings::FrontendCollection("_frontend");
std::string const StaticStrings::QueriesCollection("_queries");
std::string const StaticStrings::StatisticsCollection("_statistics");
std::string const StaticStrings::Statistics15Collection("_statistics15");
std::string const StaticStrings::StatisticsRawCollection("_statisticsRaw");

// analyzers names
std::string const StaticStrings::AnalyzersRevision("revision");
std::string const StaticStrings::AnalyzersBuildingRevision("buildingRevision");
std::string const StaticStrings::AnalyzersDeletedRevision("revisionDeleted");

// Database definition fields
std::string const StaticStrings::DatabaseId("id");
std::string const StaticStrings::DatabaseName("name");
std::string const StaticStrings::Properties("properties");

// LogicalDataSource definition fields
std::string const StaticStrings::DataSourceDeleted("deleted");
std::string const StaticStrings::DataSourceGuid("globallyUniqueId");
std::string const StaticStrings::DataSourceId("id");
std::string const StaticStrings::DataSourceCid("cid");
std::string const StaticStrings::DataSourceName("name");
std::string const StaticStrings::DataSourcePlanId("planId");
std::string const StaticStrings::DataSourceSystem("isSystem");
std::string const StaticStrings::DataSourceType("type");
std::string const StaticStrings::DataSourceParameters("parameters");

// Index definition fields
std::string const StaticStrings::IndexDeduplicate("deduplicate");
std::string const StaticStrings::IndexExpireAfter("expireAfter");
std::string const StaticStrings::IndexFields("fields");
std::string const StaticStrings::IndexId("id");
std::string const StaticStrings::IndexInBackground("inBackground");
std::string const StaticStrings::IndexIsBuilding("isBuilding");
std::string const StaticStrings::IndexName("name");
std::string const StaticStrings::IndexSparse("sparse");
std::string const StaticStrings::IndexStoredValues("storedValues");
std::string const StaticStrings::IndexPrefixFields("prefixFields");
std::string const StaticStrings::IndexType("type");
std::string const StaticStrings::IndexUnique("unique");
std::string const StaticStrings::IndexEstimates("estimates");
std::string const StaticStrings::IndexLegacyPolygons("legacyPolygons");
std::string const StaticStrings::IndexCreationError("creationError");

// static index names
std::string const StaticStrings::IndexNameEdge("edge");
std::string const StaticStrings::IndexNameEdgeFrom("edge_from");
std::string const StaticStrings::IndexNameEdgeTo("edge_to");
std::string const StaticStrings::IndexNamePrimary("primary");

// index hint strings
std::string const StaticStrings::IndexHintDisableIndex("disableIndex");
std::string const StaticStrings::IndexHintOption("indexHint");
std::string const StaticStrings::IndexHintOptionForce("forceIndexHint");

// query options
std::string const StaticStrings::Filter("filter");
std::string const StaticStrings::MaxProjections("maxProjections");
std::string const StaticStrings::ProducesResult("producesResult");
std::string const StaticStrings::ReadOwnWrites("readOwnWrites");
std::string const StaticStrings::UseCache("useCache");
std::string const StaticStrings::Parallelism("parallelism");
std::string const StaticStrings::ForceOneShardAttributeValue(
    "forceOneShardAttributeValue");
std::string const StaticStrings::JoinStrategyType("joinStrategyType");

// HTTP headers
std::string const StaticStrings::Accept("accept");
std::string const StaticStrings::AcceptEncoding("accept-encoding");
std::string const StaticStrings::AccessControlAllowCredentials(
    "access-control-allow-credentials");
std::string const StaticStrings::AccessControlAllowHeaders(
    "access-control-allow-headers");
std::string const StaticStrings::AccessControlAllowMethods(
    "access-control-allow-methods");
std::string const StaticStrings::AccessControlAllowOrigin(
    "access-control-allow-origin");
std::string const StaticStrings::AccessControlExposeHeaders(
    "access-control-expose-headers");
std::string const StaticStrings::AccessControlMaxAge("access-control-max-age");
std::string const StaticStrings::AccessControlRequestHeaders(
    "access-control-request-headers");
std::string const StaticStrings::Allow("allow");
std::string const StaticStrings::AllowDirtyReads("x-arango-allow-dirty-read");
std::string const StaticStrings::Async("x-arango-async");
std::string const StaticStrings::AsyncId("x-arango-async-id");
std::string const StaticStrings::Authorization("authorization");
std::string const StaticStrings::BatchContentType(
    "application/x-arango-batchpart");
std::string const StaticStrings::CacheControl("cache-control");
std::string const StaticStrings::Chunked("chunked");
std::string const StaticStrings::Close("Close");
std::string const StaticStrings::ClusterCommSource("x-arango-source");
std::string const StaticStrings::Code("code");
std::string const StaticStrings::Connection("connection");
std::string const StaticStrings::ContentEncoding("content-encoding");
std::string const StaticStrings::ContentLength("content-length");
std::string const StaticStrings::ContentTypeHeader("content-type");
std::string const StaticStrings::Cookie("cookie");
std::string const StaticStrings::CorsMethods(
    "DELETE, GET, HEAD, OPTIONS, PATCH, POST, PUT");
std::string const StaticStrings::Error("error");
std::string const StaticStrings::ErrorCode("errorCode");
std::string const StaticStrings::ErrorMessage("errorMessage");
std::string const StaticStrings::ErrorNum("errorNum");
std::string const StaticStrings::Errors("x-arango-errors");
std::string const StaticStrings::ErrorCodes("x-arango-error-codes");
std::string const StaticStrings::Etag("etag");
std::string const StaticStrings::Expect("expect");
std::string const StaticStrings::ExposedCorsHeaders(
    "etag, content-encoding, content-length, location, server, "
    "x-arango-errors, x-arango-async-id");
std::string const StaticStrings::HLCHeader("x-arango-hlc");
std::string const StaticStrings::LeaderEndpoint("x-arango-endpoint");
std::string const StaticStrings::Location("location");
std::string const StaticStrings::LockLocation("lockLocation");
std::string const StaticStrings::NoSniff("nosniff");
std::string const StaticStrings::Origin("origin");
std::string const StaticStrings::PotentialDirtyRead(
    "x-arango-potential-dirty-read");
std::string const StaticStrings::RequestForwardedTo(
    "x-arango-request-forwarded-to");
std::string const StaticStrings::Server("server");
std::string const StaticStrings::TransferEncoding("transfer-encoding");
std::string const StaticStrings::TransactionBody("x-arango-trx-body");
std::string const StaticStrings::TransactionId("x-arango-trx-id");

std::string const StaticStrings::Unlimited("unlimited");
std::string const StaticStrings::UserAgent("user-agent");
std::string const StaticStrings::WwwAuthenticate("www-authenticate");
std::string const StaticStrings::XContentTypeOptions("x-content-type-options");
std::string const StaticStrings::XArangoFrontend("x-arango-frontend");
std::string const StaticStrings::XArangoQueueTimeSeconds(
    "x-arango-queue-time-seconds");
std::string const StaticStrings::ContentSecurityPolicy(
    "content-security-policy");
std::string const StaticStrings::Pragma("pragma");
std::string const StaticStrings::Expires("expires");
std::string const StaticStrings::HSTS("strict-transport-security");

// mime types
std::string const StaticStrings::MimeTypeDump(
    "application/x-arango-dump; charset=utf-8");
std::string const StaticStrings::MimeTypeDumpNoEncoding(
    "application/x-arango-dump");
std::string const StaticStrings::MimeTypeHtml("text/html; charset=utf-8");
std::string const StaticStrings::MimeTypeHtmlNoEncoding("text/html");
std::string const StaticStrings::MimeTypeJson(
    "application/json; charset=utf-8");
std::string const StaticStrings::MimeTypeJsonNoEncoding("application/json");
std::string const StaticStrings::MimeTypeText("text/plain; charset=utf-8");
std::string const StaticStrings::MimeTypeTextNoEncoding("text/plain");
std::string const StaticStrings::MimeTypeVPack("application/x-velocypack");
std::string const StaticStrings::MultiPartContentType("multipart/form-data");

// accept-encodings
std::string const StaticStrings::EncodingArangoLz4("x-arango-lz4");
std::string const StaticStrings::EncodingDeflate("deflate");
std::string const StaticStrings::EncodingGzip("gzip");
std::string const StaticStrings::EncodingLz4("lz4");

std::string const StaticStrings::Body("body");
std::string const StaticStrings::ParsedBody("parsedBody");

// collection attributes
std::string const StaticStrings::AllowUserKeys("allowUserKeys");
std::string const StaticStrings::CacheEnabled("cacheEnabled");
std::string const StaticStrings::ComputedValues("computedValues");
std::string const StaticStrings::DistributeShardsLike("distributeShardsLike");
std::string const StaticStrings::Indexes("indexes");
std::string const StaticStrings::IsSmart("isSmart");
std::string const StaticStrings::IsSmartChild("isSmartChild");
std::string const StaticStrings::GroupId("groupId");
std::string const StaticStrings::KeyOptions("keyOptions");
std::string const StaticStrings::NumberOfShards("numberOfShards");
std::string const StaticStrings::MinReplicationFactor("minReplicationFactor");
std::string const StaticStrings::ObjectId("objectId");
std::string const StaticStrings::ReplicationFactor("replicationFactor");
std::string const StaticStrings::Satellite("satellite");
std::string const StaticStrings::ShardKeys("shardKeys");
std::string const StaticStrings::Sharding("sharding");
std::string const StaticStrings::ShardingStrategy("shardingStrategy");
std::string const StaticStrings::SmartJoinAttribute("smartJoinAttribute");
std::string const StaticStrings::SyncByRevision("syncByRevision");
std::string const StaticStrings::UsesRevisionsAsDocumentIds(
    "usesRevisionsAsDocumentIds");
std::string const StaticStrings::Schema("schema");
std::string const StaticStrings::InternalValidatorTypes(
    "internalValidatorType");
std::string const StaticStrings::Version("version");
std::string const StaticStrings::WriteConcern("writeConcern");
std::string const StaticStrings::ShardingSingle("single");
std::string const StaticStrings::ReplicationVersion("replicationVersion");
std::string const StaticStrings::ReplicatedLogs("replicatedLogs");

// graph attribute names
std::string const StaticStrings::GraphFrom("from");
std::string const StaticStrings::GraphTo("to");
std::string const StaticStrings::GraphOptions("options");
std::string const StaticStrings::GraphSmartGraphAttribute(
    "smartGraphAttribute");
std::string const StaticStrings::GraphEdgeDefinitions("edgeDefinitions");
std::string const StaticStrings::GraphOrphans("orphanCollections");

std::string const StaticStrings::GraphName("name");
std::string const StaticStrings::GraphTraversalProfileLevel("traversalProfile");

// smart graph relevant attributes
std::string const StaticStrings::IsDisjoint("isDisjoint");
std::string const StaticStrings::GraphIsSmart("isSmart");
std::string const StaticStrings::GraphIsSatellite("isSatellite");
std::string const StaticStrings::GraphSatellites("satellites");
std::string const StaticStrings::GraphInitial("initial");
std::string const StaticStrings::GraphInitialCid("initialCid");
std::string const StaticStrings::ShadowCollections("shadowCollections");
std::string const StaticStrings::FullLocalPrefix("_local_");
std::string const StaticStrings::FullFromPrefix("_from_");
std::string const StaticStrings::FullToPrefix("_to_");

// Graph directions
std::string const StaticStrings::GraphDirection("direction");
std::string const StaticStrings::GraphDirectionInbound("inbound");
std::string const StaticStrings::GraphDirectionOutbound("outbound");

// Query Strings
std::string const StaticStrings::QuerySortASC("ASC");
std::string const StaticStrings::QuerySortDESC("DESC");

// Graph Query Strings
std::string const StaticStrings::GraphQueryEdges("edges");
std::string const StaticStrings::GraphQueryVertices("vertices");
std::string const StaticStrings::GraphQueryPath("path");
std::string const StaticStrings::GraphQueryGlobal("global");
std::string const StaticStrings::GraphQueryNone("none");
std::string const StaticStrings::GraphQueryWeight("weight");
std::string const StaticStrings::GraphQueryWeights("weights");
std::string const StaticStrings::GraphQueryOrder("order");
std::string const StaticStrings::GraphQueryOrderBFS("bfs");
std::string const StaticStrings::GraphQueryOrderDFS("dfs");
std::string const StaticStrings::GraphQueryOrderWeighted("weighted");
std::string const StaticStrings::GraphQueryShortestPathType("shortestPathType");

// rest query parameter
std::string const StaticStrings::GraphDropCollections("dropCollections");
std::string const StaticStrings::GraphDropCollection("dropCollection");
std::string const StaticStrings::GraphCreateCollection("createCollection");

// Replication
std::string const StaticStrings::ReplicationSoftLockOnly("doSoftLockOnly");
std::string const StaticStrings::FailoverCandidates("failoverCandidates");
std::string const StaticStrings::RevisionTreeCount("count");
std::string const StaticStrings::RevisionTreeHash("hash");
std::string const StaticStrings::RevisionTreeMaxDepth("maxDepth");
std::string const StaticStrings::RevisionTreeNodes("nodes");
std::string const StaticStrings::RevisionTreeRangeMax("rangeMax");
std::string const StaticStrings::RevisionTreeRangeMin("rangeMin");
std::string const StaticStrings::RevisionTreeInitialRangeMin("initialRangeMin");
std::string const StaticStrings::RevisionTreeRanges("ranges");
std::string const StaticStrings::RevisionTreeResume("resume");
std::string const StaticStrings::RevisionTreeResumeHLC("resumeHLC");
std::string const StaticStrings::RevisionTreeVersion("version");
std::string const StaticStrings::FollowingTermId("followingTermId");

// Replication 2.0
std::string const StaticStrings::Config("config");
std::string const StaticStrings::CurrentTerm("currentTerm");
std::string const StaticStrings::Follower("follower");
std::string const StaticStrings::Id("id");
std::string const StaticStrings::Index("index");
std::string const StaticStrings::Leader("leader");
std::string const StaticStrings::Unconfigured("unconfigured");
std::string const StaticStrings::LocalStatus("localStatus");
std::string const StaticStrings::Participants("participants");
std::string const StaticStrings::ServerId("serverId");
std::string const StaticStrings::Spearhead("spearhead");
std::string const StaticStrings::Term("term");
std::string const StaticStrings::CommitIndex("commitIndex");
std::string const StaticStrings::FirstIndex("firstIndex");
std::string const StaticStrings::ReleaseIndex("releaseIndex");
std::string const StaticStrings::SyncIndex("syncIndex");

// Generic attribute names
std::string const StaticStrings::AttrCoordinator("coordinator");
std::string const StaticStrings::AttrCoordinatorRebootId("coordinatorRebootId");
std::string const StaticStrings::AttrCoordinatorId("coordinatorId");
std::string const StaticStrings::AttrIsBuilding("isBuilding");

// misc strings
std::string const StaticStrings::LastValue("lastValue");
std::string const StaticStrings::checksumFileJs("JS_SHA1SUM.txt");

std::string const StaticStrings::RebootId("rebootId");

std::string const StaticStrings::New("new");
std::string const StaticStrings::Old("old");
std::string const StaticStrings::UpgradeEnvName(
    "ARANGODB_UPGRADE_DURING_RESTORE");
std::string const StaticStrings::BackupToDeleteName("DIRECTORY_TO_DELETE");
std::string const StaticStrings::BackupSearchToDeleteName(
    "DIRECTORY_TO_DELETE_SEARCH");

// aql api strings
std::string const StaticStrings::AqlDocumentCall("x-arango-aql-document-aql");
std::string const StaticStrings::AqlFastPath("x-arango-fast-path");
std::string const StaticStrings::AqlRemoteExecute("execute");
std::string const StaticStrings::AqlRemoteCallStack("callStack");
std::string const StaticStrings::AqlRemoteLimit("limit");
std::string const StaticStrings::AqlRemoteLimitType("limitType");
std::string const StaticStrings::AqlRemoteLimitTypeSoft("soft");
std::string const StaticStrings::AqlRemoteLimitTypeHard("hard");
std::string const StaticStrings::AqlRemoteFullCount("fullCount");
std::string const StaticStrings::AqlRemoteOffset("offset");
std::string const StaticStrings::AqlRemoteInfinity("infinity");
std::string const StaticStrings::AqlRemoteResult("result");
std::string const StaticStrings::AqlRemoteBlock("block");
std::string const StaticStrings::AqlRemoteSkipped("skipped");
std::string const StaticStrings::AqlRemoteState("state");
std::string const StaticStrings::AqlRemoteStateDone("done");
std::string const StaticStrings::AqlRemoteStateHasmore("hasmore");
std::string const StaticStrings::AqlCallListSpecific("specifics");
std::string const StaticStrings::AqlCallListDefault("default");
std::string const StaticStrings::ArangoSearchAnalyzersRevision(
    "analyzersRevision");
std::string const StaticStrings::ArangoSearchCurrentAnalyzersRevision(
    "current");
std::string const StaticStrings::ArangoSearchSystemAnalyzersRevision("system");

// aql http headers
std::string const StaticStrings::AqlShardIdHeader("x-shard-id");

// validation
std::string const StaticStrings::ValidationLevelNone("none");
std::string const StaticStrings::ValidationLevelNew("new");
std::string const StaticStrings::ValidationLevelModerate("moderate");
std::string const StaticStrings::ValidationLevelStrict("strict");

std::string const StaticStrings::ValidationParameterMessage("message");
std::string const StaticStrings::ValidationParameterLevel("level");
std::string const StaticStrings::ValidationParameterRule("rule");
std::string const StaticStrings::ValidationParameterType("type");
