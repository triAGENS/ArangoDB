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

#pragma once

#include <string>
#include <string_view>

namespace arangodb {
class StaticStrings {
  StaticStrings() = delete;

 public:
  // constants
  static std::string const Base64;
  static std::string const Binary;
  static std::string const Empty;
  static std::string const N1800;

  // index lookup strings
  static std::string const IndexEq;
  static std::string const IndexIn;
  static std::string const IndexLe;
  static std::string const IndexLt;
  static std::string const IndexGe;
  static std::string const IndexGt;

  // system attribute names
  static std::string const AttachmentString;
  static std::string const IdString;
  static std::string const KeyString;
  static std::string const PrefixOfKeyString;
  static std::string const PostfixOfKeyString;
  static std::string const RevString;
  static std::string const FromString;
  static std::string const ToString;

  // URL parameter names
  static std::string const IgnoreRevsString;
  static std::string const IsRestoreString;
  static std::string const KeepNullString;
  static std::string const MergeObjectsString;
  static std::string const ReturnNewString;
  static std::string const ReturnOldString;
  static std::string const SilentString;
  static std::string const WaitForSyncString;
  static std::string const SkipDocumentValidation;
  static std::string const OverwriteCollectionPrefix;
  static std::string const IsSynchronousReplicationString;
  static std::string const RefillIndexCachesString;
  static std::string const VersionAttributeString;
  static std::string const Group;
  static std::string const Namespace;
  static std::string const Prefix;
  static std::string const Overwrite;
  static std::string const OverwriteMode;
  static std::string const Compact;
  static std::string const DontWaitForCommit;
  static std::string const UserString;

  // dump headers
  static std::string const DumpAuthUser;
  static std::string const DumpBlockCounts;
  static std::string const DumpId;
  static std::string const DumpShardId;

  // replication headers
  static std::string const ReplicationHeaderCheckMore;
  static std::string const ReplicationHeaderLastIncluded;
  static std::string const ReplicationHeaderLastScanned;
  static std::string const ReplicationHeaderLastTick;
  static std::string const ReplicationHeaderFromPresent;
  static std::string const ReplicationHeaderActive;

  // database names
  static std::string const SystemDatabase;

  // collection names
  static std::string const AnalyzersCollection;
  static std::string const LegacyAnalyzersCollection;
  static std::string const UsersCollection;
  static std::string const GraphsCollection;
  static std::string const AqlFunctionsCollection;
  static std::string const QueuesCollection;
  static std::string const JobsCollection;
  static std::string const AppsCollection;
  static std::string const AppBundlesCollection;
  static std::string const FrontendCollection;
  static std::string const QueriesCollection;
  static std::string const StatisticsCollection;
  static std::string const Statistics15Collection;
  static std::string const StatisticsRawCollection;

  // analyzers names
  static std::string const AnalyzersRevision;
  static std::string const AnalyzersBuildingRevision;
  static std::string const AnalyzersDeletedRevision;

  // database definition fields
  static std::string const DatabaseId;
  static std::string const DatabaseName;
  static std::string const Properties;

  // LogicalDataSource definition fields
  static std::string const DataSourceDeleted;  // data-source deletion marker
  static std::string const DataSourceGuid;     // data-source globaly-unique id
  static std::string const DataSourceId;       // data-source id
  static std::string const DataSourceCid;      // data-source collection id
  static std::string const DataSourceName;     // data-source name
  static std::string const DataSourcePlanId;   // data-source plan id
  static std::string const DataSourceSystem;   // data-source system marker
  static std::string const DataSourceType;     // data-source type
  static std::string const DataSourceParameters;

  // Index definition fields
  static std::string const
      IndexDeduplicate;  // index deduplicate flag (for array indexes)
  static std::string const IndexExpireAfter;   // ttl index expire value
  static std::string const IndexFields;        // index fields
  static std::string const IndexId;            // index id
  static std::string const IndexInBackground;  // index in background
  static std::string_view constexpr IndexParallelism{"parallelism"};
  static std::string const IndexIsBuilding;      // index build in-process
  static std::string const IndexName;            // index name
  static std::string const IndexSparse;          // index sparsity marker
  static std::string const IndexStoredValues;    // index stored values
  static std::string const IndexPrefixFields;    // index prefix fields (zkd)
  static std::string const IndexType;            // index type
  static std::string const IndexUnique;          // index uniqueness marker
  static std::string const IndexEstimates;       // index estimates flag
  static std::string const IndexLegacyPolygons;  // index legacyPolygons flag
  static std::string_view constexpr IndexLookahead{"lookahead"};
  static std::string const IndexCreationError;  // index failed to create

  // static index names
  static std::string const IndexNameEdge;
  static std::string const IndexNameEdgeFrom;
  static std::string const IndexNameEdgeTo;
  static std::string const IndexNamePrimary;

  // index hint strings
  static std::string const IndexHintDisableIndex;
  static std::string const IndexHintOption;
  static std::string const IndexHintOptionForce;

