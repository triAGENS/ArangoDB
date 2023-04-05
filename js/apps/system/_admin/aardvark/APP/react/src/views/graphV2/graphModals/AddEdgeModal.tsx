import {
  Box,
  Button,
  FormLabel,
  HStack,
  Input,
  Select,
  Stack
} from "@chakra-ui/react";
import { DocumentMetadata } from "arangojs/documents";
import { JsonEditor } from "jsoneditor-react";
import React, { useState } from "react";
import useSWR from "swr";
import {
  Modal,
  ModalBody,
  ModalFooter,
  ModalHeader
} from "../../../components/modal";
import { InfoTooltip } from "../../../components/tooltip/InfoTooltip";
import { getCurrentDB } from "../../../utils/arangoClient";
import { useGraph } from "../GraphContext";

type SaveEdgeResponse = DocumentMetadata & {
  new: { _id: string; _key: string; _to: string; _from: string };
};
const useAddEdgeAction = ({
  graphName,
  onSuccess,
  onFailure
}: {
  graphName: string;
  onSuccess: (response: SaveEdgeResponse) => void;
  onFailure: () => void;
}) => {
  const addEdge = async ({
    data,
    collection
  }: {
    data: { [key: string]: string | undefined };
    collection: string;
  }) => {
    const db = getCurrentDB();
    const graphCollection = db.graph(graphName).edgeCollection(collection);
    try {
      const response = (await graphCollection.save(data, {
        returnNew: true
      })) as SaveEdgeResponse;
      window.arangoHelper.arangoNotification(
        `The edge ${response.new._id} was successfully created`
      );
      onSuccess(response);
    } catch (error: unknown) {
      console.log("Error creating this edge: ", error);
      const errorMessage = (error as any)?.response?.body?.errorMessage;
      window.arangoHelper.arangoError(
        "Could not create edge",
        errorMessage ? errorMessage : ""
      );
      onFailure();
    }
  };
  return { addEdge };
};

const useEdgeCollections = () => {
  const { graphName } = useGraph();
  const { data: edgeCollections } = useSWR(
    ["edgeCollections", graphName],
    async () => {
      const db = getCurrentDB();
      const collections = await db.graph(graphName).listEdgeCollections();
      return collections;
    }
  );
  return { edgeCollections };
};

export const AddEdgeModal = () => {
  const { graphName, onClearAction, selectedAction, graphData } = useGraph();
  const { edgeCollections } = useEdgeCollections();
  const { from, to } = selectedAction || {};
  const { addEdge } = useAddEdgeAction({
    graphName,
    onSuccess: response => {
      const { _id: id, _key: label, _from: from, _to: to } = response.new;
      const edgeModel = {
        id,
        label,
        from,
        to
      };
      // this adds the edge to the graph
      selectedAction?.callback?.(edgeModel);
      onClearAction();
    },
    onFailure: onClearAction
  });

  const [json, setJson] = useState({});
  const [key, setKey] = useState("");
  const [collection, setCollection] = useState(edgeCollections?.[0] || "");
  const isEnterprise =
    graphData?.settings.isSmart && !graphData.settings.smartGraphAttribute;
  return (
    <Modal isOpen onClose={onClearAction}>
      <ModalHeader>
        <Box>Add Edge</Box>
      </ModalHeader>
      <ModalBody>
        <Box>_from: {from}</Box>
        <Box>_to: {to}</Box>
        <Stack spacing="4">
          {!isEnterprise && (
            <Stack spacing="2">
              <FormLabel htmlFor="_key">_key</FormLabel>
              <HStack>
                <Input
                  id="_key"
                  placeholder="Optional: Leave empty for autogenerated key"
                  onChange={event => setKey(event.target.value)}
                />
                <InfoTooltip label="The edge's unique key (optional attribute, leave empty for autogenerated key)" />
              </HStack>
            </Stack>
          )}
          <Stack spacing="2">
            <FormLabel htmlFor="edgeCollection">Vertex Collection</FormLabel>
            <HStack>
              <Select
                id="edgeCollection"
                onChange={event => setCollection(event.target.value)}
              >
                {edgeCollections?.map(edgeCollectionName => {
                  return (
                    <option key={edgeCollectionName} value={edgeCollectionName}>
                      {edgeCollectionName}
                    </option>
                  );
                })}
              </Select>
              <InfoTooltip label="Please select the target collection for the new edge." />
            </HStack>
          </Stack>
          <JsonEditor
            value={{}}
            onChange={value => {
              setJson(value);
            }}
            mode={"code"}
            history={true}
          />
        </Stack>
      </ModalBody>
      <ModalFooter>
        <HStack>
          <Button onClick={onClearAction}>Cancel</Button>
          <Button
            colorScheme="green"
            onClick={() => {
              if (from && to) {
                addEdge({
                  collection: collection
                    ? collection
                    : edgeCollections?.[0] || collection,
                  data: {
                    ...json,
                    _key: key || undefined,
                    _from: from,
                    _to: to
                  }
                });
              }
            }}
          >
            Create
          </Button>
        </HStack>
      </ModalFooter>
    </Modal>
  );
};
