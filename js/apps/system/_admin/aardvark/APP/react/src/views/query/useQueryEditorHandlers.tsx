import React from "react";
import { parseQuery } from "./queryHelper";
import { isEqual } from "lodash";
type CachedQuery = {
  query: string;
  parameter: { [key: string]: string };
};

export const useQueryEditorHandlers = () => {
  const initialQueryString = window.sessionStorage.getItem("cachedQuery");
  const initialQuery = initialQueryString
    ? JSON.parse(initialQueryString)
    : ({} as CachedQuery);
  const [queryValue, setQueryValue] = React.useState<string>(
    initialQuery?.query || ""
  );

  const [currentQueryName, setCurrentQueryName] = React.useState<string>("");
  const [queryBindParams, setQueryBindParams] = React.useState<{
    [key: string]: string;
  }>(initialQuery?.parameter || {});
  const [disabledRules, setDisabledRules] = React.useState<string[]>([]);
  const [queryOptions, setQueryOptions] = React.useState<{
    [key: string]: any;
  }>({});

  /**
   * Called when the query value is changed by the user
   */
  const onQueryValueChange = (value: string) => {
    const { bindParams: parsedBindParams } = parseQuery(value);
    setQueryValue(value);
    setQueryBindParams(prevQueryBindParams => {
      const parsedBindParamsMap = parsedBindParams.reduce((acc, bindParam) => {
        const prevQueryBindParam = prevQueryBindParams[bindParam];
        acc[bindParam] = prevQueryBindParam;
        return acc;
      }, {} as { [key: string]: string });

      const needsUpdate = isEqual(parsedBindParamsMap, prevQueryBindParams);
      if (needsUpdate) {
        return prevQueryBindParams;
      }

      window.sessionStorage.setItem(
        "cachedQuery",
        JSON.stringify({ query: value, parameter: parsedBindParamsMap })
      );
      return parsedBindParamsMap;
    });
  };
  /**
   * Called when the query bind params are changed by the user (JSON  or form)
   */
  const onBindParamsChange = (value: { [key: string]: string }) => {
    window.sessionStorage.setItem(
      "cachedQuery",
      JSON.stringify({ query: queryValue, parameter: value })
    );
    setQueryBindParams(value);
  };
  /**
   * Called when setting the query value from saved queries
   * or when creating a new query
   */
  const onQueryChange = ({
    value,
    parameter,
    name
  }: {
    value: string;
    parameter: { [key: string]: string };
    name?: string;
  }) => {
    window.sessionStorage.setItem(
      "cachedQuery",
      JSON.stringify({ query: value, parameter: parameter })
    );
    setQueryValue(value);
    setQueryBindParams(parameter);
    setCurrentQueryName(name || "");
  };
  return {
    onQueryChange,
    onQueryValueChange,
    onBindParamsChange,
    queryValue,
    currentQueryName,
    queryBindParams,
    setQueryBindParams,
    setCurrentQueryName,
    disabledRules,
    setDisabledRules,
    queryOptions,
    setQueryOptions
  };
};
