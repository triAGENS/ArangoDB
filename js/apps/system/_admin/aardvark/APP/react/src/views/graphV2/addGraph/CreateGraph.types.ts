import { ArangoCollection } from "arangojs/collection";
import { GraphCreateOptions } from "arangojs/graph";

type FormEdgeDefinitionOptions = {
  collection: string;
  from: string[];
  to: string[];
};

type GraphCreateValues = {
  name: string;
  edgeDefinitions: FormEdgeDefinitionOptions[];
  orphanCollections: string[];
};

export type GeneralGraphCreateValues = GraphCreateValues;
export type SatelliteGraphCreateValues = GraphCreateValues & {
  options: (string | ArangoCollection)[] | string | ArangoCollection; // where do I find these informations?
};
export type EnterpriseGraphCreateValues = GraphCreateValues &
  Pick<
    GraphCreateOptions,
    "isSmart" | "numberOfShards" | "replicationFactor" | "minReplicationFactor"
  >;
export type SmartGraphCreateValues = GraphCreateValues &
  Pick<
    GraphCreateOptions,
    | "isSmart"
    | "numberOfShards"
    | "replicationFactor"
    | "minReplicationFactor"
    | "isDisjoint"
    | "smartGraphAttribute"
  >;
