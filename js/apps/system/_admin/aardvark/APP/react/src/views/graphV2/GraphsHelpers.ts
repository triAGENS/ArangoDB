import { GraphInfo } from "arangojs/graph";
import { getCurrentDB } from "../../utils/arangoClient";
import { SmartGraphCreateValues } from "./addGraph/CreateGraph.types";
import { GraphTypes } from "./Graphs.types";

export const TYPE_TO_LABEL_MAP: {
  [key in GraphTypes]: string;
} = {
  smart: "Smart",
  enterprise: "Enterprise"
};

export const createGraph = async (values: SmartGraphCreateValues) => {
  const currentDB = getCurrentDB();
  const { orphanCollections, edgeDefinitions, isSmart, name, ...options } =
    values;
  return currentDB.request(
    {
      method: "POST",
      path: "/_api/gharial",
      body: {
        orphanCollections,
        edgeDefinitions,
        isSmart,
        name,
        options
      }
    },
    res => res.body.graph
  );
};

type DetectedGraphType = "satellite" | "enterprise" | "smart" | "general";

export const detectType = (
  graph: GraphInfo
): {
  type: DetectedGraphType;
  isDisjoint?: boolean;
} => {
  if (graph.isSatellite || (graph.replicationFactor as any) === "satellite") {
    return {
      type: "satellite"
    };
  }

  if (graph.isSmart && graph.smartGraphAttribute === undefined) {
    return {
      type: "enterprise",
      isDisjoint: graph.isDisjoint
    };
  }

  if (graph.isSmart === true && graph.smartGraphAttribute !== undefined) {
    return {
      type: "smart",
      isDisjoint: graph.isDisjoint
    };
  }
  return {
    type: "general"
  };
};

export const GENERAL_GRAPH_FIELDS_MAP = {
  name: {
    name: "name",
    type: "text",
    label: "Name",
    tooltip:
      "String value. The name to identify the graph. Has to be unique and must follow the Document Keys naming conventions.",
    isRequired: true
  },
  orphanCollections: {
    name: "orphanCollections",
    type: "creatableMultiSelect",
    label: "Orphan collections",
    tooltip:
      "Collections that are part of a graph but not used in an edge definition.",
    isRequired: true,
    noOptionsMessage: () => "Please enter a new and valid collection name"
  }
};
export const CLUSTER_GRAPH_FIELDS_MAP = {
  numberOfShards: {
    name: "numberOfShards",
    type: "number",
    label: "Shards",
    tooltip:
      "Numeric value. Must be at least 1. Number of shards the graph is using.",
    isRequired: true
  },
  replicationFactor: {
    name: "replicationFactor",
    type: "number",
    label: "Replication factor",
    tooltip:
      "Numeric value. Must be at least 1. Total number of copies of the data in the cluster.If not given, the system default for new collections will be used.",
    isRequired: false
  },
  minReplicationFactor: {
    name: "minReplicationFactor",
    type: "number",
    label: "Write concern",
    tooltip:
      "Numeric value. Must be at least 1. Must be smaller or equal compared to the replication factor. Total number of copies of the data in the cluster that are required for each write operation. If we get below this value, the collection will be read-only until enough copies are created. If not given, the system default for new collections will be used.",
    isRequired: false
  }
};

export const SMART_GRAPH_FIELDS_MAP = {
  isDisjoint: {
    name: "isDisjoint",
    type: "boolean",
    label: "Create disjoint graph",
    tooltip:
      "Creates a Disjoint SmartGraph. Creating edges between different SmartGraph components is not allowed.",
    isRequired: false
  },
  smartGraphAttribute: {
    name: "smartGraphAttribute",
    type: "text",
    label: "SmartGraph Attribute",
    tooltip:
      "String value. The attribute name that is used to smartly shard the vertices of a graph. Every vertex in this Graph has to have this attribute. Cannot be modified later.",
    isRequired: true
  }
};
