import React, { useCallback } from "react";
import { getCurrentDB } from "../../utils/arangoClient";
import { QueryResultType } from "./ArangoQuery.types";

export const useQueryResultHandlers = () => {
  const [queryResults, setQueryResults] = React.useState<QueryResultType[]>([]);

  const setQueryResultById = useCallback((queryResult: QueryResultType) => {
    setQueryResults(prev => {
      const newResults = prev.map(prevQueryResult => {
        if (prevQueryResult.asyncJobId === queryResult.asyncJobId) {
          return queryResult;
        }
        return prevQueryResult;
      });
      return newResults;
    });
  }, []);

  const appendQueryResultById = ({
    asyncJobId,
    result,
    status
  }: {
    asyncJobId?: string;
    result: any;
    status: "success" | "error" | "loading";
  }) => {
    setQueryResults(prev => {
      const newResults = prev.map(prevQueryResult => {
        if (asyncJobId === prevQueryResult.asyncJobId) {
          return {
            ...prevQueryResult,
            result: [...prevQueryResult.result, ...result],
            status
          };
        }
        return prevQueryResult;
      });
      return newResults;
    });
  };
  const onRemoveResult = (index: number) => {
    setQueryResults(queryResults => {
      const newResults = [...queryResults];
      newResults.splice(index, 1);
      return newResults;
    });
  };
  const onRemoveAllResults = async () => {
    const queriesToCancel = queryResults
      .map(queryResult => {
        return queryResult.status === "loading" ? queryResult.asyncJobId : "";
      })
      .filter(id => id);
    const db = getCurrentDB();
    const promises = queriesToCancel.map(async asyncJobId => {
      if (asyncJobId) {
        return db.job(asyncJobId).cancel();
      }
    });
    await Promise.all(promises);
    setQueryResults([]);
  };
  return {
    queryResults,
    setQueryResults,
    setQueryResultById,
    appendQueryResultById,
    onRemoveAllResults,
    onRemoveResult
  };
};
