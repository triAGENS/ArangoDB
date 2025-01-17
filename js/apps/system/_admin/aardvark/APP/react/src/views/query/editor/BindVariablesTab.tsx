import {
  Box,
  Flex,
  Input,
  Table,
  TableContainer,
  Tag,
  Tbody,
  Td,
  Text,
  Th,
  Thead,
  Tr
} from "@chakra-ui/react";
import { ValidationError } from "jsoneditor-react";
import React from "react";
import { ControlledJSONEditor } from "../../../components/jsonEditor/ControlledJSONEditor";
import { JSONErrors } from "../../../components/jsonEditor/JSONErrors";
import { useQueryContext } from "../QueryContextProvider";

const detectType = (value: string) => {
  try {
    const parsedValue = JSON.parse(value);
    if (parsedValue instanceof Array) {
      return "array";
    }
    return typeof parsedValue;
  } catch (ignore) {}
};
const parseInput = (value: string) => {
  try {
    return JSON.parse(value);
  } catch (e) {
    return value;
  }
};
const DEFINED_TYPES = ["string", "number", "array", "object", "boolean"];
export const BindVariablesTab = ({ mode }: { mode: "json" | "table" }) => {
  const { queryBindParams, onBindParamsChange, bindVariablesJsonEditorRef } =
    useQueryContext();
  const queryBindParamKeys = Object.keys(queryBindParams);
  const [errors, setErrors] = React.useState<ValidationError[]>([]);
  if (mode === "table") {
    return (
      <TableContainer>
        <Table size="sm">
          <Thead>
            <Tr>
              <Th>Key</Th>
              <Th>Value</Th>
            </Tr>
          </Thead>
          <Tbody>
            {queryBindParamKeys.length > 0 ? (
              queryBindParamKeys.map(key => {
                return <BindVariableRow key={key} bindParamKey={key} />;
              })
            ) : (
              <Tr>
                <Td colSpan={2}>No bind parameters found in query.</Td>
              </Tr>
            )}
          </Tbody>
        </Table>
      </TableContainer>
    );
  }
  return (
    <Box position="relative" height="full">
      <ControlledJSONEditor
        ref={bindVariablesJsonEditorRef}
        mode="code"
        defaultValue={queryBindParams}
        onChange={updatedValue => {
          !errors.length && onBindParamsChange(updatedValue || {});
        }}
        htmlElementProps={{
          style: {
            height: "calc(100% - 20px)"
          }
        }}
        onValidationError={errors => {
          setErrors(errors);
        }}
        /**
         * This option will get passed on the jsonEditor.
         * */
        mainMenuBar={false}
      />
      <JSONErrors
        position="absolute"
        bottom={0}
        left={0}
        right={0}
        zIndex={10}
        errors={errors}
      />
    </Box>
  );
};

const BindVariableRow = ({ bindParamKey }: { bindParamKey: string }) => {
  const [type, setType] = React.useState<
    "string" | "number" | "array" | "object" | "boolean" | "unknown"
  >("unknown");
  const { queryBindParams, onBindParamsChange } = useQueryContext();
  const value = queryBindParams[bindParamKey];
  let parsedValue = value;
  if (typeof value === "object") {
    parsedValue = JSON.stringify(value);
  }
  const handleValueChange = (e: React.ChangeEvent<HTMLInputElement>) => {
    setType(detectType(e.target.value) as any);
    const parsedValue = parseInput(e.target.value);
    const newQueryBindParams = {
      ...queryBindParams,
      [bindParamKey]: parsedValue
    };
    onBindParamsChange(newQueryBindParams);
  };
  return (
    <Tr>
      <Td>
        <Flex maxWidth="200px" alignItems="center" gap="2">
          <Text isTruncated>{bindParamKey}</Text>
          {DEFINED_TYPES.includes(type) && (
            <Tag flexShrink={0} size="sm">
              {type}
            </Tag>
          )}
        </Flex>
      </Td>
      <Td>
        <Input
          size="sm"
          placeholder="Value"
          value={parsedValue}
          onChange={handleValueChange}
        />
      </Td>
    </Tr>
  );
};