  // query options
  static std::string const Filter;
  static std::string const MaxProjections;
  static std::string const ProducesResult;
  static std::string const ReadOwnWrites;
  static std::string const UseCache;
  static std::string const Parallelism;
  static std::string const ForceOneShardAttributeValue;
  static std::string const JoinStrategyType;

  // HTTP headers
  static std::string const Accept;
  static std::string const AcceptEncoding;
  static std::string const AccessControlAllowCredentials;
  static std::string const AccessControlAllowHeaders;
  static std::string const AccessControlAllowMethods;
  static std::string const AccessControlAllowOrigin;
  static std::string const AccessControlExposeHeaders;
  static std::string const AccessControlMaxAge;
  static std::string const AccessControlRequestHeaders;
  static std::string const Allow;
  static std::string const AllowDirtyReads;
  static std::string const Async;
  static std::string const AsyncId;
  static std::string const Authorization;
  static std::string const BatchContentType;
  static std::string const CacheControl;
  static std::string const Chunked;
  static std::string const Close;
  static std::string const ClusterCommSource;
  static std::string const Code;
  static std::string const Connection;
  static std::string const ContentEncoding;
  static std::string const ContentLength;
  static std::string const ContentTypeHeader;
  static std::string const Cookie;
  ;
  static std::string const CorsMethods;
  static std::string const Error;
  static std::string const ErrorCode;
  static std::string const ErrorMessage;
  static std::string const ErrorNum;
  static std::string const Errors;
  static std::string const ErrorCodes;
  static std::string const Etag;
  static std::string const Expect;
  static std::string const ExposedCorsHeaders;
  static std::string const HLCHeader;
  static std::string const LeaderEndpoint;
  static std::string const Location;
  static std::string const LockLocation;
  static std::string const NoSniff;
  static std::string const Origin;
  static std::string const PotentialDirtyRead;
  static std::string const RequestForwardedTo;
  static std::string const Server;
  static std::string const TransferEncoding;
  static std::string const TransactionBody;
  static std::string const TransactionId;
  static std::string const Unlimited;
  static std::string const UserAgent;
  static std::string const WwwAuthenticate;
  static std::string const XContentTypeOptions;
  static std::string const XArangoFrontend;
  static std::string const XArangoQueueTimeSeconds;
  static std::string const ContentSecurityPolicy;
  static std::string const Pragma;
  static std::string const Expires;
  static std::string const HSTS;

  // mime types
  static std::string const MimeTypeDump;
  static std::string const MimeTypeDumpNoEncoding;
  static std::string const MimeTypeHtml;
  static std::string const MimeTypeHtmlNoEncoding;
  static std::string const MimeTypeJson;
  static std::string const MimeTypeJsonNoEncoding;
  static std::string const MimeTypeText;
  static std::string const MimeTypeTextNoEncoding;
  static std::string const MimeTypeVPack;
  static std::string const MultiPartContentType;

  // encodings
  static std::string const EncodingArangoLz4;
  static std::string const EncodingDeflate;
  static std::string const EncodingGzip;
  static std::string const EncodingLz4;

  // arangosh result body
  static std::string const Body;
  static std::string const ParsedBody;

  // collection attributes
  static std::string const AllowUserKeys;
  static std::string const CacheEnabled;  // also used for indexes
  static std::string const ComputedValues;
  static std::string const DistributeShardsLike;
  static std::string const Indexes;
  static std::string const IsSmart;
  static std::string const IsSmartChild;
  static std::string const KeyOptions;
  static std::string const MinReplicationFactor;
  static std::string const NumberOfShards;
  static std::string const GroupId;
  static std::string const ObjectId;
  static std::string const ReplicationFactor;
  static std::string const Satellite;
  static std::string const ShardKeys;
  static std::string const Sharding;
  static std::string const ShardingStrategy;
  static std::string const SmartJoinAttribute;
  static std::string const SyncByRevision;
  static std::string const UsesRevisionsAsDocumentIds;
  static std::string const Schema;
  static std::string const InternalValidatorTypes;
  static std::string const Version;
  static std::string const WriteConcern;
  static std::string const ShardingSingle;
  static std::string const ReplicationVersion;
  static std::string const ReplicatedLogs;

  // graph attribute names
  static std::string const GraphFrom;
  static std::string const GraphTo;
  static std::string const GraphOptions;
  static std::string const GraphSmartGraphAttribute;
  static std::string const GraphDropCollections;
  static std::string const GraphDropCollection;
  static std::string const GraphCreateCollection;
  static std::string const GraphEdgeDefinitions;
  static std::string const GraphOrphans;
  static std::string const GraphName;
  static std::string const GraphTraversalProfileLevel;

  // smart graph relevant attributes
  static std::string const IsDisjoint;
  static std::string const GraphIsSatellite;
  static std::string const GraphSatellites;
  static std::string const GraphIsSmart;
  static std::string const GraphInitial;
  static std::string const GraphInitialCid;
  static std::string const ShadowCollections;
  static std::string const FullLocalPrefix;
  static std::string const FullFromPrefix;
  static std::string const FullToPrefix;

