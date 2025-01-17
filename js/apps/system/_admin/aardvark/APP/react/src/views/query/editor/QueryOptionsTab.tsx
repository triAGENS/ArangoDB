import { MultiSelect, OptionType } from "@arangodb/ui";
import {
  Accordion,
  AccordionButton,
  AccordionIcon,
  AccordionItem,
  AccordionPanel,
  Box,
  FormLabel,
  Grid,
  Input,
  Switch
} from "@chakra-ui/react";
import { ValidationError } from "jsoneditor-react";
import React from "react";
import { ControlledJSONEditor } from "../../../components/jsonEditor/ControlledJSONEditor";
import { JSONErrors } from "../../../components/jsonEditor/JSONErrors";
import { getCurrentDB } from "../../../utils/arangoClient";
import { useQueryContext } from "../QueryContextProvider";
import type { QueryOptions } from "arangojs/database";

const BASIC_QUERY_OPTIONS = {
  memoryLimit: {
    type: "number",
    label: "Memory Limit",
    tooltip: "Maximum memory usage for this query in bytes.",
    name: "memoryLimit"
  },
  failOnWarning: {
    type: "boolean",
    label: "Fail on Warning",
    name: "failOnWarning"
  },
  allowDirtyReads: {
    type: "boolean",
    label: "Allow Dirty Reads",
    name: "allowDirtyReads"
  }
};

const ADVANCED_QUERY_OPTIONS = {
  usePlanCache: {
    type: "boolean",
    label: "Use Plan Cache",
    name: "usePlanCache"
  },
  fillBlockCache: {
    type: "boolean",
    label: "Fill Block Cache",
    name: "fillBlockCache"
  },
  maxNumberOfPlans: {
    type: "number",
    label: "Max Number of Plans"
  },
  maxNodesPerCallstack: {
    type: "number",
    label: "Max Nodes Per Callstack"
  },
  maxWarningCount: {
    type: "number",
    label: "Max Warning Count"
  },
  spillOverThresholdMemoryUsage: {
    type: "number",
    label: "Spill Over Threshold Memory Usage"
  },
  spillOverThresholdNumRows: {
    type: "number",
    label: "Spill Over Threshold Num Rows"
  },
  maxRuntime: {
    type: "number",
    label: "Max Runtime"
  },
  maxTransactionSize: {
    type: "number",
    label: "Max Transaction Size"
  },
  intermediateCommitSize: {
    type: "number",
    label: "Intermediate Commit Size"
  },
  intermediateCommitCount: {
    type: "number",
    label: "Intermediate Commit Count"
  }
};

type QueryOptionsType = QueryOptions & {
  optimizer?: {
    rules?: string[];
  };
};

