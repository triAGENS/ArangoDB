import { ChevronDownIcon } from "@chakra-ui/icons";
import {
  Button,
  ButtonGroup,
  Flex,
  FormLabel,
  IconButton,
  Input,
  Menu,
  MenuButton,
  MenuItem,
  MenuList,
  Stack,
  Text
} from "@chakra-ui/react";
import React from "react";
import {
  Modal,
  ModalBody,
  ModalFooter,
  ModalHeader
} from "../../../components/modal";
import { useQueryContext } from "../QueryContextProvider";

export const QueryBottomBar = () => {
  const {
    onExecute,
    onProfile,
    onExplain,
    queryValue,
    queryBindParams,
    queryResults,
    setQueryResults,
    queryName,
    onOpenSaveAsModal
  } = useQueryContext();
  return (
    <Flex
      direction="row"
      borderBottom="1px solid"
      borderX="1px solid"
      borderColor="gray.300"
      padding="2"
      alignItems="center"
    >
      <SaveAsModal />
      <Text fontWeight="medium">
        Query name: {queryName ? queryName : "Untitled"}
      </Text>
      {queryName && (
        <Button marginLeft="2" size="sm" colorScheme="gray">
          Save
        </Button>
      )}
      <Button
        marginLeft="2"
        size="sm"
        colorScheme="gray"
        onClick={onOpenSaveAsModal}
      >
        Save as
      </Button>
      <Stack direction={"row"} marginLeft="auto">
        {queryResults.length > 0 ? (
          <Button
            size="sm"
            colorScheme="gray"
            onClick={() => {
              setQueryResults([]);
            }}
          >
            Remove all results
          </Button>
        ) : null}
        <Button size="sm" colorScheme="gray">
          Create debug pacakge
        </Button>
        <ButtonGroup isAttached>
          <Button
            size="sm"
            colorScheme="green"
            onClick={() => onExecute({ queryValue, queryBindParams })}
          >
            Execute
          </Button>
          <Menu>
            <MenuButton
              as={IconButton}
              size="sm"
              colorScheme="green"
              aria-label="Query execution options"
              icon={<ChevronDownIcon />}
            />
            <MenuList>
              <MenuItem
                onClick={() => onProfile({ queryValue, queryBindParams })}
              >
                Profile
              </MenuItem>
              <MenuItem
                onClick={() => onExplain({ queryValue, queryBindParams })}
              >
                Explain
              </MenuItem>
            </MenuList>
          </Menu>
        </ButtonGroup>
      </Stack>
    </Flex>
  );
};

const SaveAsModal = () => {
  const [queryName, setQueryName] = React.useState<string>("");
  const { onSaveAs, isSaveAsModalOpen, onCloseSaveAsModal } = useQueryContext();
  return (
    <Modal isOpen={isSaveAsModalOpen} onClose={onCloseSaveAsModal}>
      <ModalHeader>Save Query</ModalHeader>
      <ModalBody>
        <FormLabel htmlFor="queryName">Query Name</FormLabel>
        <Input
          id="queryName"
          onChange={e => {
            setQueryName(e.target.value);
          }}
        />
      </ModalBody>
      <ModalFooter>
        <Stack direction="row">
          <Button colorScheme="gray" onClick={() => onCloseSaveAsModal()}>
            Cancel
          </Button>
          <Button
            isDisabled={queryName === ""}
            colorScheme="green"
            onClick={() => onSaveAs(queryName)}
          >
            Save
          </Button>
        </Stack>
      </ModalFooter>
    </Modal>
  );
};
