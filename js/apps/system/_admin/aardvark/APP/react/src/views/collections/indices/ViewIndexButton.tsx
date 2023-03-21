import { ViewIcon } from "@chakra-ui/icons";
import { Box, IconButton, useDisclosure } from "@chakra-ui/react";
import React from "react";
import { Modal, ModalBody, ModalHeader } from "../../../components/modal";
import { InvertedIndexView } from "./addIndex/invertedIndex/InvertedIndexView";
import { useCollectionIndicesContext } from "./CollectionIndicesContext";
import { IndexType } from "./useFetchIndices";

export const ViewIndexButton = ({ indexRow }: { indexRow: IndexType }) => {
  const { onOpen, onClose, isOpen } = useDisclosure();
  const { readOnly } = useCollectionIndicesContext();
  return (
    <Box display="flex" justifyContent="center">
      <IconButton
        isDisabled={readOnly}
        colorScheme="gray"
        variant="ghost"
        size="sm"
        aria-label="View Index"
        icon={<ViewIcon />}
        onClick={onOpen}
      />
      <ViewModal indexRow={indexRow} onClose={onClose} isOpen={isOpen} />
    </Box>
  );
};
const ViewModal = ({
  onClose,
  isOpen,
  indexRow
}: {
  onClose: () => void;
  isOpen: boolean;
  indexRow: IndexType;
}) => {
  return (
    <Modal size="max" onClose={onClose} isOpen={isOpen}>
      <ModalHeader>
        Index: "{indexRow.name}" (ID: {indexRow.id})
      </ModalHeader>
      <ModalBody>
        <InvertedIndexView onClose={onClose} indexRow={indexRow} />
      </ModalBody>
    </Modal>
  );
};