  // Graph directions
  static std::string const GraphDirection;
  static std::string const GraphDirectionInbound;
  static std::string const GraphDirectionOutbound;

  // Query Strings
  static std::string const QuerySortASC;
  static std::string const QuerySortDESC;

  // Graph Query Strings
  static std::string const GraphQueryEdges;
  static std::string const GraphQueryVertices;
  static std::string const GraphQueryPath;
  static std::string const GraphQueryGlobal;
  static std::string const GraphQueryNone;
  static std::string const GraphQueryWeight;
  static std::string const GraphQueryWeights;
  static std::string const GraphQueryOrder;
  static std::string const GraphQueryOrderBFS;
  static std::string const GraphQueryOrderDFS;
  static std::string const GraphQueryOrderWeighted;
  static std::string const GraphQueryShortestPathType;

  // Replication
  static std::string const ReplicationSoftLockOnly;
  static std::string const FailoverCandidates;
  static std::string const RevisionTreeCount;
  static std::string const RevisionTreeHash;
  static std::string const RevisionTreeMaxDepth;
  static std::string const RevisionTreeNodes;
  static std::string const RevisionTreeRangeMax;
  static std::string const RevisionTreeRangeMin;
  static std::string const RevisionTreeInitialRangeMin;
  static std::string const RevisionTreeRanges;
  // deprecated
  static std::string const RevisionTreeResume;

  static std::string const RevisionTreeResumeHLC;
  static std::string const RevisionTreeVersion;
  static std::string const FollowingTermId;

  // Replication 2.0
  static std::string const Config;
  static std::string const CurrentTerm;
  static std::string const Follower;
  static std::string const Id;
  static std::string const Index;
  static std::string const Leader;
  static std::string const Unconfigured;
  static std::string const LocalStatus;
  static std::string const Participants;
  static std::string const ServerId;
  static std::string const Spearhead;
  static std::string const Term;
  static std::string const CommitIndex;
  static std::string const FirstIndex;
  static std::string const ReleaseIndex;
  static std::string const SyncIndex;
  constexpr static std::string_view AppliedIndex = "appliedIndex";
  constexpr static std::string_view MessageId = "messageId";
  constexpr static std::string_view LogIndex = "logIndex";
  constexpr static std::string_view LogTerm = "logTerm";
  constexpr static std::string_view Payload = "payload";
  constexpr static std::string_view Meta = "meta";

  // Replication 2.0 API Strings
  constexpr static std::string_view ApiLogInternal = "/_api/log-internal";
  constexpr static std::string_view ApiLogExternal = "/_api/log";
  constexpr static std::string_view ApiDocumentStateExternal =
      "/_api/document-state";
  constexpr static std::string_view ApiUpdateSnapshotStatus =
      "update-snapshot-status";

  // generic attribute names
  static std::string const AttrCoordinator;
  static std::string const AttrCoordinatorRebootId;
  static std::string const AttrCoordinatorId;
  static std::string const AttrIsBuilding;

  // misc strings
  static std::string const LastValue;
  static std::string const checksumFileJs;
  static std::string const RebootId;
  static std::string const New;
  static std::string const Old;
  static std::string const UpgradeEnvName;
  static std::string const BackupToDeleteName;
  static std::string const BackupSearchToDeleteName;

  // aql api strings
  static std::string const AqlDocumentCall;
  static std::string const AqlFastPath;
  static std::string const AqlRemoteExecute;
  static std::string const AqlRemoteCallStack;
  static std::string const AqlRemoteLimit;
  static std::string const AqlRemoteLimitType;
  static std::string const AqlRemoteLimitTypeSoft;
  static std::string const AqlRemoteLimitTypeHard;
  static std::string const AqlRemoteFullCount;
  static std::string const AqlRemoteOffset;
  static std::string const AqlRemoteInfinity;
  static std::string const AqlRemoteResult;
  static std::string const AqlRemoteBlock;
  static std::string const AqlRemoteSkipped;
  static std::string const AqlRemoteState;
  static std::string const AqlRemoteStateDone;
  static std::string const AqlRemoteStateHasmore;
  static std::string const AqlCallListSpecific;
  static std::string const AqlCallListDefault;
  static std::string const ArangoSearchAnalyzersRevision;
  static std::string const ArangoSearchCurrentAnalyzersRevision;
  static std::string const ArangoSearchSystemAnalyzersRevision;

  // aql http headers
  static std::string const AqlShardIdHeader;

  // validation
  static std::string const ValidationLevelNone;
  static std::string const ValidationLevelNew;
  static std::string const ValidationLevelModerate;
  static std::string const ValidationLevelStrict;

  static std::string const ValidationParameterMessage;
  static std::string const ValidationParameterLevel;
  static std::string const ValidationParameterRule;
  static std::string const ValidationParameterType;
};
}  // namespace arangodb
