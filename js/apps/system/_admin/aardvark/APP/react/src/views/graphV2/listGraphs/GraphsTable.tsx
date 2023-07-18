import { EditIcon } from "@chakra-ui/icons";
import { Button, Link, Stack, useDisclosure } from "@chakra-ui/react";
import { CellContext, createColumnHelper } from "@tanstack/react-table";
import { GraphInfo } from "arangojs/graph";
import React from "react";
import { Link as RouterLink } from "react-router-dom";
import { FiltersList } from "../../../components/table/FiltersList";
import { ReactTable } from "../../../components/table/ReactTable";
import { useSortableReactTable } from "../../../components/table/useSortableReactTable";
import { useGraphsListContext } from "./GraphsListContext";
import { detectType } from "./GraphsHelpers";
import { GraphsModeProvider } from "./GraphsModeContext";
import { EditGraphModal } from "./EditGraphModal";

const columnHelper = createColumnHelper<GraphInfo>();

const TABLE_COLUMNS = [
  columnHelper.accessor("name", {
    header: "Name",
    id: "name",
    cell: info => {
      const cellValue = info.cell.getValue();
      return (
        <Link
          as={RouterLink}
          to={`/graphs-v2/${encodeURIComponent(cellValue)}`}
          textDecoration="underline"
          color="blue.500"
          _hover={{
            color: "blue.600"
          }}
        >
          {cellValue}
        </Link>
      );
    },
    meta: {
      filterType: "text"
    }
  }),
  columnHelper.accessor(row => getGraphTypeString(row), {
    header: "Type",
    id: "type",
    filterFn: "equals"
  }),
  columnHelper.accessor("actions" as any, {
    header: "Actions",
    id: "actions",
    enableColumnFilter: false,
    enableSorting: false,
    cell: info => {
      return <ActionCell info={info} />;
    }
  })
];

const ActionCell = ({ info }: { info: CellContext<GraphInfo, any> }) => {
  const { isOpen, onOpen, onClose } = useDisclosure();

  return (
    <>
      <GraphsModeProvider mode="edit">
        <EditGraphModal
          isOpen={isOpen}
          onClose={onClose}
          graph={info.cell.row.original}
        />
      </GraphsModeProvider>
      <Button onClick={onOpen} size="xs">
        <EditIcon />
      </Button>
    </>
  );
};

const GRAPH_TYPE_TO_LABEL_MAP = {
  satellite: "Satellite",
  smart: "Smart",
  enterprise: "Enterprise",
  general: "General"
};
const getGraphTypeString = (graph: GraphInfo) => {
  const { type, isDisjoint } = detectType(graph);
  let graphTypeString = GRAPH_TYPE_TO_LABEL_MAP[type];
  if (isDisjoint) {
    return `Disjoint ${graphTypeString}`;
  }
  return graphTypeString;
};

export const GraphsTable = () => {
  const { graphs } = useGraphsListContext();
  const tableInstance = useSortableReactTable<GraphInfo>({
    data: graphs || [],
    columns: TABLE_COLUMNS,
    initialSorting: [
      {
        id: "name",
        desc: false
      }
    ]
  });

  return (
    <>
      <Stack>
        <FiltersList<GraphInfo> columns={TABLE_COLUMNS} table={tableInstance} />
        <ReactTable<GraphInfo>
          table={tableInstance}
          emptyStateMessage="No graphs found"
        />
      </Stack>
    </>
  );
};
