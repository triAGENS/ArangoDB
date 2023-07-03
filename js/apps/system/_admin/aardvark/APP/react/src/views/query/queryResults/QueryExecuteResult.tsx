import { WarningIcon } from "@chakra-ui/icons";
import {
  Box,
  Button,
  ButtonGroup,
  Flex,
  Spinner,
  Stack,
  Text
} from "@chakra-ui/react";
import _ from "lodash";
import React from "react";
import { ControlledJSONEditor } from "../../../components/jsonEditor/ControlledJSONEditor";
import { QueryResultType } from "../QueryContextProvider";
import { QueryGraphView } from "./QueryGraphView";
import { RemoveResultButton } from "./RemoveResultButton";
import { ResultTypeBox } from "./ResultTypeBox";
import { TimingInfo } from "./TimingInfo";
import { useSyncQueryExecuteJob } from "./useSyncQueryExecuteJob";

/**
 * check if result could be displayed as graph
 * case a) result has keys named vertices and edges
 * case b) 95% results _from and _to attribute
 */
const useDisplayType = ({ queryResult }: { queryResult: QueryResultType }) => {
  const [displayType, setDisplayType] = React.useState<"graph" | "json">(
    "json"
  );
  const [currentView, setCurrentView] = React.useState<"json" | "graph">(
    "json"
  );
  const [graphDataType, setGraphDataType] = React.useState<"object" | "array">(
    "object"
  );
  React.useEffect(() => {
    const detectGraph = () => {
      const { result } = queryResult;
      if (!result) {
        return;
      }
      let found = false;

      // find first index with data
      let index = 0;
      for (let i = 0; i < result.length; i++) {
        if (result[i]) {
          index = i;
          break;
        }
      }
      if (result[index]) {
        if (result[index].vertices && result[index].edges) {
          let hitsa = 0;
          let totala = 0;

          _.each(result, function (obj) {
            if (obj.edges) {
              _.each(obj.edges, function (edge) {
                if (edge !== null) {
                  if (edge._from && edge._to) {
                    hitsa++;
                  }
                  totala++;
                }
              });
            }
          });

          let percentagea = 0;
          if (totala > 0) {
            percentagea = (hitsa / totala) * 100;
          }

          if (percentagea >= 95) {
            found = true;
            setDisplayType("graph");
            setCurrentView(prevView => {
              if (prevView !== "graph") {
                return "graph";
              }
              return prevView;
            });
            setGraphDataType("object");
          }
        } else {
          // case b) 95% have _from and _to attribute
          let hitsb = 0;
          let totalb = result.length;

          _.each(result, function (obj) {
            if (obj) {
              if (obj._from && obj._to && obj._id) {
                hitsb++;
              }
            }
          });

          let percentageb = 0;
          if (totalb > 0) {
            percentageb = (hitsb / totalb) * 100;
          }

          if (percentageb >= 95) {
            found = true;
            setDisplayType("graph");
            setCurrentView(prevView => {
              if (prevView !== "graph") {
                return "graph";
              }
              return prevView;
            });
            setGraphDataType("array");
            // then display as graph
          }
        }
      }
      if (!found) {
        setDisplayType("json");
        setCurrentView(prevView => {
          if (prevView !== "json") {
            return "json";
          }
          return prevView;
        });
      }
    };
    detectGraph();
  }, [queryResult]);
  return { displayType, graphDataType, currentView, setCurrentView };
};
export const QueryExecuteResult = ({
  index,
  queryResult
}: {
  index: number;
  queryResult: QueryResultType;
}) => {
  useSyncQueryExecuteJob({
    queryResult,
    asyncJobId: queryResult.asyncJobId,
    index
  });
  const { displayType, graphDataType, currentView, setCurrentView } =
    useDisplayType({
      queryResult
    });

  if (queryResult.status === "error") {
    return (
      <Stack direction="row" alignItems="center" padding="2">
        <ResultTypeBox queryResult={queryResult} />
        <WarningIcon color="red.500" />
        <Text>Query error: {queryResult.errorMessage}</Text>
      </Stack>
    );
  }
  if (queryResult.status === "loading") {
    return (
      <Stack direction="row" alignItems="center" padding="2">
        <ResultTypeBox queryResult={queryResult} />
        <Spinner size="sm" />
        <Text>Query in progress</Text>
      </Stack>
    );
  }
  return (
    <Box
      boxShadow="0 0 15px 0 rgba(0,0,0,0.2)"
      borderRadius="md"
      marginBottom="4"
    >
      <Flex padding="2" alignItems="center">
        <Stack direction="row">
          <ResultTypeBox queryResult={queryResult} />
          <TimingInfo queryResult={queryResult} />
        </Stack>
        <Stack direction="row" alignItems="center" marginLeft="auto">
          <ButtonGroup isAttached size="xs">
            <Button
              onClick={() => {
                setCurrentView("json");
              }}
              colorScheme={currentView === "json" ? "blue" : "gray"}
            >
              JSON
            </Button>
            {displayType === "graph" && (
              <Button
                onClick={() => {
                  setCurrentView("graph");
                }}
                colorScheme={currentView === "graph" ? "blue" : "gray"}
              >
                Graph
              </Button>
            )}
          </ButtonGroup>
          <RemoveResultButton index={index} />
        </Stack>
      </Flex>
      <QueryExecuteResultDisplay
        currentView={currentView}
        queryResult={queryResult}
        graphDataType={graphDataType}
      />
    </Box>
  );
};

const QueryExecuteResultDisplay = ({
  queryResult,
  currentView,
  graphDataType
}: {
  queryResult: QueryResultType;
  currentView: "json" | "graph";
  graphDataType: "object" | "array";
}) => {
  if (currentView === "graph") {
    return (
      <QueryGraphView graphDataType={graphDataType} queryResult={queryResult} />
    );
  }
  const { result } = queryResult;
  return (
    <ControlledJSONEditor
      isReadOnly
      mode="code"
      value={result}
      theme="ace/theme/textmate"
      htmlElementProps={{
        style: {
          height: "calc(100% - 48px)"
        }
      }}
      mainMenuBar={false}
    />
  );
};