export const QueryOptionsTab = ({ mode }: { mode: "json" | "table" }) => {
  const {
    queryOptions,
    setQueryOptions,
    setDisabledRules,
    disabledRules,
    bindVariablesJsonEditorRef
  } = useQueryContext();
  const [errors, setErrors] = React.useState<ValidationError[]>([]);
  if (mode === "table") {
    return <QueryOptionForm />;
  }
  return (
    <Box position="relative" height="full">
      <ControlledJSONEditor<QueryOptionsType>
        ref={bindVariablesJsonEditorRef}
        mode="code"
        defaultValue={{
          ...queryOptions,
          optimizer: {
            rules: disabledRules
          }
        }}
        onChange={updatedValue => {
          const updatedOptions = {
            ...updatedValue,
            optimizer: undefined
          };
          if (!errors.length) {
            setQueryOptions(updatedOptions || {});
            setDisabledRules(updatedValue.optimizer?.rules || []);
          }
        }}
        htmlElementProps={{
          style: {
            height: "calc(100% - 20px)"
          }
        }}
        onValidationError={errors => {
          setErrors(errors);
        }}
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

const QueryOptionForm = () => {
  return (
    <Box overflow="auto" height="full">
      <Grid templateColumns="repeat(2, 1fr)" gap="2" padding="2">
        {Object.keys(BASIC_QUERY_OPTIONS).map(key => {
          return (
            <QueryOptionRow
              options={BASIC_QUERY_OPTIONS}
              key={key}
              queryOptionKey={key as any}
            />
          );
        })}
      </Grid>
      <Accordion size="sm" allowToggle>
        <AccordionItem borderColor="gray.300">
          <AccordionButton>
            <Box as="span" flex="1" textAlign="left" fontWeight="bold">
              Advanced
            </Box>
            <AccordionIcon />
          </AccordionButton>

          <AccordionPanel pb={4}>
            <Grid templateColumns="repeat(2, 1fr)" gap="2">
              {Object.keys(ADVANCED_QUERY_OPTIONS).map(key => {
                return (
                  <QueryOptionRow
                    options={ADVANCED_QUERY_OPTIONS}
                    key={key}
                    queryOptionKey={key as any}
                  />
                );
              })}
            </Grid>
          </AccordionPanel>
        </AccordionItem>
        <AccordionItem borderColor="gray.300">
          <AccordionButton>
            <Box as="span" flex="1" textAlign="left" fontWeight="bold">
              Optimizer Rules
            </Box>
            <AccordionIcon />
          </AccordionButton>

          <AccordionPanel pb={4}>
            <OptimizerRules />
          </AccordionPanel>
        </AccordionItem>
      </Accordion>
    </Box>
  );
};

const QueryOptionRow = ({
  queryOptionKey,
  options
}: {
  queryOptionKey: string;
  options: {
    [key: string]: any;
  };
}) => {
  const { queryOptions, setQueryOptions } = useQueryContext();
  const value = queryOptions[queryOptionKey];
  const option = options[queryOptionKey];
  if (option.type === "boolean") {
    return (
      <>
        <FormLabel lineHeight="20px">{option.label}</FormLabel>
        <Switch
          size="sm"
          isChecked={value}
          onChange={e => {
            const updatedValue = e.target.checked;
            setQueryOptions(prev => {
              return { ...prev, [queryOptionKey]: updatedValue };
            });
          }}
        />
      </>
    );
  }
  return (
    <>
      <FormLabel lineHeight="20px">{option.label}</FormLabel>
      <Input
        size="xs"
        value={value}
        // this is required to override bootstrap styles
        css={{
          height: "24px !important"
        }}
        type={option.type}
        onChange={e => {
          const updatedValue = e.target.value;
          setQueryOptions(prev => {
            return { ...prev, [queryOptionKey]: updatedValue };
          });
        }}
      />
    </>
  );
};
type OptimizerRule = {
  name: string;
  flags: {
    canBeDisabled: boolean;
    canCreateAdditionalPlans: boolean;
    clusterOnly: boolean;
    disabledByDefault: boolean;
    enterpriseOnly: boolean;
    hidden: boolean;
  };
};
const useFetchQueryOptimizerRuleOptions = () => {
  const [optimizerRules, setOptimizerRuleOptions] = React.useState<
    OptionType[]
  >([]);
  const isEnterprise = window.frontendConfig.isEnterprise;
  const isCluster = window.frontendConfig.isCluster;
  React.useEffect(() => {
    const fetchRules = async () => {
      const currentDB = getCurrentDB();
      const response = await currentDB.request(
        {
          method: "GET",
          path: "/_api/query/rules"
        },
        res => res.parsedBody
      );
      const ruleOptions = response
        .filter((rule: OptimizerRule) => {
          const { flags } = rule;
          const { enterpriseOnly, clusterOnly, canBeDisabled, hidden } = flags;
          if (enterpriseOnly && !isEnterprise) {
            return false;
          }
          if (clusterOnly && !isCluster) {
            return false;
          }
          if (!canBeDisabled) {
            return false;
          }
          if (hidden) {
            return false;
          }
          return true;
        })
        .map((rule: OptimizerRule) => {
          return {
            label: rule.name,
            value: `-${rule.name}`
          };
        });
      setOptimizerRuleOptions(ruleOptions);
    };
    fetchRules();
  }, [isCluster, isEnterprise]);
  return optimizerRules;
};
const OptimizerRules = () => {
  const ruleOptions = useFetchQueryOptimizerRuleOptions();
  const { setDisabledRules, disabledRules } = useQueryContext();
  return (
    <Grid templateColumns="repeat(2, 1fr)" gap="2" alignItems="center">
      <FormLabel lineHeight="20px">Disabled Rules</FormLabel>
      <MultiSelect
        options={ruleOptions}
        value={ruleOptions.filter(option => {
          return disabledRules.includes(option.value);
        })}
        onChange={value => {
          const disabledRuleNames = value.map((v: any) => v.value);
          setDisabledRules(disabledRuleNames);
        }}
      />
    </Grid>
  );
};
